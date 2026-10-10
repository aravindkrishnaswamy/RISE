//////////////////////////////////////////////////////////////////////
//
//  ConnectionTypeSplitTest.cpp - DL-481: the per-lobe-label split of a
//    material's connection value (IBSDF::valueByScatterType{,NM}) means
//    what the per-type bounce caps need it to mean.
//
//  Under a per-type bounce cap that can bind, BDPT / VCM / MLT price a
//  counted connection or merge endpoint whose material has several
//  possible non-delta lobe types by the sum over its ALLOWED types of
//  `out[t]` (BDPTUtilities::AllowedTypeProduct).  That is unbiased only
//  if, for every direction function g,
//
//      E[ w * [type == t] * g(wo) ]  ==  integral of out[t](wo) |cos| g(wo)
//
//  where the left side runs the material's own SPF exactly as the
//  bidirectional walks do (Scatter, then RandomlySelect; the walk's
//  weight is kray / selectProb and its counted label is the selected
//  ray's `type`).  This test checks exactly that, per type, per channel,
//  for three direction functions, at three incidences, RGB and NM, for
//  every material that declares the split
//  (IMaterial::HasConnectionTypeSplit): GGX (Schlick and conductor
//  Fresnel), Cook-Torrance, Schlick, both Ward models, isotropic Phong,
//  Ashikhmin-Shirley, polished, translucent (its entry side) and hair.
//  --dl502-weave-pin retains the unresolved weave declaration checks.  It also
//  checks that the parts sum to `valueStateful` at random directions.
//
//  Left: Monte-Carlo mean over the SPF's draws, with its standard error.
//  Right: midpoint quadrature over the whole sphere.  The gate is
//  |MC - Q| <= 5 se + 1.5 % of the type's total (quadrature of the glossy
//  peaks at the grid used is good to well under that).
//
//  Author: Claude (Opus 4.8)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <string>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/CookTorranceMaterial.h"
#include "../src/Library/Materials/SchlickMaterial.h"
#include "../src/Library/Materials/WardIsotropicGaussianMaterial.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianMaterial.h"
#include "../src/Library/Materials/IsotropicPhongMaterial.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongMaterial.h"
#include "../src/Library/Materials/PolishedMaterial.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "TestStubObject.h"
#include "WeaveTestFixture.h"
#include "../src/Library/Materials/HairMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static StubObject* g_stubObject = 0;

