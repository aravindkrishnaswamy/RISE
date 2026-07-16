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
