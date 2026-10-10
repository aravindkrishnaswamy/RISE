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
  NEAREST accepted hit wins.  One refusal: a candidate that contains the
  backed-off origin o (`IObjectManager::ObjectContainsPoint`, DL-370's
  winding-independent sign / two-way-parity rule) lies on the LAUNCHING
  side -- an inset flush with the exit face -- and is not a neighbour
  across it.  DL-370's in-walk crossing between random-walk bodies is
  kept: it runs first, inside the walk, and a walk only reaches this test
  once it has really left to the exterior.

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

* **NEE.**  Unchanged and already consistent: from the SSS exit (offset
  into B) and from B's contact hit (backed off into S), every shadow ray
  into the covered hemisphere is occluded by the neighbour's far face,
  exactly as in the gap twin it is occluded by the neighbour's near face
  (shadow rays hit both faces).  Residual: an EMITTING neighbour seen from
  a contact exit (section 5).
* **PT MIS.**  The continuation's density is unchanged (the same sampled
  direction); a contact hit is an ordinary hit, so emission weighting,
  bsdfMisPdf and the optimal-MIS training are untouched.
* **BDPT / VCM / MLT.**  The walk computes each new vertex's `pdfFwd` and
  the previous vertex's `pdfRev` from the RAY (direction and `range`), not
  from vertex positions, so a restricted cast's range delta enters exactly
  where the gap twin's ~1e-6 range does.  Contact vertices are connectible
  as usual; every connection from one into the covered hemisphere is
  occluded (as in the gap twin).  The SSS exit vertex is a subsurface-jump
  MIS barrier (DL-317 / DL-375), so no MIS walk evaluates the zero-length
  S-exit -> B edge from positions.  No zero-length density fold is needed.
* **Bit-identity.**  Nothing draws from the sampler.  A scene with no
  contact never takes the restricted cast (the containment query answers
  no, or the cast fails its band); its pixels are unchanged bit for bit.

* **Medium inside the back-off.**  The restricted cast's segment runs
  from o, delta inside the launching body, to the contact; the loop prices
  it in the medium the IOR stack says the ray is in (the exterior's).  A
  scatter there has probability ~sigma_t * delta = sigma_t * 1e-6 (1+s),
  the same order as the gap twin's own ~1e-6 segment in the gap; it is not
  corrected.
* **NEE through the partner.**  NEE from a contact exit (its offset point
  lies inside the neighbour) and from a contact hit (backed off into the
  launching body) relies on the partner OCCLUDING every shadow ray into the
  covered hemisphere -- through its far side instead of its near face in
  the gap twin.  That holds for every shadow-casting body; a partner with
  `casts_shadows FALSE` behaves the same in both twins; the opt-in
  transparent-shadow dielectric walk would count one interface where the
  gap twin counts two (not measured).

## 4. Cost

Every SSS exit pays one radius-widened box-tree walk (radius 96e-6 (1+s)
around the exit point).  Each candidate it returns -- a body whose box
lies within that radius, including an enclosure whose box contains the
point -- costs one single-object intersection; only a candidate whose cast
lands in the band additionally pays the containment test at the backed-off
origin (a closed-form signed distance, or two parity casts for an
uncertified mesh).  An enclosure therefore costs one intersection per exit
and NO parity casts.  Contact-hit scatters pay one single-object
intersection.  Measured numbers: section 6.

## 5. Residuals (by design)

* An EMITTING neighbour: NEE from a contact exit samples the emitter's
  contact face from inside it (direction into S, Sw = 0) and its far faces
  through its interior; the continuation (a contact launch) carries the
  emission at MIS weight ~1.  Unchanged by this slice.
* Alpha coverage (DL-214) of the neighbour is not consulted by the
  restricted cast (a cut-out contact face is treated as solid).
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
