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
// arc-85 C3 (lathe_geometry): the winding convention is pinned by firing
// rays at the surface from OUTSIDE and asserting front-face hits, which
// needs the intersection record and the vector ops alongside the mesh
// accessors.
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
// round-2: the billow-fold diagnostic is a WARNING on a build that SUCCEEDS, so
// no return code separates "detected it" from "never looked" -- the log is the
// only place the feature is observable.
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"
#include <mutex>
#include <cmath>
#include <limits>

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
// Slice A (doc 89 sect. 2 -- loft: anisotropic point_scale + profile2
// morph) shared instruments.
//
// MeshDigest is the BACK-COMPAT instrument: an order-sensitive FNV-1a
// folding every count, every vertex position, every normal, every
// texcoord and the per-face vertex INDEX stream into one number, so any
// change to the sweep bake -- a reordered ring, a shifted UV, one extra
// cap triangle -- moves it.  Values are QUANTIZED before hashing
// (positions to 1e-6, normals/UVs to 1e-9) rather than hashed bit-exactly
// ON PURPOSE: the constants below are checked in as goldens that must
// hold on Linux and MSVC too, and a bit-exact fingerprint of double
// arithmetic would be a fingerprint of THIS Clang on THIS Mac instead
// (the OSX build pairs -ffast-math with -fno-finite-math-only; other
// toolchains reassociate differently).  Every regression this guard
// exists to catch moves geometry by many orders of magnitude more than
// one quantum.
//////////////////////////////////////////////////////////////////////

static void DigestMix( unsigned long long& h, unsigned long long v )
{
	h ^= v;
	h *= 1099511628211ULL;
}

static void DigestQ( unsigned long long& h, Scalar v, Scalar quantum )
{
	// llround of the quantized value; +0.0 and -0.0 fold to the same key.
	const long long q = (long long)std::llround( (double)v / (double)quantum );
	DigestMix( h, (unsigned long long)( q == 0 ? 0 : q ) );
}

static unsigned long long MeshDigest( const TriangleMeshGeometryIndexed* m )
{
	unsigned long long h = 14695981039346656037ULL;
	if( !m ) { return 0; }
	const unsigned int nv = m->numPoints();
	DigestMix( h, nv );
	DigestMix( h, (unsigned long long)m->getFaces().size() );
	DigestMix( h, (unsigned long long)m->getNormals().size() );
	DigestMix( h, (unsigned long long)m->getCoords().size() );
	for( unsigned int i = 0; i < nv; ++i ) {
		const Vertex& p = m->getVertices()[i];
		DigestQ( h, p.x, Scalar(1e-6) ); DigestQ( h, p.y, Scalar(1e-6) ); DigestQ( h, p.z, Scalar(1e-6) );
	}
	for( std::size_t i = 0; i < m->getNormals().size(); ++i ) {
		const Normal& n = m->getNormals()[i];
		DigestQ( h, n.x, Scalar(1e-9) ); DigestQ( h, n.y, Scalar(1e-9) ); DigestQ( h, n.z, Scalar(1e-9) );
	}
	for( std::size_t i = 0; i < m->getCoords().size(); ++i ) {
		const TexCoord& c = m->getCoords()[i];
		DigestQ( h, c.x, Scalar(1e-9) ); DigestQ( h, c.y, Scalar(1e-9) );
	}
	const Vertex* base = m->getVertices().empty() ? 0 : &m->getVertices()[0];
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		for( int k = 0; k < 3; ++k ) {
			DigestMix( h, (unsigned long long)( face.pVertices[k] - base ) );
		}
	}
	return h;
}

static bool CheckDigest( const TriangleMeshGeometryIndexed* m, unsigned long long want, const char* name )
{
	const unsigned long long got = MeshDigest( m );
	if( got == want ) { ++passCount; return true; }
	++failCount;
	std::cout << "  FAIL: " << name << "  digest got 0x" << std::hex << got
	          << " want 0x" << want << std::dec << std::endl;
	return false;
}

// CCW regular n-gon of radius r -- the exact expansion `profile_circle`
// performs, so a factory-level fixture matches a scene-level one.
static std::vector<double> NGon( double r, int n )
{
	std::vector<double> v;
	v.reserve( (std::size_t)n * 2 );
	for( int k = 0; k < n; ++k ) {
		const double a = 2.0 * 3.14159265358979323846 * k / n;
		v.push_back( r * std::cos( a ) );
		v.push_back( r * std::sin( a ) );
	}
	return v;
}

// CCW sharp rect -- the exact expansion `profile_rect <w> <h>` performs
// (corner order BR -> TR -> TL -> BL).
static std::vector<double> RectProfile( double w, double h )
{
	const double hw = w * 0.5, hh = h * 0.5;
	std::vector<double> v;
	v.push_back(  hw ); v.push_back( -hh );
	v.push_back(  hw ); v.push_back(  hh );
	v.push_back( -hw ); v.push_back(  hh );
	v.push_back( -hw ); v.push_back( -hh );
	return v;
}

// The sweep's STATION (ring) count for an OPEN path, reproducing
// SampleCatmullRom3's own arithmetic: `per = max(2, n_len / segs)` samples
// per segment, plus the final control point.
//
// Fixtures used to derive the ring count as numPoints() / numProfilePoints.
// That stopped being valid when the loft moved to a UNION resample: a
// morphed sweep's section carries every authored vertex of BOTH profiles, so
// its size is a function of the two profiles' arc-length parameter sets, not
// of either count alone.  Deriving the RINGS from the path instead lets a
// fixture recover the section size as numPoints() / rings -- and assert it.
static unsigned int SweepStationCount( int nLen, unsigned int nCtrl )
{
	const int segs = int(nCtrl) - 1;
	const int per = ( nLen / segs ) < 2 ? 2 : ( nLen / segs );
	return (unsigned int)( segs * per + 1 );
}

// Does SOME vertex of ring `ring` reproduce the authored profile point
// (x, h)?  Presence rather than index equality on purpose: the loft is free
// to rotate the section's index origin (the twist-free alignment) and to
// interleave the other profile's parameters, so "vertex k is the corner" is
// not a property the loft owes anyone -- "the corner is IN there, exactly"
// is.
static bool RingContainsLocalPoint( const TriangleMeshGeometryIndexed* m,
		unsigned int ring, unsigned int np, Scalar x, Scalar h, Scalar tol )
{
	for( unsigned int k = 0; k < np; ++k ) {
		const Vertex& p = m->getVertices()[ ring * np + k ];
		if( std::fabs( p.x - x ) <= tol && std::fabs( -p.y - h ) <= tol ) return true;
	}
	return false;
}

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

//////////////////////////////////////////////////////////////////////
// Slice A (doc 89 sect. 2): loft -- 2-arg point_scale (anisotropic
// per-station section scale) and profile2 + point_morph (a section whose
// OUTLINE changes character station to station).
//////////////////////////////////////////////////////////////////////

// The three BACK-COMPAT fixtures, shaped like the sweeps existing scenes
// author: 1-arg point_scale, no profile2.  Their digests were captured
// from the PRE-slice-A tree and are pasted below as constants; the slice
// must reproduce them byte-for-byte (modulo the documented quantum).
namespace BackCompat {

	// (1) a plain capped tube: circular profile, straight 3-point path.
	static bool BuildTube( ITriangleMeshGeometryIndexed** ppi )
	{
		static std::vector<double> prof = NGon( 1.0, 16 );
		static const double pts[] = { 0,0,0,  0,0,5,  0,0,10 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = 16;
		d.pathPoints = pts; d.numPathPoints = 3;
		d.nLen = 24;
		return RISE_API_CreateSweepGeometry( ppi, d );
	}

	// (2) every legacy per-station control at once: rect profile, curved
	// path, end_scale taper on BOTH axes, point_width AND 1-arg
	// point_scale, caps on.
	static bool BuildTaperedRail( ITriangleMeshGeometryIndexed** ppi )
	{
		static std::vector<double> prof = RectProfile( 2.0, 0.5 );
		static const double pts[] = { 0,0,0,  0.4,1.5,0.2,  1.2,2.8,0.9,  1.6,3.6,2.0 };
		static const double pw[]  = { 1.0, 0.8, 0.6, 0.5 };
		static const double ps[]  = { 1.0, 0.9, 0.7, 0.5 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = 4;
		d.pathPoints = pts; d.numPathPoints = 4;
		d.nLen = 20;
		d.endScaleX = 0.8; d.endScaleY = 1.2;
		d.pointWidths = pw; d.numPointWidths = 4;
		d.pointScales = ps; d.numPointScales = 4;
		return RISE_API_CreateSweepGeometry( ppi, d );
	}

	// (3) the closed-loop path (periodic sampler + holonomy correction),
	// with a periodic 1-arg point_scale track.
	static bool BuildClosedLoop( ITriangleMeshGeometryIndexed** ppi )
	{
		static std::vector<double> prof = NGon( 0.4, 12 );
		static const double pts[] = { 3,0,0,  0,0,3,  -3,0,0,  0,0,-3 };
		static const double ps[]  = { 1.0, 0.7, 1.3, 0.8 };
		SweepDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = 12;
		d.pathPoints = pts; d.numPathPoints = 4;
		d.nLen = 32;
		d.pathClosed = true;
		d.pointScales = ps; d.numPointScales = 4;
		return RISE_API_CreateSweepGeometry( ppi, d );
	}
}

// Test A0: BACK-COMPAT.  The three legacy-shaped sweeps must bake to the
// EXACT meshes the pre-slice-A tree produced.  Red-proof by construction:
// the constants were read off the unmodified tree before a line of the
// slice was written, so the assertion cannot have been fitted to the new
// code.
static void TestSweepBackCompatDigests()
{
	std::cout << "Test 3f: loft slice -- back-compat digests (legacy-shaped sweeps unchanged)" << std::endl;
	struct Row { bool (*build)( ITriangleMeshGeometryIndexed** ); unsigned long long digest; const char* what; };
	const Row rows[] = {
		{ &BackCompat::BuildTube,        0x65aa3707f1f5678dULL, "back-compat (1): capped circular tube on a straight path" },
		{ &BackCompat::BuildTaperedRail, 0x5026613b3178e99cULL, "back-compat (2): rect profile + end_scale + point_width + 1-arg point_scale" },
		{ &BackCompat::BuildClosedLoop,  0x50894075681f42ccULL, "back-compat (3): path_closed loop + periodic 1-arg point_scale" },
	};
	for( std::size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( rows[i].build( &pi ), "back-compat fixture bakes" );
		if( !pi ) continue;
		CheckDigest( dynamic_cast<TriangleMeshGeometryIndexed*>( pi ), rows[i].digest, rows[i].what );
		pi->release();
	}
}

// On a STRAIGHT +Z path with no frame hint, BuildPathFrames picks the world
// axis most perpendicular to the tangent, giving B = +X and N = B x T = -Y.
// So a ring vertex's profile-plane coordinates read straight off the world
// position: local x is world x, local h is MINUS world y.  Every fixture
// below that measures "local x / local y" uses a straight +Z path for
// exactly this reason -- it makes the assertion a statement about the baked
// vertex, not about a reconstruction of the frame.
static void LocalOfZPathVertex( const TriangleMeshGeometryIndexed* m,
		unsigned int ring, unsigned int np, unsigned int k, Scalar& lx, Scalar& lh )
{
	const Vertex& p = m->getVertices()[ ring * np + k ];
	lx =  p.x;
	lh = -p.y;
}

// Position-keyed watertightness: every DIRECTED edge must have exactly one
// reverse partner.  Keyed on quantized POSITIONS rather than on vertex
// indices because the sweep intentionally duplicates its cap ring (the caps
// carry a flat normal), so an index-keyed check would report the seam
// between the side wall and its own cap as a boundary.
static bool MeshIsWatertightByPosition( const TriangleMeshGeometryIndexed* m )
{
	struct Key { long long ax, ay, az, bx, by, bz; };
	std::vector<Key> edges;
	const Scalar q = Scalar(1e-7);
	auto Q = []( Scalar v, Scalar quantum ) { return (long long)std::llround( (double)v / (double)quantum ); };
	edges.reserve( m->getFaces().size() * 3 );
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		for( int e = 0; e < 3; ++e ) {
			const Point3& a = *face.pVertices[e];
			const Point3& b = *face.pVertices[ ( e + 1 ) % 3 ];
			Key k; k.ax = Q(a.x,q); k.ay = Q(a.y,q); k.az = Q(a.z,q);
			k.bx = Q(b.x,q); k.by = Q(b.y,q); k.bz = Q(b.z,q);
			edges.push_back( k );
		}
	}
	std::vector<bool> matched( edges.size(), false );
	for( std::size_t i = 0; i < edges.size(); ++i ) {
		if( matched[i] ) continue;
		bool found = false;
		for( std::size_t j = 0; j < edges.size() && !found; ++j ) {
			if( j == i || matched[j] ) continue;
			if( edges[j].ax == edges[i].bx && edges[j].ay == edges[i].by && edges[j].az == edges[i].bz &&
			    edges[j].bx == edges[i].ax && edges[j].by == edges[i].ay && edges[j].bz == edges[i].az ) {
				matched[i] = matched[j] = true;
				found = true;
			}
		}
		if( !found ) return false;
	}
	return true;
}

// Signed volume of the closed mesh (divergence theorem over the triangles).
static Scalar MeshSignedVolume( const TriangleMeshGeometryIndexed* m )
{
	Scalar v6 = 0;
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		const Point3& a = *face.pVertices[0];
		const Point3& b = *face.pVertices[1];
		const Point3& c = *face.pVertices[2];
		v6 += a.x * ( b.y*c.z - b.z*c.y ) - a.y * ( b.x*c.z - b.z*c.x ) + a.z * ( b.x*c.y - b.y*c.x );
	}
	return v6 / Scalar(6);
}

// Does any non-adjacent pair of the closed polygon's segments cross?  A
// morphing section must stay SIMPLE at every intermediate station or the
// mesh self-intersects.
static bool PolygonIsSimple( const std::vector<Scalar>& px, const std::vector<Scalar>& ph )
{
	const std::size_t n = px.size();
	auto Cross = []( Scalar ox, Scalar oy, Scalar ax, Scalar ay, Scalar bx, Scalar by ) {
		return ( ax - ox ) * ( by - oy ) - ( ay - oy ) * ( bx - ox );
	};
	for( std::size_t i = 0; i < n; ++i ) {
		const std::size_t i2 = ( i + 1 ) % n;
		for( std::size_t j = i + 1; j < n; ++j ) {
			const std::size_t j2 = ( j + 1 ) % n;
			if( i == j || i2 == j || j2 == i ) continue;	// share an endpoint
			const Scalar d1 = Cross( px[i], ph[i], px[i2], ph[i2], px[j],  ph[j]  );
			const Scalar d2 = Cross( px[i], ph[i], px[i2], ph[i2], px[j2], ph[j2] );
			const Scalar d3 = Cross( px[j], ph[j], px[j2], ph[j2], px[i],  ph[i]  );
			const Scalar d4 = Cross( px[j], ph[j], px[j2], ph[j2], px[i2], ph[i2] );
			if( ( ( d1 > 0 ) != ( d2 > 0 ) ) && ( ( d3 > 0 ) != ( d4 > 0 ) ) ) {
				return false;
			}
		}
	}
	return true;
}

// Test A1: the 2-arg `point_scale <sx> <sy>` form -- ANISOTROPIC per-station
// section scale.  The money assertion is on BAKED VERTEX POSITIONS in the
// profile frame (not on the bounding box, which a sheared or rotated ring
// would also satisfy): every ring vertex's local y must be exactly half its
// profile's h, while its local x is untouched.
static void TestSweepAnisotropicPointScale()
{
	std::cout << "Test 3g: loft slice -- 2-arg point_scale (anisotropic per-station section scale)" << std::endl;
	const unsigned int NP = 16;
	std::vector<double> prof = NGon( 1.0, NP );
	const double pts[] = { 0,0,0,  0,0,4,  0,0,8 };
	const double sxs[] = { 1.0, 1.0, 1.0 };
	const double sys[] = { 0.5, 0.5, 0.5 };
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.nLen = 8;
	d.pointScales  = sxs; d.numPointScales  = 3;
	d.pointScalesY = sys; d.numPointScalesY = 3;
	d.capStart = false; d.capEnd = false;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "anisotropic point_scale factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "anisotropic: concrete type" ); pi->release(); return; }

	const unsigned int nRings = m->numPoints() / NP;
	bool exactOK = true, halfOK = true;
	Scalar worstX = 0, worstH = 0;
	for( unsigned int i = 0; i < nRings; ++i ) {
		for( unsigned int k = 0; k < NP; ++k ) {
			Scalar lx = 0, lh = 0;
			LocalOfZPathVertex( m, i, NP, k, lx, lh );
			const Scalar wantX = (Scalar)prof[ 2*k ];
			const Scalar wantH = (Scalar)prof[ 2*k + 1 ] * Scalar(0.5);
			if( std::fabs( lx - wantX ) > worstX ) worstX = std::fabs( lx - wantX );
			if( std::fabs( lh - wantH ) > worstH ) worstH = std::fabs( lh - wantH );
			if( std::fabs( lx - wantX ) > 1e-12 ) exactOK = false;
			if( std::fabs( lh - wantH ) > 1e-12 ) halfOK = false;
		}
	}
	Check( exactOK, "anisotropic: MONEY ASSERTION -- every ring vertex's local x is the profile x UNSCALED (sx = 1)" );
	Check( halfOK,  "anisotropic: MONEY ASSERTION -- every ring vertex's local y is EXACTLY half the profile h (sy = 0.5)" );
	// and the section is genuinely an ellipse, not a rotated circle
	Scalar halfX = 0, halfY = 0;
	RingExtent( m, nRings / 2, NP, halfX, halfY );
	Check( std::fabs( halfX - 1.0 ) < 1e-12 && std::fabs( halfY - 0.5 ) < 1e-12,
	       "anisotropic: mid-station half-extents are (1.0, 0.5), a 2:1 flattened section" );
	pi->release();
}

// Test A2: MORPH IDENTITY -- the no-twist / no-drift guard.  A loft whose
// two profiles are the SAME shape at the SAME size must bake the EXACT mesh
// the plain sweep bakes, at every morph value.  This is what proves the
// resample (verbatim at equal counts), the start-vertex alignment (rotation
// 0 on a tie) and the delta formulation (identically zero) all behave.  The
// profile is deliberately IRREGULAR -- its vertices are NOT uniformly spaced
// in arc length -- so an accidental unconditional resample would show up.
static void TestSweepMorphIdentity()
{
	std::cout << "Test 3h: loft slice -- profile == profile2 bakes the plain sweep, bit for bit" << std::endl;
	const double prof[] = { 1.2,-0.3,  0.9,0.8,  -0.1,1.1,  -1.0,0.2,  -0.4,-0.9 };	// irregular CCW pentagon
	const double pts[]  = { 0,0,0,  0.3,1.1,0.4,  0.9,2.0,1.4,  1.1,2.9,2.6 };
	const double morphs[] = { 0.0, 0.37, 0.91, 1.0 };

	SweepDescriptor base;
	base.profilePoints = prof; base.numProfilePoints = 5;
	base.pathPoints = pts; base.numPathPoints = 4;
	base.nLen = 16;

	ITriangleMeshGeometryIndexed* plain = 0;
	Check( RISE_API_CreateSweepGeometry( &plain, base ), "identity: plain sweep bakes" );

	SweepDescriptor lofted = base;
	lofted.profile2Points = prof; lofted.numProfile2Points = 5;
	lofted.pointMorphs = morphs; lofted.numPointMorphs = 4;
	ITriangleMeshGeometryIndexed* lofty = 0;
	Check( RISE_API_CreateSweepGeometry( &lofty, lofted ), "identity: same-profile loft bakes" );

	if( plain && lofty ) {
		const TriangleMeshGeometryIndexed* a = dynamic_cast<TriangleMeshGeometryIndexed*>( plain );
		const TriangleMeshGeometryIndexed* b = dynamic_cast<TriangleMeshGeometryIndexed*>( lofty );
		Check( a && b && MeshDigest( a ) == MeshDigest( b ),
		       "identity: MONEY ASSERTION -- profile == profile2 with arbitrary point_morph values "
		       "bakes the IDENTICAL mesh to the plain sweep (no twist, no drift, no resample)" );
		// and the strictest form: every vertex bit-identical, not merely
		// within the digest's quantum
		if( a && b && a->numPoints() == b->numPoints() ) {
			bool bitOK = true;
			for( unsigned int i = 0; i < a->numPoints(); ++i ) {
				const Vertex& pa = a->getVertices()[i];
				const Vertex& pb = b->getVertices()[i];
				if( !( pa.x == pb.x && pa.y == pb.y && pa.z == pb.z ) ) { bitOK = false; break; }
			}
			Check( bitOK, "identity: every vertex is BIT-identical (the morph delta is exactly zero)" );
		} else {
			Check( false, "identity: vertex counts agree" );
		}
	}
	if( plain ) plain->release();
	if( lofty ) lofty->release();
}

// Test A3: MORPH ENDPOINTS + no-spiral.  t = 0 is the first profile, t = 1
// is the second CENTRED ON THE FIRST'S centroid (so an off-centre profile2
// changes the SHAPE without TRANSLATING the section), and a morph between
// two same-shaped profiles never spirals -- even when profile2 is authored
// starting from a different vertex.
static void TestSweepMorphEndpointsAndAlignment()
{
	std::cout << "Test 3i: loft slice -- morph endpoints, centroid anchoring, start-vertex alignment" << std::endl;
	const unsigned int NP = 12;
	std::vector<double> prof = NGon( 1.0, NP );
	// profile2: the SAME shape at half size, translated off-centre AND
	// listed starting from vertex 5 instead of vertex 0.  Both of those are
	// things the loft must absorb: the offset must not translate the
	// section, and the index shift must not spiral it.
	std::vector<double> ring2 = NGon( 0.5, NP );
	std::vector<double> prof2;
	for( unsigned int k = 0; k < NP; ++k ) {
		const unsigned int j = ( k + 5 ) % NP;
		prof2.push_back( ring2[ 2*j ]     + 0.30 );
		prof2.push_back( ring2[ 2*j + 1 ] - 0.20 );
	}
	const double pts[] = { 0,0,0,  0,0,4,  0,0,8 };
	const double morphs[] = { 0.0, 0.5, 1.0 };
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.profile2Points = &prof2[0]; d.numProfile2Points = NP;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.pointMorphs = morphs; d.numPointMorphs = 3;
	d.nLen = 8;
	d.capStart = false; d.capEnd = false;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "morph endpoints factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "morph endpoints: concrete type" ); pi->release(); return; }
	const unsigned int nRings = m->numPoints() / NP;

	// t = 0 -> exactly the first profile
	bool startOK = true;
	for( unsigned int k = 0; k < NP; ++k ) {
		Scalar lx = 0, lh = 0;
		LocalOfZPathVertex( m, 0, NP, k, lx, lh );
		if( std::fabs( lx - (Scalar)prof[2*k] ) > 1e-12 || std::fabs( lh - (Scalar)prof[2*k+1] ) > 1e-12 ) startOK = false;
	}
	Check( startOK, "morph endpoints: at t = 0 the section is EXACTLY the first profile" );

	// t = 1 -> the second profile RE-CENTRED on the first's centroid, i.e.
	// the radius-0.5 ring about the origin, with its authored (0.30, -0.20)
	// offset dropped and its index shift undone.
	bool endOK = true, endCentredOK = true;
	Scalar cx = 0, ch = 0;
	for( unsigned int k = 0; k < NP; ++k ) {
		Scalar lx = 0, lh = 0;
		LocalOfZPathVertex( m, nRings - 1, NP, k, lx, lh );
		cx += lx; ch += lh;
		if( std::fabs( lx - (Scalar)ring2[2*k] ) > 1e-9 || std::fabs( lh - (Scalar)ring2[2*k+1] ) > 1e-9 ) endOK = false;
	}
	cx /= Scalar(NP); ch /= Scalar(NP);
	if( std::fabs( cx ) > 1e-9 || std::fabs( ch ) > 1e-9 ) endCentredOK = false;
	Check( endOK, "morph endpoints: MONEY ASSERTION -- at t = 1 the section is the second profile with its "
	              "index origin realigned (no spiral) and its own offset dropped (no translation)" );
	Check( endCentredOK, "morph endpoints: the t = 1 section's centroid sits at the FIRST profile's centroid" );

	// no spiral at the INTERMEDIATE station either: every vertex must stay
	// on its own profile ray (the two profiles are concentric circles, so a
	// correct morph is a pure radial shrink -- any residual index rotation
	// would show as an angular offset).
	bool noSpiral = true;
	Scalar worstAngle = 0;
	for( unsigned int k = 0; k < NP; ++k ) {
		Scalar lx = 0, lh = 0;
		LocalOfZPathVertex( m, nRings / 2, NP, k, lx, lh );
		const Scalar rx = (Scalar)prof[2*k], rh = (Scalar)prof[2*k+1];
		const Scalar cross = rx * lh - rh * lx;
		const Scalar dot   = rx * lx + rh * lh;
		const Scalar ang = std::fabs( std::atan2( cross, dot ) );
		if( ang > worstAngle ) worstAngle = ang;
		if( ang > 1e-9 || dot <= 0 ) noSpiral = false;
	}
	Check( noSpiral, "morph endpoints: MONEY ASSERTION -- the mid-morph section does NOT spiral "
	                 "(every vertex stays on its own profile ray)" );
	pi->release();
}

