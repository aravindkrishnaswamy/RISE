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
//    Objects with parity > 0 at the end are pushed in OUTERMOST-FIRST
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
//    The probe only considers materials whose GetSpecularInfo
//    reports canRefract=true.  Pure reflectors (mirrors) and
//    lambertian surfaces are skipped — they are not media and their
//    "interior" is not a place rays travel through with a different
//    IOR.
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

			// Per-object containment is determined by per-object PARITY of
			// exits-vs-entries along the probe ray.  An object contains
			// the seed point iff the probe sees one more exit than entry
			// (i.e. it must cross the boundary one more time outward
			// than inward to leave the object).
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
			// positive value means the seed is inside that object.
			struct Entry {
				const IObject* pObj;
				Scalar ior;
				int parity;
				int firstExitStep;  // probe step of FIRST exit, for stack-order sort
			};
			// Stack-allocated small buffer; real scenes rarely nest more
			// than 2-3 refractive volumes so the fixed cap is generous.
			static const std::size_t kMaxNestingDepth = 8;
			Entry containing[kMaxNestingDepth];
			std::size_t containingCount = 0;

			const Scalar kSeedEps = Scalar( 1e-4 );
			// +Z is arbitrary; any fixed direction avoids per-seed RNG
			// and keeps results deterministic across threads.
			Vector3 dir( 0, 0, 1 );
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
				const Scalar cosN = Vector3Ops::Dot(
					ri.geometric.vGeomNormal, probe.Dir() );

				if( ri.pObject && ri.pMaterial )
				{
					// Only track refractive materials — pure reflectors
					// (mirrors) and Lambertian surfaces have no "interior"
					// the ray travels through with a different IOR.
					IORStack queryStack( Scalar( 1.0 ) );
					const SpecularInfo info =
						ri.pMaterial->GetSpecularInfo( ri.geometric, queryStack );
					if( info.valid && info.canRefract && info.ior > 0 )
					{
						// Find or create per-object entry.  Linear scan is
						// fine — kMaxNestingDepth is 8.
						Entry* e = 0;
						for( std::size_t d = 0; d < containingCount; d++ ) {
							if( containing[d].pObj == ri.pObject ) {
								e = &containing[d];
								break;
							}
						}
						if( !e && containingCount < kMaxNestingDepth ) {
							e = &containing[containingCount++];
							e->pObj = ri.pObject;
							e->ior = info.ior;
							e->parity = 0;
							e->firstExitStep = -1;
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
			Entry* ordered[kMaxNestingDepth];
			std::size_t orderedCount = 0;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity <= 0 ) {
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
				stack.push( ordered[i]->ior );
			}
		}
	}
}

#endif
