//////////////////////////////////////////////////////////////////////
//
//  ProceduralMeshTest.cpp - Tests for the procedural mesh factories:
//
//    RISE_API_CreateGuillocheDiskGeometry   vs dial_mesh_gen.build_mesh
//                                           (Python golden values)
//    RISE_API_CreateSweepGeometry           vs FIRST PRINCIPLES (a
//                                           circular profile on a straight
//                                           path IS a cylinder; taper IS a
//                                           frustum; a circular path IS a
//                                           torus-like ring)
//    RISE_API_CreatePathInstancesGeometry   vs first principles (N spheres
//                                           at exact arc positions)
//
//  Disk goldens are full-precision doubles from build_mesh at mesh_n=160
//  (generated 2026-06-11).  Triangle-order note for the disk: build_mesh
//  emits np.concatenate([triA, triB]) (ALL first-triangles, then ALL
//  second) while the C++ factory interleaves (A_k, B_k) per cell.  The
//  sets are identical; positionally Py[k] == C++[2k] and
//  Py[M+k] == C++[2k+1] for M = ntris/2.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
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
	GuillocheDiskDescriptor d;
	d.meshN = 160;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateGuillocheDiskGeometry( &pi, d ), "factory succeeds" );
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

static void TestSweepCylinder()
{
	std::cout << "Test 2: sweep_geometry first principles -- circular profile + straight path == cylinder" << std::endl;
	// 32-gon profile of radius 2 (CCW), straight path along +Z of length 10.
	const int NP = 32;
	std::vector<double> prof;
	for( int k = 0; k < NP; ++k ) {
		const double a = 2.0 * 3.14159265358979323846 * k / NP;
		prof.push_back( 2.0 * std::cos( a ) );
		prof.push_back( 2.0 * std::sin( a ) );
	}
	const double pts[] = { 0,0,0,  0,0,5,  0,0,10 };
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.nLen = 20;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "cylinder sweep factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "cylinder concrete type" ); pi->release(); return; }

	// rings: per = max(2, 20/2) = 10 -> n = 2*10+1 = 21 stations
	const unsigned int nRings = 21;
	Check( mesh->numPoints() == nRings * NP + 2 * NP, "cylinder vertex count (rings + 2 caps)" );
	Check( mesh->getFaces().size() == ( nRings - 1 ) * NP * 2 + 2 * ( NP - 2 ), "cylinder triangle count" );

	// every ring vertex EXACTLY radius 2 from the Z axis; radial outward normals;
	// z in [0, 10]; v fraction == z/10
	bool radiusOK = true, normalOK = true, zOK = true, vOK = true;
	for( unsigned int i = 0; i < nRings * NP; ++i ) {
		const Vertex& p = mesh->getVertices()[i];
		const Normal& n = mesh->getNormals()[i];
		const Scalar r = std::sqrt( p.x*p.x + p.y*p.y );
		if( std::fabs( r - 2.0 ) > 1e-9 ) radiusOK = false;
		const Scalar ndot = ( n.x * p.x + n.y * p.y ) / ( r > 0 ? r : 1 );
		if( ndot < 1.0 - 1e-9 || std::fabs( n.z ) > 1e-9 ) normalOK = false;
		if( p.z < -1e-9 || p.z > 10.0 + 1e-9 ) zOK = false;
		const TexCoord& c = mesh->getCoords()[i];
		if( std::fabs( c.y - p.z / 10.0 ) > 1e-9 ) vOK = false;
	}
	Check( radiusOK, "cylinder: all ring vertices exactly r=2 off the axis" );
	Check( normalOK, "cylinder: all ring normals exactly radial" );
	Check( zOK, "cylinder: z within the path extent" );
	Check( vOK, "cylinder: v == path fraction" );

	// cap normals along -/+Z (start/end) -- after the rings come 2*NP cap verts
	bool capOK = true;
	for( unsigned int k = 0; k < NP; ++k ) {
		const Normal& n0 = mesh->getNormals()[ nRings * NP + k ];
		const Normal& n1 = mesh->getNormals()[ nRings * NP + NP + k ];
		if( std::fabs( n0.z + 1.0 ) > 1e-9 || std::fabs( n1.z - 1.0 ) > 1e-9 ) capOK = false;
	}
	Check( capOK, "cylinder: cap normals -Z / +Z" );

	// GEOMETRIC cap winding must agree with the shading normal (review
	// round: the flip predicate was inverted -- shading hid it because the
	// mesh is double-sided, but dielectric enter/exit classification cares).
	// Every cap face's (v1-v0)x(v2-v0) must point along its capN.
	{
		bool windOK = true;
		const size_t nFaces = mesh->getFaces().size();
		const size_t sideFaces = ( nRings - 1 ) * NP * 2;
		for( size_t f = sideFaces; f < nFaces; ++f ) {
			const PointerPolygon_Template<3>& face = mesh->getFaces()[f];
			const Point3& a = *face.pVertices[0];
			const Point3& b = *face.pVertices[1];
			const Point3& c = *face.pVertices[2];
			const Scalar gz = ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x );
			const Scalar capZ = ( a.z < 5.0 ) ? -1.0 : 1.0;	// start cap faces -Z, end +Z
			if( gz * capZ <= 0 ) windOK = false;
		}
		Check( windOK, "cylinder: cap GEOMETRIC winding matches the cap normal" );
	}
	pi->release();
}

