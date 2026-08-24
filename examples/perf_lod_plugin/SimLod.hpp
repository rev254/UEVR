// UEVR perf_lod_plugin - gaze/view contingent simulation LOD.
// Licensed under the MIT license, separate from the rest of the UEVR codebase.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "uevr/API.hpp"

#include "PerfCommon.hpp"

namespace perflod {
// Throttles skeletal mesh animation work for actors outside the player's
// attention cone, by writing FTickFunction::TickInterval through cached
// property offsets (no reflection in the hot path).
//
// Two independent features:
//   1. Blanket URO   - forces bEnableUpdateRateOptimizations / a cheaper
//                      VisibilityBasedAnimTickOption on every skeletal mesh.
//                      Needs no gaze and no cone; usually the bigger win.
//   2. Cone throttle - raises TickInterval for meshes outside the cone.
class SimLod {
public:
    enum GazeSource : int {
        GAZE_SOURCE_VIEW = 0,  // camera forward (works everywhere)
        GAZE_SOURCE_EYES = 1,  // eye gaze fed in via a custom event
    };

    struct Settings {
        bool enabled{false};

        bool blanket_uro{true};
        bool apply_visibility_option{true};
        int visibility_tick_option{3}; // EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered

        bool cone_throttle{true};
        float cone_half_angle_deg{60.0f};
        float cone_hysteresis_deg{8.0f};
        float near_radius{800.0f};   // cm; never throttled inside this radius
        float mid_radius{4000.0f};   // cm; in-cone but distant gets mid_tick_interval
        float mid_tick_interval{0.0166f};
        float far_tick_interval{0.0333f};

        int budget_per_frame{64};
        int refresh_ms{500};

        int gaze_source{GAZE_SOURCE_VIEW};
        float gaze_yaw_sign{1.0f};
        float gaze_pitch_sign{1.0f};
    };

    struct Status {
        bool resolved{false};
        std::string resolve_error{};
        int32_t tick_function_offset{-1};
        int32_t tick_interval_offset{-1};
        int32_t visibility_offset{-1};
        bool has_uro_prop{false};
        bool has_location_fn{false};
        bool location_is_double{false};
        size_t candidate_count{0};
        size_t tracked_count{0};
        size_t throttled_count{0};
        size_t sampled_last_frame{0};
        bool have_view{false};
        double last_view_yaw{0.0};
    };

    // Fed from on_pre_calculate_stereo_view_offset (world space, game thread).
    void set_view(const Vec3& position, double pitch_deg, double yaw_deg);

    // Head-space gaze direction, OpenXR convention (X right, Y up, -Z forward).
    void set_eye_gaze(double x, double y, double z);
    void on_gaze_event(const char* event_data);

    // Game thread; call once per engine tick. Returns milliseconds spent.
    float update();

    void restore_all();

    Settings& settings() { return m_settings; }
    const Settings& settings() const { return m_settings; }
    const Status& status() const { return m_status; }

    Vec3 effective_forward() const;
    bool gaze_is_fresh() const;

private:
    using Clock = std::chrono::steady_clock;

    struct CompState {
        bool captured{false};
        float original_tick_interval{0.0f};
        uint8_t original_visibility_option{0};
        bool original_uro{false};
        int applied_uro{-1};               // -1 untouched, 0 restored, 1 forced on
        int applied_visibility_option{-1}; // -1 untouched, else the value written
        float applied_interval{-1.0f};
        bool throttled{false};
    };

    struct Resolved {
        uevr::API::UClass* skeletal_mesh_class{nullptr};
        int32_t tick_function_offset{-1};
        int32_t tick_interval_offset{-1};
        int32_t visibility_offset{-1};
        uevr::API::FBoolProperty* uro_prop{nullptr};
        uevr::API::UFunction* get_location_fn{nullptr};
        size_t location_params_size{0};
        int32_t location_return_offset{-1};
        bool location_is_double{false};
    };

    bool resolve();
    void refresh_candidates();
    bool component_location(uevr::API::UObject* comp, Vec3& out);
    void apply_blanket(uevr::API::UObject* comp, CompState& state);
    void write_tick_interval(uevr::API::UObject* comp, float interval);

    Settings m_settings{};
    Status m_status{};
    Resolved m_resolved{};
    bool m_resolve_failed{false};

    std::vector<uevr::API::UObject*> m_candidates{};
    std::unordered_map<uevr::API::UObject*, CompState> m_states{};
    std::vector<uint8_t> m_params{};
    size_t m_cursor{0};

    Clock::time_point m_last_refresh{};
    bool m_have_refreshed{false};

    Vec3 m_view_position{};
    bool m_have_view{false};
    double m_view_pitch{0.0};
    double m_view_yaw{0.0};

    Vec3 m_gaze_direction{0.0, 0.0, -1.0};
    Clock::time_point m_gaze_timestamp{};
    bool m_gaze_received{false};

    uevr::API::UObject* m_local_pawn{nullptr};
    bool m_was_enabled{false};
};
} // namespace perflod
