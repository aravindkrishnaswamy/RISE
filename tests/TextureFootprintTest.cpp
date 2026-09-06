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
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CircularDiskGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
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
static TriangleMeshGeometryIndexed* BuildMeshSphere( const Scalar radius, const int nu, const int nv )
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
			coords.push_back( Point2( Scalar( i ) / Scalar( nu ), Scalar( j ) / Scalar( nv ) ) );
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

	std::cout << std::endl;
	std::cout << g_passes << " passed, " << g_failures << " failed." << std::endl;
	if( g_failures == 0 ) std::cout << "=== ALL TESTS PASSED ===" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
