//////////////////////////////////////////////////////////////////////
//
//  CIE_xyY.h - Contains CIE xyY color class, which is a normalized
//    variant of the CIE XYZ type.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CIE_xyY_
#define CIE_xyY_

namespace RISE
{
	struct xyYPel
	{
		Chel	x;
		Chel	y;
		Chel	Y;

		// Default constructor
		inline xyYPel( ) :
		x( 0 ),
		y( 0 ),
		Y( 0 )
		{}

		inline xyYPel( const Chel val ) :
		x( val ),
		y( val ),
		Y( val )
		{}

		inline xyYPel( const Chel val[3] ) :
		x( val[0] ),
		y( val[1] ),
		Y( val[2] )
		{}

		// Copy constructor
		inline xyYPel( const xyYPel& k ) :
		x( k.x ),
		y( k.y ),
		Y( k.Y )
		{}

		// Alternate constructor
		inline xyYPel( const Chel x_, const Chel y_, const Chel Y_ ) : 
		x( x_ ),
		y( y_ ),
		Y( Y_ )
		{}

		inline xyYPel& operator=( const xyYPel& v )  
		{
			x = v.x;
			y = v.y;
			Y = v.Y;
			
			return *this;  // Assignment operator returns left side.
		}

		inline xyYPel& operator=( const Scalar& d )  
		{
			x = y = Y = d;
			return *this;  // Assignment operator returns left side.
		}

		// Array style access
		inline		Scalar&		operator[]( const unsigned int i )
		{
			return i==0 ? x : i==1 ? y : Y;
		}

		// Array style access
		inline		Scalar		operator[]( const unsigned int i ) const
		{
			return i==0 ? x : i==1 ? y : Y;
		}

	#define COLOR_CLASS_TYPE xyYPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE
	};
}

#endif