// Test A4: a circle -> rect morph keeps every INTERMEDIATE section SIMPLE
// (no self-intersection), which is what makes a round-to-square furniture
// leg or a duct-to-vent transition renderable rather than a folded shell.
static void TestSweepMorphCircleToRectSimple()
{
	std::cout << "Test 3j: loft slice -- circle -> rect morph, every intermediate section stays simple" << std::endl;
	const unsigned int NP = 24;
	std::vector<double> prof = NGon( 1.0, NP );
	std::vector<double> prof2 = RectProfile( 1.4, 0.8 );
	const double pts[] = { 0,0,0,  0,0,3,  0,0,6 };
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.profile2Points = &prof2[0]; d.numProfile2Points = 4;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.nLen = 24;			// many stations => many intermediate t values
	d.capStart = false; d.capEnd = false;
	// no point_morph => the documented default linear 0 -> 1 ramp

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "circle->rect morph factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "circle->rect: concrete type" ); pi->release(); return; }
	// The UNION resample decides the section size, so derive the RINGS from
	// the path and read the section size back off the mesh.  The 24-gon's
	// parameters are k/24; the rect's four corners sit at 0, 0.8/4.4,
	// 0.5 and 3.0/4.4 of its perimeter, of which 0 and 0.5 land on the
	// 24-gon's grid -- so the union is 24 + 4 - 2 = 26 points, and both
	// profiles are in there whole.
	const unsigned int nRings = SweepStationCount( d.nLen, d.numPathPoints );
	Check( m->numPoints() % nRings == 0, "circle->rect: the vertex stream is a whole number of rings" );
	const unsigned int NS = m->numPoints() / nRings;
	Check( NS == 26, "circle->rect: the section is the UNION of both profiles' arc parameters (26 = 24 + 4 - 2 shared)" );
	Check( nRings > 8, "circle->rect: the fixture really does have many intermediate stations" );

	bool allSimple = true;
	unsigned int firstBad = 0;
	for( unsigned int i = 0; i < nRings && allSimple; ++i ) {
		std::vector<Scalar> sx( NS ), sh( NS );
		for( unsigned int k = 0; k < NS; ++k ) {
			LocalOfZPathVertex( m, i, NS, k, sx[k], sh[k] );
		}
		if( !PolygonIsSimple( sx, sh ) ) { allSimple = false; firstBad = i; }
	}
	if( !allSimple ) std::cout << "    first self-intersecting station: " << firstBad << std::endl;
	Check( allSimple, "circle->rect: MONEY ASSERTION -- NO station's section self-intersects across the whole morph" );

	// the last station really is the rect (its section's half-extents match)
	Scalar halfX = 0, halfY = 0;
	RingExtent( m, nRings - 1, NS, halfX, halfY );
	Check( std::fabs( halfX - 0.7 ) < 1e-9 && std::fabs( halfY - 0.4 ) < 1e-9,
	       "circle->rect: the final section's half-extents are the rect's (0.7, 0.4)" );

	// CORNER PRESENCE, both ends.  Half-extents are a WEAK witness: a rect
	// whose two off-grid corners were rounded off by a uniform arc-length
	// resample still reports the same bounding box (the surviving corners
	// pin it), which is exactly how the max(n1,n2) resample destroyed
	// hand-authored profiles unnoticed.  Assert instead that every AUTHORED
	// vertex of each profile is reproduced, to 1e-12, in the ring where that
	// profile is the section.
	{
		bool startVertsOK = true;
		for( unsigned int k = 0; k < NP; ++k ) {
			if( !RingContainsLocalPoint( m, 0, NS, (Scalar)prof[2*k], (Scalar)prof[2*k+1], Scalar(1e-12) ) ) {
				startVertsOK = false;
			}
		}
		Check( startVertsOK, "circle->rect: MONEY ASSERTION -- every one of the 24 authored circle vertices is "
		                     "reproduced EXACTLY in the t = 0 ring" );
		bool endCornersOK = true;
		unsigned int missing = 0;
		for( unsigned int k = 0; k < 4; ++k ) {
			if( !RingContainsLocalPoint( m, nRings - 1, NS, (Scalar)prof2[2*k], (Scalar)prof2[2*k+1], Scalar(1e-9) ) ) {
				endCornersOK = false; ++missing;
			}
		}
		if( !endCornersOK ) std::cout << "    authored rect corners MISSING from the t = 1 ring: " << missing << " of 4" << std::endl;
		Check( endCornersOK, "circle->rect: MONEY ASSERTION -- all FOUR authored rect corners are present in the "
		                     "t = 1 ring (a uniform max(n1,n2) resample keeps only the two that land on the grid)" );
	}
	pi->release();
}

// Test A4b: the REGRESSION FIXTURE for the unequal-count resample.  A
// hand-authored `profile_rect 2 2` (4 points) against a near-degenerate
// `profile2_circle 0.001 30` (30 points) with an ALL-ZERO morph track: every
// station's section is t = 0, i.e. the authored rect and nothing else.
//
// Under the old max(n1, n2) rule this baked N = 30 uniformly arc-length
// resampled samples of the rect, and only the corners whose arc positions
// (2.0 and 6.0 of perimeter 8) landed on the j/30 grid survived -- the other
// two were replaced by points partway along an edge, rounding the square off
// at EVERY station and on both caps.  A renderer's-eye version of this same
// fixture put 311 pixels' worth of difference (max dL 253/255) between it and
// the plain-rect control.
static void TestSweepMorphUnionKeepsAuthoredCorners()
{
	std::cout << "Test 3n: loft slice -- the union resample keeps EVERY authored vertex of BOTH profiles" << std::endl;
	std::vector<double> rect = RectProfile( 2.0, 2.0 );
	std::vector<double> tiny = NGon( 0.001, 30 );
	const double pts[] = { 0,0,0,  0,0,3,  0,0,6 };
	const double zeros[] = { 0.0, 0.0, 0.0 };
	SweepDescriptor d;
	d.profilePoints = &rect[0]; d.numProfilePoints = 4;
	d.profile2Points = &tiny[0]; d.numProfile2Points = 30;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.pointMorphs = zeros; d.numPointMorphs = 3;
	d.nLen = 12;
	d.capStart = false; d.capEnd = false;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "union corners: factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "union corners: concrete type" ); pi->release(); return; }
	const unsigned int nRings = SweepStationCount( d.nLen, d.numPathPoints );
	Check( m->numPoints() % nRings == 0, "union corners: the vertex stream is a whole number of rings" );
	const unsigned int NS = m->numPoints() / nRings;

	// Every AUTHORED rect corner, exactly, in the t = 0 ring.
	bool cornersOK = true;
	unsigned int found = 0;
	for( unsigned int k = 0; k < 4; ++k ) {
		if( RingContainsLocalPoint( m, 0, NS, (Scalar)rect[2*k], (Scalar)rect[2*k+1], Scalar(1e-12) ) ) ++found;
		else cornersOK = false;
	}
	std::cout << "    authored rect corners reproduced in the t = 0 ring: " << found << " of 4"
	          << " (section size " << NS << ")" << std::endl;
	Check( cornersOK, "union corners: MONEY ASSERTION -- all FOUR authored rect corners are reproduced EXACTLY "
	                  "in the t = 0 ring (max(n1,n2) uniform resampling keeps only 2)" );

	// ...and the morph being identically zero means EVERY station is that
	// same authored rect, not just the first.
	bool allStationsOK = true;
	for( unsigned int i = 0; i < nRings; ++i ) {
		for( unsigned int k = 0; k < 4; ++k ) {
			if( !RingContainsLocalPoint( m, i, NS, (Scalar)rect[2*k], (Scalar)rect[2*k+1], Scalar(1e-12) ) ) allStationsOK = false;
		}
	}
	Check( allStationsOK, "union corners: every station reproduces the authored corners (an all-zero morph "
	                      "track is a no-op on the SHAPE, at every station and both caps)" );
	pi->release();
}

// Test A4c: MORPH IDENTITY through the union path, at UNEQUAL counts.  The
// same shape authored with 5 points and with 6 (one extra vertex sitting on
// an edge midpoint) must still morph to NOTHING: the section is constant
// along the path.  Test 3h pins the equal-count case bit-for-bit; this one
// pins that the property is a property of the RESAMPLE, not of an
// equal-count shortcut around it.
static void TestSweepMorphIdentityUnequalCounts()
{
	std::cout << "Test 3o: loft slice -- same shape at UNEQUAL counts still morphs to nothing" << std::endl;
	// irregular CCW pentagon, and the SAME pentagon with the midpoint of its
	// first edge listed explicitly
	const double prof[]  = { 1.2,-0.3,  0.9,0.8,  -0.1,1.1,  -1.0,0.2,  -0.4,-0.9 };
	const double prof2[] = { 1.2,-0.3,  1.05,0.25,  0.9,0.8,  -0.1,1.1,  -1.0,0.2,  -0.4,-0.9 };
	const double pts[] = { 0,0,0,  0,0,3,  0,0,6 };
	SweepDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 5;
	d.profile2Points = prof2; d.numProfile2Points = 6;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.nLen = 12;
	d.capStart = false; d.capEnd = false;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "unequal identity: factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "unequal identity: concrete type" ); pi->release(); return; }
	const unsigned int nRings = SweepStationCount( d.nLen, d.numPathPoints );
	const unsigned int NS = m->numPoints() / nRings;
	Check( NS == 6, "unequal identity: the union is 6 points (the 5 pentagon vertices plus the extra midpoint)" );

	// the default 0 -> 1 ramp is running, so if the delta were not zero the
	// last station would differ from the first
	bool constantOK = true;
	Scalar worst = 0;
	for( unsigned int i = 0; i < nRings; ++i ) {
		for( unsigned int k = 0; k < NS; ++k ) {
			Scalar lx = 0, lh = 0, l0x = 0, l0h = 0;
			LocalOfZPathVertex( m, i, NS, k, lx, lh );
			LocalOfZPathVertex( m, 0, NS, k, l0x, l0h );
			worst = std::max( worst, std::max( std::fabs( lx - l0x ), std::fabs( lh - l0h ) ) );
			if( std::fabs( lx - l0x ) > 1e-12 || std::fabs( lh - l0h ) > 1e-12 ) constantOK = false;
		}
	}
	std::cout << "    worst station-to-station section drift: " << worst << std::endl;
	Check( constantOK, "unequal identity: MONEY ASSERTION -- the section is CONSTANT along the path "
	                   "(the same shape at unequal counts has a zero morph delta through the union path)" );

	// and every AUTHORED pentagon vertex is still in there, exactly
	bool vertsOK = true;
	for( unsigned int k = 0; k < 5; ++k ) {
		if( !RingContainsLocalPoint( m, 0, NS, (Scalar)prof[2*k], (Scalar)prof[2*k+1], Scalar(1e-12) ) ) vertsOK = false;
	}
	Check( vertsOK, "unequal identity: every authored pentagon vertex is reproduced exactly" );
	pi->release();
}

// Test A4d: the documented duplicate-a-point HARD-EDGE idiom survives the
// union resample.  A zero-length profile segment has TWO vertices at the
// same arc-length parameter; a parameter-keyed union that deduplicated by
// VALUE would silently drop one of them and take the hard edge with it.
static void TestSweepMorphUnionKeepsDuplicatePoint()
{
	std::cout << "Test 3p: loft slice -- a duplicated profile point (hard edge) survives the union resample" << std::endl;
	// square with a duplicated corner -- the idiom Test 2b pins for the
	// no-morph path
	const double prof[] = { -1,-1,  1,-1,  1,1,  1,1,  -1,1 };
	std::vector<double> circ = NGon( 1.2, 9 );
	const double pts[] = { 0,0,0,  0,0,3,  0,0,6 };
	const double zeros[] = { 0.0, 0.0, 0.0 };
	SweepDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 5;
	d.profile2Points = &circ[0]; d.numProfile2Points = 9;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.pointMorphs = zeros; d.numPointMorphs = 3;
	d.nLen = 8;
	d.capStart = true; d.capEnd = true;		// the caps ear-clip the same section

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "hard edge under morph: factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "hard edge under morph: concrete type" ); pi->release(); return; }
	const unsigned int nRings = SweepStationCount( d.nLen, d.numPathPoints );
	// numPoints = nRings*NS + 2*NS (the two caps duplicate their ring)
	Check( m->numPoints() % ( nRings + 2 ) == 0, "hard edge under morph: vertex stream is rings + two cap rings" );
	const unsigned int NSec = m->numPoints() / ( nRings + 2 );

	// every AUTHORED square vertex present, INCLUDING both copies of the
	// duplicated corner: count coincident-with-(1,1) ring vertices
	unsigned int dupCount = 0;
	for( unsigned int k = 0; k < NSec; ++k ) {
		Scalar lx = 0, lh = 0;
		LocalOfZPathVertex( m, 0, NSec, k, lx, lh );
		if( std::fabs( lx - 1.0 ) < 1e-12 && std::fabs( lh - 1.0 ) < 1e-12 ) ++dupCount;
	}
	std::cout << "    copies of the duplicated corner (1, 1) in the t = 0 ring: " << dupCount << std::endl;
	Check( dupCount == 2, "hard edge under morph: MONEY ASSERTION -- BOTH copies of the duplicated corner "
	                      "survive the union (a value-keyed dedup would collapse them and lose the hard edge)" );
	bool vertsOK = true;
	for( unsigned int k = 0; k < 5; ++k ) {
		if( !RingContainsLocalPoint( m, 0, NSec, (Scalar)prof[2*k], (Scalar)prof[2*k+1], Scalar(1e-12) ) ) vertsOK = false;
	}
	Check( vertsOK, "hard edge under morph: every authored square vertex is reproduced exactly" );
	Check( m->getFaces().size() > 0, "hard edge under morph: the caps still ear-clip (zero-length edge tolerated)" );
	pi->release();
}

// Test A5: anisotropy composes with the morph, and the scale is applied
// AFTER it.  With a constant morph t = 0.5 between concentric circles of
// radius 1 and 0.5 the section is the radius-0.75 circle; a per-station
// (sx, sy) = (1.0, 0.5) then makes it a 0.75 x 0.375 ellipse.  Reversing the
// order (scale the profiles, then morph) would give the same answer here
// ONLY because both operations are linear about the centroid -- so the
// fixture also checks the ASYMMETRIC case where the two profiles differ in
// shape, where the two orders genuinely disagree.
static void TestSweepMorphComposesWithAnisotropy()
{
	std::cout << "Test 3k: loft slice -- anisotropic point_scale composes with the morph (scale applied post-morph)" << std::endl;
	const unsigned int NP = 12;
	std::vector<double> prof  = NGon( 1.0, NP );
	std::vector<double> ring2 = NGon( 0.5, NP );
	const double pts[]    = { 0,0,0,  0,0,4,  0,0,8 };
	const double morphs[] = { 0.5, 0.5, 0.5 };
	const double sxs[]    = { 1.0, 1.0, 1.0 };
	const double sys[]    = { 0.5, 0.5, 0.5 };
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.profile2Points = &ring2[0]; d.numProfile2Points = NP;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.pointMorphs = morphs; d.numPointMorphs = 3;
	d.pointScales = sxs; d.numPointScales = 3;
	d.pointScalesY = sys; d.numPointScalesY = 3;
	d.nLen = 8;
	d.capStart = false; d.capEnd = false;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "morph x anisotropy factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "morph x anisotropy: concrete type" ); pi->release(); return; }
	const unsigned int nRings = m->numPoints() / NP;

	bool composeOK = true;
	Scalar worst = 0;
	for( unsigned int i = 0; i < nRings; ++i ) {
		for( unsigned int k = 0; k < NP; ++k ) {
			Scalar lx = 0, lh = 0;
			LocalOfZPathVertex( m, i, NP, k, lx, lh );
			// morph(0.5) of r=1 -> r=0.5 is r=0.75, THEN diag(1.0, 0.5)
			const Scalar wantX = (Scalar)prof[2*k]   * Scalar(0.75) * Scalar(1.0);
			const Scalar wantH = (Scalar)prof[2*k+1] * Scalar(0.75) * Scalar(0.5);
			worst = std::max( worst, std::max( std::fabs(lx-wantX), std::fabs(lh-wantH) ) );
			if( std::fabs( lx - wantX ) > 1e-12 || std::fabs( lh - wantH ) > 1e-12 ) composeOK = false;
		}
	}
	Check( composeOK, "morph x anisotropy: MONEY ASSERTION -- section == morph(t) THEN diag(sx, sy), "
	                  "matching the documented composition order" );

	// The asymmetric case: circle -> rect at t = 0.5 with sy = 0.5.  Scale
	// AFTER the morph flattens the blended section; scaling the two profiles
	// FIRST and blending afterwards would give a different intermediate
	// outline, so the half-extent pair below distinguishes the two orders.
	{
		std::vector<double> rectp = RectProfile( 1.4, 0.8 );
		SweepDescriptor e = d;
		e.profile2Points = &rectp[0]; e.numProfile2Points = 4;
		ITriangleMeshGeometryIndexed* pe = 0;
		Check( RISE_API_CreateSweepGeometry( &pe, e ), "asymmetric morph x anisotropy factory succeeds" );
		if( pe ) {
			const TriangleMeshGeometryIndexed* me = dynamic_cast<TriangleMeshGeometryIndexed*>( pe );
			if( me ) {
				Scalar halfX = 0, halfY = 0;
				// circle-vs-rect: the section is the UNION of the two
				// profiles' arc parameters, so read its size off the mesh
				// (the rings come from the path) rather than assuming NP.
				const unsigned int eRings = SweepStationCount( e.nLen, e.numPathPoints );
				const unsigned int eNS = me->numPoints() / eRings;
				RingExtent( me, eRings / 2, eNS, halfX, halfY );
				// half-x: blend of circle 1.0 and rect 0.7 at t=0.5 == 0.85, times sx=1.0
				// half-y: blend of circle 1.0 and rect 0.4 at t=0.5 == 0.70, times sy=0.5 == 0.35
				Check( std::fabs( halfX - 0.85 ) < 1e-9 && std::fabs( halfY - 0.35 ) < 1e-9,
				       "asymmetric morph x anisotropy: half-extents are (0.85, 0.35) -- the blend sized AFTER morphing" );
			}
			pe->release();
		}
	}
	pi->release();
}

// Test A6: a MORPHED sweep's end caps close the section that is actually
// there at each end -- so the solid is watertight and positively oriented
// even though the two caps are different polygons.
static void TestSweepMorphCapsWatertight()
{
	std::cout << "Test 3l: loft slice -- morphed end caps are watertight and correctly oriented" << std::endl;
	const unsigned int NP = 16;
	std::vector<double> prof  = NGon( 1.0, NP );
	std::vector<double> prof2 = RectProfile( 1.4, 0.7 );
	const double pts[] = { 0,0,0,  0,0,3,  0,0,6 };
	SweepDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = NP;
	d.profile2Points = &prof2[0]; d.numProfile2Points = 4;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.nLen = 8;
	d.capStart = true; d.capEnd = true;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "morphed caps factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "morphed caps: concrete type" ); pi->release(); return; }

	Check( MeshIsWatertightByPosition( m ),
	       "morphed caps: MONEY ASSERTION -- every directed edge has its reverse partner (closed, no boundary edge)" );
	const Scalar vol = MeshSignedVolume( m );
	Check( vol > 0, "morphed caps: signed volume is POSITIVE (outward winding survives the morph)" );
	// sanity on magnitude: the solid runs 6 long between a unit-ish circle
	// (area ~pi) and a 1.4 x 0.7 rect (area 0.98), so its volume must sit
	// well inside those two extruded bounds.
	Check( vol > 0.98 * 6.0 * 0.5 && vol < 3.15 * 6.0,
	       "morphed caps: volume lies between the two profiles' own extrusions" );

	// the same fixture WITHOUT the morph must also be watertight -- proving
	// the probe is measuring the morph, not a pre-existing hole
	SweepDescriptor plain = d;
	plain.profile2Points = 0; plain.numProfile2Points = 0;
	ITriangleMeshGeometryIndexed* pp = 0;
	if( RISE_API_CreateSweepGeometry( &pp, plain ) ) {
		const TriangleMeshGeometryIndexed* mp = dynamic_cast<TriangleMeshGeometryIndexed*>( pp );
		Check( mp && MeshIsWatertightByPosition( mp ), "morphed caps: control -- the UNMORPHED twin is watertight too" );
		pp->release();
	} else {
		Check( false, "morphed caps: control fixture bakes" );
	}
	pi->release();
}

// Test A6b: the CAP's winding comes from the CAP polygon's OWN shoelace, not
// from the first profile's.
//
// The morphed section's signed area is the quadratic
//     A(t) = (1-t)^2*A1 + 2t(1-t)*Amix + t^2*A2,
// so a sufficiently anti-aligned correspondence (a strongly negative mixed
// term) carries it through zero and out the other side even though BOTH
// authored profiles are CCW.  The two triangles below are exactly that case
// -- found by search over simple star-shaped polygons -- and at t = 0.4 the
// section is a SIMPLE polygon wound CLOCKWISE.  Taking the flip sign from
// the first profile's `outward` emitted both caps inside out there while the
// side wall stayed correct; taking it from the polygon actually being
// triangulated cannot.
static void TestSweepMorphCapWindingFollowsSection()
{
	std::cout << "Test 3q: loft slice -- a section whose winding FLIPS mid-morph still caps outward" << std::endl;
	const double prof[]  = { -0.772186, 0.469352,  -0.134439, -0.492234,  -0.068175, -0.385729 };
	const double prof2[] = {  0.167965, 0.372948,   0.119310,  0.678440,  -0.056223, -0.805472 };
	const double pts[] = { 0,0,0,  0,0,2,  0,0,4 };
	// a CONSTANT morph track parks every station -- and therefore both caps
	// -- at the t where the section is clockwise
	const double morphs[] = { 0.4, 0.4, 0.4 };
	SweepDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 3;
	d.profile2Points = prof2; d.numProfile2Points = 3;
	d.pathPoints = pts; d.numPathPoints = 3;
	d.pointMorphs = morphs; d.numPointMorphs = 3;
	d.nLen = 6;
	d.capStart = true; d.capEnd = true;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateSweepGeometry( &pi, d ), "cap winding flip: factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "cap winding flip: concrete type" ); pi->release(); return; }
	const unsigned int nRings = SweepStationCount( d.nLen, d.numPathPoints );
	const unsigned int NS = m->numPoints() / ( nRings + 2 );

	// FIXTURE PRECONDITION: the section really is wound the other way from
	// the first profile.  Without this the test would pass vacuously if the
	// search result ever stopped reproducing.
	Scalar secArea2 = 0;
	for( unsigned int k = 0; k < NS; ++k ) {
		Scalar ax = 0, ah = 0, bx = 0, bh = 0;
		LocalOfZPathVertex( m, 0, NS, k, ax, ah );
		LocalOfZPathVertex( m, 0, NS, ( k + 1 ) % NS, bx, bh );
		secArea2 += ax * bh - bx * ah;
	}
	Scalar profArea2 = 0;
	for( unsigned int k = 0; k < 3; ++k ) {
		const unsigned int j = ( k + 1 ) % 3;
		profArea2 += (Scalar)prof[2*k] * (Scalar)prof[2*j+1] - (Scalar)prof[2*j] * (Scalar)prof[2*k+1];
	}
	std::cout << "    profile signed area2 " << profArea2 << ", morphed section signed area2 " << secArea2 << std::endl;
	Check( profArea2 > 0 && secArea2 < 0,
	       "cap winding flip: FIXTURE PRECONDITION -- both profiles are CCW but the morphed section is CW" );

	// MONEY: every cap face's geometric normal agrees with its own cap
	// normal.  The path runs along +Z, so the start cap faces -Z and the end
	// cap faces +Z; cap faces are the LAST 2*(NS-2) in the stream.
	const std::size_t sideFaces = (std::size_t)( nRings - 1 ) * NS * 2;
	bool capWindOK = true;
	unsigned int badCaps = 0;
	for( std::size_t f = sideFaces; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		const Point3& a = *face.pVertices[0];
		const Point3& b = *face.pVertices[1];
		const Point3& c = *face.pVertices[2];
		const Scalar gz = ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x );
		const Scalar capZ = ( a.z < 2.0 ) ? Scalar(-1) : Scalar(1);
		if( gz * capZ <= 0 ) { capWindOK = false; ++badCaps; }
	}
	if( !capWindOK ) std::cout << "    inside-out cap triangles: " << badCaps << std::endl;
	Check( capWindOK, "cap winding flip: MONEY ASSERTION -- every cap triangle's GEOMETRIC normal agrees with its "
	                  "cap normal (the flip sign comes from the CAP polygon, not from profile 1)" );

	// NOT asserted here, deliberately: watertight-by-orientation.  A section
	// that inverts mid-morph passes through ZERO area on the way, so the
	// solid genuinely self-intersects and has no globally consistent
	// orientation -- and the SIDE wall's winding is chosen ONCE for the whole
	// mesh from profile 1, so wherever the section is CW the side wall faces
	// inward and cannot pair its edges with a correctly-oriented cap.  What
	// IS achievable, and is what the cap normal means, is that the cap's
	// GEOMETRIC winding agrees with the flat normal (+/-T) it is actually
	// given -- which the assertion above pins, and which the pre-fix code
	// (taking the sign from profile 1) got wrong here.  Test 3l pins
	// watertightness for the ordinary, non-inverting morph.
	pi->release();
}

