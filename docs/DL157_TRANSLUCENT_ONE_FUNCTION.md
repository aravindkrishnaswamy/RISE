# DL-157 / DL-41 / DL-38 — one function per side for `translucent_material`

**Slice `debt-dl157`, 2026-09-18, branched from `master` `bb2ccd80`.**
Closes three ledger rows that are one defect seen from three sides, opens
two (DL-223, DL-224).

---

## 1. The defect, in one sentence

`TranslucentSPF::Scatter`/`ScatterNM` (the sampler), `TranslucentSPF::Pdf`/
`PdfNM` (the MIS-partner density) and `TranslucentBSDF::value`/`valueNM`
(what NEE and every BDPT/VCM connection pay) were three independent
transcriptions of a four-lobe model, and only one cell of one of them —
the entry front reflection, which DL-112 had just fixed — described a lobe
the sampler actually draws.

* **DL-157** — `value`'s four-way `GetReflectedSide` switch. Its case 0
  priced BOTH the entry transmission and the interior exit with one
  expression (`tau * pow(sd,N) * INV_PI`), two different lobes with two
  different weights; its case 1 priced the interior BACKSCATTER with
  `pRefFront`, a painter that lobe does not carry at all.
* **DL-41** — `Pdf` returned ONE lobe at its full, unweighted density and
  reported ZERO over the entire half-space the two Phong lobes occupy.
  That is why DL-69's BDPT `pdfFwd` and DL-103's PT escape-side MIS
  partner both carry a `misFwdPdf <= NEARZERO` fallback naming this class.
* **DL-38** — the interior Beer extinction and the `(1-s)`/`s` scattering
  split, which the sampler charges as `kray`, never reached a reverse /
  NEE evaluation at all.

---

## 2. The ruling: one per-hit lobe set, read three ways

`TranslucentSPFDetail::BuildLobeSet` (declared in `TranslucentSPF.h`,
defined in `TranslucentSPF.cpp`) rebuilds — without drawing anything —
exactly the set of lobes `Scatter`/`ScatterNM` would emit at a hit,
including the same emission gates and the same per-channel exponent split.
`Pdf`, `value` and the sampler all read it, so they cannot drift.

