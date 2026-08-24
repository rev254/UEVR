/*
UEVR perf_lod_plugin

Two things in one plugin:

  1. A frame profiler that answers "is the game thread actually the
     bottleneck?" from inside the process. UE Shipping builds compile out the
     STATS system, so "stat unit" usually does not exist in a retail game.

  2. A gaze/view contingent simulation LOD throttle for skeletal mesh
     animation work, for when the answer to (1) is yes.

Licensed under the MIT license, separate from the rest of the UEVR codebase.
*/
#define _CRT_SECURE_NO_WARNINGS

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>

#include <imgui.h>

#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

#include "uevr/Plugin.hpp"

#include "FrameProfiler.hpp"
#include "PerfCommon.hpp"
#include "SimLod.hpp"

using namespace uevr;

namespace {
// Rough object counts, to see whether a location is heavy in the way you think
// it is. Off by default: counting Actors in an open world allocates a large
// vector every refresh.
struct CensusEntry {
    const wchar_t* class_path;
    const char* label;
    API::UClass* uclass;
    size_t count;
};

CensusEntry g_census[]{
    {L"Class /Script/Engine.SkeletalMeshComponent", "SkeletalMeshComponent", nullptr, 0},
    {L"Class /Script/Engine.CharacterMovementComponent", "CharacterMovementComponent", nullptr, 0},
    {L"Class /Script/Engine.AIController", "AIController", nullptr, 0},
    {L"Class /Script/Engine.ParticleSystemComponent", "ParticleSystemComponent", nullptr, 0},
    {L"Class /Script/Engine.WidgetComponent", "WidgetComponent", nullptr, 0},
    {L"Class /Script/Engine.Actor", "Actor", nullptr, 0},
};

const char* const kVisibilityOptionNames[]{
    "AlwaysTickPoseAndRefreshBones",
    "AlwaysTickPose",
    "OnlyTickMontagesWhenNotRendered",
    "OnlyTickPoseWhenRendered",
};
} // namespace

class PerfLodPlugin : public uevr::Plugin {
public:
    PerfLodPlugin() = default;

    void on_initialize() override {
        // Created here so on_message can safely query ImGui state even before
        // the first present has initialized the backends.
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        load_settings();
        API::get()->log_info("[perf_lod] Initialized. F9 = toggle sim LOD, F10 = toggle CSV capture, F11 = toggle window");
    }

    // ------------------------------------------------------------------
    // Game thread
    // ------------------------------------------------------------------
    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        m_profiler.begin_tick();

        const auto lod_ms = m_sim_lod.update();
        m_profiler.record_lod_ms(lod_ms);

        update_census();

