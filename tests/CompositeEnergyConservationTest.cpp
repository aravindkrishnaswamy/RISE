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

#include "../src/Library/Utilities/SobolSampler.h"
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
	// The BOTTOM is a denser glass (2.4): since DL-341 the gap -> bottom
	// interface refracts from the GAP's index (1.5), so a 1.5 bottom is
	// index-matched and reflects nothing back up -- no walker exit through
	// the top would exist (the pre-DL-341 bottom refracted from 1.0 and
	// reflected 4 %).  1.5 -> 2.4 reflects ~5 %.
	UniformScalarPainter* s24 = new UniformScalarPainter( 2.4 );  s24->addref();
	DielectricMaterial* dense = new DielectricMaterial( *f.s1, *s24, *sDelta, false );  dense->addref();
	CompositeMaterial* m = MakeComposite( *smooth, *dense, 3, 3, 3, 3, 3, 1.0, *sExt );
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
	std::cout << "    glass(1e6)/glass(2.4), t 1, grey ext 1.0, theta 30, hero 550 / companion 600: up-going " << up
	          << ", reconstructed " << recon << ", declined " << declined << " (walker " << walkerDeclined
	          << "), mismatched " << mism << ", worst rel " << worst << "\n";
	Check( up > 10000 && recon > 1000 && walkerDeclined > 1000, "[E2] both classes present (direct reflections reconstructed, walker exits declined)" );
	Check( mism == 0, "[E2] no reconstructed companion weight differs from the grey stack's hero weight" );
	m->release(); smooth->release(); dense->release(); s24->release(); sDelta->release(); sExt->release();
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
		// H3 was a KNOWN RESIDUAL PIN [0.40, 0.62] until DL-341 (2026-10-02):
		// the nested composite is walked FROM BELOW with the outer gap's stack,
		// which holds the shared object key, and the pre-DL-341 two-stack rule
		// (defined for from-top walks) left its layers reading each other's
		// side -- a lossless TIR ping-pong dropped at the 256-event cap (base
		// 0.4795 / 0.5064).  Gated at 1 since the stack convention is defined
		// for every entry side.
		{ "H3 composite{dielectric / dielectric} (nested, no BSDF, walked from below) / white",
		  MakeComposite( *innerGG, *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 ), 16, true, 0, 0 },
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
	// H4 -- the from-below case on its own: composite{dielectric/dielectric}
	// struck FROM INSIDE (a closed object seen from within: the ray travels
	// upward and the stack already holds the object's entry).  Lossless, so
	// the full-sphere furnace is 1 at every angle: below the critical angle
	// most of it leaves through the top, above it (41.8 deg) a genuine TIR
	// sends it back down through the index-matched bottom.  Until DL-341
	// (2026-10-02) the 35 deg row read 0.0868 (base 5c9eeb96 0.086): the two
	// layers read each other's side, total-internally-reflected forever and
	// the energy was dropped at the walk cap.
	{
		std::cout << "    H4 composite{dielectric / dielectric} struck from INSIDE (stack holds the object), lossless -> 1\n      ";
		const ISPF& spf = *innerGG->GetSPF();
		for( const double thDeg : { 20.0, 35.0, 60.0 } ) {
			RandomNumberGenerator rng( 4242u );
			IndependentSampler sampler( rng );
			const double th = thDeg * kPi / 180.0;
			const Vector3 d( std::sin( th ), 0, std::cos( th ) );
			double sum = 0, sum2 = 0;
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
				double v = 0;
				for( unsigned j = 0; j < sc.Count(); ++j ) v += ColorMath::MaxValue( sc[j].kray );
				sum += v; sum2 += v * v;
			}
			const double rho = sum / N;
			const double sem = std::sqrt( std::max( 0.0, sum2 / N - rho * rho ) / N );
			std::cout << std::fixed << std::setprecision( 4 ) << thDeg << "deg: " << rho << " +- " << sem << "   ";
			Check( std::fabs( rho - 1.0 ) <= std::max( 0.01, 5.0 * sem ),
				std::string( "[H] H4 struck from inside, lossless -> 1, theta " ) + std::to_string( (int)thDeg ) );
		}
		std::cout << "\n";
	}
	// H5 (DL-341 review round 1): the NESTED stack of the review's D6 box,
	// composite{composite{glass/glass}/glass}, full-sphere furnace from
	// above (jittered) and struck from inside at 20 / 35 / 60 deg.  Lossless:
	// 1 everywhere.  An SPF-level twin of D6 that separates a biased walk
	// estimator from a render-level stack / eta^2 defect.
	{
		CompositeMaterial* nest = MakeComposite( *innerGG, *f.dSmooth, 3, 3, 3, 3, 3, 0.0, *f.s0 );
		std::cout << "    H5 composite{composite{glass/glass}/glass}, from above (jittered) and from inside, lossless -> 1\n      ";
		for( int t = 0; t < 4; t += 2 ) {
			const FurnaceStats st = JitteredFurnace( *nest->GetSPF(), kThetas[t], 16, 20000, 1311u + t );
			std::cout << std::fixed << std::setprecision( 4 ) << "above " << kThetas[t] << "deg: " << st.mean << " +- " << st.sem << "   ";
			Check( std::fabs( st.mean - 1.0 ) <= std::max( 0.005, 5.0 * st.sem ), std::string( "[H] H5 nested from above, theta " ) + std::to_string( (int)kThetas[t] ) );
		}
		const ISPF& spf = *nest->GetSPF();
		for( const double thDeg : { 20.0, 35.0, 60.0 } ) {
			RandomNumberGenerator rng( 5151u );
			IndependentSampler sampler( rng );
			const double th = thDeg * kPi / 180.0;
			const Vector3 d( std::sin( th ), 0, std::cos( th ) );
			double sum = 0, sum2 = 0;
			const int N = 200000;
			for( int i = 0; i < N; ++i ) {
				const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
				const RasterizerState rs = { 0, 0 };
				RayIntersectionGeometric ri( Ray( Point3( p.x - d.x, p.y, -1.0 ), d ), rs );
				ri.bHit = true; ri.range = 1.0; ri.ptIntersection = p;
				ri.vNormal = Vector3( 0, 0, 1 ); ri.vGeomNormal = Vector3( 0, 0, 1 ); ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
				IORStack stk = MakeTestIORStack( g_stub );
				stk.push( 1.5 );
				ScatteredRayContainer sc;
				spf.Scatter( ri, sampler, sc, stk );
				double v = 0;
				for( unsigned j = 0; j < sc.Count(); ++j ) v += ColorMath::MaxValue( sc[j].kray );
				sum += v; sum2 += v * v;
			}
			const double rho = sum / N;
			const double sem = std::sqrt( std::max( 0.0, sum2 / N - rho * rho ) / N );
			std::cout << "inside " << thDeg << "deg: " << rho << " +- " << sem << "   ";
			Check( std::fabs( rho - 1.0 ) <= std::max( 0.005, 5.0 * sem ), std::string( "[H] H5 nested from inside, theta " ) + std::to_string( (int)thDeg ) );
		}
		std::cout << "\n";
		// The same stack under a RADIANCE consumer's eta^2 factor: below its
		// top the nested stack is index-matched, so sum kray * RadianceEtaScale
		// must equal one plain glass interface's (F + (1-F)/eta^2 from
		// above, F + (1-F) eta^2 from inside), angle by angle.
		std::cout << "      eta^2-weighted, nested | plain glass: ";
		for( int side = 0; side < 2; ++side ) {
			for( const double thDeg : { 20.0, 35.0, 60.0 } ) {
				double acc[2] = { 0, 0 }, acc2[2] = { 0, 0 };
				const int N = 100000;
				for( int which = 0; which < 2; ++which ) {
					const ISPF& sp = which == 0 ? *nest->GetSPF() : *f.dSmooth->GetSPF();
					RandomNumberGenerator rng( 6161u + (unsigned)thDeg );
					IndependentSampler sampler( rng );
					const double th = thDeg * kPi / 180.0;
					const Vector3 d( std::sin( th ), 0, side == 0 ? -std::cos( th ) : std::cos( th ) );
					for( int i = 0; i < N; ++i ) {
						const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
						const RasterizerState rs = { 0, 0 };
						RayIntersectionGeometric ri( Ray( Point3( p.x - d.x, p.y, -d.z ), d ), rs );
						ri.bHit = true; ri.range = 1.0; ri.ptIntersection = p;
						ri.vNormal = Vector3( 0, 0, 1 ); ri.vGeomNormal = Vector3( 0, 0, 1 ); ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
						IORStack stk = MakeTestIORStack( g_stub );
						if( side == 1 ) stk.push( 1.5 );
						ScatteredRayContainer sc;
						sp.Scatter( ri, sampler, sc, stk );
						double v = 0;
						for( unsigned j = 0; j < sc.Count(); ++j ) v += ColorMath::MaxValue( sc[j].kray ) * RadianceEtaScale( stk, sc[j].ior_stack );
						acc[which] += v; acc2[which] += v * v;
					}
				}
				const double a = acc[0] / N, b = acc[1] / N;
				const double sa = std::sqrt( std::max( 0.0, acc2[0] / N - a * a ) / N );
				std::cout << ( side == 0 ? "above " : "inside " ) << thDeg << ": " << std::setprecision( 4 ) << a << " | " << b << "   ";
				Check( std::fabs( a - b ) <= std::max( 0.003, 5.0 * sa ),
					std::string( "[H] H5 nested eta^2-weighted == plain glass, " ) + ( side == 0 ? "above " : "inside " ) + std::to_string( (int)thDeg ) );
			}
		}
		std::cout << "\n";
		nest->release();
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

static RISEPel ReferenceLayerWalk( const ISPF& top, const ISPF& bot, const RayIntersectionGeometric& ri, ISampler& smp, Vector3* outDir = 0 )
{
	const Vector3 n = ri.onb.w();
	IORStack outside = MakeTestIORStack( g_stub );
	ScatteredRayContainer c0;
	top.Scatter( ri, smp, c0, outside );
	Scalar q = 0;
	const ScatteredRay* r = c0.RandomlySelect( smp.Get1D(), false, &q );
	if( !r || !( q > 0 ) ) return RISEPel( 0, 0, 0 );
	RISEPel beta = r->kray * ( 1.0 / q );
	if( outDir ) *outDir = r->ray.Dir();
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
		if( outDir ) *outDir = r->ray.Dir();
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
	UniformScalarPainter* s1e6 = new UniformScalarPainter( 1000000.0 );  s1e6->addref();
	DielectricMaterial* dScat5 = new DielectricMaterial( *f.s1, *f.s15, *s5, false );  dScat5->addref();
	DielectricMaterial* dDelta = new DielectricMaterial( *f.s1, *f.s15, *s1e6, false );  dDelta->addref();
	// The third top has its warp OFF (delta transmission), so it still
	// DECLARES determinism under a tilt and runs the AGGREGATE mode there:
	// the row proves the declaration is right when it is kept.
	// The fourth is a lossless TRANSLUCENT top, which keeps its
	// unconditional declaration: its lobes are clipped to the geometric
	// side too, so the row checks the claim under the same tilts.
	const IMaterial* tops[] = { f.dScat0, dScat5, dDelta, f.transLossless };
	const char* names[] = { "dielectric scattering 0", "dielectric scattering 5", "dielectric scattering 1e6 (warp off, still declared)", "lossless translucent (declared)" };
	for( int k = 0; k < 4; ++k ) {
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
					std::string( "[T] composite{" ) + names[k] + " / white} == independent walk, tilt " +
					std::to_string( (int)tilt ) + " theta " + std::to_string( (int)th ) );
			}
		}
		// A ray that arrives BEHIND the tilted shading normal (tilt 35,
		// theta 60: d . n_s = +0.087) is geometrically from ABOVE, so it is
		// walked naturally from the top (DL-341, 2026-10-02) -- exactly what
		// the independent walk does.  Until DL-341 it was classified from
		// below by the shading normal and read 0.0899 / 0.0900 / 0.0904 /
		// 0.0903 for the four tops (base 0.0907), pinned in [0.06, 0.12].
		{
			const FurnaceStats a = TiltedFurnace( m->GetSPF(), 0, 0, 60.0, 35.0, 8, 20000, 7717u + (unsigned)k );
			const FurnaceStats r = TiltedFurnace( 0, tops[k]->GetSPF(), f.lamb->GetSPF(), 60.0, 35.0, 8, 20000, 8818u + (unsigned)k );
			const double sig = std::sqrt( a.sem * a.sem + r.sem * r.sem );
			std::cout << std::fixed << std::setprecision( 4 ) << "    " << names[k]
			          << ", tilt 35, theta 60 (ray BEHIND the shading normal): composite " << a.mean << " +- " << a.sem
			          << ", independent walk " << r.mean << " +- " << r.sem
			          << ", z " << std::setprecision( 2 ) << ( a.mean - r.mean ) / std::max( 1e-12, sig ) << "\n";
			Check( std::fabs( a.mean - r.mean ) <= 0.001 + 5.0 * sig,
				std::string( "[T] behind-shading-normal arrival == independent walk (DL-341), " ) + names[k] );
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
	dScat5->release(); s5->release(); dDelta->release(); s1e6->release();
}

