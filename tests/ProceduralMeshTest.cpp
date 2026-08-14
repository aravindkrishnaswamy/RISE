//////////////////////////////////////////////////////////////////////
//
//  ProceduralMeshTest.cpp - Tests for the procedural mesh factories:
//
//    RISE_API_CreateSweepGeometry           vs FIRST PRINCIPLES (a
//                                           circular profile on a straight
//                                           path IS a cylinder; taper IS a
//                                           frustum; a circular path IS a
//                                           torus-like ring)
//    RISE_API_CreatePathInstancesGeometry   vs first principles (N spheres
//                                           at exact arc positions)
//
//  (The guilloché dial relief is no longer a bespoke disk geometry -- it is
//  authored in-scene as an expression_function2d displaced onto a
//  cartesian_disk_geometry; its fidelity is proven in
//  tests/ExpressionFunction2DTest.)
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>
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

//////////////////////////////////////////////////////////////////////

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
	// RMF around a closed-ish circular path using the OPEN-path mode with an
	// explicitly-repeated closing point (the pre-path_closed idiom) -- kept in
	// place as the open-path regression.  See TestSweepClosedLoopRing() below
	// for the path_closed TRUE case, which needs no explicit closing point and
	// no seam skip-band: profile circle r=1 swept along a
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

// half-extent of sweep ring `ring` (np profile verts) along world X and world Y
static void RingExtent( const TriangleMeshGeometryIndexed* m, unsigned int ring,
		unsigned int np, Scalar& halfX, Scalar& halfY )
{
	const Vertex& p0 = m->getVertices()[ ring * np ];
	Scalar x0 = p0.x, x1 = p0.x, y0 = p0.y, y1 = p0.y;
	for( unsigned int k = 1; k < np; ++k ) {
		const Vertex& p = m->getVertices()[ ring * np + k ];
		if( p.x < x0 ) x0 = p.x;
		if( p.x > x1 ) x1 = p.x;
		if( p.y < y0 ) y0 = p.y;
		if( p.y > y1 ) y1 = p.y;
	}
	halfX = ( x1 - x0 ) * Scalar(0.5);
	halfY = ( y1 - y0 ) * Scalar(0.5);
}

// Per-station width (point_width) vs FIRST PRINCIPLES.  A circular profile
// (r=2) swept along a straight +Z path maps profile x -> +X (binormal) and
// h -> -Y (frame normal), so a ring's world half-extents are (2*sx, 2*sy) with
// sx = end_scale_x_taper * widthMul, sy = end_scale_y_taper.  point_width scales
// the WIDTH (x) axis only, Catmull-Rom interpolated onto the path samples and
// composed MULTIPLICATIVELY with end_scale_x.
// C2 slice: `profile_circle` / `profile_rect` are PARSER-side conveniences
// (SweepGeometryAsciiChunkParser expands them into the same profile array
// `profile_point` lines would produce, then calls RISE_API_CreateSweepGeometry
// unchanged -- see GuillocheChunkParseTest.cpp for the parse-level plumbing:
// happy path, mutual exclusion, and validation errors).  These tests
// reproduce that exact expansion formula at the RISE_API level and check the
// resulting mesh against first principles, proving the formula itself.
static void TestProfileCircleConvenience()
{
	std::cout << "Test 2c: profile_circle convenience formula -- reproduces the exact hand-built cylinder" << std::endl;
	// x = r*cos(2*pi*k/n), h = r*sin(2*pi*k/n) for k in [0,n) -- the CCW
	// regular n-gon the profile_circle chunk parameter expands to.
	const int NP = 32;
	const double r = 2.0;
	std::vector<double> prof;
	for( int k = 0; k < NP; ++k ) {
		const double a = 2.0 * 3.14159265358979323846 * k / NP;
		prof.push_back( r * std::cos( a ) );
		prof.push_back( r * std::sin( a ) );
	}
	const double pts[] = { 0,0,0,  0,0,10 };
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.pathPoints = pts; d.numPathPoints = 2;
	d.nLen = 20;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "profile_circle-equivalent cylinder factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "profile_circle cylinder concrete type" ); pi->release(); return; }

	// 2 path points -> segs=1, per=max(2,20/1)=20 -> n = 21 stations (matches
	// TestSweepCylinder's ring count exactly).
	const unsigned int nRings = 21;
	Check( mesh->numPoints() == nRings * NP + 2 * NP, "profile_circle cylinder vertex count (rings + 2 caps)" );
	Check( mesh->getFaces().size() == ( nRings - 1 ) * NP * 2 + 2 * ( NP - 2 ), "profile_circle cylinder triangle count" );

	bool radiusOK = true, normalOK = true, zOK = true, vOK = true;
	for( unsigned int i = 0; i < nRings * NP; ++i ) {
		const Vertex& p = mesh->getVertices()[i];
		const Normal& n = mesh->getNormals()[i];
		const Scalar rr = std::sqrt( p.x*p.x + p.y*p.y );
		if( std::fabs( rr - r ) > 1e-9 ) radiusOK = false;
		const Scalar ndot = ( n.x * p.x + n.y * p.y ) / ( rr > 0 ? rr : 1 );
		if( ndot < 1.0 - 1e-9 || std::fabs( n.z ) > 1e-9 ) normalOK = false;
		if( p.z < -1e-9 || p.z > 10.0 + 1e-9 ) zOK = false;
		const TexCoord& c = mesh->getCoords()[i];
		if( std::fabs( c.y - p.z / 10.0 ) > 1e-9 ) vOK = false;
	}
	Check( radiusOK, "profile_circle cylinder: all ring vertices exactly r=2 off the axis" );
	Check( normalOK, "profile_circle cylinder: all ring normals exactly radial" );
	Check( zOK, "profile_circle cylinder: z within the path extent" );
	Check( vOK, "profile_circle cylinder: v == path fraction" );
	pi->release();
}