// Test A7: degenerate / contradictory loft inputs REFUSE (and leave the out
// pointer null), and the documented CLAMP behaves as documented.
static void TestSweepLoftValidation()
{
	std::cout << "Test 3m: loft slice -- degenerate inputs refuse; the morph overshoot clamps" << std::endl;
	const unsigned int NP = 12;
	std::vector<double> prof = NGon( 1.0, NP );
	const double pts4[] = { 0,0,0,  0,0,2,  0,0,4,  0,0,6 };
	const double pts3[] = { 0,0,0,  0,0,4,  0,0,8 };
	const double loop[] = { 3,0,0,  0,0,3,  -3,0,0,  0,0,-3 };

	SweepDescriptor base;
	base.profilePoints = &prof[0]; base.numProfilePoints = NP;
	base.pathPoints = pts3; base.numPathPoints = 3;
	base.nLen = 8;

	ITriangleMeshGeometryIndexed* pi = 0;
	// (a) profile2 with fewer than 3 points
	{
		const double two[] = { 0,0,  1,0 };
		SweepDescriptor d = base;
		d.profile2Points = two; d.numProfile2Points = 2;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: profile2 with 2 points rejects" );
	}
	// (b) a ZERO-AREA profile2 (three collinear points)
	{
		const double flat[] = { -1,0,  0,0,  1,0 };
		SweepDescriptor d = base;
		d.profile2Points = flat; d.numProfile2Points = 3;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: zero-area profile2 rejects" );
	}
	// (c) point_morph with no profile2 at all -- names the missing half
	{
		const double morphs[] = { 0.0, 1.0, 1.0 };
		SweepDescriptor d = base;
		d.pointMorphs = morphs; d.numPointMorphs = 3;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: point_morph without profile2 rejects" );
	}
	// (d) an AUTHORED morph value outside [0, 1] is REFUSED, not clamped
	{
		std::vector<double> prof2 = NGon( 0.5, NP );
		const double bad[] = { 0.0, 1.4, 1.0 };
		SweepDescriptor d = base;
		d.profile2Points = &prof2[0]; d.numProfile2Points = NP;
		d.pointMorphs = bad; d.numPointMorphs = 3;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: an authored point_morph of 1.4 rejects" );
	}
	// (e) a non-finite morph value is refused by the same negated range test
	{
		std::vector<double> prof2 = NGon( 0.5, NP );
		const double nan3[] = { 0.0, std::numeric_limits<double>::quiet_NaN(), 1.0 };
		SweepDescriptor d = base;
		d.profile2Points = &prof2[0]; d.numProfile2Points = NP;
		d.pointMorphs = nan3; d.numPointMorphs = 3;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: a NaN point_morph rejects" );
	}
	// (e2) a NON-FINITE profile COORDINATE, in either slot.  The factory is
	// a PUBLIC entry point -- the GUI, the agent surface and these tests all
	// reach it without going through the scene parser's token gate -- and a
	// NaN coordinate passes every downstream gate (they are all COMPARISONS,
	// which are false for a NaN), so it used to bake a mesh of NaN vertices
	// after printing a bogus "wound OPPOSITE" warning on the way.
	{
		const double qNaN = std::numeric_limits<double>::quiet_NaN();
		const double inf  = std::numeric_limits<double>::infinity();
		double bad[10] = { 1.2,-0.3,  0.9,0.8,  -0.1,1.1,  -1.0,0.2,  -0.4,-0.9 };
		for( int slot = 0; slot < 2; ++slot ) {
			for( int which = 0; which < 2; ++which ) {
				double p1[10]; for( int q = 0; q < 10; ++q ) p1[q] = bad[q];
				double p2[10]; for( int q = 0; q < 10; ++q ) p2[q] = bad[q] * 0.5;
				const double poison = ( which == 0 ) ? qNaN : inf;
				// poison the x of point 2 in slot 0, the h of point 3 in slot 1
				if( slot == 0 ) p1[4] = poison; else p2[7] = poison;
				SweepDescriptor d = base;
				d.profilePoints = p1; d.numProfilePoints = 5;
				d.profile2Points = p2; d.numProfile2Points = 5;
				Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0,
				       slot == 0
				         ? ( which == 0 ? "loft validation: a NaN in the FIRST profile rejects"
				                        : "loft validation: an Inf in the FIRST profile rejects" )
				         : ( which == 0 ? "loft validation: a NaN in the SECOND profile rejects"
				                        : "loft validation: an Inf in the SECOND profile rejects" ) );
			}
		}
		// ...and the same guard on a plain, UNLOFTED sweep (the pre-existing
		// `profile_point inf 0` hole, which had nothing to do with the loft)
		double p1[10]; for( int q = 0; q < 10; ++q ) p1[q] = bad[q];
		p1[0] = inf;
		SweepDescriptor d = base;
		d.profilePoints = p1; d.numProfilePoints = 5;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0,
		       "loft validation: an Inf in the profile of a sweep with NO loft at all rejects too" );
	}
	// (f) an anisotropic y track with no x track / a mismatched count
	{
		const double sys[] = { 1.0, 0.5, 0.5 };
		SweepDescriptor d = base;
		d.pointScalesY = sys; d.numPointScalesY = 3;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: a y scale track with no x track rejects" );
		const double sxs[] = { 1.0, 1.0 };
		d.pointScales = sxs; d.numPointScales = 2;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: mismatched x/y scale counts reject" );
	}
	// (g) a non-positive y scale
	{
		const double sxs[] = { 1.0, 1.0, 1.0 };
		const double sys[] = { 1.0, 0.0, 1.0 };
		SweepDescriptor d = base;
		d.pointScales = sxs; d.numPointScales = 3;
		d.pointScalesY = sys; d.numPointScalesY = 3;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0, "loft validation: point_scale y of 0 rejects" );
	}
	// (h) profile2 on a CLOSED loop with NO explicit morph track: the
	// implicit 0 -> 1 ramp would jump at the seam, so it is refused...
	{
		std::vector<double> prof2 = NGon( 0.5, NP );
		SweepDescriptor d = base;
		d.pathPoints = loop; d.numPathPoints = 4;
		d.pathClosed = true;
		d.capStart = false; d.capEnd = false;
		d.profile2Points = &prof2[0]; d.numProfile2Points = NP;
		Check( !RISE_API_CreateSweepGeometry( &pi, d ) && pi == 0,
		       "loft validation: profile2 + path_closed with no point_morph rejects (the default ramp jumps at the seam)" );
		// ...but EXPLICIT values sample periodically and are legal
		const double outAndBack[] = { 0.0, 1.0, 0.0, 1.0 };
		d.pointMorphs = outAndBack; d.numPointMorphs = 4;
		Check( RISE_API_CreateSweepGeometry( &pi, d ) && pi != 0,
		       "loft validation: profile2 + path_closed WITH explicit point_morph is legal (periodic, closes smoothly)" );
		if( pi ) { pi->release(); pi = 0; }
	}
	// (i) the documented CLAMP: Catmull-Rom between non-monotone morph
	// controls overshoots [0, 1], and the clamp keeps every section inside
	// the two profiles' own radii instead of extrapolating past them.
	{
		std::vector<double> prof2 = NGon( 0.5, NP );
		const double zigzag[] = { 0.0, 1.0, 0.0, 1.0 };
		SweepDescriptor d = base;
		d.pathPoints = pts4; d.numPathPoints = 4;
		d.profile2Points = &prof2[0]; d.numProfile2Points = NP;
		d.pointMorphs = zigzag; d.numPointMorphs = 4;
		d.nLen = 40;
		d.capStart = false; d.capEnd = false;
		Check( RISE_API_CreateSweepGeometry( &pi, d ) && pi != 0, "loft validation: a non-monotone morph track bakes" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			bool inEnvelope = true;
			Scalar worstR = 0;
			if( m ) {
				for( unsigned int i = 0; i < m->numPoints() / NP; ++i ) {
					for( unsigned int k = 0; k < NP; ++k ) {
						Scalar lx = 0, lh = 0;
						LocalOfZPathVertex( m, i, NP, k, lx, lh );
						const Scalar r = std::sqrt( lx*lx + lh*lh );
						worstR = std::max( worstR, std::fabs( r - 0.75 ) );
						if( r < 0.5 - 1e-9 || r > 1.0 + 1e-9 ) inEnvelope = false;
					}
				}
			}
			Check( inEnvelope, "loft validation: MONEY ASSERTION -- the CLAMP keeps every section radius inside "
			                   "[0.5, 1.0] despite spline overshoot between non-monotone point_morph controls" );
			pi->release(); pi = 0;
		}
	}
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

//////////////////////////////////////////////////////////////////////
//
//  lathe_geometry (arc-85 C3) vs FIRST PRINCIPLES.  Every assertion
//  below is CLOSED FORM -- an exact right cylinder, an exact cone with
//  its analytic lateral area, an exact sphere with its analytic surface
//  area, an exact 90-degree angular span -- never "whatever the factory
//  produced last time".
//
//  Axis/basis recap (see RISE_API_CreateLatheGeometry's header comment):
//  for axis index a the frame is the cyclic (A, U, V) = (e[a],
//  e[(a+1)%3], e[(a+2)%3]).  For the default axis y that is A = +Y,
//  U = +Z, V = +X, so a profile point (r, h) at angle t lands at
//  (r*sin t, h, r*cos t) and t = atan2(x, z).
//
//////////////////////////////////////////////////////////////////////

static const double kPi = 3.14159265358979323846;

// Radius off the lathe axis (axis y) and the polar angle about it.
static Scalar LatheRadiusY( const Vertex& p ) { return std::sqrt( p.x*p.x + p.z*p.z ); }
static Scalar LatheThetaY ( const Vertex& p ) { return std::atan2( p.x, p.z ); }

// Total surface area of an indexed mesh, and the smallest triangle area
// in it (a lathe must never emit a zero-area triangle -- that is the
// whole point of collapsing an on-axis ring to a single pole vertex).
static void MeshAreaStats( const TriangleMeshGeometryIndexed* m, Scalar& total, Scalar& minTri )
{
	total = 0;
	minTri = std::numeric_limits<Scalar>::max();
	for( size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		const Point3& a = *face.pVertices[0];
		const Point3& b = *face.pVertices[1];
		const Point3& c = *face.pVertices[2];
		const Scalar ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
		const Scalar vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
		const Scalar cx = uy*vz - uz*vy, cy = uz*vx - ux*vz, cz = ux*vy - uy*vx;
		const Scalar area = Scalar(0.5) * std::sqrt( cx*cx + cy*cy + cz*cz );
		total += area;
		if( area < minTri ) minTri = area;
	}
}

// Fire one ray and report whether it struck a FRONT face -- i.e. whether
// the TRUE geometric normal opposes the ray.  The lathe factory builds a
// DOUBLE-SIDED TriangleMeshGeometryIndexed, which flips vGeomNormal to
// face the ray and records that in bGeomNormalOrientedToRay, so the raw
// dot product is always negative and useless; undo the flip with the
// recovery formula the flag's own documentation prescribes.
static bool LatheFrontFaceHit( const IGeometry* g, const Point3& o, const Vector3& d )
{
	RayIntersectionGeometric ri( Ray( o, d ), nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	if( !ri.bHit ) {
		return false;
	}
	const Scalar raw = Vector3Ops::Dot( ri.vGeomNormal, ri.ray.Dir() );
	const Scalar facing = ri.bGeomNormalOrientedToRay ? -raw : raw;
	return facing < 0;
}

// Probe the surface at a given polar angle about `axis`, from `startScale`
// times the expected hit radius, aimed straight at the axis.  Callers pass
// a MID-FACET angle so the ray never lands exactly on a shared vertex
// column or facet edge, where a hit is legitimately ambiguous.
static bool LatheProbeAtAngle( const IGeometry* g, const int axis, const double theta,
		const double h, const double rHit, const double startScale )
{
	const int au = ( axis + 1 ) % 3, av = ( axis + 2 ) % 3;
	const double ct = std::cos( theta ), st = std::sin( theta );
	Scalar o[3] = { 0, 0, 0 }, dir[3] = { 0, 0, 0 };
	o[axis] = (Scalar)h;
	o[au]   = (Scalar)( rHit * startScale * ct );
	o[av]   = (Scalar)( rHit * startScale * st );
	dir[au] = (Scalar)( -ct );
	dir[av] = (Scalar)( -st );
	return LatheFrontFaceHit( g, Point3( o[0], o[1], o[2] ), Vector3( dir[0], dir[1], dir[2] ) );
}

// Fire one ray and report whether the TRUE geometric normal at the hit --
// the double-sided flip undone exactly as RayCaster.cpp:456 does it -- lies
// in the same half-space as `want`.  This is the assertion a front-face
// probe CANNOT make: TriangleMeshGeometryIndexedSpecializations.h (~206)
// re-orients ri.vGeomNormal to AGREE with the interpolated shading normal
// BEFORE the double-sided flip is considered, so a vertex normal sitting in
// the wrong half-space silently flips the reported GEOMETRIC normal -- which
// is what SMS chain-physics validation, every side test, and RayCaster.cpp
// read.  A front-face probe still passes in that state; this does not.
static bool LatheGeomNormalInHalfSpace( const IGeometry* g, const Point3& o, const Vector3& d, const Vector3& want )
{
	RayIntersectionGeometric ri( Ray( o, d ), nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	if( !ri.bHit ) {
		return false;
	}
	const Scalar raw = Vector3Ops::Dot( ri.vGeomNormal, want );
	const Scalar trueDot = ri.bGeomNormalOrientedToRay ? -raw : raw;
	return trueDot > 0;
}

// WINDING, part 2.  A front-face ray probe alone CANNOT pin the emitted
// triangle winding on this mesh: TriangleMeshGeometryIndexed re-orients
// ri.vGeomNormal to agree with the Phong shading normal
// (TriangleMeshGeometryIndexedSpecializations.h ~line 206) before the
// double-sided flip is even considered, so a mesh whose winding is
// reversed but whose vertex normals still point outward probes
// IDENTICALLY.  The probes above therefore pin the NORMAL field; this
// pins the WINDING against it -- non-circularly, because the normal field
// itself is pinned in closed form elsewhere (exactly radial on the
// cylinder, exactly -Y at the cone apex, exactly the cut-plane normal on
// a partial-sweep cap) and by the outside-in probes.
static bool MeshWindingAgreesWithNormals( const TriangleMeshGeometryIndexed* m )
{
	for( size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		const Point3& a = *face.pVertices[0];
		const Point3& b = *face.pVertices[1];
		const Point3& c = *face.pVertices[2];
		const Scalar ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
		const Scalar vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
		const Scalar gx = uy*vz - uz*vy, gy = uz*vx - ux*vz, gz = ux*vy - uy*vx;
		const Normal& n0 = *face.pNormals[0];
		const Normal& n1 = *face.pNormals[1];
		const Normal& n2 = *face.pNormals[2];
		const Scalar rx = n0.x+n1.x+n2.x, ry = n0.y+n1.y+n2.y, rz = n0.z+n1.z+n2.z;
		if( gx*rx + gy*ry + gz*rz <= 0 ) {
			return false;
		}
	}
	return true;
}

// (1) CYLINDER IDENTITY.  profile (R,0) (R,H), 360 degrees, N radial:
// an exact right cylinder, and the money assertion on the SEAM -- a full
// turn stitches the last radial column straight back to the first, so
// the vertex count is exactly rows*N, never rows*(N+1).
static void TestLatheCylinderIdentity()
{
	std::cout << "Test 5: lathe_geometry -- cylinder identity + 360-degree seam (no duplicated column)" << std::endl;
	const double R = 2.0, H = 5.0;
	const int N = 32;
	const double prof[] = { R, 0.0,  R, H };
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 2;
	d.nRadial = N;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe cylinder factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "lathe cylinder concrete type" ); pi->release(); return; }

	Check( m->numPoints() == (unsigned int)( 2 * N ),
		"lathe cylinder: MONEY ASSERTION -- 360 degrees emits rows*n_radial vertices (no duplicated seam column)" );
	Check( m->numPoints() != (unsigned int)( 2 * ( N + 1 ) ),
		"lathe cylinder: the seam column is NOT duplicated (would be rows*(n_radial+1))" );
	Check( m->getFaces().size() == (size_t)( 2 * N ),
		"lathe cylinder: analytic triangle count 2*n_radial (one band, two triangles per quad)" );

	bool radiusOK = true, spanOK = true, normalOK = true, uvOK = true;
	Scalar hMin = m->getVertices()[0].y, hMax = m->getVertices()[0].y;
	for( unsigned int i = 0; i < m->numPoints(); ++i ) {
		const Vertex& p = m->getVertices()[i];
		const Normal& n = m->getNormals()[i];
		if( std::fabs( LatheRadiusY( p ) - R ) > 1e-9 ) radiusOK = false;
		if( p.y < hMin ) hMin = p.y;
		if( p.y > hMax ) hMax = p.y;
		if( p.y < -1e-12 || p.y > H + 1e-12 ) spanOK = false;
		// exactly radial: no axial component, unit dot with the radial direction
		const Scalar rr = LatheRadiusY( p );
		if( std::fabs( n.y ) > 1e-12 ) normalOK = false;
		if( ( n.x * p.x + n.z * p.z ) / ( rr > 0 ? rr : 1 ) < 1.0 - 1e-9 ) normalOK = false;
		// v = normalized arc length along the profile: 0 on the bottom row, 1 on the top
		const TexCoord& c = m->getCoords()[i];
		if( std::fabs( c.y - p.y / H ) > 1e-9 ) uvOK = false;
		if( c.x < -1e-12 || c.x > 1.0 - 1e-12 ) uvOK = false;	// u in [0,1), the seam never repeats
	}
	Check( radiusOK, "lathe cylinder: every vertex EXACTLY at radius R from the axis" );
	Check( spanOK && std::fabs( hMin ) < 1e-12 && std::fabs( hMax - H ) < 1e-12,
		"lathe cylinder: height span is exactly [0, H]" );
	Check( normalOK, "lathe cylinder: every normal exactly radial" );
	Check( uvOK, "lathe cylinder: u in [0,1) (no repeated seam), v == arc-length fraction" );

	// exact lateral area 2*pi*R*H, up to the inscribed-polygon factor
	Scalar area = 0, minTri = 0;
	MeshAreaStats( m, area, minTri );
	const double exact = 2.0 * kPi * R * H;
	const double inscribed = exact * ( std::sin( kPi / N ) / ( kPi / N ) );	// an N-gon prism's exact lateral area
	Check( std::fabs( (double)area - inscribed ) < 1e-9 * exact,
		"lathe cylinder: lateral area EXACTLY matches the inscribed N-gon prism 2*N*R*sin(pi/N)*H" );
	Check( minTri > 1e-12, "lathe cylinder: no zero-area triangles" );

	// CLOSED-FORM winding anchor, independent of the normal field: on a
	// cylinder about +Y the outward direction at a face centroid is simply
	// its own radial direction, so (b-a)x(c-a) . (cx, 0, cz) must be > 0
	// for EVERY face.
	{
		bool windOK = true;
		for( size_t f = 0; f < m->getFaces().size(); ++f ) {
			const PointerPolygon_Template<3>& face = m->getFaces()[f];
			const Point3& a = *face.pVertices[0];
			const Point3& b = *face.pVertices[1];
			const Point3& c = *face.pVertices[2];
			const Scalar ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
			const Scalar vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
			const Scalar gx = uy*vz - uz*vy, gz = ux*vy - uy*vx;
			const Scalar cx = ( a.x + b.x + c.x ) / 3, cz = ( a.z + b.z + c.z ) / 3;
			if( gx*cx + gz*cz <= 0 ) windOK = false;
		}
		Check( windOK, "lathe cylinder: MONEY ASSERTION -- every face's GEOMETRIC winding points radially OUTWARD" );
	}
	Check( MeshWindingAgreesWithNormals( m ), "lathe cylinder: winding agrees with the (independently pinned) normal field" );
	pi->release();
}

// (2) CONE IDENTITY.  profile (0,0) (R,H): the r == 0 point is a POLE.
// It must be ONE vertex fanned to the base ring, not N coincident copies
// with a zero-area quad band, and the lateral area must match the
// analytic pi*R*sqrt(R^2+H^2) to tessellation tolerance.
static void TestLatheConePole()
{
	std::cout << "Test 5b: lathe_geometry -- cone identity, apex is a single POLE vertex" << std::endl;
	const double R = 1.5, H = 3.0;
	const int N = 64;
	const double prof[] = { 0.0, 0.0,  R, H };
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 2;
	d.nRadial = N;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe cone factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "lathe cone concrete type" ); pi->release(); return; }

	Check( m->numPoints() == (unsigned int)( 1 + N ),
		"lathe cone: MONEY ASSERTION -- the apex is ONE vertex, so the count is 1 + n_radial (not 2*n_radial)" );
	Check( m->getFaces().size() == (size_t)N,
		"lathe cone: a pole emits a FAN of n_radial triangles (not 2*n_radial with half of them degenerate)" );

	unsigned int apexCount = 0, ringCount = 0;
	bool ringRadiusOK = true;
	for( unsigned int i = 0; i < m->numPoints(); ++i ) {
		const Vertex& p = m->getVertices()[i];
		const Scalar rr = LatheRadiusY( p );
		if( rr < 1e-12 ) {
			++apexCount;
			if( std::fabs( p.y ) > 1e-12 ) ringRadiusOK = false;
		} else {
			++ringCount;
			if( std::fabs( rr - R ) > 1e-9 || std::fabs( p.y - H ) > 1e-12 ) ringRadiusOK = false;
		}
	}
	Check( apexCount == 1, "lathe cone: exactly ONE vertex sits on the axis" );
	Check( ringCount == (unsigned int)N, "lathe cone: the base ring carries exactly n_radial vertices" );
	Check( ringRadiusOK, "lathe cone: apex at (0,0,0), base ring exactly at radius R and height H" );

	// the apex normal is the theta-average of the revolved profile normal:
	// the radial part cancels, leaving -Y for a cone opening upward.
	{
		bool apexNormalOK = false;
		for( unsigned int i = 0; i < m->numPoints(); ++i ) {
			if( LatheRadiusY( m->getVertices()[i] ) < 1e-12 ) {
				const Normal& n = m->getNormals()[i];
				apexNormalOK = ( std::fabs( n.x ) < 1e-12 && std::fabs( n.z ) < 1e-12 && n.y < -0.999 );
			}
		}
		Check( apexNormalOK, "lathe cone: the apex normal is exactly -Y (the radial part averages away)" );
	}

	Scalar area = 0, minTri = 0;
	MeshAreaStats( m, area, minTri );
	const double exact = kPi * R * std::sqrt( R*R + H*H );
	Check( std::fabs( (double)area - exact ) < 0.005 * exact,
		"lathe cone: lateral area matches the analytic pi*R*sqrt(R^2+H^2) within 0.5% at n_radial 64" );
	Check( minTri > 1e-12, "lathe cone: no zero-area triangles (the pole fan replaces the degenerate band)" );
	Check( MeshWindingAgreesWithNormals( m ),
		"lathe cone: MONEY ASSERTION -- every pole-fan triangle's GEOMETRIC winding faces outward" );
	pi->release();
}

// (3) SPHERE IDENTITY.  A semicircular profile revolved 360 degrees.
// Every vertex is EXACTLY on the sphere; the mesh area approaches the
// analytic 4*pi*R^2 from below, and raising n_radial must move it
// strictly closer (a monotone-improvement assertion, not a magic
// constant).
static void TestLatheSphereConvergence()
{
	std::cout << "Test 5c: lathe_geometry -- sphere identity + monotone area convergence to 4*pi*R^2" << std::endl;
	const double R = 1.25;
	const int M = 128;		// profile subdivisions (semicircle)
	std::vector<double> prof;
	prof.reserve( ( M + 1 ) * 2 );
	for( int i = 0; i <= M; ++i ) {
		const double phi = kPi * i / M;
		// AUTHORED AS GENERATED, deliberately un-patched.  sin(pi) is 1.5e-16,
		// not 0, in IEEE double, so the last profile point is NOT bit-exactly
		// on the axis -- and the factory's pole test is an exact `r > 0`.  An
		// earlier version of this test hand-pinned the two endpoints to 0.0 so
		// the factory would see them as poles; that hid the defect instead of
		// catching it (un-patched, the factory emitted a full n_radial ring of
		// radius 1.5e-16 with a skirt of ~1e-18-area slivers).  The factory
		// now snaps any radius within 1e-9 of the profile's own extent to a
		// hard 0.0 BEFORE the pole decision, which is what a generator -- or
		// the agent surface -- actually needs; this test is its guard.
		prof.push_back( R * std::sin( phi ) );		// r
		prof.push_back( -R * std::cos( phi ) );		// h, running -R -> +R
	}
	const double exact = 4.0 * kPi * R * R;

	double areas[2] = { 0, 0 };
	const int radial[2] = { 12, 96 };
	for( int t = 0; t < 2; ++t ) {
		LatheDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = (unsigned int)( M + 1 );
		d.nRadial = radial[t];
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe sphere factory succeeds" );
		if( !pi ) return;
		const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
		if( !m ) { Check( false, "lathe sphere concrete type" ); pi->release(); return; }

		bool onSphere = true;
		for( unsigned int i = 0; i < m->numPoints(); ++i ) {
			const Vertex& p = m->getVertices()[i];
			const Scalar rad = std::sqrt( p.x*p.x + p.y*p.y + p.z*p.z );
			if( std::fabs( rad - R ) > 1e-9 ) onSphere = false;
		}
		Check( onSphere, "lathe sphere: every vertex lies EXACTLY on the sphere of radius R" );

		// both poles collapse: 2 single vertices + (M-1) full rings
		Check( m->numPoints() == (unsigned int)( 2 + ( M - 1 ) * radial[t] ),
			"lathe sphere: MONEY ASSERTION -- both GENERATED on-axis endpoints collapse to ONE vertex each (the sin(pi) = 1.5e-16 end is SNAPPED to the axis, not emitted as a ring)" );

		// The same fact stated on the vertices rather than the count: exactly
		// two vertices sit BIT-EXACTLY on the axis.  Un-snapped there is only
		// one, plus a ring of n_radial vertices at radius 1.5e-16.
		unsigned int exactlyOnAxis = 0;
		for( unsigned int i = 0; i < m->numPoints(); ++i ) {
			const Vertex& p = m->getVertices()[i];
			if( p.x == 0 && p.z == 0 ) ++exactlyOnAxis;
		}
		Check( exactlyOnAxis == 2,
			"lathe sphere: exactly TWO vertices lie bit-exactly on the axis (the generated 1.5e-16 endpoint became a pole)" );

		Scalar area = 0, minTri = 0;
		MeshAreaStats( m, area, minTri );
		// 1e-12, not 1e-18: the real pole-fan triangles are ~3e-5 at these
		// resolutions, while an un-snapped 1.5e-16 ring produces ~1e-18
		// slivers, so this bound separates the two.
		Check( minTri > 1e-12, "lathe sphere: no zero-area triangles at either pole" );
		Check( MeshWindingAgreesWithNormals( m ), "lathe sphere: every face's GEOMETRIC winding faces outward" );
		areas[t] = (double)area;
	}
	Check( areas[0] < exact && areas[1] < exact,
		"lathe sphere: an inscribed mesh under-estimates 4*pi*R^2 at both resolutions" );
	Check( areas[1] > areas[0],
		"lathe sphere: MONOTONE improvement -- n_radial 96 is strictly closer to 4*pi*R^2 than n_radial 12" );
	Check( std::fabs( areas[1] - exact ) < 0.01 * exact,
		"lathe sphere: n_radial 96 lands within 1% of the analytic 4*pi*R^2" );
}

