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
//       past the geometric tangent plane guard, the tilt ceiling holds
//       where the hostile spread actually clamps, and the rebuilt ONB
//       is orthonormal with the tangent direction preserved — checked
//       against a NON-canonical base tangent so preserve-u is actually
//       discriminated from a CreateFromW rebuild.
//    6. GGX RGB + NM pointwise SPF/BRDF consistency at modifier-
//       perturbed hits: sampled-pdf vs queried-pdf self-consistency
//       and SPF-vs-BRDF albedo agreement at the perturbed frame (the
//       ingredient the integrators' MIS relies on; not a full
//       NEE-vs-BSDF MIS proof).
//    7. Seed / scale / shift semantics: differing seeds give a
//       materially different field; `shift` translates the field
//       exactly; anisotropic `scale` elongates it (boundary-transition
//       rates scale with the stretch).
//    8. DielectricSPF hero path at perturbed hits: delta reflection
//       mirrors about the FACET normal, refraction obeys Snell about
//       it, Fresnel weights sum to 1 at entry, and the facet visibly
//       redirects the delta versus the smooth surface.
//    9. Hostile-coordinate determinism: the 2^40 cell fold keeps
//       FindFacet finite, stable, and bit-deterministic at extreme
//       object-space coordinates.
//    10. Ray-anchored geometric-horizon gate (a4fb884d regression pin):
//        in the DISAGREEMENT BAND (front-facing hit whose shading
//        normal is tilted past the view), GGXSPF never emits a
//        tunnelling ray and Pdf/value agree it's zero/black for a
//        probe pointing into the object.
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
#include <cstring>
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
#include "../src/Library/Materials/DielectricSPF.h"
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
	// per unit cell.  The TRUE jittered-grid value is ~0.46 (the
	// one-per-cell packing constraint suppresses overlap versus the
	// Poisson 1-exp(-pi/6)=0.41 lower bound; review round 1 measured
	// independent-jitter MC at ~0.459, this impl at ~0.447).  The band
	// is tight enough that a ~5% facet-radius error fails it.
	const GlintModifier& modFull = *MakeMod( 2.0, 1.0, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 3 );
	const double cFull = MeasureCoverage( modFull, N, 102 );
	std::cout << "  coverage(1.0, fill=1): " << std::fixed << std::setprecision(4) << cFull << std::endl;
	CHECK( cFull > 0.40 && cFull < 0.48, "full coverage out of expected band (got " << cFull << ")" );

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

		// Tilt ceiling asserted WHERE IT ACTUALLY FIRES: at spread=40deg
		// ~32% of Rayleigh draws exceed 60deg and must clamp (at
		// production spreads the ceiling is unreachable, so a ceiling
		// assert there would be vacuous).
		const GlintFacet hf = hostile.FindFacet( p );
		if( hf.found ) {
			CHECK( hf.theta <= 1.0471975511965976 + 1e-12, "hostile tilt beyond ceiling (" << hf.theta << ")" );
		}

		if( Vector3Ops::Dot( ri.vNormal, Vector3( 0, 0, 1 ) ) < 1.0 - 1e-12 ) perturbed++;
	}
	CHECK( perturbed > 5000, "hostile-spread modifier barely perturbing (" << perturbed << ")" );
	std::cout << "  hostile spread: " << perturbed << "/20000 hits perturbed, all guarded" << std::endl;

	// Tangent coherence at gentle spread: the rebuilt u must be the
	// PROJECTION of the pre-perturbation u onto the new tangent plane.
	// The base ONB deliberately carries a NON-CANONICAL tangent
	// (geometry-defined, as bShadingTangentFromGeometry produces) —
	// with a canonical base frame, CreateFromW would reproduce the same
	// u and the preserve-u construction would be indistinguishable from
	// it (review round 1, P1-a: the original form of this check let a
	// CreateFromW rebuild pass 2000/2000).
	const Vector3 baseTangent = Vector3Ops::Normalize( Vector3( 0.4, 0.9, 0 ) );
	int coherent = 0, facetHits = 0;
	for( int i = 0; i < 20000 && facetHits < 2000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );
		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.2, -0.3, -1 ) ) );
		ri.onb.CreateFromWU( Vector3( 0, 0, 1 ), baseTangent );
		const Vector3 oldU = ri.onb.u();
		gentle.Modify( ri );
		if( Vector3Ops::Dot( ri.vNormal, Vector3( 0, 0, 1 ) ) > 1.0 - 1e-12 ) continue;
		facetHits++;

		// Expected u: old u projected onto the new tangent plane.
		const Vector3 n = ri.vNormal;
		const Vector3 expectU = Vector3Ops::Normalize( oldU - n * Vector3Ops::Dot( oldU, n ) );
		if( Vector3Ops::Dot( ri.onb.u(), expectU ) > 1.0 - 1e-9 ) {
			coherent++;
		}
	}
	std::cout << "  tangent coherence (non-canonical base): " << coherent << "/" << facetHits << std::endl;
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
	std::cout << "  RGB: valid=" << valid << " pdfFail=" << pdfFailures
	          << " spfAlbedo=" << std::setprecision(4) << spfAlbedo
	          << " brdfAlbedo=" << brdfAlbedo << std::endl;

	CHECK( valid > 10000, "too few valid perturbed-frame samples (" << valid << ")" );
	CHECK( pdfFailures <= valid / 1000, "pdf self-consistency failures at perturbed frame (" << pdfFailures << "/" << valid << ")" );
	CHECK( brdfAlbedo > 1e-3 && fabs( spfAlbedo - brdfAlbedo ) / brdfAlbedo < 0.05,
		"SPF/BRDF estimator mismatch at perturbed frame (" << spfAlbedo << " vs " << brdfAlbedo << ")" );
	CHECK( spfAlbedo < 1.05, "energy gain at perturbed frame (" << spfAlbedo << ")" );

	// NM (spectral) twin at the same perturbed frames: ScatterNM /
	// PdfNM / valueNM must uphold the identical contract — the hero
	// renders spectrally, and an RGB-only check would leave the NM
	// path's frame handling unproven (review round 1, P2-d).
	const Scalar nm = 550.0;
	int validNM = 0, pdfFailuresNM = 0;
	double spfAccumNM = 0, brdfAccumNM = 0;
	int accumNNM = 0;

	for( int i = 0; i < 40000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );
		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.45, 0.1, -0.9 ) ) );
		mod.Modify( ri );

		ScatteredRayContainer scattered;
		spf->ScatterNM( ri, sampler, nm, scattered, iorStack );

		for( unsigned int s = 0; s < scattered.Count(); s++ )
		{
			const ScatteredRay& scat = scattered[s];
			if( scat.isDelta ) continue;

			const Vector3 wo = scat.ray.Dir();
			const Scalar cosWo = Vector3Ops::Dot( wo, ri.onb.w() );
			if( cosWo <= 0 ) continue;

			const Scalar spfPdf = spf->PdfNM( ri, wo, nm, iorStack );
			if( spfPdf < 1e-10 ) continue;

			validNM++;

			const Scalar pdfErr = ( scat.pdf > 1e-10 ) ? fabs( scat.pdf - spfPdf ) / scat.pdf : 0;
			if( pdfErr > 1e-4 ) pdfFailuresNM++;

			spfAccumNM += scat.krayNM;
			brdfAccumNM += brdf->valueNM( wo, ri, nm ) * cosWo / spfPdf;
			accumNNM++;
		}
	}

	const double spfAlbedoNM = ( accumNNM > 0 ) ? spfAccumNM / accumNNM : 0;
	const double brdfAlbedoNM = ( accumNNM > 0 ) ? brdfAccumNM / accumNNM : 0;
	std::cout << "  NM(550): valid=" << validNM << " pdfFail=" << pdfFailuresNM
	          << " spfAlbedo=" << spfAlbedoNM << " brdfAlbedo=" << brdfAlbedoNM << std::endl;

	CHECK( validNM > 10000, "too few valid NM perturbed-frame samples (" << validNM << ")" );
	CHECK( pdfFailuresNM <= validNM / 1000, "NM pdf self-consistency failures at perturbed frame (" << pdfFailuresNM << "/" << validNM << ")" );
	CHECK( brdfAlbedoNM > 1e-3 && fabs( spfAlbedoNM - brdfAlbedoNM ) / brdfAlbedoNM < 0.05,
		"NM SPF/BRDF estimator mismatch at perturbed frame (" << spfAlbedoNM << " vs " << brdfAlbedoNM << ")" );
	CHECK( spfAlbedoNM < 1.05, "NM energy gain at perturbed frame (" << spfAlbedoNM << ")" );

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
//  Test 7: seed / scale / shift semantics
// ============================================================

