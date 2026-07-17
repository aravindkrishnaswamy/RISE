# Wave 3 calibration — `sdf_reconstruct_truth` RMSE grading thresholds

This document records how the per-view `rmseMax` thresholds for the
image->scene-reconstruction eval were derived, so Wave 5's render
checkpoints can consume them without re-deriving the numbers, and so a
future recalibration (if the ground-truth scene or grading spp change)
has a reproducible procedure to follow.

RMSE formula (matches the Wave-2 render checkpoint): 8-bit RGB pixels
normalized to `[0,1]`, `sqrt(mean(((a-b)/255)^2))` after the `/255`
normalization — i.e. `sqrt(mean((a_norm - b_norm)^2))` where
`a_norm = a/255`.

## 1. The four poses

The ground-truth scene's own in-scene camera IS `view1`. All four are
pinhole, 256x256, fov as listed, `up = (0,1,0)`. Full machine-readable
copy: [`POSES.json`](POSES.json).

| view | role | location | lookat | fov |
|---|---|---|---|---|
| view1 | primary front-3/4 (= scene camera) | (2.6, 1.8, 3.2) | (0, 0.8, 0) | 40.0 |
| view2 | right side (+X), notch face-on | (4.0, 1.5, 0.2) | (0, 0.9, 0) | 40.0 |
| view3 | back-3/4 (+X, -Z; leans toward the notch side) | (2.0, 1.8, -3.2) | (0, 0.8, 0) | 40.0 |
| view4 | elevated top-down-ish (~60° elevation) | (1.4, 4.6, 1.7) | (0, 0.7, 0) | 45.0 |