// (4) PARTIAL SWEEP.  90 degrees: the surface is open along the two cut
// half-planes, so both get a flat ear-clipped cap, and the emitted
// vertices span EXACTLY 90 degrees (columns are nRadial+1, not nRadial).
static void TestLathePartialSweep()
{
	std::cout << "Test 5d: lathe_geometry -- 90-degree partial sweep (caps present, exact angular span)" << std::endl;
	const double R = 1.0, H = 2.0;
	const int N = 8;
	const double prof[] = { R, 0.0,  R, H };
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 2;
	d.nRadial = N;
	d.sweepDegrees = 90.0;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe 90-degree factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "lathe partial concrete type" ); pi->release(); return; }

	// 2 rows x (N+1) columns of side vertices, plus 2 caps of 4
	// cross-section vertices each ((R,0) (R,H) axis(0,H) axis(0,0)).
	Check( m->numPoints() == (unsigned int)( 2 * ( N + 1 ) + 2 * 4 ),
		"lathe partial: a partial sweep emits n_radial+1 columns (the seam does NOT wrap) plus two 4-vertex caps" );
	// side 2*N triangles + 2 caps x 2 triangles
	Check( m->getFaces().size() == (size_t)( 2 * N + 4 ),
		"lathe partial: MONEY ASSERTION -- CAPS ARE PRESENT (2*n_radial side triangles + 2 ear-clipped caps of 2)" );

	// angular span of every OFF-AXIS vertex is exactly [0, 90 degrees]
	Scalar tMin = Scalar( 10.0 ), tMax = Scalar( -10.0 );
	for( unsigned int i = 0; i < m->numPoints(); ++i ) {
		const Vertex& p = m->getVertices()[i];
		if( LatheRadiusY( p ) < 1e-12 ) continue;	// the cross-section's on-axis corners have no angle
		const Scalar th = LatheThetaY( p );
		if( th < tMin ) tMin = th;
		if( th > tMax ) tMax = th;
	}
	Check( std::fabs( (double)tMin ) < 1e-12 && std::fabs( (double)tMax - kPi/2.0 ) < 1e-12,
		"lathe partial: the emitted angular span is EXACTLY [0, 90 degrees]" );

	Scalar area = 0, minTri = 0;
	MeshAreaStats( m, area, minTri );
	Check( minTri > 1e-12, "lathe partial: no zero-area triangles" );
	// side (a quarter of the inscribed prism) + two R x H rectangular caps
	const double side = 2.0 * N * R * std::sin( ( kPi / 2.0 ) / ( 2 * N ) ) * H;
	const double caps = 2.0 * R * H;
	Check( std::fabs( (double)area - ( side + caps ) ) < 1e-9 * ( side + caps ),
		"lathe partial: total area EXACTLY equals the quarter prism plus the two flat R x H caps" );

	// CLOSED-FORM cap winding.  The side bands are emitted first (2*N
	// faces), then the theta=0 cap (2 faces) and the theta=90 cap (2).
	// With axis y the frame is A=+Y, U=+Z, V=+X, so the start cap must
	// wind toward -X and the end cap toward -Z, exactly.
	{
		bool capWindOK = true;
		for( size_t f = (size_t)( 2 * N ); f < m->getFaces().size(); ++f ) {
			const PointerPolygon_Template<3>& face = m->getFaces()[f];
			const Point3& a = *face.pVertices[0];
			const Point3& b = *face.pVertices[1];
			const Point3& c = *face.pVertices[2];
			const Scalar ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
			const Scalar vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
			const Scalar gx = uy*vz - uz*vy, gy = uz*vx - ux*vz, gz = ux*vy - uy*vx;
			const bool isStartCap = ( f < (size_t)( 2 * N + 2 ) );
			const Scalar want = isStartCap ? -gx : -gz;		// -X for the start cap, -Z for the end cap
			const Scalar other = isStartCap ? ( std::fabs(gy) + std::fabs(gz) ) : ( std::fabs(gx) + std::fabs(gy) );
			if( !( want > 0 ) || other > 1e-12 * ( std::fabs(gx)+std::fabs(gy)+std::fabs(gz) + 1 ) ) capWindOK = false;
		}
		Check( capWindOK,
			"lathe partial: MONEY ASSERTION -- the theta=0 cap winds toward -X and the theta=90 cap toward -Z" );
	}
	Check( MeshWindingAgreesWithNormals( m ), "lathe partial: winding agrees with the cap and side normals" );
	pi->release();
}

// (5) WINDING.  The convention (dP/dtheta x dP/dh points outward, so a
// band quad is emitted +theta first and +h second) is PINNED by ray
// probes from OUTSIDE, not by the comment that derives it: every probe
// must strike a FRONT face.  Covers the cylinder side, the cone side and
// pole fan, and both partial-sweep caps.
static void TestLatheWinding()
{
	std::cout << "Test 5e: lathe_geometry -- outward winding pinned by front-face ray probes" << std::endl;

	// (a) cylinder side, probed at 8 angles and 3 heights
	{
		const double R = 2.0, H = 4.0;
		const double prof[] = { R, 0.0,  R, H };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 2;
		d.nRadial = 64;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "winding cylinder factory succeeds" );
		if( pi ) {
			bool allFront = true;
			for( int a = 0; a < 8 && allFront; ++a ) {
				const double th = 2.0 * kPi * ( 8.0 * a + 0.5 ) / 64.0;	// mid-facet of n_radial 64
				for( int hi = 1; hi <= 3; ++hi ) {
					if( !LatheProbeAtAngle( pi, 1, th, H * hi / 4.0, R, 3.0 ) ) allFront = false;
				}
			}
			Check( allFront, "winding: every inward probe at the cylinder side strikes a FRONT face" );
			pi->release();
		}
	}

	// (b) cone side + apex fan
	{
		const double R = 1.0, H = 2.0;
		const double prof[] = { 0.0, 0.0,  R, H };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 2;
		d.nRadial = 48;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "winding cone factory succeeds" );
		if( pi ) {
			bool allFront = true;
			for( int a = 0; a < 8 && allFront; ++a ) {
				const double th = 2.0 * kPi * ( 6.0 * a + 0.5 ) / 48.0;	// mid-facet of n_radial 48
				const double ys[3] = { H * 0.25, H * 0.5, H * 0.85 };
				for( int k = 0; k < 3; ++k ) {
					if( !LatheProbeAtAngle( pi, 1, th, ys[k], R * ys[k] / H, 4.0 ) ) allFront = false;
				}
			}
			Check( allFront, "winding: every inward probe at the cone lateral surface strikes a FRONT face" );
			pi->release();
		}
	}

	// (c) partial-sweep caps.  With axis y the frame is A=+Y, U=+Z, V=+X,
	// so a 90-degree sweep occupies the x>=0, z>=0 quadrant: the START cap
	// lies in the plane x = 0 facing -X, the END cap in z = 0 facing -Z.
	{
		const double R = 1.0, H = 2.0;
		const double prof[] = { R, 0.0,  R, H };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 2;
		d.nRadial = 16;
		d.sweepDegrees = 90.0;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "winding partial-sweep factory succeeds" );
		if( pi ) {
			// (r, h) = (0.7, 1.0) inside the R x H cross-section and off both
			// of its possible ear-clip diagonals (h = 2r and h = 2 - 2r).
			const bool startFront = LatheFrontFaceHit( pi, Point3( -3.0, 1.0, 0.7 ), Vector3( 1, 0, 0 ) );
			const bool endFront   = LatheFrontFaceHit( pi, Point3( 0.7, 1.0, -3.0 ), Vector3( 0, 0, 1 ) );
			Check( startFront, "winding: the theta=0 cap faces -X and is struck FRONT-facing from -X" );
			Check( endFront,   "winding: the theta=90 cap faces -Z and is struck FRONT-facing from -Z" );
			pi->release();
		}
	}
}

// (6) VASE.  The headline case: a profile that starts and ends ON the
// axis is a closed vessel -- both poles collapse, no caps are emitted at
// 360 degrees, and there is not a single zero-area triangle.  Also pins
// the duplicate-a-point hard-edge idiom (a zero-length profile segment
// splits the normals and emits NO band, rather than a zero-area one).
static void TestLatheVaseAndHardEdge()
{
	std::cout << "Test 5f: lathe_geometry -- closed vase (both poles) + the duplicate-point hard-edge idiom" << std::endl;
	{
		const int N = 24;
		const double prof[] = {
			0.00, 0.00,
			0.35, 0.05,
			0.42, 0.30,
			0.18, 0.72,
			0.22, 0.90,
			0.00, 0.94
		};
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 6;
		d.nRadial = N;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe vase factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			if( m ) {
				// 2 poles + 4 full rings; 2 pole fans of N + 3 bands of 2N
				Check( m->numPoints() == (unsigned int)( 2 + 4 * N ),
					"lathe vase: MONEY ASSERTION -- rings*n_radial minus the two pole collapses" );
				Check( m->getFaces().size() == (size_t)( 2 * N + 3 * 2 * N ),
					"lathe vase: two pole fans of n_radial plus three full bands of 2*n_radial" );
				Scalar area = 0, minTri = 0;
				MeshAreaStats( m, area, minTri );
				Check( minTri > 1e-12, "lathe vase: MONEY ASSERTION -- watertight with NO zero-area triangles and no caps" );
				// front-face probes around the widest part
				bool allFront = true;
				for( int a = 0; a < 6; ++a ) {
					const double th = 2.0 * kPi * ( 4.0 * a + 0.5 ) / 24.0;	// mid-facet of n_radial 24
					if( !LatheProbeAtAngle( pi, 1, th, 0.30, 0.40, 8.0 ) ) allFront = false;
				}
				Check( allFront, "lathe vase: inward probes at the belly strike FRONT faces" );
				Check( MeshWindingAgreesWithNormals( m ),
					"lathe vase: every face's GEOMETRIC winding faces outward (both pole fans included)" );
			}
			pi->release();
		}
	}
	// hard-edge idiom: a duplicated profile point splits the two normals
	// and contributes NO band (so no zero-area triangles either).
	{
		const int N = 8;
		const double prof[] = { 0.0, 0.0,   1.0, 1.0,   1.0, 1.0,   1.0, 2.0 };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 4;
		d.nRadial = N;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe hard-edge factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			if( m ) {
				// pole + 3 rings; a pole fan of N and one band of 2N (the
				// duplicated segment emits nothing at all)
				Check( m->numPoints() == (unsigned int)( 1 + 3 * N ),
					"hard edge: the duplicated point still gets its own row" );
				Check( m->getFaces().size() == (size_t)( N + 2 * N ),
					"hard edge: MONEY ASSERTION -- the zero-length segment emits NO band (not a zero-area one)" );
				// rows 1 and 2 are coincident but must carry DIFFERENT normals
				const Normal& nA = m->getNormals()[ 1 ];			// first vertex of row 1 (cone side)
				const Normal& nB = m->getNormals()[ 1 + N ];		// first vertex of row 2 (cylinder side)
				const Scalar dot = nA.x*nB.x + nA.y*nB.y + nA.z*nB.z;
				Check( dot < 0.99, "hard edge: the duplicated point's two rows carry DIFFERENT normals" );
				Scalar area = 0, minTri = 0;
				MeshAreaStats( m, area, minTri );
				Check( minTri > 1e-12, "hard edge: no zero-area triangles" );
			}
			pi->release();
		}
	}
}

// (7) AXIS + ORIENTATION.  axis x / y / z produce the same cylinder about
// their own axis; and a profile authored TOP-TO-BOTTOM still faces
// outward, because the orientation is derived from the signed volume of
// revolution rather than assumed from the authoring order.
static void TestLatheAxisAndReversedProfile()
{
	std::cout << "Test 5g: lathe_geometry -- axis x/y/z, and a reversed profile still faces outward" << std::endl;
	const double R = 1.0, H = 3.0;
	for( int axis = 0; axis < 3; ++axis ) {
		const double prof[] = { R, 0.0,  R, H };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 2;
		d.nRadial = 24;
		d.axis = axis;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe axis factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			bool onAxis = true;
			if( m ) for( unsigned int i = 0; i < m->numPoints(); ++i ) {
				const Vertex& p = m->getVertices()[i];
				const Scalar comp[3] = { p.x, p.y, p.z };
				const Scalar h = comp[axis];
				const Scalar rr = std::sqrt( comp[(axis+1)%3]*comp[(axis+1)%3] + comp[(axis+2)%3]*comp[(axis+2)%3] );
				if( std::fabs( rr - R ) > 1e-9 || h < -1e-12 || h > H + 1e-12 ) onAxis = false;
			}
			Check( onAxis, "axis: the cylinder is exactly about the requested world axis" );
			// probe perpendicular to the axis, mid-facet of n_radial 24
			Check( LatheProbeAtAngle( pi, axis, 2.0 * kPi * 0.5 / 24.0, H * 0.5, R, 4.0 ),
				"axis: an inward probe strikes a FRONT face on every axis" );
			// The two assertions above are both invariant under a U/V swap
			// (radius and height are symmetric in the two off-axis
			// components, and a probe fired straight at the axis hits either
			// way), so a HANDEDNESS flip on x or z alone would sail through
			// them -- the closed-form radial-outward winding check lives only
			// in the axis-y cylinder test.  This pins the frame per axis:
			// under a coordinate-swap reflection the geometric normal picks
			// up the reflection's sign while the vertex normals are written
			// componentwise, so the two disagree and this fails.
			Check( m && MeshWindingAgreesWithNormals( m ),
				"axis: MONEY ASSERTION -- winding agrees with the normal field on EVERY axis (a handedness flip on x or z alone is caught here)" );
			pi->release();
		}
	}
	// reversed profile (top -> bottom): the signed volume of revolution is
	// negative, so the factory flips both the normals and the winding.
	{
		const double prof[] = { R, H,  R, 0.0 };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 2;
		d.nRadial = 24;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe reversed-profile factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			bool radialOut = true;
			if( m ) for( unsigned int i = 0; i < m->numPoints(); ++i ) {
				const Vertex& p = m->getVertices()[i];
				const Normal& n = m->getNormals()[i];
				const Scalar rr = LatheRadiusY( p );
				if( ( n.x * p.x + n.z * p.z ) / ( rr > 0 ? rr : 1 ) < 1.0 - 1e-9 ) radialOut = false;
			}
			Check( radialOut, "reversed profile: normals still point radially OUTWARD (orientation is derived, not assumed)" );
			Check( LatheProbeAtAngle( pi, 1, 2.0 * kPi * 0.5 / 24.0, H * 0.5, R, 4.0 ),
				"reversed profile: an inward probe still strikes a FRONT face" );
			Check( m && MeshWindingAgreesWithNormals( m ),
				"reversed profile: MONEY ASSERTION -- the winding is flipped in lockstep with the normals" );
			pi->release();
		}
	}
}

// (8) INTERIOR POLE.  A waisted / hourglass profile pinched to r == 0 in
// the MIDDLE has live bands on BOTH sides of the pinch, and the two bands
// demand provably OPPOSITE axial normals: below the pinch dr < 0 so
// snh = -outward*dr/l > 0, above it dr > 0 so snh < 0.  No single shared
// vertex can serve both -- so the pole is emitted TWICE, once per band,
// exactly as `smooth FALSE` already does structurally.
static void TestLatheInteriorPolePinch()
{
	std::cout << "Test 5i: lathe_geometry -- an INTERIOR pole splits into one vertex per adjacent band" << std::endl;
	const double R = 1.0;
	const int N = 24;
	const double prof[] = { R, 0.0,   0.0, 1.0,   R, 2.0 };	// hourglass, pinched on the axis at h = 1
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 3;
	d.nRadial = N;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "lathe pinch factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "lathe pinch concrete type" ); pi->release(); return; }

	Check( m->numPoints() == (unsigned int)( 2 * N + 2 ),
		"pinch: MONEY ASSERTION -- the interior pole emits TWO vertices (one per adjacent band), not one shared vertex" );
	Check( m->getFaces().size() == (size_t)( 2 * N ),
		"pinch: each band is a pole fan of n_radial triangles" );

	// The two pole vertices are emitted in row order: the lower band's
	// first.  Their axial normals must be exact, opposite unit vectors.
	int poleCount = 0;
	Scalar poleNy[4] = { 0, 0, 0, 0 };
	bool poleAxialOnly = true;
	for( unsigned int i = 0; i < m->numPoints(); ++i ) {
		if( LatheRadiusY( m->getVertices()[i] ) < 1e-12 ) {
			const Normal& n = m->getNormals()[i];
			if( poleCount < 4 ) poleNy[poleCount] = n.y;
			if( std::fabs( n.x ) > 1e-12 || std::fabs( n.z ) > 1e-12 ) poleAxialOnly = false;
			++poleCount;
		}
	}
	Check( poleCount == 2, "pinch: exactly two vertices sit on the axis" );
	Check( poleAxialOnly, "pinch: both pole normals are purely axial (the radial part averages away)" );
	Check( poleCount == 2 && poleNy[0] > 0.999 && poleNy[1] < -0.999,
		"pinch: MONEY ASSERTION -- the lower band's pole normal is +Y and the upper band's is -Y (no single sign serves both)" );

	// The consequence that actually matters.  Probe each band CLOSE to the
	// pinch (barycentric weight ~0.9 on the pole vertex, so the interpolated
	// shading normal is pole-dominated) and demand that the recovered
	// GEOMETRIC normal lies in the same half-space as that band's analytic
	// surface normal.  Analytically the lower band's normal is
	// (e_r + A)/sqrt(2) and the upper band's is (e_r - A)/sqrt(2); with one
	// shared +A pole vertex the upper band's reported vGeomNormal is FLIPPED
	// into the solid and this fails.
	{
		const double th = 2.0 * kPi * 0.5 / N;		// mid-facet
		const double ct = std::cos( th ), st = std::sin( th );	// point = h*Y + r*ct*Z + r*st*X
		bool bandOK[2] = { false, false };
		for( int side = 0; side < 2; ++side ) {
			const double h     = ( side == 0 ) ? 0.9 : 1.1;		// r = 0.1 on both bands
			const double ySign = ( side == 0 ) ? 1.0 : -1.0;
			const Point3  o( (Scalar)( 0.5 * st ), (Scalar)h, (Scalar)( 0.5 * ct ) );
			const Vector3 dir( (Scalar)(-st), 0, (Scalar)(-ct) );
			const Vector3 want( (Scalar)st, (Scalar)ySign, (Scalar)ct );	// e_r +/- A, unnormalized
			bandOK[side] = LatheGeomNormalInHalfSpace( pi, o, dir, want );
		}
		Check( bandOK[0], "pinch: the LOWER band's near-pole hit reports a geometric normal in its own band's half-space" );
		Check( bandOK[1],
			"pinch: MONEY ASSERTION -- the UPPER band's near-pole hit does too (a shared pole vertex reports it FLIPPED, pointing into the solid)" );
	}

	Scalar area = 0, minTri = 0;
	MeshAreaStats( m, area, minTri );
	Check( minTri > 1e-12, "pinch: no zero-area triangles" );
	Check( MeshWindingAgreesWithNormals( m ), "pinch: winding agrees with the normal field on both fans" );
	pi->release();
}

// (9) V IS ARC LENGTH, NOT INDEX FRACTION.  The cylinder test cannot tell
// the two apart (a 2-point profile gives {0,1} either way), so the spec's
// arc-length requirement is pinned HERE, on an unevenly-sampled profile
// where every interior row differs between the two conventions.
static void TestLatheArcLengthV()
{
	std::cout << "Test 5j: lathe_geometry -- V is normalized ARC LENGTH (not index fraction)" << std::endl;
	{
		const int N = 8;
		const double prof[] = {
			0.00, 0.00,
			0.35, 0.05,
			0.42, 0.30,
			0.18, 0.72,
			0.22, 0.90,
			0.00, 0.94
		};
		// Segment lengths 0.35355339, 0.25961510, 0.48373546, 0.18439089,
		// 0.22360680; total 1.50490164.  Normalized cumulative sums below.
		const double wantV[6]  = { 0.0, 0.234934550, 0.407447552, 0.728887473, 0.851414344, 1.0 };
		const double indexV[6] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0 };
		bool discriminates = true;
		for( int j = 1; j < 5; ++j ) {
			if( std::fabs( wantV[j] - indexV[j] ) < 1e-6 ) discriminates = false;
		}
		Check( discriminates,
			"arc-length V: the vase profile SEPARATES arc length from index fraction at every interior row (so the assertions below discriminate)" );

		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 6;
		d.nRadial = N;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "arc-length V factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			if( m && m->numPoints() == (unsigned int)( 2 + 4 * N ) ) {
				// Row layout: pole (1 vertex), 4 rings of N, pole (1 vertex).
				bool rowVOK = true, ringUniformOK = true;
				if( std::fabs( (double)m->getCoords()[0].y - wantV[0] ) > 1e-9 ) rowVOK = false;
				for( int j = 1; j <= 4; ++j ) {
					const unsigned int base = (unsigned int)( 1 + ( j - 1 ) * N );
					if( std::fabs( (double)m->getCoords()[base].y - wantV[j] ) > 1e-9 ) rowVOK = false;
					for( int k = 1; k < N; ++k ) {
						if( m->getCoords()[ base + k ].y != m->getCoords()[base].y ) ringUniformOK = false;
					}
				}
				if( std::fabs( (double)m->getCoords()[ 4*N + 1 ].y - wantV[5] ) > 1e-9 ) rowVOK = false;
				Check( rowVOK,
					"arc-length V: MONEY ASSERTION -- every row's v is the normalized ARC LENGTH to that profile point (index fraction would give 0/.2/.4/.6/.8/1)" );
				Check( ringUniformOK, "arc-length V: every vertex of a ring carries that row's single v" );
			} else {
				Check( false, "arc-length V: expected vertex layout (2 poles + 4 rings)" );
			}
			pi->release();
		}
	}
	// A ZERO-LENGTH segment (the duplicate-a-point hard-edge idiom) adds no
	// arc length, so its two rows must carry IDENTICAL v -- under index
	// fraction they would be 1/3 and 2/3 and the texture would tear across
	// an edge that is geometrically in one place.
	{
		const int N = 8;
		const double prof[] = { 1.0, 0.0,   1.0, 1.0,   1.0, 1.0,   0.6, 2.0 };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 4;
		d.nRadial = N;
		ITriangleMeshGeometryIndexed* pi = 0;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "arc-length V hard-edge factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			if( m && m->numPoints() == (unsigned int)( 4 * N ) ) {
				// vFrac[2] == vFrac[1] + sqrt(0) exactly, then both scaled by
				// the same total -- so this is an EXACT equality, not a
				// tolerance.
				const Scalar v1 = m->getCoords()[ N ].y;
				const Scalar v2 = m->getCoords()[ 2 * N ].y;
				Check( v1 == v2,
					"arc-length V: MONEY ASSERTION -- a zero-length profile segment gives its two rows IDENTICAL v (index fraction would give 1/3 vs 2/3)" );
				Check( v1 > 0 && v1 < 1, "arc-length V: the duplicated point's v is interior to [0,1]" );
			} else {
				Check( false, "arc-length V: expected hard-edge vertex layout" );
			}
			pi->release();
		}
	}
}

// (10) A GENERATED CLOSED PROFILE AT A PARTIAL SWEEP.  A bead/torus ring
// built from cos/sin does not close BIT-EXACTLY, so an `==` loop test read
// it as OPEN, appended a spur from the ring THROUGH its own interior to the
// axis, and ear-clipping the self-intersecting cross-section failed -- i.e.
// the ring rendered fine at 360 and HARD-FAILED the instant the author set a
// partial sweep.  The loop test is a tolerance, and the cap carries M
// cross-section vertices (the near-duplicate closing point dropped).
static void TestLatheGeneratedClosedProfile()
{
	std::cout << "Test 5k: lathe_geometry -- a GENERATED closed bead profile builds at a partial sweep" << std::endl;
	const int M = 32, N = 12;
	const double rc = 1.0, hc = 0.5, rad = 0.3;
	std::vector<double> prof;
	prof.reserve( ( M + 1 ) * 2 );
	for( int k = 0; k <= M; ++k ) {
		const double a = 2.0 * kPi * k / M;
		prof.push_back( rc + rad * std::cos( a ) );
		prof.push_back( hc + rad * std::sin( a ) );
	}
	Check( prof[0] != prof[ 2*M ] || prof[1] != prof[ 2*M + 1 ],
		"bead: the generated closure is NOT bit-exact (so an `==` loop test cannot see it -- this is the premise of the case)" );

	LatheDescriptor d;
	d.profilePoints = &prof[0]; d.numProfilePoints = (unsigned int)( M + 1 );
	d.nRadial = N;
	d.sweepDegrees = 180.0;
	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ),
		"bead: MONEY ASSERTION -- a generated closed profile at sweep_degrees 180 BUILDS" );
	if( pi ) {
		const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
		if( m ) {
			Check( m->numPoints() == (unsigned int)( ( M + 1 ) * ( N + 1 ) + 2 * M ),
				"bead: MONEY ASSERTION -- each cap carries M cross-section vertices (the loop is detected by TOLERANCE, so the near-duplicate closing point is dropped)" );
			Check( m->getFaces().size() == (size_t)( M * 2 * N + 2 * ( M - 2 ) ),
				"bead: each cap ear-clips a simple M-gon to M-2 triangles" );
			Scalar area = 0, minTri = 0;
			MeshAreaStats( m, area, minTri );
			Check( minTri > 1e-12, "bead: no zero-area triangles (a retained near-duplicate vertex would make a ~1e-17 sliver)" );
		}
		pi->release();
	}
}

// (11) A LEVEL-ENDED TUBE.  A shell authored up one wall and back down the
// other ends at the SAME height, so closing the cross-section "back through
// the axis" would append a spur that is COLLINEAR with both endpoints: zero
// area, and an ear-clipped zero-area triangle -- contradicting the "no
// zero-area triangles" contract.  Level ends close DIRECTLY instead, which
// is both non-degenerate and the right shape (the wall, not the filled
// bore).  (When the two ends are at DIFFERENT heights the spur has real
// area and the cap DOES fill the bore; that is the documented limitation of
// the close-through-the-axis rule.)
static void TestLatheLevelEndTubeCap()
{
	std::cout << "Test 5l: lathe_geometry -- a level-ended TUBE caps without a zero-area spur" << std::endl;
	const int N = 8;
	const double prof[] = { 2.0, 0.0,   2.0, 1.0,   1.0, 1.0,   1.0, 0.0 };
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 4;
	d.nRadial = N;
	d.sweepDegrees = 90.0;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "tube: a level-ended shell profile builds at a partial sweep" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "tube: concrete type" ); pi->release(); return; }

	Check( m->numPoints() == (unsigned int)( 4 * ( N + 1 ) + 2 * 4 ),
		"tube: MONEY ASSERTION -- the cap is the 4-vertex wall cross-section (closing through the axis appends a 5th, collinear, vertex)" );
	Check( m->getFaces().size() == (size_t)( 3 * 2 * N + 2 * 2 ),
		"tube: three side bands plus two 2-triangle caps" );

	Scalar area = 0, minTri = 0;
	MeshAreaStats( m, area, minTri );
	Check( minTri > 1e-12,
		"tube: MONEY ASSERTION -- no zero-area triangles (the axis spur would ear-clip to a collinear one)" );

	// Cap area, in closed form: the cross-section is exactly the 1 x 1 wall
	// rectangle, on each of the two cut half-planes.
	{
		Scalar capArea = 0;
		for( size_t f = (size_t)( 3 * 2 * N ); f < m->getFaces().size(); ++f ) {
			const PointerPolygon_Template<3>& face = m->getFaces()[f];
			const Point3& a = *face.pVertices[0];
			const Point3& b = *face.pVertices[1];
			const Point3& c = *face.pVertices[2];
			const Scalar ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
			const Scalar vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
			const Scalar cx = uy*vz - uz*vy, cy = uz*vx - ux*vz, cz = ux*vy - uy*vx;
			capArea += Scalar(0.5) * std::sqrt( cx*cx + cy*cy + cz*cz );
		}
		Check( std::fabs( (double)capArea - 2.0 ) < 1e-12,
			"tube: the two caps are EXACTLY the 1 x 1 wall rectangle (not the filled bore)" );
	}
	Check( MeshWindingAgreesWithNormals( m ), "tube: winding agrees with the side and cap normals" );
	pi->release();
}

