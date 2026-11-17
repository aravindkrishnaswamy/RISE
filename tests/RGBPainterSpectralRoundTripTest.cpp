//////////////////////////////////////////////////////////////////////
//
//  RGBPainterSpectralRoundTripTest.cpp - Verifies that
//    UniformColorPainter (and other RGB-source leaf painters) now
//    return physically-meaningful spectral responses via
//    GetColorNM, instead of the previous luminance-proxy
//    `MaxValue(C)` (which made spectral integrators silently sample
//    materials at one scalar regardless of wavelength).
//
//    The test:
//      For each of N random in-gamut RISEPel (Rec.709 Linear) triples:
//        1. Construct a UniformColorPainter with that RGB.
//        2. Sample GetColorNM at every 5nm bin from 380-780nm.
//        3. Integrate the sampled REFLECTANCE against CIE 1931
//           WEIGHTED BY THE REFERENCE ILLUMINANT (D65), normalising
//           by ∫D65·ȳ dλ — the Stage C forward model
//           (docs/SPECTRAL_ILLUMINANT_CONVENTION.md).
//        4. Convert XYZ(D65) → Rec.709(D65).
//        5. Assert the round-trip matches the original RGB within
//           tolerance.
//
//    This is L3.B's acceptance test.
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
#include <random>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Materials/LambertianEmitter.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"
#include "../src/Library/Utilities/MediaPathLocator.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/ILightPriv.h"

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

	// Integrate painter.GetColorNM samples — a REFLECTANCE — against
	// CIE 1931 under the reference illuminant, matching the LUT
	// generator's rec709 forward model
	// (tools/JakobHanikaLUTGen.cpp::IntegrateToTarget):
	//
	//   rgb = M · ∫ S·D65·cmf dλ / ∫ D65·ȳ dλ
	//
	// Before Stage C (2026-09-02) this integrated under a FLAT
	// illuminant, matching the LUT's then-flat-E training.  That
	// convention could not represent white or a neutral grey (see
	// docs/SPECTRAL_ILLUMINANT_CONVENTION.md); the illuminant now lives
	// in the forward model on both ends.  D65 comes from the production
	// table via RGBIlluminantSpectrum::ReferenceIlluminant, whose own
	// Y-normalisation cancels out of the ratio.
	RISEPel IntegratePainterToRec709( const IPainter& painter, const RayIntersectionGeometric& ri )
	{
		double X = 0, Y = 0, Z = 0, normY = 0;
		for( int i = 0; i < 81; ++i ) {
			const Scalar lambda = Scalar(380) + Scalar(i * 5);
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			const double illum   = double( RGBIlluminantSpectrum::ReferenceIlluminant( lambda ) );
			const double specVal = double( painter.GetColorNM( ri, lambda ) ) * illum;
			X += specVal * obs.X;
			Y += specVal * obs.Y;
			Z += specVal * obs.Z;
			normY += illum * obs.Y;
		}
		const double inv = 1.0 / normY;
		X *= inv; Y *= inv; Z *= inv;

		// XYZ(D65) → Rec.709(D65) — matches the LUT generator's rec709
		// target forward model (no Bradford adapt; both spaces D65).
		// Matrix inlined from src/Library/Utilities/Color/Color.cpp
		// (mxXYZtoRec709).
		RISEPel out;
		out.r = Scalar(  3.240479 * X - 1.537150 * Y - 0.498535 * Z );
		out.g = Scalar( -0.969256 * X + 1.875992 * Y + 0.041556 * Z );
		out.b = Scalar(  0.055648 * X - 0.204043 * Y + 1.057311 * Z );
		return out;
	}

	//////////////////////////////////////////////////////////////////
	// Stage C slice 2 (docs/SPECTRAL_ILLUMINANT_CONVENTION.md)
	//
	// The helper above integrates a REFLECTANCE the way the LUT
	// GENERATOR does (weighted by D65, normalised by ∫D65·ȳ).  The one
	// below integrates a RADIANCE the way the FILM does: no illuminant
	// weighting at all, normalised by ∫ȳ alone
	// (PixelBasedSpectralIntegratingRasterizer's mYNormalization =
	// (b-a)/k_y composed with FilteredFilm's un-adapted
	// XYZ(D65)→Rec709(D65) resolve).  Putting D65 in BOTH places would
	// double-apply it; the whole point of slice 2 is that the D65 shape
	// rides in the SOURCE, which is exactly what GetRadianceNM /
	// emittedRadianceNM now return.
	//
	// So: a FLAT unit spectrum through THIS integral lands on
	// (1.20485, 0.94827, 0.90894) — Y = 1 but reddish — while a
	// D65norm-shaped unit source lands on (1, 1, 1).  Every check below
	// that says "must integrate to the authored RGB" is testing exactly
	// that distinction.
	//////////////////////////////////////////////////////////////////
	template<class Fn>
	RISEPel IntegrateRadianceThroughFilm( Fn sampleAtNM )
	{
		double X = 0, Y = 0, Z = 0, k_y = 0;
		for( int i = 0; i < 81; ++i ) {
			const Scalar lambda = Scalar(380) + Scalar(i * 5);
			XYZPel obs;
			if( !ColorUtils::XYZFromNM( obs, lambda ) ) continue;
			const double L = double( sampleAtNM( lambda ) );
			X += L * obs.X;
			Y += L * obs.Y;
			Z += L * obs.Z;
			k_y += double( obs.Y );		// the film's ∫ȳ dλ, same 5 nm grid
		}
		const double inv = 1.0 / k_y;
		X *= inv; Y *= inv; Z *= inv;

		RISEPel out;
		out.r = Scalar(  3.240479 * X - 1.537150 * Y - 0.498535 * Z );
		out.g = Scalar( -0.969256 * X + 1.875992 * Y + 0.041556 * Z );
		out.b = Scalar(  0.055648 * X - 0.204043 * Y + 1.057311 * Z );
		return out;
	}

	double MaxAbsDiff( const RISEPel& a, const RISEPel& b )
	{
		return std::max( std::max( std::fabs( a.r - b.r ), std::fabs( a.g - b.g ) ),
		                 std::fabs( a.b - b.b ) );
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

	// A hit with a real SHADING FRAME, needed by anything that evaluates a
	// BSDF: `LambertianBRDF::valueNM` gates on
	// `ShouldReflect( vLightIn, ri, ri.onb.w() )`, which requires both the
	// light direction AND the outgoing (-ray) direction to be on the
	// normal's side.  The plain dummy above leaves `onb` / `vNormal` at
	// zero and the ray pointing +Z, so every BSDF there returns 0 -- which
	// would make a "the BSDF is wavelength-flat" check pass VACUOUSLY.
	// Surface normal +Z, viewer above looking down (-Z), light straight up.
	RayIntersectionGeometric MakeShadingRi()
	{
		const Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, -1 ) );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( r, rs );
		ri.bHit = true;
		ri.ptCoord = Point2( 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		return ri;
	}
}

