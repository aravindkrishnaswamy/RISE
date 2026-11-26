//////////////////////////////////////////////////////////////////////
//
//  AP1RGB.h - Linear ACES AP1 RGB (a.k.a. ACEScg).
//
//    AP1 is an Academy-defined wide-gamut RGB working space whose
//    primaries all lie INSIDE the CIE 1931 visible spectral locus
//    (R=(0.713, 0.293), G=(0.165, 0.830), B=(0.128, 0.044)) and
//    whose whitepoint is the ACES D60-ish chromaticity
//    (0.32168, 0.33767).
//
//    Used as the industry-standard internal working space for VFX and
//    increasingly for film mastering.  Pre-staged in RISE so a future
//    migration from Rec.709-Linear → AP1 can be:
//      1. retrain the JH LUT with `--target acescg`
//      2. extend RGBToSpectrumTable's boundary-conversion typedef
//      3. flip `RISEPel = Rec709RGBPel → AP1RGBPel`
//      4. coordinated FrameStoreColorSpace + RGBSpectra D60-illuminant
//         update
//    No new conversion-constructor surface needed at that point —
//    this header already declares all of AP1 ↔ XYZ ↔ {Rec.709, ROMM,
//    sRGB} via the standard pattern other RGB headers use.
//
//    XYZ↔AP1 matrices verified by independent recomputation
//    (Bradford-cone-response procedure: M_xyz_to_rgb solving
//    M @ (1,1,1) = ACES_D60_white_XYZ).  See Color.cpp's
//    mxXYZD60toAP1 / mxAP1toXYZD60 definitions.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-05-24 (Stage B of colour-space migration)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef AP1RGBPel_
#define AP1RGBPel_

namespace RISE
{
	struct AP1RGBPel
	{
		Chel	r, g, b;

		// Default constructor
		inline AP1RGBPel( ) :
		r( 0 ),
		g( 0 ),
		b( 0 )
		{}

		inline AP1RGBPel( const Chel val ) :
		r( val ),
		g( val ),
		b( val )
		{}

		inline AP1RGBPel( const Chel val[3] ) :
		r( val[0] ),
		g( val[1] ),
		b( val[2] )
		{}

		// Copy constructor
		inline AP1RGBPel( const AP1RGBPel& k ) :
		r( k.r ),
		g( k.g ),
		b( k.b )
		{}

		// Alternate constructor
		inline AP1RGBPel( const Chel r_, const Chel g_, const Chel b_ ) :
		r( r_ ),
		g( g_ ),
		b( b_ )
		{}

		// Conversions to / from other colour spaces in the codebase.
		inline AP1RGBPel( const XYZPel& xyz )
		{
			*this = ColorUtils::XYZtoAP1RGB( xyz );
		}

		inline AP1RGBPel( const Rec709RGBPel& rgb )
		{
			*this = ColorUtils::Rec709RGBtoAP1RGB( rgb );
		}

		inline AP1RGBPel( const ROMMRGBPel& rgb )
		{
			*this = ColorUtils::ROMMRGBtoAP1RGB( rgb );
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

		inline AP1RGBPel& operator=( const AP1RGBPel& v )
		{
			r = v.r;
			g = v.g;
			b = v.b;

			return *this;  // Assignment operator returns left side.
		}

		inline AP1RGBPel& operator=( const Scalar& d )
		{
			r = g = b = d;
			return *this;  // Assignment operator returns left side.
		}


	#define COLOR_CLASS_TYPE AP1RGBPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE

	};
}

#endif