static void TestSeedScaleShift()
{
	std::cout << "--- Test 7: seed / scale / shift semantics ---" << std::endl;

	// 7a. Differing seeds give materially different fields.
	const GlintModifier& seedA = *MakeMod( 2.0, 0.4, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 );
	const GlintModifier& seedB = *MakeMod( 2.0, 0.4, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 8 );

	RandomNumberGenerator rng( 71 );
	int differ = 0;
	const int NS = 20000;
	for( int i = 0; i < NS; i++ )
	{
		const Point3 p(
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0 );
		const GlintFacet a = seedA.FindFacet( p );
		const GlintFacet b = seedB.FindFacet( p );
		if( a.found != b.found ||
			( a.found && ( a.centreQ.x != b.centreQ.x || a.theta != b.theta || a.phi != b.phi ) ) ) {
			differ++;
		}
	}
	std::cout << "  seed fields differ at " << differ << "/" << NS << " points" << std::endl;
	CHECK( differ > NS / 5, "differing seeds barely change the field (" << differ << "/" << NS << ")" );

	// 7b. `shift` translates the field EXACTLY: q(p; shift=s) equals
	// q(p + s; shift=0) at scale 1, density 1, so the two must agree
	// bit-for-bit (found flag, centre, tilt).
	const Vector3 s( 0.37, 0.11, -0.23 );
	const GlintModifier& shifted = *MakeMod( 1.0, 0.3, 1.0, 4.0, Vector3(1,1,1), s, 7 );
	const GlintModifier& unshifted = *MakeMod( 1.0, 0.3, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 );
	int checkedShift = 0;
	for( int i = 0; i < 20000; i++ )
	{
		const Point3 p(
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0 );
		const GlintFacet a = shifted.FindFacet( p );
		const GlintFacet b = unshifted.FindFacet( Point3( p.x + s.x, p.y + s.y, p.z + s.z ) );
		CHECK( a.found == b.found, "shift does not translate the field (found mismatch at i=" << i << ")" );
		if( a.found && b.found ) {
			CHECK( a.theta == b.theta && a.phi == b.phi,
				"shift does not translate the field (tilt mismatch at i=" << i << ")" );
			checkedShift++;
		}
	}
	std::cout << "  shift translation: " << checkedShift << " facet agreements" << std::endl;
	CHECK( checkedShift > 1000, "shift test barely exercised (" << checkedShift << ")" );

	// 7c. Anisotropic `scale` elongates the field: with scale (3,1,1)
	// the q-space x-axis runs 3x faster, so found/not-found boundary
	// transitions per unit OBJECT-space length along x should be ~3x
	// those along y.
	const GlintModifier& aniso = *MakeMod( 1.0, 0.5, 1.0, 4.0, Vector3(3,1,1), Vector3(0,0,0), 7 );
	int transX = 0, transY = 0;
	for( int line = 0; line < 300; line++ )
	{
		const Point3 o(
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0 );
		bool prevX = aniso.FindFacet( o ).found;
		bool prevY = prevX;
		for( int k = 1; k <= 200; k++ )
		{
			const Scalar t = k * 0.05;
			const bool fx = aniso.FindFacet( Point3( o.x + t, o.y, o.z ) ).found;
			const bool fy = aniso.FindFacet( Point3( o.x, o.y + t, o.z ) ).found;
			if( fx != prevX ) { transX++; prevX = fx; }
			if( fy != prevY ) { transY++; prevY = fy; }
		}
	}
	const double anisoRatio = ( transY > 0 ) ? (double)transX / (double)transY : 0;
	std::cout << "  anisotropy: " << transX << " x-transitions vs " << transY
	          << " y-transitions (ratio " << anisoRatio << ")" << std::endl;
	CHECK( anisoRatio > 2.2 && anisoRatio < 3.8,
		"scale (3,1,1) does not elongate the field ~3x (ratio " << anisoRatio << ")" );

	// 7d. COMPOSED scale+shift order (Worley convention q = pt*scale +
	// shift): with a power-of-two scale and exactly-representable shift,
	// q(p; scale=2, shift=0.5) is BIT-IDENTICAL to
	// q(p + shift/scale; scale=2, shift=0) — doubling commutes with FP
	// rounding.  A swapped composition ((pt+shift)*scale) shifts the
	// field by a whole extra half-cell and fails this.  (Review round 2,
	// P2-3: 7b used unit scale, 7c used zero shift, so the composition
	// ORDER was untested.)
	const GlintModifier& composed = *MakeMod( 1.0, 0.3, 1.0, 4.0, Vector3(2,1,1), Vector3(0.5,0.25,-0.75), 7 );
	const GlintModifier& composedRef = *MakeMod( 1.0, 0.3, 1.0, 4.0, Vector3(2,1,1), Vector3(0,0,0), 7 );
	int checkedComposed = 0;
	for( int i = 0; i < 20000; i++ )
	{
		const Point3 p(
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0 );
		const GlintFacet a = composed.FindFacet( p );
		const GlintFacet b = composedRef.FindFacet( Point3( p.x + 0.25, p.y + 0.25, p.z - 0.75 ) );
		CHECK( a.found == b.found, "scale+shift composition order wrong (found mismatch at i=" << i << ")" );
		if( a.found && b.found ) {
			CHECK( a.theta == b.theta && a.phi == b.phi,
				"scale+shift composition order wrong (tilt mismatch at i=" << i << ")" );
			checkedComposed++;
		}
	}
	std::cout << "  composed scale+shift: " << checkedComposed << " facet agreements" << std::endl;
	CHECK( checkedComposed > 1000, "composed scale+shift barely exercised (" << checkedComposed << ")" );
}

