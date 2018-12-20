//////////////////////////////////////////////////////////////////////
//
//  RGB.h - An RGB PEL, which is linear RGB, the most commonly
//  used system
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RGBPel_
#define RGBPel_

namespace RISE
{
	struct RGBPel
	{
		Chel	r, g, b;

		// Default constructor
		inline RGBPel( ) :
		r( 0 ),
		g( 0 ),
		b( 0 )
		{}

		inline RGBPel( const Chel val ) :
		r( val ),
		g( val ),
		b( val )
		{}

		inline RGBPel( const Chel val[3] ) :
		r( val[0] ),
		g( val[1] ),
		b( val[2] )
		{}


		// Copy constructor
		inline RGBPel( const RGBPel& k ) :
		r( k.r ),
		g( k.g ),
		b( k.b )
		{}

		// Alternate constructor
		inline RGBPel( const Chel r_, const Chel g_, const Chel b_ ) : 
		r( r_ ),
		g( g_ ),
		b( b_ )
		{}

		// Constructor from sRGB
		RGBPel( const sRGBPel& srgb );


		// Constructor from XYZ
		RGBPel( const XYZPel& xyz_ );


		// Constructor from xyY
		RGBPel( const xyYPel& xyy_ );

		// Array style access
		inline		Scalar&		operator[]( const unsigned int i )
		{
			return i==0 ? r : i==1 ? g : b;
		}

		// Array style access
		inline		Scalar		operator[]( const unsigned int i ) const
		{
			return i==0 ? r : i==1 ? g : b;
		}

		// Returns the luminance of the pixel
		inline Scalar Luminance( ) const
		{
			return 0.212671 * r + 0.715160 * g + 0.072169 * b;
		}

		inline RGBPel& operator=( const RGBPel& v )  
		{
			r = v.r;
			g = v.g;
			b = v.b;
			
			return *this;  // Assignment operator returns left side.
		}

		inline RGBPel& operator=( const Scalar& d )  
		{
			r = g = b = d;
			return *this;  // Assignment operator returns left side.
		}


	#define COLOR_CLASS_TYPE RGBPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE

	};
}

#endif