// (12) AN INTERIOR POLE AT A PARTIAL SWEEP.  The hourglass -- the shape the
// interior-pole split exists for -- had never been asked to CAP itself.
// Closing (1,0) (0,1) (1,2) back through the axis appends the spur (0,2)
// (0,0), and the profile's own pinch point (0,1) lies EXACTLY on the closing
// edge (0,2)->(0,0): the polygon is not simple, it merely touches itself.
// Ear clipping still succeeds and the total area is still right, but one of
// the emitted triangles has EXACTLY zero area -- the same contract violation
// the level-ends rule cites as its own reason to exist.  The cross-section is
// therefore SPLIT at the interior on-axis point and each lobe capped on its
// own, which is both non-degenerate and the same total area.
static void TestLatheInteriorPolePartialCap()
{
	std::cout << "Test 5m: lathe_geometry -- an INTERIOR pole at a partial sweep caps as two lobes (no zero-area cap triangle)" << std::endl;
	const int N = 8;
	const double prof[] = { 1.0, 0.0,   0.0, 1.0,   1.0, 2.0 };	// hourglass, pinched on the axis at h = 1
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 3;
	d.nRadial = N;
	d.sweepDegrees = 90.0;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "hourglass-90: factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "hourglass-90: concrete type" ); pi->release(); return; }

	// Sides: two rings of N+1 columns plus the two split pinch poles.
	// Caps: two sides x two 3-vertex lobes.
	Check( m->numPoints() == (unsigned int)( 2 * ( N + 1 ) + 2 + 2 * ( 3 + 3 ) ),
		"hourglass-90: MONEY ASSERTION -- each cap is TWO 3-vertex lobes (the un-split cross-section would be one 5-gon)" );
	Check( m->getFaces().size() == (size_t)( 2 * N + 4 ),
		"hourglass-90: two pole fans of n_radial, plus one triangle per lobe per cap" );

	Scalar area = 0, minTri = 0;
	MeshAreaStats( m, area, minTri );
	Check( minTri > 1e-12,
		"hourglass-90: MONEY ASSERTION -- no zero-area triangles (the un-split 5-gon ear-clips one of EXACTLY zero area at the pinch)" );

	// Cap area in closed form: the cross-section is the two unit right
	// triangles (0,0)(1,0)(0,1) and (0,1)(1,2)(0,2), area 1/2 each, on each
	// of the two cut half-planes.
	{
		Scalar capArea = 0;
		for( size_t f = (size_t)( 2 * N ); f < m->getFaces().size(); ++f ) {
			const PointerPolygon_Template<3>& face = m->getFaces()[f];
			const Point3& a = *face.pVertices[0];
			const Point3& b = *face.pVertices[1];
			const Point3& c = *face.pVertices[2];
			const Scalar ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
			const Scalar vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
			const Scalar cx = uy*vz - uz*vy, cy = uz*vx - ux*vz, cz = ux*vy - uy*vx;
			capArea += Scalar(0.5) * std::sqrt( cx*cx + cy*cy + cz*cz );
		}
		Check( std::fabs( (double)capArea - 2.0 ) < 1e-12,
			"hourglass-90: the two split caps carry EXACTLY the same total area as the un-split cross-section (2 x 1.0)" );
	}
	Check( MeshWindingAgreesWithNormals( m ), "hourglass-90: winding agrees with the side and cap normals" );
	pi->release();
}

// (13) A FLAT ANNULUS AT A PARTIAL SWEEP.  A zero-thickness radial washer
// (1,0) (3,0) is level-ended, so no axis spur is appended and the
// cross-section is the 2-point segment itself -- which has no area to cap.
// Refusing to build there is the same "renders fine at 360, HARD-FAILS the
// instant the author types sweep_degrees 180" pathology the loop tolerance
// exists to remove; the right answer is the band with NO caps.
static void TestLatheFlatAnnulusPartialSweep()
{
	std::cout << "Test 5n: lathe_geometry -- a FLAT annulus builds at a partial sweep (band, no caps)" << std::endl;
	const int N = 6;
	const double r0 = 1.0, r1 = 3.0, sweep = 120.0;
	const double prof[] = { r0, 0.0,   r1, 0.0 };
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 2;
	d.nRadial = N;
	d.sweepDegrees = sweep;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ),
		"flat annulus: MONEY ASSERTION -- a zero-thickness cross-section BUILDS at a partial sweep instead of refusing" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "flat annulus: concrete type" ); pi->release(); return; }

	Check( m->numPoints() == (unsigned int)( 2 * ( N + 1 ) ),
		"flat annulus: two rows of n_radial+1 columns and NOTHING else (no cap vertices)" );
	Check( m->getFaces().size() == (size_t)( 2 * N ),
		"flat annulus: the band only -- a zero-area cross-section contributes no cap triangles" );

	Scalar area = 0, minTri = 0;
	MeshAreaStats( m, area, minTri );
	Check( minTri > 1e-12, "flat annulus: no zero-area triangles" );
	// Closed form: each radial segment is an annular-sector polygon of area
	// (1/2)*sin(dTheta)*(r1^2 - r0^2).
	const double dTheta = ( sweep * kPi / 180.0 ) / N;
	const double exact = N * 0.5 * std::sin( dTheta ) * ( r1*r1 - r0*r0 );
	Check( std::fabs( (double)area - exact ) < 1e-9 * exact,
		"flat annulus: area EXACTLY equals the inscribed annular-sector polygon" );
	Check( MeshWindingAgreesWithNormals( m ), "flat annulus: winding agrees with the (purely axial) normal field" );
	pi->release();
}

// (14) DEAD ROWS MUST NOT REACH THE MESH.  A profile that walks UP THE AXIS
// before it leaves it -- (0,-100) (0,0) (1,1) (0,2) -- has a first segment
// that lies entirely on the axis, so that segment is not live and its first
// point is referenced by no band at all.  Emitting a row for it anyway puts
// an unreferenced vertex at h = -100 in the mesh, and
// TriangleMeshGeometryIndexed builds its BVH root box over ALL points rather
// than over referenced triangles -- so the root grows from [0,2] to [-100,2],
// a 50x oversized box that every traversal pays for.
static void TestLatheDeadRowBoundingBox()
{
	std::cout << "Test 5o: lathe_geometry -- an unreferenced on-axis row never reaches the mesh (bbox = the LIVE extent)" << std::endl;
	const int N = 8;
	const double prof[] = { 0.0, -100.0,   0.0, 0.0,   1.0, 1.0,   0.0, 2.0 };
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 4;
	d.nRadial = N;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "dead row: factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "dead row: concrete type" ); pi->release(); return; }

	// two pole vertices (h = 0 and h = 2) + one ring; the h = -100 row is
	// referenced by no band and is never emitted.
	Check( m->numPoints() == (unsigned int)( 2 + N ),
		"dead row: MONEY ASSERTION -- the unreferenced on-axis row is NOT emitted (it would add a third pole vertex)" );
	Check( m->getFaces().size() == (size_t)( 2 * N ), "dead row: two pole fans of n_radial" );

	const BoundingBox bb = m->GenerateBoundingBox();
	Check( (double)bb.ll.y > -1.0 && std::fabs( (double)bb.ll.y ) < 1e-6,
		"dead row: MONEY ASSERTION -- the bbox LOWER bound is the live extent h = 0, not the dead row's h = -100" );
	Check( std::fabs( (double)bb.ur.y - 2.0 ) < 1e-6,
		"dead row: the bbox upper bound is the live extent h = 2" );
	pi->release();
}

// (15) THE LOOP TOLERANCE IS PINNED IN BOTH DIRECTIONS.  Test 5k red-proves
// TIGHTENING it to `==` (a generated ring must still be seen as closed).
// Nothing pinned LOOSENING it, because no fixture had ends separated by a
// distance between the tolerance and something visible -- a tolerance of 1e-3
// would have passed every test while silently dropping the closing point of
// any profile that stops a micron short.  This fixture stops 1e-6 short:
// genuinely OPEN at the scale of the part, so the closing point must be KEPT
// and the cross-section closed through the axis around it.
static void TestLatheNearLoopIsNotALoop()
{
	std::cout << "Test 5p: lathe_geometry -- a profile that closes to within 1e-6 is OPEN (the closing point is retained)" << std::endl;
	const int N = 6;
	const double gap = 1e-6;
	// a unit box section 1..2 in r, 0..1 in h, walked all the way round but
	// stopping `gap` short of its own start.
	const double prof[] = { 1.0, 0.0,   2.0, 0.0,   2.0, 1.0,   1.0, 1.0,   1.0, gap };
	LatheDescriptor d;
	d.profilePoints = prof; d.numProfilePoints = 5;
	d.nRadial = N;
	d.sweepDegrees = 90.0;

	ITriangleMeshGeometryIndexed* pi = 0;
	Check( RISE_API_CreateLatheGeometry( &pi, d ), "near-loop: factory succeeds" );
	if( !pi ) return;
	const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
	if( !m ) { Check( false, "near-loop: concrete type" ); pi->release(); return; }

	// 5 profile rows of N+1 columns, and a cap of 7 cross-section vertices
	// (the 5 profile points plus the two axis-spur points) on each side.  A
	// tolerance loose enough to call this a LOOP would drop the closing point
	// and cap a 4-gon instead -- 2*4 cap vertices, not 2*7.
	Check( m->numPoints() == (unsigned int)( 5 * ( N + 1 ) + 2 * 7 ),
		"near-loop: MONEY ASSERTION -- the 1e-6 closing point is RETAINED and the cross-section closes through the AXIS around it" );
	Check( m->getFaces().size() == (size_t)( 4 * 2 * N + 2 * 5 ),
		"near-loop: four side bands plus two 5-triangle ear-clipped caps (a 7-gon, not a 4-gon)" );
	pi->release();
}

// (16) FACTORY-LEVEL VALIDATION.  The chunk parser owns the author-facing
// diagnostics (GuillocheChunkParseTest covers those); these pin the
// factory's own refusals, which any direct API caller hits.
static void TestLatheValidation()
{
	std::cout << "Test 5h: lathe_geometry -- factory validation" << std::endl;
	ITriangleMeshGeometryIndexed* pi = 0;
	{
		const double prof[] = { 1.0, 0.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 1;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: a single profile point rejects" );
	}
	{
		const double prof[] = { -1.0, 0.0,  1.0, 1.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: a negative radius rejects" );
	}
	{
		const double prof[] = { 0.0, 0.0,  0.0, 1.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: an all-on-axis profile rejects" );
	}
	{
		const double prof[] = { 1.0, 0.0,  1.0, 1.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2; d.sweepDegrees = 0.0;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: sweep_degrees 0 rejects" );
		d.sweepDegrees = 361.0;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: sweep_degrees > 360 rejects" );
	}
	{
		const double prof[] = { 1.0, 0.0,  1.0, 1.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2; d.axis = 3;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: an out-of-range axis rejects" );
	}
	// NON-FINITE profile components.  `r >= 0` rejects a NaN (it fails the
	// compare) but PASSES +inf, and h was never checked at all: an infinite
	// h makes every segment length inf, so the segment reads LIVE with a NaN
	// normal and the factory emits inf vertices with NaN texcoords for the
	// BVH to build over.  The chunk parser's token gate covers scene text;
	// these pin the direct RISE_API.h caller.
	{
		const double inf = std::numeric_limits<double>::infinity();
		const double prof[] = { 1.0, 0.0,  1.0, inf };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0,
			"validation: MONEY ASSERTION -- an INFINITE profile height rejects (it would otherwise reach the BVH as inf vertices and NaN texcoords)" );
	}
	{
		const double nan = std::numeric_limits<double>::quiet_NaN();
		const double prof[] = { 1.0, nan,  1.0, 1.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: a NaN profile height rejects" );
	}
	{
		const double inf = std::numeric_limits<double>::infinity();
		const double prof[] = { inf, 0.0,  1.0, 1.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0,
			"validation: an INFINITE profile radius rejects (the `>= 0` sign test alone passes +inf)" );
	}
	// Profile-point cap.
	{
		std::vector<double> prof( 2 * 5000 );
		for( int k = 0; k < 5000; ++k ) { prof[ 2*k ] = 1.0; prof[ 2*k + 1 ] = 0.001 * k; }
		LatheDescriptor d; d.profilePoints = &prof[0]; d.numProfilePoints = 5000;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0, "validation: more than 4096 profile points rejects" );
	}
	// Vertex budget.  A guard has to be REACHABLE to be a guard: 1001 rows x
	// 2049 columns = 2.05M, just over the 2M ceiling.  (The original 20M
	// ceiling could never fire -- the profile-point and n_radial clamps top
	// out at 8190 x 2049 = 16.8M.)
	{
		std::vector<double> prof( 2 * 1001 );
		for( int k = 0; k < 1001; ++k ) { prof[ 2*k ] = 1.0; prof[ 2*k + 1 ] = 0.01 * k; }
		LatheDescriptor d;
		d.profilePoints = &prof[0]; d.numProfilePoints = 1001;
		d.nRadial = 2048; d.sweepDegrees = 90.0;
		Check( !RISE_API_CreateLatheGeometry( &pi, d ) && pi == 0,
			"validation: MONEY ASSERTION -- the ring-vertex budget REJECTS a REACHABLE request (1001 rows x 2049 columns)" );
	}
	// n_radial CLAMPS (with a warning) rather than rejecting
	{
		const double prof[] = { 1.0, 0.0,  1.0, 1.0 };
		LatheDescriptor d; d.profilePoints = prof; d.numProfilePoints = 2; d.nRadial = 1;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "validation: n_radial 1 CLAMPS to 3 rather than rejecting" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			Check( m && m->numPoints() == 6, "validation: n_radial 1 clamped to the minimum 3 columns" );
			pi->release();
			pi = 0;
		}
	}
	// smooth FALSE: two rows per live segment, each flat
	{
		const int N = 8;
		const double prof[] = { 1.0, 0.0,  1.0, 1.0,  0.5, 2.0 };
		LatheDescriptor d;
		d.profilePoints = prof; d.numProfilePoints = 3; d.nRadial = N; d.smooth = false;
		Check( RISE_API_CreateLatheGeometry( &pi, d ), "smooth FALSE factory succeeds" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			Check( m && m->numPoints() == (unsigned int)( 4 * N ),
				"smooth FALSE: two rows per profile segment (faceted), so 2*segments*n_radial vertices" );
			// the two rows of the FIRST segment share one flat normal
			if( m ) {
				const Normal& a = m->getNormals()[0];
				const Normal& b = m->getNormals()[ N ];
				const Scalar dot = a.x*b.x + a.y*b.y + a.z*b.z;
				Check( dot > 1.0 - 1e-12, "smooth FALSE: both rows of a segment carry that segment's own flat normal" );
			}
			pi->release();
			pi = 0;
		}
	}
}


//////////////////////////////////////////////////////////////////////
//
//  skin_geometry (doc 89 slice B) vs FIRST PRINCIPLES.  Every assertion
//  below is CLOSED FORM -- an exact planar quad and its exact area, an
//  exact plane the whole mesh must lie in, an exact billow amplitude at
//  the mid-line, an exact perimeter edge count -- never "whatever the
//  factory produced last time".
//
//  Parameterization recap (see RISE_API_CreateSkinGeometry's header):
//  u runs ALONG the rails (normalized arc length = the U texcoord), v
//  runs ACROSS (0 at rail A, 1 at rail B = the V texcoord), stations are
//  the UNION of both rails' authored arc-length parameters plus the
//  n_len uniform refinements that are not already served, and the mesh
//  is emitted station-major: vertex index = station*n_across + row.
//
//////////////////////////////////////////////////////////////////////

// Build a skin from two rail point lists.  Returns the concrete mesh (or
// 0), and leaves ownership with the caller through `pi`.
static const TriangleMeshGeometryIndexed* MakeSkin(
		ITriangleMeshGeometryIndexed*& pi,
		const std::vector<double>& railA, const std::vector<double>& railB,
		const int nLen, const int nAcross, const double billow )
{
	SkinDescriptor d;
	d.railAPoints    = &railA[0];
	d.numRailAPoints = (unsigned int)( railA.size() / 3 );
	d.railBPoints    = &railB[0];
	d.numRailBPoints = (unsigned int)( railB.size() / 3 );
	d.nLen    = nLen;
	d.nAcross = nAcross;
	d.billow  = billow;
	pi = 0;
	if( !RISE_API_CreateSkinGeometry( &pi, d ) ) {
		return 0;
	}
	return dynamic_cast<const TriangleMeshGeometryIndexed*>( pi );
}

// Is `p` present in the mesh's vertex buffer within `tol`?  This is the
// CORNER-PRESENCE instrument, and it is deliberately a presence test
// rather than an extents test: slice A's round-1 P1 was a resampler that
// CHAMFERED authored corners while leaving the bounding box (and so an
// extents assertion) untouched.
static bool SkinHasVertex( const TriangleMeshGeometryIndexed* m,
		const double x, const double y, const double z, const Scalar tol )
{
	for( unsigned int i = 0; i < m->numPoints(); ++i ) {
		const Vertex& p = m->getVertices()[i];
		if( std::fabs( p.x - x ) <= tol && std::fabs( p.y - y ) <= tol && std::fabs( p.z - z ) <= tol ) {
			return true;
		}
	}
	return false;
}

// BOUNDARY EDGES: undirected edges used by exactly ONE triangle.  An open
// sheet has no watertightness invariant to check, so this is its
// structural analogue -- the perimeter must be exactly the perimeter, and
// nothing inside the sheet may be torn.
static unsigned int SkinBoundaryEdgeCount( const TriangleMeshGeometryIndexed* m )
{
	const Vertex* base = m->getVertices().empty() ? 0 : &m->getVertices()[0];
	std::vector< std::pair<unsigned int,unsigned int> > edges;
	edges.reserve( m->getFaces().size() * 3 );
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		unsigned int idx[3];
		for( int k = 0; k < 3; ++k ) { idx[k] = (unsigned int)( face.pVertices[k] - base ); }
		for( int k = 0; k < 3; ++k ) {
			unsigned int a = idx[k], b = idx[ ( k + 1 ) % 3 ];
			if( a > b ) { const unsigned int t = a; a = b; b = t; }
			edges.push_back( std::make_pair( a, b ) );
		}
	}
	std::sort( edges.begin(), edges.end() );
	unsigned int boundary = 0;
	for( std::size_t i = 0; i < edges.size(); ) {
		std::size_t j = i;
		while( j < edges.size() && edges[j] == edges[i] ) { ++j; }
		if( j - i == 1 ) { ++boundary; }
		i = j;
	}
	return boundary;
}

// (1) RULED QUAD IDENTITY.  Two parallel straight rails, billow 0: an
// exact planar rectangle, with every count, every UV, every normal and
// the total area pinned in closed form.
static void TestSkinRuledQuadIdentity()
{
	std::cout << "Test 6: skin_geometry -- ruled quad identity (counts, UVs, normals, area, perimeter)" << std::endl;
	// rail A along +X at z = 0, rail B the same line at z = W.  Both are
	// 2-point rails, so the union contributes parameters {0, 1} only and
	// the station set is exactly the n_len uniform grid.
	const double L = 4.0, W = 3.0;
	std::vector<double> railA, railB;
	railA.push_back( 0 ); railA.push_back( 0 ); railA.push_back( 0 );
	railA.push_back( L ); railA.push_back( 0 ); railA.push_back( 0 );
	railB.push_back( 0 ); railB.push_back( 0 ); railB.push_back( W );
	railB.push_back( L ); railB.push_back( 0 ); railB.push_back( W );

	const int NL = 5, NA = 4;
	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, NL, NA, 0.0 );
	Check( m != 0, "skin quad: factory succeeds" );
	if( !m ) { if( pi ) pi->release(); return; }

	Check( m->numPoints() == (unsigned int)( NL * NA ),
		"skin quad: MONEY ASSERTION -- n_len stations x n_across rows vertices (the 2-point rails add no station the uniform grid does not already carry)" );
	Check( m->getFaces().size() == (std::size_t)( 2 * ( NL - 1 ) * ( NA - 1 ) ),
		"skin quad: two triangles per grid cell, none dropped" );

	// EXACT planarity: every vertex on y = 0.
	bool planar = true, uvOK = true, normalOK = true, gridOK = true;
	for( int i = 0; i < NL; ++i ) {
		for( int j = 0; j < NA; ++j ) {
			const unsigned int k = (unsigned int)( i * NA + j );
			const Vertex& p = m->getVertices()[k];
			const Normal& n = m->getNormals()[k];
			const TexCoord& c = m->getCoords()[k];
			if( std::fabs( p.y ) > 1e-12 ) planar = false;
			// station-major layout, exact ruled positions
			if( std::fabs( p.x - L * i / ( NL - 1 ) ) > 1e-12 ) gridOK = false;
			if( std::fabs( p.z - W * j / ( NA - 1 ) ) > 1e-12 ) gridOK = false;
			// U = arc length along the rails, V = 0 at rail A, 1 at rail B
			if( std::fabs( c.x - Scalar(i) / Scalar( NL - 1 ) ) > 1e-12 ) uvOK = false;
			if( std::fabs( c.y - Scalar(j) / Scalar( NA - 1 ) ) > 1e-12 ) uvOK = false;
			// dP/du = +X, dP/dv = +Z, so the normal is X x Z = -Y everywhere
			if( std::fabs( n.x ) > 1e-12 || std::fabs( n.z ) > 1e-12 || std::fabs( n.y + 1 ) > 1e-12 ) normalOK = false;
		}
	}
	Check( planar,   "skin quad: MONEY ASSERTION -- a planar pair of rails at billow 0 bakes an EXACTLY planar mesh" );
	Check( gridOK,   "skin quad: every vertex sits at its exact ruled position (station-major layout)" );
	Check( uvOK,     "skin quad: U == arc length along the rails, V == 0 at rail_a and 1 at rail_b" );
	Check( normalOK, "skin quad: every normal is exactly the plane normal d/du x d/dv, consistently oriented" );

	Scalar total = 0, minTri = 0;
	MeshAreaStats( m, total, minTri );
	Check( std::fabs( (double)total - L * W ) < 1e-9,
		"skin quad: total area EXACTLY the rectangle L*W" );
	Check( minTri > 1e-12, "skin quad: no zero-area triangles" );
	Check( MeshWindingAgreesWithNormals( m ),
		"skin quad: every face's winding agrees with the (independently pinned) normal field" );

	// PERIMETER.  An open sheet's structural invariant: the boundary is
	// exactly the four sides of the grid, so a tear anywhere inside (or a
	// dropped quad) moves this number.
	Check( SkinBoundaryEdgeCount( m ) == (unsigned int)( 2 * ( NL - 1 ) + 2 * ( NA - 1 ) ),
		"skin quad: MONEY ASSERTION -- boundary edge count is EXACTLY the sheet's perimeter, 2*(stations-1) + 2*(rows-1)" );

	// No duplicated vertices anywhere in the grid.
	bool anyDup = false;
	for( unsigned int a = 0; a < m->numPoints() && !anyDup; ++a ) {
		for( unsigned int b = a + 1; b < m->numPoints(); ++b ) {
			const Vertex& p = m->getVertices()[a];
			const Vertex& q = m->getVertices()[b];
			if( std::fabs( p.x - q.x ) < 1e-12 && std::fabs( p.y - q.y ) < 1e-12 && std::fabs( p.z - q.z ) < 1e-12 ) {
				anyDup = true; break;
			}
		}
	}
	Check( !anyDup, "skin quad: no duplicated interior vertices" );
	pi->release();
}

// (2) THE UNION RESAMPLE.  Two rails of DIFFERENT point counts, each with
// sharp authored kinks at parameters that do NOT land on the other rail's
// or on the uniform grid.  Every authored vertex of BOTH rails must be in
// the mesh VERBATIM.
//
// RED PROOF (slice A's round-1 P1, repeated here by construction): swap
// the union resampler for "pick N = max(nA, nB) and uniformly resample
// both" and this test fails -- the kinks survive only where they happen
// to land on the j/N grid, which for these deliberately irrational-ish
// arc-length fractions is nowhere.  An EXTENTS assertion survives that
// substitution untouched, which is exactly why this is a PRESENCE test.
static void TestSkinUnionKeepsAuthoredVertices()
{
	std::cout << "Test 6b: skin_geometry -- union resample keeps EVERY authored rail vertex (corner PRESENCE)" << std::endl;
	// rail A: 5 points with two hard kinks.  rail B: 3 points, kinked
	// elsewhere.  Counts differ; no kink of one is a kink of the other.
	const double A[] = {
		0.0, 0.0, 0.0,
		1.3, 0.9, 0.0,
		2.1, 0.4, 0.0,
		3.4, 1.7, 0.0,
		4.0, 0.2, 0.0 };
	const double B[] = {
		0.0, 0.0, 2.5,
		1.9, 1.4, 2.5,
		4.0, 0.0, 2.5 };
	std::vector<double> railA( A, A + 15 ), railB( B, B + 9 );

	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 16, 5, 0.0 );
	Check( m != 0, "skin union: factory succeeds on unequal-count kinked rails" );
	if( !m ) { if( pi ) pi->release(); return; }

	int missA = 0, missB = 0;
	for( int k = 0; k < 5; ++k ) {
		if( !SkinHasVertex( m, A[3*k], A[3*k+1], A[3*k+2], Scalar(1e-12) ) ) ++missA;
	}
	for( int k = 0; k < 3; ++k ) {
		if( !SkinHasVertex( m, B[3*k], B[3*k+1], B[3*k+2], Scalar(1e-12) ) ) ++missB;
	}
	Check( missA == 0,
		"skin union: MONEY ASSERTION -- every authored rail_a vertex is in the mesh VERBATIM (a uniform max-N resample loses the interior kinks)" );
	Check( missB == 0,
		"skin union: MONEY ASSERTION -- every authored rail_b vertex is in the mesh VERBATIM, at a DIFFERENT point count from rail_a" );

	// The union bound: max(nA, nB) <= stations <= nA + nB + n_len.
	const unsigned int rows = 5;
	const unsigned int stations = m->numPoints() / rows;
	Check( m->numPoints() % rows == 0, "skin union: the grid is rectangular (vertex count divides by n_across)" );
	Check( stations >= 5 && stations <= 5 + 3 + 16,
		"skin union: station count is bounded by the union rule (>= the larger rail, <= both rails plus the refinement grid)" );
	Check( SkinBoundaryEdgeCount( m ) == 2 * ( stations - 1 ) + 2 * ( rows - 1 ),
		"skin union: the perimeter is still exactly the perimeter at unequal rail counts" );
	pi->release();
}

