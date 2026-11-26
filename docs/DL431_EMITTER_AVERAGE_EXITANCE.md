# DL-431: an emitter's average exitance was estimated at P = Po = 0

Status: **FIXED 2026-10-03** (`debt-dl431`, pending review/strike).

## Mechanism

`LambertianEmitter` / `PhongEmitter` / `CompositeEmitter::RefreshAverages`
estimated `averageRadiantExitance()` (and the NM spectrum) at CONSTRUCTION, from
a 10x10 UV grid over a default-constructed record -- `P = Po = (0,0,0)` --
because an emitter does not know its geometry.  That average is a per-luminary
quantity with three consumers:

| consumer | what the average decides |
|---|---|
| `LightSampler::Prepare` | the light-selection weight (alias table, light BVH, RIS) and the `pdfSelect` every hit-side MIS partner reads; an entry with exitance <= 0 is DROPPED |
| `PhotonTracer` / `SpectralPhotonTracer` | the source-selection weight and each photon's power |
| `SMSPhotonMap` | the per-emitter seed-photon budget and the seed power |

Emission keyed on world / object position (`expression_painter`, a 3-D
procedural painter) was therefore weighted by its value AT THE ORIGIN: a field
that vanishes there got zero importance and was never sampled, and one that did
not vanish got the wrong multi-light importance (variance) and a wrong photon
power (bias).  Found by DL-298's red-proof, whose first fixture
(`1.5 (P.x^2 + P.y^2)`) read PT 0.00009 against the closed form 0.0831.

## Repair

* `IEmitter::radiantExitanceAt(ri)` / `radiantExitanceAtNM(ri, nm)` -- the
  LOCAL exitance (painter x scale, no lobe or cosine), the quantity the average
  is the surface mean of.  Lambertian and Phong read their painter at the
  record; `CompositeEmitter` is `top + bottom * exp(-2 * thickness *
  extinction)` with every term read at the record (the same combination its
  constructor averages).  The default forwards to the average, exact for a
  uniform emitter, so a third-party `IEmitter` keeps working.
* `LightSampler::AverageLuminaryExitance(pLum)` -- the per-luminary surface
  mean: a deterministic 10x10 stratified set of `Object::UniformRandomPoint`
  draws (cell centres on the two surface coordinates -- exactly the old UV
  grid's points on a clipped plane -- and a golden-ratio sequence on the third,
  which a triangle mesh uses to pick its triangle), each filled through
  `FillEmitterRecord` (DL-298) so the record carries the physical `P`, `Po`, UV
  and normals.  No render RNG is consumed; the cost is 100 point draws and 100
  local evaluations per luminary, once per `Prepare` / photon batch.
