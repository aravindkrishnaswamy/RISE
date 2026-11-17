//////////////////////////////////////////////////////////////////////
//
//  RaySphereIntersection.cpp - Implements a ray sphere intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"
#include "../Functions/Polynomial.h"
#include "../Utilities/Profiling.h"

#include "math.h"			// for sqrt

namespace RISE
{

	void RaySphereIntersection( const Ray& ray, HIT& hit, const Scalar radius, const Point3& center )
	{
		RISE_PROFILE_INC(nSphereIntersectionTests);
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;

		Vector3 vTemp = Vector3Ops::mkVector3( ray.origin, center );

		Scalar		fSqrRadius = radius*radius;
		Scalar		coeffs[3] = {0};

		coeffs[0] = Vector3Ops::SquaredModulus( ray.Dir() );
		coeffs[1] = 2 * (ray.Dir().x * vTemp.x + ray.Dir().y * vTemp.y + ray.Dir().z * vTemp.z);
		coeffs[2] = Vector3Ops::SquaredModulus( vTemp ) - (fSqrRadius);

		Scalar		solutions[2] = {0};

		// Self-intersection floor, scale-relative to the coordinates
		// involved rather than the fixed absolute NEARZERO (1e-12).  A
		// shadow or continuation ray whose origin was PUBLISHED from a hit
		// on this sphere (Object::IntersectRay backs the hit point off by
		// SURFACE_INTERSEC_ERROR = 1e-12 along the incoming ray) has a
		// mathematically ~1e-12 root here, t = depth / |cos out|, which
		// straddles 1e-12 whenever the outgoing ray is more grazing than
		// the incoming one; and at coordinates of ~1e3 the eye hit's own
		// range already carries ~1e-12 of absolute round-off, so the
		// published point lands INSIDE the sphere for a sizeable fraction
		// of pixels.  Measured 2026-09-05 (PT vs BDPT, which advances its
		// shadow rays 1e-6 and never sees it): a radius-1000 Lambertian
		// sphere under an omni light read PT 0.62x of BDPT (1.000 at unit
		// scale; advancing PT's shadow ray alone restores 1.0006), and a
		// unit sphere carrying a full-sphere transmissive weave lit from
		// behind read 0.32x.  Same pattern and same floor as
		// RayBilinearPatchIntersection's debt-21 fix and the per-axis band
		// BoxGeometry::DropSelfHitRoot uses (debt 25, docs/CLOTH_FABRIC_
		// DESIGN.md section 15): fixed once here at the producer so every
		// caller (IntersectRay, IntersectRay_IntersectionOnly, the CSG
		// exit probe) benefits.  |origin| + radius is the coordinate scale
		// that matters: a point published from this surface sits at
		// |origin| ~ radius.
		const Scalar coordScale = fabs( ray.origin.x ) + fabs( ray.origin.y ) + fabs( ray.origin.z ) + fabs( radius );
		const Scalar tMin = NEARZERO * ( Scalar(1) + coordScale );

		int			numSolutions = Polynomial::SolveQuadric( coeffs, solutions );

		switch( numSolutions )
		{
		default:
		case 0:
			return;

		case 1:
			if( solutions[0] > tMin )
			{
				hit.bHit = true;
				hit.dRange = hit.dRange2 = solutions[0];
			}
			break;
		case 2:
			if( solutions[0] > tMin && solutions[1] > tMin)
			{
				if( solutions[0] < solutions[1] )
				{
					hit.bHit = true;
					hit.dRange = solutions[0];
					hit.dRange2 = solutions[1];
				}

				if( solutions[1] < solutions[0] )
				{
					hit.bHit = true;
					hit.dRange = solutions[1];
					hit.dRange2 = solutions[0];
				}	
			}
			else if( solutions[0] > tMin )
			{
				hit.bHit = true;
				hit.dRange = solutions[0];
				hit.dRange2 = 0.0; //solutions[0];
			}
			else if( solutions[1] > tMin )
			{
				hit.bHit = true;
				hit.dRange = solutions[1];
				hit.dRange2 = 0.0; //solutions[1];
			}
			break;
		}

		if( hit.bHit ) { RISE_PROFILE_INC(nSphereIntersectionHits); }
	}