//////////////////////////////////////////////////////////////////////
//  Section W -- DL-297: THE SHAPE OF A WARPED COAT'S EXIT.
//
//  DielectricSPF with a finite `scattering` warps its delta-tagged
//  transmission by a clipped Phong cos^N lobe about the Snell direction.
//  The furnace sections cannot see whether the layered evaluator's term
//  (a) reproduces that warp: an ideal-Snell connection conserves the same
//  energy, it just puts it in the wrong DIRECTIONS.  So this section bins
//  the exit energy by cos(theta_out) -- composite emissions (covered +
//  direct + walker, the kray of every emitted up-going ray) against the
//  independent natural layer walk of Section T, which follows the warp
//  because it calls the top's own Scatter.  Untilted, jittered position,
//  per-bin band 5 sigma + 0.002.  Pre-fix (ideal-Snell term (a)) the
//  scattering-0 row at theta 0 read evaluator 0.013 / 0.064 / 0.128 /
//  0.193 / 0.251 / 0.349 against the walk's 0.123 / 0.136 / 0.150 /
//  0.167 / 0.185 / 0.239 (the DL-297 ledger row); the parser default
//  (scattering 10000) is a control that agrees either way.
//////////////////////////////////////////////////////////////////////
static const int kWarpBins = 6;

