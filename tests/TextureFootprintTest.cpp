//////////////////////////////////////////////////////////////////////
//
//  TextureFootprintTest.cpp - Pins the per-hit pixel footprint
//  (RayIntersectionGeometric::txFootprint) now that it is produced at
//  the OBJECT layer for every geometry rather than inside the two
//  triangle-mesh intersectors, and promoted to world by the exact
//  forward linear map rather than by |det M|^(1/3).
//
//  Design + rationale: docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md
//  (this suite is its section 5).
//
//  WHAT IS UNDER TEST, and why each test can fail
//
//    1. Distance scaling.  A pinhole camera's footprint on a plane
//       facing it is exactly (distance to the surface) x (pixel
//       angular step), so on the SAME sphere seen from two distances
//       the ratio of worldWidth must be the ratio of the two
//       camera-to-surface distances.  Catches a footprint that is
//       stuck, that measures the wrong length, or that quietly uses
//       the camera-to-CENTRE distance.
//    2. Closed form.  The same hit against d*theta with
//       theta = 2*tan(fov/2)/height, within 3% (the residual is the
//       sphere's curvature over one pixel, plus tan(theta) vs theta).
//       This is the test that would catch a half-pixel-vs-full-pixel
//       convention slip, which a ratio test cannot see.
//    3. Mesh / analytic agreement.  A SphereGeometry and a
//       triangle-mesh sphere of the same radius at the same distance
//       must agree within 3%.  The analytic path is new; the mesh
//       path shipped.  This is the test that would catch a units or
//       frame mismatch between the two.
//    4. Uniform instance scale is exact.  Mirrors ReliefModifierTest
//       4c on the analytic path: scale 10 => exactly 10x, at 1e-9.
//    5. Non-uniform instance scale is exact -- the headline
//       regression.  `stretch (4, 0.05, 4)` on a face-on
//       clipped_plane, compared against the identical world ray at
//       the identical world plane through an UNSTRETCHED instance
//       (the plane y = 0 is invariant under that stretch, so the two
//       casts describe the same world geometry and must report the
//       same world footprint).  The retired `|det M|^(1/3)` fold
//       reports 4.31x too small here (0.9283 applied where the true
//       in-plane factor is 4) -- see the red-proof note in main().
//    6. UV-free geometries report a WIDTH and NO Jacobian.  A
//       box_geometry and an sdf_geometry sphere: widthValid true,
//       worldWidth > 0, valid FALSE.  `valid` false is load-bearing,
//       not incidental: TexturePainter::SampleTextured and WeaveBRDF
//       key on it, and handing them a zero UV Jacobian would read as
//       LOD 0 (finest mip) instead of the honest base-level fallback.
//    7. The `valid => widthValid` invariant, on the mesh path and on
//       an analytic primitive that has a UV chart.
//    8. CSG composes.  A csg_object union with a scaled operand
//       matches the same-scaled plain object at 1e-9, and a stretch
//       on the CSG level composes on top of that -- the per-level
//       promotion rule.
//    9. No differentials, no footprint (Ray::Set clears
//       hasDifferentials, so this is every shadow ray, NEE ray,
//       photon, and post-bounce ray in the renderer).
//   10. Grazing guard.  A near-tangent sphere hit must not produce a
//       non-finite worldWidth.
//   11. THE CHART ORACLE.  dudx..dvdy must be the derivatives of
//       ri.ptCoord -- the coordinate a texture is actually sampled at
//       -- and not of whatever private parameters the geometry
//       differentiates.  For each of sphere, ellipsoid, cylinder and
//       torus (plus the mesh control), the published Jacobian is
//       compared against a CENTRAL finite difference of ptCoord taken
//       across the pixel's own ray differentials.  This is the test
//       that catches a chart mismatch, which no ratio or closed-form
//       width test can see: before the chart map existed every one of
//       the four analytic primitives published radians where the
//       sampler expects [0, 1], mipping 1.59 (cylinder) to 2.65
//       (torus) LOD levels too blurry while the mesh control was
//       correct to 6e-4.  (Those two figures read 1.02 / 2.64 until
//       fix round 2, disagreeing with both design docs.  Re-measured
//       by replacing the chart multiply-through in
//       `SolveFootprintUV` with the raw derivative-chart
//       differentials and reading the LOD error this very test
//       prints: sphere 1.651, ellipsoid 2.330, cylinder 1.585, torus
//       2.651, mesh control unmoved at 1.8e-5.)
//   12. Chart-map honesty on the paths that have no map.  A mesh whose
//       UV triangle is degenerate falls back to a barycentric EDGE
//       frame whose dpdu is unrelated to ptCoord; it must report
//       `valid` false rather than an edge-chart Jacobian.  Likewise a
//       sphere pole (singular chart) and the +/-X seam (discontinuous
//       chart) must decline or stay finite -- never NaN.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/Cameras/OrthographicCamera.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CircularDiskGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Utilities/FiniteMath.h"	// -Ofast-resistant finiteness (test 10)
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RuntimeContext.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_passes = 0;
static int g_failures = 0;

#define CHECK( cond, msg )													\
	do {																	\
		if( !(cond) ) {														\
			std::cout << "  FAIL: " << msg << std::endl;					\
			g_failures++;													\
		} else {															\
			g_passes++;														\
		}																	\
	} while( 0 )

//////////////////////////////////////////////////////////////////////
//  Fixtures
//////////////////////////////////////////////////////////////////////

static const unsigned int kRes = 512;
static const Scalar       kFovDeg = 30.0;
static const Scalar       kPi = 3.14159265358979323846;

//! `theta`: the on-axis angular size of one pixel.
//! PinholeCamera::ComputeScaleFromFOV stretches by h/height on the y
//! axis with h = 2*tan(fov/2), and the image plane sits at unit
//! distance, so one pixel of screen y subtends 2*tan(fov/2)/height.
//! With pixelAR 1 and a square frame the x axis matches.
static Scalar PixelAngle()
{
	return Scalar( 2 ) * std::tan( kFovDeg * kPi / Scalar( 180 ) * Scalar( 0.5 ) ) / Scalar( kRes );
}

//! A square pinhole camera at `eye` looking at `at`.  `up` is a
//! parameter because a straight-down view (used by the plane tests)
//! cannot use +Y.
static PinholeCamera* MakeCamera( const Point3& eye, const Point3& at, const Vector3& up )
{
	return new PinholeCamera(
		eye, at, up,
		kFovDeg * kPi / Scalar( 180 ),			// fov, RADIANS at this layer
		kRes, kRes,
		Scalar( 1 ),							// pixelAR
		Scalar( 1 ),							// exposure
		Scalar( 0 ),							// scanningRate
		Scalar( 0 ),							// pixelRate
		Vector3( 0, 0, 0 ),						// orientation
		Vector2( 0, 0 ) );						// target_orientation
}

//! The dead-centre pixel ray, differentials included.  mxTrans
//! translates by (-w/2, -h/2, -1), so screen (kRes/2, kRes/2) maps to
//! the optical axis exactly.
static Ray CentreRay( const PinholeCamera& cam )
{
	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	Ray r;
	cam.GenerateRay( rc, r, Point2( Scalar( kRes ) * Scalar( 0.5 ), Scalar( kRes ) * Scalar( 0.5 ) ) );
	return r;
}

//! Fires `ray` at `obj` through the real Object::IntersectRay gateway
//! and hands back the resulting record.
static RayIntersection Cast( const Object& obj, const Ray& ray )
{
	RayIntersection ri( ray, nullRasterizerState );
	obj.IntersectRay( ri, RISE_INFINITY, true, true, false );
	return ri;
}

//! A smooth-normal UV sphere.  Poles on +/-Y, so the +Z point the
//! on-axis camera hits sits on the well-sampled equator.  UVs are
//! (phi/2pi, theta/pi), which gives the mesh a non-degenerate UV
//! Jacobian there -- test 7 needs `valid` to actually come back true.
//! `mirrorU` writes the u texcoord BACKWARDS (`1 - i/nu`) while leaving
//! the geometry and the triangle winding untouched -- a mirrored-UV
//! asset, the commonest real one being a symmetric character whose two
//! halves share one texture island.  It reverses `dpdu` without
//! reversing the surface, so `cross(dpdu, dpdv) . N` changes sign and
//! the mesh intersectors' right-handedness fix-up fires: it negates
//! `dpdv` and records that in the chart map as `dtdv = -1`.  Test 11
//! uses it to put that branch under the finite-difference oracle;
//! nothing else in this file mirrors.
static TriangleMeshGeometryIndexed* BuildMeshSphere( const Scalar radius, const int nu, const int nv,
                                                     const bool mirrorU = false )
{
	VerticesListType verts;
	NormalsListType  norms;
	TexCoordsListType coords;

	for( int j = 0; j <= nv; j++ ) {
		const Scalar theta = kPi * Scalar( j ) / Scalar( nv );
		const Scalar st = std::sin( theta ), ct = std::cos( theta );
		for( int i = 0; i <= nu; i++ ) {
			const Scalar phi = Scalar( 2 ) * kPi * Scalar( i ) / Scalar( nu );
			const Vector3 n( st * std::cos( phi ), ct, st * std::sin( phi ) );
			verts.push_back( Point3( n.x * radius, n.y * radius, n.z * radius ) );
			norms.push_back( n );
			const Scalar uCoord = mirrorU ? ( Scalar( 1 ) - Scalar( i ) / Scalar( nu ) )
			                              : ( Scalar( i ) / Scalar( nu ) );
			coords.push_back( Point2( uCoord, Scalar( j ) / Scalar( nv ) ) );
		}
	}

	IndexTriangleListType tris;
	for( int j = 0; j < nv; j++ ) {
		for( int i = 0; i < nu; i++ ) {
			const unsigned int a = (unsigned int)( j * ( nu + 1 ) + i );
			const unsigned int b = a + 1;
			const unsigned int c = (unsigned int)( ( j + 1 ) * ( nu + 1 ) + i );
			const unsigned int d = c + 1;

			IndexedTriangle t0;
			t0.iVertices[0] = a; t0.iVertices[1] = c; t0.iVertices[2] = b;
			t0.iNormals[0]  = a; t0.iNormals[1]  = c; t0.iNormals[2]  = b;
			t0.iCoords[0]   = a; t0.iCoords[1]   = c; t0.iCoords[2]   = b;
			tris.push_back( t0 );

			IndexedTriangle t1;
			t1.iVertices[0] = b; t1.iVertices[1] = c; t1.iVertices[2] = d;
			t1.iNormals[0]  = b; t1.iNormals[1]  = c; t1.iNormals[2]  = d;
			t1.iCoords[0]   = b; t1.iCoords[1]   = c; t1.iCoords[2]   = d;
			tris.push_back( t1 );
		}
	}

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, false );
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( verts );
	mesh->AddNormals( norms );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();
	return mesh;
}

