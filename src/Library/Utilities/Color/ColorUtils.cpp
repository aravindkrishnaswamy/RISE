//////////////////////////////////////////////////////////////////////
//
//  ColorUtils.cpp - Implements all the color utility functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 11, 2003
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Color.h"
#include "ColorUtils.h"
#include "CIE_xyY.h"

using namespace RISE;

Scalar ColorUtils::SRGBTransferFunction( const Scalar& x )
{
	if( x == 0 ) {
		return 0;
	}

	if( x <= 0.003040247678 ) {
		return 12.92*x;
	} else {
		return 1.055*pow( x, 1.0/2.4 ) - 0.055;
	}
}

Scalar ColorUtils::SRGBTransferFunctionInverse( const Scalar& x )
{
	if( x == 0 ) {
		return 0;
	}

	if( x <= 0.03928 ) {
		return x/12.92;
	} else {
		return pow((x + 0.055)/1.055,2.4);
	}
}

Scalar ColorUtils::ROMMRGBTransferFunction(
	const Scalar& x							///< [in] Value to transform
	)
{
	if( x == 0 ) {
		return 0;
	}

	// The comparison here is Et which is = 16^(1.8/(1-1.8))
	if( x <= 0.001953 ) {
		return 16*x;
	} else {
		return pow( x, 1.0/1.8 );
	}
}

Scalar ColorUtils::ROMMRGBTransferFunctionInverse( 
	const Scalar& x							///< [in] Value to transform
	)
{
	if( x == 0 ) {
		return 0;
	}

	// The comparison here is 16*Et
	if( x <= 0.031248 ) {
		return x/16;
	} else {
		return pow( x, 1.8 );
	}
}

namespace RISE
{
	namespace CIE_DATA
	{
		static const unsigned int min_wavelength = 380;//nm
		static const unsigned int max_wavelength = 780;//nm
		static const unsigned int wavelength_step = 5;//nm
	
		static const unsigned int n_wavelengths = (max_wavelength - min_wavelength)/wavelength_step + 1;
	
		typedef const Scalar spd_table[n_wavelengths];
	
		spd_table x_2 = 
		{
			0.0014, 0.0022, 0.0042, 0.0077, 0.0143, 0.0232, 0.0435, 0.0776, 0.1344, 0.2148,
			0.2839, 0.3285, 0.3483, 0.3481, 0.3362, 0.3187, 0.2908, 0.2511, 0.1954, 0.1421,
			0.0956, 0.0580, 0.0320, 0.0147, 0.0049, 0.0024, 0.0093, 0.0291, 0.0633, 0.1096,
			0.1655, 0.2257, 0.2904, 0.3597, 0.4334, 0.5121, 0.5945, 0.6784, 0.7621, 0.8425,
			0.9163, 0.9786, 1.0263, 1.0567, 1.0622, 1.0456, 1.0026, 0.9384, 0.8544, 0.7514,
			0.6424, 0.5419, 0.4479, 0.3608, 0.2835, 0.2187, 0.1649, 0.1212, 0.0874, 0.0636, 
			0.0468, 0.0329, 0.0227, 0.0158, 0.0114, 0.0081, 0.0058, 0.0041, 0.0029, 0.0020,
			0.0014, 0.0010, 0.0007, 0.0005, 0.0003, 0.0002, 0.0002, 0.0001, 0.0001, 0.0001,
			0.0000
		};
	
