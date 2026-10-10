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
//  Ashikhmin-Shirley, polished and translucent (its entry side).  It also
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
#include "TestStubObject.h"

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
static RayIntersectionGeometric MakeIntersection( double theta )
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
};

//! One material at one incidence, one lane (RGB channel `ch`, or NM when
//! nm > 0).
static void RunCase( const Case& c, double thetaDeg, int ch, Scalar nm, ISampler& sampler )
{
	const RayIntersectionGeometric ri = MakeIntersection( thetaDeg * PI / 180.0 );
	const ISPF* spf = c.mat->GetSPF();
	const IBSDF* bsdf = c.mat->GetBSDF();
	const bool isNM = nm > 0;
	const unsigned int mask = c.mat->ConnectionScatterTypes();
	std::ostringstream tag;
	tag << c.name << " theta " << thetaDeg << ( isNM ? " NM " : " RGB ch " ) << ( isNM ? nm : ch );

	Check( c.mat->HasConnectionTypeSplit(), tag.str() + ": declares a split" );

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

int main()
{
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
	for( Case& c : cases ) c.mat->addref();

	const double thetas[] = { 0.0, 40.0, 70.0 };
	for( const Case& c : cases ) {
		for( double th : thetas ) {
			RunCase( c, th, 0, 0, sampler );
			RunCase( c, th, 2, 0, sampler );
			RunCase( c, th, 0, 550.0, sampler );
		}
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
	g_stubObject->release();

	std::cout << std::endl << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
