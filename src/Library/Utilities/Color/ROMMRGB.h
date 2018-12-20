//////////////////////////////////////////////////////////////////////
//
//  ROMMRGB.h - Linear ROMM RGB.  ROMM RGB was defined by Kodak as a
//    large gamut RGB format.  You can find more information here:
//    http://www.color.org/rommrgb.pdf
//    http://www.colour.org/tc8-05/Docs/colorspace/PICS2000_RIMM-ROMM.pdf
//
//    Interesting Aside: 
//    Interestingly enough it seems when ROMM RGB was introduced/proposed
//    professional photographers didn't know what it was and it wasn't
//    widely used.  Accoring to Chris Cox, Kodak released it to the public
//    under the name of 'ProPhotoRGB' as a way of appealing to photographers
//    which worked, since now ProPhotoRGB is used by many photographers in 
//    their color managed workflows (particularily for printing).
//
//    We use the term ProPhotoRGB in RISE as will, however in our case
//    when we refer to ROMM RGB we mean linear ROMM RGB, and when we 
//    refer to ProPhotoRGB, we mean non-linear ROMM RGB.  Note that the only
//    difference is the linearty, and it is something I have done so that
//    I don't have to keep typing 'Linear' and 'NonLinear' everywhere in 
//    the code.  It is purely for my typing ease, and I apologize if this
//    causes any confusion.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 14, 2006
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ROMMRGBPel_
#define ROMMRGBPel_

namespace RISE
{
	struct ROMMRGBPel
	{
		Chel	r, g, b;

		// Default constructor
		inline ROMMRGBPel( ) :
		r( 0 ),
		g( 0 ),
		b( 0 )
		{}

		inline ROMMRGBPel( const Chel val ) :
		r( val ),
		g( val ),
		b( val )
		{}

		inline ROMMRGBPel( const Chel val[3] ) :
		r( val[0] ),
		g( val[1] ),
		b( val[2] )
		{}


		// Copy constructor
		inline ROMMRGBPel( const ROMMRGBPel& k ) :
		r( k.r ),
		g( k.g ),
		b( k.b )
		{}

		// Alternate constructor
		inline ROMMRGBPel( const Chel r_, const Chel g_, const Chel b_ ) : 
		r( r_ ),
		g( g_ ),
		b( b_ )
		{}

		// Conversion
		inline ROMMRGBPel( const XYZPel& xyz )
		{
			*this = ColorUtils::XYZtoROMMRGB( xyz );
		}

		inline ROMMRGBPel( const Rec709RGBPel& rec709 )
		{
			*this = ColorUtils::Rec709RGBtoROMMRGB( rec709 );
		}

		inline ROMMRGBPel( const ProPhotoRGBPel& rgb )
		{
			*this = ColorUtils::ProPhotoRGB_Linearization( rgb );
		}
		
		inline ROMMRGBPel( const sRGBPel& rgb )
		{
			*this = ColorUtils::sRGBtoROMMRGB( rgb );
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

		inline ROMMRGBPel& operator=( const ROMMRGBPel& v )  
		{
			r = v.r;
			g = v.g;
			b = v.b;
			
			return *this;  // Assignment operator returns left side.
		}

		inline ROMMRGBPel& operator=( const Scalar& d )  
		{
			r = g = b = d;
			return *this;  // Assignment operator returns left side.
		}


	#define COLOR_CLASS_TYPE ROMMRGBPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE

	};
}

#endif