int main()
{
	std::cout << "RGBPainterSpectralRoundTripTest -- UniformColorPainter spectral path\n";

	GlobalMediaPathLocator().AddPath( "." );
	GlobalMediaPathLocator().AddPath( "../" );
	GlobalMediaPathLocator().AddPath( "../../" );

	const RayIntersectionGeometric ri = MakeDummyRi();

	// Test 1: GetColor still returns the exact RGB (no regression on
	// the non-spectral path).
	std::cout << "\n[1/8] GetColor unchanged on RGB path\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 0.7, 0.4, 0.2 ) );
		const RISEPel c = p->GetColor( ri );
		Check( std::fabs( c.r - 0.7 ) < 1e-9 &&
		       std::fabs( c.g - 0.4 ) < 1e-9 &&
		       std::fabs( c.b - 0.2 ) < 1e-9,
		       "GetColor returns exact input RGB" );
		p->release();
	}

	// Test 2: GetColorNM returns wavelength-varying values (not a
	// single luminance constant).
	std::cout << "\n[2/8] GetColorNM is wavelength-varying\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 0.8, 0.2, 0.2 ) );
		const Scalar v500 = p->GetColorNM( ri, Scalar(500) );	// short — should be low for red
		const Scalar v650 = p->GetColorNM( ri, Scalar(650) );	// long — should be high for red
		Check( v650 > v500,
		       "red painter's spectrum is higher at long λ than short λ" );
		std::printf( "    red painter: S(500)=%.3f, S(650)=%.3f\n",
			double(v500), double(v650) );
		p->release();
	}

	// Test 3: round-trip in-gamut sweep — the headline spectral
	// correctness test.
	std::cout << "\n[3/8] Random in-gamut round-trip\n";
	std::mt19937 rng( 54321 );
	std::uniform_real_distribution<double> uni( 0.05, 0.85 );
	const int kSamples = 50;
	double sumErr = 0, maxErr = 0;
	int withinTol = 0;
	for( int i = 0; i < kSamples; ++i ) {
		const RISEPel rgb( uni(rng), uni(rng), uni(rng) );
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, rgb );
		const RISEPel rt = IntegratePainterToRec709( *p, ri );
		p->release();

		const double err = std::sqrt(
			(rt.r - rgb.r) * (rt.r - rgb.r) +
			(rt.g - rgb.g) * (rt.g - rgb.g) +
			(rt.b - rgb.b) * (rt.b - rgb.b) );
		sumErr += err;
		maxErr  = std::max( maxErr, err );
		if( err < 5e-3 ) ++withinTol;
	}
	const double meanErr = sumErr / kSamples;
	std::printf( "  %d painters, mean L2 err = %.5f, max = %.5f, within-5e-3 = %d/%d\n",
		kSamples, meanErr, maxErr, withinTol, kSamples );
	// Measured post-Stage-C on this seed: mean 1.1e-4, max 2.7e-4.
	// Pre-Stage-C thresholds were 0.02 / 0.05, sized for the flat-E
	// LUT's unconverged cells; with 100 % cell convergence the residual
	// is pure grid quantisation, so tighten ~10x.
	Check( meanErr < 2e-3,
	       "interior mean L2 error < 2e-3" );
	Check( withinTol >= int(0.95 * kSamples),
	       "≥ 95% of painter spectra round-trip within 5e-3 L2" );

	//////////////////////////////////////////////////////////////////
	// Stage C slice 2 — the SOURCE-term path (GetRadianceNM and the
	// consumers routed onto it).  Everything above tests reflectance;
	// everything below tests radiance.
	//////////////////////////////////////////////////////////////////

	std::cout << "\n[4/8] IPainter::GetRadianceNM round-trips through the FILM model\n";
	{
		// White.  This is the headline: pre-slice-2 a white emissive
		// painter was sampled through GetColorNM, whose spectrum is a
		// (nearly) FLAT reflectance, and the film resolved it to the
		// equal-energy chromaticity (1.20485, 0.94827, 0.90894).
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, RISEPel( 1, 1, 1 ) );
		const RISEPel rt = IntegrateRadianceThroughFilm(
			[&]( Scalar nm ){ return p->GetRadianceNM( ri, nm ); } );
		std::printf( "    white  radiance -> (%.5f, %.5f, %.5f)\n",
			double(rt.r), double(rt.g), double(rt.b) );
		Check( MaxAbsDiff( rt, RISEPel( 1, 1, 1 ) ) < 2e-3,
		       "white GetRadianceNM integrates to (1,1,1) within 2e-3" );

		// And prove the OLD path really was the reddish one, so this
		// check cannot pass vacuously if GetRadianceNM ever regresses
		// to forwarding GetColorNM.
		const RISEPel flat = IntegrateRadianceThroughFilm(
			[&]( Scalar nm ){ return p->GetColorNM( ri, nm ); } );
		std::printf( "    white reflectance through the same film model -> (%.5f, %.5f, %.5f)\n",
			double(flat.r), double(flat.g), double(flat.b) );
		Check( flat.r - flat.g > 0.15,
		       "a FLAT (reflectance) white through the film IS reddish -- the two paths differ" );
		p->release();
	}
	{
		// Saturated colour: the illuminant uplift must still round-trip
		// the authored chromaticity, not just neutrals.
		const RISEPel authored( 0.8, 0.2, 0.2 );
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, authored );
		const RISEPel rt = IntegrateRadianceThroughFilm(
			[&]( Scalar nm ){ return p->GetRadianceNM( ri, nm ); } );
		std::printf( "    (0.8,0.2,0.2) radiance -> (%.5f, %.5f, %.5f)\n",
			double(rt.r), double(rt.g), double(rt.b) );
		Check( MaxAbsDiff( rt, authored ) < 2e-3,
		       "saturated GetRadianceNM integrates to the authored RGB within 2e-3" );
		p->release();
	}

	std::cout << "\n[5/8] Composite painter uses the IPainter DEFAULT (uplift the composed colour)\n";
	{
		// BlendPainter has no GetRadianceNM override, so it must fall to
		// IPainter's default -- which uplifts its COMPOSED GetColor as an
		// illuminant.  Forwarding to the children's reflectance spectra
		// instead would be wrong (a sum of reflectance-shaped spectra is
		// never D65-shaped), so this pins the composition semantics.
		IPainter *pa = nullptr, *pb = nullptr, *pm = nullptr, *pblend = nullptr;
		RISE_API_CreateUniformColorPainter( &pa, RISEPel( 0.9, 0.3, 0.1 ) );
		RISE_API_CreateUniformColorPainter( &pb, RISEPel( 0.1, 0.4, 0.85 ) );
		RISE_API_CreateUniformColorPainter( &pm, RISEPel( 0.5, 0.5, 0.5 ) );
		RISE_API_CreateBlendPainter( &pblend, *pa, *pb, *pm );

		const RISEPel composed = pblend->GetColor( ri );
		double maxDelta = 0;
		for( int i = 0; i < 81; ++i ) {
			const Scalar lambda = Scalar(380) + Scalar(i * 5);
			const double got = double( pblend->GetRadianceNM( ri, lambda ) );
			const double want = double( RGBIlluminantSpectrum::FromRGB( composed ).Eval( lambda ) );
			maxDelta = std::max( maxDelta, std::fabs( got - want ) );
		}
		std::printf( "    blend composed = (%.4f, %.4f, %.4f), max |GetRadianceNM - default| = %.3e\n",
			double(composed.r), double(composed.g), double(composed.b), maxDelta );
		Check( maxDelta < 1e-9,
		       "BlendPainter::GetRadianceNM == illuminant uplift of the COMPOSED colour" );

		// It must also round-trip that composed colour through the film.
		const RISEPel rt = IntegrateRadianceThroughFilm(
			[&]( Scalar nm ){ return pblend->GetRadianceNM( ri, nm ); } );
		Check( MaxAbsDiff( rt, composed ) < 3e-3,
		       "composite GetRadianceNM integrates back to the composed RGB" );

		pblend->release(); pm->release(); pb->release(); pa->release();
	}

	std::cout << "\n[6/8] LambertianEmitter::emittedRadianceNM carries the illuminant shape\n";
	{
		// NOT wavelength-flat: it is D65-shaped now.  The invariant that
		// actually matters is that integrating it through the film model
		// returns the authored exitance (times the emitter's INV_PI *
		// scale, which is wavelength-independent).
		IPainter* pExit = nullptr;
		RISE_API_CreateUniformColorPainter( &pExit, RISEPel( 1, 1, 1 ) );
		LambertianEmitter* em = new LambertianEmitter( *pExit, Scalar(1) );
		em->addref();

		// Emit along the normal so the one-sided Dot(out, N) > 0 gate passes.
		const Vector3 N( 0, 0, 1 );
		const Vector3 out( 0, 0, 1 );

		const RISEPel rt = IntegrateRadianceThroughFilm(
			[&]( Scalar nm ){ return em->emittedRadianceNM( ri, out, N, nm ); } );
		const RISEPel wantPel = em->emittedRadiance( ri, out, N );
		std::printf( "    emitter NM -> (%.5f, %.5f, %.5f)   RGB path -> (%.5f, %.5f, %.5f)\n",
			double(rt.r), double(rt.g), double(rt.b),
			double(wantPel.r), double(wantPel.g), double(wantPel.b) );
		Check( MaxAbsDiff( rt, wantPel ) < 2e-3,
		       "white emitter's NM radiance integrates to its own RGB radiance within 2e-3" );

		// Positive control on the shape: D65 is NOT flat, so the sampled
		// radiance must vary across the band.  (A regression to a flat
		// spectrum would still satisfy Y = 1 but not this.)
		const Scalar e450 = em->emittedRadianceNM( ri, out, N, Scalar(450) );
		const Scalar e650 = em->emittedRadianceNM( ri, out, N, Scalar(650) );
		Check( std::fabs( double(e450) - double(e650) ) > 0.05 * double(e450),
		       "emitted spectrum is D65-shaped (450 nm and 650 nm differ by > 5%)" );

		em->release();
		pExit->release();
	}

	std::cout << "\n[7/8] Point light is spectrally COLOURED, not a luma scalar\n";
	{
		// Pre-slice-2 every ILight NM consumer used
		// 0.2126r + 0.7152g + 0.0722b at EVERY wavelength, so a red light
		// was spectrally grey: S(450) == S(650) exactly.
		const RISEPel authored( 1.0, 0.2, 0.2 );
		ILightPriv* pl = nullptr;
		RISE_API_CreatePointOmniLight( &pl, Scalar(1), authored, false );

		const Vector3 dir( 0, 0, 1 );
		const Scalar l450 = pl->emittedRadianceNM( dir, Scalar(450) );
		const Scalar l650 = pl->emittedRadianceNM( dir, Scalar(650) );
		const Scalar s450 = RGBIlluminantSpectrum::FromRGB( authored ).Eval( Scalar(450) );
		const Scalar s650 = RGBIlluminantSpectrum::FromRGB( authored ).Eval( Scalar(650) );
		std::printf( "    light S(450)=%.5f S(650)=%.5f  ratio=%.4f (illuminant ratio %.4f)\n",
			double(l450), double(l650), double(l650/l450), double(s650/s450) );
		Check( double(l650) > 2.0 * double(l450),
		       "a red point light really is red at 650 nm vs 450 nm (was EQUAL under luma collapse)" );
		Check( std::fabs( double(l650/l450) - double(s650/s450) ) < 1e-9,
		       "the light's 450/650 ratio IS the illuminant spectrum's ratio" );

		// White Lambertian receiver: its valueNM is wavelength-FLAT (the
		// JH white guard makes an untinted slot a bit-exact no-op), so the
		// spectral direct-lighting ratio at a white surface equals the
		// light's own ratio above.  Asserted here rather than by running
		// ComputeDirectLightingNM, which would need a live IRayCaster.
		IPainter* pWhite = nullptr;
		RISE_API_CreateUniformColorPainter( &pWhite, RISEPel( 1, 1, 1 ) );
		IMaterial* pMat = nullptr;
		RISE_API_CreateLambertianMaterial( &pMat, *pWhite );
		IBSDF* pBrdf = pMat->GetBSDF();
		const RayIntersectionGeometric sri = MakeShadingRi();
		const Vector3 vIn( 0, 0, 1 );
		const Scalar b450 = pBrdf->valueNM( vIn, sri, Scalar(450) );
		const Scalar b650 = pBrdf->valueNM( vIn, sri, Scalar(650) );
		std::printf( "    white Lambertian valueNM: %.6f @450, %.6f @650 (1/pi = %.6f)\n",
			double(b450), double(b650), double(INV_PI) );
		// Guard against a VACUOUS pass: a mis-built `ri` makes valueNM
		// return 0 at every wavelength, which is trivially "flat".
		Check( double(b450) > 0.1,
		       "the white Lambertian BSDF actually evaluates (not a vacuously-flat zero)" );
		Check( std::fabs( double(b450) - double(b650) ) < 1e-12,
		       "white Lambertian valueNM is wavelength-flat, so the receiver adds no tint" );

		// And the whole light integrates back to its authored colour.
		const RISEPel rt = IntegrateRadianceThroughFilm(
			[&]( Scalar nm ){ return pl->emittedRadianceNM( dir, nm ); } );
		std::printf( "    light radiance -> (%.5f, %.5f, %.5f)\n",
			double(rt.r), double(rt.g), double(rt.b) );
		Check( MaxAbsDiff( rt, authored ) < 2e-3,
		       "point light's NM radiance integrates to its authored RGB within 2e-3" );

		pMat->release();
		pWhite->release();
		pl->release();
	}

	std::cout << "\n[8/8] End-to-end film neutrality: white source x white Lambertian\n";
	{
		// The whole slice-2 chain in one number: a white source's
		// GetRadianceNM times a white Lambertian's BRDF (times pi, the
		// cosine-weighted hemispherical normalisation) integrated with the
		// FILM's normalisation must land on neutral (1, 1, 1).  This is
		// the unit-test twin of the `_abtest2` scene-(c) render.
		IPainter* pSrc = nullptr;
		RISE_API_CreateUniformColorPainter( &pSrc, RISEPel( 1, 1, 1 ) );
		IPainter* pAlb = nullptr;
		RISE_API_CreateUniformColorPainter( &pAlb, RISEPel( 1, 1, 1 ) );
		IMaterial* pMat = nullptr;
		RISE_API_CreateLambertianMaterial( &pMat, *pAlb );
		IBSDF* pBrdf = pMat->GetBSDF();
		const RayIntersectionGeometric sri = MakeShadingRi();
		const Vector3 vIn( 0, 0, 1 );
		// Sanity: the BSDF must be live, else the neutrality check below
		// would pass on (0,0,0) being 2e-3 from... nothing.  It is not.
		Check( double( pBrdf->valueNM( vIn, sri, Scalar(550) ) ) > 0.1,
		       "receiver BSDF evaluates non-zero (neutrality check is not vacuous)" );

		const RISEPel rt = IntegrateRadianceThroughFilm(
			[&]( Scalar nm ){
				return pSrc->GetRadianceNM( sri, nm ) * pBrdf->valueNM( vIn, sri, nm ) * Scalar(PI);
			} );
		std::printf( "    source x BRDF x pi -> (%.5f, %.5f, %.5f)\n",
			double(rt.r), double(rt.g), double(rt.b) );
		Check( MaxAbsDiff( rt, RISEPel( 1, 1, 1 ) ) < 2e-3,
		       "white source on a white Lambertian resolves to neutral (1,1,1) within 2e-3" );

		pMat->release();
		pAlb->release();
		pSrc->release();
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