// ============================================================
//  Test 7e: nearest-wins tie-break between overlapping facets
// ============================================================

static void TestNearestWins()
{
	std::cout << "--- Test 7e: nearest-wins between overlapping facets ---" << std::endl;

	// Dense field so overlapping facet spheres are common.  Scan lines
	// with a small step; at every point where the winning centre
	// SWITCHES while both consecutive points are covered, nearest-wins
	// requires the new centre to be no farther than the old one (up to
	// the step's worth of movement).  A farthest-wins mutation reverses
	// the inequality at every such boundary.
	const GlintModifier& mod = *MakeMod( 1.0, 1.0, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 5 );

	RandomNumberGenerator rng( 91 );
	const Scalar step = 0.01;
	int switches = 0, violations = 0;

	for( int line = 0; line < 600; line++ )
	{
		const Point3 o(
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0,
			rng.CanonicalRandom() * 40.0 );

		GlintFacet prev = mod.FindFacet( o );
		for( int k = 1; k <= 300; k++ )
		{
			const Point3 p( o.x + k * step, o.y, o.z );
			const GlintFacet cur = mod.FindFacet( p );
			if( prev.found && cur.found &&
				( prev.centreQ.x != cur.centreQ.x || prev.centreQ.y != cur.centreQ.y || prev.centreQ.z != cur.centreQ.z ) )
			{
				switches++;
				const Vector3 dNew( p.x - cur.centreQ.x, p.y - cur.centreQ.y, p.z - cur.centreQ.z );
				const Vector3 dOld( p.x - prev.centreQ.x, p.y - prev.centreQ.y, p.z - prev.centreQ.z );
				// At the crossing the two distances are equal; one step
				// past it the new centre must be strictly nearer (allow
				// the step's slack).
				if( Vector3Ops::Magnitude( dNew ) > Vector3Ops::Magnitude( dOld ) + 2.0 * step ) {
					violations++;
				}
			}
			prev = cur;
		}
	}

	std::cout << "  " << switches << " overlap boundary switches, " << violations << " nearest-wins violations" << std::endl;
	CHECK( switches > 200, "too few overlap switches to test nearest-wins (" << switches << ")" );
	CHECK( violations == 0, "nearest-wins violated at " << violations << "/" << switches << " switches" );
}

