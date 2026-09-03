//////////////////////////////////////////////////////////////////////
//
//  LayeredWhiteFurnaceTest.cpp - Landing 6 audit.
//
//  Drives every layered-material configuration that L7-L11 will
//  build on through a Monte-Carlo furnace test and reports the
//  directional albedo  ρ(θ_i) = E[Σ_j kray_j]  at each incident
//  angle.  An energy-conserving material has ρ ≤ 1 ± noise; gain
//  > 1 is a bug; significant loss is the documented limitation we
//  want to localise.
//
//  Configurations under audit (matches docs/PHYSICALLY_BASED_PIPELINE_PLAN.md
//  Landing 6 §"Audit scope"):
//
//    1. GGX-PBR, schlick_f0 — sanity baseline (Kulla-Conty active)
//    2. Sheen alone (Charlie distribution, no LUT compensation)
//    3. Composite: dielectric over Lambertian — known-working
//    4. Composite: GGX over Lambertian — clearcoat-style
//    5. Composite: GGX over GGX-PBR — clearcoat over PBR base
//    6. Composite: Sheen over GGX-PBR — fabric over PBR base
//    8-10. polished_material (docs/WETNESS_COAT_DESIGN.md §6.2/§13
//          Phase-1 exit gate) at tau in {1.0, 0.5, 0.9} — puts a
//          measured number on the Rd·Rs·(1−c) coverage-mask deficit
//    11-18. coated_material (docs/WETNESS_COAT_DESIGN.md Phase 2)
//    19-20. substrate reference rows for the fabric block below
//    21-36. fabric_material (docs/CLOTH_FABRIC_DESIGN.md Phase 1,
//           9.9 gate 3, round 5) — Charlie sheen + Kulla-Conty
//           product-form base compensation over four substrate shapes
//           x four sheen roughnesses
//
//  Build (matches existing GGXWhiteFurnaceTest / SPFBSDFConsistencyTest
//  patterns):
//
//    Linux/macOS: `make -C build/make/rise tests` picks this up via
//    the tests/*.cpp glob and links against bin/librise.a.
//
//    Windows: `cmake -S build/cmake/rise-tests` ditto.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>   // std::min / std::max -- used by the fabric energy helpers
#include <cstdlib>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"

#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Materials/SheenSPF.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/Materials/CompositeSPF.h"
#include "../src/Library/Materials/PolishedSPF.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/Materials/CoatedLayer.h"
#include "../src/Library/Materials/OrenNayarMaterial.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/Materials/FibreLobeMath.h"
#include "WeaveTestFixture.h"
#include "../src/Library/Materials/SheenDirectionalAlbedo.h"
#include "../src/Library/Materials/CharlieSheen.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Configuration
// ============================================================

// 100k samples per (config, angle) gives a 1-σ on the mean of
// ~3e-3 for albedos near 1.0 — well below the per-config tolerance
// gates (1-5 %).
static const int    FURNACE_SAMPLES = 100000;

// Incident angles to probe.  Grazing (80°) is the regime where
// most known energy-loss patterns surface — the furnace test
// reports per-angle so we can localise "loses at grazing only"
// vs "loses uniformly".
static const double THETA_DEG[] = { 0.0, 30.0, 60.0, 80.0 };
static const int    NUM_THETA   = sizeof(THETA_DEG) / sizeof(THETA_DEG[0]);

// Per-configuration audit posture.  The L6 audit is partly a
// regression suite ("this configuration WAS passing; flag if it
// regresses") and partly a record of known limitations ("this
// configuration is known to fail until we add X compensation").
// Mixing those into one pass-criterion would either (a) tighten
// every gate and report nothing useful when the known-bad cases
// fail, or (b) loosen every gate and stop catching regressions.
// Per-config posture lets the test exit-zero when behaviour
// matches the documented expectation, and exit-nonzero only when
// something unexpectedly drifts.
enum AuditPosture
{
	kPosturePass,			// Must pass within tolerance: 1.0 - tol ≤ ρ ≤ 1.0 + tol.  Failure is a regression.
	kPostureBounded,		// 2026-05: must be energy-bounded (0 ≤ ρ ≤ 1.0 + tol) but no lower constraint — for single-scattering BRDFs (Charlie sheen) that legitimately dissipate energy without violating conservation.  Catches both negative values and over-unity blow-ups.
	kPostureMatchesPrediction,	// 2026-08-31: must match a per-config, per-angle analytic prediction (ConfigReport::predicted[]) to within ConfigReport::predictionEps.  For configurations whose expected DEFICIT is itself the thing under test (e.g. docs/WETNESS_COAT_DESIGN.md §6.2's Rd·Rs·(1-c) coverage-mask deficit) -- kPostureBounded's generic [0, 1+tol] band would let the deficit silently vanish (tau becoming a no-op) or blow past its analytic value without failing, which defeats the point of a regression gate built to confirm a specific number.  Distinct from kPostureBounded, which stays for configs (Charlie sheen) whose dissipation has no single predicted curve to check against.
	kPostureKnownFailure	// Documented non-conservation; record numbers but don't fail.
};

static StubObject* g_stubObject = 0;

// ============================================================
//  Synthetic intersection — same fixture pattern as
//  SPFBSDFConsistencyTest::MakeIntersection.
// ============================================================

static RayIntersectionGeometric MakeIntersection( double incomingThetaRad )
{
	const double sinT = std::sin( incomingThetaRad );
	const double cosT = std::cos( incomingThetaRad );

	// Incoming ray direction is INTO the surface (negative z when
	// the surface normal is +z).  RISE follows the convention that
	// ri.ray.Dir() points along the camera-side ray, so a hit
	// looking down at the surface from above has -cosT in z.
	const Vector3 inDir( sinT, 0, -cosT );
	const Ray inRay( Point3( sinT, 0, 1.0 ), inDir );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0 / cosT;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );

	return ri;
}

// ============================================================
//  Furnace driver
//
//  Returns ρ(θ_i) = E[Σ_j kray_j] across FURNACE_SAMPLES MC
//  draws.  For energy-conserving materials with unity inputs this
//  is the directional albedo and ≤ 1 (modulo MC noise).
//
//  Reports max-over-channels so a bug that hits one channel only
//  (e.g. F0 colour) doesn't get washed out by averaging.
// ============================================================

static double DirectionalAlbedo(
	ISPF& spf,
	double incomingThetaRad,
	double* outRejectionRate = 0 )
{
	RayIntersectionGeometric ri = MakeIntersection( incomingThetaRad );
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	const Vector3 normal = ri.onb.w();
	double sum = 0;
	int    validSamples = 0;		// samples that produced at least one usable ray

	for( int i = 0; i < FURNACE_SAMPLES; ++i )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );

		if( scattered.Count() == 0 ) continue;

		double sampleContrib = 0;
		bool   any = false;

		for( unsigned int j = 0; j < scattered.Count(); ++j )
		{
			const ScatteredRay& scat = scattered[j];

			// Include BOTH delta and non-delta rays.  For energy
			// conservation, kray is the fraction of incoming
			// irradiance carried by this scattered ray regardless
			// of measure: delta rays carry an integer fraction
			// (e.g. F for a perfect mirror); non-delta rays carry
			// BSDF·cos/pdf which integrates to directional albedo
			// across many samples.  Summing kray over ALL outgoing
			// rays from a single Scatter call gives the per-sample
			// energy throughput; mean across samples is the
			// directional albedo.  Earlier revision of this test
			// skipped delta rays and reported ρ=0 for layered
			// materials whose top layer is a perfect dielectric —
			// every Scatter walk emerged as a delta ray, so
			// "skipping delta" zeroed the signal.
			//
			// Skip only the reflected-into-substrate hemisphere
			// (cosWo ≤ 0): those are below-surface paths the
			// integrator wouldn't propagate.
			const Vector3 wo = Vector3Ops::Normalize( scat.ray.Dir() );
			const double cosO = Vector3Ops::Dot( wo, normal );
			if( cosO <= 0 ) continue;

			// Max-channel albedo so a per-channel gain stands out
			// even when the average across channels is OK.  All
			// inputs in this test are grayscale (R=G=B) so this
			// equals any single channel.
			const double kMax = ColorMath::MaxValue( scat.kray );
			if( kMax >= 0 && kMax < 1e6 )	// guard against NaN / inf
			{
				sampleContrib += kMax;
				any = true;
			}
		}

		if( any )
		{
			sum += sampleContrib;
			validSamples++;
		}
	}

	// NORMALIZE BY THE SAMPLE COUNT, NOT BY THE SURVIVORS.
	//
	// rho is E[sum_j kray_j] over ALL draws.  A draw whose sampler
	// rejected everything (below the horizon, zero Fresnel branch,
	// a recursion budget exhausted) contributed ZERO energy and is a
	// legitimate zero in that expectation -- dividing it out instead
	// re-normalises the estimate upward by 1/(survival rate) and
	// reports a material as more energy-conserving than it is.  The
	// old `sum / validSamples` form hid exactly the failure mode this
	// audit exists to find.
	//
	// The rejection rate is reported alongside so a config whose
	// numbers move can be told apart from a config whose SAMPLER
	// started rejecting.
	if( outRejectionRate ) {
		*outRejectionRate = ( FURNACE_SAMPLES > 0 )
			? ( 1.0 - (double)validSamples / (double)FURNACE_SAMPLES ) : 0.0;
	}
	return sum / (double)FURNACE_SAMPLES;
}

// ============================================================
//  Downward-ray probe (docs/CLOTH_FABRIC_DESIGN.md 9.9 gate 4)
//
//  Config 7's 2026-09-01 re-diagnosis rested on a direct measurement:
//  GGXSPF only ever emits UPWARD lobes, so a GGX top layer never hands
//  CompositeSPF's random walk a downward ray and the substrate is NEVER
//  REACHED.  That measurement was made by hand and written into the
//  note.  Gate 4 asks for the same probe to be run against config 6's
//  top layer (Charlie sheen), and for config 6's note to record the
//  answer whichever way it comes out -- because if sheen also emits no
//  downward ray, then config 6's deficit is the SAME structural defect
//  as config 7's, not the "sheen's intrinsic dissipation, amplified"
//  the old note asserted.
//
//  Returns the fraction of Scatter draws that produced at least one ray
//  with cos(wo, n) < 0 -- i.e. a ray the composite walk could carry
//  INTO the substrate.
// ============================================================

static double DownwardRayFraction( ISPF& spf, double incomingThetaRad, int samples )
{
	RayIntersectionGeometric ri = MakeIntersection( incomingThetaRad );
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	const Vector3 normal = ri.onb.w();
	int down = 0;

	for( int i = 0; i < samples; ++i )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );
		for( unsigned int j = 0; j < scattered.Count(); ++j )
		{
			const Vector3 wo = Vector3Ops::Normalize( scattered[j].ray.Dir() );
			if( Vector3Ops::Dot( wo, normal ) < 0 ) { ++down; break; }
		}
	}
	return ( samples > 0 ) ? ( (double)down / (double)samples ) : 0.0;
}

// ============================================================
//  Substrate albedo with the fuzz layer's per-direction transmission
//  folded in -- the exact quantity fabric's directional albedo needs.
//
//      W(v) = INT f_base(l, v) * (1 - m*E(alpha, n.l)) * (n.l) dl
//
//  IDENTICAL to DirectionalAlbedo above except for the extra per-ray
//  weight, deliberately: the two share the estimator, the sample count
//  and the normalise-by-ALL-draws convention, so they cannot disagree
//  about anything but the weight itself.
//
//  WHY THIS EXISTS RATHER THAN REUSING rho_substrate.  Fabric's
//  directional albedo is
//
//      rho(v) = sheenColor*E(v) + (1 - m*E(v))/(1 - m*Ebar) * W(v)
//
//  and the tempting shortcut is W(v) = rho_substrate(v) * (1 - m*Ebar),
//  i.e. pulling the substrate's albedo out of the integral.  That is
//  route 1's uncorrelated-response approximation, and at GRAZING it is
//  not small: a GGX substrate's own grazing over-unity comes from a
//  narrow specular lobe near the mirror direction, which at theta_v =
//  80 deg sits exactly where E(l) is LARGE, so the (1 - m*E(l)) factor
//  suppresses it far more than an uncorrelated model predicts.
//  Measured: the shortcut over-predicts the iso-GGX row by 0.029 at
//  theta = 80, an order of magnitude more than the bihemispherical
//  residual gate 5b reports (<= 0.64 %), because the bihemispherical
//  average dilutes exactly that corner.
//
//  So this measures W directly and the prediction carries NO
//  approximation -- which is what lets the tolerance below be set from
//  MC noise rather than from whichever value happens to pass.
// ============================================================

// ============================================================
//  The fuzz-layer factors, RE-DERIVED IN THE TEST from the baked
//  table alone -- deliberately NOT FabricBRDF's own statics.
//
//  M3 review, 2026-09-02: the gate-3 prediction used to call
//  `FabricBRDF::SheenTransmit` / `SheenTransmitMean`, i.e. the very
//  functions `FabricBRDF::value` uses.  That made the non-Lambertian
//  rows' oracle "Scatter agrees with value()'s own formula" rather than
//  an independently rederived physical target: a self-consistent error
//  inside those two helpers -- one that still preserved the Lambertian
//  rho = 1 identity, which IS independent -- would have passed.
//
//  These read `SheenDirectionalAlbedo` and nothing else, so the only
//  shared surface left between the model and its oracle is the baked
//  table, which has its own independent test
//  (tests/SheenDirectionalAlbedoTest.cpp, brute-force spot checks
//  against CharlieSheen.h).
// ============================================================

//! `1 - m*Ehat(alpha, cosTheta)`, Ehat = min(E, 1).
static double FuzzTransmit( double alpha, double m, double cosTheta )
{
	const double eHat = std::min( 1.0, (double)SheenDirectionalAlbedo::E( alpha, cosTheta ) );
	return 1.0 - std::min( 1.0, m * eHat );
}

//! `1 - m*EhatMean(alpha)`.
static double FuzzTransmitMean( double alpha, double m )
{
	return 1.0 - std::min( 1.0, m * (double)SheenDirectionalAlbedo::EHatMean( alpha ) );
}

//! `max(1, E(v), E(l))` -- the symmetric normaliser, re-derived.
static double FuzzNormaliser( double alpha, double cosV, double cosL )
{
	return std::max( 1.0, std::max( (double)SheenDirectionalAlbedo::E( alpha, cosV ),
	                                (double)SheenDirectionalAlbedo::E( alpha, cosL ) ) );
}

// ============================================================
//  THE CLOSED-FORM DIRECTIONAL ALBEDO of a WHITE Lambertian fabric.
//
//  docs/CLOTH_FABRIC_DESIGN.md 9.9 gate 3 claims the Lambertian rows are
//  cross-checked against a closed form, and until now they were not --
//  the shipped assertion was the flat 5 % posture band alone, and the
//  "<= 0.002" in the doc came from a cross-check done by hand during
//  development and never committed (M5 review, 2026-09-03).  This is
//  that check, implemented.
//
//      rho(v) = INT_H D(a,n.h) V(a,n.l,n.v) / N(v,l) * (n.l) dl
//             + (1 - m*Ehat(v)) / (1 - m*EhatMean)
//               * 2 INT_0^1 (1 - m*Ehat(mu_l)) mu_l dmu_l
//
//  The second line is `f_base = 1/pi` folded through the product
//  scaling; the first is the sheen lobe divided by the symmetric
//  normaliser.
//
//  INDEPENDENCE.  This calls `CharlieSheen` (the lobe definition) and
//  `SheenDirectionalAlbedo` (the baked table) and re-derives the
//  product-form algebra locally.  It does NOT call FabricBRDF, so a
//  self-consistent error inside `ComputeTerms` / `BaseScaling` /
//  `SheenNormaliser` cannot appear on both sides of the comparison.
//  It is a genuinely different route to the same number: a deterministic
//  quadrature of the model against the furnace's Monte-Carlo sampling of
//  the implementation.
// ============================================================