// Mirrors ChunkParserRegistry.cpp's profile_rect ROUNDED-corner branch
// EXACTLY, including the C2 fix-round conditional dedup pass (Fix 3): at
// the stadium/capsule boundary (r == min(w,h)/2) or the fully-degenerate
// circle case (w == h == 2r), two adjacent corners' arc-generation formulas
// -- each using its OWN corner centre -- land on the exact same point, so a
// post-pass drops any profile vertex that is coincident with its
// predecessor (checked cyclically, so the wrap edge is covered too).  A
// blanket "always emit 4 points per corner" would also cut the genuinely
// distinct, still-finite straight edges on a partial-degenerate case like
// (4,2,1) (only the SHORT dimension's pair of transitions collapses), so
// this is conditional, not a fixed point-per-corner count.
static void BuildRoundedRectProfile( double w, double h, double r, std::vector<double>& prof )
{
	const double hw = w * 0.5, hh = h * 0.5;
	const double cx[4] = {  hw-r,  hw-r, -(hw-r), -(hw-r) };
	const double cy[4] = { -(hh-r), hh-r,  hh-r, -(hh-r) };
	const double startDeg[4] = { -90, 0, 90, 180 };
	prof.clear();
	prof.reserve( 4 * 5 * 2 );
	for( int c = 0; c < 4; ++c ) {
		for( int k = 0; k <= 4; ++k ) {
			const double a = ( startDeg[c] + 22.5 * k ) * 3.14159265358979323846 / 180.0;
			prof.push_back( cx[c] + r * std::cos( a ) );
			prof.push_back( cy[c] + r * std::sin( a ) );
		}
	}
	const double eps = 1e-9 * ( ( hw > hh ? hw : hh ) + r );
	std::vector<double> deduped;
	deduped.reserve( prof.size() );
	for( std::size_t k = 0; k < prof.size(); k += 2 ) {
		const double x = prof[k], y = prof[k+1];
		if( !deduped.empty() ) {
			const double px = deduped[ deduped.size()-2 ];
			const double py = deduped[ deduped.size()-1 ];
			if( std::fabs( x-px ) < eps && std::fabs( y-py ) < eps ) continue;
		}
		deduped.push_back( x );
		deduped.push_back( y );
	}
	if( deduped.size() >= 4 ) {
		const double x0 = deduped[0], y0 = deduped[1];
		const double xn = deduped[deduped.size()-2], yn = deduped[deduped.size()-1];
		if( std::fabs( x0-xn ) < eps && std::fabs( y0-yn ) < eps ) {
			deduped.pop_back(); deduped.pop_back();
		}
	}
	prof.swap( deduped );
}

// No two CONSECUTIVE points of a closed 2D polygon (cyclic, including the
// wrap edge) coincide, and its shoelace signed area is POSITIVE (CCW,
// matching the profile winding every profile_rect / profile_circle /
// hand-authored profile_point polygon in this codebase is documented to
// use for outward normals).  Used both directly on a profile array (proves
// the FORMULA) and on a parsed mesh ring's vertices (proves the round trip
// through RISE_API_CreateSweepGeometry didn't corrupt it).
static bool NoConsecutiveCoincidentAndPositiveArea( const std::vector<double>& xy, double eps )
{
	const std::size_t np = xy.size() / 2;
	if( np < 3 ) return false;
	for( std::size_t k = 0; k < np; ++k ) {
		const std::size_t kn = ( k + 1 ) % np;
		const double dx = xy[2*kn] - xy[2*k], dy = xy[2*kn+1] - xy[2*k+1];
		if( std::sqrt( dx*dx + dy*dy ) < eps ) return false;
	}
	double area2 = 0;
	for( std::size_t k = 0; k < np; ++k ) {
		const std::size_t kn = ( k + 1 ) % np;
		area2 += xy[2*k] * xy[2*kn+1] - xy[2*kn] * xy[2*k+1];
	}
	return area2 > 0;
}

