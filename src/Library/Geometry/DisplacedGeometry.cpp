//////////////////////////////////////////////////////////////////////
//
//  DisplacedGeometry.cpp - Implementation of DisplacedGeometry.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DisplacedGeometry.h"
#include "TriangleMeshGeometryIndexed.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Observable.h"
#include "../Utilities/GeometricUtilities.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "../Utilities/RenderParallelScope.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace {
	// Diagnostic counter: total BuildMesh() invocations process-wide.  See
	// the GetBuildMeshCount()/ResetBuildMeshCount() doc in the header.
	std::atomic<unsigned int> s_buildMeshCount( 0 );
	// Diagnostic counter: TessellateToMeshEx calls (see GetTessellateCopyCount in the header).
	std::atomic<unsigned int> s_tessellateCopyCount( 0 );

	// ---- Mesh refinement for `detail` on mesh bases --------------------------
	//
	// A base that is already a MESH ignores `detail` in its TessellateToMesh and
	// would be displaced only at its own vertices -- a 4-point-profile lathe has
	// 4 rows however fine the relief.  PRINCIPLE: REFINEMENT NEVER CHANGES THE
	// AUTHORED SURFACE.  It adds samples on the authored piecewise-linear surface
	// (new vertices lie on authored chords / inside authored triangles, with
	// interpolated normals and UVs); creases exist only where the author made them;
	// the authored correspondence is kept; the silhouette is the authored polygon.
	// (An earlier cut re-baked the builder from its descriptor at higher density;
	// that re-ran loft alignment, smoothed straight profile segments' normals and
	// turned every inserted row of a non-smooth lathe into a crease -- it changed
	// the surface, and is gone.)  TWO constructions, both REGULAR (a first adaptive
	// per-edge splitting made 48% of the triangles slivers under 20 degrees):
	//
	//  (a) BUILDER BASES (lathe / sweep / skin): the factory records its authored grid
	//      (TriangleMeshGeometryIndexed::MeshGrid) and TessellateRefined() subdivides
	//      each line family by its own length -- quads stay regular quads, pole /
	//      pinch fans stay fans -- interpolating position, normal and UV
	//      barycentrically inside the authored triangles.  A triangle no cell replaced
	//      (an end cap; a skin triangle the zero-area test half-ate) subdivides the
	//      edges it shares with refined cells identically (matched by POSITION, since a
	//      cap's rim vertices are duplicates of the wall's) and is closed by a centroid
	//      fan, so the result has no T-junction and a cap's rim matches the wall's.
	//  (b) OTHER indexed meshes (imports, path instances): UNIFORM whole-level 1->4
	//      midpoint subdivision (RefineUniform below) -- every edge splits, so edges
	//      stay parallel to the base's and every child is similar to its parent.  The
	//      level count comes from the base's MEDIAN edge length, so one long cap or
	//      outlier edge cannot drive the whole mesh.
	//
	// (b) in detail: midpoints are keyed by the combined index pair, so a crease /
	// cap / pinch vertex -- a DISTINCT combined index from its geometric twin --
	// keeps its midpoints split too: nothing is welded by position (a face-normal
	// base is never refined at all).
	//
	// Attributes at a (b) midpoint: position LINEAR, normal = normalize(na + nb)
	// (falls back to na for opposed normals), UV LINEAR -- except on an axis the
	// base DECLARES periodic (TriangleMeshGeometryIndexed::SetUVPeriodicRegion:
	// the ring-stitch edge spanning ~(1 -> 0) is taken the short way round,
	// modulo 1) and at a POLAR vertex (a pole's u / a pinch's v is a placeholder:
	// an edge to a normal vertex takes the normal vertex's value).  A chart that is
	// NOT declared periodic (an imported mesh, planar caps, a [0,4] tiled chart) is
	// always linear.  Linear positions: the silhouette of a coarse (b) base stays
	// polygonal -- (b) adds relief resolution, it does not round the base (nor does (a)).
	//
	// Requires iVertices == iNormals == iCoords on every triangle (the
	// TessellateToMesh contract); otherwise the mesh is left unrefined.
	struct RefineResult
	{
		unsigned int levels;
		size_t       trianglesBefore;
		size_t       trianglesAfter;
		bool         capped;			//!< stopped by the triangle / level cap with edges still too long
		RefineResult() : levels( 0 ), trianglesBefore( 0 ), trianglesAfter( 0 ), capped( false ) {}
	};

	const unsigned int kMaxRefineLevels    = 6;
	const size_t       kMaxRefineTriangles = 1500000;

	inline std::uint64_t EdgeKey( const unsigned int a, const unsigned int b )
	{
		return ( a < b ) ? ( ( std::uint64_t( a ) << 32 ) | b ) : ( ( std::uint64_t( b ) << 32 ) | a );
	}

	// Midpoint of a chart coordinate on an axis the mesh DECLARES periodic with
	// period 1, taken the short way round from `a` to `b` and expressed in the
	// unit cell that holds the lower endpoint (so it works for any offset chart,
	// not just [0,1)).  Called only for an edge flagged periodic on this axis
	// whose chart delta exceeds half a period -- i.e. the ring-stitch edge that
	// spans ~(1 -> 0); every other edge on every mesh is interpolated linearly.
	inline Scalar PeriodicMid( const Scalar a, const Scalar b )
	{
		Scalar d = b - a;
		d -= std::floor( d + 0.5 );
		Scalar m = a + 0.5 * d;
		const Scalar cell = std::floor( a < b ? a : b );
		m -= std::floor( m - cell );
		return m;
	}

	inline Scalar ChartMid( const Scalar a, const Scalar b, const bool bPeriodicAxis )
	{
		return ( bPeriodicAxis && std::fabs( a - b ) > 0.5 ) ? PeriodicMid( a, b ) : 0.5 * ( a + b );
	}

	// Midpoint of one chart axis between two vertices.  A POLAR end (the axis'
	// value there is a placeholder -- a lathe pole's u, a skin pinch's v) must
	// not leak into a neighbour: an edge with exactly one polar end takes the
	// OTHER end's value; two polar ends (or none) interpolate as ChartMid does.
	inline Scalar AxisMid( const Scalar a, const Scalar b, const bool polarA, const bool polarB, const bool bPeriodicAxis )
	{
		if( polarA && !polarB ) return b;
		if( polarB && !polarA ) return a;
		return ChartMid( a, b, bPeriodicAxis );
	}

	// Median unique-edge length of a triangle list (0 if there are no edges).
	Scalar MedianEdgeLength( const IndexTriangleListType& tris, const VerticesListType& vertices )
	{
		std::vector<std::pair<std::uint64_t, Scalar> > lens;
		lens.reserve( tris.size() * 3 );
		for( size_t t = 0; t < tris.size(); ++t ) {
			for( int k = 0; k < 3; ++k ) {
				const unsigned int a = tris[t].iVertices[k], b = tris[t].iVertices[( k + 1 ) % 3];
				if( a == b ) continue;
				const Vector3 e = Vector3Ops::mkVector3( vertices[a], vertices[b] );
				lens.push_back( std::make_pair( EdgeKey( a, b ), Vector3Ops::Magnitude( e ) ) );
			}
		}
		if( lens.empty() ) {
			return 0;
		}
		std::sort( lens.begin(), lens.end() );
		lens.erase( std::unique( lens.begin(), lens.end(),
			[]( const std::pair<std::uint64_t, Scalar>& x, const std::pair<std::uint64_t, Scalar>& y ) { return x.first == y.first; } ), lens.end() );
		std::vector<Scalar> only( lens.size() );
		for( size_t i = 0; i < lens.size(); ++i ) only[i] = lens[i].second;
		std::nth_element( only.begin(), only.begin() + only.size() / 2, only.end() );
		return only[ only.size() / 2 ];
	}

	// ONE uniform 1->4 midpoint subdivision pass: every edge splits (midpoints shared
	// per unordered combined-index pair), every triangle becomes four with the
	// winding preserved.  Appends the new vertices after the existing ones.
	void SubdivideUniformOnce(
		IndexTriangleListType& tris,
		VerticesListType&      vertices,
		NormalsListType&       normals,
		TexCoordsListType&     coords,
		std::vector<unsigned char>& uvFlags )
	{
		std::unordered_map<std::uint64_t, unsigned int> edgeMid;
		edgeMid.reserve( tris.size() * 2 );
		auto midOf = [&]( const unsigned int a, const unsigned int b ) -> unsigned int {
			const std::uint64_t key = EdgeKey( a, b );
			const auto it = edgeMid.find( key );
			if( it != edgeMid.end() ) {
				return it->second;
			}
			const unsigned int mid = static_cast<unsigned int>( vertices.size() );
			edgeMid[key] = mid;
			const Point3  pa = vertices[a], pb = vertices[b];
			const Vector3 na = normals[a],  nb = normals[b];
			const TexCoord ca = coords[a],  cb = coords[b];
			// Position and UV are LINEAR: the refined, undisplaced mesh is exactly the
			// base's own piecewise-linear surface.
			vertices.push_back( Point3( 0.5 * ( pa.x + pb.x ), 0.5 * ( pa.y + pb.y ), 0.5 * ( pa.z + pb.z ) ) );
			Vector3 nm = na + nb;
			const Scalar nl = Vector3Ops::Magnitude( nm );
			normals.push_back( nl > 1e-12 ? nm * ( 1.0 / nl ) : na );
			// A new vertex is on a periodic / polar chart region only if BOTH ends
			// are (planar caps never wrap; a spoke midpoint is no longer AT the pole).
			const unsigned char fa = uvFlags[a], fb = uvFlags[b];
			const unsigned char fm = static_cast<unsigned char>( fa & fb );
			uvFlags.push_back( fm );
			coords.push_back( TexCoord(
				AxisMid( ca.x, cb.x, ( fa & TriangleMeshGeometryIndexed::kUPolar ) != 0, ( fb & TriangleMeshGeometryIndexed::kUPolar ) != 0,
					( fm & TriangleMeshGeometryIndexed::kUPeriodic ) != 0 ),
				AxisMid( ca.y, cb.y, ( fa & TriangleMeshGeometryIndexed::kVPolar ) != 0, ( fb & TriangleMeshGeometryIndexed::kVPolar ) != 0,
					( fm & TriangleMeshGeometryIndexed::kVPeriodic ) != 0 ) ) );
			return mid;
		};
		IndexTriangleListType out;
		out.reserve( tris.size() * 4 );
		auto emit = [&]( const unsigned int a, const unsigned int b, const unsigned int c ) {
			IndexedTriangle nt;
			nt.iVertices[0] = nt.iNormals[0] = nt.iCoords[0] = a;
			nt.iVertices[1] = nt.iNormals[1] = nt.iCoords[1] = b;
			nt.iVertices[2] = nt.iNormals[2] = nt.iCoords[2] = c;
			out.push_back( nt );
		};
		for( size_t t = 0; t < tris.size(); ++t ) {
			const unsigned int p0 = tris[t].iVertices[0], p1 = tris[t].iVertices[1], p2 = tris[t].iVertices[2];
			const unsigned int m01 = midOf( p0, p1 ), m12 = midOf( p1, p2 ), m20 = midOf( p2, p0 );
			emit( p0, m01, m20 );
			emit( m01, p1, m12 );
			emit( m20, m12, p2 );
			emit( m01, m12, m20 );
		}
		tris.swap( out );
	}

	// UNIFORM refinement toward `targetLen`: L whole 1->4 levels, L = ceil(log2(median
	// edge / targetLen)) clamped to [0, kMaxRefineLevels] and reduced until the
	// triangle count fits kMaxRefineTriangles.
	RefineResult RefineUniform(
		IndexTriangleListType& tris,
		VerticesListType&      vertices,
		NormalsListType&       normals,
		TexCoordsListType&     coords,
		std::vector<unsigned char>& uvFlags,
		const Scalar           targetLen )
	{
		RefineResult res;
		uvFlags.resize( vertices.size(), 0 );
		res.trianglesBefore = res.trianglesAfter = tris.size();
		if( tris.empty() || !( targetLen > 0 ) ||
			vertices.size() != normals.size() || vertices.size() != coords.size() ) {
			return res;
		}
		for( size_t t = 0; t < tris.size(); ++t ) {
			for( int k = 0; k < 3; ++k ) {
				if( tris[t].iVertices[k] != tris[t].iNormals[k] || tris[t].iVertices[k] != tris[t].iCoords[k] ||
					tris[t].iVertices[k] >= vertices.size() ) {
					return res;
				}
			}
		}
		const Scalar median = MedianEdgeLength( tris, vertices );
		if( !( median > targetLen ) ) {
			return res;
		}
		unsigned int levels = 0;
		while( levels < kMaxRefineLevels && median / Scalar( 1u << levels ) > targetLen ) {
			++levels;
		}
		const unsigned int wanted = levels;
		while( levels > 0 && tris.size() * ( size_t( 1 ) << ( 2 * levels ) ) > kMaxRefineTriangles ) {
			--levels;
		}
		res.capped = ( levels < wanted ) || ( median / Scalar( 1u << levels ) > targetLen );
		for( unsigned int l = 0; l < levels; ++l ) {
			SubdivideUniformOnce( tris, vertices, normals, coords, uvFlags );
		}
		res.levels = levels;
		res.trianglesAfter = tris.size();
		return res;
	}

	inline std::uint64_t TopologySignature( const IndexTriangleListType& tris, const size_t vertexCount )
	{
		std::uint64_t h = 1469598103934665603ull;
		auto mix = [&]( const std::uint64_t x ) { h ^= x; h *= 1099511628211ull; };
		mix( tris.size() );
		mix( vertexCount );
		for( size_t t = 0; t < tris.size(); ++t ) {
			mix( tris[t].iVertices[0] ); mix( tris[t].iVertices[1] ); mix( tris[t].iVertices[2] );
		}
		return h ? h : 1ull;	// 0 is reserved for "not refined"
	}
}

