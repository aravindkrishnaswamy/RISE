//////////////////////////////////////////////////////////////////////
//
//  MirroredUVWeaveDirectionTest.cpp
//
//  Red-proof and regression test for debt-ledger row DL-12:
//  "Mirrored UV seams flip weave direction: vShadingTangent has no
//   bitangentSign companion, so a dpdu-derived tangent can mirror
//   across a UV seam" (docs/CLOTH_FABRIC_DESIGN.md §15 item 15).
//
//  Verifies:
//    1. Indexed mesh with mirrored UVs (u -> 1-u) has onb.v() pointing
//       along +dpdv, and overall ONB handedness matching -1.0.
//    2. Weave material with non-zero weave_rotation produces symmetric
//       BSDF values across the mirrored UV seam (X -> -X reflection).
//    3. Authored glTF tangents with bitangentSign = -1.0 propagate to
//       shadingBitangentSign and produce the same correct frame.
//    4. Non-indexed mesh twin behaves identically.
//    5. Mirrored transform (scale -1 1 1) combined with mirrored UVs
//       cancels out the reflection: (-1) * (-1) = +1.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Materials/WeaveMaterial.h"
#include "../src/Library/Materials/WeavePresets.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* msg )
{
	if( cond ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "FAIL: " << msg << std::endl;
	}
}

static bool Close( Scalar a, Scalar b, Scalar tol = 1e-5 )
{
	return std::fabs( a - b ) <= tol;
}

static bool VecClose( const Vector3& a, const Vector3& b, Scalar tol = 1e-5 )
{
	return Close( a.x, b.x, tol ) && Close( a.y, b.y, tol ) && Close( a.z, b.z, tol );
}

static void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
{
	ri.geometric.bHit = false;
	ri.geometric.range = RISE_INFINITY;
	ri.geometric.range2 = RISE_INFINITY;
	ri.geometric.ray = r;
	pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
}

// Build a two-quad indexed mesh in the XY plane:
// Quad 1: X in [-1, 0], Y in [0, 1] with standard UVs (u in [0, 1], v in [0, 1])
// Quad 2: X in [0, 1], Y in [0, 1] with mirrored UVs (u in [1, 0], v in [0, 1])
// At seam X = 0: both quads have u = 1.0, v in [0, 1].
static TriangleMeshGeometryIndexed* BuildTwoQuadMeshIndexed( bool withAuthoredTangents = false )
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->BeginIndexedTriangles();

	// Quad 1 vertices (indices 0..3)
	const Point3 v1[4] = {
		Point3( -1, 0, 0 ), Point3( 0, 0, 0 ), Point3( 0, 1, 0 ), Point3( -1, 1, 0 )
	};
	const Point2 uv1[4] = {
		Point2( 0, 0 ), Point2( 1, 0 ), Point2( 1, 1 ), Point2( 0, 1 )
	};

	// Quad 2 vertices (indices 4..7)
	const Point3 v2[4] = {
		Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 1, 1, 0 ), Point3( 0, 1, 0 )
	};
	const Point2 uv2[4] = {
		Point2( 1, 0 ), Point2( 0, 0 ), Point2( 0, 1 ), Point2( 1, 1 )
	};

	const Vector3 n( 0, 0, 1 );

	for( int i = 0; i < 4; i++ ) {
		mesh->AddVertex( v1[i] );
		mesh->AddNormal( n );
		mesh->AddTexCoord( uv1[i] );
		if( withAuthoredTangents ) {
			Tangent4 t;
			t.dir = Vector3( 1, 0, 0 );
			t.bitangentSign = 1.0;
			mesh->AddTangent( t );
		}
	}

	for( int i = 0; i < 4; i++ ) {
		mesh->AddVertex( v2[i] );
		mesh->AddNormal( n );
		mesh->AddTexCoord( uv2[i] );
		if( withAuthoredTangents ) {
			Tangent4 t;
			t.dir = Vector3( -1, 0, 0 );
			t.bitangentSign = -1.0;
			mesh->AddTangent( t );
		}
	}

	// Quad 1 triangles
	IndexedTriangle q1_t1, q1_t2;
	q1_t1.iVertices[0] = 0; q1_t1.iVertices[1] = 1; q1_t1.iVertices[2] = 2;
	q1_t2.iVertices[0] = 0; q1_t2.iVertices[1] = 2; q1_t2.iVertices[2] = 3;
	for( int k = 0; k < 3; k++ ) {
		q1_t1.iNormals[k] = q1_t1.iVertices[k]; q1_t1.iCoords[k] = q1_t1.iVertices[k];
		q1_t2.iNormals[k] = q1_t2.iVertices[k]; q1_t2.iCoords[k] = q1_t2.iVertices[k];
	}
	mesh->AddIndexedTriangle( q1_t1 );
	mesh->AddIndexedTriangle( q1_t2 );

	// Quad 2 triangles
	IndexedTriangle q2_t1, q2_t2;
	q2_t1.iVertices[0] = 4; q2_t1.iVertices[1] = 5; q2_t1.iVertices[2] = 6;
	q2_t2.iVertices[0] = 4; q2_t2.iVertices[1] = 6; q2_t2.iVertices[2] = 7;
	for( int k = 0; k < 3; k++ ) {
		q2_t1.iNormals[k] = q2_t1.iVertices[k]; q2_t1.iCoords[k] = q2_t1.iVertices[k];
		q2_t2.iNormals[k] = q2_t2.iVertices[k]; q2_t2.iCoords[k] = q2_t2.iVertices[k];
	}
	mesh->AddIndexedTriangle( q2_t1 );
	mesh->AddIndexedTriangle( q2_t2 );

	mesh->DoneIndexedTriangles();
	return mesh;
}