static void TestProfileRectConvenience()
{
	std::cout << "Test 2d: profile_rect convenience formula -- sharp box shell + rounded-corner box" << std::endl;

	// (a) SHARP (r == 0): 4 profile vertices in BR -> TR -> TL -> BL CCW
	// order -- the exact order the profile_rect r==0 parser branch emits.
	{
		const double w = 3.0, h = 2.0;
		const double hw = w * 0.5, hh = h * 0.5;
		const double prof[] = { hw,-hh,  hw,hh,  -hw,hh,  -hw,-hh };
		const double pts[] = { 0,0,0,  0,0,5 };
		SweepDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 4;
		d.pathPoints = pts; d.numPathPoints = 2;
		d.nLen = 6;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "profile_rect sharp box factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			// segs=1, per=max(2,6/1)=6 -> nRings=7
			const unsigned int nRings = 7;
			Check( mesh && mesh->numPoints() == nRings * 4 + 2 * 4,
				"profile_rect sharp: vertex count = 4 profile verts per ring (+ 2 caps)" );
			bool extentOK = true;
			if( mesh ) for( unsigned int i = 0; i < nRings; ++i ) {
				Scalar hx, hy; RingExtent( mesh, i, 4, hx, hy );
				if( std::fabs( hx - hw ) > 1e-9 || std::fabs( hy - hh ) > 1e-9 ) extentOK = false;
			}
			Check( extentOK, "profile_rect sharp: every ring's half-extent == (w/2, h/2)" );
			pi->release();
		}
	}

	// (b) ROUNDED (r > 0): 4 corners x 5 points (4 arc intervals) = 20
	// profile vertices; every corner-arc vertex sits exactly radius r from
	// its own corner centre, and every ring vertex stays within the (w/2,
	// h/2) bounding box.  Reproduces the parser's rounded-corner formula
	// exactly (corner centres + start angles + 22.5-degree steps).
	{
		const double w = 4.0, h = 2.0, r = 0.6;
		const double hw = w * 0.5, hh = h * 0.5;
		const double cx[4] = {  hw-r,  hw-r, -(hw-r), -(hw-r) };
		const double cy[4] = { -(hh-r), hh-r,  hh-r, -(hh-r) };
		const double startDeg[4] = { -90, 0, 90, 180 };
		std::vector<double> prof;
		for( int c = 0; c < 4; ++c ) {
			for( int k = 0; k <= 4; ++k ) {
				const double a = ( startDeg[c] + 22.5 * k ) * 3.14159265358979323846 / 180.0;
				prof.push_back( cx[c] + r * std::cos( a ) );
				prof.push_back( cy[c] + r * std::sin( a ) );
			}
		}
		Check( prof.size() == 40, "profile_rect rounded: 4 corners x 5 points x 2 coords == 40 doubles (20 verts)" );

		const double pts[] = { 0,0,0,  0,0,4 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = (unsigned int)( prof.size() / 2 );
		d.pathPoints = pts; d.numPathPoints = 2;
		d.nLen = 4;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "profile_rect rounded box factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			const unsigned int NP = 20;
			// segs=1, per=max(2,4/1)=4 -> nRings=5
			const unsigned int nRings = 5;
			Check( mesh && mesh->numPoints() == nRings * NP + 2 * NP,
				"profile_rect rounded: vertex count = 20 profile verts per ring (+ 2 caps)" );
			bool boundOK = true;
			if( mesh ) for( unsigned int ring = 0; ring < nRings; ++ring ) {
				for( unsigned int k = 0; k < NP; ++k ) {
					const Vertex& p = mesh->getVertices()[ ring * NP + k ];
					if( std::fabs( p.x ) > hw + 1e-9 || std::fabs( p.y ) > hh + 1e-9 ) boundOK = false;
				}
			}
			Check( boundOK, "profile_rect rounded: every ring vertex within the (w/2, h/2) bounding box" );
			// C2 fix round, Fix 3: the old per-vertex "exactly radius r from a
			// REBUILT cx[]/cy[] formula" check was tautological -- cx[]/cy[]
			// here are computed with the exact same expression the test just
			// used to BUILD `prof[]` a few lines up, so a shared bug in both
			// places would sail through undetected.  Replace it with two
			// properties that hold from first principles for ANY valid simple
			// CCW polygon, computed straight off the mesh's own ring-0
			// vertices (world.x == profile x, world.y == -profile h for this
			// straight +Z path -- see the comment above): no two consecutive
			// ring vertices coincide, and the ring's shoelace area is positive.
			if( mesh ) {
				std::vector<double> ring0xy;
				ring0xy.reserve( NP * 2 );
				for( unsigned int k = 0; k < NP; ++k ) {
					const Vertex& p = mesh->getVertices()[ k ];
					ring0xy.push_back( p.x ); ring0xy.push_back( -p.y );
				}
				Check( NoConsecutiveCoincidentAndPositiveArea( ring0xy, 1e-9 ),
					"profile_rect rounded (non-boundary, r=0.6): no consecutive coincident ring vertices, positive signed area" );
			}
			pi->release();
		}
	}

	// (c) BOUNDARY cases (C2 fix round, Fix 3): r == min(w,h)/2 (stadium) or
	// the fully-degenerate w==h==2r (a full circle from 4 quarter arcs
	// sharing one centre) used to duplicate profile points at the collapsed
	// transition(s) -- shading crease + degenerate side triangles.  Checked
	// via BuildRoundedRectProfile (mirrors the parser's fixed formula
	// exactly, dedup included) both directly on the profile array AND on
	// the mesh produced by round-tripping it through
	// RISE_API_CreateSweepGeometry (a short 2-point path, ring 0 only).
	{
		struct BoundaryCase { double w, h, r; const char* label; };
		const BoundaryCase cases[] = {
			{ 4.0, 2.0, 1.0, "profile_rect 4 2 1 (partial-degenerate stadium: only the short-axis transitions collapse)" },
			{ 2.0, 2.0, 1.0, "profile_rect 2 2 1 (fully-degenerate: all four transitions collapse into one circle)" },
			{ 1.0, 1.0, 0.5, "profile_rect 1 1 0.5 (fully-degenerate: same shape, smaller scale)" },
		};
		for( std::size_t ci = 0; ci < sizeof(cases)/sizeof(cases[0]); ++ci ) {
			const BoundaryCase& bc = cases[ci];
			std::vector<double> prof;
			BuildRoundedRectProfile( bc.w, bc.h, bc.r, prof );
			std::string tag = std::string( "boundary " ) + bc.label + ": ";
			Check( NoConsecutiveCoincidentAndPositiveArea( prof, 1e-9 ),
				( tag + "the DEDUPED profile array itself has no consecutive coincident points and positive signed area" ).c_str() );

			const double pts[] = { 0,0,0,  0,0,4 };
			SweepDescriptor d;
			d.profilePoints = &prof[0]; d.numProfilePoints = (unsigned int)( prof.size() / 2 );
			d.pathPoints = pts; d.numPathPoints = 2;
			d.nLen = 4;
			d.capStart = false; d.capEnd = false;
			ITriangleMeshGeometryIndexed* pi = 0;
			Check( RISE_API_CreateSweepGeometry( &pi, d ), ( tag + "factory succeeds" ).c_str() );
			if( pi ) {
				const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
				const unsigned int NPb = (unsigned int)( prof.size() / 2 );
				if( mesh ) {
					std::vector<double> ring0xy;
					ring0xy.reserve( NPb * 2 );
					for( unsigned int k = 0; k < NPb; ++k ) {
						const Vertex& p = mesh->getVertices()[ k ];
						ring0xy.push_back( p.x ); ring0xy.push_back( -p.y );
					}
					Check( NoConsecutiveCoincidentAndPositiveArea( ring0xy, 1e-9 ),
						( tag + "MONEY ASSERTION: the PARSED (round-tripped) mesh ring has no consecutive "
						  "coincident vertices and positive signed area" ).c_str() );
				}
				pi->release();
			}
		}
	}
}