// ============================================================
//  Test 7f: geometric-side guard with a FLIPPED shading orientation
// ============================================================

static void TestFlippedOrientationGuard()
{
	std::cout << "--- Test 7f: guard under flipped shading orientation ---" << std::endl;

	// Some geometries flip the shading normal toward the ray at Phong
	// silhouettes while vGeomNormal stays outward — dot(vNormal,
	// vGeomNormal) < 0.  The guard's geomSign must then keep facets in
	// the SHADING hemisphere (dot(newN, vGeomNormal) <= -0.05), not
	// reject everything or accept wrong-side facets.  A flattened
	// geomSign (always +1) accepts facets with dot(newN, +Z) >= 0.05,
	// which this test fails.
	const GlintModifier& hostile = *MakeMod( 2.0, 1.0, 1.0, 40.0, Vector3(1,1,1), Vector3(0,0,0), 9 );

	// Shading normal leans 130 degrees off the geometric +Z (flipped
	// hemisphere, as a silhouette Phong normal would be).
	const Vector3 lean = Vector3Ops::Normalize(
		Vector3( sin( 130.0 * DEG_TO_RAD ), 0, cos( 130.0 * DEG_TO_RAD ) ) );

	RandomNumberGenerator rng( 95 );
	int perturbed = 0, rejected = 0;

	for( int i = 0; i < 20000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );

		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.2, -0.3, -1 ) ) );
		ri.vNormal = lean;
		ri.onb.CreateFromW( lean );
		// vGeomNormal stays +Z: flipped orientation (dot = cos130 < 0).

		hostile.Modify( ri );

		// Orientation-matched guard: outputs stay in the SHADING
		// hemisphere relative to the geometric normal.
		CHECK( Vector3Ops::Dot( ri.vNormal, ri.vGeomNormal ) <= -( 0.05 - 1e-9 ),
			"flipped-orientation output crossed to the geometric-normal side (dot="
			<< Vector3Ops::Dot( ri.vNormal, ri.vGeomNormal ) << ")" );

		const Scalar dLean = Vector3Ops::Dot( ri.vNormal, lean );
		if( dLean < 1.0 - 1e-12 ) {
			perturbed++;
		} else if( hostile.FindFacet( p ).found ) {
			rejected++;
		}
	}

	std::cout << "  flipped orientation: " << perturbed << " perturbed, " << rejected
	          << " guard-rejected, all outputs shading-side" << std::endl;
	CHECK( perturbed > 2000, "flipped-orientation case barely perturbing (" << perturbed << ")" );
	CHECK( rejected > 50, "guard never fired under flipped orientation (" << rejected << ")" );
}

// ============================================================
//  Test 8: DielectricSPF hero path at perturbed hits
// ============================================================

static void TestDielectricAtPerturbedHits()
{
	std::cout << "--- Test 8: DielectricSPF delta consistency at perturbed hits ---" << std::endl;

	StubObject* stub = new StubObject();
	stub->addref();

	UniformScalarPainter* tau = new UniformScalarPainter( 1.0 );		tau->addref();
	UniformScalarPainter* ior = new UniformScalarPainter( 1.55 );		ior->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 1000000.0 );	scat->addref();

	DielectricSPF* spf = new DielectricSPF( *tau, *ior, *scat, false );	spf->addref();

	const GlintModifier& mod = *MakeMod( 2.0, 1.0, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 17 );

	RandomNumberGenerator rng( 81 );
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( stub );

	int facetHits = 0, redirected = 0;
	const Vector3 inDir = Vector3Ops::Normalize( Vector3( 0.5, 0.2, -0.8 ) );

	for( int i = 0; i < 20000 && facetHits < 4000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );
		RayIntersectionGeometric ri = MakeRI( p, inDir );
		mod.Modify( ri );
		if( Vector3Ops::Dot( ri.vNormal, Vector3( 0, 0, 1 ) ) > 1.0 - 1e-12 ) continue;
		facetHits++;

		ScatteredRayContainer scattered;
		spf->Scatter( ri, sampler, scattered, iorStack );

		const Vector3 w = ri.onb.w();
		Scalar krayReflect = -1, krayRefract = -1;

		for( unsigned int s = 0; s < scattered.Count(); s++ )
		{
			const ScatteredRay& scat2 = scattered[s];
			CHECK( scat2.isDelta, "polished dielectric emitted a non-delta ray" );

			if( scat2.type == ScatteredRay::eRayReflection )
			{
				// Delta reflection must mirror about the FACET normal.
				const Vector3 mirror = Vector3Ops::Normalize(
					inDir - w * ( 2.0 * Vector3Ops::Dot( inDir, w ) ) );
				CHECK( Vector3Ops::Dot( scat2.ray.Dir(), mirror ) > 1.0 - 1e-9,
					"reflection not mirrored about facet normal" );
				krayReflect = ColorMath::MaxValue( scat2.kray );

				// The facet must actually redirect the delta vs the
				// smooth surface.
				const Vector3 smoothMirror = Vector3Ops::Normalize(
					inDir - Vector3(0,0,1) * ( 2.0 * Vector3Ops::Dot( inDir, Vector3(0,0,1) ) ) );
				if( Vector3Ops::Dot( scat2.ray.Dir(), smoothMirror ) < 1.0 - 1e-8 ) redirected++;
			}
			else if( scat2.type == ScatteredRay::eRayRefraction )
			{
				// Snell about the facet normal: sinT/sinI = 1/1.55.
				const Scalar cosI = fabs( Vector3Ops::Dot( inDir, w ) );
				const Scalar sinI = sqrt( r_max( Scalar(0), 1.0 - cosI * cosI ) );
				const Scalar cosT = fabs( Vector3Ops::Dot( scat2.ray.Dir(), w ) );
				const Scalar sinT = sqrt( r_max( Scalar(0), 1.0 - cosT * cosT ) );
				if( sinI > 1e-6 ) {
					CHECK( fabs( sinT / sinI - 1.0 / 1.55 ) < 1e-6,
						"refraction violates Snell about facet normal (ratio " << ( sinT / sinI ) << ")" );
				}
				krayRefract = ColorMath::MaxValue( scat2.kray );
			}
		}

		// Fresnel split sums to 1 at entry (tau=1 clear dielectric).
		if( krayReflect >= 0 && krayRefract >= 0 ) {
			CHECK( fabs( krayReflect + krayRefract - 1.0 ) < 1e-9,
				"Fresnel R+T != 1 at facet (" << ( krayReflect + krayRefract ) << ")" );
		}
	}

	std::cout << "  " << facetHits << " facet hits, " << redirected << " redirected deltas" << std::endl;
	CHECK( facetHits > 1000, "too few facet hits (" << facetHits << ")" );
	// Allow a couple of near-zero-tilt facets (P(theta small enough that
	// the facet mirror sits within 1e-8 of the smooth mirror) ~ 5e-7 per
	// facet at spread=4deg) — a strict == would be seed-fragile.
	CHECK( redirected >= facetHits - 2, "facets failed to redirect the delta (" << redirected << "/" << facetHits << ")" );

	spf->release();
	tau->release();
	ior->release();
	scat->release();
	stub->release();
}

