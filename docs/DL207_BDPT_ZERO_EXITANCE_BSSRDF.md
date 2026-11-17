# DL-207 — BDPT/MLT zero-exitance sweep priced a BSSRDF entry vertex through the raw aggregate BSDF, not Sw

Status: **CLOSED 2026-09-18** (debt-dl207 slice, branch `debt-dl207`).
Red-proof `tests/BDPTZeroExitanceBSSRDFTest.cpp` (new, 29 checks).

Related: found by review of the `debt-dl96` slice (docs/DL96_BSSRDF_OPEN_SHEET_ENTRY.md
"A note on the light source"), which mis-attributed the same symptom to
a general BDPT directional-light gap and declined to file it — that
attribution was wrong (see that doc's own correction). CLAUDE.md's
"VCM has no directional-light sampling" note is a *different*, still
open gap and is explicitly NOT what this row is about — BDPT's
directional/ambient NEE is fine in general (Part A below);
only its pricing of a BSSRDF entry vertex was broken.

---

## 1. The bug

`BDPTIntegrator.cpp`'s `EvaluateAllStrategiesImpl` (the shared,
Tag-templated (s,t) driver behind both `EvaluateAllStrategies` (RGB)
and `EvaluateAllStrategiesNM` (spectral hero/companion, i.e. also the
HWSS path — `BDPTSpectralRasterizer.cpp`/`MLTSpectralRasterizer.cpp`
call it once per active wavelength)) contains a deterministic
"zero-exitance-light sweep": directional and ambient lights have
`radiantExitance() == 0`, so they are excluded from the light-selection
alias table and are otherwise invisible to every stochastic (s,t)
connection strategy. This sweep hand-delivers their contribution to
every eligible eye vertex with `misWeight = 1.0` (the light cannot be
reached any other way, so there is nothing to partition against —
this is the unique unbiased estimator, and it correctly mirrors
`LightSampler::EvaluateDirectLighting`'s own "Step 1").

Before this fix, the sweep read:

```cpp
for( unsigned int t = 2; t <= nEye; t++ )
{
    const BDPTVertex& eyeEnd = eyeVerts[t - 1];

    if( eyeEnd.type != BDPTVertex::SURFACE ) continue;
    if( !eyeEnd.isConnectible ) continue;
    if( !eyeEnd.pMaterial ) continue;

    const IBSDF* pBSDF = eyeEnd.pMaterial->GetBSDF();
    if( !pBSDF ) continue;

    ... l->ComputeDirectLighting( ri, caster, *pBSDF, ... );
```

`eyeEnd.pMaterial->GetBSDF()` is the material's **raw aggregate
front-reflection BSDF** — for `subsurfacescattering_material`, that is
`SubSurfaceScatteringBSDF`, whose own header comment is explicit about
its scope: *"With the BSSRDF approach, only surface reflection is
evaluated ... All other cases: return 0. Subsurface transport is
handled by the diffusion profile, not the BSDF connection."* Both its
`value()`/`valueNM()` additionally require `NdotO > 0 && NdotI > 0` in
the vertex's own local frame, and at the default `roughness 0` they
return exactly 0 unconditionally (a delta lobe, no continuous `value()`
at all).

The eye subpath's own BSSRDF-sampling block (same file, run per bounce
*before* this sweep is ever reached) can, on the transmission branch of
its Fresnel coin flip, `push_back` a **second, distinct vertex**:
`entryV`, with `entryV.isBSSRDFEntry = true`, whose `position`/`normal`
are the diffusion-profile **entry point** returned by
`BSSRDFSampling::SampleEntryPoint` (diffusion-profile materials) or
`RandomWalkSSS::SampleExit` (random-walk materials) — a location
elsewhere on the surface from the camera-visible exit hit, standing in
for "where light re-enters the material to reach the camera via
subsurface diffusion." The zero-exitance sweep, with no special case
for `isBSSRDFEntry`, evaluated `eyeEnd.pMaterial->GetBSDF()` at THIS
vertex too — pricing the wrong physical quantity (a front-reflection
lobe) at the wrong surface location (the entry point, not the exit
point that lobe is defined at) — not merely evaluating a legitimate
lobe at zero for an unrelated reason.

Net effect: **BDPT (and MLT, which constructs a `BDPTIntegrator`
internally) delivered EXACTLY ZERO direct light from a directional or
ambient light to any `subsurfacescattering_material` /
`randomwalk_sss_material` vertex, front face included**, in every tag
(RGB/spectral-hero/HWSS-companion).

