// UEVR perf_lod_plugin - frame/thread timing.
// Licensed under the MIT license, separate from the rest of the UEVR codebase.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace perflod {
// Measures where wall-clock frame time actually goes, from inside the process.
//
// This exists because UE Shipping builds compile out the STATS system, so
// "stat unit" is usually unavailable in a retail game - which is exactly the
// number you need to know whether the game thread is the bottleneck.
//
// game_ms  : pre_engine_tick -> post_engine_tick (game thread work)
// slate_ms : pre_slate_draw_window -> post_slate_draw_window (render thread)
// frame_ms : wall clock between consecutive pre_engine_tick calls
//
// frame_ms - game_ms is time the game thread was NOT ticking: waiting on the
// render thread, the RHI thread, the GPU, or the VR compositor.
class FrameProfiler {
public:
    using Clock = std::chrono::steady_clock;

    struct Stats {
        float fps{0.0f};
        float frame_avg{0.0f};
        float frame_p99{0.0f};
        float game_avg{0.0f};
        float game_p99{0.0f};
        float slate_avg{0.0f};
        float lod_avg{0.0f};
        float lod_max{0.0f};
        float outside_avg{0.0f};
        float game_ratio{0.0f};
        size_t sample_count{0};
    };

    // Game thread.
    void begin_tick();
    void end_tick();
    void record_lod_ms(float ms) { m_pending_lod_ms = ms; }

    // Render thread.
    void begin_slate();
    void end_slate();

    Stats compute() const;
    const char* verdict(const Stats& stats) const;

    bool csv_enabled() const { return m_csv_file != nullptr; }
    void set_csv_enabled(bool enabled);
    std::string csv_path() const { return m_csv_path; }

    void reset();

private:
    struct Sample {
        float frame_ms{0.0f};
        float game_ms{0.0f};
        float slate_ms{0.0f};
        float lod_ms{0.0f};
    };

    void push(const Sample& sample);
    void flush_csv();

    static constexpr size_t kCapacity = 720; // ~10s at 72fps

    std::vector<Sample> m_ring{};
    size_t m_head{0};

    Sample m_pending{};
    bool m_have_pending{false};

    Clock::time_point m_tick_begin{};
    Clock::time_point m_last_tick_begin{};
    bool m_have_last{false};

    float m_pending_lod_ms{0.0f};

    // Written on the render thread, read on the game thread.
    std::atomic<int64_t> m_slate_begin_ns{0};
    std::atomic<float> m_slate_ms{0.0f};

    std::FILE* m_csv_file{nullptr};
    std::string m_csv_path{};
    std::string m_csv_buffer{};
    uint64_t m_csv_frame_index{0};

    mutable std::vector<float> m_scratch{};
};
} // namespace perflod
