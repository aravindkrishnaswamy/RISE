//////////////////////////////////////////////////////////////////////
//
//  ProceduralMeshTest.cpp - Fidelity tests for the procedural mesh
//  factories that replaced the Python bakers:
//
//    RISE_API_CreateGuillocheDialGeometry  vs dial_mesh_gen.build_mesh
//    RISE_API_CreateSweptBandGeometry      vs strap_mesh_gen.py
//
//  Golden values were generated from the Python sources (2026-06-11).
//  Dial goldens are full-precision doubles from build_mesh at
//  mesh_n=160; band/stitch goldens come from the default-args raw2
//  files, whose values are %.5f-rounded (hence the 1e-4 tolerances).
//
//  Triangle-order note: build_mesh emits np.concatenate([triA, triB])
//  (ALL first-triangles, then ALL second-triangles) while the C++
//  factory interleaves (A_k, B_k) per cell.  The sets are identical;
//  positionally Py[k] == C++[2k] and Py[M+k] == C++[2k+1] for
//  M = ntris/2.  The band/stitch generators append inline, so their
//  emission order matches the C++ exactly.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( Scalar got, Scalar want, Scalar tol, const char* name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 12 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want
			<< "  |d| " << std::fabs( got - want ) << std::endl;
	}
}

struct VertGolden {
	unsigned int idx;
	Scalar px, py, pz, nx, ny, nz, u, v;
};

static void CheckVert( const TriangleMeshGeometryIndexed* mesh, const VertGolden& g,
                       Scalar posTol, Scalar nTol, Scalar uvTol, const char* label )
{
	char name[128];
	const Vertex& p = mesh->getVertices()[g.idx];
	const Normal& n = mesh->getNormals()[g.idx];
	const TexCoord& c = mesh->getCoords()[g.idx];
	snprintf( name, sizeof(name), "%s v[%u] pos", label, g.idx );
	Check( std::fabs(p.x-g.px) <= posTol && std::fabs(p.y-g.py) <= posTol && std::fabs(p.z-g.pz) <= posTol, name );
	snprintf( name, sizeof(name), "%s v[%u] normal", label, g.idx );
	Check( std::fabs(n.x-g.nx) <= nTol && std::fabs(n.y-g.ny) <= nTol && std::fabs(n.z-g.nz) <= nTol, name );
	snprintf( name, sizeof(name), "%s v[%u] uv", label, g.idx );
	Check( std::fabs(c.x-g.u) <= uvTol && std::fabs(c.y-g.v) <= uvTol, name );
}

// Compare a face's three vertex POSITIONS against the golden vertex indices
// (faces store raw Point3* pointers; positions sidestep storage layout).
static void CheckFace( const TriangleMeshGeometryIndexed* mesh, unsigned int face,
                       unsigned int ia, unsigned int ib, unsigned int ic, const char* label )
{
	char name[128];
	snprintf( name, sizeof(name), "%s face[%u] = (%u,%u,%u)", label, face, ia, ib, ic );
	const PointerPolygon_Template<3>& f = mesh->getFaces()[face];
	const VerticesListType& verts = mesh->getVertices();
	const unsigned int want[3] = { ia, ib, ic };
	bool ok = true;
	for( int k = 0; k < 3; ++k ) {
		const Point3& got = *f.pVertices[k];
		const Point3& exp = verts[ want[k] ];
		ok = ok && std::fabs(got.x-exp.x) <= 1e-12 && std::fabs(got.y-exp.y) <= 1e-12 && std::fabs(got.z-exp.z) <= 1e-12;
	}
	Check( ok, name );
}

//////////////////////////////////////////////////////////////////////