//! A large, axis-aligned clipped plane in the y = 0 plane.  UV-free by
//! construction (ClippedPlaneGeometry never populates ri.derivatives).
//!
//! CAUTION, and the reason test 5 does NOT use this: a
//! ClippedPlaneGeometry traces the canonical bilinear surface through
//! `RayBilinearPatchIntersection`, whose Ramsey-Potter-Hansen
//! elimination is hard-coded on the ray direction's z component
//! (A1 = ax*qz - az*qx, and so on for B/C/D).  A ray with
//! dir.x == 0 AND dir.z == 0 -- i.e. exactly along +/-Y -- makes every
//! coefficient of the quadratic identically zero and the patch is
//! MISSED.  That is a pre-existing bug in the intersector, not in the
//! footprint work; test 6 therefore approaches this plane on a
//! deliberately tilted ray, and test 5 uses a circular disk (a plain
//! plane test) so its exactness assertion is not entangled with it.
static ClippedPlaneGeometry* BuildXZPlane( const Scalar half )
{
	const Point3 corners[4] = {
		Point3( -half, 0, -half ),
		Point3(  half, 0, -half ),
		Point3(  half, 0,  half ),
		Point3( -half, 0,  half )
	};
	return new ClippedPlaneGeometry( corners, true );
}

//////////////////////////////////////////////////////////////////////
//  Test 1 -- worldWidth scales linearly with camera distance
//////////////////////////////////////////////////////////////////////

//! The camera sits on +Z at `d`, the unit sphere's near pole is at
//! z = R, so the camera-to-SURFACE distance is d - R.  The footprint
//! is the auxiliary ray's displacement on the tangent plane at that
//! hit, which is exactly (d - R) * tan(pixel angle): linear in d - R,
//! NOT in d.  A footprint keyed on the camera-to-centre distance would
//! give 2.000 here instead of 2.250.
static void Test1_DistanceScaling()
{
	std::cout << "Test 1: worldWidth is linear in the camera-to-surface distance (analytic sphere)" << std::endl;

	const Scalar R = 1.0;
	SphereGeometry* g = new SphereGeometry( R );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	const Scalar d1 = 5.0, d2 = 10.0;
	Scalar w1 = 0, w2 = 0;
	bool v1 = false, v2 = false;

	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, d1 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const RayIntersection ri = Cast( *o, CentreRay( *cam ) );
		CHECK( ri.geometric.bHit, "1: the d=5 centre ray hits the sphere" );
		v1 = ri.geometric.txFootprint.widthValid;
		w1 = ri.geometric.txFootprint.worldWidth;
		cam->release();
	}
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, d2 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const RayIntersection ri = Cast( *o, CentreRay( *cam ) );
		CHECK( ri.geometric.bHit, "1: the d=10 centre ray hits the sphere" );
		v2 = ri.geometric.txFootprint.widthValid;
		w2 = ri.geometric.txFootprint.worldWidth;
		cam->release();
	}
	o->release();

	CHECK( v1 && v2, "1: an ANALYTIC sphere populates widthValid (this is the whole arc)" );
	CHECK( w1 > Scalar( 0 ), "1: (oracle) the near footprint is non-degenerate (" << w1 << ")" );

	const Scalar expected = ( d2 - R ) / ( d1 - R );		// 9/4
	const Scalar ratio    = ( w1 > 0 ) ? ( w2 / w1 ) : Scalar( 0 );
	std::cout << "    w(d=5) " << std::scientific << std::setprecision(6) << w1
	          << "   w(d=10) " << w2
	          << "   ratio " << std::defaultfloat << std::setprecision(12) << ratio
	          << " (expected " << expected << ")" << std::endl;
	CHECK( std::fabs( ratio - expected ) < Scalar( 1e-3 ),
		"1: worldWidth ratio == (d2-R)/(d1-R) == 2.25, not 2.0 (camera-to-centre) ("
		<< std::setprecision(12) << ratio << ")" );
}

//////////////////////////////////////////////////////////////////////
//  Test 2 -- closed form
//////////////////////////////////////////////////////////////////////

static void Test2_ClosedForm()
{
	std::cout << "Test 2: worldWidth matches d*theta, the pinhole closed form" << std::endl;

	const Scalar R = 1.0, d = 5.0;
	SphereGeometry* g = new SphereGeometry( R );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	PinholeCamera* cam = MakeCamera( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	const RayIntersection ri = Cast( *o, CentreRay( *cam ) );
	CHECK( ri.geometric.bHit, "2: (control) the centre ray hits" );

	const Scalar w = ri.geometric.txFootprint.worldWidth;
	const Scalar closed = ( d - R ) * PixelAngle();
	const Scalar rel = ( closed > 0 ) ? std::fabs( w - closed ) / closed : Scalar( 1 );
	std::cout << "    worldWidth " << std::scientific << std::setprecision(6) << w
	          << "   d*theta " << closed
	          << "   rel " << std::defaultfloat << std::setprecision(6) << rel << std::endl;
	CHECK( rel < Scalar( 0.03 ),
		"2: worldWidth is within 3% of (d-R)*theta -- a half-pixel-step convention would read 0.5x ("
		<< std::setprecision(6) << rel << ")" );

	cam->release();
	o->release();
}

//////////////////////////////////////////////////////////////////////
//  Test 3 -- mesh and analytic agree
//////////////////////////////////////////////////////////////////////

static void Test3_MeshAnalyticAgreement()
{
	std::cout << "Test 3: an analytic sphere and a triangle-mesh sphere of the same radius agree" << std::endl;

	const Scalar R = 1.0, d = 5.0;
	PinholeCamera* cam = MakeCamera( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	const Ray ray = CentreRay( *cam );

	Scalar wAnalytic = 0, wMesh = 0;
	{
		SphereGeometry* g = new SphereGeometry( R );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "3: (control) the analytic sphere is hit" );
		CHECK( ri.geometric.txFootprint.widthValid, "3: the analytic sphere reports widthValid" );
		wAnalytic = ri.geometric.txFootprint.worldWidth;
		o->release();
	}
	{
		TriangleMeshGeometryIndexed* m = BuildMeshSphere( R, 90, 45 );
		Object* o = new Object( m );
		m->release();
		// Rotate off the tessellation lattice: without this the on-axis
		// ray lands EXACTLY on the (0, 0, R) vertex, where the
		// interpolated normal is exactly the analytic one and the
		// comparison degenerates to an identity.  1.3 degrees about Y
		// puts the hit mid-facet, so the 3% tolerance is really
		// absorbing the mesh's normal-interpolation error.
		o->RotateObjectYAxis( Scalar( 1.3 ) );
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "3: (control) the mesh sphere is hit" );
		CHECK( ri.geometric.txFootprint.widthValid, "3: the mesh sphere reports widthValid" );
		wMesh = ri.geometric.txFootprint.worldWidth;
		o->release();
	}
	cam->release();

	const Scalar rel = ( wMesh > 0 ) ? std::fabs( wAnalytic - wMesh ) / wMesh : Scalar( 1 );
	std::cout << "    analytic " << std::scientific << std::setprecision(6) << wAnalytic
	          << "   mesh " << wMesh
	          << "   rel " << std::defaultfloat << std::setprecision(6) << rel << std::endl;
	CHECK( rel < Scalar( 0.03 ),
		"3: analytic and mesh footprints agree within 3% -- catches a units or frame mismatch "
		"between the shipped mesh path and the new analytic one (" << std::setprecision(6) << rel << ")" );
}

//////////////////////////////////////////////////////////////////////
//  Test 4 -- uniform instance scale is exact
//////////////////////////////////////////////////////////////////////

//! The analytic-path twin of ReliefModifierTest 4c.  A pinhole's
//! DIRECTION differentials depend only on fov and resolution, never on
//! where the camera sits, so moving the camera from d to 10d while
//! scaling the sphere by 10 reproduces the identical object-space ray
//! -- the raw object-space footprint is bit-identical and the only
//! thing that can differ is the object-to-world promotion.
static void Test4_UniformScaleExact()
{
	std::cout << "Test 4: a uniform instance scale promotes the footprint by exactly that scale" << std::endl;

	const Scalar d = 5.0, s = 10.0;
	Scalar w1 = 0, w10 = 0;

	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const RayIntersection ri = Cast( *o, CentreRay( *cam ) );
		CHECK( ri.geometric.bHit, "4: (control) the scale-1 cast hits" );
		w1 = ri.geometric.txFootprint.worldWidth;
		cam->release();
		o->release();
	}
	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->SetScale( s );
		o->FinalizeTransformations();
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, d * s ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const RayIntersection ri = Cast( *o, CentreRay( *cam ) );
		CHECK( ri.geometric.bHit, "4: (control) the scale-10 cast hits" );
		w10 = ri.geometric.txFootprint.worldWidth;
		cam->release();
		o->release();
	}

	CHECK( w1 > Scalar( 1e-9 ), "4: (oracle) the scale-1 footprint is non-degenerate (" << w1 << ")" );
	const Scalar ratio = ( w1 > 0 ) ? ( w10 / w1 ) : Scalar( 0 );
	CHECK( std::fabs( ratio - s ) < Scalar( 1e-9 ),
		"4: worldWidth(scale 10) / worldWidth(scale 1) == 10 exactly ("
		<< std::setprecision(15) << ratio << ")" );
}

//////////////////////////////////////////////////////////////////////
//  Test 5 -- non-uniform instance scale is EXACT (the headline)
//////////////////////////////////////////////////////////////////////

//! `stretch (4, 0.05, 4)` on a circular disk lying in y = 0, viewed
//! face-on down the flattened axis.  The footprint lies entirely in the 4x plane, so the true
//! object-to-world factor for it is 4 -- while
//! |det M|^(1/3) = (4*0.05*4)^(1/3) = 0.9283, i.e. 4.31x too small.
//!
//! The oracle is independent of the promotion code: y = 0 is INVARIANT
//! under diag(4, 0.05, 4), so the stretched instance and an
//! unstretched one describe the SAME world plane, and the same world
//! camera ray must report the SAME world footprint.  Cross-checked
//! against the pinhole closed form, which neither cast can fake.
static void Test5_NonUniformScaleExact()
{
	std::cout << "Test 5: a NON-uniform instance scale promotes the footprint exactly (not |det|^(1/3))" << std::endl;

	const Scalar h = 10.0;
	PinholeCamera* cam = MakeCamera( Point3( 0, h, 0 ), Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	const Ray ray = CentreRay( *cam );
	Scalar wPlain = 0, wStretched = 0;
	bool   vPlain = false, vStretched = false;

	{
		CircularDiskGeometry* g = new CircularDiskGeometry( 50.0, 'y' );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "5: (control) the unstretched disk is hit" );
		vPlain = ri.geometric.txFootprint.widthValid;
		wPlain = ri.geometric.txFootprint.worldWidth;
		o->release();
	}
	{
		CircularDiskGeometry* g = new CircularDiskGeometry( 50.0, 'y' );
		Object* o = new Object( g );
		g->release();
		o->SetStretch( Vector3( 4.0, 0.05, 4.0 ) );
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "5: (control) the stretched disk is hit" );
		vStretched = ri.geometric.txFootprint.widthValid;
		wStretched = ri.geometric.txFootprint.worldWidth;
		o->release();
	}
	cam->release();

	CHECK( vPlain && vStretched, "5: both disk casts report widthValid (a UV-free geometry still gets a width)" );
	CHECK( wPlain > Scalar( 1e-9 ), "5: (oracle) the unstretched footprint is non-degenerate (" << wPlain << ")" );

	// Independent oracle: face-on at height h, one pixel subtends
	// h*tan(theta) on the plane; theta is small enough that theta and
	// tan(theta) agree far inside 3%.
	const Scalar closed = h * PixelAngle();
	CHECK( std::fabs( wPlain - closed ) / closed < Scalar( 0.03 ),
		"5: (oracle) the unstretched footprint matches h*theta (" << std::setprecision(6)
		<< wPlain << " vs " << closed << ")" );

	const Scalar rel = std::fabs( wStretched - wPlain ) / wPlain;
	std::cout << "    plain " << std::scientific << std::setprecision(9) << wPlain
	          << "   stretched(4, 0.05, 4) " << wStretched
	          << "   rel " << std::defaultfloat << std::setprecision(6) << rel << std::endl;
	CHECK( rel < Scalar( 1e-9 ),
		"5: stretch (4, 0.05, 4) reports the SAME world footprint as no stretch -- the promotion "
		"is the exact forward map, applying the in-plane 4x (" << std::setprecision(15)
		<< wStretched << " vs " << wPlain << ", rel " << rel << ")" );

	// Name the counterfactual so a regression is diagnosed, not merely
	// flagged: `worldWidth *= |det M|^(1/3)` applies 0.9283 where the
	// exact map applies 4, i.e. it reports wPlain / 4.3089.
	const Scalar detCubeRoot = std::pow( Scalar( 4.0 * 0.05 * 4.0 ), Scalar( 1.0 ) / Scalar( 3.0 ) );
	const Scalar geometricMeanWouldBe = wPlain / Scalar( 4 ) * detCubeRoot;
	CHECK( std::fabs( wStretched - geometricMeanWouldBe ) > wPlain * Scalar( 0.5 ),
		"5: and it is NOT the retired |det|^(1/3) value " << std::setprecision(9)
		<< geometricMeanWouldBe << " (" << std::setprecision(4) << Scalar( 4 ) / detCubeRoot
		<< "x too small)" );
}

