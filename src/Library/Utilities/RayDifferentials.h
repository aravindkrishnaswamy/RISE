//////////////////////////////////////////////////////////////////////
//
//  RayDifferentials.h - Per-ray screen-space differentials for
//    texture LOD selection / expression `fw` (Igehy 1999).
//
//  Embedded in Ray (docs/PHYSICALLY_BASED_PIPELINE_PLAN.md decision #1).
//  Object::IntersectRay projects these +x/+y one-pixel offsets onto the
//  tangent plane and solves through dpdu/dpdv to produce TextureFootprint.
//
//  SCOPE: Primary visibility only (no scattering bounce propagation).
//    - Ray::Set / Ray::SetDir clear hasDifferentials (scattered, shadow,
//      NEE, and photon rays point-sample with fw = 0).
//    - Straight-line transfers (x-ray, CSG exit probe, alpha null
//      traversal) preserve the auxiliary ray lines.
//    - All cameras set differentials via one-pixel finite differences;
//      FisheyeCamera leaves hasDifferentials false at the 180-deg rim.
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
	//! One-pixel (+x, +y) origin and direction offsets from the central ray.
	//!   Pinhole / thin-lens: zero origin offsets (thin-lens reuses the same
	//!     sampled lens point per PBRT-v4; defocus comes from lens integration).
	//!   Orthographic: zero direction offsets; origin offsets = viewport pitch.
	//!   Fisheye: zero origin offsets; direction offsets = r = sin(theta) chords.
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
