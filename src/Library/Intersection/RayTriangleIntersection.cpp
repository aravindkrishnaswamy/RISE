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
		hit.bHit = false;
		hit.dRange = INFINITY;

		// Here is my intersection test.  
		// You can find this in Real-time Rendering 10.5.2
		
		// Equation of the ray
		// X = raybegin + nRange * raydir
		
		// Equation of the triangle
		// X = vPoint1 + X * vPoint2 + Y * vPoint3;
		
		Matrix3	m;
		m._00 = ray.dir.x;
		m._01 = ray.dir.y;
		m._02 = ray.dir.z;

		m._10 = vEdgeA.x;
		m._11 = vEdgeA.y;
		m._12 = vEdgeA.z;

		m._20 = vEdgeB.x;
		m._21 = vEdgeB.y;
		m._22 = vEdgeB.z;

		const Scalar determinant = Matrix3Ops::Determinant(m);

		if( determinant > -NEARZERO && determinant < NEARZERO ) {
			return;
		}

		Scalar	oodet = 1.0/determinant;
		
		Matrix3	m2;
		// The vector v gives the vector from the ray's begining to the first point on the triangle
		const Vector3	v = Vector3Ops::mkVector3( vPt1, ray.origin );

		m2 = m;
		m2._10 = v.x;
		m2._11 = v.y;
		m2._12 = v.z;

		const Scalar a = -(Matrix3Ops::Determinant(m2))*oodet;

		if( (a < 0.0-NEARZERO) || (a > 1.0+NEARZERO) ) {
			return;
		}

		m2 = m;
		m2._20 = v.x;
		m2._21 = v.y;
		m2._22 = v.z;
		const Scalar b = -(Matrix3Ops::Determinant(m2))*oodet;

		if( (b<0.0-NEARZERO) || ((a+b)>1.0+NEARZERO) ) {
			return;
		}

		m2 = m;
		m2._00 = v.x;
		m2._01 = v.y;
		m2._02 = v.z;
		hit.dRange = (Matrix3Ops::Determinant(m2))*oodet;

		if( hit.dRange >= NEARZERO ) {
			hit.dRange2 = hit.dRange;
			hit.bHit = true;
			hit.alpha = a;
			hit.beta = b;
		}
	}



	////////////////////////////////////////////// DEAD CODE ///////////////////////////////////////
	/*

		// Didier Badouel's intersection test Graphics Gems pp.390
		// This is pretty much a straight rip of Badouel's algorithm

		// Test if it intersects the plane first
		Vector3	vTriNormal = vEdgeA * vEdgeB;
		Scalar		fDotNormalDir = vTriNormal % ray.dir;

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

