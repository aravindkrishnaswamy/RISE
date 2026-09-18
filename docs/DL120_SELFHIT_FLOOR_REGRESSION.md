# DL-120 — `constant_materials_polish`'s "118× render regression"

**Verdict: REFUTED as a renderer regression. `d01a320a` is correct; the
scenario's committed render band was calibrated against a
self-intersection artifact that `d01a320a` removed.**

Ledger row: `DL-120` (opened by the `debt-cov` slice, 2026-09-14).
Closed by the `debt-dl120` slice, 2026-09-17, on `master` `e290fc64`.
Regression guard: `tests/SelfHitFloorCoincidentOriginTest.cpp` (11
checks; **7 passed / 4 failed** against `d01a320a`'s own predicate
reverted on today's code, 11/0 on HEAD — the verbatim red output is in
§4d).

---

## 1. The claim, and what is true about it

`evals/scenarios/constant_materials_polish.json` carries a `render`
checkpoint banding the scene's mean Rec.709 luma to `[0.01, 0.35]`, with
the scenario's own comment recording *"Measured against the committed
fixture (T10) at samples=256, 32x24: meanLuma ~= 0.18 (the luminaire's
`scale` was tuned 30.0 -> 9.0 to land here)"*. Today the byte-identical
scene renders at **meanLuma 0.001549**, and `AgentEvalCheckTest` fails
that checkpoint.

Everything factual in the row holds up:

* the two live renders (0.182353 at `c33734c7`, 0.001549 at `d471d5d1`)
  reproduce;
* the `debt-cov` slice's first `git bisect run` really was unreliable
  (`set -e` swallowing the intended `exit 125`), and its docs-only
  verdict `85a15d35` really is impossible;
