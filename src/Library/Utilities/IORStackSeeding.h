//////////////////////////////////////////////////////////////////////
//
//  IORStackSeeding.h - Seed a light / eye subpath's IORStack so that
//    `containsCurrent()` reports the correct inside/outside state
//    when the subpath's origin is physically inside one or more
//    dielectric (or otherwise refractive) objects.
//
//  Why this exists
//
//    BDPT (and integrators that reuse its light subpaths, e.g. VCM)
//    used to initialize the IOR stack to just the environment IOR
//    and rely on the stack to reflect the ray's current medium.  That
//    works for rays starting in free space — the stack grows as the
//    ray enters dielectrics and shrinks as it exits them.  It FAILS
//    silently for light subpaths whose origin is a luminaire sealed
//    inside nested dielectrics, and for eye subpaths whose camera
//    sits inside a dielectric (submerged camera, camera inside a
//    medium-filled room, etc.).
//
//    Symptom: the first hit on an enclosing boundary comes back with
//    bFromInside=false (the stack doesn't know we started inside),
//    which routes DielectricSPF through its "entering" branch.  For
//    an IOR-matched inner boundary (e.g. an `air_cavity` dielectric
//    with ior=1.0 surrounding a light in a glass egg), that branch's
//    Fresnel formula returns floating-point cancellation noise
//    instead of exact zero, the transmission lobe fails the sign
//    check that assumes refracted ≠ incoming, and the path is left
//    with a reflection whose kray equals that noise.  Throughput dies
//    by ~32 orders of magnitude per bounce and no photon reaches the
//    rest of the scene.
//
//  Algorithm
//
//    Shoot a probe ray in a fixed direction from the seed point and
//    count, PER OBJECT, the net parity of exits-vs-entries along the
//    probe.  An object contains the seed iff its net parity is
//    positive (with inward closed-solid winding normalized by signed containment;
//    more exits than entries — the probe must cross its
//    boundary one more time outward than inward to leave).
//
//    Counting exits-only is INCORRECT.  A probe that passes
//    THROUGH an object (camera in front of a glass sphere, probe
//    direction going +Z through it) sees one entry and one exit;
//    counting only the exit would treat the object as containing
//    the seed.  That bug caused every BDPT/VCM/MLT sphere render
//    where the camera sat behind a glass object to compute the
//    first refraction at the sphere with reversed IORs (glass→air
//    instead of air→glass), missing ~66% of the scene radiance.
//    See `tests/IORStackSeedingRegressionTest.cpp` for the pinning
//    regression test.
//
//    Per-object tallying:
//      - cosN > 0 (probe aligned with outward normal): EXIT, parity++.
//      - cosN < 0 (probe opposite to outward normal):  ENTRY, parity--.
//
//    A single probe only decides containment for a CLOSED surface,
//    so an object is seeded only when BOTH the probe and its REVERSE
//    report positive parity (P2-4).  A closed manifold agrees in both
//    directions by construction; an OPEN sheet (a translucent leaf,
//    curtain or card -- every `translucent_material` object is a probe
//    participant since DL-46) is met by only one of the two and is
//    rejected.  The reverse probe runs only when the forward one found
//    a candidate at all.  See the rule's documented limit at its
//    implementation in `SeedFromPoint`.
//
//    Hits whose geometric normal is RAY-DERIVED rather than a static
//    surface property (`RayIntersectionGeometric::bGeomNormalRayDerived`
//    -- `HairGeometry`) are skipped outright: un-flipping such a normal
//    reads "exit" at every crossing, so a hair curtain between the seed
//    and infinity would tally unbounded positive parity.  A 1-D curve
//    has no interior to be inside.
//
//    Objects confirmed in both directions are pushed in OUTERMOST-FIRST
//    order (stack convention: bottom = outermost, top = innermost).
//    Order is determined by each containing object's FIRST exit's
//    probe-step index: smaller index = closer to seed = innermost.
//    Insertion-sort by firstExitStep descending and push in that order.
//
//    A small iteration cap prevents pathological geometry (e.g.
//    coincident surfaces, self-intersecting objects) from spinning
//    forever.  Probe direction +Z is arbitrary but deterministic; any
//    fixed direction that isn't degenerate for the scene works, and
//    we accept the rare case where a probe grazes a tangent — the
//    worst outcome is a slightly-incorrect stack at that specific
//    pathological ray, which the rest of the path walk then corrects
//    as it enters/exits real boundaries.
//
//    The probe only considers materials whose GetSpecularInfo reports
//    canRefract=true OR hasInterior=true (DL-46).  Pure reflectors
//    (mirrors) and lambertian surfaces report neither and are skipped
//    — they are not media and their "interior" is not a place rays
//    travel through with a different IOR.  `hasInterior` covers a
//    second, narrower case: a material that is NOT specular (its
//    lobes are stochastic, never delta) and carries no distinct IOR
//    of its own, but still classifies entry/exit from
//    ior_stack.containsCurrent() exactly like a refractor does —
//    TranslucentSPF's diffuse lampshade model is the motivating
//    example.  For a `canRefract` object the probe pushes that
//    object's OWN captured `ior`; for a `hasInterior`-only object it
//    instead re-pushes whatever IOR is already on the stack (see the
//    push loop below), because such a material never introduces a
//    new numeric medium — only membership changes.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 23, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOR_STACK_SEEDING_
#define IOR_STACK_SEEDING_