// (3) THE HARD-EDGE IDIOM.  A rail point DUPLICATED produces a zero-length
// segment, hence two stations at the same parameter, hence two vertex
// columns carrying the two sides' own normals -- the same crease idiom
// sweep_geometry and lathe_geometry document for their profiles.
static void TestSkinDuplicatedRailPointCrease()
{
	std::cout << "Test 6c: skin_geometry -- a duplicated rail point splits the normals (crease idiom)" << std::endl;
	// A rail that turns 90 degrees, with the corner point DUPLICATED.
	const double A[] = {
		0.0, 0.0, 0.0,
		2.0, 0.0, 0.0,
		2.0, 0.0, 0.0,
		2.0, 2.0, 0.0 };
	const double B[] = {
		0.0, 0.0, 1.0,
		2.0, 0.0, 1.0,
		2.0, 0.0, 1.0,
		2.0, 2.0, 1.0 };
	std::vector<double> railA( A, A + 12 ), railB( B, B + 12 );

	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 2, 2, 0.0 );
	Check( m != 0, "skin crease: factory succeeds" );
	if( !m ) { if( pi ) pi->release(); return; }

	// 4 authored parameters per rail, pairing exactly (identical arc-length
	// fractions), and n_len 2 contributes only the two endpoints -> 4
	// stations, 2 rows.
	Check( m->numPoints() == 8,
		"skin crease: the duplicated point is KEPT as its own station (4 stations x 2 rows), not collapsed" );
	if( m->numPoints() == 8 ) {
		// stations 1 and 2 are the two copies of the corner: coincident
		// positions, DIFFERENT normals (one per side of the crease).
		const Vertex& p1 = m->getVertices()[ 1 * 2 ];
		const Vertex& p2 = m->getVertices()[ 2 * 2 ];
		const Normal& n1 = m->getNormals()[ 1 * 2 ];
		const Normal& n2 = m->getNormals()[ 2 * 2 ];
		Check( std::fabs( p1.x - p2.x ) < 1e-12 && std::fabs( p1.y - p2.y ) < 1e-12 && std::fabs( p1.z - p2.z ) < 1e-12,
			"skin crease: the two corner stations are coincident in POSITION" );
		const Scalar dot = n1.x*n2.x + n1.y*n2.y + n1.z*n2.z;
		Check( dot < 0.99,
			"skin crease: MONEY ASSERTION -- the two corner stations carry DIFFERENT normals, so the crease stays hard" );
	}
	pi->release();
}

// (4) BILLOW.  Closed form on a flat sheet whose normal is constant: the
// mid-line moves EXACTLY billow * span along it, the rails do not move at
// ALL, and a negated billow mirrors the displacement exactly.
static void TestSkinBillow()
{
	std::cout << "Test 6d: skin_geometry -- billow amplitude, rail invariance, sign convention" << std::endl;
	const double L = 4.0, W = 2.0;
	std::vector<double> railA, railB;
	railA.push_back( 0 ); railA.push_back( 0 ); railA.push_back( 0 );
	railA.push_back( L ); railA.push_back( 0 ); railA.push_back( 0 );
	railB.push_back( 0 ); railB.push_back( 0 ); railB.push_back( W );
	railB.push_back( L ); railB.push_back( 0 ); railB.push_back( W );

	const int NL = 4, NA = 5;			// NA odd -> a row lands exactly on v = 0.5
	const double amt = 0.3;
	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, NL, NA, amt );
	Check( m != 0, "skin billow: factory succeeds" );
	if( !m ) { if( pi ) pi->release(); return; }

	bool railsFixed = true, midOK = true, falloffOK = true;
	for( int i = 0; i < NL; ++i ) {
		const Vertex& a = m->getVertices()[ (unsigned int)( i * NA + 0 ) ];
		const Vertex& b = m->getVertices()[ (unsigned int)( i * NA + NA - 1 ) ];
		// BIT-EXACT: the rails are written as their own endpoints, never
		// through a weight that merely evaluates to zero.
		if( a.y != 0 || b.y != 0 || a.z != 0 || std::fabs( b.z - W ) > 1e-15 ) railsFixed = false;
		const Vertex& mid = m->getVertices()[ (unsigned int)( i * NA + ( NA - 1 ) / 2 ) ];
		// The flat sheet's normal is X x Z = -Y, so a POSITIVE billow moves
		// the interior to -Y by exactly billow * span * sin^2(pi/2).
		if( std::fabs( mid.y + amt * W ) > 1e-12 ) midOK = false;
		// v = 0.25 and v = 0.75 rows: sin^2(pi/4) = sin^2(3pi/4) = 0.5
		const Vertex& q1 = m->getVertices()[ (unsigned int)( i * NA + 1 ) ];
		const Vertex& q3 = m->getVertices()[ (unsigned int)( i * NA + 3 ) ];
		if( std::fabs( q1.y + amt * W * 0.5 ) > 1e-12 ) falloffOK = false;
		if( std::fabs( q3.y + amt * W * 0.5 ) > 1e-12 ) falloffOK = false;
	}
	Check( railsFixed, "skin billow: MONEY ASSERTION -- both authored rails are BIT-EXACTLY unmoved at any billow" );
	Check( midOK,      "skin billow: the mid-line moves EXACTLY billow * span along the ruled sheet's normal" );
	Check( falloffOK,  "skin billow: the falloff is exactly sin^2(pi*v) (0.5 of full amplitude at v = 0.25 and v = 0.75)" );
	pi->release();

	// NEGATIVE billow: the exact mirror.
	ITriangleMeshGeometryIndexed* pi2 = 0;
	const TriangleMeshGeometryIndexed* m2 = MakeSkin( pi2, railA, railB, NL, NA, -amt );
	Check( m2 != 0, "skin billow: negative billow builds" );
	if( m2 ) {
		bool mirrored = true;
		for( int i = 0; i < NL; ++i ) {
			const Vertex& mid = m2->getVertices()[ (unsigned int)( i * NA + ( NA - 1 ) / 2 ) ];
			if( std::fabs( mid.y - amt * W ) > 1e-12 ) mirrored = false;
		}
		Check( mirrored, "skin billow: a NEGATIVE billow inflates to the OTHER side by the same amount" );
		pi2->release();
	}

	// n_across 2 has no interior row, so billow is inert (and warns).
	ITriangleMeshGeometryIndexed* pi3 = 0;
	const TriangleMeshGeometryIndexed* m3 = MakeSkin( pi3, railA, railB, NL, 2, amt );
	Check( m3 != 0, "skin billow: n_across 2 still builds" );
	if( m3 ) {
		bool flat = true;
		for( unsigned int k = 0; k < m3->numPoints(); ++k ) {
			if( std::fabs( m3->getVertices()[k].y ) > 1e-15 ) flat = false;
		}
		Check( flat, "skin billow: at n_across 2 the sheet is the two rails alone, so billow is inert (warned, not refused)" );
		pi3->release();
	}
}

// (5) THE BILLOWED SHEET'S NORMALS come from the DISPLACED positions, not
// from the flat sheet the displacement was measured against -- otherwise a
// billowed membrane shades as though it were still flat.
static void TestSkinBillowRecomputesNormals()
{
	std::cout << "Test 6e: skin_geometry -- a billowed sheet's normals follow the DISPLACED surface" << std::endl;
	std::vector<double> railA, railB;
	railA.push_back( 0 ); railA.push_back( 0 ); railA.push_back( 0 );
	railA.push_back( 4 ); railA.push_back( 0 ); railA.push_back( 0 );
	railB.push_back( 0 ); railB.push_back( 0 ); railB.push_back( 2 );
	railB.push_back( 4 ); railB.push_back( 0 ); railB.push_back( 2 );

	const int NL = 4, NA = 9;
	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, NL, NA, 0.3 );
	Check( m != 0, "skin billow normals: factory succeeds" );
	if( !m ) { if( pi ) pi->release(); return; }

	// The base sheet lies in the XZ plane with du = +X and dv = +Z, so its
	// normal is X x Z = -Y and a positive billow pushes the interior to -Y.
	// On the displaced surface dP/dv tips DOWN near rail A and UP near rail
	// B, and N = du x dv carries that as a Z component of opposite sign on
	// the two flanks (closed form: -0.514 and +0.514 at these rows).  A
	// normal field left over from the FLAT sheet would be -Y everywhere, so
	// both of these would read exactly 0.
	const Normal& nearA = m->getNormals()[ (unsigned int)( 1 * NA + 1 ) ];
	const Normal& nearB = m->getNormals()[ (unsigned int)( 1 * NA + NA - 2 ) ];
	Check( nearA.z < -0.05 && nearB.z > 0.05,
		"skin billow normals: MONEY ASSERTION -- the emitted normals tilt with the DISPLACED surface (a flat-sheet normal field would read exactly 0 here)" );
	Check( std::fabs( nearA.z + nearB.z ) < 1e-9,
		"skin billow normals: the sin^2 falloff is symmetric, so the two flank tilts are exact mirrors" );
	Check( MeshWindingAgreesWithNormals( m ),
		"skin billow normals: the winding still agrees with the recomputed normal field" );
	pi->release();
}

// (6) A LEAF: rails that MEET at both ends.  The degenerate quads at the
// two tips must be DROPPED, not emitted as slivers -- and the rest of the
// sheet must be intact.
static void TestSkinPinchedLeaf()
{
	std::cout << "Test 6f: skin_geometry -- rails meeting at both ends (a leaf) drop the degenerate quads" << std::endl;
	const double A[] = {  0.0, 0.0, 0.0,   2.0, 0.0, 1.0,   4.0, 0.0, 0.0 };
	const double B[] = {  0.0, 0.0, 0.0,   2.0, 0.0, -1.0,  4.0, 0.0, 0.0 };
	std::vector<double> railA( A, A + 9 ), railB( B, B + 9 );

	const int NA = 5;
	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 8, NA, 0.0 );
	Check( m != 0, "skin leaf: factory succeeds on rails that touch at both ends" );
	if( !m ) { if( pi ) pi->release(); return; }

	Scalar total = 0, minTri = 0;
	MeshAreaStats( m, total, minTri );
	Check( minTri > 1e-12,
		"skin leaf: MONEY ASSERTION -- NO zero-area triangles, even though the two rails are coincident at both tips" );
	// The leaf is two triangles' worth of area on each side of the midline:
	// rail A rises to z = +1 at x = 2, rail B falls to z = -1, so the sheet
	// is the quadrilateral (0,0)-(2,1)-(4,0)-(2,-1) in the XZ plane: two
	// triangles of base 4 and height 1 -> area 4.
	Check( std::fabs( (double)total - 4.0 ) < 1e-9,
		"skin leaf: total area is EXACTLY the spanned quadrilateral (nothing lost with the dropped tip quads)" );
	pi->release();
}

// (7) VALIDATION.  Every refusal path, plus the two CLAMP paths.
static void TestSkinValidation()
{
	std::cout << "Test 6g: skin_geometry -- factory validation" << std::endl;
	const double inf = std::numeric_limits<double>::infinity();
	const double nan = std::numeric_limits<double>::quiet_NaN();
	ITriangleMeshGeometryIndexed* pi = 0;

	const double okA[] = { 0.0, 0.0, 0.0,  4.0, 0.0, 0.0 };
	const double okB[] = { 0.0, 0.0, 2.0,  4.0, 0.0, 2.0 };

	{
		SkinDescriptor d; d.railAPoints = okA; d.numRailAPoints = 1; d.railBPoints = okB; d.numRailBPoints = 2;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: a single rail_a point rejects" );
	}
	{
		SkinDescriptor d; d.railAPoints = okA; d.numRailAPoints = 2; d.railBPoints = okB; d.numRailBPoints = 1;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: a single rail_b point rejects" );
	}
	{
		SkinDescriptor d; d.railAPoints = 0; d.numRailAPoints = 2; d.railBPoints = okB; d.numRailBPoints = 2;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: a null rail_a pointer rejects" );
	}
	// ZERO-LENGTH rail: every point coincident.
	{
		const double deg[] = { 1.0, 1.0, 1.0,  1.0, 1.0, 1.0,  1.0, 1.0, 1.0 };
		SkinDescriptor d; d.railAPoints = deg; d.numRailAPoints = 3; d.railBPoints = okB; d.numRailBPoints = 2;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0,
			"validation: MONEY ASSERTION -- a ZERO-LENGTH rail_a (every point coincident) rejects" );
		SkinDescriptor e; e.railAPoints = okA; e.numRailAPoints = 2; e.railBPoints = deg; e.numRailBPoints = 3;
		Check( !RISE_API_CreateSkinGeometry( &pi, e ) && pi == 0, "validation: a ZERO-LENGTH rail_b rejects" );
	}
	// THE SAME CURVE on both rails -- a zero-area sheet.  Authored at
	// DIFFERENT point counts, so an element-wise comparison would miss it.
	{
		const double a[] = { 0.0, 0.0, 0.0,  4.0, 0.0, 0.0 };
		const double b[] = { 0.0, 0.0, 0.0,  1.0, 0.0, 0.0,  4.0, 0.0, 0.0 };
		SkinDescriptor d; d.railAPoints = a; d.numRailAPoints = 2; d.railBPoints = b; d.numRailBPoints = 3;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0,
			"validation: MONEY ASSERTION -- two rails describing the SAME curve (at different point counts) reject as a zero-area sheet" );
	}
	// NON-FINITE coordinates, on BOTH rails and on billow.
	{
		const double bad[] = { 0.0, 0.0, 2.0,  4.0, inf, 2.0 };
		SkinDescriptor d; d.railAPoints = okA; d.numRailAPoints = 2; d.railBPoints = bad; d.numRailBPoints = 2;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: an INFINITE rail_b coordinate rejects" );
	}
	{
		const double bad[] = { 0.0, nan, 0.0,  4.0, 0.0, 0.0 };
		SkinDescriptor d; d.railAPoints = bad; d.numRailAPoints = 2; d.railBPoints = okB; d.numRailBPoints = 2;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: a NaN rail_a coordinate rejects" );
	}
	{
		SkinDescriptor d; d.railAPoints = okA; d.numRailAPoints = 2; d.railBPoints = okB; d.numRailBPoints = 2;
		d.billow = nan;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: a NaN billow rejects" );
		d.billow = inf;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: an INFINITE billow rejects" );
	}
	// Rail point cap.
	{
		std::vector<double> big( 3 * 5000 );
		for( int k = 0; k < 5000; ++k ) { big[ 3*k ] = 0.001 * k; big[ 3*k + 1 ] = 0; big[ 3*k + 2 ] = 0; }
		SkinDescriptor d; d.railAPoints = &big[0]; d.numRailAPoints = 5000; d.railBPoints = okB; d.numRailBPoints = 2;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0, "validation: more than 4096 points on a rail rejects" );
	}
	// Vertex budget: 4096 stations x 1024 rows = 4.19M, over the 2M ceiling.
	{
		SkinDescriptor d; d.railAPoints = okA; d.numRailAPoints = 2; d.railBPoints = okB; d.numRailBPoints = 2;
		d.nLen = 4096; d.nAcross = 1024;
		Check( !RISE_API_CreateSkinGeometry( &pi, d ) && pi == 0,
			"validation: MONEY ASSERTION -- the sheet-vertex budget REJECTS a REACHABLE request (4096 stations x 1024 rows)" );
	}
	// n_len / n_across CLAMP (with a warning) rather than rejecting.
	{
		SkinDescriptor d; d.railAPoints = okA; d.numRailAPoints = 2; d.railBPoints = okB; d.numRailBPoints = 2;
		d.nLen = 0; d.nAcross = 1;
		Check( RISE_API_CreateSkinGeometry( &pi, d ), "validation: n_len 0 / n_across 1 CLAMP rather than rejecting" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			Check( m && m->numPoints() == 4, "validation: n_len 0 -> 2 stations and n_across 1 -> 2 rows (the 2x2 minimum sheet)" );
			pi->release();
			pi = 0;
		}
	}
	{
		SkinDescriptor d; d.railAPoints = okA; d.numRailAPoints = 2; d.railBPoints = okB; d.numRailBPoints = 2;
		d.nLen = 100000; d.nAcross = 2;
		Check( RISE_API_CreateSkinGeometry( &pi, d ), "validation: an absurd n_len clamps rather than rejecting" );
		if( pi ) {
			const TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( pi );
			Check( m && m->numPoints() == 4096 * 2, "validation: n_len 100000 clamped to the 4096 maximum" );
			pi->release();
			pi = 0;
		}
	}
}


// (8) THE TWO-SIDED DECISION, pinned.  The skin bakes ONE sheet with the
// mesh flagged DOUBLE-SIDED, which is only the right call if a face
// shades correctly from EITHER side -- i.e. if the rail ORDER (which is
// what decides which way the surface normal points) never creates a side
// that is black.
//
// WHAT EACH ASSERTION BELOW ACTUALLY PINS, stated precisely because two
// of the three are weaker than they look:
//   * `n1 == -n2` pins that rail ORDER is what sets the normal -- the
//     PREMISE.  Without it the rest could be a tautology on two meshes
//     that turned out identical.
//   * `ok1 && ok2` -- both meshes hand the shader a shading AND geometric
//     normal opposing the ray -- is a property of ANY double-sided mesh,
//     since IntersectRay re-orients both toward the incoming ray
//     unconditionally.  It pins that the skin IS flagged double-sided and
//     that its normals are non-degenerate; it does NOT by itself pin
//     anything skin-specific.
//   * `flip1 != flip2` is the skin-specific one: the flip fires on
//     exactly ONE of the two, i.e. the two meshes really are opposite
//     faces of the same surface and it is the double-sided flag -- not
//     some accident of rail order -- that makes both shade.
//
// All three are on what a SHADER ACTUALLY RECEIVES (the post-flip
// ri.vNormal / ri.vGeomNormal), never on the raw triangle orientation: a
// test that undid the flip would assert the opposite of the property
// under test.  The render companion is
// scenes/Tests/Geometry/skin_stress.RISEscene's two-sided pair.
static bool SkinShadingFrontFacing( const IGeometry* g, const Point3& o, const Vector3& d, bool& outFlipped )
{
	RayIntersectionGeometric ri( Ray( o, d ), nullRasterizerState );
	g->IntersectRay( ri, true, true, false );
	outFlipped = false;
	if( !ri.bHit ) {
		return false;
	}
	outFlipped = ri.bGeomNormalOrientedToRay;
	// Both the shading normal a BSDF integrates against and the geometric
	// normal every side test reads must oppose the incoming ray.
	return Vector3Ops::Dot( ri.vNormal, ri.ray.Dir() ) < 0
	    && Vector3Ops::Dot( ri.vGeomNormal, ri.ray.Dir() ) < 0;
}

static void TestSkinTwoSided()
{
	std::cout << "Test 6h: skin_geometry -- the two-sided decision (rail order never makes a black side)" << std::endl;
	// A flat sheet in the XZ plane, spanned along +X and across +Z.
	const double A[] = { -1.0, 0.0, 0.0,   1.0, 0.0, 0.0 };
	const double B[] = { -1.0, 0.0, 2.0,   1.0, 0.0, 2.0 };
	std::vector<double> railA( A, A + 6 ), railB( B, B + 6 );
	std::vector<double> swapA( B, B + 6 ), swapB( A, A + 6 );

	ITriangleMeshGeometryIndexed* pi = 0;
	ITriangleMeshGeometryIndexed* pj = 0;
	const TriangleMeshGeometryIndexed* m  = MakeSkin( pi, railA, railB, 4, 4, 0.0 );
	const TriangleMeshGeometryIndexed* m2 = MakeSkin( pj, swapA, swapB, 4, 4, 0.0 );
	Check( m != 0 && m2 != 0, "skin two-sided: both rail orders build" );
	if( !m || !m2 ) { if( pi ) pi->release(); if( pj ) pj->release(); return; }

	// The two normal fields are exact opposites -- that is the PREMISE of
	// the test, not its conclusion, and it is asserted so a future change
	// that quietly made rail order irrelevant could not turn the assertion
	// below into a tautology.
	const Normal& n1 = m->getNormals()[0];
	const Normal& n2 = m2->getNormals()[0];
	Check( std::fabs( n1.y + n2.y ) < 1e-12 && std::fabs( n1.y ) > 0.99,
		"skin two-sided: swapping rail_a and rail_b gives EXACTLY the opposite surface normal" );

	// One ray, fired at the sheet's middle from +Y, at BOTH meshes.  The
	// rails put rail_a-first's own normal at -Y, i.e. ALONG this ray, so
	// this ray hits one sheet's "front" and the other's "back".
	const Point3  o( 0.0, 3.0, 1.0 );
	const Vector3 d( 0.0, -1.0, 0.0 );
	bool flip1 = false, flip2 = false;
	const bool ok1 = SkinShadingFrontFacing( pi, o, d, flip1 );
	const bool ok2 = SkinShadingFrontFacing( pj, o, d, flip2 );
	Check( ok1 && ok2,
		"skin two-sided: MONEY ASSERTION -- BOTH rail orders hand the shader a shading normal AND a geometric normal that oppose the ray, so an opaque membrane has no black side" );
	Check( flip1 != flip2,
		"skin two-sided: MONEY ASSERTION -- and it is the double-sided FLIP that does it: the flag fires on exactly ONE of the two (the one whose authored normal points away), which is what makes their shading identical" );
	pi->release();
	pj->release();
}

//////////////////////////////////////////////////////////////////////
//
//  skin_geometry, ROUND-2 REVIEW FIXES.  Every test below pins a defect
//  that the round-1 suite could not see because it only ever exercised
//  the FLAT quad and the STRAIGHT-rail specimens -- nowhere near the
//  creased, pinched and unevenly-parameterized skins the scene actually
//  ships.
//
//////////////////////////////////////////////////////////////////////

// Vertices no triangle references.  A procedural bake has no business
// leaving any: they inflate the buffer, they can inflate the BVH, and
// (as the pinched skins did) they are the visible symptom of a mesh
// whose fan was torn apart into unshared coincident copies.
static unsigned int MeshOrphanVertexCount( const TriangleMeshGeometryIndexed* m )
{
	const unsigned int nv = m->numPoints();
	if( nv == 0 ) return 0;
	const Vertex* base = &m->getVertices()[0];
	std::vector<unsigned char> used( nv, 0 );
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		for( int k = 0; k < 3; ++k ) { used[ (unsigned int)( face.pVertices[k] - base ) ] = 1; }
	}
	unsigned int orphans = 0;
	for( unsigned int i = 0; i < nv; ++i ) { if( !used[i] ) ++orphans; }
	return orphans;
}

// Vertices sharing a position with an earlier vertex.  On a skin this is
// never legitimate EXCEPT at the duplicated-rail-point crease idiom
// (where two stations deliberately coincide so their normals can split),
// so the tests below use it only on specimens that carry no crease.
static unsigned int MeshDuplicatePositionCount( const TriangleMeshGeometryIndexed* m )
{
	const unsigned int nv = m->numPoints();
	unsigned int dups = 0;
	for( unsigned int i = 0; i < nv; ++i ) {
		for( unsigned int j = 0; j < i; ++j ) {
			const Vertex& p = m->getVertices()[i];
			const Vertex& q = m->getVertices()[j];
			if( std::fabs( p.x - q.x ) < 1e-12 && std::fabs( p.y - q.y ) < 1e-12 && std::fabs( p.z - q.z ) < 1e-12 ) {
				++dups;
				break;
			}
		}
	}
	return dups;
}

// How many faces have their winding INVERTED against the shipped normal
// field?  MeshWindingAgreesWithNormals answers yes/no; a fold count is
// what the billow diagnostic reports, so the tests want the number.
static unsigned int MeshFoldedFaceCount( const TriangleMeshGeometryIndexed* m, double& worstCos )
{
	unsigned int folded = 0;
	worstCos = 1.0;
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		const Point3& a = *face.pVertices[0];
		const Point3& b = *face.pVertices[1];
		const Point3& c = *face.pVertices[2];
		const double ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
		const double vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
		const double gx = uy*vz - uz*vy, gy = uz*vx - ux*vz, gz = ux*vy - uy*vx;
		const Normal& n0 = *face.pNormals[0];
		const Normal& n1 = *face.pNormals[1];
		const Normal& n2 = *face.pNormals[2];
		const double rx = n0.x+n1.x+n2.x, ry = n0.y+n1.y+n2.y, rz = n0.z+n1.z+n2.z;
		const double gl = std::sqrt( gx*gx + gy*gy + gz*gz );
		const double rl = std::sqrt( rx*rx + ry*ry + rz*rz );
		const double d  = ( gl > 0 && rl > 0 ) ? ( ( gx*rx + gy*ry + gz*rz ) / ( gl * rl ) ) : 1.0;
		if( d <= 0 ) { ++folded; if( d < worstCos ) worstCos = d; }
	}
	return folded;
}

// The SHARPER fold measure, and the one the factory itself now uses: a
// face is folded if the shading normal opposes its geometric normal at
// ANY CORNER, not merely when the three-normal SUM does.  The shading
// normal inside a face is the barycentric blend of its corners, and a
// linear function on a simplex takes its minimum at a vertex, so this is
// exactly "some ray hitting this face is handed an inverted geometric
// normal" -- where the sum tests the centroid alone.
//
// The difference is not academic and it is not symmetric: a COLLAPSED
// POLE ships ONE normal for a whole fan, so a fan with a single folded
// lobe hands each of its faces two sound corner normals and one bad one,
// and the sum outvotes the bad one.  MeshFoldedFaceCount reads 0 on
// exactly the specimen TestSkinFoldAtCollapsedApex builds.
//
// `outAtPosition` (when non-null) additionally counts the folded faces
// whose OFFENDING corner sits at `atPosition` -- the instrument for
// "the masked fold is the collapsed apex's".
static unsigned int MeshFoldedFaceCountPerCorner( const TriangleMeshGeometryIndexed* m, double& worstCos,
		const Point3* atPosition = 0, unsigned int* outAtPosition = 0 )
{
	unsigned int folded = 0;
	if( outAtPosition ) { *outAtPosition = 0; }
	worstCos = 1.0;
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		const Point3& a = *face.pVertices[0];
		const Point3& b = *face.pVertices[1];
		const Point3& c = *face.pVertices[2];
		const double ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
		const double vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
		double gx = uy*vz - uz*vy, gy = uz*vx - ux*vz, gz = ux*vy - uy*vx;
		const double gl = std::sqrt( gx*gx + gy*gy + gz*gz );
		if( gl > 0 ) { gx /= gl; gy /= gl; gz /= gl; }
		bool bad = false, badHere = false;
		for( int k = 0; k < 3; ++k ) {
			const Normal& n = *face.pNormals[k];
			const double d = gx*n.x + gy*n.y + gz*n.z;
			if( d < worstCos ) { worstCos = d; }
			if( d <= 0 ) {
				bad = true;
				const Point3& p = *face.pVertices[k];
				if( atPosition && std::fabs( p.x - atPosition->x ) < 1e-9
				               && std::fabs( p.y - atPosition->y ) < 1e-9
				               && std::fabs( p.z - atPosition->z ) < 1e-9 ) { badHere = true; }
			}
		}
		if( bad ) { ++folded; if( badHere && outAtPosition ) { ++(*outAtPosition); } }
	}
	return folded;
}

