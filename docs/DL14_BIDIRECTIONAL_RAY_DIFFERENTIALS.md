# DL-14: ray differentials under BDPT/VCM/MLT — CLOSED on re-verification

Debt-cleanup slice `debt-dl14`, branched from `master` `aa64c45e`,
2026-09-17.

## 1. The row as filed

> Ray differentials (`fw`/`fwo`) are 0 under BDPT/VCM/MLT — the FIELD
> exists and is carried, but no differentials are ever computed for
> those walks, so it stays zero; a PT-vs-bidirectional difference on
> filtered-noise / footprint-driven bands.

Source: `docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md` §10, itself
citing §2's site-audit table, itself citing `BDPTVertex.h`'s own field
comment ("all-zero under today's bidirectional rasterizers (they emit
no ray differentials) but carried so a future landing cannot silently
reopen the gap").

## 2. Re-verification: the premise is false

Per the debt-cleanup red-proof protocol, step 1 is "re-verify the row
on this HEAD first... if the defect is gone, strike the row with
evidence and stop that row." That is what happened here — no
production code needed to change.

### 2.1 The camera ray already carries differentials, unconditionally

Every `ICamera` implementation stamps Igehy (1999) ray differentials
onto the ray it returns from `GenerateRay`, with **no parameter, flag,
or caller check anywhere in that call** — it does not know or care
which rasterizer asked for the ray:

- `PinholeCamera::GenerateRay` — `PinholeCamera.cpp:203-207`
- `ThinLensCamera::GenerateRay` — `ThinLensCamera.cpp:450`
- `OrthographicCamera::GenerateRay` — `OrthographicCamera.cpp:170`
- `FisheyeCamera::GenerateRay` — `FisheyeCamera.cpp:202` (with a
  documented, pre-existing, integrator-agnostic exception for a rim
  pixel whose +x/+y neighbour falls outside the 180-degree disc — see
  `docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md`)

### 2.2 BDPT/VCM/MLT obtain the eye subpath's first ray through that same call

- `BDPTPelRasterizer.cpp:95` — `camera.GenerateRay( rc, cameraRay, ptOnScreen )`
- `VCMPelRasterizer.cpp:299` — `pCamera->GenerateRay( rc, cameraRay, ptOnScreen )`
- `MLTRasterizer.cpp:198` — `camera.GenerateRay( rc, ray, ptOnScreen )`

MLT does not have its own eye-subpath generator at all: `MLTRasterizer`
constructs a `BDPTIntegrator` directly (`MLTRasterizer.cpp:166`) and
calls its `GenerateEyeSubpath` (`MLTRasterizer.cpp:376`) — the
identical function BDPT itself calls.

`BDPTIntegrator::GenerateEyeSubpathImpl` takes that returned
`cameraRay` and assigns it, unmodified, to the walk's `currentRay`
(`BDPTIntegrator.cpp:1652`, `Ray currentRay = cameraRay;`) before the
very first `scene.GetObjects()->IntersectRay(...)` call of the loop.

### 2.3 The shared geometry-intersection layer computes the footprint from it

`Object::IntersectRay` (`Object.cpp:1013-1022`) calls
`ComputeFootprintVectors` + `SolveFootprintUV` — populating
`ri.geometric.txFootprint` — **iff** `ri.geometric.ray.hasDifferentials`
is true, and this code path is shared by every rasterizer; there is no
BDPT/VCM/MLT-specific branch anywhere in it. The same function's own
comment (right above that call) states the actual, integrator-agnostic
rule directly:

> Costs nothing at all when ray.hasDifferentials is false, which is
> every shadow ray, every NEE ray, every photon, every ray after the
> first scattering bounce...

