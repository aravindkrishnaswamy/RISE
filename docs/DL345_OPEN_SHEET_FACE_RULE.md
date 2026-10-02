# DL-345 + DL-339 (b): an open transmissive sheet is crossed by its face

Slice `debt-smsopen`, 2026-10-02, branched from `master` `d83c87dd3`.
Regression: `tests/OpenSheetIndexConventionTest.cpp`.

## 1. The problem

Since DL-372's split suppression (docs/SMS_ENERGY_LOSS_INVESTIGATION.md
section 7) the shipped OPEN displaced glass-sheet scenes read ABOVE PT
(`sms_k1_refract` PT+SMS / PT 1.086, `sms_k2_flatslab` 1.048), and a single
open ior-1.5 perfect-refractor sheet in front of an area emitter read 2.25x
with SMS on against off (DL-339 (b)).  Separately (DL-345), a slab built from
two open sheets read up to 3x apart between PT, BDPT and VCM while every
integrator agreed on the same slab as one closed box.

Both had one cause.  `DielectricSPF` and `PerfectRefractorSPF` decided
"entering" from the IOR stack alone (`!containsCurrent()`): a sheet not on
the walk's stack is ENTERED, whichever face the ray struck.  On a closed
solid that is the same thing as the face; on an open sheet it is not, and
the eye walk and the light walk then refract DIFFERENTLY at the same sheet:

- one sheet, receiver seen directly under it: the light walk enters through
  the front (bends toward the normal, going down); the eye walk from the
  receiver reaches the BACK, finds the sheet off its stack and also ENTERS
  (bends toward the normal, going up) -- not the reverse of the light's
  refraction, and priced `(1/n)^2` where the light's flux carries none;
- a slab of two sheets: each walk enters at the first sheet it crosses and
  treats the second as an index-matched entry, so the eye side bends at one
  sheet and the light side at the other.

SMS's seed walk had decided the crossing by its FACE (DL-290) all along.  So
SMS priced the open-sheet chains it covers with one convention and PT's
kept paths (and its old suppressed ones) used the other: the "SMS ~2.5x"
was never an SMS pricing defect.  The single-sheet fixture shows it
directly: PT+SMS reads 0.20261 BEFORE and AFTER this slice (identical to 5
digits); PT without SMS moved 0.09002 -> 0.20254.

## 2. The ruling