// Build a two-quad non-indexed mesh with the same geometry and UVs
static TriangleMeshGeometry* BuildTwoQuadMeshNonIndexed()
{
	TriangleMeshGeometry* mesh = new TriangleMeshGeometry( false );

	const Point3 v1[4] = {
		Point3( -1, 0, 0 ), Point3( 0, 0, 0 ), Point3( 0, 1, 0 ), Point3( -1, 1, 0 )
	};
	const Point2 uv1[4] = {
		Point2( 0, 0 ), Point2( 1, 0 ), Point2( 1, 1 ), Point2( 0, 1 )
	};

	const Point3 v2[4] = {
		Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 1, 1, 0 ), Point3( 0, 1, 0 )
	};
	const Point2 uv2[4] = {
		Point2( 1, 0 ), Point2( 0, 0 ), Point2( 0, 1 ), Point2( 1, 1 )
	};

	const Vector3 n( 0, 0, 1 );

	// Quad 1
	Triangle q1_t1, q1_t2;
	const int idx1[3] = { 0, 1, 2 };
	const int idx2[3] = { 0, 2, 3 };
	for( int k = 0; k < 3; k++ ) {
		q1_t1.vertices[k] = v1[ idx1[k] ]; q1_t1.normals[k] = n; q1_t1.coords[k] = uv1[ idx1[k] ];
		q1_t2.vertices[k] = v1[ idx2[k] ]; q1_t2.normals[k] = n; q1_t2.coords[k] = uv1[ idx2[k] ];
	}
	mesh->AddTriangle( q1_t1 );
	mesh->AddTriangle( q1_t2 );

	// Quad 2
	Triangle q2_t1, q2_t2;
	for( int k = 0; k < 3; k++ ) {
		q2_t1.vertices[k] = v2[ idx1[k] ]; q2_t1.normals[k] = n; q2_t1.coords[k] = uv2[ idx1[k] ];
		q2_t2.vertices[k] = v2[ idx2[k] ]; q2_t2.normals[k] = n; q2_t2.coords[k] = uv2[ idx2[k] ];
	}
	mesh->AddTriangle( q2_t1 );
	mesh->AddTriangle( q2_t2 );

	mesh->DoneTriangles();
	return mesh;
}

