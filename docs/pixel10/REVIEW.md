# Pixel 10 Pro XL (Tensor G5 / PowerVR) — Eden work for review

This branch makes **Pokémon: Let's Go, Pikachu!** launch, render, take input, and run on a
**Pixel 10 Pro XL** (Tensor G5, Imagination **PowerVR D-Series DXT-48**, Vulkan driver 1.662.3024,
Android 17 / API 37). It is split into (1) landed fixes, (2) two opt-in performance features, and
(3) a fully-characterized open performance problem (overworld frame rate) on which an outside
opinion is wanted.

Device of record: Pixel 10 Pro XL (serial redacted). All claims below are on-device measured.

## 1. How to build / run / test
- Build (host: macOS, Android SDK, NDK 28.2.13676358, CMake 3.31.5, JDK 17, host `glslangValidator`):
  from `src/android`: `JAVA_HOME=<jdk17> ./gradlew assembleMainlineRelWithDebInfo` (mainline = Pixel 10
  flavor, arm64, minSdk 33).
- Repro + measurement helpers live in `../scripts/` (outside the repo): `deploy.sh`, `measure-fps.sh`
  (deterministic boot → controller-select → Continue → overworld), `status.sh`, `profile`-style ADPF
  checks. FPS read from the on-screen counter; CPU via `top -H`; GPU/CPU clocks via
  `/sys/.../devfreq|cpufreq`.

## 2. Landed fixes (4 commits on this branch)
Each is minimal, driver-scoped where device-specific, and device-verified. Details + evidence:
`./crash_matrix.md`, `compatibility_design.md`, `controller_input_fix.md`.

1. **`android: prevent a second emulation start from corrupting the first`** — the launch
   surface-recreation spawned a 2nd `EmulationFragment` → 2nd `KernelCore::Initialize` that re-zeroed
   the shared kernel page manager mid-`svcSetHeapSize` of the 1st guest → map phys 0 → black
   screen/garbage/crash. Atomic duplicate-run guard in the JNI `RunEmulation` (`native.cpp`). This was
   *the* black screen; desktop never hit it (single `run()`).
2. **`android: stop a settings-switch restore from crashing on launch`** — `Config::SaveValues`
   null-deref on launch (shared view-id instance-state restore fired `saveSettings` before native
   config loaded). JNI null-guards + `isSaveEnabled=false` on QuickSettings rows
   (`native_config.cpp`, `QuickSettings.kt`).
3. **`vulkan: PowerVR (Imagination) rendering compatibility`** — present-quad constant corners
   (PowerVR miscompiles the branch chain), swapchain view-format narrowing, PowerVR-gated present
   clear via `loadOp`, A8B8G8R8_PACK32→R8G8B8A8 format fallbacks, format-44 storage clamp, deduped
   unsupported-format logging. All gated on `driverID == IMAGINATION_PROPRIETARY`.
4. **`android: connect the configured controller at startup`** — Android `ReadAndroidPlayerValues`
   never read/wrote `connected`/`type`, so no npad connected at boot and games sat on the
   controller-select applet. Mirror desktop (read+write), and route a Handheld player to the Handheld
   npad. (`android_config.cpp`, `emulated_controller.cpp`.)

## 3. Opt-in performance features (default OFF — no behavior change)
Both were built while diagnosing the overworld; neither helps LGP on this device (see §4) but both
are real capabilities for other titles/devices, so they ship behind config flags.
- **`force_max_clock`** (existing setting, now works on PowerVR): ports the GPU clock-keeper compute
  spinner to Android non-Adreno + adds Imagination to `Device::ShouldBoostClocks()`
  (`vk_turbo_mode.*`, `vulkan_device.cpp`, `renderer_vulkan.cpp`). Verified it boosts the PowerVR GPU
  clock 396→864 MHz. Helps genuinely GPU-bound PowerVR titles.
- **`adpf_max_performance`** (new, `Category::Core`): makes the ADPF Render session report the true
  per-frame interval (uncapped) and mark emulation threads non-power-efficient, for devices whose
  PerfHint honors it (`adpf.cpp`, `settings.h`).

## 4. OPEN PROBLEM for review: overworld ~20 FPS
LGP battles/menus hit 60 (with the community 60 FPS exefs mod applied); the **dense outdoor overworld
is ~15–20 FPS** (native is 30). This was chased exhaustively; full data in
`./overworld_latency_analysis.md`. Summary of what it is and isn't:

**Ruled out** (all on-device): GPU fill (2× resolution = same FPS), GPU clock (boosted 2.2× → no
change), present/compositor (gates ~60µs/frame; vsync 29 kHz; compose ~0µs), guest GPU-fence waits
(0 registrations), sleeps/yields (0), `WaitSynchronization` (~1 ms/s), buffer dequeue (0), CPU
throughput (emulator ~1.25 of 8 cores), CPU clock via ADPF (honest 15× overrun reported, prime cores
stayed 400–533 MHz — Tensor G5 PerfHint does not scale frequency from it).

**Located (the actual bottleneck):** guest **inter-thread synchronization**. Per wall-second in the
overworld, summed across guest threads:

| svc primitive | rate | blocked | per-op |
|---|---|---|---|
| Condition variable (`WaitProcessWideKeyAtomic`) | ~1,300/s (~65/frame) | ~5,000 ms/s | ~3.85 ms/wait |
| Contended mutex (`ArbitrateLock`) | ~16,000/s (~800/frame) | ~900 ms/s | ~56 µs/lock |
| Address arbiter (`WaitForAddress`) | 0 | 0 | — |

The frame is a serial chain of guest-thread handoffs. The engine locks/handoffs are fine-grained
(~800 *contended* locks + ~65 condvar waits per frame); emulating each through the HLE kernel + NCE
thread park/wake carries high per-op/wakeup latency (**~56 µs per *contended* lock vs ~1–10 µs
native**; ~3.85 ms average condvar wait), which multiplies into the ~48 ms frame while no single CPU
core or the GPU is ever saturated (hardware ~75% idle; CPU prime cores stay parked low because the
governor sees no sustained load).

## 5. Questions for the reviewer
1. Is the HLE-kernel `svcArbitrateLock` / condition-variable fast-path and thread-wakeup under NCE a
   known, improvable latency source? Is the ~56 µs/contended-lock plausibly emulation overhead that
   can be cut (e.g. userspace fast-path for more lock/unlock cases, lighter `KScheduler` reschedule,
   faster host futex/cv wake), or is it inherent?
2. Would co-scheduling/affinity-pinning the contending guest threads (reduce cross-core wakeup
   latency) help, and is that feasible without breaking the HLE scheduler's correctness?
3. Is there precedent in yuzu/forks for this "finely-threaded title runs at half-speed on mobile with
   everything idle" pattern, and what fixed it?
4. For the two opt-in perf features: are the gating/driver-scoping and the ADPF report-uncap
   acceptable, and worth upstreaming independently of LGP?
5. Any lower-risk lever we missed before committing to deep HLE-kernel scheduler work?

## 6. Constraints / policy
Per `AGENTS.md` and `docs/policies/AI.md`: anything submitted upstream must be human-authored,
human-tested, and disclose AI assistance (`Assisted-by:` in commit + PR); no AI
`Signed-off-by`/`Co-authored-by`. See `./review_notes.md`. This branch is for
review on a personal fork, not an upstream PR. No asserts were disabled; no speculative
memory/correctness changes were landed; the open-problem fixes (§4/§5) are explicitly *not* landed.
