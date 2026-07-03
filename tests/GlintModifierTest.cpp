//////////////////////////////////////////////////////////////////////
//
//  GlintModifierTest.cpp - Validates the discrete-facet glint normal
//    modifier (src/Library/Modifiers/GlintModifier.{h,cpp}):
//
//    1. Determinism + view-independence: the facet at a surface point
//       is a pure function of the object-space point (object-space
//       stability contract — flecks are pinned to the surface).
//    2. Coverage statistics: facet area fraction is zero at coverage 0,
//       scales linearly with `coverage`, scales ~cubically with `fill`
//       (3D sphere-slice), and matches the Poisson-approx absolute
//       bound at coverage=1/fill=1.
//    3. Tilt statistics: mean tilt tracks the Rayleigh mean
//       sigma*sqrt(pi/2); no tilt beyond the 60-degree safety ceiling;
//       azimuths are not clustered.
//    4. Cell-boundary integrity: a facet's disc never clips at cell
//       boundaries (the 3x3x3 neighbourhood search) — points inside a
//       facet's radius return that same facet from EITHER side of a
//       cell wall.
//    5. Modify() guards: perturbed normals are unit length, never tilt
//       past the geometric tangent plane guard, and the rebuilt ONB is
//       orthonormal with the tangent direction preserved (u stays in
//       the plane spanned by old u and the new normal).
//    6. GGX SPF/BRDF consistency at modifier-perturbed hits: sampling
//       pdf self-consistency and SPF-vs-BRDF albedo agreement hold at
//       the perturbed frame — the "MIS-consistent by construction"
//       claim, tested rather than asserted.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Modifiers/GlintModifier.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Interfaces/ILog.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_failures = 0;

#define CHECK( cond, msg )													\
	do {																	\
		if( !(cond) ) {														\
			std::cout << "  FAIL: " << msg << std::endl;					\
			g_failures++;													\
		}																	\
	} while( 0 )

// ============================================================
//  Helpers
// ============================================================

//! GlintModifier has a protected destructor (Reference pattern); tests
//! heap-allocate and release the whole set at exit.
static std::vector<GlintModifier*> g_mods;
static GlintModifier* MakeMod(
	Scalar density, Scalar coverage, Scalar fill, Scalar spreadDeg,
	const Vector3& scale, const Vector3& shift, unsigned int seed )
{
	GlintModifier* p = new GlintModifier( density, coverage, fill, spreadDeg, scale, shift, seed );
	p->addref();
	g_mods.push_back( p );
	return p;
}
static void ReleaseMods()
{
	for( size_t i = 0; i < g_mods.size(); i++ ) g_mods[i]->release();
	g_mods.clear();
}

//! A flat +Z surface hit at the given object-space point, arriving
//! from `dir` (must have negative z).  vGeomNormal == vNormal == +Z.
static RayIntersectionGeometric MakeRI( const Point3& objPt, const Vector3& dir )
{
	Ray inRay( Point3( objPt.x - dir.x, objPt.y - dir.y, objPt.z - dir.z ), dir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = objPt;
	ri.ptObjIntersec = objPt;
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );

	return ri;
}

//! Fraction of uniformly random 3D points covered by a facet.
static double MeasureCoverage( const GlintModifier& mod, int samples, unsigned int rngSeed )
{
	RandomNumberGenerator rng( rngSeed );
	int covered = 0;
	for( int i = 0; i < samples; i++ )
	{
		const Point3 p(
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0 );
		if( mod.FindFacet( p ).found ) {
			covered++;
		}
	}
	return (double)covered / (double)samples;
}

// ============================================================
//  Test 1: Determinism + view-independence
// ============================================================

