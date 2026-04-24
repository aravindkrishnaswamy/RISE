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
//    Shoot a probe ray in a fixed direction from the seed point.
//    Every time the ray hits a surface with the outward normal
//    pointing ALONG the ray direction (dot > 0) we're exiting that
//    object, which means the seed point was inside it.  Record the
//    object plus its scalar IOR (read via the material's
//    GetSpecularInfo).  Advance the probe past the hit and repeat
//    until the probe exits all containing volumes.
//
//    The recorded objects are pushed onto the stack in reverse
//    order: outermost first, innermost last.  The stack convention
//    is that the top is the immediate medium, which matches what
//    DielectricSPF's bFromInside check expects.
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

			struct Entry {
				const IObject* pObj;
				Scalar ior;
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

				const Scalar cosN = Vector3Ops::Dot(
					ri.geometric.vNormal, probe.Dir() );
				if( cosN > 0 && ri.pObject && ri.pMaterial )
				{
					// We're exiting this object, so the seed point was
					// inside it.  Query IOR via the material's specular
					// info; only refractive materials count.
					IORStack queryStack( Scalar( 1.0 ) );
					const SpecularInfo info =
						ri.pMaterial->GetSpecularInfo( ri.geometric, queryStack );
					if( info.valid && info.canRefract && info.ior > 0 )
					{
						// Deduplicate: non-convex geometry can produce
						// exit → re-enter → exit crossings along a single
						// probe ray, which would push the same object
						// twice onto the stack.  Parity check (odd # of
						// exits means we're inside) is handled by the
						// rest of the loop — we just need to avoid the
						// duplicate push here.
						bool alreadyRecorded = false;
						for( std::size_t d = 0; d < containingCount; d++ ) {
							if( containing[d].pObj == ri.pObject ) {
								alreadyRecorded = true;
								break;
							}
						}
						if( !alreadyRecorded &&
							containingCount < kMaxNestingDepth )
						{
							containing[containingCount].pObj = ri.pObject;
							containing[containingCount].ior = info.ior;
							containingCount++;
						}
					}
				}

				// Step past the hit to find the next one.
				probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
				probe.Advance( kSeedEps );
			}

			// Push outermost first, innermost last — collection order is
			// innermost-first (probe hit them first traveling outward),
			// so walk the array in reverse.
			for( std::size_t i = containingCount; i > 0; i-- )
			{
				const Entry& e = containing[i - 1];
				stack.SetCurrentObject( e.pObj );
				stack.push( e.ior );
			}
		}
	}
}

#endif