		spd_table y_2 = 
		{
			0, 0.0001, 0.0001, 0.0002, 0.0004, 0.0006, 0.0012, 0.0022, 0.004, 0.0073,
			0.0116, 0.0168, 0.023, 0.0298, 0.038, 0.048, 0.06, 0.0739 ,0.091, 0.1126,
			0.139, 0.1693, 0.208, 0.2586, 0.323, 0.4073, 0.503, 0.6082, 0.71, 0.7932,
			0.862, 0.9149, 0.954, 0.9803, 0.995, 1, 0.995, 0.9786, 0.952, 0.9154,
			0.87, 0.8163, 0.757, 0.6949, 0.631, 0.5668, 0.503, 0.4412, 0.381, 0.321,
			0.265, 0.217, 0.175, 0.1382, 0.107, 0.0816, 0.061, 0.0446, 0.032, 0.0232,
			0.017, 0.0119, 0.0082, 0.0057, 0.0041, 0.0029, 0.0021, 0.0015, 0.001, 0.0007,
			0.0005, 0.0004, 0.0002, 0.0002, 0.0001, 0.0001, 0.0001, 0, 0, 0,
			0
		};
	
		spd_table z_2 =
		{
			0.0065, 0.0105, 0.0201, 0.0362, 0.0679, 0.1102, 0.2074, 0.3713, 0.6456, 1.0391, 
			1.3856, 1.623, 1.7471, 1.7826, 1.7721, 1.7441, 1.6692, 1.5281, 1.2876, 1.0419,
			0.813, 0.6162, 0.4652, 0.3533, 0.272, 0.2123, 0.1582, 0.1117, 0.0782, 0.0573,
			0.0422, 0.0298, 0.0203, 0.0134, 0.0087, 0.0057, 0.0039, 0.0027, 0.0021, 0.0018,
			0.0017, 0.0014, 0.0011, 0.001, 0.0008, 0.0006, 0.0003, 0.0002, 0.0002, 0.0001,
			0
		};
	}
}

// Rec 709 primaries, from Charles Poynton's color FAQ
//	       R           G           B           white       
//	x      0.640       0.300       0.150       0.3127      
//	y      0.330       0.600       0.060       0.3290      
//	z      0.030       0.100       0.790       0.3582     

static const Point2	Rec709RGBRedInXYZGamut			( 0.640, 0.330 );
static const Point2	Rec709RGBGreenInXYZGamut		( 0.300, 0.600 );
static const Point2	Rec709RGBBlueInXYZGamut			( 0.150, 0.060 );
static const Point2	Rec709RGBWhiteInXYZGamut		( 0.3127, 0.3290 );

// ROMM RGB (ProPhotoRGB) primaries, from: http://www.color.org/rommrgb.pdf
//	       R           G           B           white       
//	x      0.7347      0.1596      0.0366      0.3457      
//	y      0.2653      0.8404      0.0001      0.3585      
//	z      0           0           0.9633      0.2958

static const Point2 ROMMRGBRedInXYZGamut			( 0.7347, 0.2653 );
static const Point2 ROMMRGBGreenInXYZGamut			( 0.1596, 0.8404 );
static const Point2 ROMMRGBBlueInXYZGamut			( 0.0366, 0.0001 );
static const Point2 ROMMRGBWhiteInXYZGamut			( 0.3457, 0.3585 );

static inline bool LineIntersection2D( const Point2& Astart, const Point2& Aend, const Point2& Bstart, const Point2& Bend, Point2& intersection )
{
#if 1
	//
	// This version *should* be slightly faster
	//
	const Scalar&	X1 = Astart.x;
	const Scalar&	Y1 = Astart.y;
	const Scalar&	X2 = Aend.x;
	const Scalar&	Y2 = Aend.y;
	const Scalar&	X3 = Bstart.x;
	const Scalar&	Y3 = Bstart.y;
	const Scalar&	X4 = Bend.x;
	const Scalar&	Y4 = Bend.y;

	Scalar		denom = (Y4 - Y3)*(X2 - X1) - (X4 - X3)*(Y2 - Y1);

	if( denom > -NEARZERO && denom < NEARZERO )		// lines are parallel
		return false;

	Scalar		Ua = ( (X4 - X3)*(Y1 - Y3) - (Y4 - Y3)*(X1 - X3) ) / denom;
//	Scalar		Ub = ( (X2 - X1)*(Y1 - Y3) - (Y2 - Y1)*(X1 - X3) ) / denom;

	intersection.x = X1 + Ua*( X2 - X1 );
	intersection.y = Y1 + Ua*( Y2 - Y1 );

	return true;
#else
	//
	// This is based on Math3D usage, it might be slower than the one above
	// not really sure though
	//
	Vector3	v1( Aend.x-Astart.x, Aend.y-Astart.y, 0 );
	Vector3	v2( Bend.x-Bstart.x, Bend.y-Bstart.y, 0 );

	Vector3	pp( Bstart.x - Astart.x, Bstart.y - Astart.y, 0 );

	Vector3	vCross = v1 * v2;
	Scalar		length = !vCross;

	if( length > -NEARZERO && length < NEARZERO )	// lines are parallel
		return false;

	Scalar		dot = Vector3( pp * v2 ) % vCross;
	dot /= length;

	intersection.x = Astart.x + v1.x * dot;
	intersection.y = Astart.y + v1.y * dot;
	
	return true;
#endif
}