namespace {
	// Rotate `v` by the minimal rotation taking direction `fromIn` to direction
	// `toIn` (both normalized here; a zero-length one leaves `v` unrotated).
	Vector3 RotateFromTo( const Vector3& fromIn, const Vector3& toIn, const Vector3& v )
	{
		const Scalar lf = Vector3Ops::Magnitude( fromIn ), lt = Vector3Ops::Magnitude( toIn );
		if( !( lf > 1e-12 ) || !( lt > 1e-12 ) ) {
			return v;
		}
		const Vector3 from = fromIn * ( 1.0 / lf ), to = toIn * ( 1.0 / lt );
		const Vector3 axis = Vector3Ops::Cross( from, to );
		const Scalar sinT = Vector3Ops::Magnitude( axis );
		const Scalar cosT = Vector3Ops::Dot( from, to );
		if( sinT < 1e-12 ) {
			if( cosT > 0.0 ) {
				return v;
			}
			// antiparallel: a half turn about any axis perpendicular to `from`
			Vector3 u = ( std::fabs( from.x ) < 0.9 ) ? Vector3( 1, 0, 0 ) : Vector3( 0, 1, 0 );
			u = Vector3Ops::Normalize( Vector3Ops::Cross( from, u ) );
			return u * ( 2.0 * Vector3Ops::Dot( v, u ) ) - v;
		}
		const Vector3 k = axis * ( 1.0 / sinT );
		return v * cosT + Vector3Ops::Cross( k, v ) * sinT + k * ( Vector3Ops::Dot( k, v ) * ( 1.0 - cosT ) );
	}