| side | lobe | shape | axis | clip half-space | `kray` |
|---|---|---|---|---|---|
| ENTRY | front reflection | clipped COSINE (DL-45's exact 2-draw Malley remap, DL-112) | `OrientedLobeAxis(n, geomN)` | `Dot(w, geomN) > 0` | `ref` |
| ENTRY | transmission | clipped PHONG `cos^N` (DL-68) | `OrientedLobeAxis(n, -geomN)` | `Dot(w, -geomN) > 0` | `tau` |
| EXIT | diffuse exit | clipped COSINE (DL-45) | `OrientedExitNormal(n, geomNRaw)` | `Dot(w, geomNRaw) > 0` | `B*(1-s)` |
| EXIT | backscatter | clipped PHONG `cos^N` (DL-68) | `OrientedLobeAxis(n, geomN)` | `Dot(w, geomN) > 0` | `B*s` |

`B = exp(-ext * |ri.ray.origin - ri.ptIntersection|)` is the Beer
extinction over the interior segment (DL-01's "`pTrans` is charged once at
entry; each interior segment then pays only Beer"), `s` the scattering
split, `n = ri.onb.w()`, `geomNRaw` the TRUE outward geometric normal
(DL-70's `UnflippedGeomNormal()` recovery), and `geomN` the side the
incoming ray arrived from.

The three readings:

```
sampler :  draw lobe I,   carry kray_I
Pdf(w)  :  sum_I  q_I * p_I(w)          q_I = RandomlySelect's realized weight
value(w):  sum_I  kray_I * p_I(w) / |cos(w, n)|
```

with

```
clipped cosine:  p(w) = cos(w,axis) / pi / P,      P = (1 + Dot(axis,clipN)) / 2
clipped Phong :  p(w) = (N+1) cos^N(w,axis) / (2 * halfArc(theta))
```

and `halfArc` transcribed from `SampleClippedPhong`'s own arithmetic.

### 2.1 Two structural properties that make this exact in closed form

Where `SchlickSPF` / `IsotropicPhongSPF` /
`AshikminShirleyAnisotropicPhongSPF` needed DL-67 / DL-98 / DL-99's replay
quadrature to estimate `q_I`, this material does not, for two reasons:

1. **Every lobe's `kray` is direction-independent** — a painter read times
   a Beer factor that depends only on the INCOMING segment — so
   `RandomlySelect`'s realized probability IS the raw weight ratio
   `MaxValue(kray_I) / sum_J MaxValue(kray_J)`. This is exactly the reason
   DL-98/DL-99 recorded both Ward SPFs as immune to that pattern (before
   DL-177 found Ward's own, unrelated defect).
2. **On each side the two lobes occupy COMPLEMENTARY half-spaces** —
   `Dot(w, geomN) > 0` against its exact complement — so they never
   overlap and no direction is priced by two of them.

Neither is an accident of the current painters; both are structural, and
(1) is what a future direction-dependent `kray` here would break. The
`BuildLobeSet` contract block in `TranslucentSPF.h` says so.

### 2.2 `value` cancels the cosine analytically

Every lobe's axis is `+n` or `-n`, so on its own support
`Dot(w, axis) == |cos(w, n)| > 0`. `EvalLobe` therefore returns
`pdf / |cos|` directly — `INV_PI / P` for a cosine lobe and
`(N+1) cos^{N-1} / (2 halfArc)` for a Phong one — and never divides by a
vanishing cosine at grazing.

The cosine convention is the integrators': `LightSampler.cpp` multiplies
`brdf.value` by `Dot(vToLight, ri.vNormal)` (its magnitude under
`ScattersFullSphere`), and `BDPTUtilities::GeometricTerm` takes `fabs` of
both cosines.

---

## 3. The entry-vs-exit bit: `IBSDF::valueStateful`

`Scatter` and `Pdf` take the side from `ior_stack.containsCurrent()`;
`value` had no stack and classified by geometric sign tests, which is
DL-157(b) — at a double-sided exit hit the two landed in different
branches.

A new DEFAULTED pair of virtuals, `IBSDF::valueStateful{,NM}`, forwards to
`value`/`valueNM` for every other BSDF in the tree (no other implementer
changes) and is overridden by `TranslucentBSDF`. The stackless
`value`/`valueNM` are a null-stack call to the same body, so the two can
never be two code paths. Callers that hold the live stack now hand it
over: `LightSampler`'s six NEE arms (via the `pMisIorStack` DL-74 P2
already threaded) and `PathVertexEval::EvalBSDFAtVertex{,NM}` (via
`BuildVertexIORStack`, from `BDPTVertex::insideObject`), and so do
`PathTracingIntegrator`'s HWSS companion fallback and the three legacy ops
that already take one as a parameter (`FinalGatherShaderOp`,
`AreaLightShaderOp`, `AmbientOcclusionShaderOp`) — see §9 for the full
list and for how the first pass of that audit missed them. Callers that
genuinely have none — `PointSetOctree`'s SSS irradiance cache, the
interactive preview — keep calling `value` and get a GEOMETRIC inference
of the side, exact for a closed object and for a double-sided mesh (the
`UnflippedGeomNormal()` recovery is what makes the latter true).

### 3.1 The lobe frame, derived per side — and the assertion that was wrong

> **This section was rewritten by review round 1 (2026-09-18). Its first
> version derived the frame from the SIDE alone and asserted, three times
> over, that a leaving ray paired with a not-inside stack is
> self-contradictory and "no real walk produces" it. That is false, and
> the assertion was hiding a 26 % error on a first-class authoring case.**

`Scatter` derives its frame like this (read it, ~:760):

```
geomNRaw = UnflippedGeomNormal()          // the TRUE outward normal, DL-70
geomN    = Dot(geomNRaw, ri.ray.Dir()) < 0 ? geomNRaw : -geomNRaw   // RAY-anchored
bEntering = !ior_stack.containsCurrent()                            // STACK

ENTRY  front reflection  clip = geomN     transmission  clip = -geomN
EXIT   backscatter       clip = geomN     diffuse exit  clip = geomNRaw,
                                          axis = OrientedExitNormal(n, geomNRaw)
```

So the side comes from the stack and the frame from the ray — and the two
are NOT redundant.

**The record where they disagree, and both are right.** A camera ray
striking the BACK FACE FIRST of an open double-sided sheet has a
not-inside stack (true — the walk never entered anything) and a leaving
ray (true — it really did hit the back face). This is not a corner:
`clippedplane_geometry`'s `doublesided` defaults to TRUE
(`ChunkParserRegistry.cpp`), non-indexed `TriangleMeshGeometry` and
`BezierPatchGeometry` behave the same way, and translucent is the
material authors put on open sheets (DL-46 review round 3(c)). Measured
on that record with a side-derived frame, gate 1's
`E[kray]/E[value*cos/pdf]` at tilt 0: **1.260222** on the front-reflection
lobe and **2.336741** on the transmission lobe, where both must read
1.000000 — and the FRONT-face control on the same sheet reads 1.000000 in
both builds, which is what says the record and not the sheet is the
problem. The support gate cannot see it, because the two lobes tile the
sphere and an inverted frame merely swaps which lobe prices which
half-space.

**Why a pure ray anchor is not the answer either.**
`PathVertexEval::EvalBSDFAtVertex` rebuilds a record as
`Ray(vertex.position, -wo)`, and the two BDPT generators pass their
`(wi, wo)` in OPPOSITE roles — the eye walk as
`(scatDir, -currentRay.Dir())`, so the rebuilt ray IS the incoming
segment, and the light walk as `(-currentRay.Dir(), scatDir)`, so it is
the REVERSE of the outgoing one. On the light side a ray-anchored EXIT
frame puts BOTH exit-branch lobes in the same half-space and leaves the
interior direction the walk is actually asking about with no lobe at all,
so `f == 0` and `GenerateLightSubpathImpl`'s `PositiveMagnitude(f)` gate
kills the walk: `TranslucentIORStackTest`'s BDPT light rows measured
`reached=512 -> 0` on every mode.

**The rule, split by side, each half derived from what `Scatter` does.**

```
bEntering = stack ? !containsCurrent()
                  : (bProvablyNoInterior || Dot(geomNRaw, rayDir) < 0)
geomN     = bEntering ? (Dot(geomNRaw, rayDir) < 0 ? geomNRaw : -geomNRaw)   // Scatter's own expression
                      : -geomNRaw
```

(Review round 3, P2-2: this block read `bOpenSheet` in the round-2
revision of this document, three lines above prose saying that flag is
not the one. The shipped rule is above; `bOpenSheet` is never consulted
here.)

* **Entry** uses literally `Scatter`'s expression, so the back-face-first
  record matches exactly.
* **Exit** uses `-geomNRaw`, and that is not an approximation: `Scatter`'s
  own `geomN` IS `-geomNRaw` at every record its exit branch can be
  reached with, because that branch requires
  `ior_stack.containsCurrent()` — the walk is inside the object — and a
  ray inside an object travels outward at its boundary hit. It is also
  ray-INDEPENDENT, which is the property that survives the rebuild. (The
  one record where the identity fails is a stack claiming "inside" for an
  entering ray — the open-sheet parity failure DL-76 tracks, where the
  exit branch has no meaning to begin with.)
* **Stackless** callers infer the side. That is exact for a closed
  object; on a sheet it additionally asks `ri.bProvablyNoInterior`.
  **`ri.bOpenSheet` is NOT that flag, and review round 2 measured what
  asking it costs.** For the two mesh classes `bOpenSheet` means
  *uncertified*: `TriangleMeshGeometryIndexed` sets it whenever DL-143's
  build-time weld could not certify watertightness — all four glTF assets
  DL-143 audited, every one a closed solid — and the non-indexed twin
  sets it on every double-sided hit. A round-2 revision used it here, and
  at a genuine interior EXIT on such a mesh the stackless path then
  priced the ENTRY lobes: gate 1's `dsMeshExit` stackless rows read
  **1.745299** (diffuse exit) and **0.647376** (backscatter) at tilt 0,
  against 1.000000 with the certification flag. Re-run against the
  round-3 suite (which adds the `bezierExit` record below) that A/B reads
  **1066/48 vs 1114/0**, the 48 being the `dsMeshExit` AND `bezierExit`
  stackless rows, which carry the identical ratios because they are the
  identical record.
  `ri.bProvablyNoInterior` is set only by a geometry whose SHAPE forbids
  an interior, and **round 3's P1 cut that set to exactly one class**:
  `ClippedPlaneGeometry` — four corners spanning one bounded bilinear
  sheet, which cannot enclose a volume however it is transformed, so the
  claim needs no build-time check and cannot be wrong. **Two kinds of
  geometry can never set it.** A MESH cannot, because "not certified
  closed" is not "certified open." And neither can a geometry holding a
  COLLECTION of primitives, however interior-free each primitive is on
  its own — an interior is a property of the whole surface, and N open
  sheets can bound a volume no single sheet can. Round 2 stamped the flag
  in `BezierPatchGeometry` on the premise that "a single patch cannot
  enclose a volume"; the class is not a single patch. `patches` is a
  `BezierPatchList` (a `std::vector`) behind a BSP/Octree, `AddPatch`
  appends, and `Job.cpp`'s `.bezier` loader puts EVERY patch of a file
  into ONE geometry — `models/raw/teapot.bezier` declares 28, and
  `models/bezier/aphrodite.bezier` and `f16.bezier` are closed solids.
  At a genuine interior exit on such an object `Dot(N, rayDir) > 0`, the
  patch normal flips, and the round-2 stamp therefore asserted "no
  interior" on precisely the hit that disproves it; the stackless arm
  then priced the ENTRY lobes at an exit. (A second reason the flip
  cannot carry a topological claim: `RayElementIntersection`'s own
  comment says the flip exists because patch WINDING varies inside one
  file — the teapot traverses some patches CCW and others CW.) The
  setter was removed and the new `bezierExit` gate row measures what it
  cost: **1.745299** (diffuse exit) / **0.647376** (backscatter) at tilt
  0 stackless, bit-for-bit the `dsMeshExit` pair, 1089/25 against
  1114/0. This was LATENT — no shipped scene binds
  `bezierpatch_geometry` to `translucent_material` — but as authorable as
  the clipped-plane case, and `sms_teapot_close_sms.RISEscene` already
  drives a Bezier teapot through `ManifoldSolver`'s four stackless sites.
  Round 5 explicitly clears the flag in `CSGObject::IntersectRay`
  after whole-record copies and payload adoption: `bOpenSheet` is a property of the
  SURFACE and survives compositing, this is a property of the OBJECT, and
  a CSG tree built from planes can have an interior. Omitting the field
  from `AdoptCsgSurfacePayload` alone did not clear the earlier
  `ri = riObjA/B` copy. `DisplacedGeometry` is conservative too: it
  constructs its tessellated hit without inheriting the base geometry's
  certification, so even a displaced clipped plane uses the uncertified
  fallback. This can retain DL-223's stackless sheet limitation; it cannot
  falsely certify a closed object. An open CSG wrapper also loses the
  primitive certification even if its particular surface is still open;
  it shares that conservative stackless-side limitation. The CSG class
  does not prove the topology of each assembled composite.

**Round-5 CSG regression (2026-09-19).** A live union hit from a
back-facing double-sided plane, with the other operand off the ray,
exercises the whole-record copy in both operand orders. It deliberately
isolates certification propagation; it does not certify a closed volume
for that fixture. The consumer gate supplies an inside stack and sweeps
six tilts through RGB/NM and both evaluator entry points. Before clearing
the flag, 24 stackless lobe checks and two operand-order certification
checks fail: **1196/26**. After clearing, **1222/0**. At RGB tilt zero,
exit/backscatter ratios move **1.745299 / 0.647376 -> 1.000000 / 1.000000**;
stacked rows remain 1.000000. Both states explicitly relinked
`build-test/TranslucentLobeConsistencyTest` after their library state was
built; library and individual test builds emitted zero warnings.

**What is genuinely left, stated precisely — two residuals, both on
DL-223, both bounded.**

1. *A LIGHT-subpath ENTRY vertex* evaluated through the rebuilt record
   gets the ray-anchored frame computed from `-scatDir`, which is
   inverted relative to the walk's own incoming segment. It does not kill
   the walk (the inverted transmission lobe still has support at the
   queried direction) and it is not new — the pre-DL-157
   `GetReflectedSide` was equally ray-anchored — but it is the
   caller-convention half of DL-223.
2. *A STACKLESS caller on a double-sided MESH open sheet struck
   back-face-first* cannot be told apart from that same mesh's interior
   EXIT, because a mesh cannot certify. Gate 1's `meshBack` stackless
   rows read **1.680295** (front reflection) and **1.001460**
   (transmission) at tilt 0 — printed and bounded as a KNOWN-RESIDUAL
   row rather than gated at 1, so its closure is as visible as a
   regression. **The current affected set is 16 calls in eight files** — the round-2
   list stopped at the families this slice had already converted or
   inspected and never swept the photon-map layer. The full list, grouped
   into ten rows (16 calls, eight distinct files):

   | # | site | note |
   |---|---|---|
   | 1 | `TranslucentPelPhotonMap::RadianceEstimate` (`TranslucentPelPhotonMap.cpp:91`) | its shader op HAS a stack; the `IPhotonMap` interface does not carry one |
   | 2 | `GlobalPelPhotonMap.cpp:97` | global photon gather, RGB |
   | 3 | `GlobalSpectralPhotonMap.cpp:82` | global gather, spectral XYZ arm |
   | 4 | `GlobalSpectralPhotonMap.cpp:135` | global gather, NM arm |
   | 5 | `CausticSpectralPhotonMap.cpp:82` | caustic gather, spectral XYZ arm |
   | 6 | `CausticSpectralPhotonMap.cpp:137` | caustic gather, NM arm |
   | 7 | `PhotonMap.h:705` | the shared templated gather |
   | 8 | `PointSetOctree.cpp:271` / `:287` | SSS irradiance cache (two calls, one family) |
   | 9 | `InteractivePelRasterizer.cpp:322` / `:323` / `:368` | preview (three calls, one family) |
   | 10 | `ManifoldSolver.cpp:5684` / `:5798` / `:6624` / `:8085` | the four SMS sites |

   `FinalGatherShaderOp.cpp:220` was also on this list and is
   **fixed in round 3 instead of recorded**: the same file's primary
   gather arm at `:521` was converted to `valueStateful` by this slice
   while the gradient-estimator arm was left behind, with the live
   `ior_stack` already a parameter of that helper. Final gather IS a production path, so the round-2 sentence
   "none of them a production integrator path" was wrong as written; it
   does not describe the remaining photon-gather and SMS calls.
   `PathValueOps::EvalBSDF` has no production caller; its two callers are
   test coverage.

   The MODERN integrator paths — PT NEE, BDPT/VCM connections, the
   zero-exitance sweep and PT guiding — are all STACKED and correct on
   both records.

The rule above is the one that is right for PT / NEE and for the eye
subpath, both of which hold a record whose ray IS the incoming ray.

---

## 4. `ScattersFullSphere()` — derived, not chosen

`TranslucentMaterial::ScattersFullSphere()` is now `true`. Both Phong
lobes live BELOW the shading horizon, so without it `LightSampler.cpp`'s
three NEE arms `break` at `cosSurface <= 0` and never light them, while
PT's own BSDF-sampling side still multiplies its below-horizon emitter
hits by `w_bsdf = PowerHeuristic(p_b, p_l) < 1` — the two strategies then
sum to less than 1 over the whole transmissive half-space and the
estimator reads systematically UNDER. BDPT/VCM never had that gate
(`PathVertexEval::EvalBSDFAtVertex` has no hemisphere test and
`BDPTUtilities::GeometricTerm` takes `fabs` of both cosines), which is
exactly the PT-vs-BDPT asymmetry DL-157 recorded.

The alternative the row offers — gate BDPT's connections into the interior
lobes to match PT — was rejected on derivation: it throws away a valid,
lower-variance strategy AND leaves PT's own partition broken, because the
BSDF-sampled side still discounts against an NEE arm that never fires.

The capability's own safety condition (`IMaterial.h`, and
`LightSampler.cpp`'s FULL-SPHERE NEE block) is that the material's
`value()` must really transmit and its aggregate `Pdf()` must really have
support there. Both became true in the SAME slice: DL-157 for `value`,
DL-41 for `Pdf`. Granting it before those would have lit back faces at
full weight against a zero partner density.

### 4.1 `CompositeMaterial` has to forward it (review round 1, P2-1)

`CompositeMaterial::GetBSDF()` returns the TOP material's BSDF when it
has one, else the BOTTOM's — and that is the `value()` NEE calls.
`FabricMaterial` and `CoatedMaterial` both forward
`ScattersFullSphere()`; `CompositeMaterial` did not, so
`composite { top = translucent }` — which is `mat_wax_gold` in
`scenes/Tests/Materials/composite_material.RISEscene` — presented a
transmitting `TranslucentBSDF` to a `bFullSphere == false` NEE and lost
the whole transmissive half-space, exactly the under-reading §4 derives.

It forwards from **whichever layer's BSDF the composite presents**, not
an OR over both: a Lambertian TOP over a translucent BOTTOM presents the
LAMBERTIAN BSDF, which does not transmit, and granting the flag there
would light its back faces at full weight — the failure mode
`IMaterial::ScattersFullSphere`'s own doc warns about. Both directions
are gated (`TranslucentLobeConsistencyTest` gate 6).

Rendered, isolating that one file (n = 3, same hygiene as §6):

| | forwarding OFF | forwarding ON | delta |
|---|---|---|---|
| `composite_material` PT | 1.041349 (1.6e-5) | 1.043799 (6.0e-6) | **+0.2352 %** |
| `composite_material` BDPT | 1.059352 (1.3e-5) | 1.059336 (7.1e-6) | -0.0015 % |
| PT/BDPT | 0.983006 (-1.699 %) | 0.985333 (-1.467 %) | |

BDPT does not move, as predicted — it never had the hemisphere gate — and
the PT-vs-BDPT disagreement narrows by 0.23 pp. The effect is small
because that scene's `ext 2 3 5` extinguishes most of the interior.

`SourceHygieneTest`'s closed claimer list grows to SIX and learns a fifth
claiming form (a cached member — `CompositeMaterial` does not retain its
layers, so it resolves the flag once at construction). A claimer that
matcher cannot SEE is worse than one it lists.

---

## 5. Red-proof

`tests/TranslucentLobeConsistencyTest.cpp` (new). Four gates over the real
`TranslucentMaterial`'s own SPF and BSDF, both pipes, both sides, six
shading-normal tilts (0/30/45/60/75/89 deg), chromatic reflectance and
transmittance, three rigs (`ext 0`; `ext 0.35` over a 2.0 interior
segment; `N 1` wide lobes with `scattering 0.6`):

| gate | statement |
|---|---|
| 1 | `E[kray_I] == E[value(w) * abs(cos(w,n)) / pdf_I]` per LOBE, over the SPF's own draws |
| 2 | full-sphere quadrature of `Pdf` vs the measured emission probability (MASS) |
| 3 | total variation between a histogram of directions `Scatter` + `RandomlySelect` produced and `Pdf` (SHAPE) |
| 4 | `Pdf(w) > 0` wherever `value(w) != 0` (the DL-74 partition side condition) |

**Review round 1 (2026-09-18) added three rigs and a sixth gate**, and
two of them found things:

* **[D], a PER-CHANNEL Phong exponent** (`RGBScalarPainter(5,10,15)`).
  Every round-1 rig built `N` from a `UniformScalarPainter`, so
  `Scatter`'s three-ray branch — four lobes in the set, a single-channel
  `kray` on each Phong ray, `RandomlySelect` weighing `MaxValue` of that
  — was entirely untested. It immediately failed at **0.664694** (entry
  transmission) and **0.736752** (interior backscatter), and the defect
  was in the TEST: reducing both sides by `MaxValue` independently is the
  DL-69 aggregate-over-per-lobe pairing in miniature, because `value` is
  a SUM and at lobe *i*'s direction the other two channel-lobes also have
  support. Reducing by the lobe's OWN channel (`argmax(kray)`) isolates
  its term and is numerically identical on every non-per-channel rig
  (`[A]` reads 0.500000 / 0.600000, ratio 1.000000, before and after).
  The rig now reads 1.000000 with `E[kray] = 1.300000 = tau_R+tau_G+tau_B`
  over `n = 120000` emitted rays, which is what says the three-ray branch
  is really being exercised.
* **[E], the records where the SIDE and the RAY disagree** — an open
  sheet's front face (control), a CLIPPED PLANE struck back-face-first
  (`planeBack`, which certifies "no interior" so even the stackless path
  is right), the same hit on a double-sided MESH (`meshBack`, which
  cannot certify — DL-223's stackless residual), a CLOSED but
  uncertified double-sided mesh hit from INSIDE (`dsMeshExit`, review
  round 2's P1-2 record), a live multi-patch Bezier back-face record
  (`bezierExit`), a live CSG plane-operand back-face record (`csgExit`,
  whose copied certification must be cleared), and an ordinary entry made while the walk is
  inside a DIFFERENT enclosure (`otherEnclosure`, which separates "the
  stack is non-empty" from "the stack contains US").
* **[F], gate 5**: `EvaluateKrayNM` against `ScatterNM`'s own `krayNM`
  (DL-222; see §10).
* **[G], gate 6**: `CompositeMaterial`'s forwarding, both directions
  (§4.1).
* **Both ENTRY POINTS on every gate-1 row** (review round 2, P2-2): the
  stackless `value(wo, ri)` and the STACKED
  `valueStateful(wo, ri, &stack)` that PT's NEE (`pMisIorStack`),
  `PathVertexEval::EvalBSDFAtVertex` and BDPT's zero-exitance row
  actually call. That entry point previously had no gate at all, which
  is what let round 2's mis-attributed red-proof stand.

**Isolated A/B** (`git checkout bb2ccd80 -- <the eight source files>`,
`make -C build/make/rise -j8 all`, then
`make -C build/make/rise build-test/TranslucentLobeConsistencyTest` — the
test target was rebuilt on BOTH sides, per COMMON_RULES' stale-binary rule):

```
round-1 file against bb2ccd80 :  Passed:  67   Failed: 173
round-1 file with the fix     :  Passed: 240   Failed:   0
round-2 file with the fix     :  Passed: 1008  Failed:   0
round-3/4 file with the fix   :  Passed: 1114  Failed:   0
round-5 file, copied CSG flag :  Passed: 1196  Failed:  26
round-5 file, flag cleared    :  Passed: 1222  Failed:   0
```

> **⚠ The headline pair is the ROUND-1 FILE against `bb2ccd80`, and it
> is quoted that way deliberately.** The current file cannot be built
> against `bb2ccd80` at all — it references `valueStateful`,
> `EvaluateKrayNM`, `ri.bProvablyNoInterior` and the five-argument
> `TranslucentBSDF` constructor, none of which exist there. Every
> number below is therefore a historical SINGLE-CHANGE A/B at the
> stated file revision, with the
> library and the test target rebuilt on both sides.

**Review round 2 (2026-09-18) found that round 2's own red-proof was
MIS-ATTRIBUTED, and the cause was a hole in this suite.** Gate 1
originally drove only the STACKLESS `value(wo, ri)`, while gates 2/3/4
drive the STACKED `Pdf` — so the entry point every modern PT/BDPT/VCM integrator
actually calls, `valueStateful(wo, ri, &stack)`, had no gate at all, and
numbers measured on it by hand were written down against a gate that
could not have produced them. P2-2 closed the hole: **every gate-1 row
now runs through BOTH entry points**, and gate 4 pairs the stacked `Pdf`
with the stacked `value` rather than crossing entry points. With that
done, the frame split's evidence lands where it belongs — on gate 1, on
the records where the side and the ray disagree.

**A/B 1 — the `geomN` split rule reverted ALONE** (to round 1's
side-derived `bEntering ? geomNRaw : -geomNRaw`), everything else held
(**round-2 test-file revision**):

```
side-derived frame :  Passed: 924   Failed: 84
split-by-side rule :  Passed: 1008  Failed:  0
```

| gate | record | entry point | side-derived | split rule |
|---|---|---|---|---|
| 1 | `planeBack` front reflection, tilt 0 RGB | stackless | 1.260222 | **1.000000** |
| 1 | `planeBack` transmission, tilt 0 RGB | stackless | 2.336741 | **1.000000** |
| 1 | `planeBack` front reflection, tilt 0 RGB | **STACKED** | 1.260222 | **1.000000** |
| 1 | `planeBack` transmission, tilt 0 RGB | **STACKED** | 2.336741 | **1.000000** |
| 1 | `meshBack` front reflection, tilt 0 RGB | **STACKED** | 1.260222 | **1.000000** |
| 1 | `meshBack` transmission, tilt 0 RGB | **STACKED** | 2.336741 | **1.000000** |
| 3 | `planeBack` TVD, tilt 0 RGB | (stacked `Pdf`) | 0.57972 vs a 0.08999 floor | 0.04–0.05 |
| 1 | `sheetFront` CONTROL, both lobes | both | 1.000000 | 1.000000 |

So the `1.260222 / 2.336741` pair IS real and IS reproducible — round 2
measured it by hand on the stacked path and then attributed it to a gate
that was stackless-only. It is now produced by a gated row, on both
entry points, and the gate-3 TVD rows the previous round's A/B actually
moved are listed alongside it.

**A/B 2 — the stackless side rule's flag, `ri.bProvablyNoInterior`
replaced by round 2's `ri.bOpenSheet`**, everything else held
(**round-2 test-file revision**):

```
bOpenSheet (round 2)     :  Passed: 984   Failed: 24
bProvablyNoInterior      :  Passed: 1008  Failed:  0
```

All 24 are the `dsMeshExit` STACKLESS rows — a CLOSED but uncertified
double-sided mesh hit from inside, which `bOpenSheet` misreads as an
open sheet and therefore prices through the ENTRY lobes:

| record | lobe | tilt | `bOpenSheet` | certification |
|---|---|---|---|---|
| `dsMeshExit` stackless | diffuse exit | 0 | 1.745299 | **1.000000** |
| `dsMeshExit` stackless | backscatter | 0 | 0.647376 | **1.000000** |
| `dsMeshExit` stackless | diffuse exit | 30 | 1.755305 | **1.000000** |
| `dsMeshExit` stackless | backscatter | 30 | 0.631976 | **1.000000** |
| `dsMeshExit` **STACKED** | both | all | 1.000000 | 1.000000 |

Gate 1, rig [A] (`ext 0`, `ref (.5,.3,.2)`, `tau (.4,.6,.3)`, `N 10`,
`scattering .3`), ratio `E[kray]/E[value*cos/pdf]` at tilt 0/30/45/60/75/89:

| lobe | pre-fix | post-fix |
|---|---|---|
| entry front reflection (RGB and NM) | 1.000000 at every tilt | 1.000000 |
| entry transmission, RGB | 6.003025 / 7.294478 / 13.699167 / 155.740005 / 79.450923 / 53.001392 | 1.000 |
| entry transmission, NM | identical to the RGB column to six digits | 1.000 |
| interior exit, RGB | 6.977667 / 7.139324 / 7.269674 / 8.085397 / ... | 1.000 |
| interior backscatter, RGB | 0.647376 / 0.677350 / 0.749798 / ... | 1.000 |

(The entry-transmission column reproduces the ledger row's own
`5.999 / 7.181 / 13.818 / 153.17 / 748.4 / 116.4` in shape; the two rigs
differ in painters and in the MC noise of a quantity that is wrong by two
orders of magnitude, so the high-tilt cells are not expected to agree
digit for digit.)

Gates 2/3/4, rig [A], entry side, RGB:

| | pre-fix | post-fix |
|---|---|---|
| `int Pdf` (full sphere) | 1.00000 | 0.99952 .. 1.00005 |
| measured emission probability | 1.00000 | 1.00000 |
| TVD(sampler, `Pdf`) | 0.54462 .. 0.54628 | 0.03203 .. 0.04585 |
| measured noise floor for that TVD | 0.06462 .. 0.08999 | same |
| directions with `value > 0` and `Pdf == 0` | 19296 .. 19178 (of 41472 probed) | 0 |

**The MASS gate is GREEN pre-fix**, exactly as DL-98/DL-99 warned: the
single-lobe `Pdf` integrates to 1 over the sphere while the sampler draws
from a two-lobe mixture. Gate 3 is what fails, and gate 3's tolerance is
SELF-CALIBRATING rather than a fixed number: a TVD over 4608 cells at
200000 draws has a large pure-multinomial floor (`~0.5*sqrt(2K/(pi N))`,
about 0.06 here), so the same draws are split into two independent halves
and `TVD(halfA, halfB)` MEASURES that floor. For pure noise
`TVD(full, exact) ~ C/sqrt(N)` and `TVD(halfA, halfB) ~ 2C/sqrt(N)`, so
the model-vs-sampler distance must come in at about HALF the halves'
distance; the gate allows 0.75 of it plus 0.004 absolute. Post-fix every
row reads almost exactly half its own floor, which is the signature of an
exact density.

---

## 6. Renders

All at `oidn_denoise FALSE`, `pixel_filter box`, EXR
`Rec709RGB_Linear`; each figure is a mean over repeats with its sample sd,
against an ISOLATED build of `bb2ccd80` (the eight source files reverted,
library rebuilt, restored and rebuilt afterwards).

### 6.1 The money fixture — a bump-mapped translucent sphere

A `sphere_geometry` (radius 1.2) carrying `translucent_material`
(`ref (.5,.3,.2)`, `tau (.4,.6,.3)`, `N 10`, `scattering .3`) and a
`relief_modifier` over a smooth `expression_function2d` uv field, lit by a
`lambertian_luminaire_material` quad overhead and outside the frame.
200x200, 256 spp, central 80x80 mean of R/G/B, n = 4.

**With `ext 0`** (so the DL-223 residual below is absent):

| relief scale | PT before | BDPT before | PT/BDPT before | PT after | BDPT after | PT/BDPT after |
|---|---|---|---|---|---|---|
| -0.20 | 0.085325 (1.2e-4) | 0.049373 (4.0e-5) | 1.728180 (+72.818 %) | 0.280357 (9.9e-5) | 0.241208 (1.6e-4) | 1.162301 (+16.230 %) |
| 0.00 | 0.076759 (2.9e-5) | 0.056682 (7.1e-6) | 1.354206 (+35.421 %) | 0.232551 (6.3e-5) | 0.254913 (1.8e-5) | 0.912276 (-8.772 %) |
| +0.02 | 0.078405 (6.3e-5) | 0.056915 (1.2e-5) | 1.377574 (+37.757 %) | 0.236529 (2.4e-4) | 0.253097 (4.6e-5) | 0.934538 (-6.546 %) |
| +0.05 | 0.084354 (8.2e-5) | 0.058128 (2.4e-5) | 1.451178 (+45.118 %) | 0.251048 (3.7e-4) | 0.249817 (7.8e-5) | 1.004926 (+0.493 %) |
| +0.20 | 0.085443 (1.1e-4) | 0.049426 (6.3e-5) | 1.728692 (+72.869 %) | 0.280731 (4.0e-4) | 0.241704 (1.3e-4) | 1.161464 (+16.146 %) |

At ZERO relief — the one cell with no shading-normal tilt at all — the
PT-vs-BDPT disagreement goes from **+35.421 %** to **-8.772 %**, a 4.0x
reduction in magnitude. Both integrators also move UP by 3.0x / 4.5x,
which is the two Phong lobes finally being priced at the weight the
sampler charges instead of at a `cos^10` lobe's.

### 6.2 The tilt term is NOT DL-157's — a control that refutes the DL-112 doc's attribution

`docs/DL111_DL112_TRANSMISSION_PUSH_GATES.md` §14.3 recorded a tilt-driven
PT-vs-BDPT gap on this fixture and called it "consistent with DL-157 —
BDPT connects into lobes `value()` misprices and PT mostly cannot — though
this measurement does not by itself attribute it". **It does not
attribute it, and the attribution is wrong.** Replacing the translucent
material with a plain `lambertian_material` and changing NOTHING else
(same sphere, same relief modifier, same light, same rasterizers, n = 3):

| relief scale | PT | BDPT | PT/BDPT |
|---|---|---|---|
| -0.20 | 0.040789 (1.3e-5) | 0.024948 (1.8e-5) | 1.634924 (**+63.492 %**) |
| 0.00 | 0.050316 (2.6e-6) | 0.050319 (5.8e-6) | 0.999956 (**-0.004 %**) |
| +0.20 | 0.040807 (1.4e-5) | 0.024991 (6.5e-6) | 1.632881 (**+63.288 %**) |

A Lambertian sphere has the same tilt-driven gap, of the same sign and
comparable magnitude, and is EXACT at zero relief. So `relief_modifier`
itself produces a large PT-vs-BDPT disagreement on any material, and the
tilt dependence in §6.1 is that artifact, not this row's. Opened as
**DL-224**.

This also means the only honest cell in §6.1 is the zero-relief one, and
that cell is where the fix is measured.

### 6.3 Extinction isolates DL-223

The same sweep with `ext 0.35` over the same geometry (so an interior
chord up to 2.4 gives `B` down to 0.43):

| relief scale | PT/BDPT before | PT/BDPT after |
|---|---|---|
| -0.20 | 1.653508 (+65.351 %) | 0.842630 (-15.737 %) |
| 0.00 | 1.189637 (+18.964 %) | 0.522062 (-47.794 %) |
| +0.02 | 1.216466 (+21.647 %) | 0.542438 (-45.756 %) |
| +0.05 | 1.317791 (+31.779 %) | 0.611240 (-38.876 %) |
| +0.20 | 1.657424 (+65.742 %) | 0.841227 (-15.877 %) |

At zero relief, post-fix, `ext 0` reads 0.912276 and `ext 0.35` reads
0.522062 — BDPT over-reads by `0.912276 / 0.522062 = 1.747` purely from
the extinction. That is **DL-223**: a BDPT/VCM connection evaluates the
exit side through a record rebuilt by
`PathVertexEval::PopulateRIGFromVertex`, whose ray origin IS the vertex,
so `distance == 0` and `B == 1`. PT's own NEE, which evaluates at a real
intersection record, sees the true `B` and is correct.

### 6.4 Shipped scenes

Four of the thirteen `scenes/` files that bind `translucent_material`, run
through scratch copies with the measurement-hygiene overrides above and an
EXR linear output; whole-image mean of R/G/B, n = 3:

| scene | before | after | delta |
|---|---|---|---|
| `cornellbox_bdpt_materials_pt` (PT, 512^2 / 128 spp) | 0.950649 (3.0e-4) | 0.946057 (2.8e-4) | **-0.483 %** |
| `cornellbox_bdpt_materials` (BDPT, 512^2 / 64 spp) | 0.940756 (3.7e-4) | 0.949113 (3.8e-4) | **+0.888 %** |
| `cornellbox_fg` (pixelpel + final gather) | 0.353221 (3.2e-4) | 0.353925 (1.3e-3) | +0.199 % |
| `sss` (pixelpel) | 0.068262 (2.8e-6) | 0.068388 (3.0e-6) | +0.185 % |

The first two are the same Cornell box with the same twelve materials
(one of them the translucent sphere) under the two integrators, so their
RATIO is the production-level statement: **PT/BDPT `1.01052` -> `0.99678`**,
i.e. the disagreement falls from +1.052 % to -0.322 %, a 3.3x reduction.
The two `pixelpel_rasterizer` scenes move by under 0.2 % — that rasterizer
is direct-only (DL-26), so only the NEE arm's `value` can move there.

---

## 7. The four gate suites that pinned the defective behaviour

None of these is a tolerance relaxation; each replaces a pin on a measured
defect with the statement the fix makes true.

**`TranslucentSpectralParityTest`** — "diffuse PDF excludes backscatter
hemisphere" and "entry reflection PDF excludes back hemisphere" were
DL-41's own symptom, and "exit evaluated density equals stored density"
assumed `Pdf` carries no selection weight. They now assert the
selection-weighted share (computed from the rays the sampler really
produced, not from a second transcription of the mixture formula) and
that the two hemispheres sum to one. **1918 checks, 0 failures** (was
1846 / 180).

**`TranslucentIORStackTest` / `TranslucentGuidedStackProbe.h`** — its rig
had `scattering 0`, so the exit branch emits ONLY the outward diffuse exit
lobe; there is no interior backscatter lobe at all. Pre-fix `value`
nonetheless returned `pRefFront * INV_PI` for an INWARD direction there
(case 1, a lobe that does not exist), and that phantom value is what let
the guided one-sample branch accept **371 of 512** inward candidates —
which the `substitutedIn > 0` controls were pinning. The rig now has
`scattering 0.3`, so those controls exercise DL-03's inward path against a
direction the material genuinely scatters into (`substituted_in`
**0 -> 295**). DL-43's "the substituted candidate's evaluated PDF is ITS
OWN" check keeps its form and gains the exit lobe's selection share
(exactly 0.7 for this rig). **ALL TESTS PASSED** (was 20 CHECK(S) FAILED).

**`SPFPdfConsistencyTest`** — the `Translucent` Part 2b domain-split pin
(0.44469 in a two-sided [0.40, 0.48] band) is retired for the ordinary
`SUBDENSITY_TOL` gate: the two shares now agree to **2.3e-4** (`Pdf`
upper-hemisphere share 0.555538 vs emitted 0.55531) at both incidence
angles, with full-sphere mass 1.00007 against an emission probability of
1, and chi2 mean 816.972 over 8 seeds (z = 1.27, dof 799). Part 2's
`int Pdf over the HEMISPHERE == 1` is not a full-sphere sampler's
contract, so `Translucent` joins a new `fullSphereSamplers` side list and
is gated by Part 2b's PAIR instead — which is strictly IMPLIED, since
`pdfIntegral == pdfUpperShare * fullSphereIntegral` and both factors are
gated. **All SPF PDF consistency tests passed.**

**`TranslucentDoubleSidedTest` sub-test 4** — the pinned lune fractions
(0.0000 / 0.1695 / 0.3381 at tilt 0/30/60) are UNCHANGED, and that is not
a null result: the mechanism changed from "classified into the wrong
branch, then clipped" to "neither exit-side lobe can reach the lune",
which is correct. The check that distinguishes the two is new — over the
whole sphere, `value` and the aggregate `Pdf` must have the SAME support
at this hit: **0/40000 disagreements** (pre-fix `value` priced the entire
reported-shading-normal hemisphere through cases 1/2 while `Pdf` covered
the exit lobe alone). **Passed: 53** (was 50).

**`PTGuidingMISPartitionTest`'s DL-74 P2 premise probe** had to be
retargeted for the second time in two rounds, and the reason is §3.1: its
(b) case made the premise on a SELF-CONTRADICTORY input (a ray that says
"leaving" with a stack that says "not inside"), which the side-anchored
lobe frame no longer treats as two different branches. The premise is
still true and is now made on something a walk can be in — a rig whose
entry side has NO front-reflection lobe (`ref == 0`, an authored pure
transmitter), where the entry side has density only BELOW the geometric
horizon while the exit side keeps its outward diffuse exit lobe, so the
sentinel really does return **0 where the live stack returns 0.109153**.
A second, weaker check was added for the general case: with
`scattering 0.2` the two branches' SELECTION shares differ and the
sentinel shifts a positive density (**0.109153 vs 0.174645**).
**99 passed, 0 failed.**

---

## 8. Cost

`Pdf()` and `value()` on the fixture of §5, 4096 random directions x 4000
reps, measured on an isolated build of each side:

| call | before | after |
|---|---|---|
| `Pdf` entry | 6.85 ns | 23.99 ns |
| `Pdf` exit | 7.62 ns | 30.05 ns |
| `value` entry | 28.69 ns | 24.68 ns |
| `value` exit | 26.75 ns | 31.05 ns |

`Pdf` is 3.5x-3.9x more expensive and `value` is unchanged within the
measurement's own spread. In ABSOLUTE terms all four stay in the tens of
nanoseconds, against `SchlickSPF::Pdf`'s ~690 ns (DL-67 Slice 0),
`IsotropicPhongSPF`'s ~490 ns (DL-98) and
`AshikminShirleyAnisotropicPhongSPF`'s ~2580 ns (DL-99) — this material's
aggregate needs no quadrature (§2.1), which is why.

Render-level, on the MAXIMAL case (the all-translucent sphere of §6.1,
PT, 200x200 / 256 spp), two separately built binaries INTERLEAVED,
n = 10 each, user+sys CPU:

```
before  9.018 s (sd 0.386)      after  8.672 s (sd 0.580)
        -3.84 %,  paired t = -1.87
```

Not significant, and the sign is "faster after". The extra `Pdf` cost is
tens of nanoseconds against a render that does far more work per sample;
`ScattersFullSphere` adds shadow rays PT previously skipped, and this is
the scene where that is maximal.

---

## 9. Sibling audit (`docs/skills/audit-by-bug-pattern.md`)

**The pattern in one sentence:** a stateful SPF whose `Scatter` branches
on the IOR stack has a `value`/`Pdf` pair that cannot see that branch, so
the two describe lobes the sampler does not draw.

| sibling | verdict | evidence |
|---|---|---|
| `SubSurfaceScatteringSPF` / `SubSurfaceScatteringBSDF` | **REFUTED as a mispricing** — it is the recipe's option (b), applied deliberately | `value` returns 0 unless BOTH directions are on the front face, with an in-source note that "the subsurface transport is handled by the diffusion profile in the integrator, not by BSDF connections"; `Pdf` likewise covers only the outside rough-reflection lobe and returns 0 from inside. `value` and `Pdf` therefore agree on support, so no MIS pair is mispriced. What is LOST is the interior strategy (a known coverage gap of DL-41's family, not a bias); out of this row's scope. |
| `GenericHumanTissueSPF`, `BioSpecSkinSPF` | **NOT IN SCOPE, confirmed** | `GetBSDF()` is null for both, so there is no `value` to misprice; DL-126 (2026-09-18) routes BDPT/VCM/MLT around them with a dedicated `nullBSDFContinuation` branch. |
| `DielectricSPF`, `PerfectRefractorSPF` | **IMMUNE** | both materials' `GetBSDF()` returns `0` (`DielectricMaterial.h:48`, `PerfectRefractorMaterial.h:47`); their lobes are delta. They DO read `containsCurrent()` in `Scatter`, but there is no evaluator to disagree with. |
| `CompositeSPF` / `CompositeMaterial` | **SAME FAMILY, already tracked as DL-24** | `CompositeMaterial::GetBSDF()` returns the TOP sub-material's BSDF (or the bottom's), while `CompositeSPF::Pdf` is a documented 50/50 placeholder and its transport is a random walk over both layers — so `value` and `Pdf` describe neither. Not fixed here. Note that a `composite_material` whose layer IS translucent now reaches `TranslucentBSDF::valueStateful` correctly for that layer's own lobes; the composite-level mismatch is unchanged and is DL-24's. |

One hop deeper, on the CONSUMED field rather than the producer: every
call site of `IBSDF::value`/`valueNM` was enumerated
(`grep -rn -e '->value(' -e '->valueNM(' src`), and the question asked of
each was "does this caller HOLD a stack?", not "is this caller important?"
— the first pass of this audit asked the second question and left four
sites behind that do hold one.

> **Review round 1 (2026-09-18) found this audit had asked the wrong
> question a SECOND time.** Round 1's own text already records that it
> first asked "is this caller important?" instead of "does this caller
> HOLD a stack?"; the review found four MORE sites that answer yes to the
> second — PT's three guiding `PTEvalBSDFAtSurface` candidate sites
> (which already hand the same `iorStack` to `PTEvalPdfAtSurface` one
> line away) and, through `ILight::ComputeDirectLighting{,NM}`, the
> zero-exitance light sweep. That virtual carried no stack at all, so
> `LightSampler`'s Step-1 arm and BDPT's mirroring `s == 1` row inferred
> the side geometrically while the other three NEE arms got the live
> stack. It now takes a trailing defaulted `const IORStack*` — the same
> pattern `bFullSphereReceiver` / `bVolumeReceiver` already established
> on it — threaded through all four light types, both pipes, and both
> call sites.

**Sites that hold a stack and now pass it (18):** `LightSampler.cpp`'s six
NEE arms (`pMisIorStack`, the DL-74 P2 parameter);
`PathVertexEval::EvalBSDFAtVertex{,NM}` (reconstructed from
`BDPTVertex::insideObject` via `BuildVertexIORStack`);
`PathTracingIntegrator.cpp`'s HWSS companion fallback, whose own `pSPF`
branch one line above already passes `iorStack` — leaving the BSDF
fallback stackless would have been a drift inside a single `if/else`;
`FinalGatherShaderOp`, `AreaLightShaderOp` and
`AmbientOcclusionShaderOp` (each takes `const IORStack& ior_stack` as a
parameter and simply was not using it here); PT's three guiding
`PTEvalBSDFAtSurface` candidate sites; and the three `brdf.value` /
`brdf.valueNM` calls inside `PointLight` / `SpotLight` /
`DirectionalLight` / `AmbientLight`, reached through
`ILight::ComputeDirectLighting{,NM}`.

Render-neutral, as expected — the geometric inference and the stack agree
at a closed-object hit: `cornellbox_fg` -0.166 % (against a run-to-run sd
of 0.37 %), `sss` -0.005 %, `cornellbox_bdpt_materials_pt` +0.044 %, n = 3
each.

**Sites with no stack, which keep `value` and the geometric inference**
(exact for a closed object, and for a geometry that PROVES it has no
interior — see §3.1)**:**
`CoatedBRDF`/`CoatedSPF`/`FabricBRDF` (delegating to a substrate, which
cannot be translucent — `CoatedMaterial`/`FabricMaterial`'s
`IsSupportedSubstrate` allowlists are Lambertian / OrenNayar / GGX /
Weave), `ManifoldSolver` (SMS; a translucent surface is not a specular
caster), `PointSetOctree` (the SSS irradiance cache),
`InteractivePelRasterizer` (preview shading), `TranslucentPelPhotonMap`'s
own gather, and `ILightManager::ComputeDirectLighting`'s forwarding loop
— which has no in-tree caller at all and whose comment now says the stack
is one of the three things it drops.

**And one implementer outside `src/`** (review round 2, P1-1):
`tests/LightBVHTest.cpp`'s `MockSpotLight` is the only `ILightPriv`
subclass in the tests tree, and it deliberately does not mark its
overrides `override` — so the round-1 `ComputeDirectLighting` parameter
left it abstract and the failure surfaced at three `new MockSpotLight`
sites rather than at the declaration. **`make -C build/make/rise all`
does not build tests, so a library-only build is not evidence that the
tree compiles**; the mock's own comment says it is the site to update
when that virtual changes, and this slice walked past it. A grep of
`tests/` confirms it is the only one: no other file subclasses `ILight`,
`ILightPriv`, `IBSDF` or `ISPF` in a way this slice's signatures reach
(the four test-side `IBSDF` subclasses are unaffected, because
`valueStateful{,NM}` are DEFAULTED).

---

## 10. Residuals opened

**DL-223 — a BDPT/VCM connection at a translucent vertex does not
reproduce the SPF's own transport weight.** Two measured sub-cases at the
same site:

1. *The interior Beer factor.* `PathVertexEval::PopulateRIGFromVertex`
   sets `ri.ptIntersection = vertex.position` and the caller builds
   `Ray(vertex.position, -wo)`, so `|ri.ray.origin - ri.ptIntersection|`
   is 0 and `B == 1` in every connection evaluation. PT's own NEE, at a
   real intersection record, sees the true `B`. Measured §6.3: BDPT
   over-reads by **1.747x** at `ext 0.35` on the §6.1 fixture; identically
   zero at `ext 0`, which is the default and what most shipped scenes use.
2. *The model is not reciprocal, and BDPT evaluates it with the roles
   swapped.* `f(a->b)` for `a` outside and `b` inside is the ENTRY
   transmission lobe (`tau`), while `f(b->a)` is the EXIT lobe
   (`B*(1-s)`) — different weights for the same pair of directions. The
   two BDPT generators pass `(wi, wo)` in opposite roles, so BDPT
   evaluates the one PT does not. Measured at zero relief, `ext 0`:
   **-8.772 %** at `N 10` and **+30.071 %** at `N 1` (the `N 1` rig makes
   every lobe direction-constant, so the residual is the SIDE asymmetry
   alone, not the Phong shape).

3. *The light-subpath ENTRY frame* (added by review round 1). §3.1's
   rule anchors the ENTRY side to `ri.ray.Dir()` because that is what
   `Scatter` does and because a side-derived frame is wrong on the
   back-face-first open-sheet record; at a LIGHT-subpath entry vertex the
   rebuilt record's ray is `-scatDir`, so that frame is inverted there.
   It does not kill the walk and it is not new (the pre-DL-157
   `GetReflectedSide` was equally ray-anchored), and it is the same
   caller-convention defect as (2).

DL-127 is the precedent worth citing on (2): there the implied BRDF's
non-reciprocity was the EVIDENCE that decided which of two disagreeing
sides was wrong. Here the non-reciprocity is in the MODEL itself and
neither side is wrong, which is why this is a residual rather than a fix.

All three need a change to what a connection is allowed to ask a
stateful, non-reciprocal BSDF — an adjoint-BSDF convention and a per-vertex incoming
segment length on `BDPTVertex` — which is materially larger than this row
and is deliberately not attempted here.

**DL-224 — `relief_modifier` produces a large, material-independent
PT-vs-BDPT disagreement.** §6.2: a plain `lambertian_material` sphere
reads PT/BDPT **0.999956** at `scale 0` and **+63.5 %** at
`|scale| = 0.20`, on the same geometry, light and rasterizers. This
refutes `DL111_DL112_TRANSMISSION_PUSH_GATES.md` §14.3's attribution of
the tilt-driven gap on that same fixture to DL-157.

---

### 10.0 Two deferred cases that are MISPRICINGS, not no-ops

Review round 2's P3 asked these to be stated honestly, because calling
them "no-ops" understates them: in both, `BuildLobeSet` still returns a
lobe set and `value`/`Pdf` still return a number — just the wrong side's.

* **`HasTrueGeomSide()` false** (`HairGeometry`, whose geometric normal
  is ray-derived, and a degenerate `vGeomNormal`). `geomNRaw` falls back
  to the shading normal, so an EXIT hit's `-geomNRaw` is `-n` and the
  exit lobe is built about the wrong axis. `Scatter` has the same
  fallback, so the sampler and the evaluator still agree with EACH OTHER
  — which is why no gate in this suite can see it — but both describe a
  lobe the surface does not have. `translucent_material` on hair is an
  unusual but not forbidden combination (DL-75's own verdict).
* **A stale "inside" stack at an inward re-hit.** If the stack claims
  containment for a ray that is entering (the open-sheet parity failure
  DL-76 tracks), the EXIT branch runs and prices the interior pair.
  `Scatter` does the same thing, so again the two agree; the pricing is
  simply of the other side's lobes.

Both are deferred, and both are recorded here rather than in a ledger
row because they are properties of `Scatter`'s own side/frame contract
that this row inherits rather than introduces.

### 10.1 DL-222 closed in passing

`DL-222` (opened on the concurrent `debt-dl125` branch, not in this
tree) asks for a `TranslucentSPF::EvaluateKrayNM` override so PT's and
BDPT's HWSS companion lanes stop falling back to DL-125's
`value*cos/pdf` reconstruction. `BuildLobeSet` makes the direct answer
nearly free, so it is implemented here: `ScatterNM` emits at most two
rays with DISTINCT types, so `rayType` alone identifies the lobe on
either side. Gated at **1e-12** against `ScatterNM`'s own `krayNM` with
the same wavelength as hero, over nine record kinds x six tilts x three
wavelengths (`TranslucentLobeConsistencyTest` gate 5) — the strongest
available statement, since the two are the same quantity asked twice.
**DL-222 CLOSED 2026-09-19 at integration.** The combined gate also
found that DL-125 throughput replay used a unit-length synthetic ray.
Both generators now store their live incoming distance and replay uses
it for Beer attenuation. Independent entry/exit and replay oracles are
in `HWSSCompanionKrayTest`; producer stamps are checked against live
sampler records by `TranslucentIORStackTest`. Unsupported scatter types
retain the diagnostic name. Ordinary connection reconstruction remains
zero-length (DL-223), unchanged by the replay fix.

---

## 11. Gate

| suite | result |
|---|---|
| `TranslucentLobeConsistencyTest` (new) | **1222 / 0** (round-5 CSG red: 1196 / 26; round-3/4: 1114 / 0) (round-1 file revision: 240 / 0, red 67 / 173 against `bb2ccd80`; round-2 file revision: 1008 / 0. Single-change A/Bs against the round-3/4 file: 1030 / 84 without the frame split, 1066 / 48 with `bOpenSheet` in place of the certification, 1089 / 25 with round 2's `BezierPatchGeometry` certification restored) |
| `TranslucentSpectralParityTest` | 1918 checks, 0 failures |
| `TranslucentTiltedExitTest` | ALL TESTS PASSED |
| `TranslucentEntryHorizonTest` | 251 checks, 0 failures |
| `TransmissionPushGateTest` | 416 checks, 0 failures |
| `TranslucentDoubleSidedTest` | 53 / 0 |
| `TranslucentIORStackTest` | ALL TESTS PASSED |
| `TranslucentInitialContainmentTest` | 43 / 0 |
| `TranslucentPhotonEnergyTest` | 14 checks, 0 failures |
| `TranslucentSamplerDimensionCountTest` | 65589 checks, 0 failures |
| `SPFBSDFConsistencyTest` | all passed (Part F 1.00000 at all six tilts, both pipes) |
| `SPFPdfConsistencyTest` | all passed |
| `PTGuidingMISPartitionTest` | 101 / 0 |
| `LightBVHTest` | 20 / 0 (**did not COMPILE** at the round-1 HEAD -- review round 2's P1-1) |
| `BDPTStrategyBalanceTest` | 123 / 0 |
| `VCMStrategyBalanceTest` | 74 / 0 |
| `PathValueOpsTest` | all passed |
| `CompositeExtinctionTest` | all passed |
| `GeomNormalOrientationSitesTest` | 72 / 0 |
| `PTGuidedSelectProbTest` | ALL TESTS PASSED |
| `LayeredWhiteFurnaceTest` | 0 of 58 configurations failed |
| `CstDeriveGoldenTest` | 452 MATCH, 0 DRIFT (459 corpus scenes, 0 UNCOVERED, 0 STALE) |
| `CstResolverTest` | 70 / 0 |
| `RasterizerDefaultsConsistencyTest` | 164 / 0 |
| `SourceHygieneTest` | 167 / 0 |

`make -C build/make/rise -j8 all` is warning-free on a clean rebuild.
