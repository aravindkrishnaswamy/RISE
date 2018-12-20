//////////////////////////////////////////////////////////////////////
//
//  sRGB.h - An sRGB PEL, which is the nonlinear Rec709 RGB space that 
//  just swell for monitors!
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SRGB_
#define SRGB_

namespace RISE
{
	struct sRGBPel
	{
	public:
		Chel	r, g, b;

		// Default constructor
		inline sRGBPel( ) :
		r( 0 ),
		g( 0 ),
		b( 0 )
		{}

		inline sRGBPel( const Chel val ) :
		r( val ),
		g( val ),
		b( val )
		{}

		inline sRGBPel( const Chel val[3] ) :
		r( val[0] ),
		g( val[1] ),
		b( val[2] )
		{}

		// Copy constructor
		inline sRGBPel( const sRGBPel& k ) :
		r( k.r ),
		g( k.g ),
		b( k.b )
		{}

		// Alternate constructor
		inline sRGBPel( const Chel r_, const Chel g_, const Chel b_ ) : 
		r( r_ ),
		g( g_ ),
		b( b_ )
		{}

		// Conversion
		inline sRGBPel( const Rec709RGBPel& rgb )
		{
			*this = ColorUtils::sRGBNonLinearization( rgb );
		}

		inline sRGBPel& operator=( const sRGBPel& v )  
		{
			r = v.r;
			g = v.g;
			b = v.b;
			
			return *this;  // Assignment operator returns left side.
		}

		inline sRGBPel& operator=( const Scalar& d )  
		{
			r = g = b = d;
			return *this;  // Assignment operator returns left side.
		}

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

	#define COLOR_CLASS_TYPE sRGBPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE
	};
}

#endif
