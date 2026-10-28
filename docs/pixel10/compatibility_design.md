# Compatibility design — why each conditional exists

Principles applied (from the runbook and Eden's policies): smallest change that the evidence supports; driver scoping via `VkPhysicalDeviceDriverProperties::driverID`, never device-name strings; no semantic lies (no faked format support, no removed clears, no suppressed asserts); every PowerVR-specific behavior reversible.

## Stage 1a — `vulkan_present.vert` static corners (unconditional)

**Why unconditional:** `Layer::SetVertexData` (`present/layer.cpp:298-310`) is the only writer of the push-constant quad and always produces an axis-aligned rect with `[0]`=top-left and `[3]`=bottom-right; deriving all four corners from those two with constant indices is mathematically identical for every driver (`corner=(idx&1, idx>>1)` reproduces vertices 0,1,2,3 exactly, preserving strip winding). Gating would mean shipping two SPIR-V blobs for zero benefit. The constant indices also satisfy Vulkan 15.8.1 (dynamically uniform indexing of push-constant arrays) trivially, where the previous branch chain satisfied it only by construction — the construction PowerVR's compiler miscompiles.
**Tradeoff:** none identified. Validated with glslangValidator (vulkan1.1) + spirv-val.

## Stage 1b — swapchain `pViewFormats` narrowing (unconditional)

**Why unconditional:** no code in the tree creates a `VkImageView` of a swapchain image — swapchain images are exclusively blit/copy destinations (`vk_present_manager.cpp`); the format-reinterpreted views live on the separate `Frame` image with its own format list. The old list additionally mixed BGRA and RGBA orderings on Android, which is spec-legal but corrupts PowerVR. Restricting to the base format's own UNORM/SRGB pair is the exact device-tested community change.
**Tradeoff:** if a future change starts creating alternate-ordering swapchain views, it must extend this list again. The `MUTABLE_FORMAT_BIT` block is arguably removable outright (no views exist), but that is a behavioral change beyond the tested patch — flagged as follow-up, not done here.

## Stage 2 — present clear via `loadOp=CLEAR` (automatic, PowerVR only)

**Why gated:** this is a behavioral change in command-stream shape (clear moves from an explicit command into the render-pass begin). It is semantically identical here — the old `ClearAttachments` covered the full render area of a pass whose attachment was `DONT_CARE`-loaded, and the only `BeginRenderPass` of this render pass is this call site — and it is, if anything, the preferred pattern on tile-based GPUs. It is still gated to PowerVR to keep every other driver's command stream byte-identical, which keeps the patch trivially safe to review and bisect.
**Why `driverID`:** `VK_DRIVER_ID_IMAGINATION_PROPRIETARY` is the precise unit of blame (the open-source Mesa PowerVR driver has a different ID and is not implicated). Driver-version narrowing was considered and rejected: both reported driver versions (1.634.2906, 1.662.3024) crash, and no fixed version is known.
**Guarantee kept:** the background clear always happens — either via `pClearValues` (quirk on) or via `ClearAttachments` (quirk off). There is no path that skips clearing (would leak undefined prior-frame pixels).
**Render-pass compatibility:** `loadOp` does not participate in render-pass compatibility (Vulkan spec §8.2), so the pipelines and the `Frame` framebuffers created against this render pass remain valid unchanged.

## Stage 3 — rasterizer draw-based clear fallback (PowerVR AND `avoid_clear_attachments`, default OFF)

**Why default OFF:** the crash evidence points at *a* `vkCmdClearAttachments`, and the present-path one (Stage 2) is the first reached every boot. Rerouting the game renderer's clears has weaker evidence and real semantic restrictions, so it ships as a diagnostic/escalation toggle for the device session, not a default.
**Why the restrictions:** `BlitImageHelper::ClearColor/ClearDepthStencil` draw a fullscreen triangle into attachment 0 of the current framebuffer, non-layered:
- `color_attachment == 0` — the draw pipeline writes color attachment 0 only; a clear of RT>0 via this path would clear the wrong image.
- `layer == 0 && layer_count == 1` — the draw is not instanced/multiview; layered clears would silently only clear layer 0.
- `!is_integer` — ClearColor masks channels via blend constants, and blending is undefined on integer attachments; integer targets keep `ClearAttachments`.
- Depth/stencil: full stencil write mask (0xFF) + the pipeline's own always-pass state reproduce a full clear; depth-only clears pass stencil mask 0 (no stencil writes).
Everything outside these bounds intentionally keeps the original `ClearAttachments` — if PowerVR crashes on one of *those*, that is new evidence, and the log + tombstone will show it.
**Cost:** a draw instead of a clear command (pipeline bind + fullscreen triangle). Measure frame-times before leaving it enabled.

## Stage 4a — `A8B8G8R8_*_PACK32` → `R8G8B8A8_*` alternatives (unconditional)

**Why unconditional and why safe:** `VK_FORMAT_A8B8G8R8_*_PACK32` and `VK_FORMAT_R8G8B8A8_*` describe the identical byte layout on little-endian hosts (every supported host is little-endian); components, numeric format, and sRGB-ness all match 1:1 (UNORM→UNORM, SNORM→SNORM, UINT→UINT, SINT→SINT — **no cross-numeric remap**). No swizzle, no data conversion, no shader change is needed; views/uploads/attachments/storage all behave identically. On drivers that support the PACK32 formats, `GetSupportedFormat` returns early and the alternatives are dead code.
**Scope note:** `A8B8G8R8_SRGB_PACK32 → R8G8B8A8_SRGB` would be equally sound but no failure was reported for it; left out to keep the diff evidence-bound (trivial follow-up).

## Stage 4b — format 44 (`B8G8R8A8_UNORM`): deliberately NOT remapped

`B8G8R8A8` ↔ `R8G8B8A8` differ in component order in memory. A sampled-view swizzle (the `emulate_bgr565` pattern) fixes *reads* only: storage-image writes and color-attachment writes would land channel-swapped, and the requested usage (0xC083) includes both. A correct emulation needs either data-level conversion on upload/download plus swizzled render paths, or proof that the storage/attachment bits aren't actually exercised for these images — neither of which can be designed responsibly without the device log showing *which* feature bit PowerVR actually lacks for format 44. Stage 4c's log provides exactly that. Returning the unsupported format with a precise error (current behavior) is honest; silently returning a channel-swapped format would violate the "no misrepresentation" rule.

## Stage 4c — deduplicated, decoded failure logging (unconditional)

One log line per distinct (format, usage, type) failure instead of per-call spam (issue #398 shows the spam itself degrades the session), now including the supported and missing feature bits so a single device log answers the key question for each failing format. Stored in a small per-Device `mutable` set; `GetSupportedFormat` stays `const` (precedent: existing `mutable` members on Device).

## Quirk detection architecture

A single `is_imagination` bool derived once in the Device constructor next to the existing per-driver bools, feeding dedicated `has_broken_*` quirk fields with accessors — exactly the existing Qualcomm/NVIDIA pattern. No string comparisons, no scattered driver checks at call sites; call sites only ever test the semantic quirk (`HasBrokenClearAttachments()`), so future driver fixes can be version-scoped in one place.

## Known, intentionally-untouched items

- `IsTiler()` omits Imagination: affects MSAA-resolve heuristics and descriptor sizing. No reported symptom ties to it; changing it would alter many paths at once, destroying bisectability. Revisit with device data.
- Swapchain `preTransform` stays `IDENTITY` on Android (upstream behavior): the runbook explicitly warns against forcing `currentTransform` without end-to-end orientation evidence.
- Class B memory asserts and Class E config crash: documented in `crash_matrix.md`, no code changes (no reproduction; masking forbidden).
- SPIR-V SSBO bounds polyfill and conservative-raster emulation: excluded per the runbook (last-resort experiments requiring isolated reproductions that do not exist yet).
