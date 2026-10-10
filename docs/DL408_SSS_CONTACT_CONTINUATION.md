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

* **SSS exit (diffusion profile and random walk).**  The exit's own offset
  point q = p + n * BSSRDF_RAY_EPSILON is the point the defect puts inside
  a neighbour.  `IObjectManager::SolidObjectContaining(q, n, self)`
  (new, tail-appended) asks the same per-candidate question DL-370's
  `RandomWalkObjectContaining` asks -- box tree, then the candidate's
  signed distance or two-way crossing parity, winding-independent -- but
  without the random-walk material filter.  Only exits whose q lies in
  another object's world box pay it; the restricted cast then verifies the
  contact geometrically.  DL-370's in-walk crossing between random-walk
  bodies is kept: it runs first, inside the walk, and a walk only reaches
  this test once it has really left to the exterior.
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

## 4. Cost

Every SSS exit whose offset point lies in ANOTHER object's world box pays
one containment query (closed-form signed distance for analytic solids,
parity rays for meshes) and, on a yes, one single-object intersection;
exits in no other box pay one box-tree walk.  Contact-hit scatters pay one
single-object intersection.  Measured numbers: section 6.

## 5. Residuals (by design)

* An EMITTING neighbour: NEE from a contact exit samples the emitter's
  contact face from inside it (direction into S, Sw = 0) and its far faces
  through its interior; the continuation (a contact launch) carries the
  emission at MIS weight ~1.  Unchanged by this slice.
* Alpha coverage (DL-214) of the neighbour is not consulted by the
  restricted cast (a cut-out contact face is treated as solid).
* The legacy shader-op chain (frozen) and the photon tracers keep the old
  launch; SMS is untouched.
* Two touching shells of ONE object (the containment query excludes self,
  DL-370's same residual).
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