// ============================================================
//  Test 8b: non-finite parameters force INERT (laundered guard)
// ============================================================

//! Materialize a Scalar from raw IEEE-754 bits at RUNTIME, through a
//! volatile so the compiler cannot constant-fold it: under -ffast-math
//! clang treats std::numeric_limits quiet_NaN()/infinity() as poison
//! and folds them at compile time (-Wnan-infinity-disabled), so the
//! NaN would never reach the constructor under test.
static Scalar BitsToScalar( unsigned long long bits )
{
	volatile unsigned long long vu = bits;
	const unsigned long long u = vu;
	Scalar v;
	memcpy( &v, &u, sizeof( v ) );
	return v;
}

static void TestNonFiniteInert()
{
	std::cout << "--- Test 8b: non-finite parameters force inert ---" << std::endl;

	const Scalar qnan = BitsToScalar( 0x7FF8000000000000ULL );	// quiet NaN
	const Scalar pinf = BitsToScalar( 0x7FF0000000000000ULL );	// +infinity

	// NaN in each scalar slot, NaN in a scale/shift component, and an
	// Inf coverage: every one must yield a completely inert modifier.
	// HISTORICAL (pre-2026-07-29): this used to DISCRIMINATE the
	// volatile-laundered ctor guard from the plain memcpy form, which
	// -ffast-math deleted via nofpclass parameter poison (measured then:
	// NaN coverage -> FULLY-LIT field).  clang does not emit
	// nofpclass(nan inf) with -fno-finite-math-only, so the plain form
	// now passes too and this no longer discriminates the two.  The
	// coverage itself is still valid and worth keeping -- it pins that a
	// non-finite parameter yields an inert modifier -- but do not cite it
	// as evidence that the laundered form is REQUIRED.
	const GlintModifier* bad[7] = {
		MakeMod( qnan, 0.5, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 ),
		MakeMod( 2.0, qnan, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 ),
		MakeMod( 2.0, 0.5, qnan, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 ),
		MakeMod( 2.0, 0.5, 1.0, qnan, Vector3(1,1,1), Vector3(0,0,0), 7 ),
		MakeMod( 2.0, 0.5, 1.0, 4.0, Vector3(1,qnan,1), Vector3(0,0,0), 7 ),
		MakeMod( 2.0, 0.5, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,qnan), 7 ),
		MakeMod( 2.0, pinf, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 ),
	};

	for( int b = 0; b < 7; b++ )
	{
		const double c = MeasureCoverage( *bad[b], 5000, 200 + b );
		CHECK( c == 0.0, "non-finite parameter slot " << b << " did not force inert (coverage " << c << ")" );
	}
	std::cout << "  7/7 non-finite configurations inert" << std::endl;
}

// ============================================================
//  Test 8bb: TIR under a tilted (glint-perturbed) shading frame does
//  NOT drop the mandatory Fresnel lobe (review-round-1 F1)
// ============================================================

