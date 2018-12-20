//////////////////////////////////////////////////////////////////////
//
//  Quaternion.h - Contains definition for a templated quaternion class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 15, 2001
//  Tabs: 4
//  Comments: Chad Faragher wrote most of this in the original Math3D
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef QUATERNION_
#define QUATERNION_

////////////////////////////////////
//
// Quaternion class
//
////////////////////////////////////

namespace RISE
{
	struct Quaternion
	{
		// Actual data
		Scalar		s;
		Vector3		v;

		//
		// Constructors
		//

		// Default constructor
		inline		Quaternion( void ) : 
			s( 0 )
		{}

		// Constructor from a scalar and a vector
		inline		Quaternion( const Scalar s_, const Vector3& v_ ) : 
		s( s_ ),
		v( v_ )
		{}

		// Linear combination constructor
		inline		Quaternion( const Scalar a, const Scalar b, const Scalar c, const Scalar d ) :
		s( a ),
		v( Vector3( b, c, d ) )
		{}

		// Copy constructor
		inline		Quaternion( const Quaternion& q ) :
		s( q.s ),
		v( q.v )
		{}

		//
		// Operators
		//

		// Addition
		inline friend Quaternion operator +( const Quaternion& a, const Quaternion& b ) 
		{
			return Quaternion( a.s + b.s,
				Vector3( a.v.x + b.v.x, a.v.y + b.v.y, a.v.z + b.v.z ) );
		}

		// Subtraction
		inline friend Quaternion operator -( const Quaternion& a, const Quaternion& b ) 
		{
			return Quaternion( a.s - b.s, a.v - b.v );
		}

		// Scalar Multiplication
		inline friend Quaternion operator *( const Quaternion& a, const Scalar t )  
		{
			return Quaternion( a.s * t, a.v.x*t, a.v.y*t, a.v.z*t );
		}

		inline friend Quaternion operator *( const Scalar t, const Quaternion& a )  
		{
			return Quaternion( a.s * t, a.v.x*t, a.v.y*t, a.v.z*t );
		}

		// Multiplication
		inline friend Quaternion operator *( const Quaternion& a, const Quaternion& b ) 
		{
			// linear combination form
			return Quaternion( 
				a.s * b.s - a.v.x * b.v.x - a.v.y * b.v.y - a.v.z * b.v.z,
				a.s * b.v.x + a.v.x * b.s + a.v.y * b.v.z - a.v.z * b.v.y,
				a.s * b.v.y - a.v.x * b.v.z + a.v.y * b.s + a.v.z * b.v.x,
				a.s * b.v.z + a.v.x * b.v.y - a.v.y * b.v.x + a.v.z * b.s
			);
		}
	};
}

#endif
