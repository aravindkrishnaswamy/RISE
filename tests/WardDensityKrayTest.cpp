//////////////////////////////////////////////////////////////////////
//
//  WardDensityKrayTest.cpp - Red-proof and regression gate for DL-177:
//    `WardIsotropicGaussianSPF` and
//    `WardAnisotropicEllipticalGaussianSPF` must report the density
//    they actually sample from, must report the density `Scatter()` +
//    `RandomlySelect()` actually produce, and must carry each lobe's
//    own `f_I * cos / p_I` as that lobe's `kray`.
//
//  THREE INDEPENDENT DEFECTS, measured separately here.
//
//  (1) THE STORED PER-LOBE DENSITY WAS THE TRUE ONE TIMES cos^4(th_h).
//      `GenerateSpecularRay` draws `tan(theta_h) = alpha*sqrt(-ln xi)`
//      with `phi` uniform (isotropic) or Ward-warped (anisotropic).
//      Deriving the solid-angle density of that half-vector from the
//      sampler's own inverse CDF -- which section A does from scratch,
//      without reading the production expression -- gives
//
//          p_h(th_h) = exp(-tan^2 th_h / a^2) / (PI a^2 cos^3 th_h)
//
//      (anisotropic: `a^2 -> ax*ay` in the prefactor and
//      `tan^2/a^2 -> tan^2 * (cos^2 phi/ax^2 + sin^2 phi/ay^2)`),
//      but both files stored `cos(th_h) * exp(...) / (PI a^2)` -- the
//      true density times `cos^4(th_h)`.  Section C measures the ratio
//      per draw.
//
//  (2) `Pdf()`/`PdfNM()` WERE A RAW-ALBEDO-WEIGHTED MIXTURE.  Once (3)
//      makes `kray_S` direction-dependent, `RandomlySelect`'s realized
//      weight `MaxValue(kray)` is direction-dependent too, so the
//      density of the SELECTED direction is
//      `C_D p_D(wo) + q_S(wo) p_S(wo)` with `C_D` an expectation over
//      the specular draw (dominated by its REJECTION rate, which no
//      reflectance average can see) -- the DL-67/DL-98/DL-99
//      construction.  Sections D and E are those rows' own two-sided
//      gate: full-SPHERE normalisation against the measured emission
//      probability, and total variation against a histogram of real
//      `Scatter` + `RandomlySelect` draws.
//
//      Ward's diffuse `kray` is the constant `Rd`, so -- unlike
//      Ashikmin-Shirley (DL-99) -- the specular lobe's own selection
//      coefficient does NOT need a second quadrature over the diffuse
//      draw: `q_S` depends on the query direction alone, modulated only
//      by the scalar probability that the diffuse ray survived its own
//      geometric-horizon gate.
//
//  (3) `kray` WAS THE REFLECTANCE PAINTER'S OWN COLOUR.  `Ward*BRDF`'s
//      specular term is `Rs exp(...)/(4 PI a^2 sqrt(nl nv))`, so
//
//          f_S cos_o / p_S = Rs * (h.wo) * cos^3(th_h) * sqrt(nl/nv)
//
//      -- the whole roughness dependence cancels, exactly as it does in
//      DL-127's Schlick derivation.  Section F measures
//      `kray * pdf / (f cos)` per draw (RGB, NM, and the per-channel
//      alpha branch).  Section G characterises that weight's grazing
//      tail, which is the question DL-177 was held open on.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/WardIsotropicGaussianSPF.h"
#include "../src/Library/Materials/WardIsotropicGaussianBRDF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianSPF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Materials/SchlickSPF.h"
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

//! Deterministic seeds -- nothing in this file may depend on an
//! un-`srand`ed libc `rand()` (the DL-176 lesson, same day).
static const unsigned int kSeedA = 990001u;

