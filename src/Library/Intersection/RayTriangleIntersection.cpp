//////////////////////////////////////////////////////////////////////
//
//  RayTriangleIntersection.cpp - Implements a ray triangle intersection
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
#include "../Utilities/Profiling.h"
#include <algorithm>		// std::max (self-hit floor's largest-vertex coordinate scale)

namespace RISE
{

	void RayTriangleIntersection( 
		const Ray& ray,
		TRIANGLE_HIT& hit,
		const Point3& vPt1,
		const Vector3& vEdgeA,
		const Vector3& vEdgeB
		)
	{
		RISE_PROFILE_INC(nTriangleIntersectionTests);

		hit.bHit = false;
		hit.dRange = RISE_INFINITY;
		hit.dRange2 = RISE_INFINITY;

		// Moller-Trumbore ray/triangle intersection with early exits.
		// This preserves the existing alpha/beta/range contract used by the callers.
		const Vector3 pvec = Vector3Ops::Cross( ray.Dir(), vEdgeB );
		const Scalar determinant = Vector3Ops::Dot( vEdgeA, pvec );

		if( determinant > -NEARZERO && determinant < NEARZERO ) {
			return;
		}

		const Scalar oodet = 1.0 / determinant;
		const Vector3 tvec = Vector3Ops::mkVector3( ray.origin, vPt1 );
		const Scalar a = Vector3Ops::Dot( tvec, pvec ) * oodet;

		if( (a < 0.0-NEARZERO) || (a > 1.0+NEARZERO) ) {
			return;
		}

		const Vector3 qvec = Vector3Ops::Cross( tvec, vEdgeA );
		const Scalar b = Vector3Ops::Dot( ray.Dir(), qvec ) * oodet;

		if( (b<0.0-NEARZERO) || ((a+b)>1.0+NEARZERO) ) {
			return;
		}

		hit.dRange = Vector3Ops::Dot( vEdgeB, qvec ) * oodet;

		// Scale-relative self-intersection floor -- same mechanism and
		// same floor as RayBilinearPatchIntersection (debt 21) and
		// RaySphereIntersection (see its note).  A ray published from a
		// hit on this triangle (1e-12 back-off along the incoming ray)
		// that crosses it has its self root at t = 1e-12 * |cos in| /
		// |cos out|, which the fixed NEARZERO gate let through; and at
		// coordinates ~1e3 the hit's own round-off puts the published
		// point on either side.  Measured 2026-09-05 (docs/CLOTH_FABRIC_
		// DESIGN.md section 15, debt 25 sibling audit): a two-triangle
		// PLY quad carrying a full-sphere transmissive weave lit from
		// behind read PT 1/157 of BDPT at unit scale; a Lambertian
		// tessellated sphere at radius 1000 read PT 3-8 % dark.  The
		// coordinate scale is the origin's plus the LARGEST of the three
		// vertices' -- vPt1, vPt1+vEdgeA, vPt1+vEdgeB.  a8bef210 charged
		// vPt1's alone, which is only the largest by accident of which corner
		// the mesh happened to store first: a sliver whose far corner sits at
		// 1e4 while vPt1 sits at the origin got a unit-scale floor, exactly
		// the under-estimate this floor exists to prevent (adversarial review
		// of a8bef210, P2-2 -- the commit's own comment already said "the
		// vertex's", which reads as the triangle's extent).  Matches
		// RayBilinearPatchIntersection, which maxes over all four corners.
		const Scalar oL1 = fabs( ray.origin.x ) + fabs( ray.origin.y ) + fabs( ray.origin.z );
		const Scalar v1L1 = fabs( vPt1.x ) + fabs( vPt1.y ) + fabs( vPt1.z );
		const Scalar v2L1 = fabs( vPt1.x + vEdgeA.x ) + fabs( vPt1.y + vEdgeA.y ) + fabs( vPt1.z + vEdgeA.z );
		const Scalar v3L1 = fabs( vPt1.x + vEdgeB.x ) + fabs( vPt1.y + vEdgeB.y ) + fabs( vPt1.z + vEdgeB.z );
		const Scalar coordScale = oL1 + std::max( v1L1, std::max( v2L1, v3L1 ) );
		const Scalar tMin = NEARZERO * ( Scalar(1) + coordScale );

		if( hit.dRange >= tMin ) {
			hit.dRange2 = hit.dRange;
			hit.bHit = true;
			hit.alpha = a;
			hit.beta = b;
			RISE_PROFILE_INC(nTriangleIntersectionHits);
		}
	}



	////////////////////////////////////////////// DEAD CODE ///////////////////////////////////////
	/*

		// Didier Badouel's intersection test Graphics Gems pp.390
		// This is pretty much a straight rip of Badouel's algorithm

		// Test if it intersects the plane first
		Vector3	vTriNormal = vEdgeA * vEdgeB;
		Scalar		fDotNormalDir = vTriNormal % ray.Dir();

		#define findthemax(x,y,z) \
		((x > y) ? ((x > z) ? 0 : 2) : ((y > z) ? 1 : 2))

		if( fDotNormalDir != 0.0f )
		{
			// Ray intersects the plane its sitting on, compute the intersection point
			Scalar		fPlaneDistance = - (vTriNormal % vPt1);
			Scalar		fN = - ((vTriNormal % ray.origin) + fPlaneDistance);
			Scalar		fDistance = fN / fDotNormalDir;

			if( fDistance > 1.0f )
			{
				// Plane is also in front of us, so definite plane intersection at this point
				Point3		ptIntersectionPoint = ray.PointAtLength( fDistance );

				const Point3&	P1 = vPt1;
				const Point3&	P2 = vPt2;
				const Point3&	P3 = vPt3;

				// Determine the polygon's dominant axis
				unsigned int Nx = fabs(vTriNormal.x);
				unsigned int Ny = fabs(vTriNormal.y);
				unsigned int Nz = fabs(vTriNormal.z);
				
				unsigned char dominant_axis = findthemax( Nx, Ny, Nz );

				unsigned char i1, i2;

				switch( dominant_axis )
				{
				case 0:
					i1 = 1;
					i2 = 2;
					break;
				case 1:
					i1 = 0;
					i2 = 2;
					break;
				case 2:
					i1 = 0;
					i2 = 1;
				}

				Scalar		alpha, beta;

				bool		inter = false;

				Scalar		u0 = ptIntersectionPoint[i1] - P1[i1];
				Scalar		v0 = ptIntersectionPoint[i2] - P1[i2];

				Scalar		u1 = P2[i1] - P1[i1];
				Scalar		v1 = P2[i2] - P1[i2];

				Scalar		u2 = P3[i1] - P1[i1];
				Scalar		v2 = P3[i2] - P1[i2];

				if( u1 == 0 )
				{
					beta = u0 / u2;
					if( (beta >= 0) && (beta <= 1.0) )
					{
						alpha = (v0 - beta*v2) / v1;
						inter = ((alpha >= 0) && ((alpha+beta) <= 1.0));
					}
				}
				else
				{
					beta = (v0*u1 - u0*v1)/(v2*u1-u2*v1);
					if( (beta >= 0) && (beta <= 1.0) )
					{
						alpha = (u0 - beta*u2)/u1;
						inter = ((alpha >= 0) && ((alpha+beta) <= 1.0));
					}
				}

				if( inter )
				{
					// There is an intersection
					hit.dRange = fDistance;
					hit.dRange2 = hit.dRange;
					hit.bHit = true;
					hit.alpha = alpha;
					hit.beta = beta;
				}
			}
		}

	*/

}