	// Carry the base's smooth SHADING-normal field through the displacement.
	//   shading0 / geom0 : the base's shading normals and its angle-weighted
	//                      geometric normals on the UNdisplaced (refined) mesh;
	//   normals          : on entry the displaced mesh's geometric normals g1,
	//                      on exit the final shading normals.
	// Each vertex's base shading normal is ROTATED by the minimal rotation that
	// takes its undisplaced geometric normal to its displaced one, so the
	// perturbation the relief makes is applied in the displaced frame (adding a
	// world-space difference instead pushes normals behind steep faces).  A
	// vertex whose result would sit behind an incident face the geometric normal
	// is in front of falls back to the geometric normal there, so the count of
	// back-facing shading normals never exceeds the geometric-only count.
	void ApplyTransportedShading(
		const NormalsListType&       shading0,
		const NormalsListType&       geom0,
		const IndexTriangleListType& tris,
		const VerticesListType&      vertices,
		NormalsListType&             normals )
	{
		if( shading0.size() != normals.size() || geom0.size() != normals.size() ) {
			return;
		}
		const NormalsListType g1 = normals;
		for( size_t i = 0; i < normals.size(); ++i ) {
			const Vector3 s = RotateFromTo( geom0[i], g1[i], shading0[i] );
			const Scalar l = Vector3Ops::Magnitude( s );
			normals[i] = ( l > 1e-12 ) ? s * ( 1.0 / l ) : g1[i];
		}
		std::vector<unsigned char> fallback( normals.size(), 0 );
		for( size_t t = 0; t < tris.size(); ++t ) {
			const Vector3 c = Vector3Ops::Cross(
				Vector3Ops::mkVector3( vertices[tris[t].iVertices[1]], vertices[tris[t].iVertices[0]] ),
				Vector3Ops::mkVector3( vertices[tris[t].iVertices[2]], vertices[tris[t].iVertices[0]] ) );
			const Scalar m = Vector3Ops::Magnitude( c );
			if( m < NEARZERO ) {
				continue;
			}
			const Vector3 nf = c * ( 1.0 / m );
			for( int k = 0; k < 3; ++k ) {
				const unsigned int vi = tris[t].iVertices[k];
				if( Vector3Ops::Dot( normals[vi], nf ) <= 0.0 && Vector3Ops::Dot( g1[vi], nf ) > 0.0 ) {
					fallback[vi] = 1;
				}
			}
		}
		for( size_t i = 0; i < normals.size(); ++i ) {
			if( fallback[i] ) {
				normals[i] = g1[i];
			}
		}
	}
}

namespace {
	// A POLAR vertex (a lathe pole, a skin pinch -- its u or v is only a placeholder)
	// has no meaningful chart coordinate, so the field it displaces by is arbitrary
	// (f at the placeholder), while the spokes around it displace by f at their own
	// coordinates -- a star of folds at the lid.  Displace it by the AVERAGE of its
	// neighbours' displacements along its own normal instead.  `base` / `n0` are the
	// pre-displacement positions and normals; `displaced` is rewritten at polar
	// vertices only.
	void AveragePolarDisplacement(
		const VerticesListType&      base,
		const NormalsListType&       n0,
		const IndexTriangleListType& tris,
		const std::vector<unsigned char>& flags,
		VerticesListType&            displaced )
	{
		const unsigned char polarMask = TriangleMeshGeometryIndexed::kUPolar | TriangleMeshGeometryIndexed::kVPolar;
		if( flags.size() != displaced.size() || base.size() != displaced.size() || n0.size() != displaced.size() ) {
			return;
		}
		std::vector<Scalar> sum( displaced.size(), 0.0 );
		std::vector<unsigned int> cnt( displaced.size(), 0 );
		auto dispOf = [&]( const unsigned int i ) {
			return Vector3Ops::Dot( Vector3Ops::mkVector3( displaced[i], base[i] ), n0[i] );
		};
		for( size_t t = 0; t < tris.size(); ++t ) {
			for( int k = 0; k < 3; ++k ) {
				const unsigned int a = tris[t].iVertices[k], b = tris[t].iVertices[( k + 1 ) % 3];
				const bool pa = ( flags[a] & polarMask ) != 0, pb = ( flags[b] & polarMask ) != 0;
				if( pa && !pb ) { sum[a] += dispOf( b ); ++cnt[a]; }
				else if( pb && !pa ) { sum[b] += dispOf( a ); ++cnt[b]; }
			}
		}
		for( size_t i = 0; i < displaced.size(); ++i ) {
			if( cnt[i] > 0 && ( flags[i] & polarMask ) ) {
				const Scalar d = sum[i] / Scalar( cnt[i] );
				displaced[i] = Point3( base[i].x + n0[i].x * d, base[i].y + n0[i].y * d, base[i].z + n0[i].z * d );
			}
		}
	}
}

int DisplacedGeometry::BaseWeldPolicy() const
{
	// The base MESH: the base itself, or a displaced base's baked mesh.
	const TriangleMeshGeometryIndexed* pIdx = dynamic_cast<const TriangleMeshGeometryIndexed*>( m_pBase );
	if( !pIdx ) {
		if( const DisplacedGeometry* pDsp = dynamic_cast<const DisplacedGeometry*>( m_pBase ) ) {
			pIdx = dynamic_cast<const TriangleMeshGeometryIndexed*>( pDsp->GetBakedMesh() );
		}
	}
	if( !pIdx || pIdx->UsesFaceNormals() ) {
		return -1;
	}
	return pIdx->GetDisplacementWeldPolicy();
}

void DisplacedGeometry::WeldCoincidentDisplacement(
	const VerticesListType& undisplaced, const NormalsListType& baseNormals, const int policy, VerticesListType& displaced ) const
{
	if( policy == TriangleMeshGeometryIndexed::kWeldNone || undisplaced.size() != displaced.size() || undisplaced.size() < 2 ) {
		return;
	}
	std::vector<unsigned int> wid;
	if( !GeometricUtilities::WeldVertexPositions( undisplaced, wid ) ) {
		return;
	}
	const bool cone = ( policy == TriangleMeshGeometryIndexed::kWeldCreaseCone ) && baseNormals.size() == undisplaced.size();
	unsigned int maxId = 0;
	for( size_t i = 0; i < wid.size(); ++i ) maxId = std::max( maxId, wid[i] );

	// Members of each position group (CSR), then -- for the crease-cone policy -- split the group
	// into connected components of "base normals within 120 degrees" (dot > -1/2).
	std::vector<unsigned int> start( size_t( maxId ) + 2, 0 );
	for( size_t i = 0; i < wid.size(); ++i ) ++start[ size_t( wid[i] ) + 1 ];
	for( size_t g = 0; g <= maxId; ++g ) start[g + 1] += start[g];
	std::vector<unsigned int> members( wid.size() ), fill( start.begin(), start.end() - 1 );
	for( size_t i = 0; i < wid.size(); ++i ) members[ fill[ wid[i] ]++ ] = static_cast<unsigned int>( i );

	std::vector<unsigned int> comp;		// scratch: component label per member of the current group
	for( size_t g = 0; g <= maxId; ++g ) {
		const unsigned int b = start[g], e = start[g + 1], m = e - b;
		if( m < 2 ) continue;
		comp.assign( m, 0 );
		for( unsigned int k = 0; k < m; ++k ) comp[k] = k;
		if( cone ) {
			auto find = [&]( unsigned int x ) { while( comp[x] != x ) { comp[x] = comp[comp[x]]; x = comp[x]; } return x; };
			for( unsigned int i = 0; i < m; ++i ) {
				const Vector3& ni = baseNormals[ members[b + i] ];
				for( unsigned int j = i + 1; j < m; ++j ) {
					const Vector3& nj = baseNormals[ members[b + j] ];
					if( Vector3Ops::Dot( ni, nj ) > -0.5 ) {
						const unsigned int ri = find( i ), rj = find( j );
						if( ri != rj ) comp[ri] = rj;
					}
				}
			}
			for( unsigned int k = 0; k < m; ++k ) comp[k] = find( k );
		} else {
			for( unsigned int k = 0; k < m; ++k ) comp[k] = 0;		// the whole group is one crease
		}
		// weld each component with >= 2 members to its mean displacement vector
		for( unsigned int root = 0; root < m; ++root ) {
			unsigned int count = 0, first = 0;
			Vector3 sum( 0, 0, 0 );
			for( unsigned int k = 0; k < m; ++k ) {
				if( comp[k] != root ) continue;
				const unsigned int vi = members[b + k];
				if( count++ == 0 ) first = vi;
				sum = sum + Vector3Ops::mkVector3( displaced[vi], undisplaced[vi] );
			}
			if( count < 2 ) continue;
			const Vector3 mean = sum * ( 1.0 / Scalar( count ) );
			const Point3& o = undisplaced[first];
			const Point3 target( o.x + mean.x, o.y + mean.y, o.z + mean.z );
			for( unsigned int k = 0; k < m; ++k ) {
				if( comp[k] == root ) displaced[ members[b + k] ] = target;
			}
		}
	}
}

