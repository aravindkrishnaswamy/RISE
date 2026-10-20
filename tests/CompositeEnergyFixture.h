//////////////////////////////////////////////////////////////////////
//
//  CompositeEnergyFixture.h - Shared material/furnace fixtures for
//    CompositeEnergyConservationTest and Coated* accuracy/probe tests.
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_TESTS_COMPOSITE_ENERGY_FIXTURE_H
#define RISE_TESTS_COMPOSITE_ENERGY_FIXTURE_H

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#if defined(_WIN32)
	#include <process.h>
	#ifndef getpid
		#define getpid _getpid
	#endif
#else
	#include <unistd.h>
#endif

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/DielectricMaterial.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/OrenNayarMaterial.h"
#include "../src/Library/Materials/PolishedMaterial.h"
#include "../src/Library/Materials/CompositeMaterial.h"
#include "../src/Library/Materials/CompositeSPF.h"
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/Materials/CoatedLayer.h"
#include "../src/Library/Materials/GenericHumanTissueMaterial.h"

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/ILog.h"

#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "TestStubObject.h"
#include "WeaveTestFixture.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static inline void Check( bool condition, const std::string& name )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

static StubObject* g_stub = 0;

static const double kPi = 3.14159265358979323846;

//////////////////////////////////////////////////////////////////////
//  Fixture: a flat +Z surface at the origin, viewed from direction
//  theta in the x-z plane (same construction as LayeredWhiteFurnaceTest).
//////////////////////////////////////////////////////////////////////
static inline RayIntersectionGeometric MakeIntersection( double thetaRad )
{
	const double s = std::sin( thetaRad );
	const double c = std::cos( thetaRad );
	const Vector3 inDir( s, 0, -c );
	const Ray inRay( Point3( s, 0, 1.0 ), inDir );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0 / c;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}

//////////////////////////////////////////////////////////////////////
//  Painters / materials shared by every section.
//////////////////////////////////////////////////////////////////////
struct Fixtures
{
	UniformColorPainter*  white;
	UniformColorPainter*  red;
	UniformColorPainter*  transRef;
	UniformColorPainter*  transTau;
	UniformScalarPainter* s0;
	UniformScalarPainter* s1;
	UniformScalarPainter* s133;
	UniformScalarPainter* s15;
	UniformScalarPainter* sScatDefault;		// dielectric_material's parser default
	UniformScalarPainter* sCoatRough;
	UniformScalarPainter* sN10;
	UniformScalarPainter* sHalf;
	UniformScalarPainter* sAlpha;
	UniformScalarPainter* sDielF0;

	LambertianMaterial*   lamb;
	LambertianMaterial*   lambRed;
	DielectricMaterial*   dScat0;			// config 3's top: scattering 0 (widest transmission warp)
	DielectricMaterial*   dSmooth;			// scattering 10000 (the parser default) -- the clearcoat case
	DielectricMaterial*   d133;				// water
	TranslucentMaterial*  trans;
	TranslucentMaterial*  transLossless;	// ext 0, white ref/tau
	GGXMaterial*          clearcoat;		// reflection-only GGX top (LayeredWhiteFurnaceTest config 7's)
	GGXMaterial*          redGgx;
	PolishedMaterial*     polishedWhite;	// delta coat + diffuse: the "tail class" bottom
	CoatedMaterial*       coatedControl;
};

static inline Fixtures MakeFixtures()
{
	Fixtures f;
	f.white        = new UniformColorPainter( RISEPel( 1, 1, 1 ) );        f.white->addref();
	f.red          = new UniformColorPainter( RISEPel( 0.8, 0.2, 0.2 ) );  f.red->addref();
	f.transRef     = new UniformColorPainter( RISEPel( 0.3, 0.3, 0.3 ) );  f.transRef->addref();
	f.transTau     = new UniformColorPainter( RISEPel( 0.7, 0.7, 0.7 ) );  f.transTau->addref();
	f.s0           = new UniformScalarPainter( 0.0 );    f.s0->addref();
	f.s1           = new UniformScalarPainter( 1.0 );    f.s1->addref();
	f.s133         = new UniformScalarPainter( 1.33 );   f.s133->addref();
	f.s15          = new UniformScalarPainter( 1.5 );    f.s15->addref();
	f.sScatDefault = new UniformScalarPainter( 10000.0 ); f.sScatDefault->addref();
	f.sCoatRough   = new UniformScalarPainter( 0.001 );  f.sCoatRough->addref();
	f.sN10         = new UniformScalarPainter( 10.0 );   f.sN10->addref();
	f.sHalf        = new UniformScalarPainter( 0.5 );    f.sHalf->addref();
	f.sAlpha       = new UniformScalarPainter( 0.16 );   f.sAlpha->addref();
	f.sDielF0      = new UniformScalarPainter( 0.04 );   f.sDielF0->addref();

	f.lamb    = new LambertianMaterial( *f.white );  f.lamb->addref();
	f.lambRed = new LambertianMaterial( *f.red );    f.lambRed->addref();
	f.dScat0  = new DielectricMaterial( *f.s1, *f.s15, *f.s0, false );           f.dScat0->addref();
	f.dSmooth = new DielectricMaterial( *f.s1, *f.s15, *f.sScatDefault, false ); f.dSmooth->addref();
	f.d133    = new DielectricMaterial( *f.s1, *f.s133, *f.sScatDefault, false );f.d133->addref();
	f.trans   = new TranslucentMaterial( *f.transRef, *f.transTau, *f.sHalf, *f.sN10, *f.sHalf ); f.trans->addref();
	f.transLossless = new TranslucentMaterial( *f.transRef, *f.transTau, *f.s0, *f.sN10, *f.s0 );  f.transLossless->addref();
	f.clearcoat = new GGXMaterial( *new UniformColorPainter( RISEPel( 0, 0, 0 ) ), *new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) ),
		*f.sAlpha, *f.sAlpha, *f.s15, *f.s0, eFresnelSchlickF0 );
	f.clearcoat->addref();
	f.redGgx = new GGXMaterial( *f.red, *new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) ),
		*f.sAlpha, *f.sAlpha, *f.s15, *f.s0, eFresnelSchlickF0 );
	f.redGgx->addref();
	f.polishedWhite = new PolishedMaterial( *f.white, *f.s1, *f.s15, *new UniformScalarPainter( 1000000.0 ), false );
	f.polishedWhite->addref();
	f.coatedControl = new CoatedMaterial( *f.lamb, *f.s1, *f.s15, *f.sCoatRough, *f.s0, *f.s0, *f.white );
	f.coatedControl->addref();
	return f;
}

