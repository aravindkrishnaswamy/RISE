//////////////////////////////////////////////////////////////////////
//
//  BSSRDFPlanarProbeReachTest.cpp - DL-52 red-proof regression:
//    BSSRDFSampling::SampleEntryPoint's normal-axis probe must reach
//    the nearby (coplanar) surface on a broad flat face.
//
//    THE BUG THIS GUARDS AGAINST:
//      SampleEntryPoint's Step 6 used to start BOTH probe rays (+axis
//      and -axis) AT probeCenter -- a point that lies IN the
//      projection plane through the exit point -- and advance
//      BSSRDF_RAY_EPSILON before the first intersection test.  On a
//      broad flat face, a lateral tangent/bitangent offset never
//      leaves the plane of the surface, so probeCenter is ITSELF
//      coplanar with the nearby surface.  Advancing along +normal
//      moves the ray origin to the far side of that surface (away
//      from it, so tracing +normal never crosses it); advancing along
//      -normal moves the origin to the near side (behind where the
//      ray started, so tracing -normal has already passed it).  The
//      near-coplanar surface -- the entire point of the disk-
//      projection scheme -- was skipped BY CONSTRUCTION on every
//      normal-axis probe (50% of samples), on any flat or nearly-flat
//      surface, regardless of scene content.
//
//    WHAT THIS TEST DOES:
//      Uses a REAL InfinitePlaneGeometry (the flattest possible face,
//      and the starkest failure mode: there is no "distant" surface
//      anywhere for a broken probe to fall back onto, so a skipped
//      near-hit means num_hits == 0 and SampleEntryPoint reports
//      invalid) and a real SubSurfaceScatteringMaterial's diffusion
//      profile.  A deterministic sampler forces the axis-selection
//      draw below 0.5 (the "normal axis" branch) on every attempt,
//      while leaving every other draw genuinely random.  On the
//      unfixed library this must fail to reach the surface (valid ==
//      false) on essentially every attempt; after the fix it must
//      reach it (valid == true, with an entry point coplanar with the
//      exit point) on essentially every attempt.
//
//    DL-68 red-proof (added in the same slice that closed DL-52):
//      the single-chord fix above travels in ONE fixed direction
//      (+probeAxis) for its whole length.  On a geometry that
//      re-orients its reported normal to face the incoming ray
//      (RayIntersectionGeometric::bGeomNormalOrientedToRay --
//      double-sided triangle meshes and ClippedPlaneGeometry), a hit
//      on the near (-axis) side of probeCenter -- exactly the
//      coplanar hit this test's flat-face case exists to find --
//      came back with an INVERTED entry normal (facing into the
//      solid instead of out of it) on the unfixed chord code, because
//      the flip orients against the chord's actual +probeAxis travel
//      direction rather than the physically-correct -probeAxis
//      approach.  `DoubleSidedQuad`/`DoubleSidedClippedPlane` below
//      build a coplanar double-sided surface (the InfinitePlaneGeometry
//      case above is single-sided and therefore immune -- it never
//      sets the flag) and assert the recovered entryNormal agrees
//      with the exit point's own outward normal, and that the real
//      production NEE adapter (BSSRDFAdapters::BSSRDFEntryBSDF) gives
//      a positive value for an exterior light straight above the
//      surface -- both fail on the unfixed orientation logic and pass
//      after it.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
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

//////////////////////////////////////////////////////////////////////
// Deterministic sampler: forces the SECOND Get1D() draw (the
// axis-selection sample in SampleEntryPoint's Step 2) below 0.5, so
// every attempt takes the "normal axis" branch (probeAxis =
// exitNormal).  Every other draw is a genuine random sample from a
// per-attempt seed, so radius/phi/hit-selection/cosine-direction vary
// normally across attempts.
//////////////////////////////////////////////////////////////////////
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
			// The axis-selection draw: force < 0.5 -> normal axis.
			return 0.15;
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

