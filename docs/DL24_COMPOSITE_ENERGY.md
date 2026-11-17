# DL-24 — `composite_material` energy, density and evaluator

Slice `debt-dl24`, branched from `master` `5c9eeb96`, 2026-09-28.
Ledger row: [DEBT_LEDGER.md](DEBT_LEDGER.md) DL-24.  Residuals opened:
DL-296, DL-297; two more recorded for the supervisor to file at merge,
DL-341 (the composite's stack-gap family, section 5) and DL-342
(`coated_material` too bright under an absorbing coat).  Two external
review rounds (round 1 FAIL: 2 P1, 3 P2; round 2 FAIL: 1 P1, 2 P2, P3s)
were addressed the same day; their fixes and the corrected numbers are in
sections 7 and 8, and the sections below are updated to the post-review
state.  After round 2 the branch was merged with `master` `1e20e1f6`
(DL-05, DL-306, DL-291); section 8 records the one semantic conflict.  Related rows: DL-221 (narrowed here, still open), DL-67
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
  a conditional selection of the top's natural rule.  When the top
  DECLARES a deterministic up/down split AT THIS RECORD
  (`ISPF::SelectionMassIsDeterministic( ri, nm )`: perfect
  reflector/refractor and translucent always; a dielectric only when its
  transmission warp is off or its shading normal equals its geometric
  normal -- section 8, P1-A) they are weighted `w1 = Qup`, so
  `w1 * conditional == top.Pdf` exactly (AGGREGATE mode).  Every other top
  runs PER-BRANCH mode (section 7, P1-1), whose weights average
  `kPerBranchProbes` = 8 hashed probes of the top (section 8).
* **COVERED.**  Transport that enters through the top, reaches the bottom
  and leaves through the top in one of two ways: (a) a delta refraction
  after an evaluable bottom event, or (b) a non-delta exit through a top
  that has a BSDF.  A **position-free Monte-Carlo layered evaluator**
  prices it (Guo et al. 2018; PBRT-v4 `LayeredBxDF::f`).  The walk runs
  from the entry direction.  At every bottom visit it CONNECTS to the
  requested exit through the top's delta refraction (term a: bottom BSDF
  at the internal direction, the top's delta transmission kray, gap
  attenuation, `1/eta^2`).  At every top visit it evaluates the top's exit
  BSDF (term b).  The walk is drawn ONCE per (incoming direction,
  position) from a hash-seeded PCG32 stream and recorded; each exit query
  re-evaluates only the two connection terms, term (a)'s top transmission
  drawn from a second stream seeded by (incoming, outgoing, position)
  (section 7, P2-3).  So `value(w)` is a deterministic function of its
  arguments -- but it is ONE Monte-Carlo estimate per query, not the
  closed form: per-evaluation relative sd ~0.80 for a smooth coat over
  white, ~0.39 over red with an absorbing gap (section 3.2).  COVERED
  directions are sampled from a known mixture: a cosine hemisphere
  (`w2`), and the bottom's own sampler at the outer record (`w3`,
  CoatedSPF's substrate proposal).
* **WALKER.**  Everything else: all-delta walks, a delta event after the
  last non-delta event, layers with no BSDF, transmission out through the
  BOTTOM (except, since DL-296, a non-delta exit through a bottom whose
  own BSDF transmits -- term (c), section 10), and entries from below.  An unbiased single-path walk samples
  these and emits them DELTA-TAGGED.  No NEE or connection strategy
  partners them, so the MIS partition stays exact.  The walker keeps a
  floor share (0.05 of the walked mass, or 0.5 when a one-path probe finds
  transport the evaluator cannot price), so a mis-probe costs variance and
  never energy.

**Neither walk is truncated.**  The five budgets (`max_recursion`,
`max_*_recursion`) are now **Russian-roulette onsets**.  Past a budget,
the walk continues under roulette with `p = min(1, max(beta))`, and the
survival is compensated.  `kMaxWalkEvents = 256` is only a safety cap.  A
walk that reaches it still carrying energy is either a lossless trapping
pair or -- far more often in practice -- the DL-341 stack-gap residual
(section 5: 91 323 of 100 000 H4 walks at 35 deg reach it, and that energy
is lost); the SPF warns once per process.  The chunk descriptor, `IJob.h`,
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
transmission through the stack is WALKER-only (DL-296).  **Superseded for
transmitting bottoms by section 10 (2026-10-09):** term (c) gives the
value a below-horizon part and the capability is claimed exactly then.  That supersedes
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
20 000 draws; the gate is |rho − 1| ≤ max(5 sem, 0.01).  Since the review
the shading POSITION moves every draw: the evaluator draws one walk per
(incoming direction, position), so it is unbiased averaged over positions,
and a fixed-position furnace would read one walk realisation.

| configuration | pre-fix 0 / 30 / 60 / 80 deg | post-fix RGB | post-fix NM 550 |
|---|---|---|---|
| A1 diel(scat 0)/white, 4/2/2/2/2 (config 3) | 0.4258±.0035 / 0.4275±.0030 / 0.4585±.0029 / 0.6339±.0020 | 1.0020±.0066 / 1.0026±.0052 / 0.9985±.0070 / 0.9982±.0065 | 1.0022±.0061 / 1.0007±.0051 / 0.9985±.0070 / 0.9982±.0065 |
| A2 diel(scat 1e4)/white, 3/3 (parser default) | same as A1 | same as A1 | same as A1 |
| A3 diel(scat 1e4)/white, 5/3, t=0.5 (shipped budgets) | 0.4266±.0018 / 0.4302±.0031 / 0.4571±.0028 / 0.6347±.0023 | 1.0012±.0059 / 1.0055±.0047 / 1.0010±.0069 / 0.9996±.0081 | 1.0012±.0058 / 1.0022±.0070 / 1.0006±.0066 / 0.9993±.0081 |
| A4 water(1.33)/white, 3/3 | 0.5360±.0041 / 0.5360±.0036 / 0.5573±.0043 / 0.6903±.0023 | 0.9993±.0073 / 0.9999±.0031 / 1.0003±.0042 / 0.9995±.0053 | 0.9993±.0072 / 1.0000±.0030 / 1.0004±.0040 / 0.9995±.0054 |
| control `coated_material` (ior 1.5, rough 0.001)/white | ~1.000 | 0.9998 / 1.0001 / 1.0001 / 1.0001 (±≤.0011) | |

A1 equalled A2 bit for bit because the evaluator treated the top's
delta transmission as ideal Snell whatever its `scattering` was -- DL-297,
fixed 2026-10-02 (section 9): A1 (`scattering 0`, the widest warp) now
carries its own estimate, still 1 within noise but with a larger
per-evaluation spread (sd of the batch means up to 0.017 at 60 deg).

Section A is **production transport**, so it is gated in
`LayeredWhiteFurnaceTest` too.  Config 3 was pinned at the truncated
{0.4271, …} and now reads **1.0000** at kPosturePass (band 0.01).  Two new
material-path rows, configs 58 (config 3 through `CompositeMaterial`) and
59 (water, t=0.5), read {0.9972, 1.0039, 0.9961, 1.0002} and
{0.9991, 1.0005, 1.0009, 1.0027} in a 0.02 band -- position-jittered, the
only rows in that suite that move the shading point.  The suite reads **0 of 60
configurations failed**; the red-proof on base is 3 of 60.

### 3.2 Closed-form value (section G)

For a smooth coat over a Lambertian with an absorbing gap:

`f = T_i T_o a_i a_o rho / (pi n^2 (1 - rho E_ret))`

Here `a` is the slant gap attenuation and `E_ret` is the internal
diffuse-return reflectance.  `value` matches this at five
`(theta_i, theta_o)` pairs, white and red, per channel, inside the test's
band of max(4 sem, 0.4 %).  (An earlier revision of this sentence said
"within 1 sd"; the worst rows were 1.55 and 1.47 sem off.)  After the
review the sample count is 24 000 per pair and every row prints its
distance in sem: the largest is 2.6 sem (the white rows' per-evaluation
distribution is heavy-tailed, so their sem is itself noisy; a 60 000-sample
check of the worst row read 1.3 sem), and red channel 0 at (0,0) reads
0.078546 against 0.078517 (0.15 sem).  The same rows print the
per-evaluation relative sd -- ~0.8-0.96 for G1 (white), ~0.39-0.46 for G2
red channel 0 -- which is the noise one `value` query carries into an NEE
sample or a BDPT connection.  The MIS partition is not affected by it:
only the exact `Pdf` enters the weights.

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
  pre-fix and **0.9990** post-review (1.0026 before it); the control reads
  1.0021.  BDPT has a known env-only bias of ~+22 % (identical on the
  control), so it is gated on the ratio: 1.117 pre-fix → **0.9922**
  (1.0052 before the review).  Both inside the 2 % gate; run-to-run noise
  of this 32×32 render is about ±0.6 %.
* **D2, directional light.**  The composite/control ratio must equal the
  smooth-coat closed form `T(v) T(l) / (eta^2 (1 - r_i))`:

  | theta_l | pre-fix PT / BDPT | post-fix PT / BDPT | closed form |
  |---|---|---|---|
  | 0 | 1.0000 / 1.0000 | 1.0083 / 1.0176 | 1.0147 |
  | 60 | 1.0000 / 1.0000 | 0.9714 / 0.9596 | 0.9627 |
  | 80 | 1.0000 / 1.0000 | 0.6513 / 0.6492 | 0.6472 |

  The pre-fix ratio was exactly 1 because NEE priced the bare substrate.
* **`BDPTStrategyBalanceTest` topology S** (lettered Q before the review;
  a concurrent slice owns Q/R).  A clear coat with extinction
  (0.2, 0.5, 2.0) and t = 0.1 over red, plus a translucent-over-gold
  floor, lit by an area emitter.  PT and BDPT agree to ~0.5 %.  It is a
  consistency pin, not a red-proof: base also passes at ~0.25 %, because
  both integrators shared the same wrong model.

### 3.5 HWSS / DL-221 (section E, `HWSSCompanionKrayTest` section D)

On the material path, every non-delta emitted ray's companion weight is
reconstructed from `(ri, dir, nm)`:

* `EvaluateLobeFNM` returns the aggregate layered `valueNM`.
* The 6-argument `EvaluateKrayNM` equals `valueNM(nm) cos / pdfHero`.
* The 5-argument form reconstructs the top's DIRECT delta reflection by
  re-probing the top at `nm` -- in AGGREGATE mode only, and only for an
  `eRayReflection`-typed ray matched to a reflection-typed top lobe
  (section 7, P1-2).  Exact for a non-dispersive top.  For a DISPERSIVE
  top it returns `kray_nm / q_nm`, and a dielectric selects its
  reflection with probability `q = F`, so that is the hero's own weight
  (companion ratio 1) where the right ratio is `F(nm) / F(hero)` -- the
  method cannot form it (the hero wavelength is not an argument).  Close,
  not exact.

The count is 3627 reconstructed, 0 declined, 0 mismatches.  Over a
chromatic bottom the companion weight really moves with `nm`, which was
the row's own "divergent spectra" premise.  `HWSSCompanionKrayTest` reads
193/0; base reads 190/3.

**DL-221 verdict: narrowed, not closable.**  A WALKER-emitted ray is one
realisation of a stochastic walk: a bottom exit, a from-below entry, a
null-BSDF layer, or an all-delta chain.  It cannot be recovered from the
`EvaluateKrayNM` arguments, so it declines and names itself.  (The first
revision of this section claimed that and was WRONG: an all-delta walker
path leaves the top exactly along the entry's mirror direction, so the
5-argument form matched it as a direct reflection and returned a wrong
number -- 7 366 of 15 709 up-going rays on grey glass/glass, worst
relative error 7.34.  Fixed by type, section 7 P1-2; the walker exits now
decline, 0 mismatches.)  PER-BRANCH composites and SPF-only composites (no
layer BSDFs) decline everywhere.  For the
coat-over-diffuse regime, WALKER emissions are rare.  After an evaluable
bottom the walker's top exits are COVERED and are not emitted, so most
composite vertices now reconstruct exactly.  That does NOT hold over a
bottom with a DELTA coat.  The round-2 reviewer measured
composite{glass / polished-red}: 3 768 of every 100 000 up-going emissions
are walker rays priced by the fallback, and `hwss TRUE` reads **-9.7 %**
against `hwss FALSE` under PT and **-8.5 %** under BDPT
(composite{glass/glass}, whose walker rays are grey, shows no significant
difference).  Carried into the DL-221 row.

### 3.6 Sibling configuration classes (section F, full-sphere furnace)

| class | pre-fix 0 / 60 deg | post-fix 0 / 60 deg | note |
|---|---|---|---|
| F1 dielectric / dielectric | 0.9985 / 0.9965 | 1.0000 / 1.0000 | walker, gated |
| F2 dielectric / composite(dielectric/white) (`mat_double_composite` shape) | 0.2359 / 0.2749 | 1.0001 / 0.9947 | delta bottom lobe → walker, gated |
| F2b dielectric / polished(white) | 0.4502 / 0.4766 | 1.1098 / 1.0724 | KNOWN-DEFECT PIN [1.05, 1.15]: DL-285, `polished_material::GetBSDF()` is the bare Lambertian |
| F3 lossless translucent / white | 1.0000 / 1.0000 | 1.0009 / 0.9992 | gated |
| F4 translucent / red (`mat_wax_gold` class) | 0.5800 / 0.5800 | 0.7683 / 0.7658 | absorbing, not gated; post-fix includes the whole interreflection series |
| F5 dielectric / translucent (transmitting bottom) | 0.8283 / 0.8368 | 0.9927 / 0.9989 | bottom exits via walker, gated |
| F6 clearcoat GGX / red GGX (config 7) | 0.0389 / 0.0655 | 0.0389 / 0.0656 | unchanged: `GGXSPF` emits no downward lobe, so the substrate is never reached; `coated_material` is the answer |
| F7 dielectric / generic_human_tissue (null bottom BSDF) | 1.0000 / 0.9172 | 1.0000 / 1.0000 | DL-126 composition |