//! Review-round-1 F1: when a delta dielectric hits Total Internal
//! Reflection from inside (no transmission lobe will ever be emitted),
//! the geometric-horizon gate must not silently drop the reflection --
//! it must re-derive the reflection direction about the TRUE geometric
//! normal instead.  This constructs the tilted-frame scenario directly
//! (rather than searching the GlintModifier's stochastic facet field
//! for a qualifying hit) so the incidence/tilt angles are exact and the
//! TIR condition is guaranteed by construction:
//!
//!   - Flat geometric normal geomN = +Z.
//!   - Shading (facet) normal tilted 55 deg off geomN in the X-Z plane
//!     -- within GlintModifier's 60-degree tilt ceiling.
//!   - Ray travelling from inside straight along +Z (normal incidence
//!     against the FLAT surface, but 55 deg off the TILTED shading
//!     normal).
//!   - IOR 1.55 (glass) -> 1.0 (air): critical angle is asin(1/1.55) =
//!     ~40.2 deg, so the 55-degree incidence angle against the shading
//!     normal is comfortably past it => guaranteed TIR (ref forced to
//!     1.0; CalculateRefractedRay returns false).
//!
//!   Reflecting about the tilted shading normal sends the ray to a
//!   positive-Z (outward / tunnelling) direction -- the naive gate
//!   would fail and (pre-fix) drop the ONLY ray this delta material
//!   could emit, a fully black sample.  The fix must instead emit
//!   exactly one reflection ray, staying on the true geometric-normal
//!   side (non-positive Z: reflected back into the medium, as a TIR
//!   event must).
static void TestDielectricTIRUnderTiltedNormal()
{
	std::cout << "--- Test 8bb: TIR under tilted shading frame keeps the mandatory lobe ---" << std::endl;

	StubObject* stub = new StubObject();
	stub->addref();

	UniformScalarPainter* tau  = new UniformScalarPainter( 1.0 );		tau->addref();
	UniformScalarPainter* ior  = new UniformScalarPainter( 1.55 );		ior->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 1000000.0 );	scat->addref();

	DielectricSPF* spf = new DielectricSPF( *tau, *ior, *scat, false );	spf->addref();

	class FixedSampler : public ISampler
	{
	public:
		Scalar Get1D() { return 0.5; }
		Point2 Get2D() { return Point2( 0.5, 0.5 ); }
		void StartStream( int ) {}
	};
	FixedSampler sampler;

	const Scalar tiltRad = 55.0 * PI / 180.0;
	const Vector3 geomN( 0, 0, 1 );
	const Vector3 nShade = Vector3Ops::Normalize( Vector3( sin( tiltRad ), 0, cos( tiltRad ) ) );
	const Vector3 inDir( 0, 0, 1 );	// from inside, normal incidence vs the FLAT surface

	Ray inRay( Point3( 0, 0, -1 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.ptObjIntersec = Point3( 0, 0, 0 );
	ri.vNormal = nShade;
	ri.vGeomNormal = geomN;
	ri.onb.CreateFromW( nShade );
	ri.ptCoord = Point2( 0.5, 0.5 );

	// From-inside IOR stack: current object pushed with the glass IOR,
	// base environment is air (matches DielectricARTest's "FROM INSIDE"
	// stack pattern).
	IORStack iorStack( 1.0 );
	iorStack.SetCurrentObject( stub );
	iorStack.push( 1.55 );

	// --- RGB path ---
	{
		ScatteredRayContainer scattered;
		spf->Scatter( ri, sampler, scattered, iorStack );

		int nReflect = 0, nOther = 0;
		Vector3 reflectDir( 0, 0, 0 );
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			if( scattered[i].type == ScatteredRay::eRayReflection ) {
				nReflect++;
				reflectDir = scattered[i].ray.Dir();
			} else {
				nOther++;
			}
		}

		CHECK( nReflect == 1, "TIR must emit exactly one mandatory reflection ray (RGB), got " << nReflect );
		CHECK( nOther == 0, "TIR must not also emit a transmission ray (RGB), got " << nOther );
		if( nReflect == 1 ) {
			const Scalar dotGeom = Vector3Ops::Dot( reflectDir, geomN );
			CHECK( dotGeom <= 1e-6, "TIR reflection tunnelled through the geometric horizon (RGB, dot=" << dotGeom << ")" );
		}
	}

	// --- Spectral (NM) path ---
	{
		ScatteredRayContainer scattered;
		spf->ScatterNM( ri, sampler, 550.0, scattered, iorStack );

		int nReflect = 0, nOther = 0;
		Vector3 reflectDir( 0, 0, 0 );
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			if( scattered[i].type == ScatteredRay::eRayReflection ) {
				nReflect++;
				reflectDir = scattered[i].ray.Dir();
			} else {
				nOther++;
			}
		}

		CHECK( nReflect == 1, "TIR must emit exactly one mandatory reflection ray (NM), got " << nReflect );
		CHECK( nOther == 0, "TIR must not also emit a transmission ray (NM), got " << nOther );
		if( nReflect == 1 ) {
			const Scalar dotGeom = Vector3Ops::Dot( reflectDir, geomN );
			CHECK( dotGeom <= 1e-6, "TIR reflection tunnelled through the geometric horizon (NM, dot=" << dotGeom << ")" );
		}
	}

	spf->release();
	tau->release();
	ior->release();
	scat->release();
	stub->release();
}

// ============================================================
//  Test 8c: zero vGeomNormal falls back to the shading normal
// ============================================================