bool DisplacedGeometry::WillDisplaceVertices() const
{
	return ( m_pDisplacement || m_pHeight ) && m_dispScale != 0.0;
}

bool DisplacedGeometry::TessellateAndRefine(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const bool             bLog,
	NormalsListType&       shading0,
	NormalsListType&       geom0,
	std::vector<unsigned char>& uvFlags ) const
{
	shading0.clear();
	geom0.clear();
	uvFlags.clear();

	// A MESH base (an indexed mesh -- lathe / sweep / skin / imported -- or a
	// displaced base's re-emitted baked mesh) tessellates to itself: its own
	// TessellateToMesh IGNORES `detail`.  Spheres, boxes, SDFs, ... tessellate AT
	// `detail` already and are left completely alone.
	const TriangleMeshGeometryIndexed* pIdx = dynamic_cast<const TriangleMeshGeometryIndexed*>( m_pBase );
	const DisplacedGeometry*           pDsp = dynamic_cast<const DisplacedGeometry*>( m_pBase );
	bool bOk;
	if( pIdx ) {
		bOk = pIdx->TessellateToMeshEx( tris, vertices, normals, coords, m_detail, &uvFlags );
	} else if( pDsp ) {
		bOk = pDsp->TessellateToMeshEx( tris, vertices, normals, coords, m_detail, &uvFlags );
	} else {
		bOk = m_pBase->TessellateToMesh( tris, vertices, normals, coords, m_detail );
	}
	if( !bOk ) {
		return false;
	}
	const bool bMeshBase = ( pIdx != 0 ) || ( pDsp != 0 );
	if( !bMeshBase || !WillDisplaceVertices() || vertices.empty() ) {
		return true;
	}
	// A base that shades with FACE normals (face_normals TRUE) is flattened per
	// corner by its tessellation -- every edge of it is already a crack, each
	// triangle displacing along its own face normal -- so there is no shared
	// topology to refine (refining would only multiply the cracks) and no smooth
	// shading field to carry.  It displaces exactly as before.
	if( ( pIdx && pIdx->UsesFaceNormals() ) || ( pDsp && pDsp->UsesFaceNormals() ) ) {
		return true;
	}

	// REFINE to `detail` (see the construction notes at the top of this file).  A
	// DISPLACED base is never refined: it is already baked at ITS OWN detail (the
	// inner layer governs) and, over an analytic base, its sphere / ellipsoid /
	// disk tessellation carries placeholder pole UVs this layer cannot know about.
	if( m_detail != 0 && pIdx ) {
		Point3 lo( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), hi( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );
		for( size_t i = 0; i < vertices.size(); ++i ) {
			lo.x = r_min( lo.x, vertices[i].x ); lo.y = r_min( lo.y, vertices[i].y ); lo.z = r_min( lo.z, vertices[i].z );
			hi.x = r_max( hi.x, vertices[i].x ); hi.y = r_max( hi.y, vertices[i].y ); hi.z = r_max( hi.z, vertices[i].z );
		}
		const Scalar diag = Vector3Ops::Magnitude( Vector3Ops::mkVector3( hi, lo ) );
		const Scalar target = diag / Scalar( m_detail );

		if( pIdx->GetGrid() ) {
			// (a) a BUILDER base: subdivide its authored grid -- never changes the authored surface
			IndexTriangleListType nt; VerticesListType nv; NormalsListType nn; TexCoordsListType nc;
			std::vector<unsigned char> nf;
			const size_t trisBefore = tris.size();
			if( pIdx->TessellateRefined( target, kMaxRefineTriangles, nt, nv, nn, nc, &nf ) > 0 && !nt.empty() ) {
				tris.swap( nt ); vertices.swap( nv ); normals.swap( nn ); coords.swap( nc ); uvFlags.swap( nf );
				if( bLog ) {
					GlobalLog()->PrintEx( eLog_Event,
						"DisplacedGeometry: refined the builder base's authored grid for detail %u (target edge %g): %u -> %u triangles",
						m_detail, (double)target, (unsigned)trisBefore, (unsigned)tris.size() );
				}
			}
		} else {
			const RefineResult r = RefineUniform( tris, vertices, normals, coords, uvFlags, target );
			if( r.levels > 0 && bLog ) {
				GlobalLog()->PrintEx( eLog_Event,
					"DisplacedGeometry: refined the mesh base to detail %u (target edge %g): %u uniform 1->4 level(s), %u -> %u triangles",
					m_detail, (double)target, r.levels, (unsigned)r.trianglesBefore, (unsigned)r.trianglesAfter );
			}
			if( r.capped && bLog ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"DisplacedGeometry: mesh refinement for detail %u stopped at the %u-level / %u-triangle cap (%u triangles); "
					"the median edge is still longer than the target %g -- raise the base's own tessellation or lower `detail`",
					m_detail, kMaxRefineLevels, (unsigned)kMaxRefineTriangles, (unsigned)tris.size(), (double)target );
			}
		}
	}

	// The base's smooth shading-normal field and its geometric normals on the
	// undisplaced (possibly refined) mesh -- ApplyTransportedShading carries the
	// former through the displacement, REFINED OR NOT, so the look is continuous
	// across the refinement threshold.
	if( !m_bUseFaceNormals ) {
		shading0 = normals;
		geom0 = normals;
		RecomputeVertexNormalsFromTopology( tris, vertices, geom0 );
	}
	return true;
}

bool DisplacedGeometry::TessellateToMeshEx(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail,
	std::vector<unsigned char>* pUVPeriodic ) const
{
	s_tessellateCopyCount.fetch_add( 1, std::memory_order_relaxed );
	const TriangleMeshGeometryIndexed* pIdx = dynamic_cast<const TriangleMeshGeometryIndexed*>( m_pMesh );
	if( !pIdx ) {
		return false;
	}
	return pIdx->TessellateToMeshEx( tris, vertices, normals, coords, detail, pUVPeriodic );
}

unsigned int DisplacedGeometry::GetBuildMeshCount()
{
	return s_buildMeshCount.load( std::memory_order_relaxed );
}

void DisplacedGeometry::ResetBuildMeshCount()
{
	s_buildMeshCount.store( 0, std::memory_order_relaxed );
}

unsigned int DisplacedGeometry::GetTessellateCopyCount()
{
	return s_tessellateCopyCount.load( std::memory_order_relaxed );
}

void DisplacedGeometry::ResetTessellateCopyCount()
{
	s_tessellateCopyCount.store( 0, std::memory_order_relaxed );
}