// DL-68: a broad DOUBLE-SIDED quad, authored coplanar with the exit
// point exactly like MakeInfinitePlane() above, but through a
// geometry type that sets bGeomNormalOrientedToRay -- the disk
// probe's coplanar near-hit is now REACHABLE (DL-52 fixed that), but
// on the unfixed orientation logic its normal comes back flipped.
// Per-vertex normals are stored explicitly as +Z on both triangles
// (the common, consistent-winding authoring case), so probing along
// +probeAxis==+Z triggers the ray-facing flip on every near-half hit
// -- exactly the scenario the fix targets.
static Object* MakeDoubleSidedQuadMesh()
{
	TriangleMeshGeometry* pGeo = new TriangleMeshGeometry( /*bDoubleSided_=*/true );

	const Scalar half = 100.0;
	const Point3 verts[4] = {
		Point3( -half, -half, 0 ), Point3( half, -half, 0 ),
		Point3( half, half, 0 ), Point3( -half, half, 0 ) };
	const Vector3 n( 0, 0, 1 );
	const Point2 uv[4] = { Point2(0,0), Point2(1,0), Point2(1,1), Point2(0,1) };

	Triangle t1, t2;
	const int idx1[3] = { 0, 1, 2 };
	const int idx2[3] = { 0, 2, 3 };
	for( int k = 0; k < 3; k++ ) {
		t1.vertices[k] = verts[ idx1[k] ]; t1.normals[k] = n; t1.coords[k] = uv[ idx1[k] ];
		t2.vertices[k] = verts[ idx2[k] ]; t2.normals[k] = n; t2.coords[k] = uv[ idx2[k] ];
	}
	pGeo->AddTriangle( t1 );
	pGeo->AddTriangle( t2 );
	pGeo->DoneTriangles();

	pGeo->addref();
	Object* pObj = new Object( pGeo );
	pObj->addref();
	pGeo->release();
	return pObj;
}

// DL-68: same coplanar broad-quad scenario through ClippedPlaneGeometry
// (analytic bilinear normal, double-sided by construction here) --
// the sibling flip site named in the DL-68 recipe alongside the mesh.
static Object* MakeDoubleSidedClippedPlane()
{
	const Scalar half = 100.0;
	const Point3 corners[4] = {
		Point3( -half, -half, 0 ), Point3( half, -half, 0 ),
		Point3( half, half, 0 ), Point3( -half, half, 0 ) };
	ClippedPlaneGeometry* pGeo = new ClippedPlaneGeometry( corners, /*bDoubleSided_=*/true );
	pGeo->addref();
	Object* pObj = new Object( pGeo );
	pObj->addref();
	pGeo->release();
	return pObj;
}

static Object* MakeInfinitePlane()
{
	InfinitePlaneGeometry* pGeo = new InfinitePlaneGeometry( 1.0, 1.0 );
	pGeo->addref();
	Object* pObj = new Object( pGeo );
	pObj->addref();
	pGeo->release();
	return pObj;
}

// A control object with a genuinely nearby BUT non-coplanar surface
// (a sphere small enough that the exit point's tangent plane visibly
// curves away from the surface within the profile's probe reach),
// used to confirm the forcing sampler and material setup are
// otherwise healthy -- SampleEntryPoint should find SOME entry points
// here even under the old center-anchored two-direction probe, since
// tracing "inward" (toward the sphere's interior) from a
// tangent-plane point that sits just outside a curved surface still
// crosses that surface; only the DEGENERATE coplanar case (an exactly
// flat face) skips it on both directions. This isolates the flat-face
// failure below from a broken harness -- it is a weak sanity floor
// (some successes), not a claim that the old algorithm is otherwise
// correct on curved geometry.
static Object* MakeLargeSphere()
{
	SphereGeometry* pGeo = new SphereGeometry( 2.0 );
	pGeo->addref();
	Object* pObj = new Object( pGeo );
	pObj->addref();
	pGeo->release();
	return pObj;
}

static RayIntersectionGeometric MakeExitRecordOnPlane()
{
	// Exit point at the plane origin, normal +Z (matches
	// InfinitePlaneGeometry's front-face normal), incoming ray from
	// above so FresnelTransmission at the exit point is well-defined
	// (cosExit == 1).
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
	ri.onb.CreateFromW( normal );
	return ri;
}