static void TestDialMeshBuilder()
{
	std::cout << "Test 1: guilloche dial mesh vs dial_mesh_gen.build_mesh (mesh_n=160, defaults)" << std::endl;
	GuillocheDialDescriptor d;
	d.meshN = 160;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateGuillocheDialGeometry( &pi, d ), "factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	Check( mesh != 0, "concrete mesh type" );
	if( !mesh ) { pi->release(); return; }

	Check( mesh->numPoints() == 19856, "vertex count 19856" );
	Check( mesh->getFaces().size() == 39082, "triangle count 39082" );

	// Python build_mesh full-precision goldens.
	const VertGolden gold[] = {
		{ 0,     -3.238993711, -20.340880503,  0.131848819, -0.000000000,  0.210263107, 0.977644836, 0.421383648, 0.006289308 },
		{ 137,    1.684276730, -19.563522013, -0.198239384, -0.065670371, -0.265682466, 0.961821309, 0.540880503, 0.025157233 },
		{ 9999,  -1.943396226,   0.129559748,  0.094222472, -0.414400587,  0.433895178, 0.800004455, 0.452830189, 0.503144654 },
		{ 19855,  3.238993711,  20.340880503,  0.131848819, -0.000000000, -0.210263107, 0.977644836, 0.578616352, 0.993710692 },
	};
	for( size_t i = 0; i < sizeof(gold)/sizeof(gold[0]); ++i ) {
		CheckVert( mesh, gold[i], Scalar(1e-6), Scalar(1e-6), Scalar(1e-6), "dial" );
	}

	// Triangle goldens with the A/B-block position mapping (M = 19541):
	//   Py t[0]     = (0, 1, 32)             -> C++ face[0]
	//   Py t[12345] = (12441, 12442, 12598)  -> C++ face[24690]
	//   Py t[39081] = (19823, 19855, 19854)  -> C++ face[39081]
	CheckFace( mesh, 0,     0,     1,     32,    "dial" );
	CheckFace( mesh, 24690, 12441, 12442, 12598, "dial" );
	CheckFace( mesh, 39081, 19823, 19855, 19854, "dial" );

	// every normal unit length; every uv inside [0,1]; every vertex inside the dial
	bool unitN = true, uvIn = true, rIn = true;
	for( unsigned int i = 0; i < mesh->numPoints(); ++i ) {
		const Normal& n = mesh->getNormals()[i];
		const Scalar len = std::sqrt( n.x*n.x + n.y*n.y + n.z*n.z );
		if( std::fabs( len - 1.0 ) > 1e-9 ) unitN = false;
		const TexCoord& c = mesh->getCoords()[i];
		if( c.x < 0 || c.x > 1 || c.y < 0 || c.y > 1 ) uvIn = false;
		const Vertex& p = mesh->getVertices()[i];
		if( std::sqrt( p.x*p.x + p.y*p.y ) > d.radius * (1.0 + 1e-12) ) rIn = false;
	}
	Check( unitN, "all normals unit length" );
	Check( uvIn, "all uv in [0,1]" );
	Check( rIn, "all vertices inside the dial radius" );

	pi->release();
}

static void TestSweptBandBuilder()
{
	std::cout << "Test 2: swept band vs strap_mesh_gen.py (default args)" << std::endl;
	// The Python's built-in control path with flatz = -10.30 + 3.0/2.
	const double pts[] = { 24.0,-3.4, 32.0,-4.9, 43.0,-7.3, 56.0,-8.25, 70.0,-8.68, 86.0,-8.50, 104.0,-8.78 };
	SweptBandDescriptor d;
	d.pathPoints = pts;
	d.numPathPoints = 7;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweptBandGeometry( &pi, d ), "band factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "band concrete type" ); pi->release(); return; }

	Check( mesh->numPoints() == 3892, "band vertex count 3892" );
	Check( mesh->getFaces().size() == 7754, "band triangle count 7754" );

	// raw2 goldens are %.5f-rounded -> 1e-4 tolerances (uv %.6f -> 5e-6).
	const VertGolden gold[] = {
		{ 0,    -12.63000,  24.00870, -3.35337, -0.30483,  0.17472,  0.93625, 0.000000, 0.000000 },
		{ 7,      0.97154,  24.37547, -1.38799,  0.00670,  0.18344,  0.98301, 0.538462, 0.000000 },
		{ 1946, -11.36500,  55.99761, -8.29737,  0.00000, -0.05040, -0.99873, 0.000000, 0.500000 },
		{ 3891,  10.10000, 103.99924, -8.82743,  0.00000, -0.01605, -0.99987, 1.000000, 1.000000 },
	};
	for( size_t i = 0; i < sizeof(gold)/sizeof(gold[0]); ++i ) {
		CheckVert( mesh, gold[i], Scalar(1e-4), Scalar(1e-4), Scalar(5e-6), "band" );
	}
	CheckFace( mesh, 0,    0,    28,   29,   "band" );
	CheckFace( mesh, 3877, 1935, 1964, 1936, "band" );
	CheckFace( mesh, 7753, 3876, 3891, 3890, "band" );

	// extents (the Python printout: y[23.72,104.03] z[-10.28,-1.39] x +-12.63)
	Scalar ymin = 1e30, ymax = -1e30, zmin = 1e30, zmax = -1e30, xmax = -1e30;
	for( unsigned int i = 0; i < mesh->numPoints(); ++i ) {
		const Vertex& p = mesh->getVertices()[i];
		if( p.y < ymin ) ymin = p.y;
		if( p.y > ymax ) ymax = p.y;
		if( p.z < zmin ) zmin = p.z;
		if( p.z > zmax ) zmax = p.z;
		if( std::fabs(p.x) > xmax ) xmax = std::fabs(p.x);
	}
	CheckClose( ymin,  23.724830, 1e-4, "band ymin" );
	CheckClose( ymax, 104.032850, 1e-4, "band ymax" );
	CheckClose( zmin, -10.279810, 1e-4, "band zmin" );
	CheckClose( zmax,  -1.387990, 1e-4, "band zmax" );
	CheckClose( xmax,  12.630000, 1e-4, "band |x|max" );

	pi->release();
}