static WeaveMaterial* CreateTestWeaveMaterial( Scalar rotRad )
{
	UniformScalarPainter* scale = new UniformScalarPainter( 10.0 ); scale->addref();
	UniformScalarPainter* rot   = new UniformScalarPainter( rotRad ); rot->addref();
	UniformScalarPainter* skew  = new UniformScalarPainter( 0.0 ); skew->addref();
	const IScalarPainter* cov   = 0;
	UniformScalarPainter* gap   = new UniformScalarPainter( 0.0 ); gap->addref();

	UniformScalarPainter* wIor  = new UniformScalarPainter( 1.5 ); wIor->addref();
	UniformScalarPainter* wWid  = new UniformScalarPainter( 0.05 ); wWid->addref(); // narrow specular
	UniformScalarPainter* wAzi  = new UniformScalarPainter( 1.0 ); wAzi->addref();
	UniformScalarPainter* wKd   = new UniformScalarPainter( 0.1 ); wKd->addref();
	UniformScalarPainter* wTilt = new UniformScalarPainter( 0.0 ); wTilt->addref();

	UniformScalarPainter* fIor  = new UniformScalarPainter( 1.5 ); fIor->addref();
	UniformScalarPainter* fWid  = new UniformScalarPainter( 0.5 ); fWid->addref();
	UniformScalarPainter* fAzi  = new UniformScalarPainter( 1.0 ); fAzi->addref();
	UniformScalarPainter* fKd   = new UniformScalarPainter( 0.1 ); fKd->addref();
	UniformScalarPainter* fTilt = new UniformScalarPainter( 0.0 ); fTilt->addref();

	UniformColorPainter*  wCol  = new UniformColorPainter( RISEPel( 1, 1, 1 ) ); wCol->addref();
	UniformColorPainter*  fCol  = new UniformColorPainter( RISEPel( 0.2, 0.2, 0.2 ) ); fCol->addref();
	UniformScalarPainter* noTr  = new UniformScalarPainter( 0.0 ); noTr->addref();

	WeaveMaterial* mat = new WeaveMaterial(
		eWeaveCustom, eWeaveTransmissionNone, *scale, *rot, *skew, cov, *gap,
		*wCol, *wIor, *wWid, *wAzi, *wKd, *wTilt, *noTr,
		*fCol, *fIor, *fWid, *fAzi, *fKd, *fTilt, *noTr );
	mat->addref();

	scale->release(); rot->release(); skew->release(); gap->release();
	wIor->release(); wWid->release(); wAzi->release(); wKd->release(); wTilt->release();
	fIor->release(); fWid->release(); fAzi->release(); fKd->release(); fTilt->release();
	wCol->release(); fCol->release(); noTr->release();

	return mat;
}

