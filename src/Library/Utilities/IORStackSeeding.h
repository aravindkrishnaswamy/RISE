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
//    positive (more exits than entries — the probe must cross its
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
#include <cstdlib>
#include "../Interfaces/IScene.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IObject.h"
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
			Scalar ior;
			int parity;
			int firstExitStep;  // probe step of FIRST exit, for stack-order sort
			// DL-46: true for a `hasInterior`-only (non-refracting,
			// stateful) material -- push re-pushes the stack's CURRENT
			// top instead of this entry's captured `ior`, matching
			// TranslucentSPF::Scatter's own `push(ior_stack.top())`.
			// False for an ordinary `canRefract` medium, which pushes
			// its own distinct `ior` as before.
			bool repushParentIor;
		};

		// Stack-allocated small buffer; real scenes rarely nest more
		// than 2-3 refractive volumes so the fixed cap is generous.
		static const std::size_t kMaxNestingDepth = 8;

		/// Walks one probe ray from `pos` along `dir` and fills `out`
		/// with the per-object exit/entry parity tally.  Returns the
		/// number of entries written.
		inline std::size_t TallyProbe(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& dir,
			ProbeEntry* out
			)
		{
			std::size_t count = 0;
			const Scalar kSeedEps = Scalar( 1e-4 );
			Ray probe( pos, dir );

			// Safety cap: guards against pathological geometry (coincident
			// faces, self-intersections) that could loop indefinitely.
			const int kMaxSteps = 32;
			for( int step = 0; step < kMaxSteps; step++ )
			{
				RayIntersection ri( probe, nullRasterizerState );
				pObjects->IntersectRay( ri, true, true, false );
				if( !ri.geometric.bHit ) {
					break;
				}

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
				const Vector3 trueGeomNormal = ri.geometric.bGeomNormalOrientedToRay
					? -ri.geometric.vGeomNormal : ri.geometric.vGeomNormal;
				const Scalar cosN = Vector3Ops::Dot(
					trueGeomNormal, probe.Dir() );

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
						ri.pMaterial->GetSpecularInfo( ri.geometric, queryStack );
					const bool bTrackable = info.canRefract ? (info.ior > 0) : info.hasInterior;
					if( info.valid && bTrackable )
					{
						// Find or create per-object entry.  Linear scan is
						// fine — kMaxNestingDepth is 8.
						ProbeEntry* e = 0;
						for( std::size_t d = 0; d < count; d++ ) {
							if( out[d].pObj == ri.pObject ) {
								e = &out[d];
								break;
							}
						}
						if( !e && count < kMaxNestingDepth ) {
							e = &out[count++];
							e->pObj = ri.pObject;
							e->ior = info.ior;
							e->parity = 0;
							e->firstExitStep = -1;
							e->repushParentIor = !info.canRefract;
						}
						if( e )
						{
							// +1 per exit, -1 per entry.  Positive net =
							// seed is inside this object.
							if( cosN > 0 ) {
								e->parity++;
								if( e->firstExitStep < 0 ) {
									e->firstExitStep = step;
								}
							} else {
								e->parity--;
							}
						}
					}
				}

				// Step past the hit to find the next one.
				probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
				probe.Advance( kSeedEps );
			}
			return count;
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
			const IScene& scene
			)
		{
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

			// +Z is arbitrary; any fixed direction avoids per-seed RNG
			// and keeps results deterministic across threads.
			ProbeEntry containing[kMaxNestingDepth];
			const std::size_t containingCount =
				TallyProbe( pObjects, pos, Vector3( 0, 0, 1 ), containing );

			// P2-4 (review round 3): a single probe's parity is only a
			// containment test for a CLOSED surface.  Every
			// `translucent_material` object is now a probe participant
			// (DL-46), and translucent is exactly the material authors put
			// on OPEN sheets -- a leaf, a curtain, a lampshade panel, a
			// paper card.  An open single-sided card sitting above the
			// seed with its normal pointing up registers one "exit" and no
			// entry, so parity reads +1 and the seed is falsely declared
			// inside it: the camera's very first hit then runs
			// TranslucentSPF's EXIT branch (Beer extinction + a pop of an
			// IOR that was never pushed) instead of the entry branch.
			//
			// Rule: require positive parity along the probe AND along its
			// REVERSE.  For a closed manifold both directions agree by
			// construction (the seed is inside or it is not), so this is a
			// no-op for every case the mechanism was built for; for an
			// open surface the two disagree (the reverse probe never meets
			// it) and the object is rejected.  The reverse probe only runs
			// when the forward one actually found a candidate, so the
			// overwhelmingly common "not inside anything" call costs
			// exactly what it did before.
			//
			// LIMIT, documented rather than papered over: a configuration
			// of SEVERAL open surfaces that happens to present an
			// away-facing sheet in BOTH directions (e.g. two parallel
			// cards straddling the seed, normals pointing outward) still
			// reads as containment.  Deciding that correctly needs real
			// solid-angle / winding-number containment, not a pair of
			// probes; this rule removes the single-open-surface false
			// positive, which is the one open-geometry authors actually
			// build.
			ProbeEntry reverse[kMaxNestingDepth];
			std::size_t reverseCount = 0;
			bool anyCandidate = false;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity > 0 ) { anyCandidate = true; break; }
			}
			if( anyCandidate ) {
				reverseCount = TallyProbe( pObjects, pos, Vector3( 0, 0, -1 ), reverse );
			}

			// Push containing objects (parity > 0) onto the stack.
			//
			// Stack ORDER: bottom = outermost, top = innermost.  Along an
			// outward-going probe, the FIRST exit of an object is at its
			// INNERMOST surface, the LAST exit at its OUTERMOST.  Objects
			// with smaller firstExitStep are inner — push them last.
			// Insertion-sort to OUTERMOST-FIRST (largest firstExitStep
			// first); buffer is at most 8 entries.
			//
			// Objects with parity == 0 (probe passed through) and
			// parity < 0 (unbalanced entries — only possible from
			// pathological geometry hitting the step cap) are skipped.
			ProbeEntry* ordered[kMaxNestingDepth];
			std::size_t orderedCount = 0;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity <= 0 ) {
					continue;
				}
				// P2-4: confirm against the reverse probe (see above).
				bool confirmed = false;
				for( std::size_t r = 0; r < reverseCount; r++ ) {
					if( reverse[r].pObj == containing[i].pObj && reverse[r].parity > 0 ) {
						confirmed = true;
						break;
					}
				}
				if( !confirmed ) {
					continue;
				}
				// Insert into ordered[] keeping descending firstExitStep.
				std::size_t j = orderedCount;
				while( j > 0 && ordered[j-1]->firstExitStep <
					containing[i].firstExitStep )
				{
					ordered[j] = ordered[j-1];
					j--;
				}
				ordered[j] = &containing[i];
				orderedCount++;
			}
			for( std::size_t i = 0; i < orderedCount; i++ ) {
				stack.SetCurrentObject( ordered[i]->pObj );
				// DL-46: a hasInterior-only entry (e.g. TranslucentSPF) has
				// no distinct IOR of its own -- re-push whatever is
				// CURRENTLY on top (set by the previous, more-outer push,
				// or the stack's initial environment IOR if this is the
				// outermost entry), matching Scatter()'s own
				// `push(ior_stack.top())`.  An ordinary canRefract medium
				// pushes its own captured `ior` as before.
				stack.push( ordered[i]->repushParentIor ? stack.top() : ordered[i]->ior );
			}
		}
	}
}

#endif