static void TestDeterminism()
{
	std::cout << "--- Test 1: Determinism + view-independence ---" << std::endl;

	const GlintModifier& mod = *MakeMod( 2.0, 0.8, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 );

	RandomNumberGenerator rng( 11 );
	int facetsSeen = 0;

	for( int i = 0; i < 2000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 30.0, rng.CanonicalRandom() * 30.0, 0.0 );

		// Two different view rays at the SAME surface point.
		RayIntersectionGeometric riA = MakeRI( p, Vector3Ops::Normalize( Vector3(  0.3, 0.1, -1 ) ) );
		RayIntersectionGeometric riB = MakeRI( p, Vector3Ops::Normalize( Vector3( -0.6, 0.4, -0.7 ) ) );

		mod.Modify( riA );
		mod.Modify( riB );

		const Scalar dN = Vector3Ops::Dot( riA.vNormal, riB.vNormal );
		CHECK( dN > 1.0 - 1e-12, "perturbed normal differs under view change at same point (i=" << i << ", dot=" << dN << ")" );

		if( mod.FindFacet( p ).found ) {
			facetsSeen++;

			// Repeated query is bit-stable.
			const GlintFacet f1 = mod.FindFacet( p );
			const GlintFacet f2 = mod.FindFacet( p );
			CHECK( f1.centreQ.x == f2.centreQ.x && f1.theta == f2.theta && f1.phi == f2.phi,
				"FindFacet not bit-deterministic" );
		}
	}

	CHECK( facetsSeen > 100, "expected a substantial facet population (saw " << facetsSeen << "/2000)" );
	std::cout << "  facets on " << facetsSeen << "/2000 sample points" << std::endl;
}

// ============================================================
//  Test 2: Coverage statistics
// ============================================================

static void TestCoverage()
{
	std::cout << "--- Test 2: Coverage statistics ---" << std::endl;

	const int N = 200000;

	// coverage = 0 -> exactly no facets.
	{
		const GlintModifier& mod = *MakeMod( 2.0, 0.0, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 3 );
		const double c = MeasureCoverage( mod, 20000, 101 );
		CHECK( c == 0.0, "coverage=0 must yield zero facets (got " << c << ")" );
	}

	// Absolute bound at coverage=1, fill=1: one sphere of radius 0.5
	// per unit cell covers a fraction ~1-exp(-pi/6) ~ 0.41 of space
	// (Poisson approximation of the jittered-grid overlap).
	const GlintModifier& modFull = *MakeMod( 2.0, 1.0, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 3 );
	const double cFull = MeasureCoverage( modFull, N, 102 );
	std::cout << "  coverage(1.0, fill=1): " << std::fixed << std::setprecision(4) << cFull << std::endl;
	CHECK( cFull > 0.30 && cFull < 0.52, "full coverage out of expected band (got " << cFull << ")" );

	// Linear scaling in `coverage` (sparse regime -> overlap negligible).
	const GlintModifier& modC30 = *MakeMod( 2.0, 0.30, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 3 );
	const GlintModifier& modC60 = *MakeMod( 2.0, 0.60, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 3 );
	const double c30 = MeasureCoverage( modC30, N, 103 );
	const double c60 = MeasureCoverage( modC60, N, 104 );
	const double ratioC = c60 / c30;
	std::cout << "  coverage scaling: c(0.6)/c(0.3) = " << ratioC << std::endl;
	CHECK( ratioC > 1.75 && ratioC < 2.25, "coverage scaling not ~linear (got " << ratioC << ")" );

	// ~Cubic scaling in `fill` (sphere volume).
	const GlintModifier& modF50 = *MakeMod( 2.0, 0.30, 0.5, 4.0, Vector3(1,1,1), Vector3(0,0,0), 3 );
	const double f50 = MeasureCoverage( modF50, N, 105 );
	const double ratioF = c30 / f50;		// expect ~ (1.0/0.5)^3 = 8
	std::cout << "  fill scaling: c(fill=1)/c(fill=0.5) = " << ratioF << std::endl;
	CHECK( ratioF > 6.0 && ratioF < 10.5, "fill scaling not ~cubic (got " << ratioF << ")" );
}

// ============================================================
//  Test 3: Tilt statistics
// ============================================================