static void WarpHistogram( const ISPF* composite, const ISPF* top, const ISPF* bot, double thDeg,
	int batches, int perBatch, unsigned seedBase, double mean[kWarpBins], double sem[kWarpBins] )
{
	std::vector<double> per[kWarpBins];
	for( int b = 0; b < batches; ++b ) {
		RandomNumberGenerator rng( seedBase + 7919u * (unsigned)b );
		IndependentSampler smp( rng );
		double acc[kWarpBins] = { 0 };
		for( int i = 0; i < perBatch; ++i ) {
			const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
			const RayIntersectionGeometric ri = MakeTiltedIntersection( thDeg, p, 0.0 );
			if( composite ) {
				IORStack st = MakeTestIORStack( g_stub );
				ScatteredRayContainer sc;
				composite->Scatter( ri, smp, sc, st );
				for( unsigned j = 0; j < sc.Count(); ++j ) {
					const Vector3 d = Vector3Ops::Normalize( sc[j].ray.Dir() );
					if( d.z <= 0 ) continue;
					acc[ std::min( kWarpBins - 1, (int)( d.z * kWarpBins ) ) ] += ColorMath::MaxValue( sc[j].kray );
				}
			} else {
				Vector3 d( 0, 0, 0 );
				const RISEPel v = ReferenceLayerWalk( *top, *bot, ri, smp, &d );
				d = Vector3Ops::Normalize( d );
				if( d.z > 0 ) {
					acc[ std::min( kWarpBins - 1, (int)( d.z * kWarpBins ) ) ] += ColorMath::MaxValue( v );
				}
			}
		}
		for( int k = 0; k < kWarpBins; ++k ) per[k].push_back( acc[k] / perBatch );
	}
	for( int k = 0; k < kWarpBins; ++k ) {
		double m = 0; for( double v : per[k] ) m += v; m /= per[k].size();
		double var = 0; for( double v : per[k] ) var += ( v - m ) * ( v - m );
		var /= std::max<size_t>( 1, per[k].size() - 1 );
		mean[k] = m; sem[k] = std::sqrt( var / per[k].size() );
	}
}