DisplacedGeometry::DisplacedGeometry(
	IGeometry*            pBase,
	const unsigned int    detail,
	const IFunction2D*    displacement,
	const Scalar          disp_scale,
	const bool            bDoubleSided,
	const bool            bUseFaceNormals,
	const bool            bSeamFold,
	const IScalarPainter* pHeight
	) :
  m_pBase( pBase ),
  m_pDisplacement( displacement ),
  m_pHeight( pHeight ),
  m_dispScale( disp_scale ),
  m_detail( detail ),
  m_bDoubleSided( bDoubleSided ),
  m_bUseFaceNormals( bUseFaceNormals ),
  m_bSeamFold( bSeamFold ),
  m_pPlaneBase( dynamic_cast<const ClippedPlaneGeometry*>( pBase ) ),
  m_pMesh( 0 ),
  m_bRealized( false ),
  m_displacementSubscription(),
  m_topologySignature( 0 )
{
	if( m_pBase ) {
		m_pBase->addref();
	}
	if( m_pDisplacement ) {
		m_pDisplacement->addref();
	}
	if( m_pHeight ) {
		m_pHeight->addref();
	}

	if( !m_pBase ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry: base geometry is null" );
		return;
	}

	// DEFERRED REALIZATION: the tessellate + displace + mesh-build work is
	// NOT done here.  It runs in Realize(), called once single-threaded from
	// the render pipeline (RayCaster::AttachScene) before the parallel
	// rasterize.  The constructor only validates + stores the recipe.  A
	// displaced geometry that is never bound to a rendered object is thus
	// never baked.

	// If the height source exposes the Observable mixin (i.e. it derives
	// from Painter — true for every in-tree painter), subscribe so we rebuild
	// the mesh whenever the painter's keyframable state changes.  Out-of-tree
	// IFunction2D / IScalarPainter implementations that don't derive from
	// Observable simply don't subscribe; we keep today's bake-once behaviour
	// for them.  The two routes are mutually exclusive, so at most one of the
	// dynamic_casts below can find a subject.
	{
		const Observable* obs = m_pDisplacement
			? dynamic_cast<const Observable*>( m_pDisplacement )
			: ( m_pHeight ? dynamic_cast<const Observable*>( m_pHeight ) : 0 );
		if( obs ) {
			m_displacementSubscription = Subscription( obs, [this]{
				// Tier 1 §3 animation refit: instead of destroying and
				// rebuilding the mesh from scratch, re-run tessellate +
				// displacement and feed the new vertices to the existing
				// mesh's UpdateVertices().  Topology (triangle indices,
				// vertex count) is stable across painter notifications
				// because m_detail is a constructor argument — only
				// vertex positions and normals change.  See
				// docs/BVH_ACCELERATION_PLAN.md §4.6 for the full design.
				//
				// DEFERRED-REALIZATION interaction: if the geometry has not
				// been realized yet (painter changed during parse / before
				// the render's realize pass), do NOTHING — baking now would
				// defeat the deferral and the eventual Realize() will pick up
				// the painter's current state anyway.  Only refit once a mesh
				// actually exists.  This callback is always single-threaded
				// (editor / parse-time keyframe propagation).
				if( m_bRealized.load( std::memory_order_acquire ) ) {
					RefreshMeshVertices();
				}
			} );
		}
	}
}

DisplacedGeometry::~DisplacedGeometry()
{
	// The destructor BODY runs before any member destructor.  Detach from the
	// painter's observer list first, while m_pDisplacement is still alive.
	// Otherwise, m_pDisplacement->release() below could delete the painter,
	// and the subscription's own destructor (which runs after this body) would
	// then call Detach() on freed memory.  Move-assigning an empty Subscription
	// triggers the current subscription's Detach via the assignment operator.
	m_displacementSubscription = Subscription();

	DestroyMesh();
	if( m_pDisplacement ) {
		m_pDisplacement->release();
		m_pDisplacement = 0;
	}
	if( m_pHeight ) {
		m_pHeight->release();
		m_pHeight = 0;
	}
	if( m_pBase ) {
		m_pBase->release();
		m_pBase = 0;
	}
}

void DisplacedGeometry::Realize() const
{
	// Fast path: already realized.  Acquire-load so the mesh another thread
	// built under the lock is fully visible.  No lock for the common idempotent
	// call (e.g. a UI-thread PrepareForRendering / picking on already-realized
	// geometry while a viewport render runs on another thread).  The freeze
	// assert is BELOW this early-return, not above it, so an idempotent no-op
	// does NOT false-trip merely because a render is active.
	if( m_bRealized.load( std::memory_order_acquire ) ) {
		return;
	}

	// Serialize the actual bake.  RISE realizes single-threaded before the
	// parallel rasterize, but a GUI can race a viewport render's AttachScene
	// against a UI-thread PrepareForRendering; without this lock both could
	// pass the !m_bRealized check and BuildMesh() the same instance (double
	// alloc / refcount corruption).  Uncontended in normal single-threaded prepare.
	std::lock_guard<std::mutex> realizeLock( m_realizeMutex );
	if( m_bRealized.load( std::memory_order_relaxed ) ) {
		return;
	}

	// DEBUG freeze guard: the bake itself must run single-threaded BEFORE the
	// parallel rasterize (the scene is immutable during it).  Compiles out in release.
	assert( g_renderParallelDepth.load( std::memory_order_seq_cst ) == 0 &&
		"DisplacedGeometry::Realize() during the parallel render \u2014 realize in RayCaster::AttachScene before the rasterize pass" );

	// CASCADE: a displaced base (another DisplacedGeometry) must be realized
	// before we tessellate it — TessellateToMesh re-emits the base's internal
	// mesh, which is null until the base is baked.  No-op for cheap bases.
	if( m_pBase ) {
		m_pBase->Realize();
	}

	BuildMesh();

	// Mark realized AFTER BuildMesh (release, pairing the fast-path acquire).
	// Setting it AFTER — not before — means the painter-observer callback's
	// `if(m_bRealized)` guard sees FALSE during BuildMesh, so a notification
	// fired mid-bake cannot recurse into RefreshMeshVertices -> BuildMesh.
	m_bRealized.store( true, std::memory_order_release );
}

bool DisplacedGeometry::ApplyHeightField(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords ) const
{
	// Did we actually move any vertex positions?  With disp_scale==0 (or
	// neither height route bound) the appliers are a no-op and the analytical
	// per-vertex normals coming out of TessellateToMesh are still correct.
	// Topology-averaged normals would REPLACE those analytic normals with a
	// locally-linear approximation — fine for displaced surfaces, but
	// unnecessarily lossy when nothing was displaced.  This matters a lot for
	// SMS / Manifold-Solver tests that use disp_scale=0 as a "force tessellation
	// of an otherwise analytic shape" idiom: with this shortcut the tessellated
	// sphere's |∂N/∂u|/|∂P/∂u| ratio lands on 1/R (exactly the analytic value)
	// everywhere except the pole-cap degenerate triangles.
	const bool bVerticesDisplaced = WillDisplaceVertices();
	if( !bVerticesDisplaced ) {
		return false;
	}

	// RemapTextureCoords is a tent-fold (u → 1−2u on [0,0.5]; u → 2u−1 on
	// [0.5,1]) used ONLY to keep the displacement value consistent across
	// the u=0 / u=1 wrap seam of closed parametric surfaces (sphere,
	// torus, cylinder).  It is destructive to the linear (u, v)
	// parameterisation that the SMS / Manifold-Solver UV-Jacobian path
	// relies on: every triangle straddling u=0.5 or v=0.5 ends up with
	// a non-monotonic UV triple (tent vertex in the middle), degenerating
	// the 2×2 Jacobian.  Keep the original coords for the mesh and feed
	// a tent-remapped COPY into the displacement evaluator only.
	//
	// BOTH routes get the SAME treatment: the scalar route's synthetic hit
	// carries this same folded `ptCoord`, so a field written against (u,v)
	// reads identically whether it arrives as an IFunction2D or as an
	// IScalarPainter.  Diverging here is the seam-parity bug this shared
	// helper exists to make impossible.
	TexCoordsListType folded;
	if( m_bSeamFold ) {
		folded = coords;
		RemapTextureCoords( folded );
	}
	// open Cartesian field: raw UV, no tent mirror (see ctor bSeamFold)
	TexCoordsListType& evalCoords = m_bSeamFold ? folded : coords;

	if( m_pDisplacement ) {
		ApplyDisplacementMapToObject( tris, vertices, normals, evalCoords, *m_pDisplacement, m_dispScale );
	} else {
		ApplyScalarHeightToObject( tris, vertices, normals, evalCoords, *m_pHeight, m_dispScale );
	}
	return true;
}