static void TestTiltStatistics()
{
	std::cout << "--- Test 3: Tilt statistics ---" << std::endl;

	const Scalar spreadDeg = 3.0;
	const GlintModifier& mod = *MakeMod( 2.0, 1.0, 1.0, spreadDeg, Vector3(1,1,1), Vector3(0,0,0), 21 );

	RandomNumberGenerator rng( 31 );
	double sumTheta = 0;
	double sumCosPhi = 0, sumSinPhi = 0;
	double maxTheta = 0;
	int n = 0;

	for( int i = 0; i < 100000; i++ )
	{
		const Point3 p(
			rng.CanonicalRandom() * 60.0,
			rng.CanonicalRandom() * 60.0,
			rng.CanonicalRandom() * 60.0 );
		const GlintFacet f = mod.FindFacet( p );
		if( !f.found ) continue;

		sumTheta += f.theta;
		if( f.theta > maxTheta ) maxTheta = f.theta;
		sumCosPhi += cos( f.phi );
		sumSinPhi += sin( f.phi );
		n++;
	}

	CHECK( n > 10000, "too few facets for tilt statistics (" << n << ")" );

	// NOTE: consecutive queries inside ONE facet are correlated (same
	// tilt); this averages over ~n distinct facets since query points
	// are far apart w.h.p.  Rayleigh mean = sigma * sqrt(pi/2).
	const double meanTheta = sumTheta / n;
	const double expectMean = (spreadDeg * PI / 180.0) * sqrt( PI / 2.0 );
	std::cout << "  mean tilt: " << (meanTheta * 180.0 / PI) << " deg (Rayleigh expects "
	          << (expectMean * 180.0 / PI) << " deg), max " << (maxTheta * 180.0 / PI) << " deg over " << n << " facets" << std::endl;
	CHECK( fabs( meanTheta - expectMean ) / expectMean < 0.08,
		"mean tilt off Rayleigh expectation (got " << meanTheta << ", want " << expectMean << ")" );

	// Safety ceiling.
	CHECK( maxTheta <= 1.0471975511965976 + 1e-12, "tilt beyond 60-degree ceiling (" << maxTheta << ")" );

	// Azimuth not clustered: mean resultant length of a uniform sample
	// is ~1/sqrt(n).
	const double R = sqrt( sumCosPhi*sumCosPhi + sumSinPhi*sumSinPhi ) / n;
	std::cout << "  azimuth mean resultant: " << R << std::endl;
	CHECK( R < 0.05, "azimuths clustered (resultant " << R << ")" );
}

// ============================================================
//  Test 4: Cell-boundary integrity (the 3x3x3 search)
// ============================================================