static void SectionW( Fixtures& f )
{
	std::cout << "\n[W] DL-297: exit-energy histogram by cos(theta_out), 6 bins grazing -> normal, composite{dielectric / white} vs the independent layer walk (16 x 20000 each)\n";
	UniformScalarPainter* s5 = new UniformScalarPainter( 5.0 );  s5->addref();
	DielectricMaterial* dScat5 = new DielectricMaterial( *f.s1, *f.s15, *s5, false );  dScat5->addref();
	const IMaterial* tops[] = { f.dScat0, dScat5, f.dSmooth };
	const char* names[] = { "scattering 0", "scattering 5", "scattering 10000 (parser default, control)" };
	for( int k = 0; k < 3; ++k ) {
		CompositeMaterial* m = MakeComposite( *tops[k], *f.lamb, 3, 3, 3, 3, 3, 0.0, *f.s0 );
		for( const double th : { 0.0, 45.0 } ) {
			double am[kWarpBins], as[kWarpBins], rm[kWarpBins], rs[kWarpBins];
			WarpHistogram( m->GetSPF(), 0, 0, th, 16, 20000, 3301u + (unsigned)th + 31u * (unsigned)k, am, as );
			WarpHistogram( 0, tops[k]->GetSPF(), f.lamb->GetSPF(), th, 16, 20000, 4403u + (unsigned)th + 37u * (unsigned)k, rm, rs );
			std::cout << "    " << names[k] << ", theta " << th << "\n      composite:";
			for( int b = 0; b < kWarpBins; ++b ) std::cout << " " << std::fixed << std::setprecision( 4 ) << am[b];
			std::cout << "\n      walk     :";
			for( int b = 0; b < kWarpBins; ++b ) std::cout << " " << std::fixed << std::setprecision( 4 ) << rm[b];
			std::cout << "\n      z        :";
			for( int b = 0; b < kWarpBins; ++b ) {
				const double sig = std::sqrt( as[b] * as[b] + rs[b] * rs[b] );
				std::cout << " " << std::setprecision( 2 ) << ( am[b] - rm[b] ) / std::max( 1e-12, sig );
				Check( std::fabs( am[b] - rm[b] ) <= 0.002 + 5.0 * sig,
					std::string( "[W] warped-coat exit histogram == independent walk, " ) + names[k] +
					" theta " + std::to_string( (int)th ) + " bin " + std::to_string( b ) );
			}
			std::cout << "\n";
		}
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
		// F2b was a KNOWN-DEFECT PIN at [1.05, 1.15] until DL-285 (2026-09-28):
		// polished_material's GetBSDF() was the bare Lambertian its SPF does
		// not sample, so term (a) priced the wrong substrate.  With the
		// PolishedBRDF the bottom is one lossless function (tau F + Rd (1-F)
		// at tau = Rd = 1), and the stack reads 0.996 / 0.995 -- gated at 1.
		{ "F2b dielectric / polished(white, delta coat) -- lossless since DL-285 (the bottom's BSDF is the function its SPF samples)",
		  MakeComposite( *f.dSmooth, *f.polishedWhite, 3, 3, 3, 3, 3, 0.0, *f.s0 ), true },
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
			Check( s.mean <= 1.0 + std::max( 0.01, 5.0 * s.sem ),
				std::string( "[F] " ) + c.name + " theta " + std::to_string( (int)kThetas[t] ) + " energy-bounded" );
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
	// same env furnace, against the EQUIVALENT PAIR OF SEPARATE SURFACES it
	// stands for: two separate open glass quads, the second 0.002 below the
	// first, both facing up.  Under the DL-345 face rule the second sheet is
	// an index-matched entry, so the pair reads like ONE glass sheet:
	// F + (1 - F) / eta^2 = 0.467 at normal incidence -- the ray that crossed
	// is inside the glass, and a white environment of radiance 1 seen inside
	// a medium of index 1.5 is not an equilibrium (that would be n^2 = 2.25),
	// so 1 was never the expectation.  The composite must equal the pair.
	// Until DL-341 (2026-10-02) the composite's BOTTOM refracted 1.0 -> 1.5
	// (the outside index, not the gap's: a second Fresnel reflection and a
	// second bend) and read 0.48737 against the pair's 0.46693 (+4.4 %),
	// pinned in [0.43, 0.54] under a "truth 1" that DL-341 corrects.
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
			"clippedplane_geometry\n{\n\tname qR2\n\tpta 0 -3 -0.002\n\tptb 4 -3 -0.002\n\tptc 4 3 -0.002\n\tptd 0 3 -0.002\n}\n\n"
			"standard_object\n{\n\tname objL\n\tgeometry qL\n\tmaterial mat_gg\n}\n\n"
			"standard_object\n{\n\tname objR\n\tgeometry qR\n\tmaterial mat_glass\n}\n\n"
			"standard_object\n{\n\tname objR2\n\tgeometry qR2\n\tmaterial mat_glass2\n}\n\n";
		for( int r = 0; r < 2; ++r ) {
			const std::string scene = scene3 + ( r == 0 ? PtRasterizer( true, 64 ) : BdptRasterizer( true, 64 ) );
			CapturingRasterizerOutput* cap = 0;
			const bool ok = Render( scene, r == 0 ? "gg_pt" : "gg_bdpt", cap, 40960u + r );
			const double mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
			const double mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
			std::cout << "    D3 transmitting composite glass/glass under env, " << ( r == 0 ? "PT  " : "BDPT" )
			          << ": composite = " << std::setprecision(5) << mL << ", separate glass pair = " << mR
			          << ", ratio = " << ( mR > 0 ? mL / mR : -1 ) << "  (truth: ratio 1; the pair is F + (1-F)/eta^2 = 0.467 at normal incidence)\n";
			Check( ok && mR > 0 && std::fabs( mL / mR - 1.0 ) <= 0.015,
				std::string( "[D3] transmitting composite == the equivalent pair of separate sheets (" ) + ( r == 0 ? "PT" : "BDPT" ) + ")" );
			if( cap ) safe_release( cap );
		}
	}

	// D4: the same stack as a CLOSED object -- a composite{glass/glass} box
	// beside a plain glass box, both 3.9 x 6 x 1, under the env furnace, the
	// camera outside.  Every path that enters must leave through the far
	// faces, which it meets FROM INSIDE (the H4 case at render level: the
	// stack holds the object, the ray travels up through the bottom first),
	// so the composite box must render like the glass box it stands for
	// (second interface index-matched).  Lossless and identical in both, so
	// the ratio is 1; a box also traps some directions by TIR, the same in
	// both.  Until DL-341 the composite's exits between ~30 and 42 deg
	// ping-ponged to the walk cap (0.981).
	{
		const std::string glassComp =
			"dielectric_material\n{\n\tname mat_glass2\n\ttau 1\n\tior 1.5\n}\n\n"
			"composite_material\n{\n\tname mat_gg\n\ttop mat_glass\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n";
		const std::string scene4 = std::string( "RISE ASCII SCENE 7\n" ) +
			"film\n{\n\twidth 32\n\theight 16\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n" +
			kLayers + glassComp +
			"box_geometry\n{\n\tname bx\n\twidth 3.9\n\theight 6\n\tdepth 1\n}\n\n"
			"standard_object\n{\n\tname boxL\n\tgeometry bx\n\tposition -2 0 0\n\tmaterial mat_gg\n}\n\n"
			"standard_object\n{\n\tname boxR\n\tgeometry bx\n\tposition 2 0 0\n\tmaterial mat_glass\n}\n\n";
		for( int r = 0; r < 2; ++r ) {
			const std::string scene = scene4 + ( r == 0 ? PtRasterizer( true, 128 ) : BdptRasterizer( true, 128 ) );
			CapturingRasterizerOutput* cap = 0;
			const bool ok = Render( scene, r == 0 ? "box_pt" : "box_bdpt", cap, 51200u + r );
			const double mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
			const double mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
			std::cout << "    D4 closed composite{glass/glass} box vs glass box under env, " << ( r == 0 ? "PT  " : "BDPT" )
			          << ": composite = " << std::setprecision(5) << mL << ", glass box = " << mR
			          << ", ratio = " << ( mR > 0 ? mL / mR : -1 ) << "  (truth: ratio 1)\n";
			// Every path here is lossless and all-delta, so each sample
			// carries exactly the env radiance: both boxes read 1.00000
			// with zero variance and the band only absorbs rounding.  Base
			// (pre-DL-341) 0.98089 PT / 0.98063 BDPT.
			Check( ok && mR > 0 && std::fabs( mL / mR - 1.0 ) <= 0.005,
				std::string( "[D4] closed composite box == the glass box it stands for (" ) + ( r == 0 ? "PT" : "BDPT" ) + ")" );
			if( cap ) safe_release( cap );
		}
	}

	// D5 / D6 (DL-341 review round 1, 2026-10-02): the same closed-box
	// furnace on the cases the round-0 fix got wrong or never measured.
	// D5: a DOUBLE-SIDED indexed-mesh box (indexedmesh_geometry's default):
	// both normals flip toward the ray, so a hit from INSIDE presented the
	// composite's top; the round-0 fix then popped O and refracted the exit
	// as an entry -- 0.46687 against base 1.00000 (PT and BDPT).  The left
	// half is the composite, the right half the same double-sided mesh in
	// plain glass (a control that must read 1 too).  D6: a NESTED
	// composite{composite{glass/glass}/glass} box_geometry box beside a
	// glass box (base 0.971; round 0 1.0056, a gain).  Every path is
	// lossless and all-delta, so each sample is exactly the env radiance
	// and the band only absorbs rounding.
	{
		const std::string mesh =
			"indexedmesh_geometry\n{\n\tname mb\n"
			"\tvertex -1.95 -3 -0.5\n\tvertex 1.95 -3 -0.5\n\tvertex 1.95 3 -0.5\n\tvertex -1.95 3 -0.5\n"
			"\tvertex -1.95 -3 0.5\n\tvertex 1.95 -3 0.5\n\tvertex 1.95 3 0.5\n\tvertex -1.95 3 0.5\n"
			"\ttriangle 0 2 1\n\ttriangle 0 3 2\n\ttriangle 4 5 6\n\ttriangle 4 6 7\n\ttriangle 0 1 5\n\ttriangle 0 5 4\n"
			"\ttriangle 3 7 6\n\ttriangle 3 6 2\n\ttriangle 0 4 7\n\ttriangle 0 7 3\n\ttriangle 1 2 6\n\ttriangle 1 6 5\n"
			"\tdouble_sided TRUE\n\tface_normals TRUE\n}\n\n";
		const std::string glassComp =
			"dielectric_material\n{\n\tname mat_glass2\n\ttau 1\n\tior 1.5\n}\n\n"
			"composite_material\n{\n\tname mat_gg\n\ttop mat_glass\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n"
			"composite_material\n{\n\tname mat_nest\n\ttop mat_gg\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n";
		const std::string head = std::string( "RISE ASCII SCENE 7\n" ) +
			"film\n{\n\twidth 32\n\theight 16\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n" + kLayers + glassComp;
		const std::string scene5 = head + mesh +
			"standard_object\n{\n\tname boxL\n\tgeometry mb\n\tposition -2 0 0\n\tmaterial mat_gg\n}\n\n"
			"standard_object\n{\n\tname boxR\n\tgeometry mb\n\tposition 2 0 0\n\tmaterial mat_glass\n}\n\n";
		// D5b (review round 3): the same closed box with ONE T-junction (a
		// vertex in the middle of a top edge, so the mesh is NOT certified
		// watertight -- `bOpenSheet` -- although it is closed).  Round 3
		// keyed the unflip on that certificate and read 0.46639 here.
		const std::string meshTJ =
			"indexedmesh_geometry\n{\n\tname tb\n"
			"\tvertex -1.95 -3 -0.5\n\tvertex 1.95 -3 -0.5\n\tvertex 1.95 3 -0.5\n\tvertex -1.95 3 -0.5\n"
			"\tvertex -1.95 -3 0.5\n\tvertex 1.95 -3 0.5\n\tvertex 1.95 3 0.5\n\tvertex -1.95 3 0.5\n\tvertex 0 -3 0.5\n"
			"\ttriangle 0 2 1\n\ttriangle 0 3 2\n\ttriangle 4 8 6\n\ttriangle 8 5 6\n\ttriangle 4 6 7\n\ttriangle 0 1 5\n\ttriangle 0 5 4\n"
			"\ttriangle 3 7 6\n\ttriangle 3 6 2\n\ttriangle 0 4 7\n\ttriangle 0 7 3\n\ttriangle 1 2 6\n\ttriangle 1 6 5\n"
			"\tdouble_sided TRUE\n\tface_normals TRUE\n}\n\n";
		const std::string scene5b = head + meshTJ +
			"standard_object\n{\n\tname boxL\n\tgeometry tb\n\tposition -2 0 0\n\tmaterial mat_gg\n}\n\n"
			"standard_object\n{\n\tname boxR\n\tgeometry tb\n\tposition 2 0 0\n\tmaterial mat_glass\n}\n\n";
		const std::string scene6 = head +
			"box_geometry\n{\n\tname bx\n\twidth 3.9\n\theight 6\n\tdepth 1\n}\n\n"
			"standard_object\n{\n\tname boxL\n\tgeometry bx\n\tposition -2 0 0\n\tmaterial mat_nest\n}\n\n"
			"standard_object\n{\n\tname boxR\n\tgeometry bx\n\tposition 2 0 0\n\tmaterial mat_glass\n}\n\n";
		for( int sc = 0; sc < 3; ++sc ) {
			for( int r = 0; r < 2; ++r ) {
				// D5 is zero-variance (one render); D6's nested top runs the
				// per-branch estimator, so it is n = 4 SALTED renders
				// (independent randomized-QMC replicates) and gated on
				// mean +- sem, a band that resolves 0.5 %.
				const int nRep = ( sc == 1 ) ? 4 : 1;
				std::vector<double> L, Rr;
				bool ok = true;
				for( int k = 0; k < nRep; ++k ) {
					const int spp = ( sc == 1 ) ? 2048 : 128;
					const std::string scene = ( sc == 0 ? scene5 : sc == 1 ? scene6 : scene5b ) + ( r == 0 ? PtRasterizer( true, spp ) : BdptRasterizer( true, spp ) );
					CapturingRasterizerOutput* cap = 0;
					SobolSamplerTestHooks::ValueSalt().store( sc != 1 ? 0u : 0x9E3779B9u * (unsigned)( k + 1 ) + (unsigned)r );
					const bool okk = Render( scene, sc != 1 ? ( r == 0 ? "dsbox_pt" : "dsbox_bdpt" ) : ( r == 0 ? "nest_pt" : "nest_bdpt" ), cap, 61440u + 2u * sc + r + 16u * k );
					SobolSamplerTestHooks::ValueSalt().store( 0u );
					ok = ok && okk;
					L.push_back( okk ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1 );
					Rr.push_back( okk ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1 );
					if( cap ) safe_release( cap );
				}
				double mL = 0, mR = 0; for( int k = 0; k < nRep; ++k ) { mL += L[k]; mR += Rr[k]; }
				mL /= nRep; mR /= nRep;
				double vL = 0; for( int k = 0; k < nRep; ++k ) vL += ( L[k] - mL ) * ( L[k] - mL );
				const double semL = nRep > 1 ? std::sqrt( vL / ( nRep - 1 ) / nRep ) : 0.0;
				const char* tag = ( sc == 0 ) ? "D5 double-sided mesh box, composite{glass/glass} | plain glass"
				                : ( sc == 2 ) ? "D5b double-sided mesh box with a T-junction (not certified watertight), composite{glass/glass} | plain glass"
				                              : "D6 nested composite{composite{glass/glass}/glass} box | glass box";
				std::cout << "    " << tag << ", " << ( r == 0 ? "PT  " : "BDPT" ) << ": "
				          << std::setprecision(5) << mL << " (sem " << semL << ", n " << nRep << ") | " << mR << "  (truth 1 | 1)\n";
				Check( ok && std::fabs( mL - 1.0 ) <= std::max( 0.002, 4.0 * semL ),
					std::string( "[D5/D6] " ) + tag + ": left == 1 (" + ( r == 0 ? "PT" : "BDPT" ) + ")" );
				Check( ok && std::fabs( mR - 1.0 ) <= 0.002,
					std::string( "[D5/D6] " ) + tag + ": glass control == 1 (" + ( r == 0 ? "PT" : "BDPT" ) + ")" );
				if( sc == 1 ) {
					Check( semL < 0.00125, std::string( "[D6] the salted band resolves 0.5 % (5 sem < 0.5 %), " ) + ( r == 0 ? "PT" : "BDPT" ) );
				}
			}
		}
	}

	// D7 (DL-341 review round 2, 2026-10-02): an OPEN double-sided composite
	// SHEET seen from BEHIND.  Left half: an `indexedmesh_geometry` quad
	// (double-sided by default, not certified watertight -> `bOpenSheet`);
	// right half: its `clippedplane_geometry` twin (provably open).  The
	// camera is at z = -7, behind both.  An open sheet has no inside for the
	// bottom to face, so both present the composite's TOP to the ray on
	// either face and the two halves must agree, in PT, BDPT and VCM alike.
	// Round 2 unflipped the mesh half and walked it from below, delta-tagged:
	// coat over a 0.8 Lambertian under the env furnace read PT / BDPT / VCM
	// 0.800 / 0.909 / 1.010 on the mesh against ~0.635 on the plane (BDPT
	// repriced its connections on a rebuilt record that had lost the flip),
	// glass/glass 0.977 against 0.487, and an omni light on the camera side
	// lit the mesh half at 0 against 1.25.  Base: the halves agree.
	{
		const std::string mats7 =
			"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_w8\n\tcolor 0.8 0.8 0.8\n}\n\n"
			"lambertian_material\n{\n\tname mat_l8\n\treflectance pnt_w8\n}\n\n"
			"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
			"dielectric_material\n{\n\tname mat_glass2\n\ttau 1\n\tior 1.5\n}\n\n"
			"composite_material\n{\n\tname mat_cc\n\ttop mat_glass\n\tbottom mat_l8\n\tthickness 0\n\textinction 0.0\n}\n\n"
			"composite_material\n{\n\tname mat_gg\n\ttop mat_glass\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n";
		const std::string geo7 =
			"indexedmesh_geometry\n{\n\tname qm\n\tvertex -4 -3 0\n\tvertex 0 -3 0\n\tvertex 0 3 0\n\tvertex -4 3 0\n"
			"\ttriangle 0 1 2\n\ttriangle 0 2 3\n\tdouble_sided TRUE\n\tface_normals TRUE\n}\n\n"
			"clippedplane_geometry\n{\n\tname qc\n\tpta 0 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd 0 3 0\n}\n\n";
		const std::string cam7 = "film\n{\n\twidth 32\n\theight 16\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 -7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n";
		auto vcm = []( bool env, int spp ) {
			std::ostringstream s;
			s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			  << "vcm_pel_rasterizer\n{\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tsamples " << spp
			  << "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n\toidn_denoise FALSE\n\tpixel_filter box\n";
			if( env ) s << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";
			s << "}\n\nfile_rasterizeroutput\n{\n\tpattern rendered/composite_energy_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";
			return s.str();
		};
		struct Cfg7 { const char* name; const char* mat; bool env; };
		const Cfg7 cfgs[] = {
			{ "coat over Lambertian 0.8, env furnace", "mat_cc", true },
			{ "glass/glass, env furnace", "mat_gg", true },
			{ "coat over Lambertian 0.8, omni on the camera side", "mat_cc", false },
		};
		// D7b (review round 3): a single open BEZIER patch (a 4x4 control
		// net of a flat quad) in place of the mesh.  `BezierPatchGeometry`
		// never sets `bOpenSheet` (DL-220), so round 3's certificate-keyed
		// unflip read it as closed: 0.800 against the plane's 0.63, omni 0,
		// glass/glass 0.977 against 0.467.
		char bzPath[512];
		std::snprintf( bzPath, sizeof( bzPath ), "/tmp/composite_energy_sheet_%d.bezier", (int)getpid() );
		{
			std::ofstream bz( bzPath );
			bz << "1\n";
			for( int j = 0; j < 4; ++j ) for( int i = 0; i < 4; ++i ) bz << ( -4.0 + 4.0 * i / 3.0 ) << " " << ( -3.0 + 2.0 * j ) << " 0\n";
		}
		const std::string geoBz = std::string( "bezierpatch_geometry\n{\n\tname qm\n\tfile " ) + bzPath + "\n}\n\n"
			"clippedplane_geometry\n{\n\tname qc\n\tpta 0 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd 0 3 0\n}\n\n";
		// The patch's raw normal is -z for this control net (the plane's is
		// +z), so its BACK face is seen from z = +7: the Bezier variant is
		// rendered from BOTH sides (g == 1: camera -7, g == 2: camera +7),
		// one of which is the plane's front and the patch's back.
		const std::string cam7f = "film\n{\n\twidth 32\n\theight 16\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n";
		for( int g = 0; g < 3; ++g )
		for( const Cfg7& c : cfgs ) {
			const char* gname = ( g == 0 ) ? "mesh" : ( g == 1 ) ? "Bezier patch, camera -z" : "Bezier patch, camera +z";
			double perInt[3] = { -1, -1, -1 };
			for( int r = 0; r < 3; ++r ) {
				if( !c.env && r == 2 ) continue;	// VCM has no point-light row here
				std::string scene = std::string( "RISE ASCII SCENE 7\n" ) + ( g == 2 ? cam7f : cam7 ) + mats7 + ( g == 0 ? geo7 : geoBz ) +
					"standard_object\n{\n\tname M\n\tgeometry qm\n\tmaterial " + c.mat + "\n}\n\n"
					"standard_object\n{\n\tname C\n\tgeometry qc\n\tmaterial " + c.mat + "\n}\n\n";
				if( !c.env ) scene += std::string( "omni_light\n{\n\tname ol\n\tpower 200\n\tcolor 1 1 1\n\tposition 0 0 " ) + ( g == 2 ? "5" : "-5" ) + "\n}\n\n";
				scene += ( r == 0 ) ? PtRasterizer( c.env, 256 ) : ( r == 1 ) ? BdptRasterizer( c.env, 256 ) : vcm( c.env, 256 );
				CapturingRasterizerOutput* cap = 0;
				const bool ok = Render( scene, "opensheet", cap, 71680u + 3u * (unsigned)r );
				// Seen from behind the image is mirrored: its LEFT half is
				// the clipped plane (world +x), its RIGHT half the mesh.
				const double leftHalf  = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
				const double rightHalf = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
				// From +z the image's left half is world -x (the mesh / patch).
				const double mC = ( g == 2 ) ? rightHalf : leftHalf;
				const double mM = ( g == 2 ) ? leftHalf : rightHalf;
				const char* in = ( r == 0 ) ? "PT  " : ( r == 1 ) ? "BDPT" : "VCM ";
				std::cout << "    D7 open double-sided sheet from behind (" << gname << "), " << c.name << ", " << in
				          << ": " << gname << " = " << std::setprecision(5) << mM << ", clipped plane = " << mC
				          << ", ratio = " << ( mC > 0 ? mM / mC : -1 ) << "\n";
				Check( ok && mC > 0 && std::fabs( mM / mC - 1.0 ) <= 0.03,
					std::string( "[D7] open " ) + gname + " sheet == clipped-plane twin from behind, " + c.name + " (" + in + ")" );
				perInt[r] = mM;
				if( cap ) safe_release( cap );
			}
			for( int r = 1; r < 3; ++r ) {
				if( perInt[r] < 0 ) continue;
				Check( perInt[0] > 0 && std::fabs( perInt[r] / perInt[0] - 1.0 ) <= 0.04,
					std::string( "[D7] open " ) + gname + " sheet from behind, " + ( r == 1 ? "BDPT" : "VCM" ) + " == PT, " + c.name );
			}
		}
		std::remove( bzPath );
	}
	// D8 (DL-341 review round 4, 2026-10-02): a NESTED composite (another
	// composite as the TOP) and a translucent-topped composite on an OPEN
	// double-sided sheet -- the mesh quad and its clipped-plane twin side by
	// side -- seen from BEHIND must read as from the FRONT (an open sheet
	// presents its top on both faces).  Round 4 kept the outer's record
	// flipped but left `bGeomNormalOrientedToRay` set, so the inner
	// composite re-decided the unflip from the walk's internal stack (which
	// holds O from its own crossing) and unflipped mid-walk:
	// composite{composite{glass/water}/Lambertian 0.8} back PT 0.374 /
	// BDPT 0.367 / VCM 0.368 against front 0.684; a translucent top read
	// UnflippedGeomNormal() and opposed the composite's frame: back 0.711
	// against front 0.860 (also on master).  Gate: back / front per half
	// within 3 %, PT, BDPT and VCM (translucent: PT and BDPT).
	{
		const std::string mats8 =
			"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_w8\n\tcolor 0.8 0.8 0.8\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_tr\n\tcolor 0.3 0.3 0.3\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_tt\n\tcolor 0.7 0.7 0.7\n}\n\n"
			"lambertian_material\n{\n\tname mat_l8\n\treflectance pnt_w8\n}\n\n"
			"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
			"dielectric_material\n{\n\tname mat_glass2\n\ttau 1\n\tior 1.5\n}\n\n"
			"dielectric_material\n{\n\tname mat_water\n\ttau 1\n\tior 1.33\n}\n\n"
			"translucent_material\n{\n\tname mat_tr\n\tref pnt_tr\n\ttau pnt_tt\n\text 0\n\tN 10\n\tscattering 0\n}\n\n"
			"composite_material\n{\n\tname mat_gw\n\ttop mat_glass\n\tbottom mat_water\n\tthickness 0\n\textinction 0.0\n}\n\n"
			"composite_material\n{\n\tname mat_gg\n\ttop mat_glass\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n"
			"composite_material\n{\n\tname mat_gwl\n\ttop mat_gw\n\tbottom mat_l8\n\tthickness 0\n\textinction 0.0\n}\n\n"
			"composite_material\n{\n\tname mat_ggl\n\ttop mat_gg\n\tbottom mat_l8\n\tthickness 0\n\textinction 0.0\n}\n\n"
			"composite_material\n{\n\tname mat_trl\n\ttop mat_tr\n\tbottom mat_l8\n\tthickness 0\n\textinction 0.0\n}\n\n";
		const std::string geo8 =
			"indexedmesh_geometry\n{\n\tname qm\n\tvertex -4 -3 0\n\tvertex 0 -3 0\n\tvertex 0 3 0\n\tvertex -4 3 0\n"
			"\ttriangle 0 1 2\n\ttriangle 0 2 3\n\tdouble_sided TRUE\n\tface_normals TRUE\n}\n\n"
			"clippedplane_geometry\n{\n\tname qc\n\tpta 0 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd 0 3 0\n}\n\n";
		auto vcm8 = []( int spp ) {
			std::ostringstream s;
			s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			  << "vcm_pel_rasterizer\n{\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tsamples " << spp
			  << "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n\toidn_denoise FALSE\n\tpixel_filter box\n"
			  << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
			  << "}\n\nfile_rasterizeroutput\n{\n\tpattern rendered/composite_energy_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";
			return s.str();
		};
		struct Cfg8 { const char* name; const char* mat; int nInt; };
		const Cfg8 cfgs[] = {
			{ "nested composite{composite{glass/water}/Lambertian 0.8}", "mat_gwl", 3 },
			{ "nested composite{composite{glass/glass}/Lambertian 0.8}", "mat_ggl", 3 },
			{ "composite{translucent/Lambertian 0.8}", "mat_trl", 2 },
		};
		for( const Cfg8& c : cfgs ) {
			for( int r = 0; r < c.nInt; ++r ) {
				double mesh[2] = { -1, -1 }, plane[2] = { -1, -1 };	// [0] front, [1] back
				for( int side = 0; side < 2; ++side ) {
					const char* z = ( side == 0 ) ? "7.0" : "-7.0";
					const std::string scene = std::string( "RISE ASCII SCENE 7\n" ) +
						"film\n{\n\twidth 32\n\theight 16\n}\n\n"
						"pinhole_camera\n{\n\tlocation 0 0 " + z + "\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n" +
						mats8 + geo8 +
						"standard_object\n{\n\tname M\n\tgeometry qm\n\tmaterial " + c.mat + "\n}\n\n"
						"standard_object\n{\n\tname C\n\tgeometry qc\n\tmaterial " + c.mat + "\n}\n\n" +
						( r == 0 ? PtRasterizer( true, 256 ) : r == 1 ? BdptRasterizer( true, 256 ) : vcm8( 256 ) );
					CapturingRasterizerOutput* cap = 0;
					const bool ok = Render( scene, "nested_sheet", cap, 81920u + 7u * (unsigned)r + (unsigned)side );
					const double lh = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
					const double rh = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
					// From +z the image's left half is world -x (the mesh);
					// from -z it is mirrored.
					mesh[side]  = ( side == 0 ) ? lh : rh;
					plane[side] = ( side == 0 ) ? rh : lh;
					if( cap ) safe_release( cap );
				}
				const char* in = ( r == 0 ) ? "PT  " : ( r == 1 ) ? "BDPT" : "VCM ";
				std::cout << "    D8 " << c.name << ", " << in << ": mesh front / back " << std::setprecision(5)
				          << mesh[0] << " / " << mesh[1] << ", clipped plane front / back " << plane[0] << " / " << plane[1] << "\n";
				Check( mesh[0] > 0 && std::fabs( mesh[1] / mesh[0] - 1.0 ) <= 0.03,
					std::string( "[D8] open mesh sheet, back == front, " ) + c.name + " (" + in + ")" );
				Check( plane[0] > 0 && std::fabs( plane[1] / plane[0] - 1.0 ) <= 0.03,
					std::string( "[D8] open clipped-plane sheet, back == front, " ) + c.name + " (" + in + ")" );
			}
		}
	}
}