// path_closed TRUE: a circular 12-control-point path (the first point is NOT
// repeated -- path_closed wraps the last control point back to the first
// automatically) + a circular profile == a torus, checked with NO seam
// exclusion (unlike the open-path regression above, periodic Catmull-Rom
// sampling has no reflective-padding seam to skip), plus cyclic triangle
// count and PERIODIC SAMPLING seam continuity (ring n-1 sits immediately
// adjacent to ring 0, not doubled back or gapped).  This path is PLANAR
// (a circle in Z=0), so the rotation-minimizing frame's holonomy angle
// theta is identically zero here -- BuildPathFrames's holonomy-correction
// branch (RISE_API.cpp, the `if( closed && n > 1 )` block) never has
// anything to DO on this fixture, so this test does NOT exercise it.
// TestSweepClosedLoopTorsionalHolonomy below is the one that does (a
// genuinely non-planar path, so theta != 0 and the correction is load-
// bearing).
static void TestSweepClosedLoopRing()
{
	std::cout << "Test 3c: sweep_geometry path_closed -- circular path + circular profile == torus, no seam exclusion" << std::endl;
	std::vector<double> circ;
	const int NC = 12;
	for( int k = 0; k < NC; ++k ) {
		const double a = 2.0 * 3.14159265358979323846 * k / NC;
		circ.push_back( 10.0 * std::cos( a ) );
		circ.push_back( 10.0 * std::sin( a ) );
		circ.push_back( 0.0 );
	}
	std::vector<double> prof1;
	const int NP1 = 12;
	for( int k = 0; k < NP1; ++k ) {
		const double a = 2.0 * 3.14159265358979323846 * k / NP1;
		prof1.push_back( std::cos( a ) );
		prof1.push_back( std::sin( a ) );
	}
	SweepDescriptor d;
	d.profilePoints = &prof1[0]; d.numProfilePoints = NP1;
	d.pathPoints = &circ[0]; d.numPathPoints = NC;
	d.nLen = 120;
	d.pathClosed = true;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "closed-loop ring sweep factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "closed-loop ring concrete type" ); pi->release(); return; }

	// segs = numPathPoints = 12, per = max(2,120/12) = 10 -> n = 120 stations,
	// no trailing duplicate.
	const unsigned int n = mesh->numPoints() / (unsigned int)NP1;
	Check( n == 120, "closed-loop ring: station count == segs*per (no trailing duplicate station)" );
	Check( mesh->numPoints() == n * (unsigned int)NP1, "closed-loop ring: vertex count == n * profile verts (no caps)" );
	Check( mesh->getFaces().size() == 2u * (unsigned int)NP1 * n, "closed-loop ring: triangle count == 2*nProf*n (cyclic stitching, no caps)" );

	bool tubeOK = true;
	for( unsigned int ring = 0; ring < n; ++ring ) {
		for( unsigned int k = 0; k < (unsigned int)NP1; ++k ) {
			const Vertex& p = mesh->getVertices()[ ring * NP1 + k ];
			const Scalar ringR = std::sqrt( p.x*p.x + p.y*p.y );
			const Scalar dr = ringR - 10.0;
			const Scalar dist = std::sqrt( dr*dr + p.z*p.z );
			if( std::fabs( dist - 1.0 ) > 0.05 ) tubeOK = false;
		}
	}
	Check( tubeOK, "closed-loop ring: EVERY station (including the wrap) tracks the ideal torus -- no seam exclusion needed" );

	// seam continuity: corresponding profile vertex k of ring n-1 and ring 0
	// must be close -- within ~1.5x the typical adjacent-ring vertex
	// spacing.  On this PLANAR path theta == 0 (see the function comment
	// above), so this is a periodic-sampling-continuity check, NOT a
	// holonomy-correction check -- it would pass even with the correction
	// block deleted entirely.
	Scalar typicalAdjacent = 0;
	{
		const Vertex& a = mesh->getVertices()[ 0 * NP1 + 0 ];
		const Vertex& b = mesh->getVertices()[ 1 * NP1 + 0 ];
		typicalAdjacent = std::sqrt( (a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) + (a.z-b.z)*(a.z-b.z) );
	}
	bool seamOK = true;
	for( unsigned int k = 0; k < (unsigned int)NP1; ++k ) {
		const Vertex& p0 = mesh->getVertices()[ 0 * NP1 + k ];
		const Vertex& pN = mesh->getVertices()[ ( n - 1 ) * NP1 + k ];
		const Scalar jump = std::sqrt( (p0.x-pN.x)*(p0.x-pN.x) + (p0.y-pN.y)*(p0.y-pN.y) + (p0.z-pN.z)*(p0.z-pN.z) );
		if( jump > Scalar(1.5) * typicalAdjacent ) seamOK = false;
	}
	Check( seamOK, "closed-loop ring: periodic-sampling seam continuity -- ring n-1 and ring 0 are adjacent within ~1.5x typical spacing (planar path, theta==0, NOT a holonomy-correction check)" );
	pi->release();
}