static void TestBoundaryIntegrity()
{
	std::cout << "--- Test 4: Cell-boundary integrity ---" << std::endl;

	// density 1, scale 1: q-space == object space, cells are unit boxes.
	const GlintModifier& mod = *MakeMod( 1.0, 0.15, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 5 );
	const Scalar r = 0.5;		// fill=1 -> radius = half cell

	RandomNumberGenerator rng( 41 );
	int facets = 0, probes = 0, mismatches = 0, boundaryProbes = 0, boundaryMismatches = 0;

	for( int i = 0; i < 30000 && facets < 800; i++ )
	{
		const Point3 p(
			rng.CanonicalRandom() * 50.0,
			rng.CanonicalRandom() * 50.0,
			rng.CanonicalRandom() * 50.0 );
		const GlintFacet f = mod.FindFacet( p );
		if( !f.found ) continue;
		facets++;

		// Probe points inside this facet's sphere, including points in
		// NEIGHBOURING cells (the disc straddles cell walls whenever the
		// centre is within r of one).  Every probe must find a facet;
		// nearly all must find THIS one (rare nearest-centre steals from
		// an overlapping facet are legitimate at low coverage).
		for( int k = 0; k < 20; k++ )
		{
			// Uniform direction, radius biased toward the rim (worst case).
			Vector3 d(
				rng.CanonicalRandom() * 2.0 - 1.0,
				rng.CanonicalRandom() * 2.0 - 1.0,
				rng.CanonicalRandom() * 2.0 - 1.0 );
			const Scalar m = Vector3Ops::Magnitude( d );
			if( m < 1e-6 ) continue;
			const Scalar rad = r * ( 0.5 + 0.5 * rng.CanonicalRandom() ) * 0.98;
			d = d * ( rad / m );

			const Point3 q( f.centreQ.x + d.x, f.centreQ.y + d.y, f.centreQ.z + d.z );
			const GlintFacet g = mod.FindFacet( q );

			probes++;
			const bool sameCell =
				floor( q.x ) == floor( f.centreQ.x ) &&
				floor( q.y ) == floor( f.centreQ.y ) &&
				floor( q.z ) == floor( f.centreQ.z );
			if( !sameCell ) boundaryProbes++;

			const bool same = g.found &&
				g.centreQ.x == f.centreQ.x && g.centreQ.y == f.centreQ.y && g.centreQ.z == f.centreQ.z;
			if( !same ) {
				mismatches++;
				if( !sameCell ) boundaryMismatches++;
				// A mismatch may ONLY be a steal by a nearer overlapping
				// facet — never a miss.
				CHECK( g.found, "facet lost inside its own radius (boundary clip!) at probe " << probes );
			}
		}
	}

	std::cout << "  " << facets << " facets, " << probes << " probes (" << boundaryProbes
	          << " cross-boundary), " << mismatches << " steals, " << boundaryMismatches
	          << " cross-boundary steals" << std::endl;
	CHECK( facets >= 400, "too few facets probed (" << facets << ")" );
	CHECK( boundaryProbes > probes / 20, "test not exercising cell boundaries (" << boundaryProbes << ")" );
	CHECK( mismatches < probes / 20, "excessive facet steals (" << mismatches << "/" << probes << ")" );
}

// ============================================================
//  Test 5: Modify() guards + ONB integrity
// ============================================================

