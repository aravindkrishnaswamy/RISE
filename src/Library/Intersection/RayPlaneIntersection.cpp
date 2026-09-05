//////////////////////////////////////////////////////////////////////
//
//  RayPlaneIntersection.cpp - Implements a ray plane intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"

namespace RISE
{

	void RayPlaneIntersection( const Ray& ray, HIT& hit, const Vector3& vPlaneNormal )
	{
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;

		const Scalar		Vd = Vector3Ops::Dot( ray.Dir(), vPlaneNormal );

		if( Vd < NEARZERO && Vd > -NEARZERO ) {
			return;
		}

		hit.dRange = Vector3Ops::Dot(vPlaneNormal, Vector3(-ray.origin.x, -ray.origin.y, -ray.origin.z)) / Vd;

		// Scale-relative self-intersection floor -- same mechanism and
		// same floor as RayBilinearPatchIntersection (debt 21) and
		// RaySphereIntersection (see its note): a ray published from a
		// hit on this plane (1e-12 back-off along the incoming ray) that
		// CROSSES the plane has its self root at t = 1e-12 * |cos in| /
		// |cos out|, which the fixed NEARZERO gate lets through whenever
		// the outgoing ray is more grazing than the incoming one.
		// Measured 2026-09-05 (docs/CLOTH_FABRIC_DESIGN.md section 15,
		// debt 25 sibling audit): an infiniteplane_geometry / circular-
		// disk carrying a full-sphere transmissive weave lit from behind
		// read PT 1/354 of BDPT at unit scale -- PT's NEE shadow rays were
		// almost all self-occluded -- while a clippedplane_geometry twin
		// (already on the scale-relative floor) read 1.000.
		const Scalar coordScale = fabs( ray.origin.x ) + fabs( ray.origin.y ) + fabs( ray.origin.z );
		const Scalar tMin = NEARZERO * ( Scalar(1) + coordScale );

		if( hit.dRange > tMin ) {
			hit.bHit = true;
			hit.dRange2 = hit.dRange;
		}
	}

	int RayPlaneIntersectionSimple(
									const Vector3& point, 
									const Vector3& vPlaneNormal,
									const Scalar planeD
									)
	{
		const Scalar		Vd = Vector3Ops::Dot( point, vPlaneNormal ) + planeD;

		if( Vd < -NEARZERO ) {
			return -1;
		} else if( Vd > NEARZERO ) {
			return 1;
		}

		return 0;
	}

}

