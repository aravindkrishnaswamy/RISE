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
	}
}

#endif
