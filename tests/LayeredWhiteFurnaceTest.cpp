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
#include "../src/Library/Painters/UniformColorPainter.h"

#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Materials/SheenSPF.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/Materials/CompositeSPF.h"

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
	kPosturePass,				// ρ ≈ 1 within tolerance.  Failure is a regression (gain OR loss).
	kPostureEnergyConserving,	// ρ ≤ 1 + tolerance.  Lower bound NOT enforced.  Use ONLY
								// when sub-unity is the legitimate physical signature and
								// any value below unity is acceptable: V-cavities masking-
								// shadowing on Charlie sheen dissipates inter-fiber
								// energy by physical design; the integrated albedo is
								// determined by the BRDF, not by an external constraint.
								// **Do NOT use** for configurations where sub-unity is a
								// known-but-not-PB-correct under-conservation (recursion
								// budget loss, missing-base-layer composites, etc.) —
								// those need kPostureRangeCheck or kPostureKnownFailure
								// so a future regression toward "even darker" is caught.
	kPostureRangeCheck,			// ρ ∈ [expected - tolerance, expected + tolerance] per angle.
								// Locks in the current measured behaviour so a regression
								// in either direction surfaces as a test failure.  Use
								// for configurations whose ρ is intentionally NOT 1.0
								// (architectural / under-conservation that the audit
								// isn't claiming to fully fix) but whose CURRENT numbers
								// represent a real improvement worth gating.  The
								// `expectedAlbedo[]` array carries the locked-in baseline.
	kPostureKnownFailure		// Documented non-conservation; record numbers but don't fail.
								// Use when even a per-angle baseline would be misleading —
								// e.g. the configuration is structurally broken (#6 sheen-
								// over-base composite never invokes the base) and the
								// numbers will jump to a different regime once the
								// structural fix lands.
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
	double incomingThetaRad )
{
	RayIntersectionGeometric ri = MakeIntersection( incomingThetaRad );
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	const Vector3 normal = ri.onb.w();
	double sum = 0;

	for( int i = 0; i < FURNACE_SAMPLES; ++i )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );

		// Per-sample throughput is the sum of kray over every ray
		// that actually escapes the upper hemisphere.  A trial that
		// scatters nothing (Count()==0) or only emits below-surface
		// rays contributes ZERO — energy that gets absorbed inside
		// the material or trapped behind a recursion budget is a
		// real loss and must be visible in the directional-albedo
		// estimate.  An earlier revision divided by `validSamples`
		// (trials with at least one upward escape), which made the
		// test compute  E[throughput | escape]  instead of
		// E[throughput]  — that conditional mean would let a
		// regression that drove a layered material toward "very
		// dark but the few escape paths still under unity" sail past
		// the energy-conservation gate.  Both numerator and
		// denominator must see every trial.
		double sampleContrib = 0;

		for( unsigned int j = 0; j < scattered.Count(); ++j )
		{
			const ScatteredRay& scat = scattered[j];

			// Include BOTH delta and non-delta rays.  For energy
			// conservation, kray is the fraction of incoming
			// irradiance carried by this scattered ray regardless
			// of measure: delta rays carry an integer fraction
			// (e.g. F for a perfect mirror); non-delta rays carry
			// BSDF·cos/pdf which integrates to directional albedo
			// across many samples.
			//
			// Skip only the reflected-into-substrate hemisphere
			// (cosWo ≤ 0): those are below-surface paths the
			// integrator wouldn't propagate, and they are the
			// source of the "trapped energy" that the per-sample
			// zero contribution above is meant to capture.
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
			}
		}

		sum += sampleContrib;
	}

	return sum / static_cast<double>( FURNACE_SAMPLES );
}

// ============================================================
//  Pretty printing + pass/fail accumulation
// ============================================================

struct ConfigReport
{
	std::string  name;
	AuditPosture posture;
	double       tolerance;					// see posture comment for what this gates
	double       albedo[NUM_THETA];			// per-incident-angle directional albedo
	double       expectedAlbedo[NUM_THETA];	// per-angle baseline for kPostureRangeCheck
	bool         passed;					// only meaningful when posture != kPostureKnownFailure
	std::string  note;
};

