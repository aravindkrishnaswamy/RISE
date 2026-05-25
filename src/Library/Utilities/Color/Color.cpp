//////////////////////////////////////////////////////////////////////
//
//  Color.cpp - Implements all the conversion constructors in the
//    various color classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Color.h"
#include "ColorUtils.h"

using namespace RISE;

// Conversion matrix for CIE_XYZ (D65) to Rec 709 RGB with its D65 whitepoint
static const Matrix3 mxXYZtoRec709 = Matrix3(
	 3.240479, -1.537150, -0.498535,
	-0.969256,  1.875992,  0.041556,
	 0.055648, -0.204043,  1.057311 );

// Conversion matrix for Rec 709 RGB with its D65 whitepoint to CIE_XYZ (D65)
static const Matrix3 mxRec709toXYZ = Matrix3(
	0.412453, 0.357580, 0.180423,
	0.212671, 0.715160, 0.072169,
	0.019334, 0.119193, 0.950227 );

// Conversion matrix for CIE_XYZ (D50) to ROMM RGB with its D50 whitepoint
static const Matrix3 mxXYZD50toROMM = Matrix3(
	 1.3460, -0.2556, -0.0511,
	-0.5446,  1.5082,  0.0205,
	 0.0,     0.0,     1.2123 );

// Conversion matrix for ROMM RGB with its D50 whitepoint to CIE_XYZ (D50)
static const Matrix3 mxROMMtoXYZD50 = Matrix3Ops::Inverse( mxXYZD50toROMM );

// Conversion matrix for XYZ(D65) to XYZ(D50) using the Bradford Transform
// NOTE: We can also use the von Kries transform to do this, however according to 
// Hunt in "The Reproduction of Color", the Bradford Transform does a better job 
static const Matrix3 mxXYZD65toXYZD50 = Matrix3(
 	 1.0479, 0.0229, -0.0502,
	 0.0296, 0.9904, -0.0171,
	-0.0092, 0.0151,  0.7519 );

// Conversion matrix for XYZ(D50) to XYZ(D65) using the Bradford Transform
static const Matrix3 mxXYZD50toXYZD65 = Matrix3Ops::Inverse( mxXYZD65toXYZD50 );

// Conversion matrix for Rec709RGB with its D65 whitepoint to ROMMRGB with its D50 whitepoint.
//
// MULTIPLICATION ORDER: the RGB-to-RGB matrix multiplier in this
// file (`RGBtoRGBMatrixMultiply`) applies `out = M · in` with `in` as
// a COLUMN vector — so the matrix that is applied FIRST must appear
// RIGHTMOST in the composite product.  Order here is:
//   1. mxRec709toXYZ          (Rec.709 → XYZ D65)
//   2. mxXYZD65toXYZD50       (XYZ D65 → XYZ D50 Bradford)
//   3. mxXYZD50toROMM         (XYZ D50 → ROMM D50)
// So the composite is `M3 · M2 · M1`.  Pre-Stage-B (RISEPel = ROMM)
// these matrices were assembled in the wrong order — the bug was
// latent because no production code path was actually exercising
// Rec709↔ROMM (every render emitted RISEPel = ROMM directly).
// Stage B activated the path (FrameStoreColorSpace::ROMM_Linear now
// goes through `ColorUtils::Rec709RGBtoROMMRGB`) and the bug surfaced
// as ROMM-tagged archival output being chromatically wrong (white
// not preserved: Rec.709 (1,1,1) was landing at ~(0.951, 0.994, 1.009)
// instead of ROMM (1,1,1)).
static const Matrix3 mxRec709toROMM = mxXYZD50toROMM * mxXYZD65toXYZD50 * mxRec709toXYZ;

// Conversion matrix for ROMMRGB with its D50 whitepoint to Rec709RGB with its D65 whitepoint.
// Inverse direction; same M3 · M2 · M1 convention.
static const Matrix3 mxROMMtoRec709 = mxXYZtoRec709 * mxXYZD50toXYZD65 * mxROMMtoXYZD50;