A second, independent defect compounds this for `randomwalk_sss_material`
specifically: its entry vertex is marked `isConnectible = false` (a
random walk's exit point has no analytic area-measure `pdfSurface`, so
the GENERAL (s>=1) connection strategies — which need one for MIS —
deliberately skip it; see that vertex's own construction comment, "Mark
the vertex as delta + non-connectible"). The zero-exitance sweep's
`if( !eyeEnd.isConnectible ) continue;` gate, applied unconditionally,
also skips a random-walk entry vertex — so even fixing the BSDF choice
alone would not have restored light to `randomwalk_sss_material`.

## 2. Two candidate designs, and which one is correct

The ledger row's own ruling named two options: route through
`PathVertexEval::EvalBSDFAtVertex` directly, or evaluate the
directional/ambient contribution "at the BSSRDF entry vertex the
subpath already carries." These turn out to be the same design stated
two ways, and a third, more literal option was rejected on inspection:

* **Rejected: call `PathVertexEval::EvalBSDFAtVertex(eyeEnd, wi, wo)`
  directly** and multiply the result by the light's radiance/cosine
  by hand. This would duplicate `ILight::ComputeDirectLighting`'s own
  cosine, shadow-ray (including `transparent_shadows` Fresnel
  attenuation and, for directional lights, medium-transmittance)
  logic outside the `ILight` interface — a second, drifting
  implementation of "how a zero-exitance light illuminates a point."
  `EvalBSDFAtVertex`'s own `isBSSRDFEntry` branch computes `Sw` from
  `wi` and the vertex's `normal` alone (it does not use `wo` at all
  for that branch), so nothing about the *physics* requires bypassing
  `ComputeDirectLighting`.

* **Chosen: keep calling `ILight::ComputeDirectLighting`/
  `ComputeDirectLightingNM` exactly as before, but hand it an `IBSDF`
  that evaluates `Sw(direction)` instead of the raw aggregate.**
  `PathTracingIntegrator.cpp` already solved this exact problem for
  its OWN BSSRDF-entry NEE (`BSSRDFEntryBSDF` / `RandomWalkEntryBSDF`,
  `BSSRDFEntryAdapters.h`) — two small stack-local `IBSDF` adapters,
  each wrapping either the diffusion profile or the random-walk
  material's Fresnel/IOR state, whose `value()`/`valueNM()` compute
  exactly `Ft(cosTheta)/(c*PI)` from the incoming light direction and
  the vertex's own normal — the identical formula
  `PathVertexEval::EvalBSDFAtVertex`'s `isBSSRDFEntry` branch uses.
  Reusing them means the sweep's evaluation, PT's own NEE, and BDPT's
  general (s>=2) connection strategies now all price a BSSRDF entry
  vertex through the SAME three call sites of the SAME formula — no
  new physics, no fourth implementation.

**Derivation check (why Sw at the entry, not the raw BSDF at the
exit, is correct for a zero-exitance light):** a directional/ambient
light illuminating a BSSRDF surface contributes through the standard
separable-BSSRDF factorization `S(xo,wo; xi,wi) ~= (1-Fr(wo)) * Rd(xo,xi) *
(1-Fr(wi))`. The diffusion term `Rd(xo,xi)` and the exit Fresnel
`(1-Fr(wo))` are already priced once, at the moment the entry point
`xi` was *sampled* (folded into `entryV`'s stored `throughput`/
`throughputNM`, computed from `bssrdf.weight{,Spatial}{,NM}` at the
point the vertex was pushed). What remains for a light arriving at
`xi` from direction `wi` is exactly the ENTRY Fresnel transmission term,
`Sw(wi) = Ft(wi)/(c*PI)` — which is precisely what `BSSRDFEntryBSDF`/
`RandomWalkEntryBSDF` compute, and precisely what `ComputeDirectLighting`
then multiplies by the light's own cosine/radiance/shadow-visibility
factors, exactly the same way it does for an ordinary BSDF surface.

## 3. The fix

`BDPTIntegrator.cpp`'s zero-exitance sweep (the single Tag-templated
function that serves RGB, spectral-hero, and every HWSS companion
wavelength) now branches on `eyeEnd.isBSSRDFEntry` **before** the
general `isConnectible`/`GetBSDF()` gates:

* If the vertex's material has a diffusion profile
  (`GetDiffusionProfile()`), construct a `BSSRDFEntryBSDF` bound to it
  and use its address as `pBSDF`.