        if (m_imgui_initialized) {
            std::scoped_lock _{m_imgui_mutex};

            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            draw_window();

            ImGui::EndFrame();
            ImGui::Render();
        }
    }

    void on_post_engine_tick(API::UGameEngine* engine, float delta) override {
        m_profiler.end_tick();
    }

    void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters,
                                             UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double) override {
        // view_index 0 is enough - both eyes share the head transform for our
        // purposes and we only need a cone, not a projection.
        if (view_index != 0 || position == nullptr || rotation == nullptr) {
            return;
        }

        perflod::Vec3 view_position{};
        double pitch = 0.0;
        double yaw = 0.0;

        if (is_double) {
            const auto position_d = (const UEVR_Vector3d*)position;
            const auto rotation_d = (const UEVR_Rotatord*)rotation;

            view_position = perflod::Vec3{position_d->x, position_d->y, position_d->z};
            pitch = rotation_d->pitch;
            yaw = rotation_d->yaw;
        } else {
            view_position = perflod::Vec3{(double)position->x, (double)position->y, (double)position->z};
            pitch = (double)rotation->pitch;
            yaw = (double)rotation->yaw;
        }

        m_sim_lod.set_view(view_position, pitch, yaw);
    }

    // Eye gaze arrives as a custom event so the gaze producer (an eye tracking
    // plugin, or a UEVR build with XR_EXT_eye_gaze_interaction wired up) stays
    // completely decoupled from this consumer.
    //
    // Expected: dispatch_custom_event("perf_lod_gaze", "x,y,z")
    // with a head-space direction in OpenXR convention (+X right, +Y up, -Z forward).
    void on_custom_event(const char* event_name, const char* event_data) override {
        if (event_name == nullptr) {
            return;
        }

        if (strcmp(event_name, "perf_lod_gaze") == 0 || strcmp(event_name, "uevr_eye_gaze") == 0) {
            m_sim_lod.on_gaze_event(event_data);
        }
    }

    // ------------------------------------------------------------------
    // Render thread
    // ------------------------------------------------------------------
    void on_pre_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
        m_profiler.begin_slate();
    }

    void on_post_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
        m_profiler.end_slate();
    }

    // ------------------------------------------------------------------
    // Rendering / input plumbing (same pattern as example_plugin)
    // ------------------------------------------------------------------
    void on_present() override {
        std::scoped_lock _{m_imgui_mutex};

        if (!m_imgui_initialized) {
            if (!initialize_imgui()) {
                return;
            }
        }

        const auto renderer_data = API::get()->param()->renderer;

        if (!API::get()->param()->vr->is_hmd_active()) {
            if (!m_was_rendering_desktop) {
                m_was_rendering_desktop = true;
                on_device_reset();
                return;
            }

            m_was_rendering_desktop = true;

            if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
                ImGui_ImplDX11_NewFrame();
                g_d3d11.render_imgui();
            } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
                if ((ID3D12CommandQueue*)renderer_data->command_queue == nullptr) {
                    return;
                }

                ImGui_ImplDX12_NewFrame();
                g_d3d12.render_imgui();
            }
        }
    }

    void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                          ID3D11RenderTargetView* rtv) override {
        if (!m_imgui_initialized || !API::get()->param()->vr->is_hmd_active()) {
            return;
        }

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX11_NewFrame();
        g_d3d11.render_imgui_vr(context, rtv);
    }

    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt,
                                          D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
        if (!m_imgui_initialized || !API::get()->param()->vr->is_hmd_active()) {
            return;
        }

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX12_NewFrame();
        g_d3d12.render_imgui_vr(command_list, rtv);
    }

    void on_device_reset() override {
        std::scoped_lock _{m_imgui_mutex};

        const auto renderer_data = API::get()->param()->renderer;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_Shutdown();
            g_d3d11 = {};
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            g_d3d12.reset();
            ImGui_ImplDX12_Shutdown();
            g_d3d12 = {};
        }

        m_imgui_initialized = false;
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override {
        if (ImGui::GetCurrentContext() == nullptr) {
            return true;
        }

        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

        if (msg == WM_KEYDOWN) {
            switch (wparam) {
            case VK_F9:
                m_sim_lod.settings().enabled = !m_sim_lod.settings().enabled;
                API::get()->log_info("[perf_lod] Simulation LOD %s", m_sim_lod.settings().enabled ? "ON" : "OFF");
                break;
            case VK_F10:
                m_profiler.set_csv_enabled(!m_profiler.csv_enabled());
                break;
            case VK_F11:
                m_window_open = !m_window_open;
                break;
            default:
                break;
            }
        }

        return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
    }