// ACES AP1 / ACEScg matrices.  AP1 whitepoint is the ACES D60-ish
// chromaticity (0.32168, 0.33767).  XYZ(D60)↔AP1(D60) derived from
// the published AP1 primaries; round-trip M_AP1toXYZ · (1,1,1) =
// ACES_D60_white_XYZ verified to numerical precision.
// (Same matrices the Stage A LUT generator pre-staged in
// tools/JakobHanikaLUTGen.cpp:kTarget_ACEScg.)
static const Matrix3 mxXYZD60toAP1 = Matrix3(
	 1.6410233797, -0.3248032942, -0.2364246952,
	-0.6636628587,  1.6153315917,  0.0167563477,
	 0.0117218943, -0.0082844420,  0.9883948585 );
static const Matrix3 mxAP1toXYZD60 = Matrix3Ops::Inverse( mxXYZD60toAP1 );

// Bradford D65 → D60 (ACES whitepoint).  Independently recomputed
// from the canonical Bradford CRF + D65/ACES-D60 whitepoint XYZ via
// M_adapt = M_Bradford^-1 · diag(rho_dst/rho_src) · M_Bradford.
// Round-trip M_adapt · D65_XYZ ≈ ACES_D60_XYZ verified.
static const Matrix3 mxXYZD65toXYZD60 = Matrix3(
	 1.0129910965,  0.0060845191, -0.0149298715,
	 0.0076709636,  0.9981726261, -0.0050179063,
	-0.0028339778,  0.0046733535,  0.9247039866 );
static const Matrix3 mxXYZD60toXYZD65 = Matrix3Ops::Inverse( mxXYZD65toXYZD60 );

// Composite matrices Rec.709(D65) ↔ AP1(D60-ish) and ROMM(D50) ↔
// AP1(D60-ish) via Bradford adapt at the XYZ stage.  Same M_n · ... · M_1
// column-vector composition convention as mxRec709toROMM above (apply
// rightmost first).
static const Matrix3 mxRec709toAP1  = mxXYZD60toAP1 * mxXYZD65toXYZD60 * mxRec709toXYZ;
static const Matrix3 mxAP1toRec709  = mxXYZtoRec709 * mxXYZD60toXYZD65 * mxAP1toXYZD60;
static const Matrix3 mxROMMtoAP1    = mxXYZD60toAP1 * mxXYZD65toXYZD60 * mxXYZD50toXYZD65 * mxROMMtoXYZD50;
static const Matrix3 mxAP1toROMM    = mxXYZD50toROMM * mxXYZD65toXYZD50 * mxXYZD60toXYZD65 * mxAP1toXYZD60;

//
// Helper stuff
//

template< class RGB_T >
XYZPel RGBtoXYZMatrixMultiply( const RGB_T& rgb, const Matrix3& mx )
{
	XYZPel ret;
	ret.X = mx._00*rgb.r + mx._01*rgb.g + mx._02*rgb.b;
	ret.Y = mx._10*rgb.r + mx._11*rgb.g + mx._12*rgb.b;
	ret.Z = mx._20*rgb.r + mx._21*rgb.g + mx._22*rgb.b;
	return ret;
}

template< class RGB_T >
RGB_T XYZtoRGBMatrixMultiply( const XYZPel& xyz, const Matrix3& mx )
{
	RGB_T ret;
	ret.r = mx._00*xyz.X + mx._01*xyz.Y + mx._02*xyz.Z;
	ret.g = mx._10*xyz.X + mx._11*xyz.Y + mx._12*xyz.Z;
	ret.b = mx._20*xyz.X + mx._21*xyz.Y + mx._22*xyz.Z;
	return ret;
}

XYZPel XYZMatrixMultiply( const XYZPel& xyz, const Matrix3& mx )
{
	XYZPel ret;
	ret.X = mx._00*xyz.X + mx._01*xyz.Y + mx._02*xyz.Z;
	ret.Y = mx._10*xyz.X + mx._11*xyz.Y + mx._12*xyz.Z;
	ret.Z = mx._20*xyz.X + mx._21*xyz.Y + mx._22*xyz.Z;
	return ret;
}