static void TestStitchBuilder()
{
	std::cout << "Test 3: stitch thread capsules vs strap_mesh_gen.py (default args)" << std::endl;
	const double pts[] = { 24.0,-3.4, 32.0,-4.9, 43.0,-7.3, 56.0,-8.25, 70.0,-8.68, 86.0,-8.50, 104.0,-8.78 };
	SweptBandDescriptor d;
	d.pathPoints = pts;
	d.numPathPoints = 7;
	d.emitStitches = true;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweptBandGeometry( &pi, d ), "stitch factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "stitch concrete type" ); pi->release(); return; }

	// 34 verts (4 rings x 8 + 2 tips) and 64 tris per capsule; 68 capsules.
	Check( mesh->numPoints() == 2312, "stitch vertex count 2312" );
	Check( mesh->getFaces().size() == 4352, "stitch triangle count 4352" );

	const VertGolden gold[] = {
		{ 0,    -10.49172,  24.99773, -2.12409,  0.96126, -0.27117,  0.04944, 0.000000, 0.500000 },
		{ 7,    -10.52128,  24.99275, -2.19865,  0.67971, -0.31858, -0.66068, 0.875000, 0.500000 },
		{ 1156,  -9.25917,  64.97017, -7.15938,  0.96126, -0.27556,  0.00658, 0.000000, 0.500000 },
		{ 2311,   8.19694, 104.67183, -7.35367, -0.27564,  0.96114, -0.01543, 0.500000, 0.500000 },
	};
	for( size_t i = 0; i < sizeof(gold)/sizeof(gold[0]); ++i ) {
		CheckVert( mesh, gold[i], Scalar(1e-4), Scalar(1e-4), Scalar(5e-6), "stitch" );
	}
	CheckFace( mesh, 0,    0,    8,    9,    "stitch" );
	CheckFace( mesh, 2176, 1156, 1164, 1165, "stitch" );
	CheckFace( mesh, 4351, 2311, 2309, 2302, "stitch" );

	pi->release();
}

// Regression for the review-round-1 P1: with lightning_relief < 0 (the
// documented "recess" mode) the relief term is GATED by the petal smoothstep,
// so the analytic RawMin/RawMax over-state the achievable raw range -- the
// factory must normalise by the ACHIEVED grid range (exact Python `_finish`),
// not the analytic shortcut.  Goldens from dial_variants_gen
// --field radial --lightning-relief -0.3 --mesh-n 160.
static void TestDialNegativeReliefNormalization()
{
	std::cout << "Test 4: dial grid normalization at lightning_relief < 0 (recess mode)" << std::endl;
	GuillocheDialDescriptor d;
	d.pattern = eGuillochePatternRadial;
	d.lightningRelief = -0.3;
	d.meshN = 160;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateGuillocheDialGeometry( &pi, d ), "recess factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "recess concrete type" ); pi->release(); return; }

	Check( mesh->numPoints() == 19856, "recess vertex count 19856" );
	const VertGolden gold[] = {
		{ 0,     -3.238993711, -20.340880503,  0.111406931,  0.070891112,  0.147809502, 0.986471896, 0.421383648, 0.006289308 },
		{ 137,    1.684276730, -19.563522013, -0.151068349, -0.409774675, -0.426626803, 0.806271845, 0.540880503, 0.025157233 },
		{ 9999,  -1.943396226,   0.129559748, -0.207089861, -0.434304004,  0.135130389, 0.890572743, 0.452830189, 0.503144654 },
		{ 19855,  3.238993711,  20.340880503,  0.111406931, -0.070891112, -0.147809502, 0.986471896, 0.578616352, 0.993710692 },
	};
	for( size_t i = 0; i < sizeof(gold)/sizeof(gold[0]); ++i ) {
		CheckVert( mesh, gold[i], Scalar(1e-6), Scalar(1e-6), Scalar(1e-6), "recess" );
	}
	pi->release();
}

int main( int, char** )
{
	std::cout << "ProceduralMeshTest -- procedural mesh factories vs Python baker goldens" << std::endl << std::endl;
	TestDialMeshBuilder();
	TestSweptBandBuilder();
	TestStitchBuilder();
	TestDialNegativeReliefNormalization();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