static inline bool IsPointOnLineSegment( const Point2& start, const Point2& end, const Point2& point )
{
	Scalar dx = end.x - start.x;
	Scalar pdx = point.x - start.x;
	Scalar v = pdx/dx;

	return( (v >= NEARZERO) && (v <= (1-NEARZERO)) );
}

static inline bool ClipAgainstGamutEdge( const Point2& color, const Point2& ptA, const Point2& ptB, const Point2& ptWhiteInGamut, Point2& intersection )
{
	if( LineIntersection2D( ptWhiteInGamut, color, ptA, ptB, intersection ) )
	{
		if( IsPointOnLineSegment( ptA, ptB, intersection ) )
		{
			if( IsPointOnLineSegment( ptWhiteInGamut, intersection, color ) ) {
				intersection = color;
				return true;
			}
			if( IsPointOnLineSegment( ptWhiteInGamut, color, intersection )  ) {
				return true;
			}
		}
	}
	return false;
}


bool FindPointInGamut( const Point2& color, const Point2& ptRedInGamut, const Point2& ptGreenInGamut, const Point2& ptBlueInGamut, const Point2& ptWhiteInGamut, Point2& intersection )
{
	if( ClipAgainstGamutEdge( color, ptRedInGamut, ptGreenInGamut, ptWhiteInGamut, intersection) ) {
		return true;
	}
	if( ClipAgainstGamutEdge( color, ptRedInGamut, ptBlueInGamut, ptWhiteInGamut, intersection ) ) {
		return true;
	}
	if( ClipAgainstGamutEdge( color, ptBlueInGamut, ptGreenInGamut, ptWhiteInGamut, intersection ) ) {
		return true;
	}

	return false;
}


//
// CIE specific SPD functions for converting spectra to CIE_XYZ
//

inline Scalar ApplyXBar( const unsigned int& min_index, const unsigned int& max_index, const Scalar& weight )
{
	return ColorUtils::ApplySPDFunction( min_index, max_index, weight, CIE_DATA::x_2 );
}

inline Scalar ApplyYBar( const unsigned int& min_index, const unsigned int& max_index, const Scalar& weight )
{
	return ColorUtils::ApplySPDFunction( min_index, max_index, weight, CIE_DATA::y_2 );
}

inline Scalar ApplyZBar( const unsigned int& min_index, const unsigned int& max_index, const Scalar& weight )
{
	return ColorUtils::ApplySPDFunction( min_index, max_index, weight, CIE_DATA::z_2 );
}

//! Given a particular wavelength computes the indices to interpolate and the amount to
//! interpolate for some arbritary spectral sensitivity table
/// \return TRUE if the given wavelength is in the range for conversion, FALSE otherwise
bool InterpSPDIndices( 
	Scalar nm,								///< [in] Wavelength to compute 
	unsigned int& min_index, 				///< [out] Index to interpolate from
	unsigned int& max_index,	 			///< [out] Index to interpolate to
	Scalar& weight,							///< [out] Interpolation amount
	const int min_wavelength,				///< [in] The wavelength that the table starts at			
	const int max_wavelength,				///< [in] The wavelength that the table ends at
	const int wavelength_step				///< [in] Increment interval for the table
	)
{
	if( nm < min_wavelength ) {
		return false;
	}
	if( nm > max_wavelength ) {
		return false;
	}

	nm -= min_wavelength;
	nm /= wavelength_step;

	min_index = (int)floor(nm);
	max_index = min_index + 1;

	weight = (nm - min_index);
	return true;
}