static void Run( ConfigReport& r, ISPF& spf )
{
	r.passed = true;
	for( int i = 0; i < NUM_THETA; ++i )
	{
		const double rad = THETA_DEG[i] * PI / 180.0;
		r.albedo[i] = DirectionalAlbedo( spf, rad );

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
		else if( r.posture == kPostureEnergyConserving )
		{
			// One-sided check: BRDF must not gain energy.  Loss is
			// allowed (and expected) for the materials in this bucket.
			if( r.albedo[i] > 1.0 + r.tolerance ) {
				r.passed = false;
				if( r.note.empty() ) r.note = "GAIN (regression)";
			}
		}
		else if( r.posture == kPostureRangeCheck )
		{
			// Two-sided lock-in around the measured baseline.  Catches
			// regressions in BOTH directions (gain OR loss) for a
			// configuration whose physical answer isn't 1.0.
			if( r.albedo[i] > r.expectedAlbedo[i] + r.tolerance ) {
				r.passed = false;
				if( r.note.empty() ) r.note = "above expected band (regression)";
			}
			else if( r.albedo[i] < r.expectedAlbedo[i] - r.tolerance ) {
				r.passed = false;
				if( r.note.empty() ) r.note = "below expected band (regression)";
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
		if( r.posture == kPosturePass ) {
			std::cout << "  " << ( r.passed ? "PASS" : "FAIL" );
			if( !r.note.empty() ) std::cout << " (" << r.note << ")";
		} else if( r.posture == kPostureEnergyConserving ) {
			std::cout << "  " << ( r.passed ? "PASS-EC" : "FAIL" );
			if( !r.note.empty() ) std::cout << " (" << r.note << ")";
		} else if( r.posture == kPostureRangeCheck ) {
			std::cout << "  " << ( r.passed ? "PASS-RC" : "FAIL" );
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

	// Roughness ≈ 0.4 (α = 0.16) puts GGX in a regime where
	// Kulla-Conty's multi-scatter compensation matters but doesn't
	// dominate.  Same value is used by the existing
	// scenes/Tests/Materials/ggx_white_furnace.RISEscene comparison
	// surface, so any L6 finding is comparable to the established
	// single-material baseline.
	UniformColorPainter* alpha    = new UniformColorPainter( RISEPel( 0.16, 0.16, 0.16 ) );  alpha->addref();

	// Sheen roughness in the middle of the legal [0, 1] range.
	// Charlie distribution's energy loss at grazing depends on
	// roughness; 0.5 is broadly representative of fabric assets.
	UniformColorPainter* sheenR   = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  sheenR->addref();

	// "Perfect dielectric" scattering value for DielectricSPF.  The
	// SPF's internal Phong-perturbation gate is `if (scatfunc < 1e6)
	// perturb` — so any value at or above 1e6 yields true delta
	// refraction.  Using *zero* here (as an earlier revision of this
	// test did) silently routes through the perturbation branch with
	// `alpha = acos(rand)` (uniform-cosine), turning the dielectric
	// into a wide-cone scatterer and dropping any perturbed-into-front
	// hemisphere refractions wholesale via DielectricSPF's
	// `bDielectric = false` guard.  That was the dominant component
	// of Landing 6's reported 96 % loss for the dielectric/Lambertian
	// audit configuration before this fix.  Scene-language default for
	// `dielectric_material.scattering` is "10000" — we use 1e7 to be
	// unambiguously above the 1e6 perturbation cutoff.
	UniformColorPainter* perfectScat = new UniformColorPainter( RISEPel( 1e7, 1e7, 1e7 ) );  perfectScat->addref();

	// ---------- Build SPFs ----------

	LambertianSPF* lambertian = new LambertianSPF( *one );  lambertian->addref();

	// GGX-PBR (white inputs, schlick_f0 — glTF-MR convention).
	// rd = baseColor (unity here) acts as albedo; rs = baseColor
	// acts as F0 directly under schlick_f0; (1 - max(F0)) is
	// applied inside the BSDF for the diffuse weight.
	GGXSPF* ggxPBR = new GGXSPF(
		*one, *one, *alpha, *alpha, *iorPnt, *zero, eFresnelSchlickF0 );
	ggxPBR->addref();

	// Sheen alone.
	SheenSPF* sheen = new SheenSPF( *one, *sheenR );  sheen->addref();

	// Dielectric "clear coat" with full transmission (tau = 1) and a
	// delta-like scattering exponent (perfectScat = 1e7) so the
	// refraction is a true perfect-dielectric event, not a Phong-
	// perturbed cone.  See the perfectScat comment above for why
	// passing zero would silently break this configuration.
	DielectricSPF* dielectric = new DielectricSPF( *one, *iorPnt, *perfectScat, /*hg*/ false );
	dielectric->addref();

	// GGX-only top layer for clearcoat-style composite.
	GGXSPF* ggxOnly = new GGXSPF(
		*zero, *one, *alpha, *alpha, *iorPnt, *zero, eFresnelSchlickF0 );
	ggxOnly->addref();

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

	// 2. Sheen alone (Charlie distribution, V-cavities Λ-based visibility).
	//    Energy-conserving by construction (Imageworks 2017 / glTF spec):
	//    G2 ∈ [0, 1] caps the visibility, V is clamped to ≤ 1, and the
	//    inter-fiber masking dissipates energy below unity.  Sub-unity
	//    directional albedo IS the physical signature of V-cavities sheen
	//    on a fuzzy fiber distribution; we assert energy-conservation
	//    (ρ ≤ 1) and let the lower bound float.  Pre-fix this configuration
	//    diverged at grazing (ρ(80°) = 8.7 with the Estevez-Kulla form).
	{ ConfigReport& r = add( "2. Sheen alone (Charlie + V-cavities)", kPostureEnergyConserving, 0.05, 0 );
	  Run( r, *sheen ); }

	// 3. Composite: dielectric over Lambertian.  Pre-fix this lost ~96 %
	//    of energy because (a) the test fixture passed scattering=0 to
	//    DielectricSPF (uniform-Phong perturbation, not delta refraction),
	//    and (b) CompositeSPF's random walk dropped the IOR stack across
	//    inter-layer transitions, evaluating the second-pass dielectric
	//    crossing as if it were entering glass from outside instead of
	//    exiting from inside.  Both fixed; ρ now bounded by 1 at every
	//    angle.  A residual ~30-50 % loss remains (ρ(0°) ≈ 0.43): the
	//    cosine-weighted Lambertian above the substrate sends ~44 % of
	//    its samples into the TIR cone of the dielectric/air interface,
	//    and the random walk's `max_recur = 4` budget cuts the resulting
	//    multi-bounce inter-reflection chain too short to fully release
	//    that energy.  This is an architectural property of the random-
	//    walk model, not a bug in the dielectric or the Lambertian; it
	//    would take A2 (analytic Belcour-style layered BSDF) to close.
	//
	//    Posture is kPostureRangeCheck — the current numbers represent
	//    the post-A.1+A.2 fix and are NOT energy-conserving in the
	//    "ρ ≈ 1" sense, but they ARE the locked-in regression baseline.
	//    A future change that pushes ρ further toward zero (regressing
	//    the IOR-stack-propagation fix, for example) would slip past a
	//    one-sided ρ ≤ 1 + tol gate; the two-sided range check catches
	//    it.  Expected values below are the measured baseline at FURNACE_
	//    SAMPLES = 100000 (RNG seed-independent within MC noise of ~3e-3).
	//    Tolerance 0.05 absolute leaves ~16σ of headroom for noise while
	//    catching >5 pp regressions in either direction.
	{ ConfigReport& r = add( "3. Dielectric / Lambertian", kPostureRangeCheck, 0.05,
	    "TIR-cone+recursion-budget under-conservation, see comment" );
	  r.expectedAlbedo[0] = 0.428;	// θ = 0°
	  r.expectedAlbedo[1] = 0.429;	// θ = 30°
	  r.expectedAlbedo[2] = 0.458;	// θ = 60°
	  r.expectedAlbedo[3] = 0.635;	// θ = 80°
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

	// 6. Composite: Sheen top over GGX-PBR base.  Pre-fix this was
	//    KNOWN-FAILURE — CompositeSPF's random walk only delegated to
	//    the base when the top emitted a downward ray, but SheenSPF's
	//    cosine-hemisphere sampling always emits UP, so ggxPBR was
	//    never evaluated and the composite collapsed to standalone
	//    sheen (numbers tracked config #2 within MC noise).
	//
	//    Closed via the Khronos additive composition in
	//    CompositeSPF::Scatter (and CompositeBRDF for direct lighting):
	//
	//      f_combined = f_sheen + f_base · (1 − sheenColor · E_sheen)
	//
	//    where E_sheen is the sheen-BRDF directional albedo from the
	//    LUT exposed via SheenSPF::AlbedoLookup.  The composite now
	//    invokes BOTH layers per Scatter call: the top's natural
	//    sheen response plus the attenuated base's response.  With
	//    white inputs throughout, ρ ≈ 1 at every angle (the base's
	//    GGX-PBR Kulla-Conty over-correction shows through unchanged
	//    because the sheen lobe's sub-unity albedo leaves room for
	//    it).  Tolerance 5 % matches config #1.
	{ ConfigReport& r = add( "6. Sheen / GGX-PBR (sheen over PBR)", kPosturePass, 0.05, 0 );
	  Run( r, *compSheenPbr ); }

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
	safe_release( compSheenPbr );
	safe_release( compGgxPbr );
	safe_release( compGgxLamb );
	safe_release( compDielLamb );
	safe_release( ggxOnly );
	safe_release( dielectric );
	safe_release( sheen );
	safe_release( ggxPBR );
	safe_release( lambertian );
	safe_release( perfectScat );
	safe_release( sheenR );
	safe_release( alpha );
	safe_release( iorPnt );
	safe_release( zero );
	safe_release( one );
	safe_release( g_stubObject );

	return ( failures > 0 ) ? 1 : 0;
}
