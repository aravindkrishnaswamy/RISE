//////////////////////////////////////////////////////////////////////
//
//  Points.h - Contains definition for a templated 2-D and 3D
//             Point classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 14, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POINTS_
#define POINTS_

namespace RISE
{
	struct Point2
	{
		//
		// Actual data
		//

		Scalar		x;
		Scalar		y;

		//
		// Constructors
		//
		inline		Point2( ) : 
		x( 0 ),
		y( 0 )
		{};

		inline		Point2( const Scalar val[2] ) : 
		x( val[0] ),
		y( val[1] )
		{};

		inline		Point2( const Scalar x_, const Scalar y_ ) : 
		x( x_ ),
		y( y_ )
		{};

		// Copy Constructor
		inline		Point2( const Point2& p ) : 
		x( p.x ),
		y( p.y )
		{};

		// Array style access
		inline		Scalar		operator[]( const unsigned int i ) const
		{
			return i==0 ? x : y;
		}

		// Array style access
		inline		Scalar&		operator[]( const unsigned int i )
		{
			return i==0 ? x : y;
		}
	};

	struct Point3
	{
		//
		// Actual data
		//
		Scalar		x;
		Scalar		y;
		Scalar		z;

		//
		// Constructors
		//
		inline		Point3( ) : 
		x( 0 ),
		y( 0 ),
		z( 0 )
		{};

		inline		Point3( const Scalar val[3] ) : 
		x( val[0] ),
		y( val[1] ),
		z( val[2] )
		{};

		inline		Point3( const Scalar x_, const Scalar y_, const Scalar z_ ) : 
		x( x_ ),
		y( y_ ),
		z( z_ )
		{};

		// Copy Constructor
		inline		Point3( const Point3& p ) : 
		x( p.x ),
		y( p.y ),
		z( p.z )
		{};

		// Array style access
		inline		Scalar		operator[]( const unsigned int i ) const
		{
			return i==0 ? x : i==1 ? y : z;
		}

		inline		Scalar&		operator[]( const unsigned int i )
		{
			return i==0 ? x : i==1 ? y : z;
		}
	};
}

#endif