//////////////////////////////////////////////////////////////////////
//  Section K -- DL-342: `coated_material` UNDER AN ABSORBING COAT.
//
//  The DL-24 composite matches Section G's independent closed form, so
//  it is the reference.  `coated_material`'s recycling term used to be
//  r_i * a(mu_bar)^2 with mu_bar the refraction of an outer cosine 0.5,
//  where the exact round trip is E_ret = INT 2 mu a(mu)^2 F_in(mu) dmu
//  (the TIR-dominated, LONGEST paths carry most of r_i).  Three gates:
//
//   K1 (deterministic, sharp).  For a smooth coat over a Lambertian the
//      coated value at an off-specular pair IS ClosedFormLayered, exactly,
//      at every coat optical depth: RGB and NM, grey and tinted coats.
//   K2 (deterministic).  IBSDF::hemisphericalAlbedo against the white-sky
//      closed form r_e + R T_h^2 / (n^2 (1 - R E_ret)).
//   K3 (Monte Carlo).  Directional albedo of coated vs the composite of
//      the same physical layers: Lambertian gated to MC noise, GGX pinned
//      on its measured residual band (DL-388, see the comment there).
//
//  Optical depth is coat_absorption * coat_thickness (thickness 1 here)
//  for coated and extinction * thickness for the composite: the same
//  normal-incidence Beer exponent, raised to 1/mu inside the film.
//////////////////////////////////////////////////////////////////////
static double FresnelOutside15( const double c, const double n )
{
	// Outer->coat unpolarised Fresnel, written out independently.
	const double s2 = ( 1.0 - c * c ) / ( n * n );
	const double ct = std::sqrt( std::max( 0.0, 1.0 - s2 ) );
	const double rs = ( c - n * ct ) / ( c + n * ct );
	const double rp = ( n * c - ct ) / ( n * c + ct );
	return 0.5 * ( rs * rs + rp * rp );
}

