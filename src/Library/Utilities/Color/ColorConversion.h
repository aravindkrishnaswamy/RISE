//////////////////////////////////////////////////////////////////////
//
//  ColorConversion.h - Color conversion functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 4, 2006
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COLOR_CONVERSION_
#define COLOR_CONVERSION_

namespace RISE
{
	namespace ColorUtils
	{
		//! Color conversion functions
		XYZPel xyYtoXYZ( const xyYPel& p );
		xyYPel XYZtoxyY( const XYZPel& p );

		XYZPel Rec709RGBtoXYZ( const Rec709RGBPel& p );
		Rec709RGBPel XYZtoRec709RGB( const XYZPel& xyz );

		// sRGB linearization/non-linearization
		Rec709RGBPel Linearize_sRGB( const sRGBPel& srgb );
		sRGBPel sRGBNonLinearization( const Rec709RGBPel& rgb );

		XYZPel ROMMRGBtoXYZ( const ROMMRGBPel& p );
		ROMMRGBPel XYZtoROMMRGB( const XYZPel& xyz );

		ROMMRGBPel Rec709RGBtoROMMRGB( const Rec709RGBPel& rgb );
		Rec709RGBPel ROMMRGBtoRec709RGB( const ROMMRGBPel& rgb );

		// ProPhotoRGB linearization/non-linearization
		ROMMRGBPel ProPhotoRGB_Linearization( const ProPhotoRGBPel& rgb );
		ProPhotoRGBPel ProPhotoRGB_NonLinearization( const ROMMRGBPel& rgb );

		Rec709RGBPel ProPhotoRGBtoRec709RGB( const ProPhotoRGBPel& rgb );
		ROMMRGBPel sRGBtoROMMRGB( const sRGBPel& rgb );

		// ACES AP1 (ACEScg) conversions — pre-staged for a future
		// internal-working-space migration to AP1.  AP1's whitepoint
		// is ACES D60-ish (xy 0.32168, 0.33767) so each conversion
		// here applies the appropriate Bradford chromatic adapt as
		// needed (D65↔D60 for Rec.709, D50↔D60 for ROMM).
		AP1RGBPel    XYZtoAP1RGB( const XYZPel& xyz );
		XYZPel       AP1RGBtoXYZ( const AP1RGBPel& rgb );
		AP1RGBPel    Rec709RGBtoAP1RGB( const Rec709RGBPel& rgb );
		Rec709RGBPel AP1RGBtoRec709RGB( const AP1RGBPel& rgb );
		AP1RGBPel    ROMMRGBtoAP1RGB( const ROMMRGBPel& rgb );
		ROMMRGBPel   AP1RGBtoROMMRGB( const AP1RGBPel& rgb );
	}
}

#endif
