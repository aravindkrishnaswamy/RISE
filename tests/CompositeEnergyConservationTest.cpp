//////////////////////////////////////////////////////////////////////
//
//  CompositeEnergyConservationTest.cpp - Red-proof and regression guard
//    for DL-24 (`composite_material`'s random walk loses the energy of
//    the coat-over-diffuse configuration) and the three contracts a
//    layered material has to satisfy once its walk conserves energy:
//
//    A. ENERGY.  A lossless top over an albedo-1 diffuse bottom must
//       return rho == 1 at every incidence, RGB and NM, at every
//       recursion budget -- the budgets are a cost knob, not an energy
//       sink.  Pre-fix the walk hard-truncated the internal Fresnel/TIR
//       reflection series at the top interface's underside (per-bounce
//       energy ledger in docs/DL24_COMPOSITE_ENERGY.md: exit 0.4260 +
//       dropped 0.5740 == 1.0000 at normal incidence).  A
//       `coated_material` of the same physical layers is the control.
//
//    B. DENSITY (the DL-67 Slice 0 / DL-98 / DL-99 contract).  `Pdf`
//       must report the density of the NON-DELTA directions `Scatter`
//       actually emits: its full-sphere integral must equal the measured
//       non-delta emission probability, and a histogram of real emitted
//       directions must match it in shape (total variation against a
//       MEASURED split-half noise floor).  Pre-fix: the 50/50 placeholder.
//
//    C. ONE FUNCTION PER SIDE (DL-157).  Every emitted non-delta ray
//       must satisfy kray * Pdf(dir) == value(dir) * cos, pointwise, with
//       `value` the material's own `GetBSDF()`.  Pre-fix `GetBSDF()` was
//       top-wins (the BARE substrate under a dielectric coat).
//
//    D. RENDER-LEVEL CLOSED FORMS (PT and BDPT).  (1) A white-furnace
//       env: a lossless coat over an albedo-1 Lambertian quad must read
//       exactly the environment radiance.  (2) A directional light: the
//       composite/Lambertian-control ratio must equal the smooth-coat
//       closed form T(theta_v) T(theta_l) / (eta^2 (1 - r_i)) -- which
//       NEE can only produce if `value` is the layered response.
//
//    E. HWSS (DL-221).  The companion-wavelength weight of every emitted
//       non-delta ray must be reconstructible from (ri, dir, nm):
//       `EvaluateLobeFNM` returns the aggregate valueNM and the 6-arg
//       `EvaluateKrayNM` equals valueNM(nm) * cos / pdfHero exactly.
//
//    F. SIBLING TABLE.  Every composite configuration class the tree
//       builds, full-sphere furnace, printed; lossless ones gated.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `debt-dl24`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

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
	#define getpid _getpid
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
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/DielectricMaterial.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Materials/GGXMaterial.h"
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

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

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

static StubObject* g_stub = 0;

static const double kPi = 3.14159265358979323846;

//////////////////////////////////////////////////////////////////////
//  Fixture: a flat +Z surface at the origin, viewed from direction
//  theta in the x-z plane (same construction as LayeredWhiteFurnaceTest).
//////////////////////////////////////////////////////////////////////
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

static Fixtures MakeFixtures()
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

static CompositeMaterial* MakeComposite(
	const IMaterial& top, const IMaterial& bottom,
	unsigned r, unsigned a, unsigned b, unsigned c, unsigned d,
	double thickness, const IScalarPainter& ext )
{
	CompositeMaterial* m = new CompositeMaterial( top, bottom, r, a, b, c, d, thickness, ext );
	m->addref();
	return m;
}

//////////////////////////////////////////////////////////////////////
//  SPF-level white furnace, position-jittered (see the loop).
//
//  rho(theta) = E[ sum_j kray_j ] over every emitted ray (reflection
//  half-space only unless `fullSphere`), batch means over independent
//  seeds so the report carries a standard deviation.  Max-over-channels
//  of the mean, as in LayeredWhiteFurnaceTest.
//////////////////////////////////////////////////////////////////////
struct FurnaceStats
{
	double mean;
	double sd;		// sd of the batch means
	double sem;		// sd / sqrt(batches)
};

