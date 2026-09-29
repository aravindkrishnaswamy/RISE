//////////////////////////////////////////////////////////////////////
//
//  PolishedBRDFConsistencyTest.cpp - Red-proof and regression guard for
//    DL-285: `polished_material`'s `IBSDF::value` and its SPF's `kray`
//    must describe ONE function (DL-67's premise 2, f_BSDF == f_SPF).
//
//  Before DL-285 `PolishedMaterial::GetBSDF()` was a bare
//  `LambertianBRDF(Rd)` while `PolishedSPF` sampled a Fresnel-weighted
//  coat lobe plus a `(1-F)`-weighted diffuse substrate, so NEE, every
//  BDPT/VCM connection and the DL-67 guide draw (which price `value`)
//  and the BSDF-sampled continuation (which prices `kray`) integrated
//  two different functions.
//
//  GATES (every row, both pipes where noted):
//
//    1. ENERGY / ALBEDO.  S = E[ sum over emitted NON-delta rays of kray ]
//       (the SPF side of every estimator that sums or selects lobes),
//       against B = integral of value(wo) * |cos(wo, n)| over the sphere
//       (the BSDF side, deterministic quadrature).  Per channel,
//       |S/B - 1| within MC error.  Pre-fix B is the bare Lambertian's
//       Rd while S carries the coat and the (1-F) substrate.
//    2. SHAPE.  Total variation between the SPF's energy histogram
//       (kray binned by direction) and value*cos quadrature per bin,
//       gated against a MEASURED floor (two independent halves).
//    3. RECIPROCITY of value: value(a->b) == value(b->a) to 1e-9 relative
//       (the model is required to be reciprocal -- DL-127's ruling), and
//       of the SPF's own per-lobe evaluator (`EvaluateLobeFNM` summed
//       over lobes), which is what the pre-fix kray implied.
//    4. DENSITY.  Pdf integrates to the measured probability that
//       `RandomlySelect` returns a non-delta ray, and matches the
//       histogram of the directions it returns (TVD against a floor).
//       Gates 2 and 4b run twice: in 8x16 shading-frame bins and (2L/4bL)
//       in LOBE-frame bins -- Phong-CDF polar bins about the mirror
//       direction -- which alone resolve a N 256 / 350 coat lobe.
//    5. HWSS.  `EvaluateKrayNM` at the hero wavelength reproduces every
//       emitted ray's `krayNM` exactly (the companion ladders' premise).
//    6. SIBLING AUDIT (DL-285's pattern: a material whose GetBSDF() is a
//       different function than its SPF samples).  Every pair already in
//       SPFBSDFConsistencyTest's furnace is covered there; the two that
//       were not are run here: `sheen_material` (gates 1-4, green) and
//       `datadriven_material`, which has a BSDF and NO SPF at all -- the
//       sampled function is identically 0 -- pinned as DL-325.
//    0. The model's building blocks (post-fix API; the gate-1..5 rows are
//       the red-proof and compile against the pre-fix tree): the model's
//       g-form Fresnel equals `Optics::CalculateDielectricReflectanceCosine`,
//       the closed-form hemispherical transmittance equals a brute-force
//       quadrature of it (internal and external interfaces, eta -> 1), and
//       `PolishedSPF::Pdf`'s per-thread replay memo returns bit-identical
//       values regardless of call order.
//
//  Pass `--skip-hg` to skip the Henyey-Greenstein rows.  The pre-fix HG
//  coat sampler "truncated" with a `do { } while( alpha > PI/2 )` loop
//  that re-evaluated the SAME random number, an infinite loop in the
//  C++ abstract machine for a draw below the forward-hemisphere threshold
//  (undefined behaviour; the optimized build drops the loop and draws the
//  untruncated lobe, a -O0 build hangs), so the red-proof run skipped
//  them.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `debt-dl285`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/PolishedMaterial.h"
#include "../src/Library/Materials/PolishedBRDF.h"
#include "../src/Library/Materials/SheenMaterial.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/DataDrivenMaterial.h"
#include <fstream>
#include <unistd.h>

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool ok, const std::string& what )
{
	if( ok ) {
		++g_pass;
	} else {
		++g_fail;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

static StubObject* g_stub = 0;

// --------------------------------------------------------------------
// Fixture: a surface at the origin.  The incoming ray lies in the x-z
// plane at `thetaDeg` from `vGeomNormal`; `tiltDeg` tilts the SHADING
// normal about y; `backface` makes the ray arrive from below the
// geometric normal (single-sided geometry struck from behind).
// --------------------------------------------------------------------
struct Fixture
{
	double thetaDeg;
	double tiltDeg;
	bool   backface;
};

static RayIntersectionGeometric MakeRI( const Fixture& fx )
{
	const double t = fx.thetaDeg * PI / 180.0;
	const double s = std::sin( t ), c = std::cos( t );
	// Ray travels TOWARD the surface: from above (dir.z < 0) for a front
	// hit, from below (dir.z > 0) for a back-face hit.
	const Vector3 dir = fx.backface ? Vector3( s, 0, c ) : Vector3( s, 0, -c );
	const Point3  org = fx.backface ? Point3( -s, 0, -c ) : Point3( -s, 0, c );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( Ray( org, dir ), rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	const double tt = fx.tiltDeg * PI / 180.0;
	ri.vNormal = Vector3( std::sin( tt ), 0, std::cos( tt ) );
	ri.onb.CreateFromW( ri.vNormal );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}

// A copy of `ri` whose incoming ray arrives along -wi (so wi is the
// direction toward the viewer).  Used for reciprocity swaps.
static RayIntersectionGeometric WithIncoming( const RayIntersectionGeometric& base, const Vector3& wi )
{
	RayIntersectionGeometric ri = base;
	ri.ray = Ray( Point3( wi.x, wi.y, wi.z ), -wi );
	return ri;
}

// --------------------------------------------------------------------
// Direction binning in the SHADING-normal frame: 8 bins in 1-cos^2
// (equal projected solid angle) x 16 in azimuth, over the full sphere
// (the sign of cos picks the half; 32 x 16 bins in total).
// --------------------------------------------------------------------
static const int kBinT = 8, kBinP = 16;
static const int kBins = 2 * kBinT * kBinP;

static int BinOf( const Vector3& w, const OrthonormalBasis3D& onb )
{
	const double z = Vector3Ops::Dot( w, onb.w() );
	const double x = Vector3Ops::Dot( w, onb.u() );
	const double y = Vector3Ops::Dot( w, onb.v() );
	const double u = 1.0 - z * z;
	int bt = int( u * kBinT );  if( bt >= kBinT ) bt = kBinT - 1;  if( bt < 0 ) bt = 0;
	double ph = std::atan2( y, x );  if( ph < 0 ) ph += 2 * PI;
	int bp = int( ph / ( 2 * PI ) * kBinP );  if( bp >= kBinP ) bp = kBinP - 1;
	return ( z >= 0 ? 0 : kBinT * kBinP ) + bt * kBinP + bp;
}

// Lobe-frame binning (review P3): 8 polar bins in the Phong CDF
// 1 - cos(alpha)^(N+1) about the MIRROR direction (uniform for a Phong
// lobe of exponent N, so a N 256 / 350 lobe spreads over all 8 instead of
// one shading-frame cell) plus one band for cos(alpha) <= 0, x 16 azimuths.
static const int kLobeBinT = 9;
static const int kLobeBins = kLobeBinT * kBinP;

static int LobeBinOf( const Vector3& w, const OrthonormalBasis3D& f, const double N )
{
	const double ca = Vector3Ops::Dot( w, f.w() );
	int bt = kLobeBinT - 1;
	if( ca > 0 ) {
		const double t = 1.0 - std::pow( ca, N + 1.0 );
		bt = int( t * ( kLobeBinT - 1 ) );
		if( bt >= kLobeBinT - 1 ) bt = kLobeBinT - 2;
		if( bt < 0 ) bt = 0;
	}
	double ph = std::atan2( Vector3Ops::Dot( w, f.v() ), Vector3Ops::Dot( w, f.u() ) );
	if( ph < 0 ) ph += 2 * PI;
	int bp = int( ph / ( 2 * PI ) * kBinP );  if( bp >= kBinP ) bp = kBinP - 1;
	return bt * kBinP + bp;
}

static double TVD( const std::vector<double>& a, const std::vector<double>& b )
{
	double sa = 0, sb = 0;
	for( size_t i = 0; i < a.size(); ++i ) { sa += a[i]; sb += b[i]; }
	if( sa <= 0 || sb <= 0 ) return ( sa <= 0 && sb <= 0 ) ? 0.0 : 1.0;
	double d = 0;
	for( size_t i = 0; i < a.size(); ++i ) d += std::fabs( a[i] / sa - b[i] / sb );
	return 0.5 * d;
}

// --------------------------------------------------------------------
// Deterministic quadrature over the sphere in a frame about `axis`,
// dense near the axis (alpha = pi s^2).  Calls fn(w, dOmega).
// --------------------------------------------------------------------
template< class Fn >
static void SphereQuadrature( const Vector3& axis, int ns, int npsi, Fn fn )
{
	OrthonormalBasis3D f;
	f.CreateFromW( axis );
	for( int i = 0; i < ns; ++i ) {
		const double s = ( i + 0.5 ) / ns;
		const double a = PI * s * s;
		const double da = 2.0 * PI * s / ns;
		const double sa = std::sin( a ), ca = std::cos( a );
		for( int j = 0; j < npsi; ++j ) {
			const double p = ( j + 0.5 ) / npsi * 2.0 * PI;
			const Vector3 w = Vector3Ops::Normalize(
				f.w() * ca + f.u() * ( sa * std::cos( p ) ) + f.v() * ( sa * std::sin( p ) ) );
			fn( w, sa * da * ( 2.0 * PI / npsi ) );
		}
	}
}

struct Config
{
	std::string name;
	RISEPel rd;
	double tau;
	double ior;
	double scat;
	bool   hg;
};

// ====================================================================
// One row: gates 1-5 for (config, fixture).
// ====================================================================
static void RunRowMaterial( const IMaterial& mat, const std::string& name, const Fixture& fx, int draws, unsigned int seed, const double lobeN = 1.0 )
{
	const ISPF&  spf  = *mat.GetSPF();
	const IBSDF& bsdf = *mat.GetBSDF();

	const RayIntersectionGeometric ri = MakeRI( fx );
	const IORStack stack = MakeTestIORStack( g_stub );
	const Vector3 nShade = fx.backface ? -ri.vNormal : ri.vNormal;
	OrthonormalBasis3D binOnb;  binOnb.CreateFromW( nShade );

	char label[256];
	std::snprintf( label, sizeof(label), "%s th%.0f%s%s", name.c_str(), fx.thetaDeg,
		fx.tiltDeg != 0 ? " tilt" : "", fx.backface ? " BACKFACE" : "" );

	// ---- SPF side --------------------------------------------------
	RandomNumberGenerator rng( seed );
	IndependentSampler sampler( rng );
	double S[3] = { 0, 0, 0 }, S2[3] = { 0, 0, 0 };
	std::vector<double> histA( kBins, 0.0 ), histB( kBins, 0.0 );
	std::vector<double> selA( kBins, 0.0 ), selB( kBins, 0.0 );
	std::vector<double> lHistA( kLobeBins, 0.0 ), lHistB( kLobeBins, 0.0 );
	std::vector<double> lSelA( kLobeBins, 0.0 ), lSelB( kLobeBins, 0.0 );
	OrthonormalBasis3D lobeOnb;  lobeOnb.CreateFromW( Optics::CalculateReflectedRay( ri.ray.Dir(), nShade ) );
	long selectedNonDelta = 0;
	double worstKrayNM = 0;
	for( int k = 0; k < draws; ++k ) {
		ScatteredRayContainer sc_;
		spf.Scatter( ri, sampler, sc_, stack );
		double sum[3] = { 0, 0, 0 };
		for( unsigned int j = 0; j < sc_.Count(); ++j ) {
			const ScatteredRay& r = sc_[j];
			if( r.isDelta ) continue;
			for( int c = 0; c < 3; ++c ) sum[c] += r.kray[c];
			const int b = BinOf( Vector3Ops::Normalize( r.ray.Dir() ), binOnb );
			const double e = ( r.kray[0] + r.kray[1] + r.kray[2] ) / 3.0;
			( ( k & 1 ) ? histB : histA )[b] += e;
			( ( k & 1 ) ? lHistB : lHistA )[LobeBinOf( Vector3Ops::Normalize( r.ray.Dir() ), lobeOnb, lobeN )] += e;
		}
		for( int c = 0; c < 3; ++c ) { S[c] += sum[c];  S2[c] += sum[c] * sum[c]; }

		// Gate 4 sampling side: the direction RandomlySelect returns.
		const ScatteredRay* pSel = sc_.RandomlySelect( rng.CanonicalRandom(), false );
		if( pSel && !pSel->isDelta ) {
			++selectedNonDelta;
			const int b = BinOf( Vector3Ops::Normalize( pSel->ray.Dir() ), binOnb );
			( ( k & 1 ) ? selB : selA )[b] += 1.0;
			( ( k & 1 ) ? lSelB : lSelA )[LobeBinOf( Vector3Ops::Normalize( pSel->ray.Dir() ), lobeOnb, lobeN )] += 1.0;
		}
	}
	// Gate 5 on the NM pipe: EvaluateKrayNM reproduces krayNM.
	{
		RandomNumberGenerator rng2( seed + 7 );
		IndependentSampler s2( rng2 );
		for( int k = 0; k < 20000; ++k ) {
			ScatteredRayContainer sc_;
			spf.ScatterNM( ri, s2, 550.0, sc_, stack );
			for( unsigned int j = 0; j < sc_.Count(); ++j ) {
				const ScatteredRay& r = sc_[j];
				const Scalar e = spf.EvaluateKrayNM( ri, r.ray.Dir(), r.type, 550.0, stack );
				if( e < 0 ) continue;		// declined: the companion ladder falls back (not polished's case)
				const double d = std::fabs( e - r.krayNM ) / std::max( 1e-12, std::fabs( r.krayNM ) );
				if( r.krayNM > 1e-9 || e > 1e-9 ) worstKrayNM = std::max( worstKrayNM, d );
			}
		}
	}

	// ---- BSDF side (quadrature about the shading mirror direction) -
	const Vector3 wiView = -ri.ray.Dir();
	const Vector3 axis = Optics::CalculateReflectedRay( ri.ray.Dir(), nShade );
	double B[3] = { 0, 0, 0 };
	double pdfMass = 0;
	std::vector<double> histQ( kBins, 0.0 ), histPdf( kBins, 0.0 );
	std::vector<double> lHistQ( kLobeBins, 0.0 ), lHistPdf( kLobeBins, 0.0 );
	SphereQuadrature( axis, 1500, 512, [&]( const Vector3& w, double dOm ) {
		const RISEPel f = bsdf.value( w, ri );
		const double cw = std::fabs( Vector3Ops::Dot( w, ri.vNormal ) );
		const int b = BinOf( w, binOnb );
		for( int c = 0; c < 3; ++c ) B[c] += f[c] * cw * dOm;
		histQ[b] += ( f[0] + f[1] + f[2] ) / 3.0 * cw * dOm;
		const double p = spf.Pdf( ri, w, stack );
		pdfMass += p * dOm;
		histPdf[b] += p * dOm;
		const int lb = LobeBinOf( w, lobeOnb, lobeN );
		lHistQ[lb] += ( f[0] + f[1] + f[2] ) / 3.0 * cw * dOm;
		lHistPdf[lb] += p * dOm;
	} );
	(void)wiView;

	// ---- gates -----------------------------------------------------
	bool okAlbedo = true;
	double worstRel = 0;
	for( int c = 0; c < 3; ++c ) {
		const double mean = S[c] / draws;
		const double var = std::max( 0.0, S2[c] / draws - mean * mean );
		const double se = std::sqrt( var / draws );
		const double denom = std::max( 1e-9, B[c] );
		const double rel = ( mean - B[c] ) / denom;
		if( std::fabs( rel ) > std::fabs( worstRel ) ) worstRel = rel;
		// 4 standard errors plus the quadrature's own resolution (2e-3 relative).
		if( std::fabs( mean - B[c] ) > 4.0 * se + 2e-3 * denom + 1e-6 ) okAlbedo = false;
	}
	std::vector<double> histS( kBins, 0.0 );
	for( int b = 0; b < kBins; ++b ) histS[b] = histA[b] + histB[b];
	const double tvdShape = TVD( histS, histQ );
	const double shapeFloor = TVD( histA, histB );

	std::vector<double> histSel( kBins, 0.0 );
	for( int b = 0; b < kBins; ++b ) histSel[b] = selA[b] + selB[b];
	const double emitRate = double( selectedNonDelta ) / draws;
	const double tvdPdf = TVD( histSel, histPdf );
	const double pdfFloor = TVD( selA, selB );
	std::vector<double> lHistS( kLobeBins, 0.0 ), lSel( kLobeBins, 0.0 );
	for( int b = 0; b < kLobeBins; ++b ) { lHistS[b] = lHistA[b] + lHistB[b];  lSel[b] = lSelA[b] + lSelB[b]; }
	const double lTvdShape = TVD( lHistS, lHistQ ), lShapeFloor = TVD( lHistA, lHistB );
	const double lTvdPdf = TVD( lSel, lHistPdf ), lPdfFloor = TVD( lSelA, lSelB );

	std::cout << "  " << std::left << std::setw( 40 ) << label << std::right << std::fixed << std::setprecision( 5 )
	          << " S/B-1=" << std::setw( 9 ) << worstRel
	          << " (S0=" << S[0] / draws << " B0=" << B[0] << ")"
	          << " TVDshape=" << tvdShape << " [floor " << shapeFloor << "]"
	          << " intPdf=" << pdfMass << " emit=" << emitRate
	          << " TVDpdf=" << tvdPdf << " [floor " << pdfFloor << "]"
	          << " lobe-frame TVDshape/pdf=" << lTvdShape << "/" << lTvdPdf << " [floors " << lShapeFloor << "/" << lPdfFloor << "]"
	          << std::scientific << std::setprecision( 1 ) << " krayNM=" << worstKrayNM
	          << std::defaultfloat << std::endl;

	Check( okAlbedo, std::string( "gate 1 (albedo S==B per channel): " ) + label );
	Check( tvdShape <= std::max( 3.0 * shapeFloor, 0.01 ), std::string( "gate 2 (energy shape vs value*cos): " ) + label );
	Check( std::fabs( pdfMass - emitRate ) <= 0.01 + 4.0 * std::sqrt( emitRate * ( 1 - emitRate ) / draws ),
		std::string( "gate 4a (integral Pdf == emission probability): " ) + label );
	Check( tvdPdf <= std::max( 3.0 * pdfFloor, 0.015 ), std::string( "gate 4b (Pdf shape vs RandomlySelect histogram): " ) + label );
	Check( lTvdShape <= std::max( 3.0 * lShapeFloor, 0.01 ), std::string( "gate 2L (energy shape, lobe-frame bins): " ) + label );
	Check( lTvdPdf <= std::max( 3.0 * lPdfFloor, 0.015 ), std::string( "gate 4bL (Pdf shape, lobe-frame bins): " ) + label );
	Check( worstKrayNM <= 1e-9, std::string( "gate 5 (EvaluateKrayNM reproduces krayNM): " ) + label );
}

static void RunRow( const Config& cfg, const Fixture& fx, int draws, unsigned int seed )
{
	UniformColorPainter* rd = new UniformColorPainter( cfg.rd );  rd->addref();
	UniformScalarPainter* tau = new UniformScalarPainter( cfg.tau );  tau->addref();
	UniformScalarPainter* nt  = new UniformScalarPainter( cfg.ior );  nt->addref();
	UniformScalarPainter* sc  = new UniformScalarPainter( cfg.scat ); sc->addref();
	PolishedMaterial* mat = new PolishedMaterial( *rd, *tau, *nt, *sc, cfg.hg );  mat->addref();
	// Lobe-frame bins keyed to the Phong exponent (HG and delta coats bin
	// with N 1: their glossy energy is either wide or absent).
	const double lobeN = ( !cfg.hg && cfg.scat < 1e6 ) ? cfg.scat : 1.0;
	RunRowMaterial( *mat, cfg.name, fx, draws, seed, lobeN );
	safe_release( mat );
	safe_release( sc );
	safe_release( nt );
	safe_release( tau );
	safe_release( rd );
}

// ====================================================================
// Gate 3: reciprocity of value(), and of the SPF's per-lobe evaluator.
// ====================================================================
static void RunReciprocity( const Config& cfg, const Fixture& fx )
{
	UniformColorPainter* rd = new UniformColorPainter( cfg.rd );  rd->addref();
	UniformScalarPainter* tau = new UniformScalarPainter( cfg.tau );  tau->addref();
	UniformScalarPainter* nt  = new UniformScalarPainter( cfg.ior );  nt->addref();
	UniformScalarPainter* sc  = new UniformScalarPainter( cfg.scat ); sc->addref();
	PolishedMaterial* mat = new PolishedMaterial( *rd, *tau, *nt, *sc, cfg.hg );  mat->addref();
	const IBSDF& bsdf = *mat->GetBSDF();
	const ISPF&  spf  = *mat->GetSPF();
	const IORStack stack = MakeTestIORStack( g_stub );

	const RayIntersectionGeometric base = MakeRI( fx );
	const Vector3 nShade = fx.backface ? -base.vNormal : base.vNormal;
	OrthonormalBasis3D f;  f.CreateFromW( nShade );
	RandomNumberGenerator rng( 991 );
	double worstValue = 0, worstLobe = 0;
	int nonzero = 0;
	for( int k = 0; k < 4000; ++k ) {
		auto dirAbove = [&]() {
			const double z = 0.02 + 0.98 * rng.CanonicalRandom();
			const double p = 2 * PI * rng.CanonicalRandom();
			const double r = std::sqrt( 1 - z * z );
			return Vector3Ops::Normalize( f.w() * z + f.u() * ( r * std::cos( p ) ) + f.v() * ( r * std::sin( p ) ) );
		};
		const Vector3 a = dirAbove();
		// Half the pairs near the mirror of `a` so the coat lobe is exercised.
		Vector3 b = dirAbove();
		if( k & 1 ) {
			const Vector3 m = Optics::CalculateReflectedRay( -a, nShade );
			b = Vector3Ops::Normalize( m + ( b - m ) * 0.03 );
			if( Vector3Ops::Dot( b, nShade ) <= 0.01 ) continue;
		}
		const RayIntersectionGeometric riA = WithIncoming( base, a );
		const RayIntersectionGeometric riB = WithIncoming( base, b );
		const RISEPel fab = bsdf.value( b, riA );
		const RISEPel fba = bsdf.value( a, riB );
		for( int c = 0; c < 3; ++c ) {
			const double m = std::max( std::fabs( fab[c] ), std::fabs( fba[c] ) );
			if( m > 1e-12 ) {
				++nonzero;
				worstValue = std::max( worstValue, std::fabs( fab[c] - fba[c] ) / m );
			}
		}
		// SPF side: sum of per-lobe BSDF values the SPF prices at 550 nm.
		auto lobeSum = [&]( const RayIntersectionGeometric& r, const Vector3& o ) {
			double s = 0;
			const Scalar fr = spf.EvaluateLobeFNM( r, o, ScatteredRay::eRayReflection, 550.0, stack );
			const Scalar fd = spf.EvaluateLobeFNM( r, o, ScatteredRay::eRayDiffuse,    550.0, stack );
			if( fr > 0 ) s += fr;
			if( fd > 0 ) s += fd;
			return s;
		};
		const double la = lobeSum( riA, b ), lb = lobeSum( riB, a );
		const double lm = std::max( la, lb );
		if( lm > 1e-12 ) worstLobe = std::max( worstLobe, std::fabs( la - lb ) / lm );
	}
	char label[256];
	std::snprintf( label, sizeof(label), "%s%s%s", cfg.name.c_str(),
		fx.tiltDeg != 0 ? " tilt" : "", fx.backface ? " BACKFACE" : "" );
	std::cout << "  " << std::left << std::setw( 40 ) << label << std::right << std::scientific << std::setprecision( 2 )
	          << " value recip worst=" << worstValue << "  SPF-lobe recip worst=" << worstLobe
	          << "  (" << nonzero << " nonzero)" << std::defaultfloat << std::endl;
	Check( nonzero > 0, std::string( "gate 3 exercised a nonzero value: " ) + label );
	Check( worstValue <= 1e-9, std::string( "gate 3a (value reciprocal): " ) + label );
	Check( worstLobe <= 1e-9, std::string( "gate 3b (SPF per-lobe evaluator reciprocal): " ) + label );

	safe_release( mat );
	safe_release( sc );
	safe_release( nt );
	safe_release( tau );
	safe_release( rd );
}

// ====================================================================
// Gate 0: building blocks.
// ====================================================================
static void RunBuildingBlocks()
{
	std::cout << "-- gate 0 (building blocks) --" << std::endl;
	// (a) g-form Fresnel == Optics' unpolarized Fresnel.
	double worstF = 0;
	const double etas[] = { 0.5, 1.0 / 1.5, 0.9, 0.999, 1.0, 1.001, 1.05, 1.33, 1.5, 2.4, 10.0, 1e4 };
	for( double eta : etas ) {
		for( int i = 0; i <= 2000; ++i ) {
			const double mu = i / 2000.0;
			const double a = PolishedBRDF::Fresnel( mu, 1.0, eta );
			const double b = Optics::CalculateDielectricReflectanceCosine( mu, 1.0, eta );
			worstF = std::max( worstF, std::fabs( a - b ) );
		}
	}
	std::cout << "  Fresnel g-form vs Optics: worst |diff| " << std::scientific << worstF << std::defaultfloat << std::endl;
	Check( worstF <= 1e-10, "gate 0a: PolishedBRDF::Fresnel equals Optics::CalculateDielectricReflectanceCosine" );

	// (b) T_avg closed form (+ eta<1 identity, + near-1 quadrature) vs a
	//     4e6-point midpoint rule of 2 mu (1 - F(mu)).
	double worstT = 0;
	for( double eta : etas ) {
		const int n = 4000000;
		double q = 0;
		for( int i = 0; i < n; ++i ) {
			const double mu = ( i + 0.5 ) / n;
			q += 2.0 * mu * ( 1.0 - Optics::CalculateDielectricReflectanceCosine( mu, 1.0, eta ) );
		}
		q /= n;
		const double t = PolishedBRDF::HemisphericalTransmittance( 1.0, eta );
		worstT = std::max( worstT, std::fabs( t - q ) );
	}
	std::cout << "  T_avg closed form vs quadrature: worst |diff| " << std::scientific << worstT << std::defaultfloat << std::endl;
	Check( worstT <= 1e-7, "gate 0b: hemispherical transmittance matches brute-force quadrature" );

	// (c) Pdf memo: interleaved queries at two shading points give the same
	//     bits as each point queried alone after a cold start.
	UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.4, 0.4, 0.4 ) );  rd->addref();
	UniformScalarPainter* tau = new UniformScalarPainter( 0.9 );  tau->addref();
	UniformScalarPainter* nt  = new UniformScalarPainter( 1.5 );  nt->addref();
	UniformScalarPainter* sc  = new UniformScalarPainter( 20.0 ); sc->addref();
	PolishedMaterial* mat = new PolishedMaterial( *rd, *tau, *nt, *sc, false );  mat->addref();
	const ISPF& spf = *mat->GetSPF();
	const IORStack stack = MakeTestIORStack( g_stub );
	std::vector<RayIntersectionGeometric> ris;
	for( int k = 0; k < 6; ++k ) ris.push_back( MakeRI( Fixture{ 10.0 + 13.0 * k, 0.0, false } ) );
	const Vector3 wo = Vector3Ops::Normalize( Vector3( -0.3, 0.2, 0.9 ) );
	double ref[6];
	for( int k = 0; k < 6; ++k ) ref[k] = spf.Pdf( ris[k], wo, stack );
	bool same = true;
	for( int rep = 0; rep < 50; ++rep ) {
		for( int k = 0; k < 6; ++k ) {
			const int j = ( k * 5 + rep ) % 6;
			if( spf.Pdf( ris[j], wo, stack ) != ref[j] ) same = false;
		}
	}
	Check( same, "gate 0c: Pdf replay memo is order-independent (bit-identical)" );
	safe_release( mat ); safe_release( sc ); safe_release( nt ); safe_release( tau ); safe_release( rd );
}

// ====================================================================
// Per-channel (dispersive) rows: a per-channel coat index (one lobe
// shape, three Fresnel curves) and a per-channel scattering exponent
// (three coat components sampled as a mixture) -- the RGB branches the
// scalar-painter rows above never reach.
// ====================================================================
static void RunPerChannelRows( unsigned int& seed )
{
	std::cout << "-- per-channel coat rows --" << std::endl;
	UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.5, 0.4, 0.3 ) );  rd->addref();
	UniformScalarPainter* tau = new UniformScalarPainter( 0.9 );  tau->addref();
	UniformScalarPainter* ior1 = new UniformScalarPainter( 1.5 );  ior1->addref();
	UniformScalarPainter* scat1 = new UniformScalarPainter( 30.0 );  scat1->addref();
	RGBScalarPainter* iorRGB  = new RGBScalarPainter( 1.3, 1.5, 1.8 );  iorRGB->addref();
	RGBScalarPainter* scatRGB = new RGBScalarPainter( 8.0, 40.0, 200.0 );  scatRGB->addref();
	RGBScalarPainter* scatMix = new RGBScalarPainter( 20.0, 2e6, 60.0 );  scatMix->addref();
	struct Row { const char* name; const IScalarPainter* nt; const IScalarPainter* sc; };
	const Row rows[] = {
		{ "per-channel ior (1.3,1.5,1.8) N30",          iorRGB, scat1 },
		{ "per-channel N (8,40,200) ior1.5",            ior1,   scatRGB },
		{ "per-channel N (20,DELTA,60) ior(1.3,1.5,1.8)", iorRGB, scatMix },
	};
	for( const Row& r : rows ) {
		PolishedMaterial* mat = new PolishedMaterial( *rd, *tau, *r.nt, *r.sc, false );  mat->addref();
		RunRowMaterial( *mat, r.name, Fixture{ 0.0, 0.0, false }, 400000, seed++ );
		RunRowMaterial( *mat, r.name, Fixture{ 70.0, 0.0, false }, 400000, seed++ );
		safe_release( mat );
	}
	safe_release( scatMix ); safe_release( scatRGB ); safe_release( iorRGB );
	safe_release( scat1 ); safe_release( ior1 ); safe_release( tau ); safe_release( rd );
}

