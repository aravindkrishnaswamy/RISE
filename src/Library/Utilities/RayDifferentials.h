//////////////////////////////////////////////////////////////////////
//
//  RayDifferentials.h - Per-ray screen-space differentials for
//  texture LOD selection (Igehy 1999, "Tracing Ray Differentials").
//
//  Each populated ray carries the offsets to the auxiliary rays
//  one screen-pixel to the +x and +y, expressed as origin and
//  direction offsets from the central ray.  Object::IntersectRay
//  consumes the differentials, projects them onto the surface
//  tangent plane and (where the surface has a UV chart) solves that
//  projection through dpdu/dpdv, producing the TextureFootprint the
//  texture painter uses to pick a mip LOD and the expression VM
//  uses as `fw`.
//
//  Embedded in Ray (PBRT / Mitsuba / Arnold convention).  The
//  +96 bytes per Ray is real cache pressure on BVH traversal —
//  this is a deliberate trade against the alternative parallel-
//  struct plumbing tech debt.  See
//  docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md decision #1.
//
//  SCOPE: PRIMARY VISIBILITY ONLY.  Differentials are NEVER
//  propagated through a scattering bounce.  There are no
//  propagation helpers — no PropagateThroughReflection, no
//  PropagateThroughRefraction, nothing applying Igehy's closed-form
//  specular formulas anywhere in src/.  (An earlier version of this
//  header claimed those helpers existed; they were never written.)
//  Writing them is a separate, larger arc: Igehy §3.2/§3.3 plus a
//  dndu/dndv requirement at every specular vertex.
//
//  Consequences of that scope, all load-bearing when reading a
//  footprint-driven fade:
//    - Ray::Set / Ray::SetDir CLEAR hasDifferentials, so any
//      freshly-Set ray — every scattered ray, shadow ray, NEE ray
//      and photon — is differential-free by construction, and the
//      surfaces it hits report fw = 0 and point-sample.
//    - Only PinholeCamera::GenerateRay ever SETS them.
//      ThinLensCamera, OrthographicCamera and FisheyeCamera do not.
//    - The only two transfers in the renderer are straight-line,
//      not scattering: RayCaster's x-ray continuation and
//      CSGObject's reversed exit probe.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 3, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_DIFFERENTIALS_
#define RAY_DIFFERENTIALS_

#include "Math3D/Math3D.h"

namespace RISE
{
	//! Offsets (from the central ray's origin and direction) to the
	//! auxiliary rays one screen-pixel to the +x and +y.
	//!
	//! For a pinhole camera, all primary rays share an origin, so
	//! `rxOrigin` and `ryOrigin` are zero on freshly-spawned rays;
	//! after a refractive bounce, origin offsets become non-zero.
	//! For a thin-lens camera, primary rays already have non-zero
	//! origin offsets (each pixel-neighbour samples a different
	//! lens position) — that's a v1.1 enhancement; v1's pinhole
	//! emits zero origin offsets.
	struct RayDifferentials
	{
		Vector3 rxOrigin;
		Vector3 ryOrigin;
		Vector3 rxDir;
		Vector3 ryDir;

		RayDifferentials() :
		rxOrigin( 0, 0, 0 ),
		ryOrigin( 0, 0, 0 ),
		rxDir   ( 0, 0, 0 ),
		ryDir   ( 0, 0, 0 )
		{}
	};
}

#endif