* **Uniform fields keep the emitter's own cached average, verbatim.**  When all
  100 local values are bitwise identical (a constant, spectral or blackbody
  exitance -- every shipped emitter, by DL-298's census) the helper returns
  `uniform = true` and `average = emitter->averageRadiantExitance()`.  Selection
  weights, photon budgets and photon power are then bit for bit what they were.
  On a clipped plane the sample points are the old grid's, so a UV-keyed painter
  averages to the same number too (9.900000000 both ways in the test).
* `LightSampler::Prepare` and `SMSPhotonMap::Build` use the helper's average.
  The two legacy photon tracers use it for the SOURCE weight, and then shoot
  each photon with the exitance AT ITS EMISSION POINT (`M(P) * A / q`, `q` the
  source's selection probability) unless the luminary is uniform.  That second
  step is not optional: positions are drawn uniformly by area, so a photon that
  carried only the surface-mean power would get the right total flux but the
  wrong spatial distribution (flux-weighted `<z^2>` 1/3 instead of the field's
  1/5 on the test sphere).
* `CompositeEmitter::getEmmittedPhotonDir` chooses its layer from the layers'
  exitances at the record rather than their construction-time averages (the
  same P = 0 defect; ulp-level change for uniform layers).

`RefreshAverages` itself is unchanged: it remains the emitter's own estimate and
the verbatim answer for a uniform emitter.

## Red-proof (`tests/EmitterAverageExitanceTest.cpp`)

Row 1 -- the DL-44/DL-298 wall-and-camera rig with `M(P) = 1.5 (P.x^2 + P.y^2)`
(zero at the origin; area mean 1.0) against the same field keyed on the
emitter's UV (a control every path reads correctly) and a deterministic closed
form; 4 salted renders (`SobolSamplerTestHooks::ValueSalt`) per row.  Row 2 --
the real `PhotonTracer` / `SpectralPhotonTracer` emission loops with an ideal
deposit sink on a unit sphere with exitance `1.5 (P.x^2 + P.y^2)`: total flux
closed form `4 pi`, flux-weighted `<z^2>` closed form `1/5`; a second sphere
displaced to x = -10 with `1.5 ((P.x+10)^2 + P.y^2)` has origin value 150
(nonzero, wrong).  Row 3 -- the helper directly (P-keyed surface mean 9.9 vs the
emitter's own 0; UV-keyed equals the old grid; constant returns the emitter's
own average bit for bit; composite local exitance).

| row | master `388907e94` | fixed |
|---|---|---|
| PT RGB, P / closed form | **0.0011** (0.000089 vs 0.083155) | 0.9998 |
| BDPT RGB, P / closed form | **0.0001** (0.000006) | 1.0000 |
| VCM RGB, P / closed form | 0.9937 (sd 0.0019, light-pass fallback) | 0.9998 |
| PT spectral, P / UV control | **0.0011** | 0.9984 |
| BDPT spectral, P / UV control | **0.0001** | 0.9989 |
| photons RGB, flux / `4 pi` (vanishing at origin) | **0.0000** (source dropped) | 0.9994 |
| photons RGB, flux / `4 pi` (origin value 150) | **150.0** | 1.0003 |
| photons RGB flux-weighted `<z^2>` (field 0.2000) | -- / 0.3324 | 0.2002 / 0.1998 |
| photons NM, flux / constant-1 control (vanishing at origin) | **0.0000** | 1.0005 |
| photons NM flux-weighted `<z^2>` | -- | 0.1998 |

Master 18 passed / 12 failed; fixed 37 / 0.  (The render rows' sd are over 4
salted renders: PT RGB 0.000022, BDPT 0.000002, VCM 0.000084, spectral
0.00013-0.00015.)

## Shipped scenes

No shipped scene keys emission on `P`/`Po` (DL-298's census), so every shipped
luminary is `uniform` and takes the verbatim path: light-selection weights and
photon / SMS budgets are unchanged.  Measured, not assumed: a scratch census
loaded all 230 shipped scenes that name a luminaire material (one,
`scenes/pr.RISEscene`, does not load on master either: an unresolved
`standard_object` reference) and called `AverageLuminaryExitance` on every
luminary of the 220 that have one -- **317 luminaries, 0 non-uniform, 0 whose
average differs from the emitter's own** (including `emitter_louvres`, whose
`proximity()` painter reads a gated signal the helper's record does not carry,
so it is constant there).

## Residuals

* The helper's 100 points are a fixed stratified set: a position-keyed field
  with structure finer than the 0.1 pitch is averaged to ~1 % (the P-keyed test
  field reads 9.9 against 10.0 exactly).  That is only an importance /
  photon-budget error -- photon power is local, so no bias -- and a larger grid
  is a one-constant change.
* The emitter's own `averageRadiantExitance()` is still the P = 0 estimate for
  any caller that does not go through the helper (none in the tree after this
  change; `CompositeEmitter` still combines its sub-emitters' constructor
  averages for its own).
* Photons from a non-uniform luminary whose exitance is zero over part of the
  surface now carry zero-power photons there (unbiased, wasteful); an
  importance-sampled emission position is the follow-up if a scene needs it.