static FurnaceStats Furnace(
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
			// The layered evaluator draws ONE walk per (wi, position) and
			// reuses it for every exit there (DL-24 review P2-3), so it is
			// unbiased AVERAGED OVER POSITIONS: move the shading point every
			// draw on this flat, uniform fixture.
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

static const double kThetas[] = { 0.0, 30.0, 60.0, 80.0 };
static const int kNumThetas = 4;

//////////////////////////////////////////////////////////////////////
//  Section A -- ENERGY.
//////////////////////////////////////////////////////////////////////
static void SectionA( Fixtures& f )
{
	std::cout << "\n[A] White furnace: lossless top over albedo-1 Lambertian (truth rho = 1)\n";
	std::cout << "    n = 8 batches x 20000 draws; mean +- sd(batch means)\n";

	struct Cfg { const char* name; CompositeMaterial* m; };
	Cfg cfgs[] = {
		{ "A1 dielectric(scat 0, ior 1.5)/white, 4/2/2/2/2, t=0   (LayeredWhiteFurnaceTest config 3)",
		  MakeComposite( *f.dScat0,  *f.lamb, 4, 2, 2, 2, 2, 0.0, *f.s0 ) },
		{ "A2 dielectric(scat 1e4, ior 1.5)/white, 3/3/3/3/3, t=0 (parser-default budgets)",
		  MakeComposite( *f.dSmooth, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ) },
		{ "A3 dielectric(scat 1e4, ior 1.5)/white, 5/3/3/3/3, t=0.5 ext 0 (shipped budgets)",
		  MakeComposite( *f.dSmooth, *f.lamb, 5, 3, 3, 3, 3, 0.5, *f.s0 ) },
		{ "A4 dielectric(scat 1e4, ior 1.33)/white, 3/3/3/3/3, t=0 (water)",
		  MakeComposite( *f.d133,    *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ) },
	};

	for( const Cfg& c : cfgs ) {
		for( int pipe = 0; pipe < 2; ++pipe ) {
			std::cout << "    " << c.name << ( pipe ? "  [NM 550]" : "  [RGB]" ) << "\n      ";
			for( int t = 0; t < kNumThetas; ++t ) {
				const FurnaceStats s = Furnace( *c.m->GetSPF(), kThetas[t], pipe == 1, false, 8, 20000, 1001u + t );
				std::cout << std::fixed << std::setprecision(4) << kThetas[t] << "deg: "
				          << s.mean << " +- " << s.sd << "   ";
				const double tol = std::max( 0.01, 5.0 * s.sem );
				Check( std::fabs( s.mean - 1.0 ) <= tol,
					std::string( "[A] " ) + c.name + ( pipe ? " NM" : " RGB" ) + " theta " + std::to_string( (int)kThetas[t] ) + " rho == 1" );
			}
			std::cout << "\n";
		}
		c.m->release();
	}

	// Control: coated_material with the same physical layers (smooth
	// 1.5 coat over the same white Lambertian).  Must read ~1 within its
	// own documented DL-37/DL-63 residual (LayeredWhiteFurnaceTest config
	// 11 reads 0.9889 at 80 deg with a 0.02 coat; this one is 0.001).
	std::cout << "    CONTROL coated_material(ior 1.5, rough 0.001)/white  [RGB]\n      ";
	for( int t = 0; t < kNumThetas; ++t ) {
		const FurnaceStats s = Furnace( *f.coatedControl->GetSPF(), kThetas[t], false, false, 8, 20000, 3001u + t );
		std::cout << std::fixed << std::setprecision(4) << kThetas[t] << "deg: " << s.mean << " +- " << s.sd << "   ";
		const double tol = ( kThetas[t] >= 80.0 ) ? 0.02 : std::max( 0.01, 5.0 * s.sem );
		Check( std::fabs( s.mean - 1.0 ) <= tol,
			std::string( "[A] coated control theta " ) + std::to_string( (int)kThetas[t] ) + " rho ~ 1" );
	}
	std::cout << "\n";
}

//////////////////////////////////////////////////////////////////////
//  Section B -- DENSITY (DL-67 contract): mass and shape.
//
//  Full-sphere quadrature of Pdf over (cos theta in [-1,1], phi) vs the
//  measured non-delta emission probability, and total variation of a
//  (cos theta, phi) histogram of emitted non-delta directions against
//  Pdf integrated per bin.  The TVD gate is 3x a measured split-half
//  noise floor (two independent halves of the same draws), per
//  TranslucentLobeConsistencyTest's lesson that a TVD gate over many
//  cells needs its floor measured, not guessed.
//////////////////////////////////////////////////////////////////////
static const int kBinsMu  = 16;
static const int kBinsPhi = 16;

static int BinOf( const Vector3& d )
{
	const double mu = std::max( -1.0, std::min( 1.0, d.z ) );
	int im = (int)( ( mu + 1.0 ) * 0.5 * kBinsMu );
	if( im >= kBinsMu ) im = kBinsMu - 1;
	if( im < 0 ) im = 0;
	double phi = std::atan2( d.y, d.x );
	if( phi < 0 ) phi += 2.0 * kPi;
	int ip = (int)( phi / ( 2.0 * kPi ) * kBinsPhi );
	if( ip >= kBinsPhi ) ip = kBinsPhi - 1;
	return im * kBinsPhi + ip;
}

static void PdfBins( const ISPF& spf, const RayIntersectionGeometric& ri, const IORStack& stack,
	std::vector<double>& bins, double& total )
{
	bins.assign( kBinsMu * kBinsPhi, 0.0 );
	total = 0;
	const int sub = 8;
	const double dMu  = 2.0 / ( kBinsMu * sub );
	const double dPhi = 2.0 * kPi / ( kBinsPhi * sub );
	for( int a = 0; a < kBinsMu * sub; ++a ) {
		const double mu = -1.0 + ( a + 0.5 ) * dMu;
		const double st = std::sqrt( std::max( 0.0, 1.0 - mu * mu ) );
		for( int b = 0; b < kBinsPhi * sub; ++b ) {
			const double phi = ( b + 0.5 ) * dPhi;
			const Vector3 w( st * std::cos( phi ), st * std::sin( phi ), mu );
			const double p = spf.Pdf( ri, w, stack ) * dMu * dPhi;
			bins[ BinOf( w ) ] += p;
			total += p;
		}
	}
}

static void SectionB( Fixtures& f )
{
	std::cout << "\n[B] Pdf == density of what Scatter emits (mass + shape), theta 30\n";

	struct Cfg { const char* name; CompositeMaterial* m; };
	Cfg cfgs[] = {
		{ "B1 dielectric(ior 1.5)/white Lambertian", MakeComposite( *f.dSmooth, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ) },
		{ "B2 translucent/red Lambertian",           MakeComposite( *f.trans,   *f.lambRed, 3, 3, 3, 3, 3, 0.0, *f.s0 ) },
		{ "B3 clearcoat GGX/red GGX (reflection-only top)", MakeComposite( *f.clearcoat, *f.redGgx, 3, 3, 3, 3, 3, 0.0, *f.s0 ) },
	};

	const RayIntersectionGeometric ri = MakeIntersection( 30.0 * kPi / 180.0 );
	for( const Cfg& c : cfgs ) {
		const ISPF& spf = *c.m->GetSPF();
		IORStack stack = MakeTestIORStack( g_stub );
		RandomNumberGenerator rng( 4242u );
		IndependentSampler sampler( rng );
		const int N = 200000;
		std::vector<double> hA( kBinsMu * kBinsPhi, 0.0 ), hB( kBinsMu * kBinsPhi, 0.0 );
		int nonDelta = 0, nonDeltaA = 0, nonDeltaB = 0, multi = 0;
		for( int i = 0; i < N; ++i ) {
			ScatteredRayContainer sc;
			spf.Scatter( ri, sampler, sc, stack );
			if( sc.Count() > 1 ) multi++;
			// The density a consumer sees is that of the ray
			// RandomlySelect returns, exactly as PT/BDPT consume it.
			Scalar q = 0;
			const ScatteredRay* p = sc.RandomlySelect( sampler.Get1D(), false, &q );
			if( !p || p->isDelta ) continue;
			const int bin = BinOf( Vector3Ops::Normalize( p->ray.Dir() ) );
			nonDelta++;
			if( i & 1 ) { hB[bin] += 1; nonDeltaB++; } else { hA[bin] += 1; nonDeltaA++; }
		}
		std::vector<double> e; double integral = 0;
		PdfBins( spf, ri, stack, e, integral );
		const double emitProb = (double)nonDelta / N;
		std::cout << "    " << c.name << ": int Pdf = " << std::setprecision(5) << integral
		          << ", measured non-delta emission probability = " << emitProb
		          << "  (Scatter calls emitting >1 ray: " << multi << ")\n";
		Check( std::fabs( integral - emitProb ) <= 0.01 + 0.02 * emitProb,
			std::string( "[B] " ) + c.name + " full-sphere int Pdf == measured non-delta emission probability" );
		if( nonDelta > 1000 && integral > 1e-6 ) {
			double tvd = 0, floorTvd = 0;
			for( size_t k = 0; k < e.size(); ++k ) {
				tvd      += std::fabs( ( hA[k] + hB[k] ) / nonDelta - e[k] / integral );
				floorTvd += std::fabs( hA[k] / std::max( 1, nonDeltaA ) - hB[k] / std::max( 1, nonDeltaB ) );
			}
			tvd *= 0.5; floorTvd *= 0.5;
			// The split-half TVD of two n/2 samples has ~sqrt(2) the
			// fluctuation of one n sample against the truth, so the
			// floor for the full histogram is floorTvd / sqrt(2) / sqrt(2)
			// (n doubles) ~= floorTvd / 2.  Gate at 3x that.
			const double gate = std::max( 0.01, 1.5 * floorTvd );
			std::cout << "      TVD(histogram, Pdf) = " << tvd << "  split-half floor = " << floorTvd
			          << "  gate = " << gate << "\n";
			Check( tvd <= gate, std::string( "[B] " ) + c.name + " TVD(real draws, Pdf) within the measured noise floor" );
		} else {
			Check( false, std::string( "[B] " ) + c.name + " emits non-delta directions (premise of the shape gate)" );
		}
		c.m->release();
	}
}

//////////////////////////////////////////////////////////////////////
//  Section C -- ONE FUNCTION PER SIDE: kray * Pdf == value * cos,
//  pointwise, for every emitted non-delta ray (RGB and NM).
//////////////////////////////////////////////////////////////////////
static void SectionC( Fixtures& f )
{
	std::cout << "\n[C] kray * Pdf(dir) == GetBSDF()->value(dir) * cos, pointwise (RGB and NM)\n";

	struct Cfg { const char* name; CompositeMaterial* m; };
	Cfg cfgs[] = {
		{ "C1 dielectric/white Lambertian", MakeComposite( *f.dSmooth, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ) },
		{ "C2 dielectric/red Lambertian, t=0.5 ext 0.3", MakeComposite( *f.dSmooth, *f.lambRed, 3, 3, 3, 3, 3, 0.5, *new UniformScalarPainter( 0.3 ) ) },
		{ "C3 translucent/red Lambertian",  MakeComposite( *f.trans,   *f.lambRed, 3, 3, 3, 3, 3, 0.0, *f.s0 ) },
	};

	for( const Cfg& c : cfgs ) {
		const IBSDF* pBSDF = c.m->GetBSDF();
		const ISPF& spf = *c.m->GetSPF();
		for( int pipe = 0; pipe < 2; ++pipe ) {
			int checked = 0, bad = 0;
			double worst = 0;
			for( int t = 0; t < 3; ++t ) {
				const RayIntersectionGeometric ri = MakeIntersection( kThetas[t] * kPi / 180.0 );
				IORStack stack = MakeTestIORStack( g_stub );
				RandomNumberGenerator rng( 555u + t );
				IndependentSampler sampler( rng );
				for( int i = 0; i < 4000; ++i ) {
					ScatteredRayContainer sc;
					if( pipe ) spf.ScatterNM( ri, sampler, 550.0, sc, stack );
					else       spf.Scatter( ri, sampler, sc, stack );
					for( unsigned j = 0; j < sc.Count(); ++j ) {
						const ScatteredRay& s = sc[j];
						if( s.isDelta ) continue;
						const Vector3 d = Vector3Ops::Normalize( s.ray.Dir() );
						const double cosO = std::fabs( Vector3Ops::Dot( d, ri.onb.w() ) );
						const double pdf = pipe ? spf.PdfNM( ri, d, 550.0, stack ) : spf.Pdf( ri, d, stack );
						double lhs, rhs;
						if( pipe ) {
							lhs = s.krayNM * pdf;
							rhs = pBSDF ? pBSDF->valueStatefulNM( d, ri, 550.0, &stack ) * cosO : 0.0;
						} else {
							lhs = ColorMath::MaxValue( s.kray * pdf );
							rhs = pBSDF ? ColorMath::MaxValue( pBSDF->valueStateful( d, ri, &stack ) * cosO ) : 0.0;
						}
						const double rel = std::fabs( lhs - rhs ) / std::max( 1e-12, std::fabs( rhs ) );
						worst = std::max( worst, rel );
						if( rel > 1e-9 ) bad++;
						checked++;
					}
				}
			}
			std::cout << "    " << c.name << ( pipe ? " [NM]" : " [RGB]" ) << ": non-delta rays checked = "
			          << checked << ", mismatches = " << bad << ", worst rel = " << worst << "\n";
			Check( checked > 1000, std::string( "[C] " ) + c.name + ( pipe ? " NM" : " RGB" ) + " emits non-delta rays" );
			Check( bad == 0, std::string( "[C] " ) + c.name + ( pipe ? " NM" : " RGB" ) + " kray*Pdf == value*cos for every non-delta ray" );
		}
		c.m->release();
	}
}

//////////////////////////////////////////////////////////////////////
//  Section E -- HWSS companion reconstructibility (DL-221).
//////////////////////////////////////////////////////////////////////
static void SectionE( Fixtures& f )
{
	std::cout << "\n[E] HWSS companion weight reconstructible from (ri, dir, nm)  (DL-221)\n";
	CompositeMaterial* m = MakeComposite( *f.dSmooth, *f.lambRed, 3, 3, 3, 3, 3, 0.2, *new UniformScalarPainter( 0.4 ) );
	const ISPF& spf = *m->GetSPF();
	const IBSDF* pBSDF = m->GetBSDF();
	const RayIntersectionGeometric ri = MakeIntersection( 35.0 * kPi / 180.0 );
	IORStack stack = MakeTestIORStack( g_stub );
	RandomNumberGenerator rng( 9090u );
	IndependentSampler sampler( rng );
	const double heroNM = 520.0, compNM = 640.0;
	int checked = 0, bad = 0, unimplemented = 0;
	for( int i = 0; i < 4000; ++i ) {
		ScatteredRayContainer sc;
		spf.ScatterNM( ri, sampler, heroNM, sc, stack );
		for( unsigned j = 0; j < sc.Count(); ++j ) {
			const ScatteredRay& s = sc[j];
			if( s.isDelta ) continue;
			const Vector3 d = Vector3Ops::Normalize( s.ray.Dir() );
			const double w = spf.EvaluateKrayNM( ri, d, s.type, compNM, stack, s.pdf );
			if( w < 0 ) { unimplemented++; continue; }
			const double cosO = std::fabs( Vector3Ops::Dot( d, ri.vNormal ) );
			const double ref = pBSDF ? pBSDF->valueStatefulNM( d, ri, compNM, &stack ) * cosO / s.pdf : -1;
			const double rel = std::fabs( w - ref ) / std::max( 1e-12, std::fabs( ref ) );
			if( rel > 1e-9 ) bad++;
			checked++;
			// And the hero reconstructs the hero kray exactly.
			const double wh = spf.EvaluateKrayNM( ri, d, s.type, heroNM, stack, s.pdf );
			if( std::fabs( wh - s.krayNM ) > 1e-9 * std::max( 1e-12, std::fabs( s.krayNM ) ) ) bad++;
		}
	}
	std::cout << "    non-delta rays: " << checked << " reconstructed, " << unimplemented
	          << " declined (-1), mismatches " << bad << "\n";
	Check( checked > 1000 && unimplemented == 0, "[E] every non-delta composite ray's companion weight is implemented (not the aggregate fallback)" );
	Check( bad == 0, "[E] companion weight == valueNM(nm) * cos / pdfHero, and the hero reconstructs its own kray" );
	m->release();
}

//////////////////////////////////////////////////////////////////////
//  Section E2 -- WALKER delta rays must DECLINE (DL-24 review P1-2).
//
//  A composite is a parallel slab: every all-delta walker path that
//  leaves through the top exits EXACTLY along the entry's mirror
//  direction, so a direction match cannot tell a walker ray from the
//  top's DIRECT delta reflection.  Grey, non-dispersive layers: the true
//  companion weight EQUALS the hero weight, so anything the 5-argument
//  EvaluateKrayNM returns must reproduce the ray's own krayNM, and the
//  walker class must be declined (-1).  Pre-fix (c03807a2) this read
//  15709 up-going emissions, 15709 "reconstructed", 7366 of them wrong,
//  worst relative error 7.34 (a walker ray priced at the direct total).
//////////////////////////////////////////////////////////////////////
static void SectionE2( Fixtures& f )
{
	std::cout << "\n[E2] Walker delta rays decline; the direct delta reflection reconstructs (DL-24 review P1-2)\n";
	UniformScalarPainter* sDelta = new UniformScalarPainter( 1000000.0 );  sDelta->addref();
	UniformScalarPainter* sExt   = new UniformScalarPainter( 1.0 );  sExt->addref();
	DielectricMaterial* smooth = new DielectricMaterial( *f.s1, *f.s15, *sDelta, false );  smooth->addref();
	CompositeMaterial* m = MakeComposite( *smooth, *smooth, 3, 3, 3, 3, 3, 1.0, *sExt );
	const ISPF& spf = *m->GetSPF();
	const double th = 30.0 * kPi / 180.0;
	const Vector3 d( std::sin( th ), 0, -std::cos( th ) );
	RandomNumberGenerator rng( 1234u );
	IndependentSampler sampler( rng );
	int up = 0, recon = 0, declined = 0, mism = 0, walkerDeclined = 0;
	double worst = 0;
	for( int i = 0; i < 200000; ++i ) {
		const Point3 p( rng.CanonicalRandom(), rng.CanonicalRandom(), 0 );
		const Ray r( Point3( p.x - d.x, p.y, 1.0 ), d );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( r, rs );
		ri.bHit = true; ri.range = 1.0; ri.ptIntersection = p;
		ri.vNormal = Vector3( 0, 0, 1 ); ri.vGeomNormal = Vector3( 0, 0, 1 ); ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		IORStack st = MakeTestIORStack( g_stub );
		ScatteredRayContainer sc;
		spf.ScatterNM( ri, sampler, 550.0, sc, st );
		for( unsigned j = 0; j < sc.Count(); ++j ) {
			if( sc[j].ray.Dir().z <= 0 ) continue;
			up++;
			const double c = spf.EvaluateKrayNM( ri, sc[j].ray.Dir(), sc[j].type, 600.0, st, sc[j].isDelta ? -1.0 : sc[j].pdf );
			if( c < 0 ) {
				declined++;
				if( sc[j].type != ScatteredRay::eRayReflection ) walkerDeclined++;
				continue;
			}
			recon++;
			const double rel = std::fabs( c - sc[j].krayNM ) / std::max( 1e-12, (double)sc[j].krayNM );
			if( rel > 1e-6 ) { mism++; worst = std::max( worst, rel ); }
		}
	}
	std::cout << "    glass(1e6)/glass, t 1, grey ext 1.0, theta 30, hero 550 / companion 600: up-going " << up
	          << ", reconstructed " << recon << ", declined " << declined << " (walker " << walkerDeclined
	          << "), mismatched " << mism << ", worst rel " << worst << "\n";
	Check( up > 10000 && recon > 1000 && walkerDeclined > 1000, "[E2] both classes present (direct reflections reconstructed, walker exits declined)" );
	Check( mism == 0, "[E2] no reconstructed companion weight differs from the grey stack's hero weight" );
	m->release(); smooth->release(); sDelta->release(); sExt->release();
}

//////////////////////////////////////////////////////////////////////
//  Section H -- STOCHASTIC TOPS, position-jittered (DL-24 review P1-1).
//
//  Every other furnace in this file uses ONE fixed intersection, so the
//  hash-seeded probe never changes.  A probe that INFERS determinism from
//  two hashed draws calls a single-emit stochastic top (tissue, a nested
//  composite) deterministic at the positions where its draws agree and
//  then gives the direct branch or every down branch probability ZERO
//  there -- invisible at one position, a bias over many.  New points per
//  draw.  Pre-fix (c03807a2, reviewer's independent harness, mean +- sem):
//  tissue / white 0.9985 +- .0032 (0 deg), 0.6250 +- .0022 (60 deg);
//  composite{glass / lossless translucent} / white 0.9418 +- .0042,
//  0.9319 +- .0037.  Truth 1 (lossless).
//////////////////////////////////////////////////////////////////////
static FurnaceStats JitteredFurnace( const ISPF& spf, double thetaDeg, int batches, int perBatch, unsigned seedBase )
{
	std::vector<double> means;
	for( int b = 0; b < batches; ++b ) {
		RandomNumberGenerator rng( seedBase + 7919u * (unsigned)b );
		IndependentSampler sampler( rng );
		RISEPel sum( 0, 0, 0 );
		for( int i = 0; i < perBatch; ++i ) {
			RayIntersectionGeometric ri = MakeIntersection( thetaDeg * kPi / 180.0 );
			const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
			ri.ptIntersection = p;
			ri.ray.origin = Point3( p.x + std::sin( thetaDeg * kPi / 180.0 ), p.y, 1.0 );
			IORStack stack = MakeTestIORStack( g_stub );
			ScatteredRayContainer sc;
			spf.Scatter( ri, sampler, sc, stack );
			for( unsigned j = 0; j < sc.Count(); ++j ) sum = sum + sc[j].kray;
		}
		means.push_back( ColorMath::MaxValue( sum * ( 1.0 / perBatch ) ) );
	}
	FurnaceStats st;
	double m = 0; for( double v : means ) m += v; m /= means.size();
	double var = 0; for( double v : means ) var += ( v - m ) * ( v - m );
	var /= std::max<size_t>( 1, means.size() - 1 );
	st.mean = m; st.sd = std::sqrt( var ); st.sem = st.sd / std::sqrt( (double)means.size() );
	return st;
}

static void SectionH( Fixtures& f )
{
	std::cout << "\n[H] Stochastic TOP layers, position-jittered full-sphere furnace (DL-24 review P1-1), mean +- sem\n";
	GenericHumanTissueMaterial* tissue = new GenericHumanTissueMaterial( *new UniformScalarPainter( 0.85 ), *new UniformScalarPainter( 0.0 ), 0.75, 0.012, 0.05, 7.0e-5, true );
	tissue->addref();
	CompositeMaterial* inner = MakeComposite( *f.dSmooth, *f.transLossless, 3, 3, 3, 3, 3, 0.0, *f.s0 );
	CompositeMaterial* innerGG = MakeComposite( *f.dSmooth, *f.dSmooth, 3, 3, 3, 3, 3, 0.0, *f.s0 );
	struct Cfg { const char* name; CompositeMaterial* m; int batches; bool gated; double pinLo, pinHi; };
	Cfg cfgs[] = {
		{ "H1 generic_human_tissue (single-emit, rolls up or down) / white",
		  MakeComposite( *tissue, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ), 16, true, 0, 0 },
		{ "H2 composite{dielectric / lossless translucent} (single-emit nested, has a BSDF) / white",
		  MakeComposite( *inner, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ), 32, true, 0, 0 },
		{ "H3 composite{dielectric / dielectric} (nested, no BSDF) / white -- KNOWN RESIDUAL PIN [0.40, 0.62]: the nested composite is walked FROM BELOW with the outer gap's stack, which already holds the shared object key, and the two-stack convention (defined for from-top walks) leaves its layers reading the wrong side -- see docs/DL24_COMPOSITE_ENERGY.md section 5",
		  MakeComposite( *innerGG, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ), 16, false, 0.40, 0.62 },
	};
	for( const Cfg& c : cfgs ) {
		std::cout << "    " << c.name << "\n      ";
		for( int t = 0; t < 4; t += 2 ) {
			const FurnaceStats s = JitteredFurnace( *c.m->GetSPF(), kThetas[t], c.batches, 20000, 911u + t );
			std::cout << std::fixed << std::setprecision(4) << kThetas[t] << "deg: " << s.mean << " +- " << s.sem << "   ";
			if( c.gated ) {
				Check( std::fabs( s.mean - 1.0 ) <= std::max( 0.02, 5.0 * s.sem ),
					std::string( "[H] " ) + c.name + " theta " + std::to_string( (int)kThetas[t] ) + " lossless -> 1" );
			} else {
				Check( s.mean >= c.pinLo && s.mean <= c.pinHi,
					std::string( "[H] H3 (known nested from-below residual) theta " ) + std::to_string( (int)kThetas[t] ) + " inside its pin band" );
			}
		}
		std::cout << "\n";
		c.m->release();
	}
	// H4 -- the same residual on its own: composite{dielectric/dielectric}
	// struck FROM INSIDE (a closed object seen from within: the ray
	// travels upward and the stack already holds the object's entry).
	// Below the critical angle the stack passes it through (gated at 1);
	// at 35 deg the two layers read each other's side, total-internally-
	// reflect forever and the energy is dropped at the walk cap
	// (pre-existing: the base 5c9eeb96 reads the same 0.086).  Pinned.
	{
		std::cout << "    H4 composite{dielectric / dielectric} struck from INSIDE (stack holds the object) -- KNOWN RESIDUAL PIN at 35 deg [0.04, 0.20]\n      ";
		const ISPF& spf = *innerGG->GetSPF();
		for( const double thDeg : { 20.0, 35.0 } ) {
			RandomNumberGenerator rng( 4242u );
			IndependentSampler sampler( rng );
			const double th = thDeg * kPi / 180.0;
			const Vector3 d( std::sin( th ), 0, std::cos( th ) );
			double sum = 0;
			const int N = 100000;
			for( int i = 0; i < N; ++i ) {
				const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
				const RasterizerState rs = { 0, 0 };
				RayIntersectionGeometric ri( Ray( Point3( p.x - d.x, p.y, -1.0 ), d ), rs );
				ri.bHit = true; ri.range = 1.0; ri.ptIntersection = p;
				ri.vNormal = Vector3( 0, 0, 1 ); ri.vGeomNormal = Vector3( 0, 0, 1 ); ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
				IORStack st = MakeTestIORStack( g_stub );
				st.push( 1.5 );
				ScatteredRayContainer sc;
				spf.Scatter( ri, sampler, sc, st );
				for( unsigned j = 0; j < sc.Count(); ++j ) sum += ColorMath::MaxValue( sc[j].kray );
			}
			const double rho = sum / N;
			std::cout << std::fixed << std::setprecision( 4 ) << thDeg << "deg: " << rho << "   ";
			if( thDeg < 30.0 ) {
				Check( std::fabs( rho - 1.0 ) <= 0.02, "[H] H4 struck from inside below the critical angle -> 1" );
			} else {
				Check( rho >= 0.04 && rho <= 0.20, "[H] H4 (known nested/inside residual) 35 deg inside its pin band" );
			}
		}
		std::cout << "\n";
	}
	inner->release(); innerGG->release(); tissue->release();
}

//////////////////////////////////////////////////////////////////////
//  Section T -- TILTED SHADING NORMAL (DL-24 review round 2, P1-A).
//
//  DielectricSPF's transmission warp (finite `scattering`) is clipped to
//  the GEOMETRIC side of the surface, so when the shading normal is
//  tilted the warp's wedge between the two planes moves mass across the
//  shading plane stochastically: the per-draw up/down split is NOT a
//  function of the record, and a top that still DECLARED a deterministic
//  split (SelectionMassIsDeterministic) had the aggregate mode price the
//  branches with a split no draw realises.  The reference is an
//  INDEPENDENT natural random walk written here (not the composite's
//  code): top Scatter, RandomlySelect, bounce between layers with the
//  two-stack convention (down-going arrivals see the outside stack,
//  up-going ones the gap stack), Russian roulette after 8 events.  Both
//  sides are furnaces over the full sphere at jittered positions; the
//  band is 5 sigma of the combined sem plus a 0.001 floor.
//////////////////////////////////////////////////////////////////////
static RayIntersectionGeometric MakeTiltedIntersection( double thDeg, const Point3& p, double tiltDeg )
{
	const double th = thDeg * kPi / 180.0;
	const Vector3 d( std::sin( th ), 0, -std::cos( th ) );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( Ray( Point3( p.x - d.x, p.y - d.y, p.z - d.z ), d ), rs );
	ri.bHit = true; ri.range = 1.0; ri.ptIntersection = p; ri.ptObjIntersec = p;
	const double t = tiltDeg * kPi / 180.0;
	const Vector3 ns( std::sin( t ), 0, std::cos( t ) );
	ri.vNormal = ns;
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( ns );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}

static RISEPel ReferenceLayerWalk( const ISPF& top, const ISPF& bot, const RayIntersectionGeometric& ri, ISampler& smp )
{
	const Vector3 n = ri.onb.w();
	IORStack outside = MakeTestIORStack( g_stub );
	ScatteredRayContainer c0;
	top.Scatter( ri, smp, c0, outside );
	Scalar q = 0;
	const ScatteredRay* r = c0.RandomlySelect( smp.Get1D(), false, &q );
	if( !r || !( q > 0 ) ) return RISEPel( 0, 0, 0 );
	RISEPel beta = r->kray * ( 1.0 / q );
	if( Vector3Ops::Dot( r->ray.Dir(), n ) >= 0 ) return beta;
	IORStack gap( r->ior_stack ? *r->ior_stack : outside );
	Vector3 w = Vector3Ops::Normalize( r->ray.Dir() );
	bool atBottom = true;
	RayIntersectionGeometric rec( ri );
	for( int ev = 0; ev < 4000; ++ev ) {
		rec.ray.origin = ri.ptIntersection;
		rec.ray.SetDir( w );
		const bool down = Vector3Ops::Dot( w, n ) <= 0;
		ScatteredRayContainer c;
		( atBottom ? bot : top ).Scatter( rec, smp, c, down ? outside : gap );
		r = c.RandomlySelect( smp.Get1D(), false, &q );
		if( !r || !( q > 0 ) ) return RISEPel( 0, 0, 0 );
		beta = beta * r->kray * ( 1.0 / q );
		const Scalar cosN = Vector3Ops::Dot( r->ray.Dir(), n );
		if( atBottom ) {
			if( cosN <= 0 ) return beta;
		} else {
			if( cosN >= 0 ) return beta;
			if( r->ior_stack ) gap = *r->ior_stack;
		}
		w = Vector3Ops::Normalize( r->ray.Dir() );
		atBottom = !atBottom;
		if( ev > 8 ) {
			const double p = std::min( 1.0, (double)ColorMath::MaxValue( beta ) );
			if( !( p > 0 ) || smp.Get1D() >= p ) return RISEPel( 0, 0, 0 );
			beta = beta * ( 1.0 / p );
		}
	}
	return RISEPel( 0, 0, 0 );
}

static FurnaceStats TiltedFurnace( const ISPF* composite, const ISPF* top, const ISPF* bot,
	double thDeg, double tiltDeg, int batches, int perBatch, unsigned seedBase )
{
	std::vector<double> means;
	for( int b = 0; b < batches; ++b ) {
		RandomNumberGenerator rng( seedBase + 7919u * (unsigned)b );
		IndependentSampler smp( rng );
		RISEPel sum( 0, 0, 0 );
		for( int i = 0; i < perBatch; ++i ) {
			const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
			const RayIntersectionGeometric ri = MakeTiltedIntersection( thDeg, p, tiltDeg );
			if( composite ) {
				IORStack st = MakeTestIORStack( g_stub );
				ScatteredRayContainer sc;
				composite->Scatter( ri, smp, sc, st );
				for( unsigned j = 0; j < sc.Count(); ++j ) sum = sum + sc[j].kray;
			} else {
				sum = sum + ReferenceLayerWalk( *top, *bot, ri, smp );
			}
		}
		means.push_back( ColorMath::MaxValue( sum * ( 1.0 / perBatch ) ) );
	}
	FurnaceStats st;
	double m = 0; for( double v : means ) m += v; m /= means.size();
	double var = 0; for( double v : means ) var += ( v - m ) * ( v - m );
	var /= std::max<size_t>( 1, means.size() - 1 );
	st.mean = m; st.sd = std::sqrt( var ); st.sem = st.sd / std::sqrt( (double)means.size() );
	return st;
}

static void SectionT( Fixtures& f )
{
	std::cout << "\n[T] Tilted shading normal: composite{dielectric / white} vs an independent layer walk (DL-24 review round 2 P1-A), mean +- sem, composite n = 64 x 20000, walk n = 16 x 20000\n";
	UniformScalarPainter* s5 = new UniformScalarPainter( 5.0 );  s5->addref();
	DielectricMaterial* dScat5 = new DielectricMaterial( *f.s1, *f.s15, *s5, false );  dScat5->addref();
	DielectricMaterial* tops[] = { f.dScat0, dScat5 };
	const char* names[] = { "scattering 0", "scattering 5" };
	for( int k = 0; k < 2; ++k ) {
		CompositeMaterial* m = MakeComposite( *tops[k], *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 );
		for( const double tilt : { 0.0, 5.0, 20.0, 35.0 } ) {
			for( const double th : { 0.0, 45.0 } ) {
				const FurnaceStats a = TiltedFurnace( m->GetSPF(), 0, 0, th, tilt, 64, 20000, 911u + (unsigned)th + 13u * (unsigned)tilt );
				const FurnaceStats r = TiltedFurnace( 0, tops[k]->GetSPF(), f.lamb->GetSPF(), th, tilt, 16, 20000, 555u + (unsigned)th + 17u * (unsigned)tilt );
				const double sig = std::sqrt( a.sem * a.sem + r.sem * r.sem );
				std::cout << std::fixed << std::setprecision( 4 ) << "    " << names[k] << ", tilt " << tilt << ", theta " << th
				          << ": composite " << a.mean << " +- " << a.sem << ", independent walk " << r.mean << " +- " << r.sem
				          << ", z " << std::setprecision( 2 ) << ( a.mean - r.mean ) / std::max( 1e-12, sig ) << "\n";
				Check( std::fabs( a.mean - r.mean ) <= 0.001 + 5.0 * sig,
					std::string( "[T] composite{dielectric " ) + names[k] + " / white} == independent walk, tilt " +
					std::to_string( (int)tilt ) + " theta " + std::to_string( (int)th ) );
			}
		}
		// A ray that arrives BEHIND the tilted shading normal (tilt 35,
		// theta 60: d . n_s = +0.087) is classified up-going and takes the
		// from-below walker, which the independent from-top walk above does
		// not model.  Pre-existing (the base reads the same ~0.09) and part
		// of DL-341's stack-gap family; pinned, not gated against the walk.
		{
			const FurnaceStats a = TiltedFurnace( m->GetSPF(), 0, 0, 60.0, 35.0, 8, 20000, 7717u + (unsigned)k );
			std::cout << std::fixed << std::setprecision( 4 ) << "    " << names[k]
			          << ", tilt 35, theta 60 (ray BEHIND the shading normal) -- KNOWN RESIDUAL PIN [0.06, 0.12] (DL-341): composite "
			          << a.mean << " +- " << a.sem << "\n";
			Check( a.mean >= 0.06 && a.mean <= 0.12,
				std::string( "[T] behind-shading-normal arrival (DL-341 residual) inside its pin band, " ) + names[k] );
		}
		m->release();
	}

	// T2 -- the E2 twin under tilt: grey layers, so a reconstructed 5-arg
	// companion weight must reproduce the ray's own hero krayNM.  A top
	// that is not deterministic at this record is PER-BRANCH, and the
	// per-branch mode declines every reconstruction (-1).
	std::cout << "    T2 companion reconstruction under a 20 deg tilt, composite{dielectric scattering 5 / white}, hero 550 / companion 600\n";
	{
		CompositeMaterial* m = MakeComposite( *dScat5, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 );
		const ISPF& spf = *m->GetSPF();
		RandomNumberGenerator rng( 4321u );
		IndependentSampler smp( rng );
		int up = 0, recon = 0, declined = 0, mism = 0;
		double worst = 0;
		for( int i = 0; i < 100000; ++i ) {
			const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
			const RayIntersectionGeometric ri = MakeTiltedIntersection( 30.0, p, 20.0 );
			IORStack st = MakeTestIORStack( g_stub );
			ScatteredRayContainer sc;
			spf.ScatterNM( ri, smp, 550.0, sc, st );
			for( unsigned j = 0; j < sc.Count(); ++j ) {
				if( Vector3Ops::Dot( sc[j].ray.Dir(), ri.onb.w() ) <= 0 ) continue;
				up++;
				const double c = spf.EvaluateKrayNM( ri, sc[j].ray.Dir(), sc[j].type, 600.0, st, sc[j].isDelta ? -1.0 : sc[j].pdf );
				if( c < 0 ) { declined++; continue; }
				recon++;
				const double rel = std::fabs( c - sc[j].krayNM ) / std::max( 1e-12, (double)sc[j].krayNM );
				if( rel > 1e-6 ) { mism++; worst = std::max( worst, rel ); }
			}
		}
		std::cout << "      up-going " << up << ", reconstructed " << recon << ", declined " << declined
		          << ", mismatched " << mism << ", worst rel " << worst << "\n";
		Check( up > 10000, "[T2] up-going emissions present under tilt" );
		Check( mism == 0, "[T2] no reconstructed companion weight differs from the grey stack's hero weight under a tilted shading normal" );
		m->release();
	}
	dScat5->release(); s5->release();
}

//////////////////////////////////////////////////////////////////////
//  Section F -- sibling table.  Full-sphere furnace (reflection +
//  transmission through the stack), RGB, theta 0 and 60.  Lossless
//  configurations are gated at 1; the rest are printed as a record.
//////////////////////////////////////////////////////////////////////
static void SectionF( Fixtures& f )
{
	std::cout << "\n[F] Sibling configuration classes (full-sphere furnace, RGB, n = 8 x 10000)\n";
	struct Cfg { const char* name; CompositeMaterial* m; bool lossless; };
	Cfg cfgs[] = {
		{ "F1 dielectric / dielectric (both ior 1.5) -- transmits through",
		  MakeComposite( *f.dSmooth, *f.dSmooth, 3, 3, 3, 3, 3, 0.0, *f.s0 ), true },
		{ "F2 dielectric / composite(dielectric/white) -- the double-composite shape: the inner coat's delta reflection is a delta BOTTOM lobe (walker class)",
		  MakeComposite( *f.dSmooth, *MakeComposite( *f.dSmooth, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ), 3, 3, 3, 3, 3, 0.0, *f.s0 ), true },
		{ "F2b dielectric / polished(white, delta coat) -- KNOWN-DEFECT PIN [1.05, 1.15]: polished_material's GetBSDF() is the bare Lambertian its SPF does not sample (DL-285), so term (a) prices the wrong substrate",
		  MakeComposite( *f.dSmooth, *f.polishedWhite, 3, 3, 3, 3, 3, 0.0, *f.s0 ), false },
		{ "F3 lossless translucent / white Lambertian",
		  MakeComposite( *f.transLossless, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ), false },
		{ "F4 translucent / red Lambertian (mat_wax_gold class)",
		  MakeComposite( *f.trans, *f.lambRed, 3, 3, 3, 3, 3, 0.0, *f.s0 ), false },
		{ "F5 dielectric / translucent (transmitting bottom)",
		  MakeComposite( *f.dSmooth, *f.trans, 3, 3, 3, 3, 3, 0.0, *f.s0 ), false },
		{ "F6 clearcoat GGX / red GGX (LayeredWhiteFurnaceTest config 7: substrate never reached)",
		  MakeComposite( *f.clearcoat, *f.redGgx, 4, 2, 2, 2, 2, 0.0, *f.s0 ), false },
		{ "F7 dielectric / generic_human_tissue (null bottom BSDF -- DL-126's composition case)",
		  MakeComposite( *f.dSmooth, *new GenericHumanTissueMaterial( *new UniformScalarPainter( 0.85 ), *new UniformScalarPainter( 0.0 ), 0.75, 0.012, 0.05, 7.0e-5, true ),
		                 3, 3, 3, 3, 3, 0.0, *f.s0 ), false },
	};
	for( const Cfg& c : cfgs ) {
		std::cout << "    " << c.name << "\n      ";
		for( int t = 0; t < 4; t += 2 ) {
			const FurnaceStats s = Furnace( *c.m->GetSPF(), kThetas[t], false, true, 8, 10000, 7001u + t );
			std::cout << std::fixed << std::setprecision(4) << kThetas[t] << "deg: " << s.mean << " +- " << s.sd << "   ";
			if( c.lossless ) {
				Check( std::fabs( s.mean - 1.0 ) <= std::max( 0.01, 5.0 * s.sem ),
					std::string( "[F] " ) + c.name + " theta " + std::to_string( (int)kThetas[t] ) + " lossless -> 1" );
			}
			if( std::string( c.name ).compare( 0, 3, "F2b" ) == 0 ) {
				// KNOWN-DEFECT PIN (DL-285, not this row's): polished_material's
				// GetBSDF() is the bare Lambertian its SPF does not sample, so
				// term (a) prices the wrong substrate and the stack reads over
				// unity.  Pinned so it can neither silently worsen nor silently
				// "improve" without this band being revisited.
				Check( s.mean >= 1.05 && s.mean <= 1.15,
					std::string( "[F] F2b (DL-285 known defect) theta " ) + std::to_string( (int)kThetas[t] ) + " pinned in [1.05, 1.15]" );
			} else {
				Check( s.mean <= 1.0 + std::max( 0.01, 5.0 * s.sem ),
					std::string( "[F] " ) + c.name + " theta " + std::to_string( (int)kThetas[t] ) + " energy-bounded" );
			}
		}
		std::cout << "\n";
		c.m->release();
	}
}

//////////////////////////////////////////////////////////////////////
//  Section G -- THE LAYERED VALUE AGAINST A CLOSED FORM.
//
//  For a SMOOTH dielectric top of index n over a Lambertian of albedo rho
//  across a gap of thickness t and extinction sigma, the layered BRDF is
//  EXACTLY (the Lambertian re-randomises every bounce, so the internal
//  interreflection series is geometric -- Saunderson's form generalised
//  to an absorbing gap):
//
//     f(wi, wo) = T(wi) T(wo) a(mu_ti) a(mu_to) rho
//                 ------------------------------------------
//                     pi n^2 ( 1 - rho E_ret )
//
//     a(mu)  = exp( -sigma t / mu )                  one gap crossing
//     E_ret  = INT_0^1 2 mu a(mu)^2 F_in(mu) dmu     one internal round trip
//
//  with mu_t the refracted cosines, T = 1 - F the outer Fresnel
//  transmittance and F_in the Fresnel reflectance at the top's underside
//  (1 under TIR).  At sigma = 0 this is exactly CoatedLayer's
//  T T rho / (pi n^2 (1 - r_i rho)).  The layered evaluator is ONE
//  Monte-Carlo estimate per query (a walk drawn once per (wi, position),
//  connected to wo with a (wi, wo, position)-seeded draw), so its MEAN
//  over many positions is compared: band max(4 sem, 0.4 %), and the
//  printed per-evaluation relative sd is the noise every NEE sample and
//  BDPT connection carries.  The comparison is also run with wi and wo
//  swapped (reciprocity).  This shares no code with the evaluator.
//////////////////////////////////////////////////////////////////////
static double FresnelInside15( const double mu, const double n )
{
	const double s2 = n * n * ( 1.0 - mu * mu );
	if( s2 >= 1.0 ) return 1.0;
	const double ct = std::sqrt( 1.0 - s2 );
	const double rs = ( n * mu - ct ) / ( n * mu + ct );
	const double rp = ( mu - n * ct ) / ( mu + n * ct );
	return 0.5 * ( rs * rs + rp * rp );
}

static double ClosedFormLayered( const double thetaI, const double thetaO, const double rho,
	const double sigma, const double t, const double n )
{
	const double ci = std::cos( thetaI ), co = std::cos( thetaO );
	const double Ti = 1.0 - CoatedLayer::Fresnel( ci, n );
	const double To = 1.0 - CoatedLayer::Fresnel( co, n );
	const double mti = CoatedLayer::CosRefracted( ci, n );
	const double mto = CoatedLayer::CosRefracted( co, n );
	const double ai = std::exp( -sigma * t / mti );
	const double ao = std::exp( -sigma * t / mto );
	const int N = 40000;
	double eRet = 0;
	for( int i = 0; i < N; ++i ) {
		const double mu = ( i + 0.5 ) / N;
		const double a = std::exp( -sigma * t / mu );
		eRet += 2.0 * mu * a * a * FresnelInside15( mu, n ) / N;
	}
	return Ti * To * ai * ao * rho / ( kPi * n * n * ( 1.0 - rho * eRet ) );
}

static void SectionG( Fixtures& f )
{
	std::cout << "\n[G] Layered value vs the closed form (smooth coat over a Lambertian, absorbing gap)\n";
	UniformScalarPainter* sDelta = new UniformScalarPainter( 1000000.0 );  sDelta->addref();
	DielectricMaterial* smooth = new DielectricMaterial( *f.s1, *f.s15, *sDelta, false );  smooth->addref();
	UniformScalarPainter* sExt = new UniformScalarPainter( 0.8 );  sExt->addref();

	struct Cfg { const char* name; CompositeMaterial* m; double rho[3]; double sigma; double t; };
	Cfg cfgs[] = {
		{ "G1 smooth coat / white, no gap", MakeComposite( *smooth, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ), { 1, 1, 1 }, 0.0, 0.0 },
		{ "G2 smooth coat / red, gap 0.3 ext 0.8", MakeComposite( *smooth, *f.lambRed, 3, 3, 3, 3, 3, 0.3, *sExt ), { 0.8, 0.2, 0.2 }, 0.8, 0.3 },
	};
	const double pairs[][2] = { { 0, 0 }, { 30, 60 }, { 60, 30 }, { 75, 10 }, { 10, 80 } };
	const int M = 24000;
	for( const Cfg& c : cfgs ) {
		const IBSDF* b = c.m->GetBSDF();
		Check( b != 0, std::string( "[G] " ) + c.name + " presents a BSDF" );
		if( !b ) { c.m->release(); continue; }
		for( const auto& pr : pairs ) {
			const double ti = pr[0] * kPi / 180.0, to = pr[1] * kPi / 180.0;
			// wi in the x-z plane; wo at azimuth 120 deg.
			const Vector3 wo( std::sin( to ) * std::cos( 2.0944 ), std::sin( to ) * std::sin( 2.0944 ), std::cos( to ) );
			double sum[3] = { 0, 0, 0 }, sum2[3] = { 0, 0, 0 };
			for( int k = 0; k < M; ++k ) {
				RayIntersectionGeometric ri = MakeIntersection( ti );
				ri.ptIntersection = Point3( 1e-3 * k, 7e-4 * k, 0 );
				IORStack stack = MakeTestIORStack( g_stub );
				const RISEPel v = b->valueStateful( wo, ri, &stack );
				for( int ch = 0; ch < 3; ++ch ) { sum[ch] += v[ch]; sum2[ch] += v[ch] * v[ch]; }
			}
			for( int ch = 0; ch < 3; ++ch ) {
				const double mean = sum[ch] / M;
				const double var = r_max( 0.0, sum2[ch] / M - mean * mean );
				const double sem = std::sqrt( var / M );
				const double truth = ClosedFormLayered( ti, to, c.rho[ch], c.sigma, c.t, 1.5 );
				const bool ok = std::fabs( mean - truth ) <= r_max( 4.0 * sem, 0.004 * truth );
				if( ch == 0 || c.rho[ch] != c.rho[0] ) {
					std::cout << "    " << c.name << " (" << pr[0] << "," << pr[1] << ") ch" << ch
					          << ": mean " << std::setprecision( 6 ) << mean << " +- " << sem
					          << " (sem; " << std::setprecision( 3 ) << ( mean > 0 ? std::fabs( mean - truth ) / sem : 0.0 )
					          << " sem off; per-evaluation relative sd " << ( mean > 0 ? std::sqrt( var ) / mean : 0.0 ) << ")"
					          << std::setprecision( 6 ) << "  closed form " << truth << "  " << ( ok ? "ok" : "MISMATCH" ) << "\n";
				}
				Check( ok, std::string( "[G] " ) + c.name + " theta (" + std::to_string( (int)pr[0] ) + "," +
					std::to_string( (int)pr[1] ) + ") ch" + std::to_string( ch ) + " value == closed form" );
			}
		}
		c.m->release();
	}
	sExt->release(); smooth->release(); sDelta->release();
}

//////////////////////////////////////////////////////////////////////
//  Render-level harness (same pattern as BDPTStrategyBalanceTest).
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;
	CapturingRasterizerOutput() : width(0), height(0) {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		width = img.GetWidth();
		height = img.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = img.GetPEL( x, y );
			}
		}
	}
};

