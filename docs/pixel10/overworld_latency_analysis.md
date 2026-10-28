# LGP overworld ~20 FPS on Pixel 10 Pro — architectural analysis (chased to the end)

Target: Pokémon: Let's Go overworld, ~20 FPS (native 30, 60 with the FPS mod). Everything below is
on-device measured (temporary instrumentation, since reverted) on the Pixel 10 Pro XL
(Tensor G5 + Imagination PowerVR DXT-48).

## Every single-resource bottleneck is ruled out

| Suspect | Measurement | Verdict |
|---|---|---|
| GPU fill/pixels | 2× internal resolution (4× pixels) → 16 FPS ≈ native | **Not GPU fill-bound** |
| GPU clock/throughput | TurboMode boosted GPU 396→864 MHz (2.2×) → FPS unchanged at 15 | **Not GPU-bound at all** |
| Present / swapchain | per-frame: acquire ~40µs, present_done ~19µs, swapchain gate <1µs | negligible |
| Compositor / vsync | vsync signalled **~29,000×/sec**; `ComposeOnDisplay` ~0µs | not a gate (unlocked) |
| Guest GPU-fence waits | `IocCtrlEventWait` registers a host action: **0** hits/this session (SYNCWAIT=0); ioctl calls ~28/s; timeout=0 polls 0 | guest barely touches it |
| Guest buffer dequeue | `WaitForDequeueCondition` blocked time = **0** | not buffer-starved |
| Guest sleeps | `svcSleepThread(ns>0)` = **0/s**; yields `(ns≤0)` = **0/s** | no self-pacing / spin |
| Guest sync waits | `svcWaitSynchronization` ~80/s but **~1 ms/s total** blocked | non-blocking |
| CPU saturation | whole emulator uses **~1.25 of 8 cores**; ~4.6 cores idle; no thread >~31% | not CPU-throughput-bound |
| CPU clock | prime cores clocked **low** during play (cpu7 800 MHz / 3782; cpu6 533 MHz); mid cores moderate (cpu4 2534) | governor not boosting |

## The architectural gap

The frame is **~48 ms** and *no* resource is busy for it — CPU cores, GPU, present threads all sit
~75% idle, and the load-based governors (GPU `precise_ondemand`, CPU `sched_pixel`) therefore keep
clocks low. This is the signature of a **latency-bound serialized pipeline**, not a throughput
bottleneck:

- The guest produces each frame through a dependency chain — game-logic thread → render/submit
  thread → emulated GPU → back — where each stage does a short burst, then hands off to the next.
- The stages **do not overlap** (data dependencies), so frame time ≈ the *sum* of the short
  segments plus the cross-thread/cross-unit handoff latencies, while each unit is mostly idle.
- Because no single unit sees sustained load, neither the CPU prime cores nor the GPU boost — which
  *lengthens* every segment, compounding the latency.
- This is why boosting the GPU clock in isolation did nothing: the GPU is not on the critical
  segment long enough for its clock to matter; the critical path is the serialized hand-offs.

In short: on a big.LITTLE mobile SoC with aggressive power governors, Eden's finely-threaded,
dependency-serialized per-frame pipeline for this title can't keep any unit saturated, so clocks
stay low and the handoff latencies dominate — yielding ~20 FPS with the hardware ~75% idle. Part of
this is inherent to the game's serial dependency structure; part is addressable in the emulator
(keep clocks up during active emulation; cut handoff latency; overlap stages).

## Why the obvious levers don't apply
- Can't raise clocks from outside: GPU/CPU `devfreq`/`cpufreq` sysfs nodes are **root-only**; device
  is stock/non-rooted. Only the app can influence clocks (ADPF hints, sustained load).
- `use_speed_limit=false` already runs the frame timer unlocked (vsync 29 kHz) — not a cap.
- GPU TurboMode (now feature-flagged for PowerVR) boosts the GPU but not the CPU critical path.