// Review-round regression: the documented duplicate-a-point hard-edge idiom
// must not break the cap triangulation (zero-length edges used to deadlock
// the ear clipper).
static void TestSweepDuplicatedProfilePoint()
{
	std::cout << "Test 2b: duplicated profile point (hard-edge idiom) with caps on" << std::endl;
	const double prof[] = { -1,-1,  1,-1,  1,1,  1,1,  -1,1 };	// square with a duplicated corner
	const double pts[] = { 0,0,0,  0,0,4 };
	SweepDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 5;
	d.pathPoints = pts; d.numPathPoints = 2;
	d.nLen = 4;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "duplicated-corner sweep with caps succeeds" );
	if( pi ) {
		const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
		Check( mesh && mesh->getFaces().size() > 0, "duplicated-corner sweep emits triangles" );
		pi->release();
	}
}

static void TestSweepTaperAndTorus()
{
	std::cout << "Test 3: sweep taper (cone frustum) + closed-curve RMF (torus-like ring)" << std::endl;
	// taper: same cylinder but end_scale 0.25 -> end ring radius 0.5
	const int NP = 16;
	std::vector<double> prof;
	for( int k = 0; k < NP; ++k ) {
		const double a = 2.0 * 3.14159265358979323846 * k / NP;
		prof.push_back( 2.0 * std::cos( a ) );
		prof.push_back( 2.0 * std::sin( a ) );
	}
	{
		const double pts[] = { 0,0,0,  0,0,10 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = NP;
		d.pathPoints = pts; d.numPathPoints = 2;
		d.nLen = 10;
		d.endScaleX = 0.25; d.endScaleY = 0.25;
		d.capStart = false; d.capEnd = false;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "frustum factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			const unsigned int n = mesh->numPoints();
			// no caps: vertices are exactly rings; last ring = last NP vertices
			bool endOK = true, startOK = true;
			for( int k = 0; k < NP; ++k ) {
				const Vertex& pe = mesh->getVertices()[ n - NP + k ];
				const Vertex& ps = mesh->getVertices()[ k ];
				if( std::fabs( std::sqrt( pe.x*pe.x + pe.y*pe.y ) - 0.5 ) > 1e-9 ) endOK = false;
				if( std::fabs( std::sqrt( ps.x*ps.x + ps.y*ps.y ) - 2.0 ) > 1e-9 ) startOK = false;
			}
			Check( startOK, "frustum: start ring r=2" );
			Check( endOK, "frustum: end ring r=0.5 (end_scale 0.25)" );
			pi->release();
		}
	}
	// RMF around a closed-ish circular path: profile circle r=1 swept along a
	// 12-point circle of radius 10 -> a torus-like ring; every vertex must be
	// ~1 from the ring centreline (Catmull-Rom through 12 points deviates from
	// a true circle by < 0.6%, hence the loose tolerance), and the frame must
	// never flip (consecutive ring binormal continuity is implied by vertex
	// continuity: max nearest-vertex jump between consecutive rings stays small).
	{
		std::vector<double> circ;
		const int NC = 12;
		for( int k = 0; k < NC; ++k ) {
			const double a = 2.0 * 3.14159265358979323846 * k / NC;
			circ.push_back( 10.0 * std::cos( a ) );
			circ.push_back( 10.0 * std::sin( a ) );
			circ.push_back( 0.0 );
		}
		// close the loop explicitly (back to the start point)
		circ.push_back( 10.0 ); circ.push_back( 0.0 ); circ.push_back( 0.0 );
		std::vector<double> prof1;
		const int NP1 = 12;
		for( int k = 0; k < NP1; ++k ) {
			const double a = 2.0 * 3.14159265358979323846 * k / NP1;
			prof1.push_back( std::cos( a ) );
			prof1.push_back( std::sin( a ) );
		}
		SweepDescriptor d;
		d.profilePoints = &prof1[0]; d.numProfilePoints = NP1;
		d.pathPoints = &circ[0]; d.numPathPoints = NC + 1;
		d.nLen = 120;
		d.capStart = false; d.capEnd = false;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "ring sweep factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			bool tubeOK = true;
			Scalar maxJump = 0;
			const unsigned int n = mesh->numPoints();
			const unsigned int nRings = n / NP1;
			// skip the 10% of stations nearest each end: the path sampler is
			// an OPEN-path Catmull-Rom (reflective end padding), so the two
			// seam segments of a closed loop legitimately deviate from the
			// ideal circle; the interior must track it tightly.
			const unsigned int skip = nRings / 10 + 1;
			for( unsigned int ring = 0; ring < nRings; ++ring ) {
				for( unsigned int k = 0; k < (unsigned int)NP1; ++k ) {
					const unsigned int i = ring * NP1 + k;
					const Vertex& p = mesh->getVertices()[i];
					if( ring >= skip && ring < nRings - skip ) {
						const Scalar ringR = std::sqrt( p.x*p.x + p.y*p.y );
						const Scalar dr = ringR - 10.0;
						const Scalar dist = std::sqrt( dr*dr + p.z*p.z );
						if( std::fabs( dist - 1.0 ) > 0.05 ) tubeOK = false;
					}
					if( i >= (unsigned int)NP1 ) {
						const Vertex& q = mesh->getVertices()[ i - NP1 ];
						const Scalar jump = std::sqrt( (p.x-q.x)*(p.x-q.x) + (p.y-q.y)*(p.y-q.y) + (p.z-q.z)*(p.z-q.z) );
						if( jump > maxJump ) maxJump = jump;
					}
				}
			}
			Check( tubeOK, "ring: interior vertices ~1 from the centreline circle" );
			Check( maxJump < 1.0, "ring: RMF never flips (consecutive-ring vertex jumps stay small)" );
			pi->release();
		}
	}
}