//! Mean of max-channel radiance (composited over black) over columns
//! [x0, x1) and all rows except a 2-pixel border.  Returns -1 on a
//! failed or nonfinite render.
static double RegionMean( const CapturingRasterizerOutput& cap, unsigned x0, unsigned x1 )
{
	if( cap.pixels.empty() ) return -1;
	double sum = 0; int n = 0;
	for( unsigned y = 2; y + 2 < cap.height; ++y ) {
		for( unsigned x = x0; x < x1; ++x ) {
			const RISEColor& c = cap.pixels[y * cap.width + x];
			const double v = std::max( c.base.r, std::max( c.base.g, c.base.b ) ) * c.a;
			if( !std::isfinite( v ) ) return -1;
			sum += v; n++;
		}
	}
	return n ? sum / n : -1;
}

static bool Render( const std::string& sceneText, const char* tag, CapturingRasterizerOutput*& pCapOut, unsigned seed )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/composite_energy_%s_%d.RISEscene", tag, (int)getpid() );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return false;
		ofs << sceneText;
	}
	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return false;
	if( !pJob->LoadAsciiSceneViaCst( path ) ) { safe_release( pJob ); std::remove( path ); return false; }
	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );
	// Renders are seeded from libc rand(): fix it per render so repeats
	// are distinct but the run is reproducible (docs: rise-render-seeding).
	std::srand( seed );
	const bool ok = pJob->Rasterize();
	safe_release( pJob );
	std::remove( path );
	if( !ok ) { safe_release( pCap ); return false; }
	pCapOut = pCap;
	return true;
}