	extern void RaySphereIntersection( const Ray& ray, HIT& hit, const Scalar radius )
	{
		RISE_PROFILE_INC(nSphereIntersectionTests);
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;

		Scalar		fSqrRadius = radius*radius;
		Scalar		coeffs[3] = {0};

		coeffs[0] = Vector3Ops::SquaredModulus( ray.Dir() );
		coeffs[1] = 2 * (ray.Dir().x * ray.origin.x + ray.Dir().y * ray.origin.y + ray.Dir().z * ray.origin.z);
		Vector3 d = Vector3Ops::mkVector3( ray.origin, Point3(0, 0, 0) );
		coeffs[2] = Vector3Ops::SquaredModulus( d ) - (fSqrRadius);

		Scalar		solutions[2] = {0};

		// Self-intersection floor, scale-relative to the coordinates
		// involved rather than the fixed absolute NEARZERO (1e-12).  A
		// shadow or continuation ray whose origin was PUBLISHED from a hit
		// on this sphere (Object::IntersectRay backs the hit point off by
		// SURFACE_INTERSEC_ERROR = 1e-12 along the incoming ray) has a
		// mathematically ~1e-12 root here, t = depth / |cos out|, which
		// straddles 1e-12 whenever the outgoing ray is more grazing than
		// the incoming one; and at coordinates of ~1e3 the eye hit's own
		// range already carries ~1e-12 of absolute round-off, so the
		// published point lands INSIDE the sphere for a sizeable fraction
		// of pixels.  Measured 2026-09-05 (PT vs BDPT, which advances its
		// shadow rays 1e-6 and never sees it): a radius-1000 Lambertian
		// sphere under an omni light read PT 0.62x of BDPT (1.000 at unit
		// scale; advancing PT's shadow ray alone restores 1.0006), and a
		// unit sphere carrying a full-sphere transmissive weave lit from
		// behind read 0.32x.  Same pattern and same floor as
		// RayBilinearPatchIntersection's debt-21 fix and the per-axis band
		// BoxGeometry::DropSelfHitRoot uses (debt 25, docs/CLOTH_FABRIC_
		// DESIGN.md section 15): fixed once here at the producer so every
		// caller (IntersectRay, IntersectRay_IntersectionOnly, the CSG
		// exit probe) benefits.  |origin| + radius is the coordinate scale
		// that matters: a point published from this surface sits at
		// |origin| ~ radius.
		const Scalar coordScale = fabs( ray.origin.x ) + fabs( ray.origin.y ) + fabs( ray.origin.z ) + fabs( radius );
		const Scalar tMin = NEARZERO * ( Scalar(1) + coordScale );

		int			numSolutions = Polynomial::SolveQuadric( coeffs, solutions );

		switch( numSolutions )
		{
		default:
		case 0:
			return;

		case 1:
			if( solutions[0] > tMin )
			{
				hit.bHit = true;
				hit.dRange = hit.dRange2 = solutions[0];
			}
			break;
		case 2:
			if( solutions[0] > tMin && solutions[1] > tMin)
			{
				if( solutions[0] < solutions[1] )
				{
					hit.bHit = true;
					hit.dRange = solutions[0];
					hit.dRange2 = solutions[1];
				}

				if( solutions[1] < solutions[0] )
				{
					hit.bHit = true;
					hit.dRange = solutions[1];
					hit.dRange2 = solutions[0];
				}	
			}
			else if( solutions[0] > tMin )
			{
				hit.bHit = true;
				hit.dRange = solutions[0];
				hit.dRange2 = 0.0; //solutions[0];
			}
			else if( solutions[1] > tMin )
			{
				hit.bHit = true;
				hit.dRange = solutions[1];
				hit.dRange2 = 0.0; //solutions[1];
			}
			break;
		}

		if( hit.bHit ) { RISE_PROFILE_INC(nSphereIntersectionHits); }
	}

}


