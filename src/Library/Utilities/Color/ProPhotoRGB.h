//////////////////////////////////////////////////////////////////////
//
//  ProPhotoRGB.h - NonLinear ROMM RGB.  ROMM RGB was defined by Kodak as a
//    large gamut RGB format.  NOTE: ProPhotoRGB and ROMM RGB are the same
//    Read below for the reasons behind the naming difference in RISE
//    You can find more information here:
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

#ifndef ProPhotoRGBPel_
#define ProPhotoRGBPel_

namespace RISE
{
	struct ProPhotoRGBPel
	{
		Chel	r, g, b;

		// Default constructor
		inline ProPhotoRGBPel( ) :
		r( 0 ),
		g( 0 ),
		b( 0 )
		{}

		inline ProPhotoRGBPel( const Chel val ) :
		r( val ),
		g( val ),
		b( val )
		{}

		inline ProPhotoRGBPel( const Chel val[3] ) :
		r( val[0] ),
		g( val[1] ),
		b( val[2] )
		{}


		// Copy constructor
		inline ProPhotoRGBPel( const ProPhotoRGBPel& k ) :
		r( k.r ),
		g( k.g ),
		b( k.b )
		{}

		// Alternate constructor
		inline ProPhotoRGBPel( const Chel r_, const Chel g_, const Chel b_ ) : 
		r( r_ ),
		g( g_ ),
		b( b_ )
		{}

		// Conversion
		inline ProPhotoRGBPel( const ROMMRGBPel& rgb )
		{
			*this = ColorUtils::ProPhotoRGB_NonLinearization( rgb );
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

		inline ProPhotoRGBPel& operator=( const ProPhotoRGBPel& v )  
		{
			r = v.r;
			g = v.g;
			b = v.b;
			
			return *this;  // Assignment operator returns left side.
		}

		inline ProPhotoRGBPel& operator=( const Scalar& d )  
		{
			r = g = b = d;
			return *this;  // Assignment operator returns left side.
		}


	#define COLOR_CLASS_TYPE ProPhotoRGBPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE

	};
}

#endif
