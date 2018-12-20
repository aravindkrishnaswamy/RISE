//////////////////////////////////////////////////////////////////////
//
//  Direction.h - Contains definition for a
//                Euler angle class called Direction
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 14, 2001
//  Tabs: 4
//  Comments: This is from Chad Faragher's Math3D
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIRECTION_
#define DIRECTION_

////////////////////////////////////
//
// Euler angle class called Direction
//
////////////////////////////////////

namespace RISE
{
	struct Direction
	{
		Scalar		x;

		// Default constructor
		inline Direction( void ) :
		x( 0 )
		{}

		// Copy constructor
		inline Direction( const Direction& d ) :
		x( d.x )
		{}

		// Scalar constructor
		inline Direction( const Scalar n )  
		{
			x = Scalar( fmod( Scalar( n ), TWO_PI ) );
			if( x < -PI ) {
				x += TWO_PI;
			} else if( x > PI ) {
				x -= TWO_PI;
			}
		}

		// addition operator
		inline friend const Direction operator+( const Direction& a, const Direction& b ) 
		{
			return Direction( a.x + b.x );
		}
	};
}

#endif