bool ColorUtils::InterpCIE_SPDIndices( 
	Scalar nm,
	unsigned int& min_index,
	unsigned int& max_index,
	Scalar& weight
	)
{
	return InterpSPDIndices( nm, min_index, max_index, weight,
		CIE_DATA::min_wavelength, CIE_DATA::max_wavelength, CIE_DATA::wavelength_step );
}

bool ColorUtils::XYZFromNM( XYZPel& p, const Scalar nm )
{
	unsigned int min_index = 0;
	unsigned int max_index = 0;
	Scalar weight = 0;
	if( InterpCIE_SPDIndices( nm, min_index, max_index, weight ) ) {
		p.X = ApplyXBar( min_index, max_index, weight );
		p.Y = ApplyYBar( min_index, max_index, weight );
		p.Z = ApplyZBar( min_index, max_index, weight );
		return true;
	}
	return false;
}

void ColorUtils::MoveXYZIntoRec709RGBGamut( XYZPel& p )
{
	xyYPel pxy = XYZtoxyY( p );

	Point2	ptColor( pxy.x, pxy.y );
	Point2	ptIntersection;

	if( FindPointInGamut( ptColor, Rec709RGBRedInXYZGamut, Rec709RGBGreenInXYZGamut, Rec709RGBBlueInXYZGamut, Rec709RGBWhiteInXYZGamut, ptIntersection ) ) {
		pxy.x = ptIntersection.x;
		pxy.y = ptIntersection.y;
		p = xyYtoXYZ( pxy );
	}
}

void ColorUtils::MoveXYZIntoROMMRGBGamut( XYZPel& p )
{
	xyYPel pxy = XYZtoxyY( p );

	Point2	ptColor( pxy.x, pxy.y );
	Point2	ptIntersection;

	if( FindPointInGamut( ptColor, ROMMRGBRedInXYZGamut, ROMMRGBGreenInXYZGamut, ROMMRGBBlueInXYZGamut, ROMMRGBWhiteInXYZGamut, ptIntersection ) ) {
		pxy.x = ptIntersection.x;
		pxy.y = ptIntersection.y;
		p = xyYtoXYZ( pxy );
	}
}




//! Saves the given color to the buffer
void ColorUtils::SerializeXYZPel( 
	const XYZPel& xyz,						///< [in] Pel to serialize
	IWriteBuffer& buffer					///< [in] Buffer to serialize into
	)
{
	buffer.ResizeForMore(sizeof(Chel)*3);
	buffer.setDouble( xyz.X );
	buffer.setDouble( xyz.Y );
	buffer.setDouble( xyz.Z );
}

//! Gets a Pel from a buffer
/// \return The deserialized pel
void ColorUtils::DeserializeXYZPel( 
	XYZPel& xyz,							///< [out] The deserialized pel
	IReadBuffer& buffer						///< [in] Buffer to read from
	)
{
	xyz.X = buffer.getDouble();
	xyz.Y = buffer.getDouble();
	xyz.Z = buffer.getDouble();
}
















/************************ DEAD CODE section *********************/
// Though this code is dead, it may still be useful someday