// C2 fix round (2026-08-14), Fix 2: TestSweepClosedLoopRing's path is a
// PLANAR circle -- its rotation-minimizing frame's holonomy angle theta is
// IDENTICALLY ZERO there, so BuildPathFrames's holonomy-correction block
// (RISE_API.cpp, `if( closed && n > 1 ) { ... }`) never has anything to
// correct on that fixture; the "no holonomy twist" label on that test was
// therefore describing a check that would pass even with the correction
// deleted.  This is the test that actually exercises it: a genuinely
// non-planar, torsional closed path (a trefoil-knot-shaped control polygon)
// where theta != 0, so the correction is load-bearing.
//
// Ground truth without re-deriving BuildPathFrames' internal theta: for
// vertex k=0 of a circular profile (angle 0), the mesh vertex's OFFSET from
// its ring's centroid is exactly r*B[i] (the frame binormal at that
// station) -- so the signed rotation of that offset from ring i to ring
// i+1, projected into the plane perpendicular to the local edge tangent, IS
// the per-edge twist BuildPathFrames' correction distributes.  Post-
// correction, that twist is UNIFORM (~ -theta/n) across ALL n edges,
// INCLUDING the seam (n-1 -> 0) -- a sign-flipped or mis-scaled correction
// leaves the seam a lone outlier (the reviewer's own measurement on this
// class of path: interior edges ~-0.53 degrees, a flipped seam +105.6
// degrees), which is exactly what the seam-vs-median assertion below
// catches.
static void TestSweepClosedLoopTorsionalHolonomy()
{
	std::cout << "Test 3d: sweep_geometry path_closed -- trefoil path, torsional holonomy correction is uniform (no seam outlier)" << std::endl;
	const double PI = 3.14159265358979323846;
	std::vector<double> trefoil;
	const int NC = 12;
	for( int j = 0; j < NC; ++j ) {
		const double a = 2.0 * PI * j / NC;
		trefoil.push_back( std::sin(a) + 2.0 * std::sin(2.0*a) );
		trefoil.push_back( std::cos(a) - 2.0 * std::cos(2.0*a) );
		trefoil.push_back( -std::sin(3.0*a) );
	}
	std::vector<double> prof;
	const int NP = 12;
	const double r = 0.25;
	for( int k = 0; k < NP; ++k ) {
		const double a = 2.0 * PI * k / NP;
		prof.push_back( r * std::cos(a) );
		prof.push_back( r * std::sin(a) );
	}
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.pathPoints = &trefoil[0]; d.numPathPoints = NC;
	d.nLen = 120;
	d.pathClosed = true;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "trefoil closed-loop sweep factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "trefoil closed-loop concrete type" ); pi->release(); return; }

	const unsigned int n = mesh->numPoints() / (unsigned int)NP;
	Check( n == 120, "trefoil: station count == segs*per (no trailing duplicate station)" );

	// ring centroids (== path[i] up to FP noise, for a symmetric profile --
	// but computed from the MESH, not re-derived from the path formula) and
	// the k=0 vertex offset from its own ring's centroid (== r*B[i]).
	std::vector<Vertex> centroid( n );
	std::vector<Vertex> offset0( n );
	for( unsigned int i = 0; i < n; ++i ) {
		Scalar cx = 0, cy = 0, cz = 0;
		for( int k = 0; k < NP; ++k ) {
			const Vertex& p = mesh->getVertices()[ i * NP + k ];
			cx += p.x; cy += p.y; cz += p.z;
		}
		centroid[i] = Vertex( cx / NP, cy / NP, cz / NP );
		const Vertex& v0 = mesh->getVertices()[ i * NP + 0 ];
		offset0[i] = Vertex( v0.x - centroid[i].x, v0.y - centroid[i].y, v0.z - centroid[i].z );
	}

	auto sub3 = []( const Vertex& a, const Vertex& b ) { return Vertex( a.x-b.x, a.y-b.y, a.z-b.z ); };
	auto dot3 = []( const Vertex& a, const Vertex& b ) { return a.x*b.x + a.y*b.y + a.z*b.z; };
	auto cross3 = []( const Vertex& a, const Vertex& b ) {
		return Vertex( a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x );
	};
	auto len3 = [&]( const Vertex& a ) { return std::sqrt( dot3(a,a) ); };
	auto norm3 = [&]( const Vertex& a ) {
		const Scalar l = len3(a);
		return ( l > 0 ) ? Vertex( a.x/l, a.y/l, a.z/l ) : a;
	};

	// per-edge signed twist (radians) of the k=0 offset, about the local
	// edge tangent, for all n edges (including the seam n-1 -> 0).
	std::vector<Scalar> twist( n );
	for( unsigned int i = 0; i < n; ++i ) {
		const unsigned int i1 = ( i + 1 ) % n;
		const Vertex te = norm3( sub3( centroid[i1], centroid[i] ) );
		const Vertex oi  = sub3( offset0[i],  Vertex( te.x*dot3(offset0[i],te),  te.y*dot3(offset0[i],te),  te.z*dot3(offset0[i],te) ) );
		const Vertex oi1 = sub3( offset0[i1], Vertex( te.x*dot3(offset0[i1],te), te.y*dot3(offset0[i1],te), te.z*dot3(offset0[i1],te) ) );
		const Vertex c = cross3( oi, oi1 );
		twist[i] = std::atan2( dot3( c, te ), dot3( oi, oi1 ) );
	}

	std::vector<Scalar> sortedTwist = twist;
	std::sort( sortedTwist.begin(), sortedTwist.end() );
	const Scalar median = sortedTwist[ sortedTwist.size() / 2 ];

	// this path genuinely has torsion -- make sure the fixture is not
	// accidentally near-planar (which would make the whole test vacuous
	// the same way the circular-path test was).
	// C2 fix round (2026-08-14), Fix 4: raised from 1 degree to ~10 -- the
	// trefoil fixture's actual accumulated holonomy is ~60 degrees, so a 1
	// degree floor left enormous headroom for a regression (e.g. a
	// correction scaled down by 10x) to still clear the "not vacuous" gate.
	Check( std::fabs( median ) * n > ( 10.0 * PI / 180.0 ),
	       "trefoil: MONEY ASSERTION: the fixture has non-trivial accumulated holonomy (> 10 degrees total) -- this test is not vacuous" );

	bool uniformOK = true;
	const Scalar tinyEps = Scalar( 1e-3 );	// ~0.057 degrees, generous floor for a near-zero median
	for( unsigned int i = 0; i < n; ++i ) {
		if( std::fabs( twist[i] ) > 2.0 * std::fabs( median ) + tinyEps ) uniformOK = false;
	}
	Check( uniformOK, "trefoil: every edge's twist is within 2x the median (uniform correction across all stations)" );

	const Scalar seamTwist = twist[ n - 1 ];
	const Scalar seamDeviation = std::fabs( seamTwist - median );
	// C2 fix round (2026-08-14), Fix 4: the old additive `+ 1e-6` floor was
	// too tight to survive ordinary FP noise once the median twist itself is
	// small; a multiplicative-or-floor form (whichever is larger) is the
	// standard robust-epsilon shape here.
	const Scalar seamEpsilon = std::max( 3.0 * std::fabs( median ), Scalar( 3e-3 ) );
	Check( seamDeviation < seamEpsilon,
	       "trefoil: MONEY ASSERTION: the SEAM edge (n-1 -> 0) is NOT an outlier vs the median twist -- "
	       "a sign-flipped or mis-scaled holonomy correction leaves exactly this seam-only spike" );

	// seam-band triangle orientation consistency: for one seam quad (ring
	// n-1 -> ring 0) and one interior quad (ring 0 -> ring 1), check the
	// ACTUALLY EMITTED triangle's geometric face normal points OUTWARD
	// (positive dot with the ring-centroid -> vertex direction) -- a
	// flipped seam winding is a plausible sibling bug to a flipped twist
	// correction.
	//
	// C2 fix round (2026-08-14), Fix 3: read the face straight out of the
	// mesh's own STORED index buffer (mesh->getFaces(), the same
	// pVertices-pointer accessor the cylinder cap-winding check above
	// uses) instead of reconstructing (a,b,c) index arithmetic by hand and
	// indexing getVertices() with it -- a hand-reconstruction that assumes
	// the same formula RISE_API.cpp's side-quad loop uses would not catch
	// a real flip IN that loop (e.g. a swapped AddIndexedTriangle(a,b,c)
	// vs (a,c,b) argument order), since the reconstruction and the bug
	// would agree with each other while both disagreeing with a hand
	// count of what "outward" should mean.  RISE_API.cpp emits 2
	// triangles per profile edge, NP edges per ring band, ring i's band
	// first (k=0) at face index i*2*NP -- that IS the seam quad when
	// i == n-1 and the interior quad when i == 0.
	auto checkQuadOutward = [&]( unsigned int i, const char* label ) {
		const size_t faceIdx = size_t(i) * 2 * size_t(NP);
		const PointerPolygon_Template<3>& face = mesh->getFaces()[faceIdx];
		const Vertex& vA = *face.pVertices[0];
		const Vertex& vB = *face.pVertices[1];
		const Vertex& vC = *face.pVertices[2];
		const Vertex faceN = norm3( cross3( sub3( vB, vA ), sub3( vC, vA ) ) );
		const Vertex outDir = norm3( sub3( vA, centroid[i] ) );
		Check( dot3( faceN, outDir ) > 0, label );
	};
	checkQuadOutward( n - 1, "trefoil: seam-quad (ring n-1 -> ring 0) triangle winding faces outward" );
	checkQuadOutward( 0,     "trefoil: interior-quad (ring 0 -> ring 1) triangle winding faces outward" );

	pi->release();
}

