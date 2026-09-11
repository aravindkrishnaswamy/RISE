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
//    - The cameras that SET them are PinholeCamera,
//      ThinLensCamera and OrthographicCamera (the latter two since
//      2026-09-10).  FisheyeCamera does not, and is the only
//      remaining camera that does not -- its equal-area /
//      equidistant mapping has no single linear pixel-to-direction
//      Jacobian, so its differentials are their own piece of work.
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
	//! `rxOrigin` and `ryOrigin` are zero on freshly-spawned rays.
	//!
	//! For a THIN-LENS camera they are zero as well, and that is a
	//! deliberate choice rather than an omission: the +x / +y
	//! auxiliary rays go through the SAME sampled lens point as the
	//! main ray (PBRT-v4's `PerspectiveCamera::
	//! GenerateRayDifferential` convention), so only the film sample
	//! moves.  The differential then measures the pixel footprint of
	//! the pinhole sitting at that lens point; defocus blur is
	//! produced by integrating many lens samples per pixel, NOT by
	//! inflating each sample's texture filter.
	//!
	//! For an ORTHOGRAPHIC camera it is the mirror image: the
	//! direction offsets are exactly zero (parallel projection) and
	//! the origin offsets carry the one-pixel viewport pitch.
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