//////////////////////////////////////////////////////////////////////
// Synthetic flat-surface intersections.  `MakeIntersection` puts the
// incoming ray above the surface (front face); `MakeBackFaceIntersection`
// puts it below, so `Scatter`'s `FlipW` fires and the DL-100 frame trap
// is exercised.
//////////////////////////////////////////////////////////////////////
static RayIntersectionGeometric MakeIntersection( double incomingTheta )
{
	const double sinT = sin( incomingTheta );
	const double cosT = cos( incomingTheta );
	const Vector3 inDir( sinT, 0, -cosT );

	Ray inRay( Point3( sinT, 0, 1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0 / cosT;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );

	return ri;
}

static RayIntersectionGeometric MakeBackFaceIntersection( double incomingTheta )
{
	const double sinT = sin( incomingTheta );
	const double cosT = cos( incomingTheta );
	// Travelling UPWARD: Dot(dir, onb.w()) = +cosT > 0, so Scatter's
	// FlipW fires and every lobe is sampled about -Z.
	const Vector3 inDir( sinT, 0, cosT );

	Ray inRay( Point3( sinT, 0, -1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0 / cosT;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );

	return ri;
}

//! A hit whose SHADING normal is tilted away from the GEOMETRIC one, so
//! `Scatter`'s diffuse ray can fall below the geometric horizon and be
//! dropped.  That is the only configuration in which
//! `WardDiffuseAcceptFraction` returns less than 1 and the "lone
//! specular ray wins outright" arm of `Ward*SpecularDensity` is
//! reachable at all -- every other fixture in this file has
//! `vNormal == vGeomNormal == (0,0,1)`, so that branch went untested
//! (review round 1, P2-3).  The tilt is about the y axis, in the same
//! plane as the incoming ray, and the ray still arrives from above the
//! GEOMETRIC surface.
static RayIntersectionGeometric MakeTiltedIntersection( double incomingTheta, double tiltRad )
{
	const double sinT = sin( incomingTheta );
	const double cosT = cos( incomingTheta );
	const Vector3 inDir( sinT, 0, -cosT );

	Ray inRay( Point3( sinT, 0, 1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0 / cosT;
	ri.ptIntersection = Point3( 0, 0, 0 );
	// Geometric normal stays +Z; only the SHADING normal tilts.
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.vNormal = Vector3( sin( tiltRad ), 0, cos( tiltRad ) );
	ri.onb.CreateFromW( ri.vNormal );
	ri.ptCoord = Point2( 0.5, 0.5 );

	return ri;
}

//! The frame `Scatter`/`Pdf` sample about, recomputed here from the
//! same public inputs rather than trusted from the SPF.
static OrthonormalBasis3D SamplingFrame( const RayIntersectionGeometric& ri )
{
	OrthonormalBasis3D onb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		onb.FlipW();
	}
	return onb;
}

//////////////////////////////////////////////////////////////////////
//  SECTION A's reference densities -- derived here from the SAMPLER's
//  inverse CDF, deliberately NOT copied from the production
//  expression.
//
//  ISOTROPIC.  `theta = atan(a*sqrt(-ln xi2))` with `xi2 ~ U(0,1)`.
//  Writing `t = tan(theta)`, `xi2 = exp(-t^2/a^2)`, so
//  `P(T<=t) = 1 - exp(-t^2/a^2)` and `p_T(t) = (2t/a^2) exp(-t^2/a^2)`.
//  Changing variable with `dt/dtheta = 1/cos^2 theta`,
//  `p_Theta = (2 tan/a^2) exp(...) / cos^2`, and a solid-angle density
//  is `p_Theta * p_Phi / sin(theta)` with `p_Phi = 1/(2 PI)`:
//
//      p_h = exp(-tan^2/a^2) / (PI a^2 cos^3 theta)
//
//  ANISOTROPIC.  `theta = atan(sqrt(-ln xi2 / D))` with
//  `D(phi) = cos^2 phi/ax^2 + sin^2 phi/ay^2`, so
//  `p_Theta(theta|phi) = 2 tan(theta) D exp(-tan^2 D)/cos^2`, and the
//  azimuthal warp `tan phi = (ay/ax) tan(PI val/2)`, `xi1 = val/4` on
//  the first quadrant, differentiates to
//  `p_Phi(phi) = 1/(2 PI ax ay D(phi))`.  The `D` cancels:
//
//      p_h = exp(-tan^2 D) / (PI ax ay cos^3 theta)
//////////////////////////////////////////////////////////////////////
static double RefHalfDensityIso( double cosThetaH, double alpha )
{
	if( cosThetaH <= 0 ) return 0;
	const double c2 = cosThetaH * cosThetaH;
	const double tan2 = ( 1.0 - c2 ) / c2;
	const double a2 = alpha * alpha;
	return exp( -tan2 / a2 ) / ( PI * a2 * c2 * cosThetaH );
}

static double RefHalfDensityAniso( double cosThetaH, double phi, double ax, double ay )
{
	if( cosThetaH <= 0 ) return 0;
	const double c2 = cosThetaH * cosThetaH;
	const double tan2 = ( 1.0 - c2 ) / c2;
	const double cp = cos( phi ), sp = sin( phi );
	const double D = ( cp * cp ) / ( ax * ax ) + ( sp * sp ) / ( ay * ay );
	return exp( -tan2 * D ) / ( PI * ax * ay * c2 * cosThetaH );
}

//! `f_S cos_o / p_S` divided by `Rs` -- the factor a correct `kray`
//! must carry.  Derived independently from `Ward*BRDF::ComputeFactors`
//! and the reference densities above; both alpha-dependences cancel.
static double RefKrayRatio( double hdotwo, double cosThetaH, double cosO, double cosI )
{
	if( cosThetaH <= 0 || cosO <= 0 || cosI <= 0 ) return 0;
	return hdotwo * cosThetaH * cosThetaH * cosThetaH * sqrt( cosO / cosI );
}

//////////////////////////////////////////////////////////////////////
//  Small statistics helpers
//////////////////////////////////////////////////////////////////////
struct RatioStat {
	double mean;
	double minR;
	double maxR;
	unsigned int n;
};

static void AccumRatio( RatioStat& st, double r )
{
	if( st.n == 0 ) { st.minR = st.maxR = r; }
	else { if( r < st.minR ) st.minR = r; if( r > st.maxR ) st.maxR = r; }
	st.mean = ( st.mean * double(st.n) + r ) / double( st.n + 1 );
	st.n++;
}

//////////////////////////////////////////////////////////////////////
//  Full-SPHERE quadrature of `Pdf` (DL-98/DL-99 gate 1).  The sphere,
//  not the hemisphere: a sampler whose accept-check can be tripped by a
//  tilted geometric normal legitimately prices directions below the
//  shading horizon, and a hemisphere-only quadrature would measure its
//  own domain error instead of the SPF's mass.
//////////////////////////////////////////////////////////////////////
static double IntegratePdfFullSphere( ISPF& spf, const RayIntersectionGeometric& ri,
                                      const IORStack& iorStack, int nTheta, int nPhi )
{
	double total = 0;
	for( int t = 0; t < nTheta; t++ ) {
		const double theta = ( t + 0.5 ) * PI / nTheta;
		const double dTheta = PI / nTheta;
		const double sinT = sin( theta ), cosT = cos( theta );
		for( int p = 0; p < nPhi; p++ ) {
			const double phi = ( p + 0.5 ) * TWO_PI / nPhi;
			const double dPhi = TWO_PI / nPhi;
			Vector3 wo( sinT * cos( phi ), sinT * sin( phi ), cosT );
			wo = Vector3Ops::Normalize( wo );
			total += spf.Pdf( ri, wo, iorStack ) * sinT * dTheta * dPhi;
		}
	}
	return total;
}

//! The probability `Scatter` + `RandomlySelect` produce a non-delta ray
//! at all -- what `int Pdf` over the whole sphere must equal.
static double MeasureEmissionProbability( ISPF& spf, const RayIntersectionGeometric& ri,
                                          const IORStack& iorStack, unsigned int seed,
                                          int nDraws )
{
	RandomNumberGenerator rng( seed );
	IndependentSampler sampler( rng );
	int emitted = 0;
	for( int i = 0; i < nDraws; i++ ) {
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );
		ScatteredRay* sel = scattered.RandomlySelect( rng.CanonicalRandom(), false );
		if( sel && !sel->isDelta ) emitted++;
	}
	return double( emitted ) / double( nDraws );
}

//////////////////////////////////////////////////////////////////////
//  Total-variation distance between a histogram of real
//  `Scatter` + `RandomlySelect` draws and `Pdf`, over a full-sphere
//  equal-solid-angle grid.  The normalisation gate above is BLIND to a
//  wrong SPLIT between two lobes that each integrate correctly (the
//  DL-98/DL-99 review's point); this is the gate that sees it.
//////////////////////////////////////////////////////////////////////
static const int kTvdTheta = 24;
static const int kTvdPhi   = 24;

static bool TvdBin( const Vector3& dir, int& tb, int& pb )
{
	// Equal-solid-angle in cos(theta) over the FULL sphere.
	const double c = r_max( -1.0, r_min( 1.0, dir.z ) );
	tb = int( ( 1.0 - c ) * 0.5 * kTvdTheta );
	if( tb >= kTvdTheta ) tb = kTvdTheta - 1;
	if( tb < 0 ) tb = 0;
	double phi = atan2( dir.y, dir.x );
	if( phi < 0 ) phi += TWO_PI;
	pb = int( phi / TWO_PI * kTvdPhi );
	if( pb >= kTvdPhi ) pb = kTvdPhi - 1;
	if( pb < 0 ) pb = 0;
	return true;
}

//! Returns the TVD and stores the two mass vectors' totals.
static double MeasureTVD( ISPF& spf, const RayIntersectionGeometric& ri,
                          const IORStack& iorStack, unsigned int seed, int nDraws,
                          double& outSampledMass, double& outPdfMass )
{
	const int nBins = kTvdTheta * kTvdPhi;
	std::vector<double> obs( nBins, 0.0 );
	std::vector<double> exp_( nBins, 0.0 );

	RandomNumberGenerator rng( seed );
	IndependentSampler sampler( rng );
	int emitted = 0;
	for( int i = 0; i < nDraws; i++ ) {
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );
		ScatteredRay* sel = scattered.RandomlySelect( rng.CanonicalRandom(), false );
		if( !sel || sel->isDelta ) continue;
		const Vector3 wo = Vector3Ops::Normalize( sel->ray.Dir() );
		int tb, pb;
		TvdBin( wo, tb, pb );
		obs[ tb * kTvdPhi + pb ] += 1.0;
		emitted++;
	}

	// Per-bin integral of Pdf, 4x4 sub-integration, matching the bins.
	const int SUB = 4;
	for( int tb = 0; tb < kTvdTheta; tb++ ) {
		const double c0 = 1.0 - 2.0 * double(tb) / kTvdTheta;
		const double c1 = 1.0 - 2.0 * double(tb+1) / kTvdTheta;
		for( int pb = 0; pb < kTvdPhi; pb++ ) {
			const double p0 = double(pb) * TWO_PI / kTvdPhi;
			const double p1 = double(pb+1) * TWO_PI / kTvdPhi;
			double acc = 0;
			for( int a = 0; a < SUB; a++ ) {
				const double c = c0 + ( a + 0.5 ) * ( c1 - c0 ) / SUB;
				const double dC = fabs( c1 - c0 ) / SUB;
				const double s = sqrt( r_max( 0.0, 1.0 - c*c ) );
				for( int b = 0; b < SUB; b++ ) {
					const double phi = p0 + ( b + 0.5 ) * ( p1 - p0 ) / SUB;
					const double dP = ( p1 - p0 ) / SUB;
					Vector3 wo( s * cos( phi ), s * sin( phi ), c );
					wo = Vector3Ops::Normalize( wo );
					acc += spf.Pdf( ri, wo, iorStack ) * dC * dP;
				}
			}
			exp_[ tb * kTvdPhi + pb ] = acc;
		}
	}

	double obsTotal = 0, expTotal = 0;
	for( int i = 0; i < nBins; i++ ) { obsTotal += obs[i]; expTotal += exp_[i]; }
	outSampledMass = double( emitted ) / double( nDraws );
	outPdfMass = expTotal;

	if( obsTotal <= 0 || expTotal <= 0 ) return 1.0;

	double tvd = 0;
	for( int i = 0; i < nBins; i++ ) {
		tvd += fabs( obs[i] / obsTotal - exp_[i] / expTotal );
	}
	return 0.5 * tvd;
}

//////////////////////////////////////////////////////////////////////
//  MAIN
//////////////////////////////////////////////////////////////////////
int main()
{
	GlobalLog();

	std::cout << "=== WardDensityKrayTest (DL-177) ===" << std::endl;
	std::cout << std::fixed;

	g_stubObject = new StubObject();
	g_stubObject->addref();

	IORStack iorStack = MakeTestIORStack( g_stubObject );

	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) ); black->addref();
	UniformColorPainter* spec  = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) ); spec->addref();

	const double alphas[] = { 0.1, 0.3, 0.6 };
	const double degs[]   = { 0.0, 30.0, 60.0, 80.0 };

	//----------------------------------------------------------------
	// SECTION A -- the reference densities are normalised.
	//
	// Self-check on the derivation itself, BEFORE it is used to judge
	// production code: a 2000x2000 spherical quadrature of
	// `RefHalfDensity*` must integrate to 1.  An algebra slip in the
	// inverse-CDF change of variables shows up here, not as a false
	// accusation against the SPF.
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section A: the test's own reference half-vector densities "
	             "integrate to 1 over the hemisphere" << std::endl;
	{
		const int NT = 2000, NP = 800;
		for( int ai = 0; ai < 3; ai++ ) {
			double total = 0;
			for( int t = 0; t < NT; t++ ) {
				const double theta = ( t + 0.5 ) * PI_OV_TWO / NT;
				const double dT = PI_OV_TWO / NT;
				total += RefHalfDensityIso( cos( theta ), alphas[ai] ) * sin( theta ) * dT * TWO_PI;
			}
			std::cout << "   iso  alpha=" << std::setprecision(2) << alphas[ai]
			          << "   integral " << std::setprecision(8) << total << std::endl;
			Check( fabs( total - 1.0 ) < 2e-3,
			       "Section A: isotropic reference density normalised" );
		}

		const double axs[] = { 0.3, 0.2 };
		const double ays[] = { 0.12, 0.5 };
		for( int k = 0; k < 2; k++ ) {
			double total = 0;
			for( int t = 0; t < NT; t++ ) {
				const double theta = ( t + 0.5 ) * PI_OV_TWO / NT;
				const double dT = PI_OV_TWO / NT;
				for( int p = 0; p < NP; p++ ) {
					const double phi = ( p + 0.5 ) * TWO_PI / NP;
					const double dP = TWO_PI / NP;
					total += RefHalfDensityAniso( cos( theta ), phi, axs[k], ays[k] )
					       * sin( theta ) * dT * dP;
				}
			}
			std::cout << "   aniso ax=" << std::setprecision(2) << axs[k]
			          << " ay=" << ays[k]
			          << "   integral " << std::setprecision(8) << total << std::endl;
			Check( fabs( total - 1.0 ) < 2e-3,
			       "Section A: anisotropic reference density normalised" );
		}
	}

	//----------------------------------------------------------------
	// SECTION B -- the reference density really is the SAMPLER's.
	//
	// Histogram the half-vectors of real `Scatter()` draws in the
	// sampling frame and compare against the reference density.  This
	// is what makes section C an accusation about the STORED value
	// rather than about the derivation.
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section B: histogram of real sampled half-vectors vs the "
	             "reference density (TVD over 24x24 full-sphere bins)" << std::endl;
	{
		for( int ai = 0; ai < 3; ai++ ) {
			UniformScalarPainter* al = new UniformScalarPainter( alphas[ai] ); al->addref();
			WardIsotropicGaussianSPF* spf = new WardIsotropicGaussianSPF( *black, *spec, *al ); spf->addref();

			const RayIntersectionGeometric ri = MakeIntersection( 30.0 * PI / 180.0 );
			const OrthonormalBasis3D onb = SamplingFrame( ri );
			const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );

			const int nBins = kTvdTheta * kTvdPhi;
			std::vector<double> obs( nBins, 0.0 );
			RandomNumberGenerator rng( kSeedA + ai );
			IndependentSampler sampler( rng );
			const int N = 400000;
			int n = 0;
			for( int i = 0; i < N; i++ ) {
				ScatteredRayContainer sc;
				spf->Scatter( ri, sampler, sc, iorStack );
				for( unsigned int j = 0; j < sc.Count(); j++ ) {
					if( sc[j].type != ScatteredRay::eRayReflection ) continue;
					const Vector3 wo = Vector3Ops::Normalize( sc[j].ray.Dir() );
					const Vector3 h = Vector3Ops::Normalize( wi + wo );
					// Express h in the sampling frame.
					const Vector3 hl( Vector3Ops::Dot( h, onb.u() ),
					                  Vector3Ops::Dot( h, onb.v() ),
					                  Vector3Ops::Dot( h, onb.w() ) );
					int tb, pb;
					TvdBin( hl, tb, pb );
					obs[ tb * kTvdPhi + pb ] += 1.0;
					n++;
				}
			}

			// Reference mass per bin (equal-solid-angle in cos), with
			// the SAMPLER'S OWN ACCEPT TEST applied.  `Scatter` drops a
			// half-vector whose `h.wi <= 0`, and drops the reflected
			// direction if it falls below the shading or geometric
			// horizon -- at alpha 0.6 that is 14% of draws, so a
			// reference that integrated the raw density would be
			// comparing two different populations and would read a TVD
			// of 0.13 against correct code.  The test is a
			// deterministic function of `h` and `wi`, so it belongs in
			// the reference rather than in a tolerance.
			const Vector3 wiL( Vector3Ops::Dot( wi, onb.u() ),
			                   Vector3Ops::Dot( wi, onb.v() ),
			                   Vector3Ops::Dot( wi, onb.w() ) );
			std::vector<double> ref( nBins, 0.0 );
			const int SUB = 8;
			double refTotal = 0;
			for( int tb = 0; tb < kTvdTheta; tb++ ) {
				const double c0 = 1.0 - 2.0 * double(tb) / kTvdTheta;
				const double c1 = 1.0 - 2.0 * double(tb+1) / kTvdTheta;
				for( int pb = 0; pb < kTvdPhi; pb++ ) {
					const double p0 = double(pb) * TWO_PI / kTvdPhi;
					const double p1 = double(pb+1) * TWO_PI / kTvdPhi;
					double acc = 0;
					for( int a = 0; a < SUB; a++ ) {
						const double c = c0 + ( a + 0.5 ) * ( c1 - c0 ) / SUB;
						const double dC = fabs( c1 - c0 ) / SUB;
						const double s = sqrt( r_max( 0.0, 1.0 - c*c ) );
						for( int b = 0; b < SUB; b++ ) {
							const double phi = p0 + ( b + 0.5 ) * ( p1 - p0 ) / SUB;
							const double dP = ( p1 - p0 ) / SUB;
							const Vector3 hL( s * cos( phi ), s * sin( phi ), c );
							const double hdotwi = Vector3Ops::Dot( hL, wiL );
							if( hdotwi <= 0 ) continue;
							// reflect wi about h:  r = 2 (h.wi) h - wi
							const Vector3 rL = 2.0 * hdotwi * hL - wiL;
							if( rL.z <= 0 ) continue;   // shading AND geometric horizon (both +Z here)
							acc += RefHalfDensityIso( c, alphas[ai] ) * dC * dP;
						}
					}
					ref[ tb * kTvdPhi + pb ] = acc;
					refTotal += acc;
				}
			}

			double tvd = 0;
			if( n > 0 && refTotal > 0 ) {
				for( int i = 0; i < nBins; i++ ) {
					tvd += fabs( obs[i] / double(n) - ref[i] / refTotal );
				}
				tvd *= 0.5;
			} else {
				tvd = 1.0;
			}

			std::cout << "   iso  alpha=" << std::setprecision(2) << alphas[ai]
			          << "   TVD(sampled h, reference density) = "
			          << std::setprecision(6) << tvd
			          << "   (n=" << n << ", reference mass above accepted cone "
			          << std::setprecision(4) << refTotal << ")" << std::endl;
			// Rejected draws (hdotk<=0, or a reflected direction below
			// the horizon) are missing from `obs` but present in `ref`,
			// so the two agree only where the sampler accepts.  At 30
			// degrees and these roughnesses that is nearly everything.
			Check( tvd < 0.03, "Section B: sampled half-vectors follow the reference density" );

			spf->release(); al->release();
		}
	}

	//----------------------------------------------------------------
	// SECTION C -- DEFECT (1): the STORED per-lobe density.
	//
	// Per draw: `sr.pdf` against `RefHalfDensity / (4 (h.wo))`.  The
	// pre-fix ratio is exactly `cos^4(theta_h)`, which this section
	// also checks directly so the reading is diagnostic and not just
	// "wrong".
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section C: stored specular pdf / true density "
	             "(must be 1; pre-fix it is cos^4(theta_h))" << std::endl;
	std::cout << "   model  alpha   theta       mean         min         max     "
	             "worst |ratio - cos^4|" << std::endl;
	{
		UniformScalarPainter* ay = new UniformScalarPainter( 0.12 ); ay->addref();

		for( int model = 0; model < 2; model++ ) {
			for( int ai = 0; ai < 3; ai++ ) {
				UniformScalarPainter* al = new UniformScalarPainter( alphas[ai] ); al->addref();
				WardIsotropicGaussianSPF* iso =
					new WardIsotropicGaussianSPF( *black, *spec, *al ); iso->addref();
				WardAnisotropicEllipticalGaussianSPF* aniso =
					new WardAnisotropicEllipticalGaussianSPF( *black, *spec, *al, *ay ); aniso->addref();
				ISPF* spf = ( model == 0 ) ? (ISPF*)iso : (ISPF*)aniso;

				for( int d = 0; d < 4; d++ ) {
					const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
					const OrthonormalBasis3D onb = SamplingFrame( ri );
					const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );

					RatioStat st = { 0, 0, 0, 0 };
					double worstCos4 = 0;
					RandomNumberGenerator rng( kSeedA + 100u * unsigned(model) + 10u * unsigned(ai) + unsigned(d) );
					IndependentSampler sampler( rng );

					for( int i = 0; i < 40000; i++ ) {
						ScatteredRayContainer sc;
						spf->Scatter( ri, sampler, sc, iorStack );
						for( unsigned int j = 0; j < sc.Count(); j++ ) {
							if( sc[j].type != ScatteredRay::eRayReflection ) continue;
							if( sc[j].pdf <= 0 ) continue;
							const Vector3 wo = Vector3Ops::Normalize( sc[j].ray.Dir() );
							const Vector3 h = Vector3Ops::Normalize( wi + wo );
							const double cH = Vector3Ops::Dot( h, onb.w() );
							const double hw = Vector3Ops::Dot( h, wo );
							if( cH <= 0 || hw <= 0 ) continue;

							double pTrue;
							if( model == 0 ) {
								pTrue = RefHalfDensityIso( cH, alphas[ai] );
							} else {
								const double hu = Vector3Ops::Dot( h, onb.u() );
								const double hv = Vector3Ops::Dot( h, onb.v() );
								double phi = atan2( hv, hu );
								if( phi < 0 ) phi += TWO_PI;
								pTrue = RefHalfDensityAniso( cH, phi, alphas[ai], 0.12 );
							}
							pTrue /= ( 4.0 * hw );
							if( pTrue <= 0 ) continue;

							const double ratio = sc[j].pdf / pTrue;
							AccumRatio( st, ratio );
							const double c4 = cH * cH * cH * cH;
							const double devFromCos4 = fabs( ratio - c4 );
							if( devFromCos4 > worstCos4 ) worstCos4 = devFromCos4;
						}
					}

					std::cout << "   " << ( model == 0 ? "iso  " : "aniso" )
					          << "   " << std::setprecision(2) << std::setw(4) << alphas[ai]
					          << "   " << std::setw(5) << std::setprecision(1) << degs[d]
					          << "   " << std::setprecision(6) << std::setw(10) << st.mean
					          << "  " << std::setw(10) << st.minR
					          << "  " << std::setw(10) << st.maxR
					          << "        " << std::setw(10) << worstCos4
					          << std::endl;

					Check( st.n > 1000, "Section C: draws produced" );
					Check( fabs( st.minR - 1.0 ) < 1e-9 && fabs( st.maxR - 1.0 ) < 1e-9,
					       "Section C: stored specular pdf equals the true sampling density" );
				}

				iso->release(); aniso->release(); al->release();
			}
		}
		ay->release();
	}

	//----------------------------------------------------------------
	// SECTION D -- DEFECT (2), gate 1: full-SPHERE mass.
	//
	// `int Pdf` over the whole sphere must equal the measured
	// probability that `Scatter` + `RandomlySelect` produce anything at
	// all.  Chromatic reflectances throughout (the `MaxValue` reduction
	// trap), plus a BACK-FACE row (the DL-100 frame trap) and a
	// per-channel-alpha row (the DL-101 branch).
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section D: full-sphere int Pdf vs measured emission probability" << std::endl;
	std::cout << "   config                                   int Pdf    emission    |diff|" << std::endl;
	{
		// Chromatic, and deliberately NOT co-maximising: MaxValue(Rd)
		// comes from the red channel and MaxValue(Rs) from the green.
		UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.9, 0.1, 0.1 ) ); rd->addref();
		UniformColorPainter* rs = new UniformColorPainter( RISEPel( 0.1, 0.9, 0.1 ) ); rs->addref();
		UniformScalarPainter* ayp = new UniformScalarPainter( 0.12 ); ayp->addref();
		RGBScalarPainter* alphaRGB = new RGBScalarPainter( 0.1, 0.3, 0.6 ); alphaRGB->addref();

		// `tilt` degrees between the SHADING and GEOMETRIC normals.  A
		// nonzero tilt is what makes `WardDiffuseAcceptFraction` read
		// less than 1, which is the ONLY way into the "lone specular
		// ray wins outright" arm of `Ward*SpecularDensity` and into
		// `C_D`'s own `nAcceptedSpec == 0` short-circuit; review round
		// 1 (P2-3) found every row here was at tilt 0.
		struct Row { const char* name; int model; double alpha; double deg; bool backFace; bool perChannel; bool blackDiffuse; double tilt; };
		const Row rows[] = {
			{ "iso   a.1  th30",                     0, 0.1, 30.0, false, false, false,  0.0 },
			{ "iso   a.3  th30",                     0, 0.3, 30.0, false, false, false,  0.0 },
			{ "iso   a.6  th30",                     0, 0.6, 30.0, false, false, false,  0.0 },
			{ "iso   a.3  th60",                     0, 0.3, 60.0, false, false, false,  0.0 },
			{ "iso   a.3  th80",                     0, 0.3, 80.0, false, false, false,  0.0 },
			{ "iso   a.1  th30 BLACK diffuse",       0, 0.1, 30.0, false, false, true,   0.0 },
			{ "iso   a.3  th30 BLACK diffuse",       0, 0.3, 30.0, false, false, true,   0.0 },
			{ "iso   a.6  th30 BLACK diffuse",       0, 0.6, 30.0, false, false, true,   0.0 },
			{ "iso   a.3  th45 BACK FACE",           0, 0.3, 45.0, true,  false, false,  0.0 },
			{ "iso   perchannel(.1,.3,.6) th30",     0, 0.3, 30.0, false, true,  false,  0.0 },
			{ "iso   perchannel(.1,.3,.6) th60",     0, 0.3, 60.0, false, true,  false,  0.0 },
			{ "aniso a.3/.12 th30",                  1, 0.3, 30.0, false, false, false,  0.0 },
			{ "aniso a.3/.12 th60",                  1, 0.3, 60.0, false, false, false,  0.0 },
			{ "aniso a.6/.12 th30",                  1, 0.6, 30.0, false, false, false,  0.0 },
			{ "aniso a.3/.12 th45 BACK FACE",        1, 0.3, 45.0, true,  false, false,  0.0 },
			{ "aniso perchannel(.1,.3,.6)/.12 th30", 1, 0.3, 30.0, false, true,  false,  0.0 },
			// TILTED rows (P2-3).  `aD = (1 + cos tilt)/2` reads
			// 1.000 / 0.983 / 0.933 / 0.854 / 0.750 at 0/15/30/45/60
			// ((1+cos phi)/2; the 45-degree value is 0.854, not 0.866).
			{ "iso   a.3  th30 TILT 15",             0, 0.3, 30.0, false, false, false, 15.0 },
			{ "iso   a.3  th30 TILT 30",             0, 0.3, 30.0, false, false, false, 30.0 },
			{ "iso   a.3  th30 TILT 45",             0, 0.3, 30.0, false, false, false, 45.0 },
			{ "iso   a.3  th30 TILT 60",             0, 0.3, 30.0, false, false, false, 60.0 },
			{ "iso   a.6  th45 TILT 45",             0, 0.6, 45.0, false, false, false, 45.0 },
			{ "iso   perchannel(.1,.3,.6) th30 TILT 45", 0, 0.3, 30.0, false, true, false, 45.0 },
			{ "aniso a.3/.12 th30 TILT 15",          1, 0.3, 30.0, false, false, false, 15.0 },
			{ "aniso a.3/.12 th30 TILT 30",          1, 0.3, 30.0, false, false, false, 30.0 },
			{ "aniso a.3/.12 th30 TILT 45",          1, 0.3, 30.0, false, false, false, 45.0 },
			{ "aniso a.3/.12 th30 TILT 60",          1, 0.3, 30.0, false, false, false, 60.0 },
			{ "aniso a.6/.12 th45 TILT 45",          1, 0.6, 45.0, false, false, false, 45.0 },
			{ "aniso perchannel(.1,.3,.6)/.12 th30 TILT 45", 1, 0.3, 30.0, false, true, false, 45.0 },
		};
		const int nRows = int( sizeof(rows)/sizeof(rows[0]) );

		for( int r = 0; r < nRows; r++ ) {
			UniformScalarPainter* al = new UniformScalarPainter( rows[r].alpha ); al->addref();
			IScalarPainter* ax = rows[r].perChannel ? (IScalarPainter*)alphaRGB : (IScalarPainter*)al;
			const IPainter& diffusePainter = rows[r].blackDiffuse ? *(const IPainter*)black : *(const IPainter*)rd;

			WardIsotropicGaussianSPF* iso =
				new WardIsotropicGaussianSPF( diffusePainter, *rs, *ax ); iso->addref();
			WardAnisotropicEllipticalGaussianSPF* aniso =
				new WardAnisotropicEllipticalGaussianSPF( diffusePainter, *rs, *ax, *ayp ); aniso->addref();
			ISPF* spf = ( rows[r].model == 0 ) ? (ISPF*)iso : (ISPF*)aniso;

			const RayIntersectionGeometric ri = rows[r].backFace
				? MakeBackFaceIntersection( rows[r].deg * PI / 180.0 )
				: ( rows[r].tilt > 0
					? MakeTiltedIntersection( rows[r].deg * PI / 180.0, rows[r].tilt * PI / 180.0 )
					: MakeIntersection( rows[r].deg * PI / 180.0 ) );

			const double integral = IntegratePdfFullSphere( *spf, ri, iorStack, 400, 800 );
			const double emission = MeasureEmissionProbability( *spf, ri, iorStack, kSeedA + 7000u + unsigned(r), 200000 );

			std::cout << "   " << std::left << std::setw(40) << rows[r].name << std::right
			          << std::setprecision(6) << std::setw(10) << integral
			          << std::setw(12) << emission
			          << std::setw(11) << fabs( integral - emission ) << std::endl;

			// 1% band: the quadrature and the 200k-draw estimate each
			// carry their own error, and (per DL-98/DL-99's review) a
			// deterministic 16x16 selection quadrature inside `Pdf`
			// contributes a systematic residual that does not shrink
			// with the draw count.
			Check( fabs( integral - emission ) < 0.01,
			       std::string( "Section D: int Pdf == emission probability: " ) + rows[r].name );

			iso->release(); aniso->release(); al->release();
		}

		rd->release(); rs->release(); ayp->release(); alphaRGB->release();
	}

	//----------------------------------------------------------------
	// SECTION E -- DEFECT (2), gate 2: SHAPE.
	//
	// The normalisation gate above cannot see mass moved BETWEEN the
	// two lobes.  Total variation against a real
	// `Scatter` + `RandomlySelect` histogram can.
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section E: TVD( Scatter+RandomlySelect histogram, Pdf )" << std::endl;
	std::cout << "   config                                      TVD   sampled mass   Pdf mass" << std::endl;
	{
		UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.9, 0.1, 0.1 ) ); rd->addref();
		UniformColorPainter* rs = new UniformColorPainter( RISEPel( 0.1, 0.9, 0.1 ) ); rs->addref();
		UniformScalarPainter* ayp = new UniformScalarPainter( 0.12 ); ayp->addref();
		RGBScalarPainter* alphaRGB = new RGBScalarPainter( 0.1, 0.3, 0.6 ); alphaRGB->addref();

		struct Row { const char* name; int model; double alpha; double deg; bool backFace; bool perChannel; double tilt; };
		const Row rows[] = {
			{ "iso   a.1  th30",                     0, 0.1, 30.0, false, false,  0.0 },
			{ "iso   a.3  th30",                     0, 0.3, 30.0, false, false,  0.0 },
			{ "iso   a.6  th60",                     0, 0.6, 60.0, false, false,  0.0 },
			{ "iso   a.3  th45 BACK FACE",           0, 0.3, 45.0, true,  false,  0.0 },
			{ "iso   perchannel(.1,.3,.6) th30",     0, 0.3, 30.0, false, true,   0.0 },
			{ "aniso a.3/.12 th30",                  1, 0.3, 30.0, false, false,  0.0 },
			{ "aniso a.6/.12 th60",                  1, 0.6, 60.0, false, false,  0.0 },
			{ "aniso perchannel(.1,.3,.6)/.12 th30", 1, 0.3, 30.0, false, true,   0.0 },
			// TILTED (P2-3): the shape gate on the `aD < 1` arm.
			{ "iso   a.3  th30 TILT 30",             0, 0.3, 30.0, false, false, 30.0 },
			{ "iso   a.3  th30 TILT 60",             0, 0.3, 30.0, false, false, 60.0 },
			{ "aniso a.3/.12 th30 TILT 30",          1, 0.3, 30.0, false, false, 30.0 },
			{ "aniso a.3/.12 th30 TILT 60",          1, 0.3, 30.0, false, false, 60.0 },
		};
		const int nRows = int( sizeof(rows)/sizeof(rows[0]) );

		for( int r = 0; r < nRows; r++ ) {
			UniformScalarPainter* al = new UniformScalarPainter( rows[r].alpha ); al->addref();
			IScalarPainter* ax = rows[r].perChannel ? (IScalarPainter*)alphaRGB : (IScalarPainter*)al;

			WardIsotropicGaussianSPF* iso =
				new WardIsotropicGaussianSPF( *rd, *rs, *ax ); iso->addref();
			WardAnisotropicEllipticalGaussianSPF* aniso =
				new WardAnisotropicEllipticalGaussianSPF( *rd, *rs, *ax, *ayp ); aniso->addref();
			ISPF* spf = ( rows[r].model == 0 ) ? (ISPF*)iso : (ISPF*)aniso;

			const RayIntersectionGeometric ri = rows[r].backFace
				? MakeBackFaceIntersection( rows[r].deg * PI / 180.0 )
				: ( rows[r].tilt > 0
					? MakeTiltedIntersection( rows[r].deg * PI / 180.0, rows[r].tilt * PI / 180.0 )
					: MakeIntersection( rows[r].deg * PI / 180.0 ) );

			double sampledMass = 0, pdfMass = 0;
			const double tvd = MeasureTVD( *spf, ri, iorStack, kSeedA + 8000u + unsigned(r),
			                               600000, sampledMass, pdfMass );

			std::cout << "   " << std::left << std::setw(40) << rows[r].name << std::right
			          << std::setprecision(6) << std::setw(10) << tvd
			          << std::setw(14) << sampledMass
			          << std::setw(12) << pdfMass << std::endl;

			Check( tvd < 0.02,
			       std::string( "Section E: Pdf describes the sampler's shape: " ) + rows[r].name );

			iso->release(); aniso->release(); al->release();
		}

		rd->release(); rs->release(); ayp->release(); alphaRGB->release();
	}

	//----------------------------------------------------------------
	// SECTION F -- DEFECT (3): `kray_I * p_I == f_I cos`.
	//
	// Diffuse painter BLACK so `value()` IS the specular term, exactly
	// as DL-127 section 1 does it.  RGB, NM, and the per-channel alpha
	// branch (where each lane's kray is a single channel).
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section F: kray_S * p_S / (f_S cos) over real Scatter draws "
	             "(must be exactly 1)" << std::endl;
	std::cout << "   model  lane  alpha   theta        mean          min          max"
	             "     | kray vs this file's own closed form" << std::endl;
	{
		UniformScalarPainter* ay = new UniformScalarPainter( 0.12 ); ay->addref();
		RGBScalarPainter* alphaRGB = new RGBScalarPainter( 0.1, 0.3, 0.6 ); alphaRGB->addref();

		for( int model = 0; model < 2; model++ ) {
		for( int lane = 0; lane < 3; lane++ ) {   // 0 = RGB, 1 = NM, 2 = per-channel RGB
			for( int ai = 0; ai < 3; ai++ ) {
				if( lane == 2 && ai > 0 ) continue;   // per-channel row has its own alphas
				UniformScalarPainter* al = new UniformScalarPainter( alphas[ai] ); al->addref();
				IScalarPainter* ax = ( lane == 2 ) ? (IScalarPainter*)alphaRGB : (IScalarPainter*)al;

				WardIsotropicGaussianSPF*  isoS = new WardIsotropicGaussianSPF( *black, *spec, *ax ); isoS->addref();
				WardIsotropicGaussianBRDF* isoB = new WardIsotropicGaussianBRDF( *black, *spec, *ax ); isoB->addref();
				WardAnisotropicEllipticalGaussianSPF*  anS =
					new WardAnisotropicEllipticalGaussianSPF( *black, *spec, *ax, *ay ); anS->addref();
				WardAnisotropicEllipticalGaussianBRDF* anB =
					new WardAnisotropicEllipticalGaussianBRDF( *black, *spec, *ax, *ay ); anB->addref();

				// Companion BRDFs with BOTH painters black: their
				// `valueNM` is exactly the JH black-cell leak the NM
				// lane has to subtract (see below).  Zero in RGB.
				WardIsotropicGaussianBRDF* isoLeak =
					new WardIsotropicGaussianBRDF( *black, *black, *ax ); isoLeak->addref();
				WardAnisotropicEllipticalGaussianBRDF* anLeak =
					new WardAnisotropicEllipticalGaussianBRDF( *black, *black, *ax, *ay ); anLeak->addref();

				ISPF*  spf  = ( model == 0 ) ? (ISPF*)isoS : (ISPF*)anS;
				IBSDF* brdf = ( model == 0 ) ? (IBSDF*)isoB : (IBSDF*)anB;
				IBSDF* leakBrdf = ( model == 0 ) ? (IBSDF*)isoLeak : (IBSDF*)anLeak;

				for( int d = 0; d < 4; d++ ) {
					const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
					const Vector3 n = ri.onb.w();
					RatioStat st = { 0, 0, 0, 0 };
					// The SAME quantity re-derived without going through
					// `Ward*BRDF::value` at all: `kray / (Rs * R)` with
					// `R` this file's own closed form.  An error shared
					// between the SPF and its BRDF would leave `st` at 1
					// and move this instead.
					RatioStat stRef = { 0, 0, 0, 0 };
					const OrthonormalBasis3D onbF = SamplingFrame( ri );
					const Vector3 wiF = Vector3Ops::Normalize( -ri.ray.Dir() );
					const double cosIF = Vector3Ops::Dot( wiF, onbF.w() );
					RandomNumberGenerator rng( kSeedA + 20000u + 1000u*unsigned(model) + 100u*unsigned(lane) + 10u*unsigned(ai) + unsigned(d) );
					IndependentSampler sampler( rng );
					const double nmHero = 550.0;

					for( int i = 0; i < 20000; i++ ) {
						ScatteredRayContainer sc;
						if( lane == 1 ) spf->ScatterNM( ri, sampler, nmHero, sc, iorStack );
						else            spf->Scatter( ri, sampler, sc, iorStack );

						for( unsigned int j = 0; j < sc.Count(); j++ ) {
							if( sc[j].type != ScatteredRay::eRayReflection ) continue;
							if( sc[j].pdf <= 0 ) continue;
							const Vector3 wo = Vector3Ops::Normalize( sc[j].ray.Dir() );
							const double cosO = Vector3Ops::Dot( wo, n );
							if( cosO <= 0 ) continue;

							double f, kr;
							if( lane == 1 ) {
								// The diffuse painter is the literal
								// black `(0,0,0)`, but `GetColorNM` puts
								// it through the Jakob-Hanika uplift,
								// which leaks ~2.5e-5 -- enough to
								// dominate `valueNM` wherever the
								// specular lobe is faint.  A companion
								// BRDF with BOTH painters black reads
								// `D*leak + S*leak`; the live one reads
								// `D*leak + S*Rs`, so their difference is
								// `S*(Rs - leak)` and the specular term
								// alone is that times `Rs/(Rs - leak)`.
								// (Subtracting the companion outright
								// over-removes `S*leak` and biases the
								// ratio by exactly `leak/Rs` = 5.0e-5,
								// which is what this row read before the
								// rescale.)
								const double vLive = brdf->valueNM( wo, ri, nmHero );
								const double vLeak = leakBrdf->valueNM( wo, ri, nmHero );
								const double rsNM  = GuardedGetColorNM( *spec, ri, nmHero );
								const double lkNM  = GuardedGetColorNM( *black, ri, nmHero );
								f  = ( rsNM > lkNM )
								   ? ( vLive - vLeak ) * rsNM / ( rsNM - lkNM )
								   : 0.0;
								kr = sc[j].krayNM;
							} else {
								// Per-channel: each lane's kray is ONE
								// channel, and `value()` at that channel
								// is the matching term.  MaxValue picks
								// the live channel in both.
								f  = ColorMath::MaxValue( brdf->value( wo, ri ) );
								kr = ColorMath::MaxValue( sc[j].kray );
								if( lane == 2 ) {
									// Identify the live channel and read
									// the BRDF there, since the three
									// lanes have different alphas.
									int ch = 0;
									double best = sc[j].kray[0];
									for( int c = 1; c < 3; c++ ) {
										if( sc[j].kray[c] > best ) { best = sc[j].kray[c]; ch = c; }
									}
									f  = brdf->value( wo, ri )[ch];
									kr = sc[j].kray[ch];
								}
							}
							if( f <= 0 || kr <= 0 ) continue;

							const double ratio = ( kr * sc[j].pdf ) / ( f * cosO );
							if( !std::isfinite( ratio ) ) continue;
							AccumRatio( st, ratio );

							// BRDF-free cross-check.  `Rs` is 0.5 on
							// every channel here, so the live channel's
							// reflectance is 0.5 whichever lane fired.
							const Vector3 hF = Vector3Ops::Normalize( wiF + wo );
							const double cHF = Vector3Ops::Dot( hF, onbF.w() );
							const double hwF = Vector3Ops::Dot( hF, wo );
							const double cosOF = Vector3Ops::Dot( wo, onbF.w() );
							// The specular reflectance this lane
							// actually carries: 0.5 exactly in RGB, the
							// uplift's own 0.499999 at 550 nm.
							const double rsLane = ( lane == 1 )
								? GuardedGetColorNM( *spec, ri, nmHero )
								: 0.5;
							const double refK = rsLane * RefKrayRatio( hwF, cHF, cosOF, cosIF );
							if( refK > 0 ) {
								AccumRatio( stRef, kr / refK );
							}
						}
					}

					const char* laneName = ( lane == 0 ) ? "RGB " : ( lane == 1 ? "NM  " : "chan" );
					std::cout << "   " << ( model == 0 ? "iso  " : "aniso" )
					          << "  " << laneName
					          << "  " << std::setprecision(2) << std::setw(5)
					          << ( lane == 2 ? 0.0 : alphas[ai] )
					          << "   " << std::setw(5) << std::setprecision(1) << degs[d]
					          << "   " << std::setprecision(6) << std::setw(11) << st.mean
					          << "  " << std::setw(11) << st.minR
					          << "  " << std::setw(11) << st.maxR
					          << "   | vs closed form: mean " << stRef.mean
					          << " [" << stRef.minR << ", " << stRef.maxR << "]" << std::endl;

					Check( st.n > 1000, "Section F: draws produced" );
					Check( fabs( st.minR - 1.0 ) < 1e-6 && fabs( st.maxR - 1.0 ) < 1e-6,
					       "Section F: kray_S * p_S == f_S cos per draw" );
					Check( stRef.n > 1000 && fabs( stRef.minR - 1.0 ) < 1e-6 && fabs( stRef.maxR - 1.0 ) < 1e-6,
					       "Section F: kray_S == Rs * (h.wo) cos^3(th_h) sqrt(nl/nv), BRDF-free" );
				}

				isoS->release(); isoB->release(); anS->release(); anB->release();
				isoLeak->release(); anLeak->release();
				al->release();
			}
		}
		}
		ay->release(); alphaRGB->release();
	}

	//----------------------------------------------------------------
	// SECTION G -- the GRAZING TAIL (the question DL-177 was held on).
	//
	// The corrected weight is `Rs (h.wo) cos^3(th_h) sqrt(nl/nv)`.
	// Every factor but the last is at most 1, and `nl = cos_o` is the
	// SAMPLED direction's own cosine, so the weight is bounded above by
	// `Rs / sqrt(nv)` -- a per-shading-point constant, NOT a per-draw
	// divergence.  This section measures the realized distribution at
	// 80/85/89 degrees so the claim is data and not algebra, and
	// reports the furnace estimator's own relative standard error.
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section G: grazing tail of the corrected kray" << std::endl;
	std::cout << "   model  alpha  theta   E[sum kray]      max      p99.9   bound Rs/sqrt(nv)"
	             "   rel.s.e." << std::endl;
	{
		UniformScalarPainter* ay = new UniformScalarPainter( 0.12 ); ay->addref();
		const double tailDegs[] = { 0.0, 30.0, 60.0, 80.0, 85.0, 89.0, 89.9 };
		const int nTailDegs = int( sizeof(tailDegs)/sizeof(tailDegs[0]) );

		for( int model = 0; model < 2; model++ ) {
			for( int ai = 0; ai < 3; ai++ ) {
				UniformScalarPainter* al = new UniformScalarPainter( alphas[ai] ); al->addref();
				WardIsotropicGaussianSPF* isoS = new WardIsotropicGaussianSPF( *black, *spec, *al ); isoS->addref();
				WardAnisotropicEllipticalGaussianSPF* anS =
					new WardAnisotropicEllipticalGaussianSPF( *black, *spec, *al, *ay ); anS->addref();
				ISPF* spf = ( model == 0 ) ? (ISPF*)isoS : (ISPF*)anS;

				for( int d = 0; d < nTailDegs; d++ ) {
					const RayIntersectionGeometric ri = MakeIntersection( tailDegs[d] * PI / 180.0 );
					const double nv = cos( tailDegs[d] * PI / 180.0 );
					const double bound = 0.5 / sqrt( nv );

					RandomNumberGenerator rng( kSeedA + 30000u + 1000u*unsigned(model) + 10u*unsigned(ai) + unsigned(d) );
					IndependentSampler sampler( rng );
					const int N = 200000;
					std::vector<double> draws;
					draws.reserve( N );
					double sum = 0, sumSq = 0;
					for( int i = 0; i < N; i++ ) {
						ScatteredRayContainer sc;
						spf->Scatter( ri, sampler, sc, iorStack );
						double perCall = 0;
						for( unsigned int j = 0; j < sc.Count(); j++ ) {
							if( sc[j].type != ScatteredRay::eRayReflection ) continue;
							perCall += ColorMath::MaxValue( sc[j].kray );
						}
						draws.push_back( perCall );
						sum += perCall;
						sumSq += perCall * perCall;
					}
					// Independent reference: the BRDF's OWN directional
					// albedo, `int max(value()) cos dw` on a 400x800 grid.
					// `E[sum kray]` must land on it -- that is what
					// "the sampled continuation integrates the BRDF"
					// MEANS, and it is the DL-127 section-3 check.
					double Q = 0;
					{
						WardIsotropicGaussianBRDF* isoB =
							new WardIsotropicGaussianBRDF( *black, *spec, *al ); isoB->addref();
						WardAnisotropicEllipticalGaussianBRDF* anB =
							new WardAnisotropicEllipticalGaussianBRDF( *black, *spec, *al, *ay ); anB->addref();
						IBSDF* brdfQ = ( model == 0 ) ? (IBSDF*)isoB : (IBSDF*)anB;
						const int QT = 400, QP = 800;
						for( int t = 0; t < QT; t++ ) {
							const double th = ( t + 0.5 ) * PI_OV_TWO / QT;
							const double dT = PI_OV_TWO / QT;
							const double sT = sin( th ), cT = cos( th );
							for( int q = 0; q < QP; q++ ) {
								const double ph = ( q + 0.5 ) * TWO_PI / QP;
								const double dP = TWO_PI / QP;
								Vector3 wo( sT*cos(ph), sT*sin(ph), cT );
								wo = Vector3Ops::Normalize( wo );
								Q += ColorMath::MaxValue( brdfQ->value( wo, ri ) ) * cT * sT * dT * dP;
							}
						}
						isoB->release(); anB->release();
					}

					std::sort( draws.begin(), draws.end() );
					const double mean = sum / double(N);
					const double var = r_max( 0.0, sumSq/double(N) - mean*mean );
					const double relSE = ( mean > 0 ) ? sqrt( var / double(N) ) / mean : 0.0;
					const double p999 = draws[ size_t( 0.999 * double(N) ) ];
					const double mx = draws.back();

					std::cout << "   " << ( model == 0 ? "iso  " : "aniso" )
					          << "  " << std::setprecision(2) << std::setw(4) << alphas[ai]
					          << "   " << std::setw(5) << std::setprecision(1) << tailDegs[d]
					          << "   " << std::setprecision(6) << std::setw(11) << mean
					          << "  " << std::setw(9) << mx
					          << "  " << std::setw(9) << p999
					          << "        " << std::setw(9) << bound
					          << "   " << std::setw(9) << relSE
					          << "   int f cos = " << std::setw(9) << Q
					          << "   E/Q = " << std::setw(9) << ( Q > 0 ? mean/Q : 0.0 )
					          << std::endl;

					// The bound is the whole point: the corrected weight
					// cannot exceed Rs/sqrt(nv), which is 1.29 at 80deg,
					// 1.70 at 85deg and 3.79 at 89deg for Rs = 0.5.
					Check( mx <= bound * 1.000001,
					       "Section G: per-draw kray is bounded by Rs/sqrt(nv)" );
					// 1.5%: the estimator's own relative s.e. is <= 0.4%
					// here and the quadrature's grid error is the rest.
					Check( Q > 0 && fabs( mean/Q - 1.0 ) < 0.015,
					       "Section G: E[sum kray] == int max(value()) cos dw" );
				}

				isoS->release(); anS->release(); al->release();
			}
		}
		ay->release();
	}

	//----------------------------------------------------------------
	// SECTION H -- the tail in CONTEXT (REPORTING ONLY, not gated).
	//
	// Section G bounds the corrected weight absolutely.  That answers
	// "is it finite", not "is it heavy for a path tracer", and the
	// second question is only answerable against the tails the renderer
	// already lives with.  So: the same per-draw statistic at the same
	// grazing incidence, on the two Ward SPFs and on two controls --
	// `GGXSPF` (single-emit, energy-compensated, the tightest weight in
	// the tree) and the post-DL-127 `SchlickSPF` (whose own model omits
	// Schlick's geometric term, DL-178, and which therefore carries a
	// genuinely heavy tail that SHIPPED).  `max/mean` is the shape
	// number: 1 is a constant weight, large is a firefly source.
	// (Review round 1, P3-b.)
	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section H: per-draw sum(max(kray)) at 89.9 deg, Ward against controls "
	             "(reporting only)" << std::endl;
	std::cout << "   SPF                                     mean        p99.9         max    max/mean" << std::endl;
	{
		UniformScalarPainter* a03  = new UniformScalarPainter( 0.3 );  a03->addref();
		UniformScalarPainter* a012 = new UniformScalarPainter( 0.12 ); a012->addref();
		UniformScalarPainter* iorS = new UniformScalarPainter( 1.5 );  iorS->addref();
		UniformScalarPainter* extS = new UniformScalarPainter( 2.5 );  extS->addref();
		UniformScalarPainter* isoS = new UniformScalarPainter( 1.0 );  isoS->addref();

		WardIsotropicGaussianSPF* wi = new WardIsotropicGaussianSPF( *black, *spec, *a03 ); wi->addref();
		WardAnisotropicEllipticalGaussianSPF* wa =
			new WardAnisotropicEllipticalGaussianSPF( *black, *spec, *a03, *a012 ); wa->addref();
		// GGX conductor: single-emit, so one ray per call.
		GGXSPF* gg = new GGXSPF( *black, *spec, *a03, *a03, *iorS, *extS ); gg->addref();
		// Schlick at the same specular reflectance and roughness.
		SchlickSPF* sc = new SchlickSPF( *black, *spec, *a03, *isoS ); sc->addref();

		struct Entry { const char* name; ISPF* spf; };
		const Entry entries[] = {
			{ "WardIsotropicGaussianSPF   a.3",        wi },
			{ "WardAnisotropicEllipticalSPF a.3/.12",  wa },
			{ "GGXSPF conductor a.3 (control)",        gg },
			{ "SchlickSPF r.3 iso1 (control, DL-178)", sc },
		};

		const RayIntersectionGeometric ri = MakeIntersection( 89.9 * PI / 180.0 );
		for( int e = 0; e < 4; e++ ) {
			RandomNumberGenerator rng( kSeedA + 40000u + unsigned(e) );
			IndependentSampler sampler( rng );
			const int N = 200000;
			std::vector<double> draws;
			draws.reserve( N );
			double sum = 0;
			for( int i = 0; i < N; i++ ) {
				ScatteredRayContainer sc2;
				entries[e].spf->Scatter( ri, sampler, sc2, iorStack );
				double perCall = 0;
				for( unsigned int j = 0; j < sc2.Count(); j++ ) {
					if( sc2[j].isDelta ) continue;
					perCall += ColorMath::MaxValue( sc2[j].kray );
				}
				draws.push_back( perCall );
				sum += perCall;
			}
			std::sort( draws.begin(), draws.end() );
			const double mean = sum / double(N);
			const double p999 = draws[ size_t( 0.999 * double(N) ) ];
			const double mx   = draws.back();
			std::cout << "   " << std::left << std::setw(40) << entries[e].name << std::right
			          << std::setprecision(4) << std::setw(10) << mean
			          << std::setw(13) << p999
			          << std::setw(12) << mx
			          << std::setw(12) << ( mean > 0 ? mx/mean : 0.0 ) << std::endl;
			Check( draws.size() == size_t(N),
			       std::string( "Section H: control probe ran: " ) + entries[e].name );
		}

		wi->release(); wa->release(); gg->release(); sc->release();
		a03->release(); a012->release(); iorS->release(); extS->release(); isoS->release();
	}

	spec->release();
	black->release();
	g_stubObject->release();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