private:
    bool initialize_imgui() {
        if (m_imgui_initialized) {
            return true;
        }

        static const auto imgui_ini = API::get()->get_persistent_dir(L"imgui_perf_lod_plugin.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini.c_str();

        const auto renderer_data = API::get()->param()->renderer;

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        const auto swapchain = (IDXGISwapChain*)renderer_data->swapchain;

        if (swapchain == nullptr) {
            return false;
        }

        swapchain->GetDesc(&swap_desc);
        m_wnd = swap_desc.OutputWindow;

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            return false;
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            if (!g_d3d11.initialize()) {
                return false;
            }
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            if (!g_d3d12.initialize()) {
                return false;
            }
        }

        m_imgui_initialized = true;
        return true;
    }

    void update_census() {
        if (!m_census_enabled) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        if (m_census_stamped && (now - m_last_census) < std::chrono::milliseconds{2000}) {
            return;
        }

        m_last_census = now;
        m_census_stamped = true;

        for (auto& entry : g_census) {
            if (entry.uclass == nullptr) {
                entry.uclass = API::get()->find_uobject<API::UClass>(entry.class_path);
            }

            entry.count = entry.uclass != nullptr ? entry.uclass->get_objects_matching(false).size() : 0;
        }
    }

    void draw_window() {
        if (!m_window_open) {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2{540.0f, 0.0f}, ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("UEVR Perf / Sim LOD", &m_window_open)) {
            ImGui::End();
            return;
        }

        draw_profiler_section();
        draw_census_section();
        draw_sim_lod_section();

        ImGui::Separator();

        if (ImGui::Button("Save settings")) {
            save_settings();
        }

        ImGui::SameLine();

        if (ImGui::Button("Reload settings")) {
            load_settings();
        }

        ImGui::End();
    }

    void draw_profiler_section() {
        const auto stats = m_profiler.compute();

        if (!ImGui::CollapsingHeader("Frame timing", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        ImGui::Text("FPS %.1f   frame %.2f ms (p99 %.2f)", stats.fps, stats.frame_avg, stats.frame_p99);
        ImGui::Text("game thread  %.2f ms (p99 %.2f)  = %.0f%% of frame",
                    stats.game_avg, stats.game_p99, stats.game_ratio * 100.0f);
        ImGui::Text("outside tick %.2f ms   slate/render %.2f ms", stats.outside_avg, stats.slate_avg);
        ImGui::Text("this plugin  %.3f ms avg / %.3f ms peak", stats.lod_avg, stats.lod_max);
        ImGui::TextWrapped("%s", m_profiler.verdict(stats));

        bool csv = m_profiler.csv_enabled();

        if (ImGui::Checkbox("Capture per-frame CSV (F10)", &csv)) {
            m_profiler.set_csv_enabled(csv);
        }

        if (m_profiler.csv_enabled()) {
            ImGui::TextWrapped("Writing %s", m_profiler.csv_path().c_str());
        }

        if (ImGui::Button("Reset stats")) {
            m_profiler.reset();
        }
    }

    void draw_census_section() {
        if (!ImGui::CollapsingHeader("Object census")) {
            return;
        }

        ImGui::Checkbox("Enabled (allocates; refreshes every 2s)", &m_census_enabled);

        if (!m_census_enabled) {
            return;
        }

        for (const auto& entry : g_census) {
            if (entry.uclass == nullptr) {
                ImGui::Text("%-28s (class not found)", entry.label);
            } else {
                ImGui::Text("%-28s %zu", entry.label, entry.count);
            }
        }
    }

    void draw_sim_lod_section() {
        if (!ImGui::CollapsingHeader("Simulation LOD", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        auto& settings = m_sim_lod.settings();
        const auto& status = m_sim_lod.status();

        ImGui::Checkbox("Enabled (F9)", &settings.enabled);

        if (!status.resolved) {
            ImGui::TextWrapped("Not resolved yet: %s", status.resolve_error.empty() ? "waiting" : status.resolve_error.c_str());
        } else {
            ImGui::Text("tracked %zu / candidates %zu / throttled %zu (sampled %zu this frame)",
                        status.tracked_count, status.candidate_count, status.throttled_count,
                        status.sampled_last_frame);
        }

        ImGui::SeparatorText("Blanket (no gaze needed)");
        ImGui::Checkbox("Force bEnableUpdateRateOptimizations", &settings.blanket_uro);
        ImGui::Checkbox("Force VisibilityBasedAnimTickOption", &settings.apply_visibility_option);
        ImGui::Combo("Option", &settings.visibility_tick_option, kVisibilityOptionNames,
                     (int)(sizeof(kVisibilityOptionNames) / sizeof(kVisibilityOptionNames[0])));

        ImGui::SeparatorText("Cone throttle");
        ImGui::Checkbox("Enabled##cone", &settings.cone_throttle);

        if (settings.cone_throttle && !status.have_view) {
            ImGui::TextWrapped("No view transform received yet - cone throttling is idle "
                               "(blanket settings still apply).");
        }

        ImGui::SliderFloat("Cone half angle (deg)", &settings.cone_half_angle_deg, 10.0f, 170.0f);
        ImGui::SliderFloat("Hysteresis (deg)", &settings.cone_hysteresis_deg, 0.0f, 30.0f);
        ImGui::SliderFloat("Never throttle within (cm)", &settings.near_radius, 0.0f, 5000.0f);
        ImGui::SliderFloat("Distant radius (cm)", &settings.mid_radius, 500.0f, 20000.0f);
        ImGui::SliderFloat("Distant tick interval (s)", &settings.mid_tick_interval, 0.0f, 0.2f);
        ImGui::SliderFloat("Out-of-cone tick interval (s)", &settings.far_tick_interval, 0.0f, 0.5f);
        ImGui::SliderInt("Components per frame", &settings.budget_per_frame, 8, 1024);
        ImGui::SliderInt("Candidate refresh (ms)", &settings.refresh_ms, 100, 5000);

        ImGui::SeparatorText("Attention source");
        ImGui::Combo("Source", &settings.gaze_source, "Camera forward\0Eye gaze (custom event)\0");
        ImGui::Text("Eye gaze data: %s", m_sim_lod.gaze_is_fresh() ? "receiving" : "none");

        if (settings.gaze_source == perflod::SimLod::GAZE_SOURCE_EYES) {
            ImGui::SliderFloat("Gaze yaw sign", &settings.gaze_yaw_sign, -1.0f, 1.0f);
            ImGui::SliderFloat("Gaze pitch sign", &settings.gaze_pitch_sign, -1.0f, 1.0f);
        }

        if (ImGui::Button("Restore everything now")) {
            m_sim_lod.restore_all();
        }

        if (ImGui::TreeNode("Resolved offsets")) {
            ImGui::Text("PrimaryComponentTick +0x%x", status.tick_function_offset);
            ImGui::Text("TickInterval         +0x%x", status.tick_interval_offset);
            ImGui::Text("VisibilityBasedAnimTickOption +0x%x", status.visibility_offset);
            ImGui::Text("bEnableUpdateRateOptimizations: %s", status.has_uro_prop ? "found" : "missing");
            ImGui::Text("K2_GetComponentLocation: %s (%s precision)",
                        status.has_location_fn ? "found" : "missing",
                        status.location_is_double ? "double" : "float");
            ImGui::TreePop();
        }
    }

    // ------------------------------------------------------------------
    // Settings persistence (plain key=value, easy to hand-edit)
    // ------------------------------------------------------------------
    std::string settings_path() const {
        return API::get()->get_persistent_dir(L"perf_lod_plugin.cfg").string();
    }

    void save_settings() {
        const auto path = settings_path();
        const auto file = fopen(path.c_str(), "wb");

        if (file == nullptr) {
            API::get()->log_error("[perf_lod] Could not write %s", path.c_str());
            return;
        }

        const auto& s = m_sim_lod.settings();

        fprintf(file, "enabled=%d\n", (int)s.enabled);
        fprintf(file, "blanket_uro=%d\n", (int)s.blanket_uro);
        fprintf(file, "apply_visibility_option=%d\n", (int)s.apply_visibility_option);
        fprintf(file, "visibility_tick_option=%d\n", s.visibility_tick_option);
        fprintf(file, "cone_throttle=%d\n", (int)s.cone_throttle);
        fprintf(file, "cone_half_angle_deg=%f\n", s.cone_half_angle_deg);
        fprintf(file, "cone_hysteresis_deg=%f\n", s.cone_hysteresis_deg);
        fprintf(file, "near_radius=%f\n", s.near_radius);
        fprintf(file, "mid_radius=%f\n", s.mid_radius);
        fprintf(file, "mid_tick_interval=%f\n", s.mid_tick_interval);
        fprintf(file, "far_tick_interval=%f\n", s.far_tick_interval);
        fprintf(file, "budget_per_frame=%d\n", s.budget_per_frame);
        fprintf(file, "refresh_ms=%d\n", s.refresh_ms);
        fprintf(file, "gaze_source=%d\n", s.gaze_source);
        fprintf(file, "gaze_yaw_sign=%f\n", s.gaze_yaw_sign);
        fprintf(file, "gaze_pitch_sign=%f\n", s.gaze_pitch_sign);
        fprintf(file, "census_enabled=%d\n", (int)m_census_enabled);

        fclose(file);
        API::get()->log_info("[perf_lod] Saved %s", path.c_str());
    }

    void load_settings() {
        const auto path = settings_path();
        const auto file = fopen(path.c_str(), "rb");

        if (file == nullptr) {
            return;
        }

        auto& s = m_sim_lod.settings();
        char line[256]{};

        while (fgets(line, sizeof(line), file) != nullptr) {
            char key[128]{};
            char value[128]{};

            if (sscanf(line, "%127[^=]=%127s", key, value) != 2) {
                continue;
            }

            const auto as_int = atoi(value);
            const auto as_float = (float)atof(value);

            if (strcmp(key, "enabled") == 0) { s.enabled = as_int != 0; }
            else if (strcmp(key, "blanket_uro") == 0) { s.blanket_uro = as_int != 0; }
            else if (strcmp(key, "apply_visibility_option") == 0) { s.apply_visibility_option = as_int != 0; }
            else if (strcmp(key, "visibility_tick_option") == 0) { s.visibility_tick_option = as_int; }
            else if (strcmp(key, "cone_throttle") == 0) { s.cone_throttle = as_int != 0; }
            else if (strcmp(key, "cone_half_angle_deg") == 0) { s.cone_half_angle_deg = as_float; }
            else if (strcmp(key, "cone_hysteresis_deg") == 0) { s.cone_hysteresis_deg = as_float; }
            else if (strcmp(key, "near_radius") == 0) { s.near_radius = as_float; }
            else if (strcmp(key, "mid_radius") == 0) { s.mid_radius = as_float; }
            else if (strcmp(key, "mid_tick_interval") == 0) { s.mid_tick_interval = as_float; }
            else if (strcmp(key, "far_tick_interval") == 0) { s.far_tick_interval = as_float; }
            else if (strcmp(key, "budget_per_frame") == 0) { s.budget_per_frame = as_int; }
            else if (strcmp(key, "refresh_ms") == 0) { s.refresh_ms = as_int; }
            else if (strcmp(key, "gaze_source") == 0) { s.gaze_source = as_int; }
            else if (strcmp(key, "gaze_yaw_sign") == 0) { s.gaze_yaw_sign = as_float; }
            else if (strcmp(key, "gaze_pitch_sign") == 0) { s.gaze_pitch_sign = as_float; }
            else if (strcmp(key, "census_enabled") == 0) { m_census_enabled = as_int != 0; }
        }

        fclose(file);
        API::get()->log_info("[perf_lod] Loaded %s", path.c_str());
    }

    perflod::FrameProfiler m_profiler{};
    perflod::SimLod m_sim_lod{};

    std::recursive_mutex m_imgui_mutex{};
    HWND m_wnd{};
    bool m_imgui_initialized{false};
    bool m_was_rendering_desktop{false};
    bool m_window_open{true};

    bool m_census_enabled{false};
    bool m_census_stamped{false};
    std::chrono::steady_clock::time_point m_last_census{};
};

// UEVR instantiates this on load.
std::unique_ptr<PerfLodPlugin> g_plugin{new PerfLodPlugin()};
