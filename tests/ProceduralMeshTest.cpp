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
	TestSweepPerStationWidth();
	TestPathInstances();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