// The weakest |cos| between any vertex normal and the plane normal of a
// face it belongs to.  On a mesh built from PLANAR lobes this is exactly
// 1 when every vertex carries its own lobe's normal, and collapses toward
// 0 the moment one lobe is handed the other lobe's answer -- which is what
// a single shared normal at an interior pinch does.
static double MeshWorstNormalPlaneAlignment( const TriangleMeshGeometryIndexed* m )
{
	double worst = 1.0;
	for( std::size_t f = 0; f < m->getFaces().size(); ++f ) {
		const PointerPolygon_Template<3>& face = m->getFaces()[f];
		const Point3& a = *face.pVertices[0];
		const Point3& b = *face.pVertices[1];
		const Point3& c = *face.pVertices[2];
		const double ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
		const double vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
		double gx = uy*vz - uz*vy, gy = uz*vx - ux*vz, gz = ux*vy - uy*vx;
		const double gl = std::sqrt( gx*gx + gy*gy + gz*gz );
		if( gl > 0 ) { gx /= gl; gy /= gl; gz /= gl; }
		for( int k = 0; k < 3; ++k ) {
			const Normal& n = *face.pNormals[k];
			const double d = std::fabs( gx*n.x + gy*n.y + gz*n.z );
			if( d < worst ) { worst = d; }
		}
	}
	return worst;
}

// The SHIPPED specimens, verbatim from scenes/Tests/Geometry/skin_stress.RISEscene.
// Kept here as data so the tests below assert on the geometry the scene
// actually bakes, not on a simplified stand-in.
static void SkinKinkRails( std::vector<double>& A, std::vector<double>& B )
{
	const double a[] = { -1.7, 0.0,  0.0,   -0.6, 1.35, 0.0,   0.15, 0.55, 0.0,
	                      1.05, 1.75, 0.0,   1.7,  0.25, 0.0 };
	const double b[] = { -1.7, 0.15, -1.7,   0.35, 1.95, -1.7,  1.7,  0.35, -1.7 };
	A.assign( a, a + 15 );
	B.assign( b, b + 9 );
}
static void SkinWingRails( std::vector<double>& A, std::vector<double>& B )
{
	const double a[] = { 0.0, 0.35, 0.0,   0.9, 1.15, 0.05,  1.9, 1.85, 0.05,  2.6, 2.75, 0.05 };
	const double b[] = { 0.0, 0.35, 0.0,   1.2, 0.75, 0.02,  2.4, 0.95, 0.02,
	                     3.35, 1.25, 0.05, 3.15, 2.05, 0.05,  2.6, 2.75, 0.05 };
	A.assign( a, a + 12 );
	B.assign( b, b + 18 );
}

// A minimal ILogPrinter that records messages containing `needle`.  Same
// device (and the same never-removed installation) as
// tests/CstSourceInstanceTest.cpp's: RemoveAllPrinters would also kill the
// default stdout printer for everything that runs afterwards.
//
// WHY A TEST NEEDS THE LOG.  A folded skin BUILDS -- the factory returns
// the mesh and a true return code, exactly as an unfolded one does -- so
// no return value separates "detected and reported the fold" from "never
// looked".  The diagnostic IS the feature; the log is where it lands.
class SkinLogCapture : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	explicit SkinLogCapture( std::string needle ) : mNeedle( std::move( needle ) ) {}
	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( mNeedle ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMatches.push_back( msg );
		}
	}
	void Flush() override {}
	int MatchCount() const { std::lock_guard<std::mutex> lk( mMutex ); return (int)mMatches.size(); }
	std::string LastMatch() const { std::lock_guard<std::mutex> lk( mMutex ); return mMatches.empty() ? std::string() : mMatches.back(); }
protected:
	~SkinLogCapture() override {}
private:
	std::string                mNeedle;
	mutable std::mutex         mMutex;
	std::vector<std::string>   mMatches;
};

// (9) THE BILLOW FOLD.  `billow` displaces along the RULED sheet's normal
// by a distance set by the rail-to-rail SPAN, so across a sharp authored
// crease it can push the two sides of the crease through each other.  The
// recomputed normal field then disagrees with the triangle winding, and
// TriangleMeshGeometryIndexed::IntersectRay flips the TRUE face normal to
// agree with the (wrong) shading normal -- so vGeomNormal, which every
// side test reads (dielectric inside/outside, SMS chain physics), is
// INVERTED on those faces.
//
// Two halves, and both are load-bearing:
//   * the SHIPPED specimens (the scene's creased kink sheet at its
//     authored billow, its leaf-class and wing-class skins) must fold
//     NOWHERE.  Round 1 asserted MeshWindingAgreesWithNormals only on the
//     flat quad and the straight-rail billow sheet -- neither of which can
//     fold -- so it was green on a factory that folded the scene's own
//     marquee geometry.
//   * a DELIBERATELY over-billowed skin must SAY SO.  It is not refused
//     and `billow` is not clamped (the fold bound depends on the rails'
//     crease angle, so a clamp would silently change authored shapes);
//     the author is told instead.
static void TestSkinBillowFoldDiagnostic()
{
	std::cout << "Test 6i: skin_geometry -- a billow that FOLDS the sheet is detected and reported" << std::endl;

	SkinLogCapture* pFoldLogOwned = new SkinLogCapture( "FOLDS the sheet through itself" );
	RISE::GlobalLogPriv()->AddPrinter( pFoldLogOwned );
	SkinLogCapture* pFoldLog = pFoldLogOwned;		// AddPrinter addref'd; keep a raw read handle

	std::vector<double> kA, kB;
	SkinKinkRails( kA, kB );

	// --- the SHIPPED creased sheet, at the scene's own billow ---
	const int before = pFoldLog->MatchCount();
	{
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, kA, kB, 30, 8, 0.05 );
		Check( m != 0, "skin fold: the shipped creased sheet builds" );
		if( m ) {
			double worst = 1.0;
			const unsigned int folded = MeshFoldedFaceCount( m, worst );
			Check( folded == 0,
				"skin fold: MONEY ASSERTION -- the SHIPPED creased sheet (kink rails, billow 0.05) folds NOWHERE" );
			Check( MeshWindingAgreesWithNormals( m ),
				"skin fold: every face of the shipped creased sheet winds with its normals" );
		}
		if( pi ) pi->release();
	}
	Check( pFoldLog->MatchCount() == before,
		"skin fold: and it is SILENT about folds it does not have" );

	// --- the same rails, deliberately over-billowed ---
	{
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, kA, kB, 30, 8, 0.15 );
		Check( m != 0, "skin fold: an over-billowed sheet still BUILDS (a warning, not a refusal)" );
		if( m ) {
			double worst = 1.0;
			const unsigned int folded = MeshFoldedFaceCount( m, worst );
			Check( folded > 0 && worst < 0,
				"skin fold: the over-billowed sheet really does invert its normal field (the premise of the assertion below)" );
		}
		if( pi ) pi->release();
	}
	Check( pFoldLog->MatchCount() == before + 1,
		"skin fold: MONEY ASSERTION -- exactly ONE warning names the fold (RED PROOF: delete the detection in RISE_API_CreateSkinGeometry and this reads 0)" );
	{
		const std::string msg = pFoldLog->LastMatch();
		Check( msg.find( "face(s)" ) != std::string::npos && msg.find( "station" ) != std::string::npos,
			"skin fold: the warning states HOW MANY faces folded and WHERE the first one is" );
		Check( msg.find( "Reduce billow" ) != std::string::npos && msg.find( "n_len" ) != std::string::npos,
			"skin fold: the warning states the REMEDY (reduce billow, or raise n_len near the crease)" );
	}

	// --- the other two shipped classes, at their shipped billows ---
	{
		std::vector<double> wA, wB;
		SkinWingRails( wA, wB );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, wA, wB, 48, 14, 0.06 );
		Check( m != 0, "skin fold: the shipped wing membrane builds" );
		if( m ) {
			double worst = 1.0;
			// PER CORNER, not the three-normal SUM: a pinched-tip fan (both
			// ends of this wing are pinched) ships one normal for the whole
			// fan, and TestSkinFoldAtCollapsedApex proves the summed check
			// (what MeshWindingAgreesWithNormals uses) reads 0 on a mesh with
			// a genuinely folded lobe there.  This is the same criterion the
			// factory's own diagnostic at RISE_API.cpp:3701-3714 acts on.
			Check( MeshFoldedFaceCountPerCorner( m, worst ) == 0,
				"skin fold: MONEY ASSERTION -- the SHIPPED wing membrane (pinched at both ends, billow 0.06) folds nowhere either" );
		}
		if( pi ) pi->release();
	}
	{
		const double A[] = {  0.0, 0.0, 0.0,   2.0, 0.0, 1.0,   4.0, 0.0, 0.0 };
		const double B[] = {  0.0, 0.0, 0.0,   2.0, 0.0, -1.0,  4.0, 0.0, 0.0 };
		std::vector<double> railA( A, A + 9 ), railB( B, B + 9 );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 8, 5, 0.2 );
		Check( m != 0, "skin fold: the leaf-class skin builds" );
		if( m ) {
			double worst = 1.0;
			// Same reasoning as the wing above: both tips of a leaf are
			// pinched poles, so the per-corner check is the one that can
			// actually see a folded lobe there.
			Check( MeshFoldedFaceCountPerCorner( m, worst ) == 0,
				"skin fold: MONEY ASSERTION -- a leaf-class skin (rails meeting at both tips) folds nowhere at billow 0.2" );
		}
		if( pi ) pi->release();
	}
}

// (10) PINCHED TOPOLOGY.  Where the two rails MEET, the station's whole
// column of rows is ONE point.  Emitting it as n_across coincident copies
// and dropping the degenerate half of each quad TEARS the fan: successive
// tip triangles reference different coincident vertices, share no edge,
// and every one of those edges reads as a boundary edge -- while the rows
// nothing references at all stay in the buffer as orphans.  Measured
// before the fix: the shipped wing had 142 boundary edges against a
// perimeter of 120, 26 duplicate-position vertices, and 2 orphans; the
// leaf 26 against 22.
//
// The fix is the lathe's: a coincident row collapses to a SINGLE
// pole-style vertex, so the tip is a connected fan.
//
// THE CLOSED FORM.  An un-pinched sheet of S stations and R rows is a
// rectangular grid whose boundary is its perimeter, 2*(S-1) + 2*(R-1).
// Collapsing an END station to a point deletes that end's whole (R-1)-edge
// side (its edges are all zero-length and gone), leaving the two
// rail-length sides intact.  So:
//
//     boundary = 2*(S-1) + (R-1) * (number of UN-pinched end stations)
//
// -- 2*(S-1) + 2*(R-1) with no pinch, 2*(S-1) + (R-1) for a sail pinched
// at one corner, and 2*(S-1) for a leaf or wing pinched at both.  The
// vertex count drops by (R-1) per pinched station for the same reason.
static void TestSkinPinchedTopologyIsConnected()
{
	std::cout << "Test 6j: skin_geometry -- a pinched tip is ONE vertex, so the fan stays connected" << std::endl;

	// --- the leaf: rails meeting at BOTH tips ---
	{
		const double A[] = {  0.0, 0.0, 0.0,   2.0, 0.0, 1.0,   4.0, 0.0, 0.0 };
		const double B[] = {  0.0, 0.0, 0.0,   2.0, 0.0, -1.0,  4.0, 0.0, 0.0 };
		std::vector<double> railA( A, A + 9 ), railB( B, B + 9 );
		const int R = 5;
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 8, R, 0.0 );
		Check( m != 0, "skin pinch: the leaf builds" );
		if( m ) {
			// 8 stations (both rails' {0, 0.5, 1} plus the n_len 8 grid,
			// which already serves all three), both END stations pinched.
			const unsigned int S = 8;
			Check( m->numPoints() == S * (unsigned int)R - 2 * ( (unsigned int)R - 1 ),
				"skin pinch: each pinched tip costs ONE vertex, not n_across of them" );
			Check( SkinBoundaryEdgeCount( m ) == 2 * ( S - 1 ),
				"skin pinch: MONEY ASSERTION -- the leaf's boundary is EXACTLY its two rails, 2*(S-1) (was 26 against the un-pinched 22 when the fan was torn)" );
			Check( MeshOrphanVertexCount( m ) == 0,
				"skin pinch: MONEY ASSERTION -- no vertex is left in the buffer that no triangle references" );
			Check( MeshDuplicatePositionCount( m ) == 0,
				"skin pinch: MONEY ASSERTION -- no two vertices share a position (the tips are ONE vertex each, not n_across copies)" );
			Scalar total = 0, minTri = 0;
			MeshAreaStats( m, total, minTri );
			Check( std::fabs( (double)total - 4.0 ) < 1e-9,
				"skin pinch: the collapse is exactly a collapse -- the leaf's area is still the spanned quadrilateral" );
			Check( minTri > 1e-12, "skin pinch: still no zero-area triangles" );
		}
		if( pi ) pi->release();
	}

	// --- the shipped wing: rails meeting at the shoulder and at the
	//     outermost fingertip, with UNEQUAL point counts between them ---
	{
		std::vector<double> wA, wB;
		SkinWingRails( wA, wB );
		const int R = 14;
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, wA, wB, 48, R, 0.06 );
		Check( m != 0, "skin pinch: the shipped wing builds" );
		if( m ) {
			// S is not pinned by hand here: it is the union resample's own
			// answer, recovered from the vertex count and the two known
			// pinches.  The INVARIANT is what is under test, not the count.
			const unsigned int nv = m->numPoints();
			Check( ( nv + 2 * ( (unsigned int)R - 1 ) ) % (unsigned int)R == 0,
				"skin pinch: the wing is a rectangular grid MINUS exactly two collapsed columns" );
			const unsigned int S = ( nv + 2 * ( (unsigned int)R - 1 ) ) / (unsigned int)R;
			Check( SkinBoundaryEdgeCount( m ) == 2 * ( S - 1 ),
				"skin pinch: MONEY ASSERTION -- the wing's boundary is EXACTLY its two rails, 2*(S-1) (was 142 against the un-pinched 120)" );
			Check( MeshOrphanVertexCount( m ) == 0,
				"skin pinch: MONEY ASSERTION -- the wing leaves no orphan vertices (was 2)" );
			Check( MeshDuplicatePositionCount( m ) == 0,
				"skin pinch: MONEY ASSERTION -- the wing has no duplicate-position vertices (was 26)" );
		}
		if( pi ) pi->release();
	}
}

// (11) NEAR-PINCH SNAP.  The collapse above is an EXACT test, and an exact
// test cannot survive a GENERATED rail: two rails traced from the same
// analytic curve land their tips a few ULPs apart rather than
// bit-identical.  Un-snapped, the factory then emits a skirt of
// sub-degenerate slivers where the author meant a single tip -- measured
// at a 1e-12 tip gap: a minimum triangle area of 7.1e-14, BELOW this
// suite's own 1e-12 no-zero-area floor.
//
// The snap is the lathe's SNAP TO THE AXIS, at the lathe's own threshold:
// 1e-9 of the part's own extent.  Deliberately NOT looser: above
// FP-evaluation noise there is no principled scale to snap at, because a
// genuinely thin ribbon is a legitimate sheet at any width.
static void TestSkinNearPinchSnap()
{
	std::cout << "Test 6k: skin_geometry -- a near-coincident tip snaps to an exact pinch" << std::endl;
	const double A[] = {  0.0, 0.0, 0.0,   2.0, 0.0, 1.0,   4.0, 0.0, 0.0 };
	std::vector<double> railA( A, A + 9 );
	const int R = 5;

	// the exact leaf -- the topology the near-pinch one must reproduce
	unsigned int exactVerts = 0, exactBoundary = 0, exactTris = 0;
	{
		const double B[] = {  0.0, 0.0, 0.0,   2.0, 0.0, -1.0,  4.0, 0.0, 0.0 };
		std::vector<double> railB( B, B + 9 );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 8, R, 0.0 );
		Check( m != 0, "skin snap: the exact leaf builds" );
		if( m ) { exactVerts = m->numPoints(); exactBoundary = SkinBoundaryEdgeCount( m ); exactTris = (unsigned int)m->getFaces().size(); }
		if( pi ) pi->release();
	}

	// the same leaf with its two tips 1e-12 apart (2.5e-13 of the sheet's
	// own 4-unit extent -- squarely FP-evaluation noise)
	{
		const double B[] = {  0.0, 0.0, 1e-12,   2.0, 0.0, -1.0,  4.0, 0.0, 1e-12 };
		std::vector<double> railB( B, B + 9 );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 8, R, 0.0 );
		Check( m != 0, "skin snap: the near-pinched leaf builds" );
		if( m ) {
			Check( m->numPoints() == exactVerts && SkinBoundaryEdgeCount( m ) == exactBoundary
			       && (unsigned int)m->getFaces().size() == exactTris,
				"skin snap: MONEY ASSERTION -- a 1e-12 tip gap bakes the SAME topology as an exactly-coincident one (counts, perimeter, triangles)" );
			Scalar total = 0, minTri = 0;
			MeshAreaStats( m, total, minTri );
			Check( minTri > 1e-12,
				"skin snap: MONEY ASSERTION -- and no sub-degenerate sliver skirt (RED PROOF: remove the snap and this reads 7.1e-14)" );
			Check( MeshOrphanVertexCount( m ) == 0 && MeshDuplicatePositionCount( m ) == 0,
				"skin snap: the snapped tip is one connected vertex, like the exact one" );
		}
		if( pi ) pi->release();
	}

	// AND THE OTHER SIDE OF THE LINE.  A tip gap of 1e-6 on the same
	// 4-unit sheet is 2.5e-7 of its extent -- 250x the snap threshold, and
	// a real (if tiny) separation the author may have meant.  It is NOT
	// snapped, and it does NOT have to be: the sheet it bakes is a healthy
	// mesh by this suite's own no-zero-area standard.  Asserted so a
	// future "let's just widen the tolerance" cannot pass unnoticed.
	{
		const double B[] = {  0.0, 0.0, 1e-6,   2.0, 0.0, -1.0,  4.0, 0.0, 1e-6 };
		std::vector<double> railB( B, B + 9 );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 8, R, 0.0 );
		Check( m != 0, "skin snap: a 1e-6 tip gap builds" );
		if( m ) {
			Check( m->numPoints() > exactVerts,
				"skin snap: a 1e-6 tip gap is NOT snapped -- it is 250x the threshold, and a thin ribbon is a legitimate sheet" );
			Scalar total = 0, minTri = 0;
			MeshAreaStats( m, total, minTri );
			Check( minTri > 1e-12,
				"skin snap: and the un-snapped near-pinch is still a healthy mesh (no zero-area triangles)" );
			Check( MeshOrphanVertexCount( m ) == 0,
				"skin snap: no orphans on the un-snapped near-pinch either" );
		}
		if( pi ) pi->release();
	}
}

// (11b) THE INTERIOR PINCH.  Rails that MEET mid-span -- a bowtie that
// touches, or an hourglass whose rails CROSS -- pinch the sheet to a
// point at an INTERIOR station, and that is a different object from the
// end pinch above.  An end pinch has ONE fan, so a single vertex carrying
// that fan's mean normal is meaningful.  An interior pinch has TWO fans,
// one per side, and they are different surfaces: on a TOUCH whose lobes
// lie in different planes their normals differ by the angle between the
// planes, and on a CROSSING they are exactly OPPOSED (the rail-A-to-rail-B
// direction reverses through the crossing, so dP/dv and the whole normal
// field flip sign).
//
// Collapsing it like an end pinch ships ONE normal to both fans, and the
// borrowed value it collapses is whatever the degenerate-normal fill
// found -- which, when that fill tested `i - d` before `i + d`, was always
// the -u side.  The +u lobe then shipped the -u lobe's normal, silently:
// no warning, and the fold check could not see it either (the face's two
// genuine corners outvote the one wrong one).
//
// The fix gives the station TWO coincident poles, one per fan, each with
// its own normals.  The two specimens below are chosen so that each of
// them FAILS a different way if that is undone:
//
//   * TOUCH, with the two lobes in PERPENDICULAR planes.  Every vertex
//     normal must be parallel to the plane of every face it belongs to
//     (MeshWorstNormalPlaneAlignment == 1).  A shared normal makes the
//     +u lobe's apex normal perpendicular to its own faces -> 0.
//   * CROSSING, with both lobes coplanar but OPPOSITELY oriented.  Plane
//     alignment cannot see that (|cos| is 1 either way), so this one is
//     read by the per-corner fold count: a shared normal is antiparallel
//     to one lobe's faces.
//
// THE TOPOLOGY IT PREDICTS, derived rather than observed.  The two lobes
// share no vertex, so the sheet is two grids each pinched at ONE end.  A
// grid of S stations and R rows has perimeter 2*(S-1) + 2*(R-1), and
// collapsing an end station deletes that end's (R-1)-edge side.  With
// S1 + S2 = S + 1 stations split across the two lobes:
//
//     [2*(S1-1) + (R-1)] + [2*(S2-1) + (R-1)] = 2*(S-1) + 2*(R-1)
//
// -- the SAME perimeter as the un-pinched grid: the pinch contributes no
// boundary of its own, and neither fan is torn.  The vertex count drops
// by (R-2) (a column of R becomes two poles), and there is EXACTLY ONE
// duplicate position in the whole mesh, the pinch's two poles -- the same
// coincident-with-different-normals idiom a duplicated rail point uses
// for a crease, which is what an interior pinch is once the crease has
// closed to a point.
static void TestSkinInteriorPinch()
{
	std::cout << "Test 6n: skin_geometry -- rails MEETING mid-span split into per-side poles" << std::endl;

	SkinLogCapture* pMeetLog = new SkinLogCapture( "MEET at" );
	RISE::GlobalLogPriv()->AddPrinter( pMeetLog );
	const int meetBefore = pMeetLog->MatchCount();

	const int R = 5;

	// --- TOUCH: lobe 1 in the y = 0 plane, lobe 2 in the z = 0 plane ---
	{
		const double A[] = { 0.0, 0.0, 0.0,   2.0, 0.0, 0.0,   4.0, 0.0, 0.0 };
		const double B[] = { 0.0, 0.0, 1.0,   2.0, 0.0, 0.0,   4.0, 1.0, 0.0 };
		std::vector<double> railA( A, A + 9 ), railB( B, B + 9 );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 7, R, 0.0 );
		Check( m != 0, "skin interior pinch: rails that TOUCH mid-span build" );
		if( m ) {
			const unsigned int nv = m->numPoints();
			Check( ( nv + (unsigned int)R - 2 ) % (unsigned int)R == 0,
				"skin interior pinch: the sheet is a rectangular grid MINUS one column collapsed to TWO poles" );
			const unsigned int S = ( nv + (unsigned int)R - 2 ) / (unsigned int)R;
			Check( SkinBoundaryEdgeCount( m ) == 2 * ( S - 1 ) + 2 * ( (unsigned int)R - 1 ),
				"skin interior pinch: MONEY ASSERTION -- boundary is EXACTLY the derived 2*(S-1) + 2*(R-1): two lobes, each pinched at one end, and neither torn" );
			Check( MeshOrphanVertexCount( m ) == 0,
				"skin interior pinch: no vertex ships that no triangle references" );
			Check( MeshDuplicatePositionCount( m ) == 1,
				"skin interior pinch: EXACTLY ONE duplicate position -- the pinch's two poles, and nothing else" );
			Check( std::fabs( MeshWorstNormalPlaneAlignment( m ) - 1.0 ) < 1e-9,
				"skin interior pinch: MONEY ASSERTION -- every vertex normal is parallel to the plane of every face it belongs to, on BOTH perpendicular lobes (RED PROOF: share one pole between the two fans and this reads ~0)" );
			double worst = 1.0;
			Check( MeshFoldedFaceCountPerCorner( m, worst ) == 0,
				"skin interior pinch: and no face is handed a corner normal opposing its own winding" );
			Scalar total = 0, minTri = 0;
			MeshAreaStats( m, total, minTri );
			Check( minTri > 1e-12, "skin interior pinch: no zero-area triangles at the pinch" );
			// Two right triangles: (0,0,0)-(2,0,0)-(0,0,1) has legs 2 and 1
			// in the y = 0 plane; (2,0,0)-(4,0,0)-(4,1,0) has legs 2 and 1
			// in the z = 0 plane.  Area 1 each.
			Check( std::fabs( (double)total - 2.0 ) < 1e-9,
				"skin interior pinch: total area is EXACTLY the two spanned triangles" );
		}
		if( pi ) pi->release();
	}
	Check( pMeetLog->MatchCount() == meetBefore + 1,
		"skin interior pinch: MONEY ASSERTION -- exactly ONE warning names the mid-span meeting (it is as often a mistake as an intent, and it is never silent)" );
	{
		const std::string msg = pMeetLog->LastMatch();
		Check( msg.find( "INTERIOR station" ) != std::string::npos && msg.find( "u 0.5" ) != std::string::npos,
			"skin interior pinch: the warning NAMES the station and its u" );
		Check( msg.find( "OWN normals" ) != std::string::npos,
			"skin interior pinch: and states what was done about it" );
	}

	// --- CROSSING (an hourglass): both lobes in the y = 0 plane, opposite
	//     orientations.  Plane alignment is blind here; the per-corner fold
	//     count is not. ---
	{
		const double A[] = { 0.0, 0.0,  0.0,   2.0, 0.0, 0.0,   4.0, 0.0, 0.0 };
		const double B[] = { 0.0, 0.0, -1.0,   2.0, 0.0, 0.0,   4.0, 0.0, 1.0 };
		std::vector<double> railA( A, A + 9 ), railB( B, B + 9 );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 7, R, 0.0 );
		Check( m != 0, "skin crossing: rails that CROSS mid-span build" );
		if( m ) {
			double worst = 1.0;
			Check( MeshFoldedFaceCountPerCorner( m, worst ) == 0 && std::fabs( worst - 1.0 ) < 1e-9,
				"skin crossing: MONEY ASSERTION -- both lobes wind with their OWN normals (RED PROOF: share one pole and one lobe's apex normal is antiparallel to its faces, worst cos -1)" );
			Check( MeshWindingAgreesWithNormals( m ),
				"skin crossing: the mesh-wide winding check agrees too" );
			const unsigned int nv = m->numPoints();
			const unsigned int S = ( nv + (unsigned int)R - 2 ) / (unsigned int)R;
			Check( ( nv + (unsigned int)R - 2 ) % (unsigned int)R == 0
			       && SkinBoundaryEdgeCount( m ) == 2 * ( S - 1 ) + 2 * ( (unsigned int)R - 1 ),
				"skin crossing: the same derived perimeter as the touch case -- a crossing is a pinch, not a tear" );
			Check( MeshOrphanVertexCount( m ) == 0 && MeshDuplicatePositionCount( m ) == 1,
				"skin crossing: no orphans, and exactly the two coincident poles" );
		}
		if( pi ) pi->release();
	}

	// --- A CROSSING THAT NEVER PINCHES.  The same hourglass with the two
	//     rails passing 1e-6 apart instead of meeting: too wide to snap
	//     (250x the 1e-9-of-extent threshold), so no station is pinched and
	//     the sheet really does fold through itself.  billow is 0, so the
	//     warning must not blame billow -- the rails are the cause and the
	//     remedy. ---
	{
		SkinLogCapture* pRailFold = new SkinLogCapture( "RAILS FOLD the sheet" );
		RISE::GlobalLogPriv()->AddPrinter( pRailFold );
		const int before = pRailFold->MatchCount();
		const double A[] = {  0.0, 0.0, 0.0,   2.0, 0.0, 1.0,   4.0, 0.0, 0.0 };
		const double B[] = {  0.0, 0.0, 1e-6,  2.0, 0.0, -1.0,  4.0, 0.0, 1e-6 };
		std::vector<double> railA( A, A + 9 ), railB( B, B + 9 );
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 8, R, 0.0 );
		Check( m != 0, "skin crossing: an un-snapped 1e-6 crossing still builds" );
		if( m ) {
			double worst = 1.0;
			Check( MeshFoldedFaceCountPerCorner( m, worst ) > 0 && worst < 0,
				"skin crossing: the un-snapped crossing really does invert its normal field (the premise of the assertion below)" );
			double sumWorst = 1.0;
			Check( MeshFoldedFaceCount( m, sumWorst ) == 0,
				"skin crossing: PREMISE -- and the three-normal SUM cannot see any of it, so the report below is not a restatement of the old check" );
		}
		if( pi ) pi->release();
		Check( pRailFold->MatchCount() == before + 1,
			"skin crossing: MONEY ASSERTION -- exactly ONE warning, and at billow 0 it blames the RAILS (RED PROOF: restore the sum-based check and this reads 0)" );
		const std::string msg = pRailFold->LastMatch();
		Check( msg.find( "billow is 0" ) != std::string::npos && msg.find( "cross or double back" ) != std::string::npos,
			"skin crossing: the remedy names the actual cause, not `reduce billow`" );
	}
}

