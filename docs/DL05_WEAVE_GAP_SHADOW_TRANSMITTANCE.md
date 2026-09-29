# DL-05: a delta light's shadow ray sees through a thin weave's gap

Slice `debt-dl05`, branched from `master` `5f0c57f2`, 2026-09-28.
Source: docs/CLOTH_FABRIC_DESIGN.md §15 item 27 ("debt 27").
Regressions: `tests/WeaveGapShadowTransmittanceTest.cpp` (new) and
`tests/BDPTStrategyBalanceTest.cpp` topologies Q and R (new).

## 1. The defect

A `transmission thin` `weave_material` has a DELTA gap lobe: `WeaveSPF`
draws it FIRST, with probability `gap(x)`, as an undeviated pass-through
(`isDelta`, `pdf = 1`, `kray = 1` -- the lobe's coefficient and its
selection probability are the same number and cancel).  Every PT NEE
shadow ray, however, was binary: a shadow ray that met a weave was blocked,
and the only walk that could let a shadow ray through a delta surface
(`RayCaster::CastShadowRayTransmittance`, the opt-in `transparent_shadows`
path) admitted perfect-specular dielectrics only.

So the path **delta light -> straight through a weave gap -> receiver** had
no estimator in PT at all: NEE was blocked, and a BSDF-sampled ray can never
hit a delta (point / spot / directional) light.  BDPT and VCM reach it by
light tracing.  A two-layer weave lit from outside -- a closed weave box, a
pair of curtains, a hollow weave sphere -- read PT 1.26-1.89x UNDER BDPT/VCM.
A single layer or a light already inside the box has no far layer to block
the shadow ray, which is why those rows were exact.

`BDPT`'s own deterministic zero-exitance sweep (directional / ambient
lights) has the same blind spot: it calls the light's
`ComputeDirectLighting`, which casts the same binary shadow ray, and no other
BDPT strategy can reach a directional light.

## 2. Ruling

**Only a DELTA light's NEE shadow ray sees through a non-bending delta
pass-through; an area or environment light's NEE arm keeps a binary shadow
there.**

- For a delta light the shadow ray is the path's ONLY estimator (no
  BSDF-sampled strategy can hit it), so seeing through the gap adds a path
  that had no estimator and competes with nothing -- no MIS is involved,
  none is needed.
- For an area / environment light, PT ALREADY reaches the light through the
  gap: the continuation from the receiver hits the weave, draws the gap
  lobe (a delta vertex), and the emitter hit / env escape beyond it has no
  NEE partner, so it takes MIS weight 1.  That was unbiased before DL-05
  (only noisier than it needs to be).  Letting the area/env NEE arm see
  through the gap as well would count the same path twice -- measured
  **+103 %** on the closed-form area row when that arm was forced to see
  through (§5).  This is the partition the brief asked for, satisfied
  trivially: the arm that transmits has no partner, and the arm that has a
  partner does not transmit.
- **BDPT / VCM connections are NOT changed.**  BDPT already reaches the far
  layer through its delta-vertex walk (the gap vertex is a real subpath
  vertex, flagged `isDelta`, and `MISWeight` skips connections at it);
  letting `ConnectVertices` / VCM's shadow test see through the gap would
  add a SECOND representation of the same path (a connection that jumps over
  the gap vertex) with no place in either integrator's MIS.  The only BDPT
  site that moves is the zero-exitance sweep, because it calls the light's
  own `ComputeDirectLighting` and there is no other BDPT estimator for a
  directional light (the directional closed-form row: BDPT 0 before, exact
  after).
- **Suppressed while the scene carries a radiance-carrying photon map**
  (caustic / global / translucent, pel or spectral): the legacy gather ops
  already estimate light-through-a-delta-lobe transport (the gap ray is
  `eRayRefraction`, which the caustic tracer follows), so a legacy chain
  would count it twice.  No shipped weave scene carries a photon map.

