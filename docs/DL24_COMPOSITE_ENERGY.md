# DL-24 — `composite_material` energy, density and evaluator

Slice `debt-dl24`, branched from `master` `5c9eeb96`, 2026-09-28.
Ledger row: [DEBT_LEDGER.md](DEBT_LEDGER.md) DL-24.  Residuals opened:
DL-296, DL-297.  Related rows: DL-221 (narrowed here, still open), DL-67
(its "placeholder `CompositeSPF::Pdf`" residual is resolved here), DL-285
(polished bottom, unchanged).

## 1. What the row claimed, and what was actually wrong

The row said `CompositeSPF`'s random walk "loses ~96 % of the energy" of a
coat-over-diffuse stack.  That figure is stale.  It predates the
2026-09-01 IOR-stack fix ([WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md)
§3.2 update), after which `LayeredWhiteFurnaceTest` config 3
(`dielectric ior 1.5 / white Lambertian`) read
{0.4271, 0.4284, 0.4569, 0.6367} at 0/30/60/80 degrees.  So on this
slice's base the loss was **57 % at normal incidence**, not 96 %.

Three defects were live on the base, not one:

1. **Energy.**  The walk dropped every continuation past its recursion
   budgets.
2. **Density.**  `CompositeSPF::Pdf` was a fixed 50/50 blend of the two
   layers' densities.  It described nothing `Scatter` generated.  The
   walk also emitted up to 12 rays per call (89007 of 200000 calls emitted
   more than one ray at 30 degrees), and every one of them was
   delta-tagged or carried a sub-SPF's density.
3. **Evaluator.**  `CompositeMaterial::GetBSDF()` was "top-wins": it was
   the top layer's BSDF, or the bottom layer's when the top had none.  So
   NEE under a dielectric coat priced the BARE substrate.  It ignored the
   coat's Fresnel, the `1/eta^2` compression and the gap extinction, and
   it did this on top of the walk's own estimate.

### 1.1 Energy ledger (root cause of defect 1)

This measurement came from a scratch instrumentation commit, `683c2bc7`,
which `d1f61bc7` reverts.  The instrumentation followed every emitted and
every dropped continuation of the pre-fix walk per step.  Config 3 at
normal incidence:

| budgets | exit | dropped | where it drops |
|---|---|---|---|
| test 4/2/2/2/2 | 0.4260 | 0.5740 | reflection budget, step 2 |
| parser default 3/3 | 0.4260 | 0.5740 | `max_recursion`, step 3 |
| shipped scene 5/3 | 0.4254 | 0.5746 | diffuse budget, step 3 |
| deep 20/10 | 0.9278 | 0.0722 | reflection budget, step 10 |

`exit + dropped == 1.0000` in every row, so the walk itself conserved
every event.  The loss is the internal Fresnel/TIR reflection at the top
interface's underside.  It is ~56 % of the returning diffuse population,
and the budgets cut its series.  Three other candidates were ruled out by
the same ledger: double attenuation, a missing `1/pdf`, and a convention
flip.

## 2. The fix

`CompositeSPF` now prices three disjoint transport classes.  The header
comment in [CompositeSPF.h](../src/Library/Materials/CompositeSPF.h) is
the authoritative description; this section summarises it.

* **DIRECT.**  The top layer's own up-going lobes.  Scatter draws them by
  a conditional selection of the top's natural rule.  They are weighted
  `w1 = Qup`, so `w1 * conditional == top.Pdf` exactly.
