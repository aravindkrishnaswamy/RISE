//////////////////////////////////////////////////////////////////////
//
//  RGBToSpectrumTable.cpp - See RGBToSpectrumTable.h.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#define _USE_MATH_DEFINES
#include "RGBToSpectrumTable.h"
#include "ColorMath.h"
#include "../MediaPathLocator.h"
#include "../../Interfaces/ILog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace RISE;

namespace
{
	const char*  kROMMPath    = "extlib/jakob-hanika-luts/romm.coeff";
	const char   kMagic[4]    = { 'R', 'J', 'H', 'L' };
	const uint32_t kVersion   = 0x00010000;
}

const RGBToSpectrumTable& RGBToSpectrumTable::ROMM()
{
	// Lazy singleton.  std::call_once guarantees Load() runs once and
	// completes before any other thread observes `loaded = true` —
	// concurrent rendering threads on first frame can't race the
	// `data.resize()` / `fread()` work inside Load().  After
	// initialisation the read path is lock-free (the const data
	// vector + flag are the only state read by operator()).
	static RGBToSpectrumTable instance;
	static std::once_flag     loadOnce;
	std::call_once( loadOnce, [](){ instance.Load( kROMMPath ); } );
	return instance;
}

RGBToSpectrumTable::RGBToSpectrumTable() :
	loaded( false ),
	resolution( 0 )
{
}

bool RGBToSpectrumTable::Load( const char* relativePath )
{
	const String resolved = GlobalMediaPathLocator().Find( relativePath );
	const std::string path = std::string( resolved.c_str() );
	std::FILE* fp = std::fopen( path.c_str(), "rb" );
	if( !fp ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"RGBToSpectrumTable: cannot open '%s' (resolved from '%s'); "
			"spectral painters will fall back to constant 0.5 spectrum.",
			path.c_str(), relativePath );
		return false;
	}

	char     magic[4];
	uint32_t version, res, nChannels, nCoeffs;
	if( std::fread( magic,      1, 4, fp ) != 4 ||
	    std::fread( &version,   4, 1, fp ) != 1 ||
	    std::fread( &res,       4, 1, fp ) != 1 ||
	    std::fread( &nChannels, 4, 1, fp ) != 1 ||
	    std::fread( &nCoeffs,   4, 1, fp ) != 1 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"RGBToSpectrumTable: '%s' truncated header.", path.c_str() );
		std::fclose( fp );
		return false;
	}

	if( std::memcmp( magic, kMagic, 4 ) != 0 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"RGBToSpectrumTable: '%s' bad magic; expected 'RJHL'.",
			path.c_str() );
		std::fclose( fp );
		return false;
	}
	if( version != kVersion ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"RGBToSpectrumTable: '%s' version 0x%08x, expected 0x%08x.",
			path.c_str(), (unsigned)version, (unsigned)kVersion );
		std::fclose( fp );
		return false;
	}
	if( nChannels != 3 || nCoeffs != 3 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"RGBToSpectrumTable: '%s' nChannels=%u, nCoeffs=%u; "
			"expected 3 each.",
			path.c_str(), (unsigned)nChannels, (unsigned)nCoeffs );
		std::fclose( fp );
		return false;
	}
	if( res < 8 || res > 256 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"RGBToSpectrumTable: '%s' resolution=%u out of range [8, 256].",
			path.c_str(), (unsigned)res );
		std::fclose( fp );
		return false;
	}

	const size_t totalCells = size_t( 3 ) * res * res * res;
	data.resize( totalCells );
	const size_t got = std::fread( data.data(), sizeof(CoeffSet), totalCells, fp );
	std::fclose( fp );
	if( got != totalCells ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"RGBToSpectrumTable: '%s' truncated body (read %zu of %zu cells).",
			path.c_str(), got, totalCells );
		data.clear();
		return false;
	}

	resolution = int( res );
	loaded     = true;
	GlobalLog()->PrintEx( eLog_Info,
		"RGBToSpectrumTable: loaded '%s' (resolution=%d, %zu cells).",
		path.c_str(), resolution, totalCells );
	return true;
}

