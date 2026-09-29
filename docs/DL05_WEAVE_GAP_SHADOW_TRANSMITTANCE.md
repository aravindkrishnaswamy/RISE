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
| PT with `sms_enabled`: a continuation that draws the gap and then hits an AREA light | **FIXED by DL-295 (section 9)**; as found here: PART 3 sets `nextConsiderEmission = false` after ANY delta scatter when SMS is on, on the premise that SMS covers the specular chain, but SMS does not treat a weave as a caster: the closed-form area row reads **0.000000** with `sms_enabled TRUE` vs 0.009312 (= 0.3 x L0) without |
| legacy photon-map chains | pass-through suppressed while a radiance photon map exists (§2) |

## 8. Residuals

- **DL-294** (new): BDPT's and VCM's light-tracing (t = 1) splat is off at a
  very narrow pinhole field of view -- on this file's `kTight` framing (fov 2
  deg, 16 px), where the through-gap light reaches the camera only by t = 1,
  BDPT reads the closed form -6.05 % (gap 0.3) / -5.87 % (0.1) and VCM's own
  no-sheet L0 reads -1.45 %, identically before and after DL-05, independent
  of the pixel sampler and of spp (-6.14 % at 16384 spp), and exact from fov
  5 deg up (fov 1/2/3/5/10: -6.2/-6.1/-2.5/+0.03/+0.1 %).  Not root-caused.
  `WEAVE_GAP_FILTER=dl294` prints it.
- **DL-295** (new): the SMS emission suppression after a weave gap (§7);
  fixed in section 9.
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

## 9. DL-295: PT with `sms_enabled` dropped emission reached through a gap

Slice `debt-dl295`, branched from `master` `56303797`, 2026-09-28.
Regression: `tests/WeaveGapShadowTransmittanceTest.cpp` section `sms`
(new), plus opt-in `dl295audit` and `scenehash` measurement sections.

### 9.1 The defect

`PathTracingIntegrator` has two SMS double-count guards, both premised on
"SMS already estimates the specular chain this emitter hit ends":

- PART 3 (the BSDF branch, RGB/NM and HWSS) set `considerEmission =
  false` for the next vertex after ANY delta scatter;
- PART 1 (RGB/NM) suppressed an emitter hit when the previous scatter was
  delta (`bPassedThroughSpecular`) and an earlier one was not
  (`bHadNonSpecularShading`).

SMS (`ManifoldSolver`) builds and validates its chains from
`IMaterial::GetSpecularInfo( ri, ior ).isSpecular` and nothing else: its
seed trace stops at the first hit that reports false, and its
chain-visibility test (`SegmentOccludedByNonChainSpeculars`) treats such a
hit as an opaque blocker.  A weave reports false (no override), and so do
every DL-05 wrapper and `composite_material`.  So a chain through a weave
gap has NO SMS estimate, and PT's own continuation -- the only estimator
left -- was thrown away: the area closed form read exactly 0 with
`sms_enabled TRUE`.  It is the DL-05 pattern again (a non-bending
pass-through treated as a generic delta vertex), but the rule it breaks is
wider than pass-throughs: `CompositeSPF`'s delta-TAGGED walker exits
(DL-24), which leave in refracted directions, were dropped the same way.

### 9.2 The fix

SMS estimates an emitter hit that ends a specular chain only when (1) an
SMS ANCHOR precedes the chain -- **a non-delta vertex at which SMS is
EVALUATED, i.e. one that reaches PART 2 with a BSDF; BSDF-less (SPF-only)
surfaces, BSSRDF exits and medium vertices are NOT anchors** -- because
SMS seeds from that vertex toward the light through the casters; and (2)
every delta vertex in the chain is one SMS represents.  The guards now
fire only when both hold:

- **(2), the round-1 fix.**  `PTNextSMSChainUncovered` tracks, per
  specular chain (reset at every non-delta vertex), whether the chain
  crossed a delta vertex whose material SMS does not treat as a caster --
  the same `GetSpecularInfo().isSpecular` query SMS itself makes, asked
  only at a delta vertex with SMS on.  While that is true neither guard
  fires: PART 3 keeps `considerEmission`, PART 1's predicate gains `&&
  !bSMSChainUncovered`.  The rule is "suppress only a chain SMS can
  represent", so it is not keyed on `HasDeltaPassThrough()` or on the
  continuation being straight: a straight test would miss the composite
  walker's refracted exits, and every pass-through material fails the
  caster test anyway.
- **(1), found by the external review (P2-2), fixed in round 2.**  PART 3
  set `considerEmission = false` after a caster's delta lobe even with NO
  anchor before it, so camera -> smooth `randomwalk_sss` / `polished`
  (scattering 1e6) coat -> area emitter read 0 under PT+SMS (pel and
  HWSS) with nothing else estimating it -- SMS never seeds a caster's own
  reflection, and NEE cannot sample a delta lobe.  PART 3 now also
  requires the anchor (`bHadNonSpecularShading` in the Pel/NM body, a new
  `bSMSAnchor` in the HWSS body) -- PART 1's own condition; the SPF-only
  branch already relied on PART 1 alone for this reason.  This is a
  pre-existing sibling of DL-295 (same guard, same wrong premise), not a
  consequence of it.
- **(1) again, found by the external review's round 2 (P1-1), fixed in
  round 3.**  The anchor flag itself was set at vertices where SMS never
  runs.  The SPF-only branch (a material with no BSDF:
  `biospec_skin_material`, `generic_human_tissue_material`, a composite
  whose `GetBSDF()` is null) set `bHadNonSpecularShading = true` on a
  non-delta scatter and `continue`d BEFORE PART 2 -- no NEE, no SMS -- so
  the next caster chain passed both guards and nothing estimated it: a
  skin receiver under a closed ior-1.5 slab read PT+SMS 0 against 0.267
  (pel) / 0.244 (spectral) without SMS, before DL-295 and after both
  earlier rounds.  That arm now CLEARS the anchor, and so do both BSSRDF
  exit continuations (`rs2.smsHadNonSpecularShading`, which passed `true`
  into a `CastRay` whose first vertex is the exit point -- also never
  SMS-evaluated; no measurable effect on the review's fixture, 0.9974 ->
  1.0025, but the same wrong claim).  It cannot double-count: SMS never
  runs at either kind of vertex.  The medium vertex is the one remaining
  inhabitant of the same wrong claim (DL-340).
- **The HWSS hand-offs (review P1-1, round 2).**  The HWSS body hands a
  BSDF-less vertex (a glass slab) and a subsurface material to the
  per-wavelength `IntegrateFromHitNM`.  Round 1 forwarded only
  `smsHadNonSpecularShading = !bSMSChainUncovered` on the no-BSDF
  hand-off, which switched off PART 1's latch but not PART 3, and left the
  SSS hand-off at a constant `true`: the delegated body restarted
  `bSMSChainUncovered` at false, so the first BSDF-carrying caster past
  the hand-off re-suppressed the chain (camera -> gap -> smooth SSS ->
  emitter read 0 under HWSS).  Both hand-offs now pass the chain's own
  state: `smsHadNonSpecularShading = bSMSAnchor` (it was a constant `true`
  on the claim that a BSDF vertex always precedes the hand-off -- a camera
  -> polished coat -> glass chain has none) and a new trailing
  `IntegrateFromHitNM( ..., smsChainUncovered_ )` that initialises the
  delegated body's `bSMSChainUncovered`.  The HWSS medium-walk hand-off
  still passes a constant `true` (DL-340).

Deliberately unchanged:

- `bsdfMisPdf` stays 0 at a delta vertex, so an emitter hit behind a gap
  takes MIS weight 1 -- correct, because the area/env NEE arm keeps its
  binary shadow at a pass-through (section 2).
- `bPassedThroughSpecular` itself.  It also drives the GUI `indirect`
  mode's depth==1 MIS-partner test, for which a gap IS a delta event (NEE
  at the gap vertex cannot sample through it); the new state is a separate
  variable.
- The env-escape branch: it never read `considerEmission` or the latch,
  so the env rows were correct before (measured, below).

### 9.3 Red -> green (`WEAVE_GAP_FILTER=sms`, isolated A/B)

Black-yarn gap-0.3 sheet (`warp/weft_color` 0, `warp/weft_ior` 1: the
sheet transmits exactly its gap and reflects nothing, so every closed form
is exact), 4 x 4 area emitter at y = 4 (the DL-05 0.5 x 0.5 emitter leaves
a ~5 % per-render sd through the gap at 1024 spp), every render
Sobol'-salted per (seed base, render index).

**Round 3 (current).**  The suite now renders on ONE worker (section 9.3's
band note), so every figure below is exactly reproducible from its seed
base.  All three builds are the branch merged with `master` `abfd09269`
and the final tests; they differ in `PathTracingIntegrator.{cpp,h}` only:
**master** (`abfd09269`), **round 2** (`b5b5d4c9d`), **post**.  The
section reads **5/22, 25/2 and 27/0** at seed bases 1000, 2000 and 3000;
the whole file **137/22, 157/2, 159/0**.  Round 2 fails exactly the two
skin rows; everything else in the table below keeps its round-2 reading.

| row | expected | master | round 2 | post |
|---|---|---|---|---|
| skin receiver under a closed slab, PT RGB, SMS on / off (review r2 P1-1) | 1 | 0 | 0 | 0.99486 +/- 0.01288 |
| same, PT spectral hwss off | 1 | 0 | 0 | 1.00177 +/- 0.03353 |

**Rounds 1-2 (the table below).**  Merged with `master` `4c286bd5`;
**master** (`4c286bd5`), **round 1** (`b15dc49a`), **post** (round 2);
n = 3 seed bases (1000-3000) per build, MULTITHREADED (see the band
note), mean +/- sd; the section read **5/20, 17/8 and 25/0** at every seed.

| row | expected | master | round 1 | post |
|---|---|---|---|---|
| area, PT RGB | 0.3 | 0 | 0.29962 +/- 0.00396 | 0.29953 +/- 0.00402 |
| area, PT spectral hwss off | 0.3 | 0 | 0.30064 +/- 0.00179 | 0.29957 +/- 0.00457 |
| camera -> gap -> emitter (look-up), PT RGB | 0.3 | 0 | 0.29991 +/- 0.00016 | 0.29991 +/- 0.00016 |
| area, composite of two weaves, PT RGB | 0.09 | 0 | 0.09194 +/- 0.00282 | 0.09233 +/- 0.00791 |
| look-up, composite of two weaves, PT RGB | 0.09 | 0 | 0.09072 +/- 0.00108 | 0.09139 +/- 0.00073 |
| env box (uniform env, closed black-weave box), PT RGB | 0.3 | 0.30008 | 0.30008 | 0.30008 +/- 0.00012 |
| area / look-up / env box, PT HWSS, SMS on / off | 1 | 0 / 0 / 0.996 | 1.009 / 1.003 / 0.991 | 1.001 / 1.002 / 0.996 |
| area / look-up composite(dielectric over weave), on / off | 1 | 0 / 0 | 1.024 / 0.997 | 1.025 / 1.007 |
| look-up fabric / coated over weave, on / off | 1 | 0 / 0 | 0.999 / 1.000 | 0.999 / 1.000 |
| direct smooth-SSS / polished reflection, PT RGB, on / off (review P2-2) | 1 | 0.00001 / 0 | 0.00001 / 0 | 1.000 / 1.000 |
| same, PT HWSS | 1 | 0.00001 / 0.00002 | 0.00001 / 0.00002 | 1.000 / 1.000 |
| gap -> smooth SSS (SSS hand-off, review S1), HWSS, on / off | 1 | 0.00001 | 0.00001 | 1.00048 +/- 0.00728 |
| gap -> slab -> smooth SSS (no-BSDF hand-off, S4), HWSS | 1 | 0.00008 | 0.00007 | 1.00252 +/- 0.00780 |
| receiver -> gap -> SSS ceiling, PT RGB | 1 | 0.00037 | 0.99672 +/- 0.02473 | 0.99704 +/- 0.02425 |
| receiver -> gap -> SSS ceiling, HWSS (SSS hand-off, review P1-1) | 1 | 0.00026 | 0.00028 | 1.00959 +/- 0.00294 |
| receiver -> gap -> slab -> SSS ceiling, HWSS (no-BSDF hand-off, P1-1) | 1 | 0.00041 | 0.00028 | 1.00016 +/- 0.01295 |
| receiver -> SSS / polished ceiling, NO gap (must stay suppressed), on / off | < 0.05 | 0.0003 / 0 | 0.0002 / 0 | 0.0003 / 0 |
| open refractor plane ior 1.0 / 1.5, on / off (printed) | -- | 0.999 / 2.249 | 0.999 / 2.249 | 0.999 / 2.248 |

- The HWSS rows are parity, not closed forms: PT-HWSS's companion lanes
  still price a gap continuation with the continuum BSDF (DL-329), which
  moves SMS on and off identically.
- The two anchored-ceiling HWSS rows are the ones that isolate P1-1: the
  anchor exists, so the P2-2 anchor gate cannot rescue them -- only the
  forwarded chain state does (round 1 reads 0 there, like master).
- **The "must stay suppressed" rows read ~0, not ~1.**  receiver ->
  caster -> emitter with no gap is an SMS chain by PT's accounting (an
  anchor, then a caster), so PT correctly leaves it to SMS -- but SMS
  never estimates it: it treats smooth SSS, polished coats and clear
  dielectrics as refractors (`canRefract`) and seeds refraction chains,
  so their REFLECTION reaches nobody (a clear dielectric ceiling reads
  0 vs 0.0314 the same way, `WEAVE_GAP_FILTER=dl295probe`).  That is the
  documented "reflection caustics through dielectrics are out of scope
  for SMS" convention, identical in all three builds; the rows pin it so
  a change to it is deliberate, and DL-339 records it.
- The whole file reads **137/20 (master), 149/8 (round 1), 157/0
  (post)** -- the 132 non-`sms` checks are DL-05's own and pass in all
  three.

**Bands (round 3).**  A multithreaded render is NOT reproducible: each
render worker seeds its RNG from libc `rand()` in thread-start order
(`RasterizeDispatchers.h`) and tiles go to workers nondeterministically,
so the same seed base AND Sobol' salt read differently run to run (a
look-up-composite row read -2.53 % and -1.12 % on two runs of one seed,
-2.553 % twice single-threaded).  Round 1 set bands from salted seeds of
multithreaded runs (">= 3.3 sd", wrong on two rows, review P1-2); round 2
from an n = 8 multithreaded spread (two bands still did not reproduce,
review round 2 P2-1: composite look-up 4.46 % against a claimed 1.9 %,
anchored-ceiling SSS hand-off 1.96 % against a claimed <= 1.38 %).
Round 3 makes the suite deterministic instead: `main()` writes an options
file with `force_number_of_threads 1` unless the caller supplies one.
Every fixture is at most 24 x 24 -- ONE 32-pixel tile -- so one worker
costs a few percent, and single-threaded rendering draws from
`GlobalRNG()`, identical run to run (checked: two runs of the section
diff empty).  A seed sweep is then the whole run-to-run spread, and every
band is >= 3.3 sd of a 12-seed sweep (seed bases 1000-12000, all 12 at
27/0).  Relative sd (band / sd): area RGB 0.83 % (3.6), spectral 0.69 %
(5.8), look-up 0.08 %, composite area 5.35 % (3.7) / look-up 2.14 % (15 %
band: 7.0 here and 4.7 against the review's multithreaded pooled 3.16 %),
env 0.05 %; HWSS parity area 1.00 % (5.0) / look-up 0.73 % (5.5) / env
0.90 % (4.4); composite(dielectric) 2.29 % (3.5) / 0.93 % (4.3); fabric,
coated, direct-view <= 0.17 %; S1 / S4 0.44 / 0.53 %; anchored ceiling
RGB 1.26 % (4.8), HWSS SSS hand-off 2.29 % (9 % band: 3.9), no-BSDF
hand-off 2.11 % (4.3); skin under a slab RGB 1.18 % (4.2), spectral
1.85 % (4.3).

### 9.4 Double-count audit (`WEAVE_GAP_FILTER=dl295audit`)

A caustic whose chain STARTS behind a weave: sms_k2_glasssphere's caster
(perfect refractor, ior 1.5, r 0.3 at y 0.6) over a Lambertian floor, a
1 x 1 emitter at y 1.8, a black-yarn gap-0.3 sheet at y 1.2 -- light ->
gap -> glass -> glass -> floor.  PT+SMS, PT without SMS, VCM, 48 x 36,
1024 spp each, n = 4 salted replicates, isolated pre/post builds; whole
image (ROI under the sphere in parentheses):

| sheet | build | PT+SMS | PT | VCM | PT+SMS / PT |
|---|---|---|---|---|---|
| none (control) | pre | 0.13357 (0.28893) | 0.13937 (0.31543) | 0.13933 (0.31574) | 0.9584 (0.9160) |
| none (control) | post | 0.13364 (0.28932) | 0.13944 (0.31530) | 0.13936 (0.31575) | 0.9583 (0.9176) |
| over the whole emitter | pre | 0.00000 (0.00000) | 0.01258 (0.02831) | 0.01248 (0.02807) | 0 |
| over the whole emitter | post | 0.01242 (0.02817) | 0.01261 (0.02855) | 0.01250 (0.02832) | 0.9855 (0.9868) |
| over the x<0 half | pre | 0.05831 (0.09419) | 0.06862 (0.12619) | 0.06868 (0.12750) | 0.8497 (0.7464) |
| over the x<0 half | post | 0.06757 (0.12176) | 0.06855 (0.12544) | 0.06863 (0.12696) | 0.9857 (0.9706) |

Re-run on the branch merged with `master` `4c286bd5` after round 2 (the
anchor rule and the hand-offs; pre = `PathTracingIntegrator.{cpp,h}` at
`4c286bd5`), same protocol, n = 4:

| sheet | build | PT+SMS | PT | VCM | PT+SMS / PT |
|---|---|---|---|---|---|
| none (control) | pre | 0.13353 (0.28881) | 0.13949 (0.31636) | 0.13933 (0.31563) | 0.9573 (0.9129) |
| none (control) | post | 0.13357 (0.28894) | 0.13949 (0.31647) | 0.13936 (0.31577) | 0.9576 (0.9130) |
| over the whole emitter | pre | 0 (0) | 0.01258 (0.02864) | 0.01248 (0.02827) | 0 |
| over the whole emitter | post | 0.01244 (0.02816) | 0.01256 (0.02849) | 0.01252 (0.02816) | 0.9907 (0.9884) |
| over the x<0 half | pre | 0.05829 (0.09394) | 0.06861 (0.12608) | 0.06865 (0.12690) | 0.8495 (0.7451) |
| over the x<0 half | post | 0.06750 (0.12130) | 0.06868 (0.12655) | 0.06862 (0.12696) | 0.9828 (0.9585) |

The un-suppressed path and SMS's paths are DISJOINT, from the code and in
the measurement: SMS's seed trace from the floor stops at the weave (not
specular), and its visibility test for a chain found by the uniform-caster
seeding is blocked by the weave, so SMS contributes nothing through a
sheet, and PT's through-the-gap emitter hit is the only estimate.  With
the sheet over the whole emitter PT+SMS reads PT and VCM within noise
(sd 0.00028 per replicate); a double count would read ABOVE PT, and the
half-covered row, where SMS still runs on the uncovered half, sits between
the control's own SMS/PT ratio (0.958, the pre-existing biased-SMS offset
on this caster, identical pre and post) and 1.  The control rows are
unchanged by the fix.

### 9.5 Shipped SMS scenes are bit-identical

Every shipped scene whose ACTIVE rasterizer sets `sms_enabled TRUE` -- 21
of them: the 17 in `scenes/Tests/SMS/` (with `sms_slab_close_pt_sms_hispp`),
`triplecaustic_pt_sms`, `diacaustic_pt_sms`, and the two spectral ones,
`scenes/Tests/Spectral/sms_through_glass_emitter_pt_sms` and
`spectral_dispersive_caustic_pt_sms` (the only legacy shader-op / spectral
SMS scenes) -- contains
only Lambertian receivers, luminaires, dielectrics and perfect
reflectors/refractors -- no BSDF-bearing material with a delta lobe and no
non-caster delta vertex -- so the new state never becomes true.
`WEAVE_GAP_FILTER=scenehash` renders them in-process with `std::srand`
fixed, the Sobol' salt 0, one render thread (`force_number_of_threads 1`;
the per-thread RNGs are seeded from libc `rand()` in thread start order,
and the CLI `rise` itself seeds `srand` from the clock, so neither
multithreaded nor CLI renders are reproducible) and `samples` capped at 8;
rounds 1 and 2 hashed a 19-scene list that was WRONG (external review
round 2, P3): it held `pool_caustics_vcm`, whose SMS rasterizer chunk is
commented out, and missed the three scenes above whose `sms_enabled` is
tab-separated.  The review hashed those three identical pre/post at round
2 (md5 `2c7002ec...`), so the conclusion held.  The 19 hashed were
identical between the pre and post
builds in each of three interleaved rounds (all six hash listings have
the same md5, `646deb65...`, so each build is also reproducible run to
run under this protocol).  Repeated after round 2 on the branch merged with `master`
`4c286bd5` (pre = `PathTracingIntegrator.{cpp,h}` at `4c286bd5`): all 19
identical again in three interleaved rounds, all six listings md5
`9d0676fe...` -- a different set from before, because DL-290 changed
SMS itself.  Round 2's anchor rule cannot reach these scenes either: its
only new condition sits at a PART 3 delta vertex, and none of them has a
BSDF-carrying material with a delta lobe.  Round 3 (the anchor-flag
correction at BSDF-less surfaces and BSSRDF exits), merged with `master`
`abfd09269`: the CORRECT 21-scene list (now `scenehash`'s built-in
default) hashes identical pre/post in two interleaved rounds (all four listings md5 `25338d99...`; the three previously missed scenes included: `sms_slab_close_pt_sms_hispp` mean 0.322728, `sms_through_glass_emitter_pt_sms` 2.884176, `spectral_dispersive_caustic_pt_sms` 1.626959).

### 9.6 Sibling audit

| site | verdict |
|---|---|
| weave gap, and fabric / coated / luminaire wrappers over a weave | FIXED (area, look-up, fabric and coated parity rows) |
| `composite_material` of weaves (straight gap -> gap walker exit) | FIXED (g^2 rows) |
| `CompositeSPF` walker exits that REFRACT (dielectric over weave) | FIXED -- same rule; a pass-through-only exemption would have missed these (parity rows, 0 pre) |
| BSDF-less materials (SPF-only branch) as DELTA vertices | same rule applied; every shipped one (dielectric, perfect reflector/refractor) is a caster, so unchanged |
| BSDF-less materials as NON-delta vertices (`biospec_skin_material`, `generic_human_tissue_material`, a composite with a null `GetBSDF()`) | **MISSED in rounds 1 and 2, found by the external review's round 2 (P1-1), FIXED in round 3** (section 9.2): they were treated as SMS anchors although SMS never runs there -- skin under a closed slab 0 vs 0.267 (pel) / 0.244 (spectral) under PT+SMS, pre-existing.  Same claim at the BSSRDF exit continuations, fixed alongside |
| rough `subsurfacescattering_material`'s delta TIR / back-face lobes | covered by the rule (reports `isSpecular` false above roughness 1e-3); not measured |
| hair | REFUTED: `HairBSDF` emits only `isDelta = false` rays |
| `transparency_shaderop` / alpha | REFUTED for PT: the integrator never dispatches the shader-op chain (DL-214); the legacy `SMSShaderOp` has no emission suppression at all |
| a BSDF-carrying caster's OWN delta reflection with no SMS anchor (camera -> smooth `randomwalk_sss` / `polished` coat -> area emitter) | **MISSED in round 1, found by the external review (P2-2), FIXED in round 2** (section 9.2): 0.000003 / 0.000000 vs 0.3124 under PT+SMS, pel and HWSS, pre-existing -- 100 % of an area light's reflection in a smooth SSS or polished surface seen directly.  Round 1's "rough SSS ... covered" row never looked at the caster case |
| the HWSS -> NM hand-offs | **half-fixed in round 1** (P1-1): the chain state reached PART 1 only, and the SSS hand-off not at all; round 2 forwards the anchor and the uncovered state through both (section 9.2) |
| anchored reflection off a refractive caster (receiver -> smooth SSS / polished / clear dielectric -> emitter, no gap) | NOT this defect: PT leaves it to SMS, correctly by the anchor rule, and SMS never estimates a refractive caster's REFLECTION -- 0 vs 0.019-0.031 without SMS, identical in every build.  The documented "reflection caustics are out of scope for SMS" convention; recorded as **DL-339** (rewritten at the `4c286bd5` merge) |
| perfect refractor, open plane (an SMS caster) | NOT this defect.  Round 1 measured PT+SMS 0 vs 0.152 at ior 1.0 and 2.26x at ior 1.5 on an open plane, and a closed ior-1.0 sphere at 0.913 / 0.775 of PT; `master`'s DL-290 (SMS's matched-index seed walk) closes every ior-1.0 figure (open plane 0.999, closed thin box 0.1522 / 0.1527, closed sphere 1.000 / 1.000, measured by the external review on the merged tree), and the ior-1.5 2.25x remains ONLY on an open sheet (a closed 0.05-thick box reads SMS 0.14156 / PT 0.14202 / VCM 0.14237) -- the open-sheet convention, DL-345's family; now part of DL-339 |
| a MEDIUM vertex after a caster | NOT a delta vertex, but the same wrong anchor claim: a medium vertex is never SMS-evaluated, yet the latch survives it.  Round 1 called the measurement inconclusive; the external review's round 2 measured it CONCLUSIVELY on a blocked-emitter fixture: PT+SMS/PT 0.779 (0.800 before DL-295) with a slab against 0.975 / 0.978 without -- a ~20 % pre-existing loss.  The in-tree fixture (`FogSlabScene`, a 2 x 2 up-facing emitter over a black blocker, sigma_s 0.2; `dl295probe` rows `DL-340`) reads 0.942 +/- 0.011 against 1.011 +/- 0.010 (n = 4, 2048 spp): the size of the loss is the phase-sampled share of the medium vertex's MIS pair, so it scales with the emitter's solid angle.  Left open as **DL-340** by the slice's brief |
| rough random-walk SSS receiver under a closed slab | pre-existing, UNATTRIBUTED: PT+SMS/PT 0.884 after DL-295 / 0.879 before, against 1.016 with no slab (external review round 2; `dl295probe` rows `rough RW-SSS ...`).  Recorded, not filed |
| class 4: floor -> slab1 -> slab2 -> diffuser D -> slab2 -> emitter | NOT DL-295 state (the chain flags after D are identical whichever vertex the camera sees), UNATTRIBUTED: camera at D 0.9987 / 1.0131 (no gap / gap -- the rule holds); FLOOR view 0.927 with no gap (= 0.9275 on master), 0.870 / 0.821 / 0.902 with a weave gap between slab2 and D over three runs (0 on master), 0.981 / 0.983 with slab1 removed; the independent sampler reads the same (0.924 / 0.902) -- external review round 2.  The in-tree fixture (`TwoChainScene`, with a blocker under the up-facing emitter -- without one SMS reaches its black back, DL-347, and reads 5.5x / 16x) reads 0.941 / 0.926 / 0.975 (no gap / gap / slab1 removed; n = 4, 2048 spp, sd ~2-7 %).  **DL-373 (filed at merge)** |

### 9.7 Residuals

- **DL-339** (rewritten twice): PT+SMS relies on SMS for every anchored
  chain through a caster, and two small, specific chains SMS does not
  estimate remain: (a) a refractive caster's REFLECTION -- 0 against
  0.019-0.031 on a worst-case fixture, ~0.1-0.2 % on shipped scenes (the
  documented out-of-scope convention); (b) a single OPEN ior-1.5 refractor
  sheet, 2.25x (the open-sheet convention, DL-345's family).  The round-1
  ior-1.0 content is closed by DL-290.  Pre-existing, identical before and
  after.
- **DL-372 (filed at merge): SMS refraction-chain coverage loss.**  The
  large shipped loss, which DL-339 used to be read as: on shipped
  `sms_visibility_unoccluded` (shipped resolution and spp, n = 3 salted,
  OIDN off, box filter) PT+SMS 3.8255 +/- 0.0018 / PT 4.1709 / VCM 4.1741
  -- turning SMS ON loses **8.3 % of the whole image** and up to **61 %**
  in the 20 x 20-pixel cells over the sphere and its caustic; PT without
  SMS matches VCM to 0.08 %.  Not the reflection convention (a
  `perfectrefractor_material` with no reflection lobe loses the same
  8.31 %) and not the biased estimator (`sms_biased FALSE` 3.8198);
  `sms_luminous_orb` reads 0.998.  This slice's audit control (0.958
  whole / 0.913-0.918 caustic ROI) is the same family.  External review
  round 2's measurement; `ManifoldSolver.cpp` was out of this slice's
  bounds.
- **DL-373 (filed at merge)**: the class-4 floor-view deficit (section
  9.6), unattributed, not DL-295 state.
- **DL-340**: the medium-vertex anchor leak (section 9.6), now measured:
  PT+SMS/PT 0.779 against 0.975 without the slab; and the HWSS medium-walk
  hand-off that still passes a constant `smsHadNonSpecularShading = true`
  (section 9.2).  Its fixtures need a blocker or a one-sided back because
  SMS ignores emitter sidedness (DL-347, on master: a one-sided emitter
  facing away behind a slab reads PT+SMS 0.1009 against PT / VCM 0).
- DL-329 is untouched: the HWSS rows above are parity rows because of it.