// ====================================================================
// Gate 6: sibling audit.
// ====================================================================
static void RunSiblingAudit( unsigned int& seed )
{
	std::cout << "-- gate 6 (sibling audit) --" << std::endl;
	{
		UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );  white->addref();
		const double roughs[] = { 0.3, 0.8 };
		for( double r : roughs ) {
			UniformScalarPainter* rough = new UniformScalarPainter( r );  rough->addref();
			SheenMaterial* sheen = new SheenMaterial( *white, *rough );  sheen->addref();
			char name[64];
			std::snprintf( name, sizeof(name), "sibling sheen rough%.1f", r );
			RunRowMaterial( *sheen, name, Fixture{ 0.0, 0.0, false }, 400000, seed++ );
			RunRowMaterial( *sheen, name, Fixture{ 60.0, 0.0, false }, 400000, seed++ );
			safe_release( sheen );
			safe_release( rough );
		}
		safe_release( white );
	}

	// datadriven_material: a synthetic constant table (value 0.4/pi on
	// both hemispheres of the view/light pair), so its BSDF integrates to
	// exactly 0.4 -- while it has no SPF, i.e. the sampled function is 0.
	// KNOWN-DEFECT PIN (DL-325): PT terminates at it (direct light only),
	// BDPT renders it black, VCM prices indirect light into it.
	{
		char path[256];
		std::snprintf( path, sizeof(path), "/tmp/dl285_const04_%d.bdf", (int)::getpid() );
		{
			std::ofstream f( path, std::ios::binary );
			const int hdr[4] = { 0xBDF, 1, 1, 2 };
			f.write( reinterpret_cast<const char*>( hdr ), sizeof( hdr ) );
			const double v = 0.4 / PI;
			const double rec[21] = { PI / 2,
				0.0, PI / 4, v, v, v,   0.0, PI / 4, 0, 0, 0,
				PI / 4, PI / 2, v, v, v,   PI / 4, PI / 2, 0, 0, 0 };
			f.write( reinterpret_cast<const char*>( rec ), sizeof( rec ) );
		}
		DataDrivenMaterial* dd = new DataDrivenMaterial( path );  dd->addref();
		const RayIntersectionGeometric ri = MakeRI( Fixture{ 30.0, 0.0, false } );
		double B = 0;
		SphereQuadrature( Vector3( 0, 0, 1 ), 600, 256, [&]( const Vector3& w, double dOm ) {
			B += dd->GetBSDF()->value( w, ri )[0] * std::fabs( w.z ) * dOm;
		} );
		std::cout << "  datadriven_material: BSDF albedo " << B << ", SPF " << ( dd->GetSPF() ? "present" : "ABSENT (sampled function == 0)" ) << std::endl;
		Check( std::fabs( B - 0.4 ) < 0.01, "gate 6 (DL-325 pin): datadriven BSDF integrates to its table's 0.4" );
		Check( dd->GetSPF() == 0, "gate 6 (DL-325 pin): datadriven_material has no SPF -- the known defect; closing DL-325 flips this" );
		safe_release( dd );
		std::remove( path );
	}
}

