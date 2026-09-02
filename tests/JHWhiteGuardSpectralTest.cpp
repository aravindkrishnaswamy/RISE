//////////////////////////////////////////////////////////////////////
//
//  JHWhiteGuardSpectralTest.cpp - Regression test for the
//    Jakob-Hanika white-corner guard (IPainter.h: IsUntintedWhite /
//    GuardedGetColorNM).
//
//    Background: `IPainter::GetColorNM` runs the Jakob-Hanika RGB->
//    spectrum uplift.  Pure white in the Rec.709 LUT collapses toward
//    zero above ~620nm (measured in src/Library/Materials/CoatedLayer.h
//    PassTransmittance, ~line 302: 640nm -> 0.5189, 660nm -> 1.28e-5).
//    A multiplicative material slot (reflectance, tint, specular color,
//    ...) at authored white must be an exact no-op at every wavelength,
//    matching the RGB path exactly -- CoatedBRDF::ResolveCoat established
//    this precedent (~line 107) by deciding "tinted?" once from the
//    authored RGB and skipping GetColorNM entirely when untinted.
//    IPainter.h's `GuardedGetColorNM` generalizes that decision into a
//    single shared helper, now applied at every confirmed multiplicative
//    NM-path site across src/Library/Materials.
//
//    This test locks down:
//      1. White painter's raw GetColorNM is still tiny at the red end
//         (documents the underlying LUT behavior the guard exists for).
//      2. GuardedGetColorNM(white) == exactly 1.0 at 640/660/700nm.
//      3. GuardedGetColorNM(0.95 grey) == raw GetColorNM(0.95 grey)
//         (the guard must not fire below the epsilon threshold).
//      4. LambertianBRDF::valueNM at white reflectance == INV_PI
//         exactly at 660nm (the guard applied end-to-end through a
//         real material), and is strictly less than INV_PI for a
//         0.95 grey reflectance (tinted materials are NOT flattened).
//      5. SheenBRDF::valueNM at an authored-white sheen colour equals
//         SheenBRDF::value's (RGB) channel exactly at 660nm -- the
//         guard applied through the Charlie sheen lobe, a second
//         material independent of Lambertian.
//      6. A chromatic-boosted (1.2, 1.0, 1.0) triple also fires the
//         guard (floor-only min check), and its raw (unguarded) uplift
//         collapses like white's -- because RGBAlbedoSpectrum::FromRGB
//         clamps to [0,1] before the LUT lookup, documenting why the
//         floor-only check is exact for Albedo-kind painters.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Materials/SheenBRDF.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/MediaPathLocator.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"

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

	RayIntersectionGeometric MakeDummyRi()
	{
		const Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( r, rs );
		ri.bHit = true;
		ri.ptCoord = Point2( 0, 0 );
		return ri;
	}

	// A ri/vLightIn pair that satisfies LambertianBRDF::ShouldReflect:
	// viewer arrives from above (-Z, i.e. Dir() == (0,0,-1)) onto a
	// surface with normal (0,0,1); the light direction (0,0,1) is also
	// in front of the surface.  Mirrors PathValueOpsTest.cpp's MakeRI.
	RayIntersectionGeometric MakeShadingRi()
	{
		const Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, -1 ) );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( r, rs );
		ri.bHit = true;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		return ri;
	}
}

