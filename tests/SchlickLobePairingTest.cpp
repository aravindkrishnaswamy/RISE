//////////////////////////////////////////////////////////////////////
//
//  SchlickLobePairingTest.cpp - Closed-form red-proof for DL-69:
//    a multi-lobe SPF's AGGREGATE BSDF value must not be paired with a
//    PER-LOBE selection density.
//
//  DL-69 (docs/DL69_BDPT_LOBE_THROUGHPUT.md).  `SchlickSPF::Scatter`
//  emits TWO non-delta lobes with OVERLAPPING support into one
//  `ScatteredRayContainer` -- a cosine-weighted diffuse lobe
//  (`SchlickSPF.cpp`, `GenerateDiffuseRay`) and a Schlick half-vector
//  specular lobe (`GenerateSpecularRay`) -- each carrying its OWN
//  conditional density in `ScatteredRay::pdf`.  Exactly one lobe is
//  then drawn by `ScatteredRayContainer::RandomlySelect` with
//  realized probability `q_I = max(kray_I) / sum_J max(kray_J)`.
//
//  Three pairings are arithmetically available for the throughput of
//  that one draw.  Writing `f_I` for lobe I's own BSDF term, `f_agg =
//  sum_J f_J` for `SchlickBRDF::value()`, `p_I` for the lobe's own
//  conditional density, and `P_mix` for `SchlickSPF::Pdf()` (a fixed,
//  direction-only mixture of the two conditionals):
//
//    (a) PER-LOBE      X_a = kray_I / q_I
//                          = f_I * cos / (p_I * q_I)
//        E[X_a] = sum_I integral p_I * kray_I dw  ==  integral f_agg cos dw.
//        This is RISE's shipped convention: PT's ordinary
//        initialization (`PathTracingIntegrator.cpp`,
//        `PTScatterKray<Tag>(*pS) * (1/selectProb)`) and BDPT's own
//        DELTA branch (`KrayValue<Tag>(*pScat) * (.../selectProb)`).
//
//    (b) AGGREGATE     X_b = f_agg * cos / P_mix(w)
//        where `P_mix = ISPF::Pdf` is the TRUE generating density of
//        `Scatter` + `RandomlySelect` (DL-67 Slice 0) -- the MARGINAL
//        `E_D[ sum_I q_I(D) delta_{w_I(D)} ]` over the container draw
//        D AND the lobe choice.  Drawn with that same real procedure,
//        E[X_b] = integral f_agg cos dw exactly.  Veach 9.2 / PBRT's
//        BxDF convention.
//
//    (c) MISMATCHED    X_c = f_agg * cos / (q_I * p_I)     <-- the bug
//        E[X_c] = sum_I integral p_I * f_agg cos / p_I dw
//               = N * integral f_agg cos dw,
//        an N-times OVER-COUNT for N accepted overlapping lobes (N = 2
//        here; up to 4 on `SchlickSPF`'s per-channel specular branch).
//        This is what `GenerateEyeSubpathImpl` /
//        `GenerateLightSubpathImpl` computed for every non-delta lobe
//        before the DL-69 fix (aggregate `EvalBSDFAtVertex` over
//        `scatterPdf = selectProb * pScat->pdf`).
//
//  The N* identity above is EXACT, not asymptotic, and holds even
//  though `q_I` is realization-dependent (it is a function of the
//  already-drawn directions): conditioned on one joint draw
//  (w_D, w_S), the expectation over the lobe choice of
//  `f_agg(w_I) cos / (q_I p_I(w_I))` is `sum_I f_agg(w_I) cos /
//  p_I(w_I)`, whose expectation over the joint draw is N * Q.
//
//  This test measures all three against a BRDF quadrature reference Q
//  and asserts the over-count.  It also pins the SECOND half of DL-69:
//  at the same vertex BDPT stored `pdfFwd = q_I * p_I` (per-lobe,
//  realization-dependent) while `pdfRev` and every connection strategy
//  evaluate `P_mix` via `ISPF::Pdf()` -- two different formulas for
//  what the MIS ratio chain treats as one density.  Section 3 shows
//  the two disagree by a wide margin on real draws.
//
//  Reference-value caveat (deliberate, do not "fix"): `SchlickSPF`'s
//  `kray` is the Schlick-1994 sampling weight, which equals
//  `f_I cos / p_I` only approximately -- tests/SPFBSDFConsistencyTest.cpp
//  documents the resulting MC-vs-quadrature gap for this material
//  (2.1% at 30 deg, 12.7% at 60 deg) and carries a 15% tolerance for
//  it.  That gap is a property of the material, NOT of DL-69, so this
//  test bands (a) against Q at that same 15% and puts its tight
//  assertions on the RATIO (c)/(a), which the gap cancels out of.
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
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/SchlickSPF.h"
#include "../src/Library/Materials/SchlickBRDF.h"
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