int main( int argc, char** argv )
{
	bool skipHG = false;
	for( int i = 1; i < argc; ++i ) {
		if( std::strcmp( argv[i], "--skip-hg" ) == 0 ) skipHG = true;
	}

	g_stub = new StubObject();
	g_stub->addref();

	std::cout << "PolishedBRDFConsistencyTest (DL-285)" << std::endl;
	RunBuildingBlocks();

	std::vector<Config> cfgs = {
		{ "topoL N20 ior1.5 tau.9 rd.4",      RISEPel( 0.4, 0.4, 0.4 ), 0.9, 1.5,  20.0,  false },
		{ "cloister N350 ior1.5 tau1 rd.5",   RISEPel( 0.5, 0.5, 0.5 ), 1.0, 1.5,  350.0, false },
		{ "wide N1 ior1.33 tau1 rd(.8,.3,.1)",RISEPel( 0.8, 0.3, 0.1 ), 1.0, 1.33, 1.0,   false },
		{ "teapot N256 ior1e4 tau.9 rd.3",    RISEPel( 0.3, 0.3, 0.3 ), 0.9, 1e4,  256.0, false },
		{ "delta N1e7 ior1.5 tau.9 rd.4",     RISEPel( 0.4, 0.4, 0.4 ), 0.9, 1.5,  1e7,   false },
	};
	if( !skipHG ) {
		cfgs.push_back( { "HG g.6 ior1.5 tau.9 rd.4",  RISEPel( 0.4, 0.4, 0.4 ), 0.9, 1.5, 0.6, true } );
		cfgs.push_back( { "HG g-.3 ior1.5 tau.9 rd.4", RISEPel( 0.4, 0.4, 0.4 ), 0.9, 1.5, -0.3, true } );
	}

	const double thetas[] = { 0.0, 30.0, 60.0, 80.0 };
	unsigned int seed = 20260928;
	std::cout << "-- gates 1, 2, 4, 5 --" << std::endl;
	for( const Config& c : cfgs ) {
		for( double t : thetas ) {
			RunRow( c, Fixture{ t, 0.0, false }, 400000, seed++ );
		}
	}
	// Tilted shading normal and back-face rows on the topology-L material.
	RunRow( cfgs[0], Fixture{ 45.0, 15.0, false }, 400000, seed++ );
	RunRow( cfgs[0], Fixture{ 30.0, 0.0,  true  }, 400000, seed++ );
	RunRow( cfgs[1], Fixture{ 60.0, 0.0,  true  }, 400000, seed++ );

	RunPerChannelRows( seed );
	RunSiblingAudit( seed );

	std::cout << "-- gate 3 (reciprocity) --" << std::endl;
	for( const Config& c : cfgs ) {
		RunReciprocity( c, Fixture{ 0.0, 0.0, false } );
	}
	RunReciprocity( cfgs[0], Fixture{ 0.0, 15.0, false } );
	RunReciprocity( cfgs[0], Fixture{ 0.0, 0.0, true } );

	safe_release( g_stub );

	std::cout << std::endl << g_pass << " passed, " << g_fail << " failed" << std::endl;
	return g_fail == 0 ? 0 : 1;
}
