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
		hit.dRange = INFINITY;

		const Scalar		Vd = Vector3Ops::Dot( ray.dir, vPlaneNormal );

		if( Vd < NEARZERO && Vd > -NEARZERO ) {
			return;
		}

		hit.dRange = Vector3Ops::Dot(vPlaneNormal, Vector3(-ray.origin.x, -ray.origin.y, -ray.origin.z)) / Vd;

		if( hit.dRange > NEARZERO ) {
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

