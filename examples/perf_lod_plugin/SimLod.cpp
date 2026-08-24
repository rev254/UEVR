// UEVR perf_lod_plugin - gaze/view contingent simulation LOD.
// Licensed under the MIT license, separate from the rest of the UEVR codebase.
#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <cstdio>

#include "SimLod.hpp"

using namespace uevr;

namespace perflod {
namespace {
constexpr auto kGazeStaleAfter = std::chrono::milliseconds{200};

size_t align_up(size_t value, size_t alignment) {
    if (alignment <= 1) {
        return value;
    }

    return ((value + alignment - 1) / alignment) * alignment;
}
} // namespace

bool SimLod::resolve() {
    if (m_resolved.skeletal_mesh_class != nullptr) {
        return true;
    }

    if (m_resolve_failed) {
        return false;
    }

    auto& api = API::get();

    // May legitimately not exist yet during early startup - keep retrying.
    const auto skeletal_mesh_class = api->find_uobject<API::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");

    if (skeletal_mesh_class == nullptr) {
        m_status.resolve_error = "waiting for SkeletalMeshComponent class...";
        return false;
    }

    Resolved resolved{};
    resolved.skeletal_mesh_class = skeletal_mesh_class;

    // FTickFunction::TickInterval, reached through UActorComponent::PrimaryComponentTick.
    const auto tick_prop = find_property_recursive(skeletal_mesh_class, L"PrimaryComponentTick");

    if (tick_prop == nullptr || property_class_name(tick_prop) != L"StructProperty") {
        m_status.resolve_error = "PrimaryComponentTick is missing or not a StructProperty";
        m_resolve_failed = true;
        api->log_error("[perf_lod] %s", m_status.resolve_error.c_str());
        return false;
    }

    const auto tick_struct = ((API::FStructProperty*)tick_prop)->get_struct();
    const auto interval_prop = find_property_recursive(tick_struct, L"TickInterval");

    if (interval_prop == nullptr || property_class_name(interval_prop) != L"FloatProperty") {
        m_status.resolve_error = "FTickFunction::TickInterval is missing or not a FloatProperty";
        m_resolve_failed = true;
        api->log_error("[perf_lod] %s", m_status.resolve_error.c_str());
        return false;
    }

    resolved.tick_function_offset = tick_prop->get_offset();
    resolved.tick_interval_offset = interval_prop->get_offset();

    // Optional: update rate optimizations (bitfield bool).
    if (const auto uro_prop = find_property_recursive(skeletal_mesh_class, L"bEnableUpdateRateOptimizations");
        uro_prop != nullptr && property_class_name(uro_prop) == L"BoolProperty") {
        resolved.uro_prop = (API::FBoolProperty*)uro_prop;
    } else {
        api->log_warn("[perf_lod] bEnableUpdateRateOptimizations not found; URO toggle disabled");
    }

    // Optional: VisibilityBasedAnimTickOption (uint8 enum).
    if (const auto vis_prop = find_property_recursive(skeletal_mesh_class, L"VisibilityBasedAnimTickOption");
        vis_prop != nullptr) {
        const auto class_name = property_class_name(vis_prop);

        if (class_name == L"ByteProperty" || class_name == L"EnumProperty") {
            resolved.visibility_offset = vis_prop->get_offset();
        } else {
            api->log_warn("[perf_lod] VisibilityBasedAnimTickOption has unexpected type %s",
                          narrow(class_name).c_str());
        }
    } else {
        api->log_warn("[perf_lod] VisibilityBasedAnimTickOption not found (pre-4.21 engine?)");
    }

    // Position sampling. Cached UFunction* so the hot path never looks it up by name.
    if (const auto location_fn = find_function_recursive(skeletal_mesh_class, L"K2_GetComponentLocation");
        location_fn != nullptr) {
        resolved.location_params_size = align_up((size_t)location_fn->get_properties_size(),
                                                 (size_t)location_fn->get_min_alignment());

        for (auto field = location_fn->get_child_properties(); field != nullptr; field = field->get_next()) {
            const auto prop = (API::FProperty*)field;

            if (!prop->is_param() || !prop->is_return_param()) {
                continue;
            }

            resolved.location_return_offset = prop->get_offset();

            if (property_class_name(prop) == L"StructProperty") {
                const auto vector_struct = ((API::FStructProperty*)prop)->get_struct();

                if (vector_struct != nullptr && vector_struct->get_properties_size() >= 24) {
                    resolved.location_is_double = true;
                }
            }

            break;
        }

        if (resolved.location_return_offset >= 0) {
            resolved.get_location_fn = location_fn;
        }
    }

    if (resolved.get_location_fn == nullptr) {
        api->log_warn("[perf_lod] K2_GetComponentLocation unavailable; cone throttling disabled, "
                      "blanket URO still works");
    }

    m_resolved = resolved;
    m_params.assign(m_resolved.location_params_size, 0);

    m_status.resolved = true;
    m_status.resolve_error.clear();
    m_status.tick_function_offset = m_resolved.tick_function_offset;
    m_status.tick_interval_offset = m_resolved.tick_interval_offset;
    m_status.visibility_offset = m_resolved.visibility_offset;
    m_status.has_uro_prop = m_resolved.uro_prop != nullptr;
    m_status.has_location_fn = m_resolved.get_location_fn != nullptr;
    m_status.location_is_double = m_resolved.location_is_double;

    api->log_info("[perf_lod] Resolved: PrimaryComponentTick+0x%x, TickInterval+0x%x, "
                  "VisibilityBasedAnimTickOption+0x%x, URO=%d, LocationFn=%d, doubles=%d",
                  m_resolved.tick_function_offset, m_resolved.tick_interval_offset,
                  m_resolved.visibility_offset, (int)m_status.has_uro_prop,
                  (int)m_status.has_location_fn, (int)m_resolved.location_is_double);

    return true;
}

void SimLod::refresh_candidates() {
    m_candidates = m_resolved.skeletal_mesh_class->get_objects_matching(false);
    m_local_pawn = API::get()->get_local_pawn(0);
    m_status.candidate_count = m_candidates.size();

    if (m_cursor >= m_candidates.size()) {
        m_cursor = 0;
    }

    // Drop state for components that are gone. Pointers can be recycled by the
    // engine's object allocator, so this bounds how long a stale entry lives.
    for (auto it = m_states.begin(); it != m_states.end();) {
        if (!API::UObjectHook::exists(it->first)) {
            if (it->second.throttled && m_status.throttled_count > 0) {
                --m_status.throttled_count;
            }

            it = m_states.erase(it);
        } else {
            ++it;
        }
    }

    m_status.tracked_count = m_states.size();
}

bool SimLod::component_location(API::UObject* comp, Vec3& out) {
    if (m_resolved.get_location_fn == nullptr || m_resolved.location_return_offset < 0) {
        return false;
    }

    if (m_params.size() < m_resolved.location_params_size) {
        m_params.assign(m_resolved.location_params_size, 0);
    }

    std::fill(m_params.begin(), m_params.end(), (uint8_t)0);
    m_resolved.get_location_fn->call(comp, m_params.data());

    const auto base = m_params.data() + m_resolved.location_return_offset;

    if (m_resolved.location_is_double) {
        const auto values = (const double*)base;
        out = Vec3{values[0], values[1], values[2]};
    } else {
        const auto values = (const float*)base;
        out = Vec3{(double)values[0], (double)values[1], (double)values[2]};
    }

    return true;
}

void SimLod::write_tick_interval(API::UObject* comp, float interval) {
    const auto address = (uintptr_t)comp + (uintptr_t)m_resolved.tick_function_offset
                         + (uintptr_t)m_resolved.tick_interval_offset;

    *(float*)address = interval;
}

// Applies (or undoes) the blanket settings for one component. Handles both
// directions so toggling the checkboxes mid-session takes effect on the next
// pass instead of only on newly seen components.
void SimLod::apply_blanket(API::UObject* comp, CompState& state) {
    if (m_resolved.uro_prop != nullptr) {
        if (m_settings.blanket_uro && state.applied_uro != 1) {
            m_resolved.uro_prop->set_value_in_object(comp, true);
            state.applied_uro = 1;
        } else if (!m_settings.blanket_uro && state.applied_uro == 1) {
            m_resolved.uro_prop->set_value_in_object(comp, state.original_uro);
            state.applied_uro = 0;
        }
    }

    if (m_resolved.visibility_offset >= 0) {
        const auto address = (uint8_t*)((uintptr_t)comp + (uintptr_t)m_resolved.visibility_offset);

        if (m_settings.apply_visibility_option) {
            const auto option = std::clamp(m_settings.visibility_tick_option, 0, 3);

            if (state.applied_visibility_option != option) {
                *address = (uint8_t)option;
                state.applied_visibility_option = option;
            }
        } else if (state.applied_visibility_option >= 0) {
            *address = state.original_visibility_option;
            state.applied_visibility_option = -1;
        }
    }
}

float SimLod::update() {
    const auto start = Clock::now();

    const auto elapsed_ms = [&start]() {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        return (float)((double)ns / 1e6);
    };

    if (!m_settings.enabled) {
        if (m_was_enabled) {
            restore_all();
            m_was_enabled = false;
        }

        return elapsed_ms();
    }

    m_was_enabled = true;

    if (!resolve()) {
        return elapsed_ms();
    }

    const auto now = Clock::now();

    if (!m_have_refreshed || (now - m_last_refresh) >= std::chrono::milliseconds{std::max(m_settings.refresh_ms, 50)}) {
        refresh_candidates();
        m_last_refresh = now;
        m_have_refreshed = true;
    }

    if (m_candidates.empty()) {
        return elapsed_ms();
    }

    const auto forward = effective_forward();
    const auto half_angle = (double)std::clamp(m_settings.cone_half_angle_deg, 1.0f, 179.0f);
    const auto hysteresis = (double)std::clamp(m_settings.cone_hysteresis_deg, 0.0f, 45.0f);
    const auto cos_enter = std::cos(std::clamp(half_angle - hysteresis, 0.0, 180.0) * kDeg2Rad);
    const auto cos_stay = std::cos(std::clamp(half_angle + hysteresis, 0.0, 180.0) * kDeg2Rad);

    const auto budget = std::clamp(m_settings.budget_per_frame, 1, 4096);
    const auto near_radius_sq = (double)m_settings.near_radius * (double)m_settings.near_radius;
    const auto mid_radius_sq = (double)m_settings.mid_radius * (double)m_settings.mid_radius;

    size_t sampled = 0;

    for (int i = 0; i < budget; ++i) {
        if (m_cursor >= m_candidates.size()) {
            m_cursor = 0;
        }

        const auto comp = m_candidates[m_cursor++];

        if (comp == nullptr || !API::UObjectHook::exists(comp)) {
            continue;
        }

        // Never touch the player's own mesh - throttling it is instantly visible.
        if (m_local_pawn != nullptr && comp->get_outer() == m_local_pawn) {
            continue;
        }

        auto& state = m_states[comp];

        if (!state.captured) {
            const auto interval_address = (uintptr_t)comp + (uintptr_t)m_resolved.tick_function_offset
                                          + (uintptr_t)m_resolved.tick_interval_offset;

            state.original_tick_interval = *(const float*)interval_address;

            if (m_resolved.visibility_offset >= 0) {
                state.original_visibility_option =
                    *(const uint8_t*)((uintptr_t)comp + (uintptr_t)m_resolved.visibility_offset);
            }

            if (m_resolved.uro_prop != nullptr) {
                state.original_uro = m_resolved.uro_prop->get_value_from_object(comp);
            }

            state.captured = true;
            state.applied_interval = state.original_tick_interval;
        }

        apply_blanket(comp, state);

        // Without a view transform there is no cone; blanket settings still apply.
        if (!m_settings.cone_throttle || !m_have_view || m_resolved.get_location_fn == nullptr) {
            if (state.throttled) {
                write_tick_interval(comp, state.original_tick_interval);
                state.applied_interval = state.original_tick_interval;
                state.throttled = false;

                if (m_status.throttled_count > 0) {
                    --m_status.throttled_count;
                }
            }

            continue;
        }

        Vec3 position{};

        if (!component_location(comp, position)) {
            continue;
        }

        ++sampled;

        const auto delta = position - m_view_position;
        const auto distance_sq = delta.length_sq();

        bool in_cone = true;

        if (distance_sq > near_radius_sq) {
            const auto alignment = dot(normalize(delta), forward);

            // Hysteresis: a component already being throttled has to come further
            // inside the cone before it starts ticking every frame again.
            in_cone = state.throttled ? (alignment > cos_enter) : (alignment > cos_stay);
        }

        float desired = state.original_tick_interval;

        if (!in_cone) {
            desired = std::max(m_settings.far_tick_interval, state.original_tick_interval);
        } else if (distance_sq > mid_radius_sq) {
            desired = std::max(m_settings.mid_tick_interval, state.original_tick_interval);
        }

        if (std::fabs(desired - state.applied_interval) > 1e-5f) {
            write_tick_interval(comp, desired);
            state.applied_interval = desired;

            const auto now_throttled = desired > state.original_tick_interval + 1e-5f;

            if (now_throttled != state.throttled) {
                if (now_throttled) {
                    ++m_status.throttled_count;
                } else if (m_status.throttled_count > 0) {
                    --m_status.throttled_count;
                }

                state.throttled = now_throttled;
            }
        }
    }

    m_status.sampled_last_frame = sampled;
    m_status.have_view = m_have_view;
    m_status.tracked_count = m_states.size();
    m_status.last_view_yaw = m_view_yaw;

    return elapsed_ms();
}

void SimLod::restore_all() {
    size_t restored = 0;

    for (auto& [comp, state] : m_states) {
        if (!state.captured || !API::UObjectHook::exists(comp)) {
            continue;
        }

        write_tick_interval(comp, state.original_tick_interval);

        if (m_resolved.visibility_offset >= 0) {
            *(uint8_t*)((uintptr_t)comp + (uintptr_t)m_resolved.visibility_offset) = state.original_visibility_option;
        }

        if (m_resolved.uro_prop != nullptr) {
            m_resolved.uro_prop->set_value_in_object(comp, state.original_uro);
        }

        ++restored;
    }

    m_states.clear();
    m_status.throttled_count = 0;
    m_status.tracked_count = 0;

    API::get()->log_info("[perf_lod] Restored %zu components to their original tick settings", restored);
}

void SimLod::set_view(const Vec3& position, double pitch_deg, double yaw_deg) {
    m_view_position = position;
    m_view_pitch = pitch_deg;
    m_view_yaw = yaw_deg;
    m_have_view = true;
}

void SimLod::set_eye_gaze(double x, double y, double z) {
    const auto direction = normalize(Vec3{x, y, z});

    if (direction.length_sq() <= 0.0) {
        return;
    }

    m_gaze_direction = direction;
    m_gaze_timestamp = Clock::now();
    m_gaze_received = true;
}

void SimLod::on_gaze_event(const char* event_data) {
    if (event_data == nullptr) {
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    if (sscanf(event_data, "%f,%f,%f", &x, &y, &z) == 3) {
        set_eye_gaze((double)x, (double)y, (double)z);
    }
}

bool SimLod::gaze_is_fresh() const {
    if (!m_gaze_received) {
        return false;
    }

    return (Clock::now() - m_gaze_timestamp) < kGazeStaleAfter;
}

Vec3 SimLod::effective_forward() const {
    if (m_settings.gaze_source == GAZE_SOURCE_EYES && gaze_is_fresh()) {
        // OpenXR head space: +X right, +Y up, -Z forward.
        const auto yaw_offset = std::atan2(m_gaze_direction.x, -m_gaze_direction.z) / kDeg2Rad;
        const auto pitch_offset = std::asin(std::clamp(m_gaze_direction.y, -1.0, 1.0)) / kDeg2Rad;

        return rotator_to_forward(m_view_pitch + (pitch_offset * (double)m_settings.gaze_pitch_sign),
                                  m_view_yaw + (yaw_offset * (double)m_settings.gaze_yaw_sign));
    }

    return rotator_to_forward(m_view_pitch, m_view_yaw);
}
} // namespace perflod
