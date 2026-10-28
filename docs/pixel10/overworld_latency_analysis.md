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