static double LambertianFabricRhoClosedForm( double alpha, double m, double muV )
{
	// Deterministic 2-D midpoint quadrature.  1024 x 512 puts the
	// quadrature error two orders below the furnace's MC noise, which is
	// what sets the tolerance below.
	static const int kNMu  = 1024;
	static const int kNPhi = 512;

	const double sinV = std::sqrt( std::max( 0.0, 1.0 - muV * muV ) );
	const double dmu  = 1.0 / (double)kNMu;
	const double dphi = 2.0 * PI / (double)kNPhi;

	double sheen = 0.0;
	for( int j = 0; j < kNMu; ++j )
	{
		const double muL = ( j + 0.5 ) * dmu;
		const double V = CharlieSheen::V( alpha, muL, muV );
		if( V <= 0 ) continue;
		const double invN = 1.0 / FuzzNormaliser( alpha, muV, muL );
		const double sinL = std::sqrt( std::max( 0.0, 1.0 - muL * muL ) );

		double row = 0.0;
		for( int k = 0; k < kNPhi; ++k )
		{
			const double phi = ( k + 0.5 ) * dphi;
			const double hx = sinL * std::cos( phi ) + sinV;
			const double hy = sinL * std::sin( phi );
			const double hz = muL + muV;
			const double hlen = std::sqrt( hx * hx + hy * hy + hz * hz );
			if( hlen < 1e-12 ) continue;
			row += CharlieSheen::D( alpha, hz / hlen );
		}
		sheen += row * V * muL * invN;
	}
	sheen *= dmu * dphi;

	// 2 INT (1 - m*Ehat(mu)) mu dmu, over the runtime's own interpolant
	// (including its cosTheta floor), which is what the material
	// integrates.
	double baseInner = 0.0;
	for( int j = 0; j < kNMu; ++j ) {
		const double muL = ( j + 0.5 ) * dmu;
		baseInner += 2.0 * FuzzTransmit( alpha, m, muL ) * muL * dmu;
	}

	return sheen + FuzzTransmit( alpha, m, muV ) / FuzzTransmitMean( alpha, m ) * baseInner;
}

static double SubstrateAlbedoFuzzWeighted(
	ISPF& spf,
	double incomingThetaRad,
	double alpha,
	double m,
	double weaveAngle )
{
	RayIntersectionGeometric ri = MakeIntersection( incomingThetaRad );

	// THE WEAVE ROTATION MUST BE APPLIED HERE TOO, and this is not a
	// detail: FabricSPF hands the substrate a frame rotated by
	// `weave_rotation` (9.5), so W(v) is a property of the substrate IN
	// THAT FRAME.  For an ANISOTROPIC substrate the two differ
	// materially -- the view sits in the x-z plane, so rotating the
	// tangent frame by 45 deg changes which of alphax / alphay the lobe
	// presents along the view azimuth.  Measured: omitting this made
	// the aniso rows miss by more than 0.01 at low sheen alpha while
	// every isotropic row passed, which is exactly the signature of a
	// prediction evaluated in the wrong frame.  Same helper the
	// material uses, so the two cannot drift.
	ri.onb = MicrofacetUtils::RotateTangent( ri.onb, weaveAngle );
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	const Vector3 normal = ri.onb.w();
	double sum = 0;

	for( int i = 0; i < FURNACE_SAMPLES; ++i )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );

		for( unsigned int j = 0; j < scattered.Count(); ++j )
		{
			const ScatteredRay& scat = scattered[j];
			const Vector3 wo = Vector3Ops::Normalize( scat.ray.Dir() );
			const double cosO = Vector3Ops::Dot( wo, normal );
			if( cosO <= 0 ) continue;

			const double kMax = ColorMath::MaxValue( scat.kray );
			if( kMax >= 0 && kMax < 1e6 ) {
				// Re-derived here, NOT FabricBRDF's own helper -- see
				// the block comment above.
				sum += kMax * FuzzTransmit( alpha, m, cosO );
			}
		}
	}
	return sum / (double)FURNACE_SAMPLES;
}

// ============================================================
//  Pretty printing + pass/fail accumulation
// ============================================================

struct ConfigReport
{
	std::string  name;
	AuditPosture posture;
	double       tolerance;			// 1.0 ± tolerance is the pass band (kPosturePass only)
	double       albedo[NUM_THETA];	// per-incident-angle directional albedo
	double       reject[NUM_THETA] = { 0.0, 0.0, 0.0, 0.0 };	// fraction of Scatter draws that yielded no usable ray
	bool         passed;			// only meaningful when posture == kPosturePass
	std::string  note;
	// kPostureMatchesPrediction only: the analytic prediction to check
	// r.albedo[i] against, and the absolute tolerance around it.  Left at
	// their default (hasPrediction = false) for every other posture.
	bool         hasPrediction = false;
	double       predicted[NUM_THETA] = { 0.0, 0.0, 0.0, 0.0 };
	double       predictionEps = 0.0;
};

static void Run( ConfigReport& r, ISPF& spf )
{
	r.passed = true;
	for( int i = 0; i < NUM_THETA; ++i )
	{
		const double rad = THETA_DEG[i] * PI / 180.0;
		r.albedo[i] = DirectionalAlbedo( spf, rad, &r.reject[i] );

		if( r.posture == kPosturePass )
		{
			if( r.albedo[i] > 1.0 + r.tolerance ) {
				r.passed = false;
				if( r.note.empty() ) r.note = "GAIN (regression)";
			}
			else if( r.albedo[i] < 1.0 - r.tolerance ) {
				r.passed = false;
				if( r.note.empty() ) r.note = "energy loss (regression)";
			}
		}
		else if( r.posture == kPostureBounded )
		{
			// Energy-bounded: must be in [0, 1 + tol].  Catches the
			// pre-Imageworks Charlie blow-up (ρ → 8+ at grazing) but
			// allows the new bounded sheen to report ρ < 1 legitimately.
			if( r.albedo[i] > 1.0 + r.tolerance ) {
				r.passed = false;
				if( r.note.empty() ) r.note = "GAIN (energy violation)";
			}
			else if( r.albedo[i] < 0.0 ) {
				r.passed = false;
				if( r.note.empty() ) r.note = "negative albedo (regression)";
			}
		}
		else if( r.posture == kPostureMatchesPrediction )
		{
			// Enforces the actual analytic number, not just an energy
			// band -- kPostureBounded's [0, 1+tol] would silently pass a
			// regression that makes `tau` a no-op (ρ stays ~1.0 for every
			// tau) or one that blows the deficit past its analytic value
			// (e.g. ρ collapsing toward 0.5).  |measured - predicted| ≤
			// predictionEps is the actual regression gate.
			const double diff = r.albedo[i] - r.predicted[i];
			if( std::fabs( diff ) > r.predictionEps ) {
				r.passed = false;
				if( r.note.empty() ) {
					std::ostringstream oss;
					oss << "prediction mismatch at theta=" << (int)THETA_DEG[i]
					    << ": measured " << r.albedo[i] << " vs predicted " << r.predicted[i]
					    << " (eps " << r.predictionEps << ")";
					r.note = oss.str();
				}
			}
		}
		else	// kPostureKnownFailure: record numbers, don't fail
		{
			r.passed = true;
		}
	}
}

static void PrintReport( const std::vector<ConfigReport>& rs )
{
	std::cout << "\n";
	std::cout << "================================================================\n";
	std::cout << "  Landing 6 — Layered Material White-Furnace Audit\n";
	std::cout << "================================================================\n";
	std::cout << "  Samples per (config, angle): " << FURNACE_SAMPLES << "\n";
	std::cout << "  Per-config tolerance + posture documented inline.\n";
	std::cout << "\n";
	std::cout << std::left << std::setw( 42 ) << "Configuration";
	for( int i = 0; i < NUM_THETA; ++i ) {
		std::cout << "  theta=" << std::setw( 3 ) << (int)THETA_DEG[i];
	}
	std::cout << "  Status\n";
	std::cout << "----------------------------------------------------------------------------\n";

	for( const ConfigReport& r : rs ) {
		std::cout << std::left << std::setw( 42 ) << r.name;
		std::cout << std::fixed << std::setprecision( 4 );
		for( int i = 0; i < NUM_THETA; ++i ) {
			std::cout << "  " << std::setw( 8 ) << r.albedo[i];
		}
		// Per-angle rejection rate, printed whenever ANY angle rejected
		// a draw.  rho is now normalised by the full sample count, so a
		// config that starts rejecting shows up as a drop in rho -- this
		// line is what tells you the drop is a SAMPLER change rather
		// than a material one.
		{
			double maxReject = 0;
			for( int i = 0; i < NUM_THETA; ++i ) maxReject = r_max( maxReject, r.reject[i] );
			if( maxReject > 1e-9 ) {
				std::cout << "  rej<=" << std::setprecision( 3 ) << maxReject << std::setprecision( 4 );
			} else {
				std::cout << "          ";
			}
		}
		if( r.posture == kPosturePass || r.posture == kPostureBounded || r.posture == kPostureMatchesPrediction ) {
			std::cout << "  " << ( r.passed ? "PASS" : "FAIL" );
			if( !r.note.empty() ) std::cout << " (" << r.note << ")";
		} else {
			std::cout << "  KNOWN-FAIL";
			if( !r.note.empty() ) std::cout << " (" << r.note << ")";
		}
		std::cout << "\n";
	}
	std::cout << "============================================================================\n";
}

// ============================================================
//  Independent white-weave energy floor -- REVIEW_P2R3.md P1-1/P1-2.
//
//  WHY THIS EXISTS.  Every other weave gate (locked curve, reciprocity,
//  the pdf hemisphere integral, `hemisphericalAlbedo`'s own 20% band)
//  either compares `value()` against ITSELF at a different time, or
//  against a closed form (`hemisphericalAlbedo`) that is DERIVED
//  assuming the BRDF's own normalisers are exact -- so none of them
//  would have caught the `C_v = 2(1+k_d)` defect this session found:
//  a normaliser that was a genuine, self-consistent LOOSE BOUND rather
//  than the exact integral, silently halving the volume lobe.  The
//  locked curve pinned the buggy number; the hemispherical-albedo band
//  compared the buggy `value()` against a closed form that cancels
//  `C_v` symbolically and so agreed with it regardless.
//
//  This block re-derives `ComputeThreadTerms` FROM SCRATCH -- plain
//  doubles, no RISE material classes -- reusing only the shared,
//  separately-regression-tested `FibreLobeMath.h` primitives (`Mp`,
//  `TrimmedLogistic`, `FrDielectric`, `SafeASin`, `SafeSqrt`, `Clamp`),
//  and its own independently-written closed form for the volume
//  normaliser `C_v`.  It never calls `WeaveBRDF::value()`,
//  `ComputeThreadTerms` or any other WeaveBRDF/WeaveSPF method, so a
//  bug in THOSE would show up as a mismatch here rather than being
//  invisible because both sides share the same buggy arithmetic.
// ============================================================

namespace WeaveIndependentCheck
{
	using namespace RISE::FibreLobeMath;

	struct V3 { double x, y, z; };
	static inline V3    Sub( const V3& a, const V3& b ) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
	static inline V3    Add( const V3& a, const V3& b ) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
	static inline V3    Mul( const V3& a, double s )    { return V3{ a.x * s, a.y * s, a.z * s }; }
	static inline double Dot( const V3& a, const V3& b ){ return a.x * b.x + a.y * b.y + a.z * b.z; }
	static inline V3    Cross( const V3& a, const V3& b ){ return V3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x }; }
	static inline double Mag( const V3& a )             { return std::sqrt( Dot( a, a ) ); }

	static const double kQPI    = 3.14159265358979323846;
	static const double kQTwoPI = 2.0 * kQPI;

	static inline double QWrapPi( double x )
	{
		x = std::fmod( x + kQPI, kQTwoPI );
		if( x < 0 ) x += kQTwoPI;
		return x - kQPI;
	}

	struct Thread { V3 tangent; double eta, vSurf, vVol, s, kd; };
	struct FF     { V3 t, nk, bk; bool valid; };
	struct DA     { double sinTheta, cosTheta, phi, cosPhi; };

	static FF MakeFrame( const V3& tangent, const V3& n )
	{
		FF f; f.t = tangent; f.valid = false;
		const double sinAlpha = Dot( n, tangent );
		const V3     perp     = Sub( n, Mul( tangent, sinAlpha ) );
		const double len      = Mag( perp );
		if( !( len > 1e-9 ) ) { f.nk = n; f.bk = n; return f; }
		f.nk = Mul( perp, 1.0 / len );
		f.bk = Cross( tangent, f.nk );
		f.valid = true;
		return f;
	}

	static DA Project( const FF& f, const V3& w )
	{
		DA a;
		a.sinTheta = Clamp( Dot( w, f.t ), -1.0, 1.0 );
		a.cosTheta = SafeSqrt( 1.0 - a.sinTheta * a.sinTheta );
		const double e1 = Dot( w, f.nk ), e2 = Dot( w, f.bk );
		const double h  = std::sqrt( e1 * e1 + e2 * e2 );
		a.phi    = std::atan2( e2, e1 );
		a.cosPhi = ( h > 1e-12 ) ? ( e1 / h ) : 1.0;
		return a;
	}

	//! `C_v`'s k_d term, re-derived independently (matches the closed
	//! form documented at WeaveBRDF.cpp's `VolumeKdIntegral`, but is a
	//! separate hand-written copy, not a call to it).
	static double VolumeKdIntegral( double c )
	{
		c = Clamp( c, 0.0, 1.0 );
		const double s2 = 1.0 - c * c;
		if( !( s2 > 1e-9 ) ) return 2.0 * ( 4.0 - kQPI );		// c -> 1 limit
		const double t = std::atan( std::sqrt( ( 1.0 - c ) / ( 1.0 + c ) ) );
		return 2.0 * ( ( 2.0 - c * kQPI ) + c * c * 4.0 / std::sqrt( s2 ) * t );
	}

	struct TT { bool valid; double surface, volume; };

	static TT ComputeTerms( const Thread& t, const V3& n, const V3& wi, const V3& wo )
	{
		TT out{ false, 0.0, 0.0 };
		const FF f = MakeFrame( t.tangent, n );
		if( !f.valid ) return out;
		const DA a = Project( f, wi ), b = Project( f, wo );
		const double thetaI = SafeASin( a.sinTheta ), thetaO = SafeASin( b.sinTheta );
		const double thetaD = ( thetaI - thetaO ) * 0.5;
		const double phiD   = QWrapPi( a.phi - b.phi );
		const double cosGamma = std::cos( thetaD ) * std::cos( phiD * 0.5 );
		const double F = FrDielectric( cosGamma, t.eta );

		const double mI = ( a.cosPhi > 0 ) ? a.cosPhi : 0.0;
		const double mO = ( b.cosPhi > 0 ) ? b.cosPhi : 0.0;
		const double sigma = 20.0 * kQPI / 180.0;
		const double u = std::exp( -( phiD * phiD ) / ( 2.0 * sigma * sigma ) );
		const double mask = ( 1.0 - u ) * mI * mO + u * std::min( mI, mO );
		if( !( mask > 0.0 ) ) return out;

		// The surface lobe's untrimmed [-pi,pi] azimuthal normaliser is
		// used here rather than `value()`'s visible-interval trim
		// (`AzimuthalTrimBoost`) -- a <= ~8% correction on a lobe that
		// carries a few percent of a white weave's total energy (P2-2),
		// well inside this check's tolerance.
		out.surface = F * Mp( a.cosTheta, b.cosTheta, a.sinTheta, b.sinTheta, t.vSurf )
		            * TrimmedLogistic( phiD, t.s, -kQPI, kQPI ) * mask;

		const double denom   = std::max( 1e-3, a.cosTheta + b.cosTheta );
		const double bracket = ( 1.0 - t.kd ) * Mp( a.cosTheta, b.cosTheta, a.sinTheta, b.sinTheta, t.vVol ) + t.kd;
		const double jd      = std::sqrt( VolumeKdIntegral( a.cosTheta ) * VolumeKdIntegral( b.cosTheta ) );
		const double Cv      = ( 1.0 - t.kd ) + jd * t.kd;
		out.volume = ( 1.0 - F ) * bracket / ( denom * Cv ) * mask;

		out.valid = true;
		return out;
	}

	//! Bihemispherical directional albedo of ONE thread family at a
	//! fixed view, by brute-force quadrature.
	static double IntegrateFamily( const Thread& t, const V3& n, const V3& wo, int NT, int NP )
	{
		double acc = 0.0;
		for( int i = 0; i < NT; ++i )
		{
			const double th  = ( i + 0.5 ) * ( kQPI * 0.5 ) / NT;
			const double ct  = std::cos( th ), st = std::sin( th );
			const double dth = ( kQPI * 0.5 ) / NT;
			for( int j = 0; j < NP; ++j )
			{
				const double ph  = ( j + 0.5 ) * kQTwoPI / NP;
				const double dph = kQTwoPI / NP;
				const V3 wi{ st * std::cos( ph ), st * std::sin( ph ), ct };
				const TT tt = ComputeTerms( t, n, wi, wo );
				const double w = ct * st * dth * dph;		// dw = cos(th) sin(th) dth dph
				acc += ( tt.surface + tt.volume ) * w;
			}
		}
		return acc;
	}

	//! The independent prediction for a WHITE (both dyes forced to 1)
	//! two-family weave's directional albedo at view latitude
	//! `thetaViewDeg`, view azimuth 0, zero `weave_rotation` and a
	//! STATED constant `aWarp` (the fixture forces coverage to the
	//! draft's own mean, so this prediction does not have to guess
	//! where in the cell the harness's ray lands).
	static double PredictWhiteRho( const RISE::Implementation::WeavePreset& P, double aWarp, double thetaViewDeg )
	{
		const V3 n{ 0, 0, 1 };
		const V3 warpU{ 1, 0, 0 };
		const V3 weftU{ 0, 1, 0 };
		const Thread W{
			Add( Mul( warpU, std::cos( (double)P.warp.tilt ) ), Mul( n, std::sin( (double)P.warp.tilt ) ) ),
			(double)P.warp.ior, (double)P.warp.width * (double)P.warp.width,
			4.0 * (double)P.warp.width * (double)P.warp.width,
			(double)P.warp.azimuth * 0.5513288954217921, (double)P.warp.kd };
		const Thread F{
			Add( Mul( weftU, std::cos( (double)P.weft.tilt ) ), Mul( n, std::sin( (double)P.weft.tilt ) ) ),
			(double)P.weft.ior, (double)P.weft.width * (double)P.weft.width,
			4.0 * (double)P.weft.width * (double)P.weft.width,
			(double)P.weft.azimuth * 0.5513288954217921, (double)P.weft.kd };

		const double th = thetaViewDeg * kQPI / 180.0;
		const V3 wo{ std::sin( th ), 0, std::cos( th ) };
		const double rw = IntegrateFamily( W, n, wo, 220, 440 );
		const double rf = IntegrateFamily( F, n, wo, 220, 440 );
		// `available = 1 - gap` is a direction-independent energy factor
		// (WeaveBRDF.h section "THE GAP IS AN ENERGY FACTOR"), applied
		// here too -- linen is the one shipped preset with a real gap
		// (0.10) and was the tell when this was first omitted: its
		// measured/predicted ratio sat at ~0.90, exactly `1 - gap`.
		const double available = 1.0 - (double)P.gap;
		return available * ( aWarp * rw + ( 1.0 - aWarp ) * rf );
	}
}