* **COVERED.**  Transport that enters through the top, reaches the bottom
  and leaves through the top in one of two ways: (a) a delta refraction
  after an evaluable bottom event, or (b) a non-delta exit through a top
  that has a BSDF.  A **position-free Monte-Carlo layered evaluator**
  prices it (Guo et al. 2018; PBRT-v4 `LayeredBxDF::f`).  The walk runs
  from the entry direction.  At every bottom visit it CONNECTS to the
  requested exit through the top's delta refraction (term a: bottom BSDF
  at the internal direction, the top's delta transmission kray, gap
  attenuation, `1/eta^2`).  At every top visit it evaluates the top's exit
  BSDF (term b).  The walk is seeded by a hash (PCG32 + SplitMix64) of the
  quantised incoming direction, outgoing direction and position.  So
  `value(w)` is a DETERMINISTIC function of its arguments.  COVERED
  directions are sampled from a known mixture: a cosine hemisphere
  (`w2`), and the bottom's own sampler at the outer record (`w3`,
  CoatedSPF's substrate proposal).
* **WALKER.**  Everything else: all-delta walks, a delta event after the
  last non-delta event, layers with no BSDF, transmission out through the
  BOTTOM, and entries from below.  An unbiased single-path walk samples
  these and emits them DELTA-TAGGED.  No NEE or connection strategy
  partners them, so the MIS partition stays exact.  The walker keeps a
  floor share (0.05 of the walked mass, or 0.5 when a one-path probe finds
  transport the evaluator cannot price), so a mis-probe costs variance and
  never energy.

**Neither walk is truncated.**  The five budgets (`max_recursion`,
`max_*_recursion`) are now **Russian-roulette onsets**.  Past a budget,
the walk continues under roulette with `p = min(1, max(beta))`, and the
survival is compensated.  `kMaxWalkEvents = 256` is only a safety cap.  A
walk that reaches it still carrying energy is a lossless trapping pair,
and the SPF warns once per process.  The chunk descriptor, `IJob.h`,
`RISE_API.h`, `Job.{h,cpp}` and `RISE_API.cpp` all document the budgets
this way now.

Walk selection is **throughput-aware** (`SelectCarried`): it selects by
`MaxValue(beta * kray)`, not by `MaxValue(kray)`.  Selecting by the lobe
alone read 0.926 on a dispersive per-channel top, because the carried
colour and the lobe colour diverge.  Throughput-aware selection reads
0.996 / 1.003 / 1.000.

### 2.1 Density (defect 2)

`Pdf`/`PdfNM` return the density of the NON-DELTA directions `Scatter`
emits.  That is the DL-67 contract.  Where no known non-delta emission
exists the value is a sub-density.  It is the mixture:

`Pdf(w) = w1 * top.Pdf(w) / Qup (only when the top has a BSDF) + w2 * cos/pi + w3 * bottom.Pdf(w)`

It is computed with the SAME probe and weights `Scatter` uses.  A
per-thread 4-entry cache holds each probe.  The cache is keyed on the
composite's process-unique `instanceId` (never its address), the
wavelength, and 32 record/stack fields.  So a probe is computed once per
vertex and shared by Scatter, every NEE sample's partner and the HWSS
companions.  A cache miss only costs a recomputation.  `Scatter` emits
**at most one ray per call**.

When the top has NO BSDF (skin, or an SPF-only composite), a non-delta
DIRECT lobe cannot be priced by NEE either.  It is re-tagged delta and
priced by its own lobe estimator.  The `w1` term is then left out of
`Pdf`.  An earlier draft priced it with `direct = 0` and lost that energy.

### 2.2 Evaluator (defect 3) — DL-157 "one function per side"

`CompositeMaterial` hands both layers' BSDFs to the SPF.  When either
layer has one, it presents a new `CompositeBSDF` whose `value` /
`valueNM` / `valueStateful{,NM}` forward to `CompositeSPF::EvaluateLayered`.
That is the same function the SPF prices its COVERED and non-delta DIRECT
emissions with, so for every emitted non-delta ray:

`kray * Pdf(dir) == value(dir) * cos`

This holds pointwise (section C: 0 mismatches over 65 857 rays, RGB and NM).
The pre-DL-24 top-wins BSDF is gone.  `CompositeMaterial::ScattersFullSphere()`
is now **false**: the layered value has no below-horizon support, and
transmission through the stack is WALKER-only (DL-296).  That supersedes
DL-157 P2-1's forwarding rule.  `composite { top = translucent }` used to
forward the capability, which lit back faces of opaque-bottomed
composites.  DL-126's null-BSDF composition still works: a composite with
no layer BSDF presents no BSDF, and its whole transport is WALKER-class.

