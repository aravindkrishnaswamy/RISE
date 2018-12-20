//////////////////////////////////////////////////////////////////////
//
//  Rec709_RGB.h - Linear Rec709 RGB.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef Rec709RGBPel_
#define Rec709RGBPel_

namespace RISE
{
	struct Rec709RGBPel
	{
		Chel	r, g, b;

		// Default constructor
		inline Rec709RGBPel( ) :
		r( 0 ),
		g( 0 ),
		b( 0 )
		{}

		inline Rec709RGBPel( const Chel val ) :
		r( val ),
		g( val ),
		b( val )
		{}

		inline Rec709RGBPel( const Chel val[3] ) :
		r( val[0] ),
		g( val[1] ),
		b( val[2] )
		{}


		// Copy constructor
		inline Rec709RGBPel( const Rec709RGBPel& k ) :
		r( k.r ),
		g( k.g ),
		b( k.b )
		{}

		// Alternate constructor
		inline Rec709RGBPel( const Chel r_, const Chel g_, const Chel b_ ) : 
		r( r_ ),
		g( g_ ),
		b( b_ )
		{}

		// Conversion
		inline Rec709RGBPel( const XYZPel& xyz )
		{
			*this = ColorUtils::XYZtoRec709RGB( xyz );
		}

		inline Rec709RGBPel( const sRGBPel& srgb )
		{
			*this = ColorUtils::Linearize_sRGB( srgb );
		}

		inline Rec709RGBPel( const ROMMRGBPel& rgb )
		{
			*this = ColorUtils::ROMMRGBtoRec709RGB( rgb );
		}

		inline Rec709RGBPel( const ProPhotoRGBPel& rgb )
		{
			*this = ColorUtils::ProPhotoRGBtoRec709RGB( rgb );
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

		inline Rec709RGBPel& operator=( const Rec709RGBPel& v )  
		{
			r = v.r;
			g = v.g;
			b = v.b;
			
			return *this;  // Assignment operator returns left side.
		}

		inline Rec709RGBPel& operator=( const Scalar& d )  
		{
			r = g = b = d;
			return *this;  // Assignment operator returns left side.
		}


	#define COLOR_CLASS_TYPE Rec709RGBPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE

	};
}

#endif