static inline CompositeMaterial* MakeComposite(
	const IMaterial& top, const IMaterial& bottom,
	unsigned r, unsigned a, unsigned b, unsigned c, unsigned d,
	double thickness, const IScalarPainter& ext )
{
	CompositeMaterial* m = new CompositeMaterial( top, bottom, r, a, b, c, d, thickness, ext );
	m->addref();
	return m;
}

static inline CoatedMaterial* MakeCoated( const IMaterial& base, double sigma, const IPainter& tint, double eta = 1.5 )
{
	UniformScalarPainter* w  = new UniformScalarPainter( 1.0 );
	UniformScalarPainter* n  = new UniformScalarPainter( eta );
	UniformScalarPainter* a  = new UniformScalarPainter( 0.001 );
	UniformScalarPainter* th = new UniformScalarPainter( 1.0 );
	UniformScalarPainter* ab = new UniformScalarPainter( sigma );
	CoatedMaterial* m = new CoatedMaterial( base, *w, *n, *a, *th, *ab, tint );
	m->addref();
	return m;
}

static inline GGXMaterial* MakeSchlickGgx( double diffuse, double f0, double alpha )
{
	UniformColorPainter* d = new UniformColorPainter( RISEPel( diffuse, diffuse, diffuse ) );  d->addref();
	UniformColorPainter* s = new UniformColorPainter( RISEPel( f0, f0, f0 ) );                 s->addref();
	UniformScalarPainter* a = new UniformScalarPainter( alpha );                               a->addref();
	UniformScalarPainter* n = new UniformScalarPainter( 1.5 );                                 n->addref();
	UniformScalarPainter* k = new UniformScalarPainter( 0.0 );                                 k->addref();
	GGXMaterial* g = new GGXMaterial( *d, *s, *a, *a, *n, *k, eFresnelSchlickF0 );
	g->addref();
	d->release(); s->release(); a->release(); n->release(); k->release();
	return g;
}

struct FurnaceStats
{
	double mean;
	double sd;		// sd of the batch means
	double sem;		// sd / sqrt(batches)
};

static inline FurnaceStats Furnace(
	const ISPF& spf, double thetaDeg, bool nm, bool fullSphere,
	int batches, int perBatch, unsigned seedBase )
{
	RayIntersectionGeometric ri = MakeIntersection( thetaDeg * kPi / 180.0 );
	const Point3 origin0 = ri.ray.origin;
	std::vector<double> means;
	for( int b = 0; b < batches; ++b ) {
		RandomNumberGenerator rng( seedBase + 7919u * (unsigned)b );
		IndependentSampler sampler( rng );
		IORStack stack = MakeTestIORStack( g_stub );
		RISEPel sum( 0, 0, 0 );
		double sumNM = 0;
		for( int i = 0; i < perBatch; ++i ) {
			const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
			ri.ptIntersection = p;
			ri.ray.origin = Point3( origin0.x + p.x, origin0.y + p.y, origin0.z );
			ScatteredRayContainer sc;
			if( nm ) {
				spf.ScatterNM( ri, sampler, 550.0, sc, stack );
			} else {
				spf.Scatter( ri, sampler, sc, stack );
			}
			for( unsigned j = 0; j < sc.Count(); ++j ) {
				const Vector3 d = Vector3Ops::Normalize( sc[j].ray.Dir() );
				if( !fullSphere && d.z <= 0 ) continue;
				if( nm ) sumNM += sc[j].krayNM;
				else     sum = sum + sc[j].kray;
			}
		}
		means.push_back( nm ? sumNM / perBatch : ColorMath::MaxValue( sum * ( 1.0 / perBatch ) ) );
	}
	FurnaceStats s;
	double m = 0; for( double v : means ) m += v; m /= means.size();
	double var = 0; for( double v : means ) var += ( v - m ) * ( v - m );
	var /= std::max<size_t>( 1, means.size() - 1 );
	s.mean = m;
	s.sd = std::sqrt( var );
	s.sem = s.sd / std::sqrt( (double)means.size() );
	return s;
}

#endif