static void TestZeroGeomNormalFallback()
{
	std::cout << "--- Test 8c: zero vGeomNormal falls back to shading normal ---" << std::endl;

	// A (hypothetical future) geometry that leaves vGeomNormal at its
	// zero default must NOT silently disable the modifier: the guard
	// falls back to the shading normal.  Facets must still fire, and
	// outputs stay guarded against the fallback normal.
	const GlintModifier& mod = *MakeMod( 2.0, 1.0, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 9 );

	RandomNumberGenerator rng( 99 );
	int perturbed = 0;
	for( int i = 0; i < 5000; i++ )
	{
		const Point3 p( rng.CanonicalRandom() * 40.0, rng.CanonicalRandom() * 40.0, 0.0 );
		RayIntersectionGeometric ri = MakeRI( p, Vector3Ops::Normalize( Vector3( 0.2, -0.3, -1 ) ) );
		ri.vGeomNormal = Vector3( 0, 0, 0 );

		mod.Modify( ri );

		const Scalar lenN = Vector3Ops::Magnitude( ri.vNormal );
		CHECK( fabs( lenN - 1.0 ) < 1e-9, "non-unit normal under zero vGeomNormal (" << lenN << ")" );
		if( Vector3Ops::Dot( ri.vNormal, Vector3( 0, 0, 1 ) ) < 1.0 - 1e-12 ) perturbed++;
	}
	std::cout << "  " << perturbed << "/5000 hits perturbed with zero vGeomNormal" << std::endl;
	CHECK( perturbed > 1500, "modifier silently inert under zero vGeomNormal (" << perturbed << ")" );
}

// ============================================================
//  Test 8d: ray-anchored geometric-horizon gate holds in the
//  DISAGREEMENT BAND (review-round-4 W1 regression pin; a4fb884d)
// ============================================================