## LOCATED (2026-10-28): guest inter-thread synchronization is the ~48ms

The missing per-frame time was invisible to the earlier probes because it is **not** in sleep, GPU
fences, WaitSynchronization, dequeue, or vsync — it is in the guest's **mutex + condition-variable**
syscalls (which block a thread without a sleep/WaitSync, matching the "low CPU%, nothing saturated"
paradox). Measured in the overworld (per wall-second, summed across guest threads):

| Primitive (svc) | rate | blocked time | per-op avg |
|---|---|---|---|
| Condition variable (`WaitProcessWideKeyAtomic`) | ~1,300/s (~65/frame) | **~5,000 ms/s** | ~3.85 ms/wait |
| Contended mutex (`ArbitrateLock`) | ~16,000/s (~800/frame) | ~900 ms/s | **~56 µs/lock** |
| Address arbiter (`WaitForAddress`) | 0 | 0 | — |

The LGP engine is heavily multithreaded with fine-grained locking + condvar handoffs. Two signals
of **emulation overhead** (vs. inherent game cost):
- **~56 µs per *contended* `ArbitrateLock`** — native contended lock is ~1–10 µs; the ~5–50× inflation
  is HLE-kernel + NCE thread park/wake overhead, multiplied by ~800 contended locks/frame ≈ ~45 ms/frame.
- **~3.85 ms average condvar wait** — far above a native µs-scale wakeup; much of it is likely
  signal→reschedule→NCE-resume wakeup latency, ×~65 waits/frame.

So the frame is a serial chain of guest-thread handoffs whose **per-handoff HLE/NCE wakeup latency**
is the dominant cost. This is the concrete, located architectural gap. Candidate emulator-side fixes
(for review): reduce the HLE kernel's `svcArbitrateLock`/condition-variable fast-path and
thread-wakeup latency (KScheduler reschedule + host futex/cv wake under NCE); investigate whether a
userspace fast-path can resolve more uncontended/contended locks without a full syscall+reschedule;
assess core-affinity/co-scheduling of the contending guest threads to cut cross-core wakeup latency.
All deep, correctness-sensitive HLE-kernel work — out of scope to land here; flagged for the reviewer.

## Reviewer-roadmap results (2026-10-28): clocks ruled out on BOTH CPU and GPU

Acting on the external review (P0-P2):
- **Opportunity A (CPU-core placement) — disproven.** Actual core per guest thread
  (`/proc/<tid>/stat` field 39) in the overworld: all emulation threads run on the **big cluster
  (cpu2-7)**, not the little cores (0-1) — e.g. CPUCore_0 observed on 5/6/7, GPU/VulkanWorker on 6-7.
  They are *not* mis-placed on efficiency cores. They do **migrate heavily** across the big cluster
  every sample (a secondary cost), and those big cores are the ones clocked low.
- **Opportunity B (unlocked emulated-vsync) / CPU clock — ruled out for FPS.** Setting
  `use_speed_limit=100%` (vs unlocked) made the governor boost the prime cores **800 MHz → ~3.4 GHz
  (cpu7), 533 → up to 3.0 GHz (cpu6)** — a ~4× CPU clock increase — yet overworld **FPS stayed 19-20,
  unchanged.** So the unlocked 29 kHz vsync was suppressing CPU clocks, but CPU clock is **not** the
  FPS limiter.

**Combined with the earlier GPU-clock result (396→864 MHz, FPS unchanged), two independent clock
boosts — CPU and GPU — produced zero FPS change.** This is decisive: the overworld is **purely
latency-bound**, not clock/throughput-bound on either processor. The reviewer's caution holds — the
large summed sync blocked-time is dominated by *latency* (dependency waits + handoff/wakeup
round-trips), which faster clocks do not shorten.

