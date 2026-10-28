# ✅ SOLVED (2026-10-28): the game boots on the Pixel 10 Pro XL

**Root cause:** Eden's Android frontend started emulation **twice concurrently** during the launch (activity/surface recreation spawns a second `EmulationFragment` → second `NativeLibrary.run`). The second `KernelCore::Initialize` re-creates and zeroes the kernel's shared block-info page manager while the first guest's `svcSetHeapSize` is mid-flight, zeroing its page group → map address 0 → crash. Desktop (Mac v0.2.1) has no such surface lifecycle, so it starts once → always worked.

**Fix:** atomic duplicate-run guard at the top of the JNI `RunEmulation` (`native.cpp`) — `stage7-duplicate-run-guard.patch`. **Device-verified:** Pokémon: Let's Go, Pikachu! boots to the controller-select screen at ~104–107 FPS on the Pixel 10 Pro XL (`eden-FIX2-dup-run-guard.apk`).

Full evidence and fix in `crash_matrix.md` (B1 SOLVED section). The sections below are the historical investigation plan that led here.

---

# Pixel 10 Pro XL — why the game still black-screens, and the plan to make it run

Date: 2026-10-27. Written from on-device evidence (`device_results.md`, `eden_log-P5-full.txt`, tombstones in `device_logs/`). Device: Pixel 10 Pro XL, Android 17/API 37, PowerVR DXT-48, driver 1.662.3024.

## Why it black-screens (root cause — confirmed, not a theory)

The launch **crash** (E1, `Config::SaveValues` null deref) is fixed. What remains is a **guest-execution failure during boot**, class B:

```
k_page_table_base.cpp:5788  assert num_pages == page_group.GetNumPages()   ← root divergence
memory.cpp:73               assert Out of bounds target: 0x0
memory.cpp:546              assert Mapping memory page without a pointer @ 0x2104400000
host_memory.cpp:758         assert host_offset + length <= backing_size
k_memory_block.h:475        assert this->Contains(addr)  (×2)
arm_dynarmic_64.cpp:120     Cannot execute instruction at unmapped address 0x0000000000000004
```

Localized to: **`KPageTableBase::MapPhysicalMemory` (k_page_table_base.cpp:5146) → `Operate(…, OperationType::MapFirstGroupPhysical)` (5405) with `separate_heap=true`.** The guest calls `svcMapPhysicalMemory` during boot; Eden's separate-heap / fastmem **heap-tracker** lazy-mapping path builds a page group whose page count doesn't equal the requested `num_pages` and maps past the backing buffer. Because these are `<Critical>` asserts that **log-and-continue in release**, the process survives but guest memory is now corrupt — so the guest branches to a garbage pointer (`0x4`), that CPU core dies, and only the `HostTiming` thread spins (black screen with the input overlay, no GPU work ever submitted).

This was always present; the E1 crash previously killed the app *earlier* in startup, so it was never reached until E1 was fixed.

### What is NOT the cause (ruled out on device)
- **CPU backend (your JIT-vs-NCE change):** identical stall both ways. Not the lever.
- **fastmem off:** not a workaround — fails differently and worse (`framebuffer_layout width>0&&height>0` abort, wild PC in the HostMemory region). Confirms the bug lives in the separate-heap/heap-tracker path, but disabling it is not viable.
- **Missing firmware:** firmware IS installed (238 registered NCAs); the game's own content decrypts and loads.
- **PowerVR rendering / format 44:** the guest dies before any GPU submission, so rendering is downstream, not the blocker.

## Blockers, prioritized

| ID | Blocker | Type | Severity |
|---|---|---|---|
| **B1** | `svcMapPhysicalMemory` separate-heap page-group size mismatch → guest memory corruption | Eden HLE-kernel bug | **Fatal — the black screen.** Fix first. |
| **B2** | `prod.keys` older than installed 23.0.1 firmware → 217 gen-0x17 NCAs undecryptable; no title.keys; no game update (v1.0.2) | Content/keys config (user-fixable) | High — must be clean to trust any result; possible contributor |
| **B3** | `maxVertexInputAttributes`/`Bindings` = 16 < 32 required | PowerVR hardware limit vs Maxwell | High — will break vertex pipelines once rendering starts |
| **B4** | Format 44 `B8G8R8A8_UNORM` lacks STORAGE_IMAGE | PowerVR driver limit | Medium — Stage 6 keeps image creation legal; correct emulation is a follow-up |
| **B5** | Missing `VK_EXT_conservative_rasterization`, `depth_bias_control`, `robustness2`, `external_memory_host` | PowerVR driver gaps | Medium/low — per-feature fallbacks |

## The plan

