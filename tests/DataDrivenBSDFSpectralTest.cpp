//////////////////////////////////////////////////////////////////////
//
//  DataDrivenBSDFSpectralTest.cpp - Regression for two DataDrivenBSDF bugs:
//    (1) the spectral-black valueNM stub, and (2) negative BSDF values from
//    Catmull-Rom interpolation overshoot.
//
//    BUG 1 (spectral black): valueNM() was a hard `return 0`, ignoring
//    vLightIn/ri/nm.  Every spectral integrator multiplies the emitter's
//    spectral radiance by the BSDF's valueNM (AreaLightShaderOp,
//    AmbientOcclusionShaderOp, ...) with NO RGB fallback, so any
//    datadriven_material rendered through a *_spectral_* rasterizer
//    reflected nothing and came out FULLY BLACK -- even though value() works.
//    FIX: valueNM uplifts the RGB value() to a spectral value via
//    RGBUnboundedSpectrum::FromRGB(value()).Eval(nm) -- the chroma-preserving
//    JH uplift, keeping the BRDF's HUE (a red BRDF stays red across lambda),
//    which a luminance/flat-spectrum fallback would have flattened to gray.
//
//    BUG 2 (negative BSDF): value() returns a Catmull-Rom interpolation of
//    the tabulated patches.  Catmull-Rom is an INTERPOLATING spline and
//    overshoots BELOW zero between a bright patch and an adjacent dark one.
//    A BRDF must never be negative -- it would SUBTRACT energy; and in the
//    spectral path RGBUnboundedSpectrum's scale = max-channel goes negative
//    and contaminates EVERY wavelength.  FIX: value() clamps its result to
//    the non-negative octant (which also feeds valueNM).
//
//    Assertions:
//      A. value() (RGB path) is non-zero (red bdf) -- test premise / sanity.
//      B. valueNM(nm) > 0 for every probed wavelength -- BUG 1 fix (was 0).
//      C. CHROMA preserved: for a RED reflectance valueNM peaks at LONG
//         wavelengths (620 > 530 and > 470).  A luminance fallback would be
//         flat and FAIL C -- so C pins the chroma-preserving choice.
//      D. NON-NEGATIVE: across a fuzz of view angles on a bright/dark patch
//         set (which drives Catmull-Rom overshoot), value() and valueNM are
//         never < 0 -- BUG 2 fix.  (Pre-fix this FAILS: the overshoot regime
//         is genuinely hit, so the test has teeth.)
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/DataDrivenBSDF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	// Write a minimal .bdf (format per DataDrivenBSDF.cpp / gen_chunkcoverage_
	// assets.py): int sig(0xBDF), uint ver(1), int numEmitter, int numPatches,
	// then per emitter a double theta and, per patch, 5 doubles brdf
	// (thetaLo, thetaHi, R, G, B) + 5 doubles btdf.  One emitter with a WIDE
	// theta (> pi/2) so any front-facing query matches it.  `patches` is a
	// flat array of {thetaLo, thetaHi, R, G, B} rows (the brdf lobe); the btdf
	// lobe is written as zeros (unused by value()/valueNM()).
	bool WriteBDF( const char* path, const double* patches, int numPatches )
	{
		FILE* f = fopen( path, "wb" );
		if( !f ) return false;
		const int32_t  sig = 0x0BDF;       fwrite( &sig, sizeof(int32_t), 1, f );
		const uint32_t ver = 1;            fwrite( &ver, sizeof(uint32_t), 1, f );
		const int32_t  numEmitter = 1;     fwrite( &numEmitter, sizeof(int32_t), 1, f );
		const int32_t  np = numPatches;    fwrite( &np, sizeof(int32_t), 1, f );
		const double   emitterTheta = 1.6; fwrite( &emitterTheta, sizeof(double), 1, f );  // wide (> pi/2)
		for( int p = 0; p < numPatches; ++p ) {
			const double* row = patches + p * 5;
			const double brdf[5] = { row[0], row[1], row[2], row[3], row[4] };
			const double btdf[5] = { row[0], row[1], 0.0, 0.0, 0.0 };
			fwrite( brdf, sizeof(double), 5, f );
			fwrite( btdf, sizeof(double), 5, f );
		}
		fclose( f );
		return true;
	}

	// Build an RI whose incoming ray travels along rayDir with geometric
	// normal +Z (mirrors ThinFilmBRDFTest::MakeRI).  value() reads only
	// ri.ray.Dir() and ri.vNormal.
	RayIntersectionGeometric MakeRI( const Vector3& rayDir )
	{
		Ray inRay( Point3( 0, 0, 1 ), rayDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		return ri;
	}

	// ---- Test 1: spectral uplift (non-zero, chroma) on a RED bdf ----
	void TestRedUplift()
	{
		std::cout << "--- Test 1: spectral valueNM uplift (non-zero + chroma) ---\n";
		const char* kPath = "datadriven_nm_red_tmp.bdf";
		// Two patches, both pure RED.
		const double patches[] = {
			0.00, 0.05, 0.8, 0.1, 0.1,
			0.10, 0.15, 0.8, 0.1, 0.1,
		};
		if( !WriteBDF( kPath, patches, 2 ) ) { Check( false, "write red .bdf" ); return; }

		DataDrivenBSDF* bsdf = new DataDrivenBSDF( kPath );
		bsdf->addref();

		// Normal incidence: v (light) and r (view) both along +Z.
		const Vector3 v( 0, 0, 1 );
		RayIntersectionGeometric ri = MakeRI( Vector3( 0, 0, -1 ) );  // r = -dir = +Z

		// A. RGB path sanity: value() non-zero + red.
		const RISEPel rgb = bsdf->value( v, ri );
		std::cout << std::fixed << std::setprecision( 4 )
				  << "  value() = (" << rgb.r << ", " << rgb.g << ", " << rgb.b << ")\n";
		Check( ColorMath::MaxValue( rgb ) > 1e-6, "value() (RGB path) is non-zero at normal incidence" );
		Check( rgb.r > rgb.g && rgb.r > rgb.b, "value() is RED (r dominates g,b) as authored" );

		// B. THE FIX: valueNM > 0 for every wavelength (was a hard 0 -> black).
		const Scalar nms[] = { 420.0, 470.0, 530.0, 580.0, 620.0, 680.0 };
		bool allPositive = true;
		std::cout << "  valueNM:";
		for( int i = 0; i < 6; ++i ) {
			const Scalar x = bsdf->valueNM( v, ri, nms[i] );
			std::cout << " " << (int)nms[i] << "nm=" << std::setprecision( 4 ) << x;
			if( !( x > 0.0 ) || !std::isfinite( x ) ) allPositive = false;
		}
		std::cout << "\n";
		Check( allPositive, "valueNM(nm) > 0 and finite for every wavelength (spectral-black fix)" );

		// C. CHROMA preserved: a RED reflectance peaks at LONG wavelengths.
		const Scalar red = bsdf->valueNM( v, ri, 620.0 );
		const Scalar grn = bsdf->valueNM( v, ri, 530.0 );
		const Scalar blu = bsdf->valueNM( v, ri, 470.0 );
		std::cout << "  chroma: 620nm=" << red << "  530nm=" << grn << "  470nm=" << blu << "\n";
		Check( red > grn && red > blu,
			"valueNM preserves CHROMA: red BRDF peaks at long lambda (not a flat luminance fallback)" );

		bsdf->release();
		std::remove( kPath );
	}

	// ---- Test 2: non-negativity under Catmull-Rom overshoot ----
	void TestNonNegativeOvershoot()
	{
		std::cout << "\n--- Test 2: value()/valueNM non-negative under Catmull-Rom overshoot ---\n";
		const char* kPath = "datadriven_nm_overshoot_tmp.bdf";
		// FOUR patches BRIGHT,DARK,DARK,BRIGHT (0.9,0,0,0.9) at spread angles.
		// Catmull-Rom UNDERSHOOTS below zero across the middle DARK,DARK pair:
		// the bright outer neighbors give the two zero control points tangents
		// (-0.45 / +0.45) that dip the interpolated curve negative between them.
		// thetaLo/Hi spread out so the dTheta = pi/2 - theta lookup keys span a
		// wide nr_theta range the view-angle fuzz below lands inside.
		const double patches[] = {
			0.00, 0.10, 0.9, 0.9, 0.9,
			0.40, 0.50, 0.0, 0.0, 0.0,
			0.80, 0.90, 0.0, 0.0, 0.0,
			1.20, 1.30, 0.9, 0.9, 0.9,
		};
		if( !WriteBDF( kPath, patches, 4 ) ) { Check( false, "write overshoot .bdf" ); return; }

		DataDrivenBSDF* bsdf = new DataDrivenBSDF( kPath );
		bsdf->addref();

		const Vector3 v( 0, 0, 1 );          // light along normal (nv_theta = 0, matches wide emitter)
		const Scalar nms[] = { 450.0, 550.0, 650.0 };
		int neg = 0, samples = 0;
		Scalar worst = 0.0;                  // most-negative value seen (0 if all non-negative)
		for( int i = 0; i <= 70; ++i ) {
			const Scalar to = 0.02 + i * ( 1.52 / 70.0 );  // view polar angle in (0, ~pi/2)
			const Vector3 r( sin( to ), 0, cos( to ) );
			RayIntersectionGeometric ri = MakeRI( -r );    // value() uses r = -ray.Dir()
			const RISEPel rgb = bsdf->value( v, ri );
			++samples;
			if( rgb.r < worst ) worst = rgb.r;
			if( rgb.g < worst ) worst = rgb.g;
			if( rgb.b < worst ) worst = rgb.b;
			if( rgb.r < 0.0 || rgb.g < 0.0 || rgb.b < 0.0 ) ++neg;
			for( int k = 0; k < 3; ++k ) {
				const Scalar x = bsdf->valueNM( v, ri, nms[k] );
				if( x < worst ) worst = x;
				if( x < 0.0 ) ++neg;
				if( !std::isfinite( x ) ) ++neg;
			}
		}
		std::cout << std::scientific << std::setprecision( 3 )
				  << "  fuzzed " << samples << " view angles; negatives=" << neg
				  << "; most-negative=" << worst << "\n";
		Check( neg == 0, "value()/valueNM are NEVER negative across the overshoot fuzz (BUG 2 clamp)" );

		bsdf->release();
		std::remove( kPath );
	}
}

int main()
{
	std::cout << "=== DataDrivenBSDF spectral valueNM Test ===\n";
	GlobalLog();	// initialize the global log

	TestRedUplift();
	TestNonNegativeOvershoot();

	std::cout << "\n=== DataDrivenBSDFSpectralTest: " << s_pass << " passed, " << s_fail << " failed ===\n";
	return s_fail == 0 ? 0 : 1;
}
