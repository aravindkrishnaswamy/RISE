//////////////////////////////////////////////////////////////////////
//
//  HairSSSEntryNormalTest.cpp - DL-75 closure: characterizes what
//    BSSRDFSampling::SampleEntryPoint actually produces when the probe
//    lands on HairGeometry.
//
//  BACKGROUND (see BSSRDFSampling.cpp's own DL-71/DL-75 comment block,
//  HairGeometry.h ~:193-206, HairGeometry.cpp ~:1029-1040):
//
//    HairGeometry reports TWO different normals at a hit:
//      - vGeomNormal ("Nflat", the ribbon-plane normal) is DERIVED
//        FROM THE RAY -- it is defined to always face whichever ray
//        hit the strand, so it carries no fixed physical meaning and
//        `bGeomNormalOrientedToRay`'s un-flip recovery (DL-71) cannot
//        recover a genuine "other side" from it.  DL-75 records this:
//        `bGeomNormalRayDerived` is set so BSSRDFSampling's probe loop
//        and IORStackSeeding::TallyProbe both SKIP the recovery for
//        hair rather than apply a meaningless correction.
//      - vNormal ("Ncyl", the cylinder shading normal) is a genuine
//        exact-cylinder radial-outward direction at the point on the
//        cross-section the ray happened to graze -- a real geometric
//        fact about the fibre's surface, independent of which
//        direction the ray approached from.
//
//    SampleEntryPoint's continuation frame is built from vNormal
//    (`entryNormal` in SampleResult, driving both the cosine-weighted
//    scattered ray and the NEE Fresnel angle in BSSRDFEntryAdapters.h)
//    -- NOT from vGeomNormal.  Because the gate in BSSRDFSampling.cpp
//    skips the DL-71 orientation correction for
//    `bGeomNormalRayDerived` hits, a hair hit's `h.normal` (Ncyl) is
//    passed through UNCHANGED, and the cosine-weighted continuation
//    is built directly around it
//    (`cosineONB.CreateFromW(entryNormal)`).  A cosine-hemisphere
//    sample built around a given axis satisfies
//    `Dot(sampledDir, axis) == cosTheta >= 0` BY CONSTRUCTION -- so
//    the continuation can never point into the "wrong" hemisphere
//    relative to `entryNormal`, whatever `entryNormal` physically
//    means for a sub-micron fibre.  This is the closure this test
//    pins: no crash, no NaN, no inverted continuation.
//
//    What remains UNDEFINED (and is NOT fixed here, matching the DL-75
//    recipe's option (a) -- characterize, don't force a specific
//    number): the disk-to-surface area Jacobian
//    (`entryGeomNormal` = Nflat, ray-derived) feeds `cosN`/`cosT`/
//    `cosB` in SampleEntryPoint's PDF, so the SAME physical point on a
//    fibre can report a DIFFERENT Jacobian depending on which probe
//    axis/direction reached it.  A `subsurfacescattering_material`
//    bound to `hair_geometry` therefore renders SOMETHING well-defined
//    (a real, finite, non-inverted continuation) but its BSSRDF
//    energy/PDF is not claimed to be physically correct for a
//    sub-micron fibre treated as a locally-flat diffusion surface --
//    this combination is accepted as a documented, bounded limitation
//    rather than rejected outright, since it neither crashes nor
//    silently corrupts the walk (unlike an un-gated orientation
//    "correction" would have).
//
//  WHAT THIS TEST DOES:
//    Builds a real HairGeometry strand (wide enough, relative to the
//    profile's effective radius, that the stochastic disk probe finds
//    it on close to every attempt -- the same "generous width" trick
//    BSSRDFPlanarProbeReachTest.cpp uses for its flat-plane control)
//    and a real SubSurfaceScatteringMaterial, then drives
//    BSSRDFSampling::SampleEntryPoint through many trials.  For every
//    valid sample it asserts: every returned field is finite; the
//    reported entry normal is unit length; and the cosine-weighted
//    continuation's direction is never in the opposite hemisphere
//    from the entry normal (`Dot(scatteredRay.Dir(), entryNormal) >=
//    0`).  A white-box sub-test also pins HairGeometry's own
//    `bGeomNormalOrientedToRay`/`bGeomNormalRayDerived` contract
//    directly, and confirms the production NEE adapter
//    (BSSRDFAdapters::BSSRDFEntryBSDF) returns a finite, non-negative
//    value rather than crashing or NaN-ing on a hair entry record.
//
//  Build (from project root):
//    make -C build/make/rise build-test/HairSSSEntryNormalTest
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Shaders/BSSRDFEntryAdapters.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{

static int gFailures = 0;

static void Fail( const std::string& label )
{
	std::cerr << "FAIL: " << label << std::endl;
	++gFailures;
}

static bool AllFinite3( const Vector3& v )
{
	return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
}

static bool AllFinite3( const Point3& p )
{
	return std::isfinite( p.x ) && std::isfinite( p.y ) && std::isfinite( p.z );
}

static bool AllFinitePel( const RISEPel& p )
{
	return std::isfinite( p[0] ) && std::isfinite( p[1] ) && std::isfinite( p[2] );
}

//////////////////////////////////////////////////////////////////////
// Deterministic axis-forcing sampler (mirrors
// BSSRDFPlanarProbeReachTest.cpp's NormalAxisForcingSampler): forces
// SampleEntryPoint's Step 2 axis-selection draw so the probe always
// travels along the exit normal (+Z here), while every other draw
// remains a genuine per-attempt random sample.
//////////////////////////////////////////////////////////////////////
static const Scalar kNormalAxisSample = 0.15;

class NormalAxisForcingSampler : public ISampler
{
	RandomNumberGenerator rng;
	int callIndex;
public:
	explicit NormalAxisForcingSampler( const unsigned int seed ) :
		rng( seed ), callIndex( 0 ) {}

	Scalar Get1D()
	{
		const int idx = callIndex++;
		if( idx == 1 ) {
			return kNormalAxisSample;
		}
		return rng.CanonicalRandom();
	}

	Point2 Get2D()
	{
		const Scalar u = Get1D();
		const Scalar v = Get1D();
		return Point2( u, v );
	}
};

// A single long, wide, perfectly straight strand lying along the X
// axis in the z=0 plane, passing through the origin.  HairGeometry's
// ribbon always re-orients to face the incoming ray (the "ribbon
// facing the ray" convention this whole test exists to probe), so for
// a probe ray travelling along a fixed +Z axis the strand behaves,
// for intersection purposes, like a cylinder of radius halfWidth
// around its centerline projected onto the XY view plane -- any probe
// whose lateral (tangent/bitangent) offset from the strand's line is
// within halfWidth will hit it.  `halfWidth` is sized to the
// profile's own effective probe reach so that ANY sampled radius
// (bounded by that same reach) lands within it regardless of the
// sampled angle phi -- the same "generous width" trick
// BSSRDFPlanarProbeReachTest.cpp's flat-plane control relies on for a
// near-100% hit rate.
static Object* MakeWideHairStrand( const Scalar halfLength, const Scalar fullWidth )
{
	std::vector<HairGeometry::StrandDesc> strands( 1 );
	strands[0].controlPoints.push_back( Point3( -halfLength, 0, 0 ) );
	strands[0].controlPoints.push_back( Point3(  halfLength, 0, 0 ) );
	strands[0].rootWidth = fullWidth;
	strands[0].tipWidth  = fullWidth;
	strands[0].rootUV    = Point2( 0, 0 );

	HairGeometry* pGeo = new HairGeometry( strands );
	pGeo->addref();
	Object* pObj = new Object( pGeo );
	pObj->addref();
	pGeo->release();
	return pObj;
}

// Exit point at the origin, normal +Z, tangent pinned to +X (aligned
// with the strand's own axis so the "tangent axis" probe branch --
// not exercised by this test, which forces the normal-axis branch --
// would also travel parallel to the fibre rather than across it).
static RayIntersectionGeometric MakeExitRecordOnPlane()
{
	const Point3 point( 0, 0, 0 );
	const Vector3 normal( 0, 0, 1 );
	const Vector3 incoming( 0, 0, -1 );
	RayIntersectionGeometric ri(
		Ray( Point3Ops::mkPoint3( point, -incoming * 2.0 ), incoming ),
		nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = point;
	ri.vNormal = normal;
	ri.vGeomNormal = normal;
	ri.onb.CreateFromWU( normal, Vector3( 1, 0, 0 ) );
	return ri;
}

} // namespace

int main()
{
	std::cout << "=== HairSSSEntryNormalTest (DL-75) ===" << std::endl;

	UniformScalarPainter* ior = new UniformScalarPainter( 1.3 );
	RGBScalarPainter* absorption = new RGBScalarPainter( 0.05, 0.10, 0.20 );
	RGBScalarPainter* scattering = new RGBScalarPainter( 1.0, 1.0, 1.0 );
	SubSurfaceScatteringMaterial* material = new SubSurfaceScatteringMaterial(
		*ior, *absorption, *scattering, 0.0, 0.2 );

	ISubSurfaceDiffusionProfile* pProfile = material->GetDiffusionProfile();
	const Scalar probeMaxDist = pProfile->GetMaximumDistanceForError( 1e-4 );

	// -- White-box sub-test: pin HairGeometry's own documented contract
	//    directly, so this test is tied to the actual mechanism DL-75
	//    is about rather than incidentally passing.
	{
		Object* pStrand = MakeWideHairStrand( 4.0 * probeMaxDist, 4.0 * probeMaxDist );
		Ray straightDown( Point3( 0, 0, -10.0 * probeMaxDist - 10.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( straightDown, nullRasterizerState );
		pStrand->IntersectRay( ri, 1e30, true, true, false );

		if( !ri.geometric.bHit ) {
			Fail( "white-box: a ray straight down the strand's own axis must hit it" );
		} else {
			if( !ri.geometric.bGeomNormalOrientedToRay ) {
				Fail( "white-box: HairGeometry must set bGeomNormalOrientedToRay (its ribbon "
					"always re-orients to face the incoming ray)" );
			}
			if( !ri.geometric.bGeomNormalRayDerived ) {
				Fail( "white-box: HairGeometry must set bGeomNormalRayDerived (DL-75: its "
					"reported normal is fabricated from the ray, not a recovered winding normal, "
					"so BSSRDFSampling's/IORStackSeeding's orientation-recovery must skip it)" );
			}
			if( !AllFinite3( ri.geometric.vNormal ) ||
				std::fabs( Vector3Ops::Magnitude( ri.geometric.vNormal ) - 1.0 ) > 1e-3 ) {
				Fail( "white-box: HairGeometry's shading normal (Ncyl) must be finite and unit length" );
			}
		}
		pStrand->release();
	}

	// -- Stochastic sub-test: drive the real BSSRDFSampling probe onto
	//    the hair strand and characterize every valid sample.
	{
		Object* pStrand = MakeWideHairStrand( 4.0 * probeMaxDist, 4.0 * probeMaxDist );
		const RayIntersectionGeometric exit = MakeExitRecordOnPlane();

		const int N = 500;
		int valid = 0;
		int badFinite = 0;
		int badUnitNormal = 0;
		int badContinuationHemisphere = 0;
		int badNeeAdapter = 0;
		int offStrand = 0;

		BSSRDFAdapters::BSSRDFEntryBSDF entryBSDF( pProfile, 0 );
		const Vector3 exteriorLightDir( 0, 0, 1 );	// straight "outward" in the exit's own frame

		for( int i = 0; i < N; i++ )
		{
			NormalAxisForcingSampler sampler( 6000u + static_cast<unsigned int>(i) );
			const BSSRDFSampling::SampleResult result = BSSRDFSampling::SampleEntryPoint(
				exit, pStrand, material, sampler, 0 );

			if( !result.valid ) continue;
			++valid;

			const bool finiteOK =
				AllFinite3( result.entryPoint ) &&
				AllFinite3( result.entryNormal ) &&
				AllFinite3( result.entryGeomNormal ) &&
				AllFinite3( result.scatteredRay.Dir() ) &&
				AllFinitePel( result.weight ) &&
				AllFinitePel( result.weightSpatial ) &&
				std::isfinite( result.weightNM ) &&
				std::isfinite( result.weightSpatialNM ) &&
				std::isfinite( result.cosinePdf ) &&
				std::isfinite( result.pdfSurface );
			if( !finiteOK ) {
				++badFinite;
				continue;
			}

			if( std::fabs( Vector3Ops::Magnitude( result.entryNormal ) - 1.0 ) > 1e-3 ) {
				++badUnitNormal;
			}

			// THE closure invariant: a cosine-weighted hemisphere sample
			// built around `entryNormal` can never point into the
			// opposite hemisphere, whatever that normal physically means
			// for a sub-micron fibre.
			const Scalar cosContinuation = Vector3Ops::Dot(
				Vector3Ops::Normalize( result.scatteredRay.Dir() ), result.entryNormal );
			if( cosContinuation < -1e-9 ) {
				++badContinuationHemisphere;
			}

			// The real production NEE adapter must not crash or NaN on a
			// hair entry record either.
			RayIntersectionGeometric entryRI = exit;
			entryRI.ptIntersection = result.entryPoint;
			entryRI.vNormal = result.entryNormal;
			entryRI.vGeomNormal = result.entryGeomNormal;
			entryRI.onb = result.entryONB;
			const RISEPel Sw = entryBSDF.value( exteriorLightDir, entryRI );
			if( !AllFinitePel( Sw ) || Sw[0] < 0 || Sw[1] < 0 || Sw[2] < 0 ) {
				++badNeeAdapter;
			}

			// Sanity: confirm we actually landed on the fibre (y,z near
			// the strand's own line), not vacuously passing on zero real
			// hits.
			if( std::fabs( result.entryPoint.y ) > 4.0 * probeMaxDist + 1.0 ||
				std::fabs( result.entryPoint.z ) > 4.0 * probeMaxDist + 1.0 ) {
				++offStrand;
			}
		}

		std::cout << "Hair strand (normal-axis forced): valid " << valid << "/" << N
			<< "  badFinite " << badFinite << "  badUnitNormal " << badUnitNormal
			<< "  badContinuationHemisphere " << badContinuationHemisphere
			<< "  badNeeAdapter " << badNeeAdapter << "  offStrand " << offStrand << std::endl;

		if( valid < N / 10 ) {
			Fail( "hair strand: normal-axis probe must reach the wide strand on a healthy "
				"fraction of attempts (check the strand/profile sizing is not degenerate)" );
		}
		if( badFinite > 0 ) {
			Fail( "hair strand: SampleEntryPoint must never return a non-finite field on a hair hit" );
		}
		if( badUnitNormal > 0 ) {
			Fail( "hair strand: entryNormal (HairGeometry's Ncyl) must remain unit length through the probe" );
		}
		if( badContinuationHemisphere > 0 ) {
			Fail( "hair strand: DL-75 closure invariant violated -- the cosine-weighted "
				"continuation pointed into the opposite hemisphere from entryNormal" );
		}
		if( badNeeAdapter > 0 ) {
			Fail( "hair strand: BSSRDFAdapters::BSSRDFEntryBSDF must return a finite, "
				"non-negative value for a hair entry record, not crash or NaN" );
		}
		if( offStrand > 0 ) {
			Fail( "hair strand: a 'valid' entry point must actually lie on the strand "
				"(harness sanity check)" );
		}

		pStrand->release();
	}

	material->release();
	scattering->release();
	absorption->release();
	ior->release();

	std::cout << "Failures: " << gFailures << std::endl;
	return gFailures == 0 ? 0 : 1;
}
