// UEVR perf_lod_plugin - frame/thread timing.
// Licensed under the MIT license, separate from the rest of the UEVR codebase.
#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <cstdarg>

#include "uevr/API.hpp"

#include "FrameProfiler.hpp"

using namespace uevr;

namespace perflod {
namespace {
float percentile(std::vector<float>& values, float fraction) {
    if (values.empty()) {
        return 0.0f;
    }

    auto index = (size_t)(fraction * (float)(values.size() - 1));
    index = std::min(index, values.size() - 1);

    std::nth_element(values.begin(), values.begin() + index, values.end());

    return values[index];
}

float average(const std::vector<float>& values) {
    if (values.empty()) {
        return 0.0f;
    }

    double sum = 0.0;

    for (const auto value : values) {
        sum += value;
    }

    return (float)(sum / (double)values.size());
}
} // namespace

void FrameProfiler::begin_tick() {
    const auto now = Clock::now();

    m_tick_begin = now;

    if (m_have_last) {
        const auto frame_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_last_tick_begin).count();
        m_pending.frame_ms = (float)((double)frame_ns / 1e6);
        m_have_pending = true;
    }

    m_last_tick_begin = now;
    m_have_last = true;
    m_pending_lod_ms = 0.0f;
}

void FrameProfiler::end_tick() {
    if (!m_have_pending) {
        return;
    }

    const auto game_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_tick_begin).count();

    m_pending.game_ms = (float)((double)game_ns / 1e6);
    m_pending.slate_ms = m_slate_ms.load(std::memory_order_relaxed);
    m_pending.lod_ms = m_pending_lod_ms;

    push(m_pending);

    m_pending = Sample{};
    m_have_pending = false;
}

void FrameProfiler::begin_slate() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    m_slate_begin_ns.store(ns, std::memory_order_relaxed);
}

void FrameProfiler::end_slate() {
    const auto begin_ns = m_slate_begin_ns.load(std::memory_order_relaxed);

    if (begin_ns == 0) {
        return;
    }

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    m_slate_ms.store((float)((double)(now_ns - begin_ns) / 1e6), std::memory_order_relaxed);
}

void FrameProfiler::push(const Sample& sample) {
    if (m_ring.size() < kCapacity) {
        m_ring.push_back(sample);
    } else {
        m_ring[m_head] = sample;
        m_head = (m_head + 1) % kCapacity;
    }

    if (m_csv_file != nullptr) {
        char line[192]{};
        const auto written = snprintf(line, sizeof(line), "%llu,%.4f,%.4f,%.4f,%.4f\n",
                                      (unsigned long long)m_csv_frame_index++,
                                      sample.frame_ms, sample.game_ms, sample.slate_ms, sample.lod_ms);

        if (written > 0) {
            m_csv_buffer.append(line, (size_t)written);
        }

        // Flush in chunks so the file write cost lands once every few seconds
        // instead of every frame.
        if (m_csv_buffer.size() >= 32768) {
            flush_csv();
        }
    }
}

void FrameProfiler::flush_csv() {
    if (m_csv_file == nullptr || m_csv_buffer.empty()) {
        return;
    }

    fwrite(m_csv_buffer.data(), 1, m_csv_buffer.size(), m_csv_file);
    fflush(m_csv_file);
    m_csv_buffer.clear();
}

void FrameProfiler::set_csv_enabled(bool enabled) {
    if (enabled == (m_csv_file != nullptr)) {
        return;
    }

    if (!enabled) {
        flush_csv();
        fclose(m_csv_file);
        m_csv_file = nullptr;
        API::get()->log_info("[perf_lod] Stopped frame CSV capture (%s)", m_csv_path.c_str());
        return;
    }

    m_csv_path = API::get()->get_persistent_dir(L"perf_lod_frames.csv").string();
    m_csv_file = fopen(m_csv_path.c_str(), "wb");

    if (m_csv_file == nullptr) {
        API::get()->log_error("[perf_lod] Failed to open %s for writing", m_csv_path.c_str());
        return;
    }

    m_csv_frame_index = 0;
    m_csv_buffer.clear();
    m_csv_buffer.reserve(65536);
    m_csv_buffer.append("frame,frame_ms,game_ms,slate_ms,lod_ms\n");

    API::get()->log_info("[perf_lod] Started frame CSV capture (%s)", m_csv_path.c_str());
}

FrameProfiler::Stats FrameProfiler::compute() const {
    Stats stats{};

    if (m_ring.empty()) {
        return stats;
    }

    stats.sample_count = m_ring.size();

    m_scratch.clear();
    m_scratch.reserve(m_ring.size());

    double frame_sum = 0.0;
    double game_sum = 0.0;
    double slate_sum = 0.0;
    double lod_sum = 0.0;
    float lod_max = 0.0f;

    for (const auto& sample : m_ring) {
        frame_sum += sample.frame_ms;
        game_sum += sample.game_ms;
        slate_sum += sample.slate_ms;
        lod_sum += sample.lod_ms;
        lod_max = std::max(lod_max, sample.lod_ms);
        m_scratch.push_back(sample.frame_ms);
    }

    const auto count = (double)m_ring.size();

    stats.frame_avg = (float)(frame_sum / count);
    stats.game_avg = (float)(game_sum / count);
    stats.slate_avg = (float)(slate_sum / count);
    stats.lod_avg = (float)(lod_sum / count);
    stats.lod_max = lod_max;
    stats.fps = stats.frame_avg > 0.0f ? (1000.0f / stats.frame_avg) : 0.0f;
    stats.outside_avg = stats.frame_avg - stats.game_avg;
    stats.game_ratio = stats.frame_avg > 0.0f ? (stats.game_avg / stats.frame_avg) : 0.0f;
    stats.frame_p99 = percentile(m_scratch, 0.99f);

    m_scratch.clear();

    for (const auto& sample : m_ring) {
        m_scratch.push_back(sample.game_ms);
    }

    stats.game_p99 = percentile(m_scratch, 0.99f);

    return stats;
}

const char* FrameProfiler::verdict(const Stats& stats) const {
    if (stats.sample_count < 60 || stats.frame_avg <= 0.0f) {
        return "collecting samples...";
    }

    if (stats.game_ratio >= 0.85f) {
        return "GAME THREAD BOUND - the tick itself is the frame. Simulation LOD can help.";
    }

    if (stats.game_ratio <= 0.55f) {
        return "NOT game thread bound - most of the frame is spent outside the tick "
               "(GPU, render/RHI thread, or compositor wait). Try sync/rendering mode first.";
    }

    return "MIXED - the tick is a large but not dominant share of the frame.";
}

void FrameProfiler::reset() {
    m_ring.clear();
    m_head = 0;
    m_have_last = false;
    m_have_pending = false;
    m_pending = Sample{};
    m_pending_lod_ms = 0.0f;
}
} // namespace perflod
