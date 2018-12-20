//////////////////////////////////////////////////////////////////////
//
//  Ray.h - Definition of a 3D ray class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_
#define RAY_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	class Ray
	{
	public:
		Point3		origin;
		Vector3	dir;

		Ray( ){}

		Ray( const Point3& p, const Vector3& d ) : 
		origin( p ), dir( d )
		{}

		Ray( const Ray& r ) : 
		origin( r.origin ), dir( r.dir )
		{}

		Point3		PointAtLength( const Scalar s ) const
		{
			return( Point3Ops::mkPoint3(origin, (dir * s)) );
		}

		void		Advance( const Scalar s )
		{
			origin = Point3Ops::mkPoint3(origin, (dir * s));
		}

		void		Set( const Point3& p, const Vector3& d )
		{
			origin = p;
			dir = d;
		}

		void		Set( const Vector3& d, const Point3& p )
		{
			origin = p;
			dir = d;
		}

		Ray& operator=( const Ray& r )
		{
			origin = r.origin;
			dir = r.dir;

			return *this;
		}

		static inline bool AreEqual( const Ray& a, const Ray& b, const Scalar epsilon )
		{
			return Point3Ops::AreEqual(a.origin, b.origin, epsilon ) &&
				Vector3Ops::AreEqual(a.dir, b.dir, epsilon );
		}
	};
}

#endif
