//////////////////////////////////////////////////////////////////////
//
//  CoatedEtaContinuityTest.cpp - DL-426: `coated_material`'s value()
//    must be CONTINUOUS in the relative coat index.
//
//  The GGX lobe reservoir (DL-388) prices the recycled field from a
//  per-(alpha, eta, tau) basis.  Before DL-426 that basis was a
//  DISCONTINUOUS function of eta: the spill table held point masses
//  (at a normal view every azimuth stratum of one radial ring reflects
//  to the same mu_u, up to 1/32 of the lobe) whose price jumped as the
//  critical cosine mu_c(eta) = sqrt(1 - 1/eta^2) swept across them, the
//  bins and cells priced by special rules near mu_c changed with eta,
//  and the critical patch's far-end join was skipped once mu_c + W
//  passed 1.  Measured: the recycled term 14 % over delta-eta 1e-5 at
//  alpha 0.1, eta ~1.01415 (view 80 / light 0); the clear-coat table
//  1.9 % at eta ~2.874.
//
//  Gate: sweep eta densely through each window at fixed directions and
//  require every adjacent-sample step to be no larger than a smooth
//  function's (20 x the sweep's own median step, floored at 0.001 %),
//  for absorbing coats (the direct basis build) and clear ones (the
//  table blend), RGB and NM.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/CoatedMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& name )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

static const double kPi = 3.14159265358979323846;

static RayIntersectionGeometric MakeIntersection( double thetaRad )
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

static GGXMaterial* MakeGgx( double diffuse, double f0, double alpha )
{
	UniformColorPainter* d = new UniformColorPainter( RISEPel( diffuse, diffuse, diffuse ) );
	UniformColorPainter* s = new UniformColorPainter( RISEPel( f0, f0, f0 ) );
	UniformScalarPainter* a = new UniformScalarPainter( alpha );
	UniformScalarPainter* n = new UniformScalarPainter( 1.5 );
	UniformScalarPainter* k = new UniformScalarPainter( 0.0 );
	GGXMaterial* g = new GGXMaterial( *d, *s, *a, *a, *n, *k, eFresnelSchlickF0 );
	g->addref();
	return g;
}

static CoatedMaterial* MakeCoated( const IMaterial& base, double eta, double sigma )
{
	UniformScalarPainter* w  = new UniformScalarPainter( 1.0 );
	UniformScalarPainter* n  = new UniformScalarPainter( eta );
	UniformScalarPainter* a  = new UniformScalarPainter( 0.001 );
	UniformScalarPainter* th = new UniformScalarPainter( 1.0 );
	UniformScalarPainter* ab = new UniformScalarPainter( sigma );
	UniformColorPainter*  t  = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	CoatedMaterial* m = new CoatedMaterial( base, *w, *n, *a, *th, *ab, *t );
	m->addref();
	return m;
}

struct Sweep
{
	const char* name;
	double diffuse, f0, alpha, sigma;
	double etaLo, etaHi;
	int    steps;
	double viewDeg, lightDeg;
};