**Open (P3, the decisive measurement):** split the ~3.85 ms average condvar wait into legitimate
waiting-for-the-producer vs emulator/host wakeup latency (runnable→running). Only the latter is
fixable (P4/P5); if the former dominates, the overworld limit is inherent to emulating this engine's
thread structure. Method: per-guest-thread runnable→running latency (KScheduler/KThread, sampled
histograms) + a Perfetto sched trace.

### P3 result (2026-10-28): wakeup is fast — it's handoff VOLUME × fixed per-switch cost

Instrumented runnable→running latency (stamp at `KThread::EndWait`, measure at
`KScheduler::SwitchThread`): in the overworld, **~16,000-18,000 wakes/s, average runnable→running
= ~20 µs** (rare max spikes 5-54 ms). So a woken guest thread runs ~20 µs later — the emulator/host
scheduler is **not** slow to run woken threads. This **closes the reviewer's P4/P5 "scheduler wakeup
latency" direction**: the ~3.85 ms condvar waits are not dominated by runnable→running overhead.

Reframed bottleneck: ~800 guest thread handoffs/frame (the ~16k wakes/s ÷ ~20 fps) × ~20 µs fixed
per-handoff HLE cost (wakeup + scheduler lock + fiber/NCE context restore) ≈ tens of ms/frame, fully
serialized along the engine's dependency chain. This is consistent with *every* prior result:
clock-insensitive (both CPU and GPU boosts did nothing — a fixed per-switch cost doesn't scale with
clock), GPU idle, and fast-per-wakeup-but-very-many. ~20 µs/context-switch is ~10× a native futex
wake (~1-2 µs), so the lever — if any — is **reducing the per-handoff emulation cost** (HLE
`KScheduler`/fiber-switch/NCE context save-restore), i.e. the reviewer's P4, but applied to
*throughput of switches* rather than *latency of one wait*. That is deep, correctness-sensitive
kernel/NCE work with an uncertain floor (~20 µs may be near the practical minimum), and reducing the
*count* of handoffs isn't possible (it's the game's threading).

### CORRECTION (2026-10-28): the ~20 µs measurement stopped too early; NCE vs Dynarmic overworld A/B

A second external review flagged that the "~20 µs runnable→running" stamp ended at
`KScheduler::SwitchThread`, which only sets the current-thread pointer — it runs **before**
`KScheduler::Reload` (guest-context restore via `PhysicalCore::LoadContext`), the boost.context
`Common::Fiber::YieldTo` fiber switch, and the NCE guest re-entry. Under NCE the common overworld
resume takes the **signal path** `ArmNce::ReturnToRunCodeByExceptionLevelChange`, a self-`tkill` of
SIGUSR2 + signal-handler context restore (`arm_nce.s`), none of which the ~20 µs captures. So the
earlier "inherent" conclusion rested on a floor that excludes the fiber + NCE-resume tail.

Decisive free test — matched overworld A/B, same scene (Pallet Town), cooled, 60 fps mod off,
prime cores confirmed un-clamped at 2.7–3.2 GHz in **both** runs (so not a clock artifact):

| cpu_backend | overworld standing | walking |
|---|---|---|
| **Dynarmic** (JIT, no per-handoff signal) | ~24–27 FPS (median ~25) | ~28 FPS |
| **NCE** (SIGUSR2 self-signal per resume) | ~19–20.5 FPS (median ~20) | ~20 FPS |

Dynarmic is **~25 % faster standing and ~40 % faster walking** than NCE at matched clocks. NCE's
~20 FPS is exactly the figure the whole investigation was built on. Since Dynarmic avoids the
per-handoff SIGUSR2 round-trip and gains ~25–40 %, the NCE guest-resume path **is a material,
addressable cost** — the overworld is **not** simply "inherent." (Dynarmic still falls short of 30,
so a latency-bound remainder common to both backends persists — HLE scheduler handoffs + CPU-side
GPU submission.) Minor caveat: the NCE leg ran ~1.6 °C hotter (37.3 vs 35.7 °C) having run second,
but clocks held in both, far too small to explain a 25–40 % gap.