//////////////////////////////////////////////////////////////////////
//  Test 6 -- UV-free geometries: a width, and no Jacobian
//////////////////////////////////////////////////////////////////////

static void Test6_UVFreeGeometries()
{
	std::cout << "Test 6: UV-free geometries report a width and NO UV Jacobian" << std::endl;

	PinholeCamera* cam = MakeCamera( Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	const Ray ray = CentreRay( *cam );

	{
		BoxGeometry* g = new BoxGeometry( 2.0, 2.0, 2.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "6: (control) the box is hit" );
		CHECK( ri.geometric.txFootprint.widthValid, "6: box_geometry reports widthValid" );
		CHECK( ri.geometric.txFootprint.worldWidth > Scalar( 0 ),
			"6: box_geometry reports a positive worldWidth (" << ri.geometric.txFootprint.worldWidth << ")" );
		// Load-bearing: TexturePainter::SampleTextured and WeaveBRDF key
		// on `valid`, and a zeroed Jacobian would read as LOD 0 (finest
		// mip) rather than the honest base-level fallback.
		CHECK( !ri.geometric.txFootprint.valid,
			"6: box_geometry leaves `valid` FALSE -- TexturePainter's mip path stays on the base level" );
		o->release();
	}
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), Scalar( 1.0 ), 0, 0, 0 ) );
		SDFGeometry* g = new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "6: (control) the sdf sphere is hit" );
		CHECK( ri.geometric.txFootprint.widthValid, "6: sdf_geometry reports widthValid" );
		CHECK( ri.geometric.txFootprint.worldWidth > Scalar( 0 ),
			"6: sdf_geometry reports a positive worldWidth (" << ri.geometric.txFootprint.worldWidth << ")" );
		CHECK( !ri.geometric.txFootprint.valid,
			"6: sdf_geometry leaves `valid` FALSE (no natural (u,v) -- the reason derivatives.valid "
			"is deliberately false there while curvatureValid is true)" );
		o->release();
	}

	// ClippedPlaneGeometry, approached on a deliberately TILTED ray --
	// see BuildXZPlane's caution: the bilinear intersector misses a ray
	// exactly along +/-Y, which is a pre-existing intersector bug this
	// suite must not be entangled with.
	{
		ClippedPlaneGeometry* g = BuildXZPlane( 5.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		PinholeCamera* tilted = MakeCamera( Point3( 1.0, 8, 1.0 ), Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const RayIntersection ri = Cast( *o, CentreRay( *tilted ) );
		CHECK( ri.geometric.bHit, "6: (control) the clipped plane is hit on a tilted ray" );
		CHECK( ri.geometric.txFootprint.widthValid, "6: clipped_plane reports widthValid" );
		CHECK( ri.geometric.txFootprint.worldWidth > Scalar( 0 ),
			"6: clipped_plane reports a positive worldWidth (" << ri.geometric.txFootprint.worldWidth << ")" );
		CHECK( !ri.geometric.txFootprint.valid,
			"6: clipped_plane leaves `valid` FALSE (it populates vShadingTangent only, never ri.derivatives)" );
		tilted->release();
		o->release();
	}

	cam->release();
}

//////////////////////////////////////////////////////////////////////
//  Test 7 -- the valid => widthValid invariant
//////////////////////////////////////////////////////////////////////

static void Test7_ValidImpliesWidthValid()
{
	std::cout << "Test 7: valid => widthValid, on the mesh path and on a UV-carrying primitive" << std::endl;

	PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	const Ray ray = CentreRay( *cam );

	{
		TriangleMeshGeometryIndexed* m = BuildMeshSphere( 1.0, 128, 64 );
		Object* o = new Object( m );
		m->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		const TextureFootprint& f = ri.geometric.txFootprint;
		CHECK( ri.geometric.bHit, "7: (control) the mesh sphere is hit" );
		CHECK( f.valid, "7: (oracle) the mesh sphere's UV chart yields a usable Jacobian -- this test "
			"would be vacuous otherwise" );
		CHECK( !f.valid || f.widthValid, "7: mesh path upholds valid => widthValid" );
		o->release();
	}
	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		const TextureFootprint& f = ri.geometric.txFootprint;
		CHECK( ri.geometric.bHit, "7: (control) the analytic sphere is hit" );
		CHECK( f.valid, "7: an ANALYTIC sphere now sets `valid` too -- textured spheres start mip-mapping" );
		CHECK( !f.valid || f.widthValid, "7: analytic path upholds valid => widthValid" );
		o->release();
	}

	cam->release();
}

//////////////////////////////////////////////////////////////////////
//  Test 8 -- CSG composes, per level
//////////////////////////////////////////////////////////////////////

//! A CSG_UNION whose winning operand carries a uniform scale must
//! report exactly what the same-scaled standalone object reports; and
//! a stretch applied at the CSG LEVEL must compose on top of the
//! operand's own promotion, since each level applies its own forward
//! map once to whatever the level below produced.
static void Test8_CsgComposes()
{
	std::cout << "Test 8: a csg_object promotes the footprint once per level" << std::endl;

	const Scalar d = 20.0, s = 2.0;
	PinholeCamera* cam = MakeCamera( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	const Ray ray = CentreRay( *cam );

	// Reference: a standalone scale-2 sphere.
	Scalar wRef = 0;
	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->SetScale( s );
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "8: (control) the reference sphere is hit" );
		wRef = ri.geometric.txFootprint.worldWidth;
		o->release();
	}
	CHECK( wRef > Scalar( 1e-12 ), "8: (oracle) the reference footprint is non-degenerate (" << wRef << ")" );

	// Builds a UNION of the scale-2 sphere (the near, winning operand)
	// and a far-away decoy, optionally with a scale on the CSG itself.
	auto castCsg = []( const Scalar operandScale, const Scalar csgScale, const Ray& r, bool& outHit ) -> Scalar
	{
		SphereGeometry* gA = new SphereGeometry( 1.0 );
		Object* a = new Object( gA );
		gA->release();
		a->SetScale( operandScale );
		a->FinalizeTransformations();

		SphereGeometry* gB = new SphereGeometry( 1.0 );
		Object* b = new Object( gB );
		gB->release();
		b->SetPosition( Point3( 1000, 1000, 1000 ) );
		b->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_UNION );
		csg->AssignObjects( a, b );
		if( csgScale != Scalar( 1 ) ) { csg->SetScale( csgScale ); }
		csg->FinalizeTransformations();

		RayIntersection ri( r, nullRasterizerState );
		csg->IntersectRay( ri, RISE_INFINITY, true, true, false );
		outHit = ri.geometric.bHit;
		const Scalar w = outHit ? ri.geometric.txFootprint.worldWidth : Scalar( 0 );
		csg->release();
		return w;
	};

	bool hitFlat = false;
	const Scalar wCsg = castCsg( s, Scalar( 1 ), ray, hitFlat );
	CHECK( hitFlat, "8: (control) the CSG union is hit" );
	CHECK( wRef > 0 && std::fabs( wCsg - wRef ) / wRef < Scalar( 1e-9 ),
		"8: the CSG's winning operand reports the same worldWidth as the same-scaled standalone object ("
		<< std::setprecision(15) << wCsg << " vs " << wRef << ")" );

	// Two levels: operand scale 2 inside a CSG scaled by 3.  The world
	// sphere has radius 6, so move the camera out by 3 to keep the same
	// object-space ray at every level -- the promotion is then the pure
	// composition 3 * 2 = 6 of what the unscaled-operand, unscaled-CSG
	// cast would have produced.
	PinholeCamera* camFar = MakeCamera( Point3( 0, 0, d * Scalar( 3 ) ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	const Ray rayFar = CentreRay( *camFar );

	bool hitNested = false, hitBase = false;
	const Scalar wNested = castCsg( s, Scalar( 3 ), rayFar, hitNested );
	const Scalar wBase   = castCsg( s, Scalar( 1 ), ray,    hitBase );
	CHECK( hitNested && hitBase, "8: (control) both nested-level casts hit" );
	const Scalar nestedRatio = ( wBase > 0 ) ? ( wNested / wBase ) : Scalar( 0 );
	CHECK( std::fabs( nestedRatio - Scalar( 3 ) ) < Scalar( 1e-9 ),
		"8: a scale on the CSG LEVEL composes once on top of the operand's own promotion ("
		<< std::setprecision(15) << nestedRatio << ")" );

	camFar->release();
	cam->release();
}

//////////////////////////////////////////////////////////////////////
//  Test 9 -- no differentials, no footprint
//////////////////////////////////////////////////////////////////////

static void Test9_NoDifferentials()
{
	std::cout << "Test 9: a hand-built Ray carries no differentials, so there is no footprint" << std::endl;

	SphereGeometry* g = new SphereGeometry( 1.0 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// Ray's ctor routes through Set(), which clears hasDifferentials --
	// this is what makes every shadow ray, NEE ray, photon and
	// post-bounce ray footprint-free by construction.
	const Ray ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) );
	CHECK( !ray.hasDifferentials, "9: (oracle) a freshly-Set ray has no differentials" );

	const RayIntersection ri = Cast( *o, ray );
	CHECK( ri.geometric.bHit, "9: (control) the ray hits" );
	CHECK( !ri.geometric.txFootprint.widthValid, "9: widthValid is false with no incoming differentials" );
	CHECK( !ri.geometric.txFootprint.valid, "9: valid is false with no incoming differentials" );
	CHECK( ri.geometric.txFootprint.worldWidth == Scalar( 0 ),
		"9: worldWidth stays 0 with no incoming differentials" );

	o->release();
}

