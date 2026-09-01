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
//      7. PerfectReflectorSPF::ScatterNM at an authored-white reflectance:
//         the specular ray's krayNM == exactly 1.0 at 660nm.  End-to-end
//         through the highest-exposure delta-BSDF site (mutation-probe
//         confirmed this guard had NO tripwire prior to this check).
//      8. PerfectRefractorSPF::ScatterNM at an authored-white refractance
//         with a non-dispersive IOR: the transmitted (eRayRefraction)
//         ray's krayNM is wavelength-flat -- identical at 660nm and
//         550nm -- even though the Fresnel term `ref` and the raw
//         (unguarded) uplift both vary enormously across that span.
//      9. GGXBRDF::valueNM with white diffuse + white specular
//         (conductor Fresnel, non-dispersive ior/ext) at a fixed
//         off-mirror geometry: valueNM(660nm) == valueNM(550nm) exactly,
//         AND both equal the corresponding (identical) channel of the
//         RGB value() at the same geometry -- the guard applied through
//         a third, independent material family.
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
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Materials/SheenBRDF.h"
#include "../src/Library/Materials/PerfectReflectorSPF.h"
#include "../src/Library/Materials/PerfectRefractorSPF.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/MediaPathLocator.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/ISPF.h"

#include "TestStubObject.h"

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

	// [1/9] Document the raw LUT collapse the guard exists for.
	std::cout << "\n[1/9] Raw GetColorNM on white collapses at the red end (documents the bug)\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 1, 1, 1 ) );
		const Scalar raw660 = p->GetColorNM( ri, Scalar(660) );
		std::printf( "    raw white GetColorNM(660nm) = %.6e\n", double(raw660) );
		Check( raw660 < Scalar(0.01), "raw white GetColorNM(660nm) < 0.01 (JH collapse, not a guard bug)" );
		p->release();
	}

	// [2/9] GuardedGetColorNM(white) == exactly 1.0 at several red/NIR bins.
	std::cout << "\n[2/9] GuardedGetColorNM(white) == exactly 1.0\n";
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

	// [3/9] GuardedGetColorNM(0.95 grey) must NOT fire the guard: it must
	// equal the raw (unguarded) GetColorNM sample, and be < 1.
	std::cout << "\n[3/9] GuardedGetColorNM(0.95 grey) == raw GetColorNM (guard does not fire)\n";
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

	// [4/9] End-to-end through LambertianBRDF::valueNM.
	std::cout << "\n[4/9] LambertianBRDF::valueNM: white == INV_PI exactly, 0.95 grey < INV_PI\n";
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

	// [5/9] End-to-end through SheenBRDF::valueNM (Charlie sheen lobe --
	// a second material family independent of Lambertian).  Pick a
	// light/view pair off the mirror direction so D*V is nonzero (at
	// nDotH == 1 the Charlie distribution is exactly 0 regardless of the
	// guard, which would make this check vacuous).
	std::cout << "\n[5/9] SheenBRDF::valueNM(white) == value(white)'s channel exactly at 660nm\n";
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

	// [6/9] Chromatic-boosted (1.2,1.0,1.0): documents WHY the floor-only
	// `IsUntintedWhite` check is exact even though only two of its three
	// channels are exactly 1.0.  min(1.2,1.0,1.0) == 1.0 >= 1-1e-6, so the
	// guard fires -- and because `RGBAlbedoSpectrum::FromRGB` clamps its
	// input to [0,1] BEFORE the LUT lookup (RGBSpectra.h:39-40,
	// RGBToSpectrumTable.h:93), the raw (unguarded) uplift of this triple
	// clamps to the same (1,1,1) pure white and collapses at the red end
	// exactly like [1/9]'s raw-white case, so the guard's answer is exact
	// here too, not merely a floor-only approximation.
	std::cout << "\n[6/9] Chromatic-boosted (1.2,1.0,1.0): guard fires, raw uplift collapses like white\n";
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

	// [7/9] End-to-end through PerfectReflectorSPF::ScatterNM -- the highest-
	// exposure delta-reflection site (mutation probe: reverting this guard
	// site to raw GetColorNM survived the entire pre-existing suite).  A
	// white mirror's specular krayNM must be exactly 1.0 at 660nm -- an
	// independently-derived expected value (the literal constant), never
	// GuardedGetColorNM itself.
	std::cout << "\n[7/9] PerfectReflectorSPF::ScatterNM(white): krayNM == exactly 1.0 at 660nm\n";
	{
		IPainter* white = nullptr;
		RISE_API_CreateUniformColorPainter( &white, RISEPel( 1, 1, 1 ) );

		PerfectReflectorSPF* mirror = new PerfectReflectorSPF( *white );
		mirror->addref();

		StubObject* stub = new StubObject();
		stub->addref();
		const IORStack iorStack = MakeTestIORStack( stub );
		RandomNumberGenerator rng;
		IndependentSampler sampler( rng );

		ScatteredRayContainer scattered;
		mirror->ScatterNM( shadingRi, sampler, Scalar(660), scattered, iorStack );

		Check( scattered.Count() >= 1, "sanity: white mirror ScatterNM produces at least one ray" );
		bool foundSpecular = false;
		for( unsigned int i = 0; i < scattered.Count(); ++i ) {
			if( scattered[i].type == ScatteredRay::eRayReflection ) {
				foundSpecular = true;
				std::printf( "    mirror krayNM(660nm) = %.9f\n", double( scattered[i].krayNM ) );
				Check( scattered[i].krayNM == Scalar(1), "PerfectReflectorSPF::ScatterNM(white).krayNM == exactly 1.0 at 660nm" );
			}
		}
		Check( foundSpecular, "sanity: found the specular reflection lobe (test is not vacuous)" );

		mirror->release();
		white->release();
		stub->release();
	}

	// [8/9] End-to-end through PerfectRefractorSPF::ScatterNM -- a second
	// delta-BSDF family (transmission, not reflection).  `ref` depends on
	// Fresnel reflectance, which is itself wavelength-dependent in general,
	// so we don't compare against a fixed literal; instead we pin the
	// SHARPER invariant the guard buys us: with a non-dispersive IOR (so
	// `ref` -- and every other geometric factor -- is bit-identical at both
	// wavelengths), the transmitted ray's krayNM must be wavelength-flat.
	// The raw (unguarded) uplift of white is NOT wavelength-flat (it
	// collapses at the red end, per [1/9] above) -- so this invariant is a
	// genuine tripwire, not a restatement of the helper.
	std::cout << "\n[8/9] PerfectRefractorSPF::ScatterNM(white, non-dispersive IOR): krayNM(660nm) == krayNM(550nm)\n";
	{
		IPainter* white = nullptr;
		RISE_API_CreateUniformColorPainter( &white, RISEPel( 1, 1, 1 ) );
		IScalarPainter* ior = nullptr;
		RISE_API_CreateUniformScalarPainter( &ior, Scalar(1.5) );

		PerfectRefractorSPF* glass = new PerfectRefractorSPF( *white, *ior );
		glass->addref();

		StubObject* stub = new StubObject();
		stub->addref();
		// Environment IOR 1.0 (air), no object yet pushed -> containsCurrent()
		// is false -> the SPF takes the "entering" branch (air -> glass),
		// which can never TIR, guaranteeing a transmitted lobe exists.
		const IORStack iorStack = MakeTestIORStack( stub, Scalar(1.0) );
		RandomNumberGenerator rng;
		IndependentSampler sampler( rng );

		Scalar krayAt660 = -1, krayAt550 = -1;
		bool found660 = false, found550 = false;

		{
			ScatteredRayContainer scattered;
			glass->ScatterNM( shadingRi, sampler, Scalar(660), scattered, iorStack );
			for( unsigned int i = 0; i < scattered.Count(); ++i ) {
				if( scattered[i].type == ScatteredRay::eRayRefraction ) {
					found660 = true;
					krayAt660 = scattered[i].krayNM;
				}
			}
		}
		{
			ScatteredRayContainer scattered;
			glass->ScatterNM( shadingRi, sampler, Scalar(550), scattered, iorStack );
			for( unsigned int i = 0; i < scattered.Count(); ++i ) {
				if( scattered[i].type == ScatteredRay::eRayRefraction ) {
					found550 = true;
					krayAt550 = scattered[i].krayNM;
				}
			}
		}

		Check( found660 && found550, "sanity: transmitted (eRayRefraction) lobe present at both wavelengths (not TIR)" );
		Check( krayAt660 > Scalar(0), "sanity: krayNM(660nm) is nonzero (test is not vacuous)" );
		std::printf( "    glass krayNM(660nm) = %.9f, krayNM(550nm) = %.9f\n", double( krayAt660 ), double( krayAt550 ) );
		Check( krayAt660 == krayAt550, "PerfectRefractorSPF::ScatterNM(white).krayNM is wavelength-flat (660nm == 550nm)" );

		glass->release();
		white->release();
		ior->release();
		stub->release();
	}

	// [9/9] End-to-end through GGXBRDF::valueNM (conductor Fresnel mode,
	// non-dispersive ior/ext) -- a third material family independent of
	// Lambertian/Sheen and of the two delta-BSDF SPFs above.  Off-mirror
	// light direction so D*G2 (and therefore the specular lobe) is nonzero.
	// Both diffuse and specular painters are white, so:
	//   (a) valueNM(660nm) == valueNM(550nm) exactly -- alphaX/alphaY/ior/
	//       ext are all non-dispersive IScalarPainter reads, so the ONLY
	//       possible wavelength dependence left is the two GuardedGetColorNM
	//       calls (specColor, diffuse); if either regresses to raw
	//       GetColorNM, white's LUT collapse breaks this equality.
	//   (b) valueNM(660nm) equals value()'s (RGB) channel exactly -- the
	//       RGB path reads the same white/white painters via GetColor (no
	//       LUT uplift at all), so this pins the guarded NM path to the
	//       LUT-free RGB reference.
	std::cout << "\n[9/9] GGXBRDF::valueNM(white diffuse, white specular): wavelength-flat and RGB-matching\n";
	{
		IPainter* diffuse = nullptr;
		RISE_API_CreateUniformColorPainter( &diffuse, RISEPel( 1, 1, 1 ) );
		IPainter* specular = nullptr;
		RISE_API_CreateUniformColorPainter( &specular, RISEPel( 1, 1, 1 ) );
		IScalarPainter* alphaX = nullptr;
		RISE_API_CreateUniformScalarPainter( &alphaX, Scalar(0.3) );
		IScalarPainter* alphaY = nullptr;
		RISE_API_CreateUniformScalarPainter( &alphaY, Scalar(0.3) );
		IScalarPainter* iorS = nullptr;
		RISE_API_CreateUniformScalarPainter( &iorS, Scalar(2.5) );
		IScalarPainter* extS = nullptr;
		RISE_API_CreateUniformScalarPainter( &extS, Scalar(3.0) );

		GGXBRDF* ggx = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *iorS, *extS );
		ggx->addref();

		// Off-mirror light direction (mirrors the SheenBRDF setup above):
		// with vNormal/onb.w() == (0,0,1) and view == (0,0,1) (ray arrives
		// along (0,0,-1)), the mirror direction is exactly (0,0,1); using
		// vLightIn == (0.6,0,0.8) keeps h != n so D, G2 are both nonzero.
		const Vector3 vLightIn( 0.6, 0.0, 0.8 );

		const Scalar nmAt660 = ggx->valueNM( vLightIn, shadingRi, Scalar(660) );
		const Scalar nmAt550 = ggx->valueNM( vLightIn, shadingRi, Scalar(550) );
		const RISEPel rgbVal = ggx->value( vLightIn, shadingRi );

		std::printf( "    valueNM(660nm) = %.9f, valueNM(550nm) = %.9f, value()[0] = %.9f\n",
			double( nmAt660 ), double( nmAt550 ), double( rgbVal[0] ) );

		Check( nmAt660 > Scalar(0), "sanity: valueNM(660nm) is nonzero (off-mirror geometry is not vacuous)" );
		Check( nmAt660 == nmAt550, "GGXBRDF::valueNM(white,white) is wavelength-flat (660nm == 550nm)" );
		Check( nmAt660 == rgbVal[0], "GGXBRDF::valueNM(white,white, 660nm) == value()'s (RGB) channel exactly" );

		ggx->release();
		diffuse->release();
		specular->release();
		alphaX->release();
		alphaY->release();
		iorS->release();
		extS->release();
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