static void TestPathInstances()
{
	std::cout << "Test 4: path_instances_geometry -- spheres along a straight path" << std::endl;
	// template: unit sphere at the origin (tessellates through the universal contract)
	IGeometry* pSphere = 0;
	Check( RISE_API_CreateSphereGeometry( &pSphere, 1.0 ), "template sphere created" );
	if( !pSphere ) return;

	const double pts[] = { 0,0,0,  0,0,50,  0,0,100 };
	PathInstancesDescriptor d;
	d.pGeometry = pSphere;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.pitch = 10.0;
	d.phase = 5.0;
	d.detail = 8;
	d.scale = 0.5;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreatePathInstancesGeometry( &pi, d ), "instancer factory succeeds" );
	if( pi ) {
		const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
		// 10 instances at z = 5, 15, ..., 95
		IndexTriangleListType tTris; VerticesListType tVerts; NormalsListType tNorms; TexCoordsListType tCoords;
		pSphere->TessellateToMesh( tTris, tVerts, tNorms, tCoords, 8 );
		const unsigned int perInst = (unsigned int)tVerts.size();
		Check( perInst > 0 && mesh->numPoints() == perInst * 10, "instancer: 10 instances (pitch 10, phase 5, length 100)" );
		bool centroidsOK = true, radiusOK = true;
		for( unsigned int inst = 0; inst < 10 && perInst > 0; ++inst ) {
			Scalar cx = 0, cy = 0, cz = 0;
			for( unsigned int v = 0; v < perInst; ++v ) {
				const Vertex& p = mesh->getVertices()[ inst * perInst + v ];
				cx += p.x; cy += p.y; cz += p.z;
			}
			cx /= perInst; cy /= perInst; cz /= perInst;
			if( std::fabs( cx ) > 0.05 || std::fabs( cy ) > 0.05 ||
				std::fabs( cz - ( 5.0 + 10.0 * inst ) ) > 0.05 ) centroidsOK = false;
			// scaled radius 0.5 about the EXACT instance position (the
			// tessellated UV sphere's duplicated seam column shifts the
			// vertex centroid slightly, so don't measure from it)
			const Scalar iz = 5.0 + 10.0 * inst;
			for( unsigned int v = 0; v < perInst; ++v ) {
				const Vertex& p = mesh->getVertices()[ inst * perInst + v ];
				const Scalar r = std::sqrt( p.x*p.x + p.y*p.y + (p.z-iz)*(p.z-iz) );
				if( r > 0.5 + 1e-6 ) radiusOK = false;
			}
		}
		Check( centroidsOK, "instancer: centroids at z = 5 + 10k" );
		Check( radiusOK, "instancer: uniform scale 0.5 applied" );
		pi->release();
	}
	pSphere->release();
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
	GuillocheDiskDescriptor d;
	d.pattern = eGuillochePatternRadial;
	d.lightningRelief = -0.3;
	d.meshN = 160;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateGuillocheDiskGeometry( &pi, d ), "recess factory succeeds" );
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
	TestSweepCylinder();
	TestSweepDuplicatedProfilePoint();
	TestSweepTaperAndTorus();
	TestPathInstances();
	TestDialNegativeReliefNormalization();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
