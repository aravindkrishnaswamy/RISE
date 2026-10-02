//////////////////////////////////////////////////////////////////////
//
//  NegativeReflectanceClampTest.cpp - DL-386 (docs/DEBT_LEDGER.md,
//    ruled 2026-10-02): a reflectance colour authored outside the
//    Rec.709 working gamut (e.g. `colorspace ROMMRGB_Linear`, converted
//    by matrix only: Cornell red (0.57, 0.025, 0.025) becomes
//    (1.134, -0.100, 0.020)) carries NEGATIVE channels.  Every material
//    read of a reflectance-type slot now clamps those channels to 0
//    through IPainter.h's `ReflectanceColor` / `ReflectanceColorNM`;
//    channels above 1 stay as authored.
//
//    Deterministic probes (no rendering):
//      1. LambertianBRDF::value / albedo: the negative channel reads
//         exactly 0, the above-1 channel keeps its authored value.
//      2. LambertianSPF::Scatter: kray has no negative channel.
//      3. A two-lobe SPF (IsotropicPhongSPF) with an all-negative
//         diffuse colour: no emitted ray carries a negative kray, and
//         `RandomlySelect`'s selection probability stays in [0, 1] (a
//         negative `MaxValue(kray)` reverses its CDF -- cf. DL-100);
//         Pdf() is non-negative at every sampled direction.
//      4. GGXSPF with a negative diffuse: emitted kray and Pdf
//         non-negative (its selection weight is MaxValue(diffuse)).
//      5. RGB / spectral agreement: the Jakob-Hanika albedo uplift
//         already clamps a negative input, so GetColorNM of the signed
//         colour equals GetColorNM of its clamped twin bit-for-bit, and
//         LambertianBRDF::valueNM agrees with the clamped RGB value's
//         behaviour (zero-green colour, same spectrum).
//      6. Spectrally authored negative data: a painter whose GetColorNM
//         is negative reads 0 through LambertianBRDF::valueNM.
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
#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Materials/IsotropicPhongSPF.h"
#include "../src/Library/Materials/GGXSPF.h"
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

	// Viewer arrives from above onto a +Z surface (cf. JHWhiteGuardSpectralTest).
	RayIntersectionGeometric MakeShadingRi( const Vector3& dir = Vector3( 0, 0, -1 ) )
	{
		const Ray r( Point3( 0, 0, 0 ), dir );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( r, rs );
		ri.bHit = true;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		return ri;
	}

	//! A spectrally AUTHORED painter whose GetColorNM dips below zero
	//! (a `spectral_painter` with a negative sample would do the same).
	class NegativeNMPainter : public UniformColorPainter
	{
	public:
		NegativeNMPainter() : UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) ) {}
		Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar ) const override { return Scalar( -0.25 ); }
	protected:
		virtual ~NegativeNMPainter() {}
	};

	bool AnyNegative( const RISEPel& c ) { return c[0] < 0 || c[1] < 0 || c[2] < 0; }
}