What it is NOT: a dielectric.  A dielectric's transmission BENDS; its
straight shadow ray is the existing documented APPROXIMATION behind
`transparent_shadows`.  The gap's continuation is EXACTLY the incoming
direction, so the straight shadow segment is the same geometric path the
SPF's own gap draw carries, and the transmittance applied is its exact
expectation.

## 3. Mechanism

- `IMaterial::HasDeltaPassThrough()` (capability, default false) and
  `ISPF::DeltaPassThroughTransmittance{,NM}(ri)` (the EXPECTED throughput of
  the pass-through lobe for a ray along `ri.ray.Dir()`, default 0).  A
  capability of the material, not a per-sample flag (debt 23's lesson).
- `WeaveSPF`: `gap(x)`, mirroring `ScatterImpl`'s gates (`transmission
  thin`, the ray-facing `view . n > 0` early-out).  `WeaveBRDF::ResolveGap`
  is now the one copy of the gap expression (`ResolveWeave` calls it).
  `WeaveMaterial::HasDeltaPassThrough` is `thin && GapCanOpen()`:
  `GapCanOpen` is false only for a provably-zero gap (a uniform painter that
  clamps to 0) -- the `silk` / `satin` presets ship `transmission thin` with
  `gap 0`, whose SPF can never draw the lobe, and without this gate every
  delta-light shadow ray they occlude paid for a walk that found zero
  (measured +5-7 % CPU on `fabric_swatches` / `denim_and_satin_drape` for no
  image change, before the gate).
- Forwarding wrappers re-price exactly as their `Scatter` does, in
  expectation over their own branch selection:
  - `FabricSPF`: `(1 - w) * baseT * SheenTransmit(cos)^2 / max(1e-12, 1 - w)`
    (the fuzz layer crossed twice, no recycling factor -- the reasoning in
    `ScatterImpl`'s delta branch), queried on the weave-rotated record.
  - `CoatedSPF`: `(1 - pCoat) * baseT * Ain * Aout * Tin * Tout / eta^2 /
    max(1e-12, 1 - pCoat)`, per channel on RGB, `tint[0]` on NM (exactly
    `ScatterImpl`'s expressions).
  - `CompositeSPF`: first-layer pass-through x gap Beer crossing x
    second-layer pass-through, the layer order, propagation gate and
    recursion cap mirroring the walk (every other walk turns the ray).
    `CompositeMaterial::HasDeltaPassThrough` follows the SPF it presents
    (both layers for a `CompositeSPF`, the forwarded layer's otherwise).
  - The two luminaire wrappers forward `HasDeltaPassThrough` (their
    `GetSPF()` forwards the SPF verbatim).
- `RayCaster::CastShadowRayAuto( ..., bDeltaLight )`: the binary any-hit
  test first (a clear segment costs what it always did); if occluded and
  the scene has a pass-through material, `IObjectManager::
  IntersectShadowRayOpaque` (new; `IntersectShadowRay` with every object
  whose material reports the capability excluded) -- an OPAQUE occluder
  answers "blocked" with one further any-hit traversal; only a segment
  blocked by pass-through surfaces alone reaches `WalkShadowSegment`, which
  multiplies their transmittances.  `bSceneHasDeltaPassThrough` is
  recomputed on every `AttachScene` (CSG operands included).
- `WalkShadowSegment` is the old `CastShadowRayTransmittance` body with two
  switches (dielectrics / pass-through).  It also STEPS OVER an object that
  does not cast shadows -- the binary test ignores such objects, and a walk
  entered because the binary test saw a different occluder must not then
  block on one it skipped.  This is a behaviour change for the opt-in
  `transparent_shadows` walk too (it used to block on a non-shadow-caster);
  `TransparentShadowTest` 40/0 unchanged.
- Callers: `LightSampler`'s delta arm (RGB, NM -- hence HWSS per lane) and
  `PointLight` / `SpotLight` / `DirectionalLight::ComputeDirectLighting{,NM}`
  pass `bDeltaLight = true`; the mesh-luminary and env arms pass false.
- DL-70: nothing here is sign-sensitive (the gap is the same aperture from
  either side, every wrapper's attenuation is a product of two identical
  single-crossing arms, and a thin weave pushes no IOR-stack entry), so the
  ray-relative double-sided normal question does not arise.  The pass-
  through expectation is symmetric in direction, so walking from the shading
  point toward the light prices light-to-shading-point transport.

## 4. Closed forms (`tests/WeaveGapShadowTransmittanceTest.cpp`)

A 0.2 x 0.2 (`kTight`, fov 2 deg) or 2 x 2 (`kWide`, fov 10 deg) Lambertian
patch (rho 0.5) at y = 0, an 8 x 8 double-sided `weave_material { fabric
custom transmission thin gap g }` sheet at y = 2 (`custom` has zero diffuse
transmission, so the underside is lit ONLY through the gap), the light above
at y = 4.  The exact answer is `L(g) = g * L0`, L0 the no-sheet render
(checked absolutely against `rho/pi * I/d^2`: PT 0.159142 vs 0.159155).
A/B against committed state: the shadow-routing files (`RayCaster`,
`LightSampler`, the three delta lights) and the composite override reverted
to `5f0c57f2`, everything else at the fix commit -- the suite reads
**103/25** there and **128/0** at the fix.

| row | pre-fix L/L0 | post-fix L/L0 | closed form |
|---|---|---|---|
| omni, PT RGB, gap 0.3 / 0.1 | 0.00000 / 0.00000 | 0.30008 / 0.10003 | 0.3 / 0.1 |
| omni, PT spectral hwss off, 0.3 / 0.1 | 0 / 0 | 0.30100 / 0.10111 | 0.3 / 0.1 |
| omni, PT spectral hwss on, 0.3 / 0.1 | 0 / 0 | 0.30030 / 0.10005 | 0.3 / 0.1 |
| spot, PT RGB (kWide), 0.3 / 0.1 | 0 / 0 | 0.30038 / 0.10015 | 0.3 / 0.1 |
| spot, BDPT (kWide), 0.3 / 0.1 | 0.30032 / 0.10038 | 0.30024 / 0.10035 | 0.3 / 0.1 |
| spot, VCM (kWide), 0.3 / 0.1 | 0.30060 / 0.10054 | 0.29985 / 0.10009 | 0.3 / 0.1 |
| composite of two weaves, PT (g^2) | 0.00000 | 0.09004 | 0.09 |
| composite of two weaves, BDPT | 0.09025 | 0.09025 | 0.09 |
| directional, PT RGB | 0.00000 | 0.30008 | 0.3 |
| directional, PT spectral hwss on | 0.00000 | 0.30004 | 0.3 |
| directional, BDPT (zero-exitance sweep) | 0.00000 | 0.30007 | 0.3 |
| AREA, PT (partition guard) | 0.29573 | 0.30230 | 0.3 |
| AREA, BDPT (printed, not gated) | 0.29169 | 0.29324 | 0.3 |

(The composite rows' pre column is the composite override reverted alone,
the weave pass-through in place.)  The AREA rows are unchanged by
construction and noisy (PT reaches the light through the gap by BSDF
sampling only): over six runs (seed bases 1000-4000, before and after) PT
spans -1.42 .. +1.04 %, with the pre-fix run inside the post-fix spread.
BDPT spans -0.20 .. -5.35 % over nine runs (mean ~-2.2 %) before and after
alike, so it is printed rather than gated (§8).  Forcing the mesh-
luminary NEE arm to see through the gap reads PT **0.60932 (+103 %)** -- the
double count this row exists to catch.

`query` section: for weave, fabric (weave rotation 0.6), coated (clear and
tinted/absorbing), coated over fabric, a luminaire wrapper and a composite,
`DeltaPassThroughTransmittance{,NM}` equals the Monte-Carlo expectation of
the SPF's own delta rays along the incoming direction (200 000 `Scatter`
calls, views 0/40/75/140 deg, RGB and NM at 550 nm) within 5 standard
errors; dropping the fabric's `SheenTransmit^2` fails 24 of those checks.
The composite override reverted fails 9 (query 0 vs sampled 0.0067-0.029).

## 5. The design-doc table (docs/CLOTH_FABRIC_DESIGN.md §15 item 27)

`WEAVE_GAP_FILTER=table ./WeaveGapShadowTransmittanceTest 1000 4`: 24 x 24,
512 spp, `fabric custom transmission thin warp/weft_transmit 0.25`, omni at
(0,0,-3) power 6, camera (0,0,3.2) fov 34, n = 4 per integrator (the sd is
run-to-run; RISE renders are not bit-reproducible).

| scene | PT pre | BDPT/PT pre | VCM/PT pre | PT post | BDPT/PT post | VCM/PT post |
|---|---|---|---|---|---|---|
| box gap 0.3, light outside | 0.02049 +/- 0.00001 | 1.5453 | 1.5453 | 0.03187 +/- 0.00000 | 0.9933 | 0.9935 |
| six planes gap 0.3 | 0.02050 +/- 0.00000 | 1.5447 | 1.5469 | 0.03188 +/- 0.00001 | 0.9929 | 0.9954 |
| box gap 0.1 | 0.01733 +/- 0.00001 | 1.2773 | 1.2782 | 0.02214 +/- 0.00001 | 1.0007 | 0.9983 |
| six planes gap 0.1 | 0.01735 +/- 0.00000 | 1.2760 | 1.2779 | 0.02216 +/- 0.00000 | 0.9994 | 0.9997 |
| two planes gap 0.1 | 0.01538 +/- 0.00000 | 1.2626 | 1.2636 | 0.01939 +/- 0.00001 | 1.0018 | 1.0001 |
| single plane gap 0.3 | 0.03360 | 1.0000 | 0.9992 | 0.03360 | 1.0000 | 0.9993 |
| box gap 0.3, light INSIDE | 0.69373 +/- 0.00026 | 0.9971 | 0.9963 | 0.69387 +/- 0.00039 | 0.9969 | 0.9958 |
| box gap 0.0 (control) | 0.01397 | 0.9945 | 0.9962 | 0.01397 | 0.9935 | 0.9959 |
| two planes gap 0.0 (control) | 0.01142 | 0.9984 | 0.9986 | 0.01142 | 0.9985 | 0.9983 |

BDPT and VCM are unchanged to 4-5 digits (their numbers do not move); PT
rises onto them.  The 0.6-0.7 % left on the box rows is the pre-existing
closed-box residual debt 25 records: the SAME box at gap 0 (no pass-through
exists, DL-05 cannot touch it) reads 0.9945 before and 0.9935 after.

**A closed weave SPHERE is left ungated, and reported here rather than
explained away.**  Gap 0.3: PT 0.00880 -> 0.01769 (the fix), BDPT 0.01660
before and after, VCM 0.0180 +/- 0.0004 at 4096 spp (n = 4) -- VCM sides
with PT, BDPT reads 6.2 % under.  Gap 0.0 (no pass-through, identical pre
and post): PT 0.00573, BDPT and VCM 0.00598 -- PT 4 % under both.  Both are
closed-shell residuals independent of DL-05 (the "closed-shell rows share
the box control's documented residual" note in `tests/PrimitiveSelfHitTest.cpp`
and debt 25's open note), not produced or moved by it.

`BDPTStrategyBalanceTest` topologies (`--weave-gap-only`, n = 3 each side):

| topology | BDPT/PT pre | BDPT/PT post |
|---|---|---|
| E backlit single curtain, omni | 1.0000 | 1.0000 |
| F gapped single curtain, area | 0.9958 | 0.9947 |
| Q closed two-layer box gap 0.3, omni outside (new) | 1.5453 +/- 0.0004 (mean, p99, max all FAIL) | 0.9936 +/- 0.0001 |
| R same box, AREA light outside (new) | 0.9918 | 0.9918 |

Full suite 182/0 (was 170/0 before Q/R; Q fails 3 checks pre-fix).

## 6. Shipped scenes

Every shipped scene binding `weave_material` or `fabric_material`, PT (the
scene's own rasterizer; `fabric_presets` is `pixelpel`), 1/4 resolution,
256 spp (sheer_curtain) or 64 spp, `oidn` off, n = 4 per binary with two
separately built binaries (base `5f0c57f2` vs the fix) interleaved ABBA;
mean luminance +/- sd, post/pre and Welch t.

| scene | thin weave with gap > 0? | whole post/pre | notable regions | user CPU post/pre |
|---|---|---|---|---|
| sheer_curtain | yes (linen, `sheer 0.22`) | +0.103 % (t = 54) | floor row +0.46 % (t = 113 / 90) | +6.0 % (94.1 +/- 1.2 -> 99.8 +/- 1.0 s) |
| weave_presets | yes (the `linen` sphere) | +1.34 % (t = 190) | linen sphere column +2.5 %, its lower region +19.7 % | +4.4 % |
| fabric_swatches | no (silk/satin gap 0, denim none) | -0.002 % (t = -0.5) | none | -1.8 % |
| denim_and_satin_drape | no | -0.004 % | none | -0.7 % |
| wool_throw_fuzz | no weave | +0.003 % | none | -0.9 % |
| velvet_cushion | no weave | -0.003 % | none | -0.4 % |
| fabric_presets | no | +0.003 % | none | +2.1 % (1 s renders) |
| wool_fuzz_halo_A_bare | no weave | 0.000 % | none | +1.0 % |
| wool_fuzz_halo_B_fuzz | no weave | +0.007 % | none | -6.1 % (1.5 s renders) |

(The post binary for all but velvet_cushion was the fix before the
composite override, which touches no scene here -- none binds
`composite_material`.)

**Cost.**  Zero where no pass-through material can open (the scene flag
short-circuits, and a no-weave scene takes the identical code path).  Where
one can, the extra work is: for every OCCLUDED delta-light shadow ray, one
opaque-only any-hit traversal; for one blocked only by pass-through surfaces,
the closest-hit walk (one `ResolveGap` painter read per pass-through hit) and
then the BSDF evaluation of a contribution that used to be discarded.  That
last part is the energy the fix recovers, not overhead: on the design-doc box
(every front-face NEE passes through the back layer) PT user CPU rises
**+26.8 %** (5.19 +/- 0.25 -> 6.59 +/- 0.13 s, n = 4 ABBA) for **+55.5 %** image
(the missing third of the light); on the two shipped scenes it fires in,
**+6.0 %** and **+4.4 %**.

## 7. Sibling audit

| site | verdict |
|---|---|
| `FabricMaterial` over a thin weave (forwards transmission since round 8, debt 22) | FIXED here: forwards the capability, `FabricSPF` re-prices |
| `CoatedMaterial` over a thin weave / fabric (DL-23) | FIXED here: same |
| `CompositeMaterial` with weave layers | FIXED here: PT read 0 through a composite of two gapped weaves, BDPT g^2 L0; now both g^2 L0 |
| Luminaire wrappers over a weave | FIXED here: forward the capability |
| `TranslucentMaterial` | no delta lobe at all (clipped cosine / Phong lobes) -- nothing to forward |
| thin dielectric sheets | the existing opt-in `transparent_shadows` Fresnel walk (a BENDING lobe, approximated); unchanged except it now steps over non-shadow-casters |
| `PerfectRefractor` at matched index | non-bending in the limit but an IOR-stack interface, left to the dielectric walk |
| BDPT / VCM connections | unchanged by ruling (§2) |
| BDPT zero-exitance sweep | fixed for free (it calls `ComputeDirectLighting`) |
| PT with `sms_enabled`: a continuation that draws the gap and then hits an AREA light | **still broken -- DL-295**: PART 3 sets `nextConsiderEmission = false` after ANY delta scatter when SMS is on, on the premise that SMS covers the specular chain, but SMS does not treat a weave as a caster: the closed-form area row reads **0.000000** with `sms_enabled TRUE` vs 0.009312 (= 0.3 x L0) without |
| legacy photon-map chains | pass-through suppressed while a radiance photon map exists (§2) |

## 8. Residuals

- **DL-294** (new): BDPT's and VCM's light-tracing (t = 1) splat is off at a
  very narrow pinhole field of view -- on this file's `kTight` framing (fov 2
  deg, 16 px), where the through-gap light reaches the camera only by t = 1,
  BDPT reads the closed form -6.05 % (gap 0.3) / -5.87 % (0.1) and VCM's own
  no-sheet L0 reads -1.45 %, identically before and after DL-05, independent
  of the pixel sampler and of spp (-6.14 % at 16384 spp), and exact from fov
  5 deg up (fov 1/2/3/5/10: -6.2/-6.1/-2.5/+0.03/+0.1 %).  **Root-caused
  and fixed by the `debt-dl294` slice (2026-09-28)**: not an angular
  effect -- the camera's world-to-raster inverse clipped the splat at its
  nominal [0, W) film while the rasterizers sample (and the splat film
  rounds in) [-0.5, W - 0.5), so half-pixel strips at image column 0 and
  row 0 were lost, which only a frame lit edge to edge (this fixture at
  fov <= 3) shows.  See [DL294_NARROW_FOV_SPLAT.md](DL294_NARROW_FOV_SPLAT.md);
  the test's `fovsweep` section now gates it.
- **DL-295** (new): the SMS emission suppression after a weave gap (§7).
- **OVERCLAIM correction (external review P2-1), filed as DL-329 at merge:
  "PT HWSS 0.30030" and every "PT now agrees with BDPT/VCM" statement in
  this doc hold for RGB and non-HWSS spectral only.**  Under
  `hwss TRUE`, PT drops 3 of 4 wavelength lanes on any path where a
  BSDF-SAMPLED CONTINUATION (not a shadow ray) crosses a weave gap --
  `WeaveSPF` (and the Fabric/Coated/Composite wrappers) have no
  `EvaluateKrayNM`, so the PT HWSS companion loop prices the delta gap ray
  with the continuum BSDF instead of the delta pass-through, pricing
  `pdf = 1` (the delta marker) as if it were an ordinary density.  This
  does NOT affect §4's closed-form omni/spot/directional rows: those cross
  the gap only via `LightSampler`'s delta-arm SHADOW ray (`WalkShadowSegment`
  calling `DeltaPassThroughTransmittanceNM` per companion wavelength
  directly), which is unaffected and reads the correct 0.30030 at `hwss
  TRUE`.  It DOES affect any scenario reached only by a
  Scatter()-sampled continuation -- the AREA partition guard (§4's own row,
  RGB-only in this suite) and the design-doc two-layer topologies (§5,
  also RGB-only) have no HWSS row and so never exercised it.  Measured by
  the reviewer (pre approx post, the defect is pre-existing and DL-05 does
  not move it): area closed form PT-HWSS 0.0775*L0 vs the RGB/non-HWSS
  0.3*L0; topology R PT-HWSS 0.02575 vs its own pel 0.07956; topology Q
  post-fix PT-HWSS 0.01999 vs pel 0.03187.  A one-line mutation in the HWSS
  companion loop (`if(compWeight<0 && pS->isDelta) compWeight =
  pS->krayNM;`) recovers R to 0.07928 and Q to 0.03179, confirming the
  mechanism (not landed here -- new `ISPF::EvaluateKrayNM` overrides on
  `WeaveSPF`/`FabricSPF`/`CoatedSPF`/`CompositeSPF` are a DL-125-class fix,
  out of this slice's scope).
- **Two pre-existing, unattributed BDPT deficits through a delta
  pass-through, identically before and after DL-05, filed as DL-330 at
  merge** (the slice's own two ids, DL-294/DL-295, were spent on other
  findings; flagged to the supervisor rather than left as an unfiled
  observation): (1) on the AREA closed-form row (kWide, fov 10 deg, where
  the spot rows' t = 1 splat is exact) BDPT reads -0.20 .. -5.35 % over
  nine runs, mean ~-2.2 % (t ~ 4 against 0) -- the path there is s = 0 (eye
  path through the gap to the emitter) against t = 1, with the gap a delta
  vertex both walks skip; topology F (a gapped single curtain in front of
  a full-width emitter) reads BDPT/PT 0.995, so it is specific to this
  fixture's geometry, not a property of every delta-pass-through scene.
  The external review additionally measured BDPT/PT 0.979 on this same
  area closed form pooled over n = 10 (t ~ 4), and on the closed weave
  SPHERE at gap 0.3: BDPT 0.016599 vs PT 0.017678 vs VCM@2048spp
  0.017390 +/- 0.00012 se -- BDPT -4.5 % vs VCM (t ~ 6.5) and -6.1 % vs PT,
  not a depth-cap artifact (16/16 reads 0.016612, matching the suite's own
  8/8).  (2) BDPT-HWSS on the area closed form reads 0.2651 (-11.9 % vs
  the RGB/non-HWSS closed form, n = 10, t ~ 29), while BDPT pel on the same
  row reads -1.6 % and topology R's BDPT-HWSS is fine -- likely the BDPT
  analogue of DL-329's `RecomputeSubpathThroughputNM` companion-pricing
  mechanism, not root-caused here.
- Depth / bounce caps: a BSDF-sampled path crossing a gap still counts it as
  a transmission bounce and a depth step, where the NEE shadow walk counts
  nothing -- irrelevant to delta lights (no BSDF partner) and to area/env
  lights (their NEE arm does not transmit), so it moves no partition.
- The CSG case where a composite object with NO material of its own wraps a
  weave operand keeps the binary shadow (its `GetMaterial()` reports no
  capability) -- the pre-DL-05 under-read, never a double count.
- **Photon-map suppression (external review P3) is SCENE-WIDE, keyed on
  whether the scene carries a radiance photon map at all, not on whether
  the CHAIN doing the shading actually gathers it.**  A `pixelpel_rasterizer`
  chain with a caustic/global map and its own caustic-gather op is correctly
  covered once (0.0480601 -> 0.0480599, unchanged); a chain that declares a
  map but has no gather op in its own shader (or a `pathtracing_pel_rasterizer`,
  which never builds the legacy maps at all: 0.0489283 vs 0.0489309 with/
  without a declared map, i.e. no suppression fires there either way) keeps
  the PRE-DL-05 under-read regardless of §2's suppression rule.  Not a
  double count in either direction; §2's "legacy gathers already carry the
  path" is accurate only for a chain whose shader actually has the gather.
- **Legacy direct-only chain, area light behind a gap: still reads 0, before
  and after DL-05.**  The ruling's reason for keeping the area/env NEE arm
  binary (§2: "PT's continuation already reaches it through the gap at MIS
  weight 1") does not exist in a direct-only chain (no BSDF-sampled
  continuation at all), so per DL-171's "a strategy's MIS partner is the
  sibling that actually exists in the chain" rule this arm COULD transmit
  there without double-counting -- an under-read, not fixed in this slice.
- **`kMaxCrossings` (32, `WalkShadowSegment`) now also counts pass-throughs
  and non-caster step-overs**, where before DL-05 it counted only dielectric
  interface crossings.  A very deep stack of thin-weave layers (or many
  `casts_shadows FALSE` objects) on one shadow segment can exhaust the
  budget sooner than before and fall back to the conservative "blocked"
  answer; no shipped scene approaches this depth.
- **Clay-override / material-preview mode**: a delta light's shadow ray
  still consults the REAL material's `HasDeltaPassThrough`/
  `DeltaPassThroughTransmittance{,NM}` rather than the clay override, so a
  preview render can show light passing through a weave gap that the
  clay-shaded preview material itself would not have. Preview-only
  inconsistency, not a rendered-output defect.