void DisplacedGeometry::BuildMesh() const
{
	if( !m_pBase ) {
		return;
	}

	// CASCADE (defensive): ensure the base is realized before we tessellate
	// it.  Realize() reaches here via the m_pBase->Realize() it already did,
	// but the editor rebuild paths (RefreshMeshVertices fallback) call
	// BuildMesh directly — a displaced base must be baked first or
	// TessellateToMesh re-emits an empty mesh.  Idempotent / no-op for cheap
	// bases.
	m_pBase->Realize();

	// Diagnostic: count this bake (see GetBuildMeshCount in the header).
	s_buildMeshCount.fetch_add( 1, std::memory_order_relaxed );

	IndexTriangleListType tris;
	VerticesListType      vertices;
	NormalsListType       normals;
	TexCoordsListType     coords;

	NormalsListType       shading0, geom0;
	std::vector<unsigned char> uvFlags;

	if( !TessellateAndRefine( tris, vertices, normals, coords, /*bLog=*/true, shading0, geom0, uvFlags ) ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry: base geometry does not support tessellation (e.g. InfinitePlaneGeometry)" );
		return;
	}

	// Remember the topology this mesh is built from so the refit path can tell
	// a stable topology from a changed one (see m_topologySignature).
	m_topologySignature = TopologySignature( tris, vertices.size() );

	const VerticesListType preDisplaceVertices = vertices;	// polar-vertex averaging below needs the undisplaced positions
	const bool bVerticesDisplaced = ApplyHeightField( tris, vertices, normals, coords );
	if( bVerticesDisplaced ) {
		AveragePolarDisplacement( preDisplaceVertices, normals, tris, uvFlags, vertices );
		{
			const int weldPolicy = BaseWeldPolicy();
			if( weldPolicy >= 0 ) {
				WeldCoincidentDisplacement( preDisplaceVertices, normals, weldPolicy, vertices );
			}
		}
	}

	if( !m_bUseFaceNormals && bVerticesDisplaced ) {
		RecomputeVertexNormalsFromTopology( tris, vertices, normals );
		ApplyTransportedShading( shading0, geom0, tris, vertices, normals );
	}

	TriangleMeshGeometryIndexed* pNewMesh = new TriangleMeshGeometryIndexed( m_bDoubleSided, m_bUseFaceNormals );
	m_pMesh = pNewMesh;
	GlobalLog()->PrintNew( m_pMesh, __FILE__, __LINE__, "displaced geometry internal mesh" );

	m_pMesh->BeginIndexedTriangles();
	m_pMesh->AddVertices( vertices );
	m_pMesh->AddNormals( normals );
	m_pMesh->AddTexCoords( coords );
	m_pMesh->AddIndexedTriangles( tris );
	m_pMesh->DoneIndexedTriangles();
	// Chart periodicity travels with the mesh (a displaced-of-displaced base reads it), and so does
	// the base's weld policy: a displaced layer over this one welds its coincident vertices by the
	// SAME rule (they leave this bake with different transported shading normals, so without it the
	// outer layer would re-open every crease).  A base that is not a smooth mesh (analytic, SDF,
	// face-normal) leaves the bake unwelded, as before.
	pNewMesh->SetUVPeriodicFlags( uvFlags );
	{
		const int basePolicy = BaseWeldPolicy();
		pNewMesh->SetDisplacementWeldPolicy( basePolicy >= 0 ? basePolicy : int( TriangleMeshGeometryIndexed::kWeldNone ) );
	}
}

void DisplacedGeometry::RefreshMeshVertices()
{
	// Tier 1 §3: refit-not-rebuild observer path.  See header.
	if( !m_pBase || !m_pMesh ) {
		// No current mesh — fall back to full build.
		DestroyMesh();
		BuildMesh();
		return;
	}

	IndexTriangleListType tris;
	VerticesListType      vertices;
	NormalsListType       normals;
	TexCoordsListType     coords;

	NormalsListType       shading0, geom0;
	std::vector<unsigned char> uvFlags;

	if( !TessellateAndRefine( tris, vertices, normals, coords, /*bLog=*/false, shading0, geom0, uvFlags ) ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry::RefreshMeshVertices: base tessellation failed" );
		return;
	}

	const VerticesListType preDisplaceVertices = vertices;	// polar-vertex averaging below needs the undisplaced positions
	const bool bVerticesDisplaced = ApplyHeightField( tris, vertices, normals, coords );
	if( bVerticesDisplaced ) {
		AveragePolarDisplacement( preDisplaceVertices, normals, tris, uvFlags, vertices );
		{
			const int weldPolicy = BaseWeldPolicy();
			if( weldPolicy >= 0 ) {
				WeldCoincidentDisplacement( preDisplaceVertices, normals, weldPolicy, vertices );
			}
		}
	}

	if( !m_bUseFaceNormals && bVerticesDisplaced ) {
		RecomputeVertexNormalsFromTopology( tris, vertices, normals );
		ApplyTransportedShading( shading0, geom0, tris, vertices, normals );
	}

	// Topology assumption: the (possibly refined) triangle list is the one the
	// existing m_pMesh was built from.  m_detail is a const constructor arg, so
	// for a static base this holds across every painter notification.  The
	// mesh-base refinement is driven by edge lengths, so a base that itself
	// moved (an animated displaced base) can change topology at an UNCHANGED
	// vertex count, which UpdateVertices cannot see -- compare the signature and
	// rebuild instead of refitting.  (A vertex-count change is still caught by
	// UpdateVertices' own mismatch path below.)
	if( TopologySignature( tris, vertices.size() ) != m_topologySignature ) {
		GlobalLog()->PrintEasyWarning(
			"DisplacedGeometry::RefreshMeshVertices: base topology changed; rebuilding from scratch" );
		DestroyMesh();
		BuildMesh();
		return;
	}

	const unsigned int refitMs = m_pMesh->UpdateVertices( vertices, normals );
	if( refitMs == 0 ) {
		// Mismatch (vertex count differs) — fall back to full rebuild.
		GlobalLog()->PrintEasyWarning(
			"DisplacedGeometry::RefreshMeshVertices: UpdateVertices failed; rebuilding from scratch" );
		DestroyMesh();
		BuildMesh();
	}
}

void DisplacedGeometry::DestroyMesh() const
{
	safe_release( m_pMesh );
	m_pMesh = 0;
	GeometricUtilities::BumpPatchCertificateEpoch();
}

bool DisplacedGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     /*detail*/ ) const
{
	// Re-emit the internal mesh so a DisplacedGeometry can itself be the base of another
	// DisplacedGeometry.  The detail parameter is ignored — the internal mesh was built at
	// this DisplacedGeometry's own construction-time detail.
	if( !m_pMesh ) {
		return false;
	}

	// Delegate via the internal TriangleMeshGeometryIndexed's own TessellateToMesh, which
	// re-emits its stored arrays with the shared topology intact.
	return TessellateToMeshEx( tris, vertices, normals, coords, 0, 0 );
}

void DisplacedGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( !m_pMesh ) {
		ri.bHit = false;
		return;
	}
	m_pMesh->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );

	// DL-345: displacing a coplanar convex clipped plane moves every vertex
	// along ONE normal (its analytic tessellation normal is constant), so the
	// mesh is the graph of a function over the quad -- an embedded disk,
	// which provably encloses no volume.  The shipped open displaced glass
	// sheets (`sms_k1_refract`, `sms_k2_flatslab`, ...) are exactly this, and
	// the transmissive SPFs cross such a sheet by its FACE.  The base is
	// re-asked per hit because its corners can be keyframed.  Any other base
	// (a mesh, a closed analytic primitive) leaves whatever the realized
	// mesh stamped: the general embedded open-mesh certificate OR the
	// legacy planar proof (DL-382). Object::IntersectRay restricts the wider
	// proof to native clear refraction, preserving other material consumers.
	if( ri.bHit && m_pPlaneBase && m_pPlaneBase->IsPlanarConvexQuad() ) {
		ri.bProvablyNoInterior = true;
	}
}

bool DisplacedGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( !m_pMesh ) {
		return false;
	}
	return m_pMesh->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
}

bool DisplacedGeometry::DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const
{
	// See the header for why this one realizes and the ray forwarders do not.
	Realize();
	if( !m_pMesh ) {
		return false;
	}
	return m_pMesh->DistanceToSurface( ptObject, maxDistObject, outDist );
}

bool DisplacedGeometry::SignedDistanceLower( const Point3& ptObject, const Scalar maxDistObject,
	Scalar& outSigned, bool& outExact ) const
{
	outExact = false;
	// See the header for why this one realizes and the ray forwarders do not.
	Realize();
	if( !m_pMesh ) {
		return false;
	}
	return m_pMesh->SignedDistanceLower( ptObject, maxDistObject, outSigned, outExact );
}

void DisplacedGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	if( !m_pMesh ) {
		ptCenter = Point3( 0.0, 0.0, 0.0 );
		radius   = 0.0;
		return;
	}
	m_pMesh->GenerateBoundingSphere( ptCenter, radius );
}

BoundingBox DisplacedGeometry::GenerateBoundingBox() const
{
	if( !m_pMesh ) {
		return BoundingBox( Point3( 0.0, 0.0, 0.0 ), Point3( 0.0, 0.0, 0.0 ) );
	}
	return m_pMesh->GenerateBoundingBox();
}

void DisplacedGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	if( !m_pMesh ) {
		if( point )  *point  = Point3( 0.0, 0.0, 0.0 );
		if( normal ) *normal = Vector3( 0.0, 0.0, 0.0 );
		if( coord )  *coord  = Point2( 0.0, 0.0 );
		return;
	}
	m_pMesh->UniformRandomPoint( point, normal, coord, prand );
}

Scalar DisplacedGeometry::GetArea() const
{
	if( !m_pMesh ) {
		return 0.0;
	}
	return m_pMesh->GetArea();
}

SurfaceDerivatives DisplacedGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	if( !m_pMesh ) {
		return SurfaceDerivatives();
	}
	return m_pMesh->ComputeSurfaceDerivatives( objSpacePoint, objSpaceNormal );
}

#include "../Interfaces/IFunction2D.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace {
	// The analytical-query twin of GeometryUtilities::ApplyScalarHeightToObject's
	// synthetic hit: SAME fields set, SAME object-space `P` == `Po` convention,
	// SAME invalid footprint / derivatives.  Kept field-for-field in step with
	// that function -- a field that reads one value on the baked mesh and
	// another through this query is exactly the inconsistency SMS's two-stage
	// solver cannot tolerate.
	inline RISE::Scalar EvalHeightField(
		const RISE::IScalarPainter& height,
		const RISE::Point3&         objPos,
		const RISE::Vector3&        objNormal,
		const RISE::Point2&         uv )
	{
		RISE::RayIntersectionGeometric ri( RISE::Ray(), RISE::nullRasterizerState );
		ri.bHit              = true;
		ri.ptObjIntersec     = objPos;
		ri.ptIntersection    = objPos;
		ri.ptCoord           = uv;
		ri.vNormal           = objNormal;
		ri.vGeomNormal       = objNormal;
		ri.txFootprint.valid = false;
		ri.txFootprint.widthValid = false;
		ri.derivatives.valid = false;
		return height.GetValuesAt( ri ).v[0];
	}

	// Tent-fold of [0, 1] -> [0, 1] centred on 0.5; matches the
	// destination-side mapping in GeometryUtilities::RemapTextureCoords
	// that the tessellator uses when evaluating the displacement painter
	// at mesh-build time.  Equivalent to |2u - 1| with the same
	// exact-on-0.5 handling.  Keeping the analytical query consistent
	// with on-mesh values requires the same tent-fold here.
	inline RISE::Scalar TentFold( RISE::Scalar u )
	{
		if( u > 0.5 ) return ( u - 0.5 ) * 2.0;
		if( u < 0.5 ) return 1.0 - ( u * 2.0 );
		return 0.0;
	}
}