Section H (added by the review) puts STOCHASTIC layers on TOP, with the
position jittered every draw; truth 1 for the lossless rows:

| class | pre-review `c03807a2` 0 / 60 deg | current (after round 2's 8-probe weights) 0 / 60 deg (mean ± sem) | note |
|---|---|---|---|
| H1 generic_human_tissue / white | 0.9985 / **0.6212** | 0.9999 ± .0041 / 1.0061 ± .0042 (round 1: 0.9939 ± .0112) | gated |
| H2 composite{dielectric / lossless translucent} / white | **0.9418 / 0.9351** | 1.0011 ± .0022 / 1.0004 ± .0026 (round 1: 1.0030 / 0.9969) | gated |
| H3 composite{dielectric / dielectric} / white | 0.3811 / 0.3530 | 0.4781 ± .0016 / 0.5084 ± .0032 (round 1: 0.4785 / 0.5108) | KNOWN RESIDUAL, pinned [0.40, 0.62] (section 5) |
| H4 composite{dielectric / dielectric} struck from inside | 1.0000 (20 deg) / 0.0868 (35 deg) | same | KNOWN RESIDUAL, pinned [0.04, 0.20] at 35 deg; base `5c9eeb96` reads the same 0.086 |

Section T (added by review round 2, P1-A) tilts the SHADING normal of a
flat +Z surface and compares composite{dielectric / white} against an
independent natural layer walk written in the test; the pre-fix column is
the same tree with `DielectricSPF::SelectionMassIsDeterministic` forced
back to the round-1 claim (`true`).  Composite n = 64 x 20 000, walk
n = 16 x 20 000, band 5 sigma + 0.001:

| top | tilt / theta | independent walk | pre-fix composite (z) | post-fix composite (z) |
|---|---|---|---|---|
| scattering 0 | 0 / 0, 45 | 1.0000 / 1.0000 | 0.9994 / 1.0003 | 0.9994 / 1.0003 (unchanged) |
| scattering 0 | 5 / 0 | 0.9781 | 0.9539 (**-24.9**) | 0.9766 (-1.6) |
| scattering 0 | 5 / 45 | 0.9781 | 0.9493 (**-32.6**) | 0.9778 (-0.4) |
| scattering 0 | 20 / 0 | 0.8844 | 0.8086 (**-48.8**) | 0.8856 (+0.9) |
| scattering 0 | 20 / 45 | 0.8966 | 0.8133 (**-69.4**) | 0.8976 (+0.9) |
| scattering 0 | 35 / 0 | 0.7787 | 0.6785 (**-51.3**) | 0.7754 (-2.3) |
| scattering 0 | 35 / 45 | 0.8632 | 0.7911 (**-65.7**) | 0.8618 (-1.3) |
| scattering 5 | 5 / 0, 45 | 0.9881 / 0.9881 | 0.9874 / 0.9877 | 0.9874 / 0.9892 |
| scattering 5 | 20 / 45 | 0.9202 | 0.9139 (**-7.2**) | 0.9207 (+0.5) |
| scattering 5 | 35 / 45 | 0.8685 | 0.8605 (**-8.6**) | 0.8680 (-0.5) |
| scattering 1e6 (warp off: still DECLARED, aggregate mode) | 5, 20, 35 / 0, 45 | 0.9957 .. 0.8803 | same as post-fix (the claim is unchanged) | \|z\| <= 2.2 on all six |
| lossless translucent (its unconditional declaration kept) | 5, 20, 35 / 0, 45 | 0.9986 .. 0.9362 | same as post-fix | \|z\| <= 0.94 on all six (sem 0.0002-0.0005) |

The walk itself reads below 1 because a tilted shading normal is not
energy-conserving; the gate is agreement, not 1.  T2 (the E2 twin under a
20 deg tilt, grey, hero 550 / companion 600): pre-fix 92 734 up-going rays
reconstructed, **296 wrong** (worst relative error 16.3); post-fix every
one declines (the top is per-branch there), 0 wrong.  A ray that arrives
BEHIND the tilted shading normal (tilt 35, theta 60) is pinned instead of
gated: section 5, DL-341.

`CompositeExtinctionTest` bands were calibrated to the truncated walk.
They were replaced by a closed-form interreflection series
(`AnalyticCrossed`): lo 0.95981, hi-extinction ratio 0.04868, thick ratio
0.2217, absorbing slant 0.02522 vs perpendicular 0.03767.  Each is gated
±10 % against the closed form.  Base fails 5 of these.

### 3.7 Shipped scene

`scenes/Tests/Materials/composite_material.RISEscene` is the only shipped
scene that binds `composite_material`.  Render setup: 512×288, 64 spp,
`oidn_denoise FALSE`, linear EXR.  Two separately built binaries (base
`5c9eeb96`; fix = this branch after the review), n = 5 interleaved.
Luminance mean (sd):

| region | base | fix | delta |
|---|---|---|---|
| clearcoat red sphere | 0.19579 (0.00005) | 0.15788 (0.00012) | −19.36 % |
| amber red box (extinction) | 0.22677 (0.00007) | 0.07526 (0.00024) | −66.81 % |
| white ellipsoid, blue extinction 0.2/0.5/6.0 | 0.76850 (0.00065) | 0.18706 (0.00059) | −75.66 %, now warm orange (0.33, 0.16, 0.04) |
| wax over gold torus | 1.02242 (0.00025) | 0.99986 (0.00065) | −2.21 % |
| glass over emissive sphere | 8.82884 (0.00041) | 8.44115 (0.00043) | −4.39 % |
| double composite cylinder | 0.46479 (0.00033) | 0.22674 (0.00017) | −51.22 % |
| floor (control) | 0.79227 (0.00004) | 0.78606 (0.00008) | −0.78 % |
| back wall (control) | 0.51950 (0.00011) | 0.51612 (0.00012) | −0.65 % |
| whole image | 1.04697 (0.00003) | 1.00471 (0.00007) | −4.04 % |

The pre-review build read the same deltas to within 0.1 % per region
(e.g. whole image −4.04 %, wax/gold −2.14 %): the review's changes do not
move this scene's means.  The reviewer's independent masks read the same
whole-image −4.04 %.

Composites DARKEN overall, even though the walk now conserves energy.
Pre-fix NEE priced the bare substrate.  It ignored the coat's Fresnel
transmission, the `1/eta^2` compression (−56 % for ior 1.5) and the gap
extinction, which is what makes the amber and blue-extinction objects
look amber and warm at all.  So the NEE correction darkens more than the
restored indirect energy brightens.  Section D2 is the reference-free
proof that the new NEE value is right.

### 3.8 Cost

**Shipped scene, whole-render user CPU**, n = 5 interleaved, 512×288,
64 spp.  After review round 2 and the merge, against `master` `1e20e1f6`
(two separately built binaries, interleaved): **66.05 ± 1.07 s -> 80.28 ±
1.06 s, +21.5 %** (the round-3 reviewer re-measured +23.6 %: quote **+21-24 %**); the region means match section 3.7's deltas to within
0.2 % per region (whole image −4.04 %).  Earlier builds, against base
`5c9eeb96`:

* before the review: base 66.80 ± 0.50 s, fix 90.47 ± 1.85 s — **+35.4 %**
  (the reviewer re-measured +40.7 %, 72.91 ± 3.32 → 102.56 ± 3.20 s; the
  two agree within noise, so quote **+35..41 %** for that build).
* after the review (walk recorded once per shading point, section 7
  P2-3): base 67.23 ± 1.25 s, fix 82.96 ± 4.20 s — **+23.4 %**.

The scene is composite-dominated: 6 of its 8 objects are composites.  A
scene with one composite pays proportionally less.

**Per-call microbenchmark** (coat over Lambertian):

* `Scatter`: 1.5–1.8 µs
* `Pdf`, uncached: ~0.57 µs
* `value`: 0.9–1.1 µs
* double composite: 9.5 / 3.1 / 6.3 µs

Before the review the render cost was dominated by the evaluator walk
that each NEE light sample ran (the scene has 3 lights).  Three
optimisations are now in place: the probe cache, one reused layer record
per walk, and (since the review) the walk itself, drawn once per
(incoming direction, position) and shared by the emitted ray, every NEE
sample and every connection at that point.  What remains is one top
scatter per exit query (term (a)'s transmission), the BSDF evaluations
along the recorded walk, and the mixture `Pdf`.

## 4. Gates (this slice's final state)

On the merged tree (`master` `1e20e1f6` + this branch, after review round
2).  The clean library rebuild and every test target built had **0
warnings**.

| suite | post-fix | base (red) |
|---|---|---|
| CompositeEnergyConservationTest (new) | 168/0 | the 128-check revision reads 37/91 on `5c9eeb96` (round-2 reviewer's measurement; round 1's 34/82 was the 116-check file); 121/7 on `c03807a2` for the round-1 review rows (E2 2, H1 1, H2 2, H3 pin 2); section T 29/9 with the round-1 `DielectricSPF` claim restored (section 3.6) |
| LayeredWhiteFurnaceTest | 0 of 60 failed | 3 of 60 |
| CompositeExtinctionTest | all pass | 5 FAIL |
| SPFPdfConsistencyTest | all pass | 12 FAILED |
| HWSSCompanionKrayTest | 193/0 | 190/3 |
| TranslucentLobeConsistencyTest | 1226/0 | 1222/4 (gate 6 re-ruled) |
| BDPTStrategyBalanceTest (full) | 227/0 (includes DL-05's Q/R: `--weave-gap-only` 24/0; S: `--materials-only` 19/0) | topology S passes on base too |
| WeaveGapShadowTransmittanceTest (DL-05) | 132/0 | 132/0 on master |
| PTGuidingMISPartitionTest | 185/0 | — |
| BDPTGuidedContinuationTest | 164/0 | — |
| SPFBSDFConsistencyTest | pass | — |
| VCMStrategyBalanceTest | 74/0 | — |
| CstDeriveGoldenTest | 456 MATCH, 0 DRIFT | — |
| SourceHygieneTest | 167/0 | — |
| SSSRadianceScalingTest | 576220/0 | (base 576220/0) |
| SSSExteriorIndexInvarianceTest (DL-291) | 227/0 | 227/0 on master |
| RefractiveRadianceScalingTest | 40/1 | row C, the known pre-existing regression (DL-308); master 40/1 |
| ConnectionLegality / AgentMakeFabric / ReferenceGraph / SceneEditTransaction / LightBVH | 319/0, 273/0, 375/0, 274/0, 20/0 | — |

## 5. Residuals

* **DL-296 — FIXED 2026-10-09 for a bottom that transmits through its own
  BSDF (section 10); the rest is DL-472.  Historical record: WALKER-class
  transport is invisible to NEE and to connections.**  This covers transmission out through a transmitting
  bottom, entries from below, null-BSDF layers and all-delta chains.  All
  of it is delta-tagged by design, so the MIS partition is exact.  The
  cost: a delta light (point/spot/directional) contributes nothing through
  it, and an area light reaches it only via BSDF-sampled hits.
  `ScattersFullSphere` is false.  The recipe is in the ledger row.
* **DL-297 — FIXED 2026-10-02, section 9 (Phong warps; HG and per-channel
  RGB warps are DL-406).  Historical record: term (a) treated the top's
  delta-tagged transmission as ideal Snell.**  A `DielectricSPF` with finite `scattering` warps its
  transmitted direction (Phong `cos^N` about the Snell axis) while still
  tagging it delta.  The evaluator connects through the ideal direction.
  Energy is exact, but the warp's angular blur is lost.  Energy-weighted
  exit cos-histograms at theta 0:

  | top | bins |
  |---|---|
  | scat 0, evaluator | 0.013 / 0.064 / 0.128 / 0.193 / 0.251 / 0.349 |
  | scat 0, walker-only (follows the warp) | 0.123 / 0.136 / 0.150 / 0.167 / 0.185 / 0.239 |
  | scat 1e4 (the parser default) | evaluator and walker agree within noise |

* **DL-341 (filed at merge by the supervisor) -- FIXED 2026-10-02, section
  9; its D3 "truth 1" was wrong (the derived truth is the equivalent pair
  of separate sheets, 0.467).  Historical record: the composite's IOR-stack
  gap family.**  Three presentations, one cause: the two-stack convention
  and the walk's exits are only right for a walk that enters from ABOVE
  and leaves through the TOP.  (i) **Nested composites walked FROM
  BELOW.**  The two-stack convention
  (`EvalStack`: a down-going ray sees `outside`, an up-going one `gap`) is
  defined for walks entered from ABOVE.  A walk entered from below is
  handed the medium BELOW the stack as `outside`, and the from-below loop
  never refreshes `gap` after a bottom-layer event.  With two dielectric
  layers sharing one object key (a composite{dielectric/dielectric}) and a
  stack that already holds that key, each layer then reads the other's
  side: the ray is total-internally-reflected between them without end,
  and the energy is dropped at the 256-event cap (whose warning now says
  so).  Reached two ways: a composite{dielectric/dielectric} used as
  another composite's TOP (H3: 0.48 / 0.51 at 0 / 60 deg, truth 1), and a
  transmitting composite{dielectric/dielectric} on a closed object seen
  from inside (H4: 0.087 at 35 deg; 1.0 below ~30 deg, 1.0 by genuine TIR
  above ~42 deg).  Pre-existing: the base reads the same 0.086 for H4 and
  0.236 / 0.275 for H3.  Both are pinned.  A contained fix was not found:
  updating `gap` on the upward bottom crossing alone stops the endless
  loop but leaves the down-going return reading the wrong side; the real
  fix is a from-below stack convention for the whole walk.  The evaluator
  inherits the same fault for H3 (its term (a) applies the 1/eta^2 of a
  refraction the nested top never performs).  (ii) **A TRANSMITTING
  composite seen from above.**  composite{glass / glass} with zero gap on
  an open quad under a white env furnace reads **0.487** under PT and
  BDPT pel (the round-2 reviewer: 0.486 across pel, spectral and HWSS;
  base 0.484-0.487), truth 1: the walker's exit through the BOTTOM
  carries the inside-the-object stack, so the escaping ray is priced at
  the `1/eta^2 = 0.444` basic-radiance factor of a medium it never
  entered.  A plain `dielectric_material` open quad reads 0.467 =
  `F + (1 - F)/eta^2` in the same frame -- the same half-space convention
  -- so what the composite lacks is a way for a bottom exit to pop back
  to the outside stack.  Pinned (section D3, [0.43, 0.54]).  (iii) **A ray
  arriving BEHIND a tilted shading normal** (tilt 35, theta 60:
  `d . n_s = +0.087`) is classified up-going and takes the from-below
  walker: 0.0899 on this branch, 0.0907 on base.  It is a property of the
  composite's classification, not of the top: section T reads 0.0899 /
  0.0900 / 0.0904 / 0.0903 for dielectric scattering 0 / 5 / warp-off and
  a lossless translucent top alike.  Pinned (section T, [0.06, 0.12]).
* **Probe/walk cache key** holds the IOR stack's top and
  `containsCurrent()`, not its deeper entries.  Two queries at the same
  point, direction and top but a different deeper stack would share a
  probe.  Not reachable from any walk in the tree today (a composite's
  sub-SPFs read only the top and the object membership); recorded.
* **Dispersive tops, 5-argument `EvaluateKrayNM`**: returns the hero's
  weight (ratio 1) where `F(nm)/F(hero)` is right (section 3.5), part of
  DL-221.  **Delta-coated bottoms under HWSS** are measured there too
  (composite{glass / polished-red}: `hwss TRUE` -9.7 % PT, -8.5 % BDPT).
* **Pre-existing, not this row: BDPT `hwss TRUE` reads ~-9.5 % against
  `hwss FALSE` on COVERED composites, on base and branch alike** (round-2
  reviewer; the figure was reported for BDPT only).  Because it is present
  on the base as well, it is not caused by this slice; its attribution
  was NOT investigated here.  Recorded for the supervisor.
* **PER-BRANCH variance.**  Per-branch mode is unbiased for any positive
  weights, but its variance can be large: the round-2 reviewer measured a
  per-draw sd about 30x the mean on a three-level nested composite.  The
  8-probe weights (section 8) reduce the worst of it (a floored walker
  share weighting rare exits ~100x); they do not remove it.
* **DL-342 (filed at merge) -- `coated_material` too bright under an
  ABSORBING coat** (~15-17 %; its recycling approximation), section 7.  FIXED 2026-10-02 ([DL342_COATED_ABSORBING_COAT.md](DL342_COATED_ABSORBING_COAT.md)):
  coated now matches Section G's closed form; a GGX substrate exposes DL-388.
* **DL-285** (polished bottom, now PINNED in [1.05, 1.15]) and **config 7 /
  F6** (reflection-only GGX top) are unchanged.  So is the
  RefractiveRadianceScalingTest row C regression (DL-308).
* **Premise note for DL-67.**  A composite now satisfies DL-67's premise
  2: its `IBSDF::value` and its kray describe one function for every
  non-delta ray.  Its WALKER rays are delta-tagged, and the guided
  continuation prices those at `W_b = 1`.

## 6. `add_wetness`

(Section 7's cost figure does not change this ruling.)

The route-around in WETNESS_COAT_DESIGN.md §4(a) / §12 item 2 is no
longer *necessary*: coat-over-diffuse now conserves energy, NEE sees the
layered response, and `Pdf` is exact.  It remains the right choice, and
`add_wetness` should **not** switch back.  `coated_material` is
closed-form, costs no MC evaluator walk per NEE sample, has lower
variance, and has its own furnace/HWSS guards.  `composite_material`
also loses a finite-`scattering` coat's blur (DL-297).
`composite_material` is now a valid general two-layer stack; wetness is
the coat case `coated_material` was built for.

## 7. Review round 1 (2026-09-28): FAIL, 2 P1, 3 P2 -- all addressed

An independent reviewer reproduced the diagnosis (exit + dropped =
1.0000; 0.4277 + 0.5723 at normal incidence against an analytic 0.5725),
every claimed red/green, the appearance deltas and the pointwise
`kray * Pdf == value * cos`, and found:

* **P1-1 -- the "is the top deterministic?" probe was itself stochastic.**
  It compared two hashed draws of the top's up/down selection mass (one
  draw with itself when the top had no BSDF).  For a SINGLE-EMIT
  stochastic top -- a nested composite (which emits at most one ray),
  `generic_human_tissue`, a thin weave -- one draw's mass is exactly 0 or
  1, the draws agree at least half the time, and AGGREGATE mode then gave
  the DIRECT branch or every down branch probability zero at that shading
  point.  Every furnace in the suite used one fixed intersection, so one
  hashed probe, and could not see it.  **Fix:** determinism is DECLARED,
  never inferred -- new `ISPF::SelectionMassIsDeterministic()` (default
  false, documented contract), true only for `DielectricSPF`,
  `PerfectReflectorSPF`, `PerfectRefractorSPF` and `TranslucentSPF`
  (checked: none drops a lobe at random or carries a direction-dependent
  realised weight).  Every other top runs PER-BRANCH mode, unbiased for
  any positive floored weights; `Pdf` is then an MIS partner, not the
  exact density, and the companion-weight methods decline there.
  Lambertian is deliberately NOT declared: under a tilted shading normal
  its horizon gate drops the lobe at random.  Red -> green (section H,
  position-jittered): tissue/white at 60 deg 0.6212 -> 0.9939; nested
  composite{glass/lossless translucent}/white 0.9418 / 0.9351 -> 1.0030 /
  0.9969.
* **P1-2 -- the 5-argument `EvaluateKrayNM` priced WALKER rays as direct
  reflections.**  A composite is a parallel slab, so an all-delta walker
  exit leaves exactly along the mirror direction and the direction match
  accepted it.  **Fix:** a walker ray leaves the top from INSIDE the
  stack, travelling upward, and a reflection returns a ray to the side it
  came from, so an up-going ray out of the top from inside is always a
  TRANSMISSION; the DIRECT delta ray is the only up-going
  `eRayReflection` a composite emits.  The method now reconstructs only
  that type, matched to a reflection-typed top lobe, and only in
  AGGREGATE mode (per-branch prices the direct ray with a floored `w1`).
  Red -> green (section E2, grey glass(1e6)/glass, t 1, ext 1): 15 709
  up-going, 15 709 "reconstructed", 7 366 wrong, worst relative error
  7.34 -> 8 343 reconstructed (0 wrong), 7 366 walker exits declined.
* **P2-1 -- composite{glass/glass} over white loses ~50 % and the cap
  warning called it "lossless trapping".**  Mechanism found and recorded
  (section 5, "Nested composites walked FROM BELOW"); not contained, so
  pinned (H3, H4) and the warning text corrected.
* **P2-2 -- `value` is a noisy per-query Monte-Carlo estimate, and the
  docs said "DETERMINISTIC" without saying so.**  Section 2 and 3.2 now
  state it: per-evaluation relative sd ~0.80 (smooth coat over white),
  ~0.39 (red, absorbing gap), printed by section G.  The MIS partition is
  unaffected (only the exact `Pdf` enters the weights).  Since P2-3 one
  walk is shared by all exits at a point, so a repeated query at the same
  (incoming direction, position) returns one realisation: the estimate is
  unbiased averaged over positions, which every renderer provides (every
  pixel sample hits a new point) and which the composite furnaces now do
  explicitly.
* **P2-3 -- cost +40.7 %, not +35.4 %** (consistent within noise).  The
  evaluator's walk is now recorded once per (incoming direction,
  position) and only the connection terms are recomputed per exit:
  **+23.4 %** (67.23 ± 1.25 -> 82.96 ± 4.20 s user, n = 5 interleaved).
  The emitted ray, every NEE sample and every connection at a point share
  one walk -- correlated, each still unbiased.  A thread-local depth guard
  keeps a nested composite's own evaluation out of the cache while the
  outer one holds an entry.
* **P3s:** section 3.2's "within 1 sd" corrected (1.55 / 1.47 sem;
  now max 2.6 sem at 24 000 samples, inside the 4-sem gate); topology Q renamed S with its false
  "could not agree before" comment corrected; F2b (DL-285) pinned in
  [1.05, 1.15]; the dispersive direct-reflection "exactly" corrected
  (section 3.5); the probe-cache key residual recorded (section 5).
* **Pre-existing, reported for the supervisor (not this row):** the
  reviewer measured `coated_material` ~15 % too bright under an ABSORBING
  coat -- its recycling approximation (`kRecycleMeanCos` 0.5) gives
  E_ret ~0.366 against an exact 0.258 at sigma_t 0.2; on the shipped
  scene's warm-white ellipsoid composite/coated reads R 0.852, G 0.879
  (analytic 0.855).  The composite matches the independent closed form
  (section G).


## 8. Review round 2 (2026-09-28): FAIL, 1 P1, 2 P2, P3s -- all addressed

**Merge first.**  `master` moved to `1e20e1f6` (DL-05, DL-306, DL-291) and
was merged into this branch (no rebase).  One semantic conflict: DL-05's
new `CompositeSPF::DeltaPassThroughTransmittance{,NM}` (the straight-through
transmittance a shadow ray of a delta light takes through a composite)
gated its `layer x Beer x layer` product on `max_recur` and on the
`ShouldScatteredRayBePropagated` budget this slice had removed.  The
product is kept; the gates were re-derived for the new budget model and
removed: since DL-24 the budgets are Russian-roulette ONSETS with the
survival compensated, and the walk's only hard stop is the 256-event cap,
which a two-event straight path never reaches, so no budget changes the
EXPECTED straight-through weight (the reasoning is in the comment above
the function).  The old `NEARZERO` floor on the attenuation went with them
(the walker stops only at an exactly zero throughput).

Re-running DL-05's `WeaveGapShadowTransmittanceTest` on the merged tree
then read its composite-of-weaves BDPT row **-25 %** (0.06737 against an
expected 0.09).  Not a bias in the weights -- per-branch mode is unbiased
for any positive weights -- but a heavy tail: a single-emit top (a weave)
whose true down share is ~0.3 read "no down mass" on BOTH of the two
hashed probes at 49 % of shading points, which floored the walker's share
to 1 % and weighted its rare straight-through exits ~100x, and a BDPT
light-tracing splat did not converge within 1024 spp.  The per-branch
weights now average `kPerBranchProbes` = 8 hashed probes (the declared
aggregate path still uses one): the zero-read rate falls to 5.8 %, the
batch sem from 0.0041 to 0.0013, and the row passes (132/0).  An
efficiency choice, not a correctness one: the round-3 reviewer confirmed
the per-branch mean does not move with 2, 8 or 32 probes.  **That row's
pass is on a FROZEN Sobol pattern**: unsalted it reads 0.09116 / 0.09115
whatever the libc seed, while SALTED renders give **0.0901 ± 0.0012**
(n = 16) with a single-render sd of 5.1 % against the row's ±3 % band, so
about half of single salted renders would fail.  DL-355 (filed at merge):
the row needs 8+ salted renders averaged, or more spp.

**Cost cliff of NESTED composites (known limitation, disclosed by the
round-3 reviewer).**  Every per-branch level scatters its top 8 times to
probe it, and the 4-entry probe cache thrashes beyond a nesting depth of
about 4, so `Scatter` cost grows EXPONENTIALLY with depth -- the 8-probe
change raised the growth base from ~2 to ~8.  Microseconds per `Scatter`,
weave layers nested as the TOP: depth 1 / 5 / 6 / 7 / 8 = 3.8 / 32 / 105 /
816 / 9195; nested as the BOTTOM: 3.8 / 42 / 66 / 110 / 202; `master` is
flat at 0.65-1.24.  The shipped depth-2 nest costs ~12x per `Scatter`
against `master`; at depth 128 neither form finishes.  Not fixed here (a
cache-size bump was not tried).

**Extreme tail of the per-branch walker share.**  The walker share floors
at ~0.001 whenever no probe sees down mass.  For a nested top whose own
down share is ~1 %, 91 % of shading points are in that state, so
straight-through events occur about once per 10^6 draws with weights up
to 5939.  The estimator is unbiased, but transport through such a sheet
that NEE cannot price (DL-296) will firefly.

**P1-A -- `DielectricSPF` falsely declared determinism under a tilted
shading normal with finite `scattering`.**  Its transmission warp is
clipped to the GEOMETRIC side of the surface, so when the shading normal
is tilted the wedge between the two planes moves mass across the shading
plane at random: the per-draw up/down split is not a function of the
record, and the aggregate mode priced the branches with a split no draw
realises.  Three options were offered; **(a), a query-dependent
capability, was chosen**:

* `ISPF::SelectionMassIsDeterministic( ri, nm )` now takes the record and
  wavelength.  `PerfectReflectorSPF`, `PerfectRefractorSPF` and
  `TranslucentSPF` return true unconditionally (their claim was re-checked
  and kept: no warp, lobes determined by the record).  `DielectricSPF`
  returns true only when its warp is OFF at this record and wavelength
  (non-HG `scattering >= 1e6`, HG `g >= 1`; any channel for RGB) or its
  shading normal equals its geometric normal (`1 - |n_g . n_s| < 1e-12`,
  i.e. a residual wedge of at most ~1.4e-6 rad; a degenerate geometric
  normal makes the SPF clip against the shading normal itself, so no wedge
  exists).  The tilt test is exact rather than a tolerance because both
  warps (Phong `cos^N`, Henyey-Greenstein) have support over the whole
  hemisphere: ANY tilt makes a nonzero wedge.
* Why not (c) ("false unless `scattering >= 1e6`"): the parser default is
  10 000, so (c) would have moved every default-glass composite -- on flat
  and analytic geometry too, where the split is exactly deterministic --
  to the per-branch mode, losing the exact `Pdf` and the HWSS
  reconstruction for no correctness gain.  Why not (b) (classify up/down
  against the geometric plane): the walker, the evaluator's connections
  and the reflection lobe are all defined in the SHADING frame; moving
  the classification would be a redesign of every branch, and it would not
  remove the underlying fact that the sub-SPF's own split is stochastic.
* Consequence worth knowing: on a smooth-shaded mesh or a bump-/normal-
  mapped surface, a composite whose top is a dielectric with a finite
  `scattering` (including the default 10 000) runs PER-BRANCH -- unbiased,
  but measurably costlier and noisier: the round-3 reviewer measured
  **PT +50 % and BDPT +62 % user CPU** on a bump-mapped composite against
  the aggregate mode, with a per-draw sd ~2.5x the natural walk's
  (PT/BDPT/`coated_material` still agree to 0.2 %); `Pdf` is an MIS
  partner and the 5-argument `EvaluateKrayNM` declines.  The shipped scene is all analytic primitives and does not
  move (section 3.8: every region within 0.2 % of round 1's deltas).

Red -> green: section 3.6's T table (red 29/9 with the round-1 claim
restored on this tree, via a committed-state mutation of the one function;
green 38/0), including a warp-OFF tilted dielectric and a lossless translucent top,
both of which keep their declarations and match the independent walk in
AGGREGATE mode under every tilt, and T2 (296 wrong
companion reconstructions -> 0).  The untilted rows are identical in both
builds.

**P2-1 -- a transmitting composite reads ~0.49 in a white env furnace.**
Recorded, not fixed: the bottom exit carries the inside-the-object stack
(`1/eta^2 = 0.444`).  Filed with the from-below case as DL-341 (section 5
(ii)), pinned by the new render row D3 (PT and BDPT pel 0.487, band
[0.43, 0.54]).

**P2-2 -- DL-221 quantified** (section 3.5 and the DL-221 row):
glass/polished-red `hwss TRUE` -9.7 % PT, -8.5 % BDPT, 3 768 fallback-priced
walker rays per 100 000 up-going.

**P3s.**  BDPT HWSS ~-9.5 % on covered composites is pre-existing (base and
branch) and recorded in section 5; the behind-the-tilted-normal from-below
case (0.0899 / base 0.0907) is DL-341 (iii) and pinned; the stale "seeded
from (wi, wo, position)" comments in `CompositeSPF.h` and the `.cpp`
hashing block now say the walk is seeded from (wi, position) and term (a)
from (wi, wo, position); the `kMaxWalkEvents` comment no longer says only
a lossless trap reaches the cap (91 323 of 100 000 H4 walks do); the
per-branch variance note is in section 5; the dispersive direct-reflection
reconstruction is described as returning 1 where `F(nm)/F(hero)` is right,
not "exactly"; and the red-proof figure is 37/91 for the 128-check file
(the reviewer's measurement; round 1's 34/82 was the 116-check revision).

**Round 3 (2026-09-28): PASS, no P1.**  The reviewer confirmed P1-A closed
(a per-record audit found 0 violations across 10 materials x tilts x
stacks x angles x pipes), the DL-05 reconciliation matching the walker in
expectation on 9 configurations, and the 8-probe estimator unbiased.  Its
P2s are disclosed above (the nested-composite cost cliff, the frozen-Sobol
WeaveGap row -> DL-355, the per-branch tail, the per-branch cost).  Two
more recorded residuals: (1) **hero/companion mode mismatch, unguarded
and contrived** -- if a spectrally varying `scattering` painter crosses
1e6 (or an HG `g` crosses 1) between the hero and a companion wavelength,
the hero ray is priced per-branch while the 5-argument `EvaluateKrayNM`,
queried at the companion, reconstructs an aggregate-mode weight; (2) on
the shipped scene the BDPT/PT ratio sits 0.55-0.59 % low, and 0.24 % low
on a composite-free version of it -- unattributed.  The merge commit
`3e860735` also carries the P1-A fix and the 8-probe change, so a bisect
landing on it tests three changes at once.

## 9. DL-341 + DL-297 (slice `debt-composite`, 2026-10-02)

Branched from `master` `74236b3cb`.  Both rows fixed, left unstruck for a
fresh review; DL-406 and DL-407 opened (residuals, below).

### 9.1 The stack convention (DL-341)

Derived from what the EQUIVALENT PAIR OF SEPARATE SURFACES does: two
coincident interfaces with the same orientation (both fronts on the
shading-normal side), the top's material above the bottom's.  Under RISE's
stack rules -- a closed solid, or two open sheets under DL-345's face rule,
which read the same -- a ray that crosses both from above is INSIDE both,
in the medium the bottom defines; one that leaves upward through both is
back outside.  For an object `O`:

| state | stack | where |
|---|---|---|
| OUT | entry stack with `O` popped (when it held it) | above the top |
| GAP | OUT + what the top pushes crossing down (`O` at the gap's index; nothing for a top that does not push) | between the layers |
| BELOW | GAP + what the bottom pushes crossing down | under the bottom |

The shared `IObject*` key was what made one threaded stack unworkable (the
top's push read as the bottom's).  The walk now keys the BOTTOM layer with
its own per-instance key (`BottomKey`, the address of the composite's
`instanceId`, compared and never dereferenced) and keeps `O` for the top, so
ONE internal stack is threaded and refreshed after every crossing in either
direction.  Each layer then reads "inside" exactly when the ray is behind
it, and refracts from the index of the medium the ray is actually in: the
bottom now refracts from the GAP's index (the old two-stack walk's "scope gap
(b)": glass/glass refracted 1.0 -> 1.5 twice).

Per entry side, decided by the TRUE geometric facing, with the shading
normal oriented into the true geometric normal's hemisphere.  A record a
double-sided geometry flipped toward the ray is unflipped first exactly when
the walk's IOR stack already holds the composite's object (the ray crossed
in earlier); otherwise -- an open sheet's back face with no prior crossing, a
provably open clipped plane, a stackless caller -- it keeps the flipped frame
and presents the top (sections 9.4, 9.4a, 9.4b):

* from above and against the shading normal -- the DIRECT / COVERED / WALKER
  mixture, starting at OUT (unchanged except for the stacks);
* from above but BEHIND a tilted shading normal -- a natural walk from the
  TOP, every exit delta-tagged (`value` and `Pdf` are 0 there);
* from below -- a natural walk from the BOTTOM, starting at BELOW: OUT, the
  GAP entry the top would push (read off one hashed from-above Scatter of the
  top, `GapStackForBelow`), and the bottom's key at the entry stack's top.

Both exits carry the EXTERNAL form (`ToExternal`): OUT through the top, OUT
plus `O` at the BELOW index through the bottom.  `DeltaPassThroughTransmittance`
picks its first layer by the same geometric side (DL-05 lockstep).

**D3's "truth 1" was wrong.**  A transmitting composite{glass/glass} on an
open quad is, by this derivation, the pair of separate glass sheets: the
crossed ray is inside the glass, exactly as below one open
`dielectric_material` sheet (DL-345), so a white env furnace reads
`F + (1 - F) / eta^2` = 0.467 at normal incidence.  A white environment of
radiance 1 seen inside a medium of index 1.5 is not an equilibrium (that
would be `n^2` = 2.25).  D3 now renders the literal separate pair beside the
composite and gates the ratio.  This equivalence holds seen from ABOVE only:
an open composite sheet seen from below still presents its top (DL-407).

**One consequence the fix exposed.**  Term (a) connects a substrate to the
exit by inverting Snell's law at the gap/outside index ratio.  A NESTED
composite used as a top emits delta-tagged exits that are whole walks, not
refractions; the old wrong gap index (1.0) happened to make eta = 1 and hid
it (energy right, shape wrong), the corrected one (1.5) confined the
connection to the critical cone and lost 45 % (H2 0.546).  New
`ISPF::DeltaTransmissionIsRefraction()` (default true; `CompositeSPF`
false): such a top has no term (a), and the walker carries that class --
unbiased, but invisible to NEE (the DL-296 family).

### 9.2 The warped coat (DL-297)

New `ISPF::DeltaTransmissionWarpExponent` / `DeltaTransmissionWarpPdf`
(`DielectricSPF`: the Phong `scattering` warp; the density reuses
`GenerateScatteredRay`'s own axis, DL-111 re-derivation included, and
`PerturbClipped`'s arc).  Term (a) for a warped top draws the outside
direction `t` from the ADJOINT warp (the same `cos^N` lobe about the query
direction, clipped to the exit hemisphere; plus a 10 % cosine share when the
shading normal is tilted, where the forward axis can be re-derived off `t`),
inverts Snell to `u`, and weights the ideal term by
`q(w | u) cos t / (p(t) cos w)`.  The draw comes from the (wi, wo, position)
stream, so `value` stays a deterministic function and
`kray * Pdf == value * cos` holds (section C green).  A Henyey-Greenstein
warp (its draws past 90 deg stay on the axis: a delta part with no density)
and a per-channel / dispersive RGB top have no single-density form and stay
ideal: **DL-406**.

### 9.3 Evidence

`CompositeEnergyConservationTest` **302/0** at round 0; the same file against
the base library (`74236b3cb`, the five library files checked out over a WIP
commit) **271/31** as first measured, **269/33** with D4's tightened band (the
reviewer's count; see the D4 note below):

| row | base | fixed |
|---|---|---|
| H3 composite{glass/glass} as a top, walked from below (truth 1) | 0.4795 / 0.5064 | 0.9926 / 1.0144 (sem 0.007) |
| H4 struck from inside, 20 / 35 / 60 deg (truth 1) | 1.0000 / **0.0868** / -- | 1.0000 / 1.0000 / 1.0000 |
| T behind a 35-deg tilt, theta 60, four tops vs the independent walk | 0.090 vs 0.80-0.94 (z -584 .. -1075) | \|z\| <= 1.67 |
| W exit histogram, scattering 0 and 5, theta 0 and 45 (24 bins) | z -241 .. +91 | \|z\| <= 2.02; the scattering-10000 control green in both |
| D3 composite / separate glass pair, PT and BDPT | 1.0438 | 0.99974 |
| D4 closed composite box / glass box, PT and BDPT | 0.98089 / 0.98063 | 1.00000 / 1.00000 |

(D4's band was tightened to 0.5 % after the base run -- both boxes are a
zero-variance lossless delta furnace post-fix; base sits outside it.)
`CompositeExtinctionTest` section 6b gated the dielectric(1.5)/dielectric
(1.33) stack's crossed energy at ">10 % over the bare top": that threshold
was the old 1.0 -> 1.33 bottom interface (0.0185); the gap -> bottom
interface is 1.5 -> 1.33 and the check now gates the closed form
`T F_b T / (1 - F_b F_t)` = 0.00333 (measured 0.00347, +-25 %).  E2's bottom
became an ior-2.4 glass: a 1.5 bottom is now index-matched and reflects
nothing back up.

Gates: LayeredWhiteFurnaceTest 0 of 63 failed; TranslucentLobeConsistencyTest
1226/0; OpenSheetIndexConventionTest 24/0; TransmissionPushGateTest 416/0;
SPFBSDFConsistencyTest pass; SPFPdfConsistencyTest pass; HWSSCompanionKrayTest
193/0; SourceHygieneTest 167/0; CstDeriveGoldenTest 457 MATCH / 0 DRIFT;
CompositeExtinctionTest all pass; WeaveGapShadowTransmittanceTest `query`
75/0 and `composite` 2/0; BDPTStrategyBalanceTest `--materials-only` 26/0.
DL-345's open-sheet rule is untouched for composites: every layer call
still sees the record without `bProvablyNoInterior` (D1-D3 are composites on
clipped planes).

**Shipped scene** (`composite_material.RISEscene` at 512 x 288, 64 spp, OIDN
off, linear, n = 3 salted renders per binary, interleaved, twice): every
region within noise except the clearcoat red sphere, +0.08 % / +0.105 %
(t 2.3 / 2.6) -- the default `scattering 10000` warp now reaches term (a);
whole image +0.002 % / -0.005 %.  No shipped scene has a from-below,
behind-normal or transmitting composite.  **Cost** +7.7 % user CPU on that
composite-dominated scene (base 96.7, fixed 104.2 s; paired differences 7.6 /
8.9 / 6.0 s), all from the warped term (a) the default glass now takes.

### 9.4 Review round 1 (2026-10-02): FAIL, 1 P1, 2 P2, 1 P3 -- addressed

* **P1 -- closed DOUBLE-SIDED meshes rendered 53 % dark.**  A double-sided
  mesh flips both normals toward the ray, so a hit from INSIDE presented the
  composite's top and was walked as an entry from above; round 0's OUT then
  popped O, the dielectric top refracted 1.0 -> 1.5 instead of 1.5 -> 1.0 and
  the exit claimed to be still inside: an `indexedmesh_geometry` box
  (`double_sided` defaults TRUE) of composite{glass/glass} read **0.46687**
  in the white furnace (base 1.00000; the same mesh single-sided 1.0000).
  Round 0's DL-407 claim that this was pre-existing was WRONG: the base
  handed the top the unpopped stack, and the top's own from-inside branch
  was right.  Fixed twice over: (1) `CompositeLayerFrame` UNFLIPS a flipped
  record (`bGeomNormalOrientedToRay`, a true side, not a provably open sheet):
  the geometric normal goes back to the true outward one (DL-70
  `UnflippedGeomNormal()`) and the shading normal and frame are oriented into
  its hemisphere, so the entry side is the TRUE facing and the walk sees the
  solid exactly as a single-sided mesh; (2) a walk entered from above never
  pops O (`OutRef` returns the entry stack): an above entry whose stack holds
  O is an inconsistent state and is trusted as the base trusted it.  Only a
  from-below walk pops.  A provably open sheet (a clipped plane) keeps the
  flipped frame and presents its top on both faces, as before.  New render
  row **D5** (that mesh box, composite and plain-glass control, PT and
  BDPT): round-0 library 0.46687 / 0.46687, now 1.00000 / 1.00000.
* **P2 -- nested composite{composite{glass/glass}/glass} box read 1.0056.**
  Not reproduced.  New row **D6** renders it beside a glass box at 2048 spp,
  n = 4 SALTED renders per integrator (independent randomized-QMC
  replicates), gated at max(0.2 %, 4 sem) with sem < 0.125 % enforced, so it
  resolves 0.5 %: the round-0 library reads PT 1.00045 +- 0.00056, BDPT
  0.99994 +- 0.00104; round 1 PT 1.00071 +- 0.00072, BDPT 0.99900 +- 0.00016
  (the glass control 1.00004).  At the reviewer's 128 spp the per-render sd
  is 0.0065 here, so a 0.5 % reading there is within one render's noise.
  The SPF-level twin **H5** (full-sphere furnace from above and from inside,
  and the eta^2-weighted sum against one plain glass interface, from above
  and inside at 20 / 35 / 60 deg) is green.
* **P2 -- "renders like the separate pair" holds FRONT-side only.**  Seen
  from BELOW, an open composite sheet still presents its top (a clipped
  plane is provably open, and the composite does not apply DL-345's face
  rule to itself): composite 0.467 against the separate pair's 4.52 (the
  reviewer's measurement).  That is a real defect of the DL-345 class (a
  light walk entering from above and an eye walk from below refract
  differently at the same sheet), filed in DL-407.
* **P3 -- `GapStackForBelow`.**  Every multi-emit top (dielectric, perfect
  refractor, translucent) shows its transmission on the FIRST hashed draw at
  normal incidence and a top that declares a deterministic split stops
  there, so the result is a deterministic function of the record; only a
  single-emit stochastic top (nested composite, tissue) is sampled, now up to
  16 draws (a nested glass/glass top misses with ~1e-17).  The base-run count
  above is corrected (269/33).

**Correction to the D6 entry above (review round 2).**  There was no BDPT
offset to explain: the round-2 reviewer's n = 8 salted renders at 2048 spp
read PT 1.00065 +- 0.00068 and BDPT 1.00038 +- 0.00056, and the sd of one
salted render at 128 spp is ~0.011 (not the 0.0065 quoted above), so the
round-1 report of 1.0056 was an unsalted 128-spp artifact.

### 9.4a Review round 2 (2026-10-02): FAIL, 1 P1 -- addressed

* **P1 -- round 1's unflip broke OPEN double-sided composite sheets.**  The
  unflip was keyed on "not provably open" only, so an `indexedmesh_geometry`
  quad (double-sided by default, NOT certified watertight: `bOpenSheet`) or a
  Bezier patch set hit from behind was unflipped and walked from below,
  delta-tagged: an omni light on the camera side no longer lit it (PT 1.253
  -> 0), and BDPT / VCM disagreed with PT (coat over a 0.8 Lambertian, white
  env furnace, back view: base 0.632 / 0.635 / 0.634 -> 0.800 / 0.909 /
  1.010; glass/glass 0.487 -> 0.977) because PathVertexEval::
  PopulateRIGFromVertex rebuilt the connection record WITHOUT
  `bGeomNormalOrientedToRay`, so the composite repriced it in the flipped
  frame its Scatter had unflipped -- the DL-100 frame trap.  Fixed twice
  over: (1) only a CLOSED solid is unflipped (`!bOpenSheet` added: an open
  sheet presents its top on both faces, as the base did -- SUPERSEDED in
  round 3, section 9.4b: `bOpenSheet` is not a closedness test); (2) `BDPTVertex`
  now mirrors the four surface-identity flags (`bGeomNormalOrientedToRay`,
  `bGeomNormalRayDerived`, `bOpenSheet`, `bProvablyNoInterior`) and
  `PopulateRIGFromVertex` replays them, so a closed double-sided vertex is
  repriced in the frame Scatter used (`tests/BDPTVertexRIGRebuildTest.cpp`
  one-hot passes: 68/20 without the replay, 88/0 with it).
* Round 1's no-pop rule ALONE does not keep D5 at 1, by derivation: without
  the unflip a hit from inside a closed double-sided mesh reads as an entry
  from above, the top (keyed O, stack holding O) takes its from-inside branch
  and its ray continues "down" in the flipped frame to the bottom, which is
  keyed by its own key and reads "entering from outside" -- it refracts and
  pushes again, so the exit claims to be inside.  The unflip is needed for
  closed solids.
* New row **D7**: an open double-sided mesh quad beside its clipped-plane
  twin, camera BEHIND, PT / BDPT / VCM, coat over a 0.8 Lambertian and
  glass/glass under the env furnace and the coat under an omni light on the
  camera side; gated mesh / plane within 3 % and BDPT / VCM against PT
  within 4 %.  Round-2 library: 7 failures (mesh 0.800 / 0.907 / 1.008
  against the plane's ~0.63; glass/glass 0.977 against 0.467; omni mesh 0);
  now all green (e.g. coat env PT / BDPT / VCM mesh 0.632 / 0.632 / 0.630,
  glass/glass 0.467 everywhere, omni PT 1.241 / BDPT 1.249).  Note that the
  back view of an open glass/glass sheet now reads 0.467 like the front (base
  0.487), the DL-341 bottom-index correction.
* **Side effect, measured:** the flag replay also fixes a translucent
  defect the round-2 reviewer found (not a composite one): a closed
  DOUBLE-SIDED `translucent_material` mesh box under BDPT read 0.563
  against 0.962 single-sided, because the rebuilt record's
  `UnflippedGeomNormal()` was the ray-facing normal.  On this build the
  same scenes read BDPT 0.971 / 0.970 (double / single-sided) and PT
  0.997 / 0.994 (double / single-sided; single unsalted renders, 64 spp,
  whose spread is ~0.5 %).  Filed as DL-412 (fixed here, pending strike).

### 9.4b Review round 3 (2026-10-02): FAIL, 2 P1, 1 P2, 1 P3 -- addressed

* **Both P1s were the unflip predicate.**  `bOpenSheet` is not a closedness
  test: on an indexed mesh it means NOT CERTIFIED watertight (DL-143 -- no
  audited real asset certifies), so a closed double-sided box with ONE
  T-junction read 0.46639 again (PT and BDPT, master 1.00000); and
  `BezierPatchGeometry` never sets it (DL-220), so a single open Bezier patch
  seen from its back was unflipped (coat 0.800 against the plane's 0.63, omni
  0, glass/glass 0.977 against 0.467).  Round 2's comment, section 9.4a and
  the DL-407 row claimed the Bezier case was covered: wrong.
* **Fix: the unflip is decided by the walk's STACK.**  A flipped record
  (`bGeomNormalOrientedToRay`, a true side, not `bProvablyNoInterior`) is
  unflipped only when the caller's IOR stack already holds the composite's
  object: the ray crossed in earlier, so it really is inside.  An open sheet
  hit on its back with no prior crossing keeps top-on-both-faces.  BDPT / VCM
  reprice on a record rebuilt with the replayed flags (round 2) against the
  stack `BuildVertexIORStack` rebuilds from the vertex's own `insideObject`
  -- the same two inputs Scatter decided from, so PT, BDPT and VCM see one
  frame.  A STACKLESS caller (`IBSDF::value` with no stack;
  `DeltaPassThroughTransmittance`, which has none) never unflips and sees the
  reported frame, as before DL-341; for the straight pass-through the layer
  order does not change `t1 * Beer * t2`.  Certification was NOT kept as an
  extra trigger: `BezierPatchGeometry` reports "certified" for an open patch.
  The unseeded camera inside a closed composite stays DL-407.
* **Rows.**  D5b: the T-junction double-sided box beside a plain-glass twin
  (PT and BDPT): round-3 library 0.46687, now 1.00000.  D7 Bezier variant:
  the single patch beside its clipped-plane twin, rendered from BOTH sides
  (its raw normal is -z, so its back face is seen from +z, where the plane
  shows its front), PT / BDPT / VCM, env coat, env glass/glass and omni coat:
  round-3 library 9 failures from +z (0.800, 0.977 and 0 as above), now
  green from both sides.  `CompositeEnergyConservationTest`
  `--stack-only --render` against the round-3 library: 150/11.
* **P2 -- DL-412's remaining BDPT shortfall is NOT depth.**  The round-3
  reviewer measured the translucent box at BDPT 0.9731 / 0.9728 / 0.9728 for
  `max_eye_depth` 12 / 32 / 64, and the single-sided box reads 0.9718 on
  master: a pre-existing, unattributed BDPT translucent shortfall (possibly
  the DL-223 family), independent of DL-412's double-sided defect, which is
  fixed.  The DL-412 row is reworded.

### 9.4c Review round 4 (2026-10-02): FAIL, 1 P1, 1 P2, P3s -- addressed

* **P1 -- a NESTED composite unflipped mid-walk.**  When the outer
  composite KEPT a flipped record (an open double-sided sheet's back face,
  an unseeded inside hit) it still handed its layers the record with
  `bGeomNormalOrientedToRay` set.  A nested composite used as the top then
  re-decided the unflip from the walk's INTERNAL stack -- which holds O from
  the inner's own earlier crossing -- and unflipped on the return trip: a
  spurious interface, and an exit stack claiming inside.
  composite{composite{glass/water}/Lambertian 0.8} on an open sheet, white
  env furnace, back view PT 0.374 / BDPT 0.367 / VCM 0.368 against the front
  0.684 (the round-4 reviewer, salted n = 3).  Fix (the reviewer's, validated
  and applied as prototyped): a flipped record kept flipped is copied with
  the flag CLEARED -- the walk's frame IS now the record's frame, so every
  layer, nested composites included, sees one consistent frame.
* **P2, fixed by the same change -- composite{translucent/Lambertian} back
  face** read 0.711 against 0.860 front (-17 %, pre-existing: master 0.715):
  the translucent layer read `UnflippedGeomNormal()` off the layer record and
  built its exit frame against the composite's.
* **Row D8:** the two nested-top composites (glass/water and glass/glass
  over a 0.8 Lambertian) on a mesh quad and its clipped-plane twin, back vs
  front, PT / BDPT / VCM, and composite{translucent/Lambertian} back vs front
  (PT, BDPT).  Gates per half: the nested rows are single 1024-spp renders
  in a 5 % band (their tops run the per-branch estimator; a 256-spp front /
  back pair differed by up to 3.1 % on noise alone, and the broken state
  reads ~-48 %), the translucent rows single 256-spp renders in a 3 % band
  (near deterministic, broken -17 %).  Red / green in section 9.3's
  measurement protocol (numbers in the DL-341 ledger row).
* **P3s (DL-407):** three more stackless / reseeded paths are recorded
  there -- the stackless CausticSpectralPhotonMap gather, PT's HWSS
  mid-path SSS lane reseed (`SeedFromPoint` cannot see composites), and a
  lost O (stack capacity drop, layers that never push), which degrades to
  top-on-both-faces.

### 9.4d Review round 5 (2026-10-02): FAIL, 1 P1, P3s -- addressed

* **P1 -- a closed double-sided mesh WOUND INWARD.**  On such a mesh an
  inside hit is a front face by winding, so the geometry does not flip the
  normal and `bGeomNormalOrientedToRay` is never set; the round-3 rule keyed
  the unflip on that flag, so the hit was walked from above while the stack
  held O and the exit carried an inside stack.  composite{glass/glass} box,
  white furnace (the reviewer, salted n = 3, 256 spp): all 12 triangles
  reversed PT / BDPT / VCM 0.4667 / 0.4667 / 0.4668, back face only 0.5065,
  front only 0.9801 (master 1.0000; plain glass 1.0000 on both builds).
  Fix (the reviewer's rule, with the arrival made explicit): unflip when the
  stack holds O AND the reported geometric normal OPPOSES THE ARRIVING RAY --
  inside a closed solid the ray reaching its boundary is leaving it, so a
  reported normal against the arrival points inward whoever oriented it.
  The flip flag stays a sufficient condition (a flipped record opposes its
  arrival by construction; this keeps exactly the round-3 set and is immune
  to a grazing rounding of the dot).  The unflipped normal is
  `-vGeomNormal`.
* **The caveat (the reviewer's): which ray is "arriving".**  A live record's
  `ray` is the arrival, but a record rebuilt by
  `PathVertexEval::PopulateRIGFromVertex` is aimed per query along `-wi`: on
  a light-subpath vertex, in every reverse-pdf query (the generators query
  the same vertex with the two directions swapped) and at a connection
  (`EvalPdfAtVertex( lightEnd, dirToCam, ... )` aims it at `-dirToCam`) it
  is not the walk's incoming segment.  Reading the facing off that ray would
  let the frame of one vertex change with the query direction -- the DL-100
  frame trap the round-2 flag mirroring exists to avoid.  So the facing is
  recorded at the live hit, `BDPTVertex::bGeomNormalOpposesArrival` (both
  generators), and replayed as `RayIntersectionGeometric::arrivalGeomFacing`
  (0 on a live record: read it off `ray`; -1 / +1 on a rebuilt one), read
  through `GeomNormalOpposesArrival()`.  The composite resets it to 0 on the
  frame it hands its layers, whose walk rays are live.
* **Row D9** (`--winding-only`): the consistent and the inverted double-sided
  mesh box side by side (all / back / front faces reversed), composite
  {glass/glass} and plain glass (each half == 1 within 0.002: lossless,
  all-delta, zero variance) and composite{glass/translucent} (thickness
  0.05, extinction 0.2; inverted / consistent within 2 %), PT / BDPT / VCM.
  Against the round-4 library (29fe9f3ef): **29 / 16** -- glass/glass
  inverted 0.46669 (all), 0.50587 (back), 0.9799 (front) under all three
  integrators; glass/translucent inverted/consistent +1.3 % to +4.4 % (red
  on BDPT / VCM in all three modes and PT on back-only); the plain-glass
  controls 1 on both.  Round 5: **45 / 0**, glass/glass 1.0000 (VCM 1.0001)
  everywhere, glass/translucent within 0.1 % (PT) and 0.4 % (BDPT / VCM).
* **Not this row (the reviewer's measurement): a composite with a subsurface layer
  reads ~0.017-0.045 in a white furnace** (head, round 4 and master alike)
  -- `CompositeMaterial` forwards neither `GetDiffusionProfile()` nor
  `GetRandomWalkSSSParams()`, so an SSS or random-walk layer contributes only
  its SPF's own reflection lobe and every BSSRDF / random-walk transport
  through it is lost.  Filed as **DL-422**.
* **P3:** D8's bands corrected in 9.4c above (5 % nested, 3 % translucent).

### 9.4e Review round 6 (2026-10-02): FAIL, 1 P1, P3s -- addressed

* **P1 -- a closed SINGLE-sided mesh wound inward** (fully or partly; single
  sided is the default for ply / glTF / 3ds / raw meshes, and ply even has
  `invert_faces`).  An outside hit on such a mesh meets a BACK face, which
  round 6 walked from below; that walk's upward exit treated the crossing as
  leaving the object, so O was never pushed, and the later inside hit (no O,
  so round 6 did not unflip it) was walked from above and carried O out:
  1/eta^2.  The reviewer (white furnace, salted n = 3): composite{glass/glass}
  all triangles reversed 0.4444 under PT / BDPT / VCM (master 0.979, plain
  glass 1.0); light inside, camera outside 0.222 (master 0.1046, glass
  0.1055); glass/translucent front face reversed 0.652 (master 0.940,
  consistent 0.917); nested 0.446 / 0.567; and a single-sided open sheet hit
  from behind with an empty stack disagreed with plain glass (two separate
  panes 0.936 vs 0.467; a mirror return 0.678 vs 0.929).
* **Fix (the reviewer's prototype, folded into one rule):** the frame is
  oriented BY THE WALK'S STACK.  Outside the object (stack lacks O) the
  arriving ray meets the top -- a reported normal that does not oppose the
  arrival is turned; inside (stack holds O) it meets the bottom -- a reported
  normal that opposes the arrival is turned (round 6).  Winding, sidedness
  and the flip flag no longer decide anything; the flag stays a sufficient
  condition for "opposes".  Provably open sheets (clipped planes), hair and
  stackless callers are never turned (unchanged).  This is the rule a plain
  `dielectric_material` follows, so every mesh cell below follows the
  dielectric's "separate sheets" convention.
* **Section M, the sidedness matrix** (`--sidedness-only`, ~10 min).
  {single, double}-sided x {outward, inward, mixed (every odd triangle
  reversed)} winding x {closed box in the furnace, camera outside; closed box
  with a light inside, no environment; open quad in the furnace} x
  {composite{glass/glass}, composite{glass/translucent} (thickness 0.05,
  extinction 0.2), nested composite{composite{glass/water}/glass}} x {PT,
  BDPT, VCM at depth 12}: 162 cells, each beside its twin -- plain glass on
  the SAME geometry / sidedness / winding for glass/glass, the double-sided
  outward version of the same geometry for the others.  Plus three
  glass/glass sheet families against plain glass over sidedness x winding x
  integrator: one object holding two panes, two separate one-pane objects,
  and a sheet over a mirror (54 cells).  Every render salted.

  | Cell | Expected (each half) | Convention |
  |---|---|---|
  | closed box, furnace, glass/glass | 1, and its plain-glass twin 1 | index-matched stack = one glass interface |
  | closed box, furnace, nested | 1 (lossless), == twin | -- |
  | closed box, furnace, glass/translucent | == twin (~0.915) | -- |
  | closed box, light inside | == twin (glass/glass ~0.105) | -- |
  | open sheet, furnace | == twin; glass/glass 0.467 = F + (1-F)/eta^2 | separate sheets: a ray that crossed is inside |
  | two panes, one object | == plain glass (1.0, a slab) | separate sheets |
  | two panes, separate objects | == plain glass (0.467) | separate sheets |
  | sheet over a mirror | == plain glass (0.929) | separate sheets (the return meets the sheet from behind with O on the stack) |

  No cell is "top on both faces": that convention belongs to the provably
  open clipped plane alone (DL-407), which is not in the matrix.  Bands:
  zero-variance all-delta cells 0.2 % (the mirror return 0.5 %: BDPT / VCM
  are not zero-variance there, a gate run read 0.24 %); glass/translucent
  2 % (256 spp);
  nested 5 % (1024 spp, per-branch estimator: 144 furnace / sheet ratios
  over four full runs read sd 0.69 % with a heavy tail -- max +3.47 %, which
  failed a 3 % band once); light inside 1024 spp, every
  cell the mean of 3 salted renders: 3 % glass/glass (a single render's
  ratio sd is ~0.5 % under PT / BDPT but ~1.1 % under VCM -- 10 repeats,
  the round-7 review; a single VCM render in a 3 % band was a ~2.5 sd gate,
  4-7 % spurious failures per run), 5 % translucent / nested (a single
  render's sd is ~1.5 % per half).  Every regression the
  section exists for moves a cell by 5 % or more.

  **Red / green.**  Round-6 library: **187 / 83** (270 checks).  The red
  cells are exactly the single-sided inward and mixed ones, under all three
  integrators: glass/glass closed box 0.4445 (inward) / 0.779 (mixed) against
  1; light inside 0.2225 against 0.1055 (inward) and 0.100 against 0.106
  (mixed); open sheet 0.977 / 0.699 against 0.467; glass/translucent box
  0.418 / 0.749 against 0.915, light inside 0.194 / 0.173 against 0.151,
  sheet 0.694 / 0.607 against 0.534; nested box 0.446 / 0.78 against 1, light
  inside 0.218 / 0.099 against 0.105, sheet 0.975 / 0.703 against 0.47;
  two panes one object 0.966 / 0.979 against 1.0; separate panes 0.936 /
  0.655 against 0.467; mirror 0.677 against 0.929.  Every double-sided and
  every single-sided outward cell was already green (round 6 fixed those).
  Round 7: **270 / 0** with the final bands (the one failure in an earlier
  run, glass/translucent single mixed light inside PT 0.155 vs 0.148, was a
  single-render ~2.5 sd excursion; four salted repeats of that cell read
  0.150-0.154 per half, which is why every light-inside cell now averages
  three renders -- glass/glass too since the round-7 review measured VCM's
  ratio sd there).
* **Not changed, measured (the round-7 reviewer): a camera INSIDE a closed
  composite** (DL-407, not seeded) -- composite{glass/glass} reads 0.444
  under PT / BDPT / VCM against the seeded plain-glass truth 2.25 (master
  0.465).  With the stack deciding the frame, the unseeded first inside hit
  reads as an outside arrival, meets the top and refracts as an entry.
* **P3 -- the arrival-facing replay is correct by construction and
  unit-tested only.**  With the replay line removed from
  `PopulateRIGFromVertex` the whole matrix still reads 270 / 0 and no cell
  moves outside its noise (the largest shifts, ~3 %, are in light-inside
  cells and include PT, which never rebuilds a record).  The replay matters
  only for a reverse-pdf or connection query whose rebuilt ray faces the
  other way from the arrival AT A NON-DELTA vertex on a turned record -- an
  MIS-weight input, not a throughput one -- and no fixture found moves a
  render with it.  `BDPTVertexRIGRebuildTest` pins the plumbing (93 / 11
  with the replay line removed).
* **P3 -- depth.**  D9's glass/translucent rows read ~1.3 % lower under
  BDPT / VCM than PT on BOTH boxes alike: the depth-8 rasterizer helpers
  truncate the box's internal bounces (0.3 % at depth 12).  D9 and M now run
  BDPT / VCM at depth 12.

### 9.5 Residuals

* **DL-406** -- term (a) still prices a Henyey-Greenstein-warped or a
  per-channel / dispersive RGB top as an ideal refraction (shape only; energy
  exact).
* **DL-407** (re-scoped by review round 1) -- (1) `CompositeMaterial`
  reports no `SpecularInfo`, so a camera or light inside a closed composite
  is never seeded (from-below hits then read the outside medium as BELOW);
  pre-existing.  (2) An OPEN composite sheet (a provably open clipped plane)
  presents its top on both faces instead of following DL-345's face rule, so
  the separate-pair equivalence holds front-side only (from below: 0.467
  against the pair's 4.52); pre-existing, unchanged -- and that holds for
  every OPEN double-sided sheet seen from behind with no prior crossing,
  mesh, Bezier patch or clipped plane (round 3: decided by the walk's stack,
  D7).  Double-sided CLOSED meshes are no longer in it
  (fixed in round 1, D5).
* **DL-422** (filed review round 5) -- a composite with an SSS / random-walk
  layer drops all subsurface transport (no BSSRDF forwarding): ~0.017-0.045
  in a white furnace; pre-existing on master.

## 10. DL-296 (slice `debt-dl296`, 2026-10-09): transmission out through the bottom

Branched from `master` `638591364`.  **Status: implemented for a
TRANSLUCENT bottom on a provably open sheet, PT / BDPT / VCM consistent
since the two-sided model of section 10.7 (DL-472 (1)), which resolved
the blocker recorded in section 10.6.**  A thin-transmission weave bottom
was in the first scope and is not any more (section 10.7); the rest of
the walker class is DL-472.

### 10.1 The defect

A walk entered from above that left DOWN through the bottom was WALKER
transport: delta-tagged, so no NEE arm and no BDPT/VCM connection priced
it, and `CompositeMaterial::ScattersFullSphere()` was false.  A delta light
behind a `composite{glass/translucent}` sheet therefore lit nothing through
it under any integrator, and an area light reached it only by BSDF-sampled
hits (high variance).

### 10.2 The fix -- term (c)

The mirror of term (b), at the other layer: at every bottom visit of the
evaluator's recorded walk, CONNECT to a below-horizon exit `wOut` through
the bottom's own BSDF,

    f_c(wOut) = sum_i beta_i * f_bottom(wOut | visit i) * E,

with no Jacobian (nothing refracts after the bottom event) and `E` the
radiance-mode eta^2 of the exit.  Everything else follows from DL-24's
three-class partition:

* **Who owns the class.**  `CompositeSPF::bBottomTransmits` = the bottom
  MATERIAL claims `ScattersFullSphere()` AND has a BSDF (passed in by
  `CompositeMaterial`; a direct `CompositeSPF` construction defaults to
  false, so every test/tool building one keeps its old behaviour).  The
  walker then leaves a non-delta down-going bottom exit, after a covered
  entry, on the far geometric side, to the covered sampler (`Walker`'s
  bottom-exit branch); delta bottom exits and every other walk stay walker.
  The probe's `walkerPossible` no longer fires on such an exit.
* **Sampling.**  The covered mixture gains `w5`, a cosine hemisphere BELOW
  (about `-n`), and `w3` (the bottom's own sampler at the outer record) now
  also emits its below-horizon non-delta draws.  The covered budget splits
  `0.35 / 0.45 / 0.2` (`w2` / `w3` / `w5`) when the bottom transmits, else
  `0.5 / 0.5` as before.  `Pdf` below the horizon is
  `w5 |cos|/pi + w3 bottom.Pdf(w)` on the far geometric side (no DIRECT
  term: the top's own down lobes are the walk's entry, not exits), so it is
  still the exact density of what `Scatter` emits.
* **The eta^2 contract.**  The emitted covered ray carries the BELOW stack
  (`BelowExternalStack`: OUT plus O at `BelowMediumIOR`, exactly the
  walker's `ToExternal` form for a non-refracting bottom), so a radiance
  walk applies `RadianceEtaScale` to it as it does to every transmitting
  SPF's ray (ISPF.h): its kray EXCLUDES the factor.  NEE and BDPT eye
  connections have no stack change of their own, so the VALUE includes it
  (`BelowEtaScale`).  Hence `kray * Pdf * E == value * cos` (section C,
  new row C4), and the HWSS companion weight (`EvaluateLobeFNM`) of a
  below ray is the value WITHOUT `E`.
* **Only on an open sheet.**  Term (c), the `w5` / below-`w3` proposals
  and the walker's hand-off are LIVE only at a record whose surface
  provably encloses no volume (`RayIntersectionGeometric::
  bProvablyNoInterior`: a clipped plane, a flat consistently wound mesh
  sheet since DL-382; mirrored onto BDPT's rebuilt records).  The flag is
  read off the CALLER's record at each public entry point and held in a
  thread-local guard (`LiveGuard`), because `CompositeLayerFrame` hands the
  walk a record with the flag cleared.  Why: on a surface that may bound
  an interior, the composite's frame for an arrival from INSIDE is decided
  by the IOR stack, which BDPT / VCM's reverse-density queries (built from
  the forward arrival's stack) and an unseeded walk inside a closed
  composite (DL-407 (1)) do not reproduce.  With term (c) live on a closed
  `composite{glass/translucent}` box the bidirectional MIS partition broke
  (opt-in probe `--dl296-probe`, 3 salted 1024-spp renders): emitter INSIDE
  the box PT 0.152 / BDPT 0.435 / VCM 0.417 (master 0.150 / 0.150 / 0.150),
  emitter OUTSIDE behind it PT 0.0525 / BDPT 0.0102 / VCM 0.0071 (master
  0.0502 / 0.0535 / 0.0511).  With the open-sheet rule: inside 0.152 /
  0.148 / 0.155, outside 0.0487 / 0.0497 / 0.0496 (three further salt sets
  0.053 / 0.050 / 0.052, 0.053 / 0.058 / 0.047, 0.057 / 0.054 / 0.054 --
  that estimator's own ~10 % spread).  Closed objects are DL-472.
* **Capability.**  `CompositeMaterial::ScattersFullSphere()` =
  `HasTransmissionValue()` (material-level, so also on closed objects,
  where the value below is 0 and the claim costs NEE samples only; the
  same over a top that transmits nothing).  `TranslucentLobeConsistencyTest`
  gate 6 re-ruled: open-sheet record prices the far side, a possibly
  closed record does not.

### 10.3 Evidence

`CompositeEnergyConservationTest --dl296-only` (section X): left half the
composite (thickness 0) on an open quad, right half the equivalent pair of
SEPARATE sheets (glass at z = 0, the bottom material 0.002 below), camera
in front, the only light behind; mean of 3 salted renders, 32 x 16.

| bottom, light, integrator | pre-fix (`638591364` lib) composite / pair | post-fix composite / pair (ratio) |
|---|---|---|
| translucent, omni, PT | **0** / 0.43342 | 0.43268 / 0.43343 (0.998) |
| translucent, omni, BDPT | **0** / 0.43426 | 0.43531 / 0.43428 (1.002) |
| translucent, omni, VCM | **0** / 0.43389 | 0.44807 / 0.43391 (1.033) |
| translucent, omni, PT spectral | -- | 0.43236 / 0.43245 (1.000) |
| translucent, omni, PT spectral `hwss TRUE` | -- | 0.43318 / 0.43330 (1.000) |
| translucent, area, PT | 0.11667 / 0.11899 (per-render 0.963 .. 1.012) | 0.11876 / 0.11883 (per-render 0.998 .. 1.001) |
| weave, omni, PT | **0** / 0.17694 | 0.17628 / 0.17695 (0.996) |
| weave, omni, BDPT | **0** / 0.17694 | 0.17651 / 0.17680 (0.998) |
| weave, omni, VCM | **0** / 0.17702 | 0.17666 / 0.17728 (0.996) |
| weave, omni, PT spectral | -- | 0.17546 / 0.17570 (0.999) |
| weave, omni, PT spectral `hwss TRUE` | -- | 0.17629 / 0.17678 (0.997) |
| weave, area, PT | 0.05117 / 0.04749 (per-render 0.947 .. 1.221) | 0.04738 / 0.04750 (per-render 0.997 .. 0.998) |

Post-fix numbers are the final (open-sheet-gated) build; the translucent
PT / BDPT / VCM rows of that build read 0.43305 / 0.43607 / 0.44735
against pairs 0.43342 / 0.43425 / 0.43370 (0.999 / 1.004 / 1.032).  Red
9/7 (pre-fix library, the PT/BDPT/VCM rows; the spectral rows were added
after) -> green 28/0.  The area rows were already unbiased pre-fix -- the
walker reached the emitter by BSDF sampling -- and now converge: the
per-render spread of the composite / pair ratio shrinks ~20x.

**The translucent VCM +3 % is not term (c).**  Two controls separate it:
(1) with a RECIPROCAL transmitting bottom (the weave rows) PT, BDPT and
VCM agree on the composite to 0.4 %; (2) with `composite{translucent /
translucent}` the SEPARATE translucent pair alone reads PT 0.584 / BDPT
0.625 / VCM 0.664 -- `translucent_material` is not reciprocal (DL-223),
and an open composite sheet presents its TOP to a light arriving from
behind (DL-407 (2), "top on either face"), so BDPT/VCM's light-side
strategies price a different layer order than the eye side.  Banded at
5 % for the translucent BDPT/VCM rows, 2 % elsewhere.

Pointwise (section C, new row C4 `dielectric/translucent, t 0.2, ext
0.3`): 10 735 non-delta rays per pipe, 5 518 of them below the stack,
`kray * Pdf * E == value * cos` with 0 mismatches, RGB and NM.  Full-sphere
furnace F5 (dielectric / lossless translucent): 0.9954 +- 0.0107 /
0.9999 +- 0.0057 at 0 / 60 deg.

### 10.4 What is NOT covered (DL-472)

* **Closed (or not provably open) surfaces**: term (c) is off there
  (section 10.2), so transmission INTO a closed composite object stays
  walker-only -- a delta light reaching the camera only through a closed
  composite shell still contributes nothing.  Needs the reverse-density
  and seeding consistency named in 10.2 (DL-407 (1)).
* Walks entered FROM BELOW and arrivals behind a tilted shading normal
  (the natural walks): still delta-tagged -- except a back-face arrival
  on an open sheet with a translucent bottom, which section 10.7 prices
  (two-sided model).  A thin-transmission weave bottom is out of the
  DL-296 scope altogether (section 10.7).  So a delta light seen through
  the back face of a composite sheet, or through a CLOSED composite
  object (the second wall is crossed from inside, i.e. from below), still
  contributes nothing under PT.
* A bottom whose transmission is DELTA-tagged -- a rough dielectric
  (`dielectric_material` with finite `scattering`: a Phong-warped delta
  transmission, no BSDF).  DL-297's adjoint-warp connection would extend
  to it; not done.
* Null-BSDF layers (skin / tissue): no evaluator at all.
* All-delta chains stay delta by design (a caustic class: SMS / merging).

### 10.5 Gates

`CompositeEnergyConservationTest` (final build): `--dl296-only` 28/0,
`--sheets-only` **104/4** (section 10.6), `--no-render` 481/0.  On the earlier both-faces / any-surface build: `--no-render`
481/0, `--stack-only --render` 498/1 (one open-sheet VCM twin cell,
1.021 against a 2 % band).  `TranslucentLobeConsistencyTest`
1228/0, `LayeredWhiteFurnaceTest` 0 of 63 failed, `CompositeExtinctionTest`
pass, `SPFBSDFConsistencyTest` pass, `SPFPdfConsistencyTest` pass,
`HWSSCompanionKrayTest` 204/0, `WeaveGapShadowTransmittanceTest` query
81/0 and composite 2/0.  No shipped scene binds a composite with a
transmitting bottom (`composite_material.RISEscene`'s bottoms are
Lambertian / emissive / a nested opaque composite), so no shipped render
moves.

### 10.6 Blocker: the composite is not reciprocal on an open sheet

A non-face-rule composite presents its TOP on either face of an open sheet
(DL-407 (2)).  So `f(a -> b)` evaluated from an arrival on side `a` and
`f(b -> a)` from side `b` model different layer orders, and a BDPT / VCM
connection at the sheet combines the eye-side model with the light-side
one.  Before term (c) that never mattered for transmission (the class was
delta on both sides).  With it:

| fixture | PT | BDPT | VCM |
|---|---|---|---|
| X, white furnace, glass/translucent: composite / separate pair | 0.999 | 1.064 | 1.167 |
| X, white furnace, glass/weave: composite / separate pair | 1.010 | 1.025 | 1.044 |
| M open sheet furnace, glass/translucent, outward twin (absolute) | 0.534 | 0.607 | 0.750 |
| M open sheet furnace, same, `mixed` quad (uncertified: term off) | 0.53 | 0.53 | 0.54 |

(The separate pairs agree across integrators to 0.4 %, so the integrators
are not the issue.)  Gating the term to FRONT-face arrivals fails the
other way: BDPT / VCM read 0.97x / 0.86x PT under the omni light even for
the reciprocal weave, whether the face is judged from the query's ray or
from a rebuilt record's replayed arrival facing -- a connection then
evaluates a nonzero eye-side `f(x)` against a zero light-side `f(x)`
while the MIS weights still reserve the light-tracing strategy's mass.
Only a reciprocal two-sided model closes it: DL-345's face rule for
non-delta composites (front = top, back = bottom) plus a from-below
covered evaluator (DL-472 (1)), so a back arrival prices the same
transport, mirrored.  The slice's commits carry the open-sheet, both-faces
version (`TransmissionLive`); the four failing Section M cells and the
furnace rows above are its known state.

### 10.7 Resolution: the two-sided model (DL-472 (1), same slice, 2026-10-09)

Two changes, both needed (each alone was measured insufficient).

**1. Present the sheet by its TRUE face.**  A composite whose term (c) can
be live now follows DL-345's face rule on a provably open sheet
(`FollowsOpenSheetFaceRule`), so a FRONT arrival meets the top first and
a BACK arrival meets the BOTTOM first.  A back arrival is priced by the
exact mirror of the from-above model (`CompositeSPFImpl`, "THE BACK-FACE
MODEL"): DIRECT is the bottom's own back-side lobes at the entry vertex
(priced by its BSDF); COVERED is an exit through the top after a
non-delta bottom event (term (a), whose walk now includes the ENTRY event
-- `BuildWalkFromBelow`), an exit through the top's BSDF (term (b)) and
an exit back through the bottom after a top reflection (term (c));
sampled from a cosine toward the front, the bottom's own sampler and a
cosine toward the back; WALKER is the rest (`eStartCoveredBottom`).  `Pdf`
is its exact density, `EvaluateLobeFNM` its companion weight.  Stacks: the
ray arrives in the below medium C (OUT plus O at the below index); the
bottom is met from OUTSIDE itself, exactly as the top is from above (its
key on C but not contained, so a translucent bottom shows its ENTRY lobes,
front reflection included, as a separate translucent sheet seen from
behind does), and a ray it passes into the gap is back in C, so the top is
met from inside O and exits to OUT.  (A first version used the natural
from-below walker's convention -- the bottom met from inside, EXIT lobes,
no reflection from behind -- and read a glass/translucent sheet seen from
the back at 0.058 against the separate pair's 0.035-0.044; now 0.0376.)
An exit through the top carries the radiance factor `(n_below / n_out)^2`
through `RestoreOpenSheetStacks`' crossing index -- the value includes
it, the kray does not.  Which side a query is on is read from the query's own ray
against the frame's TRUE normal, and each side normalizes the stack it
starts from (`FrontStack` / `MakeBackStacks`), so a BDPT reverse-density
query rebuilt from the other vertex gets that side's model.

**2. Light-subpath connections evaluate the ADJOINT.**  The face rule
alone took the reciprocal-bottom furnace from 1.06x / 1.17x (BDPT / VCM)
only to 1.03x / 1.17x: a non-delta BSDF that refracts INSIDE itself is
not symmetric in its arguments (its radiance value carries the
`(n_out/n_in)^2` of the index step; section R1 measures
`f_front(a->b) = f_back(b->a) / n^2` to 2e-4), and BDPT / VCM evaluate a
light-subpath vertex with the roles swapped.  `PathVertexEval::
EvalBSDFAtVertex{,NM}` now marks a light-subpath vertex with the new
`BSDFImportanceScope` (`IBSDF.h`, thread-local; every other BSDF ignores
it), and the composite answers such a query with the radiance value of
the SWAPPED query (`ImportanceSwap`: arrival from the eye side, light
toward the original arrival).  Both ends of a connection then price a
path with ONE function -- which also removes `translucent_material`'s own
non-reciprocity (DL-223) from the composite's connections.  (A first
version that only dropped the eta^2 factor in importance mode fixed the
reciprocal bottom -- 0.999 / 0.999 / 1.000 -- but left the N 3, tau 0.7
translucent at VCM 1.023 in the furnace.)

**Scope narrowed to a translucent bottom.**  A thin-transmission
`weave_material` bottom also claims the full sphere, but under the
composite's back-face layer record its value did not match its own
sampler (section R, measured before the scope: back `f(b->a)` ~0 at every
oblique pair while the front read 0.069, `kray*Pdf*eta^2 != value*cos` on
78 % of back-arrival rays, back furnace 0.39-0.46), so
`CompositeMaterial` passes the transmission flag only for a
`TranslucentMaterial` bottom; a weave bottom is walker-only as before
DL-296 (DL-472).

**Evidence** (`--dl296-only`, composite / separate pair, mean of 3 salted
renders; `--dl472-unit`):

| bottom, light | PT | BDPT | VCM |
|---|---|---|---|
| translucent (N 3), white furnace | 0.999 | 1.002 | 0.999 |
| translucent (N 3), omni behind | 1.001 | 0.998 | 0.999 |
| reciprocal translucent (N 1, tau 1), white furnace | 1.001 | 0.999 | 1.000 |
| reciprocal translucent, omni behind | 0.999 | 0.999 | 1.000 |

(Before section 10.7 the first row read 0.999 / 1.064 / 1.167.)  Area
rows 0.999 (PT); spectral rows within 0.3 %.  Section R: reciprocity
`f_front / (f_back / n^2)` = 1.0001 / 1.0001 / 1.0002 / 1.0001 over four
direction pairs; back arrivals 0 `kray*Pdf*eta^2` mismatches on ~38 000
non-delta rays per angle, back furnace 1.003 / 1.002 / 0.994.

Section M: the glass/translucent open-sheet cells now follow the face
rule, so an INWARD sheet is seen from behind and the twin keeps the cell's
winding; a new gate requires BDPT / VCM == PT on those cells (3 %); the inward (back-viewed) cells read 1.784 / 1.786 / 1.787 (PT / BDPT / VCM, single-sided) against the outward 0.532 / 0.532 / 0.538.
Section D10's glass/translucent row asserted back == front (the
pre-face-rule model); it now gates the composite across PT / BDPT / VCM
from each side (2 %) and against the separate pair from the front (PT,
3 %), with an area light on the camera's side (a delta light cannot reach
the pair's translucent sheet through its delta glass sheet).  From the
back the pair is printed only: it is not itself consistent there (PT
0.0351, BDPT / VCM 0.0441) while the composite reads 0.0376 / 0.0376 /
0.0375.

**Closed objects stay as on master.**  With the term off on a closed
composite, a non-delta transmission out of a translucent bottom is walker
transport again, and the probe must still flag it: the first version of
this slice tested the bottom's capability instead of the LIVE bit, gave
the walker its 0.05 floor share (20x weights) and read the D9 closed
glass/translucent box 1.6 % low at 256 spp.  The probe now asks
`TransmissionLive` (carried in its cache key); D9 matches master
(consistent / inverted 0.919 / 0.919 PT, 0.912 / 0.915 BDPT, 0.911 /
0.914 VCM; master 0.920 / 0.918, 0.911 / 0.915, 0.912 / 0.915).
`--dl296-probe` (closed box, emitter inside): PT / BDPT / VCM 0.149 /
0.150 / 0.150.

**Gates (final build, branch merged with `master` `4c01fb2cc`).**
`CompositeEnergyConservationTest`: `--dl472-unit` 10/0, `--dl296-only`
44/0, `--sheets-only` 116/0, `--no-render` 481/0, `--stack-only --render`
510/0 (D, D7-D10, H, T, W), `--sidedness-only` 278/0; `--dl296-probe`
closed box PT / BDPT / VCM 0.149 / 0.150 / 0.151 (emitter inside) and
0.0508 / 0.0535 / 0.0504 (outside; master 0.0502 / 0.0535 / 0.0511).
`TranslucentLobeConsistencyTest` 1229/0, `LayeredWhiteFurnaceTest` 0 of
63, `CompositeExtinctionTest` pass, `SPFBSDFConsistencyTest` pass,
`SPFPdfConsistencyTest` pass, `HWSSCompanionKrayTest` 204/0,
`SourceHygieneTest` 172/0, `WeaveGapShadowTransmittanceTest` query 81/0,
composite 2/0.  Library builds warning-free.  Cost: a light-subpath
connection at such a composite evaluates the layered value once more
(the swapped query); no shipped scene binds a translucent-bottomed
composite on an open sheet, so no shipped render changes.

## 11. DL-406 (slice `debt-dl406`, 2026-10-09): the warps DL-297 left ideal

Branched from `master` `4ef05c006`.  Term (a) (section 9.2) connected a
bottom visit to the exit through the top's delta-tagged transmission with
an adjoint draw of a Phong `cos^N` warp only; a Henyey-Greenstein
`dielectric_material` (`hg TRUE`) and a per-channel / dispersive RGB top
reported "no warp" and were connected as an ideal refraction -- the same
energy, all of it in the ideal-Snell direction.

### 11.1 What the sampler actually does

Read off `DielectricSPF::GenerateScatteredRay`, not off the ledger row:

* **HG** (`hg TRUE`, `g = scattering`): `alpha = acos(mu)` with `mu` from
  the HG inverse CDF `mu = (1 + g^2 - ((1 - g^2)/(1 - g + 2 g xi))^2)/(2g)`
  for `g < 1`.  The perturbation runs only for `0 < alpha < PI/2`, so every
  draw with `mu <= 0` leaves EXACTLY on the Snell axis: a delta part of
  weight `P_d = F_HG(0) = (1 - g^2)/(2g) (1/sqrt(1 + g^2) - 1/(1 + g))`
  (the row's "past 90 deg" is right).  The rest is the HG polar marginal
  `0.5 (1 - g^2)/(1 + g^2 - 2 g mu)^1.5` on `(0, 1]`, azimuth uniform on
  `PerturbClipped`'s valid arc.  `g == 0` makes the sampler's `1/(2g)`
  non-finite (alpha NaN, never perturbed): an ideal delta.  `g <= -1` is
  not a distribution; both report none, as before.
* **RGB per-channel** (`Scatter`'s `disperse` test: a per-channel `ior`
  or `scattering`, or an AR stack): `DoSingleRGBComponent` once per
  channel with that channel's ior, scattering and AR wavelength and ONE
  shared random pair, each transmission carrying only its own channel.

### 11.2 The fix

* `ISPF`: `DeltaTransmissionWarpLaw` (kind Phong / HG, `DeltaFraction`,
  `PolarDensity` -- the sub-density of the perturbed draws --
  `SampleWarpedPolar`), `DeltaTransmissionWarp( ri, nm, channel )`,
  `DeltaTransmissionWarpIsPerChannel( ri )`, and a `channel` argument on
  `DeltaTransmissionWarpPdf`.  `DeltaTransmissionWarpExponent` survives
  as a non-virtual Phong-only wrapper (SMS reads it).
* `DielectricSPF`: the law per channel (`ResolveWarpChannel` mirrors
  `Scatter`'s channel selection exactly); `DeltaTransmissionWarpPdf`
  builds the axis by running the real sampler with its own no-warp value
  (`scattering 1` for HG, `1e6` for Phong) and returns
  `PolarDensity(c) / (2 half)`.
* `CompositeSPF::ConnectThroughTop`: term (a) for one law = the DELTA
  part's ideal connection at `u = inverse Snell(wOut)` scaled by
  `DeltaFraction` (1 for an ideal top, 0 for Phong) + the PERTURBED
  part through one adjoint draw of the law about `wOut` (HG: the marginal
  restricted to `mu > 0`, renormalized by `1 - P_d`), weighted
  `q cos t / (p cos wOut)`.  A per-channel RGB top is connected once per
  channel through that channel's law, each masked to its channel; when no
  channel warps the single ideal connection runs as before.  The Phong
  path is BIT-IDENTICAL (Section W's output diffs empty against the base
  library); the shipped composite scene's top is a uniform Phong glass, so
  no shipped render moves.

### 11.3 Evidence

`CompositeEnergyConservationTest` Section W2 (`--dl406-only`): Section
W's exit-energy histogram (6 bins in `cos theta_out`, theta 0 / 45,
16 x 20000, untilted, jittered position) per CHANNEL, composite vs the
independent layer walk; the per-channel rows are referenced to the walk
through the UNIFORM top with that channel's ior and scattering (identical
code per channel, and a white bottom does not mix channels -- the
dispersive walk itself has a heavy tail the batch sem does not resolve).
Rows: HG g 0.6 (RGB and NM 550 nm), HG g -0.3 (`P_d` 0.714; g 0.6: 0.124), per-channel
scattering 0 / 5 / 10000, dispersive ior 1.45 / 1.5 / 1.6 at scattering
0, dispersive ior with per-channel HG g 0.3 / 0.6 / 0.85.

| row (theta 0, bin grazing -> normal) | base composite | fixed composite | walk |
|---|---|---|---|
| HG g 0.6 | 0.013 / 0.065 / 0.129 / 0.192 / 0.253 / 0.349 | 0.073 / 0.102 / 0.137 / 0.176 / 0.216 / 0.298 | 0.074 / 0.101 / 0.135 / 0.175 / 0.216 / 0.299 |
| HG g -0.3 | 0.013 / 0.065 / 0.129 / 0.192 / 0.253 / 0.349 | 0.047 / 0.087 / 0.137 / 0.185 / 0.232 / 0.314 | 0.047 / 0.086 / 0.136 / 0.185 / 0.231 / 0.315 |
| per-channel scattering, red (scattering 0) | 0.013 / 0.065 / 0.128 / 0.191 / 0.252 / 0.351 | 0.119 / 0.134 / 0.150 / 0.169 / 0.185 / 0.240 | 0.122 / 0.136 / 0.151 / 0.168 / 0.183 / 0.239 |

Base library (`4ef05c006`, the four library files checked out over the
new test): Section W + W2 **69 / 159**, W2 z from -179 (every warped
row red; the scattering-10000 channel is the control, green in both).
Fixed: **228 / 0**, W2 max |z| 2.85 over 192 bins, Section W unchanged
(max |z| 2.04, output identical).

Section M gains family 3 (re-covering the warped separate-panes case
DL-407 (2) moved to delta-sharp glass): the separate panes with the
parser-default WARPED glass, the composite being {warped glass / sharp
glass} -- its index-matched inner interface sharp, so each pane is one
warped interface exactly like the plain-glass twin, including at the TIR
edge (band 0.5 %; measured within 0.15 % in all 18 cells, e.g. inward
single-sided PT / BDPT / VCM 4.6319 / 4.6374 / 4.6313 against plain glass
4.6341 / 4.6405 / 4.6352; `--sheets-only` 134 / 0).  A {warped / warped} pair blurs that edge twice
(-0.3 %): each layer warps its own transmission by definition, the
equivalent pair of separate warped surfaces -- the composite's model, not
a defect, and not term (a) (a dielectric bottom has no BSDF; that
transport is all walker).

### 11.4 What it does not change

* The delta part, like every ideal connection, inverts Snell about the
  SHADING normal; under a tilted shading normal the sampler may re-derive
  the axis about the true surface (DL-111).  Pre-existing for every ideal
  top; the perturbed part is exact there (its `q` comes from the top).
* On a dispersive RGB top, `eta` is the walk's gap index (`gap0`), i.e.
  the index of the channel the walk's entry selected; every other channel
  of such a walk carries zero throughput, so that is exact for walks
  entered from above. DL-480 now uses each native refractor painter's
  RGB index on a back-face walk (beta 1 in every channel at its bottom
  entry), recursively through composite layers. Unknown custom refractors
  retain the scalar metadata fallback.
* DL-480 skips channel connections without bottom throughput, and skips
  zero-throughput visits within a connection. A live channel evaluates the
  delta and perturbed components of its warp. No timing change is measured;
  no shipped scene binds these per-channel tops.

### 11.5 Gates

Library warning-free.  `CompositeEnergyConservationTest` `--dl406-only`
228/0, full run 1089/0 (built before family 3), `--sheets-only` 134/0
(with family 3); `LayeredWhiteFurnaceTest` 0 of 63 failed;
`TransmissionPushGateTest` 416/0; `BDPTStrategyBalanceTest` 370/0;
`VCMStrategyBalanceTest` 165/0; `SourceHygieneTest` 172/0;
`HWSSCompanionKrayTest` 204/0; `SMSDomainReplayTest` 8212/0;
`DielectricARTest` 33/0; `CurvePainterRGBDispersionTest` 15/0;
`CompositeExtinctionTest` pass; `CompositeSubsurfaceRejectionTest` pass;
`WeaveGapShadowTransmittanceTest` query 81/0, composite 2/0 (its full
single-threaded run was not completed).  No render-level measurement: no
shipped scene binds an HG or per-channel composite top.
