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
	struct AP1RGBPel;

	// Define a particular type as the basic color space that all
	// processing is done in.
	//
	// Since 2026-05-24 (Stage B of colour-space migration):
	// RISEPel = Rec709RGBPel — sRGB / BT.709 primaries, D65 whitepoint,
	// linear-light values.  Industry-standard PBR working space.
	//
	// Pre-2026-05: RISEPel = ROMMRGBPel (D50 wide gamut, primaries
	// outside the spectral locus → 22% JH LUT corner failures).
	//
	// To migrate to ACES AP1 (ACEScg) in future: bake the JH LUT with
	// `--target acescg`, extend RGBToSpectrumTable's boundary-
	// conversion typedef to AP1RGBPel, flip this typedef, and audit
	// the Stage B sites listed in docs/COLOR_SPACE_MIGRATION.md.
	typedef Rec709RGBPel			RISEPel;
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
#include "AP1RGB.h"
#include "SpectralPacket.h"
#include "SpectralPacket_Template.h"

#include "ColorDefs.h"
#include "ColorMath.h"

#endif