That rule is not something BDPT/VCM/MLT do differently from PT — it is
architectural, and PT is subject to it too (PT's own continuation rays
are fresh `Ray(...)` objects with `hasDifferentials` defaulted to
`false`, exactly the same construction pattern BDPT's own continuation
uses; grep `currentRay = ` in both `PathTracingIntegrator.cpp` and
`BDPTIntegrator.cpp`). **The only ray any integrator ever attaches a
non-trivial footprint to is the very first camera ray**, and BDPT/
VCM/MLT's first camera ray is the identical object PT's is, obtained
through the identical interface call, run through the identical
intersection code.

### 2.4 The vertex-population copy (S1) was not inert

The 2026-09-11 S1 slice (`925c2720`,
`docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md` §3) widened
`BDPTVertex` to carry `derivatives`, `signals` and `txFootprint`, and
added the copy sites:

- `BDPTIntegrator.cpp`'s eye-subpath vertex population block:
  `v.txFootprint = ri.geometric.txFootprint;`
- `BDPTIntegrator.cpp`'s light-subpath vertex population block: same
  line, mirrored
- `PathVertexEval::PopulateRIGFromVertex`: `ri.txFootprint =
  vertex.txFootprint;`

S1's own text believed these three lines were defensive and inert
("carried so a future landing cannot silently reopen the gap") because
it believed the SOURCE (`ri.geometric.txFootprint`) was always zero for
these rasterizers. That belief was never checked against
`ICamera::GenerateRay`'s own behaviour (§2.1 above) — it is wrong for
the depth-0 vertex, and has been wrong since before S1 landed (the
camera-side code was not touched by S1 or by anything since).

### 2.5 VCM inherits this by construction, not by a separate fix

`VCMIntegrator.cpp` contains zero occurrences of `txFootprint` (grep
the symbol). VCM does not need its own copy site because it never
builds its own eye subpath — `VCMPelRasterizer`/`VCMRasterizerBase`
consume the exact same `std::vector<BDPTVertex>` that
`BDPTIntegrator::GenerateEyeSubpath` (the shared generator) produces,
the same object BDPT itself consumes. Whatever is true of vertex 1's
`txFootprint` in that array is true for both integrators simultaneously
and identically — there is no separate mechanism to verify.

## 3. Direct render measurement

New permanent regression test:
`tests/BidirectionalTextureFootprintParityTest.cpp`.

**Scene** (built as an in-memory string, one per rasterizer, rendered
via `IJobPriv::LoadAsciiSceneViaCst` + `Rasterize` + a capturing
`IRasterizerOutput`, the same harness pattern `EnvLightBalanceTest.cpp`
uses): a floor (`infiniteplane_geometry`, rotated to be Y-up) whose
`lambertian_material` reflectance is an `expression_painter` built on
`fbm(P*6.0, 6, 0.5, 2.0)` — the canonical `fw`/`fwo`-driven octave-fade
consumer (`docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md`) — viewed by a
shallow-grazing pinhole camera so the per-pixel world footprint spans
several orders of magnitude across one image, lit by a single
`omni_light`. (A point light, not the more natural directional/ambient
pairing: VCM has no NEE support for either zero-exitance "Step-1"
light type at all — confirmed empirically during this slice's own
scene development, an unrelated, pre-existing, already-documented gap;
see CLAUDE.md "no directional-light sampling at all" and
`docs/SUBMERGED_CAMERA_IOR_SEEDING.md`. A point light sidesteps it
without touching the footprint question this file exists to answer.)

Rendered at `128x96`, `samples 64`, `oidn_denoise FALSE`,
`pixel_filter box`, under `pathtracing_pel_rasterizer`,
`bdpt_pel_rasterizer` and `vcm_pel_rasterizer` (`max_eye_depth 2`,
`max_light_depth 2` on the latter two — direct lighting only, since
DL-14 is about the primary vertex).

