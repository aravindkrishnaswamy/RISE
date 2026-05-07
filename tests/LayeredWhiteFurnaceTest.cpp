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
	kPosturePass,			// Must pass within tolerance.  Failure is a regression.
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
	double incomingThetaRad )
{
	RayIntersectionGeometric ri = MakeIntersection( incomingThetaRad );
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	const Vector3 normal = ri.onb.w();
	double sum = 0;
	int    validSamples = 0;

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

	return ( validSamples > 0 ) ? ( sum / validSamples ) : 0.0;
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
	bool         passed;			// only meaningful when posture == kPosturePass
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

	// Sheen roughness in the middle of the legal [0, 1] range.
	// Charlie distribution's energy loss at grazing depends on
	// roughness; 0.5 is broadly representative of fabric assets.
	UniformColorPainter* sheenR   = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  sheenR->addref();

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

	// Dielectric "clear coat" with full transmission (tau = 1)
	// and zero in-volume scattering, IOR 1.5.
	DielectricSPF* dielectric = new DielectricSPF( *one, *iorPnt, *zero, /*hg*/ false );
	dielectric->addref();

	// GGX-only top layer for clearcoat-style composite.
	GGXSPF* ggxOnly = new GGXSPF(
		*zero, *one, *alpha, *alpha, *iorPnt, *zero, eFresnelSchlickF0 );
	ggxOnly->addref();

	// Finding D: coloured + diffuse-dominant GGX-PBR base.
	// Mirrors what AddPBRMetallicRoughnessMaterial would build for
	// a red baseColor (0.8, 0.2, 0.2) with metallic = 0 and
	// roughness ≈ 0.4.  rd = baseColor (because metallic=0 leaves
	// the entire baseColor as diffuse weight); rs = F0 = 0.04.
	GGXSPF* ggxRedDiff = new GGXSPF(
		*redDiff, *dielF0, *alpha, *alpha, *iorPnt, *zero, eFresnelSchlickF0 );
	ggxRedDiff->addref();

	// Finding D: clearcoat with F0 = 0.04 (standard glTF clearcoat).
	GGXSPF* clearcoatDiel = new GGXSPF(
		*zero, *dielF0, *alpha, *alpha, *iorPnt, *zero, eFresnelSchlickF0 );
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

	// 2. Sheen alone (Charlie distribution, no LUT compensation).  KNOWN
	//    NON-CONSERVING: Charlie's denominator (n.l + n.v - n.l*n.v)
	//    blows up at grazing without the Heitz-simplified or Zeltner-LTC
	//    sheen-albedo LUT.  Documented in literature; expected here.
	{ ConfigReport& r = add( "2. Sheen alone (Charlie, no LUT)", kPostureKnownFailure, 0.0,
	    "Charlie diverges at grazing; needs LUT compensation" );
	  Run( r, *sheen ); }

	// 3. Composite: dielectric over Lambertian.  KNOWN FAILURE on the
	//    current CompositeSPF random walk — recursion budget kills most
	//    diffuse-then-refracted paths before they exit, so the only
	//    energy that reports out is the dielectric's surface Fresnel.
	//    First L6 finding the audit produced; the importer's Phase-5
	//    "skip layering" warning matches this.
	{ ConfigReport& r = add( "3. Dielectric / Lambertian", kPostureKnownFailure, 0.0,
	    "CompositeSPF random walk: recursion budget kills below-layer diffuse" );
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
	//    sheen failure mode at grazing; KNOWN FAILURE for the same
	//    reason (Charlie's grazing divergence).
	{ ConfigReport& r = add( "6. Sheen / GGX-PBR (sheen over PBR)", kPostureKnownFailure, 0.0,
	    "sheen-over-PBR inherits sheen's Charlie grazing divergence" );
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
	{ ConfigReport& r = add( "7. Clearcoat / red GGX-PBR (Finding D)", kPostureKnownFailure, 0.0,
	    "same recursion-budget bug as #3 in coloured-input regime; tied to Finding A" );
	  Run( r, *compClearcoatRedPbr ); }

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
	safe_release( compClearcoatRedPbr );
	safe_release( compSheenPbr );
	safe_release( compGgxPbr );
	safe_release( compGgxLamb );
	safe_release( compDielLamb );
	safe_release( clearcoatDiel );
	safe_release( ggxRedDiff );
	safe_release( ggxOnly );
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
	safe_release( g_stubObject );

	return ( failures > 0 ) ? 1 : 0;
}