## 3. Evidence

Every A/B below is against committed state.  The three library files were
checked out at `5c9eeb96` on top of a WIP commit, the library and test
targets were rebuilt, the tests were run, and the files were restored with
`git checkout HEAD --`.  `git stash` was never used.

### 3.1 White furnace (`tests/CompositeEnergyConservationTest.cpp` section A)

The top is lossless and the bottom is an albedo-1 Lambertian, so the
truth is rho == 1.  Each cell is the mean ± sd of 8 batch means ×
20 000 draws; the gate is |rho − 1| ≤ max(5 sem, 0.01).

| config | pre-fix 0 / 30 / 60 / 80 deg | post-fix RGB | post-fix NM 550 |
|---|---|---|---|
| A1 diel(scat 0)/white, 4/2/2/2/2 (config 3) | 0.4258±.0035 / 0.4275±.0030 / 0.4585±.0029 / 0.6339±.0020 | 1.0005±.0045 / 1.0005±.0055 / 1.0032±.0066 / 0.9989±.0051 | 1.0008±.0046 / 0.9982±.0055 / 1.0030±.0065 / 0.9988±.0047 |
| A2 diel(scat 1e4)/white, 3/3 (parser default) | same as A1 | same as A1 | same as A1 |
| A3 diel(scat 1e4)/white, 5/3, t=0.5 (shipped budgets) | 0.4266±.0018 / 0.4302±.0031 / 0.4571±.0028 / 0.6347±.0023 | 1.0000±.0058 / 0.9969±.0042 / 1.0014±.0042 / 0.9976±.0073 | 0.9999±.0061 / 0.9984±.0045 / 1.0010±.0040 / 0.9975±.0071 |
| A4 water(1.33)/white, 3/3 | 0.5360±.0041 / 0.5360±.0036 / 0.5573±.0043 / 0.6903±.0023 | 0.9993±.0063 / 0.9994±.0051 / 1.0017±.0051 / 1.0014±.0046 | 0.9993±.0061 / 0.9997±.0051 / 1.0015±.0048 / 1.0017±.0047 |
| control `coated_material` (ior 1.5, rough 0.001)/white | 1.0001 / 1.0002 / 1.0004 / 1.0000 (±≤.0010), both builds | | |

A1 equals A2 bit for bit post-fix because the evaluator treats the top's
delta transmission as ideal Snell whatever its `scattering` is.  That is
DL-297.

Section A is **production transport**, so it is gated in
`LayeredWhiteFurnaceTest` too.  Config 3 was pinned at the truncated
{0.4271, …} and now reads **1.0000** at kPosturePass (band 0.01).  Two new
material-path rows, configs 58 (config 3 through `CompositeMaterial`) and
59 (water, t=0.5), read 1.0000 in a 0.02 band.  The suite reads **0 of 60
configurations failed**; the red-proof on base is 3 of 60.

### 3.2 Closed-form value (section G)

For a smooth coat over a Lambertian with an absorbing gap:

`f = T_i T_o a_i a_o rho / (pi n^2 (1 - rho E_ret))`

Here `a` is the slant gap attenuation and `E_ret` is the internal
diffuse-return reflectance.  `value` matches this at five
`(theta_i, theta_o)` pairs, white and red, per channel, to within 1 sd of
the 120 000-sample mean.  For example, red channel 0 at (0,0) reads
0.077926 ± 0.000382 against 0.078517.

### 3.3 Density (section B, theta 30)

| row | int Pdf vs measured non-delta emission | TVD (floor, gate) | pre-fix |
|---|---|---|---|
| B1 diel/white | 0.91055 vs 0.91098 | 0.00869 (0.01848, 0.02772) | 0.50000 vs 0.00000; 89 007 multi-ray calls |
| B2 translucent/red | 0.96500 vs 0.96439 | 0.00967 (0.01742, 0.02613) | TVD 0.34975 |
| B3 clearcoat GGX/red GGX | 0.97460 vs 0.97414 | 0.00920 (0.01654, 0.02480) | TVD 0.24800 |