bool DisplacedGeometry::ComputeAnalyticalDerivatives(
	const Point2& uv,
	Scalar        smoothing,
	Point3&       outPosition,
	Vector3&      outNormal,
	Vector3&      outDpdu,
	Vector3&      outDpdv,
	Vector3&      outDndu,
	Vector3&      outDndv
	) const
{
	if( !m_pBase ) return false;

	// Smoothing scales the displacement amplitude.  At s = 1 the
	// effective scale is 0 and the surface collapses to the base; at
	// s = 0 we get the full displaced mesh.  Clamped to [0, 1].
	const Scalar sClamped = (smoothing < 0.0) ? 0.0 :
	                        (smoothing > 1.0) ? 1.0 : smoothing;
	const Scalar effDispScale = m_dispScale * (1.0 - sClamped);

	// Pass `smoothing` through to the base — irrelevant for analytical
	// primitives (sphere / ellipsoid) but matters for nested displaced
	// geometry where the base's own displacement should also smooth.
	auto evalDisplacedSurface = [&](
		const Point2& at,
		Point3&  P_d,
		Vector3& N_d,
		Vector3& dpdu_d,
		Vector3& dpdv_d ) -> bool
	{
		Point3  P_b;
		Vector3 N_b, dpdu_b, dpdv_b, dndu_b, dndv_b;
		if( !m_pBase->ComputeAnalyticalDerivatives(
				at, sClamped, P_b, N_b, dpdu_b, dpdv_b, dndu_b, dndv_b ) )
		{
			return false;
		}

		Scalar f = 0.0, dfdu = 0.0, dfdv = 0.0;
		if( m_pDisplacement && effDispScale != 0.0 )
		{
			// Painter is evaluated at TENT-FOLDED (u, v) — matches
			// GeometryUtilities::ApplyDisplacementMapToObject which
			// receives the tent-folded coords (see DisplacedGeometry::
			// BuildMesh).  Using raw (u, v) here would describe a
			// different displacement than the mesh actually has.
			const Scalar tu = m_bSeamFold ? TentFold( at.x ) : at.x;
			const Scalar tv = m_bSeamFold ? TentFold( at.y ) : at.y;
			f = m_pDisplacement->Evaluate( tu, tv );

			// FD df/du, df/dv on the painter.  Step is in raw-(u, v)
			// space; the |2| chain-rule factor for the tent fold cancels
			// (same |2| on both ± probes within either side of 0.5).
			// At u = 0.5 exactly there's a sign-flip — acceptable;
			// SMS chains rarely site on the seam.
			const Scalar epsP = 1.0e-3;
			const Scalar f_uplus  = m_pDisplacement->Evaluate( m_bSeamFold ? TentFold( at.x + epsP ) : ( at.x + epsP ), tv );
			const Scalar f_uminus = m_pDisplacement->Evaluate( m_bSeamFold ? TentFold( at.x - epsP ) : ( at.x - epsP ), tv );
			const Scalar f_vplus  = m_pDisplacement->Evaluate( tu, m_bSeamFold ? TentFold( at.y + epsP ) : ( at.y + epsP ) );
			const Scalar f_vminus = m_pDisplacement->Evaluate( tu, m_bSeamFold ? TentFold( at.y - epsP ) : ( at.y - epsP ) );
			const Scalar inv2 = 1.0 / (2.0 * epsP);
			dfdu = ( f_uplus - f_uminus ) * inv2;
			dfdv = ( f_vplus - f_vminus ) * inv2;
		}
		else if( m_pHeight && effDispScale != 0.0 )
		{
			// SCALAR-FIELD ROUTE.  The field is a function of the 3D point,
			// not of (u, v), so the central difference has to step the
			// POSITION, not just the parameter.  The chain rule via the
			// base's own analytical tangents gives that for free:
			//     P(u ± eps) ≈ P_b ± eps · dP_b/du
			// so df/du = [h(P_b + eps·dpdu) − h(P_b − eps·dpdu)] / 2eps to
			// first order, with NO extra base ComputeAnalyticalDerivatives
			// calls (this function already costs ~5 base evaluations).
			//
			// The synthetic hit's `ptCoord` is stepped IN LOCKSTEP, with the
			// same tent fold the mesh path applies, so a field that happens
			// to read `u`/`v` instead of `Po` differentiates exactly as it
			// does on the IFunction2D route.  (Same |2| tent chain-rule
			// cancellation, and the same accepted sign flip exactly at 0.5,
			// as the branch above.)
			const Scalar tu = m_bSeamFold ? TentFold( at.x ) : at.x;
			const Scalar tv = m_bSeamFold ? TentFold( at.y ) : at.y;
			f = EvalHeightField( *m_pHeight, P_b, N_b, Point2( tu, tv ) );

			const Scalar epsP = 1.0e-3;
			const Scalar tu_p = m_bSeamFold ? TentFold( at.x + epsP ) : ( at.x + epsP );
			const Scalar tu_m = m_bSeamFold ? TentFold( at.x - epsP ) : ( at.x - epsP );
			const Scalar tv_p = m_bSeamFold ? TentFold( at.y + epsP ) : ( at.y + epsP );
			const Scalar tv_m = m_bSeamFold ? TentFold( at.y - epsP ) : ( at.y - epsP );
			const Scalar f_uplus  = EvalHeightField( *m_pHeight, Point3Ops::mkPoint3( P_b, dpdu_b *  epsP ), N_b, Point2( tu_p, tv ) );
			const Scalar f_uminus = EvalHeightField( *m_pHeight, Point3Ops::mkPoint3( P_b, dpdu_b * -epsP ), N_b, Point2( tu_m, tv ) );
			const Scalar f_vplus  = EvalHeightField( *m_pHeight, Point3Ops::mkPoint3( P_b, dpdv_b *  epsP ), N_b, Point2( tu, tv_p ) );
			const Scalar f_vminus = EvalHeightField( *m_pHeight, Point3Ops::mkPoint3( P_b, dpdv_b * -epsP ), N_b, Point2( tu, tv_m ) );
			const Scalar inv2 = 1.0 / (2.0 * epsP);
			dfdu = ( f_uplus - f_uminus ) * inv2;
			dfdv = ( f_vplus - f_vminus ) * inv2;
		}

		// Displaced position: P_d = P_b + s_eff·f·N_b
		P_d = Point3Ops::mkPoint3( P_b, N_b * (effDispScale * f) );

		// Chain rule for displaced tangents:
		//   dP_d/du = dP_b/du + s_eff·(df/du · N_b + f · dN_b/du)
		dpdu_d = Vector3(
			dpdu_b.x + effDispScale * ( dfdu * N_b.x + f * dndu_b.x ),
			dpdu_b.y + effDispScale * ( dfdu * N_b.y + f * dndu_b.y ),
			dpdu_b.z + effDispScale * ( dfdu * N_b.z + f * dndu_b.z ) );
		dpdv_d = Vector3(
			dpdv_b.x + effDispScale * ( dfdv * N_b.x + f * dndv_b.x ),
			dpdv_b.y + effDispScale * ( dfdv * N_b.y + f * dndv_b.y ),
			dpdv_b.z + effDispScale * ( dfdv * N_b.z + f * dndv_b.z ) );

		// Displaced unit normal: cross product of tangents, with handedness
		// reconciled against the base normal.  The (theta, phi) parameter-
		// isation used by EllipsoidGeometry / SphereGeometry produces a
		// LEFT-handed (dpdu, dpdv) frame whose cross points INWARD; the
		// base normal (from the implicit gradient) points OUTWARD.  Without
		// the flip the SMS constraint's tangent-plane projection mirrors
		// and Newton can't converge.  Fall back to the base normal at
		// degenerate poles.
		const Vector3 cross_d = Vector3Ops::Cross( dpdu_d, dpdv_d );
		const Scalar  cMag    = Vector3Ops::Magnitude( cross_d );
		if( cMag > NEARZERO ) {
			N_d = cross_d * (1.0 / cMag);
			if( Vector3Ops::Dot( N_d, N_b ) < 0.0 ) {
				N_d = Vector3( -N_d.x, -N_d.y, -N_d.z );
			}
		} else {
			N_d = N_b;
		}
		return true;
	};

	// Centre evaluation
	if( !evalDisplacedSurface( uv, outPosition, outNormal, outDpdu, outDpdv ) ) {
		return false;
	}

	// dN_d/du, dN_d/dv via central FD on the displaced unit normal.
	// Each probe re-runs evalDisplacedSurface (one base ComputeAnalytical
	// call + five painter evals).  Total cost per query is therefore
	// ~5 base + ~25 painter calls.  At smoothing = 1 the displacement
	// branch is skipped (effDispScale == 0) and we collapse to base
	// derivatives — much cheaper.
	const Scalar epsUv = 1.0e-3;
	Point3  P_dummy;
	Vector3 dpdu_dummy, dpdv_dummy;
	Vector3 N_uplus, N_uminus, N_vplus, N_vminus;
	if( !evalDisplacedSurface( Point2( uv.x + epsUv, uv.y ),
			P_dummy, N_uplus,  dpdu_dummy, dpdv_dummy ) ) return false;
	if( !evalDisplacedSurface( Point2( uv.x - epsUv, uv.y ),
			P_dummy, N_uminus, dpdu_dummy, dpdv_dummy ) ) return false;
	if( !evalDisplacedSurface( Point2( uv.x, uv.y + epsUv ),
			P_dummy, N_vplus,  dpdu_dummy, dpdv_dummy ) ) return false;
	if( !evalDisplacedSurface( Point2( uv.x, uv.y - epsUv ),
			P_dummy, N_vminus, dpdu_dummy, dpdv_dummy ) ) return false;
	const Scalar inv2eps = 1.0 / (2.0 * epsUv);
	outDndu = ( N_uplus  - N_uminus ) * inv2eps;
	outDndv = ( N_vplus  - N_vminus ) * inv2eps;
	return true;
}

// The realized mesh owns the audit and refreshes it during UpdateVertices.
bool DisplacedGeometry::HasUncertainSMSNormalOrientation() const
{
    Realize();
    const auto* mesh=dynamic_cast<const TriangleMeshGeometryIndexed*>(m_pMesh);
    return !mesh || mesh->HasUncertainSMSNormalOrientation();
}

bool DisplacedGeometry::HasUncertifiedOpenness(bool allowGeneralProof) const
{
    if( m_pPlaneBase && m_pPlaneBase->IsPlanarConvexQuad() ) {
        return false;
    }
    Realize();
    const auto* mesh=dynamic_cast<const TriangleMeshGeometryIndexed*>(m_pMesh);
    return mesh && !mesh->IsCertifiedWatertight()
        && !(allowGeneralProof ? mesh->IsProvablyOpenSheet() : mesh->IsZeroVolumeSheet());
}

bool DisplacedGeometry::IsCertifiedWatertight() const
{
    if( m_pPlaneBase && m_pPlaneBase->IsPlanarConvexQuad() ) {
        return false;
    }
    Realize();
    const auto* mesh=dynamic_cast<const TriangleMeshGeometryIndexed*>(m_pMesh);
    return mesh && mesh->IsCertifiedWatertight();
}

bool DisplacedGeometry::IsProvablyOpenSheet(bool allowGeneralProof) const
{
    if( m_pPlaneBase && m_pPlaneBase->IsPlanarConvexQuad() ) {
        return true;
    }
    Realize();
    const auto* mesh=dynamic_cast<const TriangleMeshGeometryIndexed*>(m_pMesh);
    return mesh && (allowGeneralProof ? mesh->IsProvablyOpenSheet() : mesh->IsZeroVolumeSheet());
}

int DisplacedGeometry::TopologicalOpenness() const
{
    if( m_pPlaneBase && m_pPlaneBase->IsPlanarConvexQuad() ) {
        return 1;
    }
    Realize();
    const auto* mesh = dynamic_cast<const TriangleMeshGeometryIndexed*>( m_pMesh );
    return mesh ? mesh->TopologicalOpenness() : -1;
}

// Preserve the pre-DL-382 proof for consumers other than clear refraction.
bool DisplacedGeometry::HasLegacyOpenSheetProof() const
{
    if(m_pPlaneBase && m_pPlaneBase->IsPlanarConvexQuad()) return true;
    Realize();
    const auto* mesh=dynamic_cast<const TriangleMeshGeometryIndexed*>(m_pMesh);
    return mesh && mesh->IsZeroVolumeSheet();
}
bool DisplacedGeometry::HasNegativeOpenSignedVolume() const
{
    if(m_pPlaneBase && m_pPlaneBase->IsPlanarConvexQuad()) return false;
    Realize();
    const auto* mesh=dynamic_cast<const TriangleMeshGeometryIndexed*>(m_pMesh);
    return mesh && mesh->IsProvablyOpenSheet() && mesh->HasNegativeOpenSignedVolume();
}
