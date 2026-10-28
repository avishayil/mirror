// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/frame_diag.h"

#ifdef EDEN_FRAME_DIAG

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "common/steady_clock.h"

namespace Common::FrameDiag {

namespace {

constexpr std::size_t kNumStages = static_cast<std::size_t>(Stage::Count);
// Bucket b covers [2^(b-1), 2^b) ns; bucket 0 holds zero/negative samples. 40 buckets reach ~9 min.
constexpr int kNumBuckets = 40;
// Per-frame event counts retained for computing per-frame distributions over a window.
constexpr std::size_t kFrameRing = 1024;
// Wall-clock length of one auto-dumped capture window.
constexpr s64 kWindowNs = 15'000'000'000; // 15 s

constexpr std::array<const char*, kNumStages> kStageNames{
    "HandoffEndWaitToSwitch", "HandoffEndWaitToGuest", "SchedReloadContext",
    "FiberYield",             "NceSignalReturn",       "NceTrampolineReturn",
    "SchedLockWait",          "SchedLockHold",         "GpuBatchProcess",
    "VkPipelineLookup",       "VkDescriptorConfigure", "VkSubmitExecution",
    "VkWorkerExecute",        "Composite",             "Present",
};

struct StageData {
    std::atomic<u64> count;
    std::atomic<u64> sum_ns;
    std::array<std::atomic<u64>, kNumBuckets> hist;
};

std::array<StageData, kNumStages> g_stages;
std::atomic<bool> g_capture_active{false};

// Touched only by the frame-end thread (OnFrameBoundary / Start / Stop / Dump).
std::array<std::array<u32, kNumStages>, kFrameRing> g_frame_counts;
std::array<u64, kNumStages> g_last_snapshot;
std::size_t g_frame_idx;
std::size_t g_window_frames;
s64 g_window_start_ns;

int BucketOf(s64 ns) noexcept {
    if (ns <= 0) {
        return 0;
    }
    const int b = std::bit_width(static_cast<u64>(ns));
    return std::min(b, kNumBuckets - 1);
}

s64 NowNs() noexcept {
    return Common::SteadyClock::Now().time_since_epoch().count();
}

void ResetCounters() noexcept {
    for (auto& s : g_stages) {
        s.count.store(0, std::memory_order_relaxed);
        s.sum_ns.store(0, std::memory_order_relaxed);
        for (auto& h : s.hist) {
            h.store(0, std::memory_order_relaxed);
        }
    }
    g_last_snapshot.fill(0);
    g_frame_idx = 0;
    g_window_frames = 0;
    g_window_start_ns = NowNs();
}

// p-th percentile latency (ns) from a stage's log-scale histogram, reported at the bucket lower edge.
s64 HistogramPercentile(const StageData& s, double p) noexcept {
    u64 total = 0;
    std::array<u64, kNumBuckets> snap{};
    for (int b = 0; b < kNumBuckets; ++b) {
        snap[b] = s.hist[b].load(std::memory_order_relaxed);
        total += snap[b];
    }
    if (total == 0) {
        return 0;
    }
    const u64 threshold = static_cast<u64>(static_cast<double>(total) * p);
    u64 cumulative = 0;
    for (int b = 0; b < kNumBuckets; ++b) {
        cumulative += snap[b];
        if (cumulative >= threshold) {
            return b == 0 ? 0 : (s64{1} << (b - 1));
        }
    }
    return s64{1} << (kNumBuckets - 2);
}

// p-th percentile of the per-frame event count for a stage, over the retained frames.
u32 PerFramePercentile(std::size_t stage, double p) noexcept {
    const std::size_t n = std::min(g_window_frames, kFrameRing);
    if (n == 0) {
        return 0;
    }
    std::vector<u32> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = g_frame_counts[i][stage];
    }
    std::sort(v.begin(), v.end());
    const std::size_t idx = std::min(n - 1, static_cast<std::size_t>(static_cast<double>(n) * p));
    return v[idx];
}

void DumpWindow(const char* label) noexcept {
    const s64 now = NowNs();
    const double secs = static_cast<double>(now - g_window_start_ns) / 1e9;
    LOG_INFO(Common, "[frame_diag] ==== {} window: {} frames over {:.2f}s ({:.1f} fps) ====", label,
             g_window_frames, secs, secs > 0 ? g_window_frames / secs : 0.0);
    for (std::size_t i = 0; i < kNumStages; ++i) {
        const auto& s = g_stages[i];
        const u64 count = s.count.load(std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        const u64 sum_ns = s.sum_ns.load(std::memory_order_relaxed);
        const double rate = secs > 0 ? static_cast<double>(count) / secs : 0.0;
        const double per_frame =
            g_window_frames > 0 ? static_cast<double>(count) / g_window_frames : 0.0;
        if (sum_ns > 0) {
            LOG_INFO(Common,
                     "[frame_diag] {:<22} n={:<10} {:>8.1f}/s {:>6.1f}/frame  mean={}ns "
                     "p50={}ns p95={}ns p99={}ns",
                     kStageNames[i], count, rate, per_frame, sum_ns / count,
                     HistogramPercentile(s, 0.50), HistogramPercentile(s, 0.95),
                     HistogramPercentile(s, 0.99));
        } else {
            LOG_INFO(Common,
                     "[frame_diag] {:<22} n={:<10} {:>8.1f}/s {:>6.1f}/frame  "
                     "perframe_p50={} p95={} p99={}",
                     kStageNames[i], count, rate, per_frame, PerFramePercentile(i, 0.50),
                     PerFramePercentile(i, 0.95), PerFramePercentile(i, 0.99));
        }
    }
}

} // namespace

bool IsCapturing() noexcept {
    return g_capture_active.load(std::memory_order_relaxed);
}

void RecordNs(Stage stage, s64 ns) noexcept {
    if (!IsCapturing()) {
        return;
    }
    auto& s = g_stages[static_cast<std::size_t>(stage)];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.sum_ns.fetch_add(static_cast<u64>(std::max<s64>(ns, 0)), std::memory_order_relaxed);
    s.hist[BucketOf(ns)].fetch_add(1, std::memory_order_relaxed);
}

void Count(Stage stage) noexcept {
    if (!IsCapturing()) {
        return;
    }
    g_stages[static_cast<std::size_t>(stage)].count.fetch_add(1, std::memory_order_relaxed);
}

void OnFrameBoundary() noexcept {
    const bool want = Settings::values.frame_diag_capture.GetValue();
    const bool active = g_capture_active.load(std::memory_order_relaxed);
    if (want && !active) {
        ResetCounters();
        g_capture_active.store(true, std::memory_order_relaxed);
        LOG_INFO(Common, "[frame_diag] capture started");
    } else if (!want && active) {
        DumpWindow("final");
        g_capture_active.store(false, std::memory_order_relaxed);
        LOG_INFO(Common, "[frame_diag] capture stopped");
        return;
    }
    if (!g_capture_active.load(std::memory_order_relaxed)) {
        return;
    }

    // Snapshot this frame's per-stage event deltas into the ring.
    auto& row = g_frame_counts[g_frame_idx];
    for (std::size_t i = 0; i < kNumStages; ++i) {
        const u64 cur = g_stages[i].count.load(std::memory_order_relaxed);
        row[i] = static_cast<u32>(cur - g_last_snapshot[i]);
        g_last_snapshot[i] = cur;
    }
    g_frame_idx = (g_frame_idx + 1) % kFrameRing;
    ++g_window_frames;

    if (NowNs() - g_window_start_ns >= kWindowNs) {
        DumpWindow("auto");
        ResetCounters();
    }
}

void Shutdown() noexcept {
    if (g_capture_active.load(std::memory_order_relaxed)) {
        DumpWindow("shutdown");
        g_capture_active.store(false, std::memory_order_relaxed);
    }
}

} // namespace Common::FrameDiag

#endif // EDEN_FRAME_DIAG
