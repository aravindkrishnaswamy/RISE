//////////////////////////////////////////////////////////////////////
//
//  Plane.h - Definition of an oriented 3D plane (point-normal form)
//
//  Author: Russell O'Connor
//  Date of Birth: May 17, 2004
//  Tabs: 4
//  Comments: `normal` is unit-length when constructed via (p, n) or Set().
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PLANE_
#define PLANE_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	class Plane
	{
	public:
		Point3		origin;
		/*co-*/Vector3		normal;

		Plane( ){}

		Plane( const Point3& p, const Vector3& n ) : 
		origin( p ), normal( Vector3Ops::Normalize(n) )
		{}

		Plane( const Plane& r ) : 
		origin( r.origin ), normal( r.normal )
		{
	//		assert(Vector3Ops::SquaredModulus(r.normal) <= epsilon);
		}

		void		Set( const Point3& p, const Vector3& n )
		{
			origin = p;
			normal = Vector3Ops::Normalize(n);
		}

		void		Set( const Vector3& n, const Point3& p )
		{
			origin = p;
			normal = Vector3Ops::Normalize(n);
		}

		// Signed perpendicular distance from the plane: positive on the
		// normal-facing half-space, assuming `normal` is unit-length.
		Scalar		Distance( const Point3& p ) const {
			return Vector3Ops::Dot(Vector3Ops::mkVector3(p,origin), normal);
		}

		Plane& operator=( const Plane& r )
		{
			origin = r.origin;
			normal = r.normal;
	//		assert(Vector3Ops::SquaredModulus(r.normal) <= epsilon);

			return *this;
		}

		// Oriented plane equality: normals match within epsilon (opposite
		// normals are not equal) and b.origin lies within epsilon of plane a.
		static inline bool AreEqual( const Plane& a, const Plane& b, const Scalar epsilon )
		{
			return Vector3Ops::AreEqual(a.normal, b.normal, epsilon) &&
				(fabs(a.Distance(b.origin)) <= epsilon);
		}
	};
}

#endif