**view3 pose note (important, changed during calibration):** the
original design intent was a "back-3/4" view at `(-2.6, 1.8, -3.0)`
(-X, -Z — the mirror image of view1's front-3/4 corner). That pose was
rendered first and calibrated, and the notch-removal anchor (b, below)
came back statistically indistinguishable from the noise floor at
that pose (`0.00326` vs a `~0.004` floor — i.e. *less* than the floor).
The reason: the notch sits on the object's +X side; a camera at
`(-2.6, ..., -3.0)` views almost exclusively the -X/-Z hemisphere, so
the notch is self-occluded on the far side of the object and never
reaches the image. Since the task requires the notch to be measurably
visible on the silhouette from *both* view2 and view3, the view3 pose
was moved to `(2.0, 1.8, -3.2)` — still a genuine back-3/4 (rotated
~180° from view1 in Z, distinct in framing from both view1 and view2,
pairwise RMSE 0.14-0.24 against every other view, see §4) but leaning
toward the notch's +X side so it clips the silhouette. This is
recorded here rather than only in git history because it's exactly the
kind of "why does this pose look like that" question a future
recalibration will ask.

## 2. Grading spp

Chosen: **64 spp** (vs. 256 spp for the committed references). Measured
as anchor (a) below: the resulting noise floor is `0.0039-0.0045` RMSE
across all four views — small and uniform enough that a single
`rmseMax` regime works for all views without per-view spp tuning.
64 spp render times were ~1-3s per 256x256 view on this machine, fast
enough for a grading loop.

## 3. Anchor measurements

All renders in this section use the grading spp (64), `pixel_filter
box`, `oidn_denoise FALSE` — identical pipeline to the references,
differing only in the stated variant and (for (a)) sample count. RMSE
is measured against the corresponding **committed reference PNG**
(256 spp). Throwaway variant scenes and renders live under the
session scratch dir, not in the repo (see §5 for how to reproduce).

| anchor | what changed | view1 | view2 | view3 | view4 |
|---|---|---|---|---|---|
| (a) noise floor | GT scene, spp 256→64, same everything else | 0.00453 | 0.00393 | 0.00447 | 0.00419 |
| (b) shape-detail (no notch) | remove the `roundbox subtract` SDF part | 0.00900 | 0.01663 | 0.01587 | 0.00967 |
| (c) lighting | key light color `1.0 0.82 0.58` → `1.0 1.0 1.0` (neutral white) | 0.03810 | 0.03250 | 0.03639 | 0.04790 |
| (d) material | object color `0.05 0.55 0.5` (teal) → `0.5 0.5 0.5` (grey) | 0.07710 | 0.05766 | 0.04600 | 0.08724 |
| (e) gross failure | `hero` object deleted entirely (empty stage) | 0.11840 | 0.13346 | 0.17000 | 0.12647 |

Observations:

- Anchors are monotonically increasing in severity in every view:
  floor < notch < lighting < material < empty-stage. This is the
  expected ordering (a single-part shape detail is a smaller
  perceptual delta than a whole-light-color change, which is smaller
  than a whole-material-color change, which is much smaller than the
  object being entirely absent).
- **Anchor (b) is only clearly separated from the noise floor on
  view2 and view3** (~3.6-4.2x the floor) — the two views the scene
  was specifically posed to catch the notch on. On view1 and view4 the
  notch delta (0.0090, 0.0097) sits right at ~2x the local noise floor,
  too close to the floor to set a threshold that reliably rejects a
  notch-less reconstruction without also rejecting clean noise. This
  matches the design note in the scene header: the notch reads
  "oblique/subtle" from the front-3/4 and is mostly hidden from
  directly above. **Per the task's explicit allowance, the notch is
  caught only by view2/view3's thresholds; view1/view4 do not gate on
  it.**

## 4. Reference-view distinctiveness (sanity gate)

Pairwise RMSE among the four committed 256-spp reference PNGs (all
comfortably above any of the thresholds below, confirming the four
poses are genuinely different images):

| pair | RMSE |
|---|---|
| view1 vs view2 | 0.1301 |
| view1 vs view3 | 0.1992 |
| view1 vs view4 | 0.1730 |
| view2 vs view3 | 0.1435 |
| view2 vs view4 | 0.1824 |
| view3 vs view4 | 0.2389 |

## 5. Chosen thresholds

Target: **moderate strictness** — "a person would say that's the same
object staged the same way." Each `rmseMax` sits at least 2x above the
local noise floor and below every anchor it's expected to catch
((c) lighting, (d) material, (e) empty-stage in all four views; plus
(b) notch-removal in view2/view3 specifically).

| view | `rmseMax` | vs. floor | vs. anchor (b) | vs. anchor (c) |
|---|---|---|---|---|
| view1 | **0.015** | 3.3x floor (0.00453) | b=0.00900 < threshold — **not caught** (documented, expected) | 39% of c (0.03810) — wide margin |
| view2 | **0.012** | 3.1x floor (0.00393) | b=0.01663 > threshold — **caught**, 27.8% margin | 37% of c (0.03250) — wide margin |
| view3 | **0.012** | 2.7x floor (0.00447) | b=0.01587 > threshold — **caught**, 24.4% margin | 33% of c (0.03639) — wide margin |
| view4 | **0.015** | 3.6x floor (0.00419) | b=0.00967 < threshold — **not caught** (documented, expected) | 31% of c (0.04790) — wide margin |

Every threshold clears (d) and (e) by an even larger margin than (c)
since those anchors are strictly larger deltas in every view (see the
table in §3).

Rationale for the specific split (0.015 for view1/view4, 0.012 for
view2/view3): view2 and view3 are the two poses whose whole purpose is
catching the notch, so their thresholds are pulled down as far as the
floor allows to preserve that signal (2.7-3.1x above floor, notch
anchor still caught with ~25-28% margin). view1 and view4 don't have a
notch signal separable from noise at this spp, so their thresholds sit
a bit higher (3.3-3.6x above floor) purely for extra noise headroom —
they're still well clear of the lighting/material/empty-stage anchors,
which is what actually matters for those two views.

## 6. Reproducing / recalibrating

1. Build `./bin/rise` if missing: `make -C build/make/rise -j8 all`.
2. Regenerate the committed references (256 spp):
   `./evals/references/generate_references.sh`
3. For recalibration at a different grading spp or after editing the
   ground-truth scene: for each of the 4 poses and each of the 5
   anchor variants (a)-(e) above, derive a scene variant (pose applied
   via the same location/lookat/fov substitution as
   `generate_references.sh`, `samples` set to the grading spp, plus
   the anchor's one-line content change — remove the `roundbox
   subtract` part line for (b), swap the key-light `color` for (c),
   swap the `pnt_teal` painter `color` for (d), delete the `hero`
   `standard_object` block for (e)), render it, and compute
   `sqrt(mean(((a-b)/255)^2))` against the matching committed
   `viewN.png`. Re-derive thresholds using the same rule: >=2x the
   local noise floor (anchor a), below every anchor the view is
   expected to catch, with view2/view3 pulled down toward the floor to
   preserve notch-removal separation and view1/view4 given extra
   headroom since they don't carry that signal.
4. This is intentionally a throwaway/manual procedure (matches the
   task's "tiny throwaway python RMSE script" framing) — no calibration
   driver script is committed under `evals/references/`; only the
   truth scene, the 4 PNGs, `POSES.json`, `generate_references.sh`, and
   this document are.

## 7. Numbers for Wave 5

```json
{
  "grading_samples": 64,
  "rmseMax": { "view1": 0.015, "view2": 0.012, "view3": 0.012, "view4": 0.015 }
}
```

## 8. Wave 5 finding: `AgentSession::Render` diverges from the CLI batch pipeline (2026-07-16)

> **RESOLVED — see §9.** Root cause was the agent render path missing the
> file-output display transform (ACES): fixed at source in
> `InMemoryRasterizerOutput`/`AgentSession` (commit `0251c62d`), after which the
> pristine ground-truth scene grades at RMSE 0.0045 through `AgentSession` —
> equal to the §3 CLI noise floor — at the UNCHANGED §5 thresholds, and the
> neutral-key adversarial control fails the caps again. The §8 text below is
> kept as the historical record of the discovery and localization; its
> "UNREACHABLE" framing no longer applies. The TLAS/BVH boundary hypothesis
> flagged below was tested and REFUTED (rendering the CLI with
> `display_transform none` collapsed the difference to RMSE 0.0023 — the tone
> curve was the entire divergence).

**These thresholds (§5/§7) are correct for CLI-vs-CLI comparisons but are
currently UNREACHABLE through the `render` checkpoint's actual grading path**
(`AgentSession::Render`, used by `evals/scenarios/image_reconstruct_single.json`
/ `image_reconstruct_multi.json`), because that path renders the SAME scene
content measurably differently from the `./bin/rise scene.RISEscene` CLI
pipeline this document's anchors were derived through. This is a genuine,
reproducible discrepancy in the shared eval-harness rendering
infrastructure (`src/Library/Agent/AgentSession.cpp`'s `Render`), **not** a
defect in the Wave 5 `image_reconstruct_*` scenarios/fixtures — the
reconstruction fixtures lift the ground-truth scene's chunks correctly (see
below). Recorded here because the render-checkpoint calibration and
recalibration procedure both live in this file, and this needs to be known
before either scenario's `rmseMax` is ever touched again.

**Reproduction (no reconstruction involved at all):** load
`sdf_reconstruct_truth.RISEscene` directly (`scene.path`, no edits) through
`RunScenario`/`AgentSession`, exactly as `CheckRenderKind` does for a
`compareToImage` checkpoint (samples=64, camera=viewN, width=height=256),
and compare the resulting PNG against the SAME committed `viewN.png`
`CheckRenderKind` compares against:

| view | CLI-pipeline RMSE (§3 anchor a) | `AgentSession`-pipeline RMSE (same file, same pose, same spp) |
|---|---|---|
| view1 | 0.00453 | **0.0616** |
| view2 | 0.00393 | **0.0566** |
| view3 | 0.00447 | **0.0611** |
| view4 | 0.00419 | **0.0770** |

~13-18x the CLI floor, for the byte-identical scene. Ruled out as the cause:

- **Not sample count / the `samples` override mechanism.** Re-rendering
  view1 through `AgentSession` at the scene's AUTHORED 256 spp (no
  `AgentRenderParams::samples` override at all, `samplesOverridden=false`,
  `effectiveSamples=256`) gives the identical linear mean (`meanR=0.09691`)
  and RMSE (0.0616) as the 64-spp override render. If this were a
  convergence/override-plumbing issue it would close at full authored spp;
  it does not.
- **Not environment/`radiance_map` handling specifically.** Stripping
  `radiance_map`/`radiance_scale` from the scene entirely and comparing an
  `AgentSession` render against a CLI render of that SAME no-env variant
  (so both sides get zero env contribution) still diverges by RMSE 0.0552
  — essentially the same magnitude as the with-env case. The discrepancy is
  present with or without env, so it is not an env-sampler caching issue
  (the known `SetEnvironmentSampler`-after-`Prepare()` class of bug
  documented elsewhere in this codebase does not explain it).
- **Not simple noise, and not even monotonic with a real defect.**
  Recolouring the key light neutral white (§3 anchor c: this scene's actual
  committed adversarial control, `TestAdversarialOracleControls` control
  (i) in `tests/AgentEvalCheckTest.cpp`) measures RMSE 0.0428-0.0533 across
  all four views through `AgentSession` — LOWER than the correct
  reconstruction's own 0.0566-0.0770 floor, in every view. Through the CLI
  pipeline the same edit (§3 anchor c) is unambiguously ABOVE the floor
  (0.033-0.048 vs a 0.0039-0.0045 floor, 8-11x). So under the CLI pipeline
  RMSE is monotonic in reconstruction correctness (wrong lighting scores
  worse than right lighting) but under the `AgentSession` pipeline it is
  NOT (this specific wrong-lighting variant scores BETTER than the
  admittedly-correct reconstruction). This means naively recalibrating
  `rmseMax` upward to accommodate the `AgentSession` floor would not just
  weaken the checkpoint — it would make the neutral-key adversarial control
  it's supposed to catch score AS PASSING, which is worse than the
  checkpoint currently failing loudly. That is why this is being
  documented and flagged rather than "fixed" by loosening the caps.
  Suspected root cause (untested, flagged for the follow-up
  investigation): the largest per-block RMSE concentrates on the ground
  plane under the key area light's soft shadow (not the background/sky
  region), which is consistent with a soft-shadow/area-light sampling
  difference between whatever acceleration/traversal path `AgentSession`'s
  session-based `Job` construction ends up using for this exact 4-object
  scene and the CLI's fresh single-shot parse — `sdf_reconstruct_truth`
  has EXACTLY 4 top-level objects (ground/backdrop/key/hero), which per
  `docs/ARCHITECTURE.md`'s top-level BVH description sits right at the
  `items.size() > nMaxObjectsPerNode` boundary that decides whether the
  linear-loop fallback or the BVH is used — worth checking first.
- **Reconstruction fidelity is not the cause.** The Wave 5 fixtures'
  reconstructed document, rendered through `AgentSession`, reproduces the
  pristine ground-truth-via-`AgentSession` baseline within measurement
  noise (Δ ≈ 0.0001 RMSE on view1: 0.061698 reconstructed vs 0.061618
  pristine-direct-load) — i.e. the chunk-lifting is correct; 100% of the
  residual against the committed reference is the pipeline discrepancy
  above.

**Consequence for Wave 5**: `evals/scenarios/image_reconstruct_single.json`
and `image_reconstruct_multi.json`'s four `compareToImage` checkpoints are
correctly calibrated against the CLI pipeline (§5/§7) and correctly
implemented, but currently FAIL in `T10`
(`TestSeedScenariosCheckpointsAreTrue`, `tests/AgentEvalCheckTest.cpp`) for
every committed fixture — including a byte-for-byte-correct
reconstruction — because of this upstream `AgentSession::Render` bug, not
because of a fixture defect. Per the explicit instruction not to silently
loosen `rmseMax` (and given the monotonicity failure above shows loosening
would silently break the neutral-key adversarial control's whole point),
these two scenarios' render checkpoints are left at their CLI-calibrated
values, and this failure mode is expected until `AgentSession::Render` is
fixed to match the CLI pipeline's output for the same scene. A follow-up
session should: (1) confirm/refute the BVH-vs-linear-loop hypothesis above,
(2) compare `AgentSession`'s `Job` construction / prepare sequence against
`Job::LoadAsciiSceneAuto` + the CLI's render entry point for a
divergence, (3) re-run this section's reproduction table to confirm a fix,
then (4) leave `rmseMax` untouched (it will simply start passing).

## 9. RESOLVED (2026-07-16): the divergence was a missing display transform

The Wave-5 finding above is now **fixed**. Root cause: the CLI
`file_rasterizeroutput` PNG pipeline applies a **display transform**
(exposure + tone curve, defaulting to **ACES filmic** for LDR formats — see
`DisplayTransformWriter` / `FrameEncoders.cpp`) between the linear film and
the sRGB OETF, but the agent-facing sink `InMemoryRasterizerOutput::ToPng`
applied only the sRGB OETF with an **identity tone curve**. So every agent
render (`AgentSession::Render` → `read_image` / `compareToImage`; also
`read_viewport`, whose interactive-frame copy is likewise raw linear) was a
raw linear→sRGB image while the committed references were ACES-tonemapped.
That is a *pure encode* difference, not a sampler/BVH/env issue — the
hypotheses in §8 (BVH-vs-linear-loop, `Job` construction) were all wrong.

**Localization that pinned it:** a session render of the pristine truth
scene vs. a CLI render with `display_transform none` added matched at
RMSE **0.0023** (zero mean diff), while vs. the default (ACES) CLI/reference
it was RMSE **0.0616** with a systematic darkening skewed toward blue/green
(A−B ≈ R−4, G−9, B−11 in 8-bit) — the ACES contrast/roll-off, not noise.

**Fix (source, not thresholds):** `InMemoryRasterizerOutput` gained a
`SetDisplayTransform(exposureEV, toneCurve)` that wraps the encode in the
SAME `DisplayTransformWriter` the CLI uses; `AgentSession` resolves the
effective transform per head (`ResolveBeautyDisplayTransform_`: a declared
LDR `file_rasterizeroutput`'s `display_transform` + `exposure`, else the LDR
default ACES, plus the active camera's exposure-compensation EV) and installs
it on the **beauty** sinks (production + draft + `read_viewport`). The
**objectmap** identity sink is deliberately left at the identity transform so
its per-pixel identity bytes pass through un-tonemapped. `MeanChannels` /
`GetPixelColor` still report the stored **linear** pixels (an
encode-independent signature; the luma-band scenarios that consume them are
unaffected).

**Re-verified against the EXISTING references (no recalibration, `rmseMax`
untouched):** a pristine-truth session render now scores RMSE **0.0045** on
view1 (== the §3 anchor-(a) CLI floor of 0.00451), and `T10`'s eight
`compareToImage` checkpoints pass. The neutral-key adversarial control
(`TestAdversarialOracleControls` control (i)) still **fails** the caps (it
now goes through the identical ACES pipeline the §3 anchor (c) 0.033–0.048
was measured on), so the checkpoint keeps its discriminating power. Because
the two pipelines are now identical, the §5/§7 CLI-calibrated thresholds are
reachable as designed; the §8 caveat is retired.

## 10. `image_reconstruct_single` two-tier grading (2026-07-17)

`image_reconstruct_single.json` supplies the model only `view1.png` (see its
single prompt attachment) but, before this change, graded ALL FOUR
`compareToImage` checkpoints at the §5/§7 CLI-calibrated caps
(view1=0.015, view2=view3=0.012, view4=0.015). Those caps were derived to
also reject a *shape* defect (anchor (b), the notch) on views 2/3 — geometry
that is genuinely UNOBSERVABLE from the single supplied view1 photo. Grading
the hidden side that strictly meant a correct, honest reconstruction that
plausibly continues the unseen surface (rather than guessing the exact
notch position/depth) could fail views 2-4 for guessing "wrong" on geometry
it was never shown — i.e. the strict caps were partly measuring priors/luck
on the hidden side, not reconstruction fidelity of what's actually visible.
`image_reconstruct_multi.json` (which supplies all four views) is UNCHANGED
and keeps the full §5/§7 strict caps on every view — every checkpoint stays
fully observable there.

**Fix: two-tier caps for `image_reconstruct_single` only.**

- **view1 stays STRICT: `rmseMax` 0.015** (unchanged from §5/§7) — it is the
  ONE fully-observable view (the model's own attached photo), so nothing
  about its grading is relaxed.
- **views 2/3/4 become SHAPE-PLAUSIBILITY caps: `rmseMax` 0.04 each**
  (relaxed from 0.012/0.012/0.015). This admits a reconstruction whose
  hidden-side geometry is a *plausible smooth continuation* of the visible
  form (no notch guessed, or a differently-shaped smooth backside) while
  still rejecting a materially wrong reconstruction (empty stage, a
  billboard/flat-cutout hero, wrong material, wrong lighting).

**Anchor arithmetic for the 0.04 relaxed cap** (reusing the §3 anchor
measurements, all in RMSE):

| anchor | view2 | view3 | view4 | vs. 0.04 cap |
|---|---|---|---|---|
| (b) shape-detail, no notch (the "hidden side, honestly plausible" proxy) | 0.01663 | 0.01587 | 0.00967 | **ADMITTED** — all three sit at ≥2.4x margin below 0.04 (0.04/0.01663 = 2.4x, 0.04/0.01587 = 2.5x, 0.04/0.00967 = 4.1x) |
| (e) gross failure, empty stage | 0.13346 | 0.17000 | 0.12647 | **REJECTED** — 3.2-4.5x ABOVE the cap |
| billboard / silhouette-collapse class | ≈ empty-stage magnitude (side-on views of a flat cutout lose nearly all silhouette agreement with a 3-D reference, the same failure shape as anchor (e)) | — | — | **REJECTED**, same margin as (e) |

Anchor (c) lighting (0.0325-0.0479) and (d) material (0.0460-0.0872) remain
ABOVE 0.04 on view3/view4 and straddle it closely on view2 (0.0325 <
0.04) — this is intentional and harmless: those defects are also caught by
view1's own strict 0.015 cap (lighting/material are observable in the
single supplied photo), so views 2-4 do not need to be the ones catching
them. The two-tier split's job is narrowly to stop penalizing UNOBSERVABLE
hidden-side shape guesses, not to re-derive every anchor's separation on
every view.

Why not just drop the view2-4 checkpoints instead of relaxing them: dropping
them entirely would re-admit a billboard/flat-cutout reconstruction that
happens to match view1 face-on but has no real volume from any other
angle — the multi-view compares are still load-bearing for that class of
defect, just at a plausibility threshold instead of a fidelity threshold.

Adversarial controls (`TestAdversarialOracleControls` in
`tests/AgentEvalCheckTest.cpp`) (h) (empty stage) and (i) (neutral-key
lighting) only assert against `checkpoints[0]` (view1, still strict at
0.015) and the non-render checkpoints — neither hardcodes an expectation
against checkpoints 1-3, so neither needed a code change for this retier;
verified by reading both control blocks. Control (h)'s empty-stage anchor
(e) is 0.118-0.170 across every view, comfortably above BOTH the 0.015
strict cap (view1) and the 0.04 relaxed cap (views 2-4) it would also fail
if it were asserted. Control (i)'s neutral-key anchor (c) view1 value
(0.0381) is above the unchanged 0.015 strict cap, so its lone assertion is
unaffected by the retier.