//! Flat surface at the origin, normal +Z, incoming ray in the X-Z plane at
//! `theta` from the normal (travelling toward -Z, from the +X side).
static RayIntersectionGeometric MakeIntersection( double theta, double glossyFilterWidth = 0 )
{
	const double sinT = sin( theta );
	const double cosT = cos( theta );
	const Vector3 inDir( -sinT, 0, -cosT );
	Ray inRay( Point3( sinT, 0, cosT ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	ri.glossyFilterWidth = glossyFilterWidth;
	return ri;
}

static const int kNumG = 3;
//! The direction functions: 1, cos^2 (shape about the normal), and the
//! half-space on the mirror side (x < 0 for an incidence from +X).
static double G( int k, const Vector3& w )
{
	switch( k ) {
	case 0: return 1.0;
	case 1: return w.z * w.z;
	default: return w.x < 0 ? 1.0 : 0.0;
	}
}

struct Case
{
	std::string name;
	IMaterial* mat;
	const IORStack* stack;
	double glossyFilterWidth = 0;	//!< ri.glossyFilterWidth of the hit (DL-62 / DL-65 widening of the lobe roughness)
};

//! One material at one incidence, one lane (RGB channel `ch`, or NM when
//! nm > 0).
static void RunCase( const Case& c, double thetaDeg, int ch, Scalar nm, ISampler& sampler )
{
	const RayIntersectionGeometric ri = MakeIntersection( thetaDeg * PI / 180.0, c.glossyFilterWidth );
	const ISPF* spf = c.mat->GetSPF();
	const IBSDF* bsdf = c.mat->GetBSDF();
	const bool isNM = nm > 0;
	const unsigned int mask = c.mat->ConnectionScatterTypes();
	std::ostringstream tag;
	tag << c.name << ( c.glossyFilterWidth > 0 ? " (gfw)" : "" ) << " theta " << thetaDeg << ( isNM ? " NM " : " RGB ch " ) << ( isNM ? nm : ch );

	Check( c.mat->HasConnectionTypeSplit(), tag.str() + ": declares a split" );
	if( !c.mat->HasConnectionTypeSplit() ) return; // Declined splits leave out[] unspecified.

	// (1) the parts sum to valueStateful, at directions over the whole sphere.
	{
		double worst = 0;
		for( int i = 0; i < 64; i++ ) {
			for( int j = 0; j < 32; j++ ) {
				const double th = ( j + 0.5 ) * PI / 32.0;
				const double ph = ( i + 0.5 ) * TWO_PI / 64.0;
				const Vector3 w( sin( th ) * cos( ph ), sin( th ) * sin( ph ), cos( th ) );
				double total, sum = 0;
				if( isNM ) {
					Scalar o[5];
					Check( bsdf->valueByScatterTypeNM( w, ri, nm, c.stack, o ), tag.str() + ": NM split available" );
					for( int t = 0; t < 5; t++ ) sum += o[t];
					total = bsdf->valueStatefulNM( w, ri, nm, c.stack );
				} else {
					RISEPel o[5];
					Check( bsdf->valueByScatterType( w, ri, c.stack, o ), tag.str() + ": RGB split available" );
					for( int t = 0; t < 5; t++ ) sum += o[t][ch];
					total = bsdf->valueStateful( w, ri, c.stack )[ch];
				}
				const double err = fabs( sum - total ) / ( fabs( total ) + 1e-12 );
				if( err > worst ) worst = err;
			}
		}
		Check( worst < 1e-9, tag.str() + ": parts sum to valueStateful" );
	}

	// (2) quadrature of out[t] |cos| g over the sphere.
	double Q[5][kNumG] = {};
	{
		const int nT = 360, nP = 720;
		const double dT = PI / nT, dP = TWO_PI / nP;
		for( int a = 0; a < nT; a++ ) {
			const double th = ( a + 0.5 ) * dT;
			const double st = sin( th ), ct = cos( th );
			for( int b = 0; b < nP; b++ ) {
				const double ph = ( b + 0.5 ) * dP;
				const Vector3 w( st * cos( ph ), st * sin( ph ), ct );
				double o[5];
				if( isNM ) {
					Scalar v[5];
					bsdf->valueByScatterTypeNM( w, ri, nm, c.stack, v );
					for( int t = 0; t < 5; t++ ) o[t] = v[t];
				} else {
					RISEPel v[5];
					bsdf->valueByScatterType( w, ri, c.stack, v );
					for( int t = 0; t < 5; t++ ) o[t] = v[t][ch];
				}
				const double dw = st * dT * dP * fabs( ct );
				for( int t = 1; t < 5; t++ ) {
					if( o[t] == 0 ) continue;
					for( int k = 0; k < kNumG; k++ ) Q[t][k] += o[t] * dw * G( k, w );
				}
			}
		}
	}

	// (3) the walk's labelled estimator.
	const int N = 400000;
	double S[5][kNumG] = {}, S2[5][kNumG] = {};
	int strayType = 0;
	for( int i = 0; i < N; i++ ) {
		ScatteredRayContainer sc;
		if( isNM ) spf->ScatterNM( ri, sampler, nm, sc, *c.stack ); else spf->Scatter( ri, sampler, sc, *c.stack );
		Scalar prob = 0;
		const ScatteredRay* r = sc.RandomlySelect( sampler.Get1D(), isNM, &prob );
		if( !r || r->isDelta || prob <= 0 ) continue;
		const double w = ( isNM ? r->krayNM : r->kray[ch] ) / prob;
		const unsigned int t = r->type;
		if( t < 1 || t > 4 || !( mask & ( 1u << t ) ) ) { strayType++; continue; }
		const Vector3 wo = Vector3Ops::Normalize( r->ray.Dir() );
		for( int k = 0; k < kNumG; k++ ) {
			const double v = w * G( k, wo );
			S[t][k] += v;
			S2[t][k] += v * v;
		}
	}
	Check( strayType == 0, tag.str() + ": every non-delta ray carries a type in ConnectionScatterTypes" );

	for( int t = 1; t < 5; t++ ) {
		if( !( mask & ( 1u << t ) ) ) continue;
		for( int k = 0; k < kNumG; k++ ) {
			const double mean = S[t][k] / N;
			const double var = S2[t][k] / N - mean * mean;
			const double se = sqrt( var > 0 ? var / N : 0 );
			const double scale = Q[t][0] > 1e-6 ? Q[t][0] : 1e-6;
			const bool ok = fabs( mean - Q[t][k] ) <= 5.0 * se + 0.015 * scale;
			if( !ok || k == 0 ) {
				std::cout << "    " << std::left << std::setw( 44 ) << tag.str() << " type " << t << " g" << k
					<< std::right << std::setprecision( 6 ) << std::fixed
					<< "  MC " << mean << " (se " << se << ")  Q " << Q[t][k] << std::endl;
			}
			std::ostringstream l;
			l << tag.str() << ": type " << t << " g" << k << " walk expectation == quadrature of out[t]";
			Check( ok, l.str() );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Direct identity check for the GGX / Cook-Torrance split.
//
// Their diffuse part is computed as `value() - single-scatter`, so "the
// parts sum to valueStateful" is automatic and says nothing about the
// copied single-scatter helpers (SingleScatterRGB/NM).  A drift in a copy
// would only show up in the loose Monte-Carlo band above.  The multiscatter
// energy compensation cannot be switched off (its LUT term is always
// added), but with a BLACK diffuse painter value() is exactly
//
//      value() = single-scatter + multiscatter(spec)
//
// so the multiscatter term can be rebuilt here from the public LUT
// helpers -- independently of the helpers under test -- and
//
//      out[eRayReflection] == value() - multiscatter     (~1e-12)
//      out[eRayDiffuse]    == multiscatter               (~1e-12)
//
// must hold at any direction pair.  Isotropic and anisotropic GGX, both
// Fresnel modes that have a closed-form F_avg (Schlick-F0, conductor), and
// Cook-Torrance, on RGB (all channels) and NM, at several incidences.
//////////////////////////////////////////////////////////////////////
enum IdKind { kIdGGXSchlick, kIdGGXConductor, kIdCookTorrance };

static void RunIdentity( const std::string& name, IMaterial* mat, IdKind kind, double alphaX, double alphaY,
	double iorV, double extV, const IPainter& specPainter, const IPainter& diffPainter,
	const IScalarPainter& iorP, const IScalarPainter& extP, FresnelMode fmode, const IORStack* stack )
{
	const IBSDF* bsdf = mat->GetBSDF();
	const double thetas[] = { 0.0, 25.0, 55.0, 75.0 };
	const double wlens[] = { 0.0, 450.0, 550.0, 650.0 };	// 0 = RGB
	double worstRefl = 0, worstDiff = 0, maxMs = 0;
	std::ostringstream worstDesc;
	int compared = 0;
	for( double thd : thetas ) {
		const RayIntersectionGeometric ri = MakeIntersection( thd * PI / 180.0 );
		const Vector3 nrm( 0, 0, 1 );
		const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
		for( double nm : wlens ) {
			const bool isNM = nm > 0;
			for( int a = 0; a < 6; a++ ) {
				for( int b = 0; b < 8; b++ ) {
					const double th = ( a + 0.5 ) * ( PI / 2 ) / 6.0 * 0.98;
					const double ph = ( b + 0.37 ) * TWO_PI / 8.0;
					const Vector3 wi( sin( th ) * cos( ph ), sin( th ) * sin( ph ), cos( th ) );
					const double nr = Vector3Ops::Dot( nrm, wo ), nv = wi.z;
					if( nv < 1e-3 ) continue;
					const Vector3 wiL( Vector3Ops::Dot( wi, ri.onb.u() ), Vector3Ops::Dot( wi, ri.onb.v() ), nv );
					const Vector3 woL( Vector3Ops::Dot( wo, ri.onb.u() ), Vector3Ops::Dot( wo, ri.onb.v() ), nr );

					// Independent multiscatter reconstruction (ch 0..2 for RGB, [0] for NM).
					double ms[3] = { 0, 0, 0 };
					double Eavg, f_ms;
					if( kind == kIdCookTorrance ) {
						Eavg = MicrofacetEnergyLUT::LookupEavg( alphaX );
						f_ms = ( 1.0 - MicrofacetEnergyLUT::LookupEss( nr, alphaX ) ) * ( 1.0 - MicrofacetEnergyLUT::LookupEss( nv, alphaX ) )
							/ ( PI * ( 1.0 - Eavg ) );
					} else {
						Eavg = MicrofacetEnergyLUT::LookupEavgG2Aniso( alphaX, alphaY );
						f_ms = ( 1.0 - MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( nr, woL.x, woL.y, alphaX, alphaY ) )
							* ( 1.0 - MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( nv, wiL.x, wiL.y, alphaX, alphaY ) )
							/ ( PI * ( 1.0 - Eavg ) );
					}
					if( isNM ) {
						const Scalar spec = ReflectanceColorNM( specPainter, ri, nm );	// the same read the BRDF makes (a grey RGB uplifts to a slightly wavelength-dependent albedo)
						Scalar Favg;
						if( kind == kIdGGXSchlick ) Favg = spec + ( 1.0 - spec ) / 21.0;
						else Favg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( nrm, 1.0, iorV, extV );
						ms[0] = MicrofacetEnergyLUT::ComputeFms<Scalar>( kind == kIdGGXSchlick ? Favg : spec * Favg, Eavg ) * f_ms;
					} else {
						const RISEPel specPel = ReflectanceColor( specPainter, ri );
						for( int ch = 0; ch < 3; ch++ ) {
							RISEPel F;
							if( kind == kIdGGXSchlick ) {
								F = specPel + ( RISEPel( 1, 1, 1 ) - specPel ) * ( 1.0 / 21.0 );
							} else {
								F = specPel * MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( nrm, RISEPel( 1, 1, 1 ), RISEPel( iorV, iorV, iorV ), RISEPel( extV, extV, extV ) );
							}
							ms[ch] = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F, Eavg )[ch] * f_ms;
						}
					}
					const int nch = isNM ? 1 : 3;
					double difRGB[3] = { 0, 0, 0 }, difNM = 0;
					{
						const GGXInterfaceFresnel gf { ri, fmode, specPainter, iorP, extP, nullptr, nullptr, nullptr };
						if( isNM ) {
							const Scalar T = kind == kIdCookTorrance ? Scalar( 1 ) : GGXInterfaceFresnel::Transmission( gf.DirectionalNM( nv, nm ), gf.DirectionalNM( nr, nm ) );
							difNM = ReflectanceColorNM( diffPainter, ri, nm ) * INV_PI * T;
						} else {
							const RISEPel T = kind == kIdCookTorrance ? RISEPel( 1, 1, 1 ) : GGXInterfaceFresnel::Transmission( gf.Directional( nv ), gf.Directional( nr ) );
							const RISEPel d = ReflectanceColor( diffPainter, ri ) * INV_PI * T;
							for( int ch = 0; ch < 3; ch++ ) difRGB[ch] = d[ch];
						}
					}
					RISEPel oP[5]; Scalar oS[5]; RISEPel tP( 0, 0, 0 ); Scalar tS = 0;
					if( isNM ) { bsdf->valueByScatterTypeNM( wi, ri, nm, stack, oS ); tS = bsdf->valueNM( wi, ri, nm ); }
					else { bsdf->valueByScatterType( wi, ri, stack, oP ); tP = bsdf->value( wi, ri ); }
					for( int ch = 0; ch < nch; ch++ ) {
						const double total = isNM ? tS : tP[ch];
						const double refl = isNM ? oS[ScatteredRay::eRayReflection] : oP[ScatteredRay::eRayReflection][ch];
						const double diff = isNM ? oS[ScatteredRay::eRayDiffuse] : oP[ScatteredRay::eRayDiffuse][ch];
						const double tol = 1e-12 * fabs( total ) + 1e-15;
						// Diffuse term the black painter still carries (exactly 0 in RGB; the NM albedo uplift of black is ~1e-5).
						const double dif = isNM ? difNM : difRGB[ch];
						const double eR = fabs( refl - ( total - ms[ch] - dif ) ) / tol;
						const double eD = fabs( diff - ( ms[ch] + dif ) ) / tol;
						if( eR > worstRefl ) { worstRefl = eR; worstDesc.str(""); worstDesc << "th " << thd << " nm " << nm << " ch " << ch << " wi.z " << nv << " ph " << ph << " refl " << refl << " total " << total << " ms " << ms[ch] << " diff " << diff; }
						if( eD > worstDiff ) worstDiff = eD;
						if( ms[ch] > maxMs ) maxMs = ms[ch];
						compared++;
					}
				}
			}
		}
	}
	std::cout << "    identity " << std::left << std::setw( 24 ) << name << std::right << std::setprecision( 3 ) << std::scientific
		<< " compared " << compared << "  worst |refl-(value-ms)|/tol " << worstRefl << "  worst |diff-ms|/tol " << worstDiff
		<< "  max ms " << maxMs << std::fixed << std::endl;
	std::cout << "      worst at: " << worstDesc.str() << std::endl;
	Check( compared > 1000, name + ": identity check compared many (wi, wo, lane) triples" );
	Check( maxMs > 2e-5, name + ": multiscatter term is non-trivial (the identity is not vacuous)" );
	Check( worstRefl <= 1.0, name + ": reflection part == value() - independently rebuilt multiscatter (1e-12 rel)" );
	Check( worstDiff <= 1.0, name + ": diffuse part == independently rebuilt multiscatter (1e-12 rel)" );
}

int main(int argc, char** argv)
{
	const bool dl502Only = argc == 2 && std::string(argv[1]) == "--dl502-only";
	const bool weavePin = argc == 2 && std::string(argv[1]) == "--dl502-weave-pin";
	GlobalLog();
	std::cout << "=== ConnectionTypeSplitTest (DL-481) ===" << std::endl;

	g_stubObject = new StubObject();
	g_stubObject->addref();
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.7, 0.5, 0.3 ) ); rd->addref();
	UniformColorPainter* rs = new UniformColorPainter( RISEPel( 0.2, 0.25, 0.3 ) ); rs->addref();
	UniformColorPainter* tau = new UniformColorPainter( RISEPel( 0.6, 0.5, 0.4 ) ); tau->addref();
	UniformColorPainter* rsGrey = new UniformColorPainter( RISEPel( 0.25, 0.25, 0.25 ) ); rsGrey->addref();
	UniformScalarPainter* a03 = new UniformScalarPainter( 0.3 ); a03->addref();
	UniformScalarPainter* a02 = new UniformScalarPainter( 0.2 ); a02->addref();
	UniformScalarPainter* a01 = new UniformScalarPainter( 0.1 ); a01->addref();
	UniformScalarPainter* iso1 = new UniformScalarPainter( 1.0 ); iso1->addref();
	UniformScalarPainter* ior15 = new UniformScalarPainter( 1.5 ); ior15->addref();
	UniformScalarPainter* iorAu = new UniformScalarPainter( 0.47 ); iorAu->addref();
	UniformScalarPainter* extAu = new UniformScalarPainter( 2.4 ); extAu->addref();
	UniformScalarPainter* ext0 = new UniformScalarPainter( 0.0 ); ext0->addref();
	UniformScalarPainter* n20 = new UniformScalarPainter( 20.0 ); n20->addref();
	UniformScalarPainter* nu = new UniformScalarPainter( 20.0 ); nu->addref();
	UniformScalarPainter* nv = new UniformScalarPainter( 60.0 ); nv->addref();
	UniformScalarPainter* tau09 = new UniformScalarPainter( 0.9 ); tau09->addref();
	UniformScalarPainter* scat30 = new UniformScalarPainter( 30.0 ); scat30->addref();
	UniformScalarPainter* trExt = new UniformScalarPainter( 0.1 ); trExt->addref();
	UniformScalarPainter* trN = new UniformScalarPainter( 10.0 ); trN->addref();
	UniformScalarPainter* trScat = new UniformScalarPainter( 0.4 ); trScat->addref();

	std::vector<Case> cases;
	cases.push_back( { "ggx schlick_f0", new GGXMaterial( *rd, *rs, *a03, *a03, *ior15, *ext0, eFresnelSchlickF0 ), &iorStack } );
	cases.push_back( { "ggx conductor", new GGXMaterial( *rd, *rs, *a03, *a02, *iorAu, *extAu ), &iorStack } );
	cases.push_back( { "cooktorrance", new CookTorranceMaterial( *rd, *rs, *a03, *ior15, *ext0 ), &iorStack } );
	cases.push_back( { "schlick", new SchlickMaterial( *rd, *rs, *a02, *iso1 ), &iorStack } );
	cases.push_back( { "ward isotropic", new WardIsotropicGaussianMaterial( *rd, *rs, *a02 ), &iorStack } );
	cases.push_back( { "ward anisotropic", new WardAnisotropicEllipticalGaussianMaterial( *rd, *rs, *a01, *a03 ), &iorStack } );
	cases.push_back( { "isotropic phong", new IsotropicPhongMaterial( *rd, *rs, *n20 ), &iorStack } );
	// GREY rs: AshikminShirleyAnisotropicPhongSPF's RGB specular lobe uses
	// MaxValue(Rs) for its Fresnel and an achromatic kray, while its BSDF
	// uses Rs per channel -- a pre-existing SPF/BSDF mismatch for a
	// chromatic Rs, independent of the type split (DL-501).
	cases.push_back( { "ashikhmin-shirley", new AshikminShirleyAnisotropicPhongMaterial( *nu, *nv, *rd, *rsGrey ), &iorStack } );
	cases.push_back( { "polished", new PolishedMaterial( *rd, *tau09, *ior15, *scat30, false ), &iorStack } );
	cases.push_back( { "translucent (entry)", new TranslucentMaterial( *rd, *tau, *trExt, *trN, *trScat ), &iorStack } );
	// Reviewer probe cases (DL-481 review): thin-film Fresnel, the DL-310 diffuse
	// clip where it binds (Schlick, Ward), the translucent EXIT side (the IOR
	// stack holds the object), and a nonzero glossy filter width.
	UniformScalarPainter* fIor = new UniformScalarPainter( 1.4 ); fIor->addref();
	UniformScalarPainter* fTh = new UniformScalarPainter( 300.0 ); fTh->addref();
	UniformScalarPainter* r005 = new UniformScalarPainter( 0.05 ); r005->addref();
	UniformScalarPainter* iso04 = new UniformScalarPainter( 0.4 ); iso04->addref();
	UniformColorPainter* rdHi = new UniformColorPainter( RISEPel( 0.9, 0.8, 0.95 ) ); rdHi->addref();
	UniformColorPainter* rsHi = new UniformColorPainter( RISEPel( 0.5, 0.4, 0.45 ) ); rsHi->addref();
	IORStack exitStack( 1.0 );
	exitStack.SetCurrentObject( g_stubObject );
	exitStack.push( 1.3 );
	cases.push_back( { "ggx thinfilm", new GGXMaterial( *rd, *rs, *a03, *a02, *iorAu, *extAu, eFresnelThinFilmConductor, nullptr, fIor, ext0, fTh ), &iorStack } );
	cases.push_back( { "schlick clip binds", new SchlickMaterial( *rdHi, *rsHi, *r005, *iso04 ), &iorStack } );
	cases.push_back( { "ward iso clip binds", new WardIsotropicGaussianMaterial( *rdHi, *rsHi, *a02 ), &iorStack } );
	cases.push_back( { "translucent (exit)", new TranslucentMaterial( *rd, *tau, *trExt, *trN, *trScat ), &exitStack } );
	cases.push_back( { "ggx schlick_f0", new GGXMaterial( *rd, *rs, *a03, *a03, *ior15, *ext0, eFresnelSchlickF0 ), &iorStack, 0.15 } );
	cases.push_back( { "ggx conductor", new GGXMaterial( *rd, *rs, *a03, *a02, *iorAu, *extAu ), &iorStack, 0.15 } );
	cases.push_back( { "schlick", new SchlickMaterial( *rd, *rs, *a02, *iso1 ), &iorStack, 0.15 } );
	cases.push_back( { "cooktorrance", new CookTorranceMaterial( *rd, *rs, *a03, *ior15, *ext0 ), &iorStack, 0.15 } );
	WeaveTest::PresetWeave weave("linen", 0.31, 0.37);
	WeaveTest::PresetWeave thin("linen", 0.31, 0.37, false, true, 0.3, 0.5, 0.2);
	// Strict residual: these splits remain unavailable; opt in to the red pin.
	if(weavePin) {
		cases.push_back({"DL502 weave", weave.Material(), &iorStack});
		cases.push_back({"DL502 thin weave", thin.Material(), &iorStack});
	}
	HairPainters hp; hp.sigma_a=trExt; hp.beta_m=a03; hp.beta_n=a03; hp.alpha=iso1; hp.ior=ior15;
	cases.push_back({"DL502 hair",new HairMaterial(hp),&iorStack});
	for( Case& c : cases ) c.mat->addref();

	const double thetas[] = { 0.0, 40.0, 70.0 };
	for( const Case& c : cases ) {
		if(dl502Only && c.name.find("DL502") != 0) continue;
		if(weavePin && c.name.find("DL502") != 0) continue;
		for( double th : thetas ) {
			if( c.glossyFilterWidth > 0 && th != 40.0 ) continue;	// filter-width rows: one incidence
			RunCase( c, th, 0, 0, sampler );
			RunCase( c, th, 2, 0, sampler );
			RunCase( c, th, 0, 550.0, sampler );
		}
	}

	// Direct single-scatter identity (black diffuse; see RunIdentity).
	if(!dl502Only && !weavePin) {
		UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) ); black->addref();
		UniformColorPainter* rsG = new UniformColorPainter( RISEPel( 0.3, 0.3, 0.3 ) ); rsG->addref();
		struct IdCase { std::string n; IMaterial* m; IdKind k; double ax, ay, ior, ext; const IScalarPainter* iorP; const IScalarPainter* extP; FresnelMode fm; };
		std::vector<IdCase> ids;
		ids.push_back( { "ggx schlick iso", new GGXMaterial( *black, *rsG, *a03, *a03, *ior15, *ext0, eFresnelSchlickF0 ), kIdGGXSchlick, 0.3, 0.3, 1.5, 0.0, ior15, ext0, eFresnelSchlickF0 } );
		ids.push_back( { "ggx schlick aniso", new GGXMaterial( *black, *rsG, *a03, *a02, *ior15, *ext0, eFresnelSchlickF0 ), kIdGGXSchlick, 0.3, 0.2, 1.5, 0.0, ior15, ext0, eFresnelSchlickF0 } );
		ids.push_back( { "ggx conductor aniso", new GGXMaterial( *black, *rsG, *a03, *a02, *iorAu, *extAu ), kIdGGXConductor, 0.3, 0.2, 0.47, 2.4, iorAu, extAu, eFresnelConductor } );
		ids.push_back( { "ggx conductor iso", new GGXMaterial( *black, *rsG, *a02, *a02, *iorAu, *extAu ), kIdGGXConductor, 0.2, 0.2, 0.47, 2.4, iorAu, extAu, eFresnelConductor } );
		ids.push_back( { "cooktorrance", new CookTorranceMaterial( *black, *rsG, *a03, *ior15, *ext0 ), kIdCookTorrance, 0.3, 0.3, 1.5, 0.0, ior15, ext0, eFresnelConductor } );
		ids.push_back( { "cooktorrance rough", new CookTorranceMaterial( *black, *rsG, *a01, *iorAu, *extAu ), kIdCookTorrance, 0.1, 0.1, 0.47, 2.4, iorAu, extAu, eFresnelConductor } );
		for( IdCase& ic : ids ) {
			ic.m->addref();
			RunIdentity( ic.n, ic.m, ic.k, ic.ax, ic.ay, ic.ior, ic.ext, *rsG, *black, *ic.iorP, *ic.extP, ic.fm, &iorStack );
			ic.m->release();
		}
		black->release(); rsG->release();
	}

	// A single-type material declares no split (its endpoint type is
	// defined without one).
	{
		LambertianMaterial* lam = new LambertianMaterial( *rd ); lam->addref();
		Check( !lam->HasConnectionTypeSplit(), "lambertian: no split needed (single type)" );
		lam->release();
	}

	for( Case& c : cases ) c.mat->release();
	rd->release(); rs->release(); tau->release(); rsGrey->release();
	a03->release(); a02->release(); a01->release(); iso1->release(); ior15->release(); iorAu->release(); extAu->release(); ext0->release();
	n20->release(); nu->release(); nv->release(); tau09->release(); scat30->release(); trExt->release(); trN->release(); trScat->release();
	fIor->release(); fTh->release(); r005->release(); iso04->release(); rdHi->release(); rsHi->release();
	g_stubObject->release();

	std::cout << std::endl << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