* Otherwise, if it has random-walk SSS parameters
  (`GetRandomWalkSSSParams()`, or — spectral tag only —
  `GetRandomWalkSSSParamsNM(nm, ...)`, mirroring the exact resolution
  order the entry vertex's own construction code uses), construct a
  `RandomWalkEntryBSDF` bound to its `ior` and use that.
* If neither applies, `continue` (unreachable in practice: a vertex
  cannot be `isBSSRDFEntry` without one of the two).
* **The `isConnectible` gate is deliberately bypassed** for this
  branch: it exists so the GENERAL (s>=1) connection strategies, which
  need a real area-measure `pdfFwd`/`pdfRev` for MIS, never target a
  vertex whose density is only a placeholder. This sweep's MIS weight
  is unconditionally 1.0 for every zero-exitance light regardless of
  the vertex's connectibility — the same reasoning that already makes
  a *non-BSSRDF* delta vertex's contribution here safe without any
  `isConnectible` check at all (it is excluded by `GetBSDF() == 0`
  instead). No pdf consistency is needed, so the exemption costs
  nothing.
* The non-`isBSSRDFEntry` path is untouched: `isConnectible` and
  `GetBSDF()` are checked exactly as before, so an ordinary surface
  vertex (including the camera-visible EXIT hit on an SSS object,
  which legitimately CAN receive a specular front-reflection highlight
  from the light) is priced identically to pre-fix.

`#include "BSSRDFEntryAdapters.h"` and two `using` declarations
(`RISE::BSSRDFAdapters::BSSRDFEntryBSDF`/`RandomWalkEntryBSDF`) were
added near the file's existing `using namespace` block.

Because `EvaluateAllStrategiesNM` is a thin forwarder to the SAME
`EvaluateAllStrategiesImpl<NMTag>` instantiation `EvaluateAllStrategies`
(RGB) forwards to, and `BDPTSpectralRasterizer.cpp`/
`MLTSpectralRasterizer.cpp` call it once per hero AND once per active
HWSS companion wavelength, this single fix reaches RGB, spectral-hero,
and every HWSS companion lane with no separate per-tag code. VCM is
untouched — it has no directional-light sampling path at all (a
separate, already-documented gap; CLAUDE.md).

## 4. Red-proof

`tests/BDPTZeroExitanceBSSRDFTest.cpp` (new, construction API via
`RISE_CreateJobPriv`/`LoadAsciiSceneViaCst`, the same idiom
`tests/BSSRDFOpenSheetEntryTest.cpp` Part B uses). All scenes are a
single light, viewed head-on, so PT's own result has low variance and
is the reference every other integrator is measured against.

**Part A (control) — `lambertian_material` floor + `directional_light`,
PT vs BDPT.** Not a BSSRDF vertex; must be (and is) unaffected by this
fix in either direction:

```
PT luma=2.15699  BDPT luma=2.15741  ratio(BDPT/PT)=1.00019
```

**Part B — `subsurfacescattering_material` slab (double-sided flat
quad) + `directional_light`, PT vs BDPT vs `mlt_rasterizer`.**

```
PRE-FIX:  PT luma=1.37281  BDPT luma=0            ratio=0
          MLTRasterizer:: "Bootstrap found zero luminance --
          scene produces no visible light!"
POST-FIX: PT luma=1.37213  BDPT luma=1.3743  MLT luma=1.36713
          ratio(BDPT/PT)=1.00158  ratio(MLT/PT)=0.996354
```

**Part C — the same slab + `ambient_light`, PT vs BDPT** (the sweep's
OTHER zero-exitance light type):

```
PRE-FIX:  PT luma=0.22905  BDPT luma=0  ratio=0
POST-FIX: PT luma=0.228513  BDPT luma=0.229371  ratio=1.00375
```

**Part D — `randomwalk_sss_material` twin of Part B, on a SPHERE (not
the flat quad).** `RandomWalkSSS::SampleExit` walks a real path THROUGH
the medium and needs an enclosing volume; a flat, zero-thickness
`clippedplane_geometry` has none. Confirmed empirically and
independently of this row's fix: PT itself reads EXACTLY 0 for
`randomwalk_sss_material` on the flat quad under either a directional
or an omni light, while the identical material on a sphere is
non-trivially bright under either — a scene-authoring requirement, not
a BDPT question, so Part D uses a sphere while Parts B/C keep the flat
quad (whose diffusion-profile material has no such requirement — a
local surface probe, not a volumetric walk):

```
PRE-FIX:  PT luma=0.291191  BDPT luma=0  ratio=0
          MLTRasterizer:: "Bootstrap found zero luminance"
POST-FIX: PT luma=0.288132  BDPT luma=0.291525  MLT luma=0.288304
          ratio(BDPT/PT)=1.01177  ratio(MLT/PT)=1.0006
```

**Totals: 29/0 post-fix; 20/5 pre-fix** (the 5 failures are exactly the
Part B/C/D BDPT/MLT money assertions above — an isolated A/B, `git
checkout <parent> -- src/Library/Shaders/BDPTIntegrator.cpp`, rebuild,
run, restore). Stable across repeated runs (BDPT/MLT within ~1.2% of
PT on every run; run-to-run MC noise is expected — RISE renders are not
bit-deterministic run to run, `BlockRasterizeSequence` shuffles from
`std::random_device`).

**A second scene-authoring trap found and fixed along the way, disclosed
because it shaped the final scene design (not a BDPT/DL-207 defect):**
a SINGLE-sided `clippedplane_geometry`'s winding order determines a
FIXED authored normal, unlike its DOUBLE-sided path which always
reports the ray-facing normal. The BSSRDF entry gate needs a SIGNED
cosine, so a single-sided quad whose authored winding happens to face
away from the camera reads EXACTLY 0 for the SSS material under **PT
itself** (a plain Lambertian control on the identical geometry is
unaffected, since ordinary diffuse shading uses an unsigned cosine) —
confirmed via a from-scratch scratch harness before this test file was
finalized. Parts A-C use `doublesided TRUE` to sidestep the question
entirely, matching `BSSRDFOpenSheetEntryTest.cpp`'s own convention.

## 5. Sibling audit (`docs/skills/audit-by-bug-pattern.md`)

**Bug pattern:** "a deterministic light-delivery loop iterates every
eye vertex and prices it through the material's default/aggregate
representation, without checking whether this PARTICULAR vertex is a
synthetic stand-in (a BSSRDF entry point) that the aggregate
representation was never meant to answer for."

* **`EvaluateAllStrategiesNM` (HWSS companions)** — same function,
  confirmed fixed for free (§3).
* **VCM's own light-sampling path** (`VCMIntegrator.cpp`) — has no
  directional-light sampling at all (CLAUDE.md, pre-existing,
  unrelated to this row); nothing to fix.
* **`EmissionShaderOp`-style consumers** — checked per the row's own
  instruction. `EmissionShaderOp.cpp` handles a BSDF-SAMPLED escape
  hitting an EMITTER (radiantExitance() > 0 mesh luminaires), a
  disjoint code path from this zero-exitance sweep (which only ever
  fires for `radiantExitance() == 0` lights); it does not read
  `eyeEnd.isBSSRDFEntry` or `GetBSDF()` in the pattern this row fixes,
  and a directional/ambient light cannot be "hit" by a BSDF-sampled
  escape ray in the first place (it has no geometry). Not affected.
* **PT's own zero-exitance handling (`LightSampler.cpp`'s "Step 1")**
  — refuted as a sibling: PT never builds a persistent `isBSSRDFEntry`
  vertex the way BDPT does; its BSSRDF-entry NEE
  (`PathTracingIntegrator.cpp`) constructs a fresh
  `RayIntersectionGeometric` for the sampled entry point and passes
  the SAME `BSSRDFEntryBSDF`/`RandomWalkEntryBSDF` adapters directly
  into `pLS->EvaluateDirectLighting{,NM}` — which internally runs its
  own Step 1 using the ADAPTER already, correctly. PT was never
  broken by this pattern; that is why Part A-D's PT numbers were the
  trusted reference throughout.
* **BDPT's general (s>=2) connection strategies** — refuted: these
  already price a BSSRDF entry vertex through
  `PathVertexEval::EvalBSDFAtVertex`/`EvalBSDFAtVertexNM`
  (`PathValueOps::EvalBSDFAtVertex<Tag>`), which has handled
  `isBSSRDFEntry` correctly since it was written. Only THIS ONE
  deterministic sweep, which predates/bypasses that shared helper,
  carried the raw-aggregate call.

## 6. Cost

Two stack-local `IBSDF` adapters (`BSSRDFEntryBSDF`/
`RandomWalkEntryBSDF`, each a pointer/scalar plus a couple of
comparisons) are constructed once per (zero-exitance light) x (eye
vertex) pair in the sweep, discarded immediately; no heap allocation,
no change to any hot per-sample connection-strategy path. Negligible
next to the sweep's own light-list iteration.
