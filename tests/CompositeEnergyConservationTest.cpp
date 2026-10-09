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
		// DL-296: a transmitting bottom -- its covered rays out through
		// the bottom are non-delta too (they carry the BELOW stack, whose
		// eta^2 the value includes and the kray does not).
		{ "C4 dielectric/translucent, t=0.2 ext 0.3", MakeComposite( *f.dSmooth, *f.trans, 3, 3, 3, 3, 3, 0.2, *new UniformScalarPainter( 0.3 ) ) },
	};

	for( const Cfg& c : cfgs ) {
		const IBSDF* pBSDF = c.m->GetBSDF();
		const ISPF& spf = *c.m->GetSPF();
		for( int pipe = 0; pipe < 2; ++pipe ) {
			int checked = 0, bad = 0, below = 0;
			double worst = 0;
			for( int t = 0; t < 3; ++t ) {
				RayIntersectionGeometric ri = MakeIntersection( kThetas[t] * kPi / 180.0 );
				// DL-296: term (c) is live only on a surface that provably
				// encloses no volume (an open sheet); C4 is that case.
				ri.bProvablyNoInterior = c.m->ScattersFullSphere();
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
						// DL-296: the radiance eta^2 of the ray's own stack.
						const double eta = RadianceEtaScale( stack, s.ior_stack );
						if( d.z < 0 ) below++;
						double lhs, rhs;
						if( pipe ) {
							lhs = s.krayNM * pdf * eta;
							rhs = pBSDF ? pBSDF->valueStatefulNM( d, ri, 550.0, &stack ) * cosO : 0.0;
						} else {
							lhs = ColorMath::MaxValue( s.kray * pdf * eta );
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
			          << checked << " (" << below << " below), mismatches = " << bad << ", worst rel = " << worst << "\n";
			Check( checked > 1000, std::string( "[C] " ) + c.name + ( pipe ? " NM" : " RGB" ) + " emits non-delta rays" );
			Check( bad == 0, std::string( "[C] " ) + c.name + ( pipe ? " NM" : " RGB" ) + " kray*Pdf == value*cos for every non-delta ray" );
			if( c.m->ScattersFullSphere() ) {
				Check( below > 500, std::string( "[C] " ) + c.name + ( pipe ? " NM" : " RGB" ) + " emits non-delta rays below the stack (DL-296)" );
			}
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

//! Mean of the channel-mean radiance (r+g+b)/3 (composited over black) over
//! columns [x0, x1) and all rows except a 2-pixel border.  Returns -1 on a
//! failed or nonfinite render.  (DL-433: this used to average each pixel's
//! MAX(r,g,b), which is biased UPWARD by the per-pixel chroma noise of a
//! spectral render and so reads high by an spp-dependent amount; the
//! channel mean is linear in the pixel, hence unbiased.)
static double RegionMean( const CapturingRasterizerOutput& cap, unsigned x0, unsigned x1 )
{
	if( cap.pixels.empty() ) return -1;
	double sum = 0; int n = 0;
	for( unsigned y = 2; y + 2 < cap.height; ++y ) {
		for( unsigned x = x0; x < x1; ++x ) {
			const RISEColor& c = cap.pixels[y * cap.width + x];
			const double v = ( c.base.r + c.base.g + c.base.b ) * ( 1.0 / 3.0 ) * c.a;
			if( !std::isfinite( v ) ) return -1;
			sum += v; n++;
		}
	}
	return n ? sum / n : -1;
}

static bool Render( const std::string& sceneText, const char* tag, CapturingRasterizerOutput*& pCapOut, unsigned seed )
{
	char path[512];
	std::snprintf( path, sizeof(path), "%s/composite_energy_%s_%d.RISEscene", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag, (int)getpid() );
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

static std::string BdptRasterizer( bool env, int spp, int depth = 8 )
{
	std::ostringstream s;
	s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	  << "bdpt_pel_rasterizer\n{\n\tmax_eye_depth " << depth << "\n\tmax_light_depth " << depth << "\n\tsamples " << spp << "\n\toidn_denoise FALSE\n\tpixel_filter box\n";
	if( env ) s << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";
	s << "}\n\nfile_rasterizeroutput\n{\n\tpattern rendered/composite_energy_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";
	return s.str();
}

//////////////////////////////////////////////////////////////////////
//  Section D9 -- DL-341 review round 6 (2026-10-02): a closed DOUBLE-SIDED
//  mesh box WOUND INWARD.  On such a mesh an inside hit is a FRONT face by
//  winding, so the geometry does not flip the normal and the round-3/4
//  flip-flag rule never unflipped it: the hit was walked from above while
//  the stack held the object, and the exit carried an inside stack.  An
//  all-inverted composite{glass/glass} box read 0.467 in a white furnace
//  (back face only inverted 0.507, front only 0.980; master 1.000).
//
//  Each scene: left half the consistently wound box, right half the same
//  box with the named faces' winding reversed, both double-sided, under
//  the white env furnace, camera outside; PT, BDPT and VCM.
//    - composite{glass/glass} and the plain-glass control are lossless and
//      all-delta, so every sample is exactly the env radiance: both halves
//      == 1 within rounding (VCM's merges read 1.00014 on both builds).
//    - composite{glass/translucent} (thickness 0.05, extinction 0.2) has
//      no closed form: the inverted box must equal the consistent one,
//      right / left within 2 % (single 256-spp renders; the reviewer's
//      salted repeats put the per-half sd near 0.2 %).
//  BDPT / VCM run at depth 12 (review round 6, P3): at the helpers'
//  depth 8 the translucent box read ~1.3 % below PT on BOTH boxes alike
//  (truncation of the box's internal bounces), 0.3 % at depth 12.
//////////////////////////////////////////////////////////////////////
static std::string VcmRasterizer( bool env, int spp, int depth = 8 )
{
	std::ostringstream s;
	s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	  << "vcm_pel_rasterizer\n{\n\tmax_eye_depth " << depth << "\n\tmax_light_depth " << depth << "\n\tsamples " << spp
	  << "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n\toidn_denoise FALSE\n\tpixel_filter box\n";
	if( env ) s << "\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n";
	s << "}\n\nfile_rasterizeroutput\n{\n\tpattern rendered/composite_energy_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";
	return s.str();
}

static std::string WindingBox( const char* name, const std::vector<int>& reversed, bool doubleSided = true )
{
	static const int T[12][3] = { {0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5} };
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n\tname " << name << "\n"
	  << "\tvertex -1.95 -3 -0.5\n\tvertex 1.95 -3 -0.5\n\tvertex 1.95 3 -0.5\n\tvertex -1.95 3 -0.5\n"
	  << "\tvertex -1.95 -3 0.5\n\tvertex 1.95 -3 0.5\n\tvertex 1.95 3 0.5\n\tvertex -1.95 3 0.5\n";
	for( int i = 0; i < 12; ++i ) {
		const bool rev = std::find( reversed.begin(), reversed.end(), i ) != reversed.end();
		s << "\ttriangle " << T[i][0] << " " << ( rev ? T[i][2] : T[i][1] ) << " " << ( rev ? T[i][1] : T[i][2] ) << "\n";
	}
	s << "\tdouble_sided " << ( doubleSided ? "TRUE" : "FALSE" ) << "\n\tface_normals TRUE\n}\n\n";
	return s.str();
}

static void SectionD9()
{
	std::cout << "\n[D9] Inward-wound closed double-sided mesh box (DL-341 round 6)\n";
	const std::string mats =
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tr\n\tcolor 0.3 0.3 0.3\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tt\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass2\n\ttau 1\n\tior 1.5\n}\n\n"
		"translucent_material\n{\n\tname mat_tr\n\tref pnt_tr\n\ttau pnt_tt\n\text 0\n\tN 10\n\tscattering 0\n}\n\n"
		"composite_material\n{\n\tname mat_gg\n\ttop mat_glass\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n"
		"composite_material\n{\n\tname mat_gtr\n\ttop mat_glass\n\tbottom mat_tr\n\tthickness 0.05\n\textinction 0.2\n}\n\n";
	struct Mode { const char* name; std::vector<int> faces; };
	const Mode modes[] = {
		{ "all 12 triangles reversed", { 0,1,2,3,4,5,6,7,8,9,10,11 } },
		{ "back (-z) face reversed",   { 0,1 } },
		{ "front (+z) face reversed",  { 2,3 } },
	};
	struct MatCfg { const char* name; const char* mat; bool exact; int spp; };
	const MatCfg cfgs[] = {
		{ "composite{glass/glass}",       "mat_gg",    true,  128 },
		{ "plain glass (control)",        "mat_glass", true,  128 },
		{ "composite{glass/translucent}", "mat_gtr",   false, 256 },
	};
	unsigned seed = 98304u;
	for( const Mode& m : modes ) {
		for( const MatCfg& c : cfgs ) {
			for( int r = 0; r < 3; ++r ) {
				std::string scene = std::string( "RISE ASCII SCENE 7\n" ) +
					"film\n{\n\twidth 32\n\theight 16\n}\n\n"
					"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n" +
					mats + WindingBox( "bg", std::vector<int>() ) + WindingBox( "bw", m.faces ) +
					"standard_object\n{\n\tname L\n\tgeometry bg\n\tposition -2 0 0\n\tmaterial " + c.mat + "\n}\n\n"
					"standard_object\n{\n\tname Rr\n\tgeometry bw\n\tposition 2 0 0\n\tmaterial " + c.mat + "\n}\n\n" +
					( r == 0 ? PtRasterizer( true, c.spp ) : r == 1 ? BdptRasterizer( true, c.spp, 12 ) : VcmRasterizer( true, c.spp, 12 ) );
				CapturingRasterizerOutput* cap = 0;
				const bool ok = Render( scene, "winding", cap, seed++ );
				const double mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
				const double mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
				if( cap ) safe_release( cap );
				const char* in = ( r == 0 ) ? "PT  " : ( r == 1 ) ? "BDPT" : "VCM ";
				std::cout << "    D9 " << m.name << ", " << c.name << ", " << in << ": consistent " << std::setprecision(5)
				          << mL << " | inverted " << mR << ( c.exact ? "  (truth 1 | 1)\n" : "  (truth: equal)\n" );
				const std::string tag = std::string( m.name ) + ", " + c.name + " (" + in + ")";
				if( c.exact ) {
					Check( ok && std::fabs( mL - 1.0 ) <= 0.002, "[D9] consistent box == 1, " + tag );
					Check( ok && std::fabs( mR - 1.0 ) <= 0.002, "[D9] inward-wound box == 1, " + tag );
				} else {
					Check( ok && mL > 0 && std::fabs( mR / mL - 1.0 ) <= 0.02, "[D9] inward-wound box == consistent box, " + tag );
				}
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
//  Section M -- DL-341 review round 7 (2026-10-02): THE SIDEDNESS MATRIX.
//
//  Rounds 1-6 each found one more geometry on which the composite's frame
//  went wrong (double-sided closed meshes, open sheets, T-junctions,
//  Bezier patches, nested tops, inward winding).  This section crosses
//  every axis at once instead of one fixture per finding:
//
//    sidedness  {single, double}
//    winding    {outward, inward, mixed}   (mixed: every odd triangle
//               reversed -- each face carries one triangle each way)
//    geometry   {closed box in a white furnace, camera outside;
//                closed box with a light INSIDE it, no environment;
//                open quad sheet in the furnace, camera on its +z side}
//    material   {composite{glass/glass}, composite{glass/translucent},
//                nested composite{composite{glass/water}/glass}}
//    integrator {PT, BDPT, VCM}  (BDPT / VCM at depth 12)
//
//  Each render holds the cell's object (left) beside its TWIN (right):
//    glass/glass -> plain glass on the SAME geometry, sidedness and
//                   winding (the two-layer stack is index-matched, so it
//                   must render as one glass interface);
//    the others  -> the same composite on the double-sided OUTWARD
//                   version of the geometry (winding must not matter).
//  Expected value per cell (docs/DL24_COMPOSITE_ENERGY.md section 9.4e):
//    closed box, furnace   glass/glass 1 and its twin 1; nested 1
//                          (lossless); glass/translucent == twin
//    closed box, light in  == twin
//    open sheet            == twin (glass/glass: 0.467 = F + (1-F)/eta^2,
//                          the dielectric "separate sheets" convention:
//                          a ray that crossed is inside the object)
//  Plus three glass/glass sheet families against plain glass, over
//  sidedness x winding: one object holding two panes (plain glass: a
//  slab, 1.0), two separate one-pane objects (0.467), and a sheet over a
//  mirror (the return trip meets the sheet from behind with the object on
//  the stack).
//
//  EVERY cell follows the plain dielectric's convention.  Since DL-382 the
//  outward and inward quads are PROVABLY open sheets, so on them that is
//  DL-345's FACE rule (DL-407 (2): an ALL-DELTA transmitting composite
//  follows it too, see CompositeSPF's OpenSheetWalkStack): the back side
//  is the below medium, so an INWARD sheet seen from the camera is viewed
//  from inside glass -- glass/glass and plain glass both read
//  F + (1 - F) eta^2 = 2.20, the nested twin keeps the cell's winding, and
//  the separate inward panes read ~4.6.  glass/translucent (non-delta)
//  presents its top on either face.  The mixed quad is not certified
//  (stack rule).  Bands (every render salted, so each cell
//  is an independent replicate): zero-variance all-delta cells 0.2 %
//  (glass/glass, plain glass; furnace and sheets); glass/translucent
//  furnace / sheet 2 % (256 spp); nested 5 % (1024 spp, per-branch
//  estimator: 144 furnace / sheet ratios over four runs had sd 0.69 % but
//  a heavy tail, max +3.47 %, so 3 % failed one run in four); the mirror-return sheets 0.5 % (BDPT /
//  VCM are not zero-variance there); light-inside cells 1024 spp, each the
//  mean of 3 salted renders: glass/glass 3 % (single-render ratio sd
//  ~0.5 % under PT / BDPT but ~1.1 % under VCM, so the mean's is
//  <= ~0.65 %), translucent / nested 5 % (single-render sd ~1.5 % per half,
//  the mean's ratio sd ~1.2 %).  Every regression this section exists for moves a cell by 5 %
//  or more (0.444 / 0.467 against 1; 0.222 against 0.105; 0.65 against
//  0.92).
//////////////////////////////////////////////////////////////////////
static std::string MatrixQuad( const char* name, double x0, double x1, double z, int winding, bool doubleSided )
{
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n\tname " << name << "\n"
	  << "\tvertex " << x0 << " -3 " << z << "\n\tvertex " << x1 << " -3 " << z << "\n"
	  << "\tvertex " << x1 << " 3 " << z << "\n\tvertex " << x0 << " 3 " << z << "\n";
	// winding 0: normal +z (toward the camera); 1: -z; 2: one of each.
	s << ( winding == 1 ? "\ttriangle 0 2 1\n" : "\ttriangle 0 1 2\n" )
	  << ( winding == 0 ? "\ttriangle 0 2 3\n" : "\ttriangle 0 3 2\n" );
	s << "\tdouble_sided " << ( doubleSided ? "TRUE" : "FALSE" ) << "\n\tface_normals TRUE\n}\n\n";
	return s.str();
}

static std::string MatrixTwoPane( const char* name, double x0, double x1, int winding, bool doubleSided )
{
	// Two quads at z = +0.3 and z = -0.3 in ONE geometry.
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n\tname " << name << "\n";
	const double zs[2] = { 0.3, -0.3 };
	for( int i = 0; i < 2; ++i ) {
		s << "\tvertex " << x0 << " -3 " << zs[i] << "\n\tvertex " << x1 << " -3 " << zs[i] << "\n"
		  << "\tvertex " << x1 << " 3 " << zs[i] << "\n\tvertex " << x0 << " 3 " << zs[i] << "\n";
	}
	for( int i = 0; i < 2; ++i ) {
		const int b = 4 * i;
		s << "\ttriangle " << b << " " << ( winding == 1 ? b + 2 : b + 1 ) << " " << ( winding == 1 ? b + 1 : b + 2 ) << "\n"
		  << "\ttriangle " << b << " " << ( winding == 0 ? b + 2 : b + 3 ) << " " << ( winding == 0 ? b + 3 : b + 2 ) << "\n";
	}
	s << "\tdouble_sided " << ( doubleSided ? "TRUE" : "FALSE" ) << "\n\tface_normals TRUE\n}\n\n";
	return s.str();
}

// --sheets-only: SectionM's open-sheet cells and sheet families only.
static bool g_matrixSheetsOnly = false;

static void SectionM()
{
	std::cout << "\n[M] Sidedness x winding x geometry x material matrix (DL-341 round 7)\n";
	const std::string mats =
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_e\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_w8\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tr\n\tcolor 0.3 0.3 0.3\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tt\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"lambertian_material\n{\n\tname mat_l8\n\treflectance pnt_w8\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass2\n\ttau 1\n\tior 1.5\n}\n\n"
		"dielectric_material\n{\n\tname mat_water\n\ttau 1\n\tior 1.33\n}\n\n"
		"translucent_material\n{\n\tname mat_tr\n\tref pnt_tr\n\ttau pnt_tt\n\text 0\n\tN 10\n\tscattering 0\n}\n\n"
		"perfectreflector_material\n{\n\tname mat_mir\n\treflectance pnt_e\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_e\n\tscale 20.0\n\tmaterial none\n}\n\n"
		"composite_material\n{\n\tname mat_gg\n\ttop mat_glass\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n"
		"composite_material\n{\n\tname mat_gw\n\ttop mat_glass\n\tbottom mat_water\n\tthickness 0\n\textinction 0.0\n}\n\n"
		"composite_material\n{\n\tname mat_nest\n\ttop mat_gw\n\tbottom mat_glass2\n\tthickness 0\n\textinction 0.0\n}\n\n"
		// DL-407 (2): delta-sharp twins (`scattering 1000000`) for the two
		// separate panes family -- see there.
		"dielectric_material\n{\n\tname mat_glassS\n\ttau 1\n\tior 1.5\n\tscattering 1000000\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass2S\n\ttau 1\n\tior 1.5\n\tscattering 1000000\n}\n\n"
		"composite_material\n{\n\tname mat_ggS\n\ttop mat_glassS\n\tbottom mat_glass2S\n\tthickness 0\n\textinction 0.0\n}\n\n"
		"composite_material\n{\n\tname mat_gtr\n\ttop mat_glass\n\tbottom mat_tr\n\tthickness 0.05\n\textinction 0.2\n}\n\n";
	const std::string head = std::string( "RISE ASCII SCENE 7\n" ) +
		"film\n{\n\twidth 32\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n" + mats;
	const std::string lights =
		"sphere_geometry\n{\n\tname sg\n\tradius 0.3\n}\n\n"
		"clippedplane_geometry\n{\n\tname fq\n\tpta -6 -3.5 -4\n\tptb -6 -3.5 4\n\tptc 6 -3.5 4\n\tptd 6 -3.5 -4\n}\n\n"
		"standard_object\n{\n\tname eL\n\tgeometry sg\n\tposition -2 0 0\n\tmaterial mat_emit\n}\n\n"
		"standard_object\n{\n\tname eR\n\tgeometry sg\n\tposition 2 0 0\n\tmaterial mat_emit\n}\n\n"
		"standard_object\n{\n\tname F\n\tgeometry fq\n\tmaterial mat_l8\n}\n\n";
	auto obj = []( const char* name, const char* geom, const std::string& mat, double x ) {
		std::ostringstream s;
		s << "standard_object\n{\n\tname " << name << "\n\tgeometry " << geom << "\n\tposition " << x << " 0 0\n\tmaterial " << mat << "\n}\n\n";
		return s.str();
	};
	auto rast = []( int r, bool env, int spp ) {
		return r == 0 ? PtRasterizer( env, spp ) : r == 1 ? BdptRasterizer( env, spp, 12 ) : VcmRasterizer( env, spp, 12 );
	};
	auto reversedFor = []( int w ) {
		std::vector<int> v;
		for( int i = 0; i < 12; ++i ) if( w == 1 || ( w == 2 && ( i & 1 ) ) ) v.push_back( i );
		return v;
	};
	const char* inName[3] = { "PT  ", "BDPT", "VCM " };
	const char* wName[3] = { "outward", "inward", "mixed" };
	struct MatCfg { const char* name; const char* mat; int kind; };	// kind 0 glass/glass, 1 translucent, 2 nested
	const MatCfg cfgs[] = {
		{ "glass/glass",       "mat_gg",   0 },
		{ "glass/translucent", "mat_gtr",  1 },
		{ "nested",            "mat_nest", 2 },
	};
	unsigned seed = 131072u;
	int cells = 0, cellsBad = 0;
	auto runPair = [&]( const std::string& scene, double& mL, double& mR ) {
		CapturingRasterizerOutput* cap = 0;
		// Every render SALTED (an independent randomized-QMC replicate):
		// unsalted, all cells share one Sobol' pattern and its fixed
		// left / right offset (~1.3 % on the nested rows) repeats in every
		// cell instead of averaging out.
		SobolSamplerTestHooks::ValueSalt().store( 0x9E3779B9u * seed + 0x85EBCA6Bu );
		const bool ok = Render( scene, "sidedness", cap, seed++ );
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
		mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
		if( cap ) safe_release( cap );
		return ok;
	};
	auto gate = [&]( bool pass, const std::string& msg ) {
		++cells; if( !pass ) ++cellsBad;
		Check( pass, msg );
	};
	for( const MatCfg& c : cfgs ) {
		for( int ds = 0; ds < 2; ++ds ) {
			for( int w = 0; w < 3; ++w ) {
				for( int g = 0; g < 3; ++g ) {
					if( g_matrixSheetsOnly && g != 2 ) continue;
					const bool env = ( g != 1 );
					int spp = ( c.kind == 0 ) ? 128 : ( c.kind == 1 ) ? 256 : 1024;
					if( g == 1 ) spp = 1024;
					const double band = ( g == 1 ) ? ( c.kind == 0 ? 0.03 : 0.05 ) : ( c.kind == 0 ) ? 0.002 : ( c.kind == 1 ) ? 0.02 : 0.05;
					std::string geo, objs;
					const std::string twinMat = ( c.kind == 0 ) ? "mat_glass" : c.mat;
					if( g < 2 ) {
						geo = WindingBox( "bT", reversedFor( w ), ds == 1 ) +
						      ( c.kind == 0 ? WindingBox( "bR", reversedFor( w ), ds == 1 ) : WindingBox( "bR", std::vector<int>(), true ) );
						objs = obj( "L", "bT", c.mat, -2 ) + obj( "Rr", "bR", twinMat, 2 );
					} else {
						// DL-407 (2): a flat consistently wound quad (outward or
						// inward) is a PROVABLY open sheet since DL-382.  An
						// all-delta transmitting composite (nested) follows
						// DL-345's face rule there, which makes its WINDING
						// physical (the back side is the below medium): its
						// twin keeps the cell's winding and varies only the
						// sidedness.  Since DL-472 (1) glass/translucent (a
						// translucent bottom, term (c)) follows the face rule too
						// -- a back arrival meets the bottom first -- so its
						// winding is physical as well (an inward sheet is seen
						// from behind: ~1.55 against an outward sheet's ~0.53)
						// and its twin keeps the cell's winding.  The mixed
						// quad is not certified (stack rule): outward twin.
						geo = MatrixQuad( "qT", -4, 0, 0, w, ds == 1 ) +
						      ( c.kind == 0 ? MatrixQuad( "qR", 0, 4, 0, w, ds == 1 ) : MatrixQuad( "qR", 0, 4, 0, ( c.kind != 0 && w != 2 ) ? w : 0, true ) );
						objs = obj( "L", "qT", c.mat, 0 ) + obj( "Rr", "qR", twinMat, 0 );
					}
					const char* gName = ( g == 0 ) ? "closed box, furnace" : ( g == 1 ) ? "closed box, light inside" : "open sheet, furnace";
					double ptCell = -1;
					for( int r = 0; r < 3; ++r ) {
						const std::string scene = head + geo + objs + ( g == 1 ? lights : std::string() ) + rast( r, env, spp );
						// A light inside the box is the noisiest geometry
						// (single-render ratio sd up to ~1.1 % for glass/glass
						// under VCM over 10 repeats, ~1.5 % per half for the
						// translucent / nested boxes): mean of 3 salted
						// replicates for every light-inside cell.
						const int nRep = ( g == 1 ) ? 3 : 1;
						double mL = 0, mR = 0;
						bool ok = true;
						for( int k = 0; k < nRep; ++k ) {
							double l = -1, rr = -1;
							ok = runPair( scene, l, rr ) && ok;
							mL += l / nRep; mR += rr / nRep;
						}
						std::cout << "    M " << c.name << " | " << ( ds ? "double" : "single" ) << " | " << wName[w] << " | "
						          << gName << " | " << inName[r] << ": " << std::setprecision(5) << mL << " vs twin " << mR << "\n";
						const std::string tag = std::string( c.name ) + ", " + ( ds ? "double" : "single" ) + "-sided, " + wName[w] + ", " + gName + " (" + inName[r] + ")";
						gate( ok && mR > 0 && std::fabs( mL / mR - 1.0 ) <= band, "[M] == twin, " + tag );
						// DL-472 (1): the two-sided glass/translucent sheet is
						// one model for every integrator (before it, BDPT /
						// VCM read 0.607 / 0.750 against PT's 0.534 on the
						// outward twin).  3 % for single 256-spp renders.
						// Certified windings only: the mixed quad is not a
						// provably open sheet, so term (c) is off there and
						// its single renders scatter ~3 % (pre-existing).
						if( g == 2 && c.kind == 1 && w != 2 ) {
							if( r == 0 ) {
								ptCell = mL;
							} else if( ptCell > 0 ) {
								gate( ok && std::fabs( mL / ptCell - 1.0 ) <= 0.03, "[M] BDPT / VCM == PT (DL-472 (1)), " + tag );
							}
						}
						if( g == 0 && c.kind != 1 ) {
							gate( ok && std::fabs( mL - 1.0 ) <= band, "[M] lossless closed box == 1, " + tag );
						}
						if( g == 0 && c.kind == 0 ) {
							gate( ok && std::fabs( mR - 1.0 ) <= band, "[M] plain-glass twin == 1, " + tag );
						}
					}
				}
			}
		}
	}
	// Glass/glass sheet families against plain glass.
	const std::string mirror =
		"clippedplane_geometry\n{\n\tname mq\n\tpta -6 -4 -2\n\tptb 6 -4 -2\n\tptc 6 4 -2\n\tptd -6 4 -2\n}\n\n"
		"standard_object\n{\n\tname M\n\tgeometry mq\n\tmaterial mat_mir\n}\n\n";
	for( int fam = 0; fam < 3; ++fam ) {
		const char* fName = ( fam == 0 ) ? "two panes, one object" : ( fam == 1 ) ? "two panes, separate objects" : "sheet over a mirror";
		for( int ds = 0; ds < 2; ++ds ) {
			for( int w = 0; w < 3; ++w ) {
				std::string geo, objs;
				if( fam == 0 ) {
					geo = MatrixTwoPane( "pL", -4, 0, w, ds == 1 ) + MatrixTwoPane( "pR", 0, 4, w, ds == 1 );
					objs = obj( "L", "pL", "mat_gg", 0 ) + obj( "Rr", "pR", "mat_glass", 0 );
				} else if( fam == 1 ) {
					geo = MatrixQuad( "aL", -4, 0, 0.3, w, ds == 1 ) + MatrixQuad( "bL", -4, 0, -0.3, w, ds == 1 ) +
					      MatrixQuad( "aR", 0, 4, 0.3, w, ds == 1 ) + MatrixQuad( "bR", 0, 4, -0.3, w, ds == 1 );
					// DL-407 (2): delta-SHARP glass here.  Wound inward, each
					// pane's back side is glass under DL-345's face rule
					// (DL-382 certifies the quads), so the ray leaving the
					// first pane meets the second from glass past its
					// critical angle near the frame edge -- a TIR edge in
					// the frame.  `dielectric_material`'s default
					// `scattering` (10000) warps EVERY transmission,
					// including the composite's index-matched inner one, so
					// the two-interface composite spreads its exit twice
					// where plain glass spreads it once and the edge blurs
					// (-0.3 % against plain glass, an ior-3 second pane
					// -0.4 %; 0.0 % with both delta-sharp).  That is the
					// warp model (DL-297), not the stack convention this
					// family tests.
					objs = obj( "La", "aL", "mat_ggS", 0 ) + obj( "Lb", "bL", "mat_ggS", 0 ) +
					       obj( "Ra", "aR", "mat_glassS", 0 ) + obj( "Rb", "bR", "mat_glassS", 0 );
				} else {
					geo = MatrixQuad( "qL", -4, 0, 0, w, ds == 1 ) + MatrixQuad( "qR", 0, 4, 0, w, ds == 1 ) + mirror;
					objs = obj( "L", "qL", "mat_gg", 0 ) + obj( "Rr", "qR", "mat_glass", 0 );
				}
				for( int r = 0; r < 3; ++r ) {
					const std::string scene = head + geo + objs + rast( r, true, 128 );
					double mL = -1, mR = -1;
					const bool ok = runPair( scene, mL, mR );
					std::cout << "    M glass/glass | " << ( ds ? "double" : "single" ) << " | " << wName[w] << " | "
					          << fName << " | " << inName[r] << ": " << std::setprecision(5) << mL << " vs plain glass " << mR << "\n";
					// The mirror return is not zero-variance under BDPT / VCM (their
					// light-side strategies reach the mirror too): 0.5 % there,
					// against a 27 % regression (0.677 vs 0.929).
					// DL-407 (2): the inward separate panes are not zero-
					// variance either -- each pane's back is a face-rule
					// exit whose reflected (weight 1) and transmitted (weight
					// T eta^2) branches differ, so the R / T selection is
					// noise: ratio sd ~0.08 % over 12 salted renders, mean
					// -0.06 % (the frame's left / right offset).  0.5 %,
					// against the pre-fix 0.94 / 0.47 vs 4.64 this family pins.
					const double famBand = ( fam == 2 || ( fam == 1 && w == 1 ) ) ? 0.005 : 0.002;
					gate( ok && mR > 0 && std::fabs( mL / mR - 1.0 ) <= famBand,
						std::string( "[M] glass/glass == plain glass, " ) + fName + ", " + ( ds ? "double" : "single" ) + "-sided, " + wName[w] + " (" + inName[r] + ")" );
				}
			}
		}
	}
	std::cout << "    M summary: " << ( cells - cellsBad ) << " of " << cells << " matrix checks pass\n";
}


//////////////////////////////////////////////////////////////////////
//  D10 (DL-407 (2) review, 2026-10-08): a composite that is NOT face-ruled
//  (a non-delta translucent layer, or an opaque one) on a provably open
//  sheet presents its TOP on either face, so seen from BEHIND under a
//  DELTA light on the camera side it is lit exactly as seen from the
//  front (mirrored scene).  Walking it from below delta-tags every exit,
//  which NEE cannot see (DL-296): the back read 0.  Rows: glass/translucent
//  on a double-sided clipped plane (P1: the first revision face-ruled every
//  translucent stack) and a coat over Lambertian on a SINGLE-sided flat
//  mesh (P2-1: no flip flag on a single-sided back hit, walked from below
//  since DL-382 certified the mesh).
//////////////////////////////////////////////////////////////////////
static void SectionD10()
{
	std::cout << "\n[D10] Non-face-ruled composite on an open sheet, back == front under an omni light\n";
	const std::string mats =
		"uniformcolor_painter\n{\n\tname pnt_w8\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tr\n\tcolor 0.3 0.3 0.3\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tt\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"lambertian_material\n{\n\tname mat_l8\n\treflectance pnt_w8\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
		"translucent_material\n{\n\tname mat_tr\n\tref pnt_tr\n\ttau pnt_tt\n\text 0\n\tN 10\n\tscattering 0\n}\n\n"
		"composite_material\n{\n\tname mat_gtr\n\ttop mat_glass\n\tbottom mat_tr\n\tthickness 0.05\n\textinction 0.2\n}\n\n"
		"composite_material\n{\n\tname mat_cc\n\ttop mat_glass\n\tbottom mat_l8\n\tthickness 0\n\textinction 0.0\n}\n\n"
		"composite_material\n{\n\tname mat_gtr0\n\ttop mat_glass\n\tbottom mat_tr\n\tthickness 0\n\textinction 0.0\n}\n\n";
	// DL-472 (1): glass/translucent now follows the face rule on an open
	// sheet (a back arrival meets the bottom first), so back != front by
	// design.  Gates: the composite reads the same under PT / BDPT / VCM
	// from each side (2 %), and from the FRONT it equals the equivalent
	// pair of SEPARATE sheets (glass at z = 0, translucent 0.002 behind it)
	// under PT (3 %).  From the BACK the pair is printed only: the
	// separate pair is itself not consistent there (PT 0.0351 against
	// BDPT / VCM 0.0441 -- a standalone translucent sheet is not
	// reciprocal, DL-223, and the light reaches it through a delta glass
	// sheet), while the composite reads 0.0376 under all three.
	{
		double compRef[2] = { -1, -1 };
		const std::string pairGeo =
			"clippedplane_geometry\n{\n\tname qg\n\tpta -4 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd -4 3 0\n}\n\n"
			"clippedplane_geometry\n{\n\tname qt\n\tpta -4 -3 -0.002\n\tptb 4 -3 -0.002\n\tptc 4 3 -0.002\n\tptd -4 3 -0.002\n}\n\n";
		const std::string compGeo =
			"clippedplane_geometry\n{\n\tname qc\n\tpta -4 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd -4 3 0\n}\n\n";
		for( int r = 0; r < 3; ++r ) {
			for( int b = 0; b < 2; ++b ) {
				const double z = b ? -1.0 : 1.0;
				double v[2] = { -1, -1 };
				for( int k = 0; k < 2; ++k ) {
					std::ostringstream sc;
					sc << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 32\n\theight 16\n}\n\n"
					   << "pinhole_camera\n{\n\tlocation 0 0 " << 7.0 * z << "\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
					   << mats << ( k == 0 ? compGeo : pairGeo )
					   << ( k == 0 ? std::string( "standard_object\n{\n\tname S\n\tgeometry qc\n\tmaterial mat_gtr0\n}\n\n" )
					               : std::string( "standard_object\n{\n\tname G\n\tgeometry qg\n\tmaterial mat_glass\n}\n\n"
					                              "standard_object\n{\n\tname T\n\tgeometry qt\n\tmaterial mat_tr\n}\n\n" ) )
					   // An AREA light on the camera's side: a delta light would
					   // not reach the separate pair's translucent sheet through
					   // the glass sheet (NEE cannot see through a delta coat),
					   // so the pair would read 0 from the glass side.
					   << "uniformcolor_painter\n{\n\tname pnt_em\n\tcolor 1 1 1\n}\n\n"
					   << "lambertian_luminaire_material\n{\n\tname mat_em\n\texitance pnt_em\n\tscale 10.0\n\tmaterial none\n}\n\n"
					   << "clippedplane_geometry\n{\n\tname gem\n\tpta -1.5 2 " << 4.0 * z << "\n\tptb 1.5 2 " << 4.0 * z << "\n\tptc 1.5 3.5 " << 4.0 * z << "\n\tptd -1.5 3.5 " << 4.0 * z << "\n}\n\n"
					   << "standard_object\n{\n\tname E\n\tgeometry gem\n\tmaterial mat_em\n}\n\n"
					   << ( r == 0 ? PtRasterizer( false, 1024 ) : r == 1 ? BdptRasterizer( false, 1024, 12 ) : VcmRasterizer( false, 1024, 12 ) );
					CapturingRasterizerOutput* cap = 0;
					SobolSamplerTestHooks::ValueSalt().store( 0x9E3779B9u * ( 91100u + 13u * (unsigned)( 4 * r + 2 * b + k ) ) + 0x85EBCA6Bu );
					const bool ok = Render( sc.str(), "d10p", cap, 91100u + 13u * (unsigned)( 4 * r + 2 * b + k ) );
					SobolSamplerTestHooks::ValueSalt().store( 0u );
					v[k] = ok ? RegionMean( *cap, 2, cap->width - 2 ) : -1;
					if( cap ) safe_release( cap );
				}
				const char* in = ( r == 0 ) ? "PT  " : ( r == 1 ) ? "BDPT" : "VCM ";
				std::cout << "    D10 glass/translucent (thickness 0) vs separate pair, " << ( b ? "BACK " : "FRONT" ) << ", " << in << ": composite "
				          << std::setprecision(5) << v[0] << " pair " << v[1] << ", ratio " << ( v[1] > 0 ? v[0] / v[1] : -1 ) << "\n";
				if( r == 0 ) {
					compRef[b] = v[0];
					if( b == 0 ) {
						Check( v[0] > 0 && v[1] > 0 && std::fabs( v[0] / v[1] - 1.0 ) <= 0.03,
							"[D10] glass/translucent == separate pair, front view (PT)" );
					}
				} else {
					Check( v[0] > 0 && compRef[b] > 0 && std::fabs( v[0] / compRef[b] - 1.0 ) <= 0.02,
						std::string( "[D10] glass/translucent composite agrees with PT, " ) + ( b ? "back" : "front" ) + " view (" + in + ")" );
				}
			}
		}
	}
	struct Row { const char* name; const char* mat; bool singleSidedMesh; };
	const Row rows[] = {
		{ "coat over Lambertian 0.8, single-sided flat mesh", "mat_cc", true },
	};
	for( const Row& row : rows ) {
		// The sheet's normal is +z (clipped plane pta..ptd, mesh wound +z).
		const std::string geo = row.singleSidedMesh
			? std::string( "indexedmesh_geometry\n{\n\tname q\n\tvertex -4 -3 0\n\tvertex 4 -3 0\n\tvertex 4 3 0\n\tvertex -4 3 0\n"
			               "\ttriangle 0 1 2\n\ttriangle 0 2 3\n\tdouble_sided FALSE\n\tface_normals TRUE\n}\n\n" )
			: std::string( "clippedplane_geometry\n{\n\tname q\n\tpta -4 -3 0\n\tptb 4 -3 0\n\tptc 4 3 0\n\tptd -4 3 0\n}\n\n" );
		for( int r = 0; r < 2; ++r ) {
			double side[2] = { -1, -1 };	// 0: front (camera +z), 1: back (camera -z)
			for( int b = 0; b < 2; ++b ) {
				const double z = b ? -1.0 : 1.0;
				std::ostringstream sc;
				sc << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 32\n\theight 16\n}\n\n"
				   << "pinhole_camera\n{\n\tlocation 0 0 " << 7.0 * z << "\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
				   << mats << geo
				   << "standard_object\n{\n\tname S\n\tgeometry q\n\tmaterial " << row.mat << "\n}\n\n"
				   << "omni_light\n{\n\tname ol\n\tpower 200\n\tcolor 1 1 1\n\tposition 0 0 " << 5.0 * z << "\n}\n\n"
				   << ( r == 0 ? PtRasterizer( false, 256 ) : BdptRasterizer( false, 256 ) );
				CapturingRasterizerOutput* cap = 0;
				const bool ok = Render( sc.str(), "d10", cap, 91000u + 7u * (unsigned)( 2 * r + b ) );
				side[b] = ok ? RegionMean( *cap, 2, cap->width - 2 ) : -1;
				if( cap ) safe_release( cap );
			}
			const char* in = ( r == 0 ) ? "PT  " : "BDPT";
			std::cout << "    D10 " << row.name << ", " << in << ": front " << std::setprecision(5) << side[0]
			          << " back " << side[1] << ", ratio " << ( side[0] > 0 ? side[1] / side[0] : -1 ) << "\n";
			Check( side[0] > 0 && side[1] > 0 && std::fabs( side[1] / side[0] - 1.0 ) <= 0.03,
				std::string( "[D10] back == front under an omni light, " ) + row.name + " (" + in + ")" );
		}
	}
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
	// DL-407 (2) (2026-10-08): that "top on both faces" now holds for an
	// OPAQUE composite only.  A TRANSMITTING one (glass/glass) on a
	// provably open sheet -- the clipped plane, and since DL-382 this flat
	// mesh quad -- follows DL-345's face rule like plain glass: from behind
	// the camera is in the below medium (2.20).  The uncertified Bezier
	// patch keeps the stack rule; its row is gated on the two closed forms.
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
		std::snprintf( bzPath, sizeof( bzPath ), "%s/composite_energy_sheet_%d.bezier", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", (int)getpid() );
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
				// DL-407 (2) (2026-10-08): a TRANSMITTING composite on the
				// provably open clipped plane follows DL-345's face rule
				// like plain glass does -- seen from its back the camera
				// is in the below medium (glass): F + (1-F) eta^2 = 2.20
				// at normal incidence.  The Bezier patch is not certified
				// (DL-382 (1)) and keeps the stack rule (the top, 0.467 =
				// F + (1-F)/eta^2), exactly the split plain glass shows on
				// the same two sheets.  The opaque coat keeps the top on
				// both faces (a card's other side), so its rows still pair.
				if( g == 1 && std::string( c.mat ) == "mat_gg" ) {
					Check( ok && std::fabs( mM / 0.4667 - 1.0 ) <= 0.03,
						std::string( "[D7] open " ) + gname + " sheet from behind == F + (1-F)/eta^2 (stack rule; DL-382 (1) KNOWN-DEFECT pin: an uncertified Bezier sheet is not face-ruled), " + c.name + " (" + in + ")" );
					Check( ok && std::fabs( mC / 2.198 - 1.0 ) <= 0.03,
						std::string( "[D7] clipped plane from behind == F + (1-F) eta^2 (face rule), " ) + c.name + " (" + in + ")" );
				} else {
					Check( ok && mC > 0 && std::fabs( mM / mC - 1.0 ) <= 0.03,
						std::string( "[D7] open " ) + gname + " sheet == clipped-plane twin from behind, " + c.name + " (" + in + ")" );
				}
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
	// against front 0.860 (also on master).  Gate: back / front per half,
	// PT, BDPT and VCM (translucent: PT and BDPT); bands below.
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
		// The nested tops run the per-branch estimator, so their single
		// renders are noisy (a 256-spp front / back pair differed by up to
		// 3.1 %): 1024 spp and a 5 % band (~5 sigma of the difference);
		// the broken state reads ~-48 %.  The translucent rows are near
		// deterministic (0.1 %) and keep 3 % (broken: -17 %).
		struct Cfg8 { const char* name; const char* mat; int nInt; int spp; double band; };
		const Cfg8 cfgs[] = {
			{ "nested composite{composite{glass/water}/Lambertian 0.8}", "mat_gwl", 3, 1024, 0.05 },
			{ "nested composite{composite{glass/glass}/Lambertian 0.8}", "mat_ggl", 3, 1024, 0.05 },
			{ "composite{translucent/Lambertian 0.8}", "mat_trl", 2, 256, 0.03 },
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
						( r == 0 ? PtRasterizer( true, c.spp ) : r == 1 ? BdptRasterizer( true, c.spp ) : vcm8( c.spp ) );
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
				Check( mesh[0] > 0 && std::fabs( mesh[1] / mesh[0] - 1.0 ) <= c.band,
					std::string( "[D8] open mesh sheet, back == front, " ) + c.name + " (" + in + ")" );
				Check( plane[0] > 0 && std::fabs( plane[1] / plane[0] - 1.0 ) <= c.band,
					std::string( "[D8] open clipped-plane sheet, back == front, " ) + c.name + " (" + in + ")" );
			}
		}
	}

	SectionD9();
	SectionM();
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
//      the same physical layers: Lambertian and (since DL-388) a
//      diffuse-dominant GGX gated to MC noise.
//   K4-K6 (DL-388).  Glossy-dominant / rough / smooth GGX and Oren-Nayar
//      substrates against the composite; the white-metal furnace; the
//      AOV albedo against the directional albedo it summarises.
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

static CoatedMaterial* MakeCoated( const IMaterial& base, double sigma, const IPainter& tint, double eta = 1.5 )
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


//////////////////////////////////////////////////////////////////////
//  K4-K6 -- DL-388: the substrate in the coat's REFRACTED frame.
//
//  Pre-DL-388 `coated_material` evaluated the substrate at the OUTER
//  directions with the Lambertian recycling factor -- exact only for a
//  Lambertian.  A glossy lobe lost T^2/eta^2 of its energy instead of
//  T^2 (0.70 of the composite on a glossy metal at normal incidence),
//  and a diffuse lobe's (1 - A) was read at the outer grazing angle.
//
//   K4  coated / composite, directional albedo, smooth 1.5 coat
//       (sigma_t 0, 0.2, 0.5): glossy-dominant, rough, mixed and smooth
//       GGX substrates and two Oren-Nayar ones.  theta 0 / 45 gated at
//       max(3 %, 5 sem); theta 70 gated for the rough / diffuse-like
//       rows and PINNED for the smooth glossy ones (DL-423: a smooth
//       lossy lobe keeps light trapped near the critical angle, which
//       the reservoir lets escape too readily -- 1.045 / 1.065 here).
//   K5  white furnace: lossless white GGX metals (F0 1) under a clear
//       coat must read 1 at every incidence, within MC noise and the
//       model's documented +0.7 % (the substrate's own GGX directional
//       albedo is 1.003 at normal incidence), at eta 1.5 and at coat
//       indices just above the surrounding medium's (1.005 .. 1.13).
//       Pre-DL-388: 1.058 at 0 deg for the smooth one; first fix 1.160
//       at eta 1.01 / 80 deg (the clear table's eta blend).
//   K6  the OIDN albedo AOV is the layered model's own directional
//       albedo (e_1 + g E / (1 - Q) through the coat) -- it must agree
//       with the furnace it summarises.
//////////////////////////////////////////////////////////////////////
static GGXMaterial* MakeSchlickGgx( double diffuse, double f0, double alpha )
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

static void SectionK4K6( Fixtures& f )
{
	UniformScalarPainter* sDelta = new UniformScalarPainter( 1000000.0 );  sDelta->addref();
	DielectricMaterial* smooth = new DielectricMaterial( *f.s1, *f.s15, *sDelta, false );  smooth->addref();

	// ---- K4: coated vs composite ---------------------------------------
	{
		struct Sub { const char* name; IMaterial* m; bool trapsNearCritical; bool isLobe; };
		UniformColorPainter* onRho = new UniformColorPainter( RISEPel( 0.8, 0.8, 0.8 ) );  onRho->addref();
		UniformScalarPainter* onSig = new UniformScalarPainter( 0.8 );                     onSig->addref();
		OrenNayarMaterial* on = new OrenNayarMaterial( *onRho, *onSig );                   on->addref();
		Sub subs[] = {
			{ "GGX glossy (diffuse 0, F0 .5, alpha .16)", MakeSchlickGgx( 0.0, 0.5, 0.16 ), true, true },
			{ "GGX rough glossy (diffuse 0, F0 .5, alpha .5)", MakeSchlickGgx( 0.0, 0.5, 0.5 ), false, true },
			{ "GGX mixed (diffuse .5, F0 .5, alpha .3)", MakeSchlickGgx( 0.5, 0.5, 0.3 ), false, true },
			{ "GGX smooth metal (diffuse 0, F0 .9, alpha .05)", MakeSchlickGgx( 0.0, 0.9, 0.05 ), true, true },
			{ "Oren-Nayar (rho .8, sigma .8)", on, false, false },
		};
		const double sigmas[] = { 0.0, 0.2, 0.5 };
		const double thetas[] = { 0.0, 45.0, 70.0 };
		std::cout << "    K4 directional albedo, coated vs composite (8 x 20000 draws each; mean +- sem):\n";
		for( Sub& sb : subs ) {
			for( double sg : sigmas ) {
				UniformScalarPainter* ext = new UniformScalarPainter( sg );  ext->addref();
				CompositeMaterial* comp = MakeComposite( *smooth, *sb.m, 3, 3, 3, 3, 3, 1.0, *ext );
				CoatedMaterial* coat = MakeCoated( *sb.m, sg, *f.white );
				for( double th : thetas ) {
					const FurnaceStats sc = Furnace( *coat->GetSPF(), th, false, false, 8, 20000, 7101u + (unsigned)th );
					const FurnaceStats sp = Furnace( *comp->GetSPF(), th, false, false, 8, 20000, 9101u + (unsigned)th );
					const double ratio = sc.mean / sp.mean;
					const double semR = ratio * std::sqrt( std::pow( sc.sem / sc.mean, 2 ) + std::pow( sp.sem / sp.mean, 2 ) );
					std::cout << "      " << sb.name << " sigma_t " << std::setprecision(3) << sg << " theta " << (int)th
					          << ": coated " << std::setprecision(5) << sc.mean << " +- " << sc.sem
					          << "  composite " << sp.mean << " +- " << sp.sem
					          << "  coated/composite " << ratio << " +- " << semR << "\n";
					const std::string tag = std::string( "[K4] " ) + sb.name + " sigma " + std::to_string( sg ) +
						" theta " + std::to_string( (int)th );
					if( th < 60.0 || !sb.trapsNearCritical ) {
						// theta 70 with an absorbing coat carries the composite's
						// own grazing bias the Lambertian K3 rows also show
						// (0.977 at 60 deg, sigma_t 2, against an exact closed
						// form), so its floor widens with sigma_t.
						const double floorTol = ( th < 60.0 ) ? 0.03 : ( 0.03 + 0.06 * sg );
						Check( std::fabs( ratio - 1.0 ) <= std::max( floorTol, 5.0 * semR ), tag + " coated / composite == 1" );
					} else {
						// DL-423 residual, measured (8 x 20000 draws per side):
						// clear coat 1.045 +- 0.004 (glossy) / 1.065 +- 0.012
						// (smooth metal), sigma_t 0.2 1.015 / 1.019, sigma_t 0.5
						// 1.009 / 1.007.  Pinned at [0.97, 1.08] widened by the
						// row's own 5 sem, so a move either way is caught.
						Check( ratio >= 0.97 - 5.0 * semR && ratio <= 1.08 + 5.0 * semR,
							tag + " coated / composite inside the DL-423 residual pin [0.97, 1.08] +- 5 sem" );
					}

					// ---- K6: AOV albedo == the directional albedo it summarises
					//      (GGX only: Oren-Nayar keeps the cosine-reservoir AOV
					//      summary, R escape / (1 - E_ret R), 2-3 % off its
					//      refracted-frame directional albedo -- an OIDN guide.)
					if( th < 60.0 && sb.isLobe ) {
						const RISEPel aov = coat->GetBSDF()->albedo( MakeIntersection( th * kPi / 180.0 ) );
						const double rAov = aov[0] / sc.mean;
						Check( std::fabs( rAov - 1.0 ) <= std::max( 0.02, 5.0 * sc.sem / sc.mean ),
							std::string( "[K6] " ) + sb.name + " sigma " + std::to_string( sg ) + " theta " +
							std::to_string( (int)th ) + " albedo() AOV == directional albedo (" + std::to_string( rAov ) + ")" );
					}
				}
				coat->release(); comp->release(); ext->release();
			}
		}
		for( Sub& sb : subs ) sb.m->release();
		onRho->release(); onSig->release();
	}

	// ---- K5: white furnace, lossless white metals ----------------------
	//  eta 1.5 at four incidences, plus coats whose index sits just above
	//  the surrounding medium's (1.005 .. 1.13: a lacquer underwater, a
	//  coat on a coat) at the grazing incidences, where the clear-coat
	//  basis table used to blend across the critical cosine's infinite
	//  slope at eta = 1 (eta 1.01: 1.160 at 80 deg pre-fix), and eta 3,
	//  where the escape cone is narrow and near-normal.
	{
		std::cout << "    K5 white furnace, clear coat over white GGX metals (8 x 50000 draws):\n";
		struct Row { double eta; std::vector<double> thetas; };
		const Row rows[] = {
			{ 1.5,   { 0.0, 45.0, 70.0, 75.0, 85.0 } },
			{ 1.005, { 70.0, 80.0, 85.0 } },
			{ 1.01,  { 70.0, 80.0, 85.0 } },
			{ 1.04,  { 70.0, 80.0, 85.0 } },
			{ 1.13,  { 70.0, 80.0, 85.0 } },
			{ 3.0,   { 60.0, 75.0, 79.0, 83.0 } },
			// DL-426: the critical patch's far end crosses normal incidence
			// at eta ~2.874 (the clear table stepped 1.9 % there).
			{ 2.87,  { 0.0, 60.0, 75.0, 80.0 } },
		};
		// alpha 0.002: a lobe narrower than the view-node spacing, whose
		// return the critical patch carries (pre-patch 1.09 at eta 1.5 /
		// 75 deg, 1.054 at eta 3 / 79 deg).
		const double alphas[] = { 0.002, 0.05, 0.4 };
		for( double al : alphas ) {
			GGXMaterial* metal = MakeSchlickGgx( 0.0, 1.0, al );
			for( const Row& rw : rows ) {
				CoatedMaterial* coat = MakeCoated( *metal, 0.0, *f.white, rw.eta );
				std::cout << "      alpha " << al << " eta " << rw.eta << ":";
				for( double th : rw.thetas ) {
					const FurnaceStats sc = Furnace( *coat->GetSPF(), th, false, false, 8, 50000, 5101u + (unsigned)th );
					std::cout << "  " << (int)th << " deg " << std::setprecision(5) << sc.mean << " +- " << sc.sem;
					Check( sc.mean <= 1.012 + 4.0 * sc.sem && sc.mean >= 0.985 - 4.0 * sc.sem,
						std::string( "[K5] white metal alpha " ) + std::to_string( al ) + " eta " + std::to_string( rw.eta ) +
						" theta " + std::to_string( (int)th ) + " furnace in [0.985, 1.012]" );
				}
				std::cout << "\n";
				coat->release();
			}
			metal->release();
		}
	}

	// ---- K5b: DL-426, an absorbing coat of relative index ~1 vs the
	//      composite.  The window where the pre-DL-426 basis stepped (alpha
	//      .1, eta 1.0145, grazing view): the coated directional albedo must
	//      still agree with the explicit two-layer walk.  A consistency pin,
	//      green on the pre-DL-426 code too (1.0034 / 0.9995 / 1.0054): the
	//      component DL-426 moves is a few per cent of the small recycled
	//      term, below this albedo's noise.
	{
		std::cout << "    K5b absorbing near-unity coat (eta 1.0145, sigma_t 0.2) over GGX F0 .9 alpha .1, coated vs composite:\n";
		UniformScalarPainter* nNear = new UniformScalarPainter( 1.0145 );  nNear->addref();
		DielectricMaterial* smoothNear = new DielectricMaterial( *f.s1, *nNear, *sDelta, false );  smoothNear->addref();
		UniformScalarPainter* ext = new UniformScalarPainter( 0.2 );  ext->addref();
		GGXMaterial* metal = MakeSchlickGgx( 0.0, 0.9, 0.1 );
		CompositeMaterial* comp = MakeComposite( *smoothNear, *metal, 3, 3, 3, 3, 3, 1.0, *ext );
		CoatedMaterial* coat = MakeCoated( *metal, 0.2, *f.white, 1.0145 );
		const double thetas[] = { 0.0, 60.0, 80.0 };
		for( double th : thetas ) {
			const FurnaceStats sc = Furnace( *coat->GetSPF(), th, false, false, 8, 20000, 7301u + (unsigned)th );
			const FurnaceStats sp = Furnace( *comp->GetSPF(), th, false, false, 8, 20000, 9301u + (unsigned)th );
			const double ratio = sc.mean / sp.mean;
			const double semR = ratio * std::sqrt( std::pow( sc.sem / sc.mean, 2 ) + std::pow( sp.sem / sp.mean, 2 ) );
			std::cout << "      theta " << (int)th << ": coated " << std::setprecision(5) << sc.mean << " +- " << sc.sem
			          << "  composite " << sp.mean << " +- " << sp.sem << "  coated/composite " << ratio << " +- " << semR << "\n";
			Check( std::fabs( ratio - 1.0 ) <= std::max( 0.03, 5.0 * semR ),
				std::string( "[K5b] near-unity absorbing coat theta " ) + std::to_string( (int)th ) + " coated / composite == 1" );
		}
		coat->release(); comp->release(); metal->release(); ext->release(); smoothNear->release(); nNear->release();
	}

	// ---- K7: double-sided indexed mesh, front vs back (render) ---------
	//  The refracted-frame substrate record is built from the RAY-FACING
	//  frame (CoatedBRDF::MakeSubstrateRecord).  A double_sided
	//  indexedmesh_geometry flips both normals toward the ray, so the same
	//  coated GGX quad must render identically whichever winding faces the
	//  camera.  Left half: winding facing AWAY from the camera; right half:
	//  facing it.  A directional light 30 deg off the normal, tilted in
	//  the vertical plane so the two halves are mirror images of each
	//  other across x = 0, reaches both only through NEE (a delta light),
	//  so the ratio is read off GetBSDF()->value.
	{
		std::ostringstream sc;
		sc << "RISE ASCII SCENE 7\n"
		   << "film\n{\n\twidth 32\n\theight 16\n}\n\n"
		   << "pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
		   << "uniformcolor_painter\n{\n\tname pnt_rd\n\tcolor 0.5 0.5 0.5\n}\n\n"
		   << "uniformcolor_painter\n{\n\tname pnt_rs\n\tcolor 0.5 0.5 0.5\n}\n\n"
		   << "ggx_material\n{\n\tname mat_base\n\trd pnt_rd\n\trs pnt_rs\n\talphax 0.3\n\talphay 0.3\n\tfresnel_mode schlick_f0\n}\n\n"
		   << "coated_material\n{\n\tname mat_coat\n\tbase mat_base\n\tcoat_ior 1.5\n\tcoat_roughness 0.02\n}\n\n"
		   << "indexedmesh_geometry\n{\n\tname qL\n\tvertex -4 -3 0\n\tvertex 0 -3 0\n\tvertex 0 3 0\n\tvertex -4 3 0\n"
		   << "\ttriangle 0 2 1\n\ttriangle 0 3 2\n\tdouble_sided TRUE\n\tface_normals TRUE\n}\n\n"
		   << "indexedmesh_geometry\n{\n\tname qR\n\tvertex 0 -3 0\n\tvertex 4 -3 0\n\tvertex 4 3 0\n\tvertex 0 3 0\n"
		   << "\ttriangle 0 1 2\n\ttriangle 0 2 3\n\tdouble_sided TRUE\n\tface_normals TRUE\n}\n\n"
		   << "standard_object\n{\n\tname objL\n\tgeometry qL\n\tmaterial mat_coat\n}\n\n"
		   << "standard_object\n{\n\tname objR\n\tgeometry qR\n\tmaterial mat_coat\n}\n\n"
		   << "directional_light\n{\n\tname key\n\tpower 1.0\n\tcolor 1 1 1\n\tdirection 0 0.5 0.8660254\n}\n\n";
		for( int r = 0; r < 2; ++r ) {
			const std::string scene = sc.str() + ( r == 0 ? PtRasterizer( false, 64 ) : BdptRasterizer( false, 64 ) );
			CapturingRasterizerOutput* cap = 0;
			const bool ok = Render( scene, r == 0 ? "k7_pt" : "k7_bdpt", cap, 7070u + r );
			const double mL = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
			const double mR = ok ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
			std::cout << "    K7 double-sided indexed mesh, coated GGX, " << ( r == 0 ? "PT  " : "BDPT" )
			          << ": back-wound " << std::setprecision(5) << mL << ", front-wound " << mR
			          << ", ratio " << ( mR > 0 ? mL / mR : -1 ) << "  (truth 1)\n";
			Check( ok && mR > 0 && std::fabs( mL / mR - 1.0 ) <= 0.02,
				std::string( "[K7] double-sided indexed mesh: back-wound == front-wound coated GGX (" ) + ( r == 0 ? "PT" : "BDPT" ) + ")" );
			if( cap ) safe_release( cap );
		}
	}

	smooth->release(); sDelta->release();
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
					// GGX (diffuse-dominant): since DL-388 the same gate.  The
					// substrate is evaluated in the coat's REFRACTED frame and its
					// recycled field priced through its own first-bounce return
					// (CoatedBRDF.cpp, "SUBSTRATE IN THE COAT'S FRAME").  Pre-DL-388
					// the outer-frame evaluation read the diffuse lobe's (1 - A) at the
					// outer grazing angle and these rows sat 3-6 % LOW (0.946 at
					// sigma_t 0.2, theta 0), pinned in [0.92, 1.02].
					Check( std::fabs( ratio - 1.0 ) <= std::max( 0.01, 5.0 * semR ),
						std::string( "[K3] " ) + sb.name + " sigma " + std::to_string( sg ) + " theta " +
						std::to_string( (int)th ) + " coated / composite == 1" );
				}
				coat->release(); comp->release(); ext->release();
			}
		}
		ggx->release(); ggxSpec->release(); ggxDiff->release(); smooth->release(); sDelta->release();
	}

	SectionK4K6( f );
}


//////////////////////////////////////////////////////////////////////
//  K8 -- DL-417 (OPEN; measurement + pin): a coat over a fabric_material
//  or weave_material substrate, coated / composite of the same physical
//  layers (smooth 1.5 coat, thickness 1).  These substrates keep the
//  pre-DL-388 OUTER-frame model with the Lambertian recycling factor.
//  Measured 2026-10-08 (8 x 20000 draws per side): grey-base fabric
//  (sheen rough .3) 1.04 / 1.10 at 45 / 70 deg under a clear coat; weave
//  silk / denim 1.05-1.08 clear and 0.82-0.90 under sigma_t 0.3.  The
//  refracted frame with the COSINE reservoir (Oren-Nayar's model) was
//  tried and is worse: a lossless white fabric reads 1.054 (an energy
//  GAIN) and the weaves 1.13-1.40 under a clear coat -- their first bounce
//  escapes far more than a Lambertian's (1 - r_i) share, which the
//  cosine factor 1 / (1 - r_i R) then amplifies.  Closing it needs the
//  substrate's own first-bounce return g (the GGX lobe reservoir's
//  kernel) for an anisotropic, spatially varying fibre BSDF.  The rows
//  are PINNED per row at its measured value +- (0.015 + 5 sem), and
//  the lossless white fabric's clear-coat furnace must stay <= 1.012
//  (it reads 1.0084 at 0 deg: the outer-frame model's own gain).  Runs with --coated-only, alone with --dl417-only (~15 s).
static void SectionK8( Fixtures& f )
{
	std::cout << "\n[K8] DL-417 (open): coated_material over fabric / weave substrates, coated vs composite (pinned)\n";
	UniformScalarPainter* sDelta = new UniformScalarPainter( 1000000.0 );  sDelta->addref();
	DielectricMaterial* smooth = new DielectricMaterial( *f.s1, *f.s15, *sDelta, false );  smooth->addref();

	UniformColorPainter*  sheenWhite = new UniformColorPainter( RISEPel( 1, 1, 1 ) );  sheenWhite->addref();
	UniformScalarPainter* sr3 = new UniformScalarPainter( 0.3 );  sr3->addref();
	UniformScalarPainter* sr7 = new UniformScalarPainter( 0.7 );  sr7->addref();
	UniformColorPainter*  grey = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  grey->addref();
	LambertianMaterial*   lambGrey = new LambertianMaterial( *grey );  lambGrey->addref();
	FabricMaterial* fabLamb3 = new FabricMaterial( *lambGrey, *sheenWhite, *sr3, *f.s0 );  fabLamb3->addref();
	FabricMaterial* fabLamb7 = new FabricMaterial( *f.lamb, *sheenWhite, *sr7, *f.s0 );  fabLamb7->addref();
	WeaveTest::PresetWeave silk( "silk", 0, 0.5, true );
	WeaveTest::PresetWeave denim( "denim", 0, 0.5, true );

	// pin[sigma][theta]: the coated / composite ratio measured 2026-10-08
	// with these seeds (outer-frame model).  lossless: the substrate's own
	// albedo is 1, so the coated furnace must not exceed 1 beyond K5's band.
	struct Sub { const char* name; IMaterial* m; double pin[2][3]; bool lossless; };
	Sub subs[] = {
		{ "fabric (sheen 1, rough .3) over Lambertian .5", fabLamb3, { { 0.9935, 1.0384, 1.1052 }, { 0.9887, 1.0314, 1.0656 } }, false },
		{ "fabric (sheen 1, rough .7) over Lambertian 1", fabLamb7, { { 1.0130, 1.0041, 0.9935 }, { 0.9752, 0.9755, 0.9766 } }, true },
		{ "weave silk (white dyes, coverage .5)", silk.Material(), { { 1.0537, 1.0692, 0.9141 }, { 0.8243, 0.8984, 0.8916 } }, false },
		{ "weave denim (white dyes, coverage .5)", denim.Material(), { { 1.0753, 1.0435, 0.9707 }, { 0.8817, 0.8775, 0.8987 } }, false },
	};
	const double sigmas[] = { 0.0, 0.3 };
	const double thetas[] = { 0.0, 45.0, 70.0 };
	for( Sub& sb : subs ) {
		int si = 0;
		{
			RISEPel H;
			sb.m->GetBSDF()->hemisphericalAlbedo( MakeIntersection( 0.3 ), H );
			std::cout << "      " << sb.name << ": bare directional albedo";
			for( double th : thetas ) {
				const FurnaceStats b = Furnace( *sb.m->GetSPF(), th, false, false, 4, 20000, 5201u + (unsigned)th );
				std::cout << " " << (int)th << ":" << std::setprecision(4) << b.mean;
			}
			std::cout << "  hemisphericalAlbedo " << H[0] << "\n";
		}
		for( double sg : sigmas ) {
			UniformScalarPainter* ext = new UniformScalarPainter( sg );  ext->addref();
			CompositeMaterial* comp = MakeComposite( *smooth, *sb.m, 3, 3, 3, 3, 3, 1.0, *ext );
			CoatedMaterial* coat = MakeCoated( *sb.m, sg, *f.white );
			int ti = 0;
			for( double th : thetas ) {
				const FurnaceStats sc = Furnace( *coat->GetSPF(), th, false, false, 8, 20000, 7201u + (unsigned)th );
				const FurnaceStats sp = Furnace( *comp->GetSPF(), th, false, false, 8, 20000, 9201u + (unsigned)th );
				const double ratio = sc.mean / sp.mean;
				const double semR = ratio * std::sqrt( std::pow( sc.sem / sc.mean, 2 ) + std::pow( sp.sem / sp.mean, 2 ) );
				std::cout << "      " << sb.name << " sigma_t " << std::setprecision(3) << sg << " theta " << (int)th
				          << ": coated " << std::setprecision(5) << sc.mean << " +- " << sc.sem
				          << "  composite " << sp.mean << " +- " << sp.sem
				          << "  coated/composite " << ratio << " +- " << semR << "\n";
				const double pin = sb.pin[si][ti];
				Check( std::fabs( ratio - pin ) <= 0.015 + 5.0 * semR,
					std::string( "[K8] " ) + sb.name + " sigma " + std::to_string( sg ) + " theta " +
					std::to_string( (int)th ) + " coated / composite at its DL-417 residual pin " + std::to_string( pin ) + " +- (0.015 + 5 sem)" );
				if( sb.lossless && sg == 0.0 ) {
					// A lossless substrate under a clear coat: the coated furnace
					// must not exceed 1 beyond K5's band (measured 1.0084 at 0
					// deg -- the outer-frame model's own gain, DL-417).
					Check( sc.mean <= 1.012 + 5.0 * sc.sem,
						std::string( "[K8] " ) + sb.name + " theta " + std::to_string( (int)th ) +
						" lossless clear-coat furnace <= 1.012 (" + std::to_string( sc.mean ) + ")" );
				}
				++ti;
			}
			coat->release(); comp->release(); ext->release();
			++si;
		}
	}
	fabLamb3->release(); fabLamb7->release(); lambGrey->release(); grey->release();
	sr3->release(); sr7->release(); sheenWhite->release(); smooth->release(); sDelta->release();
}


//////////////////////////////////////////////////////////////////////
//  Section X -- DL-296 (2026-10-09): transmission OUT THROUGH a
//  transmitting bottom is priced by the layered evaluator (term c), so a
//  DELTA light behind the sheet reaches the camera through it.
//
//  Left half: composite{glass/B} (thickness 0) on an open double-sided
//  quad at z = 0.  Right half: the equivalent pair of SEPARATE sheets -- a
//  glass quad at z = 0 and a B quad just below it.  Camera in front (+z);
//  lights are listed below.  Pre-DL-296 the
//  composite half read exactly 0 under a delta light under every
//  integrator (the whole transmitted class was delta-tagged walker
//  transport, invisible to NEE and to connections) while the separate
//  pair read its true value.  Both halves carry the same 1/eta^2 (the ray
//  that crossed the glass is inside index 1.5, the dielectric's
//  separate-sheets convention).
//
//  Bottoms B (both translucent_material; a thin-transmission weave bottom
//  is out of the DL-296 scope, DL-472):
//    translucent             ref 0.3, tau 0.7, ext 0, N 3 -- NOT
//                            reciprocal on its own (DL-223); consistent
//                            here because a light-subpath connection is
//                            the ADJOINT of the eye-side value (DL-472
//                            (1), ImportanceSwap), so one function prices
//                            each path from both ends.
//    lambertian translucent  ref 0, tau 1, ext 0, N 1 -- reciprocal.
//  Lights: a WHITE FURNACE (both faces lit -- the case DL-472 (1)'s
//  two-sided model exists for; before it BDPT / VCM read 1.06x / 1.17x PT
//  on the translucent row), an omni (delta) light behind, under PT / BDPT
//  / VCM and PT spectral (hwss off and on: the covered transmission's
//  companion weight is the layered value WITHOUT its eta^2,
//  EvaluateLobeFNM), and a small area emitter behind under PT.  Bands:
//  composite / pair and composite / PT 1 % (2 % for the spectral rows).
//  Each row is the mean of 3 salted renders.
//////////////////////////////////////////////////////////////////////
static std::string PtSpectralRasterizer( bool hwss, int spp )
{
	std::ostringstream s;
	s << "standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	  << "pathtracing_spectral_rasterizer\n{\n\tsamples " << spp << "\n\toidn_denoise FALSE\n\tpixel_filter box\n"
	  << "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss " << ( hwss ? "true" : "false" ) << "\n";
	s << "}\n\nfile_rasterizeroutput\n{\n\tpattern rendered/composite_energy_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n";
	return s.str();
}

static void SectionX()
{
	std::cout << "\n[X] DL-296: delta / area light BEHIND a composite{glass/B} sheet\n";
	const std::string common =
		"uniformcolor_painter\n{\n\tname pnt_tr\n\tcolor 0.3 0.3 0.3\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tt\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_em\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_em\n\texitance pnt_em\n\tscale 40.0\n\tmaterial none\n}\n\n";
	struct Bottom { const char* name; std::string text; double bdptVcmBand; };
	const Bottom bottoms[] = {
		{ "translucent", "translucent_material\n{\n\tname mat_b\n\tref pnt_tr\n\ttau pnt_tt\n\text 0\n\tN 3\n\tscattering 0\n}\n\n", 0.01 },
		{ "lambertian translucent", "uniformcolor_painter\n{\n\tname pnt_z\n\tcolor 0 0 0\n}\n\nuniformcolor_painter\n{\n\tname pnt_one\n\tcolor 1 1 1\n}\n\n"
		  "translucent_material\n{\n\tname mat_b\n\tref pnt_z\n\ttau pnt_one\n\text 0\n\tN 1\n\tscattering 0\n}\n\n", 0.01 },
	};
	struct Light { const char* name; std::string text; bool area; };
	const Light lights[] = {
		{ "white furnace", "uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n", false },
		{ "omni (delta)", "omni_light\n{\n\tname lgt\n\tposition 0 0 -3\n\tcolor 1.0 1.0 1.0\n\tpower 40.0\n}\n\n", false },
		{ "area quad",    std::string( "clippedplane_geometry\n{\n\tname g_em\n\tpta -0.5 -0.5 -3\n\tptb 0.5 -0.5 -3\n\tptc 0.5 0.5 -3\n\tptd -0.5 0.5 -3\n\tdoublesided FALSE\n}\n\n"
		                               "standard_object\n{\n\tname em\n\tgeometry g_em\n\tmaterial mat_em\n}\n\n" ), true },
	};
	unsigned seed = 296001u;
	for( const Bottom& B : bottoms ) {
		const std::string mats = common + B.text +
			"composite_material\n{\n\tname mat_comp\n\ttop mat_glass\n\tbottom mat_b\n\tthickness 0\n\textinction 0.0\n}\n\n";
		for( const Light& L : lights ) {
			const bool envProbe = ( L.text.find( "pnt_env" ) != std::string::npos );
			double ptComposite = -1;
			for( int r = 0; r < 5; ++r ) {
				if( L.area && r != 0 ) continue;
				if( envProbe && r >= 3 ) continue;		// the spectral helper has no environment
				const int spp = L.area ? 1024 : 256;
				std::string scene = std::string( "RISE ASCII SCENE 7\n" ) +
					"film\n{\n\twidth 32\n\theight 16\n}\n\n"
					"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n" +
					mats + MatrixQuad( "q_comp", -3.9, -0.1, 0.0, 0, true ) +
					MatrixQuad( "q_glass", 0.1, 3.9, 0.0, 0, true ) + MatrixQuad( "q_b", 0.1, 3.9, -0.002, 0, true ) +
					"standard_object\n{\n\tname C\n\tgeometry q_comp\n\tmaterial mat_comp\n}\n\n"
					"standard_object\n{\n\tname G\n\tgeometry q_glass\n\tmaterial mat_glass\n}\n\n"
					"standard_object\n{\n\tname T\n\tgeometry q_b\n\tmaterial mat_b\n}\n\n" + L.text +
					( r == 0 ? PtRasterizer( envProbe, spp ) : r == 1 ? BdptRasterizer( envProbe, spp, 12 ) : r == 2 ? VcmRasterizer( envProbe, spp, 12 )
					  : PtSpectralRasterizer( r == 4, spp ) );
				if( const char* ld = std::getenv( "DL296_LIGHT_DEPTH" ) ) {	// opt-in diagnosis: cap the light subpath
					std::string& sc = scene;
					const size_t at = sc.find( "max_light_depth 12" );
					if( at != std::string::npos ) sc.replace( at, 18, std::string( "max_light_depth " ) + ld );
				}
				const int kReps = 3;
				double mL = 0, mR = 0, rMin = 1e30, rMax = -1e30;
				bool ok = true;
				for( int k = 0; k < kReps; ++k ) {
					CapturingRasterizerOutput* cap = 0;
					// Salted: an unsalted repeat reuses the same Sobol' points.
					SobolSamplerTestHooks::ValueSalt().store( 0x9E3779B9u * seed + 0x85EBCA6Bu );
					const bool okk = Render( scene, "dl296", cap, seed++ );
					SobolSamplerTestHooks::ValueSalt().store( 0u );
					const double l = okk ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
					const double rr = okk ? RegionMean( *cap, cap->width / 2 + 2, cap->width - 2 ) : -1;
					if( cap ) safe_release( cap );
					ok = ok && okk && l >= 0 && rr > 0;
					mL += l / kReps;
					mR += rr / kReps;
					if( rr > 0 ) { rMin = std::min( rMin, l / rr ); rMax = std::max( rMax, l / rr ); }
				}
				const char* in = ( r == 0 ) ? "PT  " : ( r == 1 ) ? "BDPT" : ( r == 2 ) ? "VCM " : ( r == 3 ) ? "PT spectral" : "PT spectral hwss";
				std::cout << "    X " << B.name << ", " << L.name << ", " << in << ": composite " << std::setprecision(5) << mL
				          << " | separate pair " << mR << "  ratio " << ( mR > 0 ? mL / mR : -1 )
				          << "  (per-render [" << rMin << ", " << rMax << "])\n";
				const std::string tag = std::string( B.name ) + ", " + L.name + " (" + in + ")";
				const double band = ( r >= 3 ) ? 0.02 : B.bdptVcmBand;
				Check( ok && mR > 0, "[X] separate pair is lit, " + tag );
				Check( ok && mR > 0 && std::fabs( mL / mR - 1.0 ) <= band, "[X] composite == separate pair, " + tag );
				if( r == 0 ) {
					ptComposite = mL;
				} else if( ptComposite > 0 && r <= 2 ) {
					Check( ok && std::fabs( mL / ptComposite - 1.0 ) <= band, "[X] composite agrees with PT, " + tag );
				}
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
//  Section R -- DL-472 (1) two-sided model, SPF level (--dl472-unit).
//
//  A composite{glass / T} on a PROVABLY OPEN sheet (+z true normal),
//  T a lossless, reciprocal transmitter (translucent ref 0, tau 1, ext 0,
//  scattering 0, N 1: a cosine transmission lobe from either side).
//  R1  reciprocity: value(wi = a -> wo = b) from a FRONT arrival equals
//      (n_front / n_back)^2 value(b -> a) from the mirrored BACK arrival
//      (n_back the sheet's below medium, 1.5 -- the radiance BSDF across
//      an index step), averaged over jittered positions (value is one MC
//      estimate per query).
//  R2  back arrivals: every non-delta ray satisfies
//      kray * Pdf * eta^2(stack) == value * cos (one function per side).
//  R3  back-arrival furnace: E[sum kray * eta^2] over both sides == 1
//      (lossless stack) -- the mirror of section F5.
//////////////////////////////////////////////////////////////////////
static RayIntersectionGeometric MakeSheetIntersection( const Vector3& inDir, const Point3& p )
{
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( Ray( Point3( p.x - inDir.x, p.y - inDir.y, p.z - inDir.z ), inDir ), rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = p;
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	ri.bProvablyNoInterior = true;
	return ri;
}

static void RunSectionR( const char* label, const IMaterial& m, const bool gateReciprocity )
{
	const ISPF& spf = *m.GetSPF();
	const IBSDF& bsdf = *m.GetBSDF();
	const double nBack = 1.5;
	struct Pair { double thA, phA, thB, phB; };
	const Pair pairs[] = { { 0, 0, 0, 0 }, { 30, 0, 20, 90 }, { 60, 45, 40, 200 }, { 10, 0, 70, 180 } };
	for( const Pair& pp : pairs ) {
		auto dir = []( double th, double ph, double sz ) {
			const double t = th * kPi / 180.0, q = ph * kPi / 180.0;
			return Vector3( std::sin( t ) * std::cos( q ), std::sin( t ) * std::sin( q ), sz * std::cos( t ) );
		};
		const Vector3 a = dir( pp.thA, pp.phA, 1 );		// front side
		const Vector3 b = dir( pp.thB, pp.phB, -1 );	// back side
		RandomNumberGenerator rng( 4720u );
		double sF = 0, sB = 0, sF2 = 0, sB2 = 0;
		const int N = 40000;
		for( int i = 0; i < N; ++i ) {
			const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
			IORStack st = MakeTestIORStack( g_stub );
			const RayIntersectionGeometric rf = MakeSheetIntersection( -a, p );
			const RayIntersectionGeometric rb = MakeSheetIntersection( -b, p );
			const double vf = ColorMath::MaxValue( bsdf.valueStateful( b, rf, &st ) );
			const double vb = ColorMath::MaxValue( bsdf.valueStateful( a, rb, &st ) );
			sF += vf; sF2 += vf * vf; sB += vb; sB2 += vb * vb;
		}
		const double mF = sF / N, mB = sB / N;
		const double eF = std::sqrt( std::max( 0.0, sF2 / N - mF * mF ) / N ), eB = std::sqrt( std::max( 0.0, sB2 / N - mB * mB ) / N );
		const double k = 1.0 / ( nBack * nBack );
		const double rel = ( mB > 0 ) ? mF / ( k * mB ) : -1;
		std::cout << "    R1 " << label << " a(" << pp.thA << "," << pp.phA << ") b(" << pp.thB << "," << pp.phB << "): front f(a->b) " << std::setprecision(5) << mF << " +- " << eF
		          << ", back f(b->a) " << mB << " +- " << eB << ", f_front / ((1/n^2) f_back) = " << rel << "\n";
		if( gateReciprocity ) {
			const double tol = 4 * std::sqrt( eF * eF + k * k * eB * eB ) / std::max( 1e-12, k * mB ) + 0.01;
			Check( mF > 0 && mB > 0 && std::fabs( rel - 1 ) <= tol, std::string( "[R1] " ) + label + " reciprocity across the sheet, pair " + std::to_string( (int)pp.thA ) + "/" + std::to_string( (int)pp.thB ) );
		}
	}
	// R2 / R3 at back arrivals.
	for( int t = 0; t < 3; ++t ) {
		const double th = kThetas[t] * kPi / 180.0;
		const Vector3 inDir( std::sin( th ), 0, std::cos( th ) );	// travelling +z: a back arrival
		RandomNumberGenerator rng( 4721u + t );
		IndependentSampler sampler( rng );
		int bad = 0, nonDelta = 0;
		double sum = 0;
		const int N = 40000;
		for( int i = 0; i < N; ++i ) {
			const Point3 p( rng.CanonicalRandom() * 10, rng.CanonicalRandom() * 10, 0 );
			const RayIntersectionGeometric ri = MakeSheetIntersection( inDir, p );
			IORStack st = MakeTestIORStack( g_stub );
			ScatteredRayContainer sc;
			spf.Scatter( ri, sampler, sc, st );
			for( unsigned j = 0; j < sc.Count(); ++j ) {
				const ScatteredRay& r = sc[j];
				sum += ColorMath::MaxValue( r.kray );
				if( r.isDelta ) continue;
				nonDelta++;
				const double eta = RadianceEtaScale( st, r.ior_stack );
				const Vector3 d = Vector3Ops::Normalize( r.ray.Dir() );
				const double pdf = spf.Pdf( ri, d, st );
				const double lhs = ColorMath::MaxValue( r.kray ) * pdf * eta;
				const double rhs = ColorMath::MaxValue( bsdf.valueStateful( d, ri, &st ) ) * std::fabs( d.z );
				const double rel = std::fabs( lhs - rhs ) / std::max( 1e-12, std::fabs( rhs ) );
				if( rel > 1e-9 ) bad++;
			}
		}
		std::cout << "    R2/R3 " << label << " back arrival theta " << kThetas[t] << ": non-delta " << nonDelta << ", kray*Pdf*eta^2 mismatches " << bad
		          << ", furnace E[sum kray] " << std::setprecision(5) << sum / N << "\n";
		Check( nonDelta > 1000 && bad == 0, std::string( "[R2] " ) + label + " back arrival: kray*Pdf*eta^2 == value*cos, theta " + std::to_string( (int)kThetas[t] ) );
		if( gateReciprocity ) {
			Check( std::fabs( sum / N - 1.0 ) <= 0.02, std::string( "[R3] " ) + label + " back-arrival furnace (lossless stack) == 1, theta " + std::to_string( (int)kThetas[t] ) );
		}
	}
}

static void SectionR( Fixtures& f )
{
	std::cout << "\n[R] DL-472 (1): two-sided composite model on an open sheet (SPF level)\n";
	UniformColorPainter* zero = new UniformColorPainter( RISEPel( 0, 0, 0 ) ); zero->addref();
	TranslucentMaterial* tLam = new TranslucentMaterial( *zero, *f.white, *f.s0, *f.s1, *f.s0 ); tLam->addref();
	CompositeMaterial* m = MakeComposite( *f.dSmooth, *tLam, 3, 3, 3, 3, 3, 0.0, *f.s0 );
	RunSectionR( "glass/lambertian-translucent", *m, true );
	m->release(); tLam->release(); zero->release();

}

//! Opt-in measurement (--dl296-probe, no assertions): a composite
//! {glass/translucent} closed box with an emitter INSIDE it (Section M's
//! "light inside" cell, double-sided outward), composite half only, PT /
//! BDPT / VCM, 3 salted renders each.  The emitter is unseeded (DL-407 (1)),
//! so light-side walks meet the wall's top from inside.  DL296_PROBE_OUT=1
//! moves the emitter outside, behind the box; DL296_PROBE_SEED offsets the
//! salts.  This is the configuration that showed term (c) must stay off on
//! a surface that may bound an interior: with it live there BDPT / VCM
//! read 0.435 / 0.417 against PT 0.152 (light inside) and 0.0102 / 0.0071
//! against 0.0525 (light outside); master and the shipped (open-sheet-only)
//! rule agree across PT / BDPT / VCM within the renders' noise.
static void ProbeLightInside()
{
	std::cout << "\n[probe] composite{glass/translucent} box, light inside\n";
	const std::string scene0 = std::string( "RISE ASCII SCENE 7\n" ) +
		"film\n{\n\twidth 32\n\theight 16\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 7.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_e\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_w8\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tr\n\tcolor 0.3 0.3 0.3\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_tt\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"lambertian_material\n{\n\tname mat_l8\n\treflectance pnt_w8\n}\n\n"
		"dielectric_material\n{\n\tname mat_glass\n\ttau 1\n\tior 1.5\n}\n\n"
		"translucent_material\n{\n\tname mat_tr\n\tref pnt_tr\n\ttau pnt_tt\n\text 0\n\tN 10\n\tscattering 0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_e\n\tscale 20.0\n\tmaterial none\n}\n\n"
		"composite_material\n{\n\tname mat_gtr\n\ttop mat_glass\n\tbottom mat_tr\n\tthickness 0.05\n\textinction 0.2\n}\n\n" +
		WindingBox( "bT", std::vector<int>(), true ) +
		"standard_object\n{\n\tname L\n\tgeometry bT\n\tposition -2 0 0\n\tmaterial mat_gtr\n}\n\n"
		"sphere_geometry\n{\n\tname sg\n\tradius 0.3\n}\n\n"
		"clippedplane_geometry\n{\n\tname fq\n\tpta -6 -3.5 -4\n\tptb -6 -3.5 4\n\tptc 6 -3.5 4\n\tptd 6 -3.5 -4\n}\n\n"
		+ ( std::getenv( "DL296_PROBE_OUT" ) ? "standard_object\n{\n\tname eL\n\tgeometry sg\n\tposition -2 0 -2.5\n\tmaterial mat_emit\n}\n\n"
		                                   : "standard_object\n{\n\tname eL\n\tgeometry sg\n\tposition -2 0 0\n\tmaterial mat_emit\n}\n\n" ) +
		"standard_object\n{\n\tname F\n\tgeometry fq\n\tmaterial mat_l8\n}\n\n";
	unsigned seed = 296500u + ( std::getenv( "DL296_PROBE_SEED" ) ? (unsigned)std::atoi( std::getenv( "DL296_PROBE_SEED" ) ) : 0u );
	for( int r = 0; r < 3; ++r ) {
		const std::string scene = scene0 + ( r == 0 ? PtRasterizer( false, 1024 ) : r == 1 ? BdptRasterizer( false, 1024, 12 ) : VcmRasterizer( false, 1024, 12 ) );
		double m = 0;
		for( int k = 0; k < 3; ++k ) {
			CapturingRasterizerOutput* cap = 0;
			SobolSamplerTestHooks::ValueSalt().store( 0x9E3779B9u * seed + 0x85EBCA6Bu );
			const bool ok = Render( scene, "dl296probe", cap, seed++ );
			SobolSamplerTestHooks::ValueSalt().store( 0u );
			const double l = ok ? RegionMean( *cap, 2, cap->width / 2 - 2 ) : -1;
			if( cap ) safe_release( cap );
			m += l / 3;
		}
		std::cout << "    " << ( r == 0 ? "PT  " : r == 1 ? "BDPT" : "VCM " ) << ": " << std::setprecision(5) << m << "\n";
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
	if( argc > 1 && std::string( argv[1] ) == "--dl417-only" ) {
		SectionK8( f );
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--coated-only" ) {
		SectionK( f );
		SectionK8( f );
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--stack-only" ) {
		// DL-341 / DL-297 rows only: H, T, W (and D with --render).
		SectionH( f );
		SectionT( f );
		SectionW( f );
		if( argc > 2 && std::string( argv[2] ) == "--render" ) { SectionD(); SectionD10(); }
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--dl472-unit" ) {
		SectionR( f );
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--dl296-probe" ) {
		ProbeLightInside();
		return 0;
	}
	if( argc > 1 && std::string( argv[1] ) == "--dl296-only" ) {
		SectionX();
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--d10-only" ) {
		SectionD10();
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--sidedness-only" ) {
		SectionM();
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--sheets-only" ) {
		g_matrixSheetsOnly = true;
		SectionM();
		std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	if( argc > 1 && std::string( argv[1] ) == "--winding-only" ) {
		SectionD9();
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
	SectionK8( f );
	if( !skipRender ) {
		SectionD();
		SectionD10();
	}

	std::cout << "\n================================================" << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
