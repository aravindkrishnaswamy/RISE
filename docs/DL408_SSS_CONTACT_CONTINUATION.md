# DL-408: an SSS exit against a coincident neighbour (contact continuation)

Slice `debt-dl408`, 2026-10-10, branched from `master` `ba133dd38`.  The
earlier passes (`debt-codex2`, `codex4`) measured the defect and named the
requirement; this one starts from a new design and implements it.

## 1. What a correct renderer does at a contact

Two bodies S (subsurface) and B (any material) share a face.  An SSS exit
point p lies ON that face.  Light that leaves S through p in direction d
(outward hemisphere) does not travel through any exterior: its very next
event is B's surface at p, met from outside B.  The physical limit is the
tiny-gap twin with the gap -> 0: exit S (its Fresnel against the exterior
the stack says it is in), travel zero distance, arrive at B's front face,
and B's own surface event decides what happens next (reflect back, enter B,
absorb).  If B reflects back toward S, the reflected ray again meets S's
front face at p after zero distance.  So the contact is a SEQUENCE OF
ORDINARY SURFACE EVENTS separated by zero-length segments -- not a new kind
of interface, not a profile-level hand-off.  Every material model RISE
already has (Lambertian, dielectric, diffusion profile, random walk, ...)
is then correct at the contact without knowing it is one.