static void TestModifyGuards()
{
	std::cout << "--- Test 5: Modify() guards + ONB integrity ---" << std::endl;

	// Hostile spread: Rayleigh tail pushes many tilts to the ceiling.
	const GlintModifier& hostile = *MakeMod( 2.0, 1.0, 1.0, 40.0, Vector3(1,1,1), Vector3(0,0,0), 9 );
	// Production-like spread for tangent-coherence checks.
	const GlintModifier& gentle = *MakeMod( 2.0, 1.0, 1.0, 3.0, Vector3(1,1,1), Vector3(0,0,0), 9 );

	RandomNumberGenerator rng( 51 );
	int perturbed = 0;

	for( int i = 0; i < 20000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );

		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.2, -0.3, -1 ) ) );
		hostile.Modify( ri );

		// Unit length, orthonormal frame, w == vNormal.
		const Scalar lenN = Vector3Ops::Magnitude( ri.vNormal );
		CHECK( fabs( lenN - 1.0 ) < 1e-9, "non-unit perturbed normal (" << lenN << ")" );
		CHECK( Vector3Ops::Dot( ri.onb.w(), ri.vNormal ) > 1.0 - 1e-9, "onb.w != vNormal after Modify" );
		CHECK( fabs( Vector3Ops::Dot( ri.onb.u(), ri.onb.w() ) ) < 1e-9, "onb u/w not orthogonal" );
		CHECK( fabs( Vector3Ops::Dot( ri.onb.u(), ri.onb.v() ) ) < 1e-9, "onb u/v not orthogonal" );

		// Geometric-side guard: never past the tangent plane guard.
		CHECK( Vector3Ops::Dot( ri.vNormal, ri.vGeomNormal ) >= 0.05 - 1e-9,
			"perturbed normal violates geometric-side guard" );

		if( Vector3Ops::Dot( ri.vNormal, Vector3( 0, 0, 1 ) ) < 1.0 - 1e-12 ) perturbed++;
	}
	CHECK( perturbed > 5000, "hostile-spread modifier barely perturbing (" << perturbed << ")" );
	std::cout << "  hostile spread: " << perturbed << "/20000 hits perturbed, all guarded" << std::endl;

	// Tangent coherence at gentle spread: the rebuilt u must stay in
	// the plane spanned by the old u and the new normal (preserve-u
	// construction), i.e. dot(newU, oldV) ~ 0.
	int coherent = 0, facetHits = 0;
	for( int i = 0; i < 20000 && facetHits < 2000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );
		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.2, -0.3, -1 ) ) );
		const Vector3 oldU = ri.onb.u();
		const Vector3 oldV = ri.onb.v();
		gentle.Modify( ri );
		if( Vector3Ops::Dot( ri.vNormal, Vector3( 0, 0, 1 ) ) > 1.0 - 1e-12 ) continue;
		facetHits++;
		if( fabs( Vector3Ops::Dot( ri.onb.u(), oldV ) ) < 0.08 && Vector3Ops::Dot( ri.onb.u(), oldU ) > 0.9 ) {
			coherent++;
		}
	}
	std::cout << "  tangent coherence: " << coherent << "/" << facetHits << std::endl;
	CHECK( facetHits > 500, "too few facet hits for tangent test (" << facetHits << ")" );
	CHECK( coherent == facetHits, "preserve-u tangent construction violated (" << coherent << "/" << facetHits << ")" );

	// 5b. TILTED SHADING BASE: on a flat base the 60-degree ceiling
	// alone keeps every facet above the guard threshold, so the
	// geometric-side guard can never fire there.  Real bases (bumpy /
	// displaced / Phong-interpolated) have shading normals leaning away
	// from vGeomNormal — a hostile facet tilt stacked on a 50-degree
	// lean can cross the geometric tangent plane, and the guard must
	// reject those to the (unperturbed) shading normal.
	int rejected5b = 0, perturbed5b = 0;
	for( int i = 0; i < 20000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );

		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.2, -0.3, -1 ) ) );
		// Shading normal leans 50 degrees off the geometric +Z.
		const Vector3 lean = Vector3Ops::Normalize( Vector3( sin( 50.0 * DEG_TO_RAD ), 0, cos( 50.0 * DEG_TO_RAD ) ) );
		ri.vNormal = lean;
		ri.onb.CreateFromW( lean );

		hostile.Modify( ri );

		CHECK( Vector3Ops::Dot( ri.vNormal, ri.vGeomNormal ) >= 0.05 - 1e-9,
			"tilted-base facet crossed the geometric tangent plane (dot="
			<< Vector3Ops::Dot( ri.vNormal, ri.vGeomNormal ) << ")" );

		const Scalar dLean = Vector3Ops::Dot( ri.vNormal, lean );
		if( dLean > 1.0 - 1e-12 ) {
			// Unchanged: either no facet here, or the guard rejected one.
		} else {
			perturbed5b++;
		}
		// Count guard rejections: a facet exists at p but the output is
		// the unperturbed lean.
		if( dLean > 1.0 - 1e-12 && hostile.FindFacet( p ).found ) rejected5b++;
	}
	std::cout << "  tilted base: " << perturbed5b << " perturbed, " << rejected5b
	          << " guard-rejected, all outputs on the geometric side" << std::endl;
	CHECK( perturbed5b > 2000, "tilted-base case barely perturbing (" << perturbed5b << ")" );
	CHECK( rejected5b > 50, "guard never fired on the tilted base — case not exercising the guard (" << rejected5b << ")" );
}

// ============================================================
//  Test 6: GGX SPF/BRDF consistency at modifier-perturbed hits
// ============================================================

