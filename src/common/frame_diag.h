// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Per-frame handoff and GPU-stage profiling.
//
// Entirely gated behind the EDEN_FRAME_DIAG compile-time flag (CMake option, default OFF). When the
// flag is not set every macro below expands to nothing, so normal builds carry zero cost and
// identical codegen. When set, a runtime capture window (driven by Settings::values.frame_diag_capture)
// records latency histograms for the stages in `Stage`, attributes event counts per game frame, and
// periodically dumps count + p50/p95/p99 to the log. See docs/pixel10/overworld_latency_analysis.md.

#include "common/common_types.h"

namespace Common::FrameDiag {

enum class Stage : u32 {
    HandoffEndWaitToSwitch, // KThread::EndWait -> KScheduler::SwitchThread (reproduces the old ~20us)
    HandoffEndWaitToGuest,  // KThread::EndWait -> thread about to re-enter guest (full runnable->run)
    SchedReloadContext,     // KScheduler::Reload -> PhysicalCore::LoadContext
    FiberYield,             // Common::Fiber::YieldTo host fiber switch
    NceSignalReturn,        // count: guest resumes via the SIGUSR2 self-signal path
    NceTrampolineReturn,    // count: guest resumes via the branchless trampoline path
    SchedLockWait,          // global scheduler lock acquire wait
    SchedLockHold,          // global scheduler lock hold duration
    GpuBatchProcess,        // GPU command-thread: dequeue -> dispatch of one batch
    VkPipelineLookup,       // vk_rasterizer PrepareDraw: CurrentGraphicsPipeline()
    VkDescriptorConfigure,  // vk_rasterizer PrepareDraw: pipeline->Configure()
    VkSubmitExecution,      // vk_scheduler SubmitExecution record+submit
    VkWorkerExecute,        // vk_scheduler worker: CommandChunk::ExecuteAll
    Composite,              // renderer_vulkan Composite
    Present,                // renderer_vulkan present_manager.Present
    Count,
};

} // namespace Common::FrameDiag

#ifdef EDEN_FRAME_DIAG

#include "common/steady_clock.h"

namespace Common::FrameDiag {

/// True while a capture window is live. Cheap (relaxed atomic load); gate every stamp on it.
[[nodiscard]] bool IsCapturing() noexcept;

/// Record a single latency sample (nanoseconds) for `stage`.
void RecordNs(Stage stage, s64 ns) noexcept;

/// Increment the event counter for `stage` without a latency sample.
void Count(Stage stage) noexcept;

/// Called once per game frame (at the render frame-end boundary). Drives capture start/stop from the
/// setting, snapshots per-frame event deltas, and periodically dumps the current window.
void OnFrameBoundary() noexcept;

/// Flush any partial window. Call on emulation shutdown.
void Shutdown() noexcept;

/// RAII latency timer. Reads the monotonic clock at construction and destruction and records the
/// delta into `stage`'s histogram, but only if a capture is live at construction time.
class ScopeTimer {
public:
    explicit ScopeTimer(Stage stage_) noexcept
        : stage{stage_}, active{IsCapturing()},
          start_ns{active ? Common::SteadyClock::Now().time_since_epoch().count() : 0} {}

    ~ScopeTimer() noexcept {
        if (active) {
            const s64 end_ns = Common::SteadyClock::Now().time_since_epoch().count();
            RecordNs(stage, end_ns - start_ns);
        }
    }

    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;

private:
    Stage stage;
    bool active;
    s64 start_ns;
};

} // namespace Common::FrameDiag

#define EDEN_DIAG_CONCAT_(a, b) a##b
#define EDEN_DIAG_CONCAT(a, b) EDEN_DIAG_CONCAT_(a, b)

#define EDEN_DIAG_SCOPE(stage)                                                                     \
    ::Common::FrameDiag::ScopeTimer EDEN_DIAG_CONCAT(eden_diag_scope_, __LINE__) { stage }
#define EDEN_DIAG_COUNT(stage) ::Common::FrameDiag::Count(stage)
#define EDEN_DIAG_FRAME_BOUNDARY() ::Common::FrameDiag::OnFrameBoundary()
#define EDEN_DIAG_SHUTDOWN() ::Common::FrameDiag::Shutdown()

// Cross-function span: stash a start stamp now, record the elapsed time into `stage` later. `slot` is
// an lvalue s64 (e.g. a field on KThread) that lives from begin to end.
#define EDEN_DIAG_SPAN_BEGIN(slot)                                                                 \
    do {                                                                                           \
        if (::Common::FrameDiag::IsCapturing()) {                                                  \
            (slot) = ::Common::SteadyClock::Now().time_since_epoch().count();                      \
        } else {                                                                                   \
            (slot) = 0;                                                                            \
        }                                                                                          \
    } while (0)
#define EDEN_DIAG_SPAN_END(slot, stage)                                                            \
    do {                                                                                           \
        if ((slot) != 0) {                                                                         \
            const s64 eden_diag_now = ::Common::SteadyClock::Now().time_since_epoch().count();     \
            ::Common::FrameDiag::RecordNs(stage, eden_diag_now - (slot));                           \
        }                                                                                          \
    } while (0)

#else // !EDEN_FRAME_DIAG

#define EDEN_DIAG_SCOPE(stage) ((void)0)
#define EDEN_DIAG_COUNT(stage) ((void)0)
#define EDEN_DIAG_FRAME_BOUNDARY() ((void)0)
#define EDEN_DIAG_SHUTDOWN() ((void)0)
#define EDEN_DIAG_SPAN_BEGIN(slot) ((void)0)
#define EDEN_DIAG_SPAN_END(slot, stage) ((void)0)

#endif // EDEN_FRAME_DIAG