struct WhiteSky { double re, entry, ret; };

static WhiteSky WhiteSkyTerms( const double tau, const double n )
{
	WhiteSky w = { 0, 0, 0 };
	const int N = 40000;
	for( int i = 0; i < N; ++i ) {
		const double x = ( i + 0.5 ) / N;
		// outer cosine x: entry
		const double F = FresnelOutside15( x, n );
		const double mu = std::sqrt( 1.0 - ( 1.0 - x * x ) / ( n * n ) );
		w.re    += 2.0 * x * F / N;
		w.entry += 2.0 * x * ( 1.0 - F ) * std::exp( -tau / mu ) / N;
		// internal cosine x: one round trip
		w.ret   += 2.0 * x * std::exp( -2.0 * tau / x ) * FresnelInside15( x, n ) / N;
	}
	return w;
}

static CoatedMaterial* MakeCoated( const IMaterial& base, double sigma, const IPainter& tint )
{
	UniformScalarPainter* w  = new UniformScalarPainter( 1.0 );
	UniformScalarPainter* n  = new UniformScalarPainter( 1.5 );
	UniformScalarPainter* a  = new UniformScalarPainter( 0.001 );
	UniformScalarPainter* th = new UniformScalarPainter( 1.0 );
	UniformScalarPainter* ab = new UniformScalarPainter( sigma );
	CoatedMaterial* m = new CoatedMaterial( base, *w, *n, *a, *th, *ab, tint );
	m->addref();
	return m;
}