* the corrected bisect a reviewer subsequently ran really does land on
  **`d01a320a`** ("fix(intersection): scale-relative self-hit floor in
  the bilinear-patch solver", 2026-09-03), whose only `src/Library`
  content is a new floor in `RayBilinearPatchIntersection` and the
  removal of a now-redundant second threshold in
  `ClippedPlaneGeometry::IntersectRay_IntersectionOnly`.

What does **not** hold up is the row's framing — *"a genuine, large,
reproducible render-output regression"*. The commit did not break this
scene. It fixed it, and the reference the checkpoint was graded against
is the broken number.

## 2. The configuration

The scenario's scene has exactly one light:

```
pinhole_camera        { location 0 0.3 6.5   lookat 0 0 0   fov 45.0 }
clippedplane_geometry { name quad_emit
                        pta -1.4  1.4 6.5    ptb  1.4  1.4 6.5
                        ptc  1.4 -1.4 6.5    ptd -1.4 -1.4 6.5 }
lambertian_luminaire_material { name mat_emit  exitance pnt_emit  scale 9.0 }
```

The camera sits at `z = 6.5`; the emitter quad **is** the plane
`z = 6.5`, and `(x, y) = (0, 0.3)` is inside its extent. The pinhole is
therefore embedded in the emitter's own surface.

Every camera ray leaves that plane immediately — at `fov 45` looking
down `-z` every generated direction has `dir.z < 0` — so the emitter's
only root along a camera ray is the mathematically **zero** one at the
ray's own origin. A pinhole lying in an infinitely thin emitter sees it
**edge-on**: zero projected area, no contribution. Missing it is the
correct answer.

## 3. What the old predicate did

Pre-`d01a320a`, `RayBilinearPatchIntersection` accepted a root on the
bare test `hit.dRange > 0`. `computet` recovers that zero root as

```
t = (srfpos.z - ray.origin.z) / ray.Dir().z
```

where `srfpos.z` is `EvaluateBilinearPatchAt`'s convex combination of
four corners **all at 6.5** and `ray.origin.z` is exactly 6.5. The
numerator is therefore a pure last-bit rounding residue of a sum whose
exact value is zero, and its **sign is a coin flip** decided by the
particular `(u, v)` the quadratic solve produced. About **6.2 %** of
camera samples landed on the positive side and registered a
full-radiance emitter hit at `t ~ 1e-15`, painting the emitter over the
whole frame.

That reconstructs the "reference" number. This branch's own pre-fix
build (§4) renders the as-authored frame at **0.180096**, and solving

```
0.180096 = f x 2.8648 + (1 - f) x 0.001549
```

for the spurious fraction gives **f = 6.24 %** — i.e. the frame is 6.2 %
emitter and 93.8 % the scene's real, much darker lighting. It sits 1.2 %
under the 0.182353 the scenario recorded at `c33734c7`, the difference
being ~1066 commits of unrelated shading work plus the run-to-run
variation of a last-bit coin flip. The number is not merely wrong, it is
**not reproducible**: it is the fraction of a rounding residue that
happens to land positive, which no other compiler, libm, optimisation
level or platform is obliged to match.

`d01a320a`'s floor, `tMin = NEARZERO * (1 + coordScale)` with
`coordScale` the largest L1 magnitude among the ray origin and the four
corners, is `1.03e-11` for this quad — four orders of magnitude above
the `~1e-15` residue it must reject, and nearly twelve below the
5.8-unit distance a real NEE shadow ray to this light travels. It removes the artifact and nothing
else.

## 4. Evidence

All numbers from `master` `e290fc64` in the `debt-dl120` worktree,
`samples 256`, `32x24`, `pixel_filter box`, `oidn_denoise false`, seed
base 7100. "pre-fix" is today's tree with `d01a320a`'s own predicate —
and only it — reverted to `tMin = 0.0`, which isolates that commit's
hunk on current code rather than dragging in the unrelated
`RootLiesOnRay` / elimination-axis work the file has gained since.

### 4a. Coplanarity is the entire effect

| camera `location` | offset from the emitter plane | pre-fix meanLuma | HEAD meanLuma |
|---|---|---|---|
| `0 0.3 6.5` (as authored) | **0** | **0.180096** | **0.001549** |
| `0 0.3 6.499` | 0.001 | 0.001549 | 0.001550 |
| `0 0.3 6.2` | 0.3 | 0.001713 | 0.001714 |

A 0.001 nudge is 1/2800 of the quad's own width and moves the emitter's
subtended solid angle at the spheres by under 0.1 %. It cannot remove a
legitimate contribution — and it removes the entire 116x difference
(0.180096 -> 0.001549 on the SAME build). Off the plane the two builds
agree to four significant figures, so
`d01a320a`'s floor costs this scene's NEE and BSDF transport nothing
whatsoever; the only rays it changes are the camera rays that were
hitting their own origin.

### 4b. The luminaire is healthy, closed-form

Put the camera at `z = 7.5`, one unit behind the quad, looking at its
back face (`clippedplane_geometry` is `doublesided` by descriptor
default; at `fov 45` the frame at that distance is 0.83 × 1.10, entirely
inside the 2.8-wide quad, so every pixel is emitter):

```
measured L = 2.864801   expected exitance*scale/pi = 9.0/pi = 2.864789   ratio = 1.000004
```

So the light emits exactly what it should. The as-authored frame is dark
because the light is **edge-on**, not because anything about it is
broken — which also disposes of the row's "scaling `scale` 9.0 -> 900.0
gave a proportional 100x brightness increase, confirming the light is not
disconnected": a linear response to `scale` is equally consistent with a
light that is working and merely contributing very little.

### 4c. The scene could never have reached 0.18 by lighting alone

Closed-form ceiling. The quad has area 7.84 at ~6.5 from the spheres, so
it subtends ≈ `7.84 / 6.5²` = 0.186 sr; at `L = 2.8648` that is an
irradiance of ≈ 0.53 on a facing surface. Even for a **perfectly white
Lambertian** sphere set (the scene's `pnt_dark` is 0.12/0.10/0.09) the
reflected radiance would be `0.53 / pi` = 0.17, and the three spheres
cover ≈ 12 % of the 32×24 frame — a whole-frame mean of ≈ **0.02**,
against a band floor of 0.01 and a recorded reference of 0.18. No
lighting-only configuration of this scene reaches 0.18. The number could
only ever have come from the emitter being *in frame*.

### 4d. Red-proof, verbatim

`tests/SelfHitFloorCoincidentOriginTest.cpp` run against the pre-fix
build described above (only `d01a320a`'s own `tMin` expression reverted
to `0.0`, everything else at `master` `e290fc64`):

```
[1] camera coplanar with the emitter plane (DL-120)
  pass: all three camera positions rendered
    camera z = 6.5   (IN the emitter plane)  meanLuma = 0.180096
    camera z = 6.499 (0.001 off the plane)   meanLuma = 0.001549
    camera z = 6.2   (0.3 off the plane)     meanLuma = 0.001713
    |onPlane - nudged| / mean = 1.9659
  FAIL: a 0.001 camera nudge off the emitter plane does NOT change the frame (the coplanar frame carries no emitter contribution)
    |onPlane - moved|  / mean = 1.9623
  FAIL: moving the camera 0.3 off the emitter plane changes the frame only by its framing
  FAIL: as-authored fixture meanLuma is the scene's genuine lit level (0.001549 +- 40%)

[2] the luminaire's own radiance, closed-form
  pass: behind-the-emitter frame rendered
    measured L = 2.864801   expected exitance*scale/pi = 2.864789   ratio = 1.000004
  pass: emitter radiance matches exitance*scale/pi within 1% ...

[3] the debt-21 self-hit is still rejected
    SPURIOUS self-hit: Z=1 tilt=0.001 t=1.29849e-16
    spurious self-hits: 214 / 1536
  FAIL: a ray leaving the patch never re-hits it at its own origin (debt-21 holds)

[4] the self-hit floor is scale-relative across producers
    bilinear patch: 16 / 16
    sphere: 16 / 16
    infinite plane: 16 / 16
    triangle: 16 / 16
==========================================================
Passed: 7   Failed: 4
```

On `master` `e290fc64` unmodified the same binary reads
`Passed: 11   Failed: 0`. Note case [3]: the pre-fix predicate also
lets 214 of 1536 genuine debt-21 self-hits back through at
`t ~ 1.3e-16`, so reverting the floor to "fix" DL-120 would reopen
debt 21 as well.

## 5. Why the floor cannot be "capped below every true root"

The row's natural follow-up — make the floor relative to the ray's own
parameterisation, so it can never exceed a genuine hit distance — was
derived and **rejected on the algebra**, without implementing it,
because it provably defeats the floor in exactly the case the floor
exists for. (Not implemented: the derivation below is decisive, and
§4d's case [3] independently measures what happens to debt-21 the
moment the floor stops covering that root.)

`Object::IntersectRay` publishes a hit point backed off along the
incoming ray by `SURFACE_INTERSEC_ERROR` (1e-12). A shadow or
continuation ray from that point does **not** start on the surface; it
starts 1e-12·|cos in| off it, and its root back at the surface is
`t = 1e-12 · |cos in| / |cos out|` — a *genuine* root of that offset
origin, unbounded as the outgoing ray grazes. Capping the floor at (a
fraction of) the distance from the ray origin to the patch computes
`0.5·1e-12·|cos in| / |q|`, which is **below** that root for every
direction, so the self-hit would be accepted again and debt-21 would
reopen. The same argument applies to
`RaySphereIntersection` / `RayPlaneIntersection` /
`RayTriangleIntersection`, whose own comments already state it.

"The floor is below every true root" is therefore **not an achievable
invariant** for a self-hit floor of this kind, and it is not claimed
anywhere. The invariant that *is* achievable, and that
`tests/SelfHitFloorCoincidentOriginTest.cpp` case [4] pins, is
**scale-relativity**: `NEARZERO` is 1e-12 ≈ 4500 × `DBL_EPSILON`, so
`NEARZERO · (1 + coordScale)` is a *constant count of ulps* of the
coordinates involved, and geometry below that relative band is
unresolvable in double precision anyway. A root more than 1e-9 relative
to the coordinate scale is accepted at every world position from 1 to
1e9, for all four producers.

## 6. Sibling audit (`docs/skills/audit-by-bug-pattern.md`)

Bug pattern as stated by the row: *"a scale-relative self-hit floor
derived from an object's world position can exceed a genuine hit
distance."* Confirmed as **a property of the design, not a defect**
(§5). Every producer carrying this floor was checked at four world
scales × four relative gaps:

| producer | `coordScale` | adversarial probe (16 configs) | verdict |
|---|---|---|---|
| `RayBilinearPatchIntersection` | `max(|origin|₁, max_c |corner_c|₁)` | 16/16 accepted | no defect |
| `RaySphereIntersection` | `|origin|₁ + |radius|` | 16/16 accepted | no defect |
| `RayPlaneIntersection` | `|origin|₁` | 16/16 accepted | no defect |
| `RayTriangleIntersection` | `|origin|₁ + max_v |v|₁` | 16/16 accepted | no defect |
| `BoxGeometry::DropSelfHitRoot` | per-axis band (debt 25) | not a single scalar floor; `PrimitiveSelfHitTest` owns it | out of pattern |
| `CSGObject::SelfHitRootFloor` | delegates to the operand's own floor | inherits whatever the operand reports | out of pattern |

The bilinear patch is the only one that maxes over the **geometry's**
coordinates as well as the ray origin's, which is right for it (its
`computet` numerator is built from `EvaluateBilinearPatchAt`, so the
corners' magnitude really is in the round-off) and matches
`RayTriangleIntersection`'s own max-over-vertices, itself added by
`a8bef210`'s adversarial review for the same reason.

## 7. Are the other failing replay fixtures the same thing?

**No.** `image_reconstruct_multi` (4 render checkpoints),
`image_reconstruct_single` (1) and its `bas-relief control` (1) do build
`clippedplane_geometry` ground / backdrop / key-light quads, but none of
them is coplanar with the camera at `(2.6, 1.8, 3.2)` (the planes are
`y = 0`, `z = -4.5`, `y = 3.6`). Measured directly — the same
`AgentEvalCheckTest` binary, HEAD versus the same pre-fix build:

| checkpoint | HEAD RMSE | pre-fix RMSE | band |
|---|---|---|---|
| `image_reconstruct_multi[0]` view1 | 0.022470 | 0.022473 | ≤ 0.015 |
| `image_reconstruct_multi[1]` view2 | 0.018625 | 0.018625 | ≤ 0.012 |
| `image_reconstruct_multi[2]` view3 | 0.021009 | 0.021010 | ≤ 0.012 |
| `image_reconstruct_multi[3]` view4 | 0.023405 | 0.023403 | ≤ 0.015 |
| `image_reconstruct_single[0]` view1 | 0.022487 | 0.022476 | ≤ 0.020 |
| `bas-relief control[0]` view1 | 0.023198 | 0.023207 | ≤ 0.020 |

Identical to within 1.1e-5 — MC noise. `d01a320a` is not their cause,
and their 10 `AgentEvalCheckTest` failures are an unrelated reference-PNG
drift, still open under DL-61.

## 8. What is left, and what this slice deliberately did not do

The renderer needs no change and got none: `src/**` on `debt-dl120` is
byte-identical to `master` `e290fc64`.

What remains broken is the **fixture**: `constant_materials_polish`'s
scene is degenerate (the camera is inside its only light) and its render
checkpoint's band was graded on the resulting artifact, so the
checkpoint's own stated purpose — *"Sanity that the scene is actually
lit and visible, not a red herring on a black/blown-out frame"* — is not
served by the scene as authored. Repairing it means either

* **(a)** moving the camera off the emitter plane and raising the
  luminaire `scale` by ~100× so the scene is genuinely lit into the
  existing `[0.01, 0.35]` band (§4c fixes the required factor), or
* **(b)** re-grading the band to the scene's genuine ~0.00155 and
  rewriting the `scale was tuned 30.0 -> 9.0 to land here` comment,

both of which change an eval scenario's own measurement contract and
belong to the eval-corpus owner rather than to a debt-cleanup slice.
Filed as **DL-145**, with this document as its evidence.
`AgentEvalCheckTest` therefore stays at its measured baseline of
**2062 passed / 36 failed** on this branch.

## 9. Lesson

A committed render oracle is only as good as the render it was measured
from. This one was authored the same day it was measured, from a scene
whose camera happened to lie in its own light, and the artifact was
stable enough across re-runs on one machine to look like a physical
number — stable enough that a later, *correct* bisect onto a *correct*
fix read as a 118× regression. Before treating a bisect's "first bad
commit" as the defect, ask which side of it agrees with a
reference-free invariant: here, "nudge the camera 0.001 and re-render"
settled it in two renders and no bisect steps at all.