// C2 fix round (2026-08-14), Fix 2b: TestSweepPerStationWidth below covers
// point_width lockstep on an OPEN path only -- the periodic 1D/3D sampler
// pairing (SampleCatmullRom1Periodic / SampleCatmullRom3Periodic) that
// path_closed actually uses had zero coverage.  A closed 8-point loop with
// per-control point_width AND point_scale, checked at every control
// station: periodic Catmull-Rom passes exactly through the authored
// control values at their own knots (same "1:1 station lockstep" property
// SampleCatmullRom1Periodic's header comment documents).
//
// Unlike TestSweepPerStationWidth's straight +Z path (where the frame
// trivially locks to world axes, B=(1,0,0)/N=(0,-1,0), so a world-space
// bounding-box "ring extent" reads off sx/sy directly), a closed LOOP's
// rotation-minimizing frame axes rotate station to station and do NOT
// generally align with world X/Y.  So this measures sx and sy the
// orientation-independent way instead: the profile is a CIRCLE, so the
// EUCLIDEAN DISTANCE from a ring's own centroid to its k=0 vertex (profile
// angle 0, a pure x-axis/binormal offset) is exactly profR*sx regardless of
// which way B happens to point, and the distance to the k=NP/4 vertex
// (profile angle 90 degrees, a pure h-axis/normal offset) is exactly
// profR*sy.
static void TestSweepClosedLoopPerStationWidthScale()
{
	std::cout << "Test 3e: sweep_geometry path_closed -- point_width x point_scale periodic lockstep" << std::endl;
	const double PI = 3.14159265358979323846;
	const int NC = 8;
	std::vector<double> loop;
	for( int j = 0; j < NC; ++j ) {
		const double a = 2.0 * PI * j / NC;
		loop.push_back( 10.0 * std::cos(a) );
		loop.push_back( 0.0 );
		loop.push_back( 10.0 * std::sin(a) );
	}
	const unsigned int NP = 16;	// divisible by 4, so k=NP/4 is exactly profile angle 90 degrees
	std::vector<double> prof;
	const double profR = 2.0;
	for( unsigned int k = 0; k < NP; ++k ) {
		const double a = 2.0 * PI * k / NP;
		prof.push_back( profR * std::cos(a) );
		prof.push_back( profR * std::sin(a) );
	}
	const double pw[NC] = { 1.0, 1.1, 1.2, 1.3, 1.4, 1.3, 1.2, 1.1 };
	const double ps[NC] = { 1.0, 0.9, 0.8, 0.7, 0.6, 0.7, 0.8, 0.9 };

	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.pathPoints = &loop[0]; d.numPathPoints = NC;
	d.nLen = 80;	// segs = NC = 8, per = max(2,80/8) = 10 -> n = 80; control point j at station j*10
	d.pathClosed = true;
	d.pointWidths = pw; d.numPointWidths = NC;
	d.pointScales = ps; d.numPointScales = NC;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "closed-loop point_width x point_scale factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !mesh ) { Check( false, "closed-loop width x scale concrete type" ); pi->release(); return; }

	const unsigned int n = mesh->numPoints() / NP;
	Check( n == 80, "closed-loop width x scale: station count == segs*per" );

	bool lockstepOK = true;
	for( int j = 0; j < NC; ++j ) {
		const unsigned int station = (unsigned int)( j * 10 );
		Scalar cx = 0, cy = 0, cz = 0;
		for( unsigned int k = 0; k < NP; ++k ) {
			const Vertex& p = mesh->getVertices()[ station * NP + k ];
			cx += p.x; cy += p.y; cz += p.z;
		}
		cx /= NP; cy /= NP; cz /= NP;
		const Vertex& v0  = mesh->getVertices()[ station * NP + 0 ];
		const Vertex& v90 = mesh->getVertices()[ station * NP + NP / 4 ];
		const Scalar distX = std::sqrt( (v0.x-cx)*(v0.x-cx) + (v0.y-cy)*(v0.y-cy) + (v0.z-cz)*(v0.z-cz) );
		const Scalar distY = std::sqrt( (v90.x-cx)*(v90.x-cx) + (v90.y-cy)*(v90.y-cy) + (v90.z-cz)*(v90.z-cz) );
		const Scalar expectX = profR * pw[j] * ps[j];
		const Scalar expectY = profR * ps[j];
		if( std::fabs( distX - expectX ) > 1e-9 ) lockstepOK = false;
		if( std::fabs( distY - expectY ) > 1e-9 ) lockstepOK = false;
	}
	Check( lockstepOK, "closed-loop width x scale: MONEY ASSERTION: centroid-to-vertex distance == "
	       "profR*point_width*point_scale (angle 0) and profR*point_scale (angle 90) EXACTLY at every "
	       "control station (periodic sampler lockstep, orientation-independent measurement)" );
	pi->release();
}