### Phase A — enabling infrastructure (do before touching kernel code)
1. **Build a debug (assert-fatal) variant.** Right now the asserts log-and-continue, so we only see the cascade, never the *originating* call with its arguments. Build `assembleMainlineDebug` (or set `YUZU_ENABLE_ASSERTS`/make `ASSERT` abort) so the **first** failure (`num_pages == page_group.GetNumPages()`) aborts with a native backtrace and the actual `num_pages`, `page_group.GetNumPages()`, `virt_addr`, `phys_addr`, operation, and `separate_heap` captured.
2. **Add targeted instrumentation** at `KPageTableBase::Operate` (5784) and `MapPhysicalMemory` (5146): log `num_pages`, each page-group node's `GetNumPages()`/address, the requested region, and the heap-tracker residency decision — gated behind a debug flag, removed before any PR.
3. **Regression bisect.** Issue #398 establishes the Pixel 10 path regressed (SCHiM worked on eden v0.0.4, broke by v0.2.0-rc1). Build v0.0.4 and confirm whether Pokémon LGP boots further there; if so, `git bisect` between v0.0.4 and `26a03d2473` using the first-assert abort as the test. Prime suspects to bisect around: the **heap-tracker / separate-heap lazy-mapping** introduction and any `MapPhysicalMemory` refactor.
4. **Validation layers**: if a PowerVR-compatible layer set is available, run with `VK_LAYER_KHRONOS_validation` to catch any illegal usage once rendering is reached (for B3/B4/B5).

### Phase B — fix B1 (the fatal bug), smallest correct change
- With Phase-A evidence, determine *why* the page group's total pages ≠ `num_pages` for this `svcMapPhysicalMemory`. Likely candidates: (a) heap-tracker splitting a separate-heap region and miscounting residency vs. requested length; (b) `MakePageGroup` over the physical-memory region returning a group that doesn't span the whole request when the alias/heap region is partially mapped; (c) an alignment/rounding mismatch between `num_pages * PageSize` and the group node sizes.
- Fix at the layer where the invariant is actually violated (page-group construction or the heap-tracker map/split), **not** by weakening the assert. Add a **deterministic unit/regression test** reproducing the page-group/num_pages relationship for a `MapPhysicalMemory` of the size the game requests. Keep all asserts enabled.
- Validate on device: the cascade must disappear, the guest must advance (CPUCore threads accumulate CPU, VulkanWorker begins submitting). Re-verify no regression on a known-good GPU (desktop) and a second Android GPU if available.

### Phase C — content hygiene (B2; mostly user actions, cheap, do in parallel)
1. Align keys and firmware: either obtain `prod.keys` **at least as new as** the 23.0.1 firmware (so the 217 gen-0x17 NCAs decrypt) **or** install firmware whose keygen your current keys support. Re-dump both from your own console to be consistent. Add `title.keys` if your legitimately-owned titles need per-title keys.
2. Install the **game update (v1.0.2 / v131072)** you already have — `load/010003F003A34000` is currently empty. Some titles rely on the update for correct boot.
3. Re-test after B1; confirms whether any residual issue is content vs. emulator.

### Phase D — rendering compatibility (B3–B5; only reachable once B1 lets the GPU run)
- **B3 vertex attributes (16<32):** this is a hard PowerVR limit. Investigate whether Eden can pack/remap vertex attributes into ≤16 bindings for titles that declare ≤16 *active* attributes (many do, even if the limit check assumes 32), or whether it's a true wall for this title. The current code only warns; determine the actual failure mode when a pipeline needs >16.
- **B4 format-44 storage:** Stage 6 already prevents the invalid image creation. The correct long-term fix is a swizzled `R8G8B8A8` round-trip for BGRA8 surfaces that need storage, or confirming no title actually image-stores to them.
- **B5 missing extensions:** per-feature — verify Eden's existing fallbacks for conservative rasterization and depth-bias are driver-gated correctly on PowerVR; add graceful degradation where missing. `external_memory_host` absence affects fastmem import strategy; confirm it isn't entangled with B1.

### Phase E — validation & exit criteria
- 3 consecutive cold boots of Pokémon LGP past the current stall with no Class B cascade.
- Guest executes (CPU cores busy), GPU submits frames, first in-game frame presents without corruption.
- No new asserts; all asserts remain enabled; no FPS/behavior regression on desktop or a second Android GPU.
- Each fix: a patch + the failure it addresses + a regression test + device evidence; honest "hypothesis vs verified" labeling throughout.

## Deep-dive update (2026-10-27, session 2 — instrumented debugging on device)

I instrumented `KPageTableBase::Operate`/`MapPhysicalMemory`, built an instrumented APK, and reproduced on the device repeatedly. New findings that revise the B1 picture:

- **The boot failure is non-deterministic with several distinct manifestations**, all early in guest boot, varying by timing:
  1. `k_page_table_base.cpp:5788` page-group cascade → guest jumps to `0x4` (the original/canonical one).
  2. **`svc_exception.cpp:56 Userspace PANIC! info2=0x4`** — the *guest* itself calls `svcBreak` (panic syscall). This is new and important: a guest/system module is aborting, not (only) the host emulator.
  3. `framebuffer_layout.cpp:27 width>0 && height>0` — emulation surface comes up 0×0 (at least partly an artifact of adb launch timing / surface not laid out; did not appear in the user's own normal-play logs).
  4. `VK_ERROR_INITIALIZATION_FAILED` in `PresentManager::CopyToSwapchain` (seen once, coincident with a USB disconnect — treat as suspect/surface-loss, not confirmed).
- Once (with `use_debug_asserts`), a run reached **actual frame presentation** (`CopyToSwapchain`) before failing — proving the guest *can* intermittently execute far enough to render. This confirms a **race**, not a deterministic wall.
- `use_debug_asserts=true` traps on the *first* soft assert, but the surface-0×0 `framebuffer_layout` assert then preempts the real bug under adb launch — so it must be combined with a guaranteed-valid surface to be useful.

### Revised root-cause view
The common denominator is **early-boot guest memory/execution corruption**. The new `Userspace PANIC` shows a **guest module aborting**, but its cause is **not yet determined** — the guest panic error code (`debug_buffer_err_code`, logged by Eden for `info2=0x4`) was NOT captured, so we do not know what the guest panicked on.

**Correction to earlier framing:** I previously called the keys/firmware mismatch (B2) the "prime suspect." That was overstated. The evidence balance is genuinely split:
- *For B2:* 217 firmware NCAs fail to decrypt (keys predate 23.0.1 firmware gen 0x17) — objectively broken; a missing system service is *one* explanation for a guest panic.
- *Against B2:* the game's own content decrypts and loads fine; Eden HLEs most services (many titles boot with no firmware at all); and the `num_pages == page_group.GetNumPages()` failure is a **host-side emulator** invariant violation, not something a missing firmware module would directly produce.

So B2 is a **rule-out candidate**, not a proven cause. It should be fixed first for methodological reasons (eliminate a known-broken variable; it's wrong regardless), not because it is confirmed. The **definitive test** is to capture the guest panic error code and/or test with firmware fully removed (pure HLE) vs. the current half-decryptable firmware.

### Why I did not land a code fix for B1 this session (honest blockers)
1. **No reliable repro harness.** Launching via adb is flaky (slow 217-NCA content scan → taps dropped; multi-tap spawned several instances; surface timing injects the 0×0 mode). Without a deterministic repro I cannot validate any fix.
2. **No known-good baseline loaded.** Bisecting requires a build where this title boots on this device; not available yet.
3. **Confounding broken content** (B2) must be eliminated first, or any "fix" is measured against a corrupt system state.
4. **Blind patching of the HLE memory manager is unsafe** — it maps guest memory; a wrong change can silently corrupt saves. The runbook forbids speculative changes here.

### Concrete next steps (in order)
1. **Fix content (user):** install `prod.keys` that match firmware 23.0.1 (so the 217 gen-0x17 NCAs decrypt) *or* install firmware matching the current keys; install the game update `v131072` you already have (`load/` is empty). Re-test — this may resolve the `Userspace PANIC` outright.
2. **If it still fails:** build a deterministic repro — launch with the screen on and a valid surface, single instance, and enable `use_debug_asserts` to capture the true first-fault tombstone (not the surface 0×0). The instrumentation diff (reverted from the tree to keep the patch clean) is documented here for re-application:
   - `KPageTableBase::Operate` (k_page_table_base.cpp:~5788): log `operation`, `num_pages`, `page_group.GetNumPages()`, and each node addr/pages on mismatch.
   - `MapPhysicalMemory` (after `AllocateForProcess`): log `address,size,mapped_size,req_pages,pg.GetNumPages(),first node addr`.
3. **Bisect** against eden v0.0.4 (issue #398 implies the Pixel 10 path regressed) using the first-fault tombstone as the test, suspecting the heap-tracker / separate-heap lazy-mapping work.
4. **Then** fix at the layer where the page-group/`num_pages` invariant or the heap-tracker residency actually diverges, with a regression test, asserts kept enabled.

## Honest status
- Solidly fixed and device-verified: **E1 launch crash** (the thing that was actually closing the app).
- Root-caused but **not yet fixed**: **B1**, the guest-kernel `svcMapPhysicalMemory` separate-heap bug — this is the black screen, and fixing it needs the Phase-A debug build + bisect before any code change (it is a memory-correctness bug; a blind patch risks silent corruption, which the runbook forbids).
- The remaining blockers (B2–B5) are only *reachable* after B1; none can be claimed fixed until the game executes past boot on the device.
- No commits/pushes; see `review_notes.md`.