static RayIntersectionGeometric MakeExitRecordOnSphere()
{
	const Point3 point( 0, -2, 0 );
	const Vector3 normal( 0, -1, 0 );
	const Vector3 incoming( 0, 1, 0 );
	RayIntersectionGeometric ri(
		Ray( Point3Ops::mkPoint3( point, -incoming * 2.0 ), incoming ),
		nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = point;
	ri.vNormal = normal;
	ri.vGeomNormal = normal;
	ri.onb.CreateFromW( normal );
	return ri;
}

struct ReachStats
{
	int attempts;
	int valid;
	int coplanar;	// valid AND entry point within tolerance of the exit plane
};

static ReachStats RunReachTrial(
	const RayIntersectionGeometric& exit,
	Object* pObject,
	SubSurfaceScatteringMaterial* pMaterial,
	const unsigned int seedBase,
	const int N,
	const bool checkCoplanar )
{
	ReachStats stats{ N, 0, 0 };
	for( int i = 0; i < N; i++ )
	{
		NormalAxisForcingSampler sampler( seedBase + static_cast<unsigned int>(i) );
		const BSSRDFSampling::SampleResult result = BSSRDFSampling::SampleEntryPoint(
			exit, pObject, pMaterial, sampler, 0 );

		if( !result.valid ) continue;
		stats.valid++;

		if( checkCoplanar )
		{
			// The entry point (minus its normal-offset epsilon) should
			// lie almost exactly on the z=0 plane -- it was found by a
			// probe centered ON that plane, offset only laterally.
			const Point3 onSurface = Point3Ops::mkPoint3(
				result.entryPoint, -result.entryNormal * BSSRDFSampling::BSSRDF_RAY_EPSILON );
			if( std::fabs( onSurface.z ) < 1e-4 ) {
				stats.coplanar++;
			}
		}
	}
	return stats;
}

struct OrientationStats
{
	int attempts;
	int valid;
	int coplanar;			// valid AND entry point within tolerance of the exit plane
	int outwardOriented;	// entryNormal in the same hemisphere as the exit point's own outward normal
	int neePositive;		// real BSSRDFEntryBSDF::value() is nonzero for an exterior light
};

// DL-68: exercises the SAME reach path as RunReachTrial, but on a
// coplanar DOUBLE-SIDED surface, and additionally checks that the
// recovered entry normal is outward-oriented (matches the exit
// point's own +Z normal -- this is a single flat, coplanar surface,
// so there is no legitimate reason for the entry side to disagree)
// and that the real production NEE adapter used at BSSRDF entry
// points (BSSRDFAdapters::BSSRDFEntryBSDF, BSSRDFEntryAdapters.h)
// gives a positive value for a light straight above the surface --
// exactly the site the DL-68 recipe names as broken by an inverted
// entry normal (`cosTheta = Dot(vLightIn, vNormal) <= 0` zeroing NEE
// for every exterior light).
static OrientationStats RunOrientationTrial(
	const RayIntersectionGeometric& exit,
	Object* pObject,
	SubSurfaceScatteringMaterial* pMaterial,
	const unsigned int seedBase,
	const int N )
{
	OrientationStats stats{ N, 0, 0, 0, 0 };
	ISubSurfaceDiffusionProfile* pProfile = pMaterial->GetDiffusionProfile();
	const Vector3 exteriorLightDir( 0, 0, 1 );	// straight above the z=0 surface

	for( int i = 0; i < N; i++ )
	{
		NormalAxisForcingSampler sampler( seedBase + static_cast<unsigned int>(i) );
		const BSSRDFSampling::SampleResult result = BSSRDFSampling::SampleEntryPoint(
			exit, pObject, pMaterial, sampler, 0 );

		if( !result.valid ) continue;
		stats.valid++;

		const Point3 onSurface = Point3Ops::mkPoint3(
			result.entryPoint, -result.entryNormal * BSSRDFSampling::BSSRDF_RAY_EPSILON );
		if( std::fabs( onSurface.z ) < 1e-4 ) {
			stats.coplanar++;
		}

		if( Vector3Ops::Dot( result.entryNormal, Vector3( 0, 0, 1 ) ) > 0 ) {
			stats.outwardOriented++;
		}

		// Build the exact record shape production wires up at a BSSRDF
		// entry point (PathTracingIntegrator.cpp / BDPTIntegrator.cpp:
		// entryRI.vNormal/vGeomNormal/onb = bssrdf.entryNormal/
		// entryGeomNormal/entryONB) and drive the real adapter BSDF.
		RayIntersectionGeometric entryRI = exit;
		entryRI.ptIntersection = result.entryPoint;
		entryRI.vNormal = result.entryNormal;
		entryRI.vGeomNormal = result.entryGeomNormal;
		entryRI.onb = result.entryONB;

		BSSRDFAdapters::BSSRDFEntryBSDF entryBSDF( pProfile, 0 );
		const RISEPel Sw = entryBSDF.value( exteriorLightDir, entryRI );
		if( Sw[0] > 0 ) {
			stats.neePositive++;
		}
	}
	return stats;
}

} // namespace