/*
spd_table x_10 = 
{
	0.0002, 0.0007, 0.0024, 0.0072, 0.0191, 0.0434, 0.0847, 0.1406, 0.2045, 0.2647,
	0.3147, 0.3577, 0.3837, 0.3867, 0.3707, 0.3430, 0.3023, 0.2541, 0.1956, 0.1323,
	0.0805, 0.0411, 0.0162, 0.0051, 0.0038, 0.0154, 0.0375, 0.0714, 0.1177, 0.1730,
	0.2365, 0.3042, 0.3768, 0.4516, 0.5298, 0.6161, 0.7052, 0.7938, 0.8787, 0.9512,
	1.0142, 1.0743, 1.1185, 1.1343, 1.1240, 1.0891, 1.0305, 0.9507, 0.8563, 0.7549,
	0.6475, 0.5351, 0.4316, 0.3437, 0.2683, 0.2043, 0.1526, 0.1122, 0.0813, 0.0579,
	0.0409, 0.0286, 0.0199, 0.0138, 0.0096, 0.0066, 0.0046, 0.0031, 0.0022, 0.0015,
	0.0010, 0.0007, 0.0005, 0.0004, 0.0003, 0.0002, 0.0001, 0.0001, 0.0001, 0.0000,
	0.0000
};

spd_table y_10 = 
{
	0.0, 0.0001, 0.0003, 0.0008, 0.002, 0.0045, 0.0088, 0.0145, 0.0214, 0.0295,
	0.0387, 0.0496, 0.0621, 0.0747, 0.0895, 0.1063, 0.1282, 0.1528, 0.1852, 0.2199,
	0.2536, 0.2977, 0.3391, 0.3954, 0.4608, 0.5314, 0.6067, 0.6857, 0.7618, 0.8233,
	0.8752, 0.9238, 0.962, 0.9822, 0.9918, 0.9991, 0.9973, 0.9824, 0.9556, 0.9152,
	0.8689, 0.8256, 0.7774, 0.7204, 0.6583, 0.5939, 0.528, 0.4618, 0.3981, 0.3396,
	0.2835, 0.2283, 0.1798, 0.1402, 0.1076, 0.0812, 0.0603, 0.0441, 0.0318, 0.0226,
	0.0159, 0.0111, 0.0077, 0.0054, 0.0037, 0.0026, 0.0018, 0.0012, 0.0008, 0.0006,
	0.0004, 0.0003, 0.0002, 0.0001, 0.0001, 0.0001, 0.0, 0.0, 0.0, 0.0,
	0.0
};

spd_table z_10 = 
{
	0.0007, 0.0029, 0.0105, 0.0323, 0.0860, 0.1971, 0.3894, 0.6568, 0.9725, 1.2825,
	1.5535, 1.7985, 1.9673, 2.0273, 1.9948, 1.9007, 1.7454, 1.5549, 1.3176, 1.0302,
	0.7721, 0.5701, 0.4153, 0.3024, 0.2185, 0.1592, 0.1120, 0.0822, 0.0607, 0.0431,
	0.0305, 0.0206, 0.0137, 0.0079, 0.0040, 0.0011, 0.0000
};
*/

/*
bool ColorUtils::ArbritaryRGBFromNM(
	ArbriaryRGBPel& p,						///< [out] Resultant RGB value
	const Scalar nm,						///< [in] Wavelength to convert
	const Scalar* spd_r,					///< [in] Spectral power distribution for red
	const Scalar* spd_g,					///< [in] Spectral power distribution for green
	const Scalar* spd_b,					///< [in] Spectral power distribution for blue
	const int min_wavelength,				///< [in] The wavelength that the table starts at			
	const int max_wavelength,				///< [in] The wavelength that the table ends at
	const int wavelength_step				///< [in] Increment interval for the table
	)
{
	unsigned int min_index = 0;
	unsigned int max_index = 0;
	Scalar weight = 0;
	if( InterpSPDIndices( nm, min_index, max_index, weight, min_wavelength, max_wavelength, wavelength_step ) ) {
		p.r = ApplySPDFunction( min_index, max_index, weight, spd_r );
		p.g = ApplySPDFunction( min_index, max_index, weight, spd_g );
		p.b = ApplySPDFunction( min_index, max_index, weight, spd_b );
		return true;
	}
	return false;
}
*/