static void SectionK( Fixtures& f )
{
	std::cout << "\n[K] DL-342: coated_material under an ABSORBING coat (smooth 1.5 coat, thickness 1)\n";
	const double eta = 1.5;
	const double sigmas[] = { 0.0, 0.2, 0.5, 1.0, 2.0 };

	// ---- K1: value vs ClosedFormLayered, Lambertian (0.8, 0.2, 0.2) ----
	{
		const double pairs[][2] = { { 0, 40 }, { 30, 60 }, { 60, 30 }, { 75, 10 } };
		double worst = 0;
		std::cout << "    K1 value / closed form, red Lambertian substrate, off-specular pairs (ch0 rho .8, ch1 rho .2):\n";
		for( double sg : sigmas ) {
			CoatedMaterial* m = MakeCoated( *f.lambRed, sg, *f.white );
			std::cout << "      sigma_t " << std::setprecision(3) << sg << ":";
			for( const auto& pr : pairs ) {
				const double ti = pr[0] * kPi / 180.0, to = pr[1] * kPi / 180.0;
				const Vector3 wo( std::sin( to ) * std::cos( 2.0944 ), std::sin( to ) * std::sin( 2.0944 ), std::cos( to ) );
				const RayIntersectionGeometric ri = MakeIntersection( ti );
				const RISEPel v = m->GetBSDF()->value( wo, ri );
				const double vNM = m->GetBSDF()->valueNM( wo, ri, 550.0 );
				const double rhoNM = f.lambRed->GetBSDF()->valueNM( wo, ri, 550.0 ) * kPi;
				const double t0 = ClosedFormLayered( ti, to, 0.8, sg, 1.0, eta );
				const double t1 = ClosedFormLayered( ti, to, 0.2, sg, 1.0, eta );
				const double tNM = ClosedFormLayered( ti, to, rhoNM, sg, 1.0, eta );
				const double e0 = v[0] / t0 - 1.0, e1 = v[1] / t1 - 1.0, eNM = vNM / tNM - 1.0;
				worst = std::max( worst, std::max( std::fabs( e0 ), std::max( std::fabs( e1 ), std::fabs( eNM ) ) ) );
				std::cout << "  (" << (int)pr[0] << "," << (int)pr[1] << ") " << std::setprecision(4) << std::showpos
				          << 100.0 * e0 << "%/" << 100.0 * e1 << "%/NM " << 100.0 * eNM << "%" << std::noshowpos;
				const std::string tag = std::string( "[K1] sigma " ) + std::to_string( sg ) + " (" +
					std::to_string( (int)pr[0] ) + "," + std::to_string( (int)pr[1] ) + ") ";
				Check( std::fabs( e0 ) <= 1e-3, tag + "ch0 coated value == closed form" );
				Check( std::fabs( e1 ) <= 1e-3, tag + "ch1 coated value == closed form" );
				Check( std::fabs( eNM ) <= 1e-3, tag + "NM coated value == closed form" );
			}
			std::cout << "\n";
			m->release();
		}
		// A tinted, non-absorbing coat: per-channel optical depth -ln(tint).
		{
			UniformColorPainter* tint = new UniformColorPainter( RISEPel( 0.9, 0.6, 0.3 ) );  tint->addref();
			CoatedMaterial* m = MakeCoated( *f.lamb, 0.0, *tint );
			const double ti = 30.0 * kPi / 180.0, to = 60.0 * kPi / 180.0;
			const Vector3 wo( std::sin( to ) * std::cos( 2.0944 ), std::sin( to ) * std::sin( 2.0944 ), std::cos( to ) );
			const RISEPel v = m->GetBSDF()->value( wo, MakeIntersection( ti ) );
			const double tints[3] = { 0.9, 0.6, 0.3 };
			std::cout << "      tinted coat (0.9, 0.6, 0.3), white substrate, (30,60):";
			for( int ch = 0; ch < 3; ++ch ) {
				const double t = ClosedFormLayered( ti, to, 1.0, -std::log( tints[ch] ), 1.0, eta );
				const double e = v[ch] / t - 1.0;
				worst = std::max( worst, std::fabs( e ) );
				std::cout << " ch" << ch << " " << std::showpos << std::setprecision(4) << 100.0 * e << "%" << std::noshowpos;
				Check( std::fabs( e ) <= 1e-3, std::string( "[K1] tinted coat ch" ) + std::to_string( ch ) + " coated value == closed form" );
			}
			std::cout << "\n";
			m->release(); tint->release();
		}
		std::cout << "      worst |coated/closed form - 1| = " << std::setprecision(3) << 100.0 * worst << "%\n";
	}

	// ---- K2: hemisphericalAlbedo vs the white-sky closed form ----------
	{
		std::cout << "    K2 hemisphericalAlbedo / white-sky closed form, white and red Lambertian:\n      ";
		for( double sg : sigmas ) {
			const WhiteSky w = WhiteSkyTerms( sg, eta );
			for( int sub = 0; sub < 2; ++sub ) {
				CoatedMaterial* m = MakeCoated( sub == 0 ? (const IMaterial&)*f.lamb : (const IMaterial&)*f.lambRed, sg, *f.white );
				RISEPel H;
				const bool ok = m->GetBSDF()->hemisphericalAlbedo( MakeIntersection( 0.3 ), H );
				const double R = ( sub == 0 ) ? 1.0 : 0.8;
				const double truth = w.re + R * w.entry * w.entry / ( eta * eta * ( 1.0 - R * w.ret ) );
				const double e = H[0] / truth - 1.0;
				std::cout << "s" << sg << ( sub ? "/red " : "/white " ) << std::showpos << std::setprecision(4) << 100.0 * e << "%" << std::noshowpos << "  ";
				Check( ok && std::fabs( e ) <= 1e-3,
					std::string( "[K2] hemisphericalAlbedo sigma " ) + std::to_string( sg ) + ( sub ? " red" : " white" ) + " == white-sky closed form" );
				m->release();
			}
		}
		std::cout << "\n";
	}

	// ---- K3: directional albedo, coated vs composite (MC) -------------
	{
		UniformScalarPainter* sDelta = new UniformScalarPainter( 1000000.0 );  sDelta->addref();
		DielectricMaterial* smooth = new DielectricMaterial( *f.s1, *f.s15, *sDelta, false );  smooth->addref();
		UniformColorPainter* ggxDiff = new UniformColorPainter( RISEPel( 0.8, 0.8, 0.8 ) );  ggxDiff->addref();
		UniformColorPainter* ggxSpec = new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) );  ggxSpec->addref();
		GGXMaterial* ggx = new GGXMaterial( *ggxDiff, *ggxSpec, *f.sAlpha, *f.sAlpha, *f.s15, *f.s0, eFresnelSchlickF0 );
		ggx->addref();
		const double thetas[] = { 0.0, 60.0 };
		struct Sub { const char* name; const IMaterial* m; bool gateTight; };
		const Sub subs[] = { { "Lambertian white", f.lamb, true }, { "GGX (diffuse .8, F0 .04, alpha .16)", ggx, false } };
		std::cout << "    K3 directional albedo, coated vs composite (8 x 20000 draws each; mean +- sem):\n";
		for( const Sub& sb : subs ) {
			for( double sg : sigmas ) {
				UniformScalarPainter* ext = new UniformScalarPainter( sg );  ext->addref();
				CompositeMaterial* comp = MakeComposite( *smooth, *sb.m, 3, 3, 3, 3, 3, 1.0, *ext );
				CoatedMaterial* coat = MakeCoated( *sb.m, sg, *f.white );
				for( double th : thetas ) {
					const FurnaceStats sc = Furnace( *coat->GetSPF(), th, false, false, 8, 20000, 7001u + (unsigned)th );
					const FurnaceStats sp = Furnace( *comp->GetSPF(), th, false, false, 8, 20000, 9001u + (unsigned)th );
					const double ratio = sc.mean / sp.mean;
					const double semR = ratio * std::sqrt( std::pow( sc.sem / sc.mean, 2 ) + std::pow( sp.sem / sp.mean, 2 ) );
					std::cout << "      " << sb.name << " sigma_t " << std::setprecision(3) << sg << " theta " << (int)th
					          << ": coated " << std::setprecision(5) << sc.mean << " +- " << sc.sem
					          << "  composite " << sp.mean << " +- " << sp.sem
					          << "  coated/composite " << ratio << " +- " << semR << "\n";
					// Lambertian: the coated model is exact, so it must agree
					// with the composite within MC noise (floor 1 %).
					//
					// GGX: KNOWN RESIDUAL, DL-388, pinned in [0.92, 1.02].
					// coated_material evaluates the substrate BRDF at the
					// UNREFRACTED (outer) directions, which is exact only for
					// a Lambertian: GGX's diffuse (1 - A(o)) factor is read at
					// the outer grazing angle where the light actually leaves
					// the substrate at the critical angle, so the escape is
					// undercounted (0.3107 vs 0.3215 at sigma_t 0.2, separable
					// analysis in docs/DL342_COATED_ABSORBING_COAT.md).  At
					// sigma_t 0 an over-read recycling term hid it; DL-342's
					// exact round trip removes that cancellation, so the GGX
					// rows read 3-6 % LOW of the composite here (they read up
					// to 4 % HIGH pre-fix, the two errors partly cancelling).
					if( sb.gateTight ) {
						Check( std::fabs( ratio - 1.0 ) <= std::max( 0.01, 5.0 * semR ),
							std::string( "[K3] " ) + sb.name + " sigma " + std::to_string( sg ) + " theta " +
							std::to_string( (int)th ) + " coated / composite == 1" );
					} else {
						Check( ratio >= 0.92 && ratio <= 1.02,
							std::string( "[K3] " ) + sb.name + " sigma " + std::to_string( sg ) + " theta " +
							std::to_string( (int)th ) + " coated / composite inside the DL-388 residual pin [0.92, 1.02]" );
					}
				}
				coat->release(); comp->release(); ext->release();
			}
		}
		ggx->release(); ggxSpec->release(); ggxDiff->release(); smooth->release(); sDelta->release();
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
	if( argc > 1 && std::string( argv[1] ) == "--coated-only" ) {
		SectionK( f );
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--stack-only" ) {
		// DL-341 / DL-297 rows only: H, T, W (and D with --render).
		SectionH( f );
		SectionT( f );
		SectionW( f );
		if( argc > 2 && std::string( argv[2] ) == "--render" ) SectionD();
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
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
	SectionW( f );
	SectionG( f );
	SectionK( f );
	if( !skipRender ) {
		SectionD();
	}

	std::cout << "\n================================================" << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