// ============================================================
//  main: build each configuration, run, report, exit 0/1
// ============================================================

int main()
{
	GlobalLog();

	g_stubObject = new StubObject();
	g_stubObject->addref();

	// All-unity painters drive the worst-case furnace test: any
	// loss is the material's, not the input's.
	UniformColorPainter* one      = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) );  one->addref();
	UniformColorPainter* zero     = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) );  zero->addref();
	UniformColorPainter* iorPnt   = new UniformColorPainter( RISEPel( 1.5, 1.5, 1.5 ) );  iorPnt->addref();
	UniformScalarPainter* zeroSc  = new UniformScalarPainter( 0.0 );  zeroSc->addref();
	UniformScalarPainter* iorSc   = new UniformScalarPainter( 1.5 );  iorSc->addref();

	// Finding D verification: a coloured / low-metallic / dominant-
	// diffuse PBR base, similar to what the importer's "near-black
	// clearcoat over PBR" warning at GLTFSceneImporter.cpp:851
	// describes.  baseColor red, metallic = 0 → rd = (0.8, 0.2, 0.2),
	// rs = F0 = 0.04 dielectric.  If the warning still applies, this
	// configuration's GGX/GGX-PBR composite should report
	// significantly less than 1.0 reflectance per channel.
	UniformColorPainter* redDiff   = new UniformColorPainter( RISEPel( 0.8, 0.2, 0.2 ) );  redDiff->addref();
	UniformColorPainter* dielF0    = new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) );  dielF0->addref();

	// Roughness ≈ 0.4 (α = 0.16) puts GGX in a regime where
	// Kulla-Conty's multi-scatter compensation matters but doesn't
	// dominate.  Same value is used by the existing
	// scenes/Tests/Materials/ggx_white_furnace.RISEscene comparison
	// surface, so any L6 finding is comparable to the established
	// single-material baseline.
	UniformColorPainter* alpha    = new UniformColorPainter( RISEPel( 0.16, 0.16, 0.16 ) );  alpha->addref();
	UniformScalarPainter* alphaSc = new UniformScalarPainter( 0.16 );  alphaSc->addref();

	// Sheen roughness in the middle of the legal [0, 1] range.
	// Charlie distribution's energy loss at grazing depends on
	// roughness; 0.5 is broadly representative of fabric assets.
	UniformColorPainter* sheenR   = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  sheenR->addref();
	UniformScalarPainter* sheenRSc = new UniformScalarPainter( 0.5 );  sheenRSc->addref();

	// ---------- Build SPFs ----------

	LambertianSPF* lambertian = new LambertianSPF( *one );  lambertian->addref();

	// GGX-PBR (white inputs, schlick_f0 — glTF-MR convention).
	// rd = baseColor (unity here) acts as albedo; rs = baseColor
	// acts as F0 directly under schlick_f0; (1 - max(F0)) is
	// applied inside the BSDF for the diffuse weight.
	GGXSPF* ggxPBR = new GGXSPF(
		*one, *one, *alphaSc, *alphaSc, *iorSc, *zeroSc, eFresnelSchlickF0 );
	ggxPBR->addref();

	// Sheen alone.
	SheenSPF* sheen = new SheenSPF( *one, *sheenRSc );  sheen->addref();

	// Dielectric "clear coat" with full transmission (tau = 1)
	// and zero in-volume scattering, IOR 1.5.  tau/ior/scattering
	// are physical scalars carried by `IScalarPainter` — they no
	// longer route through `IPainter` and the JH spectral uplift.
	UniformScalarPainter* sOne  = new UniformScalarPainter( 1.0 );  sOne->addref();
	UniformScalarPainter* sZero = new UniformScalarPainter( 0.0 );  sZero->addref();
	UniformScalarPainter* sIor  = new UniformScalarPainter( 1.5 );  sIor->addref();
	DielectricSPF* dielectric = new DielectricSPF( *sOne, *sIor, *sZero, /*hg*/ false );
	dielectric->addref();

	// polished_material wetness-recipe furnace probe (docs/WETNESS_COAT_DESIGN.md
	// §6.2/§13 Phase-1 exit gate; §6.9's collected caveat list, item 5).  White
	// substrate (Rd = 1, via `one` above), water IOR 1.33, `tau` as the coverage
	// mask at three points: full coverage (1.0), the dip's worst region (0.5),
	// and the recipe's own pooled value (0.9, §6.5's rain-wet-cobbles worked
	// example: "pooling saturates damp_raw, so tau reaches 0.90" in the pooled
	// joints).  `scattering` is fixed at the recipe's pooled end (~200000,
	// §6.5's cobble_gloss `mix` expression, whose pooled end "runs to ≈180 000
	// ... effectively a mirror") for all three so tau is the only variable —
	// isolates the §6.2 coverage-mask deficit from any lobe-width effect
	// (scattering < 1e6 keeps PolishedSPF's Phong coat lobe non-delta, matching
	// the isDelta branch that PolishedSPF::Scatter takes at this value).
	UniformScalarPainter* sTau1_0    = new UniformScalarPainter( 1.0 );     sTau1_0->addref();
	UniformScalarPainter* sTau0_5    = new UniformScalarPainter( 0.5 );     sTau0_5->addref();
	UniformScalarPainter* sTau0_9    = new UniformScalarPainter( 0.9 );     sTau0_9->addref();
	UniformScalarPainter* sIor133    = new UniformScalarPainter( 1.33 );    sIor133->addref();
	UniformScalarPainter* sScat200k  = new UniformScalarPainter( 200000.0 ); sScat200k->addref();

	PolishedSPF* polishedTau1_0 = new PolishedSPF( *one, *sTau1_0, *sIor133, *sScat200k, /*hg*/ false );
	polishedTau1_0->addref();
	PolishedSPF* polishedTau0_5 = new PolishedSPF( *one, *sTau0_5, *sIor133, *sScat200k, /*hg*/ false );
	polishedTau0_5->addref();
	PolishedSPF* polishedTau0_9 = new PolishedSPF( *one, *sTau0_9, *sIor133, *sScat200k, /*hg*/ false );
	polishedTau0_9->addref();

	// GGX-only top layer for clearcoat-style composite.
	GGXSPF* ggxOnly = new GGXSPF(
		*zero, *one, *alphaSc, *alphaSc, *iorSc, *zeroSc, eFresnelSchlickF0 );
	ggxOnly->addref();

	// Finding D: coloured + diffuse-dominant GGX-PBR base.
	// Mirrors what AddPBRMetallicRoughnessMaterial would build for
	// a red baseColor (0.8, 0.2, 0.2) with metallic = 0 and
	// roughness ≈ 0.4.  rd = baseColor (because metallic=0 leaves
	// the entire baseColor as diffuse weight); rs = F0 = 0.04.
	GGXSPF* ggxRedDiff = new GGXSPF(
		*redDiff, *dielF0, *alphaSc, *alphaSc, *iorSc, *zeroSc, eFresnelSchlickF0 );
	ggxRedDiff->addref();

	// Finding D: clearcoat with F0 = 0.04 (standard glTF clearcoat).
	GGXSPF* clearcoatDiel = new GGXSPF(
		*zero, *dielF0, *alphaSc, *alphaSc, *iorSc, *zeroSc, eFresnelSchlickF0 );
	clearcoatDiel->addref();

	// Composite layers.  Recursion budgets match the scene-language
	// composite_material chunk's defaults; thickness=0.0 keeps the
	// inter-layer translucent traversal neutral; extinction=zero so
	// the audit reflects pure layer-composition behaviour, not Beer-
	// Lambert absorption between layers.  `extinction` is an
	// IScalarPainter slot (a physical Beer-Lambert coefficient, never
	// colourspace-converted), hence `zeroSc` rather than the `zero`
	// COLOUR painter -- both are 0, so every number below is unmoved.
	const unsigned int kMaxRecur          = 4;
	const unsigned int kMaxReflectRecur   = 2;
	const unsigned int kMaxRefractRecur   = 2;
	const unsigned int kMaxDiffuseRecur   = 2;
	const unsigned int kMaxTranslucent    = 2;
	const Scalar       kThickness         = 0.0;	// zero-thickness layer interface

	CompositeSPF* compDielLamb = new CompositeSPF(
		*dielectric, *lambertian,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zeroSc );
	compDielLamb->addref();

	CompositeSPF* compGgxLamb = new CompositeSPF(
		*ggxOnly, *lambertian,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zeroSc );
	compGgxLamb->addref();

	CompositeSPF* compGgxPbr = new CompositeSPF(
		*ggxOnly, *ggxPBR,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zeroSc );
	compGgxPbr->addref();

	CompositeSPF* compSheenPbr = new CompositeSPF(
		*sheen, *ggxPBR,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zeroSc );
	compSheenPbr->addref();

	// Finding D: clearcoat (GGX with F0=0.04) over a coloured /
	// diffuse-dominant PBR base.  This is the configuration the
	// importer's "Phase 4 imports the base PBR only and skips the
	// clearcoat layer" warning at GLTFSceneImporter.cpp:851 was
	// written to describe.  If the white-input #5 audit was masking
	// a coloured-base failure, this config will show the same
	// catastrophic loss the warning predicts.
	CompositeSPF* compClearcoatRedPbr = new CompositeSPF(
		*clearcoatDiel, *ggxRedDiff,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zeroSc );
	compClearcoatRedPbr->addref();

	// ---------- coated_material (docs/WETNESS_COAT_DESIGN.md Phase 2) ----------
	//
	// 7.6 makes this test Phase 2's exit gate: "Phase 2 adds
	// coated_material configurations mirroring the known-failing
	// composite ones (3 and 7) -- same substrates, same coat parameters
	// -- and they must land in kPosturePass.  That is a direct, numeric,
	// apples-to-apples improvement claim against a shipped baseline."
	//
	// Config 3 is `dielectric (ior 1.5) / white Lambertian`, a
	// KNOWN FAILURE on CompositeSPF's random walk.  Config 11 below is
	// its coated twin: same white Lambertian substrate, same 1.5 coat
	// IOR, full coverage.  Config 7 is `clearcoat (F0 = 0.04, alpha
	// 0.16) / GGX-PBR base`; configs 14 and 15 are its coated twins on
	// white and on the same coloured base respectively.
	//
	// Configs 11-14 are the HIGH-SUBSTRATE-ALBEDO cases the exit gate
	// names -- the regime that fails when 7.4's interreflection
	// compensation is missing or under-weighted.  Config 16 proves that
	// by MEASUREMENT rather than assertion: same material as 12 with the
	// compensation switched off, checked against the analytic
	// Weidlich-Wilkie-single-bounce curve.

	// Substrate materials.  The coated triad consumes a MATERIAL (7.2's
	// `base` slot), not a bare SPF, because it needs the substrate's
	// BSDF (for the closed-form layer value and its directional albedo)
	// as well as its SPF.
	LambertianMaterial* whiteLambMat = new LambertianMaterial( *one );  whiteLambMat->addref();
	// The GGX substrates mirror config 7's base SHAPE exactly: a
	// DIFFUSE-DOMINANT metallic-roughness material (F0 = 0.04
	// dielectric, roughness 0.4).  `whiteGgxMat` is the white-input
	// twin, `redGgxMat` is config 7's own (0.8, 0.2, 0.2) base.
	// Deliberately NOT F0 = 1: a mirror substrate is a different
	// material class from the one config 7 describes, and it is also
	// the regime where 7.4's WW-plus-compensation core is weakest --
	// see config 14's note.
	GGXMaterial* whiteGgxMat = new GGXMaterial(
		*one, *dielF0, *alphaSc, *alphaSc, *iorSc, *zeroSc, eFresnelSchlickF0 );
	whiteGgxMat->addref();
	GGXMaterial* redGgxMat = new GGXMaterial(
		*redDiff, *dielF0, *alphaSc, *alphaSc, *iorSc, *zeroSc, eFresnelSchlickF0 );
	redGgxMat->addref();

	// Coat parameter painters.  Water is 7.2's 1.33 / roughness
	// 0.01-0.05; the varnish/clearcoat case is 1.5.  Coat absorption and
	// thickness stay at their neutral defaults so these configurations
	// isolate the LAYERING, not Beer-Lambert absorption -- exactly the
	// discipline the composite configs use with `extinction = zero`.
	UniformScalarPainter* sCoatFull  = new UniformScalarPainter( 1.0 );   sCoatFull->addref();
	UniformScalarPainter* sCoatHalf  = new UniformScalarPainter( 0.5 );   sCoatHalf->addref();
	UniformScalarPainter* sCoatRough = new UniformScalarPainter( 0.02 );  sCoatRough->addref();

	CoatedMaterial* coatedVarnishWhiteLamb = new CoatedMaterial(
		*whiteLambMat, *sCoatFull, *sIor, *sCoatRough, *sZero, *sZero, *one );
	coatedVarnishWhiteLamb->addref();

	CoatedMaterial* coatedWaterWhiteLamb = new CoatedMaterial(
		*whiteLambMat, *sCoatFull, *sIor133, *sCoatRough, *sZero, *sZero, *one );
	coatedWaterWhiteLamb->addref();

	CoatedMaterial* coatedWaterHalfCover = new CoatedMaterial(
		*whiteLambMat, *sCoatHalf, *sIor133, *sCoatRough, *sZero, *sZero, *one );
	coatedWaterHalfCover->addref();

	CoatedMaterial* coatedClearcoatWhiteGgx = new CoatedMaterial(
		*whiteGgxMat, *sCoatFull, *sIor, *alphaSc, *sZero, *sZero, *one );
	coatedClearcoatWhiteGgx->addref();

	CoatedMaterial* coatedClearcoatRedGgx = new CoatedMaterial(
		*redGgxMat, *sCoatFull, *sIor, *alphaSc, *sZero, *sZero, *one );
	coatedClearcoatRedGgx->addref();

	// RED-PROOF twin of config 12: identical in every respect except
	// that 7.4's required interreflection compensation is DISABLED,
	// reducing the layer to plain Weidlich-Wilkie single bounce.  The
	// flag is not reachable from the scene language or RISE_API.
	CoatedMaterial* coatedWaterNoRecycle = new CoatedMaterial(
		*whiteLambMat, *sCoatFull, *sIor133, *sCoatRough, *sZero, *sZero, *one,
		/*recyclingCompensation*/ false );
	coatedWaterNoRecycle->addref();

	// ---------- Gate 4: config 7's downward-ray probe, run against
	//            config 6's top layer (docs/CLOTH_FABRIC_DESIGN.md 9.9) ----------
	//
	// Run BEFORE the config list so config 6's note can carry the
	// measured answer rather than a remembered one.
	const double kSheenDownAt0  = DownwardRayFraction( *sheen,         0.0,                    20000 );
	const double kSheenDownAt80 = DownwardRayFraction( *sheen,         80.0 * PI / 180.0,      20000 );
	const double kCoatDownAt0   = DownwardRayFraction( *clearcoatDiel, 0.0,                    20000 );
	const double kCoatDownAt80  = DownwardRayFraction( *clearcoatDiel, 80.0 * PI / 180.0,      20000 );

	std::cout << "\n";
	std::cout << "  Gate 4 -- downward-ray probe (fraction of Scatter draws emitting a\n";
	std::cout << "  ray with cos(wo, n) < 0, i.e. one CompositeSPF's walk could carry\n";
	std::cout << "  into the substrate):\n";
	std::cout << "    config 6 top layer (SheenSPF, Charlie):      theta=0 " << kSheenDownAt0
	          << "   theta=80 " << kSheenDownAt80 << "\n";
	std::cout << "    config 7 top layer (GGXSPF clearcoat, ref):  theta=0 " << kCoatDownAt0
	          << "   theta=80 " << kCoatDownAt80 << "\n";

	std::ostringstream config6Note;
	config6Note << "GATE 4 (docs/CLOTH_FABRIC_DESIGN.md 9.9), MEASURED: SheenSPF emits a downward ray "
	            << "in " << ( kSheenDownAt0 * 100.0 ) << " % of draws at theta=0 and "
	            << ( kSheenDownAt80 * 100.0 ) << " % at theta=80 (config 7's GGX top layer, the "
	            << "reference: " << ( kCoatDownAt0 * 100.0 ) << " % / " << ( kCoatDownAt80 * 100.0 )
	            << " %).  SheenSPF is reflection-only by construction (cosine-hemisphere about the "
	            << "ray-facing normal), so like config 7 the SUBSTRATE IS NEVER REACHED through the "
	            << "composite walk and this row is NOT 'sheen's dissipation amplified' -- it is the "
	            << "SAME structural defect config 7 documents.  fabric_material (configs 19+) is the "
	            << "shipped answer: it evaluates the combined closed form instead of walking.";

	// ---------- Configurations ----------

	std::vector<ConfigReport> reports;
	auto add = [&]( const std::string& name, AuditPosture posture, double tol, const char* note ) -> ConfigReport& {
		reports.emplace_back();
		reports.back().name      = name;
		reports.back().posture   = posture;
		reports.back().tolerance = tol;
		if( note ) reports.back().note = note;
		return reports.back();
	};

	// kPostureMatchesPrediction overload: `predicted` must point to
	// NUM_THETA values (one per THETA_DEG entry, same order); `eps` is the
	// absolute tolerance around each (ConfigReport::tolerance is unused by
	// this posture -- see Run()).  Kept as a separate overload rather than
	// default arguments on the one above so every pre-existing call site
	// (postures 0-7) is untouched.
	auto addPredicted = [&]( const std::string& name, const char* note,
	                          const double predicted[NUM_THETA], double eps ) -> ConfigReport& {
		ConfigReport& r = add( name, kPostureMatchesPrediction, 0.0, note );
		r.hasPrediction = true;
		r.predictionEps = eps;
		for( int i = 0; i < NUM_THETA; ++i ) r.predicted[i] = predicted[i];
		return r;
	};

	// 0. Lambertian alone — sanity baseline.  Must pass within MC noise; if
	//    this fails, the test methodology itself is broken and every other
	//    finding becomes unreliable.
	{ ConfigReport& r = add( "0. Lambertian alone (sanity)", kPosturePass, 0.01, 0 );
	  Run( r, *lambertian ); }

	// 1. GGX-PBR (schlick_f0, white inputs, alpha=0.16).  Has Kulla-Conty
	//    multi-scatter compensation which is known to slightly OVER-correct
	//    at moderate roughness — pbrt-v4 and Mitsuba use ~5% tolerance for
	//    this exact reason.  We match.
	{ ConfigReport& r = add( "1. GGX-PBR (baseline, Kulla-Conty)", kPosturePass, 0.05, 0 );
	  Run( r, *ggxPBR ); }

	// 2. Sheen alone (Imageworks production-friendly Charlie, L6 fix
	//    landed 2026-05).  Was kPostureKnownFailure pre-fix because the
	//    Ashikhmin/Neubelt closed-form V function blew up at grazing
	//    angles (ρ ≈ 8.7 at θ=80°).  Switched to Estevez & Kulla 2017
	//    "Production Friendly Microfacet Sheen BRDF" with a polynomial-
	//    fit Λ shadowing-masking term that bounds the BRDF to ≤ 1 by
	//    construction.  Now kPostureBounded: ρ may legitimately fall
	//    below 1 (Charlie sheen is single-scattering and dissipates
	//    energy at grazing) but must NOT exceed 1.  5% MC tolerance.
	{ ConfigReport& r = add( "2. Sheen alone (Imageworks-Charlie)", kPostureBounded, 0.05,
	    "Energy-bounded by Estevez/Kulla Λ; ρ < 1 expected at grazing" );
	  Run( r, *sheen ); }

	// 3. Composite: dielectric over Lambertian.
	//
	//    2026-09-01 RE-MEASURE.  The original L6 note on this row said the
	//    "recursion budget kills below-layer diffuse".  That diagnosis was
	//    WRONG.  The real cause was an IOR-stack threading bug in
	//    CompositeSPF's random walk: the walk recursed with the ior_stack it
	//    was HANDED instead of each scattered ray's OWN stack, so the return
	//    trip arrived at the top interface with an OUTSIDE stack, DielectricSPF
	//    read it as "entering from outside", and BOTH of its lobes were then
	//    culled -- the interface emitted nothing at all.  EVERY gap-crossing
	//    path died inside the walk, which is why this row sat at exactly the
	//    bare-Fresnel reflectance and why composite_material's `extinction` and
	//    `thickness` were completely inert.  Fixed via the two-stack walk
	//    (CompositeSPF::EvalStack / GapStackBelowTop); guarded by
	//    tests/CompositeExtinctionTest.
	//
	//    Measured here: {0.0400, 0.0415, 0.0892, 0.3877} before the fix ->
	//    {0.3339, 0.3044, 0.3128, 0.5324} after, bit-identical across runs.
	//
	//    Still NOT energy-conserving, and the ORIGINAL note's mechanism is now
	//    the correct description of what remains: with max_recur=4 and per-type
	//    budgets of 2, the light that TIRs back down at the top interface (~56 %
	//    of the returning diffuse population) hits its reflection budget at
	//    steps=2 and is dropped.  That is a genuine finite-budget truncation,
	//    not a bug, so this row stays a documented deficit -- but as a
	//    PREDICTION check, not a free pass: kPostureKnownFailure would silently
	//    swallow both a regression back to 0.04 and an over-unity blow-up.
	//    eps = 0.03 is ~10x the MC noise on a 100k-sample mean here.
	static const double kPredDielLamb[NUM_THETA] = { 0.3339, 0.3044, 0.3128, 0.5324 };
	{ ConfigReport& r = addPredicted( "3. Dielectric / Lambertian",
	    "post-ior-stack-fix recovery locked in: predicted rho={0.3339,0.3044,0.3128,0.5324} "
	    "(pre-fix was {0.0400,0.0415,0.0892,0.3877}, eps 0.03); residual deficit is finite "
	    "recursion-budget truncation of the TIR population",
	    kPredDielLamb, 0.03 );
	  Run( r, *compDielLamb ); }

	// 4. Composite: GGX top over Lambertian (clearcoat-style).  Top
	//    layer is non-delta GGX so the random walk's outgoing rays carry
	//    measurable density and the energy survives.  Tolerance is 6%
	//    (slightly looser than the GGX-PBR baseline's 5%) — the random
	//    walk adds a small systematic bias on top of the Kulla-Conty
	//    over-correction at grazing.  Empirically caps at 5.1% across
	//    angles 0-80°; 6% leaves a thin regression-catching margin.
	{ ConfigReport& r = add( "4. GGX / Lambertian (clearcoat-style)", kPosturePass, 0.06, 0 );
	  Run( r, *compGgxLamb ); }

	// 5. Composite: GGX top over GGX-PBR base (clearcoat over PBR).
	//    Same 6% tolerance as #4.  The importer's Phase-5 warning
	//    describes a near-black failure on this combo; this audit shows
	//    the composite over GGX-PBR with WHITE inputs is actually fine
	//    within Kulla-Conty noise — the warning may be about a different
	//    parameter regime (e.g. coloured F0 with low metallic, where the
	//    diffuse/specular mix is more sensitive).  Worth re-investigating
	//    in a follow-up; doesn't gate L6.
	{ ConfigReport& r = add( "5. GGX / GGX-PBR (clearcoat over PBR)", kPosturePass, 0.06, 0 );
	  Run( r, *compGgxPbr ); }

	// 6. Composite: Sheen top over GGX-PBR base.
	//
	//    2026-09-02 RE-DIAGNOSIS (docs/CLOTH_FABRIC_DESIGN.md 9.9 gate 4).
	//    The previous note here said "the composite walk amplifies sheen's
	//    intrinsic energy dissipation".  That diagnosis was WRONG, and
	//    gate 4 asked for it to be re-measured with the same downward-ray
	//    probe that corrected config 7.  MEASURED (the probe runs above
	//    and writes its numbers into this row's note at runtime):
	//    SheenSPF emits a downward ray in 0 % of draws at BOTH theta = 0
	//    and theta = 80, identically to config 7's GGX top layer.
	//
	//    SheenSPF is reflection-only by construction -- it draws a
	//    cosine-hemisphere direction about the RAY-FACING normal
	//    (SheenSPF.cpp:73-74) and gates anything below the geometric
	//    horizon -- so it never hands CompositeSPF's walk a downward ray
	//    and THE SUBSTRATE IS NEVER REACHED.  The corroborating number is
	//    in the table itself: this row is {0.0875, 0.1293, 0.2812,
	//    0.5461} and config 2 (BARE sheen, no substrate at all) is
	//    {0.0875, 0.1283, 0.2810, 0.5453} -- the same curve to MC noise.
	//    A composite that reached its GGX-PBR base could not possibly
	//    land there.
	//
	//    So config 6 and config 7 are ONE defect, not two, and closing it
	//    needs a transmission path for reflection-only top layers rather
	//    than a walk fix.  `fabric_material` (configs 19-34) is the
	//    shipped answer for the sheen case specifically: it evaluates the
	//    COMBINED closed form instead of walking, so the substrate is
	//    reached by construction.
	//
	//    Stays kPostureBounded at 5 %: the row is a record of the
	//    composite path's limitation, and the number it should hold is
	//    "config 2's curve", which kPostureBounded already brackets.
	{ ConfigReport& r = add( "6. Sheen / GGX-PBR (sheen over PBR)", kPostureBounded, 0.05,
	    config6Note.str().c_str() );
	  Run( r, *compSheenPbr ); }

	// 7. Finding D verification: clearcoat (F0 = 0.04) over a
	//    coloured + diffuse-dominant PBR base (red baseColor,
	//    metallic = 0).  Audit result: ρ = 0.04 at normal, growing
	//    to 0.20 at grazing.  Same loss profile as #3
	//    (dielectric/Lambertian) — the smooth-low-F0 GGX top acts
	//    like a near-delta dielectric at this α, and the diffuse-
	//    dominant base's exit paths get clipped by the same
	//    CompositeSPF recursion budget that clobbers #3.  The
	//    importer's "near-black clearcoat over PBR" warning at
	//    GLTFSceneImporter.cpp:851 IS describing real behaviour
	//    in the coloured-input regime — not stale.  Confirmed by
	//    re-running with white inputs (#5) which passes; the bug
	//    is regime-dependent.  Disposition: same as Finding A
	//    (CompositeSPF random walk fix).
	//    2026-09-01 RE-MEASURE + CORRECTED DIAGNOSIS.  This row is NOT the same
	//    bug as #3, and it is not a recursion-budget effect either.  It did not
	//    move at all when #3's IOR-stack bug was fixed ({0.0390, 0.0391, 0.0652,
	//    0.1970} before and after).  The actual cause is structural: GGXSPF only
	//    ever emits UPWARD lobes (reflection + diffuse; grep AddScatteredRay in
	//    GGXSPF.cpp -- there is no transmission lobe), so a GGX top layer never
	//    hands CompositeSPF's walk a downward ray and the SUBSTRATE IS NEVER
	//    REACHED.  Measured directly: an SPF-level probe of GGX(rd=0, rs=0.04)
	//    over a white Lambertian reports rho = 0.03900 against the BARE coat's
	//    0.03897, with zero downward rays emitted.  Configs #4 and #5 have the
	//    same structure and only look healthy because their top layer's rs = 1
	//    (a perfect mirror), which masks the missing substrate at rho = 1.
	//    Closing this needs a transmission path for reflection-only top layers,
	//    not a walk fix -- coated_material (#14/#15) is the shipped answer.
	{ ConfigReport& r = add( "7. Clearcoat / red GGX-PBR (Finding D)", kPostureKnownFailure, 0.0,
	    "reflection-only GGX top emits no downward lobe, so the substrate is never reached "
	    "(NOT #3's walk bug, NOT a recursion budget)" );
	  Run( r, *compClearcoatRedPbr ); }

	// 8-10. polished_material wetness recipe (docs/WETNESS_COAT_DESIGN.md §6.2,
	// §13 Phase-1 exit gate: "a furnace configuration putting a number on §6.2's
	// coverage dip"). White substrate (Rd=1), ior=1.33, scattering=200000
	// (recipe's pooled/near-delta end) held fixed across all three; only `tau`
	// (the coverage mask) varies. §6.2's analytic model: the coat lobe carries
	// `kray = tau·Rs`, the substrate lobe carries `kray = Rd·(1-Rs)` (NOT
	// `Rd·(1-tau·Rs)`), so the predicted directional albedo, ignoring the
	// separate geometric-horizon coat-lobe drop, is
	//     rho_pred(theta) = tau·Rs(theta) + Rd·(1-Rs(theta)) = 1 - Rs(theta)·(1-tau)
	// for Rd=1 — i.e. the missing term is exactly §6.2's `Rd·Rs·(1-c)` with
	// c=tau. Rs(theta) is the full (unpolarised) Fresnel reflectance at
	// ior=1.33, computed independently of RISE's `Optics::CalculateDielectricReflectance`
	// (same closed-form average of Rs_perp/Rp_parallel) for {0,30,60,80} deg:
	// Rs ≈ {0.0201, 0.0211, 0.0591, 0.3471}. This furnace fixture is a single
	// flat, unperturbed shading normal (vGeomNormal defaults to zero → the
	// geometric-horizon gate in PolishedSPF::Scatter degenerates to the shading
	// hemisphere test), so the horizon-lobe-drop term §6.2 separately describes
	// (which needs a shading/geometric-normal MISMATCH from a tilting modifier)
	// is not expected to contribute measurably here — this configuration
	// isolates and measures the `Rd·Rs·(1-c)` term alone, not the total §6.2
	// bound. Measured-vs-predicted numbers are filled in below per angle.

	// 8. tau = 1.0 (full coverage). c=1 makes the deficit term `Rd·Rs·(1-c)`
	//    vanish identically, so rho_pred = 1.0 at every angle exactly. This is
	//    also the methodology check for the polished-material fixture itself,
	//    analogous to config #0 for Lambertian: if this fails, the harness (not
	//    the material) is suspect.
	//    Measured (100000 samples/angle): rho = {1.0000, 1.0000, 1.0000, 1.0000}
	//    at theta = {0, 30, 60, 80} deg vs predicted {1,1,1,1} -- exact match to
	//    4 decimal places (near-delta Phong lobe at scattering=200000 has almost
	//    no sampling variance). kPostureMatchesPrediction, eps=0.002 -- enforces
	//    the actual predicted number (not just an energy band), so a regression
	//    that makes `tau` a no-op would still have to land within 0.002 of 1.0
	//    to pass, which a broken deficit computation would not do at grazing.
	{
		static const double kPredicted8[NUM_THETA] = { 1.0, 1.0, 1.0, 1.0 };
		ConfigReport& r = addPredicted( "8. Polished, tau=1.0 (full coverage)", 0,
		    kPredicted8, 0.002 );
		Run( r, *polishedTau1_0 );
	}

	// 9. tau = 0.5 -- the dip's worst region (c far from both 0 and 1, and
	//    §6.2 notes the `Rd·Rs·(1-c)` bound is largest as c -> 0). Fresnel
	//    Rs(theta) at ior=1.33 (independently computed, full unpolarised
	//    average, {0,30,60,80} deg): {0.02006, 0.02111, 0.05913, 0.34692}.
	//    Predicted rho = 1 - 0.5·Rs = {0.9900, 0.9894, 0.9704, 0.8265}.
	//    Measured (100000 samples/angle): rho = {0.9900, 0.9894, 0.9704,
	//    0.8265} -- matches the analytic `Rd·Rs·(1-c)` prediction to within
	//    0.0001 at every angle, i.e. no additional horizon-drop loss is
	//    measurable on this flat, unperturbed fixture (consistent with the
	//    note above: the horizon-lobe-drop term needs a shading/geometric
	//    normal mismatch this fixture doesn't have). Documented non-full-energy
	//    configuration by construction (intermediate coverage is supposed to
	//    leak per §6.2) -- kPostureMatchesPrediction, eps=0.002 enforces the
	//    predicted curve itself (20x the ≤0.0001 measured agreement, so real
	//    MC-seed variation has headroom) rather than the generic [0,1+tol]
	//    energy band, which would pass both a no-op `tau` (rho -> 1.0) and an
	//    unbounded/inverted deficit (rho -> 0.5 or below) without complaint.
	{
		static const double kPredicted9[NUM_THETA] = { 0.9900, 0.9894, 0.9704, 0.8265 };
		ConfigReport& r = addPredicted( "9. Polished, tau=0.5 (worst-case dip)",
		    "Rd*Rs*(1-c) deficit, c=0.5: predicted rho={0.9900,0.9894,0.9704,0.8265}, measured {0.9900,0.9894,0.9704,0.8265} -- matches to 0.0001",
		    kPredicted9, 0.002 );
		Run( r, *polishedTau0_5 );
	}

	// 10. tau = 0.9 -- the rain-wet-cobbles recipe's own pooled value (§6.5's
	//     worked example: "pooling saturates damp_raw, so tau reaches 0.90").
	//     Predicted rho = 1 - 0.1·Rs = {0.9980, 0.9979, 0.9941, 0.9653}.
	//     Measured (100000 samples/angle): rho = {0.9980, 0.9979, 0.9941,
	//     0.9653} -- matches the analytic prediction to within 0.0001 at
	//     every angle. This is the number the design doc's §13 exit gate and the
	//     §12 debt-5 entry ask for: at the recipe's actual pooled coverage, the
	//     §6.2 coverage-mask deficit is small (<=0.2% loss up to 60 deg, ~3.5%
	//     at 80 deg grazing) and matches the analytic `Rd*Rs*(1-c)` bound with
	//     no additional measurable horizon-drop contribution on this fixture.
	//     kPostureMatchesPrediction, eps=0.002 around the stated prediction --
	//     same regression-gate reasoning as #9.
	{
		static const double kPredicted10[NUM_THETA] = { 0.9980, 0.9979, 0.9941, 0.9653 };
		ConfigReport& r = addPredicted( "10. Polished, tau=0.9 (recipe pooled value)",
		    "Rd*Rs*(1-c) deficit, c=0.9 (recipe's pooled tau): predicted rho={0.9980,0.9979,0.9941,0.9653}, measured {0.9980,0.9979,0.9941,0.9653} -- matches to 0.0001",
		    kPredicted10, 0.002 );
		Run( r, *polishedTau0_9 );
	}

	// ================================================================
	// 11-16. coated_material -- docs/WETNESS_COAT_DESIGN.md Phase 2
	//        exit gate (7.6).  See the construction block above for
	//        which composite configuration each one mirrors.
	// ================================================================

	// 11. Coated: varnish coat (ior 1.5, alpha 0.02) over WHITE
	//     Lambertian, full coverage.  DIRECT MIRROR OF CONFIG 3
	//     (`dielectric ior 1.5 / white Lambertian`), which is
	//     kPostureKnownFailure because CompositeSPF's recursion budget
	//     kills the below-layer diffuse paths.  Nothing recurses here:
	//     the coat's transmission is folded into the substrate lobe's
	//     throughput analytically (7.5), so there is no budget to
	//     exhaust.  HIGH-SUBSTRATE-ALBEDO (R = 1) -- this is the
	//     configuration the exit gate names as the one that fails if
	//     7.4's recycling compensation is omitted.  With it, rho == 1
	//     is exact physics, not a tolerance: see CoatedLayer.h's
	//     energy-conservation proof.  2 % band.
	{ ConfigReport& r = add( "11. Coated varnish / white Lambertian", kPosturePass, 0.02,
	    "mirrors known-failing #3; rho=1 exact via 7.4 recycling" );
	  Run( r, *coatedVarnishWhiteLamb->GetSPF() ); }

	// 12. Coated: WATER coat (ior 1.33, alpha 0.02) over white
	//     Lambertian, full coverage -- the wetness case the design doc
	//     is actually about, at the highest substrate albedo there is.
	//     Same 2 % band.  Config 16 is this same material with the
	//     compensation disabled.
	{ ConfigReport& r = add( "12. Coated water / white Lambertian", kPosturePass, 0.02,
	    "the wetness case at R=1; red-proof twin is #16" );
	  Run( r, *coatedWaterWhiteLamb->GetSPF() ); }

	// 13. Coated: water coat at coat_weight = 0.5 over white
	//     Lambertian.  7.3's coverage semantics: at c = 0.5 the surface
	//     is a statistical mixture of a coated state and a BARE state,
	//     and BOTH conserve energy at R = 1, so rho stays 1 at every
	//     angle.  That is the discriminating check against reading
	//     coverage as a gloss knob -- contrast config 9, where
	//     polished_material's `tau`-as-coverage stand-in loses 17 % at
	//     80 deg for exactly the reason 6.2 describes.  Same 2 % band.
	{ ConfigReport& r = add( "13. Coated water c=0.5 / white Lambertian", kPosturePass, 0.02,
	    "7.3 coverage is a mixture, not a gloss knob: rho=1 at every c (cf. #9)" );
	  Run( r, *coatedWaterHalfCover->GetSPF() ); }

	// 14. Coated: clearcoat (ior 1.5 == F0 0.04, alpha 0.16) over the
	//     WHITE twin of config 7's diffuse-dominant GGX-PBR base.
	//     CONFIG 7's SHAPE at the furnace's white-input discipline.
	//
	//     ANALYTIC CROSS-CHECK against config 17 (the same substrate,
	//     bare).  The layer's hemispherical form predicts
	//        rho = F + (1-F) * A_base * (1 - r_i)/(1 - r_i * R_hemi)
	//     with r_i(1.5) = 0.596346 and R_hemi = 1.0 (this substrate's
	//     diffuse 1.0*(1-0.04) plus its Schlick hemispherical average
	//     0.04 + 0.96/21 = 0.0857 exceeds 1 and clamps), so the
	//     recycling factor is exactly 1.0 here:
	//        theta=0 : A_base 0.9988 -> pred 0.9988 vs meas 0.9997  (+0.0009)
	//        theta=30: A_base 0.9994 -> pred 0.9994 vs meas 1.0010  (+0.0016)
	//        theta=60: A_base 1.0251 -> pred 1.0229 vs meas 1.0104  (-0.0125)
	//        theta=80: A_base 1.1573 -> pred 1.0963 vs meas 0.8776  (-0.2187)
	//
	//     So the layer arithmetic is CONFIRMED analytically at 0 and
	//     30 deg (agreement ~0.001), drifts ~0.013 at 60, and diverges
	//     at 80 -- and config 15 shows the SAME +0.001 / +0.001 /
	//     -0.014 / -0.214 profile on a completely different (coloured,
	//     absorbing) substrate.  Identical magnitudes on two different
	//     substrates is what makes the diagnosis attributable: it is
	//     not about the substrate at all.
	//
	//     The mechanism, precisely, and it is the limitation 7.4 names
	//     IN ADVANCE ("reach for [Belcour] if the furnace
	//     configurations show WW-plus-compensation failing at high
	//     albedo or high coat IOR, where the single-scatter
	//     approximation is weakest"): the layer's exit factor is
	//     DIRECTIONAL (T(theta_o) = 1 - F(theta_o)) while its
	//     recycling coefficient is the DIFFUSE average r_i.  Those two
	//     are consistent for a Lambertian substrate -- that
	//     consistency is the identity that puts configs 11-13 on 1.0
	//     exactly -- but a microfacet substrate returns light near its
	//     own mirror direction, which at 80 deg incidence is also
	//     grazing, where T(theta_o) is only 0.61 while r_i still says
	//     0.60.  The model therefore under-recycles precisely where a
	//     specular substrate returns its light.  Fixing it needs
	//     Belcour's operators (which carry the recycling natively,
	//     per-lobe); 7.4 declines that for v1 on scope.  Configs 11-13
	//     carry the exit gate's kPosturePass claim; this row and 15
	//     carry the MEASURED BOUNDARY of the model.
	//
	//     kPostureMatchesPrediction against the measured curve, eps
	//     0.005.  The harness is deterministically seeded (repeated
	//     runs agree to every printed digit), so this gates a
	//     regression in EITHER direction: a collapse toward config 7's
	//     0.04 and an unphysical gain both move off it.  Expect to
	//     re-pin on a toolchain change -- see the note on config 15.
	{
		static const double kPredicted14[NUM_THETA] = { 0.9997, 1.0010, 1.0104, 0.8776 };
		ConfigReport& r = addPredicted( "14. Coated clearcoat / white GGX-PBR",
		    "config-7 shape, white inputs; analytic vs #17 confirms the layer to ~0.001 at 0-30 deg, -0.219 at 80 deg = 7.4's named WW/Belcour limit",
		    kPredicted14, 0.005 );
		Run( r, *coatedClearcoatWhiteGgx->GetSPF() );
	}

	// 15. Coated: clearcoat over the SAME coloured, diffuse-dominant
	//     GGX-PBR base as config 7 (red baseColor 0.8/0.2/0.2,
	//     metallic 0, F0 = 0.04).  THE APPLES-TO-APPLES IMPROVEMENT
	//     CLAIM: composite reports rho = {0.0390, 0.0391, 0.0652,
	//     0.1970}; this reports {0.6777, 0.6789, 0.7002, 0.6589} --
	//     17x the energy at normal incidence.
	//
	//     rho CANNOT be 1 here and is deliberately not asserted to be:
	//     the substrate absorbs, so the ceiling is its own reflectance
	//     (config 18 measures it: 0.8069 at normal).
	//
	//     ANALYTIC CROSS-CHECK against config 18, same form as #14,
	//     with r_i(1.5) = 0.596346 and R_hemi = 0.8*(1-0.04) + 0.0857
	//     = 0.8537143 -> recycling factor 0.822289:
	//        theta=0 : A_base 0.8069 -> pred 0.6770 vs meas 0.6777  (+0.0007)
	//        theta=30: A_base 0.8074 -> pred 0.6779 vs meas 0.6789  (+0.0010)
	//        theta=60: A_base 0.8344 -> pred 0.7141 vs meas 0.7002  (-0.0139)
	//        theta=80: A_base 0.9631 -> pred 0.8726 vs meas 0.6589  (-0.2137)
	//
	//     i.e. the Saunderson recycling is doing exactly the right
	//     arithmetic on a COLOURED, ABSORBING, MICROFACET substrate --
	//     not merely on the white Lambertian one -- for the angles
	//     where the model's own assumptions hold.  (Composite's 0.0390
	//     at normal is 0.64 BELOW that analytic value.)
	//
	//     Note the two substrate quantities are DIFFERENT and must not
	//     be conflated: A_base(theta) = 0.8069 is what the substrate
	//     actually returns at this angle, R_hemi = 0.8537 is what the
	//     recycling series amplifies.  See configs 17-18's header.
	//
	//     kPostureMatchesPrediction against the measured curve, eps
	//     0.005.  MEASURED PINS, not first-principles values, at 60
	//     and 80 deg -- they encode the documented WW divergence, and
	//     a toolchain whose FP differs (MSVC, or the release `Opto`
	//     configuration's -ffast-math) may need them re-pinned.  The
	//     0 and 30 deg entries are within 0.001 of the analytic value
	//     above and should be portable.
	{
		static const double kPredicted15[NUM_THETA] = { 0.6777, 0.6789, 0.7002, 0.6589 };
		ConfigReport& r = addPredicted( "15. Coated clearcoat / red GGX-PBR",
		    "coloured mirror of #7 (composite: {0.0390,0.0391,0.0652,0.1970}); analytic vs #18: +0.0007 at 0 deg, +0.0010 at 30 deg, -0.214 at 80 deg (7.4's WW limit)",
		    kPredicted15, 0.005 );
		Run( r, *coatedClearcoatRedGgx->GetSPF() );
	}

	// 16. RED PROOF for 7.4's "the compensation term is load-bearing
	//     and must land with the core, not after it".
	//
	//     Identical to config 12 except that the interreflection
	//     compensation is switched off, leaving plain Weidlich-Wilkie
	//     single bounce.  The analytic prediction is then
	//
	//        rho_WW(theta) = F(theta) + (1 - F(theta)) * (1 - r_i)
	//
	//     because the light that enters the coat reflects off the R = 1
	//     substrate once and then loses the fraction r_i to total
	//     internal reflection at the water->air boundary, with nothing
	//     recycling it back.  r_i is computed here INDEPENDENTLY of the
	//     code under test (200 000-point midpoint quadrature of the
	//     unpolarised Fresnel curve, r_i = 1 - (1 - r_e)/eta^2), giving
	//     r_i(1.33) = 0.471949 -- which is also the classical
	//     Egan-Hilgeman value.  Note it sits JUST ABOVE the top of
	//     2.1's quoted "r_i ~ 0.44-0.47" band, by 0.002, which is the
	//     rounding in the doc's own figure rather than a disagreement.
	//     Fresnel at ior 1.33 for {0,30,60,80} deg is the same
	//     {0.02006, 0.02111, 0.05913, 0.34692} configs 9 and 10 use.
	//
	//     Predicted rho = {0.5375, 0.5380, 0.5560, 0.6918}: a 46 %
	//     energy loss at normal incidence, which is 7.4's "roughly
	//     45 %" measured rather than asserted.  Config 12 with the
	//     compensation ON returns {0.9997, 1.0001, 1.0004, 0.9907} at
	//     the same angles.
	//
	//     Measured here: {0.5375, 0.5378, 0.5562, 0.6813}.  The first
	//     three sit on the analytic curve to 0.0003.  The 80 deg point
	//     is 0.0105 low, and that residual is NOT the layer term: it is
	//     the coat GGX lobe's own grazing single-scattering loss, which
	//     shows up IDENTICALLY in config 12 (0.9907 against an exact
	//     1.0, i.e. 0.0093 low at the same angle).  Same lobe, same
	//     angle, same size -- which is what makes it attributable.
	//
	//     kPostureMatchesPrediction against the ANALYTIC curve (not the
	//     measured one), eps 0.015 to clear that attributed grazing
	//     residual: this configuration must keep FAILING energy
	//     conservation in exactly the predicted way.  A regression that
	//     quietly reintroduced the compensation here, or broke it into
	//     some other wrong value, moves rho off this curve and fails.
	{
		static const double kPredicted16[NUM_THETA] = { 0.5375, 0.5380, 0.5560, 0.6918 };
		ConfigReport& r = addPredicted( "16. Coated water, NO 7.4 recycling (red proof)",
		    "WW single bounce: rho = F + (1-F)(1-r_i), r_i(1.33)=0.471949 -- the ~45% loss 7.4 predicts (cf. #12 at 1.000)",
		    kPredicted16, 0.015 );
		Run( r, *coatedWaterNoRecycle->GetSPF() );
	}

	// 17-18. BARE-SUBSTRATE REFERENCE ROWS for configs 14 and 15.
	//
	// Not gates in themselves -- they exist so the coated rows above
	// can be checked ANALYTICALLY rather than pinned to measurement
	// alone.  The layer's hemispherical prediction is
	//
	//    rho_coat(theta) = F(theta)
	//                    + (1 - F(theta)) * A_base(theta) * (1 - r_i)
	//                      / (1 - r_i * R_hemi)
	//
	// which needs TWO different substrate quantities, and conflating
	// them is the easy mistake:
	//
	//   A_base(theta) -- the substrate's ACTUAL directional albedo at
	//     this angle, i.e. what its own Scatter returns.  GGX has no
	//     closed form for it, which is why these rows measure it.
	//
	//   R_hemi -- the substrate's reflectance under a DIFFUSE field,
	//     which is what CoatedBRDF puts in the recycling denominator
	//     (IBSDF::hemisphericalAlbedo).  Closed-form for a schlick_f0
	//     GGX: diffuse*(1 - maxF0) + SchlickFresnelAvg(F0), with
	//     SchlickFresnelAvg(F0) = F0 + (1-F0)/21.
	//       white base: 1.0*0.96 + 0.0857143 = 1.0 (clamped)
	//       red base:   0.8*0.96 + 0.0857143 = 0.8537143
	//
	// A_base is the energy that actually comes back off the substrate;
	// R_hemi is what the recycling series geometrically amplifies.
	// kPostureKnownFailure, not Bounded: this row goes OVER UNITY at
	// grazing (1.1573 at 80 deg).  That is GGX's own pre-existing
	// behaviour with a low F0 -- Schlick's grazing Fresnel rising to
	// ~1 on top of a diffuse weight of (1 - maxF0) = 0.96, plus the
	// Kulla-Conty multiscatter tail -- and it is visible here only
	// because this reference row measures the BARE substrate.  It is
	// not introduced by, and not fixable from, `coated_material`; the
	// coated row above it (config 14) does not inherit the gain.
	// Recorded rather than gated so the number stays in front of
	// whoever next looks at GGX energy at grazing.
	{ ConfigReport& r = add( "17. White GGX-PBR base alone (ref for #14)", kPostureKnownFailure, 0.0,
	    "reference row: A_base(theta) for config 14's analytic check; over-unity at grazing is GGX's own low-F0 behaviour" );
	  Run( r, *whiteGgxMat->GetSPF() ); }

	{ ConfigReport& r = add( "18. Red GGX-PBR base alone (ref for #15)", kPostureBounded, 0.06,
	    "reference row: A_base(theta) for config 15's analytic check" );
	  Run( r, *redGgxMat->GetSPF() ); }

	// ================================================================
	// 19-36.  fabric_material -- docs/CLOTH_FABRIC_DESIGN.md 9.9 gate 3
	//         (round 5, 2026-09-02).
	//
	// FOUR SUBSTRATE SHAPES x FOUR SHEEN ROUGHNESSES, all white-input,
	// plus the two SUBSTRATE REFERENCE ROWS (19, 20) the predictions
	// below are built from.  The four shapes are gate 3's: a Lambertian,
	// an Oren-Nayar, an ISOTROPIC GGX-PBR, and an ANISOTROPIC GGX
	// (alphax != alphay) with a NON-ZERO weave_rotation -- the last
	// being the row that would fail if 9.5's frame-rotation plumbing
	// were wrong, since the rotation is a substrate-frame change and
	// nothing else.
	//
	// WHAT CHANGED IN ROUND 5, AND WHY THESE POSTURES.
	//
	// Round 4 specified the glTF sheen scaling with the two arms
	// combined by a `min`.  Implementation-time measurement rejected it:
	// the `min` is reciprocal but CANNOT conserve energy (near normal
	// incidence E(v) -> 0 while the min still picks 1 - m*E(l) for every
	// l, so the base loses Ebar's worth of energy the sheen never
	// returns), and glTF's own one-arm form conserves energy but is NOT
	// reciprocal.  Round 5 adopted the Kulla-Conty PRODUCT form
	//
	//     scale(l,v) = (1 - m*E(v)) * (1 - m*E(l)) / (1 - m*Ebar)
	//
	// which is BOTH.  It is the closed form of the adding-doubling
	// inter-reflection series between a lossless fuzz layer and the
	// base: energy the fuzz intercepts is re-scattered onto the
	// substrate rather than deleted.
	//
	// So the postures are now the ones the physics earns, per substrate:
	//
	//  * LAMBERTIAN rows (21-24): kPosturePass at 5 %.  For a white
	//    Lambertian base the identity is exact at ANY m and ANY alpha --
	//    rho(v) = m*E(v) + (1 - m*E(v))*(1 - m*Ebar)/(1 - m*Ebar) = 1 --
	//    so these must land on 1.000 and a deviation is a real
	//    regression.  This is the row set that PROVES the denominator's
	//    near-normal brightening (up to ~1.10 pointwise at alpha ~ 0.2;
	//    see FabricBRDF.h's measured table) is energy-NEUTRAL rather
	//    than a gain: it is exactly balanced by the darkening at
	//    grazing, and rho == 1 is what says the balance is exact.
	//
	//  * NON-LAMBERTIAN rows (25-36): kPostureMatchesPrediction against
	//    a prediction DERIVED AT RUN TIME from the bare substrate's own
	//    measured curve, using the closed form the product model gives:
	//
	//        rho_fabric(v) = E(alpha, cos v)
	//                      + rho_substrate(v) * (1 - E(alpha, cos v))
	//
	//    (m == 1 and the dye is white here, as everything in this
	//    furnace is.)  That is NOT a locked-in measurement and NOT
	//    circular: the substrate row is measured independently by the
	//    same Monte-Carlo driver, and the fabric row is then required to
	//    equal an analytic function of it.  It is the honest statement
	//    of "the fabric layer neither adds nor removes energy over the
	//    substrate's own posture" -- Oren-Nayar loses energy by its own
	//    design and GGX-PBR gains at grazing pre-existingly (config 17
	//    records the bare white GGX-PBR at 1.1555 at theta = 80), and
	//    this form inherits exactly those and nothing more.
	//
	//    eps is 0.01, set from the combined MC noise of the two
	//    independent 100k-sample estimates this check compares (the
	//    fabric row and W), each with a 1-sigma near rho = 1 of ~3e-3.
	//    The prediction itself carries no approximation, which is what
	//    makes a tolerance that tight legitimate.
	//
	//  WHAT IS ACTUALLY ASSERTED, since an earlier revision of this
	//  comment and of the design doc both quoted numbers (0.002, 0.02)
	//  that were never in the code:
	//    * Lambertian rows: kPosturePass at 5 % around rho = 1, AND the
	//      per-angle closed-form cross-check at 0.012 (the block below
	//      the config loop).
	//    * Oren-Nayar / GGX rows: kPostureMatchesPrediction at eps 0.01.
	//    * Grazing check: 5 %, conserving above mu = 0.03 and bounded
	//      below it.
	// ================================================================

	// Substrates.  White inputs throughout, so any loss is the fabric's.
	UniformScalarPainter* sOnSigma = new UniformScalarPainter( 0.4 );  sOnSigma->addref();
	OrenNayarMaterial* whiteOnMat = new OrenNayarMaterial( *one, *sOnSigma );  whiteOnMat->addref();

	UniformScalarPainter* sAnisoX = new UniformScalarPainter( 0.34 );  sAnisoX->addref();
	UniformScalarPainter* sAnisoY = new UniformScalarPainter( 0.06 );  sAnisoY->addref();
	GGXMaterial* anisoGgxMat = new GGXMaterial(
		*one, *dielF0, *sAnisoX, *sAnisoY, *iorSc, *zeroSc, eFresnelSchlickF0 );
	anisoGgxMat->addref();

	// A non-zero weave angle for the anisotropic row only; every other
	// row leaves the rotation at 0, where WeaveRotatedRI's fast path
	// hands the substrate the caller's own record unchanged.
	UniformScalarPainter* sWeave0  = new UniformScalarPainter( 0.0 );   sWeave0->addref();
	UniformScalarPainter* sWeave45 = new UniformScalarPainter( 0.7853981633974483 );  sWeave45->addref();

	// 19-20.  The two substrate reference rows that do not already exist
	//         in this file (config 0 is the Lambertian, config 17 the
	//         white iso GGX-PBR).  Same placement idiom as configs 17/18.
	{ ConfigReport& r = add( "19. White Oren-Nayar(0.4) base alone (ref for 25-28)", kPostureBounded, 0.06,
	    "reference row: rho_substrate(theta) for the Oren-Nayar fabric rows' analytic check.  "
	    "Oren-Nayar dissipates by its own design (OrenNayarBRDF.cpp documents Rd as up to 25.6 % "
	    "high at roughness 1), which is why this is Bounded and not Pass" );
	  Run( r, *whiteOnMat->GetSPF() ); }

	{ ConfigReport& r = add( "20. White aniso GGX + F0=0.04 alone (ref for 33-36)", kPostureKnownFailure, 0.0,
	    "reference row: rho_substrate(theta) for the anisotropic fabric rows.  Over-unity at "
	    "grazing is GGX's own low-F0 behaviour -- same disposition as config 17" );
	  Run( r, *anisoGgxMat->GetSPF() ); }

	struct FabricShape { const char* label; IMaterial* base; IScalarPainter* weave; bool lambertian; };
	const FabricShape fabricShapes[] = {
		{ "Lambertian",           whiteLambMat, sWeave0,  true  },
		{ "OrenNayar(0.4)",       whiteOnMat,   sWeave0,  false },
		{ "iso GGX-PBR",          whiteGgxMat,  sWeave0,  false },
		{ "aniso GGX + weave 45", anisoGgxMat,  sWeave45, false },
	};
	static const double kFabricAlphas[] = { 0.08, 0.2, 0.5, 1.0 };

	std::vector<UniformScalarPainter*> fabricAlphaPnts;
	std::vector<FabricMaterial*>       fabricMats;

	//! Per-angle closed-form cross-check for the Lambertian rows, 9.9
	//! gate 3.  Collected during the config loop, reported and asserted
	//! after it.
	struct LamCheck { double alpha; double measured[NUM_THETA]; double predicted[NUM_THETA]; };
	std::vector<LamCheck> lamClosedForm;

	{
		int row = 0;
		for( int si = 0; si < 4; ++si )
		{
			const FabricShape& shape = fabricShapes[si];
			for( double fa : kFabricAlphas )
			{
				UniformScalarPainter* sa = new UniformScalarPainter( fa );  sa->addref();
				fabricAlphaPnts.push_back( sa );

				FabricMaterial* fm = new FabricMaterial( *shape.base, *one, *sa, *shape.weave );
				fm->addref();
				fabricMats.push_back( fm );

				std::ostringstream nm;
				nm << ( 21 + row ) << ". fabric / " << shape.label
				   << "  (sheen alpha " << fa << ")";

				if( shape.lambertian )
				{
					// The exact identity.  5 % is the harness's standard
					// MC band; the row should sit on 1.000 to ~0.001.
					ConfigReport& r = add( nm.str(), kPosturePass, 0.05,
						"9.9 gate 3: the Kulla-Conty product form conserves energy EXACTLY over a "
						"white Lambertian base at any alpha and any m -- rho == 1 is an identity "
						"here, not a fit, and it is what proves the denominator's near-normal "
						"brightening is energy-neutral.  ALSO cross-checked per angle against the "
						"closed form, printed after the report" );
					Run( r, *fm->GetSPF() );

					// The per-angle closed-form cross-check (9.9 gate 3).
					lamClosedForm.emplace_back();
					LamCheck& lc = lamClosedForm.back();
					lc.alpha = fa;
					for( int ti = 0; ti < NUM_THETA; ++ti ) {
						const double cosT = std::cos( THETA_DEG[ti] * PI / 180.0 );
						lc.measured[ti]  = r.albedo[ti];
						lc.predicted[ti] = LambertianFabricRhoClosedForm( fa, 1.0, cosT );
					}
				}
				else
				{
					// The EXACT directional albedo of the product form:
					//
					//   rho(v) = E(v) + (1 - m*E(v))/(1 - m*Ebar) * W(v)
					//
					// with W(v) measured directly off the substrate's own
					// SPF (see SubstrateAlbedoFuzzWeighted's header for why
					// the rho_substrate shortcut is NOT usable at grazing).
					// m == 1 and the dye is white, as everything in this
					// furnace is.
					const double kM = 1.0;
					double pred[NUM_THETA];
					for( int ti = 0; ti < NUM_THETA; ++ti ) {
						const double rad  = THETA_DEG[ti] * PI / 180.0;
						const double cosT = std::cos( rad );
						// Ehat, not raw E: the sheen lobe's own directional
						// albedo is bounded by the symmetric normaliser, so
						// the prediction must be too.
						const double ev   = SheenDirectionalAlbedo::E( fa, cosT );
						const double w    = SubstrateAlbedoFuzzWeighted(
							*shape.base->GetSPF(), rad, fa, kM,
							shape.weave->GetValuesAt( MakeIntersection( rad ) ).v[0] );
						pred[ti] = std::min( 1.0, ev )
						         + FuzzTransmit( fa, kM, cosT )
						           / FuzzTransmitMean( fa, kM ) * w;
					}
					// eps 0.01 is set from MC NOISE, not from what passes:
					// this compares two INDEPENDENT 100k-sample estimates
					// (the fabric row and W), each with a 1-sigma on the
					// mean of ~3e-3 near rho = 1, so their difference has
					// ~4e-3 -- and 0.01 is ~2.5 sigma of headroom.  The
					// prediction itself carries no approximation, which is
					// what makes a tolerance this tight legitimate.
					ConfigReport& r = addPredicted( nm.str(),
						"9.9 gate 3 (round 5): pinned to the EXACT directional albedo "
						"E(v) + (1-m*E(v))/(1-m*Ebar) * W(v), with W(v) = INT f_base(l,v)(1-m*E(l))cos dl "
						"measured directly off the substrate's own SPF -- so the fabric layer is "
						"required to inherit the substrate's energy posture and add nothing, with "
						"no uncorrelated-response assumption anywhere.  eps 0.01 is ~2.5 sigma of "
						"the two 100k-sample estimates' combined MC noise",
						pred, 0.01 );
					Run( r, *fm->GetSPF() );
				}
				++row;
			}
		}
	}

	// ================================================================
	//  CLOSED-FORM CROSS-CHECK on the Lambertian fabric rows -- 9.9
	//  gate 3's second assertion, alongside the 5 % posture band.
	//
	//  TOLERANCE, derived rather than fitted.  Two error sources:
	//    * the furnace's Monte-Carlo error -- 100k samples puts the
	//      1-sigma on the mean near rho = 1 at ~3e-3;
	//    * the closed form's own quadrature -- 1024 x 512 midpoint,
	//      two orders below that, so it does not move the budget.
	//  The E table's interpolation residual CANCELS: both sides read the
	//  same `SheenDirectionalAlbedo::E`, so the closed form predicts what
	//  the material should produce GIVEN the table, not what the true
	//  lobe would give.  (Gate 3's grazing check and the 5 % posture
	//  band are what bound the table's own error.)
	//
	//  0.012 is 4 sigma of the MC term.  A tighter bound would flag
	//  sampling noise; a looser one would stop discriminating, since the
	//  defects this catches -- a wrong normaliser, a dropped
	//  denominator, an l/v asymmetry -- move rho by whole percent.
	// ================================================================
	{
		std::cout << "\n";
		std::cout << "  Gate 3 -- CLOSED-FORM cross-check, Lambertian fabric rows\n";
		std::cout << "  (deterministic quadrature of the product form against the furnace's\n";
		std::cout << "   MC sampling of the implementation; re-derived from CharlieSheen +\n";
		std::cout << "   SheenDirectionalAlbedo, NOT from FabricBRDF):\n";
		std::cout << "    " << std::setw( 8 ) << "alpha";
		for( int ti = 0; ti < NUM_THETA; ++ti ) {
			std::cout << std::setw( 21 ) << ( "theta=" + std::to_string( (int)THETA_DEG[ti] ) );
		}
		std::cout << "\n";

		static const double kClosedFormTol = 0.012;
		double worstDelta = 0.0;

		for( const LamCheck& lc : lamClosedForm )
		{
			std::cout << "    " << std::setw( 8 ) << std::fixed << std::setprecision( 2 ) << lc.alpha;
			for( int ti = 0; ti < NUM_THETA; ++ti ) {
				const double d = std::fabs( lc.measured[ti] - lc.predicted[ti] );
				worstDelta = std::max( worstDelta, d );
				std::ostringstream cell;
				cell << std::fixed << std::setprecision( 5 )
				     << lc.measured[ti] << "/" << lc.predicted[ti];
				std::cout << std::setw( 21 ) << cell.str();
			}
			std::cout << "\n";
		}
		std::cout << std::defaultfloat;
		std::cout << "    worst |measured - closed form| = " << worstDelta
		          << "   (tolerance " << kClosedFormTol << ", = 4 sigma of the 100k-sample MC error)\n";

		ConfigReport& r = add( "38. Gate 3 closed-form cross-check (Lambertian fabric)",
			kPosturePass, kClosedFormTol,
			"pass/fail indicator only -- the measured/predicted pairs are in the table printed "
			"above the report" );
		const bool ok = ( worstDelta <= kClosedFormTol ) && !lamClosedForm.empty();
		for( int i = 0; i < NUM_THETA; ++i ) r.albedo[i] = ok ? 1.0 : 0.0;
		r.passed = ok;
		if( !ok ) {
			std::cout << "    FAIL: a Lambertian row missed the closed form by more than "
			          << kClosedFormTol << "\n";
		}
	}

	// ================================================================
	//  GRAZING CHECK -- docs/CLOTH_FABRIC_DESIGN.md 9.9 gate 3, the half
	//  the angle columns cannot reach.
	//
	//  WHY THIS EXISTS, and it is not hypothetical.  THETA_DEG stops at
	//  80 deg, i.e. mu = 0.1736.  Until 2026-09-02 the E table's cosTheta
	//  axis was UNIFORM, so its first interior node sat at mu = 0.0323
	//  and the lookup ramped linearly from an exact 0 across the entire
	//  band the Charlie lobe occupies -- and because `value()` EMITS the
	//  true lobe while suppressing the base by the TABLED E, the
	//  white-furnace identity broke by up to +1.05 ABSOLUTE (rho ~ 2.05)
	//  under grazing illumination, at every roughness.  Every row above
	//  stayed green throughout: 0.1736 is five times the first node, so
	//  the columns never entered the broken band.
	//
	//  Adding 85/88/89 deg as COLUMNS would have meant re-measuring and
	//  re-locking all nineteen pre-existing prediction curves (configs 3,
	//  9, 10, 14-16 carry NUM_THETA-wide literals), so this is a separate
	//  sweep over the Lambertian fabric rows -- the ones whose expected
	//  answer is an identity rather than a locked number, and therefore
	//  the ones that can be checked at a new angle without re-measuring
	//  anything.
	//
	//  TWO POSTURES, matching the model's own exactness class:
	//    * mu >= 0.03 (theta <= 88.28 deg): CONSERVING.  |rho - 1| <= 5 %.
	//    * mu <  0.03: BOUNDED.  0 <= rho <= 1 + 5 %.  Inside that sliver
	//      the raw lobe still exceeds 1 (E reaches 1.152 even above the
	//      roughness floor) and a symmetric normaliser max(1, E(v), E(l))
	//      is what holds the total down; exact conservation is knowingly
	//      given up there.
	// ================================================================
	{
		std::cout << "\n";
		std::cout << "  Gate 3 -- GRAZING check on the Lambertian fabric rows\n";
		std::cout << "  (the band THETA_DEG cannot reach; conserving for mu >= 0.03,\n";
		std::cout << "   bounded below it -- see the block comment in the source):\n";
		std::cout << "    " << std::setw( 8 ) << "theta"
		          << std::setw( 10 ) << "mu"
		          << std::setw( 12 ) << "a=0.08"
		          << std::setw( 12 ) << "a=0.2"
		          << std::setw( 12 ) << "a=0.5"
		          << std::setw( 12 ) << "a=1.0"
		          << "   posture\n";

		// 89.9 and 89.99 reach mu = 1.7e-3 and 1.7e-4 -- INSIDE and
		// BELOW the E table's first cell (node 1 is mu = 1/961 =
		// 1.04e-3).  That band is where the M4 review found rho reaching
		// 1.714 with the cell un-floored, and where 89 deg (mu = 0.0175,
		// 17x above node 1) could not see it.  Both are `bounded`
		// posture: the floored domain deliberately over-reads E there so
		// the base is fully suppressed, which drives rho DOWN toward the
		// true E -> 0 limit rather than to 1.
		static const double kGrazeDeg[] = { 80.0, 85.0, 88.0, 89.0, 89.9, 89.99 };
		// TIGHTENED 0.05 -> 0.03 (round 8).  At 5 % this band admitted an
		// ~80x growth of the residual before failing, which is no gate at
		// all for a quantity whose measured worst is +1.7 %.  Measured
		// margins at 0.03: the conserving rows' worst |rho - 1| is
		// 0.00475 (theta 85, alpha 0.08) -- 6.3x of headroom -- and the
		// bounded rows' worst rho is 1.00929 (theta 89.9, alpha 0.2).
		//
		// NOT tight enough to catch the GLOBAL worst, and that is a
		// known coverage gap rather than an oversight: the true maximum
		// over the reachable domain sits at alpha ~ 0.90, theta ~ 89.87,
		// and this sweep runs alpha in {0.08, 0.2, 0.5, 1.0} at
		// theta in {80, 85, 88, 89, 89.9, 89.99}, so neither coordinate
		// is sampled.  Closing that would mean a fifth Lambertian
		// fabric row at alpha 0.9; the exactness class in FabricBRDF.h
		// carries the measured number in the meantime.
		static const double kGrazeTol   = 0.03;
		bool grazePassed = true;

		for( double td : kGrazeDeg )
		{
			const double rad = td * PI / 180.0;
			const double mu  = std::cos( rad );
			const bool conserving = ( mu >= 0.03 );

			std::cout << "    " << std::setw( 8 ) << td
			          << std::setw( 10 ) << std::fixed << std::setprecision( 5 ) << mu;

			for( int k = 0; k < 4; ++k )
			{
				// fabricMats is shape-major: the first four entries are
				// the Lambertian rows, one per kFabricAlphas value.
				const double rho = DirectionalAlbedo( *fabricMats[k]->GetSPF(), rad );
				std::cout << std::setw( 12 ) << std::setprecision( 5 ) << rho;

				const bool ok = conserving
					? ( std::fabs( rho - 1.0 ) <= kGrazeTol )
					: ( rho >= 0.0 && rho <= 1.0 + kGrazeTol );
				if( !ok ) {
					grazePassed = false;
					std::cout << "!";
				}
			}
			std::cout << "   " << ( conserving ? "conserving" : "bounded" ) << "\n";
		}
		std::cout << std::defaultfloat;

		if( !grazePassed ) {
			std::cout << "    FAIL: a Lambertian fabric row missed its grazing posture "
			             "(marked with ! above)\n";
		}
		// Fold into the same tally the configs use, as one extra unit,
		// so a grazing failure exits the suite non-zero like any other.
		// The albedo columns on THIS row are a pass/fail indicator (1 or
		// 0), not measurements -- the real per-angle rho values are in
		// the table printed just above, which THETA_DEG cannot express.
		ConfigReport& r = add( "37. Gate 3 grazing check (Lambertian fabric, theta 80-89)",
			kPosturePass, kGrazeTol,
			"pass/fail indicator only -- the measured rho per angle is in the grazing table "
			"printed above the report; conserving for mu >= 0.03, bounded below it" );
		for( int i = 0; i < NUM_THETA; ++i ) r.albedo[i] = grazePassed ? 1.0 : 0.0;
		r.passed = grazePassed;
	}

	// ================================================================
	// 39-47.  weave_material -- docs/CLOTH_FABRIC_DESIGN.md Phase 2,
	//         slice P2-A.
	//
	// EIGHT ROWS: the four shipped presets, each at TWO VIEW SETS.  The
	// second set is the same preset with a 45 deg `weave_rotation`,
	// which -- because the harness's incident directions are fixed --
	// presents each family's yarn axis to the light at a different set
	// of fibre latitudes.  That matters here in a way it would not for
	// an isotropic material: every term in this BSDF is expressed in the
	// FIBRE frame, so rotating the weave under a fixed view is the
	// cheapest way to sample a genuinely different part of the model.
	//
	// POSTURE: kPostureBounded, and the choice is the physics, not a
	// concession.  This model is provably energy-BOUNDED (WeaveBRDF.h
	// section 2's argument: the surface lobe inherits Mp's d'Eon
	// normalisation and the trimmed logistic's; the volume lobe is
	// divided by `C_v`, the EXACT hemispherical integral of its bracket
	// at zero tilt -- not a loose bound, since an earlier revision's
	// `C_v = 2(1+k_d)` bound-used-as-normaliser was itself a 2.0-2.3x
	// darkening defect, found and fixed this session) and it is NOT
	// energy-conserving, for reasons the source model shares: Sadeghi
	// et al. 2013 state plainly that their model "ignores the effect of
	// multiple scattering between different threads", and the masking
	// term (their Eq. 7-9) removes energy that nothing here puts back --
	// their own Eq. 15 reweighting normaliser Q would partially, and it
	// is not reciprocal (WeaveBRDF.h section 1d; docs/CLOTH_FABRIC_
	// DESIGN.md 10.3 debt 7 records the resulting grazing-only loss), so
	// it is not used.  A kPosturePass row would therefore be asserting
	// something the model does not claim.
	//
	// WHAT THE BOUNDED POSTURE ALONE WOULD MISS, and hence the locked
	// curve below it: a [0, 1.05] band cannot tell a working weave from
	// one that has quietly gone dark -- exactly the failure mode the
	// `C_v` defect above produced, undetected, until an outside review
	// integrated the BRDF independently of this harness.  So the
	// measured curve is LOCKED as a second, synthesized row, AND (right
	// after it) four more rows measure WHITE-DYED presets against a
	// closed-form prediction computed independently of `value()` --
	// see the "independent white-weave energy floor" block below, which
	// is what actually would have caught the `C_v` defect: the locked
	// curve only compares today's number against yesterday's, and
	// yesterday's was itself the bug.
	//
	// The 9th row is FABRIC OVER WEAVE: the Phase-1 fuzz layer on the
	// Phase-2 substrate, which is the composition the allowlist was
	// extended for and the one an author actually ships.  It must also
	// stay bounded -- the Charlie layer ADDS its own lobe and scales the
	// substrate down, and a scaling that failed to compensate would show
	// here as a row above 1.
	// ================================================================

	struct WeaveRow { const char* preset; double rotationDeg; };
	static const WeaveRow kWeaveRows[] = {
		{ "denim", 0.0 }, { "denim", 45.0 },
		{ "silk",  0.0 }, { "silk",  45.0 },
		{ "satin", 0.0 }, { "satin", 45.0 },
		{ "linen", 0.0 }, { "linen", 45.0 },
	};
	const int kNumWeaveRows = (int)( sizeof(kWeaveRows) / sizeof(kWeaveRows[0]) );

	//! THE LOCKED CURVE.  Measured 2026-09-03 at FURNACE_SAMPLES =
	//! 100000, printed by this very test; NOT a prediction from an
	//! independent model, and the difference matters -- these numbers
	//! say "the shipped presets still behave as they did", not "the
	//! model is right".  What says the model is right is the reciprocity
	//! sweep, the pdf integral and the energy bound, each of which
	//! checks a property rather than a value.
	//!
	//! eps 0.006 absolute, and it is NOT sized for Monte-Carlo noise:
	//! this harness seeds its own RNG deterministically, so three
	//! consecutive runs on one toolchain reproduce every digit above
	//! BIT-IDENTICALLY (verified 2026-09-03).  What the eps covers is
	//! cross-platform libm drift in the transcendentals this model leans
	//! on -- exp/log inside `Mp`'s Bessel branch, `acos` in the visible-
	//! azimuth bound, `atan2` in the fibre-frame projection -- amplified
	//! by the narrow lobes (satin's v is 1.9e-3).  0.006 is ~5 % of the
	//! smallest locked value and ~2.5 % of the largest, which is loose
	//! enough for that and far tighter than the [0, 1.05] band the
	//! Bounded posture applies.  A preset retune legitimately moves
	//! these; update them in the same commit and say so.
	//! THE @45 ROWS ARE MUCH DARKER AT GRAZING, AND THAT IS THE POINT.
	//! At rotation 0 the harness's incident directions lie in the plane
	//! containing the warp axis, so at theta = 80 the view runs almost
	//! ALONG the yarn: the fibre-frame cosines both collapse, the
	//! Chandrasekhar denominator 1/(cos theta_i + cos theta_o) blows up,
	//! and the surface returns its brightest.  Rotate the weave 45 deg
	//! and the same view is off-axis, the denominator relaxes, and the
	//! same fabric returns a quarter as much.  That gap -- 0.0996 vs
	//! 0.0265 on denim, 0.2051 vs 0.0633 on linen -- IS the yarn-aligned
	//! grazing sheen this material exists to produce, and a row set
	//! whose two view sets agreed would mean the fibre frame was not
	//! being used at all.
	//! RE-MEASURED THIS SESSION (2026-09-03, second pass).  The previous
	//! numbers here were captured BEFORE the `C_v` fix (P1-1 above) and
	//! were never re-measured after it landed -- they are ~2.0-3.0x the
	//! post-fix values, i.e. they silently locked in the very defect
	//! this file's other gates exist to catch.  Caught by the new
	//! independent white-weave floor check below (which does NOT derive
	//! from `value()`), not by this array, which is the point of having
	//! both.
	//!
	//! RE-MEASURED AGAIN, THIRD PASS (2026-09-03, P2R4 fix round): the
	//! masking-pole seam fix (`ProjectDir`'s pole conditioning +
	//! `SmoothedRamp`'s C1 masking gate, replacing the hard
	//! `max(cosPhi,0)` hinge) moves any configuration whose (wi,wo) pair
	//! passes near the masking term's geometric horizon `cosPhi = 0`,
	//! not only the ones near the coordinate pole -- so a few rows shift
	//! by a small, real amount even though none of their tilts are near
	//! the old defect's danger zone.  Only ONE row moved past
	//! `kWeaveLockEps`: satin (rotation 0) at theta=80, 0.2272 -> 0.2164
	//! (-4.8% relative, -0.0108 absolute) -- the untilted satin row's
	//! grazing configuration happens to sit close to that horizon at
	//! this harness's fixed theta=80 direction.  Every other row moved
	//! by <= 0.0002, well inside the existing eps.  All eight rows
	//! restated here for a clean baseline.
	static const double kWeaveLocked[8][NUM_THETA] = {
		{ 0.2047, 0.1983, 0.1819, 0.1810 },		// denim
		{ 0.2050, 0.1972, 0.1478, 0.0571 },		// denim, weave 45
		{ 0.4687, 0.4525, 0.4000, 0.3547 },		// silk
		{ 0.4704, 0.4371, 0.2928, 0.0792 },		// silk, weave 45
		{ 0.3916, 0.3672, 0.2992, 0.2164 },		// satin -- theta=80 moved, see note above
		{ 0.3928, 0.3606, 0.2340, 0.0589 },		// satin, weave 45
		{ 0.4974, 0.4797, 0.4241, 0.3885 },		// linen
		{ 0.4966, 0.4776, 0.3535, 0.1354 },		// linen, weave 45
	};
	const double kWeaveLockEps = 0.006;

	std::vector<RISE::WeaveTest::PresetWeave*> weaveFixtures;
	double weaveMeasured[8][NUM_THETA];

	for( int wr = 0; wr < kNumWeaveRows; ++wr )
	{
		const WeaveRow& row = kWeaveRows[wr];
		RISE::WeaveTest::PresetWeave* pw =
			new RISE::WeaveTest::PresetWeave( row.preset, row.rotationDeg * PI / 180.0 );
		weaveFixtures.push_back( pw );

		std::ostringstream nm;
		nm << ( 39 + wr ) << ". weave / " << row.preset;
		if( row.rotationDeg != 0 ) nm << " (weave_rotation " << (int)row.rotationDeg << " deg)";

		ConfigReport& r = add( nm.str(), kPostureBounded, 0.05,
			"Phase 2 slice P2-A: energy-BOUNDED by construction (Mp's d'Eon normalisation + the "
			"trimmed logistic's + the volume lobe's exact C_v hemispherical-integral normaliser), "
			"and deliberately not conserving -- the source model states it ignores inter-thread "
			"multiple scattering, and the masking term removes energy nothing puts back.  The "
			"measured curve is locked separately below" );
		Run( r, *pw->SPF() );
		for( int i = 0; i < NUM_THETA; ++i ) weaveMeasured[wr][i] = r.albedo[i];
	}

	//  ---- the locked-curve check, as one synthesized row.
	{
		std::cout << "\n";
		std::cout << "  Phase 2 -- LOCKED weave_material curves (measured 2026-09-03)\n";
		std::cout << "  eps " << kWeaveLockEps << " absolute, ~4 sigma of this harness's MC error\n";
		std::cout << "  " << std::left << std::setw( 34 ) << "config";
		for( int i = 0; i < NUM_THETA; ++i ) {
			std::ostringstream h; h << "th" << (int)THETA_DEG[i];
			std::cout << std::right << std::setw( 20 ) << h.str();
		}
		std::cout << "\n";

		bool lockPassed = true;
		for( int wr = 0; wr < kNumWeaveRows; ++wr )
		{
			std::ostringstream lbl;
			lbl << kWeaveRows[wr].preset;
			if( kWeaveRows[wr].rotationDeg != 0 ) lbl << " @45";
			std::cout << "  " << std::left << std::setw( 34 ) << lbl.str();
			for( int i = 0; i < NUM_THETA; ++i ) {
				const double d = std::fabs( weaveMeasured[wr][i] - kWeaveLocked[wr][i] );
				const bool   ok = ( d <= kWeaveLockEps );
				if( !ok ) lockPassed = false;
				std::ostringstream cell;
				cell << std::fixed << std::setprecision( 4 )
				     << weaveMeasured[wr][i] << "/" << kWeaveLocked[wr][i] << ( ok ? "" : " !" );
				std::cout << std::right << std::setw( 20 ) << cell.str();
			}
			std::cout << "\n";
		}
		if( !lockPassed ) {
			std::cout << "    FAIL: a weave_material row moved off its locked curve by more than "
			          << kWeaveLockEps << " (marked with ! above).  If this was a deliberate preset "
			             "or model retune, update kWeaveLocked in the same commit and say so in the "
			             "message; if it was not, something changed the BSDF.\n";
		}
		// Folded into the suite's own tally as one extra unit.  The
		// albedo columns on THIS row are a pass/fail indicator, not
		// measurements -- the real numbers are in the table above.
		ConfigReport& r = add( "47. Phase-2 locked weave curves", kPosturePass, 0.01,
			"pass/fail indicator only -- the measured/locked pairs are in the table printed above "
			"the report" );
		for( int i = 0; i < NUM_THETA; ++i ) r.albedo[i] = lockPassed ? 1.0 : 0.0;
		r.passed = lockPassed;
	}

	//  ---- fabric OVER weave: the composition the allowlist exists for.
	UniformScalarPainter* fowAlpha = new UniformScalarPainter( 0.3 );  fowAlpha->addref();
	RISE::WeaveTest::PresetWeave fowBase( "satin" );
	FabricMaterial* fabricOverWeave = new FabricMaterial(
		*fowBase.Material(), *one, *fowAlpha, *sWeave0 );
	fabricOverWeave->addref();
	{
		ConfigReport& r = add( "48. fabric (white sheen, alpha 0.3) / weave satin",
			kPostureBounded, 0.05,
			"Phase 2: the fuzz-over-weave stack the substrate allowlist was extended for.  Bounded, "
			"not Pass: the Kulla-Conty product form conserves EXACTLY only over a Lambertian base, "
			"and this substrate is neither Lambertian nor conserving itself -- the fabric layer "
			"inherits the weave's own deficit and adds the sheen lobe's share on top, which is "
			"exactly what the row is here to show stays under 1" );
		Run( r, *fabricOverWeave->GetSPF() );
	}

	//  ---- 49. THE INDEPENDENT WHITE-WEAVE ENERGY FLOOR -- the check
	//  that would have caught the `C_v = 2(1+k_d)` defect, since neither
	//  the locked curve above (which pinned the buggy number) nor
	//  `hemisphericalAlbedo`'s 20% band (which is derived assuming the
	//  BRDF's own normalisers are exact) can.  See
	//  `WeaveIndependentCheck` above main() for the from-scratch
	//  reimplementation this compares against.
	//
	//  FOUR PRESETS, both dyes forced to white, coverage forced to the
	//  draft's own mean (so the prediction does not depend on where in
	//  the cell the harness's fixed shading point happens to land),
	//  measured at theta = 0 and 30 deg only -- the independent
	//  quadrature above does not reproduce `value()`'s
	//  `AzimuthalTrimBoost` visible-interval renormalisation (a <= ~8%
	//  correction confined to the surface lobe, P2-2), and that
	//  correction grows toward grazing as the visible azimuthal range
	//  narrows, so 60/80 deg are left to the locked curve and the
	//  reciprocity/pdf gates instead of being asserted here.
	//
	//  TWO INDEPENDENT ASSERTIONS per (preset, angle):
	//    (i)  |measured - predicted| <= kWhiteFloorEps -- catches an
	//         amplitude bug (the C_v class) even though both sides use
	//         DIFFERENT code (Monte-Carlo Scatter() vs a from-scratch
	//         quadrature).
	//    (ii) measured >= kWhiteFloorMin (0.55) at theta <= 30 -- the
	//         literal physical floor: a lossless-ish dielectric weave
	//         (eta 1.35-1.54, k_d 0.1-0.7) cannot legitimately return an
	//         order of magnitude under Sadeghi's own measured 0.5-0.8
	//         band for white fabrics, so a future regression that
	//         quietly halves the volume lobe again fails HERE even if
	//         it also moved the independent prediction (a shared-root
	//         bug in `FibreLobeMath.h` could, in principle, move both).
	{
		static const char* const kWhitePresets[] = { "denim", "silk", "satin", "linen" };
		const int kNumWhitePresets = (int)( sizeof( kWhitePresets ) / sizeof( kWhitePresets[0] ) );
		static const double kWhiteThetas[2] = { 0.0, 30.0 };
		const double kWhiteFloorEps = 0.04;
		const double kWhiteFloorMin = 0.55;

		std::cout << "\n";
		std::cout << "  Phase 2 -- INDEPENDENT white-weave energy floor (from-scratch quadrature)\n";
		std::cout << "  eps " << kWhiteFloorEps << " absolute vs the independent prediction; "
		          << "floor " << kWhiteFloorMin << " at theta <= 30\n";

		bool whiteFloorPassed = true;
		for( int p = 0; p < kNumWhitePresets; ++p )
		{
			const RISE::Implementation::WeavePreset& P =
				RISE::Implementation::LookupWeavePreset( kWhitePresets[p] );
			const double meanCov = (double)RISE::Implementation::WeavePatternMeanCoverage( P.weave );

			RISE::WeaveTest::PresetWeave white( kWhitePresets[p], 0.0, meanCov, /*whiteDyes=*/true );

			std::cout << "  " << std::left << std::setw( 10 ) << kWhitePresets[p];
			for( int i = 0; i < 2; ++i )
			{
				const double measured  = DirectionalAlbedo( *white.SPF(), kWhiteThetas[i] * PI / 180.0 );
				const double predicted = WeaveIndependentCheck::PredictWhiteRho( P, meanCov, kWhiteThetas[i] );
				const double diff      = std::fabs( measured - predicted );
				const bool   predOk    = diff <= kWhiteFloorEps;
				const bool   floorOk   = ( kWhiteThetas[i] > 30.0 ) || ( measured >= kWhiteFloorMin );
				if( !predOk || !floorOk ) whiteFloorPassed = false;

				std::ostringstream cell;
				cell << std::fixed << std::setprecision( 4 )
				     << "th" << (int)kWhiteThetas[i] << "=" << measured << "/" << predicted
				     << ( predOk ? "" : " !pred" ) << ( floorOk ? "" : " !floor" );
				std::cout << "  " << std::setw( 26 ) << cell.str();
			}
			std::cout << "\n";
		}
		if( !whiteFloorPassed ) {
			std::cout << "    FAIL: a white-weave row missed the independent prediction (!pred) or fell "
			             "below the 0.55 physical floor (!floor) -- see the annotated cells above.\n";
		}
		ConfigReport& r = add( "49. Phase-2 white-weave independent energy floor", kPosturePass, 0.01,
			"pass/fail indicator only -- measured/predicted pairs are in the table printed above" );
		for( int i = 0; i < NUM_THETA; ++i ) r.albedo[i] = whiteFloorPassed ? 1.0 : 0.0;
		r.passed = whiteFloorPassed;
	}

	PrintReport( reports );

	// Tally pass/fail across the suite.
	int failures = 0;
	for( const ConfigReport& r : reports ) if( !r.passed ) ++failures;

	std::cout << "\n";
	std::cout << failures << " of " << reports.size() << " configurations failed.\n";
	if( failures > 0 ) {
		std::cout << "(see docs/PHYSICALLY_BASED_PIPELINE_PLAN.md Landing 6 for the\n"
		             " disposition of each known failure pattern)\n";
	}

	// Cleanup (matches existing test pattern; not strictly necessary
	// for a one-shot test process but exercises the destructor chain).
	safe_release( fabricOverWeave );
	safe_release( fowAlpha );
	for( RISE::WeaveTest::PresetWeave* pw : weaveFixtures ) delete pw;
	for( FabricMaterial* fm : fabricMats )            safe_release( fm );
	for( UniformScalarPainter* sp : fabricAlphaPnts ) safe_release( sp );
	safe_release( sWeave45 );
	safe_release( sWeave0 );
	safe_release( anisoGgxMat );
	safe_release( sAnisoY );
	safe_release( sAnisoX );
	safe_release( whiteOnMat );
	safe_release( sOnSigma );
	safe_release( coatedWaterNoRecycle );
	safe_release( coatedClearcoatRedGgx );
	safe_release( coatedClearcoatWhiteGgx );
	safe_release( coatedWaterHalfCover );
	safe_release( coatedWaterWhiteLamb );
	safe_release( coatedVarnishWhiteLamb );
	safe_release( sCoatRough );
	safe_release( sCoatHalf );
	safe_release( sCoatFull );
	safe_release( redGgxMat );
	safe_release( whiteGgxMat );
	safe_release( whiteLambMat );
	safe_release( compClearcoatRedPbr );
	safe_release( compSheenPbr );
	safe_release( compGgxPbr );
	safe_release( compGgxLamb );
	safe_release( compDielLamb );
	safe_release( clearcoatDiel );
	safe_release( ggxRedDiff );
	safe_release( ggxOnly );
	safe_release( polishedTau0_9 );
	safe_release( polishedTau0_5 );
	safe_release( polishedTau1_0 );
	safe_release( dielectric );
	safe_release( sheen );
	safe_release( ggxPBR );
	safe_release( lambertian );
	safe_release( sheenR );
	safe_release( alpha );
	safe_release( dielF0 );
	safe_release( redDiff );
	safe_release( iorPnt );
	safe_release( zero );
	safe_release( one );
	safe_release( sOne );
	safe_release( sZero );
	safe_release( sIor );
	safe_release( sTau1_0 );
	safe_release( sTau0_5 );
	safe_release( sTau0_9 );
	safe_release( sIor133 );
	safe_release( sScat200k );
	safe_release( g_stubObject );

	return ( failures > 0 ) ? 1 : 0;
}