`SPFPdfConsistencyTest` builds its composite rows through
`CompositeMaterial` now, with every check ON (cross-validation, chi²,
exact selected pdf): `Composite`, `Composite_DielectricLambertian` and
`Composite_TranslucentLambertian`.  All pass.  The red-proof on base is 12
FAILED.

### 3.4 Render level (section D, 32×32, PT and BDPT pel)

* **D1, environment white furnace.**  A composite quad and a Lambertian
  control quad sit under the same environment.  PT composite read 0.835
  pre-fix and **1.0026** post-fix; the control reads 1.0021.  BDPT has a
  known env-only bias of ~+22 % (identical on the control), so it is gated
  on the ratio: 1.117 pre-fix → **1.0052**.
* **D2, directional light.**  The composite/control ratio must equal the
  smooth-coat closed form `T(v) T(l) / (eta^2 (1 - r_i))`:

  | theta_l | pre-fix PT / BDPT | post-fix PT / BDPT | closed form |
  |---|---|---|---|
  | 0 | 1.0000 / 1.0000 | 1.0227 / 1.0243 | 1.0147 |
  | 60 | 1.0000 / 1.0000 | 0.9665 / 0.9662 | 0.9627 |
  | 80 | 1.0000 / 1.0000 | 0.6456 / 0.6445 | 0.6472 |

  The pre-fix ratio was exactly 1 because NEE priced the bare substrate.
* **`BDPTStrategyBalanceTest` topology Q.**  A clear coat with extinction
  (0.2, 0.5, 2.0) and t = 0.1 over red, plus a translucent-over-gold
  floor, lit by an area emitter.  PT and BDPT agree to ~0.5 %.  Topology Q
  is a consistency pin, not a red-proof: base also passes at ~0.25 %,
  because both integrators shared the same wrong model.

### 3.5 HWSS / DL-221 (section E, `HWSSCompanionKrayTest` section D)

On the material path, every non-delta emitted ray's companion weight is
reconstructed from `(ri, dir, nm)`:

* `EvaluateLobeFNM` returns the aggregate layered `valueNM`.
* The 6-argument `EvaluateKrayNM` equals `valueNM(nm) cos / pdfHero`.
* The 5-argument form reconstructs the top's DIRECT delta reflection by
  re-probing the top at `nm`.

The count is 3627 reconstructed, 0 declined, 0 mismatches.  Over a
chromatic bottom the companion weight really moves with `nm`, which was
the row's own "divergent spectra" premise.  `HWSSCompanionKrayTest` reads
193/0; base reads 190/3.

**DL-221 verdict: narrowed, not closable.**  A WALKER-emitted ray is one
realisation of a stochastic walk: a bottom exit, a from-below entry, a
null-BSDF layer, or an all-delta chain.  It cannot be recovered from the
`EvaluateKrayNM` arguments, so it still declines and names itself.  An
SPF-only composite (no layer BSDFs) still declines everywhere.  For the
coat-over-diffuse regime, WALKER emissions are rare.  After an evaluable
bottom the walker's top exits are COVERED and are not emitted, so most
composite vertices now reconstruct exactly.

### 3.6 Sibling configuration classes (section F, full-sphere furnace)

