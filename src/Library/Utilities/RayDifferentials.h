//////////////////////////////////////////////////////////////////////
//
//  RayDifferentials.h - Per-ray screen-space differentials for
//  texture LOD selection (Igehy 1999, "Tracing Ray Differentials").
//
//  Each populated ray carries the offsets to the auxiliary rays
//  one screen-pixel to the +x and +y, expressed as origin and
//  direction offsets from the central ray.  Triangle intersection
//  consumes the differentials and projects them to UV-space via
//  the surface dpdu/dpdv basis, producing a TextureFootprint that
//  the texture painter uses to pick a mip LOD.
//
//  Embedded in Ray (PBRT / Mitsuba / Arnold convention).  The
//  +96 bytes per Ray is real cache pressure on BVH traversal —
//  this is a deliberate trade against the alternative parallel-
//  struct plumbing tech debt.  See
//  docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md decision #1.
//
//  Propagation helpers (PropagateThroughReflection / Refraction)
//  apply Igehy's closed-form formulas for specular bounces.
//  Glossy / diffuse bounces invalidate the differentials
//  (caller sets hasDifferentials = false on the new ray).
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