int main()
{
	std::cout << "=== BSSRDFPlanarProbeReachTest (DL-52 / DL-68) ===" << std::endl;

	UniformScalarPainter* ior = new UniformScalarPainter( 1.3 );
	RGBScalarPainter* absorption = new RGBScalarPainter( 0.05, 0.10, 0.20 );
	RGBScalarPainter* scattering = new RGBScalarPainter( 1.0, 1.0, 1.0 );
	SubSurfaceScatteringMaterial* material = new SubSurfaceScatteringMaterial(
		*ior, *absorption, *scattering, 0.0, 0.2 );

	const int N = 500;

	// --- Control: large sphere, non-coplanar tangent plane ---
	{
		Object* pSphere = MakeLargeSphere();
		const RayIntersectionGeometric exit = MakeExitRecordOnSphere();
		const ReachStats stats = RunReachTrial( exit, pSphere, material, 9000u, N, false );
		std::cout << "Control (sphere, normal-axis forced): valid " << stats.valid
			<< "/" << stats.attempts << std::endl;
		if( stats.valid < stats.attempts / 10 ) {
			Fail( "control sphere: harness/material setup unhealthy (normal-axis probe found almost nothing on curved geometry)" );
		}
		pSphere->release();
	}

	// --- DL-52 target: broad flat face, normal-axis probe must reach it ---
	{
		Object* pPlane = MakeInfinitePlane();
		const RayIntersectionGeometric exit = MakeExitRecordOnPlane();
		const ReachStats stats = RunReachTrial( exit, pPlane, material, 1000u, N, true );
		std::cout << "Flat face (plane, normal-axis forced): valid " << stats.valid
			<< "/" << stats.attempts << "  coplanar " << stats.coplanar
			<< "/" << stats.valid << std::endl;

		if( stats.valid < stats.attempts * 9 / 10 ) {
			Fail( "normal-axis probe on a broad flat face must reach the nearby coplanar surface "
				"(DL-52: the old center-anchored two-direction probe skips it by construction)" );
		}
		if( stats.valid > 0 && stats.coplanar < stats.valid * 9 / 10 ) {
			Fail( "reached entry points on the flat face must lie on the exit point's own surface, not a distant one" );
		}
		pPlane->release();
	}

	// --- DL-68: coplanar DOUBLE-SIDED mesh, entry normal must be outward ---
	{
		Object* pMesh = MakeDoubleSidedQuadMesh();
		const RayIntersectionGeometric exit = MakeExitRecordOnPlane();
		const OrientationStats stats = RunOrientationTrial( exit, pMesh, material, 2000u, N );
		std::cout << "Double-sided quad mesh (normal-axis forced): valid " << stats.valid
			<< "/" << stats.attempts << "  coplanar " << stats.coplanar << "/" << stats.valid
			<< "  outward " << stats.outwardOriented << "/" << stats.valid
			<< "  neePositive " << stats.neePositive << "/" << stats.valid << std::endl;

		if( stats.valid < stats.attempts * 9 / 10 ) {
			Fail( "double-sided quad mesh: normal-axis probe must reach the nearby coplanar surface" );
		}
		if( stats.valid > 0 && stats.outwardOriented < stats.valid * 9 / 10 ) {
			Fail( "double-sided quad mesh: entry normal must face outward (agree with the exit "
				"point's own normal) -- DL-68: the single-direction chord reports an INVERTED "
				"normal for near-half hits on geometry that orients normals toward the incoming ray" );
		}
		if( stats.valid > 0 && stats.neePositive < stats.valid * 9 / 10 ) {
			Fail( "double-sided quad mesh: BSSRDFEntryBSDF must give a positive value for an "
				"exterior light -- DL-68: an inverted entry normal zeroes NEE at every entry point" );
		}
		pMesh->release();
	}

	// --- DL-68: coplanar DOUBLE-SIDED clipped plane, same check ---
	{
		Object* pPlane = MakeDoubleSidedClippedPlane();
		const RayIntersectionGeometric exit = MakeExitRecordOnPlane();
		const OrientationStats stats = RunOrientationTrial( exit, pPlane, material, 3000u, N );
		std::cout << "Double-sided clipped plane (normal-axis forced): valid " << stats.valid
			<< "/" << stats.attempts << "  coplanar " << stats.coplanar << "/" << stats.valid
			<< "  outward " << stats.outwardOriented << "/" << stats.valid
			<< "  neePositive " << stats.neePositive << "/" << stats.valid << std::endl;

		if( stats.valid < stats.attempts * 9 / 10 ) {
			Fail( "double-sided clipped plane: normal-axis probe must reach the nearby coplanar surface" );
		}
		if( stats.valid > 0 && stats.outwardOriented < stats.valid * 9 / 10 ) {
			Fail( "double-sided clipped plane: entry normal must face outward (agree with the exit "
				"point's own normal) -- DL-68 inverted-normal regression" );
		}
		if( stats.valid > 0 && stats.neePositive < stats.valid * 9 / 10 ) {
			Fail( "double-sided clipped plane: BSSRDFEntryBSDF must give a positive value for an "
				"exterior light -- DL-68 inverted-normal regression zeroes NEE" );
		}
		pPlane->release();
	}

	material->release();
	scattering->release();
	absorption->release();
	ior->release();

	std::cout << "Failures: " << gFailures << std::endl;
	return gFailures == 0 ? 0 : 1;
}