//////////////////////////////////////////////////////////////////////
// Synthetic flat-surface intersection, normal +Z, incoming ray in the
// X-Z plane at `incomingTheta` from the normal.  Mirrors
// tests/SPFBSDFConsistencyTest.cpp's MakeIntersection so the two
// tests describe the same geometry.
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

//////////////////////////////////////////////////////////////////////
// Quadrature reference Q = integral over the upper hemisphere of
// f_agg(w) * cos(theta) dw, evaluated from SchlickBRDF::value().
// Same construction as SPFBSDFConsistencyTest's furnace quadrature.
//////////////////////////////////////////////////////////////////////
static double QuadratureAggregate(
	const IBSDF& brdf,
	const RayIntersectionGeometric& ri,
	int nTheta = 200,
	int nPhi = 400 )
{
	const Vector3 n = ri.onb.w();
	double sum = 0;
	const double dTheta = (PI * 0.5) / nTheta;
	const double dPhi   = TWO_PI / nPhi;

	for( int it = 0; it < nTheta; it++ ) {
		const double theta = (it + 0.5) * dTheta;
		const double st = sin( theta );
		const double ct = cos( theta );
		for( int ip = 0; ip < nPhi; ip++ ) {
			const double phi = (ip + 0.5) * dPhi;
			const Vector3 wo( st * cos( phi ), st * sin( phi ), ct );
			const RISEPel f = brdf.value( wo, ri );
			const double fv = ColorMath::MaxValue( f );
			if( fv > 0 && std::isfinite( fv ) ) {
				sum += fv * ct * st * dTheta * dPhi;
			}
		}
	}
	(void)n;
	return sum;
}

//////////////////////////////////////////////////////////////////////
// One measurement point: draw N joint Scatter() containers, and for
// each one evaluate all three pairings on the SAME realized draw.
//////////////////////////////////////////////////////////////////////
struct PairingMeasurement
{
	double meanPerLobe;			// E[X_a]
	double meanAggregate;		// E[X_b]
	double meanMismatched;		// E[X_c]
	double meanLobeCount;		// average number of accepted non-delta lobes
	double meanPdfFwdRatio;		// E[ (q_I * p_I) / P_mix(w_I) ] -- 1 iff the two agree
	double maxPdfFwdRatio;
	double minPdfFwdRatio;
	unsigned int samples;
};