int main()
{
	std::cout << "NegativeReflectanceClampTest -- DL-386 negative reflectance channels read as 0\n";

	GlobalMediaPathLocator().AddPath( "." );
	GlobalMediaPathLocator().AddPath( "../" );
	GlobalMediaPathLocator().AddPath( "../../" );

	const RayIntersectionGeometric ri = MakeShadingRi();
	const Vector3 vLightIn( 0, 0, 1 );

	// The ROMM->Rec.709 Cornell red: one channel above 1, one below 0.
	const RISEPel signedRed( 1.134, -0.100, 0.020 );
	const RISEPel clampedRed( 1.134, 0.0, 0.020 );

	StubObject* stub = new StubObject();
	stub->addref();
	const IORStack iorStack = MakeTestIORStack( stub );

	// [1] LambertianBRDF value / albedo
	std::cout << "\n[1] LambertianBRDF value/albedo\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, signedRed );
		LambertianBRDF* brdf = new LambertianBRDF( *p );
		brdf->addref();
		const RISEPel v = brdf->value( vLightIn, ri );
		std::printf( "    value = (%.6f, %.6f, %.6f)\n", double(v[0]), double(v[1]), double(v[2]) );
		Check( v[1] == Scalar(0), "Lambertian value: negative green channel reads exactly 0" );
		Check( v[0] == Scalar(1.134) * INV_PI, "Lambertian value: red above 1 kept as authored (1.134/pi)" );
		Check( v[2] == Scalar(0.020) * INV_PI, "Lambertian value: positive blue unchanged" );
		const RISEPel alb = brdf->albedo( ri );
		Check( alb[1] == Scalar(0) && alb[0] == Scalar(1.134), "Lambertian albedo: (1.134, 0, .02)" );
		brdf->release();
		p->release();
	}

	// [2] LambertianSPF kray
	std::cout << "\n[2] LambertianSPF kray\n";
	{
		IPainter* p = nullptr;
		RISE_API_CreateUniformColorPainter( &p, signedRed );
		LambertianSPF* spf = new LambertianSPF( *p );
		spf->addref();
		RandomNumberGenerator rng;
		IndependentSampler sampler( rng );
		ScatteredRayContainer sc;
		spf->Scatter( ri, sampler, sc, iorStack );
		Check( sc.Count() == 1, "sanity: Lambertian emits one ray" );
		if( sc.Count() == 1 ) {
			const RISEPel k = sc[0].kray;
			std::printf( "    kray = (%.6f, %.6f, %.6f)\n", double(k[0]), double(k[1]), double(k[2]) );
			Check( k[1] == Scalar(0), "Lambertian kray: negative green reads exactly 0" );
			Check( k[0] == Scalar(1.134), "Lambertian kray: red above 1 kept" );
		}
		spf->release();
		p->release();
	}

	// [3] Two-lobe SPF with an all-negative diffuse colour.
	std::cout << "\n[3] IsotropicPhongSPF, diffuse (-0.4,-0.3,-0.2), specular 0.3 grey\n";
	{
		IPainter* rd = nullptr;
		IPainter* rs = nullptr;
		IScalarPainter* ex = nullptr;
		RISE_API_CreateUniformColorPainter( &rd, RISEPel( -0.4, -0.3, -0.2 ) );
		RISE_API_CreateUniformColorPainter( &rs, RISEPel( 0.3, 0.3, 0.3 ) );
		RISE_API_CreateUniformScalarPainter( &ex, Scalar( 20 ) );
		IsotropicPhongSPF* spf = new IsotropicPhongSPF( *rd, *rs, *ex );
		spf->addref();
		const RayIntersectionGeometric riTilt = MakeShadingRi( Vector3Ops::Normalize( Vector3( 0.4, 0, -1 ) ) );
		RandomNumberGenerator rng( 386 );
		IndependentSampler sampler( rng );
		int negKray = 0, badSel = 0, negPdf = 0, selected = 0, emitted = 0;
		for( int i = 0; i < 4000; ++i ) {
			ScatteredRayContainer sc;
			spf->Scatter( riTilt, sampler, sc, iorStack );
			for( unsigned int k = 0; k < sc.Count(); ++k ) {
				++emitted;
				if( AnyNegative( sc[k].kray ) ) ++negKray;
				const Scalar pdf = spf->Pdf( riTilt, sc[k].ray.Dir(), iorStack );
				if( pdf < 0 ) ++negPdf;
			}
			Scalar selP = -1;
			ScatteredRay* s = sc.RandomlySelect( rng.CanonicalRandom(), false, &selP );
			if( s ) {
				++selected;
				if( !( selP >= 0 && selP <= 1 ) || AnyNegative( s->kray ) ) ++badSel;
			}
		}
		std::printf( "    emitted %d, negative kray %d, negative Pdf %d; selected %d, bad selection %d\n",
			emitted, negKray, negPdf, selected, badSel );
		Check( emitted > 0 && selected > 0, "sanity: Phong emits and selects rays" );
		Check( negKray == 0, "Phong: no emitted ray carries a negative kray" );
		Check( badSel == 0, "Phong: RandomlySelect probability in [0,1] and selected kray non-negative" );
		Check( negPdf == 0, "Phong: Pdf non-negative at every emitted direction" );
		spf->release();
		rd->release(); rs->release(); ex->release();
	}

	// [4] GGXSPF with a negative diffuse (selection weight MaxValue(diffuse)).
	std::cout << "\n[4] GGXSPF, diffuse (-0.3,-0.2,-0.1), specular 0.5 grey, conductor Fresnel\n";
	{
		IPainter* dif = nullptr;
		IPainter* spec = nullptr;
		IScalarPainter* a = nullptr;
		IScalarPainter* ior = nullptr;
		IScalarPainter* ext = nullptr;
		RISE_API_CreateUniformColorPainter( &dif, RISEPel( -0.3, -0.2, -0.1 ) );
		RISE_API_CreateUniformColorPainter( &spec, RISEPel( 0.5, 0.5, 0.5 ) );
		RISE_API_CreateUniformScalarPainter( &a, Scalar( 0.3 ) );
		RISE_API_CreateUniformScalarPainter( &ior, Scalar( 1.5 ) );
		RISE_API_CreateUniformScalarPainter( &ext, Scalar( 0 ) );
		GGXSPF* spf = new GGXSPF( *dif, *spec, *a, *a, *ior, *ext, eFresnelConductor );
		spf->addref();
		RandomNumberGenerator rng( 3861 );
		IndependentSampler sampler( rng );
		int negKray = 0, negPdf = 0, emitted = 0;
		for( int i = 0; i < 4000; ++i ) {
			ScatteredRayContainer sc;
			spf->Scatter( ri, sampler, sc, iorStack );
			for( unsigned int k = 0; k < sc.Count(); ++k ) {
				++emitted;
				if( AnyNegative( sc[k].kray ) ) ++negKray;
				if( spf->Pdf( ri, sc[k].ray.Dir(), iorStack ) < 0 ) ++negPdf;
			}
		}
		std::printf( "    emitted %d, negative kray %d, negative Pdf %d\n", emitted, negKray, negPdf );
		Check( emitted > 0, "sanity: GGX emits rays" );
		Check( negKray == 0, "GGX: no emitted ray carries a negative kray" );
		Check( negPdf == 0, "GGX: Pdf non-negative at every emitted direction" );
		spf->release();
		dif->release(); spec->release(); a->release(); ior->release(); ext->release();
	}

	// [5] RGB / spectral agreement on a negative channel.
	std::cout << "\n[5] Spectral uplift of the signed colour == uplift of its clamped twin\n";
	{
		IPainter* ps = nullptr;
		IPainter* pc = nullptr;
		RISE_API_CreateUniformColorPainter( &ps, signedRed );
		RISE_API_CreateUniformColorPainter( &pc, clampedRed );
		LambertianBRDF* bs = new LambertianBRDF( *ps );
		LambertianBRDF* bc = new LambertianBRDF( *pc );
		bs->addref(); bc->addref();
		bool same = true;
		for( Scalar nm = 400; nm <= 700; nm += 25 ) {
			if( ps->GetColorNM( ri, nm ) != pc->GetColorNM( ri, nm ) ) same = false;
			if( bs->valueNM( vLightIn, ri, nm ) != bc->valueNM( vLightIn, ri, nm ) ) same = false;
		}
		Check( same, "signed and clamped colours uplift and evaluate identically on the NM pipe" );
		{ const RISEPel a = bs->value( vLightIn, ri ), b = bc->value( vLightIn, ri );
		  Check( a[0] == b[0] && a[1] == b[1] && a[2] == b[2],
		       "signed and clamped colours evaluate identically on the RGB pipe" ); }
		bs->release(); bc->release();
		ps->release(); pc->release();
	}

	// [6] Spectrally authored negative data.
	std::cout << "\n[6] Painter with negative GetColorNM\n";
	{
		NegativeNMPainter* p = new NegativeNMPainter();
		p->addref();
		LambertianBRDF* brdf = new LambertianBRDF( *p );
		brdf->addref();
		const Scalar v = brdf->valueNM( vLightIn, ri, Scalar( 550 ) );
		std::printf( "    valueNM(550) = %.6f\n", double(v) );
		Check( v == Scalar(0), "Lambertian valueNM of a negative spectral sample reads exactly 0" );
		brdf->release();
		p->release();
	}

	stub->release();

	std::cout << "\n" << s_pass << " passed, " << s_fail << " failed.\n";
	return s_fail == 0 ? 0 : 1;
}