template< class Src_T, class Dst_T >
Dst_T RGBtoRGBMatrixMultiply( const Src_T& rgb, const Matrix3& mx )
{
	Dst_T ret;
	ret.r = mx._00*rgb.r + mx._01*rgb.g + mx._02*rgb.b;
	ret.g = mx._10*rgb.r + mx._11*rgb.g + mx._12*rgb.b;
	ret.b = mx._20*rgb.r + mx._21*rgb.g + mx._22*rgb.b;
	return ret;
}


//
// XYZPel implementations
//


XYZPel ColorUtils::Rec709RGBtoXYZ( const Rec709RGBPel& p )
{
	return RGBtoXYZMatrixMultiply<Rec709RGBPel>( p, mxRec709toXYZ );
	/*
	XYZPel ret;
	ret.X = 0.412453 * p.r + 0.357580 * p.g + 0.180423 * p.b;
	ret.Y = 0.212671 * p.r + 0.715160 * p.g + 0.072169 * p.b;
	ret.Z = 0.019334 * p.r + 0.119193 * p.g + 0.950227 * p.b;
	return ret;
	*/
}

XYZPel ColorUtils::xyYtoXYZ( const xyYPel& p )
{
	XYZPel ret;

	ret.Y = p.Y;

	const Chel OVy = 1.0/p.y;
	const Chel prod = OVy * p.Y;

	ret.X = prod * p.x;
	ret.Z = prod * (1 - p.x - p.y);
	return ret;
}

xyYPel ColorUtils::XYZtoxyY( const XYZPel& p )
{
	xyYPel ret;

	ret.Y = p.Y;
	const Chel	OVsum = 1.0 / (p.X + p.Y + p.Z);
	ret.x = p.X * OVsum;
	ret.y = p.Y * OVsum;

	return ret;
}

Rec709RGBPel ColorUtils::Linearize_sRGB( const sRGBPel& srgb )
{
	Rec709RGBPel ret;
	ret.r = ColorUtils::SRGBTransferFunctionInverse( srgb.r );
	ret.g = ColorUtils::SRGBTransferFunctionInverse( srgb.g );
	ret.b = ColorUtils::SRGBTransferFunctionInverse( srgb.b );
	return ret;
}

sRGBPel ColorUtils::sRGBNonLinearization( const Rec709RGBPel& rgb )
{
	sRGBPel ret;
	ret.r = ColorUtils::SRGBTransferFunction( rgb.r );
	ret.g = ColorUtils::SRGBTransferFunction( rgb.g );
	ret.b = ColorUtils::SRGBTransferFunction( rgb.b );

	return ret;
}

Rec709RGBPel ColorUtils::XYZtoRec709RGB( const XYZPel& xyz_ )
{
	Rec709RGBPel ret;
	XYZPel	xyz( xyz_ );
	ColorUtils::MoveXYZIntoRec709RGBGamut( xyz );

	return XYZtoRGBMatrixMultiply<Rec709RGBPel>( xyz, mxXYZtoRec709 );
	/*
	ret.r =  3.240479 * xyz.X - 1.537150 * xyz.Y - 0.498535 * xyz.Z;
	ret.g = -0.969256 * xyz.X + 1.875992 * xyz.Y + 0.041556 * xyz.Z;
	ret.b =  0.055648 * xyz.X - 0.204043 * xyz.Y + 1.057311 * xyz.Z;
	*/

//	return ret;
}

XYZPel ColorUtils::ROMMRGBtoXYZ( const ROMMRGBPel& p )
{
	XYZPel xyzD50 = RGBtoXYZMatrixMultiply<ROMMRGBPel>( p, mxROMMtoXYZD50 );
	return XYZMatrixMultiply( xyzD50, mxXYZD50toXYZD65 );
}

ROMMRGBPel ColorUtils::XYZtoROMMRGB( const XYZPel& xyz )
{
	XYZPel	xyzD50 = XYZMatrixMultiply( xyz, mxXYZD65toXYZD50 );
	ColorUtils::MoveXYZIntoROMMRGBGamut( xyzD50 );

	return XYZtoRGBMatrixMultiply<ROMMRGBPel>( xyzD50, mxXYZD50toROMM );
}