static PairingMeasurement MeasurePairings(
	ISPF& spf,
	const IBSDF& brdf,
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const IORStack& iorStack,
	unsigned int nSamples )
{
	PairingMeasurement m{};
	m.minPdfFwdRatio = 1e300;
	m.maxPdfFwdRatio = 0;

	const Vector3 n = ri.onb.w();

	double sumA = 0, sumB = 0, sumC = 0, sumLobes = 0, sumRatio = 0;
	unsigned int nRatio = 0;
	unsigned int used = 0;

	// All three estimators read ONE realized draw: the container from
	// `Scatter`, then the lobe `RandomlySelect` actually chose.  They
	// differ only in what they divide by -- the lobe's own `kray/q_I`,
	// the aggregate `ISPF::Pdf` at the realized direction, or the
	// conditional `q_I * p_I`.

	for( unsigned int s = 0; s < nSamples; s++ )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );
		if( scattered.Count() == 0 ) {
			continue;
		}

		// Count accepted non-delta lobes and total selection weight.
		double totalKray = 0;
		unsigned int nonDelta = 0;
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			if( scattered[i].isDelta ) continue;
			nonDelta++;
			totalKray += ColorMath::MaxValue( scattered[i].kray );
		}
		if( nonDelta == 0 || totalKray <= 0 ) {
			continue;
		}
		sumLobes += double( nonDelta );

		// --- Draw lobe I exactly the way the integrators do ---------
		const Scalar xi = sampler.Get1D();
		const ScatteredRay* pSel = scattered.RandomlySelect( xi, false );
		if( !pSel || pSel->isDelta ) {
			continue;
		}
		const double krayI = ColorMath::MaxValue( pSel->kray );
		if( krayI <= 0 ) {
			continue;
		}
		const double qI = ( scattered.Count() > 1 ) ? ( krayI / totalKray ) : 1.0;
		if( qI <= 0 ) {
			continue;
		}

		const Vector3 woI = Vector3Ops::Normalize( pSel->ray.Dir() );
		const double cosI = fabs( Vector3Ops::Dot( woI, n ) );
		const double pI = pSel->pdf;
		if( pI <= 0 ) {
			continue;
		}

		const double fAggI = ColorMath::MaxValue( brdf.value( woI, ri ) );

		// (a) per-lobe pairing -- PT's / the delta branch's formula.
		sumA += krayI / qI;

		// (c) the mismatched pairing BDPT used for non-delta lobes.
		sumC += fAggI * cosI / ( qI * pI );

		// --- The aggregate density at the REALIZED direction --------
		// `SchlickSPF::Pdf` is (since DL-67 Slice 0,
		// docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md) the TRUE generating
		// density of `Scatter` + `RandomlySelect`: writing D for the
		// joint random state one `Scatter()` call consumes, w_I(D) for
		// the direction of lobe I in the container it produced and
		// q_I(D) for `RandomlySelect`'s realized probability of
		// choosing it, `Pdf(w)` is the MARGINAL
		//
		//     P_mix(w) = E_D[ sum_I q_I(D) delta_{w_I(D)}(w) ].
		//
		// Both (b) and section 3 below need it at the SAME realized
		// direction the real selection produced, so it is evaluated
		// once here and shared.
		const double pMixAtI = spf.Pdf( ri, woI, iorStack );

		// (b) aggregate/aggregate.  The lobe is drawn with the REAL
		// selection rule -- the container from `Scatter`, then
		// `RandomlySelect`'s max(kray) PMF -- which is exactly the
		// procedure `P_mix` is the density of.  So for any g,
		//
		//     E[ g(w)/P_mix(w) ] = integral_{supp P_mix} g(w) dw,
		//
		// and with g = f_agg cos that integral is Q: the diffuse lobe
		// is a full-hemisphere cosine proposal that `Scatter` always
		// accepts here (shading normal == geometric normal), so
		// `P_mix > 0` wherever `f_agg cos > 0` and nothing is clipped
		// out of the support.  Contrast (c), which divides by
		// `q_I * p_I` -- the density of w_I CONDITIONED on the
		// realized container, not the marginal -- and therefore sums
		// to N*Q over the N lobes.  Equivalently (b) = (c) * r with r
		// the section-3 ratio, so the three estimators below are three
		// readings of one draw.
		//
		// A FIXED 50/50 coin over the realized container is NOT the
		// right draw: `SchlickSPF::Pdf` has not described a fixed coin
		// since DL-67 Slice 0 (it reproduces `RandomlySelect`'s
		// kray-weighted, direction-dependent split, including the
		// specular sampler's rejection rate), so pairing it with a
		// fixed coin is biased -- measured (b)/Q = 0.842/0.821/0.793
		// at 0/30/60 deg against master's `Pdf`.
		if( pMixAtI > 0 ) {
			sumB += fAggI * cosI / pMixAtI;
		}

		// --- pdfFwd vs pdfRev: two formulas, one density -----------
		{
			if( pMixAtI > 0 ) {
				const double r = ( qI * pI ) / pMixAtI;
				sumRatio += r;
				nRatio++;
				if( r < m.minPdfFwdRatio ) m.minPdfFwdRatio = r;
				if( r > m.maxPdfFwdRatio ) m.maxPdfFwdRatio = r;
			}
		}

		used++;
	}

	if( used == 0 ) {
		return m;
	}

	m.samples        = used;
	m.meanPerLobe    = sumA / double( used );
	m.meanAggregate  = sumB / double( used );
	m.meanMismatched = sumC / double( used );
	m.meanLobeCount  = sumLobes / double( used );
	m.meanPdfFwdRatio = nRatio ? ( sumRatio / double( nRatio ) ) : 0.0;
	if( nRatio == 0 ) { m.minPdfFwdRatio = 0; }
	return m;
}