// (11c) THE FOLD THE OLD DETECTOR COULD NOT SEE.  A collapsed pole ships
// ONE normal for its whole fan, so when a subset of the fan's lobes folds
// the mean is still right for the fan on average -- and a per-face check
// that sums the three corner normals then finds two sound summands
// outvoting the one bad one.  The detector reads ZERO on a sheet that
// really does hand rays an inverted geometric normal at the tip.
//
// The fixture: an asymmetric leaf whose rails diverge sharply just past
// the u = 0 tip (their first interior point is at x = 0.05 of a 4-unit
// span) and then swing the sheet around, at a strong NEGATIVE billow.
// The tip fan then wraps far enough that the pole's mean normal opposes
// one of its own faces, while that face's two ring corners still agree
// with it.
//
// Both halves are asserted, and the first is the load-bearing one: the
// OLD measure must read 0 on this mesh, or the test proves nothing about
// masking.
static void TestSkinFoldAtCollapsedApex()
{
	std::cout << "Test 6o: skin_geometry -- a fold at a COLLAPSED TIP that the summed check masks" << std::endl;

	SkinLogCapture* pFold = new SkinLogCapture( "FOLDS the sheet through itself" );
	RISE::GlobalLogPriv()->AddPrinter( pFold );
	const int before = pFold->MatchCount();

	const double A[] = { 0.0, 0.0, 0.0,   0.05, 0.25, 0.35,   2.7, -1.15, 0.25,   4.0, 0.0, 0.0 };
	const double B[] = { 0.0, 0.0, 0.0,   0.05, 0.40, -0.85,  2.7,  0.10, -1.35,  4.0, 0.0, 0.0 };
	std::vector<double> railA( A, A + 12 ), railB( B, B + 12 );

	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 6, 13, -0.40 );
	Check( m != 0, "skin apex fold: the asymmetric billowed leaf builds" );
	if( m ) {
		double sumWorst = 1.0;
		Check( MeshFoldedFaceCount( m, sumWorst ) == 0,
			"skin apex fold: PREMISE -- the SUMMED per-face check reads ZERO on this mesh (this is the masking, not a restatement of it)" );
		double worst = 1.0;
		unsigned int atTip = 0;
		const Point3 tip( 0, 0, 0 );
		const unsigned int folded = MeshFoldedFaceCountPerCorner( m, worst, &tip, &atTip );
		Check( folded > 0 && worst < 0,
			"skin apex fold: MONEY ASSERTION -- but faces really are handed a corner normal opposing their winding" );
		Check( atTip > 0,
			"skin apex fold: MONEY ASSERTION -- and at least one of them is the COLLAPSED TIP's own fan, which is what the mean masked" );
	}
	if( pi ) pi->release();

	Check( pFold->MatchCount() == before + 1,
		"skin apex fold: MONEY ASSERTION -- the factory REPORTS it (RED PROOF: restore the summed check in RISE_API_CreateSkinGeometry and this reads 0)" );
}

// (11d) TWO ADJACENT PINCHES LEAVE A GAP.  Every quad between two pinched
// stations has both of its station pairs collapsed to a single site, so
// both its triangles fall out and the strip emits NOTHING.  Two ways in:
// a duplicated rail point AT a meeting (the crease idiom where the rails
// already touch), and two near-pinches that the snap resolves
// independently -- the snap is per-station and has no idea its neighbour
// also snapped.
//
// The mesh stays well-formed either way (no tear, no orphan), and the
// coincidence required for the snap route is ~1e-9 of the sheet's own
// extent at CONSECUTIVE stations, so this is REPORTED rather than
// repaired: guessing which of the two meetings the author meant to keep
// would silently change an authored shape.  What is pinned here is that
// it is never silent.
static void TestSkinAdjacentPinchGap()
{
	std::cout << "Test 6p: skin_geometry -- two ADJACENT pinched stations are reported, not silently dropped" << std::endl;

	SkinLogCapture* pGap = new SkinLogCapture( "ADJACENT pair(s)" );
	RISE::GlobalLogPriv()->AddPrinter( pGap );
	const int before = pGap->MatchCount();

	// The SNAP route: rail_b carries a duplicated crease point that lands
	// 1e-12 from rail_a's own mid vertex, so BOTH copies snap to an exact
	// pinch and land at consecutive stations.
	const double A[] = { 0.0, 0.0, 0.0,   2.0, 0.0, 0.0,   2.0, 0.0, 0.0,   4.0, 0.0, 0.0 };
	const double B[] = { 0.0, 0.0, 1.0,   2.0, 0.0, 1e-12, 2.0, 0.0, 1e-12, 4.0, 1.0, 0.0 };
	std::vector<double> railA( A, A + 12 ), railB( B, B + 12 );

	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 7, 5, 0.0 );
	Check( m != 0, "skin adjacent pinch: the doubly-pinched sheet still builds" );
	if( m ) {
		Check( MeshOrphanVertexCount( m ) == 0,
			"skin adjacent pinch: the dropped strip leaves NO orphan vertices behind" );
		Scalar total = 0, minTri = 0;
		MeshAreaStats( m, total, minTri );
		Check( minTri > 1e-12,
			"skin adjacent pinch: and no zero-area triangle ships from the collapsed strip" );
	}
	if( pi ) pi->release();

	Check( pGap->MatchCount() == before + 1,
		"skin adjacent pinch: MONEY ASSERTION -- exactly ONE warning names the GAP (RED PROOF: delete the adjacent-pinch check and this reads 0)" );
	{
		const std::string msg = pGap->LastMatch();
		Check( msg.find( "GAP" ) != std::string::npos && msg.find( "stations" ) != std::string::npos,
			"skin adjacent pinch: the warning says WHAT happened and WHICH stations" );
	}
}

// (12) n_len IS A MINIMUM.  The descriptor promises the station count is
// ">= max(nLen, |union|)".  The uniform/authored merge broke that promise:
// its "already served" test was inclusive at BOTH ends of a full-spacing
// interval, so a single authored station could eat TWO uniform samples.
//
// The fixture is the smallest one that shows it: rails whose authored
// arc-length fractions are {0, 0.375, 0.625, 1} with n_len 5.  0.375 sits
// exactly half a spacing above 0.25 and exactly half a spacing below 0.5,
// so under the old test it consumed both -- four stations, not five, with
// a widest gap of 1.5x the requested spacing.
static void TestSkinUniformStationCountIsAMinimum()
{
	std::cout << "Test 6l: skin_geometry -- n_len is a MINIMUM, and an authored station eats at most one uniform sample" << std::endl;
	const double fr[4] = { 0.0, 0.375, 0.625, 1.0 };
	std::vector<double> railA, railB;
	for( int i = 0; i < 4; ++i ) {
		railA.push_back( fr[i] * 4.0 ); railA.push_back( 0.0 ); railA.push_back( 0.0 );
		railB.push_back( fr[i] * 4.0 ); railB.push_back( 0.0 ); railB.push_back( 2.0 );
	}
	const int NL = 5, R = 3;
	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, NL, R, 0.0 );
	Check( m != 0, "skin n_len: the fixture builds" );
	if( !m ) { if( pi ) pi->release(); return; }

	Check( m->numPoints() % (unsigned int)R == 0, "skin n_len: the grid is rectangular (no pinch here)" );
	const unsigned int S = m->numPoints() / (unsigned int)R;
	Check( S >= (unsigned int)NL,
		"skin n_len: MONEY ASSERTION -- at least n_len stations, as the descriptor promises (RED PROOF: make the `pu > su - uniformEps` break test inclusive again and this reads 4)" );
	Check( S >= 4, "skin n_len: and never fewer than the authored union" );

	// Every authored fraction is still present, verbatim -- the fix adds
	// uniform stations, it does not move authored ones.
	int missing = 0;
	for( int i = 0; i < 4; ++i ) {
		if( !SkinHasVertex( m, fr[i] * 4.0, 0.0, 0.0, Scalar(1e-12) ) ) ++missing;
	}
	Check( missing == 0, "skin n_len: every authored station survives the extra uniform ones" );
	pi->release();
}

// (13) STATIONS RUN FORWARDS.  Phase 1's pairing tolerance is what can
// emit a parameter BELOW its predecessor's: rail B carrying a duplicated
// point (the crease idiom) a hair below a rail-A vertex pairs its FIRST
// copy at rail A's LARGER parameter, and the second copy is then taken at
// rail B's own smaller one.
//
// The fixture is exactly that: rail A at fractions {0, 0.5, 1}, rail B at
// {0, 0.4999999999, 0.4999999999, 1} -- a 1e-10 skew, inside the 1e-9
// pairing eps.  Before the clamp this produced a BACKWARDS U texture
// coordinate and a 1e-10-wide band of non-degenerate slivers (measured
// min triangle area 1e-10, three faces wound against their normals)
// exactly where a clean crease was authored.
static void TestSkinStationsAreMonotone()
{
	std::cout << "Test 6m: skin_geometry -- the station parameter never runs backwards" << std::endl;
	std::vector<double> railA, railB;
	const double ap[3] = { 0.0, 0.5, 1.0 };
	for( int i = 0; i < 3; ++i ) { railA.push_back( ap[i] * 4.0 ); railA.push_back( 0.0 ); railA.push_back( 0.0 ); }
	const double bp[4] = { 0.0, 0.4999999999, 0.4999999999, 1.0 };
	for( int i = 0; i < 4; ++i ) { railB.push_back( bp[i] * 4.0 ); railB.push_back( 0.0 ); railB.push_back( 2.0 ); }

	const int R = 3;
	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 2, R, 0.0 );
	Check( m != 0, "skin monotone: the near-crease fixture builds" );
	if( !m ) { if( pi ) pi->release(); return; }

	const unsigned int S = m->numPoints() / (unsigned int)R;
	Check( m->numPoints() % (unsigned int)R == 0, "skin monotone: the grid is rectangular" );
	int backwards = 0;
	for( unsigned int s = 1; s < S; ++s ) {
		if( m->getCoords()[ s * R ].x < m->getCoords()[ ( s - 1 ) * R ].x ) ++backwards;
	}
	Check( backwards == 0,
		"skin monotone: MONEY ASSERTION -- U is non-decreasing along the sheet (RED PROOF: drop the clamp in the union merge and this reads 1)" );

	Scalar total = 0, minTri = 0;
	MeshAreaStats( m, total, minTri );
	// A RELATIVE floor, not the suite's absolute 1e-12 one: the sliver the
	// clamp removes had an area of 1e-10, which clears 1e-12 comfortably
	// while being ten ORDERS below its neighbours.  "No triangle is under a
	// tenth of the mesh's average" is the scale-free way to say that.
	const Scalar meanTri = total / Scalar( m->getFaces().size() );
	Check( minTri > meanTri * Scalar(0.1),
		"skin monotone: MONEY ASSERTION -- the crease is a CLEAN zero-width band, not a sliver strip (min triangle was 1e-10 against a 1.0 mean before the clamp)" );
	Check( MeshWindingAgreesWithNormals( m ),
		"skin monotone: no face is wound against its normals (three were, before the clamp)" );
	Check( std::fabs( (double)total - 8.0 ) < 1e-9,
		"skin monotone: the sheet is still the exact 4 x 2 rectangle" );

	// The crease itself survives: rail B's duplicated point is still two
	// stations, so the two sides still carry their own normals.
	Check( S == 4, "skin monotone: the clamp KEEPS rail_b's duplicated point as its own station (the crease idiom is intact)" );
	pi->release();
}

// (14) HIGH CURVATURE IS NOT A FOLD.  The per-corner detector (the same
// criterion RISE_API_CreateSkinGeometry itself acts on at
// RISE_API.cpp:3701-3714, and the one item 1 above switched the wing/leaf
// MONEY ASSERTIONS onto) must not cry wolf on a sheet that is strongly
// curved AND strongly billowed but never actually folds.
//
// WHAT WAS TRIED FIRST, AND WHY IT IS NOT THE FIXTURE SHIPPED HERE.  A
// pinched RADIAL CONE -- rail_b = k * rail_a about the tip, rail_a sweeping
// a strictly increasing angle -- has a clean closed-form non-self-
// intersection argument (both rails carry Y == 0, so cross(du, dv) is
// EXACTLY +-Y everywhere and billow can only ever move a vertex's Y
// coordinate; the flat cone is injective in (X, Z) off the tip because a
// straight polyline segment's angle from the origin is monotonic along its
// own length, so distinct u land on distinct rays).  That argument is
// correct -- billow genuinely cannot make two different (u, v) collide --
// but it proves the wrong thing: the per-corner fold check is a
// DIFFERENTIAL (normal-vs-winding) criterion, not a positional-collision
// one, and a sharply pinched, wide (n_across >= 3) fan turned out to be
// GENUINELY fold-prone at the tip for ANY nonzero billow, however small
// (measured: worst-corner cosine went negative, first at station 0, at
// billow as low as 0.02, and stayed marginal -- a few percent past zero --
// across billow, n_across, and taper-rate variations).  The mechanism: an
// END pinch's pole normal is the MEAN of its one live neighbour's whole
// row (see meanStationNormal / RISE_API.cpp:3639-3651), and that
// neighbour's row normals fan out over the full swept angle once billowed
// -- so the mean is a genuine, if small, misfit against the row's own
// EXTREMAL corners.  That is a REAL fold (the same masking mechanism
// TestSkinFoldAtCollapsedApex exists to catch, here triggered by curvature
// instead of asymmetric divergence), not a false positive, so it is not
// shipped as a "folds nowhere" specimen.
//
// THE FIXTURE ACTUALLY SHIPPED sidesteps that failure mode by construction
// rather than by tuning around it: it reuses the SHIPPED WING rails
// (SkinWingRails, already proven fold-free at billow 0.06 by
// TestSkinBillowFoldDiagnostic) -- a real, already-curved 4-point-per-rail
// specimen whose two tips CONVERGE GRADUALLY over several segments rather
// than fanning out from a single point over a wide angle -- at billow 0.5,
// more than 8x its shipped value and above the 0.15 that already folds the
// SHARP-creased kink sheet in item 1's own fixture.  This is verified
// EMPIRICALLY below, with a genuine safety margin rather than a marginal
// one: the worst per-corner cosine measured is +0.207 (comfortably
// positive, not a few percent from the zero crossing the way the rejected
// cone construction was), and raising billow further (measured up to 0.6)
// shrinks that margin toward zero before a real curvature-induced fold
// appears near mid-sheet at billow 0.7 -- so 0.5 sits with headroom on
// both sides, not pinned against a threshold.
static void TestSkinCurledTipHighCurvatureNoFold()
{
	std::cout << "Test 6q: skin_geometry -- a strongly curved, strongly billowed sheet folds nowhere" << std::endl;

	std::vector<double> railA, railB;
	SkinWingRails( railA, railB );

	SkinLogCapture* pFoldLog = new SkinLogCapture( "the sheet through itself" );
	RISE::GlobalLogPriv()->AddPrinter( pFoldLog );
	const int before = pFoldLog->MatchCount();

	ITriangleMeshGeometryIndexed* pi = 0;
	const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 48, 14, 0.5 );
	Check( m != 0, "skin curl: the strongly curved, strongly billowed wing membrane builds" );
	if( m ) {
		double worst = 1.0;
		Check( MeshFoldedFaceCountPerCorner( m, worst ) == 0,
			"skin curl: MONEY ASSERTION -- the shipped wing curve at billow 0.5 (8x its shipped value) folds NOWHERE (RED PROOF: this is the fixture item 1's stronger per-corner check must not false-positive on)" );
		Check( worst > 0.1,
			"skin curl: and it does so with a genuine safety margin, not a marginal near-zero one (worst corner cosine measured +0.207)" );
	}
	if( pi ) pi->release();

	Check( pFoldLog->MatchCount() == before,
		"skin curl: MONEY ASSERTION -- and the factory agrees, emitting no fold warning at all" );
}

// (15) THREE MICRO-FIXTURES at the edges of the interior-pinch machinery.
// All three use `n_len 2` -- the minimum -- which (per the phase-2 uniform
// refinement above) inserts uniform samples only at u = 0 and u = 1, and
// both are always already served by the first and last AUTHORED stations,
// so no uniform station is ever inserted: the station count is EXACTLY the
// authored point count, letting each fixture assert exact indices rather
// than deriving S from the output the way the general InteriorPinch test
// above has to.
//
// All three also use the same MIRROR idiom: rail_b's point i is rail_a's
// point i with one coordinate negated.  Negation is an isometry, so
// corresponding segments on the two rails are the SAME LENGTH at every
// index, not just the pinched one -- their arc-length fractions match
// EXACTLY (bit-identically) at every station, which is what lets each
// authored index pair into its own station with no interpolation drift.
static void TestSkinInteriorPinchMicroFixtures()
{
	std::cout << "Test 6r: skin_geometry -- interior pinch micro-fixtures: single-strip fan, triple pinch, asymmetric closed form" << std::endl;

	// --- (a) PINCH AT STATION 1: a single-strip fan (S1 = 2) on the short
	//     side, a three-strip fan (S2 = 3) on the long side. ---
	{
		const double z[4] = { 1.0, 0.0, 1.0, 2.0 };		// station 1 pinches
		std::vector<double> railA, railB;
		for( int i = 0; i < 4; ++i ) {
			railA.push_back( (double)i ); railA.push_back( 0.0 ); railA.push_back(  z[i] );
			railB.push_back( (double)i ); railB.push_back( 0.0 ); railB.push_back( -z[i] );
		}
		const int R = 5;
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 2, R, 0.0 );
		Check( m != 0, "skin micro (a): the station-1 pinch fixture builds" );
		if( m ) {
			Check( MeshOrphanVertexCount( m ) == 0, "skin micro (a): no orphans" );
			double worst = 1.0;
			Check( MeshFoldedFaceCountPerCorner( m, worst ) == 0,
				"skin micro (a): the single-strip fan on the short side is not folded" );
			Check( std::fabs( MeshWorstNormalPlaneAlignment( m ) - 1.0 ) < 1e-9,
				"skin micro (a): every vertex normal is parallel to the plane of every face it belongs to -- each side keeps its OWN normals" );
			// S1 = 2 (stations 0-1), S2 = 3 (stations 1-2-3), S = 4: the
			// closed form collapses to the same 2*(S-1) + 2*(R-1) as the
			// un-pinched grid.
			Check( SkinBoundaryEdgeCount( m ) == 2 * ( 4 - 1 ) + 2 * ( R - 1 ),
				"skin micro (a): MONEY ASSERTION -- boundary is the two-lobe closed form with S1 = 2, S2 = 3" );
		}
		if( pi ) pi->release();
	}

	// --- (b) THREE CONSECUTIVE PINCHES: the middle one's poles are
	//     referenced by NEITHER of its two neighbouring strips (both are
	//     dead, pinch-to-pinch), so siteUsed discipline must SKIP them, not
	//     orphan them; the two adjacent PAIRS (1,2) and (2,3) must collapse
	//     into one aggregated warning, not a storm of one per pair. ---
	{
		SkinLogCapture* pGapLog = new SkinLogCapture( "ADJACENT pair(s)" );
		RISE::GlobalLogPriv()->AddPrinter( pGapLog );
		const int before = pGapLog->MatchCount();

		const double z[5] = { 1.0, 0.0, 0.0, 0.0, 1.0 };	// stations 1,2,3 pinch
		std::vector<double> railA, railB;
		for( int i = 0; i < 5; ++i ) {
			railA.push_back( (double)i ); railA.push_back( 0.0 ); railA.push_back(  z[i] );
			railB.push_back( (double)i ); railB.push_back( 0.0 ); railB.push_back( -z[i] );
		}
		const int R = 4;
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 2, R, 0.0 );
		Check( m != 0, "skin micro (b): the triple-pinch fixture builds" );
		if( m ) {
			Check( MeshOrphanVertexCount( m ) == 0, "skin micro (b): no orphans" );
			Check( !SkinHasVertex( m, 2.0, 0.0, 0.0, Scalar(1e-9) ),
				"skin micro (b): MONEY ASSERTION -- the MIDDLE pinch (station 2) has no live neighbouring strip on either side, so its pole is SKIPPED, not emitted (RED PROOF: emit every site unconditionally and this reads true)" );
			Check( SkinHasVertex( m, 1.0, 0.0, 0.0, Scalar(1e-9) ) && SkinHasVertex( m, 3.0, 0.0, 0.0, Scalar(1e-9) ),
				"skin micro (b): the OUTER two pinches (stations 1 and 3) each keep one live strip, so their poles DO ship" );
		}
		if( pi ) pi->release();
		Check( pGapLog->MatchCount() == before + 1,
			"skin micro (b): MONEY ASSERTION -- exactly ONE aggregated warning, not a storm of one per pair (RED PROOF: fire it inside the pair loop instead of once after and this reads 2)" );
		if( pGapLog->MatchCount() > before ) {
			Check( pGapLog->LastMatch().find( "2 ADJACENT" ) != std::string::npos,
				"skin micro (b): and it states the correct count of adjacent pairs (2)" );
		}
	}

	// --- (c) ASYMMETRIC INTERIOR PINCH, station 2 of 12 (S1 = 3, S2 = 10):
	//     the closed form at unequal lobe sizes.  RISE_API.cpp:3486-3494's
	//     own reasoning: each lobe bakes as an END-pinch grid on its own
	//     (verts = S_k*R - (R-1)), and "the two lobes then share no
	//     vertex", so
	//         verts = [S1*R-(R-1)] + [S2*R-(R-1)]
	//               = (S1+S2)*R - 2*(R-1)
	//               = (S+1)*R - 2*(R-1)              [S1+S2 = S+1]
	//               = S*R - (R-2)
	//     -- the INTERIOR-pinch reduction is (R-2) per pinch, not the
	//     (R-1) an END pinch costs, because the column becomes TWO poles
	//     sharing the point, not one. ---
	{
		const int S = 12, R = 6;
		const double z[12] = { 1.5, 0.7, 0.0, 0.5, 1.0, 1.5, 2.0, 2.3, 2.5, 2.6, 2.7, 2.8 };	// station 2 pinches
		std::vector<double> railA, railB;
		for( int i = 0; i < S; ++i ) {
			railA.push_back( (double)i ); railA.push_back( 0.0 ); railA.push_back(  z[i] );
			railB.push_back( (double)i ); railB.push_back( 0.0 ); railB.push_back( -z[i] );
		}
		ITriangleMeshGeometryIndexed* pi = 0;
		const TriangleMeshGeometryIndexed* m = MakeSkin( pi, railA, railB, 2, R, 0.0 );
		Check( m != 0, "skin micro (c): the asymmetric 12-station pinch fixture builds" );
		if( m ) {
			Check( MeshOrphanVertexCount( m ) == 0, "skin micro (c): no orphans" );
			const unsigned int expected = (unsigned int)( S * R - ( R - 2 ) );
			Check( m->numPoints() == expected,
				"skin micro (c): MONEY ASSERTION -- verts = S*R - (R-2) holds at unequal lobe sizes (S1=3, S2=10) (RED PROOF: use the end-pinch R-1 reduction here and this is off by one)" );
		}
		if( pi ) pi->release();
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
	TestSweepBackCompatDigests();
	TestSweepAnisotropicPointScale();
	TestSweepMorphIdentity();
	TestSweepMorphEndpointsAndAlignment();
	TestSweepMorphCircleToRectSimple();
	TestSweepMorphUnionKeepsAuthoredCorners();
	TestSweepMorphIdentityUnequalCounts();
	TestSweepMorphUnionKeepsDuplicatePoint();
	TestSweepMorphComposesWithAnisotropy();
	TestSweepMorphCapsWatertight();
	TestSweepMorphCapWindingFollowsSection();
	TestSweepLoftValidation();
	TestSweepPerStationWidth();
	TestPathInstances();
	TestLatheCylinderIdentity();
	TestLatheConePole();
	TestLatheSphereConvergence();
	TestLathePartialSweep();
	TestLatheWinding();
	TestLatheVaseAndHardEdge();
	TestLatheAxisAndReversedProfile();
	TestLatheInteriorPolePinch();
	TestLatheArcLengthV();
	TestLatheGeneratedClosedProfile();
	TestLatheLevelEndTubeCap();
	TestLatheInteriorPolePartialCap();
	TestLatheFlatAnnulusPartialSweep();
	TestLatheDeadRowBoundingBox();
	TestLatheNearLoopIsNotALoop();
	TestLatheValidation();
	TestSkinRuledQuadIdentity();
	TestSkinUnionKeepsAuthoredVertices();
	TestSkinDuplicatedRailPointCrease();
	TestSkinBillow();
	TestSkinBillowRecomputesNormals();
	TestSkinPinchedLeaf();
	TestSkinTwoSided();
	TestSkinBillowFoldDiagnostic();
	TestSkinPinchedTopologyIsConnected();
	TestSkinNearPinchSnap();
	TestSkinInteriorPinch();
	TestSkinFoldAtCollapsedApex();
	TestSkinAdjacentPinchGap();
	TestSkinUniformStationCountIsAMinimum();
	TestSkinStationsAreMonotone();
	TestSkinCurledTipHighCurvatureNoFold();
	TestSkinInteriorPinchMicroFixtures();
	TestSkinValidation();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