static const char* kLayers =
	"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_material\n{\n\tname mat_lamb\n\treflectance pnt_white\n}\n\n"
	"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
	"composite_material\n{\n\tname mat_comp\n\ttop mat_glass\n\tbottom mat_lamb\n\tthickness 0\n\textinction 0.0\n}\n\n";

static std::string PtRasterizer( bool env, int spp )
{
	std::ostringstream s;
	s << "standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	  << "pathtracing_pel_rasterizer\n{\n\tsamples " << spp << "\n\toidn_denoise FALSE\n\tpixel_filter box\n";
	if( env ) s << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";
	s << "}\n\nfile_rasterizeroutput\n{\n\tpattern rendered/composite_energy_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";
	return s.str();
}

static std::string BdptRasterizer( bool env, int spp )
{
	std::ostringstream s;
	s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	  << "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tsamples " << spp << "\n\toidn_denoise FALSE\n\tpixel_filter box\n";
	if( env ) s << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";
	s << "}\n\nfile_rasterizeroutput\n{\n\tpattern rendered/composite_energy_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";
	return s.str();
}

static void SectionD()
{
	std::cout << "\n[D] Render-level closed forms (PT pel, BDPT pel)\n";

	// D1: white furnace.  The left half of the frame is a composite quad,
	// the right half a white Lambertian control, env L = 1, no other
	// geometry (coplanar, so no interreflection).  A lossless coat over an
	// albedo-1 Lambertian must read exactly the env radiance, as must the
	// control.  PT is gated on the closed form directly.  BDPT carries a
	// documented, material-independent env-only bias (EnvLightBalanceTest:
	// env-only BDPT +28.5 %, pre-existing), so BDPT is gated on the
	// composite/control RATIO, which that bias cancels from.
	const std::string furnace = std::string( "RISE ASCII SCENE 7\n" ) +
		"film\n{\n\twidth 32\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n" +
		kLayers +
		"clippedplane_geometry\n{\n\tname qL\n\tpta -4 -3 0\n\tptb 0 -3 0\n\tptc 0 3 0\n\tptd -4 3 0\n}\n\n"
		"clippedplane_geometry\n{\n\tname qR\n\tpta 0 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd 0 3 0\n}\n\n"
		"standard_object\n{\n\tname objL\n\tgeometry qL\n\tmaterial mat_comp\n}\n\n"
		"standard_object\n{\n\tname objR\n\tgeometry qR\n\tmaterial mat_lamb\n}\n\n";

	for( int r = 0; r < 2; ++r ) {
		const std::string scene = furnace + ( r == 0 ? PtRasterizer( true, 128 ) : BdptRasterizer( true, 128 ) );
		CapturingRasterizerOutput* cap = 0;
		const bool ok = Render( scene, r == 0 ? "furnace_pt" : "furnace_bdpt", cap, 20240u + r );
		const double mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
		const double mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
		std::cout << "    D1 env white furnace, " << ( r == 0 ? "PT  " : "BDPT" ) << ": composite = "
		          << std::setprecision(5) << mL << ", Lambertian control = " << mR
		          << ", ratio = " << ( mR > 0 ? mL / mR : -1 ) << "  (truth 1, 1, 1)\n";
		if( r == 0 ) {
			Check( ok && std::fabs( mL - 1.0 ) <= 0.02, "[D1] composite white furnace under env == 1 (PT)" );
			Check( ok && std::fabs( mR - 1.0 ) <= 0.02, "[D1] Lambertian control under env == 1 (PT)" );
		}
		Check( ok && mR > 0 && std::fabs( mL / mR - 1.0 ) <= 0.03,
			std::string( "[D1] composite / Lambertian control == 1 under env (" ) + ( r == 0 ? "PT" : "BDPT" ) + ")" );
		if( cap ) safe_release( cap );
	}

	// D2: directional light.  Left half of the frame is the composite,
	// right half a white Lambertian control; the camera looks straight
	// down the normal.  Ratio composite/control = pi * f_composite(0, l),
	// and for a smooth coat over an albedo-1 Lambertian with no gap the
	// Saunderson form is EXACT:
	//     pi f = T(theta_v) T(theta_l) / ( eta^2 (1 - r_i) ).
	// The light reaches the surface ONLY through NEE (a delta light), so
	// this ratio is read entirely off `GetBSDF()->value`.
	const double eta = 1.5;
	const double ri15 = CoatedLayer::InternalDiffuseFresnel( eta );
	const double thetaLights[] = { 0.0, 60.0, 80.0 };
	for( double tl : thetaLights ) {
		const double tr = tl * kPi / 180.0;
		std::ostringstream light;
		light << "directional_light\n{\n\tname key\n\tpower 1.0\n\tcolor 1 1 1\n\tdirection "
		      << std::sin( tr ) << " 0 " << std::cos( tr ) << "\n}\n\n";
		const std::string scene2 = std::string( "RISE ASCII SCENE 7\n" ) +
			"film\n{\n\twidth 32\n\theight 16\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n" +
			kLayers +
			"clippedplane_geometry\n{\n\tname qL\n\tpta -4 -3 0\n\tptb 0 -3 0\n\tptc 0 3 0\n\tptd -4 3 0\n}\n\n"
			"clippedplane_geometry\n{\n\tname qR\n\tpta 0 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd 0 3 0\n}\n\n"
			"standard_object\n{\n\tname objL\n\tgeometry qL\n\tmaterial mat_comp\n}\n\n"
			"standard_object\n{\n\tname objR\n\tgeometry qR\n\tmaterial mat_lamb\n}\n\n" +
			light.str();
		const double Tl = 1.0 - CoatedLayer::Fresnel( std::cos( tr ), eta );
		const double Tv = 1.0 - CoatedLayer::Fresnel( 1.0, eta );
		const double truth = Tv * Tl / ( eta * eta * ( 1.0 - ri15 ) );
		for( int r = 0; r < 2; ++r ) {
			const std::string scene = scene2 + ( r == 0 ? PtRasterizer( false, 64 ) : BdptRasterizer( false, 64 ) );
			CapturingRasterizerOutput* cap = 0;
			const bool ok = Render( scene, r == 0 ? "dir_pt" : "dir_bdpt", cap, 31337u + r + (unsigned)tl );
			const double mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
			const double mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
			const double ratio = ( mR > 0 ) ? mL / mR : -1;
			std::cout << "    D2 directional light theta_l=" << tl << ", " << ( r == 0 ? "PT  " : "BDPT" )
			          << ": composite/control = " << std::setprecision(5) << ratio
			          << "  (closed form " << truth << ")\n";
			Check( ok && std::fabs( ratio - truth ) <= 0.03 * truth,
				std::string( "[D2] directional light, theta_l " ) + std::to_string( (int)tl ) +
				" composite/control == smooth-coat closed form (" + ( r == 0 ? "PT" : "BDPT" ) + ")" );
			if( cap ) safe_release( cap );
		}
	}

	// D3: a TRANSMITTING composite, glass / glass with zero gap, under the
	// same env furnace.  Lossless, so the truth is 1; it reads ~0.49
	// because the walker's exit through the BOTTOM carries the
	// inside-the-object stack, so the escaping ray is priced at the
	// eta^-2 = 0.444 basic-radiance factor of a medium it never entered.
	// Pre-existing (the base 5c9eeb96 reads 0.484-0.487 across PT/BDPT
	// pel, spectral and HWSS) and part of DL-341's stack-gap family.
	// KNOWN RESIDUAL PIN [0.43, 0.54]; the plain glass quad on the right
	// is printed as a record only.
	{
		const std::string glassComp =
			"dielectric_material\n{\n\tname mat_glass2\n\ttau 1\n\tior 1.5\n}\n\n"
			"composite_material\n{\n\tname mat_gg\n\ttop mat_glass\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n";
		const std::string scene3 = std::string( "RISE ASCII SCENE 7\n" ) +
			"film\n{\n\twidth 32\n\theight 16\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n" +
			kLayers + glassComp +
			"clippedplane_geometry\n{\n\tname qL\n\tpta -4 -3 0\n\tptb 0 -3 0\n\tptc 0 3 0\n\tptd -4 3 0\n}\n\n"
			"clippedplane_geometry\n{\n\tname qR\n\tpta 0 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd 0 3 0\n}\n\n"
			"standard_object\n{\n\tname objL\n\tgeometry qL\n\tmaterial mat_gg\n}\n\n"
			"standard_object\n{\n\tname objR\n\tgeometry qR\n\tmaterial mat_glass\n}\n\n";
		for( int r = 0; r < 2; ++r ) {
			const std::string scene = scene3 + ( r == 0 ? PtRasterizer( true, 64 ) : BdptRasterizer( true, 64 ) );
			CapturingRasterizerOutput* cap = 0;
			const bool ok = Render( scene, r == 0 ? "gg_pt" : "gg_bdpt", cap, 40960u + r );
			const double mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
			const double mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
			std::cout << "    D3 transmitting composite glass/glass under env, " << ( r == 0 ? "PT  " : "BDPT" )
			          << ": composite = " << std::setprecision(5) << mL << " (truth 1; KNOWN RESIDUAL PIN [0.43, 0.54], DL-341)"
			          << ", plain glass quad (record) = " << mR << "\n";
			Check( ok && mL >= 0.43 && mL <= 0.54,
				std::string( "[D3] transmitting composite (DL-341 residual) inside its pin band (" ) + ( r == 0 ? "PT" : "BDPT" ) + ")" );
			if( cap ) safe_release( cap );
		}
	}
}

int main( int argc, char** argv )
{
	std::cout << "CompositeEnergyConservationTest (DL-24 / DL-221)" << std::endl;
	std::cout << "================================================" << std::endl;

	g_stub = new StubObject();
	g_stub->addref();

	Fixtures f = MakeFixtures();

	const bool skipRender = ( argc > 1 && std::string( argv[1] ) == "--no-render" );
	if( argc > 1 && std::string( argv[1] ) == "--tilt-only" ) {
		SectionT( f );
		if( argc > 2 && std::string( argv[2] ) == "--render" ) SectionD();
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	SectionA( f );
	SectionB( f );
	SectionC( f );
	SectionE( f );
	SectionE2( f );
	SectionF( f );
	SectionH( f );
	SectionT( f );
	SectionG( f );
	if( !skipRender ) {
		SectionD();
	}

	std::cout << "\n================================================" << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