static void TestSweepPerStationWidth()
{
	std::cout << "Test 3b: sweep per-station width (point_width) first principles" << std::endl;
	const unsigned int NP = 16;
	std::vector<double> prof;
	for( unsigned int k = 0; k < NP; ++k ) {
		const double a = 2.0 * 3.14159265358979323846 * k / NP;
		prof.push_back( 2.0 * std::cos( a ) );   // x in [-2, 2]
		prof.push_back( 2.0 * std::sin( a ) );   // h in [-2, 2]
	}
	// 4 control points on +Z, nLen 12 -> segs 3, per 4, n = 13 stations;
	// control point j lands at station j*per = {0, 4, 8, 12}.
	const double pts[] = { 0,0,0,  0,0,4,  0,0,8,  0,0,12 };
	const unsigned int nRings = 13;
	const unsigned int cpStation[4] = { 0, 4, 8, 12 };

	// (a)+(b) LINEAR widths 0.6,0.8,1.0,1.2: uniform Catmull-Rom reproduces a
	// collinear control track EXACTLY, so widthMul at station i == 0.6+0.05*i
	// (per=4, +0.2 per segment) and halfX(i) == 2*(0.6+0.05*i); halfY stays 2.
	{
		const double pw[4] = { 0.6, 0.8, 1.0, 1.2 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = NP;
		d.pathPoints = pts; d.numPathPoints = 4;
		d.nLen = 12;
		d.capStart = false; d.capEnd = false;
		d.pointWidths = pw; d.numPointWidths = 4;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "linear point_width factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			Check( m && m->numPoints() == nRings * NP, "linear width: vertex count = rings (no caps)" );
			bool linOK = true, hOK = true;
			if( m ) for( unsigned int i = 0; i < nRings; ++i ) {
				Scalar hx, hy; RingExtent( m, i, NP, hx, hy );
				const Scalar expect = 2.0 * ( 0.6 + 0.05 * (double)i );
				if( std::fabs( hx - expect ) > 1e-9 ) linOK = false;
				if( std::fabs( hy - 2.0 ) > 1e-9 ) hOK = false;
			}
			Check( linOK, "linear width: halfX == 2*(0.6+0.05*station) at EVERY station (linear precision + composition)" );
			Check( hOK, "linear width: h (Y) axis untouched by width scaling" );
			pi->release();
		}
	}

	// NON-linear neck 0.72,0.9,1.0,1.0: EXACT at control stations (interpolation
	// passes through the authored widths); neck present; h untouched.
	{
		const double pw[4] = { 0.72, 0.9, 1.0, 1.0 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = NP;
		d.pathPoints = pts; d.numPathPoints = 4;
		d.nLen = 12;
		d.capStart = false; d.capEnd = false;
		d.pointWidths = pw; d.numPointWidths = 4;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "neck point_width factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			bool cpOK = true;
			Scalar hxCase = 0;
			if( m ) for( unsigned int j = 0; j < 4; ++j ) {
				Scalar hx, hy; RingExtent( m, cpStation[j], NP, hx, hy );
				if( std::fabs( hx - 2.0 * pw[j] ) > 1e-9 ) cpOK = false;
				if( std::fabs( hy - 2.0 ) > 1e-9 ) cpOK = false;
				if( j == 0 ) hxCase = hx;
			}
			Scalar hxFull = 0, dummy = 0;
			if( m ) RingExtent( m, 8, NP, hxFull, dummy );
			Check( cpOK, "neck: halfX exactly == 2*point_width at control stations; h untouched" );
			Check( hxCase < hxFull - 1e-6, "neck: case end (station 0) strictly narrower than full (station 8)" );
			pi->release();
		}
	}

	// (c) COMPOSITION with end_scale_x 0.5: at control station j (frac=j/3),
	// halfX == 2 * (1+(0.5-1)*frac) * point_width[j].
	{
		const double pw[4] = { 0.72, 0.9, 1.0, 1.0 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = NP;
		d.pathPoints = pts; d.numPathPoints = 4;
		d.nLen = 12;
		d.endScaleX = 0.5;
		d.capStart = false; d.capEnd = false;
		d.pointWidths = pw; d.numPointWidths = 4;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "width x end_scale_x factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			bool compOK = true;
			if( m ) for( unsigned int j = 0; j < 4; ++j ) {
				const double frac = (double)cpStation[j] / (double)( nRings - 1 );
				const double taper = 1.0 + ( 0.5 - 1.0 ) * frac;
				Scalar hx, hy; RingExtent( m, cpStation[j], NP, hx, hy );
				if( std::fabs( hx - 2.0 * taper * pw[j] ) > 1e-9 ) compOK = false;
			}
			Check( compOK, "composition: halfX == 2*end_scale_taper*point_width (multiplicative)" );
			pi->release();
		}
	}

	// (e) PADDING: 2 widths for 4 path points -> control stations 8,12 pad to 1.0.
	{
		const double pw[2] = { 0.7, 0.85 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = NP;
		d.pathPoints = pts; d.numPathPoints = 4;
		d.nLen = 12;
		d.capStart = false; d.capEnd = false;
		d.pointWidths = pw; d.numPointWidths = 2;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "padded point_width factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			Scalar hx2 = 0, hx3 = 0, hy = 0;
			if( m ) { RingExtent( m, 8, NP, hx2, hy ); RingExtent( m, 12, NP, hx3, hy ); }
			Check( m && std::fabs( hx2 - 2.0 ) < 1e-9 && std::fabs( hx3 - 2.0 ) < 1e-9,
				"padding: missing point_width entries pad to 1.0" );
			pi->release();
		}
	}

	// (f) DEFAULT-OFF: zero point widths == uniform full width (byte-identical).
	{
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = NP;
		d.pathPoints = pts; d.numPathPoints = 4;
		d.nLen = 12;
		d.capStart = false; d.capEnd = false;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateSweepGeometry( &pi, d ), "no point_width factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			bool offOK = true;
			if( m ) for( unsigned int i = 0; i < nRings; ++i ) {
				Scalar hx, hy; RingExtent( m, i, NP, hx, hy );
				if( std::fabs( hx - 2.0 ) > 1e-9 ) offOK = false;
			}
			Check( offOK, "default-off: zero point widths == uniform full width (no-op)" );
			pi->release();
		}
	}
}

int main( int, char** )
{
	std::cout << "ProceduralMeshTest -- procedural mesh factories vs Python baker goldens" << std::endl << std::endl;
	TestSweepCylinder();
	TestSweepDuplicatedProfilePoint();
	TestSweepTaperAndTorus();
	TestProfileCircleConvenience();
	TestProfileRectConvenience();
	TestSweepClosedLoopRing();
	TestSweepClosedLoopTorsionalHolonomy();
	TestSweepClosedLoopPerStationWidthScale();
	TestSweepPerStationWidth();
	TestPathInstances();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
