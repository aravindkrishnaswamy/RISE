//////////////////////////////////////////////////////////////////////
//
//  Color.h - Includes all aspects of color, for templated stuff or
//            for non-pel stuff include Color_Template
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COLORS_
#define COLORS_

#include "../Math3D/Math3D.h"

namespace RISE
{
	typedef double Chel;	// A Chel is a channel element

	// Color spaces enumerated
	// We enumerate linear and non-linear color spaces seperately
	enum COLOR_SPACE
	{
		eColorSpace_sRGB		= 0,	// sRGB, non-linear Rec709
		eColorSpace_Rec709RGB_Linear,	// Linear Rec709 RGB
		eColorSpace_ROMMRGB_Linear,		// Linear ROMM RGB
		eColorSpace_ProPhotoRGB			// Non-linear ROMM
	};

	// Forward decls
	struct Rec709RGBPel;
	struct sRGBPel;
	struct XYZPel;
	struct xyYPel;
	struct ROMMRGBPel;
	struct ProPhotoRGBPel;

	// Define a particular type as the basic color space that all
	// processing is done in
//	typedef Rec709RGBPel			RISEPel;
	typedef ROMMRGBPel				RISEPel;
}

// Include this first for the conversion functions which are used in the actual classes
#include "ColorConversion.h"

// Include the actual color classes
#include "Rec709RGB.h"
#include "sRGB.h"
#include "CIE_XYZ.h"
#include "CIE_xyY.h"
#include "ROMMRGB.h"
#include "ProPhotoRGB.h"
#include "SpectralPacket.h"
#include "SpectralPacket_Template.h"

#include "ColorDefs.h"
#include "ColorMath.h"

#endif