**Metric.** Two whole-image statistics per render: the mean luminance,
and a high-frequency energy metric — the mean absolute horizontal
finite difference between adjacent pixels,
`mean(|lum(x+1,y) - lum(x,y)|)`. HF energy, not a per-row-band
standard deviation, because the point light's own 1/r^2 falloff and
off-axis position give the render a smooth illumination GRADIENT that
confounds a per-region variance comparison (measured during this
file's development: per-row coefficient of variation is
NON-MONOTONIC with depth on this fixture, rising then falling, because
"far" here means both "heavily fog-faded" AND "far from the point
light" at once). A per-pixel horizontal finite difference is local
enough that the smooth illumination gradient contributes almost
nothing to it, while the `fbm` texture's own high-frequency content —
exactly what `fw`/`fwo` octave-fades — dominates it, and it needs no
"near" vs "far" row band to be named at all: wherever in the image a
missing footprint would leave unfaded aliasing, it inflates this one
number.

**Measured (one representative run; the test asserts stable bands,
not exact figures):**

| | mean | HF energy |
|---|---|---|
| PT | 0.506787 | 0.0077324 |
| BDPT | 0.506787 | 0.0077322 |
| VCM | 0.507929 | 0.0318011 |

PT vs BDPT agree to about 1e-4 relative on BOTH statistics — consistent
with "identical camera ray, identical intersection code, identical
vertex-population copy", not with independent Monte Carlo estimates of
the same quantity. VCM's mean is within 0.3%; its HF energy is ~4x
PT/BDPT's, which is discussed next.

### 3.1 VCM's higher HF energy is documented noise, not a footprint gap

Re-rendering the VCM scene at 256 and 1024 samples (16x) drops the HF
energy from 0.0318 to 0.0192 to 0.0144 — converging toward PT/BDPT's
0.0077 as sample count rises, the signature of ordinary Monte Carlo
noise, not a fixed bias. This matches RISE's own long-documented VCM
characteristic (CLAUDE.md, citing `docs/UNIFIED_INTEGRATOR_DECISION.md`):
*"VCM loses σ²·T 3-40x off-caustics"* — VCM is measurably noisier than
PT/BDPT on non-caustic scenes by design, and this fixture (a diffuse
floor, one point light, no caustics) is squarely in that regime. An
HF-energy tolerance tight enough to catch a genuine footprint
regression (which blows the metric out by one to two orders of
magnitude — see the red-proof below) would also be tripped by VCM's
ordinary noise at a test-suite-friendly sample count, and a tolerance
loose enough to absorb that noise would not usefully gate the defect
either. The test therefore gates VCM on mean only (loose, 15%) and
relies on §2.5's structural argument — VCM consumes the *same* vertex
array BDPT does, with no VCM-specific footprint code to independently
break — rather than a second noise-tolerant render-level HF check.

## 4. Red-proof

Temporarily inserted, immediately after `Ray currentRay = cameraRay;`
at the top of `GenerateEyeSubpathImpl` (`BDPTIntegrator.cpp:1652`):

```cpp
currentRay.hasDifferentials = false;   // DL14-RED-PROOF: temporary
```

This is the literal code shape the row's own text described ("no
differentials are ever computed for those walks"). Rebuilt library +
test:

```
PT   mean=0.506788 hfEnergy=0.00773251
BDPT mean=0.506233 hfEnergy=0.0235499
VCM  mean=0.506176 hfEnergy=0.0378719
FAIL: PT vs BDPT: whole-image HF energy
```

BDPT's HF energy jumps 3x (0.0077 -> 0.0235, unfaded aliasing leaking
back in) and the PT-vs-BDPT HF-energy check fails, exactly as
predicted. VCM's own HF energy moves 0.0318 -> 0.0379 (+19%) — real,
in the right direction, but small next to its own ~4x noise floor at
this sample count, confirming §3.1's reasoning that a render-level HF
check on VCM specifically would not reliably separate "noise" from
"defect" without many more samples than a fast unit test can afford.

The mutation was reverted; `git diff` on
`src/Library/Shaders/BDPTIntegrator.cpp` at this slice's final HEAD is
empty — **no production code changed in this slice.**

## 5. The light-vertex footprint question

