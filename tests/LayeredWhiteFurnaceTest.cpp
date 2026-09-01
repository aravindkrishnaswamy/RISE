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
	// Lambert absorption between layers.
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
		kThickness, *zero );
	compDielLamb->addref();

	CompositeSPF* compGgxLamb = new CompositeSPF(
		*ggxOnly, *lambertian,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zero );
	compGgxLamb->addref();

	CompositeSPF* compGgxPbr = new CompositeSPF(
		*ggxOnly, *ggxPBR,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zero );
	compGgxPbr->addref();

	CompositeSPF* compSheenPbr = new CompositeSPF(
		*sheen, *ggxPBR,
		kMaxRecur, kMaxReflectRecur, kMaxRefractRecur,
		kMaxDiffuseRecur, kMaxTranslucent,
		kThickness, *zero );
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
		kThickness, *zero );
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
	//    `thickness` were completely inert.  Fixed via
	//    CompositeSPF::EffectiveStack; guarded by tests/CompositeExtinctionTest.
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
	    "post-EffectiveStack recovery locked in: predicted rho={0.3339,0.3044,0.3128,0.5324} "
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

	// 6. Composite: Sheen top over GGX-PBR base.  Tracks the standalone-
	//    sheen behaviour after the Imageworks-Charlie fix (config #2).
	//    Still bounded above by 1 (no longer a known failure), but the
	//    composite walk amplifies sheen's intrinsic energy dissipation
	//    so ρ stays well under unity even at θ=0.  5% tolerance.
	{ ConfigReport& r = add( "6. Sheen / GGX-PBR (sheen over PBR)", kPostureBounded, 0.05,
	    "Energy-bounded; sheen-over-PBR inherits sheen's bounded dissipation" );
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