Next: the `EDEN_FRAME_DIAG` profiler (see `REVIEW.md` §6, `common/frame_diag.h`) measures the full
handoff past `SwitchThread` (Reload + fiber + guest-entry) and the CPU-side GPU stages per frame, to
quantify how much of the NCE penalty is the signal mechanism vs. the common HLE/GPU remainder, and
pick one fix. The problem is therefore **unresolved, two leading causes under investigation**, not
inherent.

### MEASURED (2026-10-28): EDEN_FRAME_DIAG capture — the bottleneck is CPU-side Vulkan, NOT the handoff

Instrumented build (`-DEDEN_FRAME_DIAG=ON`), overworld walking, 15 s auto-dump windows. Representative
per-frame figures (NCE @ 20.4 fps ≈ 49 ms; Dynarmic @ 25.8 fps ≈ 39 ms):

| stage | NCE /frame × mean | Dynarmic /frame × mean |
|---|---|---|
| **GpuBatchProcess** (GPU thread: process the frame's command list) | 1 × **~12.7 ms** | 1 × **~23 ms** |
| **VkWorkerExecute** (VulkanWorker: replay + `vkQueueSubmit` to PowerVR) | ~144 × 118 µs ≈ **17 ms** | ~153 × 162 µs ≈ **25 ms** |
| HandoffEndWaitToGuest (runnable→guest, full path) | ~730 × **25 µs** | ~880 × 23 µs |
| HandoffEndWaitToSwitch (old endpoint) | ~730 × 21 µs | ~860 × 22 µs |
| SchedReloadContext | ~1500 × 0.2 µs | ~1700 × 0.2 µs |
| VkDescriptorConfigure (per-draw bind) | ~545 × 3.2 µs ≈ 1.8 ms | ~585 × 3.8 µs ≈ 2.2 ms |
| SchedLockWait / Hold | ~2800 × 1.5 µs / 1.3 µs | ~3100 × 1.7 µs / 1.1 µs |
| **NceSignalReturn** (SIGUSR2 self-signal resume) | **~4 /frame** | 0 |
| NceTrampolineReturn (branchless resume) | ~2600 /frame | — |

**Two hypotheses killed by data:**
1. **The NCE SIGUSR2 signal path is NOT the per-handoff cost.** It fires only **~4×/frame**; ~2600
   resumes/frame take the cheap branchless trampoline (`ReturnToRunCodeByTrampoline`). The review's
   lead suspect is refuted.
2. **There is no hidden tail past `SwitchThread`.** Full runnable→guest (`HandoffEndWaitToGuest`,
   ~25 µs) is only ~5 µs above the old `SwitchThread` endpoint (~20 µs); `Reload` is 0.2 µs and the
   fiber switch is cheap. The earlier ~20 µs was essentially right.

**The actual bottleneck (confirms the "CPU-side GPU translation" hypothesis):** the frame is dominated
by the two GPU-side threads — **GpuBatchProcess** (command-list processing, ~13–23 ms) and
**VkWorkerExecute** (Vulkan command recording + submission to the immature PowerVR driver, ~17–25 ms).
These wrap/overlap the guest handoffs (which run in parallel on the emulation threads). Dynarmic wins
not by avoiding a signal path but by feeding this pipeline faster (more handoffs/frame, more frames).
Note ~545–585 draws/frame and ~150 Vulkan submissions/frame — high, with a submit tail (VkWorkerExecute
p99 2–4 ms).

**Instrumentation overhead is negligible:** Dynarmic captured 25.8 fps vs ~25 fps non-instrumented —
within noise, despite ~60 k stamped events/s. The lightweight atomic-histogram approach stayed far
under the ATrace 1–10 µs/event the review warned about. Capture-OFF cost is a single relaxed atomic load.

**Revised direction:** optimize the **CPU-side Vulkan path** on PowerVR — reduce per-frame submission
count/cost (`vk_scheduler` `SubmitExecution`/`DispatchWork`, `FlushWork` thresholds) and draw/state
redundancy (`vk_rasterizer`), not the HLE scheduler or NCE resume. The guest-handoff and NCE-signal
avenues are now closed by measurement.

#### Experiment (2026-10-28): FlushWork CHECK_MASK 3→7 — no change (reverted)

`FlushWork` (`vk_rasterizer.cpp`) forces a `scheduler.DispatchWork()` every `CHECK_MASK+1` draws;
Android uses `CHECK_MASK=3` (dispatch every 4 draws) → ~150 VulkanWorker hand-offs/frame, matching
the measured `VkWorkerExecute` count. Hypothesis: the per-dispatch cost (mutex + `event_cv` notify +
worker wake-up) was inflating the frame. Raised `CHECK_MASK` to 7 (desktop's value; dispatch every 8
draws, ~halving hand-offs; `512 % 8 == 0` so the static_assert holds), rebuilt, measured matched
(Dynarmic, clocks 3.2–3.4 GHz, 36 °C): standing 25–27, walking ~27 — **within noise of the
`CHECK_MASK=3` baseline (~25 / ~28).** No improvement → the hand-off/dispatch overhead is **not** the
bottleneck. `VkWorkerExecute`'s ~17–25 ms/frame is the raw `vkCmd*` recording cost into the PowerVR
driver (~585 draws → tens of thousands of commands/frame), not how it is batched. Change reverted.

Remaining (deeper, higher-risk, uncertain payoff): cut the **command volume** — draw/state/descriptor
redundancy in `vk_rasterizer` (dedup unchanged pipeline/descriptor binds across the ~585 draws), or
PowerVR-specific command-recording paths. Absent that, the overworld rate is gated by this device's
immature PowerVR Vulkan driver's per-command CPU cost — a driver-maturity limitation more than an
emulator-logic one.

#### Command-dedup audit + descriptor path (2026-10-28): emulator side already optimized

Code audit of the per-draw recording path (`vk_graphics_pipeline.cpp ConfigureDraw`, `vk_rasterizer.cpp
UpdateDynamicStates`): the standard redundancy is **already eliminated** — pipeline bind is guarded by
`scheduler.UpdateGraphicsPipeline` (only on change), descriptor writes by a payload memcmp +
generation guard, and every dynamic state (`UpdateViewportsState`, etc.) by a `state_tracker.Touch*`
dirty flag. No cheap redundant-command win is available.

Probed the descriptor path on this device: **`push_descriptor=false`, `descriptor_buffer=false` → the
slow legacy descriptor-set path** (allocate+update+bind). Both fast paths (`VK_KHR_push_descriptor`,
`VK_EXT_descriptor_buffer`) are unsupported by the PowerVR driver, so they cannot be forced. However,
the capture shows `VkDescriptorConfigure` is only ~3 µs × ~585 ≈ **1.8 ms/frame** — the legacy path is
suboptimal but **not** the bottleneck, so a legacy-path descriptor-set cache would not move the frame.

**Definitive conclusion.** The overworld frame (~40–49 ms) is gated by the PowerVR driver's CPU cost of
recording/replaying the frame's command stream (`VkWorkerExecute` ~20 ms + `GpuBatchProcess` ~20 ms,
partially serialized). Every emulator-side lever has been measured and exhausted: config (resolution,
frame-gen, clocks), the 60 FPS mod, cpu_backend (Dynarmic > NCE), dispatch batching (CHECK_MASK),
command dedup (already present), and descriptor path (slow but minor). The residual gap is a
**PowerVR Vulkan driver-maturity limitation**, not an emulator defect, and is expected to improve with
driver updates. The practical, already-applied win remains disabling the community 60 FPS mod
(retargets to 60 → ~43 % real speed on this device; off → ~80–90 % at 30 fps target).