| class | pre-fix 0 / 60 deg | post-fix 0 / 60 deg | note |
|---|---|---|---|
| F1 dielectric / dielectric | 0.9985 / 0.9965 | 1.0000 / 1.0000 | walker, gated |
| F2 dielectric / composite(dielectric/white) (`mat_double_composite` shape) | 0.2359 / 0.2749 | 0.9922 / 0.9985 | delta bottom lobe → walker, gated |
| F2b dielectric / polished(white) | 0.4502 / 0.4766 | 1.1020 / 1.0749 | RECORD ONLY: DL-285, `polished_material::GetBSDF()` is the bare Lambertian |
| F3 lossless translucent / white | 1.0000 / 1.0000 | 0.9989 / 1.0006 | gated |
| F4 translucent / red (`mat_wax_gold` class) | 0.5800 / 0.5800 | 0.7678 / 0.7663 | absorbing, not gated; post-fix includes the whole interreflection series |
| F5 dielectric / translucent (transmitting bottom) | 0.8283 / 0.8368 | 1.0016 / 0.9983 | bottom exits via walker, gated |
| F6 clearcoat GGX / red GGX (config 7) | 0.0389 / 0.0655 | 0.0389 / 0.0654 | unchanged: `GGXSPF` emits no downward lobe, so the substrate is never reached; `coated_material` is the answer |
| F7 dielectric / generic_human_tissue (null bottom BSDF) | 1.0000 / 0.9172 | 1.0000 / 1.0000 | DL-126 composition |

`CompositeExtinctionTest` bands were calibrated to the truncated walk.
They were replaced by a closed-form interreflection series
(`AnalyticCrossed`): lo 0.95981, hi-extinction ratio 0.04868, thick ratio
0.2217, absorbing slant 0.02522 vs perpendicular 0.03767.  Each is gated
±10 % against the closed form.  Base fails 5 of these.

### 3.7 Shipped scene

`scenes/Tests/Materials/composite_material.RISEscene` is the only shipped
scene that binds `composite_material`.  Render setup: 512×288, 64 spp,
`oidn_denoise FALSE`, linear EXR.  Two separately built binaries (base
`5c9eeb96`; fix `e761daa8`), n = 5 interleaved.  Luminance mean (sd):

| region | base | fix | delta |
|---|---|---|---|
| clearcoat red sphere | 0.19582 (0.00004) | 0.15794 (0.00010) | −19.35 % |
| amber red box (extinction) | 0.22678 (0.00008) | 0.07504 (0.00026) | −66.91 % |
| white ellipsoid, blue extinction 0.2/0.5/6.0 | 0.76837 (0.00059) | 0.18732 (0.00047) | −75.62 %, now warm orange (0.33, 0.16, 0.04) |
| wax over gold torus | 1.02212 (0.00041) | 1.00023 (0.00037) | −2.14 % |
| glass over emissive sphere | 8.82924 (0.00035) | 8.44167 (0.00044) | −4.39 % |
| double composite cylinder | 0.46448 (0.00031) | 0.22700 (0.00024) | −51.13 % |
| floor (control) | 0.79222 (0.00009) | 0.78604 (0.00004) | −0.78 % |
| back wall (control) | 0.51945 (0.00018) | 0.51614 (0.00012) | −0.64 % |
| whole image | 1.04700 (0.00007) | 1.00474 (0.00010) | −4.04 % |

Composites DARKEN overall, even though the walk now conserves energy.
Pre-fix NEE priced the bare substrate.  It ignored the coat's Fresnel
transmission, the `1/eta^2` compression (−56 % for ior 1.5) and the gap
extinction, which is what makes the amber and blue-extinction objects
look amber and warm at all.  So the NEE correction darkens more than the
restored indirect energy brightens.  Section D2 is the reference-free
proof that the new NEE value is right.

### 3.8 Cost

**Shipped scene, whole-render user CPU**, n = 5 interleaved:

* base: 66.80 ± 0.50 s
* fix: 90.47 ± 1.85 s — **+35.4 %**
* wall clock: 8.14 s → 11.13 s

The scene is composite-dominated: 6 of its 8 objects are composites.  A
scene with one composite pays proportionally less.

**Per-call microbenchmark** (coat over Lambertian):

* `Scatter`: 1.5–1.8 µs
* `Pdf`, uncached: ~0.57 µs
* `value`: 0.9–1.1 µs
* double composite: 9.5 / 3.1 / 6.3 µs

