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
*count* of handoffs isn't possible (it's the game's threading). Net: the overworld ~20 FPS is, to a
first approximation, an inherent cost of emulating this finely-threaded engine's ~800
context-switches/frame on the HLE kernel + NCE at this device's clocks.