// Test 1: Frame vectors and handedness on indexed mesh without authored tangents
static void TestIndexedMeshMirroredUVFrame()
{
	std::cout << "Test 1: Indexed mesh UV-Jacobian frame across mirrored UV seam..." << std::endl;

	TriangleMeshGeometryIndexed* g = BuildTwoQuadMeshIndexed( false );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	// Ray to Quad 1 (standard UV)
	Ray r1( Point3( -0.5, 0.5, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri1( r1, nullRasterizerState );
	Hit( o, r1, ri1 );

	Check( ri1.geometric.bHit, "Test1: Quad 1 hit" );
	Check( ri1.geometric.bHasShadingTangent, "Test1: Quad 1 has shading tangent" );
	Check( VecClose( ri1.geometric.onb.u(), Vector3( 1, 0, 0 ), 1e-6 ), "Test1: Quad 1 onb.u() == (1,0,0)" );
	Check( VecClose( ri1.geometric.onb.v(), Vector3( 0, 1, 0 ), 1e-6 ), "Test1: Quad 1 onb.v() == (0,1,0)" );
	const Scalar h1 = Vector3Ops::Dot( Vector3Ops::Cross( ri1.geometric.onb.u(), ri1.geometric.onb.v() ), ri1.geometric.onb.w() );
	Check( Close( h1, 1.0, 1e-6 ), "Test1: Quad 1 ONB is right-handed (+1)" );

	// Ray to Quad 2 (mirrored UV: u -> 1-u)
	Ray r2( Point3( 0.5, 0.5, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri2( r2, nullRasterizerState );
	Hit( o, r2, ri2 );

	Check( ri2.geometric.bHit, "Test1: Quad 2 hit" );
	Check( ri2.geometric.bHasShadingTangent, "Test1: Quad 2 has shading tangent" );
	Check( VecClose( ri2.geometric.onb.u(), Vector3( -1, 0, 0 ), 1e-6 ), "Test1: Quad 2 onb.u() == (-1,0,0)" );
	Check( VecClose( ri2.geometric.onb.v(), Vector3( 0, 1, 0 ), 1e-6 ), "Test1: Quad 2 onb.v() == (0,1,0) [dpdv direction]" );
	const Scalar h2 = Vector3Ops::Dot( Vector3Ops::Cross( ri2.geometric.onb.u(), ri2.geometric.onb.v() ), ri2.geometric.onb.w() );
	Check( Close( h2, -1.0, 1e-6 ), "Test1: Quad 2 ONB is left-handed (-1)" );

	o->release();
}

// Test 2: Weave BRDF value symmetry across mirrored UV seam
static void TestWeaveBRDFSymmetry()
{
	std::cout << "Test 2: Weave BRDF value symmetry across mirrored UV seam..." << std::endl;

	TriangleMeshGeometryIndexed* g = BuildTwoQuadMeshIndexed( false );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	const Scalar rotAngle = PI / Scalar(6.0); // 30 degrees
	WeaveMaterial* mat = CreateTestWeaveMaterial( rotAngle );
	IBSDF* bsdf = mat->GetBSDF();

	Ray r1( Point3( -0.5, 0.5, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri1( r1, nullRasterizerState );
	Hit( o, r1, ri1 );

	Ray r2( Point3( 0.5, 0.5, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri2( r2, nullRasterizerState );
	Hit( o, r2, ri2 );

	// Choose incident direction aligned with the specular peak on Quad 1
	// For normal (0,0,1) and view (0,0,-1) looking down, specular reflection for warp along
	// (cos 30°, sin 30°, 0) has peak when wi has projection along that tangent.
	const Scalar theta = PI / Scalar(4.0); // 45 deg incidence
	const Scalar sinTh = std::sin( theta );
	const Scalar cosTh = std::cos( theta );
	const Vector3 wi1( std::cos(rotAngle) * sinTh, std::sin(rotAngle) * sinTh, cosTh );

	// On Quad 2 (mirrored across X=0), the mirrored direction has X negated:
	const Vector3 wi2( -std::cos(rotAngle) * sinTh, std::sin(rotAngle) * sinTh, cosTh );

	const RISEPel val1 = bsdf->value( wi1, ri1.geometric );
	const RISEPel val2 = bsdf->value( wi2, ri2.geometric );

	Check( val1.r > Scalar(0.01), "Test2: Quad 1 specular value is positive" );
	char buf[256];
	std::snprintf( buf, sizeof(buf), "Test2: Weave BSDF values agree across seam (val1=%.6f, val2=%.6f)",
		val1.r, val2.r );
	Check( Close( val1.r, val2.r, 1e-4 ), buf );

	mat->release();
	o->release();
}

// Test 3: Authored glTF tangents with bitangentSign = -1.0
static void TestIndexedMeshAuthoredTangent()
{
	std::cout << "Test 3: Indexed mesh with authored tangents across mirrored UV seam..." << std::endl;

	TriangleMeshGeometryIndexed* g = BuildTwoQuadMeshIndexed( true );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r2( Point3( 0.5, 0.5, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri2( r2, nullRasterizerState );
	Hit( o, r2, ri2 );

	Check( ri2.geometric.bHit, "Test3: Quad 2 hit" );
	Check( ri2.geometric.bHasShadingTangent, "Test3: Quad 2 has shading tangent" );
	Check( VecClose( ri2.geometric.onb.u(), Vector3( -1, 0, 0 ), 1e-6 ), "Test3: Quad 2 onb.u() == (-1,0,0)" );
	Check( VecClose( ri2.geometric.onb.v(), Vector3( 0, 1, 0 ), 1e-6 ), "Test3: Quad 2 onb.v() == (0,1,0) [dpdv direction]" );
	const Scalar h2 = Vector3Ops::Dot( Vector3Ops::Cross( ri2.geometric.onb.u(), ri2.geometric.onb.v() ), ri2.geometric.onb.w() );
	Check( Close( h2, -1.0, 1e-6 ), "Test3: Quad 2 ONB is left-handed (-1)" );

	o->release();
}

// Test 4: Non-indexed mesh twin
static void TestNonIndexedMeshMirroredUVFrame()
{
	std::cout << "Test 4: Non-indexed mesh UV-Jacobian frame across mirrored UV seam..." << std::endl;

	TriangleMeshGeometry* g = BuildTwoQuadMeshNonIndexed();
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r2( Point3( 0.5, 0.5, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri2( r2, nullRasterizerState );
	Hit( o, r2, ri2 );

	Check( ri2.geometric.bHit, "Test4: Quad 2 hit" );
	Check( ri2.geometric.bHasShadingTangent, "Test4: Quad 2 has shading tangent" );
	Check( VecClose( ri2.geometric.onb.u(), Vector3( -1, 0, 0 ), 1e-6 ), "Test4: Quad 2 onb.u() == (-1,0,0)" );
	Check( VecClose( ri2.geometric.onb.v(), Vector3( 0, 1, 0 ), 1e-6 ), "Test4: Quad 2 onb.v() == (0,1,0) [dpdv direction]" );
	const Scalar h2 = Vector3Ops::Dot( Vector3Ops::Cross( ri2.geometric.onb.u(), ri2.geometric.onb.v() ), ri2.geometric.onb.w() );
	Check( Close( h2, -1.0, 1e-6 ), "Test4: Quad 2 ONB is left-handed (-1)" );

	o->release();
}

// Test 5: Mirrored transform (scale -1 1 1) combined with mirrored UV
static void TestMirroredTransformCombinedWithMirroredUV()
{
	std::cout << "Test 5: Mirrored transform (scale -1 1 1) combined with mirrored UV..." << std::endl;

	TriangleMeshGeometryIndexed* g = BuildTwoQuadMeshIndexed( false );
	Object* o = new Object( g );
	safe_release( g );
	o->SetStretch( Vector3( -1.0, 1.0, 1.0 ) );
	o->FinalizeTransformations();

	// Under scale -1 1 1:
	// Quad 1 (object X in [-1, 0]) moves to world X in [0, 1]
	// Quad 2 (object X in [0, 1]) moves to world X in [-1, 0]

	// Hit Quad 2 (object X=0.5 -> world X=-0.5)
	Ray r2( Point3( -0.5, 0.5, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri2( r2, nullRasterizerState );
	Hit( o, r2, ri2 );

	Check( ri2.geometric.bHit, "Test5: Quad 2 hit under scale -1 1 1" );
	// For Quad 2: shadingBitangentSign = -1, m_tangentFrameSign = -1
	// Total sign = (-1) * (-1) = +1 -> FlipV is NOT called, ONB is RIGHT-HANDED!
	const Scalar h2 = Vector3Ops::Dot( Vector3Ops::Cross( ri2.geometric.onb.u(), ri2.geometric.onb.v() ), ri2.geometric.onb.w() );
	Check( Close( h2, 1.0, 1e-6 ), "Test5: Quad 2 (mirrored UV + mirrored transform) ONB is right-handed (+1)" );

	o->release();
}

int main( int argc, char* argv[] )
{
	std::cout << "============================================================" << std::endl;
	std::cout << "MirroredUVWeaveDirectionTest (DL-12 red-proof)" << std::endl;
	std::cout << "============================================================" << std::endl;

	TestIndexedMeshMirroredUVFrame();
	TestWeaveBRDFSymmetry();
	TestIndexedMeshAuthoredTangent();
	TestNonIndexedMeshMirroredUVFrame();
	TestMirroredTransformCombinedWithMirroredUV();

	std::cout << "============================================================" << std::endl;
	std::cout << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	std::cout << "============================================================" << std::endl;

	return (failCount == 0) ? 0 : 1;
}
