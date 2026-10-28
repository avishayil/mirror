# Crash matrix — Pixel 10 series / Tensor G5 / PowerVR DXT-48

Sources: eden-emulator/Issue-Reports #561 and #398 (public reports; fetched 2026-10-27). **No crash was reproduced locally — no device was attached.** "Observation" = quoted from reports; "Hypothesis" = this investigation's inference, pending device validation.

## A1 — PowerVR driver SIGSEGV in vkCmdClearAttachments (Class A)

**Observation (#561, original report):** Every tested game (Horizon Chase, Kingdom's Return, Under Night In-Birth II, KORG Gadget) crashes during boot before gameplay. Eden v0.2.1, Pixel 10, GrapheneOS Android 17, PowerVR D-Series Vulkan driver 1.662.3024.

```
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x6cc (read)
#00 vulkan.powervr.so (IMG_vkCmdClearAttachments+404)   BuildId: 640c6025f014ca8f58d92be8ab072e7e
#01 libyuzu-android.so                                  BuildId: 36a4f66ee1900dc44f0653fc465d379337a2275e
#02 libyuzu-android.so (Vulkan::Scheduler::WorkerThread(std::__ndk1::stop_token)+740)
```

**Code-path analysis (confirmed in tree at `26a03d2473`):**
- All recorded Vulkan commands execute on `Scheduler::WorkerThread` ("VulkanWorker", `vk_scheduler.cpp:270-323`) — matches frame #02.
- The first `vkCmdClearAttachments` every boot reaches is the presentation clear: `window_adapt_pass.cpp:94-95` records `BeginRenderPass` (loadOp=DONT_CARE, `present/util.cpp:204-205`) followed immediately by a full-render-area `ClearAttachments`.
- Secondary candidates: `RasterizerVulkan::Clear` color (`vk_rasterizer.cpp:~531`) and depth/stencil (`~580`) sites — only reached for non-deferrable clears once a game renders.

**Hypothesis H-A1:** the present-path clear is the crashing call (first reached, right thread, crash "during boot" before first frame). Fix applied: Stage 2 (loadOp=CLEAR via `pClearValues`, ClearAttachments not recorded on PowerVR). If the crash persists and the stack still shows `IMG_vkCmdClearAttachments`, enable the Stage 3 setting (`avoid_clear_attachments`) to also reroute the rasterizer sites, and capture a new tombstone — per-site instrumentation guidance in `experiments.md`. A driver bug is plausible (fault addr 0x6cc = low-address read, likely a null/garbage struct pointer inside the driver), but invalid API use by Eden has not been ruled out without a validation-layer run on device.

## B1/B2 — host memory mapping assertion cascade (Class B)

**Observation (#398):** after prolonged C1 format errors in SCHiM: `host_memory.cpp:730: assert virtual_offset + length <= virtual_size` (v0.2.0-rc1).
**Observation (#561 comment, rayanmargham, Pixel 10 Pro):** boot-time cascade:
```
k_page_table_base.cpp:5758: assert num_pages == page_group.GetNumPages()
core/memory.cpp:73:  assert Out of bounds target: 0000000000000000
core/memory.cpp:540: assert Mapping memory page without a pointer @ 00000037ef000000
common/host_memory.cpp:731: assert host_offset + length <= backing_size
HW.Memory Unmapped InvalidateNCE for 4096 bytes @ 0x0
nca.cpp:113: ... (Heap size: 0x6e6f69)
SIGSEGV fault addr 0x100000000076a in tid (CPUCore_0)
```

**Static analysis (current tree):** the asserts are `HostMemory::Map` preconditions (`host_memory.cpp:757-758`). They indicate the kernel page-table layer requested a mapping outside the backing/virtual arena — an invariant violation upstream of HostMemory, not a HostMemory bug per se. Notably `Heap size: 0x6e6f69` ("noi" in ASCII, non-page-aligned) logged by `core/loader/nca.cpp:113` suggests the NPDM/metadata the loader read was already corrupted — i.e. guest-visible memory corruption precedes the mapping failures. The same session shows `IsValidCore` asserts and a CPUCore_0 SIGSEGV, consistent with wide guest-memory corruption (possibly NCE-related on this SoC, possibly fallout from the GPU-side failures).

**Hypothesis H-B1 (untested):** Class B is secondary fallout, not an independent root cause — most likely triggered by earlier failures (A1 abort paths, or C1's unsupported-format images corrupting state). Per the runbook, **no code change is made**: no reproduction is available, asserts stay enabled, and masking them is forbidden. Device work needed: capture the *earliest* assert with `(host_offset, backing_size, virtual_offset, virtual_size, length)`, test with Vulkan disabled (null/GL backend) to decouple GPU from CPU/NCE, and test with NCE vs interpreter.

## C1 — unsupported Vulkan formats 44/51/52/55/56 (Class C)

**Observation (#398, SCHiM, v0.2.0-rc1; regression vs v0.0.4):**
```
GetSupportedFormat: Format={51,52,56,55,44} with usage=49283 and type=1
has no defined alternatives and host hardware does not support it
```
Severe pixel corruption on screen; log spam; later B1. A commenter bisected the regression to upstream `5edcdea78fa60a8cf67c2b99cf2bc827865de906` (large Vulkan refactor).

**Decoding (verified against this checkout's Vulkan headers):** usage 49283 = 0xC083 = SAMPLED_IMAGE | STORAGE_IMAGE | COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST; type 1 = Optimal tiling. Formats: 44 = `VK_FORMAT_B8G8R8A8_UNORM`, 51/52/55/56 = `VK_FORMAT_A8B8G8R8_{UNORM,SNORM,UINT,SINT}_PACK32`.

**Hypothesis H-C1 (untested):** the PowerVR driver does not advertise STORAGE_IMAGE (the most commonly missing bit of that set on mobile) for the BGRA/ABGR-ordered 8-bit formats, while supporting plain `R8G8B8A8_*`. `maxwell_to_vk.cpp:313-315` adds STORAGE_IMAGE whenever the guest format is storage-capable, so these requests became common after the `5edcdea7` refactor. Fix applied: Stage 4 adds byte-identical alternatives (A8B8G8R8_*_PACK32 → R8G8B8A8_*) and a precise once-per-combination log that prints the *missing* feature bits, which will confirm or refute this hypothesis from the first device log. Format 44 is intentionally not remapped (see `compatibility_design.md`).

## D1 — presentation smearing / corruption (Class D)

**Observation (#398 screenshot + nicman23 in-thread):** pixel corruption; nicman23 reports the swapchain fix alone "produced smearing" and the present-shader change was additionally required.

**Mechanism (community-diagnosed, code-verified equivalent):** (1) PowerVR miscompiles the dynamically-uniform branch chain in `vulkan_present.vert`, corrupting `vertices[3]` → lower-right triangle smears. (2) A mutable-format swapchain whose `pViewFormats` mixes BGRA and RGBA orderings corrupts output on PowerVR. Fixes applied unconditionally in Stage 1 (semantically equivalent on all drivers; see `compatibility_design.md`).

## E1 — Config::SaveValues crash (Class E) — ROOT CAUSE CONFIRMED ON DEVICE 2026-10-27

**Device reproduction (run 1, P3 APK `806f073b…`, BuildId `71eff58f…`, Pixel 10 Pro XL, stock Android 17 API 37):** launching Pokémon: Let's Go, Pikachu! killed the process ~33s in. Full tombstone captured via live logcat (`device_logs/live-logcat-run1.txt` lines ~15905+):

```
signal 11 (SIGSEGV), SEGV_MAPERR, fault addr 0x30 (read), Cause: null pointer dereference, main thread
#00 libyuzu-android.so (Config::SaveValues()+32)            BuildId 71eff58f…
#01 libyuzu-android.so (AndroidConfig::SaveAllValues()+16)
#02..: art JNI ← QuickSettings.saveSettings ← addCustomToggle$lambda$5 ← onCheckedChanged
       ← CompoundButton.onRestoreInstanceState ← View.dispatchRestoreInstanceState
       ← Fragment.restoreViewState ← Fragment.performActivityCreated
```

The preceding `dynarmic exception_handler_posix.cpp:146 Unhandled SIGSEGV at 0x…054` line is the **same fault** observed by Dynarmic's chained signal handler (the logged value matches the faulting PC) — not a CPU-emulation bug.

**Root cause (two defects):**
1. `src/android/app/src/main/jni/native_config.cpp` — `saveGlobalConfig`/`savePerGameConfig`/`reloadGlobalConfig` dereference `global_config`/`per_game_config` without null checks; `this == nullptr` → `Config::SaveValues` reads member at offset 0x30 = fault addr 0x30.
2. `QuickSettings.kt` — every settings row inflates the same layout, so all switches share `R.id.setting_switch`. Android's ID-keyed instance-state restore pushes saved checked-state into these switches during activity recreation (game launch rotates to landscape → recreation) and fires `onCheckedChanged` → `saveSettings()` at a point where the native config object is not loaded.

**Fix (P4 build `837b7b36…`):** null-guards with LOG_ERROR in the three JNI entry points; `isSaveEnabled = false` on the QuickSettings switches/radio buttons (state is rebuilt from NativeConfig on creation, so instance-state restore was pure liability).

**Device result (run 2, P4):** process survives launch; emulation activity reaches the game with overlay rendered. E1 no longer reproduces on this path. This also explains the identical stacks reported by two users in issue #561 — on Pixel 10 the *reported GPU crash reports* were partly E1.

### Original static analysis (pre-device, kept for history)

**Observation (#561 comments, two reporters):** SIGSEGV fault addr 0x30/0x0-ish with top frames `Config::SaveValues()+32` / `AndroidConfig::SaveAllValues()+16|20`, called from Java UI paths (`QuickSettings$$ExternalSyntheticLambda2.onCheckedChanged` → `CompoundButton.setChecked` → fragment state restore during `Activity.performStart`).

**Analysis:** an Android settings/lifecycle bug — a config save triggered from view-state restoration while native config is not (or no longer) initialized. Entirely separate from the GPU thread. **Out of scope for this patch series**; recorded so device logs aren't misattributed: a tombstone whose top frame is `SaveValues` is E1, not A1, regardless of what the reporter was doing.

## B1 — SOLVED & DEVICE-VERIFIED (2026-10-28): duplicate concurrent emulation start

**Root cause (proven by on-device instrumentation):** Eden's Android frontend starts emulation **twice concurrently**. During the launch transition the activity/surface is recreated (rotation into landscape), and a second `EmulationFragment` spawns another `NativeEmulation` thread that calls `NativeLibrary.run` → `EmulationSession::InitializeEmulation` → `System::Impl::Load` → `KernelCore::Initialize` **again, while the first run's guest is already booting**. Each init re-runs `InitializeResourceManagers`, which re-creates and **memsets the kernel's shared `KDynamicPageManager` backing to zero**. The second init's zeroing lands on the block-info node that the first guest's in-flight `svcSetHeapSize` (a ~3.14 GB heap map) is holding → the page group is zeroed → Eden maps physical address 0 → guest executes unmapped memory → crash/garbage frame/`Userspace PANIC` (all the non-deterministic faces of this bug).

**Evidence chain (instrumented, device):**
1. `SetHeapSize` page group allocated correctly (`pg_pages=C9200`), then zeroed during the clear (byte dump `[next,idx/pages,next]` → all-zero).
2. Not a slab/logical free (watch on the node's free path never fired; zeros, not a free-list pointer).
3. The zero is a page `memset` from `KDynamicPageManager::Initialize`, logged **twice** for the **same manager object** (`this` identical), the 2nd mid-clear.
4. Trap on the duplicate `Initialize` → tombstone backtrace: `KDynamicPageManager::Initialize → InitializeResourceManagers → KernelCore::Impl::Initialize → System::Impl::Load → EmulationSession::InitializeEmulation → NativeLibrary_run`, on **two different threads**.

**Why desktop (Mac v0.2.1) is fine:** the Qt/desktop frontend has no Android surface-recreation lifecycle, so `run()` is called exactly once. This is why the identical game/keys run perfectly on the Mac but not on Android — it was never content/keys/firmware/CPU-backend (all ruled out on device).

**Fix (Stage 7, `stage7-duplicate-run-guard.patch`):** a global atomic compare-exchange at the top of the JNI `RunEmulation` (`native.cpp`), before the `ShutdownEmulation` scope guard, so a second concurrent start returns immediately without re-initializing the kernel or tearing down the live emulation. Minimal, race-free, and correct for all frontends.

**VERIFIED ON DEVICE (Pixel 10 Pro XL, build `eden-FIX2-dup-run-guard.apk`):** Pokémon: Let's Go, Pikachu! now boots past the crash and reaches the controller-select screen (identical to the Mac), **stable at ~104–107 FPS**, no page-group cascade, no `Userspace PANIC`, no garbage frames. First successful boot on the device.

**Note on the earlier (reverted) `clear-under-lock` attempt:** holding the general lock during the clear did NOT help, because the corruptor is a *second kernel init* (which re-creates the manager wholesale), not a lock-protected memory op — consistent with this root cause.

---

## B1 — earlier instrumented capture (2026-10-27 session 2) [superseded by the SOLVED section above]

**Confirmed the black screen is an Eden Android memory-manager corruption**, not content/keys/firmware. Decisive cross-check: the **same game + same keys runs perfectly on Eden v0.2.1 / MoltenVK on the user's Mac** (controller-select screen, 30 FPS, "Perfect"). Fails only on Android (Pixel 10 Pro XL), matching issues #398/#561.

Instrumented capture of the failing `KPageTableBase::Operate` (page-group overload), from `eden_log.txt.old.txt`:
```
Operate op=1 (MapGroup) virt_addr=0x3AABA00000 num_pages=0xC9200 pg_pages=0
   node[0] addr=0x0 pages=0
assert num_pages == page_group.GetNumPages()   # 0xC9200 != 0
→ memory.cpp:73 Out of bounds target: 0x0
→ memory.cpp:546 Mapping memory page without a pointer @ 0x3AABA00000
→ host_memory.cpp:758 host_offset + length <= backing_size
→ guest executes unmapped memory → Cannot execute instruction at 0x4 / or garbage frame ("broken TV")
```

**Interpretation:** a ~3.14 GB `MapGroup` (application heap / main memory, `op=1`) is handed a **corrupt/empty page group** — a single zero-initialized node (`addr=0, pages=0`) instead of ~823,296 pages. All three in-tree `MapGroup` callers (`k_page_table_base.cpp:1468` insecure, `:1654` AllocateAndMapPagesImpl, `:2272` SetHeapSize) `R_TRY` their `AllocateAndOpen`/`AllocateForProcess` *before* calling Operate, so an allocation failure cannot normally reach Operate — which means the page group (or the kernel slab backing its `KBlockInfo` nodes) is **corrupted after allocation**, consistent with a race / memory-ordering bug that manifests on the Pixel's ARM cores but not on the Mac.

**Characteristics:** non-deterministic (varying virt addrs; sometimes the cascade, sometimes a `Userspace PANIC`, sometimes a garbage-frame present, sometimes `framebuffer_layout 0×0`); independent of CPU backend (JIT≡NCE) and firmware version (22≡23). This is the open Pixel 10 "all games crash on launch" bug, now with a precise first-fault signature.

**Not fixed this session.** It is an intermittent memory-corruption/race in the HLE kernel/memory path; a blind patch risks save corruption. Needs TSAN/ASan or a deterministic repro + bisect against the Mac-known-good v0.2.1 and eden v0.0.4. Instrumentation diff documented in `remaining_work_plan.md`.

## Classification summary

| ID | Class | Signature | Status | Action in this series |
|---|---|---|---|---|
| A1 | A | `IMG_vkCmdClearAttachments` SIGSEGV, VulkanWorker | not locally reproducible | Stage 2 fix (default-on for PowerVR); Stage 3 fallback (opt-in) |
| B1/B2 | B | `HostMemory::Map` assert cascade, CPUCore SIGSEGV | not locally reproducible | documented only; no code change |
| C1 | C | formats 44/51/52/55/56 @ usage 0xC083 | mechanism confirmed in code | Stage 4 alternatives (51/52/55/56) + diagnostics; 44 deferred |
| D1 | D | lower-right smearing, pixel corruption | community-verified fix imported | Stage 1 (unconditional) |
| E1 | E | `Config::SaveValues` on UI thread | separate bug | documented only |