//////////////////////////////////////////////////////////////////////
//  Test 10 -- grazing guard
//////////////////////////////////////////////////////////////////////

//! At a silhouette the auxiliary rays approach parallel to the tangent
//! plane and the projected footprint grows without bound.  There is no
//! clamp by design (docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md 7:
//! measure before clamping, since any clamp in the shared helper moves
//! mesh pixels too).  What must NOT happen is a non-finite value
//! reaching `fw` or the relief step rule: the |den| < 1e-20 guard
//! either produces a finite width or declines to set widthValid.
static void Test10_GrazingGuard()
{
	std::cout << "Test 10: a near-tangent hit yields a finite width, or none at all" << std::endl;

	const Scalar R = 1.0, d = 5.0;
	SphereGeometry* g = new SphereGeometry( R );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	PinholeCamera* cam = MakeCamera( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	RandomNumberGenerator rng( 7u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	// Walk outward from the axis, one pixel at a time, until the sphere
	// stops being hit.  The last few hits are the silhouette.
	int hits = 0, grazing = 0;
	for( unsigned int px = kRes / 2; px < kRes; px++ ) {
		Ray r;
		cam->GenerateRay( rc, r, Point2( Scalar( px ), Scalar( kRes ) * Scalar( 0.5 ) ) );
		const RayIntersection ri = Cast( *o, r );
		if( !ri.geometric.bHit ) { break; }
		hits++;

		const TextureFootprint& f = ri.geometric.txFootprint;
		// RISE::IsFiniteDouble, not std::isfinite: the canonical
		// -Ofast-resistant helper (the macOS build pairs -ffast-math with
		// -fno-finite-math-only today, but the release DMG's `Opto` config
		// is the one to be resistant to).
		const bool finite = !f.widthValid ||
			( RISE::IsFiniteDouble( f.worldWidth ) && f.worldWidth >= Scalar( 0 ) );
		if( !finite ) { grazing++; }
	}

	CHECK( hits > 10, "10: (oracle) the sweep actually walked across the sphere (" << hits << " hits)" );
	CHECK( grazing == 0,
		"10: every hit up to the silhouette reports a finite, non-negative worldWidth ("
		<< grazing << " bad of " << hits << ")" );

	cam->release();
	o->release();
}

//////////////////////////////////////////////////////////////////////
//  Test 11 -- the chart oracle
//////////////////////////////////////////////////////////////////////

//! One finite-difference estimate of d(ptCoord)/d(pixel).
struct FdChart
{
	bool   ok;			//!< false = the surface was missed, or the chart wrapped
	Scalar dudx, dvdx, dudy, dvdy;
};

//! Cast a bare ray (no differentials needed -- we only want ptCoord).
static bool CastForCoord( const Object& obj, const Point3& org, const Vector3& dir, Point2& outCoord )
{
	const Ray r( org, Vector3Ops::Normalize( dir ) );
	RayIntersection ri( r, nullRasterizerState );
	obj.IntersectRay( ri, RISE_INFINITY, true, true, false );
	if( !ri.geometric.bHit ) { return false; }
	outCoord = ri.geometric.ptCoord;
	return true;
}

//! THE ORACLE.  A CENTRAL difference of `ri.ptCoord` across the pixel's
//! own ray differentials: step half a differential each way, cast both,
//! and difference.  Central (rather than forward) so the estimate is
//! second-order accurate -- on a curved primitive a forward difference
//! carries an O(h * curvature) bias of the same order as the tolerance
//! we want to assert at.
//!
//! Returns ok = false when either probe misses the surface, or when a
//! component of the difference exceeds half the [0, 1] chart -- which
//! means the probe straddled a wrap seam and the difference is
//! meaningless, not that the Jacobian is wrong.
static FdChart FiniteDifferenceChart( const Object& obj, const Ray& ray )
{
	FdChart out;
	out.ok = false;
	out.dudx = out.dvdx = out.dudy = out.dvdy = Scalar( 0 );

	if( !ray.hasDifferentials ) { return out; }

	const RayDifferentials& d = ray.diffs;
	const Point3&  o = ray.origin;
	const Vector3& v = ray.Dir();
	const Scalar   h = Scalar( 0.5 );

	Point2 xp, xm, yp, ym;
	if( !CastForCoord( obj,
			Point3( o.x + h*d.rxOrigin.x, o.y + h*d.rxOrigin.y, o.z + h*d.rxOrigin.z ),
			Vector3( v.x + h*d.rxDir.x, v.y + h*d.rxDir.y, v.z + h*d.rxDir.z ), xp ) ) { return out; }
	if( !CastForCoord( obj,
			Point3( o.x - h*d.rxOrigin.x, o.y - h*d.rxOrigin.y, o.z - h*d.rxOrigin.z ),
			Vector3( v.x - h*d.rxDir.x, v.y - h*d.rxDir.y, v.z - h*d.rxDir.z ), xm ) ) { return out; }
	if( !CastForCoord( obj,
			Point3( o.x + h*d.ryOrigin.x, o.y + h*d.ryOrigin.y, o.z + h*d.ryOrigin.z ),
			Vector3( v.x + h*d.ryDir.x, v.y + h*d.ryDir.y, v.z + h*d.ryDir.z ), yp ) ) { return out; }
	if( !CastForCoord( obj,
			Point3( o.x - h*d.ryOrigin.x, o.y - h*d.ryOrigin.y, o.z - h*d.ryOrigin.z ),
			Vector3( v.x - h*d.ryDir.x, v.y - h*d.ryDir.y, v.z - h*d.ryDir.z ), ym ) ) { return out; }

	out.dudx = xp.x - xm.x;
	out.dvdx = xp.y - xm.y;
	out.dudy = yp.x - ym.x;
	out.dvdy = yp.y - ym.y;

	const Scalar half = Scalar( 0.5 );
	if( std::fabs( out.dudx ) > half || std::fabs( out.dvdx ) > half ||
	    std::fabs( out.dudy ) > half || std::fabs( out.dvdy ) > half ) {
		return out;		// wrap seam
	}

	out.ok = true;
	return out;
}

//! max over the two screen axes of the (u, v) row norm -- the quantity
//! TexturePainter::ComputeLODFromTexelFootprint takes the log2 of.
static Scalar MaxRowNorm( const Scalar dudx, const Scalar dvdx, const Scalar dudy, const Scalar dvdy )
{
	const Scalar lx = std::sqrt( dudx*dudx + dvdx*dvdx );
	const Scalar ly = std::sqrt( dudy*dudy + dvdy*dvdy );
	return ( lx > ly ) ? lx : ly;
}

//! Compare one geometry's published Jacobian against the oracle.
//!
//! TOLERANCE.  `tol` is relative to the LARGEST component of the
//! finite-difference Jacobian, not to each component individually: at a
//! symmetric hit point one component is legitimately ~0 and a per-
//! component relative test there measures nothing but round-off.  The
//! scale-relative form still catches every chart error this test exists
//! for, because a wrong chart is a multiplicative error (2*pi, pi, the
//! cylinder height) or a transposition -- both of which move the
//! largest component.
static void CheckChart( const char* what, const Object& obj, const Ray& ray, const Scalar tol )
{
	const RayIntersection ri = Cast( obj, ray );
	CHECK( ri.geometric.bHit, "11: (control) " << what << " is hit" );
	if( !ri.geometric.bHit ) { return; }

	const TextureFootprint& f = ri.geometric.txFootprint;
	CHECK( f.valid, "11: (oracle) " << what << " publishes a UV Jacobian -- the comparison "
		"below is vacuous otherwise" );
	if( !f.valid ) { return; }

	const FdChart fd = FiniteDifferenceChart( obj, ray );
	CHECK( fd.ok, "11: (oracle) the finite-difference probe landed on " << what
		<< " away from a wrap seam" );
	if( !fd.ok ) { return; }

	Scalar scale = std::fabs( fd.dudx );
	if( std::fabs( fd.dvdx ) > scale ) scale = std::fabs( fd.dvdx );
	if( std::fabs( fd.dudy ) > scale ) scale = std::fabs( fd.dudy );
	if( std::fabs( fd.dvdy ) > scale ) scale = std::fabs( fd.dvdy );
	CHECK( scale > Scalar( 1e-12 ), "11: (oracle) " << what << " has a non-degenerate FD Jacobian" );
	if( scale <= Scalar( 1e-12 ) ) { return; }

	Scalar worst = std::fabs( f.dudx - fd.dudx );
	if( std::fabs( f.dvdx - fd.dvdx ) > worst ) worst = std::fabs( f.dvdx - fd.dvdx );
	if( std::fabs( f.dudy - fd.dudy ) > worst ) worst = std::fabs( f.dudy - fd.dudy );
	if( std::fabs( f.dvdy - fd.dvdy ) > worst ) worst = std::fabs( f.dvdy - fd.dvdy );
	const Scalar rel = worst / scale;

	// Report the error the way the CONSUMER feels it.
	// TexturePainter::ComputeLODFromTexelFootprint takes
	// log2( max over the two screen axes of the texel-space row norm ),
	// so on a square texture the LOD error is exactly the log2 of the
	// ratio of published to true max row norm -- texture dimensions
	// cancel.  A chart off by a factor k therefore mips log2(k) levels
	// too blurry.
	const Scalar aRow = MaxRowNorm( f.dudx, f.dvdx, f.dudy, f.dvdy );
	const Scalar fRow = MaxRowNorm( fd.dudx, fd.dvdx, fd.dudy, fd.dvdy );
	const Scalar lodErr = ( aRow > 0 && fRow > 0 )
		? std::log( aRow / fRow ) / std::log( Scalar( 2 ) ) : Scalar( 0 );

	std::cout << "    " << what
	          << "   rel " << std::scientific << std::setprecision(3) << rel
	          << "   LOD error " << std::defaultfloat << std::setprecision(4) << lodErr
	          << " levels" << std::endl;

	CHECK( rel < tol, "11: " << what << "'s dudx..dvdy are the derivatives of ptCoord "
		"(scale-relative error " << std::scientific << std::setprecision(3) << rel
		<< ", tol " << tol << ")" );
}

static void Test11_ChartOracle()
{
	std::cout << "Test 11: dudx..dvdy live in the ptCoord chart (finite-difference oracle)" << std::endl;

	// The mesh CONTROL first: its dpdu already IS d/d(texcoord), so it
	// was correct before the chart map existed and must stay correct.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		TriangleMeshGeometryIndexed* m = BuildMeshSphere( 1.0, 128, 64 );
		Object* o = new Object( m );
		m->release();
		o->FinalizeTransformations();
		CheckChart( "mesh sphere (control)", *o, CentreRay( *cam ), Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// The MIRRORED-UV mesh: same geometry, same winding, u texcoord
	// written backwards.  This is the mesh path's `dtdv = -1` branch --
	// the right-handedness fix-up negates dpdv and the chart map has to
	// say so.  Without that row the branch was reachable in production
	// (any mirrored-UV asset) and covered by nothing here: the control
	// above never fires it, and no analytic primitive can.  Red-proof:
	// deleting `ri.derivatives.dtdv = -1.0;` from BOTH
	// TriangleMeshGeometry{,Indexed}Specializations.h leaves the control
	// and all six analytic rows green and fails this one alone.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		TriangleMeshGeometryIndexed* m = BuildMeshSphere( 1.0, 128, 64, /*mirrorU*/ true );
		Object* o = new Object( m );
		m->release();
		o->FinalizeTransformations();
		CheckChart( "mesh sphere (mirrored UV, dtdv = -1)", *o, CentreRay( *cam ), Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// Sphere.  The on-axis hit is at (0, 0, R): azimuth pi/2 (a quarter
	// turn from the -X seam) and polar pi/2 (the equator, far from both
	// poles), so neither singularity is in play.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		CheckChart( "analytic sphere", *o, CentreRay( *cam ), Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// Ellipsoid, deliberately unequal semi-axes so a sphere-shaped
	// chart map would not accidentally fit.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		EllipsoidGeometry* g = new EllipsoidGeometry( Vector3( 1.0, 1.6, 0.7 ) );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		CheckChart( "analytic ellipsoid", *o, CentreRay( *cam ), Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// Cylinder, y axis, height 3 -- the height is what makes the axial
	// half of its chart map a scale OTHER than 2*pi or pi, so a
	// "normalise everything by 2*pi" mistake would not fit it.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		CylinderGeometry* g = new CylinderGeometry( 'y', 1.0, 3.0, false );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		CheckChart( "analytic cylinder (open tube, y)", *o, CentreRay( *cam ), Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// The x- and z-axis cylinders take the OTHER branch of the
	// handedness fix-up, so their angular chart runs the opposite way.
	// A sign-blind map would pass the y case and fail these.
	{
		// Off the axis planes on purpose: a straight-down view of an
		// x-axis cylinder lands on theta = 0, which is that chart's
		// wrap seam and would make the oracle decline rather than
		// measure.
		PinholeCamera* cam = MakeCamera( Point3( 0, 3, 3 ), Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
		CylinderGeometry* g = new CylinderGeometry( 'x', 1.0, 3.0, false );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		CheckChart( "analytic cylinder (open tube, x)", *o, CentreRay( *cam ), Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 4, 0 ), Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		CylinderGeometry* g = new CylinderGeometry( 'z', 1.0, 3.0, false );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		CheckChart( "analytic cylinder (open tube, z)", *o, CentreRay( *cam ), Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// A CAPPED cylinder seen down its own axis: the hit is on the end
	// cap, whose chart is the (ra, rb) disk -- a completely different
	// map from the side wall's, including the possible axis swap the
	// right-handedness fix-up applies.  A y-axis cylinder swaps on its
	// +y cap (NOT the -y one -- this comment said "the -axis cap" until
	// fix round 2; see CylinderGeometry.cpp's own note at the swap site
	// for the per-axis table and why 'y' is the odd one out), so the
	// camera below, which looks DOWN the +y axis at the +y cap, lands on
	// the SWAPPED branch -- the one worth an oracle.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 5, 0 ), Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		CylinderGeometry* g = new CylinderGeometry( 'y', 1.0, 3.0, true );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		// Dead centre of the cap is the disk chart's origin, where both
		// FD components are fine but the hit is exactly on the axis;
		// aim a little off so the frame is generic.
		RandomNumberGenerator rng( 3u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		Ray r;
		cam->GenerateRay( rc, r, Point2( Scalar( kRes ) * Scalar( 0.5 ) + Scalar( 60 ),
		                                 Scalar( kRes ) * Scalar( 0.5 ) + Scalar( 35 ) ) );
		CheckChart( "analytic cylinder (+y end cap)", *o, r, Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// The IDENTITY branch of the same chart map -- the pre-swap
	// `dsdu=1, dsdv=0, dtdu=0, dtdv=1` assignment, which the +y row above
	// never reaches.  Per CylinderGeometry.cpp's per-axis table, a
	// z-axis cylinder swaps on its -z cap, so its +z cap takes the
	// UNswapped branch; the camera below looks DOWN the +z axis at the
	// +z cap.  Red-proof (2026-09-06): transposed the pre-`if` identity
	// assignment (`dsdu=0,dsdv=1,dtdu=1,dtdv=0`) -- this row failed and
	// the +y row above stayed green, confirming they exercise the two
	// different branches; reverted.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		CylinderGeometry* g = new CylinderGeometry( 'z', 1.0, 3.0, true );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		// Dead centre of the cap is the disk chart's origin, where both
		// FD components are fine but the hit is exactly on the axis;
		// aim a little off so the frame is generic.
		RandomNumberGenerator rng( 7u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		Ray r;
		cam->GenerateRay( rc, r, Point2( Scalar( kRes ) * Scalar( 0.5 ) + Scalar( 60 ),
		                                 Scalar( kRes ) * Scalar( 0.5 ) + Scalar( 35 ) ) );
		CheckChart( "analytic cylinder (+z end cap)", *o, r, Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}

	// Torus.  Aim ABOVE the outer equator: the equator itself is the
	// tube chart's v = 0 seam, where the finite difference wraps.
	{
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, 8 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		TorusGeometry* g = new TorusGeometry( 2.0, 0.6 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		RandomNumberGenerator rng( 5u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		Ray r;
		cam->GenerateRay( rc, r, Point2( Scalar( kRes ) * Scalar( 0.5 ) + Scalar( 30 ),
		                                 Scalar( kRes ) * Scalar( 0.5 ) - Scalar( 45 ) ) );
		CheckChart( "analytic torus", *o, r, Scalar( 1e-3 ) );
		o->release();
		cam->release();
	}
}

//////////////////////////////////////////////////////////////////////
//  Test 12 -- the paths that must NOT publish a Jacobian
//////////////////////////////////////////////////////////////////////

//! A mesh sphere whose per-vertex texture coordinates are all the same
//! point.  Every UV triangle is degenerate, so both mesh intersectors
//! take the barycentric-EDGE fallback: dpdu / dpdv are then triangle
//! edge vectors with no relation whatever to ptCoord, and the honest
//! answer is to publish no Jacobian.
static TriangleMeshGeometryIndexed* BuildMeshSphereNoUV( const Scalar radius, const int nu, const int nv )
{
	VerticesListType verts;
	NormalsListType  norms;
	TexCoordsListType coords;

	for( int j = 0; j <= nv; j++ ) {
		const Scalar theta = kPi * Scalar( j ) / Scalar( nv );
		const Scalar st = std::sin( theta ), ct = std::cos( theta );
		for( int i = 0; i <= nu; i++ ) {
			const Scalar phi = Scalar( 2 ) * kPi * Scalar( i ) / Scalar( nu );
			const Vector3 n( st * std::cos( phi ), ct, st * std::sin( phi ) );
			verts.push_back( Point3( n.x * radius, n.y * radius, n.z * radius ) );
			norms.push_back( n );
			coords.push_back( Point2( 0, 0 ) );		// degenerate on purpose
		}
	}

	IndexTriangleListType tris;
	for( int j = 0; j < nv; j++ ) {
		for( int i = 0; i < nu; i++ ) {
			const unsigned int a = (unsigned int)( j * ( nu + 1 ) + i );
			const unsigned int b = a + 1;
			const unsigned int c = (unsigned int)( ( j + 1 ) * ( nu + 1 ) + i );
			const unsigned int d = c + 1;

			IndexedTriangle t0;
			t0.iVertices[0] = a; t0.iVertices[1] = c; t0.iVertices[2] = b;
			t0.iNormals[0]  = a; t0.iNormals[1]  = c; t0.iNormals[2]  = b;
			t0.iCoords[0]   = a; t0.iCoords[1]   = c; t0.iCoords[2]   = b;
			tris.push_back( t0 );

			IndexedTriangle t1;
			t1.iVertices[0] = b; t1.iVertices[1] = c; t1.iVertices[2] = d;
			t1.iNormals[0]  = b; t1.iNormals[1]  = c; t1.iNormals[2]  = d;
			t1.iCoords[0]   = b; t1.iCoords[1]   = c; t1.iCoords[2]   = d;
			tris.push_back( t1 );
		}
	}

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, false );
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( verts );
	mesh->AddNormals( norms );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();
	return mesh;
}

static void Test12_NoChartNoJacobian()
{
	std::cout << "Test 12: no stated chart map => no Jacobian (and never a NaN)" << std::endl;

	PinholeCamera* cam = MakeCamera( Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
	const Ray ray = CentreRay( *cam );

	// (a) The barycentric-edge mesh fallback: derivatives valid, chart
	// map absent, so `valid` must be FALSE while the width survives.
	{
		TriangleMeshGeometryIndexed* m = BuildMeshSphereNoUV( 1.0, 64, 32 );
		Object* o = new Object( m );
		m->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		const TextureFootprint& f = ri.geometric.txFootprint;
		CHECK( ri.geometric.bHit, "12: (control) the UV-less mesh sphere is hit" );
		CHECK( ri.geometric.derivatives.valid,
			"12: (oracle) the UV-less mesh still publishes derivatives (edge frame) -- "
			"otherwise this case would be indistinguishable from test 6" );
		CHECK( !ri.geometric.derivatives.texChartValid,
			"12: the barycentric-edge fallback states NO chart map" );
		CHECK( !f.valid,
			"12: a mesh with a degenerate UV triangle publishes no UV Jacobian" );
		CHECK( f.widthValid && f.worldWidth > Scalar( 0 ),
			"12: ... but still reports a width (" << f.worldWidth << ")" );
		o->release();
	}

	// (b) UV-free geometry states no chart map either (box: no
	// ri.derivatives at all).  Test 6 already pins `valid`; this pins
	// the mechanism underneath it.
	{
		BoxGeometry* g = new BoxGeometry( 2, 2, 2 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		const RayIntersection ri = Cast( *o, ray );
		CHECK( ri.geometric.bHit, "12: (control) the box is hit" );
		CHECK( !ri.geometric.derivatives.texChartValid, "12: box_geometry states no chart map" );
		CHECK( !ri.geometric.txFootprint.valid, "12: box_geometry publishes no UV Jacobian" );
		o->release();
	}

	// (c) The sphere's two chart singularities.  At a POLE the
	// derivative basis collapses (|dpdu| = r*sin(theta) -> 0) and at the
	// -X SEAM the chart is discontinuous.  Neither may produce a
	// non-finite number: the solve either declines (det guard) or
	// returns the finite local-branch answer.
	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		// Pole: look straight down the +Y axis at the north pole.
		PinholeCamera* poleCam = MakeCamera( Point3( 0, 5, 0 ), Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const RayIntersection rp = Cast( *o, CentreRay( *poleCam ) );
		CHECK( rp.geometric.bHit, "12: (control) the north pole is hit" );
		const TextureFootprint& fp = rp.geometric.txFootprint;
		CHECK( !fp.valid ||
			( RISE::IsFiniteDouble( fp.dudx ) && RISE::IsFiniteDouble( fp.dvdx ) &&
			  RISE::IsFiniteDouble( fp.dudy ) && RISE::IsFiniteDouble( fp.dvdy ) ),
			"12: the sphere's pole either declines or reports finite derivatives" );
		CHECK( !fp.widthValid || RISE::IsFiniteDouble( fp.worldWidth ),
			"12: the sphere's pole reports a finite width" );
		poleCam->release();

		// Seam: the -X meridian is where SphereTextureCoord wraps
		// 1 -> 0.  The Jacobian of the LOCAL branch is still finite
		// there; what must not happen is a NaN reaching the sampler.
		PinholeCamera* seamCam = MakeCamera( Point3( -5, 0, 0 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const RayIntersection rs = Cast( *o, CentreRay( *seamCam ) );
		CHECK( rs.geometric.bHit, "12: (control) the -X seam is hit" );
		const TextureFootprint& fs = rs.geometric.txFootprint;
		CHECK( !fs.valid ||
			( RISE::IsFiniteDouble( fs.dudx ) && RISE::IsFiniteDouble( fs.dvdx ) &&
			  RISE::IsFiniteDouble( fs.dudy ) && RISE::IsFiniteDouble( fs.dvdy ) ),
			"12: the sphere's -X seam reports finite derivatives" );
		CHECK( !fs.widthValid || RISE::IsFiniteDouble( fs.worldWidth ),
			"12: the sphere's -X seam reports a finite width" );
		seamCam->release();

		o->release();
	}

	cam->release();
}

//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
//  Test 13 -- the OBJECT-space width (2026-09-06, doc 11)
//
//  `txFootprint.objectWidth` is the same footprint measured in the
//  frame `ptObjIntersec` (the expression VM's `Po`) is written in.  It
//  is what lets `fbm(Po*k, ...)` fade; before it existed the VM could
//  not know the object->world scale and fell back to no fade at all.
//
//  Three claims, each with an oracle that does not read the promotion
//  code being tested:
//
//   (a) UNIFORM SCALE.  A unit sphere under `scale s`, viewed from `d*s`,
//       presents the IDENTICAL object-space ray as the same sphere at
//       scale 1 viewed from `d` -- so its objectWidth must equal the
//       scale-1 cast's width exactly, while its worldWidth is s times
//       that.  The ratio worldWidth/objectWidth recovers s, which is the
//       number the VM would otherwise have had to guess.
//
//   (b) NON-UNIFORM SCALE.  The `stretch (4, 0.05, 4)` disk of test 5:
//       objectWidth must be the world width divided by the IN-PLANE 4,
//       not by |det M|^(1/3) = 0.9283.  This is the same counterfactual
//       test 5 pins for worldWidth, read from the other side -- and it
//       is why objectWidth is CAPTURED before the promotion rather than
//       derived from m_worldLinearScale afterwards.
//
//   (c) CSG.  A CSG hit's `ptObjIntersec` is the winning CHILD's own
//       object-space point (AdoptCsgSurfacePayload copies it
//       untransformed), so objectWidth must stay in that same child
//       frame -- i.e. a scale on the CSG LEVEL must move worldWidth (test
//       8) and leave objectWidth alone.  Pairing the two fields is the
//       whole contract; a CSG level that "helpfully" promoted
//       objectWidth too would break `fbm(Po*k)` on every csg_object.
//////////////////////////////////////////////////////////////////////

static void Test13_ObjectSpaceWidth()
{
	std::cout << "Test 13: txFootprint.objectWidth is the footprint in ptObjIntersec's frame" << std::endl;

	// ---------------- (a) uniform scale ----------------
	const Scalar d = 5.0, s = 10.0;
	Scalar w1 = 0, o1 = 0, w10 = 0, o10 = 0;
	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const RayIntersection ri = Cast( *o, CentreRay( *cam ) );
		CHECK( ri.geometric.bHit, "13a: (control) the scale-1 cast hits" );
		w1 = ri.geometric.txFootprint.worldWidth;
		o1 = ri.geometric.txFootprint.objectWidth;
		cam->release();
		o->release();
	}
	{
		SphereGeometry* g = new SphereGeometry( 1.0 );
		Object* o = new Object( g );
		g->release();
		o->SetScale( s );
		o->FinalizeTransformations();
		PinholeCamera* cam = MakeCamera( Point3( 0, 0, d * s ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const RayIntersection ri = Cast( *o, CentreRay( *cam ) );
		CHECK( ri.geometric.bHit, "13a: (control) the scale-10 cast hits" );
		w10 = ri.geometric.txFootprint.worldWidth;
		o10 = ri.geometric.txFootprint.objectWidth;
		cam->release();
		o->release();
	}

	CHECK( w1 > Scalar( 1e-9 ), "13a: (oracle) the scale-1 footprint is non-degenerate (" << w1 << ")" );
	CHECK( w1 == o1,
		"13a: with NO transform the object and world widths are the same number, bit for bit ("
		<< std::setprecision(17) << w1 << " vs " << o1 << ")" );
	CHECK( o1 > 0 && std::fabs( o10 - o1 ) / o1 < Scalar( 1e-9 ),
		"13a: the scale-10 instance reports the SAME object-space width as the scale-1 one -- the "
		"object-space ray is identical, so the object-space footprint must be too ("
		<< std::setprecision(15) << o10 << " vs " << o1 << ")" );
	const Scalar recovered = ( o10 > 0 ) ? ( w10 / o10 ) : Scalar( 0 );
	CHECK( std::fabs( recovered - s ) < Scalar( 1e-9 ),
		"13a: worldWidth / objectWidth recovers the instance scale " << s << " ("
		<< std::setprecision(15) << recovered << ") -- the per-instance number no compiler can see" );

	// ---------------- (b) non-uniform scale ----------------
	{
		const Scalar h = 10.0;
		PinholeCamera* cam = MakeCamera( Point3( 0, h, 0 ), Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const Ray ray = CentreRay( *cam );

		CircularDiskGeometry* g = new CircularDiskGeometry( 50.0, 'y' );
		Object* obj = new Object( g );
		g->release();
		obj->SetStretch( Vector3( 4.0, 0.05, 4.0 ) );
		obj->FinalizeTransformations();
		const RayIntersection ri = Cast( *obj, ray );
		CHECK( ri.geometric.bHit, "13b: (control) the stretched disk is hit" );
		const Scalar wS = ri.geometric.txFootprint.worldWidth;
		const Scalar oS = ri.geometric.txFootprint.objectWidth;
		obj->release();
		cam->release();

		CHECK( oS > Scalar( 1e-12 ), "13b: (oracle) the stretched disk's object width is non-degenerate ("
			<< oS << ")" );
		const Scalar inPlane = ( oS > 0 ) ? ( wS / oS ) : Scalar( 0 );
		CHECK( std::fabs( inPlane - Scalar( 4 ) ) < Scalar( 1e-9 ),
			"13b: worldWidth / objectWidth is the IN-PLANE 4x the footprint actually undergoes ("
			<< std::setprecision(15) << inPlane << ")" );
		// The counterfactual: had objectWidth been DERIVED as
		// worldWidth / |det M|^(1/3) rather than captured before the
		// promotion, this ratio would read 0.9283 instead of 4.
		const Scalar detCubeRoot = std::pow( Scalar( 4.0 * 0.05 * 4.0 ), Scalar( 1.0 ) / Scalar( 3.0 ) );
		CHECK( std::fabs( inPlane - detCubeRoot ) > Scalar( 1 ),
			"13b: and it is NOT the |det|^(1/3) value " << std::setprecision(6) << detCubeRoot
			<< " a derive-by-scale implementation would give (" << Scalar( 4 ) / detCubeRoot << "x off)" );
	}

	// ---------------- (c) CSG keeps the child frame ----------------
	{
		const Scalar dd = 20.0, opScale = 2.0;
		auto castCsg = []( const Scalar operandScale, const Scalar csgScale, const Ray& r,
			bool& outHit, Scalar& outObj ) -> Scalar
		{
			SphereGeometry* gA = new SphereGeometry( 1.0 );
			Object* a = new Object( gA );
			gA->release();
			a->SetScale( operandScale );
			a->FinalizeTransformations();

			SphereGeometry* gB = new SphereGeometry( 1.0 );
			Object* b = new Object( gB );
			gB->release();
			b->SetPosition( Point3( 1000, 1000, 1000 ) );
			b->FinalizeTransformations();

			CSGObject* csg = new CSGObject( CSG_UNION );
			csg->AssignObjects( a, b );
			if( csgScale != Scalar( 1 ) ) { csg->SetScale( csgScale ); }
			csg->FinalizeTransformations();

			RayIntersection ri( r, nullRasterizerState );
			csg->IntersectRay( ri, RISE_INFINITY, true, true, false );
			outHit = ri.geometric.bHit;
			outObj = outHit ? ri.geometric.txFootprint.objectWidth : Scalar( 0 );
			const Scalar w = outHit ? ri.geometric.txFootprint.worldWidth : Scalar( 0 );
			csg->release();
			return w;
		};

		PinholeCamera* cam    = MakeCamera( Point3( 0, 0, dd ),     Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		PinholeCamera* camFar = MakeCamera( Point3( 0, 0, dd * 3 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		const Ray ray    = CentreRay( *cam );
		const Ray rayFar = CentreRay( *camFar );

		bool hitBase = false, hitNested = false;
		Scalar oBase = 0, oNested = 0;
		const Scalar wBase   = castCsg( opScale, Scalar( 1 ), ray,    hitBase,   oBase );
		const Scalar wNested = castCsg( opScale, Scalar( 3 ), rayFar, hitNested, oNested );
		cam->release();
		camFar->release();

		CHECK( hitBase && hitNested, "13c: (control) both CSG casts hit" );
		CHECK( oBase > Scalar( 1e-12 ), "13c: (oracle) the CSG hit carries a real object width ("
			<< oBase << ") -- not the neutral 0 an un-stamped field would give" );
		// The world side moves by the CSG level's own scale (test 8's claim,
		// re-asserted here so this test fails loudly if the pairing is broken
		// from the OTHER side).
		const Scalar worldRatio = ( wBase > 0 ) ? ( wNested / wBase ) : Scalar( 0 );
		CHECK( std::fabs( worldRatio - Scalar( 3 ) ) < Scalar( 1e-9 ),
			"13c: (control) worldWidth still composes the CSG level's scale ("
			<< std::setprecision(15) << worldRatio << ")" );
		// The object side does NOT: it belongs to the winning child, and so
		// does the ptObjIntersec it has to agree with.
		const Scalar objRatio = ( oBase > 0 ) ? ( oNested / oBase ) : Scalar( 0 );
		CHECK( std::fabs( objRatio - Scalar( 1 ) ) < Scalar( 1e-9 ),
			"13c: objectWidth is UNCHANGED by a scale on the CSG level -- it stays in the winning "
			"child's frame, the same frame ptObjIntersec (the VM's Po) is in ("
			<< std::setprecision(15) << objRatio << ")" );
		// And it agrees with the operand's own scale, not the composite's.
		const Scalar childScaleRecovered = ( oBase > 0 ) ? ( wBase / oBase ) : Scalar( 0 );
		CHECK( std::fabs( childScaleRecovered - opScale ) < Scalar( 1e-9 ),
			"13c: worldWidth / objectWidth on the un-scaled composite recovers the OPERAND's scale "
			<< opScale << " (" << std::setprecision(15) << childScaleRecovered << ")" );
	}
}

//////////////////////////////////////////////////////////////////////
//  Test 14 -- the thin-lens and orthographic cameras carry
//  differentials too (they used to emit none, so EVERY hit from a
//  `thinlens_camera` scene reported widthValid false / fw == 0, which
//  silently disabled mip LOD, the relief modifier's footprint-driven
//  step and the fbm octave fade on the 20 `.RISEscene` files that name
//  a `thinlens_camera` (and the 2 that name an `orthographic_camera`).
//////////////////////////////////////////////////////////////////////

//! The focal length, in MILLIMETRES, whose derived VERTICAL fov is
//! exactly `kFovDeg` on a square frame at `pixelAR = 1`.
//! `ThinLensCamera::Recompute` computes
//!   effective_sensor_v = sensor / (width*pixelAR/height)
//!   fov               = 2*atan(effective_sensor_v / (2*focal))
//! so on a square frame effective_sensor_v == sensor and inverting for
//! focal gives sensor / (2*tan(fov/2)).  Matching the fov is what lets
//! test 14a compare the two cameras' differentials at all.
static const Scalar kSensorMM = 36.0;
static Scalar MatchedFocalMM()
{
	return kSensorMM / ( Scalar( 2 ) * std::tan( kFovDeg * kPi / Scalar( 180 ) * Scalar( 0.5 ) ) );
}

//! A square thin-lens camera whose vertical fov matches `MakeCamera`'s
//! pinhole.  `fstop` sets the aperture (diameter = focal/fstop);
//! `shiftXmm` / `shiftYmm` are the Phase-1.1 lens shift in mm.
static ThinLensCamera* MakeThinLens(
	const Point3& eye, const Point3& at, const Vector3& up,
	const Scalar focusDistance, const Scalar fstop,
	const Scalar shiftXmm = Scalar( 0 ), const Scalar shiftYmm = Scalar( 0 ) )
{
	return new ThinLensCamera(
		eye, at, up,
		kSensorMM, MatchedFocalMM(), fstop, focusDistance,
		Scalar( 1 ),							// sceneUnitMeters (metres scene)
		kRes, kRes,
		Scalar( 1 ),							// pixelAR
		Scalar( 1 ),							// exposure
		Scalar( 0 ),							// scanningRate
		Scalar( 0 ),							// pixelRate
		Vector3( 0, 0, 0 ),						// orientation
		Vector2( 0, 0 ),						// target_orientation
		0,										// apertureBlades (disk)
		Scalar( 0 ),							// apertureRotation
		Scalar( 1 ),							// anamorphicSqueeze
		Scalar( 0 ), Scalar( 0 ),				// tiltX, tiltY
		shiftXmm, shiftYmm );
}

//! `GeometricUtilities::PointOnDisk`'s Shirley concentric map sends
//! (0.5, 0.5) to EXACTLY (0, 0) -- both of its intermediate
//! coordinates are exactly 0 there and the function short-circuits --
//! so `GenerateRayWithLensSample` with this sample is the aperture-0
//! limit EXACTLY, with no 1/fstop epsilon anywhere.  That is what
//! makes the 1e-12 comparison against the pinhole meaningful rather
//! than a restatement of "fstop was large".
static const Point2 kLensCentre( 0.5, 0.5 );

//! A ray through a chosen pixel and a chosen point on the aperture.
static Ray ThinLensRay( const ThinLensCamera& cam, const Point2& pixel, const Point2& lensSample )
{
	RandomNumberGenerator rng( 7u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	Ray r;
	cam.GenerateRayWithLensSample( rc, r, pixel, lensSample );
	return r;
}

static Scalar MaxAbsDiff( const Vector3& a, const Vector3& b )
{
	return std::max( std::max( std::fabs( a.x - b.x ), std::fabs( a.y - b.y ) ),
	                 std::fabs( a.z - b.z ) );
}

static void Test14_ThinLensAndOrthoDifferentials()
{
	std::cout << "Test 14: thin-lens and orthographic cameras emit ray differentials" << std::endl;

	const Point2 centrePixel( Scalar( kRes ) * Scalar( 0.5 ), Scalar( kRes ) * Scalar( 0.5 ) );
	const Point2 offPixel( Scalar( kRes ) * Scalar( 0.31 ), Scalar( kRes ) * Scalar( 0.72 ) );
	const Scalar d = 5.0;			// camera distance AND focus distance

	// ---- 14a: at aperture 0 the thin lens reproduces the pinhole ----
	//
	// Both cameras sit at the same place with the same vertical fov and
	// the same square film.  A lens of zero radius IS a pinhole, so the
	// central direction and BOTH differential offsets must agree to
	// round-off.  This is the test that catches a film-distance factor
	// left in, an sx/sy sign slip, or a half-pixel-vs-full-pixel step.
	{
		PinholeCamera*  pin  = MakeCamera( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) );
		ThinLensCamera* thin = MakeThinLens( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), d, Scalar( 2.8 ) );

		for( int k = 0; k < 2; k++ ) {
			const Point2 px = ( k == 0 ) ? centrePixel : offPixel;

			RandomNumberGenerator rng( 3u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			Ray rp;
			pin->GenerateRay( rc, rp, px );

			const Ray rt = ThinLensRay( *thin, px, kLensCentre );

			const Scalar dOrigin = std::max( std::max(
				std::fabs( rp.origin.x - rt.origin.x ), std::fabs( rp.origin.y - rt.origin.y ) ),
				std::fabs( rp.origin.z - rt.origin.z ) );
			const Scalar dDir = MaxAbsDiff( rp.Dir(), rt.Dir() );
			const Scalar dRx  = MaxAbsDiff( rp.diffs.rxDir, rt.diffs.rxDir );
			const Scalar dRy  = MaxAbsDiff( rp.diffs.ryDir, rt.diffs.ryDir );

			std::cout << "    14a pixel " << ( k == 0 ? "centre" : "off-axis" )
			          << "  |d origin| " << std::scientific << std::setprecision(3) << dOrigin
			          << "  |d dir| " << dDir
			          << "  |d rxDir| " << dRx << "  |d ryDir| " << dRy
			          << std::defaultfloat << std::endl;

			CHECK( dOrigin < Scalar( 1e-12 ),
				"14a: the zero-aperture thin lens shares the pinhole's origin (" << dOrigin << ")" );
			CHECK( dDir < Scalar( 1e-12 ),
				"14a: the zero-aperture thin lens shares the pinhole's direction (" << dDir << ")" );
			CHECK( dRx < Scalar( 1e-12 ),
				"14a: rxDir matches PinholeCamera's at matched fov / film (" << dRx << ")" );
			CHECK( dRy < Scalar( 1e-12 ),
				"14a: ryDir matches PinholeCamera's at matched fov / film (" << dRy << ")" );
			CHECK( rt.hasDifferentials, "14a: the thin-lens ray carries differentials at all" );
		}

		pin->release();
		thin->release();
	}

	// ---- 14b / 14c: the closed form, at and beyond the focal plane --
	//
	// Face-on plane, chief ray (lens centre).  One pixel of film
	// subtends exactly `2*tan(fov/2)/height` at unit image distance, so
	// the auxiliary ray's displacement on a plane a distance D away,
	// perpendicular to the axis, is exactly D * that -- EXACTLY, not to
	// within the tan/angle approximation, because the pixel step is
	// already a tangent.  At D == focusDistance this is the "the
	// footprint at the plane of focus is the pinhole footprint"
	// statement; at D == 2*focusDistance it doubles, which is the
	// statement that the differential is the pinhole-at-this-lens-point
	// and NOT something that collapses at the plane of focus.
	{
		ThinLensCamera* thin = MakeThinLens( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), d, Scalar( 2.8 ) );
		const Ray ray = ThinLensRay( *thin, centrePixel, kLensCentre );
		thin->release();

		Scalar wAtFocus = 0, wBeyond = 0;
		bool   vAtFocus = false, vBeyond = false;
		for( int k = 0; k < 2; k++ ) {
			// Disk in the z = 0 plane, moved to z = -(D - d) so the
			// camera-to-plane distance along the axis is D.
			const Scalar D = ( k == 0 ) ? d : ( Scalar( 2 ) * d );
			CircularDiskGeometry* g = new CircularDiskGeometry( 50.0, 'z' );
			Object* o = new Object( g );
			g->release();
			o->SetPosition( Point3( 0, 0, d - D ) );
			o->FinalizeTransformations();
			const RayIntersection ri = Cast( *o, ray );
			CHECK( ri.geometric.bHit, "14b: (control) the chief ray hits the face-on plane" );
			if( k == 0 ) { vAtFocus = ri.geometric.txFootprint.widthValid; wAtFocus = ri.geometric.txFootprint.worldWidth; }
			else         { vBeyond  = ri.geometric.txFootprint.widthValid; wBeyond  = ri.geometric.txFootprint.worldWidth; }
			o->release();
		}

		const Scalar closed = d * PixelAngle();		// 2*d*tan(fov/2)/height
		std::cout << "    14b worldWidth@focus " << std::scientific << std::setprecision(9) << wAtFocus
		          << "  closed form " << closed
		          << "   14c ratio " << std::defaultfloat << std::setprecision(12)
		          << ( wAtFocus > 0 ? wBeyond / wAtFocus : Scalar( 0 ) ) << std::endl;

		CHECK( vAtFocus && vBeyond,
			"14b: a thin-lens hit reports widthValid -- it reported FALSE on every thin-lens scene "
			"before this change" );
		CHECK( closed > 0 && std::fabs( wAtFocus - closed ) / closed < Scalar( 1e-9 ),
			"14b: the footprint at the plane of focus is exactly 2*d*tan(fov/2)/height ("
			<< std::setprecision(12) << wAtFocus << " vs " << closed << ")" );
		const Scalar ratio = ( wAtFocus > 0 ) ? ( wBeyond / wAtFocus ) : Scalar( 0 );
		CHECK( std::fabs( ratio - Scalar( 2 ) ) < Scalar( 1e-9 ),
			"14c: at twice the focal distance the footprint doubles (" << std::setprecision(12)
			<< ratio << ")" );

		// 14b2 -- the same claim for an OFF-CENTRE lens sample, which is
		// the one that is actually about a LENS rather than about a
		// pinhole.  A film sample's conjugate point on the plane of focus
		// does not depend on which point of the aperture the ray left
		// from -- that is what "in focus" means -- so the main-and-
		// auxiliary displacement there, and hence worldWidth, must be the
		// SAME for every lens sample even though the direction offsets
		// themselves are not.  A differential that re-derived its geometry
		// from the axis, or that widened by the circle of confusion, would
		// break here and nowhere else in this test.
		ThinLensCamera* thin2 = MakeThinLens( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), d, Scalar( 1.4 ) );
		const Point2 farLens( 0.97, 0.11 );		// well out toward the rim
		const Ray rayCentre = ThinLensRay( *thin2, centrePixel, kLensCentre );
		const Ray rayRim    = ThinLensRay( *thin2, centrePixel, farLens );
		thin2->release();

		const Scalar lensSeparation = std::sqrt(
			( rayRim.origin.x - rayCentre.origin.x ) * ( rayRim.origin.x - rayCentre.origin.x ) +
			( rayRim.origin.y - rayCentre.origin.y ) * ( rayRim.origin.y - rayCentre.origin.y ) +
			( rayRim.origin.z - rayCentre.origin.z ) * ( rayRim.origin.z - rayCentre.origin.z ) );

		Scalar wCentre = 0, wRim = 0;
		for( int k = 0; k < 2; k++ ) {
			CircularDiskGeometry* g = new CircularDiskGeometry( 50.0, 'z' );
			Object* o = new Object( g );
			g->release();
			o->FinalizeTransformations();		// disk stays at z = 0, i.e. the plane of focus
			const RayIntersection ri = Cast( *o, ( k == 0 ) ? rayCentre : rayRim );
			CHECK( ri.geometric.bHit, "14b2: (control) both lens samples hit the plane of focus" );
			if( k == 0 ) { wCentre = ri.geometric.txFootprint.worldWidth; }
			else         { wRim    = ri.geometric.txFootprint.worldWidth; }
			o->release();
		}

		std::cout << "    14b2 lens samples " << std::scientific << std::setprecision(3)
		          << lensSeparation << " apart on the aperture; worldWidth "
		          << std::setprecision(9) << wCentre << " vs " << wRim << std::defaultfloat << std::endl;

		CHECK( lensSeparation > Scalar( 1e-4 ),
			"14b2: (oracle) the two lens samples really are far apart on the aperture ("
			<< lensSeparation << ") -- otherwise the next check is vacuous" );
		CHECK( wCentre > 0 && std::fabs( wRim - wCentre ) / wCentre < Scalar( 1e-9 ),
			"14b2: the footprint at the plane of focus is the SAME from any lens sample ("
			<< std::setprecision(12) << wCentre << " vs " << wRim << ")" );
	}

	// ---- 14d: the differentials inherit the lens shift ---------------
	//
	// Oracle: two INDEPENDENTLY generated main rays, one pixel apart,
	// through the SAME point on the lens.  Their direction difference is
	// by definition what rxDir must hold -- and, unlike the shared code
	// path, the oracle never looks at `diffs`.
	//
	// RED-PROOF (the reason this test is not a tautology): the same
	// oracle evaluated on a camera with shift_x = shift_y = 0 gives a
	// MATERIALLY different vector.  If the differentials had been
	// re-derived from the unshifted image-plane geometry -- the obvious
	// hand-rolled implementation -- they would match `unshifted` here
	// and the first CHECK below would fail.  The separation printed is
	// the margin by which the test discriminates.
	{
		const Scalar shiftMM = 8.0;
		ThinLensCamera* shifted   = MakeThinLens( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		                                          d, Scalar( 2.0 ), shiftMM, -Scalar( 5 ) );
		ThinLensCamera* unshifted = MakeThinLens( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		                                          d, Scalar( 2.0 ) );

		// A genuinely off-centre point on the aperture, so the test also
		// pins that the auxiliaries share the MAIN ray's lens sample
		// rather than silently re-sampling the centre.
		const Point2 lensSample( 0.83, 0.17 );

		const Ray  r0 = ThinLensRay( *shifted, offPixel, lensSample );
		const Ray  rX = ThinLensRay( *shifted, Point2( offPixel.x + Scalar( 1 ), offPixel.y ), lensSample );
		const Ray  rY = ThinLensRay( *shifted, Point2( offPixel.x, offPixel.y + Scalar( 1 ) ), lensSample );

		const Vector3 oracleRx = rX.Dir() - r0.Dir();
		const Vector3 oracleRy = rY.Dir() - r0.Dir();

		const Ray u0 = ThinLensRay( *unshifted, offPixel, lensSample );
		const Ray uX = ThinLensRay( *unshifted, Point2( offPixel.x + Scalar( 1 ), offPixel.y ), lensSample );
		const Vector3 unshiftedRx = uX.Dir() - u0.Dir();

		const Scalar errRx = MaxAbsDiff( r0.diffs.rxDir, oracleRx );
		const Scalar errRy = MaxAbsDiff( r0.diffs.ryDir, oracleRy );
		const Scalar sep   = MaxAbsDiff( oracleRx, unshiftedRx );

		// The lens sample is shared, so the auxiliary rays sit on the
		// same point of the aperture: zero origin offsets, and the two
		// oracle rays literally share r0's origin.
		const Scalar originDrift = std::max( std::max(
			std::fabs( rX.origin.x - r0.origin.x ), std::fabs( rX.origin.y - r0.origin.y ) ),
			std::fabs( rX.origin.z - r0.origin.z ) );

		std::cout << "    14d |rxDir - oracle| " << std::scientific << std::setprecision(3) << errRx
		          << "  |ryDir - oracle| " << errRy
		          << "  red-proof separation (shifted vs unshifted oracle) " << sep
		          << std::defaultfloat << std::endl;

		CHECK( errRx < Scalar( 1e-15 ) && errRy < Scalar( 1e-15 ),
			"14d: rxDir / ryDir equal the difference of two independently generated main rays one "
			"pixel apart through the same lens point (" << errRx << ", " << errRy << ")" );
		CHECK( sep > Scalar( 1e-6 ),
			"14d (red-proof): the shifted camera's one-pixel direction step differs materially from "
			"the unshifted camera's -- so 14d's first check really does pin the shift ("
			<< sep << ")" );
		CHECK( originDrift < Scalar( 1e-15 ),
			"14d: the auxiliary rays share the main ray's lens point, hence its origin ("
			<< originDrift << ")" );
		CHECK( MaxAbsDiff( r0.diffs.rxOrigin, Vector3( 0, 0, 0 ) ) == Scalar( 0 ) &&
		       MaxAbsDiff( r0.diffs.ryOrigin, Vector3( 0, 0, 0 ) ) == Scalar( 0 ),
			"14d: shared lens point => exactly zero origin offsets" );

		shifted->release();
		unshifted->release();
	}

	// ---- 14e: EVERY generated thin-lens ray carries differentials ----
	//
	// Both entry points, across the frame, with the real random aperture
	// sampling and with polygonal blades + anamorphic squeeze engaged
	// (the shaped-aperture path is a different branch of SampleAperture,
	// and it must not be able to leave hasDifferentials false).
	{
		ThinLensCamera* thin = MakeThinLens( Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), d, Scalar( 1.4 ) );
		RandomNumberGenerator rng( 11u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

		int nRays = 0, nWithDiffs = 0, nNonDegenerate = 0;
		for( int j = 0; j < 5; j++ ) {
			for( int i = 0; i < 5; i++ ) {
				const Point2 px( Scalar( kRes ) * ( Scalar( i ) + Scalar( 0.5 ) ) / Scalar( 5 ),
				                 Scalar( kRes ) * ( Scalar( j ) + Scalar( 0.5 ) ) / Scalar( 5 ) );
				Ray a;  thin->GenerateRay( rc, a, px );
				Ray b;  thin->GenerateRayWithLensSample( rc, b, px, Point2( 0.21, 0.64 ) );
				nRays += 2;
				nWithDiffs += ( a.hasDifferentials ? 1 : 0 ) + ( b.hasDifferentials ? 1 : 0 );
				if( Vector3Ops::Magnitude( a.diffs.rxDir ) > Scalar( 0 ) &&
				    Vector3Ops::Magnitude( a.diffs.ryDir ) > Scalar( 0 ) ) nNonDegenerate++;
				if( Vector3Ops::Magnitude( b.diffs.rxDir ) > Scalar( 0 ) &&
				    Vector3Ops::Magnitude( b.diffs.ryDir ) > Scalar( 0 ) ) nNonDegenerate++;
			}
		}
		thin->release();

		CHECK( nWithDiffs == nRays,
			"14e: hasDifferentials is true on every generated thin-lens ray, both entry points ("
			<< nWithDiffs << " / " << nRays << ")" );
		CHECK( nNonDegenerate == nRays,
			"14e: and the offsets are non-degenerate, not a zeroed struct ("
			<< nNonDegenerate << " / " << nRays << ")" );
	}

	// ---- 14f: the orthographic camera ------------------------------
	//
	// Parallel projection: the one-pixel step is a pure ORIGIN offset of
	// one viewport pitch and the direction offsets are exactly zero, so
	// on a face-on plane the footprint is the pitch itself -- constant
	// with distance, unlike the pinhole's d*theta.  The camera looks
	// down -Z here deliberately: `OrthographicCamera::GenerateRay` adds
	// its film offset along the WORLD x/y axes rather than the camera's
	// own U/V (a pre-existing quirk, untouched by this change), and
	// -Z-with-+Y-up is the orientation in which the two coincide, so
	// this test measures the differentials and not that quirk.
	{
		const Scalar vp = 4.0;		// viewport scale, both axes
		const Scalar pitch = vp / Scalar( kRes );
		OrthographicCamera* cam = new OrthographicCamera(
			Point3( 0, 0, d ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			kRes, kRes, Vector2( vp, vp ),
			Scalar( 1 ), Scalar( 1 ), Scalar( 0 ), Scalar( 0 ),
			Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );

		RandomNumberGenerator rng( 5u );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		Ray ray;
		cam->GenerateRay( rc, ray, centrePixel );
		cam->release();

		CHECK( ray.hasDifferentials, "14f: the orthographic ray carries differentials" );
		CHECK( MaxAbsDiff( ray.diffs.rxDir, Vector3( 0, 0, 0 ) ) == Scalar( 0 ) &&
		       MaxAbsDiff( ray.diffs.ryDir, Vector3( 0, 0, 0 ) ) == Scalar( 0 ),
			"14f: a parallel projection's auxiliary rays share the main direction exactly" );

		Scalar wNear = 0, wFar = 0;
		for( int k = 0; k < 2; k++ ) {
			const Scalar D = ( k == 0 ) ? d : ( Scalar( 3 ) * d );
			CircularDiskGeometry* g = new CircularDiskGeometry( 50.0, 'z' );
			Object* o = new Object( g );
			g->release();
			o->SetPosition( Point3( 0, 0, d - D ) );
			o->FinalizeTransformations();
			const RayIntersection ri = Cast( *o, ray );
			CHECK( ri.geometric.bHit, "14f: (control) the orthographic ray hits the face-on plane" );
			if( k == 0 ) { wNear = ri.geometric.txFootprint.worldWidth; }
			else         { wFar  = ri.geometric.txFootprint.worldWidth; }
			o->release();
		}

		std::cout << "    14f worldWidth " << std::scientific << std::setprecision(9) << wNear
		          << "  viewport pitch " << pitch
		          << "  at 3x distance " << wFar << std::defaultfloat << std::endl;

		CHECK( std::fabs( wNear - pitch ) / pitch < Scalar( 1e-9 ),
			"14f: the orthographic footprint is one viewport pitch (" << std::setprecision(12)
			<< wNear << " vs " << pitch << ")" );
		CHECK( std::fabs( wFar - wNear ) / pitch < Scalar( 1e-9 ),
			"14f: and it does NOT change with distance, unlike the pinhole's d*theta ("
			<< std::setprecision(12) << wFar << ")" );
	}
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== TextureFootprintTest (docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md 5) ===" << std::endl;

	Test1_DistanceScaling();
	Test2_ClosedForm();
	Test3_MeshAnalyticAgreement();
	Test4_UniformScaleExact();
	Test5_NonUniformScaleExact();
	Test6_UVFreeGeometries();
	Test7_ValidImpliesWidthValid();
	Test8_CsgComposes();
	Test9_NoDifferentials();
	Test10_GrazingGuard();
	Test11_ChartOracle();
	Test12_NoChartNoJacobian();
	Test13_ObjectSpaceWidth();
	Test14_ThinLensAndOrthoDifferentials();

	std::cout << std::endl;
	std::cout << g_passes << " passed, " << g_failures << " failed." << std::endl;
	if( g_failures == 0 ) std::cout << "=== ALL TESTS PASSED ===" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