int main()
{
	std::cout << "JHWhiteGuardSpectralTest -- IPainter.h IsUntintedWhite / GuardedGetColorNM\n";

	GlobalMediaPathLocator().AddPath( "." );
	GlobalMediaPathLocator().AddPath( "../" );
	GlobalMediaPathLocator().AddPath( "../../" );

	const RayIntersectionGeometric ri = MakeDummyRi();

	// [1/4] Document the raw LUT collapse the guard exists for.
	std::cout << "\n[1/4] Raw GetColorNM on white collapses at the red end (documents the bug)\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 1, 1, 1 ) );
		const Scalar raw660 = p->GetColorNM( ri, Scalar(660) );
		std::printf( "    raw white GetColorNM(660nm) = %.6e\n", double(raw660) );
		Check( raw660 < Scalar(0.01), "raw white GetColorNM(660nm) < 0.01 (JH collapse, not a guard bug)" );
		p->release();
	}

	// [2/4] GuardedGetColorNM(white) == exactly 1.0 at several red/NIR bins.
	std::cout << "\n[2/4] GuardedGetColorNM(white) == exactly 1.0\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 1, 1, 1 ) );
		Check( IsUntintedWhite( p->GetColor( ri ) ), "authored (1,1,1) classifies as untinted white" );
		for( Scalar nm : { Scalar(640), Scalar(660), Scalar(700) } ) {
			const Scalar g = GuardedGetColorNM( *p, ri, nm );
			std::printf( "    GuardedGetColorNM(white, %.0fnm) = %.9f\n", double(nm), double(g) );
			Check( g == Scalar(1), "GuardedGetColorNM(white) is exactly 1.0" );
		}
		p->release();
	}

	// [3/4] GuardedGetColorNM(0.95 grey) must NOT fire the guard: it must
	// equal the raw (unguarded) GetColorNM sample, and be < 1.
	std::cout << "\n[3/4] GuardedGetColorNM(0.95 grey) == raw GetColorNM (guard does not fire)\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 0.95, 0.95, 0.95 ) );
		Check( !IsUntintedWhite( p->GetColor( ri ) ), "authored (0.95,0.95,0.95) does NOT classify as untinted white" );
		for( Scalar nm : { Scalar(640), Scalar(660), Scalar(700) } ) {
			const Scalar raw = p->GetColorNM( ri, nm );
			const Scalar guarded = GuardedGetColorNM( *p, ri, nm );
			std::printf( "    0.95 grey @ %.0fnm: raw=%.6f guarded=%.6f\n", double(nm), double(raw), double(guarded) );
			Check( guarded == raw, "guarded sample == raw sample for 0.95 grey (guard inert)" );
			Check( guarded < Scalar(1), "0.95 grey sample stays < 1" );
		}
		p->release();
	}

	// [4/4] End-to-end through LambertianBRDF::valueNM.
	std::cout << "\n[4/4] LambertianBRDF::valueNM: white == INV_PI exactly, 0.95 grey < INV_PI\n";
	const RayIntersectionGeometric shadingRi = MakeShadingRi();
	{
		IPainter* white = nullptr;
		RISE_API_CreateUniformColorPainter( &white, RISEPel( 1, 1, 1 ) );
		LambertianBRDF* whiteBRDF = new LambertianBRDF( *white );
		whiteBRDF->addref();

		const Vector3 vLightIn( 0, 0, 1 );
		const Scalar whiteVal = whiteBRDF->valueNM( vLightIn, shadingRi, Scalar(660) );
		std::printf( "    white valueNM(660nm) = %.9f, INV_PI = %.9f\n", double(whiteVal), double(INV_PI) );
		Check( whiteVal == INV_PI, "LambertianBRDF::valueNM(white, 660nm) == INV_PI exactly" );

		whiteBRDF->release();
		white->release();

		IPainter* grey = nullptr;
		RISE_API_CreateUniformColorPainter( &grey, RISEPel( 0.95, 0.95, 0.95 ) );
		LambertianBRDF* greyBRDF = new LambertianBRDF( *grey );
		greyBRDF->addref();

		const Scalar greyVal = greyBRDF->valueNM( vLightIn, shadingRi, Scalar(660) );
		std::printf( "    0.95 grey valueNM(660nm) = %.9f\n", double(greyVal) );
		Check( greyVal < INV_PI, "LambertianBRDF::valueNM(0.95 grey, 660nm) < INV_PI (not flattened by the guard)" );

		greyBRDF->release();
		grey->release();
	}

	// [5/5] End-to-end through SheenBRDF::valueNM (Charlie sheen lobe --
	// a second material family independent of Lambertian).  Pick a
	// light/view pair off the mirror direction so D*V is nonzero (at
	// nDotH == 1 the Charlie distribution is exactly 0 regardless of the
	// guard, which would make this check vacuous).
	std::cout << "\n[5/5] SheenBRDF::valueNM(white) == value(white)'s channel exactly at 660nm\n";
	{
		IPainter* white = nullptr;
		RISE_API_CreateUniformColorPainter( &white, RISEPel( 1, 1, 1 ) );
		IScalarPainter* roughness = nullptr;
		RISE_API_CreateUniformScalarPainter( &roughness, Scalar(0.3) );

		SheenBRDF* whiteSheen = new SheenBRDF( *white, *roughness );
		whiteSheen->addref();

		const Vector3 vLightIn( 0.6, 0.0, 0.8 );	// off-mirror: nDotH != 1, so D*V != 0
		const RISEPel rgbVal = whiteSheen->value( vLightIn, shadingRi );
		const Scalar nmVal = whiteSheen->valueNM( vLightIn, shadingRi, Scalar(660) );
		std::printf( "    white sheen value(660nm-ish RGB channel) = %.9f, valueNM(660nm) = %.9f\n",
			double( rgbVal[0] ), double( nmVal ) );
		Check( rgbVal[0] > Scalar(0), "sanity: off-mirror direction gives nonzero D*V (test is not vacuous)" );
		Check( nmVal == rgbVal[0], "SheenBRDF::valueNM(white, 660nm) == value(white)'s channel exactly" );

		whiteSheen->release();
		white->release();
		roughness->release();
	}

	// [6/6] Chromatic-boosted (1.2,1.0,1.0): documents WHY the floor-only
	// `IsUntintedWhite` check is exact even though only two of its three
	// channels are exactly 1.0.  min(1.2,1.0,1.0) == 1.0 >= 1-1e-6, so the
	// guard fires -- and because `RGBAlbedoSpectrum::FromRGB` clamps its
	// input to [0,1] BEFORE the LUT lookup (RGBSpectra.h:39-40,
	// RGBToSpectrumTable.h:93), the raw (unguarded) uplift of this triple
	// clamps to the same (1,1,1) pure white and collapses at the red end
	// exactly like [1/4]'s raw-white case, so the guard's answer is exact
	// here too, not merely a floor-only approximation.
	std::cout << "\n[6/6] Chromatic-boosted (1.2,1.0,1.0): guard fires, raw uplift collapses like white\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 1.2, 1.0, 1.0 ) );
		Check( IsUntintedWhite( p->GetColor( ri ) ), "authored (1.2,1.0,1.0) classifies as untinted white (floor-only min check)" );
		const Scalar raw660 = p->GetColorNM( ri, Scalar(660) );
		const Scalar guarded660 = GuardedGetColorNM( *p, ri, Scalar(660) );
		std::printf( "    (1.2,1,1) @ 660nm: raw=%.6e guarded=%.9f\n", double(raw660), double(guarded660) );
		Check( raw660 < Scalar(0.01), "raw uplift of (1.2,1,1) also collapses at 660nm (albedo clamp == white's uplift)" );
		Check( guarded660 == Scalar(1), "GuardedGetColorNM((1.2,1,1)) == exactly 1.0 (guard fires on the floor check)" );
		p->release();
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
