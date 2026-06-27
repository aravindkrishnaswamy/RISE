//////////////////////////////////////////////////////////////////////
//
//  DataDrivenBSDFSpectralTest.cpp - Regression for the spectral-black bug
//    in DataDrivenBSDF::valueNM.
//
//    THE BUG (pre-fix): valueNM() was a hard `return 0`, ignoring
//    vLightIn/ri/nm.  Every spectral integrator multiplies the emitter's
//    spectral radiance by the BSDF's valueNM (AreaLightShaderOp,
//    AmbientOcclusionShaderOp, ...) with NO RGB fallback, so any
//    datadriven_material rendered through a *_spectral_* rasterizer
//    reflected nothing and came out FULLY BLACK -- even though its RGB
//    path value() works.
//
//    THE FIX: valueNM uplifts the RGB value() to a spectral value via
//    RGBUnboundedSpectrum::FromRGB(value()).Eval(nm) -- the SAME
//    chroma-preserving JH uplift UniformColorPainter::GetColorNM uses.
//    This keeps the BRDF's HUE (a red BRDF stays red across wavelengths),
//    which a luminance/flat-spectrum fallback would have flattened to gray.
//
//    This test hand-writes a tiny RED .bdf (all patches reflect
//    RGB=(0.8,0.1,0.1)), constructs a DataDrivenBSDF from it, evaluates at
//    normal incidence, and asserts:
//      A. value() (RGB path) is non-zero  -- the test's premise / sanity.
//      B. valueNM(nm) > 0 for every probed wavelength  -- THE FIX (was 0,
//         which is exactly the black-render bug).
//      C. CHROMA preserved: for a RED reflectance valueNM peaks at LONG
//         wavelengths (valueNM(620) > valueNM(530) and > valueNM(470)).  A
//         luminance fallback would give a FLAT spectrum and FAIL C -- so C
//         pins the chroma-preserving choice, not merely "non-black".
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

	// Write a minimal RED .bdf (format per DataDrivenBSDF.cpp / the asset
	// generator gen_chunkcoverage_assets.py): int sig(0xBDF), uint ver(1),
	// int numEmitter, int numPatches, then per emitter a double theta and,
	// per patch, 5 doubles brdf (thetaLo, thetaHi, R, G, B) + 5 doubles btdf.
	// A WIDE emitter theta (> pi/2) makes any front-facing query match the
	// single emitter; every patch reflects pure red so the lobe is unambiguous.
	bool WriteRedBDF( const char* path )
	{
		FILE* f = fopen( path, "wb" );
		if( !f ) return false;
		const int32_t  sig = 0x0BDF;       fwrite( &sig, sizeof(int32_t), 1, f );
		const uint32_t ver = 1;            fwrite( &ver, sizeof(uint32_t), 1, f );
		const int32_t  numEmitter = 1;     fwrite( &numEmitter, sizeof(int32_t), 1, f );
		const int32_t  numPatches = 2;     fwrite( &numPatches, sizeof(int32_t), 1, f );
		const double   emitterTheta = 1.6; fwrite( &emitterTheta, sizeof(double), 1, f );  // wide (> pi/2)
		for( int p = 0; p < numPatches; ++p ) {
			const double brdf[5] = { 0.1 * p, 0.1 * p + 0.05, 0.8, 0.1, 0.1 };   // RED reflectance
			const double btdf[5] = { 0.1 * p, 0.1 * p + 0.05, 0.0, 0.0, 0.0 };   // (unused transmission)
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
}

int main()
{
	std::cout << "=== DataDrivenBSDF spectral valueNM Test ===\n";
	GlobalLog();	// initialize the global log

	const char* kPath = "datadriven_nm_test_tmp.bdf";
	if( !WriteRedBDF( kPath ) ) {
		std::cout << "  FAIL: could not write temp .bdf\n";
		return 1;
	}

	DataDrivenBSDF* bsdf = new DataDrivenBSDF( kPath );
	bsdf->addref();

	// Normal incidence, retro view: v (light) and r (view) both along +Z, so
	// nr = nv = 1 (front hemisphere) and the single wide-theta emitter + first
	// patch are selected -> value() returns the (red) lobe.
	const Vector3 v( 0, 0, 1 );                 // light direction
	RayIntersectionGeometric ri = MakeRI( Vector3( 0, 0, -1 ) );  // r = -dir = +Z

	// A. RGB path sanity: value() must be non-zero (red).
	const RISEPel rgb = bsdf->value( v, ri );
	std::cout << std::fixed << std::setprecision( 4 )
			  << "  value() = (" << rgb.r << ", " << rgb.g << ", " << rgb.b << ")\n";
	Check( ColorMath::MaxValue( rgb ) > 1e-6, "value() (RGB path) is non-zero at normal incidence" );
	Check( rgb.r > rgb.g && rgb.r > rgb.b, "value() is RED (r dominates g,b) as authored" );

	// B. THE FIX: valueNM > 0 for every wavelength (was a hard 0 -> black).
	const Scalar nms[] = { 420.0, 470.0, 530.0, 580.0, 620.0, 680.0 };
	Scalar vnm[6];
	bool allPositive = true;
	std::cout << "  valueNM:";
	for( int i = 0; i < 6; ++i ) {
		vnm[i] = bsdf->valueNM( v, ri, nms[i] );
		std::cout << " " << (int)nms[i] << "nm=" << std::setprecision( 4 ) << vnm[i];
		if( !( vnm[i] > 0.0 ) || !std::isfinite( vnm[i] ) ) allPositive = false;
	}
	std::cout << "\n";
	Check( allPositive, "valueNM(nm) > 0 and finite for every probed wavelength (the spectral-black fix)" );

	// C. CHROMA preserved: a RED reflectance must peak at LONG wavelengths.
	//    620nm (red) must exceed both 530nm (green) and 470nm (blue).  A
	//    luminance/flat fallback would make these ~equal and fail this.
	const Scalar red = bsdf->valueNM( v, ri, 620.0 );
	const Scalar grn = bsdf->valueNM( v, ri, 530.0 );
	const Scalar blu = bsdf->valueNM( v, ri, 470.0 );
	std::cout << "  chroma: 620nm=" << red << "  530nm=" << grn << "  470nm=" << blu << "\n";
	Check( red > grn && red > blu,
		"valueNM preserves CHROMA: red BRDF peaks at long lambda (not a flat/gray luminance fallback)" );

	bsdf->release();
	std::remove( kPath );

	std::cout << "\n=== DataDrivenBSDFSpectralTest: " << s_pass << " passed, " << s_fail << " failed ===\n";
	return s_fail == 0 ? 0 : 1;
}