int main()
{
	GlobalLog();

	std::cout << "=== SchlickLobePairingTest ===" << std::endl;
	std::cout << std::fixed << std::setprecision( 5 );

	g_stubObject = new StubObject();
	g_stubObject->addref();

	// Grey 0.4 / 0.4 so the two lobes carry comparable energy and the
	// N=2 over-count is not masked by one lobe dominating selection.
	UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.4, 0.4, 0.4 ) ); rd->addref();
	UniformColorPainter* rs = new UniformColorPainter( RISEPel( 0.4, 0.4, 0.4 ) ); rs->addref();
	UniformScalarPainter* rough = new UniformScalarPainter( 0.5 ); rough->addref();
	UniformScalarPainter* iso   = new UniformScalarPainter( 1.0 ); iso->addref();

	SchlickSPF*  spf  = new SchlickSPF(  *rd, *rs, *rough, *iso ); spf->addref();
	SchlickBRDF* brdf = new SchlickBRDF( *rd, *rs, *rough, *iso ); brdf->addref();

	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	const double thetas[] = { 0.0, 30.0 * PI / 180.0, 60.0 * PI / 180.0 };
	const char*  labels[] = { "theta=0deg", "theta=30deg", "theta=60deg" };

	for( int t = 0; t < 3; t++ )
	{
		const RayIntersectionGeometric ri = MakeIntersection( thetas[t] );
		const double Q = QuadratureAggregate( *brdf, ri );
		const PairingMeasurement m =
			MeasurePairings( *spf, *brdf, ri, sampler, iorStack, 200000 );

		std::cout << std::endl << "-- " << labels[t]
		          << " (draws used " << m.samples
		          << ", mean accepted non-delta lobes " << m.meanLobeCount << ")" << std::endl;
		std::cout << "   Q  (quadrature  int f_agg cos dw)      = " << Q << std::endl;
		std::cout << "   (a) per-lobe    E[kray_I / q_I]        = " << m.meanPerLobe
		          << "   (a)/Q = " << ( Q > 0 ? m.meanPerLobe / Q : 0 ) << std::endl;
		std::cout << "   (b) aggregate   E[f_agg cos / P_mix]   = " << m.meanAggregate
		          << "   (b)/Q = " << ( Q > 0 ? m.meanAggregate / Q : 0 ) << std::endl;
		std::cout << "   (c) MISMATCHED  E[f_agg cos/(q_I p_I)] = " << m.meanMismatched
		          << "   (c)/Q = " << ( Q > 0 ? m.meanMismatched / Q : 0 ) << std::endl;
		std::cout << "   over-count      (c)/(a)                = "
		          << ( m.meanPerLobe > 0 ? m.meanMismatched / m.meanPerLobe : 0 ) << std::endl;
		std::cout << "   pdfFwd/pdfRev   E[(q_I p_I) / P_mix]   = " << m.meanPdfFwdRatio
		          << "   [min " << m.minPdfFwdRatio
		          << ", max " << m.maxPdfFwdRatio << "]" << std::endl;

		Check( m.samples > 100000,
			std::string( "enough usable draws: " ) + labels[t] );
		Check( Q > 0, std::string( "quadrature reference is positive: " ) + labels[t] );
		if( Q <= 0 || m.samples == 0 ) continue;

		// Both lobes reach the container on the majority of draws
		// (measured 1.59-1.67 of 2: the specular proposal's reflected
		// direction dips below the horizon on the rest, where
		// `f_agg` is zero and the lobe is correctly dropped).  The
		// N* prediction is NOT "N = meanLobeCount": a dropped
		// specular proposal contributes zero to BOTH the estimator
		// and the integral, so (c) still lands on exactly 2*Q --
		// which is what the (c)/Q assertion below measures.
		Check( m.meanLobeCount > 1.5,
			std::string( "two overlapping non-delta lobes on most draws: " ) + labels[t] );

		// (a) is the material's own sampling convention; it tracks the
		// BRDF quadrature only to within the Schlick-1994 model gap
		// SPFBSDFConsistencyTest already documents (it bands that
		// material at 15% for roughness 0.3; at the roughness 0.5 used
		// here the measured gap reaches 20% at 60 deg).  This band is
		// deliberately loose -- the tight assertion is the (c)/(a)
		// ratio below, which the model gap cancels out of.
		Check( std::fabs( m.meanPerLobe / Q - 1.0 ) < 0.25,
			std::string( "(a) per-lobe pairing integrates f_agg*cos within the "
			             "documented Schlick model gap: " ) + labels[t] );

		// (b) is exact: aggregate BSDF over the aggregate density.
		Check( std::fabs( m.meanAggregate / Q - 1.0 ) < 0.05,
			std::string( "(b) aggregate/aggregate pairing integrates f_agg*cos: " ) + labels[t] );

		// (c) is the bug: N = 2 overlapping lobes, so it doubles.
		Check( m.meanMismatched / m.meanPerLobe > 1.6,
			std::string( "RED (DL-69): mismatched pairing over-counts vs per-lobe "
			             "by more than 1.6x: " ) + labels[t] );
		Check( m.meanMismatched / Q > 1.6,
			std::string( "RED (DL-69): mismatched pairing over-counts vs the BRDF "
			             "integral by more than 1.6x: " ) + labels[t] );

		// The pdfFwd half of DL-69: the per-lobe density BDPT stored as
		// pdfFwd is not the aggregate density pdfRev and every
		// connection strategy evaluate.
		Check( m.maxPdfFwdRatio / ( m.minPdfFwdRatio > 0 ? m.minPdfFwdRatio : 1.0 ) > 10.0,
			std::string( "RED (DL-69 pdfFwd): per-lobe q_I*p_I and aggregate "
			             "ISPF::Pdf disagree by over an order of magnitude "
			             "across draws: " ) + labels[t] );
	}

	spf->release();
	brdf->release();
	rd->release();
	rs->release();
	rough->release();
	iso->release();
	g_stubObject->release();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