#include "IORStack.h"
#include "GradedIndexMedium.h"
#include <cstdlib>
#include <cmath>
#include "../Interfaces/IScene.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/SpecularInfo.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	namespace IORStackSeeding
	{
		// Per-object containment is determined by per-object PARITY of
		// exits-vs-entries along a probe ray.  An object contains the
		// seed point iff the probe sees one more exit than entry (i.e.
		// it must cross the boundary one more time outward than inward
		// to leave the object).
		//
		// Counting exits only (or deduping per object) is INCORRECT
		// because a probe that simply *passes through* an object —
		// camera in front of a glass sphere with probe direction
		// going through it — sees one entry and one exit.  Without
		// parity tracking, that probe would record the sphere as
		// "containing" the camera and pre-seed the stack with it,
		// causing the first eye-ray hit on the sphere to be treated
		// as an exit (refraction direction computed glass→air
		// instead of air→glass).
		//
		// The `parity` field is +1 per exit, -1 per entry.  A net
		// positive value means the seed is inside that object along
		// this probe.
		struct ProbeEntry {
			const IObject* pObj;
			const IMaterial* pMat;
			Scalar ior;
			int parity;
			int firstExitStep;  // probe step of FIRST exit, for stack-order sort
			int firstEntryStep; // inward-wound closed solids reverse these faces
			Scalar firstExitDist;
			Scalar firstEntryDist;
			// DL-46: true for a `hasInterior`-only (non-refracting,
			// stateful) material -- push re-pushes the stack's CURRENT
			// top instead of this entry's captured `ior`, matching
			// TranslucentSPF::Scatter's own `push(ior_stack.top())`.
			// False for an ordinary `canRefract` medium, which pushes
			// its own distinct `ior` as before.
			bool repushParentIor;
			// DL-618: true when this entry is a certified open refractive
			// sheet (`bProvablyNoInterior && canRefract && ior > 0`).
			bool isOpenSheet;
		};

		// Safety cap on the surface hits one probe walks: guards against
		// pathological geometry (coincident faces, self-intersections)
		// that could loop indefinitely.
		static const int kMaxProbeSteps = 32;

		// Per-probe entry table, stack-allocated.  Sized to the STEP cap,
		// not to a nesting depth (DL-09 slice, 2026-09-28): the table holds
		// one entry per DISTINCT trackable object the probe MEETS, and a
		// probe meets every refractor between the seed and the scene edge
		// -- including the ones in front of it that do not contain it at
		// all.  An 8-entry table (the old `kMaxNestingDepth`) filled up with
		// those and silently dropped the containing objects the probe only
		// reaches afterwards: a camera inside 5 of 17 nested boxes, 12 of
		// them above it along +z, seeded as bare AIR, and the render read
		// 0.39x its closed form (tests/GradedIndexInteriorFactorTest.cpp row
		// E).  One step adds at most one entry, so kMaxProbeSteps entries
		// can never overflow.
		static const std::size_t kMaxProbeEntries = static_cast<std::size_t>( kMaxProbeSteps );

		// Probe-trace counter (2026-09-14, DL-76 perf follow-up).  This
		// compiles and increments UNCONDITIONALLY in every build -- it
		// is not behind a test-only `#ifdef`, so production code takes
		// the exact same path the regression test measures.  Cost: one
		// thread_local `std::size_t` increment per `TallyProbe` call,
		// sitting next to the ray-vs-scene intersection query that call
		// performs -- negligible by construction relative to that query,
		// which walks the object manager's acceleration structure doing
		// real ray-primitive tests.  Thread-local: no atomics or
		// locking, correct under the render thread pool since each
		// thread seeds its own subpaths independently.  Counts calls to
		// `TallyProbe` so regression tests can assert `SeedFromPoint`'s
		// total probe-trace budget stays bounded (see the X/Y
		// probe-hoisting comment in `SeedFromPoint` below).  Its only
		// consumer is `TranslucentInitialContainmentTest`.
		namespace Diagnostics
		{
			inline std::size_t& ProbeTraceCounter()
			{
				static thread_local std::size_t counter = 0;
				return counter;
			}
		}

		inline void DebugResetProbeTraceCount() {
			Diagnostics::ProbeTraceCounter() = 0;
		}

		inline std::size_t DebugGetProbeTraceCount() {
			return Diagnostics::ProbeTraceCounter();
		}

		/// Walks one probe ray from `pos` along `dir` and fills `out`
		/// with the per-object exit/entry parity tally.  Returns the
		/// number of entries written.
		inline std::size_t TallyProbe(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& dir,
			ProbeEntry* out,
            const Scalar nm = 0
			)
		{
			Diagnostics::ProbeTraceCounter()++;
			std::size_t count = 0;
			const Scalar kSeedEps = Scalar( 1e-4 );
			Ray probe( pos, dir );
			bool seenOpenFrontFace = false;

			for( int step = 0; step < kMaxProbeSteps; step++ )
			{
				RayIntersection ri( probe, nullRasterizerState );
				pObjects->IntersectRay( ri, true, true, false );
				if( !ri.geometric.bHit ) {
					break;
				}

				const Scalar hitDist = Point3Ops::Distance( pos, ri.geometric.ptIntersection );

				// Side-of-surface decision: use the GEOMETRIC normal,
				// not the shading normal — the question is "is the seed
				// point physically inside this object", which is a
				// topology query about the actual surface.  A bump-mapped
				// or normal-mapped enclosure boundary at a grazing probe
				// angle can flip the shading-normal cosN sign while the
				// geometric crossing is unambiguous; using shading there
				// silently leaves the seed stack empty (PBRT 4e §10.1.1).
				//
				// DL-46 double-sided-mesh follow-up: `vGeomNormal` is NOT
				// unconditionally the true surface-facing normal.  A
				// double-sided triangle mesh (TriangleMeshGeometry{,
				// Indexed}::IntersectRay), and BezierPatchGeometry /
				// ClippedPlaneGeometry on a back-face hit, flip it to face
				// whichever side the probe struck, recording that in
				// `bGeomNormalOrientedToRay`; on such a surface
				// `Dot(vGeomNormal, probe.Dir())` is ALWAYS negative, on
				// both a true entry and a true exit — `cosN > 0` (exit)
				// never fires, parity only ever decrements, and the object
				// is never seeded.  Recover the TRUE geometric normal with
				// the documented un-flip (RayIntersectionGeometric.h)
				// before dotting.
				//
				// P2-4 (review round 3): that recovery is only meaningful
				// when the reported normal is a static property of the
				// surface.  `HairGeometry` reports a RAY-DERIVED normal
				// (and sets `bGeomNormalOrientedToRay` unconditionally),
				// so un-flipping it gives "always away from the ray" =
				// "always an exit" at every strand the probe crosses — a
				// hair curtain between the seed and infinity would tally
				// arbitrarily large positive parity.  A 1-D curve has no
				// interior to be inside, so skip such a hit entirely
				// rather than tally a meaningless crossing.  (Harmless
				// today only because HairMaterial reports no
				// SpecularInfo; this guard makes it structural.)
				const Scalar cosN = ri.geometric.TrueGeomFacing( probe.Dir() );

				if( ri.pObject && ri.pMaterial && !ri.geometric.bGeomNormalRayDerived )
				{
					// Track refractive materials (own numeric IOR) AND
					// hasInterior-only stateful materials (DL-46, no
					// distinct IOR, just membership tracking) — pure
					// reflectors (mirrors) and Lambertian surfaces report
					// neither and have no "interior" the ray travels
					// through.
					IORStack queryStack( Scalar( 1.0 ) );
					const SpecularInfo info =
						nm > 0 ? ri.pMaterial->GetSpecularInfoNM( ri.geometric, queryStack, nm )
                        : ri.pMaterial->GetSpecularInfo( ri.geometric, queryStack );
					const bool bTrackable = info.canRefract ? (info.ior > 0) : info.hasInterior;
					if( info.valid && bTrackable )
					{
						const bool bOpenSheet = ri.geometric.bProvablyNoInterior && info.canRefract && (info.ior > 0);
						// Find or create per-object entry.  Linear scan is
						// fine — at most kMaxProbeEntries (32) entries.
						ProbeEntry* e = 0;
						for( std::size_t d = 0; d < count; d++ ) {
							if( out[d].pObj == ri.pObject ) {
								e = &out[d];
								break;
							}
						}
						if( !e && count < kMaxProbeEntries ) {
							e = &out[count++];
							e->pObj = ri.pObject;
							e->pMat = ri.pMaterial;
							e->ior = info.ior;
							e->parity = 0;
							e->firstExitStep = -1;
							e->firstEntryStep = -1;
							e->firstExitDist = RISE_INFINITY;
							e->firstEntryDist = RISE_INFINITY;
							e->repushParentIor = !info.canRefract;
							e->isOpenSheet = bOpenSheet;
						}
						if( e )
						{
							// +1 per exit, -1 per entry.  Positive net =
							// seed is inside this object.
							// DL-618: for a certified open refractive sheet,
							// a back-face hit (cosN > 0) only counts as an
							// enclosing exit when no open sheet has been
							// entered from its front face along this probe ray
							// (`!seenOpenFrontFace`).  Once an open sheet is
							// hit on its front face (cosN <= 0), `pos` is on
							// the exterior side of that boundary, so further
							// open-sheet back faces along the same ray close
							// that entered slab rather than enclosing `pos`.
							if( cosN > 0 ) {
								if( !bOpenSheet || !seenOpenFrontFace ) {
									e->parity++;
									if( e->firstExitStep < 0 ) {
										e->firstExitStep = step;
										e->firstExitDist = hitDist;
									}
								}
							} else {
								e->parity--;
								if( e->firstEntryStep < 0 ) {
									e->firstEntryStep = step;
									e->firstEntryDist = hitDist;
								}
								if( bOpenSheet ) {
									seenOpenFrontFace = true;
								}
							}
						}
					}
				}

				// Step past the hit to find the next one.
				probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
				probe.Advance( kSeedEps );
			}
            // DL-444: a closed inward-wound mesh has negative signed face
            // parity even when the point is inside. Normalize that sign only
            // when the object's independent signed-distance query certifies
            // containment. Open sheets keep the established face convention.
            for(std::size_t i=0;i<count;++i) {
                ProbeEntry& entry=out[i];
                if(entry.parity>=0 || entry.isOpenSheet) continue;
                const IGeometry* geometry=entry.pObj->GetGeometry();
                if(!geometry) continue;
                const Point3 local=Point3Ops::Transform(entry.pObj->GetFinalInverseTransformMatrix(),pos);
                const BoundingBox box=geometry->GenerateBoundingBox();
                const Scalar reach=Point3Ops::Distance(local,box.ll)+Vector3Ops::Magnitude(box.GetExtents());
                Scalar signedDistance=0;bool exact=false;
                if(geometry->SignedDistanceLower(local,reach,signedDistance,exact) && exact && signedDistance<0) {
                    entry.parity=-entry.parity;
                    entry.firstExitStep=entry.firstEntryStep;
                    entry.firstExitDist=entry.firstEntryDist;
                }
            }
			return count;
		}

		// DL-76: a single axis's forward+reverse probe PAIR is only a
		// containment test for a surface that is closed ALONG THAT AXIS.
		// A single `Object` built from two disjoint open pieces that
		// straddle the seed point on opposite sides of one axis, each
		// piece's normal facing away from the seed (e.g. a two-blade
		// louvre mesh with one blade above and one below along Z, both
		// part of the same triangle mesh `Object`), independently
		// registers an "exit" for that ONE `pObj` in both the forward AND
		// the reverse probe along that axis -- the per-object identity
		// check in `SeedFromPoint` below cannot distinguish that from a
		// genuine enclosure, because both probes really do see the same
		// object with positive parity.  Extending the vote to a SECOND,
		// non-coplanar axis rejects that specific counterexample: the two
		// blades lie in the plane perpendicular to the axis they straddle,
		// so a probe along either OTHER principal axis is coplanar with
		// both blades and (barring a coincidental edge-on hit) crosses
		// neither -- that axis then reports zero parity for the object,
		// failing the unanimity vote.  A genuinely closed manifold agrees
		// along every direction by construction, so this is a no-op for
		// the enclosures the mechanism targets.
		//
		// PERF (2026-09-14, review follow-up): the X/Y probe pairs used
		// to be re-traced by a per-candidate `IsConfirmedAlongAxis` call
		// INSIDE `SeedFromPoint`'s confirmation loop below -- but the
		// probe only depends on `pos` and the fixed axis, never on which
		// candidate object is being checked, so re-tracing it per
		// candidate was pure waste: up to 8 nested candidates x 4 probes
		// (X fwd/rev, Y fwd/rev) = up to 32 extra `TallyProbe` calls per
		// `SeedFromPoint`.  The X/Y pairs are now traced ONCE, exactly
		// like the Z-reverse probe already was, and candidates are
		// checked by looking their `pObj` up in the resulting arrays.
		// `HasPositiveParity` is that lookup -- a linear scan of an
		// already-traced array, no ray tracing at all.
		inline bool HasPositiveParity(
			const ProbeEntry* entries,
			std::size_t count,
			const IObject* pObj
			)
		{
			for( std::size_t i = 0; i < count; i++ ) {
				if( entries[i].pObj == pObj && entries[i].parity > 0 ) {
					return true;
				}
			}
			return false;
		}

		inline const ProbeEntry* FindPositiveParityEntry(
			const ProbeEntry* entries,
			std::size_t count,
			const IObject* pObj
			)
		{
			for( std::size_t i = 0; i < count; i++ ) {
				if( entries[i].pObj == pObj && entries[i].parity > 0 ) {
					return &entries[i];
				}
			}
			return 0;
		}

		/// Populate `stack` with the dielectric objects that physically
		/// contain `pos`, so that subsequent scatters at the first
		/// enclosing boundary see bFromInside==true.
		///
		/// Safe to call with a camera position even when the camera is
		/// not inside anything — the probe will simply find no exit
		/// hits and leave the stack as its caller initialized it.
		inline void SeedFromPoint(
			IORStack& stack,
			const Point3& pos,
			const IScene& scene,
            const Scalar nm = 0
			)
		{
			stack.SetMediumProbeScene( &scene );

			// Emergency off-switch for perf regression triage — set
			// RISE_DISABLE_IOR_STACK_SEEDING=1 in the environment to
			// restore the legacy empty-stack behaviour.  Kept as an
			// env check (not a scene param) so a single toggle covers
			// every BDPT/VCM invocation without touching scene files.
			static const bool sDisabled =
				( std::getenv( "RISE_DISABLE_IOR_STACK_SEEDING" ) != 0 );
			if( sDisabled ) {
				return;
			}

			const IObjectManager* pObjects = scene.GetObjects();
			if( !pObjects ) {
				return;
			}

			// Trace the 6 principal directions (+Z, -Z, +X, -X, +Y, -Y),
			// bounded by 6 `TallyProbe` calls total.
			//  - Closed objects (!isOpenSheet) require positive parity on all
			//    6 probes (P2-4 + DL-76 unanimity vote, unchanged).
			//  - Certified open refractive sheets (isOpenSheet, DL-618) seed
			//    when at least one probe sees an unoccluded back-face exit
			//    (parity > 0), no probe sees a front-face entry (parity < 0)
			//    of the sheet or its material, and the opposite direction on
			//    the same axis does not report an unoccluded back-face exit of
			//    a DIFFERENT material (which would be a walk-history-dependent
			//    unequal slab).
			static const Vector3 kDirs[6] = {
				Vector3( 0, 0, 1 ), Vector3( 0, 0, -1 ),
				Vector3( 1, 0, 0 ), Vector3( -1, 0, 0 ),
				Vector3( 0, 1, 0 ), Vector3( 0, -1, 0 )
			};
			ProbeEntry probes[6][kMaxProbeEntries];
			std::size_t probeCounts[6] = { 0, 0, 0, 0, 0, 0 };
			for( int d = 0; d < 6; d++ ) {
				probeCounts[d] = TallyProbe( pObjects, pos, kDirs[d], probes[d], nm );
			}

			ProbeEntry confirmed[kMaxProbeEntries];
			std::size_t confirmedCount = 0;

			// 1. Closed objects: must be present in probes[0] (+Z) and confirmed
			//    on all 5 other principal directions.
			for( std::size_t i = 0; i < probeCounts[0]; i++ ) {
				const ProbeEntry& cand = probes[0][i];
				if( cand.isOpenSheet || cand.parity <= 0 ) {
					continue;
				}
				if( !HasPositiveParity( probes[1], probeCounts[1], cand.pObj ) ||
					!HasPositiveParity( probes[2], probeCounts[2], cand.pObj ) ||
					!HasPositiveParity( probes[3], probeCounts[3], cand.pObj ) ||
					!HasPositiveParity( probes[4], probeCounts[4], cand.pObj ) ||
					!HasPositiveParity( probes[5], probeCounts[5], cand.pObj ) ) {
					continue;
				}
				if( confirmedCount < kMaxProbeEntries ) {
					confirmed[confirmedCount++] = cand;
				}
			}

			// 2. Certified open refractive sheets (DL-618): check across all 6 probes.
			for( int d = 0; d < 6; d++ ) {
				const int oppD = d ^ 1;
				for( std::size_t i = 0; i < probeCounts[d]; i++ ) {
					const ProbeEntry& cand = probes[d][i];
					if( !cand.isOpenSheet || cand.parity <= 0 ) {
						continue;
					}
					// Deduplicate by object or material (a consistent two-sheet
					// slab of the same material pushes at most one open-sheet entry).
					bool alreadyAdded = false;
					for( std::size_t k = 0; k < confirmedCount; k++ ) {
						if( confirmed[k].pObj == cand.pObj ||
							( confirmed[k].isOpenSheet && cand.pMat && confirmed[k].pMat == cand.pMat ) ) {
							alreadyAdded = true;
							break;
						}
					}
					if( alreadyAdded ) {
						continue;
					}
					// Reject if any of the 6 probes hit this sheet (or a sibling
					// open sheet of the same material) on its front face (parity < 0).
					bool anyFrontEntry = false;
					for( int q = 0; q < 6 && !anyFrontEntry; q++ ) {
						for( std::size_t m = 0; m < probeCounts[q]; m++ ) {
							const ProbeEntry& other = probes[q][m];
							if( other.isOpenSheet && other.parity < 0 &&
								( other.pObj == cand.pObj || ( cand.pMat && other.pMat == cand.pMat ) ) ) {
								anyFrontEntry = true;
								break;
							}
						}
					}
					if( anyFrontEntry ) {
						continue;
					}
					// Reject if the opposite direction along the same axis has an
					// unoccluded open-sheet exit of a DIFFERENT material (unequal
					// two-sheet slab, whose interior index is walk-history-dependent).
					bool axisMaterialConflict = false;
					for( std::size_t m = 0; m < probeCounts[oppD]; m++ ) {
						const ProbeEntry& opp = probes[oppD][m];
						if( opp.isOpenSheet && opp.parity > 0 && opp.pMat != cand.pMat ) {
							axisMaterialConflict = true;
							break;
						}
					}
					if( axisMaterialConflict ) {
						continue;
					}
					if( confirmedCount < kMaxProbeEntries ) {
						confirmed[confirmedCount++] = cand;
					}
				}
			}

			// Helper: returns true when `a` is INNER to `b` (so `b` is outer and
			// must be pushed before `a`).  When `a` and `b` both have positive
			// parity in any shared probe direction `d`, their `firstExitStep`
			// along that exact ray decides the nesting order without aspect-ratio
			// bias; otherwise fall back to `firstExitDist`.
			auto isInnerTo = [&]( const ProbeEntry& a, const ProbeEntry& b ) -> bool {
				for( int d = 0; d < 6; d++ ) {
					const ProbeEntry* ea = FindPositiveParityEntry( probes[d], probeCounts[d], a.pObj );
					const ProbeEntry* eb = FindPositiveParityEntry( probes[d], probeCounts[d], b.pObj );
					if( ea && eb ) {
						return ea->firstExitStep < eb->firstExitStep;
					}
				}
				return a.firstExitDist < b.firstExitDist;
			};

			// Sort `ordered[]` OUTERMOST-FIRST (`isInnerTo(ordered[j-1], cand)`
			// means `ordered[j-1]` is inner to `cand`, so `cand` goes before it).
			ProbeEntry* ordered[kMaxProbeEntries];
			std::size_t orderedCount = 0;
			for( std::size_t i = 0; i < confirmedCount; i++ ) {
				std::size_t j = orderedCount;
				while( j > 0 && isInnerTo( *ordered[j-1], confirmed[i] ) ) {
					ordered[j] = ordered[j-1];
					j--;
				}
				ordered[j] = &confirmed[i];
				orderedCount++;
			}

			for( std::size_t i = 0; i < orderedCount; i++ ) {
				stack.SetCurrentObject( ordered[i]->pObj );
				if( ordered[i]->isOpenSheet ) {
					stack.pushOpenSheet( ordered[i]->ior );
				} else {
					stack.push( ordered[i]->repushParentIor ? stack.top() : ordered[i]->ior );
				}
			}

			// DL-09: every entry above was recorded at its probe's FIRST
			// SURFACE HIT -- a boundary point that can be arbitrarily far
			// from `pos`.  For a constant-index medium that is the same
			// number; for a medium whose `ior` is a world-position field it
			// is not, and the walk that starts here must start from the
			// index AT the seed point, or its first interior-segment factor
			// (GradedIndexMedium::Advance) is priced from the wrong end.
			// Only the innermost medium matters: a deeper entry is re-read
			// only after the walk pops back into it, and the next Advance
			// then telescopes from whatever it holds.  No-op unless the
			// innermost medium is graded.
			GradedIndexMedium::RecordAt( stack, pos );
		}

		//! DL-345: does object @a pY ENCLOSE the crossing at @a p (the walk
		//! travelling along @a dir)?  One ray against ONE object: an EXIT
		//! hit on Y ahead means the walk is still inside Y after the
		//! crossing; a miss, an ENTRY hit, or a surface with no true side
		//! (hair) means Y is not around it.  For a closed, outward-wound Y
		//! the first hit from inside is always an exit, concave or not.
		//! The SMS seed walk's unpushed-exit rule (DL-290 section 11) is the
		//! same probe.
		inline bool ObjectEnclosesCrossing( const IObject* pY, const Point3& p, const Vector3& dir )
		{
			if( !pY ) {
				return false;
			}
			const Scalar scale = Scalar( 1 ) + std::fabs( p.x ) + std::fabs( p.y ) + std::fabs( p.z );
			const Ray probe( Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-7 ) * scale ) ), dir );
			RayIntersection pri( probe, nullRasterizerState );
			pY->IntersectRay( pri, RISE_INFINITY, true, true, false );
			if( pri.geometric.bHit && pri.geometric.HasTrueGeomSide() ) {
				return Vector3Ops::Dot( dir, pri.geometric.UnflippedGeomNormal() ) > 0;
			}
			return false;
		}

		//! DL-382: does closed object @a pY ENCLOSE the crossing at @a p (walk
		//! travelling along @a dir)?  Computed by the PARITY of pY's own crossings
		//! along the probe (odd = inside), which is winding-independent (T5c).
		inline bool ClosedObjectEnclosesCrossing( const IObject* pY, const Point3& p, const Vector3& dir )
		{
			if( !pY ) {
				return false;
			}
			const Scalar scale = Scalar( 1 ) + std::fabs( p.x ) + std::fabs( p.y ) + std::fabs( p.z );
			Ray probe( Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-7 ) * scale ) ), dir );
			int crossings = 0;
			for( int step = 0; step < 64; ++step ) {
				RayIntersection pri( probe, nullRasterizerState );
				pY->IntersectRay( pri, RISE_INFINITY, true, true, false );
				if( !pri.geometric.bHit || !pri.geometric.HasTrueGeomSide() ) {
					break;
				}
				++crossings;
				probe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, dir * ( Scalar( 1e-6 ) * scale ) );
			}
			return ( crossings % 2 ) == 1;
		}

		//! DL-382 / DL-618: does open sheet @a pY's medium continue past the
		//! crossing at @a p (walk travelling along @a dir)?  First probes along
		//! @a dir for the first surface made of pY's material (excluding @a pSheet).
		//! If the walk direction misses pY's material altogether (e.g. an oblique
		//! exit inside a lone sheet's back half-space), falls back to probing the
		//! 6 principal directions from a point just past the crossing at @a p:
		//! the medium continues iff at least one axis probe reaches pY's material
		//! on its BACK face without first crossing any open sheet's FRONT face,
		//! and no axis probe hits pY's material on its FRONT face first.
		inline bool OpenSheetEnclosesCrossing(
			const IObject* pY,
			const IObject* pSheet,
			const IScene* pScene,
			const Point3& p,
			const Vector3& dir
			)
		{
			if( !pY ) {
				return false;
			}
			if( !pScene || !pScene->GetObjects() ) {
				return ObjectEnclosesCrossing( pY, p, dir );
			}
			const IMaterial* pMat = pY->GetMaterial();
			const Scalar scale = Scalar( 1 ) + std::fabs( p.x ) + std::fabs( p.y ) + std::fabs( p.z );
			Ray probe( Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-7 ) * scale ) ), dir );
			for( int step = 0; step < 64; ++step ) {
				RayIntersection pri( probe, nullRasterizerState );
				pScene->GetObjects()->IntersectRay( pri, true, true, false );
				if( !pri.geometric.bHit ) {
					break;
				}
				if( !pri.geometric.HasTrueGeomSide() || pri.pObject == pSheet ) {
					probe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, dir * ( Scalar( 1e-6 ) * scale ) );
					continue;
				}
				const IMaterial* hitMat = pri.pMaterial ? pri.pMaterial : ( pri.pObject ? pri.pObject->GetMaterial() : nullptr );
				if( pri.pObject == pY || ( pMat && hitMat == pMat ) ) {
					return Vector3Ops::Dot( dir, pri.geometric.UnflippedGeomNormal() ) >= 0;
				}
				probe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, dir * ( Scalar( 1e-6 ) * scale ) );
			}

			// DL-618: walk direction missed `pMat`.  Probe the 6 principal
			// directions from just past the crossing (`pAfter`).  Not skipping
			// `pSheet` here is deliberate: if `pSheet` is the closing face of a
			// two-sheet slab entered through `pY`, the ray from `pAfter` back
			// toward `pY` hits `pSheet` on its FRONT face first and stops.
			const Point3 pAfter = Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-5 ) * scale ) );
			static const Vector3 kAxes[6] = {
				Vector3( 0, 0, 1 ), Vector3( 0, 0, -1 ),
				Vector3( 1, 0, 0 ), Vector3( -1, 0, 0 ),
				Vector3( 0, 1, 0 ), Vector3( 0, -1, 0 )
			};
			bool foundBackHit = false;
			for( int a = 0; a < 6; ++a ) {
				const Vector3& qDir = kAxes[a];
				Ray qProbe( pAfter, qDir );
				for( int step = 0; step < 64; ++step ) {
					RayIntersection pri( qProbe, nullRasterizerState );
					pScene->GetObjects()->IntersectRay( pri, true, true, false );
					if( !pri.geometric.bHit ) {
						break;
					}
					if( !pri.geometric.HasTrueGeomSide() ) {
						qProbe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, qDir * ( Scalar( 1e-6 ) * scale ) );
						continue;
					}
					const Scalar cosQ = Vector3Ops::Dot( qDir, pri.geometric.UnflippedGeomNormal() );
					const IMaterial* hitMat = pri.pMaterial ? pri.pMaterial : ( pri.pObject ? pri.pObject->GetMaterial() : nullptr );
					if( pri.pObject == pY || ( pMat && hitMat == pMat ) ) {
						if( cosQ >= 0 ) {
							foundBackHit = true;
							break;
						} else {
							return false;
						}
					}
					if( pri.geometric.bProvablyNoInterior && cosQ < 0 ) {
						break;
					}
					qProbe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, qDir * ( Scalar( 1e-6 ) * scale ) );
				}
			}
			return foundBackHit;
		}

		//! DL-382: resolves an unpushed exit on certified sheet @a pSheet.
		//! Pops top entry Y while it does not contain the front region.
		inline void ResolveUnpushedExit(
			const RayIntersectionGeometric& ri,
			const IObject* pSheet,
			IORStack& dest
			)
		{
			const Vector3& dir = ri.ray.Dir();
			const Point3& pt = ri.ptIntersection;
			const IScene* pScene = dest.MediumProbeScene();

			while( const IObject* pY = dest.topObject() ) {
				if( pY == pSheet ) {
					break;
				}
				bool bEncloses = false;
				if( dest.topIsOpenSheet() ) {
					bEncloses = OpenSheetEnclosesCrossing( pY, pSheet, pScene, pt, dir );
				} else {
					bEncloses = ClosedObjectEnclosesCrossing( pY, pt, dir );
				}

				if( bEncloses ) {
					break;
				}

				dest.SetCurrentObject( pY );
				dest.pop();
				if( pSheet ) {
					dest.SetCurrentObject( pSheet );
				}
			}
		}

		//! Result of ResolveOpenSheetCrossing (below).
		struct OpenSheetCrossing
		{
			bool   bEntering;
			Scalar etaFrom;
			Scalar etaTo;
		};

		//! DL-345: one crossing of a PROVABLY OPEN transmissive sheet
		//! (`RayIntersectionGeometric::bProvablyNoInterior`).
		//!
		//! THE RULING.  An open sheet is an interface whose FRONT side (the
		//! side its TRUE geometric normal points to) is the surrounding
		//! medium and whose BACK side is the sheet's material.  A crossing
		//! is decided by its FACE, never by the IOR stack: front -> back
		//! ENTERS (`etaFrom` = the walk's medium, `etaTo` = the sheet's
		//! index), back -> front EXITS (`etaFrom` = the sheet's index,
		//! `etaTo` = the medium beyond).  This is the only rule under which
		//! a walk and its reverse refract the same way at every sheet, so
		//! the eye walk (PT, BDPT/VCM eye subpaths), the light walk
		//! (BDPT/VCM light subpaths, photons) and SMS's seed walk all
		//! sample ONE path space.  The stack-based rule it replaces for
		//! these hits ("entering iff not already contained") bent the eye
		//! walk at the second sheet of a two-sheet slab it crossed and the
		//! light walk at the other (up to 3x between integrators).
		//!
		//! The stack is kept consistent where it can be:
		//!  - front hit while the sheet is already on the stack (the walk
		//!    left the back region around the sheet's edge): the stale
		//!    entry is dropped, then the sheet is pushed;
		//!  - back hit with the sheet on the stack: popped;
		//!  - back hit WITHOUT it (an "unpushed exit"): the stack top Y is
		//!    either a sibling sheet the walk entered a slab through
		//!    (popped: leaving the slab leaves Y's entry) or a medium that
		//!    encloses the crossing (kept) -- told apart by
		//!    ResolveUnpushedExit (DL-382).
		//!    With no sibling, the walk reached the sheet's back WITHOUT
		//!    crossing it (a receiver under a lone sheet, seen directly):
		//!    the crossing still refracts from the sheet's index, and the
		//!    caller records that on the scattered stack
		//!    (IORStack::SetCrossingEtaFrom) so RadianceEtaScale prices the
		//!    refraction actually performed.
		//!
		//! @param dest  a COPY of the walk's stack whose current object is
		//!              the sheet; on return, the stack after transmission.
		//! @return      whether the crossing enters, and the index pair it refracts between.
		inline OpenSheetCrossing ResolveOpenSheetCrossing(
			const RayIntersectionGeometric& ri,
			const Scalar sheetIOR,
			IORStack& dest
			)
		{
			OpenSheetCrossing c;
			const Vector3& d = ri.ray.Dir();
			const bool bFront = Vector3Ops::Dot( d, ri.UnflippedGeomNormal() ) < 0;
			if( bFront ) {
				if( dest.containsCurrent() ) {
					dest.pop();
				}
				c.bEntering = true;
				c.etaFrom = dest.top();
				dest.pushOpenSheet( sheetIOR );
				c.etaTo = sheetIOR;
			} else {
				if( dest.containsCurrent() ) {
					dest.pop();
				} else {
					ResolveUnpushedExit( ri, dest.currentObject(), dest );
				}
				c.bEntering = false;
				c.etaFrom = sheetIOR;
				c.etaTo = dest.top();
			}
			return c;
		}

		//! DL-345: true when @a ri struck a provably open sheet from BEHIND
		//! (the exit side under ResolveOpenSheetCrossing's ruling).
		inline bool OpenSheetBackFace( const RayIntersectionGeometric& ri )
		{
			return Vector3Ops::Dot( ri.ray.Dir(), ri.UnflippedGeomNormal() ) >= 0;
		}

		//! The stack a transmissive SPF hands its TRANSMITTED ray.  Without
		//! an open-sheet resolution (@a pOpenSheet null) it is the walk's
		//! stack pushed (entry) or popped (exit), as before DL-345; with one
		//! it is the resolved stack, carrying the crossing's `etaFrom` when
		//! that differs from the walk's own top (see
		//! IORStack::crossingEtaFrom).  Caller owns the result.
		inline IORStack* NewTransmittedStack(
			const IORStack& walk,
			const IORStack* pOpenSheet,
			const Scalar openSheetEtaFrom,
			const bool bEntering,
			const Scalar objectIOR
			)
		{
			IORStack* p = new IORStack( pOpenSheet ? *pOpenSheet : walk );
			GlobalLog()->PrintNew( p, __FILE__, __LINE__, "ior stack" );
			if( pOpenSheet ) {
				if( openSheetEtaFrom != walk.top() ) {
					p->SetCrossingEtaFrom( openSheetEtaFrom );
				}
			} else if( bEntering ) {
				p->push( objectIOR );
			} else {
				p->pop();
			}
			return p;
		}
	}
}

#endif