An OPEN sheet -- a surface that provably encloses no volume,
`RayIntersectionGeometric::bProvablyNoInterior` -- is an interface whose
FRONT side (the side its TRUE geometric normal points to) is the surrounding
medium and whose BACK side is the sheet's material.  Every crossing
refracts by its face: front -> back ENTERS (from the walk's medium to the
sheet's index), back -> front EXITS (from the sheet's index to the medium
beyond).  This is the only rule under which a walk and its reverse refract
the same way at every sheet, so PT, BDPT/VCM eye and light subpaths,
photons and SMS sample one path space.  Option (b) of the row ("author a
slab as a closed solid" plus a parser warning) was not needed: (a) makes
every integrator agree.

The stack is kept consistent where it can be
(`IORStackSeeding::ResolveOpenSheetCrossing`, shared by every site below):

- front hit with the sheet already on the stack (the walk left the back
  region around the sheet's edge): the stale entry is dropped, then pushed;
- back hit with the sheet on the stack: popped;
- back hit WITHOUT it (an "unpushed exit"): the stack top Y is either a
  sibling sheet the walk entered a slab through (popped) or a medium that
  encloses the crossing (kept), told apart by one ray against Y
  (`ObjectEnclosesCrossing`) -- exactly the SMS seed walk's DL-290 rule.

**Radiance factor.**  A radiance walk must pay `(eta_from/eta_to)^2` of the
refraction it actually performed; that, per crossing, is what makes a
radiance estimator agree with a flux (light-tracing) estimator whichever
side of the interface the endpoints sit on.  `RadianceEtaScale` reads
`before.top()`, which is the index the crossing refracted from except in one
case: an unpushed exit with no sibling (a receiver under a lone sheet, seen
directly -- the walk is "in air" by its stack and refracts out of the
sheet's glass).  There the SPF records the index it refracted from on the
stack it hands the transmitted ray (`IORStack::SetCrossingEtaFrom`), and
`RadianceEtaScale` reads that instead.  The field is NOT copied by the
stack's copy constructor or assignment, so it cannot reach a later vertex.

## 3. Which surfaces are "provably open"

- `ClippedPlaneGeometry`: every hit (it used to stamp the flag on back-face
  hits only, which no consumer needed beyond; `TranslucentSPF`'s stackless
  side inference -- the one prior reader -- reads the same answer on a front
  hit either way).
- `DisplacedGeometry` over a coplanar CONVEX clipped plane: every vertex is
  displaced along one (the plane's constant analytic) normal, so the mesh is
  the graph of a function over the quad -- an embedded disk.  This is the
  shipped SMS sheets' construction.  The base is re-asked per hit
  (`ClippedPlaneGeometry::IsPlanarConvexQuad`), since its corners can be
  keyframed.
- Nothing else (meshes, Bezier patch sets, CSG -- DL-157's rules).

## 4. Sites

| Site | Change |
|---|---|
| `PerfectRefractorSPF::DoSingleRGBComponent` / `ScatterNM` | face rule + resolved stack on flagged hits |
| `DielectricSPF` `Scatter` / `ScatterNM` (`bFromInside`, Beer) and `GenerateScatteredRay` | same; an unpushed exit pays the segment's Beer attenuation like any exit |
| `IORStack` / `RadianceEtaScale` | `crossingEtaFrom` (not copied) |
| `ManifoldSolver` seed walk (`SnellContinueChain`) | face rule on flagged hits (stale re-entry pops first); an unpushed exit refracts FROM the sheet's index -- the index `mv.etaI` records and Newton solves with (closes DL-290 review P3-1) |
| `SMSPhotonMap` chain flags | face rule on flagged hits |
| `RayCaster` transparent-shadow walk (opt-in) | already decided by face; on flagged hits its far side is now the resolved one (an unpushed slab exit used to read the sibling's index: one Fresnel instead of two) |
| `CompositeSPF` (all layer entry points) | the layers see the record WITHOUT the flag: a composite's internal crossings are keyed on the stacks its own walk builds, and keep their pre-DL-345 containment semantics on a clipped plane as on any surface |

Non-delta guided continuations (`PathTransportUtilities::
GuidedContinuationIORStack`) never see a dielectric (its lobes are delta),
and `BDPTVertex::insideObject` only rebuilds a stack for a non-delta `Pdf`.

## 5. Measurements

`tests/OpenSheetIndexConventionTest.cpp` (16 x 16, single worker, n = 4
salted renders per config; ratios `+/-` the independent-means sd):

| Row | master | this slice |
|---|---:|---:|
| single sheet PT+SMS / PT | **2.2506** +/- 0.0055 | 1.0004 +/- 0.0032 |
| single sheet BDPT / PT, VCM / PT | 1.0003, 1.0156 | 0.9974, 0.9979 |
| open slab / closed box, camera below: PT, PT+SMS, BDPT, VCM | **0.625**, 0.984, **0.625**, **0.634** | 0.986, 0.987, 0.985, 0.984 |
| same, displaced sheets | **0.624**, 0.984, **0.625**, **0.631** | 0.986, 0.987, 0.990, 0.987 |
| open slab / closed box, camera above: PT, PT+SMS, BDPT, VCM | **0.595**, **0.593**, **0.595**, **0.590** | 0.994, 0.991, 0.998, 0.987 |
| same, displaced sheets | **0.596**, **0.593**, **0.595**, **0.590** | 0.996, 0.991, 0.996, 0.985 |
| stacked open slabs / closed boxes (air gap), PT, PT+SMS, VCM | not measured | 0.993, 0.992, 0.988 |
| transparent shadows, omni, open / closed (flat, displaced) | not measured | 0.9998, 1.0002 |

(Single-sheet absolute values: PT 0.09002 -> 0.20254, BDPT 0.09005 ->
0.20202, VCM 0.09143 -> 0.20212, PT+SMS 0.20261 -> 0.20261.  In this
fixture BDPT/VCM agreed with PT pre-fix because their eye strategies carry
almost all of the MIS weight under a 4 x 4 emitter.  VCM's stacked rows sit
at ~0.6 of PT in both geometries: max_eye_depth 8 is below the 10 surface
vertices of the through-both-slabs path; the row is open / closed per
integrator.)

The ~1.4 % open-below-closed residual on the camera-below rows is the open
slab's side EDGES, not the convention -- see the flatslab six-sheet row.

**Shipped `sms_k2_flatslab`** (200 x 150, OIDN off, single renders: PT+SMS
1024 spp, PT 16384, BDPT 4096, VCM 4096; F = floor under the slab footprint
seen directly, 1026 px, S = slab seen from above, 1248 px, both masks from
emissive-patch renders with coverage > 0.99):

| | PT+SMS | PT | BDPT | VCM |
|---|---:|---:|---:|---:|
| F, open, master | 0.3042 | **0.1617** | **0.1680** | **0.5084** |
| F, open, this slice | 0.3179 | 0.3320 | 0.3175 | 0.3180 |
| F, closed box, this slice | 0.3367 | 0.3571 | 0.3396 | 0.3444 |
| F, SIX open sheets (a box of sheets), this slice | 0.3368 | 0.3519 (4096 spp) | -- | 0.3444 |
| S, open, master | 0.0778 | 0.0766 | 0.0713 | 0.0774 |
| S, open, this slice | 0.1347 | 0.1287 | 0.1242 | 0.1277 |
| S, closed box, this slice | 0.1287 | 0.1271 | 0.1273 | 0.1257 |

The four integrators now agree on the open slab, and the same slab built
from SIX open sheets (sides included) matches the closed box to 4 digits
under VCM (F 0.34435 / 0.34440, S 0.12590 / 0.12574): the remaining
open-vs-closed F gap (-6 to -7 % in every integrator) is the open slab's
missing side faces.  PT reads ~4-5 % above BDPT/VCM on F in BOTH geometries
and in master's closed box too (PT 0.3589 / BDPT 0.3449 / VCM 0.3443) --
pre-existing, single renders, not resolved here.  The S PT+SMS 0.1347 is
one multithreaded render; a repeat in the same build read 0.1304 (S PT+SMS
/ PT 0.994 there).

**Shipped open-sheet SMS scenes, whole image** (shipped PT+SMS rasterizer at
256 spp; PT 4096; VCM 1024; single renders):

| Scene | master PT+SMS/PT, /VCM, PT/VCM | this slice |
|---|---|---|
| `sms_k1_refract` | **1.0958**, 1.0441, **0.9528** | 0.9997, 1.0036, 1.0039 |
| `sms_k2_flatslab` | **1.0604**, 1.0347, 0.9758 | 0.9951, 0.9895, 0.9944 |
| `sms_k2_glassblock` | 1.0219, 0.9977, 0.9762 | 0.9992, 1.0082, 1.0090 |

**Scene hashes** (`WeaveGapShadowTransmittanceTest` `scenehash`: one worker,
pinned `srand`, salt 0, 8 spp, shipped resolution; master -> this slice).
Bit-identical: `diacaustic_pt_sms`, `sms_k2_glasssphere` (+ `_tess`,
`_tess_disp`), `sms_luminous_orb`, `sms_slab_close_pt_sms_hispp`,
`sms_slab_close_sms`, `sms_veach_egg`, `sms_veach_egg_bumpmap`,
`sms_visibility_occluded`, `sms_visibility_unoccluded`,
`sms_through_glass_emitter_pt_sms`, `spectral_dispersive_caustic_pt_sms`, and
the non-SMS `pool_caustics`.  Moved:

| Scene | mean master -> slice | |
|---|---|---|
| `sms_k1_botonly` | 0.182392 -> 0.178831 (-1.95 %) | open sheet |
| `sms_k1_refract` | 0.193081 -> 0.186993 (-3.15 %) | open sheet |
| `sms_k2_flatslab` | 0.193351 -> 0.183537 (-5.08 %) | open sheets |
| `sms_k2_glassblock` | 0.186601 -> 0.179202 (-3.97 %) | open sheets |
| `triplecaustic_pt_sms` | 0.649794 -> 0.649813 (+0.003 %) | seed walk (no open sheet) |
| `sms_k2_torus_cross` | 0.192334 -> 0.192334 (+1e-6) | seed walk |
| `sms_teapot_close_sms` | 0.332229 -> 0.332182 (-0.014 %) | seed walk |
| `sms_veach_egg_displaced` | 0.749161 -> 0.749161 (-3e-8) | seed walk |
| `pool_caustics_vcm` (non-SMS) | 0.0932245 -> 0.0932246 (+8e-7) | open water sheet |

The four "seed walk" scenes have no provably open sheet; they move because
an UNPUSHED exit in the SMS seed walk now refracts from the sheet's own
index for every geometry (the P3-1 consistency fix), which changes some
seeds and so Newton's iterates, not its roots.

**Shipped non-SMS scenes with an open transmissive clipped plane** (every
`standard_object` binding a dielectric / perfect refractor to a clipped
plane or a displaced one, whole corpus): `pool_caustics` (bit-identical),
`pool_caustics_vcm` (+8e-7), and the two underwater benchmarks
`dreamscape_coral_queens_hour` / `_v2`, whose camera sits UNDER a displaced
water-surface sheet.  The old rule ENTERED the surface from below (air ->
1.33, no Beer); the face rule EXITS it (1.33 -> air): Snell's window with
total internal reflection of the seabed outside it, and the surface's `tau`
charged on the underwater segment, as for a closed water body.  200 x 150,
16 spp, OIDN off, single CLI renders: **0.77356 -> 0.67650 (-12.5 %)** and
**0.74891 -> 0.65413 (-12.7 %)**.  This is a LOOK change of a shipped scene
(the supervisor rules on re-tuning, as for DL-320).

## 6. Residuals

See the DL-345 row and DL-382.  Not measured here: MLT (shares BDPT's
walks) and the spectral / HWSS integrators (the SPF NM paths carry the same
rule; no spectral row was rendered).