Scalar RGBToSpectrumTable::GridZToIndex( Scalar z, int res )
{
	// Inverse of GridZ in tools/JakobHanikaLUTGen.cpp:
	//   z(i) = sin²(π/2 · i/(res-1))
	//   ⇒ i/(res-1) = (2/π) · asin( sqrt(z) )
	if( res <= 1 ) return 0;
	const Scalar t = std::sqrt( std::max( Scalar(0), std::min( Scalar(1), z ) ) );
	const Scalar f = std::asin( t ) * (Scalar(2) / Scalar(M_PI));
	return std::max( Scalar(0), std::min( Scalar(res - 1), f * Scalar(res - 1) ) );
}

const RGBToSpectrumTable::CoeffSet&
RGBToSpectrumTable::Cell( int maxC, int iz, int ix, int iy ) const
{
	iz = std::max( 0, std::min( resolution - 1, iz ) );
	ix = std::max( 0, std::min( resolution - 1, ix ) );
	iy = std::max( 0, std::min( resolution - 1, iy ) );
	const size_t idx = size_t(((maxC * resolution + iz) * resolution + ix) * resolution + iy);
	return data[idx];
}

RGBToSpectrumTable::CoeffSet RGBToSpectrumTable::InterpolateTetra(
	const CoeffSet& c000, const CoeffSet& c100,
	const CoeffSet& c010, const CoeffSet& c110,
	const CoeffSet& c001, const CoeffSet& c101,
	const CoeffSet& c011, const CoeffSet& c111,
	Scalar fx, Scalar fy, Scalar fz )
{
	// Standard trilinear; tetrahedral subdivision is unnecessary for
	// the 3-coefficient case — the smoothness of the surrounding
	// sigmoid coefficients is high enough that trilinear is
	// indistinguishable in practice.  This also matches PBRT-v4's
	// final shipping form (their early prototypes used tetrahedral).
	const Scalar wx0 = Scalar(1) - fx, wx1 = fx;
	const Scalar wy0 = Scalar(1) - fy, wy1 = fy;
	const Scalar wz0 = Scalar(1) - fz, wz1 = fz;

	auto lerp1 = [](float a, float b, Scalar t) -> float {
		return float( double(a) * (1.0 - double(t)) + double(b) * double(t) );
	};

	const float c00_0 = lerp1( c000.c0, c100.c0, fx );
	const float c00_1 = lerp1( c001.c0, c101.c0, fx );
	const float c10_0 = lerp1( c010.c0, c110.c0, fx );
	const float c10_1 = lerp1( c011.c0, c111.c0, fx );
	const float c0_0  = lerp1( c00_0, c10_0, fy );
	const float c0_1  = lerp1( c00_1, c10_1, fy );
	const float r_c0  = lerp1( c0_0, c0_1, fz );

	const float d00_0 = lerp1( c000.c1, c100.c1, fx );
	const float d00_1 = lerp1( c001.c1, c101.c1, fx );
	const float d10_0 = lerp1( c010.c1, c110.c1, fx );
	const float d10_1 = lerp1( c011.c1, c111.c1, fx );
	const float d0_0  = lerp1( d00_0, d10_0, fy );
	const float d0_1  = lerp1( d00_1, d10_1, fy );
	const float r_c1  = lerp1( d0_0, d0_1, fz );

	const float e00_0 = lerp1( c000.c2, c100.c2, fx );
	const float e00_1 = lerp1( c001.c2, c101.c2, fx );
	const float e10_0 = lerp1( c010.c2, c110.c2, fx );
	const float e10_1 = lerp1( c011.c2, c111.c2, fx );
	const float e0_0  = lerp1( e00_0, e10_0, fy );
	const float e0_1  = lerp1( e00_1, e10_1, fy );
	const float r_c2  = lerp1( e0_0, e0_1, fz );

	(void)wx0; (void)wx1; (void)wy0; (void)wy1; (void)wz0; (void)wz1;	// unused after refactor
	CoeffSet out;
	out.c0 = r_c0;
	out.c1 = r_c1;
	out.c2 = r_c2;
	return out;
}