The task brief that opened this slice asked for a decision (and its
measurement) on what a LIGHT-subpath vertex's footprint should be at a
connection, since PT has no light-subpath concept to compare against.
Given §2.3's actual rule — footprint is non-zero ONLY on the very
first ray any walk casts, camera or otherwise, because every
subsequent ray in this renderer carries no differentials at all — the
answer falls out without a design choice: a light-subpath vertex's
first ray is the EMISSION ray, which never carries camera-style pixel
differentials (there is no "pixel" on the light side to differentiate
against), so its footprint is zero by the exact same mechanism that
zeroes an eye-subpath vertex past depth 0. This already matches what
`GenerateLightSubpathImpl` does — it has its own copy site
(`v.txFootprint = ri.geometric.txFootprint;`, mirroring the eye side)
that faithfully propagates whatever `Object::IntersectRay` computes,
which is zero for a light ray. No special-casing was needed or added.

## 6. Stale-claim corrections

| File | What was wrong | Fix |
|---|---|---|
| `src/Library/Shaders/BDPTVertex.h:162` | Field comment asserted `txFootprint` is "all-zero under today's bidirectional rasterizers (they emit no ray differentials)" | Rewritten to state the actual (correct) behaviour: non-zero at the depth-0 eye vertex, zero elsewhere, identical to PT |
| `docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md` §2 | "BDPT/VCM/MLT eye rays carry no ray differentials (no `hasDifferentials` anywhere in the BDPT/VCM/MLT rasterizers)" | Corrected in place with the §2.1-§2.4 mechanism and a pointer to this doc |
| `docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md` §10 | "Ray differentials under BDPT/VCM/MLT — none today" residual bullet | Struck (`~~...~~`) and marked CLOSED with the same mechanism |
| `tests/SignalIntegratorConsistencyTest.cpp` (file header) | Repeated the same "carry no ray differentials at all" claim as the reason its own scenes avoid `fw`/`fwo` | Corrected; the design choice to avoid `fw`/`fwo` in that suite's scenes is kept, but re-justified on the DEPTH>=1 rule (that suite exercises multi-bounce signals) rather than the false depth-0 claim |
| `docs/DEBT_LEDGER.md` DL-14 row | The row itself | Struck, CLOSED, evidence added (this document) |

## 7. Gate

Touched-class suites re-run against this slice's final (unmutated)
HEAD:

- `tests/BidirectionalTextureFootprintParityTest.cpp` (new): 8/8
- `tests/BDPTVertexRIGRebuildTest.cpp`: unaffected (no field added/
  removed, no copy-site behaviour changed)
- `tests/TextureFootprintTest.cpp`: unaffected (this suite exercises
  `Object::IntersectRay`/`ComputeTextureFootprint` directly, not
  through any bidirectional integrator)
- `tests/SignalIntegratorConsistencyTest.cpp`: unaffected in behaviour
  (comment-only change; its scenes still avoid `fw`/`fwo` by design)
- `tests/BDPTStrategyBalanceTest.cpp`, `tests/VCMStrategyBalanceTest.cpp`,
  `tests/EnvLightBalanceTest.cpp`, `tests/MISWeightsTest.cpp`: no
  production code changed, so these are re-run purely as a "nothing
  moved" confirmation

See `tests/README.md` for the exact counters from this slice's run.

## 8. Cost

None. No production code changed — there is nothing to time.

## 9. Residuals

None opened. The one thing that might look like a residual — "the
fbm-fade discrepancy PT-vs-BDPT past the first bounce" — is not a
BDPT/VCM/MLT-specific gap; it is PT's own pre-existing, disclosed,
"primary-hits-only" architecture (`docs/TEXTURE_FOOTPRINT_ANALYTIC_
DESIGN.md`, `docs/RELIEF_MODIFIER_DESIGN.md`: "Still primary-hits-only:
no ray carries differentials after a scatter"), already true of PT
before this slice and unaffected by it.
