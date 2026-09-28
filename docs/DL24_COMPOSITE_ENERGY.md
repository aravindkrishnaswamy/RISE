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

A1 equals A2 bit for bit post-fix because the evaluator treats the top's
delta transmission as ideal Snell whatever its `scattering` is.  That is
DL-297.

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

* **DL-341 (filed at merge by the supervisor) -- the composite's IOR-stack
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
  ABSORBING coat** (~15-17 %; its recycling approximation), section 7.
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