static void TestGGXConsistencyAtPerturbedHits()
{
	std::cout << "--- Test 6: GGX SPF/BRDF consistency at perturbed hits ---" << std::endl;

	GlobalLog();		// initialize logger before material construction

	StubObject* stub = new StubObject();
	stub->addref();

	UniformColorPainter* diffuse = new UniformColorPainter( RISEPel( 0.1, 0.1, 0.1 ) );	diffuse->addref();
	UniformColorPainter* specular = new UniformColorPainter( RISEPel( 0.9, 0.9, 0.9 ) );	specular->addref();
	UniformScalarPainter* alphaX = new UniformScalarPainter( 0.05 );	alphaX->addref();
	UniformScalarPainter* alphaY = new UniformScalarPainter( 0.05 );	alphaY->addref();
	UniformScalarPainter* ior = new UniformScalarPainter( 0.15 );		ior->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 3.5 );		ext->addref();

	GGXBRDF* brdf = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, eFresnelConductor );	brdf->addref();
	GGXSPF* spf = new GGXSPF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, eFresnelConductor );		spf->addref();

	const GlintModifier& mod = *MakeMod( 2.0, 1.0, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 13 );

	RandomNumberGenerator rng( 61 );
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( stub );

	int valid = 0, pdfFailures = 0;
	double spfAccum = 0, brdfAccum = 0;
	int accumN = 0;

	for( int i = 0; i < 40000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );
		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.45, 0.1, -0.9 ) ) );
		mod.Modify( ri );

		ScatteredRayContainer scattered;
		spf->Scatter( ri, sampler, scattered, iorStack );

		for( unsigned int s = 0; s < scattered.Count(); s++ )
		{
			const ScatteredRay& scat = scattered[s];
			if( scat.isDelta ) continue;

			const Vector3 wo = scat.ray.Dir();
			const Scalar cosWo = Vector3Ops::Dot( wo, ri.onb.w() );
			if( cosWo <= 0 ) continue;

			const Scalar spfPdf = spf->Pdf( ri, wo, iorStack );
			if( spfPdf < 1e-10 ) continue;

			valid++;

			// Sampled-pdf vs queried-pdf self-consistency: the MIS
			// contract.  Both are evaluated at the SAME perturbed ri,
			// which is what the integrators see after the modifier hook.
			const Scalar pdfErr = ( scat.pdf > 1e-10 ) ? fabs( scat.pdf - spfPdf ) / scat.pdf : 0;
			if( pdfErr > 1e-4 ) pdfFailures++;

			// Two MC estimators of the directional albedo at the
			// perturbed frame must agree (SPF kray vs BRDF value).
			spfAccum += ColorMath::MaxValue( scat.kray );
			brdfAccum += ColorMath::MaxValue( brdf->value( wo, ri ) ) * cosWo / spfPdf;
			accumN++;
		}
	}

	const double spfAlbedo = ( accumN > 0 ) ? spfAccum / accumN : 0;
	const double brdfAlbedo = ( accumN > 0 ) ? brdfAccum / accumN : 0;
	std::cout << "  valid=" << valid << " pdfFail=" << pdfFailures
	          << " spfAlbedo=" << std::setprecision(4) << spfAlbedo
	          << " brdfAlbedo=" << brdfAlbedo << std::endl;

	CHECK( valid > 10000, "too few valid perturbed-frame samples (" << valid << ")" );
	CHECK( pdfFailures <= valid / 1000, "pdf self-consistency failures at perturbed frame (" << pdfFailures << "/" << valid << ")" );
	CHECK( brdfAlbedo > 1e-3 && fabs( spfAlbedo - brdfAlbedo ) / brdfAlbedo < 0.05,
		"SPF/BRDF estimator mismatch at perturbed frame (" << spfAlbedo << " vs " << brdfAlbedo << ")" );
	CHECK( spfAlbedo < 1.05, "energy gain at perturbed frame (" << spfAlbedo << ")" );

	brdf->release();
	spf->release();
	diffuse->release();
	specular->release();
	alphaX->release();
	alphaY->release();
	ior->release();
	ext->release();
	stub->release();
}

// ============================================================
//  Main
// ============================================================

int main()
{
	std::cout << "=== GlintModifier Test ===" << std::endl;

	TestDeterminism();
	TestCoverage();
	TestTiltStatistics();
	TestBoundaryIntegrity();
	TestModifyGuards();
	TestGGXConsistencyAtPerturbedHits();

	ReleaseMods();

	if( g_failures == 0 ) {
		std::cout << std::endl << "=== ALL TESTS PASSED ===" << std::endl;
		return 0;
	}
	std::cout << std::endl << "=== " << g_failures << " FAILURES ===" << std::endl;
	return 1;
}
