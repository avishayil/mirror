# Review notes — human verification tasks before any upstream submission

This patch series was produced with substantial AI assistance (Claude Code, model Claude Fable 5), directed by you. Per Eden's `docs/policies/AI.md` and `AGENTS.md`, before anything goes upstream **you** must:

1. **Read and understand every line** of `fix.patch` (and the per-stage patches). You must be able to explain, defend, and revise each change in review. If any part is unclear, do not submit it.
2. **Test on the actual device.** The AI policy requires human testing; nothing here has run on a Pixel 10 Pro. Follow `experiments.md` (P0→P1→P2→P3 A/B sequence) and record results in `test_report.md`.
3. **Author the commit message(s) yourself** (no verbatim AI text; the policy's addendum shows what to avoid: concise intent, not a changelog). Suggested factual content to cover, in your own words:
   - what crashes/corruption on Pixel 10 (issues #561, #398) and which stage fixes what;
   - that the shader/swapchain changes originate from the community p10 fork (credit nicman23 and YodaDaCoda, e.g. via `Co-authored-by:` for their tested changes if they consent / as appropriate);
   - the PowerVR gating rationale.
4. **Disclose AI assistance** explicitly in both the commit message *and* the PR description, e.g.:
   `Assisted-by: Claude Fable 5 (investigation, initial draft of patches and docs)`
5. **Sign-off is yours alone.** Do not add AI `Signed-off-by:`/`Co-authored-by:` for the AI (forbidden by policy). This also means: do **not** keep any AI attribution footer a tool may auto-append to commits — authorship must be the human submitter.
6. **Do not attach private logs** or console keys to the PR. Tombstones contain install UUIDs and build fingerprints — redact before posting.
7. Consider splitting the upstream submission: Stage 1 + Stage 4a/4c are unconditional, low-risk, and reviewable standalone; Stage 2 (+3) is the PowerVR-specific series. Maintainers asked for exactly this kind of split in similar past changes.

## Device-verified additions (Stages 5 & 6, 2026-10-27)

- **Stage 5 (E1 config crash)** is the highest-value fix and is **independently valuable upstream** — it is a real null-deref / Android lifecycle bug (`native_config.cpp` + `QuickSettings.kt`) that reproduces on a Pixel 10 Pro XL and matches the stacks two users reported in issue #561. It is not PowerVR-specific. Consider submitting it on its own. Confirm the `isSaveEnabled=false` approach reads well to maintainers vs. alternatives (e.g. giving each row a stable unique view ID, or gating the listener until native config is loaded); either is defensible, but explain the shared-view-ID root cause in your own words.
- **Stage 6 (format-44 storage clamp)** changes image-creation usage for all drivers when the chosen format lacks STORAGE support. It prevents an invalid `vkCreateImage`. Note to reviewers: this silently drops image-store capability for such surfaces (shaders that store to them were already unsupportable on that device); a correct BGRA8-storage emulation is the real long-term fix.
- **Do NOT claim this makes any game playable.** On device, the game still black-screens due to a separate guest-execution stall (`device_results.md` §3). Represent the series honestly as "fixes the launch crash + two correctness issues," not "fixes Pokémon on Pixel 10."
- The CPU-backend (JIT/NCE) A/B and the stall characterization are device facts, not hypotheses — but the stall's *root cause* is still unknown; don't speculate in the PR.

## Open items for the maintainer discussion

- Swapchain `MUTABLE_FORMAT_BIT` block appears to be dead code (no views of swapchain images anywhere); candidate for removal in a follow-up.
- `IsTiler()` does not include `VK_DRIVER_ID_IMAGINATION_PROPRIETARY`; evaluate after device validation.
- Format 44 (`B8G8R8A8_UNORM`) on PowerVR: needs the Stage 4c device log to decide between dropping STORAGE from wanted usage vs a swizzled sampled-only alternative.
- `A8B8G8R8_SRGB_PACK32 → R8G8B8A8_SRGB` alternative: sound, unreported, trivial follow-up.
- Whether Stage 2's loadOp=CLEAR should become the default for all tilers (it is the canonical TBDR pattern) — needs benchmarks on Adreno/Mali.

## What was NOT done (deliberately)

- No commits, no branch pushes, no PRs (runbook + AGENTS.md).
- No changes for Class B (memory asserts) or Class E (Config::SaveValues) — documented in `crash_matrix.md` only.
- No SPIR-V bounds-check polyfills, no assertion suppression, no revert of `5edcdea7`.
- `tensor_workaround.patch` / `powervr-swapchain-fix.patch` attachments were not retrievable in this environment (network tool restrictions); their content is superseded by / reconstructed from the nicman23 fork commit, which was retrieved and matched against this tree. Verify against the originals if exactness matters to you.