RGBSigmoidPolynomial RGBToSpectrumTable::operator()( const RISEPel& rgb_in ) const
{
	if( !loaded ) {
		// LUT failed to load — return a constant-0.5 spectrum so the
		// caller sees a valid (though uninformative) result.  See
		// PrintEx warning on first load attempt.
		return RGBSigmoidPolynomial( Scalar(0), Scalar(0), Scalar(0) );
	}

	// Clamp inputs to [0, 1].  Out-of-range values reach this code
	// only when a caller forgot to wrap an HDR / unbounded source in
	// RGBUnboundedSpectrum (which scales by max(R,G,B) before lookup).
	// Clamp keeps the table valid; the caller's bug is its own.
	Scalar r = std::max( Scalar(0), std::min( Scalar(1), rgb_in.r ) );
	Scalar g = std::max( Scalar(0), std::min( Scalar(1), rgb_in.g ) );
	Scalar b = std::max( Scalar(0), std::min( Scalar(1), rgb_in.b ) );

	// Identify max channel and the canonical (max, mid, min) ordering
	// matching tools/JakobHanikaLUTGen.cpp's CellToRGB.  The max gives
	// the table index; the other two channels normalize against it.
	int    maxC;
	Scalar z, x, y;
	if( r >= g && r >= b ) {
		maxC = 0;
		z = r;
		x = (z > Scalar(1e-9)) ? g / z : Scalar(0);
		y = (z > Scalar(1e-9)) ? b / z : Scalar(0);
	} else if( g >= r && g >= b ) {
		maxC = 1;
		z = g;
		x = (z > Scalar(1e-9)) ? b / z : Scalar(0);
		y = (z > Scalar(1e-9)) ? r / z : Scalar(0);
	} else {
		maxC = 2;
		z = b;
		x = (z > Scalar(1e-9)) ? r / z : Scalar(0);
		y = (z > Scalar(1e-9)) ? g / z : Scalar(0);
	}

	// Map to fractional grid indices.
	const Scalar fIz = GridZToIndex( z, resolution );
	const Scalar fIx = std::max( Scalar(0), std::min( Scalar(resolution - 1), x * Scalar(resolution - 1) ) );
	const Scalar fIy = std::max( Scalar(0), std::min( Scalar(resolution - 1), y * Scalar(resolution - 1) ) );

	const int izLo = int( std::floor( fIz ) );
	const int ixLo = int( std::floor( fIx ) );
	const int iyLo = int( std::floor( fIy ) );
	const Scalar fz = fIz - Scalar( izLo );
	const Scalar fx = fIx - Scalar( ixLo );
	const Scalar fy = fIy - Scalar( iyLo );

	const CoeffSet& c000 = Cell( maxC, izLo,     ixLo,     iyLo     );
	const CoeffSet& c100 = Cell( maxC, izLo,     ixLo + 1, iyLo     );
	const CoeffSet& c010 = Cell( maxC, izLo,     ixLo,     iyLo + 1 );
	const CoeffSet& c110 = Cell( maxC, izLo,     ixLo + 1, iyLo + 1 );
	const CoeffSet& c001 = Cell( maxC, izLo + 1, ixLo,     iyLo     );
	const CoeffSet& c101 = Cell( maxC, izLo + 1, ixLo + 1, iyLo     );
	const CoeffSet& c011 = Cell( maxC, izLo + 1, ixLo,     iyLo + 1 );
	const CoeffSet& c111 = Cell( maxC, izLo + 1, ixLo + 1, iyLo + 1 );

	const CoeffSet out = InterpolateTetra(
		c000, c100, c010, c110, c001, c101, c011, c111, fx, fy, fz );

	return RGBSigmoidPolynomial( Scalar(out.c0), Scalar(out.c1), Scalar(out.c2) );
}