//! a4fb884d fixed an INVERTED gate.  The pre-fix orientation rule
//! picked geomN's side by checking AGREEMENT WITH THE SHADING FRAME
//! (`Dot(geomNRaw, n) >= 0`), where `n` is the shading normal AFTER
//! GGXSPF::Scatter's FlipW.  In the DISAGREEMENT BAND -- a genuinely
//! front-facing hit (Dot(rayDir, vGeomNormal) < 0) whose shading normal
//! is tilted far enough that Dot(rayDir, vNormal) > 0 -- FlipW fires on
//! that front-facing hit and flips the shading frame INWARD; the old
//! rule then flipped geomN inward too, so the gate accepted ONLY
//! directions tunnelling into the object (the exact mechanism behind
//! the glint firefly outlier growth 82->95->127 across the spp ladder
//! documented in the commit).  The fix anchors geomN's orientation to
//! `ri.ray.Dir()` alone (`Dot(geomNRaw, ri.ray.Dir()) < 0`), which
//! cannot be fooled by the shading frame at all.
//!
//! This test pins that disagreement-band configuration DIRECTLY
//! (mirroring TestDielectricTIRUnderTiltedNormal's construction style)
//! rather than searching GlintModifier's stochastic facet field for a
//! qualifying hit, so the regression can never silently return: the
//! gate must anchor to the RAY, not the shading frame.
static void TestRayAnchoredGateDisagreementBand()
{
	std::cout << "--- Test 8d: ray-anchored gate holds in the disagreement band (a4fb884d) ---" << std::endl;

	GlobalLog();		// initialize logger before material construction

	StubObject* stub = new StubObject();
	stub->addref();

	// Near-mirror silver GGX -- alpha 0.02 (near-specular) with the same
	// approximate visible-band silver conductor ior/ext used by Test 6's
	// consistency check and by glint_flakes.RISEscene's silver.
	UniformColorPainter* diffuse = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) );	diffuse->addref();
	UniformColorPainter* specular = new UniformColorPainter( RISEPel( 0.95, 0.95, 0.95 ) );	specular->addref();
	UniformScalarPainter* alphaX = new UniformScalarPainter( 0.02 );	alphaX->addref();
	UniformScalarPainter* alphaY = new UniformScalarPainter( 0.02 );	alphaY->addref();
	UniformScalarPainter* ior = new UniformScalarPainter( 0.15 );		ior->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 3.5 );		ext->addref();

	GGXSPF* spf = new GGXSPF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, eFresnelConductor );	spf->addref();
	GGXBRDF* brdf = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, eFresnelConductor );	brdf->addref();

	// Disagreement-band construction: flat geometric surface (true
	// outward normal = +Z), a ray that genuinely hits the FRONT face
	// (Dot(rayDir, geomN) < 0, ~-0.42 -- matching the commit's own
	// firefly-trace numbers), but a shading normal tilted far enough
	// off +Z that Dot(rayDir, vNormal) > 0 (~+0.12) -- the exact
	// sign(rayDir.Ns) != sign(rayDir.Ng) band the fix targets.
	const Vector3 geomNTrue( 0, 0, 1 );
	const Scalar rayTiltDeg = 65.17;		// ray's angle off normal incidence
	const Scalar shadeTiltDeg = 31.73;		// shading normal's tilt off geomNTrue
	const Vector3 rayDir = Vector3Ops::Normalize(
		Vector3( sin( rayTiltDeg * DEG_TO_RAD ), 0, -cos( rayTiltDeg * DEG_TO_RAD ) ) );
	const Vector3 nShade = Vector3Ops::Normalize(
		Vector3( sin( shadeTiltDeg * DEG_TO_RAD ), 0, cos( shadeTiltDeg * DEG_TO_RAD ) ) );

	const Scalar dotGeom = Vector3Ops::Dot( rayDir, geomNTrue );
	const Scalar dotShade = Vector3Ops::Dot( rayDir, nShade );
	CHECK( dotGeom < -0.1, "test construction: ray must clearly hit the front face (dotGeom=" << dotGeom << ")" );
	CHECK( dotShade > 0.01, "test construction: shading normal must be tilted past the view (dotShade=" << dotShade << ")" );

	Ray inRay( Point3( -rayDir.x, -rayDir.y, -rayDir.z ), rayDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.ptObjIntersec = Point3( 0, 0, 0 );
	ri.vNormal = nShade;
	ri.vGeomNormal = geomNTrue;
	ri.onb.CreateFromW( nShade );
	ri.ptCoord = Point2( 0.5, 0.5 );

	IORStack iorStack = MakeTestIORStack( stub );
	RandomNumberGenerator rng( 4177 );
	IndependentSampler sampler( rng );

	const int nDraws = 2000;
	int nEmitted = 0;

	for( int i = 0; i < nDraws; i++ )
	{
		ScatteredRayContainer scattered;
		spf->Scatter( ri, sampler, scattered, iorStack );
		for( unsigned int s = 0; s < scattered.Count(); s++ ) {
			nEmitted++;
			const Scalar dotOut = Vector3Ops::Dot( scattered[s].ray.Dir(), geomNTrue );
			CHECK( dotOut > 0, "RGB emitted ray tunnelled through the geometric horizon (dot=" << dotOut << ")" );
		}
	}

	int nEmittedNM = 0;
	const Scalar nm = 550.0;
	for( int i = 0; i < nDraws; i++ )
	{
		ScatteredRayContainer scattered;
		spf->ScatterNM( ri, sampler, nm, scattered, iorStack );
		for( unsigned int s = 0; s < scattered.Count(); s++ ) {
			nEmittedNM++;
			const Scalar dotOut = Vector3Ops::Dot( scattered[s].ray.Dir(), geomNTrue );
			CHECK( dotOut > 0, "NM emitted ray tunnelled through the geometric horizon (dot=" << dotOut << ")" );
		}
	}

	std::cout << "  dotGeom=" << dotGeom << " dotShade=" << dotShade
	          << "  RGB emitted=" << nEmitted << "/" << nDraws
	          << " NM emitted=" << nEmittedNM << "/" << nDraws
	          << " (0 tunnelling emissions required, of any count)" << std::endl;

	// Pdf()/PdfNM() must independently agree: a probe direction pointing
	// straight INTO the object (-geomNTrue) has zero density -- the
	// sampler could never emit it, so density must be zero for MIS
	// consistency (GGXSPF::Pdf's own gate comment).
	const Vector3 intoObject = -geomNTrue;
	const Scalar pdfInto = spf->Pdf( ri, intoObject, iorStack );
	const Scalar pdfIntoNM = spf->PdfNM( ri, intoObject, nm, iorStack );
	CHECK( pdfInto == 0.0, "Pdf() nonzero for a direction pointing into the object (" << pdfInto << ")" );
	CHECK( pdfIntoNM == 0.0, "PdfNM() nonzero for a direction pointing into the object (" << pdfIntoNM << ")" );

	// value()/valueNM() (the companion BRDF) must independently agree:
	// black for the same into-the-object probe direction.
	const RISEPel valInto = brdf->value( intoObject, ri );
	const Scalar valIntoNM = brdf->valueNM( intoObject, ri, nm );
	CHECK( ColorMath::MaxValue( valInto ) == 0.0, "value() nonzero for a direction pointing into the object (" << ColorMath::MaxValue( valInto ) << ")" );
	CHECK( valIntoNM == 0.0, "valueNM() nonzero for a direction pointing into the object (" << valIntoNM << ")" );

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
//  Test 9: hostile-coordinate determinism (the 2^40 fold)
// ============================================================

static void TestHostileCoordinates()
{
	std::cout << "--- Test 9: hostile-coordinate determinism ---" << std::endl;

	const GlintModifier& mod = *MakeMod( 1.0, 0.5, 1.0, 4.0, Vector3(1,1,1), Vector3(0,0,0), 7 );

	// Far beyond the 2^40 fold period, positive and negative.
	const Point3 far1( 3.1e12, -2.7e12, 5.0e11 );
	const Point3 far2( -1.9e13, 4.4e12, -8.8e12 );

	int found = 0;
	const Point3 bases[2] = { far1, far2 };
	for( int b = 0; b < 2; b++ )
	{
		for( int k = 0; k < 2000; k++ )
		{
			// Scan a local neighbourhood at the hostile offset (steps
			// resolvable in double at this magnitude).
			const Point3 p( bases[b].x + k * 0.25, bases[b].y, bases[b].z );
			const GlintFacet f1 = mod.FindFacet( p );
			const GlintFacet f2 = mod.FindFacet( p );
			CHECK( f1.found == f2.found &&
				( !f1.found || ( f1.centreQ.x == f2.centreQ.x && f1.theta == f2.theta && f1.phi == f2.phi ) ),
				"hostile-coordinate FindFacet not bit-deterministic at b=" << b << " k=" << k );
			if( f1.found ) {
				found++;
				CHECK( f1.theta >= 0 && f1.theta <= 1.0471975511965976 + 1e-12,
					"hostile-coordinate tilt out of range (" << f1.theta << ")" );
			}
		}
	}
	std::cout << "  " << found << "/4000 hostile-coordinate points on facets" << std::endl;
	CHECK( found > 400, "facet field degenerate at hostile coordinates (" << found << ")" );
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
	TestSeedScaleShift();
	TestNearestWins();
	TestFlippedOrientationGuard();
	TestDielectricAtPerturbedHits();
	TestDielectricTIRUnderTiltedNormal();
	TestNonFiniteInert();
	TestZeroGeomNormalFallback();
	TestRayAnchoredGateDisagreementBand();
	TestHostileCoordinates();

	ReleaseMods();

	if( g_failures == 0 ) {
		std::cout << std::endl << "=== ALL TESTS PASSED ===" << std::endl;
		return 0;
	}
	std::cout << std::endl << "=== " << g_failures << " FAILURES ===" << std::endl;
	return 1;
}