ROMMRGBPel ColorUtils::ProPhotoRGB_Linearization( const ProPhotoRGBPel& rgb )
{
	ROMMRGBPel ret;
	ret.r = ColorUtils::ROMMRGBTransferFunctionInverse( rgb.r );
	ret.g = ColorUtils::ROMMRGBTransferFunctionInverse( rgb.g );
	ret.b = ColorUtils::ROMMRGBTransferFunctionInverse( rgb.b );
	return ret;
}

ProPhotoRGBPel ColorUtils::ProPhotoRGB_NonLinearization( const ROMMRGBPel& rgb )
{
	ProPhotoRGBPel ret;
	ret.r = ColorUtils::ROMMRGBTransferFunction( rgb.r );
	ret.g = ColorUtils::ROMMRGBTransferFunction( rgb.g );
	ret.b = ColorUtils::ROMMRGBTransferFunction( rgb.b );
	return ret;
}

ROMMRGBPel ColorUtils::Rec709RGBtoROMMRGB( const Rec709RGBPel& rgb )
{
	return RGBtoRGBMatrixMultiply<Rec709RGBPel,ROMMRGBPel>( rgb, mxRec709toROMM );
}

Rec709RGBPel ColorUtils::ROMMRGBtoRec709RGB( const ROMMRGBPel& rgb )
{
	Rec709RGBPel ret = RGBtoRGBMatrixMultiply<ROMMRGBPel,Rec709RGBPel>( rgb, mxROMMtoRec709 );
	
	// We should check for out of gamut values, and do some gamut mapping!

	return ret;
}

Rec709RGBPel ColorUtils::ProPhotoRGBtoRec709RGB( const ProPhotoRGBPel& rgb )
{
	return ROMMRGBtoRec709RGB( ProPhotoRGB_Linearization(rgb) );
}

ROMMRGBPel ColorUtils::sRGBtoROMMRGB( const sRGBPel& rgb )
{
	return Rec709RGBtoROMMRGB( Linearize_sRGB(rgb) );
}

//
// ACES AP1 (ACEScg) conversions
//

XYZPel ColorUtils::AP1RGBtoXYZ( const AP1RGBPel& p )
{
	// AP1 → XYZ(D60) → XYZ(D65), so XYZPel callers see a D65-
	// referred XYZ consistent with every other RGBtoXYZ in this file.
	XYZPel xyzD60 = RGBtoXYZMatrixMultiply<AP1RGBPel>( p, mxAP1toXYZD60 );
	return XYZMatrixMultiply( xyzD60, mxXYZD60toXYZD65 );
}

AP1RGBPel ColorUtils::XYZtoAP1RGB( const XYZPel& xyz )
{
	// XYZ (D65) → XYZ (D60) → AP1.
	XYZPel xyzD60 = XYZMatrixMultiply( xyz, mxXYZD65toXYZD60 );
	return XYZtoRGBMatrixMultiply<AP1RGBPel>( xyzD60, mxXYZD60toAP1 );
}

AP1RGBPel ColorUtils::Rec709RGBtoAP1RGB( const Rec709RGBPel& rgb )
{
	return RGBtoRGBMatrixMultiply<Rec709RGBPel,AP1RGBPel>( rgb, mxRec709toAP1 );
}

Rec709RGBPel ColorUtils::AP1RGBtoRec709RGB( const AP1RGBPel& rgb )
{
	return RGBtoRGBMatrixMultiply<AP1RGBPel,Rec709RGBPel>( rgb, mxAP1toRec709 );
}

AP1RGBPel ColorUtils::ROMMRGBtoAP1RGB( const ROMMRGBPel& rgb )
{
	return RGBtoRGBMatrixMultiply<ROMMRGBPel,AP1RGBPel>( rgb, mxROMMtoAP1 );
}

ROMMRGBPel ColorUtils::AP1RGBtoROMMRGB( const AP1RGBPel& rgb )
{
	return RGBtoRGBMatrixMultiply<AP1RGBPel,ROMMRGBPel>( rgb, mxAP1toROMM );
}