Why the renderer cannot do this today: every continuation is a ray cast
from an origin offset off the surface (`BSSRDF_RAY_EPSILON` 1e-6 along the
outward normal for an SSS exit, `Advance(1e-8)` for a surface scatter) and
every primitive applies a scale-relative self-hit floor (DL-120: the floor
is correct and must stay).  At a contact, the offset origin is already
inside B (it misses B's near face, meets B's FAR face from behind, and
`bAbsorbBackFace` / the random-walk entry gate kill the path) and any
origin ON the plane puts BOTH bodies' roots under the floor.  A
whole-scene ray cannot tell S's face from B's at the same distance (DL-370
round 1's lesson).

## 2. The design: a contact launch is an OBJECT-RESTRICTED cast from behind

A continuation that must find a known neighbour T at its launch point is
cast against T ALONE, from an origin backed off along the continuation
direction into the launching body:

    o = p - d * delta,      delta = BSSRDF_RAY_EPSILON * (1 + max|p_i|)

`T->IntersectRay` from o finds T's surface at t ~= delta -- far above the
self-hit floor, and the launching body's own face is invisible because it
is not T.  The hit record is an ordinary hit: same direction d, backed off
toward o by the primitive's own `SURFACE_INTERSEC_ERROR`, i.e. on the
launching body's side exactly as a hit reached through a tiny gap would be.
It is accepted only if `delta/2 <= t <= delta + reach`
(`reach = 4 BSSRDF_RAY_EPSILON (1+s) / max(cos, 0.05)`), i.e. only if T's
surface really is at the launch point; otherwise the ordinary scene cast
runs unchanged (so a contact that does not cover the direction -- the rim
of a contact patch -- behaves exactly as today).

The hit carries `RayIntersection::pContactPartner` = the launching body.
That is the whole reverse direction: when ANY integrator continues from a
hit with a partner, and the continuation goes back to the side the hit was
arrived from (`dot(d_out, Ng) * dot(-d_in, Ng) > 0`), it is again a contact
launch, now restricted to the partner.  So
`S exit -> B (partner S) -> reflect -> S (partner B) -> S reflects -> B ...`
is a chain of restricted casts, each a zero-length segment, each priced by
the ordinary material at that hit.

Where T comes from:

* **SSS exit (diffusion profile and random walk).**  Every other body whose
  world box lies within the band's reach of the exit's boundary point p
  (`IObjectManager::ObjectsNearPoint`, new, a radius-widened sibling of
  the box-tree point walk) is a candidate: a touching solid, an OPEN sheet
  the exit lies on (a ground plane has no interior, so no containment test
  can see it), and any enclosure whose box merely contains p.  Each is
  asked by the restricted cast itself; the band is the arbiter and the
  NEAREST accepted hit wins.  A candidate whose interior overlaps the
  launching body at the shared face (it contains the backed-off origin o:
  `IObjectManager::ObjectContainsPoint`, DL-370's winding-independent
  sign / two-way-parity rule) is one of two NESTED configurations that
  look identical at p -- both interiors on the same side of the face:
  an ENCLOSURE floor (S rests on the inside face of T: a closed box room,
  a glass tank), whose surface really is there and must be met, and an
  INSET (T inside S, flush with S's face), whose surface is not a
  neighbour across the face (the continuation leaves both bodies).  No
  local probe separates them -- a point just outside S beside p is
  outside T in BOTH (the room's floor face is coincident with the
  block's bottom) -- so the test is the nesting itself: two closed bodies
  whose interiors overlap and whose boundaries only touch are nested, and
  nesting implies world-box nesting.  S's box inside T's (padded by the
  band radius) and not the reverse: an enclosure, accepted; otherwise
  (an inset, or boxes that do not tell: equal, or loose boxes under
  different transforms) the candidate is refused, as before.  DL-370's in-walk crossing between random-walk bodies is
  kept: it runs first, inside the walk, and a walk only reaches this test
  once it has really left to the exterior.

  *Review round 2 (2026-10-10) changed the refusal.*  Round 1 refused
  every candidate containing o, so a body resting on the INSIDE face of
  its own closed room (how Blender exports a room) lost the floor again:
  the reviewer's RW block on a closed `box_geometry` room's floor read
  contact/gap 0.9602 on both the base and round 1, against 0.9998 for an
  open-floor control.  Dropping the refusal was rejected: the inset is a
  legitimate nested configuration (a flush slab inside a wax block), and
  accepting it would reflect the exit back off a surface that is not
  across the face.  Both cases are rows of the contact test.

  *Review round 1 (2026-10-10) changed this.*  The first version asked
  for the FIRST body containing the exit's offset point; inside any
  enclosure (a glass or water box, a fog volume, a closed room mesh) that
  was the enclosure, whose far face failed the band, so the real neighbour
  was never tried and the fix was a no-op (RW | diffusion inside an
  index-1.0 glass sphere: contact/gap 0.926 PT, 0.927 BDPT, bit-identical
  to master).  It also could not see an open-sheet floor at all (RW cube
  on a clipped-plane floor over a black box: 0.928).  Both are now rows
  of the contact test.
* **Surface scatter at a contact hit.**  The partner recorded on the hit;
  no query.
* **Light emitted from a luminary in contact (BDPT / VCM / MLT light
  subpaths, review round 3).**  A light subpath sampled on an emissive
  pad under a wax block starts ON the block's bottom face; its emission
  ray is a contact launch exactly like an SSS exit
  (`BSSRDFSampling::FindSurfaceContact`, the same candidate search, with
  the luminary as the launching body).  Without it VCM's light-traced
  strategies saw the block through its bottom face and read 0.966 on the
  RW block on a pad (BDPT, whose eye path dominates there, read 0.996).

Why this and not the alternatives the earlier passes weighed:

* *Exclude the exiting object's root from a whole-scene cast*: needs an
  "ignore object X below t" argument through `IObjectManager::IntersectRay`,
  every acceleration structure and the CSG/alpha traversal; and whole-object
  exclusion is wrong for a later legitimate self-hit (codex4).  The
  restricted cast needs none of that: the only object that CAN be at the
  launch point is the one we already know.
* *Profile-level hand-off ("treat a covered exit as an entry into B")*:
  needs a per-pair model (diffusion -> Lambertian, diffusion -> dielectric,
  ...), and prices B's surface event outside B's material.  The contact
  launch reuses B's material unchanged.
* *Restrict the exit hemisphere / renormalise the profile*: wrong for a
  transmissive neighbour and biased for a reflective one (the light that
  B reflects back must re-enter S, not vanish or be redistributed).
* *Forced one-way hit (codex2's prototype)*: fixed S -> B but left B's
  reflection starting inside S (contact/gap 0.946 with a Lambertian).  The
  partner on the hit is exactly the missing return contract.

## 3. Transport consistency

* **NEE and connections at a contact (review rounds 2 and 3).**  At a
  contact exit the outward side is covered by the partner's surface at
  zero distance: in the gap twin every shadow ray or connection into it
  is occluded by the partner's near face.  The exit's own offset point
  (p + n BSSRDF_RAY_EPSILON) need NOT lie inside the partner, though --
  resting on the inside floor of a closed room it lies 1e-6 BELOW the
  floor, outside the room, where a shadow ray sees whatever is outside
  (round 1 relied on the partner occluding through its far side, which a
  solid neighbour does and an enclosure floor does not).  So the covering
  is explicit when the partner is OPAQUE and casts shadows
  (`BSSRDFSampling::ContactCovers`: not `ScattersFullSphere` -- translucent,
  fabric, weave, hair -- and no delta pass-through): PT computes the
  exit's contact before its NEE and zeroes that NEE (still evaluated, so
  the sampler stays in lockstep; its optimal-MIS training scale is 0, a
  counted zero); BDPT, VCM and MLT mark the exit vertex
  `BDPTVertex::contactCovered`, and every CONNECTION with it as an
  endpoint returns zero (s >= 1 and t >= 1: NEE, splat, interior
  connections, the zero-exitance sweep, VCM's NEE / splat / connections).
  The exit's other hemisphere carries Sw = 0, so the whole vertex is
  gated; the vertex stays connectible, so the MIS denominators keep the
  gap twin's structure.  A TRANSMITTING partner is not covered: real light
  crosses it (round 2 covered it and moved a translucent slab's BDPT /
  VCM rows from 1.030 / 0.896 to 0.978 / 0.859).

  Emission is not a connection.  An eye path that reaches an EMITTING
  partner by a contact launch (s = 0) is a real path and is never gated;
  round 2 gated it with the connections, and round 1 already lost it
  through the second mechanism below, so BDPT / VCM / MLT read EXACTLY 0
  for a block lit only by an emissive pad under it.

  **Positions follow the contact ray (DL-558, round 3).**  A strategy
  rebuilds directions and endpoint densities from vertex POSITIONS, and
  across a contact edge the stored positions lay on the wrong sides of
  the shared face (the exit 1e-6 past it, the hit 1e-12 on the arrival
  side): VCM's S0 rebuilt the pad's emission direction facing away from
  the block (Le = 0), and a Lambertian floor-contact vertex priced env
  light from below the floor (BDPT contact/gap 1.020 in a closed room).
  On every accepted contact launch the LAUNCHING vertex is now moved to
  the contact ray's origin -- delta behind the shared face, on its own
  side -- so positions agree with the walk's range and every rebuilt
  direction faces the way the ray travelled.  Not an UNCOVERED SSS exit:
  its connections still run from its offset point (moved inside its own
  body they would cross its own face), so DL-558 remains for those.
* **PT MIS.**  The continuation's density is unchanged (the same sampled
  direction); a contact hit is an ordinary hit, so emission weighting,
  bsdfMisPdf and the optimal-MIS training are untouched.
* **BDPT / VCM / MLT.**  The walk computes each new vertex's `pdfFwd` and
  the previous vertex's `pdfRev` from the RAY (direction and `range`), not
  from vertex positions, so a restricted cast's range delta enters exactly
  where the gap twin's ~1e-6 range does.  A strategy's ENDPOINT and its
  predecessor are re-priced from POSITIONS; since round 3 those agree with
  the range across a contact edge (the launching vertex sits at the
  contact ray's origin, above), except at an uncovered SSS exit (DL-558).
  Contact vertices are connectible as usual.  The SSS exit vertex is a
  subsurface-jump MIS barrier (DL-317 / DL-375), so no MIS walk evaluates
  the zero-length S-exit -> B edge from positions.  No zero-length density
  fold is needed.
* **Bit-identity.**  Nothing draws from the sampler.  A scene with no
  contact never takes the restricted cast (the containment query answers
  no, or the cast fails its band); its pixels are unchanged bit for bit.

* **Medium inside the back-off.**  The restricted cast's segment runs
  from o, delta inside the launching body, to the contact; the loop prices
  it in the medium the IOR stack says the ray is in (the exterior's).  A
  scatter there has probability ~sigma_t * delta = sigma_t * 1e-6 (1+s),
  the same order as the gap twin's own ~1e-6 segment in the gap; it is not
  corrected.
* **The band scales with COORDINATE magnitude, not object size.**  delta
  and the reach are `BSSRDF_RAY_EPSILON (1+s)` with s the launch point's
  largest absolute coordinate (the same scale every BSSRDF offset uses),
  so a millimetre contact 100 units from the origin gets a band ~100x
  wider than the same contact at the origin, and a large scene far from
  the origin can admit a candidate whose surface is a few 1e-4 away.
  The candidate radius (96 delta) scales the same way.
* **The cast is bounded.**  The restricted cast stops at the band's widest
  possible far edge (`delta + 80 BSSRDF_RAY_EPSILON (1+s)`, the 0.05 cosine
  floor), so a large candidate never pays a full closest-hit traversal
  (review round 2).
* **Fallback.**  If a PT continuation's restricted cast does not land in
  the band at cast time, `RayCaster::CastRay{,NM,HWSS}` fall back to the
  ordinary scene cast of the ray they were given -- the exit's own offset
  ray, never the backed-off origin inside the launching body (review
  round 2; the contact ray is rebuilt there from `RAY_STATE::contactPoint`).
  The PT loop and the BDPT walks already fell back from the walk's own
  ray.  `FindExitContact` verified the identical cast, so the fallback is
  reached only if the target's answer depends on rasterizer state.

## 4. Cost

Every SSS exit pays one radius-widened box-tree walk (radius 96e-6 (1+s)
around the exit point).  Each candidate it returns -- a body whose box
lies within that radius, including an enclosure whose box contains the
point -- costs one single-object intersection; only a candidate whose cast
lands in the band additionally pays the containment test at the backed-off
origin (a closed-form signed distance, or two parity casts for an
uncertified mesh).  An enclosure therefore costs one intersection per exit
and NO parity casts.  Contact-hit scatters pay one single-object
intersection.  Since review round 2 the single-object cast is bounded by
the band's widest far edge, and a candidate that contains the backed-off
origin additionally pays two bounding-box reads (the nesting test).
A light subpath sampled on a mesh luminary pays one contact query
(the same box walk) at its emission point (round 3).
Covering a contact's NEE costs nothing extra in PT (the exit's contact
query moved ahead of the NEE; the NEE is still evaluated) and one flag
test per strategy in BDPT / VCM.  Measured numbers: section 6.

## 5. Residuals (by design)

* An EMITTING neighbour (review round 3).  The continuation (a contact
  launch) carries the emission; NEE from the exit toward the pad is
  zeroed (covered), which loses nothing measurable: across a ~1e-6 edge
  the BSDF strategy's area density is ~1e12 times the light's, so its MIS
  weight is ~1 in both twins.  Rows: RW / diffusion block on a pad, PT,
  BDPT, VCM and MLT.  Was 0 in BDPT / VCM / MLT on the base and in rounds
  1 and 2 (section 6.3).
* A TRANSMITTING partner keeps a pre-existing bias that is not DL-408's:
  a RW block on a 0.1-thick `translucent_material` slab lit from below
  (block crop, 1024 spp, n = 8, contact vs a 1e-3 gap) reads BDPT
  1.0175 +/- 0.0030 se, VCM 0.9424 +/- 0.0054, PT 0.9989 +/- 0.0016; the
  base `ba133dd38` reads 1.0178 / 0.9484 on the same crop, i.e. the slice
  returns BDPT and VCM to their pre-DL-408 values.  The BDPT / VCM vs PT
  gap is the translucent model's non-reciprocity under connections
  (DL-223), present in the gap twin as well.
* Alpha coverage (DL-214) of the neighbour is not consulted by the
  restricted cast: a cut-out contact face is treated as solid.  An
  alpha-carded leaf or decal lying flat on an SSS body therefore reflects
  the exit's continuation off its transparent texels too, and (since round
  2) zeroes the exit's NEE there, where the gap twin's alpha-aware scene
  cast and shadow rays would see through the cut-out.
* A partner with `casts_shadows FALSE` (or a transmitting one) is still
  met by the continuation but does not cover: its exits keep their NEE
  and connections from the offset point, which is correct for a solid
  neighbour (the offset point is inside it and the gap twin's shadow rays
  pass it too) but leaks past an enclosure floor that casts no shadows;
  and the DL-558 position re-pricing applies there (an emissive partner
  that is uncovered keeps the S0 direction problem).
* Contact HITS are not gated at all; their predecessor's position makes
  every rebuilt direction face the right way, so a reflecting partner's
  far side reads zero BSDF and a transmitting one connects through itself
  as in the gap twin.
* The nesting test is a box test: an inset whose box is not inside the
  launching body's (a flush inset under a different, loose transform), or
  an enclosure whose box does not contain the body's, keeps the refusal.
* The legacy shader-op chain (frozen) and the photon tracers keep the old
  launch; SMS is untouched.
* Two touching shells of ONE object (the launching object is never a
  candidate, DL-370's same residual).
* More than 64 candidate boxes within the band's radius of one exit point:
  the rest are not asked (none in any shipped scene).
* `sdf_geometry` neighbour: the contact row reads 1.000 on master too --
  that RW | SDF-box pair shows no contact loss to recover, so it pins
  consistency only (the SDF's own image mean is 1.000 where the box and
  mesh variants read 1.037; not investigated here).
* MLT: its image mean is fixed by the bootstrap normalisation (contact and
  gap twins read the same mean to 1e-9), so the MLT row is a sanity row
  (renders through the contact code); MLT shares the BDPT walks the BDPT
  rows gate.
* An INWARD-wound double-sided mesh diffusion neighbour renders at 0.455
  of the furnace in both twins (master: contact 0.54 +/- 0.09, unstable);
  the contact row is 1.0007 after the fix, but the body itself is an
  authoring error (DL-370's pinned inward row).
* An index-1.0 glass ENCLOSURE changes the diffusion profile's own image
  (a diffusion cube alone 1.031 -> 0.975); pre-existing, independent of
  contact: DL-557.
* A ray inside a transparent DIELECTRIC neighbour that reaches the shared
  face (glass resting on wax) is not a contact launch: its exit continuation
  is an ordinary refraction from inside the dielectric; not changed.

## 6. Evidence

**Red / green** -- `tests/SSSContactContinuationTest.cpp`: each row renders
the touching scene and its 2e-6-gap twin under the white environment with
PAIRED Sobol salts, n = 8 per side; gate = |mean(contact - gap)| within 4 SE
of the paired differences (floor 0.2 %), plus the furnace value 1 for rows
whose materials all conserve energy.  Contact/gap ratio, master `ba133dd38`
library vs this slice:

| row | master | fix |
|---|---|---|
| RW \| diffusion, PT | 0.9034 | 0.9996 |
| RW \| diffusion reversed, PT | 0.9047 | 1.0008 |
| RW \| diffusion, BDPT | 0.9230 | 1.0006 |
| RW \| diffusion, VCM | 0.9285 | 1.0011 |
| RW \| diffusion, PT-spectral | 0.9032 | 0.9997 |
| diffusion \| diffusion, BDPT | 0.9928 | 0.9999 |
| diffusion \| diffusion, VCM | 0.9939 | 1.0006 |
| RW \| Lambert reversed, PT (contact furnace) | 0.99697 (0.99728) | 0.99990 (1.0003) |
| RW (wax) block on a Lambertian table, PT (contact furnace) | 0.99710 (0.99705) | 1.00004 (1.00008) |
| RW (wax) block on a Lambertian table, BDPT (contact furnace) | 0.99441 (0.99473) | 0.99974 (1.0000) |
| controls: RW\|RW both orders, RW\|Lambert forward PT/BDPT/VCM | 0.9988-1.0008 | 0.9993-1.0005 |
| diffusion block on a table, PT / BDPT (no significant loss pre-fix) | 0.9896 / 0.9994 | 0.9988 / 0.9997 |

Master: 12 of 18 rows red; fix: 18/0.  (Diffusion rows carry a direct /
indirect clamp of 4, identical in both twins: the profile is heavy-tailed on
a finite cube.  The PT diffusion|diffusion row stays heavy-tailed even so and
is gated only through its paired difference.)

**Bit-identity** (in-process, `force_number_of_threads 1`, `srand(4242)`,
salt 0, OIDN off, 8 spp, shipped resolution, FNV-1a over the image, master
vs fix): 11 of the 12 shipped scenes binding an SSS material hash
identical.  `vcm_sss_dragon` differs in 1744 of 172800 pixels by at most
9.4e-10 relative (mean identical to 9 digits).  No contact launch is
possible in that scene (walls and luminaire are clipped planes, which
refuse containment, and the dragon is the exit's own object); each binary
is self-deterministic; the difference is `-ffast-math` code generation in
the VCM path after the `RayIntersection` / `RAY_STATE` layout change.

**Cost:** user CPU, single thread, base/fix interleaved, n = 3: worst case -- a diffusion sphere submerged in a large dielectric box, so EVERY exit pays a containment query that answers yes and a restricted cast that is then rejected -- 22.33 +/- 0.16 -> 22.80 +/- 0.30 s (+2.1 %); shipped `rwsss_sphere` (no candidate box) 4.07 -> 4.11 s (+1.0 %); a wax and a skin block on a Lambertian table 8.14 +/- 0.17 -> 6.75 +/- 0.08 s (-17 %: a real transport change, paths no longer wander inside the table's interior).

### 6.1 Review round 1 (2026-10-10): enclosure, open-sheet floor, geometry rows

Contact/gap ratio, same harness (paired salts, n = 8), three libraries:
master `ba133dd38`, the first fix `26b9961f6`, this round.

| row | master | 26b9961f6 | this round |
|---|---|---|---|
| RW \| diffusion inside an index-1.0 glass sphere, PT | 0.9256 | 0.9277 | 0.9999 |
| same, BDPT | 0.9267 | 0.9253 | 0.9987 |
| RW block on an open clipped-plane floor over a black box, PT | 0.9278 | 0.9278 | 1.0003 |
| same, BDPT | 0.9274 | 0.9278 | 1.0009 |
| diffusion block on the open floor, PT | 0.9698 | 0.9833 | 1.0000 |
| RW \| diffusion, neighbour double-sided outward mesh, PT | 0.9029 | -- | 1.0015 |
| single-sided outward mesh | 0.9039 | -- | 1.0002 |
| INWARD double-sided mesh (body itself reads 0.455) | 1.186 +/- noise | -- | 1.0008 |
| uncertified mesh (one duplicated triangle) | 0.9710 | 1.0005 | 1.0014 |
| box rotated 90 deg | 0.9035 | -- | 1.0007 |
| CSG (box intersect big sphere) | 0.9047 | -- | 1.0015 |
| displaced (box, zero displacement) | 0.9047 | -- | 0.9996 |
| SDF box (no loss on master either) | 0.9997 | -- | 1.0000 |
| PT-spectral hwss TRUE | 0.9042 | -- | 1.0008 |
| MLT (sanity; image mean fixed by bootstrap) | render failed (no env; row rebuilt) | -- | 1.0000 |

Contact test: 33/0.  Master: 21 of 33 rows red, plus the MLT row, which
did not render there (its first version passed an environment map MLT
does not take; the row now lights the pair from a panel).  The rows that
stay green on master are the controls, the diffusion-table rows, the SDF
row and the noisy inward-mesh row (section 5).

**Bit-identity, re-run on this round** (same protocol as above, against a
fresh `ba133dd38` build): 10 of the 12 shipped SSS scenes identical.  The
two that differ are random-walk spheres resting on a clipped-plane floor
at a POINT contact (`rwsss_sphere`, mean identical to 9 digits;
`rwsss_bdpt`, 0.420399254 -> 0.420400973): exits within ~1e-2 of the
tangent point now take the contact launch to the floor, which is the
floor hit the old cast would have found from a slightly different origin.
`vcm_sss_dragon` is now identical (its earlier 9.4e-10 difference came
from a stale base build).

**Cost, review round** (user CPU, single thread, 3 libraries interleaved,
n = 3; the machine was shared, load ~25 on 18 cores, so per-run spread is
~10 %; paired per-rep ratios quoted):

| scene | master | 26b9961f6 | this round | this round / master, paired |
|---|---|---|---|---|
| diffusion sphere submerged in a dielectric BOX (every exit asks the box) | 21.49 +/- 2.31 s | 20.63 +/- 1.86 | 21.68 +/- 2.04 | 0.997 / 1.006 / 1.026 (+1.0 %) |
| same, the box an UNCERTIFIED double-sided MESH | 23.05 +/- 1.84 | 24.29 +/- 1.99 | 24.00 +/- 2.24 | 1.061 / 0.880 / 1.205 (inconclusive at this noise) |
| wax and skin blocks on a Lambertian table | 8.21 +/- 0.70 | 6.51 +/- 0.51 | 6.33 +/- 0.66 | -23 % (transport change) |
| shipped `rwsss_sphere` | 3.95 +/- 0.38 | 4.01 +/- 0.43 | 4.04 +/- 0.47 | 1.029 / 1.000 / 1.038 (+2.2 %) |

The enclosure now costs one single-object cast per exit and NO parity
casts (containment is asked only of a candidate whose cast lands in the
band), so the uncertified-mesh container pays one mesh intersection per
exit, not two parity walks.

### 6.2 Review round 2 (2026-10-10): rooms, covering, bounded cast

Contact/gap ratio, same harness (paired salts, n = 8), round 1 library
`be32378f7` against this round.  The room rows put a white environment
OUTSIDE the closed room, so light that leaks through the floor reads as a
GAIN (the reviewer's room, lit only from inside, read the same defect as
a loss: 0.9602).

| row | `be32378f7` | this round |
|---|---|---|
| RW block on the inside floor of a closed `box_geometry` room, PT | 1.6851 | 1.0016 |
| same, BDPT | 1.6837 | 0.9960 |
| same room as a closed double-sided indexed mesh, PT | 1.6886 | 1.0002 |
| same, BDPT | 1.6881 | 0.9986 |
| flush Lambertian INSET in a RW block (refusal half; consistency) | 0.9997 | 0.9999 |
| reviewer's harness, closed box room (no environment), PT | 0.9602 | 1.0002 |
| reviewer's harness, open clipped-plane floor, PT | 0.9998 | 1.0000 |

All inside their paired bands (BDPT room rows: band 2.6e-3 and 1.0e-3 on
a mean of 0.10).  The intermediate states are informative: with only the
nesting rule, PT read 1.033 and BDPT 1.054 (the exit's offset point lies
below the floor); with the exit covered, PT read 1.000 and BDPT 1.020 (a
Lambertian floor-contact vertex whose predecessor lies below it, both
BSDF arguments rebuilt from positions on the wrong side).  Contact test
38/0; every round-1 row still green.

**Bit-identity** (single thread, salt 0, 8 spp, OIDN off, against a fresh
`ba133dd38` build): 7 of 12 shipped SSS scenes identical.  `rwsss_sphere`
and `rwsss_bdpt` differ as in round 1 (point contacts).
`bdpt_sss_different_bsdf`, `bdpt_sss_dragon` and `vcm_sss_dragon` differ
in 5619 / 435 / 12570 pixels by at most 5.1e-10 / 1.0e-9 / 1.3e-9
relative, means identical to 12 digits: far below any transport change a
contact launch makes, attributed to code generation of the gated
connection code (not verified further).

**Cost, P2-1** (user CPU, n = 4 interleaved): a wax ball inside four
large tessellated index-1.0 glass shells (detail 96 each; every exit asks
four meshes whose boxes contain it): master 5.130 +/- 0.071 s,
`be32378f7` 5.135 +/- 0.118 s, this round 5.008 +/- 0.026 s -- equal
within noise; the unbounded single-mesh casts were not a measurable cost
there either, the bound removes the worst case for a candidate whose
closest hit is far.

**Correction (round 3).**  The bit-identity paragraph above attributed
its three ~1e-9 differences to code generation without checking; round 3
checked (section 6.3).

### 6.3 Review round 3 (2026-10-10): emitting partner, transmitting partner

**P1 -- an emitting partner.**  A block lit only by a 1 x 1 emissive pad
under it read EXACTLY 0 in BDPT / VCM / MLT on the base and in rounds 1
and 2 (PT was right since round 1).  Contact test rows (contact vs a 2e-6
gap, n = 8, final library):

| row | `c616c1b5f` | this round |
|---|---|---|
| RW block on a pad, PT | 1.0029 | 0.9971 |
| same, BDPT | render mean 0 | 1.0072 |
| same, VCM | render mean 0 | 1.0023 |
| diffusion block on a pad, PT | 0.9967 | 0.9973 |
| same, BDPT | render mean 0 | 0.9999 |
| same, VCM | 0.0008 | 1.0173 (band 2.2 %) |
| RW block on a pad, MLT (sanity) | render failed | 1.0008 |

The reviewer's own fixtures (block crop) read RW pad BDPT 0.9990 and VCM
0.9971 (n = 8; VCM read 0.966 before the emission ray became a contact
launch), the box emitter BDPT 1.022 +/- 0.020.  The reviewer's `_g`
twins for `lumfloor` / `lumpad` are 1e-3 gaps: the generator writes
`_g` (2e-6) and then `_G` (1e-3), and the macOS filesystem is
case-insensitive.  Against that 1e-3 twin the diffusion pad reads ~0.988
in PT and BDPT alike, a geometry difference, not a contact one.

The round-2 room rows on the final library: 0.9992 / 0.9969 / 0.9999 /
0.9968; the inset 1.0000.  Contact test 45/0.

**P2 -- a transmitting partner.**  RW block on a 0.1-thick
`translucent_material` slab lit from below, block crop, 1024 spp, n = 8,
contact vs a 1e-3 gap:

| build | BDPT | VCM | PT |
|---|---|---|---|
| `ba133dd38` (base) | 1.0178 +/- 0.0031 | 0.9484 +/- 0.0052 | -- |
| `c616c1b5f` (round 2) | 0.9831 +/- 0.0028 | 0.9089 +/- 0.0051 | -- |
| this round | 1.0175 +/- 0.0030 | 0.9424 +/- 0.0054 | 0.9989 +/- 0.0016 |

(se of the mean ratio, my block crop x 14..26, y 8..23; the reviewer's
own crop read base 1.023 / 0.913 and round 2 0.978 / 0.859 -- different
numbers, the same ordering.)  The covering is now restricted to opaque
partners, which returns BDPT and VCM to their base values; the remaining
BDPT / VCM vs PT gap is DL-223 (section 5).

**P3 -- bit-identity, verified.**  A struct-only rebuild (master plus the
three layout headers `RayIntersection.h`, `IRayCaster.h`, `BDPTVertex.h`,
every dependent recompiled; a first attempt was invalid because the
copied objects and the copied headers shared one mtime second and make
recompiled nothing) is bit-identical to master on all five scenes that
had differed.  Against it the final library leaves `bdpt_sss_dragon` and
`bdpt_sss_different_bsdf` bit-identical (round 2's differences there
came from gating contact hits, since withdrawn); `vcm_sss_dragon`
differs in 4451 pixels by at most 1.3e-9 relative, and master + layout
headers + ONLY the final `VCMIntegrator.cpp` reproduces that exact hash
-- its new branches are never taken there (no vertex is covered), so the
difference is code generation of those loops under `-ffast-math`.
`rwsss_sphere` (4 pixels, 6e-8) and `rwsss_bdpt` (2556 pixels, means as
in round 1) differ through real point contacts.