The render cost is dominated by the evaluator walk that each NEE light
sample runs (the scene has 3 lights).  Two optimisations are in place: the
probe cache, and one reused layer record per walk.  Caching the walk
itself across a vertex's NEE samples was considered but not implemented;
the walk depends on the exit direction.

## 4. Gates (this slice's final state)

The clean library rebuild and every test target built had **0 warnings**.

| suite | post-fix | base (red) |
|---|---|---|
| CompositeEnergyConservationTest (new) | 116/0 | 34/82 |
| LayeredWhiteFurnaceTest | 0 of 60 failed | 3 of 60 |
| CompositeExtinctionTest | all pass | 5 FAIL |
| SPFPdfConsistencyTest | all pass | 12 FAILED |
| HWSSCompanionKrayTest | 193/0 | 190/3 |
| TranslucentLobeConsistencyTest | 1226/0 | 1222/4 (gate 6 re-ruled) |
| BDPTStrategyBalanceTest (full) | 215/0 | topology Q passes on base too |
| PTGuidingMISPartitionTest | 185/0 | — |
| BDPTGuidedContinuationTest | 164/0 | — |
| SPFBSDFConsistencyTest | pass | — |
| VCMStrategyBalanceTest | 74/0 | — |
| CstDeriveGoldenTest | 454 MATCH, 0 DRIFT | — |
| SourceHygieneTest | 167/0 | — |
| SSSRadianceScalingTest | 576220/0 | (base 576220/0) |
| RefractiveRadianceScalingTest | 40/1 | row C, the known pre-existing regression; base 40/1 |
| ConnectionLegality / AgentMakeFabric / ReferenceGraph / SceneEditTransaction / LightBVH | 319/0, 273/0, 375/0, 274/0, 20/0 | — |

## 5. Residuals

* **DL-296 — WALKER-class transport is invisible to NEE and to
  connections.**  This covers transmission out through a transmitting
  bottom, entries from below, null-BSDF layers and all-delta chains.  All
  of it is delta-tagged by design, so the MIS partition is exact.  The
  cost: a delta light (point/spot/directional) contributes nothing through
  it, and an area light reaches it only via BSDF-sampled hits.
  `ScattersFullSphere` is false.  The recipe is in the ledger row.
* **DL-297 — term (a) treats the top's delta-tagged transmission as ideal
  Snell.**  A `DielectricSPF` with finite `scattering` warps its
  transmitted direction (Phong `cos^N` about the Snell axis) while still
  tagging it delta.  The evaluator connects through the ideal direction.
  Energy is exact, but the warp's angular blur is lost.  Energy-weighted
  exit cos-histograms at theta 0:

  | top | bins |
  |---|---|
  | scat 0, evaluator | 0.013 / 0.064 / 0.128 / 0.193 / 0.251 / 0.349 |
  | scat 0, walker-only (follows the warp) | 0.123 / 0.136 / 0.150 / 0.167 / 0.185 / 0.239 |
  | scat 1e4 (the parser default) | evaluator and walker agree within noise |

* **DL-285** (polished bottom) and **config 7 / F6** (reflection-only GGX
  top) are unchanged.  So is the RefractiveRadianceScalingTest row C
  regression.
* **Premise note for DL-67.**  A composite now satisfies DL-67's premise
  2: its `IBSDF::value` and its kray describe one function for every
  non-delta ray.  Its WALKER rays are delta-tagged, and the guided
  continuation prices those at `W_b = 1`.

## 6. `add_wetness`

The route-around in WETNESS_COAT_DESIGN.md §4(a) / §12 item 2 is no
longer *necessary*: coat-over-diffuse now conserves energy, NEE sees the
layered response, and `Pdf` is exact.  It remains the right choice, and
`add_wetness` should **not** switch back.  `coated_material` is
closed-form, costs no MC evaluator walk per NEE sample, has lower
variance, and has its own furnace/HWSS guards.  `composite_material`
also loses a finite-`scattering` coat's blur (DL-297).
`composite_material` is now a valid general two-layer stack; wetness is
the coat case `coated_material` was built for.