static void RunSweep( const Sweep& sw )
{
	GGXMaterial* g = MakeGgx( sw.diffuse, sw.f0, sw.alpha );
	const RayIntersectionGeometric ri = MakeIntersection( sw.viewDeg * kPi / 180.0 );
	const double tl = sw.lightDeg * kPi / 180.0;
	const Vector3 wo( -std::sin( tl ), 0, std::cos( tl ) );
	std::vector<double> vals, valsNM;
	std::vector<double> etas;
	for( int i = 0; i <= sw.steps; ++i ) {
		const double eta = sw.etaLo + ( sw.etaHi - sw.etaLo ) * double(i) / double(sw.steps);
		CoatedMaterial* m = MakeCoated( *g, eta, sw.sigma );
		vals.push_back( m->GetBSDF()->value( wo, ri )[0] );
		valsNM.push_back( m->GetBSDF()->valueNM( wo, ri, 550.0 ) );
		etas.push_back( eta );
		m->release();
	}
	for( int pass = 0; pass < 2; ++pass ) {
		const std::vector<double>& v = pass ? valsNM : vals;
		std::vector<double> steps;
		double worst = 0; int worstAt = 0;
		for( size_t i = 1; i < v.size(); ++i ) {
			const double rel = std::fabs( v[i] - v[i-1] ) / std::max( 1e-12, 0.5 * ( std::fabs( v[i] ) + std::fabs( v[i-1] ) ) );
			steps.push_back( rel );
			if( rel > worst ) { worst = rel; worstAt = (int)i; }
		}
		std::vector<double> sorted = steps;
		std::sort( sorted.begin(), sorted.end() );
		const double median = sorted[ sorted.size() / 2 ];
		const double tol = std::max( 1e-5, 20.0 * median );
		std::cout << "  " << sw.name << ( pass ? " [NM]" : " [RGB]" ) << ": value " << std::setprecision(6) << v.front()
		          << " .. " << v.back() << ", median step " << std::setprecision(3) << 100.0 * median
		          << " %, worst step " << 100.0 * worst << " % at eta " << std::setprecision(8) << etas[worstAt]
		          << " (" << v[worstAt-1] << " -> " << v[worstAt] << ")\n";
		Check( worst <= tol, std::string( "[DL-426] " ) + sw.name + ( pass ? " NM" : " RGB" ) +
			" value continuous in eta (worst step <= max(0.001 %, 20 x median step))" );
	}
	g->release();
}

int main()
{
	std::cout << "CoatedEtaContinuityTest (DL-426)\n================================\n";
	const Sweep sweeps[] = {
		// The reported windows (absorbing coat: the direct basis build).
		{ "alpha .1 F0 .9 sigma .2, eta 1.0136-1.0147, view 80 light 0", 0.0, 0.9, 0.1, 0.2, 1.0136, 1.0147, 1100, 80, 0 },
		{ "alpha .1 white metal sigma .5, eta 1.0136-1.0147, view 80 light 0", 0.0, 1.0, 0.1, 0.5, 1.0136, 1.0147, 1100, 80, 0 },
		{ "alpha .02 F0 .9 sigma .2, eta 1.0010-1.0020, view 80 light 0", 0.0, 0.9, 0.02, 0.2, 1.0010, 1.0020, 1000, 80, 0 },
		{ "alpha .02 F0 .9 sigma .2, eta 1.0010-1.0020, view 60 light 30", 0.0, 0.9, 0.02, 0.2, 1.0010, 1.0020, 1000, 60, 30 },
		{ "alpha .1 F0 .9 sigma .2, eta 2.860-2.890, view 80 light 0", 0.0, 0.9, 0.1, 0.2, 2.860, 2.890, 1500, 80, 0 },
		{ "alpha .05 F0 .9 sigma .2, eta 2.860-2.890, view 10 light 20", 0.0, 0.9, 0.05, 0.2, 2.860, 2.890, 1500, 10, 20 },
		{ "alpha .1 mixed sigma .2, eta 1.001-1.10, view 80 light 0", 0.5, 0.5, 0.1, 0.2, 1.001, 1.10, 2000, 80, 0 },
		{ "alpha .3 F0 .9 sigma .2, eta 1.001-1.10, view 75 light 10", 0.0, 0.9, 0.3, 0.2, 1.001, 1.10, 2000, 75, 10 },
		// The clear-coat table path.
		{ "CLEAR alpha .1 F0 .9, eta 1.0136-1.0147, view 80 light 0", 0.0, 0.9, 0.1, 0.0, 1.0136, 1.0147, 1100, 80, 0 },
		{ "CLEAR alpha .1 F0 .9, eta 2.860-2.890, view 80 light 0", 0.0, 0.9, 0.1, 0.0, 2.860, 2.890, 1500, 80, 0 },
	};
	for( const Sweep& s : sweeps ) {
		RunSweep( s );
	}
	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
