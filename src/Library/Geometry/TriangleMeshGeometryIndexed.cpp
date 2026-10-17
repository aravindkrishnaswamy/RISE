//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshGeometryIndexed.cpp - Implementation of the TriangleMesh
//  GeometryIndexed class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshGeometryIndexed.h"
#include "../Utilities/ExpressionMemo.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "GeometryUtilities.h"
#include "../Utilities/stl_utils.h"
// BSPTreeSAH / Octree headers are still needed in the .cpp because v1/v2/v3
// .risemesh files have BSP/octree byte blocks that we read into local
// temporaries during Deserialize so the byte stream advances correctly,
// even though the parsed trees are never used (BVH owns intersection now).
// They are NOT pulled into the .h — this class no longer carries any
// BSP/octree state, only the ability to consume the legacy bytes.
#include "../Octree.h"
#include "../BSPTreeSAH.h"
#include "../Utilities/RenderParallelScope.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#ifdef RISE_ENABLE_MAILBOXING
#include <atomic>
#endif

inline unsigned int VoidPtrToUInt( const void* v )
{
	return (unsigned int)reinterpret_cast<uintptr_t>(v);
}

using namespace RISE;
using namespace RISE::Implementation;

#ifdef RISE_ENABLE_MAILBOXING
namespace {
	static std::atomic<unsigned int> s_nextGeometryId(1);

	struct MailboxState {
		unsigned int rayId;
		std::vector<unsigned int> stamps;
	};

	static thread_local std::unordered_map<unsigned int, MailboxState> tl_mailboxes;

	static inline MailboxState& GetMailbox(unsigned int geomId, size_t numTris)
	{
		MailboxState& mb = tl_mailboxes[geomId];
		if( mb.stamps.size() < numTris ) {
			mb.stamps.assign(numTris, 0);
			mb.rayId = 0;
		}
		return mb;
	}
}
#endif

#include "../Utilities/SurfaceCurvature.h"	// scaleHint stamping in the specializations below
#include "TriangleMeshGeometryIndexedSpecializations.h"

TriangleMeshGeometryIndexed::TriangleMeshGeometryIndexed(
	const bool bDoubleSided_,
	const bool bUseFaceNormals_
	) :
  bDoubleSided( bDoubleSided_ ),
  bUseFaceNormals( bUseFaceNormals_ ),
  m_bWatertight( false ),
  pPtrBVH( 0 )
#ifdef RISE_ENABLE_MAILBOXING
  , geometryId( s_nextGeometryId.fetch_add(1) )
#endif
{
}

TriangleMeshGeometryIndexed::~TriangleMeshGeometryIndexed()
{
	safe_release( pPtrBVH );
}

bool TriangleMeshGeometryIndexed::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	return TessellateToMeshEx( tris, vertices, normals, coords, detail, 0 );
}

void TriangleMeshGeometryIndexed::SetUVPeriodicRegion( const unsigned char mask, const unsigned int firstVertex, const unsigned int count )
{
	if( m_uvPeriodic.size() < pPoints.size() ) {
		m_uvPeriodic.resize( pPoints.size(), 0 );
	}
	const size_t end = std::min( static_cast<size_t>( firstVertex ) + count, m_uvPeriodic.size() );
	for( size_t i = firstVertex; i < end; ++i ) {
		m_uvPeriodic[i] |= mask;
	}
}

namespace {
	// A vertex created on an authored EDGE: key = (lower authored index, higher, k steps from the lower).
	struct EdgePointKey
	{
		std::uint32_t a, b, k;
		bool operator==( const EdgePointKey& o ) const { return a == o.a && b == o.b && k == o.k; }
	};
	struct EdgePointKeyHash
	{
		size_t operator()( const EdgePointKey& e ) const
		{
			std::uint64_t h = e.a;
			h = h * 0x9E3779B97F4A7C15ull + e.b;
			h = h * 0x9E3779B97F4A7C15ull + e.k;
			h ^= h >> 31;
			return static_cast<size_t>( h );
		}
	};
}

int TriangleMeshGeometryIndexed::TessellateRefined(
	const Scalar           targetEdge,
	const size_t           maxTriangles,
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	std::vector<unsigned char>* pUVFlags ) const
{
	if( !m_grid || !( targetEdge > 0 ) || ptr_polygons.empty() ) {
		return 0;
	}
	const size_t nV = pPoints.size();
	if( pNormals.size() != nV || pCoords.size() != nV ) {
		return 0;		// the grid indexes the same-index arrays a builder emits
	}
	const MeshGrid& g = *m_grid;
	const unsigned int maxCount = 4096;

	auto len = [&]( const unsigned int a, const unsigned int b ) {
		return Vector3Ops::Magnitude( Vector3Ops::mkVector3( pPoints[a], pPoints[b] ) );
	};

	// Longest authored edge of each family.
	std::vector<Scalar> famLen( g.numFamilies, 0.0 );
	for( size_t i = 0; i < g.quads.size(); ++i ) {
		const MeshGrid::Quad& q = g.quads[i];
		famLen[q.famS] = std::max( famLen[q.famS], std::max( len( q.q[0], q.q[1] ), len( q.q[3], q.q[2] ) ) );
		famLen[q.famT] = std::max( famLen[q.famT], std::max( len( q.q[0], q.q[3] ), len( q.q[1], q.q[2] ) ) );
	}
	for( size_t i = 0; i < g.fans.size(); ++i ) {
		const MeshGrid::Fan& f = g.fans[i];
		const unsigned int P = f.t[f.pole], R1 = f.t[( f.pole + 1 ) % 3], R2 = f.t[( f.pole + 2 ) % 3];
		famLen[f.famSpoke] = std::max( famLen[f.famSpoke], std::max( len( P, R1 ), len( P, R2 ) ) );
		famLen[f.famRing]  = std::max( famLen[f.famRing], len( R1, R2 ) );
	}

	// Per-family subdivision counts for spacing t, grown until the triangle budget fits.
	std::vector<unsigned int> cnt( g.numFamilies, 1 );
	Scalar t = targetEdge;
	for( int iter = 0; iter < 80; ++iter ) {
		for( unsigned int f = 0; f < g.numFamilies; ++f ) {
			const double c = std::ceil( famLen[f] / t );
			cnt[f] = c < 1.0 ? 1u : ( c > double( maxCount ) ? maxCount : static_cast<unsigned int>( c ) );
		}
		double est = double( ptr_polygons.size() );
		for( size_t i = 0; i < g.quads.size(); ++i ) {
			const MeshGrid::Quad& q = g.quads[i];
			est += 2.0 * double( cnt[q.famS] ) * double( cnt[q.famT] ) - 2.0;
		}
		for( size_t i = 0; i < g.fans.size(); ++i ) {
			const MeshGrid::Fan& f = g.fans[i];
			est += double( cnt[f.famRing] ) * ( 2.0 * double( cnt[f.famSpoke] ) - 1.0 ) - 1.0;
		}
		if( est <= double( maxTriangles ) ) break;
		t *= 1.15;
	}
	bool anyRefined = false;
	for( unsigned int f = 0; f < g.numFamilies; ++f ) if( cnt[f] > 1 ) anyRefined = true;
	if( !anyRefined ) {
		return 0;
	}

	// ---- which authored triangles a cell replaces ----
	std::vector<unsigned char> replaced( ptr_polygons.size(), 0 );
	for( size_t i = 0; i < g.quads.size(); ++i ) { replaced[g.quads[i].tri[0]] = 1; replaced[g.quads[i].tri[1]] = 1; }
	for( size_t i = 0; i < g.fans.size(); ++i ) replaced[g.fans[i].tri] = 1;

	VerticesListType  outV = pPoints;
	NormalsListType   outN = pNormals;
	TexCoordsListType outC = pCoords;
	std::vector<unsigned char> flags( nV, 0 );
	for( size_t i = 0; i < nV && i < m_uvPeriodic.size(); ++i ) flags[i] = m_uvPeriodic[i];
	const std::vector<unsigned char> authoredFlags = flags;
	IndexTriangleListType outT;

	// A new vertex at the barycentric combination (ids[i], w[i]) of up to 3 AUTHORED vertices.
	auto addVertex = [&]( const unsigned int* ids, const double* w, const int n ) -> unsigned int {
		Scalar px = 0, py = 0, pz = 0;
		Vector3 nm( 0, 0, 0 );
		for( int i = 0; i < n; ++i ) {
			px += w[i] * pPoints[ids[i]].x; py += w[i] * pPoints[ids[i]].y; pz += w[i] * pPoints[ids[i]].z;
			nm = nm + pNormals[ids[i]] * w[i];
		}
		const Scalar nl = Vector3Ops::Magnitude( nm );
		if( !( nl > 1e-12 ) ) nm = pNormals[ids[0]]; else nm = nm * ( 1.0 / nl );
		// chart: per axis, a POLAR corner (placeholder coordinate) is left out and the weights
		// renormalized; on an axis every remaining corner declares periodic, the corners are
		// unwrapped (short way round, modulo 1) before the weighted sum.
		Scalar uv[2] = { 0, 0 };
		for( int axis = 0; axis < 2; ++axis ) {
			const unsigned char polarBit = axis == 0 ? kUPolar : kVPolar, perBit = axis == 0 ? kUPeriodic : kVPeriodic;
			Scalar cs[3]; double ws[3]; bool allPer = true; int m = 0; double wsum = 0;
			for( int pass = 0; pass < 2 && m == 0; ++pass ) {
				for( int i = 0; i < n; ++i ) {
					if( !( w[i] > 0 ) ) continue;
					if( pass == 0 && ( authoredFlags[ids[i]] & polarBit ) ) continue;
					cs[m] = axis == 0 ? pCoords[ids[i]].x : pCoords[ids[i]].y; ws[m] = w[i]; wsum += w[i];
					if( !( authoredFlags[ids[i]] & perBit ) ) allPer = false;
					++m;
				}
			}
			if( m == 0 ) { cs[0] = axis == 0 ? pCoords[ids[0]].x : pCoords[ids[0]].y; ws[0] = 1; wsum = 1; m = 1; allPer = false; }
			Scalar val = 0, lo = cs[0];
			for( int i = 0; i < m; ++i ) {
				Scalar c = cs[i];
				if( allPer && i > 0 ) c -= std::floor( c - cs[0] + 0.5 );		// within half a turn of the first
				lo = std::min( lo, cs[i] );
				val += ( ws[i] / wsum ) * c;
			}
			if( allPer ) val -= std::floor( val - std::floor( lo ) );
			uv[axis] = val;
		}
		unsigned char fl = 0xFF;
		for( int i = 0; i < n; ++i ) fl &= authoredFlags[ids[i]];
		fl &= static_cast<unsigned char>( kUPeriodic | kVPeriodic );			// a new vertex is never polar
		const unsigned int idx = static_cast<unsigned int>( outV.size() );
		outV.push_back( Point3( px, py, pz ) );
		outN.push_back( nm );
		outC.push_back( TexCoord( uv[0], uv[1] ) );
		flags.push_back( fl );
		return idx;
	};

	std::unordered_map<EdgePointKey, unsigned int, EdgePointKeyHash> edgePts;
	// k-th of n equal divisions along the authored edge X -> Y (shared with every cell on that edge).
	auto edgePoint = [&]( const unsigned int X, const unsigned int Y, const unsigned int n, const unsigned int k ) -> unsigned int {
		if( k == 0 ) return X;
		if( k == n ) return Y;
		const bool fwd = X < Y;
		EdgePointKey key; key.a = fwd ? X : Y; key.b = fwd ? Y : X; key.k = fwd ? k : n - k;
		const auto it = edgePts.find( key );
		if( it != edgePts.end() ) return it->second;
		const double f = double( key.k ) / double( n );
		const unsigned int ids[2] = { key.a, key.b };
		const double w[2] = { 1.0 - f, f };
		const unsigned int idx = addVertex( ids, w, 2 );
		edgePts[key] = idx;
		return idx;
	};

	for( size_t qi = 0; qi < g.quads.size(); ++qi ) {
		const MeshGrid::Quad& q = g.quads[qi];
		const unsigned int Ms = cnt[q.famS], Mt = cnt[q.famT];
		std::vector<unsigned int> lat( size_t( Ms + 1 ) * size_t( Mt + 1 ) );
		for( unsigned int j = 0; j <= Mt; ++j ) {
			for( unsigned int i = 0; i <= Ms; ++i ) {
				unsigned int v;
				if( j == 0 )       v = edgePoint( q.q[0], q.q[1], Ms, i );
				else if( j == Mt ) v = edgePoint( q.q[3], q.q[2], Ms, i );
				else if( i == 0 )  v = edgePoint( q.q[0], q.q[3], Mt, j );
				else if( i == Ms ) v = edgePoint( q.q[1], q.q[2], Mt, j );
				else {
					// interior: the point (s,t) of the authored quad, taken on whichever authored
					// triangle (q0,q1,q2) | (q0,q2,q3) the fold q0-q2 puts it in
					const double s = double( i ) / double( Ms ), tt = double( j ) / double( Mt );
					if( s >= tt ) {
						const unsigned int ids[3] = { q.q[0], q.q[1], q.q[2] }; const double w[3] = { 1.0 - s, s - tt, tt };
						v = addVertex( ids, w, 3 );
					} else {
						const unsigned int ids[3] = { q.q[0], q.q[2], q.q[3] }; const double w[3] = { 1.0 - tt, s, tt - s };
						v = addVertex( ids, w, 3 );
					}
				}
				lat[ size_t( j ) * ( Ms + 1 ) + i ] = v;
			}
		}
		auto emit = [&]( const unsigned int a, const unsigned int b, const unsigned int c ) { outT.push_back( MakeIndexedTriangleSameIdx( a, b, c ) ); };
		for( unsigned int j = 0; j < Mt; ++j ) {
			for( unsigned int i = 0; i < Ms; ++i ) {
				const unsigned int c00 = lat[ size_t( j ) * ( Ms + 1 ) + i ], c10 = lat[ size_t( j ) * ( Ms + 1 ) + i + 1 ];
				const unsigned int c11 = lat[ size_t( j + 1 ) * ( Ms + 1 ) + i + 1 ], c01 = lat[ size_t( j + 1 ) * ( Ms + 1 ) + i ];
				emit( c00, c10, c11 );		// the authored orientation: (q0,q1,q2) and (q0,q2,q3)
				emit( c00, c11, c01 );
			}
		}
	}

	for( size_t fi = 0; fi < g.fans.size(); ++fi ) {
		const MeshGrid::Fan& f = g.fans[fi];
		const unsigned int P = f.t[f.pole], R1 = f.t[( f.pole + 1 ) % 3], R2 = f.t[( f.pole + 2 ) % 3];
		const unsigned int kB = cnt[f.famSpoke], mc = cnt[f.famRing];
		// V(s,u): s = 0..kB outward from the pole, u = 0..mc along the ring edge R1 -> R2
		std::vector<unsigned int> V( size_t( kB + 1 ) * size_t( mc + 1 ), P );
		for( unsigned int s = 1; s <= kB; ++s ) {
			for( unsigned int u = 0; u <= mc; ++u ) {
				unsigned int v;
				if( s == kB )      v = edgePoint( R1, R2, mc, u );
				else if( u == 0 )  v = edgePoint( P, R1, kB, s );
				else if( u == mc ) v = edgePoint( P, R2, kB, s );
				else {
					const double sg = double( s ) / double( kB ), uf = double( u ) / double( mc );
					const unsigned int ids[3] = { P, R1, R2 }; const double w[3] = { 1.0 - sg, sg * ( 1.0 - uf ), sg * uf };
					v = addVertex( ids, w, 3 );
				}
				V[ size_t( s ) * ( mc + 1 ) + u ] = v;
			}
		}
		auto emit = [&]( const unsigned int a, const unsigned int b, const unsigned int c ) { outT.push_back( MakeIndexedTriangleSameIdx( a, b, c ) ); };
		for( unsigned int u = 0; u < mc; ++u ) emit( P, V[ size_t( 1 ) * ( mc + 1 ) + u ], V[ size_t( 1 ) * ( mc + 1 ) + u + 1 ] );
		for( unsigned int s = 1; s < kB; ++s ) {
			for( unsigned int u = 0; u < mc; ++u ) {
				const unsigned int A = V[ size_t( s ) * ( mc + 1 ) + u ], B = V[ size_t( s ) * ( mc + 1 ) + u + 1 ];
				const unsigned int C = V[ size_t( s + 1 ) * ( mc + 1 ) + u + 1 ], D = V[ size_t( s + 1 ) * ( mc + 1 ) + u ];
				emit( A, D, C );
				emit( A, C, B );
			}
		}
	}

	// authored triangles no cell replaced (end caps, degenerate leftovers) pass through
	const Point3* pBase = &pPoints[0];
	for( size_t i = 0; i < ptr_polygons.size(); ++i ) {
		if( replaced[i] ) continue;
		const PointerTriangle& src = ptr_polygons[i];
		outT.push_back( MakeIndexedTriangleSameIdx(
			static_cast<unsigned int>( src.pVertices[0] - pBase ),
			static_cast<unsigned int>( src.pVertices[1] - pBase ),
			static_cast<unsigned int>( src.pVertices[2] - pBase ) ) );
	}

	tris.swap( outT );
	vertices.swap( outV );
	normals.swap( outN );
	coords.swap( outC );
	if( pUVFlags ) pUVFlags->swap( flags );
	return 1;
}

bool TriangleMeshGeometryIndexed::TessellateToMeshEx(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     /*detail*/,
	std::vector<unsigned char>* pUVPeriodic ) const
{
	// Emit the stored triangles with same-index (iVertices == iNormals ==
	// iCoords) semantics.  The source mesh stores pPoints, pNormals, pCoords as
	// INDEPENDENTLY indexed attribute arrays (a position can be shared across
	// faces with distinct normals/UVs), but downstream displacement code
	// (`ApplyDisplacementMapToObject`) indexes vNormals and vCoords by the
	// VERTEX index -- so each distinct (position, normal, uv) tuple becomes ONE
	// output vertex.
	//
	// SHARING IS PRESERVED.  Each corner is keyed by its ORIGINAL index triple
	// (recovered by pointer subtraction against the attribute arrays' bases --
	// `ptr_polygons` is authoritative because DoneIndexedTriangles() clears
	// indexedtris) and every distinct triple is assigned exactly one output
	// index, in first-seen order (deterministic, so the vertex count/order is
	// stable for UpdateVertices / RefreshMeshVertices).  For the procedural
	// builders (lathe / sweep / skin / loft: MakeIndexedTriangleSameIdx meshes
	// with smooth per-vertex normals) this reproduces the builder's own topology
	// exactly: interior vertices stay shared by every incident triangle, while
	// hard edges, caps and pinch poles -- which the builders intentionally
	// DUPLICATE with different normals -- keep distinct triples and stay split.
	// Normals and UVs are never merged across distinct indices, so UV seams
	// survive.  This matters to DisplacedGeometry: its post-displacement
	// RecomputeVertexNormalsFromTopology averages face normals per vertex INDEX,
	// so a shared vertex gets a smooth normal where the old bake (one tuple per
	// triangle corner, 3N vertices) gave every index exactly one face and
	// therefore flat shading.
	//
	// FACE-NORMAL source meshes store no normals (pNormals empty,
	// src.pNormals[k]==null).  Their corners are NOT merged: the per-triangle
	// face normal emitted for them differs triangle to triangle, so each such
	// triangle keeps its own three flattened vertices.
	//
	// DoneIndexedTriangles() clears indexedtris after converting to pointer-triangle
	// form, so ptr_polygons is the authoritative source.
	if( ptr_polygons.empty() ) {
		return false;
	}

	struct CornerKey
	{
		std::uint32_t v, n, c;
		bool operator==( const CornerKey& o ) const { return v == o.v && n == o.n && c == o.c; }
	};
	struct CornerKeyHash
	{
		size_t operator()( const CornerKey& k ) const
		{
			std::uint64_t h = k.v;
			h = h * 0x9E3779B97F4A7C15ull + k.n;
			h = h * 0x9E3779B97F4A7C15ull + k.c;
			h ^= h >> 29;
			return static_cast<size_t>( h );
		}
	};
	std::unordered_map<CornerKey, unsigned int, CornerKeyHash> cornerIdx;
	cornerIdx.reserve( pPoints.size() );

	const Point3*  const pPointBase  = pPoints.empty()  ? 0 : &pPoints[0];
	const Vector3* const pNormalBase = pNormals.empty() ? 0 : &pNormals[0];
	const Point2*  const pCoordBase  = pCoords.empty()  ? 0 : &pCoords[0];

	for( MyPointerTriangleList::const_iterator it = ptr_polygons.begin(); it != ptr_polygons.end(); ++it ) {
		const PointerTriangle& src = *it;

		IndexedTriangle t;

		if( src.pNormals[0] && pPointBase && pNormalBase && pCoordBase ) {
			// Shared-topology path: one output vertex per distinct original triple.
			for( int k = 0; k < 3; k++ ) {
				CornerKey key;
				key.v = static_cast<std::uint32_t>( src.pVertices[k] - pPointBase );
				key.n = static_cast<std::uint32_t>( src.pNormals[k]  - pNormalBase );
				key.c = static_cast<std::uint32_t>( src.pCoords[k]   - pCoordBase );

				unsigned int idx;
				const auto found = cornerIdx.find( key );
				if( found != cornerIdx.end() ) {
					idx = found->second;
				} else {
					idx = static_cast<unsigned int>( vertices.size() );
					vertices.push_back( *src.pVertices[k] );
					normals.push_back( *src.pNormals[k] );
					coords.push_back( *src.pCoords[k] );
					if( pUVPeriodic ) {
						pUVPeriodic->push_back( key.v < m_uvPeriodic.size() ? m_uvPeriodic[key.v] : 0 );
					}
					cornerIdx.insert( std::make_pair( key, idx ) );
				}
				t.iVertices[k] = idx;
				t.iNormals[k]  = idx;
				t.iCoords[k]   = idx;
			}
		} else {
			// Face-normal source: per-corner flattening.  We can't emit a constant
			// placeholder like +Z -- downstream code (DisplacedGeometry) displaces
			// vertices ALONG the stored normal BEFORE any later normal recompute, so
			// a constant placeholder would push every corner along +Z regardless of
			// the triangle's actual orientation.  Compute the true face normal from
			// the triangle's vertex positions and emit it for all three corners.
			// Guarded against degenerate triangles (zero-area).
			Vector3 faceNormal( 0.0, 0.0, 1.0 );
			if( !src.pNormals[0] ) {
				const Vector3 e1 = Vector3Ops::mkVector3( *src.pVertices[1], *src.pVertices[0] );
				const Vector3 e2 = Vector3Ops::mkVector3( *src.pVertices[2], *src.pVertices[0] );
				const Vector3 cross = Vector3Ops::Cross( e1, e2 );
				const Scalar mag = Vector3Ops::Magnitude( cross );
				if( mag > NEARZERO ) {
					faceNormal = cross * (1.0 / mag);
				}
			}

			for( int k = 0; k < 3; k++ ) {
				const unsigned int localIdx = static_cast<unsigned int>( vertices.size() );
				vertices.push_back( *src.pVertices[k] );
				normals.push_back( src.pNormals[k] ? *src.pNormals[k] : faceNormal );
				coords.push_back( *src.pCoords[k] );
				if( pUVPeriodic ) {
					const size_t pi = pPointBase ? static_cast<size_t>( src.pVertices[k] - pPointBase ) : 0;
					pUVPeriodic->push_back( pi < m_uvPeriodic.size() ? m_uvPeriodic[pi] : 0 );
				}
				t.iVertices[k] = localIdx;
				t.iNormals[k]  = localIdx;
				t.iCoords[k]   = localIdx;
			}
		}
		tris.push_back( t );
	}

	return true;
}

void TriangleMeshGeometryIndexed::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool /*bComputeExitInfo*/ ) const
{
    ri.bHasShaderDirection=ri.bHasNormalMapFrame=false;
	// Triangle mesh geometry never generates exit information, it just ignores that command!

	// Bump the mailbox ray ID so duplicate triangles in multiple BSP leaves are skipped
#ifdef RISE_ENABLE_MAILBOXING
	{
		MailboxState& mb = GetMailbox(geometryId, ptr_polygons.size());
		++mb.rayId;
	}
#endif

	// Cleanup §3+§4: BVH-only.  pPtrBSPtree / pPtrOctree members
	// remain on the class for v1/v2 .risemesh deserialize compat
	// (legacy on-disk data is read into temp instances and discarded)
	// but never reached at runtime.
	if( pPtrBVH ) {
		pPtrBVH->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}

	if( ri.bHit && bDoubleSided ) {
		// Flip the normal if we must
		if( Vector3Ops::Dot(ri.vNormal, ri.ray.Dir()) > 0 ) {
			ri.vNormal = -ri.vNormal;
			// Sibling of CSGObject.cpp's P1-1 fix (and of the identical
			// fix in TriangleMeshGeometry.cpp's non-indexed twin):
			// `ri.derivatives.dndu/dndv` (set inside pPtrBVH->IntersectRay,
			// per-vertex-normal-interpolation derivatives -- direction-
			// independent, a pure function of surface position) are
			// derivatives of the UN-flipped normal.  The flip just above
			// changes which normal FIELD `ri.vNormal` reports; its
			// derivative must flip with it or a back-facing hit on a
			// double-sided mesh ships curvature for the wrong-signed
			// normal.
			if( ri.derivatives.valid ) {
				ri.derivatives.dndu = -ri.derivatives.dndu;
				ri.derivatives.dndv = -ri.derivatives.dndv;
			}
		}
		// Mirror the flip on the geometric (face) normal, and record
		// whether it happened — see TriangleMeshGeometry::IntersectRay
		// companion (RayIntersectionGeometric::bGeomNormalOrientedToRay
		// doc comment) for the full rationale.  DisplacedGeometry's
		// IntersectRay delegates straight into this function on its
		// internal TriangleMeshGeometryIndexed, so displaced double-
		// sided meshes get the flag for free.
		const bool bFlipGeomNormal = Vector3Ops::Dot(ri.vGeomNormal, ri.ray.Dir()) > 0;
		if( bFlipGeomNormal ) {
			ri.vGeomNormal = -ri.vGeomNormal;
		}
		ri.bGeomNormalOrientedToRay = bFlipGeomNormal;
		// DL-96: a double-sided mesh that is NOT certified watertight
		// (DL-143's position-weld check, m_bWatertight) has no
		// single true "outside" -- it is an open sheet where both faces
		// are legitimate physical sides.  See
		// RayIntersectionGeometric::bOpenSheet's doc comment for the one
		// consumer (the BSSRDF front-face gate) that reads this.
		ri.bOpenSheet = !m_bWatertight;
	}
	// DL-382: an embedded, consistently wound open mesh is certified for the face rule
	// (see m_bProvablyOpenSheet), so the transmissive SPFs cross it by its
	// FACE exactly as they cross a clipped plane (DL-345).  Stamped on
	// every hit, front or back, like ClippedPlaneGeometry; any other mesh
	// leaves the record's `false`.
	if( ri.bHit && m_bProvablyOpenSheet ) {
		ri.bProvablyNoInterior = true;
	}
}

bool TriangleMeshGeometryIndexed::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
#ifdef RISE_ENABLE_MAILBOXING
	{
		MailboxState& mb = GetMailbox(geometryId, ptr_polygons.size());
		++mb.rayId;
	}
#endif

	// Cleanup §3+§4: BVH-only.
	if( pPtrBVH ) {
		return pPtrBVH->IntersectRay_IntersectionOnly( ray, dHowFar, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}
	return false;
}

void TriangleMeshGeometryIndexed::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// Find the desired triangle where the CDF is greater than the rand value
	TriangleAreasList::const_iterator it = std::lower_bound( areasCDF.begin(), areasCDF.end(), prand.z );

	int idx = static_cast<int>(areasCDF.size())-1;
	if( it != areasCDF.end() ) {
		idx = static_cast<int>(std::distance( areasCDF.begin(), it ));
	}

	GeometricUtilities::PointOnTriangle( point, normal, coord, ptr_polygons[idx], prand.x, prand.y );
}

Scalar TriangleMeshGeometryIndexed::GetArea( ) const
{
	return totalArea;
}

void TriangleMeshGeometryIndexed::EnsureReferencedVertices()
{
	if( m_vertexReferenced.size() == pPoints.size() && ( pPoints.empty() || m_referencedVertexCount > 0 || ptr_polygons.empty() ) ) {
		return;
	}
	m_vertexReferenced.assign( pPoints.size(), 0 );
	m_referencedVertexCount = 0;
	if( pPoints.empty() || ptr_polygons.empty() ) {
		return;
	}
	const Point3* const pBase = pPoints.data();
	for( std::size_t t = 0; t < ptr_polygons.size(); ++t ) {
		const PointerTriangle& tri = ptr_polygons[t];
		for( int k = 0; k < 3; ++k ) {
			if( !tri.pVertices[k] ) {
				continue;
			}
			const std::ptrdiff_t off = tri.pVertices[k] - pBase;
			if( off >= 0 && static_cast<std::size_t>( off ) < pPoints.size() ) {
				unsigned char& ref = m_vertexReferenced[static_cast<std::size_t>( off )];
				if( !ref ) {
					ref = 1;
					++m_referencedVertexCount;
				}
			}
		}
	}
}

bool TriangleMeshGeometryIndexed::WeldReferencedVertices()
{
	EnsureReferencedVertices();
	if( m_referencedVertexCount == 0 ) {
		m_lastWeldedId.clear();
		m_lastWeldEps = Scalar( 0 );
		m_lastWeldTransitive = false;
		m_lastWeldValid = false;
		return false;
	}
	if( m_referencedVertexCount == pPoints.size() ) {
		m_lastWeldValid = GeometricUtilities::WeldVertexPositions(
			pPoints, m_lastWeldedId, &m_lastWeldEps, Scalar( 0 ), &m_lastWeldTransitive );
		return m_lastWeldValid;
	}
	VerticesListType refPoints;
	refPoints.reserve( m_referencedVertexCount );
	for( std::size_t i = 0; i < pPoints.size(); ++i ) {
		if( m_vertexReferenced[i] ) {
			refPoints.push_back( pPoints[i] );
		}
	}
	std::vector<unsigned int> refWeldedId;
	m_lastWeldValid = GeometricUtilities::WeldVertexPositions(
		refPoints, refWeldedId, &m_lastWeldEps, Scalar( 0 ), &m_lastWeldTransitive );
	if( !m_lastWeldValid || refWeldedId.size() != m_referencedVertexCount ) {
		m_lastWeldedId.clear();
		m_lastWeldValid = false;
		return false;
	}
	m_lastWeldedId.assign( pPoints.size(), 0xFFFFFFFFu );
	std::size_t r = 0;
	for( std::size_t i = 0; i < pPoints.size(); ++i ) {
		if( m_vertexReferenced[i] ) {
			m_lastWeldedId[i] = refWeldedId[r++];
		}
	}
	return true;
}

void TriangleMeshGeometryIndexed::InvalidateTopologyProofCaches()
{
	m_faceNormals.clear();
	m_vertexReferenced.clear();
	m_referencedVertexCount = 0;
	m_lastWeldedId.clear();
	m_lastWeldEps = Scalar( 0 );
	m_lastWeldTransitive = false;
	m_lastWeldValid = false;
	m_cachedOpenTopologyWeldedId.clear();
	m_cachedTopologicalOpenRefusal = false;
	m_cachedOpenTopologyPassed = false;
	m_certifiedOpenExactSeams = false;
	m_certifiedOpenSeparation = Scalar( 0 );
	m_certifiedOpenPositions.clear();
	m_certifiedOpenSeamPairs.clear();
	openRefusalTris[0] = openRefusalTris[1] = size_t( -1 );
	m_watertightTopologyWeldedId.clear();
	m_cachedTopologicalWatertightRefusal = false;
	m_watertightEdges.clear();
	m_topologicalOpenness.store( -2, std::memory_order_release );
	GeometricUtilities::BumpPatchCertificateEpoch();
}

void TriangleMeshGeometryIndexed::BeginIndexedTriangles( )
{
    m_uvPeriodic.clear();	// chart bits describe the vertices about to be (re)fed
    m_grid.reset();			// a builder's grid structure describes the mesh it just built, not a re-fed one
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
    m_bOpenSignedVolumeNegative=false;
	InvalidateTopologyProofCaches();
	safe_release( pPtrBVH );
	areas.clear();
	areasCDF.clear();
	// Stale topology, same reasoning as the bakes below: a re-fed mesh's
	// old watertightness answer describes edges that no longer exist.
	// DoneIndexedTriangles recomputes it once the new topology is final.
	m_bWatertight = false;
	// Any bake describes the OLD vertex data.  In practice this runs at
	// construction, before anything could have queried a signal, but the
	// invariant "a table always describes the vertices currently in this
	// geometry" has to hold at every mutation site, not just the plausible
	// ones.
	InvalidateSignalBakes();
}

void TriangleMeshGeometryIndexed::AddVertex( const Point3& point )
{
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
	InvalidateTopologyProofCaches();
	pPoints.push_back( point );
}

void TriangleMeshGeometryIndexed::AddNormal( const Vector3& normal )
{
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
	GeometricUtilities::BumpPatchCertificateEpoch();
	if( !bUseFaceNormals ) {
		pNormals.push_back( normal );
	}
}

void TriangleMeshGeometryIndexed::AddTexCoord( const Point2& coord )
{
	pCoords.push_back( coord );
}

void TriangleMeshGeometryIndexed::AddVertices( const VerticesListType& points )
{
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
	InvalidateTopologyProofCaches();
	pPoints.insert( pPoints.end(), points.begin(), points.end() );
}

void TriangleMeshGeometryIndexed::AddNormals( const NormalsListType& normals )
{
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
	GeometricUtilities::BumpPatchCertificateEpoch();
	if( !bUseFaceNormals ) {
		pNormals.insert( pNormals.end(), normals.begin(), normals.end() );
	}
}

void TriangleMeshGeometryIndexed::AddTexCoords( const TexCoordsListType& coords )
{
	pCoords.insert( pCoords.end(), coords.begin(), coords.end() );
}

void TriangleMeshGeometryIndexed::AddColor( const VertexColor& color )
{
	pColors.push_back( color );
}

void TriangleMeshGeometryIndexed::AddColors( const VertexColorsListType& colors )
{
	pColors.insert( pColors.end(), colors.begin(), colors.end() );
}

// ITriangleMeshGeometryIndexed3 — tangents + secondary UV (TEXCOORD_1).
//
// Live-only state for now.  Phase 1 of glTF import (docs/GLTF_IMPORT.md)
// adds these storage hooks to enable downstream Phase 2 consumers
// (`normal_map_modifier`, secondary-UV occlusion sampling) without
// re-touching the loader.  Serialize / Deserialize do not yet round-trip
// these arrays through .risemesh — callers that save a glTF-loaded mesh
// to .risemesh will lose tangents / TEXCOORD_1.  When Phase 2 lands, the
// .risemesh format gets a version bump and these arrays start persisting.
void TriangleMeshGeometryIndexed::AddTangent( const Tangent4& tangent )
{
	pTangents.push_back( tangent );
}

void TriangleMeshGeometryIndexed::AddTangents( const Tangent4ListType& tangents )
{
	pTangents.insert( pTangents.end(), tangents.begin(), tangents.end() );
}

void TriangleMeshGeometryIndexed::AddTexCoord1( const TexCoord& coord )
{
	pTexCoords1.push_back( coord );
}

void TriangleMeshGeometryIndexed::AddTexCoords1( const TexCoordsListType& coords )
{
	pTexCoords1.insert( pTexCoords1.end(), coords.begin(), coords.end() );
}

void TriangleMeshGeometryIndexed::AddIndexedTriangle( const IndexedTriangle& tri )
{
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
	InvalidateTopologyProofCaches();
	indexedtris.push_back( tri );
}

void TriangleMeshGeometryIndexed::AddIndexedTriangles( const IndexTriangleListType& tris )
{
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
	InvalidateTopologyProofCaches();
	indexedtris.insert( indexedtris.end(), tris.begin(), tris.end() );
}

unsigned int TriangleMeshGeometryIndexed::UpdateVertices(
	const VerticesListType& newVertices,
	const NormalsListType&  newNormals )
{
	// Tier 1 §3: refit-not-rebuild path for keyframed-painter-driven
	// DisplacedGeometry.  Topology (ptr_polygons indices) is preserved;
	// only vertex positions and normals change.

	// DEBUG contract guard, the same one DisplacedGeometry::Realize() uses:
	// this function rewrites vertex data and refits the BVH under any
	// traversal that happens to be running, so it is a BETWEEN-FRAMES
	// operation and nothing else.  It is asserted rather than trusted: motion-
	// blur temporal sampling calls IAnimator::EvaluateAtTime per pixel
	// sample, and a keyframed displacement painter's notification reaches
	// here synchronously through DisplacedGeometry::RefreshMeshVertices.
	// That path is single-threaded (a motion-blurred frame whose keyframes
	// reach geometry renders on the calling thread -- DL-465's time-indexed
	// multi-threaded path refuses them; docs/ARCHITECTURE.md "Animation /
	// Temporal Sampling", DL-457), so the assert holds; a DEBUG build says so
	// if it ever stops.
	assert( g_renderParallelDepth.load( std::memory_order_seq_cst ) == 0 &&
		"TriangleMeshGeometryIndexed::UpdateVertices() during the parallel render — vertex mutation "
		"and BVH refit are between-frames operations (docs/ARCHITECTURE.md, Animation / Temporal Sampling)" );

	if( !pPtrBVH ) {
		GlobalLog()->PrintEasyWarning(
			"TriangleMeshGeometryIndexed::UpdateVertices:: no BVH yet — call DoneIndexedTriangles first" );
		return 0;
	}
	if( newVertices.size() != pPoints.size() ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshGeometryIndexed::UpdateVertices:: vertex count mismatch (%u vs %u)",
			(unsigned)newVertices.size(), (unsigned)pPoints.size() );
		return 0;
	}
	if( !bUseFaceNormals && newNormals.size() != pNormals.size() ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshGeometryIndexed::UpdateVertices:: normal count mismatch (%u vs %u)",
			(unsigned)newNormals.size(), (unsigned)pNormals.size() );
		return 0;
	}

    EnsureReferencedVertices();
    const bool hasUnreferenced = m_referencedVertexCount < pPoints.size();
    bool positionsChanged=false;
    bool graphProofReusable=openGraphAxis>=0;
    const auto coordinate=[](const Point3& p,int k) {return k==0?p.x:k==1?p.y:p.z;};
    bool refusalProofReusable=openRefusalPair[0]<newVertices.size() && openRefusalPair[1]<newVertices.size()
        && (!hasUnreferenced || (m_vertexReferenced[openRefusalPair[0]] && m_vertexReferenced[openRefusalPair[1]]));
    if(refusalProofReusable) for(int k=0;k<3;++k) {
        if(coordinate(newVertices[openRefusalPair[0]],k)!=coordinate(newVertices[openRefusalPair[1]],k))
            refusalProofReusable=false;
    }
    for(size_t i=0;i<pPoints.size();++i) {
        if(hasUnreferenced && !m_vertexReferenced[i]) continue;
        const Point3& a=pPoints[i]; const Point3& b=newVertices[i];
        for(int k=0;k<3;++k) {
            if(coordinate(a,k)!=coordinate(b,k)) {
                positionsChanged=true;
                if(k!=openGraphAxis) graphProofReusable=false;
            }
            if(!std::isfinite(coordinate(b,k))) {graphProofReusable=false;refusalProofReusable=false;}
        }
    }
    if(graphProofReusable) for(const auto& pair : openGraphCoincidentPairs) {
        if(coordinate(newVertices[pair.first],openGraphAxis)!=coordinate(newVertices[pair.second],openGraphAxis)) {
            graphProofReusable=false; break;
        }
    }
	// In-place vertex / normal replacement.  ptr_polygons hold pointers
	// INTO pPoints/pNormals — those pointers stay valid as long as we
	// resize 0 (we're overwriting, not reallocating).  We use plain
	// assignment to overwrite without changing storage; if the old
	// arrays were sized to match (always true post-DoneIndexedTriangles)
	// the underlying storage is reused.
	for( size_t i = 0; i < pPoints.size(); ++i ) {
		pPoints[i] = newVertices[i];
	}
	if( !bUseFaceNormals ) {
		for( size_t i = 0; i < pNormals.size(); ++i ) {
			pNormals[i] = newNormals[i];
		}
	}
	if( positionsChanged ) {
		m_lastWeldValid = false;
		m_topologicalOpenness.store( -2, std::memory_order_release );
	}

	// Recompute triangle areas + CDF (vertex positions changed).
	ComputeAreas(positionsChanged,graphProofReusable,refusalProofReusable);
    // Array indices stay fixed, but the position-weld partition and folded
    // face discriminator do not. Re-prove closed membership after motion,
    // under exactly the rule DoneIndexedTriangles applies (P2-1): a
    // certified open sheet is never watertight, which ComputeWatertightness
    // itself enforces.  Exactly coincident triangle corners weld to one id,
    // so the full proof would refuse them as degenerate; skip it there.
    if(positionsChanged) {
        if(refusalProofReusable)
            m_bWatertight=false;
        else
            ComputeWatertightness();
    }
	GeometricUtilities::BumpPatchCertificateEpoch();


	// The mesh has genuinely CHANGED SHAPE under a stable IGeometry*, which
	// is the one case design doc §7.2's free-invalidation argument does not
	// cover (§14 item 10 asked whether it could happen; this path is the
	// answer).  Every per-vertex signal table now describes the previous
	// frame's geometry, so drop them and let the next production read
	// rebuild.  Same between-frames contract as the rest of this function.
	InvalidateSignalBakes();

	// Refit the BVH (bottom-up AABB recompute + filter + BVH4 redo).
	const unsigned int refitMs = pPtrBVH->Refit();

	// Tier C3 SAH-degradation safeguard: refit preserves topology but
	// per-node bboxes can grow as keyframed vertices move.  When the
	// SAH cost exceeds 2× the freshly-built tree's, ray-traversal
	// expected cost has more than doubled — refit is no longer
	// adequate and we should rebuild from scratch.  Free the BVH and
	// reconstruct from ptr_polygons; the caller (DisplacedGeometry's
	// RefreshMeshVertices observer) sees the original refitMs return,
	// not the rebuild time, but that's fine — the rebuild is logged
	// and the caller's perf budget already had to accept the worse
	// of refit-time and degradation cost.
	if( pPtrBVH->SAHDegradationRatio() > 2.0 ) {
		GlobalLog()->PrintEx( eLog_Info,
			"TriangleMeshGeometryIndexed::UpdateVertices:: SAH ratio %.2fx > 2.0, "
			"rebuilding BVH from polygon data instead of refit",
			(double)pPtrBVH->SAHDegradationRatio() );

		safe_release( pPtrBVH );

		BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
		                  Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
		std::vector<const PointerTriangle*> temp;
		for( MyPointerTriangleList::iterator pi = ptr_polygons.begin(); pi != ptr_polygons.end(); ++pi ) {
			temp.push_back( &(*pi) );
		}
		for( MyPointsList::const_iterator mi = pPoints.begin(); mi != pPoints.end(); ++mi ) {
			bbox.Include( *mi );
		}

		AccelerationConfig cfg;
		cfg.maxLeafSize         = 4;
		cfg.binCount            = 32;
		cfg.sahTraversalCost    = 1.0;
		cfg.sahIntersectionCost = 1.0;
		cfg.doubleSided         = bDoubleSided;

		pPtrBVH = new BVH<const PointerTriangle*>( *this, temp, bbox, cfg );
		GlobalLog()->PrintNew( pPtrBVH, __FILE__, __LINE__, "pointers BVH (rebuilt after SAH-degradation)" );
	}

	// 0 means FAILURE to the caller (vertex-count mismatch / no BVH above), but a
	// refit of a small mesh really can take under a millisecond; report 1 so a
	// fast successful refit is not mistaken for a failed one and rebuilt.
	return refitMs ? refitMs : 1;
}

void TriangleMeshGeometryIndexed::ComputeAreas(bool positionsChanged, bool graphProofReusable, bool refusalProofReusable)
{
	// Self-clearing: the inner loops both push_back, so we MUST clear
	// before recomputing or the CDF grows quadratically across repeated
	// calls.  Tier 1 §3 added an observer path
	// (DisplacedGeometry::RefreshMeshVertices → UpdateVertices →
	// ComputeAreas) that fires once per keyframe, so a corrupted CDF
	// would crash UniformRandomPoint with an out-of-bounds idx into
	// ptr_polygons after a few keyframes.  See Tier A2 review notes,
	// 2026-04-27.
	areas.clear();
	areasCDF.clear();
	m_faceNormals.clear();
	areas.reserve( ptr_polygons.size() );
	areasCDF.reserve( ptr_polygons.size() );
	m_faceNormals.reserve( ptr_polygons.size() );
	totalArea = 0;
    smsUncertainNormalOrientation = ptr_polygons.empty();
    ++smsOrientationAudits;
    smsOrientationTriangleVisits += ptr_polygons.size();

	// Compute triangle areas, unit face normals, and referenced-corner bounds.
	int axisSign[3] = { 0, 0, 0 };
	const bool needAxisSign = positionsChanged && !graphProofReusable && !refusalProofReusable;
	Point3 refLo( 0, 0, 0 ), refHi( 0, 0, 0 );
	{
		for( std::size_t t = 0; t < ptr_polygons.size(); ++t ) {
			const PointerTriangle& thisTri = ptr_polygons[t];
			const Point3& p0 = *thisTri.pVertices[0];
			const Point3& p1 = *thisTri.pVertices[1];
			const Point3& p2 = *thisTri.pVertices[2];
			if( t == 0 ) {
				refLo = refHi = p0;
			} else {
				refLo.x = std::min( refLo.x, p0.x ); refLo.y = std::min( refLo.y, p0.y ); refLo.z = std::min( refLo.z, p0.z );
				refHi.x = std::max( refHi.x, p0.x ); refHi.y = std::max( refHi.y, p0.y ); refHi.z = std::max( refHi.z, p0.z );
			}
			refLo.x = std::min( refLo.x, std::min( p1.x, p2.x ) );
			refLo.y = std::min( refLo.y, std::min( p1.y, p2.y ) );
			refLo.z = std::min( refLo.z, std::min( p1.z, p2.z ) );
			refHi.x = std::max( refHi.x, std::max( p1.x, p2.x ) );
			refHi.y = std::max( refHi.y, std::max( p1.y, p2.y ) );
			refHi.z = std::max( refHi.z, std::max( p1.z, p2.z ) );

			Vector3 vEdgeA = Vector3Ops::mkVector3( p1, p0 );
			Vector3 vEdgeB = Vector3Ops::mkVector3( p2, p0 );
			const Vector3 face=Vector3Ops::Cross(vEdgeA,vEdgeB);
			if( needAxisSign ) {
				const Scalar fc[3] = { face.x, face.y, face.z };
				for( int a = 0; a < 3; ++a ) {
					const int s = ( fc[a] > Scalar( 0 ) ) ? 1 : ( ( fc[a] < Scalar( 0 ) ) ? -1 : 2 );
					if( s == 2 || !std::isfinite( fc[a] ) ) {
						axisSign[a] = 2;
					} else if( axisSign[a] == 0 ) {
						axisSign[a] = s;
					} else if( axisSign[a] != s ) {
						axisSign[a] = 2;
					}
				}
			}
            const Scalar faceLength=Vector3Ops::Magnitude(face);
            const Vector3 unitFace=faceLength>Scalar(0)?face*(Scalar(1)/faceLength):Vector3(0,0,0);
            m_faceNormals.push_back( unitFace );
            // Match the native face-normal tessellation convention, including
            // its degenerate-face fallback, without materializing extra arrays.
            const Vector3 faceNormal=faceLength>NEARZERO?unitFace:Vector3(0,0,1);
            for(unsigned k=0;k<3;++k) {
                const Scalar orientation=Vector3Ops::Dot(face,thisTri.pNormals[k]?*thisTri.pNormals[k]:faceNormal);
                if(!std::isfinite(orientation) || orientation<0) smsUncertainNormalOrientation=true;
            }
            const Scalar thisArea = faceLength * 0.5;
			totalArea += thisArea;
			areas.push_back( thisArea );
		}
	}

	// Compute the areas CDF
	if( totalArea > 0 ) {
		const Scalar invArea = 1.0 / totalArea;
		Scalar sum = 0;

		TriangleAreasList::const_iterator i, e;
		for( i=areas.begin(), e=areas.end(); i!=e; i++ ) {
			sum += (*i) * invArea;
			areasCDF.push_back( sum );
		}
	}

    // Position-only proof cached across unchanged-position updates. Normal
    // agreement is always re-audited above, even when the proof is reused.
    if(positionsChanged && refusalProofReusable) {
        // Exactly coincident corners force the general certificate to refuse
        // on every such frame. The legacy planar test ignores degenerate
        // triangles, so it must still be recomputed independently.
        m_bZeroVolumeSheet=GeometricUtilities::IsPlanarConsistentlyWoundSheet(
            ptr_polygons.size(),[this](size_t t,int k)->const Point3& {return *ptr_polygons[t].pVertices[k];});
        m_bShapeOpenSheet=m_bZeroVolumeSheet;
        m_bOpenSignedVolumeNegative=false;
    }
    if(positionsChanged && graphProofReusable) {
        // The projected embedded disk and all projected coordinates stayed
        // fixed. Changing heights preserves its graph, hence embedding and
        // openness. Recompute the planar refusal and signed-volume warning.
        m_bZeroVolumeSheet=GeometricUtilities::IsPlanarConsistentlyWoundSheet(
            ptr_polygons.size(),[this](size_t t,int k)->const Point3& {return *ptr_polygons[t].pVertices[k];});
        // Match the full certificate's centre: unused storage vertices are
        // not part of this open surface and cannot affect its warning sign.
        const Point3 centre((refLo.x+refHi.x)/2,(refLo.y+refHi.y)/2,(refLo.z+refHi.z)/2);
        Scalar volume=0;
        for(const auto& triangle : ptr_polygons) {
            volume+=Vector3Ops::Dot(Vector3Ops::mkVector3(*triangle.pVertices[0],centre),
                Vector3Ops::Cross(Vector3Ops::mkVector3(*triangle.pVertices[1],centre),
                    Vector3Ops::mkVector3(*triangle.pVertices[2],centre)));
        }
        m_bOpenSignedVolumeNegative=!m_bZeroVolumeSheet && volume<0;
    }
    if(positionsChanged && !graphProofReusable && !refusalProofReusable) {
        openGraphAxis=-1;
        openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
        openGraphCoincidentPairs.clear();
        m_bZeroVolumeSheet = GeometricUtilities::IsPlanarConsistentlyWoundSheet(
            ptr_polygons.size(), [this](size_t t, int k) -> const Point3& { return *ptr_polygons[t].pVertices[k]; });
        m_bShapeOpenSheet = m_bZeroVolumeSheet;
        m_bOpenSignedVolumeNegative = false;
        if( m_bZeroVolumeSheet ) {
            ++openCertificateAudits;
            m_cachedOpenTopologyWeldedId.clear();
            m_cachedTopologicalOpenRefusal = false;
            m_cachedOpenTopologyPassed = false;
            m_certifiedOpenExactSeams = false;
            m_certifiedOpenSeparation = Scalar( 0 );
            m_certifiedOpenPositions.clear();
            m_certifiedOpenSeamPairs.clear();
            openRefusalTris[0] = openRefusalTris[1] = size_t( -1 );
        } else if( !ptr_polygons.empty() && !pPoints.empty() ) {
            // DL-623 (3): weld only the vertices referenced by `ptr_polygons`
            // so unused storage vertices cannot inflate `m_lastWeldEps` or
            // force a second weld on `corners`.
            WeldReferencedVertices();
            const bool hasUnreferenced = m_referencedVertexCount < pPoints.size();
            const Scalar diag3D = Vector3Ops::Magnitude(
                Vector3( refHi.x - refLo.x, refHi.y - refLo.y, refHi.z - refLo.z ) );
            const Scalar eps3D = m_lastWeldValid ? m_lastWeldEps
                : std::max( Scalar( 1e-9 ), Scalar( 1e-6 ) * diag3D );
            const bool pointWeldMatchesCorners = m_lastWeldValid && m_lastWeldTransitive
                && std::isfinite( eps3D ) && eps3D > Scalar( 0 );
            const Point3* const pBase = pPoints.data();

            // Check cached topological / triangle-pair refusal witnesses before
            // materializing corners or running the full surface certificate.
            bool reusedOpenRefusal = false;
            if( pointWeldMatchesCorners && !m_cachedOpenTopologyWeldedId.empty()
                && m_lastWeldedId == m_cachedOpenTopologyWeldedId )
            {
                if( m_cachedTopologicalOpenRefusal ) {
                    reusedOpenRefusal = true;
                } else if( openRefusalTris[0] < ptr_polygons.size() && openRefusalTris[1] < ptr_polygons.size() ) {
                    if( openRefusalTris[0] == openRefusalTris[1] ) {
                        const std::size_t t = openRefusalTris[0];
                        const PointerTriangle& tri = ptr_polygons[t];
                        const unsigned int I[3] = {
                            m_lastWeldedId[static_cast<std::size_t>( tri.pVertices[0] - pBase )],
                            m_lastWeldedId[static_cast<std::size_t>( tri.pVertices[1] - pBase )],
                            m_lastWeldedId[static_cast<std::size_t>( tri.pVertices[2] - pBase )]
                        };
                        if( I[0] == I[1] || I[1] == I[2] || I[0] == I[2] ) {
                            reusedOpenRefusal = true;
                        } else {
                            const Scalar twiceArea = Scalar( 2 ) * areas[t];
                            Scalar longest = 0;
                            for( int k = 0; k < 3; ++k ) {
                                longest = std::max( longest, Vector3Ops::Magnitude(
                                    Vector3Ops::mkVector3( *tri.pVertices[(k + 1) % 3], *tri.pVertices[k] ) ) );
                            }
                            if( !( twiceArea > eps3D * longest ) ) {
                                reusedOpenRefusal = true;
                            }
                        }
                    } else {
                        const std::size_t ti = openRefusalTris[0];
                        const std::size_t tj = openRefusalTris[1];
                        const PointerTriangle& triI = ptr_polygons[ti];
                        const PointerTriangle& triJ = ptr_polygons[tj];
                        const Point3 Pi[3] = { *triI.pVertices[0], *triI.pVertices[1], *triI.pVertices[2] };
                        const Point3 Pj[3] = { *triJ.pVertices[0], *triJ.pVertices[1], *triJ.pVertices[2] };
                        const unsigned int Ii[3] = {
                            m_lastWeldedId[static_cast<std::size_t>( triI.pVertices[0] - pBase )],
                            m_lastWeldedId[static_cast<std::size_t>( triI.pVertices[1] - pBase )],
                            m_lastWeldedId[static_cast<std::size_t>( triI.pVertices[2] - pBase )]
                        };
                        const unsigned int Ij[3] = {
                            m_lastWeldedId[static_cast<std::size_t>( triJ.pVertices[0] - pBase )],
                            m_lastWeldedId[static_cast<std::size_t>( triJ.pVertices[1] - pBase )],
                            m_lastWeldedId[static_cast<std::size_t>( triJ.pVertices[2] - pBase )]
                        };
                        if( Ii[0] != Ii[1] && Ii[1] != Ii[2] && Ii[0] != Ii[2] &&
                            Ij[0] != Ij[1] && Ij[1] != Ij[2] && Ij[0] != Ij[2] &&
                            !GeometricUtilities::CheckOpenSurfaceStep4Pair( Pi, Ii, Pj, Ij, eps3D * eps3D ) )
                        {
                            reusedOpenRefusal = true;
                        }
                    }
                }
            }

            // DL-623 (2): reuse a certified 3D non-graph open-sheet proof across
            // deformations that preserve the transitive weld partition, exact
            // seam coincidences, Step-1 altitudes, and the Step-4 separation
            // lower bound `m_certifiedOpenSeparation` (under either $A = I_3$ or
            // the best-fit $3 \times 3$ linear map $A$ certified via Sylvester's
            // criterion on $A^T A$).
            bool reusedOpenCertified = false;
            if( !reusedOpenRefusal && pointWeldMatchesCorners &&
                m_cachedOpenTopologyPassed && m_certifiedOpenExactSeams &&
                !m_cachedOpenTopologyWeldedId.empty() &&
                m_lastWeldedId == m_cachedOpenTopologyWeldedId &&
                m_certifiedOpenPositions.size() == pPoints.size() &&
                m_certifiedOpenSeparation > eps3D )
            {
                bool seamsExact = true;
                for( const auto& pair : m_certifiedOpenSeamPairs ) {
                    const Point3& a = pPoints[pair.first];
                    const Point3& b = pPoints[pair.second];
                    if( a.x != b.x || a.y != b.y || a.z != b.z ) {
                        seamsExact = false;
                        break;
                    }
                }
                if( seamsExact ) {
                    Point3 dMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
                    Point3 dMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );
                    std::size_t firstRef = std::size_t( -1 );
                    for( std::size_t i = 0; i < pPoints.size(); ++i ) {
                        if( hasUnreferenced && !m_vertexReferenced[i] ) continue;
                        if( firstRef == std::size_t( -1 ) ) firstRef = i;
                        const Point3& p = pPoints[i];
                        const Point3& q = m_certifiedOpenPositions[i];
                        const Scalar dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
                        dMin.x = std::min( dMin.x, dx ); dMin.y = std::min( dMin.y, dy ); dMin.z = std::min( dMin.z, dz );
                        dMax.x = std::max( dMax.x, dx ); dMax.y = std::max( dMax.y, dy ); dMax.z = std::max( dMax.z, dz );
                    }
                    const Scalar spanI = Vector3Ops::Magnitude(
                        Vector3( dMax.x - dMin.x, dMax.y - dMin.y, dMax.z - dMin.z ) );
                    bool clearanceCertified = std::isfinite( spanI ) &&
                        ( m_certifiedOpenSeparation - spanI > eps3D );
                    if( !clearanceCertified && firstRef != std::size_t( -1 ) ) {
                        const Point3& p0 = pPoints[firstRef];
                        const Point3& q0 = m_certifiedOpenPositions[firstRef];
                        Scalar cUU[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
                        Scalar cVU[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
                        Point3 qLo = q0, qHi = q0;
                        for( std::size_t i = firstRef; i < pPoints.size(); ++i ) {
                            if( hasUnreferenced && !m_vertexReferenced[i] ) continue;
                            const Point3& p = pPoints[i];
                            const Point3& q = m_certifiedOpenPositions[i];
                            qLo.x = std::min( qLo.x, q.x ); qLo.y = std::min( qLo.y, q.y ); qLo.z = std::min( qLo.z, q.z );
                            qHi.x = std::max( qHi.x, q.x ); qHi.y = std::max( qHi.y, q.y ); qHi.z = std::max( qHi.z, q.z );
                            const Scalar u[3] = { q.x - q0.x, q.y - q0.y, q.z - q0.z };
                            const Scalar v[3] = { p.x - p0.x, p.y - p0.y, p.z - p0.z };
                            for( int r = 0; r < 3; ++r ) {
                                for( int c = 0; c < 3; ++c ) {
                                    cUU[r][c] += u[r] * u[c];
                                    cVU[r][c] += v[r] * u[c];
                                }
                            }
                        }
                        const Scalar trU = cUU[0][0] + cUU[1][1] + cUU[2][2];
                        if( std::isfinite( trU ) && trU > Scalar( 0 ) ) {
                            const Scalar invTr = Scalar( 1 ) / trU;
                            Scalar u[3][3], vu[3][3];
                            for( int r = 0; r < 3; ++r ) {
                                for( int c = 0; c < 3; ++c ) {
                                    u[r][c] = cUU[r][c] * invTr;
                                    vu[r][c] = cVU[r][c] * invTr;
                                }
                            }
                            Scalar cof[3][3];
                            cof[0][0] = u[1][1] * u[2][2] - u[1][2] * u[2][1];
                            cof[0][1] = u[1][2] * u[2][0] - u[1][0] * u[2][2];
                            cof[0][2] = u[1][0] * u[2][1] - u[1][1] * u[2][0];
                            cof[1][0] = u[0][2] * u[2][1] - u[0][1] * u[2][2];
                            cof[1][1] = u[0][0] * u[2][2] - u[0][2] * u[2][0];
                            cof[1][2] = u[0][1] * u[2][0] - u[0][0] * u[2][1];
                            cof[2][0] = u[0][1] * u[1][2] - u[0][2] * u[1][1];
                            cof[2][1] = u[0][2] * u[1][0] - u[0][0] * u[1][2];
                            cof[2][2] = u[0][0] * u[1][1] - u[0][1] * u[1][0];
                            const Scalar detU = u[0][0] * cof[0][0] + u[0][1] * cof[0][1] + u[0][2] * cof[0][2];
                            if( std::isfinite( detU ) && detU > Scalar( 1e-18 ) ) {
                                const Scalar invDetU = Scalar( 1 ) / detU;
                                Scalar A[3][3];
                                for( int r = 0; r < 3; ++r ) {
                                    for( int c = 0; c < 3; ++c ) {
                                        A[r][c] = ( vu[r][0] * cof[c][0] + vu[r][1] * cof[c][1] + vu[r][2] * cof[c][2] ) * invDetU;
                                    }
                                }
                                const Scalar detA = A[0][0] * ( A[1][1] * A[2][2] - A[1][2] * A[2][1] )
                                    - A[0][1] * ( A[1][0] * A[2][2] - A[1][2] * A[2][0] )
                                    + A[0][2] * ( A[1][0] * A[2][1] - A[1][1] * A[2][0] );
                                if( std::isfinite( detA ) && detA > Scalar( 1e-12 ) ) {
                                    Scalar G[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
                                    for( int r = 0; r < 3; ++r ) {
                                        for( int c = 0; c < 3; ++c ) {
                                            G[r][c] = A[0][r] * A[0][c] + A[1][r] * A[1][c] + A[2][r] * A[2][c];
                                        }
                                    }
                                    const Scalar trG = G[0][0] + G[1][1] + G[2][2];
                                    const Scalar epsMach = std::numeric_limits<Scalar>::epsilon();
                                    if( std::isfinite( trG ) && trG > Scalar( 0 ) ) {
                                        // DL-623 review: lambda_min(A^T A) by a rigorous
                                        // bound.  SymmetricMinEigenvalueLowerBound3 proves a
                                        // lower bound on the eigenvalues of the STORED G
                                        // (Cholesky with a backward-error margin; the old
                                        // bisection on the signs of computed Sylvester
                                        // minors was not rigorous when two eigenvalues are
                                        // small: the minors' rounding error relative to
                                        // their value grows like tr(G)^2 / lambda).  The
                                        // stored G differs from the exact A^T A of the
                                        // stored A by at most gamma_3 sqrt(G_rr G_cc) per
                                        // entry (Cauchy-Schwarz; each op charged 2u, so
                                        // gamma_3 <= 3 eps), i.e. by at most 9 eps tr(G) in
                                        // the 2-norm (Frobenius over nine entries); Weyl's
                                        // inequality moves the eigenvalue by no more, and
                                        // 16 eps tr(G) covers it with the rounding of trG.
                                        const Scalar eigLo = std::max( Scalar( 0 ),
                                            GeometricUtilities::SymmetricMinEigenvalueLowerBound3( G )
                                            - Scalar( 16 ) * epsMach * trG );
                                        const Scalar sigmaLo = std::sqrt( eigLo );
                                        if( sigmaLo * m_certifiedOpenSeparation > eps3D ) {
                                            Point3 rMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
                                            Point3 rMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );
                                            for( std::size_t i = firstRef; i < pPoints.size(); ++i ) {
                                                if( hasUnreferenced && !m_vertexReferenced[i] ) continue;
                                                const Point3& p = pPoints[i];
                                                const Point3& q = m_certifiedOpenPositions[i];
                                                const Scalar ux = q.x - q0.x, uy = q.y - q0.y, uz = q.z - q0.z;
                                                const Scalar rx = ( p.x - p0.x ) - ( A[0][0] * ux + A[0][1] * uy + A[0][2] * uz );
                                                const Scalar ry = ( p.y - p0.y ) - ( A[1][0] * ux + A[1][1] * uy + A[1][2] * uz );
                                                const Scalar rz = ( p.z - p0.z ) - ( A[2][0] * ux + A[2][1] * uy + A[2][2] * uz );
                                                rMin.x = std::min( rMin.x, rx ); rMin.y = std::min( rMin.y, ry ); rMin.z = std::min( rMin.z, rz );
                                                rMax.x = std::max( rMax.x, rx ); rMax.y = std::max( rMax.y, ry ); rMax.z = std::max( rMax.z, rz );
                                            }
                                            const Scalar spanA = Vector3Ops::Magnitude(
                                                Vector3( rMax.x - rMin.x, rMax.y - rMin.y, rMax.z - rMin.z ) );
                                            const Scalar diagQ = Vector3Ops::Magnitude(
                                                Vector3( qHi.x - qLo.x, qHi.y - qLo.y, qHi.z - qLo.z ) );
                                            const Scalar roundMargin = Scalar( 32 ) * epsMach *
                                                ( diag3D + std::sqrt( trG ) * diagQ );
                                            if( std::isfinite( spanA ) &&
                                                sigmaLo * m_certifiedOpenSeparation - ( spanA + roundMargin ) > eps3D )
                                            {
                                                clearanceCertified = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if( clearanceCertified ) {
                        bool altitudesValid = true;
                        for( std::size_t t = 0; t < ptr_polygons.size(); ++t ) {
                            const PointerTriangle& tri = ptr_polygons[t];
                            const Scalar twiceArea = Scalar( 2 ) * areas[t];
                            Scalar longest = 0;
                            for( int k = 0; k < 3; ++k ) {
                                longest = std::max( longest, Vector3Ops::Magnitude(
                                    Vector3Ops::mkVector3( *tri.pVertices[(k + 1) % 3], *tri.pVertices[k] ) ) );
                            }
                            if( !( twiceArea > eps3D * longest ) ) {
                                altitudesValid = false;
                                break;
                            }
                        }
                        if( altitudesValid ) {
                            reusedOpenCertified = true;
                            m_bShapeOpenSheet = true;
                            const Point3 centre( 0.5 * ( refLo.x + refHi.x ), 0.5 * ( refLo.y + refHi.y ), 0.5 * ( refLo.z + refHi.z ) );
                            Scalar vol = 0;
                            for( const auto& triangle : ptr_polygons ) {
                                vol += Vector3Ops::Dot( Vector3Ops::mkVector3( *triangle.pVertices[0], centre ),
                                    Vector3Ops::Cross( Vector3Ops::mkVector3( *triangle.pVertices[1], centre ),
                                        Vector3Ops::mkVector3( *triangle.pVertices[2], centre ) ) );
                            }
                            m_bOpenSignedVolumeNegative = ( vol < Scalar( 0 ) );
                        }
                    }
                }
            }

            if( !reusedOpenRefusal && !reusedOpenCertified ) {
                ++openCertificateAudits;
                bool topologyVerifiedForCurrentWeld =
                    pointWeldMatchesCorners && m_cachedOpenTopologyPassed &&
                    !m_cachedOpenTopologyWeldedId.empty() &&
                    m_lastWeldedId == m_cachedOpenTopologyWeldedId;
                m_cachedOpenTopologyWeldedId.clear();
                m_cachedTopologicalOpenRefusal = false;
                m_cachedOpenTopologyPassed = false;
                m_certifiedOpenExactSeams = false;
                m_certifiedOpenSeparation = Scalar( 0 );
                m_certifiedOpenPositions.clear();
                m_certifiedOpenSeamPairs.clear();
                openRefusalTris[0] = openRefusalTris[1] = size_t( -1 );

                std::vector<Point3> corners;
                corners.reserve( ptr_polygons.size() * 3 );
                std::vector<unsigned int> cornerWeldedId;
                if( pointWeldMatchesCorners ) {
                    cornerWeldedId.reserve( ptr_polygons.size() * 3 );
                }
                for( size_t t = 0; t < ptr_polygons.size(); ++t ) {
                    for( int k = 0; k < 3; ++k ) {
                        corners.push_back( *ptr_polygons[t].pVertices[k] );
                        if( pointWeldMatchesCorners ) {
                            cornerWeldedId.push_back(
                                m_lastWeldedId[static_cast<std::size_t>( ptr_polygons[t].pVertices[k] - pBase )] );
                        }
                    }
                }

                // Single-pass projected graph proof: if all triangles have a strict
                // consistent 2D normal sign along `axis`, pPoints is a single-valued
                // height function over (u,v), every 3D triangle has altitude > eps3D,
                // and the 2D projection certifies at tolerance >= eps3D with exact
                // projected weld aliases, then by contraction of orthogonal projection
                // the 3D surface is provably embedded too -- certifying both the 3D
                // surface and the reusable projection cache in one pass.
                const auto coordinate=[](const Point3& p,int k) {return k==0?p.x:k==1?p.y:p.z;};
                const auto scalarBits=[](Scalar v)->std::uint64_t {
                    if(v==Scalar(0)) v=Scalar(0);
                    std::uint64_t b=0;
                    std::memcpy(&b,&v,sizeof(Scalar)<sizeof(std::uint64_t)?sizeof(Scalar):sizeof(std::uint64_t));
                    return b;
                };
                if( std::isfinite( eps3D ) && eps3D > Scalar( 0 ) &&
                    ( axisSign[0] == 1 || axisSign[0] == -1 ||
                      axisSign[1] == 1 || axisSign[1] == -1 ||
                      axisSign[2] == 1 || axisSign[2] == -1 ) )
                {
                    bool altitudesValid3D = true;
                    for( std::size_t t = 0; t < ptr_polygons.size(); ++t ) {
                        const Point3* P = &corners[3 * t];
                        const Scalar twiceArea = Scalar( 2 ) * areas[t];
                        Scalar longest = 0;
                        for( int k = 0; k < 3; ++k ) {
                            longest = std::max( longest, Vector3Ops::Magnitude( Vector3Ops::mkVector3( P[(k + 1) % 3], P[k] ) ) );
                        }
                        if( !( twiceArea > eps3D * longest ) ) {
                            altitudesValid3D = false;
                            break;
                        }
                    }
                    if( altitudesValid3D ) {
                        for( int axis = 0; axis < 3 && openGraphAxis < 0; ++axis ) {
                            if( axisSign[axis] != 1 && axisSign[axis] != -1 ) continue;
                            struct SeenSlot {
                                Scalar u = 0;
                                Scalar v = 0;
                                size_t idx = size_t(-1);
                            };
                            size_t seenCap = 16;
                            while( seenCap < m_referencedVertexCount * 2 ) seenCap <<= 1;
                            const size_t seenMask = seenCap - 1;
                            std::vector<SeenSlot> seen( seenCap );
                            std::vector<std::pair<size_t,size_t>> coincident;
                            bool graph = true;
                            for( size_t i = 0; i < pPoints.size(); ++i ) {
                                if( hasUnreferenced && !m_vertexReferenced[i] ) continue;
                                const auto& point = pPoints[i];
                                if( !std::isfinite( point.x ) || !std::isfinite( point.y ) || !std::isfinite( point.z ) ) { graph = false; break; }
                                const Scalar u = coordinate( point, (axis + 1) % 3 );
                                const Scalar v = coordinate( point, (axis + 2) % 3 );
                                std::uint64_t h = ( scalarBits( u ) * 0x9e3779b97f4a7c15ULL ) ^ ( scalarBits( v ) * 0xbf58476d1ce4e5b9ULL );
                                h ^= h >> 33;
                                size_t slot = static_cast<size_t>( h ) & seenMask;
                                while( seen[slot].idx != size_t(-1) && !( seen[slot].u == u && seen[slot].v == v ) ) {
                                    slot = ( slot + 1 ) & seenMask;
                                }
                                if( seen[slot].idx == size_t(-1) ) {
                                    seen[slot].u = u;
                                    seen[slot].v = v;
                                    seen[slot].idx = i;
                                } else {
                                    const size_t other = seen[slot].idx;
                                    if( coordinate( point, axis ) != coordinate( pPoints[other], axis ) ) { graph = false; break; }
                                    coincident.emplace_back( other, i );
                                }
                            }
                            if( !graph ) continue;

                            auto projected = corners;
                            for( auto& point : projected ) {
                                if( axis == 0 ) point.x = 0; else if( axis == 1 ) point.y = 0; else point.z = 0;
                            }
                            std::vector<unsigned int> welded;
                            const std::vector<unsigned int>* pPreWeld = pointWeldMatchesCorners ? &cornerWeldedId : nullptr;
                            const auto projCert = GeometricUtilities::IsEmbeddedOpenOrientedSurface(
                                projected, &welded, eps3D, pPreWeld, eps3D, topologyVerifiedForCurrentWeld );
                            if( pointWeldMatchesCorners ) {
                                if( projCert.topologicalRefusal ) {
                                    m_cachedOpenTopologyWeldedId = m_lastWeldedId;
                                    m_cachedTopologicalOpenRefusal = true;
                                    break;
                                }
                                const bool projReachedStep4 = projCert.certified || projCert.budgetRefusal ||
                                    ( projCert.refusalTriA < ptr_polygons.size() && projCert.refusalTriB < ptr_polygons.size() &&
                                      projCert.refusalTriA != projCert.refusalTriB );
                                if( projReachedStep4 ) {
                                    topologyVerifiedForCurrentWeld = true;
                                }
                            }
                            if( !projCert.certified ) {
                                continue;
                            }
                            const std::vector<unsigned int>& checkWeld = pPreWeld ? *pPreWeld : welded;
                            std::vector<size_t> first( projected.size(), projected.size() );
                            bool exactProjection = true;
                            for( size_t i = 0; i < projected.size(); ++i ) {
                                size_t& representative = first[checkWeld[i]];
                                if( representative == projected.size() ) representative = i;
                                else {
                                    const auto& a = projected[representative]; const auto& b = projected[i];
                                    if( a.x != b.x || a.y != b.y || a.z != b.z ) { exactProjection = false; break; }
                                }
                            }
                            if( !exactProjection ) continue;

                            openGraphAxis = axis;
                            openGraphCoincidentPairs = std::move( coincident );
                            m_bShapeOpenSheet = true;
                            if( pointWeldMatchesCorners ) {
                                m_cachedOpenTopologyWeldedId = m_lastWeldedId;
                                m_cachedOpenTopologyPassed = true;
                                m_watertightTopologyWeldedId = m_lastWeldedId;
                                m_cachedTopologicalWatertightRefusal = true;
                            }
                            const Point3 centre( 0.5 * ( refLo.x + refHi.x ), 0.5 * ( refLo.y + refHi.y ), 0.5 * ( refLo.z + refHi.z ) );
                            Scalar vol = 0;
                            for( std::size_t t = 0; t < ptr_polygons.size(); ++t ) {
                                const Point3* P = &corners[3 * t];
                                vol += Vector3Ops::Dot( Vector3Ops::mkVector3( P[0], centre ),
                                    Vector3Ops::Cross( Vector3Ops::mkVector3( P[1], centre ), Vector3Ops::mkVector3( P[2], centre ) ) );
                            }
                            m_bOpenSignedVolumeNegative = ( vol < Scalar( 0 ) );
                        }
                    }
                }

                if( !m_bShapeOpenSheet && !m_cachedTopologicalOpenRefusal ) {
                    const auto cert = GeometricUtilities::IsEmbeddedOpenOrientedSurface(
                        corners, nullptr, Scalar( 0 ), pointWeldMatchesCorners ? &cornerWeldedId : nullptr, eps3D,
                        topologyVerifiedForCurrentWeld );
                    m_bShapeOpenSheet = cert.certified;
                    m_bOpenSignedVolumeNegative = cert.certified && cert.signedVolumeAboutCentre < 0;
                    if( cert.certified ) {
                        if( pointWeldMatchesCorners ) {
                            m_cachedOpenTopologyWeldedId = m_lastWeldedId;
                            m_cachedTopologicalOpenRefusal = false;
                            m_cachedOpenTopologyPassed = true;
                            m_watertightTopologyWeldedId = m_lastWeldedId;
                            m_cachedTopologicalWatertightRefusal = true;
                            m_certifiedOpenSeamPairs.clear();
                            m_certifiedOpenExactSeams = true;
                            std::vector<std::size_t> firstRep( m_referencedVertexCount, std::size_t( -1 ) );
                            for( std::size_t i = 0; i < pPoints.size(); ++i ) {
                                if( hasUnreferenced && !m_vertexReferenced[i] ) continue;
                                const unsigned int w = m_lastWeldedId[i];
                                if( w >= firstRep.size() ) {
                                    m_certifiedOpenExactSeams = false;
                                    break;
                                }
                                if( firstRep[w] == std::size_t( -1 ) ) {
                                    firstRep[w] = i;
                                } else {
                                    const Point3& a = pPoints[firstRep[w]];
                                    const Point3& b = pPoints[i];
                                    if( a.x != b.x || a.y != b.y || a.z != b.z ) {
                                        m_certifiedOpenExactSeams = false;
                                        m_certifiedOpenSeamPairs.clear();
                                        break;
                                    }
                                    m_certifiedOpenSeamPairs.emplace_back( firstRep[w], i );
                                }
                            }
                            if( m_certifiedOpenExactSeams && cert.separationLowerBound > Scalar( 0 ) ) {
                                m_certifiedOpenPositions = pPoints;
                                m_certifiedOpenSeparation = cert.separationLowerBound;
                            }
                        }
                    } else {
                        for( const auto& triangle : ptr_polygons ) {
                            for( int k = 0; k < 3; ++k ) {
                                const Point3& a = *triangle.pVertices[k];
                                const Point3& b = *triangle.pVertices[(k + 1) % 3];
                                if( a.x == b.x && a.y == b.y && a.z == b.z ) {
                                    openRefusalPair[0] = triangle.pVertices[k] - pPoints.data();
                                    openRefusalPair[1] = triangle.pVertices[(k + 1) % 3] - pPoints.data();
                                    break;
                                }
                            }
                            if( openRefusalPair[0] < pPoints.size() ) break;
                        }
                        if( pointWeldMatchesCorners ) {
                            m_cachedOpenTopologyWeldedId = m_lastWeldedId;
                            m_cachedTopologicalOpenRefusal = cert.topologicalRefusal;
                            const bool reachedStep4 = cert.budgetRefusal ||
                                ( cert.refusalTriA < ptr_polygons.size() && cert.refusalTriB < ptr_polygons.size() &&
                                  cert.refusalTriA != cert.refusalTriB );
                            m_cachedOpenTopologyPassed = !cert.topologicalRefusal && ( topologyVerifiedForCurrentWeld || reachedStep4 );
                            if( m_cachedOpenTopologyPassed ) {
                                m_watertightTopologyWeldedId = m_lastWeldedId;
                                m_cachedTopologicalWatertightRefusal = true;
                            }
                            openRefusalTris[0] = cert.refusalTriA;
                            openRefusalTris[1] = cert.refusalTriB;
                        }
                    }
                }
            }
        } else {
            ++openCertificateAudits;
        }
    }
    m_bProvablyOpenSheet = m_bShapeOpenSheet && !smsUncertainNormalOrientation;
}

void TriangleMeshGeometryIndexed::DoneIndexedTriangles( )
{
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed:: Constructing acceleration structures for %u triangles", indexedtris.size() );
	{
		// Optimize the point, normal and coord lists
		// so that they only use the amount of memory they require
		stl_utils::container_optimize< VerticesListType >( pPoints );
		if( bUseFaceNormals ) {
			stl_utils::container_erase_all< NormalsListType >( pNormals );
		} else {
			stl_utils::container_optimize< NormalsListType >( pNormals );
		}
		stl_utils::container_optimize< TexCoordsListType >( pCoords );
	}

	// Convert that indexed triangle into a pointer triangle
	InvalidateTopologyProofCaches();
	{
		IndexTriangleListType::const_iterator	i, e;
		
		for( i=indexedtris.begin(), e=indexedtris.end(); i!=e; i++ )
		{
			const IndexedTriangle& tri = *i;

			PointerTriangle		myTri;

			for( int i=0; i<3; i++ )
			{
#ifdef _DEBUG
				// Check integrity
				if( tri.iVertices[i] >= pPoints.size() ||
					tri.iNormals[i] >= pNormals.size() || 
					tri.iCoords[i] >= pCoords.size()
					)
				{
					GlobalLog()->PrintEasyError( "Bad index on IndexedTriangle, bad things will happen" );
					__debugbreak();
				}
#endif

				myTri.pVertices[i] = &pPoints[tri.iVertices[i]];
				if( !bUseFaceNormals ) {
					myTri.pNormals[i] = &pNormals[tri.iNormals[i]];
				} else {
					myTri.pNormals[i] = 0;
				}
				myTri.pCoords[i] = &pCoords[tri.iCoords[i]];
			}

			ptr_polygons.push_back( myTri );
		}

		// Blow away the index triangle
		indexedtris.clear();
		stl_utils::container_erase_all< IndexTriangleListType >( indexedtris );

	}

	// We're done with all the triangles so stuff it all into an octree
	// First compute the bounds of the octree
	BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );

	std::vector<const PointerTriangle*>	temp;

	{
		MyPointerTriangleList::iterator i, e;
		for( i=ptr_polygons.begin(), e=ptr_polygons.end(); i!=e; i++ ) {
			PointerTriangle&	mytri = (*i);
			temp.push_back( &mytri );
		}

		MyPointsList::const_iterator		m, n;
		for( m=pPoints.begin(), n=pPoints.end(); m!=n; m++ ) {
			bbox.Include( *m );
		}
	}

	// Phase 1: BVH replaces BSP/octree as the active acceleration structure.
	// Build SAH-binned BVH2 over the pointer-triangle list.
	//
	// Env-var escape hatch for adversarial review and A/B regression
	// Cleanup §3+§4: BVH-only.  Legacy BSP/octree fallback removed
	// (members preserved for v1/v2 .risemesh deserialize compat).
	// SBVH off by default per Tier 1 §1 (regression on big meshes).
	safe_release( pPtrBVH );

	AccelerationConfig cfg;
	cfg.maxLeafSize            = 4;
	cfg.binCount               = 32;
	cfg.sahTraversalCost       = 1.0;
	cfg.sahIntersectionCost    = 1.0;
	cfg.doubleSided            = bDoubleSided;

	pPtrBVH = new BVH<const PointerTriangle*>( *this, temp, bbox, cfg );
	GlobalLog()->PrintNew( pPtrBVH, __FILE__, __LINE__, "pointers BVH" );

	ComputeAreas();

	// DL-31.  Topology is final now (ptr_polygons is built, indexedtris
	// -- which is what carried the index triples -- was just cleared
	// above), so perform the initial watertightness proof from live state.
	// Changed positions refresh it in UpdateVertices.
	ComputeWatertightness();
	GeometricUtilities::BumpPatchCertificateEpoch();

	// Vertex arrays and topology are final only now (the block above
	// re-points every PointerTriangle into pPoints/pNormals), so anything
	// baked before this call describes storage that no longer exists.
	InvalidateSignalBakes();
}

int TriangleMeshGeometryIndexed::TopologicalOpenness() const
{
	const int cached = m_topologicalOpenness.load( std::memory_order_acquire );
	if( cached >= -1 ) {
		return cached;
	}
	// One build, however many threads ask at once (see
	// TriangleMeshGeometry::TopologicalOpenness).
	static std::mutex s_buildMutex;
	std::lock_guard<std::mutex> lock( s_buildMutex );
	const int recheck = m_topologicalOpenness.load( std::memory_order_acquire );
	if( recheck >= -1 ) {
		return recheck;
	}
	std::vector<Point3> corners;
	corners.reserve( ptr_polygons.size() * 3 );
	for( std::size_t t = 0; t < ptr_polygons.size(); ++t ) {
		for( int k = 0; k < 3; ++k ) {
			if( !ptr_polygons[t].pVertices[k] ) {
				m_topologicalOpenness.store( -1, std::memory_order_release );
				return -1;
			}
			corners.push_back( *ptr_polygons[t].pVertices[k] );
		}
	}
	const int open = GeometricUtilities::TriangleMeshOpenness( corners );
	m_topologicalOpenness.store( open, std::memory_order_release );
	return open;
}

//! DL-31/DL-143.  Every edge of `ptr_polygons`, keyed by its two
//! ENDPOINT WELDED-VERTEX ids (undirected -- (i,j) and (j,i) are the
//! same edge), must occur in exactly two triangles for the mesh to be a
//! closed 2-manifold with no boundary.
//!
//! DL-143: this USED to key edges by the raw POSITION-INDEX pair
//! (`PointerTriangle::pVertices[k]`'s offset into `pPoints`), which is
//! only the same thing as "the same vertex" when nothing in the
//! authoring/import/tessellation pipeline ever emits two distinct
//! position-array slots for the same physical corner.  Almost nothing
//! upstream honours that assumption: `GLTFSceneImporter::
//! BuildGeometryFromPrimitive` does one `AddVertex` per glTF vertex
//! (glTF's own attribute-tuple dedup granularity, which still splits a
//! shared corner wherever its normal or UV differs across faces -- any
//! hard edge or UV seam), and every `TessellateToMesh` producer in this
//! engine (`SphereGeometry`/`BoxGeometry`'s generative tessellators give
//! each face/pole its own vertex grid with no cross-seam sharing; this
//! class's own `TessellateToMesh` re-emits one vertex per distinct
//! (position, normal, uv) index triple, so it preserves whatever sharing
//! the source mesh had but still splits a position wherever its normal
//! or UV index differs -- and flattens a face-normal mesh per corner)
//! does the same.  Keyed by raw index, a mesh built by any of those paths
//! reads as an OPEN SHEET regardless of whether it is actually closed --
//! a hand-welded mesh (a position shared by every triangle that touches
//! it, one array slot per physical corner) was the only shape that ever
//! passed.  Fixed by welding corners to a shared id by POSITION,
//! `WeldVertexPositions` below, before building the edge map -- the
//! textbook fix, and the one every other closed-mesh consumer in this
//! codebase (`GeometryUtilities`, etc.) already expects of its inputs.
//! `ptr_polygons`'s pointers, not `indexedtris`'s indices, are still the
//! source of positions, because `DoneIndexedTriangles` has already freed
//! `indexedtris` by the time this runs (see its own comment).
//!
//! A CHEAP NECESSARY CONDITION, not a full manifold/orientability
//! certificate: two triangles sharing an edge with the SAME winding
//! (a non-orientable identification) would still pass this count. That
//! residual is recorded rather than chased further here -- the parity
//! ray test below only needs "no boundary" (every edge internal) to be
//! well-defined, and a non-orientable closed surface is not constructible
//! from consistently-wound authored triangles in the first place.
//!
//! DL-150.  `WeldVertexPositions`'s epsilon is RELATIVE to this mesh's own
//! bounding-box diagonal (`eps = max(1e-9, 1e-6*diag)` -- see that
//! function's own comment), which is exactly what makes it absorb a
//! seam's float noise on a small asset AND on a kilometre-scale one alike.
//! The same relativity means `eps` can exceed the ACTUAL PHYSICAL GAP
//! between two genuinely distinct open sheets whenever anything else in
//! the mesh (a remote detail, a stray far-away vertex, a large enclosing
//! shell) inflates the bounding box: two independently-triangulated quads
//! 0.001 units apart, sharing a scene with one remote vertex ~1000 units
//! away (bbox diagonal ~1732, eps ~1.7e-3 > 0.001), weld into ONE false
//! 2-manifold whose "interior" is the sliver between them --
//! `SignedDistanceLower` then answers a confident WRONG signed depth
//! (`outExact=true`) for a point in that gap, because the edge-count
//! check above cannot tell "one seam, stitched twice" from "two unrelated
//! sheets, glued by coincidence."
//!
//! **Round 2 correction (2026-09-18, review P1):** the FIRST discriminator
//! here compared accumulated per-corner ORIENTATION across a weld group
//! (refusing on a cosine below -0.5, ~120 degrees) on the theory that two
//! facing sheets are close to anti-parallel while a legitimate seam's
//! corners are not.  That theory is FALSE for a genuinely closed solid
//! with a sharp CONVEX crease: a symmetric wedge/prism with apex angle
//! `theta` has its two side faces' outward normals at cosine `-cos(theta)`
//! -- identical, in sign and rough magnitude, to the false-stitch
//! signature it was meant to catch.  A real, closed, correctly-wound
//! triangular-prism wedge at apex 30/50/58 degrees (cosine -0.866/-0.643/
//! -0.530) was FALSELY REFUSED by that threshold; only apex >= ~60 degrees
//! (cosine > -0.5) passed.  As theta -> 0 (a knife edge) the true cosine
//! approaches -1, so NO fixed orientation threshold can separate a sharp
//! legitimate crease from a genuinely opposed false stitch -- orientation
//! alone is not a valid signal here, however plausible-looking a single
//! flat-shaded-cube control (whose real corners sit at cosine 0) made it
//! look.  (That control was itself a red herring: `BuildMesh()`'s helper
//! stamps a `(0,0,1)` PLACEHOLDER normal on every vertex regardless of
//! which face it belongs to, so the removed threshold's own regression
//! test never exercised a real per-face crease at all.)
//!
//! The discriminator below instead targets the actual GEOMETRIC SIGNATURE
//! of a false stitch, independent of orientation: after the position
//! weld, two independently-triangulated but adjoining sheets closer than
//! `eps` produce two DISTINCT triangles (by identity/authoring origin)
//! that resolve to the SAME three post-weld vertex ids -- an UNORDERED
//! triple match for an opposite-winding pair (the reviewer's own repro),
//! an exact ORDERED match too when the windings also happen to agree (the
//! prior "same-facing" residual, below).  A legitimate closed mesh, at any
//! dihedral angle from a hairline crease to a near-flat seam, never has
//! two topologically distinct triangles occupying the same three
//! vertices -- each face of a real solid is a geometrically distinct
//! patch of surface, however small the angle to its neighbour.  Detected
//! by sorting each triangle's post-weld vertex-id triple and scanning for
//! adjacent duplicates (`O(T log T)`, `T` = triangle count) -- no
//! per-vertex normal or face-normal computation needed at all, which also
//! makes this CHEAPER than the removed orientation pass (see the ledger
//! row's own commit message for the re-measured numbers).
//!
//! DL-197 CLOSED (2026-09-18).  The residual named directly above --
//! two sheets closer than `eps` that are NOT triangle-coincident after
//! the weld, e.g. two independently-tessellated quads split on
//! DIFFERENT internal diagonals -- is now caught by a SECOND,
//! edge-based discriminator that runs after the coincident-triangle one
//! above.  At every edge (already known, by the two checks above having
//! passed, to be shared by exactly two triangles with distinct vertex
//! triples), test whether those two triangles are GEOMETRICALLY
//! coplanar and OPPOSITELY WOUND: the real physical signature of "two
//! flat sheets folded against each other", independent of how their
//! diagonals happen to be split.
//!
//! Coplanarity is measured by point-to-plane distance (each triangle's
//! own RAW, pre-weld positions and geometric normal), NOT by the
//! normal-dot-product alone -- a genuinely closed wedge/prism can have
//! an arbitrarily sharp convex crease (see the coincident-triangle
//! discriminator's own "Round 2 correction" above, which hit the
//! identical trap from the orientation side: as its apex angle shrinks
//! toward zero, its two side faces' outward normals really do approach
//! exactly antiparallel), but at any FINITE apex angle its third
//! vertices sit at a distance from the opposite face's plane that
//! scales with the wedge's own physical size, not with the mesh's weld
//! tolerance.  A false stitch, by contrast, is built from points that
//! are already within `eps` of each other (that is what let them weld
//! into one mesh in the first place), so its third-vertex-to-plane
//! distance is bounded by `eps` regardless of the two sheets' in-plane
//! geometry.  Using the SAME `eps` the weld itself used (threaded
//! through via `WeldVertexPositions`'s `outEps`) keeps this test
//! relative to the mesh's own scale, exactly like the weld -- no
//! separate magic threshold.  Once coplanarity is established this way,
//! the winding question reduces to a sign: two triangles occupying (to
//! within `eps`) the same plane have geometric normals that are the
//! plane's own normal up to sign, so "oppositely facing" is simply
//! dot(n1,n2) < 0 -- checked FIRST, as a cheap pre-filter, since it is
//! necessary (though not sufficient on its own -- the wedge case above
//! also has dot < 0) before the more expensive plane-distance test
//! runs.
//!
//! A genuinely zero-thickness double-sided "fin" (two coincident flat
//! layers, meant to shade from both sides) is, by this construction,
//! INDISTINGUISHABLE from a false stitch whenever the two layers are
//! independently triangulated (different diagonals) -- both are
//! literally the same coplanar-opposite-winding configuration, and this
//! check REFUSES both, on the same reasoning DL-150's own coincident-
//! triangle case already applies to a same-diagonal fin (caught
//! earlier, above): a zero-thickness "solid" encloses no volume, so
//! `interior()` has no physically meaningful signed depth to report for
//! it either way, and a diagonal-only difference between the two layers
//! is not a distinction with a physical consequence for that judgement.
//!
//! Two residuals still open after this fix (recorded, not chased
//! further here, self-review 2026-09-18):
//!
//! (1) Two sheets closer than `eps` that are NEITHER triangle-
//! coincident NOR edge-adjacent at all -- e.g. two overlapping but
//! non-conforming tessellations whose triangle boundaries don't touch
//! along any shared edge, only through interior crossings -- have no
//! edge for this check (or the coincident-triangle one) to examine in
//! the first place.  A stronger guarantee would need an independent
//! geometric test (self-intersection / distinct connected-component
//! volume enclosure), out of scope for this position-weld check.
//!
//! (2) Two independently-tessellated, coincident sheets that happen to
//! face the SAME way (same winding, so `normalDot >= 0` at every shared
//! edge) are NOT caught by the coplanar/oppositely-wound discriminator
//! above, and this is NOT merely an unimplemented case -- it is not
//! locally decidable from edge information at all.  A coplanar,
//! same-winding pair of triangles sharing an edge is EXACTLY the local
//! signature of an ordinary, legitimate flat mesh region (e.g. two
//! adjacent quads of one tessellated `BoxGeometry` face, or a single
//! quad's own internal diagonal split): using "coplanar + same winding"
//! as a refusal signal would misfire on essentially every flat-shaded
//! surface in the corpus.  Distinguishing "one continuous authored
//! surface, incidentally flat here" from "two independently-authored,
//! coincident, same-facing sheets" needs information this per-edge
//! check does not have (e.g. connected-component provenance), so this
//! case is a genuine, structural blind spot, not a residual that a
//! sharper local rule could close.
void TriangleMeshGeometryIndexed::ComputeWatertightness()
{
	m_bWatertight = false;
	if( ptr_polygons.empty() || pPoints.empty() ) {
		return;
	}
	// DL-620 review P2-1.  "Provably open" and "watertight" are mutually
	// exclusive claims about one surface.  They were once decided at two
	// tolerances (the open certificate welded the USED corners, this proof
	// ALL of pPoints, unused storage included), so a crack wider than the
	// first eps but narrower than the second -- one far unused vertex
	// sufficed -- certified BOTH.  Since DL-623 both proofs weld only the
	// referenced vertices (WeldReferencedVertices, one shared partition
	// and eps), but this guard stays: the open certificate has proved a
	// real boundary, UpdateVertices skips this proof for an open sheet,
	// and a signed parity test on a cracked surface can be wrong for rays
	// through the crack while refusing interior() only falls back to the
	// honest unsigned distance.  Every caller (build, deformation,
	// deserialization) reaches this check after ComputeAreas, so all three
	// agree by construction.
	if( m_bShapeOpenSheet ) {
		return;
	}

	if( !m_lastWeldValid || m_lastWeldedId.size() != pPoints.size() ) {
		WeldReferencedVertices();
	}
	if( !m_lastWeldValid ) {
		return;
	}
	const std::vector<unsigned int>& weldedId = m_lastWeldedId;
	const Scalar weldEps = m_lastWeldEps;
	const std::size_t nTris = ptr_polygons.size();
	if( m_faceNormals.size() != nTris ) {
		m_faceNormals.resize( nTris );
		for( std::size_t t = 0; t < nTris; ++t ) {
			const PointerTriangle& tri = ptr_polygons[t];
			m_faceNormals[t] = Vector3Ops::Normalize( Vector3Ops::Cross(
				Vector3Ops::mkVector3( *tri.pVertices[1], *tri.pVertices[0] ),
				Vector3Ops::mkVector3( *tri.pVertices[2], *tri.pVertices[0] ) ) );
		}
	}

	// DL-150.  Report the weld's own vertex-count reduction so an author
	// can see an over-aggressive weld even when the discriminator below
	// does not (or cannot) catch it. Certification-time, never per query,
	// like the diagnostics below.  Printed before the cached-topology
	// shortcut so every proof -- full or reused -- reports it (DL-620
	// review round 2, P3-d).
	{
		unsigned int weldedCount = 0;
		for( std::size_t i = 0; i < weldedId.size(); ++i ) {
			if( weldedId[i] != 0xFFFFFFFFu && weldedId[i] + 1 > weldedCount ) {
				weldedCount = weldedId[i] + 1;
			}
		}
		GlobalLog()->PrintEx( eLog_Info,
			"TriangleMeshGeometryIndexed:: welded %zu raw vertices to %u positions "
			"(%.1f%% reduction) before the watertightness edge count; see "
			"docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 10 (DL-143/DL-150).",
			m_referencedVertexCount, weldedCount,
			m_referencedVertexCount == 0 ? 0.0 : 100.0 * ( 1.0 - (double)weldedCount / (double)m_referencedVertexCount ) );
	}

	if( m_lastWeldTransitive && !m_watertightTopologyWeldedId.empty()
		&& weldedId == m_watertightTopologyWeldedId )
	{
		if( m_cachedTopologicalWatertightRefusal ) {
			// The refusal diagnostic printed when this partition was first
			// proved still applies; say so rather than repeat its counts.
			GlobalLog()->PrintEx( eLog_Info,
				"TriangleMeshGeometryIndexed:: weld partition unchanged since the last "
				"watertightness proof, which refused it -- interior() stays unsigned "
				"(DL-31/DL-150)." );
			return;
		}
		if( !m_watertightEdges.empty() ) {
			bool coplanarOpposedFound = false;
			std::array<unsigned int, 3> worstEdgeTriple = { 0, 0, 0 };
			for( const WatertightEdgeWitness& e : m_watertightEdges ) {
				const Vector3& n1 = m_faceNormals[e.t1];
				const Vector3& n2 = m_faceNormals[e.t2];
				if( !( Vector3Ops::Dot( n1, n2 ) < Scalar( 0 ) ) ) {
					continue;
				}
				const PointerTriangle& tri1 = ptr_polygons[e.t1];
				const PointerTriangle& tri2 = ptr_polygons[e.t2];
				const Point3& third1 = *tri1.pVertices[e.opp1];
				const Point3& third2 = *tri2.pVertices[e.opp2];
				const Scalar dist1 = Vector3Ops::Dot( Vector3Ops::mkVector3( third2, *tri1.pVertices[0] ), n1 );
				const Scalar dist2 = Vector3Ops::Dot( Vector3Ops::mkVector3( third1, *tri2.pVertices[0] ), n2 );
				if( std::fabs( dist1 ) <= weldEps && std::fabs( dist2 ) <= weldEps ) {
					coplanarOpposedFound = true;
					worstEdgeTriple = { e.edgeA, e.edgeB, e.thirdW1 };
					break;
				}
			}
			if( coplanarOpposedFound ) {
				GlobalLog()->PrintEx( eLog_Info,
					"TriangleMeshGeometryIndexed:: this mesh's position weld produced a pair of "
					"COPLANAR, OPPOSITELY WOUND triangles sharing an edge (e.g. edge {%u,%u}, third "
					"vertex %u) that are not literally coincident -- this looks like two "
					"independently tessellated sheets folded flat against each other (a false "
					"stitch, or a zero-thickness double layer), not a legitimate seam or crease, so "
					"it will answer proximity() (an honest unsigned distance) but REFUSE interior() "
					"(no certified inside test).  See docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 10 "
					"(DL-197).",
					worstEdgeTriple[0], worstEdgeTriple[1], worstEdgeTriple[2] );
				return;
			}
			m_bWatertight = true;
			return;
		}
	}

	m_watertightTopologyWeldedId.clear();
	m_cachedTopologicalWatertightRefusal = false;
	m_watertightEdges.clear();

	const Point3* const pBase = &pPoints[0];

	// DL-150/DL-197.  One entry per edge: a running COUNT (DL-31's own
	// closed-2-manifold check) plus the index of the first two triangles
	// seen at this edge (DL-197's coplanar/oppositely-wound discriminator
	// below).  A flat open-addressed table avoids per-edge heap node
	// allocations (~1.47M nodes on a 980k-triangle mesh).
	struct EdgeInfo
	{
		std::uint64_t key = ~std::uint64_t( 0 );
		unsigned int count = 0;
		unsigned int triIdx[2] = { 0, 0 };
	};
	// At most 3 nTris distinct edges, about 1.5 nTris on a closed mesh: a
	// capacity of at least 4 nTris keeps the worst-case load below 3/4
	// (DL-620 review P3-1: 6 nTris cost ~200 MB at 980k triangles).
	std::size_t edgeCap = 16;
	while( edgeCap < nTris * 4 ) {
		edgeCap <<= 1;
	}
	const std::size_t edgeMask = edgeCap - 1;
	std::vector<EdgeInfo> edgeTable( edgeCap );
	std::vector<unsigned int> edgeSlots;
	edgeSlots.reserve( nTris * 2 );

	for( std::size_t t = 0; t < nTris; ++t ) {
		const PointerTriangle& tri = ptr_polygons[t];
		unsigned int idx[3];
		for( int k = 0; k < 3; ++k ) {
			const std::ptrdiff_t off = tri.pVertices[k] - pBase;
			if( off < 0 || (std::size_t)off >= pPoints.size() ) {
				// A pointer outside pPoints means broken state -- refuse
				// rather than guess.  No diagnostic: this is an internal
				// consistency failure, not an authoring one.
				return;
			}
			// DL-143: the WELDED id, not the raw position-array offset --
			// see the function's own comment for why these differ on
			// almost every imported/tessellated mesh.
			idx[k] = weldedId[(std::size_t)off];
		}

		bool degenerate = false;
		for( int k = 0; k < 3; ++k ) {
			unsigned int a = idx[k];
			unsigned int b = idx[(k + 1) % 3];
			if( a == b ) { degenerate = true; continue; }
			if( a > b ) { std::swap( a, b ); }
			const std::uint64_t key = ( (std::uint64_t)a << 32 ) | (std::uint64_t)b;
			std::size_t slot = static_cast<std::size_t>( ( key ^ ( key >> 33 ) ) * 0xff51afd7ed558ccdULL ) & edgeMask;
			while( edgeTable[slot].key != ~std::uint64_t( 0 ) && edgeTable[slot].key != key ) {
				slot = ( slot + 1 ) & edgeMask;
			}
			EdgeInfo& info = edgeTable[slot];
			if( info.key == ~std::uint64_t( 0 ) ) {
				info.key = key;
				edgeSlots.push_back( static_cast<unsigned int>( slot ) );
			}
			if( info.count < 2 ) { info.triIdx[info.count] = static_cast<unsigned int>( t ); }
			++info.count;
		}
		if( degenerate ) {
			if( m_lastWeldTransitive ) {
				m_watertightTopologyWeldedId = weldedId;
				m_cachedTopologicalWatertightRefusal = true;
			}
			GlobalLog()->PrintEx( eLog_Info,
				"TriangleMeshGeometryIndexed:: this mesh has a degenerate (repeated-vertex) "
				"triangle, so it cannot be certified watertight -- interior() will read its "
				"honest unsigned proximity() distance only, never a signed depth; see "
				"docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 10 (DL-31)." );
			return;
		}
	}

	// DL-31.  A closed 2-manifold with no boundary has EVERY edge shared
	// by exactly two triangles: 1 is a boundary (an open sheet, or a mesh
	// missing a face), 3+ is non-manifold (two or more surface sheets
	// stitched through one edge).  Either way this stays an unsigned-only
	// sheet -- see the header comment on `m_bWatertight` -- but unlike
	// `DeepestOtherContainment`'s own per-QUERY refusal (deliberately
	// silent there: a shared latch would print once per ray for every
	// mesh AND plane in the scene, see that function's comment), this is
	// a certification-time diagnostic on the mesh itself: it fires during
	// a build or shape reproof, not per interior() query, and tells an
	// author what "unsigned only" traces back to. Animated geometry may
	// repeat this information when its shape needs reproof.
	unsigned int boundaryEdges = 0, nonManifoldEdges = 0;
	for( std::size_t i = 0; i < edgeSlots.size(); ++i ) {
		const EdgeInfo& info = edgeTable[edgeSlots[i]];
		if( info.count == 1 ) { ++boundaryEdges; }
		else if( info.count > 2 ) { ++nonManifoldEdges; }
	}

	if( boundaryEdges > 0 || nonManifoldEdges > 0 ) {
		if( m_lastWeldTransitive ) {
			m_watertightTopologyWeldedId = weldedId;
			m_cachedTopologicalWatertightRefusal = true;
		}
		GlobalLog()->PrintEx( eLog_Info,
			"TriangleMeshGeometryIndexed:: this mesh is not watertight (%u boundary edge%s, "
			"%u non-manifold edge%s) -- it will answer proximity() (an honest unsigned "
			"distance) but REFUSE interior() (no certified inside test), unlike a closed "
			"mesh.  See docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 10 (DL-31).",
			boundaryEdges, boundaryEdges == 1 ? "" : "s",
			nonManifoldEdges, nonManifoldEdges == 1 ? "" : "s" );
		return;
	}

	// DL-150 / DL-197: Only reached when every edge has count == 2.
	// Since every edge is shared by exactly two triangles, two distinct
	// triangles share all 3 post-weld vertices iff at their shared edge
	// their third post-weld vertex is identical.  Check this in O(1) per
	// edge alongside the DL-197 coplanar/oppositely-wound discriminator;
	// only materialize and sort `sortedTriangleIds` when a coincident pair
	// is actually detected so the diagnostic reports the exact same count
	// and triple.
	bool hasCoincidentPair = false;
	bool coplanarOpposedFound = false;
	std::array<unsigned int, 3> worstEdgeTriple = { 0, 0, 0 };
	std::vector<WatertightEdgeWitness> candidateEdges;
	if( m_lastWeldTransitive ) {
		candidateEdges.reserve( edgeSlots.size() );
	}

	for( std::size_t i = 0; i < edgeSlots.size(); ++i ) {
		const EdgeInfo& info = edgeTable[edgeSlots[i]];
		if( info.count != 2 ) {
			continue;
		}
		const unsigned int t1 = info.triIdx[0];
		const unsigned int t2 = info.triIdx[1];
		const PointerTriangle& tri1 = ptr_polygons[t1];
		const PointerTriangle& tri2 = ptr_polygons[t2];
		const unsigned int w1[3] = {
			weldedId[static_cast<std::size_t>( tri1.pVertices[0] - pBase )],
			weldedId[static_cast<std::size_t>( tri1.pVertices[1] - pBase )],
			weldedId[static_cast<std::size_t>( tri1.pVertices[2] - pBase )]
		};
		const unsigned int w2[3] = {
			weldedId[static_cast<std::size_t>( tri2.pVertices[0] - pBase )],
			weldedId[static_cast<std::size_t>( tri2.pVertices[1] - pBase )],
			weldedId[static_cast<std::size_t>( tri2.pVertices[2] - pBase )]
		};
		const std::uint64_t key = info.key;
		const unsigned int edgeA = static_cast<unsigned int>( key >> 32 );
		const unsigned int edgeB = static_cast<unsigned int>( key & 0xFFFFFFFFu );
		const unsigned int thirdW1 = w1[0] + w1[1] + w1[2] - edgeA - edgeB;
		const unsigned int thirdW2 = w2[0] + w2[1] + w2[2] - edgeA - edgeB;
		if( thirdW1 == thirdW2 ) {
			hasCoincidentPair = true;
			break;
		}

		unsigned char opp1 = 3, opp2 = 3;
		for( unsigned char k = 0; k < 3; ++k ) {
			if( w1[k] != edgeA && w1[k] != edgeB ) { opp1 = k; break; }
		}
		for( unsigned char k = 0; k < 3; ++k ) {
			if( w2[k] != edgeA && w2[k] != edgeB ) { opp2 = k; break; }
		}
		if( opp1 >= 3 || opp2 >= 3 ) {
			continue;
		}
		if( m_lastWeldTransitive ) {
			WatertightEdgeWitness w;
			w.t1 = t1;
			w.t2 = t2;
			w.opp1 = opp1;
			w.opp2 = opp2;
			w.edgeA = edgeA;
			w.edgeB = edgeB;
			w.thirdW1 = thirdW1;
			candidateEdges.push_back( w );
		}

		if( coplanarOpposedFound ) {
			continue;
		}
		const Vector3& n1 = m_faceNormals[t1];
		const Vector3& n2 = m_faceNormals[t2];
		const Scalar normalDot = Vector3Ops::Dot( n1, n2 );
		if( !( normalDot < Scalar( 0 ) ) ) {
			continue;
		}
		const Point3& third1 = *tri1.pVertices[opp1];
		const Point3& third2 = *tri2.pVertices[opp2];
		const Scalar dist1 = Vector3Ops::Dot( Vector3Ops::mkVector3( third2, *tri1.pVertices[0] ), n1 );
		const Scalar dist2 = Vector3Ops::Dot( Vector3Ops::mkVector3( third1, *tri2.pVertices[0] ), n2 );
		if( std::fabs( dist1 ) <= weldEps && std::fabs( dist2 ) <= weldEps ) {
			coplanarOpposedFound = true;
			worstEdgeTriple = { edgeA, edgeB, thirdW1 };
		}
	}

	if( hasCoincidentPair ) {
		if( m_lastWeldTransitive ) {
			m_watertightTopologyWeldedId = weldedId;
			m_cachedTopologicalWatertightRefusal = true;
		}
		std::vector<std::array<unsigned int, 3> > sortedTriangleIds( nTris );
		for( std::size_t t = 0; t < nTris; ++t ) {
			const PointerTriangle& tri = ptr_polygons[t];
			std::array<unsigned int, 3> sorted = {
				weldedId[static_cast<std::size_t>( tri.pVertices[0] - pBase )],
				weldedId[static_cast<std::size_t>( tri.pVertices[1] - pBase )],
				weldedId[static_cast<std::size_t>( tri.pVertices[2] - pBase )]
			};
			std::sort( sorted.begin(), sorted.end() );
			sortedTriangleIds[t] = sorted;
		}
		std::sort( sortedTriangleIds.begin(), sortedTriangleIds.end() );
		unsigned int coincidentPairs = 0;
		std::array<unsigned int, 3> worstTriple = { 0, 0, 0 };
		for( std::size_t i = 1; i < sortedTriangleIds.size(); ++i ) {
			if( sortedTriangleIds[i] == sortedTriangleIds[i - 1] ) {
				if( coincidentPairs == 0 ) { worstTriple = sortedTriangleIds[i]; }
				++coincidentPairs;
			}
		}
		if( coincidentPairs > 0 ) {
			GlobalLog()->PrintEx( eLog_Info,
				"TriangleMeshGeometryIndexed:: this mesh's position weld produced %u pair%s of "
				"geometrically COINCIDENT triangles (two distinct triangles sharing the same 3 "
				"post-weld vertices, e.g. {%u,%u,%u}) -- this looks like two independent sheets "
				"stitched by coincidence, not a legitimate seam or crease, so it will answer "
				"proximity() (an honest unsigned distance) but REFUSE interior() (no certified "
				"inside test).  See docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 10 (DL-150).",
				coincidentPairs, coincidentPairs == 1 ? "" : "s",
				worstTriple[0], worstTriple[1], worstTriple[2] );
			return;
		}
	}

	if( m_lastWeldTransitive ) {
		m_watertightTopologyWeldedId = weldedId;
		m_cachedTopologicalWatertightRefusal = false;
		m_watertightEdges = std::move( candidateEdges );
	}

	if( coplanarOpposedFound ) {
		GlobalLog()->PrintEx( eLog_Info,
			"TriangleMeshGeometryIndexed:: this mesh's position weld produced a pair of "
			"COPLANAR, OPPOSITELY WOUND triangles sharing an edge (e.g. edge {%u,%u}, third "
			"vertex %u) that are not literally coincident -- this looks like two "
			"independently tessellated sheets folded flat against each other (a false "
			"stitch, or a zero-thickness double layer), not a legitimate seam or crease, so "
			"it will answer proximity() (an honest unsigned distance) but REFUSE interior() "
			"(no certified inside test).  See docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 10 "
			"(DL-197).",
			worstEdgeTriple[0], worstEdgeTriple[1], worstEdgeTriple[2] );
		return;
	}

	m_bWatertight = true;
}

//! See the header for the full contract (fixed generic direction,
//! both-faces geometric crossing count, only meaningful on a watertight
//! mesh).
bool TriangleMeshGeometryIndexed::RayParityInsideTest( const Point3& ptObject, bool& outInside ) const
{
	if( !pPtrBVH ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( (double)ptObject.x )
	 || !RISE::IsFiniteDouble( (double)ptObject.y )
	 || !RISE::IsFiniteDouble( (double)ptObject.z ) ) {
		return false;
	}

	const Scalar diagonal = SurfaceCurvature::ScaleHintFromBoundingBox( GenerateBoundingBox() );
	if( !RISE::IsFiniteDouble( (double)diagonal ) || !( diagonal > Scalar( 0 ) ) ) {
		return false;
	}
	const Scalar eps = MeshSignalBake::kOriginEpsilonFraction * diagonal;

	// A fixed, non-axis-aligned, non-"nice" direction: an authored mesh's
	// faces and edges are overwhelmingly axis-aligned or built from a few
	// round angles, so this makes an exact tangency (a ray running exactly
	// along a face, or exactly through an edge or vertex) the kind of
	// coincidence that does not happen from ordinary authoring, rather
	// than trying to detect and special-case one.
	const Vector3 dir = Vector3Ops::Normalize( Vector3( Scalar( 0.5257311121 ), Scalar( 0.8506508084 ), Scalar( 0.1234567891 ) ) );

	Point3 origin = ptObject;
	int crossings = 0;
	// A closed mesh's own ray can cross it at most once per triangle;
	// the guard exists only to bound a numerical runaway (e.g. an eps too
	// small to make progress on a degenerate mesh), never expected to
	// bind on a real one.
	const int kMaxCrossings = 1000000;
	for( int iter = 0; iter < kMaxCrossings; ++iter ) {
		RayIntersectionGeometric ri( Ray( origin, dir ), nullRasterizerState );
		ri.range = RISE_INFINITY;
		// BOTH faces: this is a geometric crossing count, not a shading
		// query, and must be unaffected by winding or `bDoubleSided` --
		// see the header comment.
		IntersectRay( ri, true, true, false );
		if( !ri.bHit || !RISE::IsFiniteDouble( (double)ri.range ) || !( ri.range >= Scalar( 0 ) ) ) {
			break;
		}
		++crossings;
		origin = Point3Ops::mkPoint3( origin, dir * ( ri.range + eps ) );
	}

	outInside = ( crossings % 2 ) == 1;
	return true;
}

//! See the header for the full contract (watertight-only, exact
// magnitude reused from DistanceToSurface, exact sign from the parity
// test, outExact honoured).
bool TriangleMeshGeometryIndexed::SignedDistanceLower( const Point3& ptObject, const Scalar /*maxDistObject*/,
	Scalar& outSigned, bool& outExact ) const
{
	outExact = false;
	if( !m_bWatertight ) {
		return false;		// DL-31 residual: an open or non-manifold mesh stays a sheet
	}

	// UNBOUNDED on purpose: IGeometry::SignedDistanceLower's contract
	// forbids a range refusal (`maxDistObject` is an effort hint only,
	// per the interface comment), while DistanceToSurface's OWN contract
	// permits one -- so the incoming budget is never forwarded to it.
	Scalar dist = Scalar( 0 );
	if( !DistanceToSurface( ptObject, RISE_INFINITY, dist ) ) {
		return false;
	}

	bool inside = false;
	if( !RayParityInsideTest( ptObject, inside ) ) {
		return false;
	}

	const Scalar sgn = inside ? -dist : dist;
	if( !RISE::IsFiniteDouble( (double)sgn ) ) {
		return false;
	}
	outSigned = sgn;
	outExact  = true;	// both the magnitude (DistanceToSurface is exact for this family) and the sign (a certified watertight mesh) are exact
	return true;
}

void TriangleMeshGeometryIndexed::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	Point3	ptMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3	ptMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) ;

	// Go through all the points and calculate the minimum and maximum values from the
	// entire set.
	MyPointsList::const_iterator m, n;
	for( m=pPoints.begin(), n=pPoints.end(); m!=n; m++ )
	{
		const Point3&	pt = *m;
		if( pt.x < ptMin.x ) ptMin.x = pt.x;
		if( pt.y < ptMin.y ) ptMin.y = pt.y;
		if( pt.z < ptMin.z ) ptMin.z = pt.z;
		if( pt.x > ptMax.x ) ptMax.x = pt.x;
		if( pt.y > ptMax.y ) ptMax.y = pt.y;
		if( pt.z > ptMax.z ) ptMax.z = pt.z;
	}

	// The center is the center of the minimum and maximum values of the points
	ptCenter = Point3Ops::WeightedAverage2( ptMin, ptMax, 0.5 );
	radius = 0;

	// Go through all the points again and calculate the radius of the sphere
	for( m=pPoints.begin(), n=pPoints.end(); m!=n; m++ ) {
		Vector3			r = Vector3Ops::mkVector3( *m, ptCenter );
		const Scalar	d = Vector3Ops::Magnitude(r);

		if( d > radius ) {
			radius = d;
		}
	}
}

BoundingBox TriangleMeshGeometryIndexed::GenerateBoundingBox( ) const
{
	// Cleanup §3+§4: BVH-only.
	if( pPtrBVH ) {
		return pPtrBVH->GetBBox();
	}
	return BoundingBox();
}

//! Exact point-to-SEGMENT distance -- the degenerate-triangle fallback and
//! nothing else, kept next to its only caller.
static Scalar PointSegmentDistance( const Point3& p, const Point3& a, const Point3& b )
{
	const Vector3 ab = Vector3Ops::mkVector3( b, a );
	const Vector3 ap = Vector3Ops::mkVector3( p, a );
	const Scalar  den = Vector3Ops::Dot( ab, ab );
	Scalar t = ( den > Scalar( 0 ) ) ? ( Vector3Ops::Dot( ap, ab ) / den ) : Scalar( 0 );
	if( t < Scalar( 0 ) ) t = Scalar( 0 );
	if( t > Scalar( 1 ) ) t = Scalar( 1 );
	const Vector3 d = Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( a, ab * t ) );
	return Vector3Ops::Magnitude( d );
}

//! See the header for the contract.  This is Ericson's region method
//! verbatim in structure: seven cheap sign tests carve the plane of the
//! triangle into three vertex regions, three edge regions and the interior,
//! and each region has a closed-form nearest point.
Scalar TriangleMeshGeometryIndexed::PointTriangleDistance(
	const Point3& p, const Point3& a, const Point3& b, const Point3& c )
{
	const Vector3 ab = Vector3Ops::mkVector3( b, a );
	const Vector3 ac = Vector3Ops::mkVector3( c, a );
	const Vector3 ap = Vector3Ops::mkVector3( p, a );

	const Scalar d1 = Vector3Ops::Dot( ab, ap );
	const Scalar d2 = Vector3Ops::Dot( ac, ap );
	// Vertex region A
	if( d1 <= Scalar( 0 ) && d2 <= Scalar( 0 ) ) {
		return Vector3Ops::Magnitude( ap );
	}

	const Vector3 bp = Vector3Ops::mkVector3( p, b );
	const Scalar d3 = Vector3Ops::Dot( ab, bp );
	const Scalar d4 = Vector3Ops::Dot( ac, bp );
	// Vertex region B
	if( d3 >= Scalar( 0 ) && d4 <= d3 ) {
		return Vector3Ops::Magnitude( bp );
	}

	const Scalar vc = d1 * d4 - d3 * d2;
	// Edge region AB
	if( vc <= Scalar( 0 ) && d1 >= Scalar( 0 ) && d3 <= Scalar( 0 ) ) {
		const Scalar den = d1 - d3;
		const Scalar v = ( den != Scalar( 0 ) ) ? ( d1 / den ) : Scalar( 0 );
		return Vector3Ops::Magnitude( Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( a, ab * v ) ) );
	}

	const Vector3 cp = Vector3Ops::mkVector3( p, c );
	const Scalar d5 = Vector3Ops::Dot( ab, cp );
	const Scalar d6 = Vector3Ops::Dot( ac, cp );
	// Vertex region C
	if( d6 >= Scalar( 0 ) && d5 <= d6 ) {
		return Vector3Ops::Magnitude( cp );
	}

	const Scalar vb = d5 * d2 - d1 * d6;
	// Edge region AC
	if( vb <= Scalar( 0 ) && d2 >= Scalar( 0 ) && d6 <= Scalar( 0 ) ) {
		const Scalar den = d2 - d6;
		const Scalar w = ( den != Scalar( 0 ) ) ? ( d2 / den ) : Scalar( 0 );
		return Vector3Ops::Magnitude( Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( a, ac * w ) ) );
	}

	const Scalar va = d3 * d6 - d5 * d4;
	// Edge region BC
	if( va <= Scalar( 0 ) && ( d4 - d3 ) >= Scalar( 0 ) && ( d5 - d6 ) >= Scalar( 0 ) ) {
		const Scalar den = ( d4 - d3 ) + ( d5 - d6 );
		const Scalar w = ( den != Scalar( 0 ) ) ? ( ( d4 - d3 ) / den ) : Scalar( 0 );
		const Vector3 bc = Vector3Ops::mkVector3( c, b );
		return Vector3Ops::Magnitude( Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( b, bc * w ) ) );
	}

	// INTERIOR -- but only if there IS an interior.  `va + vb + vc` is the
	// squared doubled area of the triangle (`4 * Area^2`, independent of `p`); it
	// vanishes exactly for a degenerate (zero-area) triangle, for which the
	// surface is the union of the three edges and the minimum over them is
	// the exact answer.  Reaching the divide with a vanishing denominator
	// would instead produce inf/NaN, and a NaN distance is the one value
	// this signal must never hand back (a NaN loses every comparison, so it
	// would be silently dropped by the traversal's `d < best` -- an
	// invisible hole in the mesh rather than a loud failure).
	const Scalar denom = va + vb + vc;
	if( !( denom > Scalar( 0 ) ) ) {
		const Scalar e0 = PointSegmentDistance( p, a, b );
		const Scalar e1 = PointSegmentDistance( p, b, c );
		const Scalar e2 = PointSegmentDistance( p, c, a );
		Scalar m = e0;
		if( e1 < m ) m = e1;
		if( e2 < m ) m = e2;
		return m;
	}

	// THE NEEDLE-TRIANGLE REGIME (review round 1, item 6c).  `denom > 0`
	// here does not mean well-conditioned: a needle -- three nearly
	// colinear vertices, tiny area relative to its own edge lengths -- can
	// pass this guard with `denom` a small positive number that is itself
	// the DIFFERENCE of much larger `d1..d6` products (`va`/`vb`/`vc` each
	// carry that same cancellation).  `v` and `w` can then carry a large
	// RELATIVE error.  What that error is NOT is unbounded or NaN: `denom`
	// is checked strictly `> 0` above (the only path to a 0/0 divide is the
	// branch already taken for it), so `v = vb/denom` and `w = vc/denom`
	// are finite.
	//
	// "Finite" is not by itself "small", though -- `q = a + ab*v + ac*w`
	// being a linear combination of THIS triangle's own edge vectors does
	// NOT, on its own, bound `q`'s displacement from `a` to "a small
	// multiple of the triangle's own size": `v`/`w` are unclamped ratios,
	// and `denom` (`va + vb + vc`, the Cramer's-rule identity `4 * Area^2`)
	// depends on the triangle ALONE while `vb`/`vc` scale with `|ap|`-sized
	// products -- so shared provenance in `d1..d6` does NOT bound the
	// ratio: fix a needle and slide `p` along its plane and `vb/denom`
	// grows without limit.  What actually bounds `v`/`w` is the REGION
	// TESTS above: reaching this line means every vertex region and every
	// edge region was excluded, so `(v, w)` are the barycentrics of `p`'s
	// in-triangle projection, which lie in `[0, 1]` in exact arithmetic
	// with `v + w <= 1`; rounding can push them past those bounds only by
	// the region tests' own rounding slack.  Measured on needles
	// `a=(0,0,0) b=(1,0,0) c=(0.5,eps,0)` with 300k probes each: eps=1e-4
	// reaches this branch 58k times with worst max(|v|,|w|) = 0.998;
	// eps=1e-8 43k times, worst 0.9976; at eps<=1e-12 the branch is never
	// reached (the region tests route every probe to an edge or vertex).
	// No NaN is possible here: `denom > 0` is tested above, and a positive
	// denormal `denom` dividing a bounded `vb` gives a finite (possibly
	// large) `v` that the region tests have already excluded in practice.
	const Scalar v = vb / denom;
	const Scalar w = vc / denom;
	const Point3 q = Point3Ops::mkPoint3( a, ab * v + ac * w );
	return Vector3Ops::Magnitude( Vector3Ops::mkVector3( p, q ) );
}

//! See the header for the full contract (sheet semantics, no
//! interpenetration clamp, refusal when there is no BVH).
bool TriangleMeshGeometryIndexed::DistanceToSurface(
	const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const
{
	if( !pPtrBVH ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( (double)ptObject.x )
	 || !RISE::IsFiniteDouble( (double)ptObject.y )
	 || !RISE::IsFiniteDouble( (double)ptObject.z ) ) {
		return false;
	}
	// An unbounded permission (Object::DistanceToSurface hands RISE_INFINITY
	// down for the refusal CONFIRM, and an anisotropic transform can inflate
	// an ordinary radius to it) means "no cut-off", not "search a DBL_MAX
	// box": the traversal's own pruning is what bounds the work, and the
	// initial `best` only has to be larger than any real answer.  Nothing
	// downstream squares it, so DBL_MAX is safe to carry as-is.
	if( !( maxDistObject > Scalar( 0 ) ) ) {
		return false;
	}

	Scalar d = Scalar( 0 );
	const bool ok = pPtrBVH->ClosestPointDistance(
		ptObject, maxDistObject,
		[]( const PointerTriangle* tri, const Point3& q ) -> Scalar {
			return TriangleMeshGeometryIndexed::PointTriangleDistance(
				q, *tri->pVertices[0], *tri->pVertices[1], *tri->pVertices[2] );
		},
		d );

	if( !ok ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( (double)d ) || d < Scalar( 0 ) ) {
		return false;
	}
	outDist = d;
	return true;
}

static const char * szSignature = "RISETMGI";
static const unsigned int cur_version = 5;
//
// Version history:
//   1 — original (legacy)
//   2 — same layout, added BSP/octree validation on load
//   3 — Tier 1 §2 .risemesh: writes BVH cache after the (now-vestigial)
//       BSP/octree block.  Readers that didn't understand v3 fell back
//       to v2 behaviour.
//   4 — Tier A2 cleanup (2026-04-27): drops the legacy on-disk fields
//       that are no longer carried as class state.  Specifically:
//         * nMaxPerOctantNode (uint32) — gone
//         * nMaxRecursionLevel (char)  — gone
//         * bUseBSP (char) + bptrbsptree/bptroctree (char) + tree-bytes — gone
//       Layout: signature(8) + version(4) + bUseFaceNormals(1) +
//         pPoints + pNormals + pCoords + ptr_polygons (variable) +
//         bDoubleSided(1) + haveBVHCache(1) + (BVH bytes if cached).
//       Pre-v4 readers cannot load v4 files (different field ordering
//       + missing bytes); v4 readers handle every prior version via
//       the per-version Deserialize branches below.
//   5 — Per-vertex color support (2026-04-28): inserts an optional
//       colors block immediately after the texture-coordinate block
//       and before the polygon block.  Layout adds:
//         * numColors (uint32) — 0 means "no colors"
//         * numColors × 3 doubles (linear ROMM RGB triplets)
//       Color indices are tied to vertex position indices, so no
//       per-triangle color index is written.  v1..v4 readers cannot
//       load v5 files; v5 readers handle every prior version via the
//       per-version Deserialize branches below.

void TriangleMeshGeometryIndexed::Serialize( IWriteBuffer& buffer ) const
{
	// stuff data into the buffer

	// first write out the signature and version
	buffer.setBytes( szSignature, 8 );
	buffer.setUInt( cur_version );

	// put geometry settings
	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( bUseFaceNormals ? 1 : 0 );

	// Now put geometry data

	// List of points
	{
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof(Vertex)*pPoints.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(pPoints.size()) );

		MyPointsList::const_iterator	it;
		for( it=pPoints.begin(); it!=pPoints.end(); it++ ) {
			const Vertex&	v = *it;
			buffer.setDouble( v.x );
			buffer.setDouble( v.y );
			buffer.setDouble( v.z );
		}
	}

	// List of normals
	{
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof(Normal)*pNormals.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(pNormals.size()) );

		MyNormalsList::const_iterator	it;
		for( it=pNormals.begin(); it!=pNormals.end(); it++ ) {
			const Normal&	n = *it;
			buffer.setDouble( n.x );
			buffer.setDouble( n.y );
			buffer.setDouble( n.z );
		}
	}

	// List of texture co-ordinates
	{
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof(Normal)*pCoords.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(pCoords.size()) );

		MyCoordsList::const_iterator	it;
		for( it=pCoords.begin(); it!=pCoords.end(); it++ ) {
			const TexCoord&	c = *it;
			buffer.setDouble( c.x );
			buffer.setDouble( c.y );
		}
	}

	// v5: optional list of per-vertex colors.  numColors == 0 when the
	// mesh has no color data.  Each color is three doubles in the
	// engine's working color space (linear ROMM RGB; see RISEPel).
	{
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof(double) * 3 * pColors.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(pColors.size()) );

		MyColorsList::const_iterator it;
		for( it = pColors.begin(); it != pColors.end(); ++it ) {
			const VertexColor& c = *it;
			buffer.setDouble( c.r );
			buffer.setDouble( c.g );
			buffer.setDouble( c.b );
		}
	}

	// List of pointer polygons (convert them to indexed polygons!)
	{
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof( IndexedTriangle ) * ptr_polygons.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(ptr_polygons.size()) );

		// Do pointer arithmetic to do the conversion
		// NOTE: this only works if the points, normals and coords list
		//  are all vectors
		unsigned int vertex_ptr_begin = VoidPtrToUInt( (void*)pPoints.data() );
		unsigned int normal_ptr_begin = VoidPtrToUInt( (void*)pNormals.data() );
		unsigned int coord_ptr_begin = VoidPtrToUInt( (void*)pCoords.data() );

		MyPointerTriangleList::const_iterator it;
		for( it=ptr_polygons.begin(); it != ptr_polygons.end(); it++ ) {
			const PointerTriangle& ptrtri = *it;

			for( int i=0; i<3; i++ ) {
				// For each of the vertices take the pointer off set, subtract by
				// begining and divide by the size of the type
				buffer.setUInt( ((VoidPtrToUInt(ptrtri.pVertices[i]))-vertex_ptr_begin) / sizeof( Vertex ) );
				buffer.setUInt( ((VoidPtrToUInt(ptrtri.pNormals[i]))-normal_ptr_begin) / sizeof( Normal ) );
				buffer.setUInt( ((VoidPtrToUInt(ptrtri.pCoords[i]))-coord_ptr_begin) / sizeof( TexCoord ) );
			}
		}
	}

	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( bDoubleSided ? 1 : 0 );

	// Tier A2 (.risemesh v4): the BVH cache is the only acceleration
	// data on disk.  No more BSP/octree byte block.
	//
	// Symmetric-gating: only emit the cache flag when we actually have
	// polygon data.  Deserialize gates the read on the same condition
	// (`ptr_polygons.size() > 0`), so an empty-mesh round-trip stays
	// byte-symmetric across Serialize/Deserialize.  Empty meshes occur
	// in test fixtures (mesh-not-yet-fed-via-Begin/Done) and would
	// otherwise leave a trailing flag byte unconsumed in composite
	// streams.  See Tier A2 review notes, 2026-04-27.
	if( !ptr_polygons.empty() ) {
		buffer.ResizeForMore( sizeof( char ) );
		if( pPtrBVH ) {
			buffer.setChar( 1 );
			// Index function: each prim is a (const PointerTriangle*) into
			// our ptr_polygons array; subtract base pointer to get the index.
			const PointerTriangle* base = &ptr_polygons[0];
			pPtrBVH->Serialize( buffer,
				[base]( const PointerTriangle* p ) -> unsigned int {
					return (unsigned int)( p - base );
				} );
		} else {
			buffer.setChar( 0 );
		}
	}
}

void TriangleMeshGeometryIndexed::Deserialize( IReadBuffer& buffer )
{
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: Begining deserialization process" );

	// First look for the triangle mesh geometry signature
	char sig[9] = {0};
	buffer.getBytes( sig, 8 );

	if( strcmp( sig, szSignature ) != 0 ) {
		GlobalLog()->PrintEasyError( "TriangleMeshGeometryIndexed::Deserialize:: Signature not found.  Are you using an older format?" );
		return;
	}

	// Next check version. The v5 writer adds vertex colors to the Tier A2
	// v4 acceleration layout; v1/v2/v3/v4 remain readable.
	const unsigned int version = buffer.getUInt();

	if( version < 1 || version > cur_version ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshGeometryIndexed::Deserialize:: Unsupported .risemesh version %u (this build understands v1..v%u)",
			version, cur_version );
		return;
	}

	// Pre-v4 layouts started with octree settings (nMaxPerOctantNode +
	// nMaxRecursionLevel) before the geometry settings.  Read+discard them
	// for backward compat — the BVH builder is parameter-free in this code
	// path (cfg below holds the live values).
	if( version < 4 ) {
		(void)buffer.getUInt();   // legacy nMaxPerOctantNode
		(void)buffer.getChar();   // legacy nMaxRecursionLevel
	}

	// Get geometry settings
	bUseFaceNormals = !!buffer.getChar();

	pShaderDirections.clear(); // live-only payload cannot survive topology deserialization
	InvalidateTopologyProofCaches();
	pPoints.clear();
	pNormals.clear();
	pCoords.clear();
	ptr_polygons.clear();

	// Now get the list of points
	{
		unsigned int numpts = buffer.getUInt();
		if( numpts > 0 ) {
			// Load all the points
			pPoints.reserve( numpts );

			for( unsigned int i=0; i<numpts; i++ ) {
				Vertex	v;
				v.x = buffer.getDouble();
				v.y = buffer.getDouble();
				v.z = buffer.getDouble();
				pPoints.push_back( v );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d points", numpts );
	}

	// Get the list of normals
	{
		unsigned int numnormals = buffer.getUInt();
		if( numnormals > 0 ) {
			// Load all the normals
			pNormals.reserve( numnormals );
			for( unsigned int i=0; i<numnormals; i++ ) {
				Normal n;
				n.x = buffer.getDouble();
				n.y = buffer.getDouble();
				n.z = buffer.getDouble();
				pNormals.push_back( n );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d normals", numnormals );
	}

	if( bUseFaceNormals ) {
		stl_utils::container_erase_all< NormalsListType >( pNormals );
	}

	// Get the list of co-ordinates
	{
		unsigned int numcoords = buffer.getUInt();
		if( numcoords > 0 ) {
			// Load all the coords
			pCoords.reserve( numcoords );
			for( unsigned int i=0; i<numcoords; i++ ) {
				TexCoord tc;
				tc.x = buffer.getDouble();
				tc.y = buffer.getDouble();
				pCoords.push_back( tc );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d texture co-ordinates", numcoords );
	}

	// v5: optional per-vertex colors.  Pre-v5 files do not have this
	// block; pColors stays empty and the vertex-color painter will fall
	// back to its configured default for any consumer of those meshes.
	pColors.clear();
	if( version >= 5 ) {
		unsigned int numColorsRead = buffer.getUInt();
		if( numColorsRead > 0 ) {
			pColors.reserve( numColorsRead );
			for( unsigned int i = 0; i < numColorsRead; ++i ) {
				VertexColor c;
				c.r = buffer.getDouble();
				c.g = buffer.getDouble();
				c.b = buffer.getDouble();
				pColors.push_back( c );
			}
		}
		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %u vertex colors", numColorsRead );
	}

	// Get the list of indexed triangles, convert them to "pointer polygons"
	{
		unsigned int numptrpolys = buffer.getUInt();
		if( numptrpolys > 0 ) {
			// Load the pointer polygons
			ptr_polygons.reserve( numptrpolys );

			for( unsigned int j=0; j<numptrpolys; j++ ) {
				PointerTriangle		ptrtri;

				for( unsigned int i=0; i<3; i++ ) {
					ptrtri.pVertices[i] = &pPoints[buffer.getUInt()];

					unsigned int normal_id = buffer.getUInt();
					if( bUseFaceNormals || pNormals.size()==0 ) {
						ptrtri.pNormals[i] = 0;
					} else {
						ptrtri.pNormals[i] = &pNormals[normal_id];
					}

					ptrtri.pCoords[i] = &pCoords[buffer.getUInt()];
				}

				ptr_polygons.push_back( ptrtri );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d pointer polygons", numptrpolys );
	}

	char bdoublesided = buffer.getChar();
	bDoubleSided = !!bdoublesided;
	GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Polygons are double sided? [%s]", bDoubleSided?"YES":"NO" );

	// Pre-v4 files have a BSP/octree block here that the live class no
	// longer carries.  Read+discard via Deserialize-local temporaries so
	// the byte stream advances past the block; the parsed trees are
	// validated for "structurally non-broken" so we can warn loudly on
	// truly corrupt files (e.g. mid-stream truncation), but the BVH is
	// the sole live acceleration structure regardless.
	if( version < 4 ) {
		const bool legacyUseBSP = !!buffer.getChar();
		const bool legacyHaveTree = !!buffer.getChar();

		if( legacyHaveTree ) {
			// v2/v3 streams stored a real serialized tree; v1 streams
			// also reach here but their tree format is not byte-compatible
			// with the v2 SAH layout, so v1 files just emit a warning
			// and fall through (rebuild from polygon data below).
			if( version >= 2 ) {
				if( legacyUseBSP ) {
					BSPTreeSAH<const PointerTriangle*>* pLegacyBSP =
						new BSPTreeSAH<const PointerTriangle*>(
							*this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), 1 );
					GlobalLog()->PrintNew( pLegacyBSP, __FILE__, __LINE__, "legacy BSP (read+discard)" );
					pLegacyBSP->Deserialize( buffer );
					safe_release( pLegacyBSP );
				} else {
					Octree<const PointerTriangle*>* pLegacyOct =
						new Octree<const PointerTriangle*>(
							*this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), 1 );
					GlobalLog()->PrintNew( pLegacyOct, __FILE__, __LINE__, "legacy Octree (read+discard)" );
					pLegacyOct->Deserialize( buffer );
					safe_release( pLegacyOct );
				}
			} else {
				GlobalLog()->PrintEasyWarning(
					"TriangleMeshGeometryIndexed::Deserialize:: Legacy v1 BSP byte block detected — skipping (BVH will rebuild from polygon data)" );
			}
		}
	}

	// v3+ files have the BVH cache after the legacy block (or, in v4,
	// directly after bDoubleSided).  Try to load it; on success we skip
	// the SAH rebuild entirely.  v1/v2 files don't have this trailing
	// section and fall through to rebuild.
	bool bvhCacheLoaded = false;
	safe_release( pPtrBVH );

	if( version >= 3 && ptr_polygons.size() > 0 ) {
		const char haveBVHCache = buffer.getChar();
		if( haveBVHCache ) {
			AccelerationConfig cfg;
			cfg.maxLeafSize            = 4;
			cfg.binCount               = 32;
			cfg.sahTraversalCost       = 1.0;
			cfg.sahIntersectionCost    = 1.0;
			cfg.doubleSided            = bDoubleSided;

			// Empty-input ctor: we only want the BVH<> shell so we can
			// call Deserialize.  Pass an empty input vector + a dummy
			// bbox.  Build() returns early on empty input;
			// Deserialize then overwrites the empty state.
			std::vector<const PointerTriangle*> emptyTemp;
			BoundingBox dummyBox( Point3(0,0,0), Point3(0,0,0) );
			pPtrBVH = new BVH<const PointerTriangle*>( *this, emptyTemp, dummyBox, cfg );
			GlobalLog()->PrintNew( pPtrBVH, __FILE__, __LINE__, "pointers BVH (cache load)" );

			const PointerTriangle* base = ptr_polygons.empty() ? nullptr : &ptr_polygons[0];
			const bool ok = pPtrBVH->Deserialize( buffer,
				(uint32_t)ptr_polygons.size(),
				[base]( unsigned int idx ) -> const PointerTriangle* {
					return base + idx;
				} );

			if( ok ) {
				bvhCacheLoaded = true;
				GlobalLog()->PrintEx( eLog_Info,
					"TriangleMeshGeometryIndexed::Deserialize:: Loaded BVH cache "
					"(%u nodes, %u prims) — skipping SAH rebuild",
					(unsigned)pPtrBVH->numNodes(),
					(unsigned)pPtrBVH->numPrims() );
			} else {
				GlobalLog()->PrintEasyWarning(
					"TriangleMeshGeometryIndexed::Deserialize:: BVH cache failed to load; rebuilding" );
				safe_release( pPtrBVH );
			}
		}
	}

	// If we don't have a usable BVH yet (legacy file, missing cache, or
	// cache deserialize failed), rebuild from the loaded polygon data.
	if( !bvhCacheLoaded && ptr_polygons.size() > 0 ) {
		GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: Rebuilding BVH from %u polygons", ptr_polygons.size() );

		// Compute bounding box from vertex data
		BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
		std::vector<const PointerTriangle*> temp;

		MyPointerTriangleList::iterator pi, pe;
		for( pi=ptr_polygons.begin(), pe=ptr_polygons.end(); pi!=pe; pi++ ) {
			temp.push_back( &(*pi) );
		}

		MyPointsList::const_iterator mi, mn;
		for( mi=pPoints.begin(), mn=pPoints.end(); mi!=mn; mi++ ) {
			bbox.Include( *mi );
		}

		safe_release( pPtrBVH );

		AccelerationConfig cfg;
		cfg.maxLeafSize            = 4;
		cfg.binCount               = 32;
		cfg.sahTraversalCost       = 1.0;
		cfg.sahIntersectionCost    = 1.0;
		cfg.doubleSided            = bDoubleSided;

		pPtrBVH = new BVH<const PointerTriangle*>( *this, temp, bbox, cfg );
		GlobalLog()->PrintNew( pPtrBVH, __FILE__, __LINE__, "pointers BVH (rebuilt from .risemesh polygon data)" );
		GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: BVH rebuilt successfully" );
	}

	ComputeAreas();
    // Modern loads derive closure from the loaded data. Legacy v1-v3 loads
    // keep their frozen unsigned eligibility (DL-382 shipped-scene audit).
    // Neither may inherit a closed certificate from the previous mesh.
    if(version>=4) ComputeWatertightness();
    else m_bWatertight=false;
	GeometricUtilities::BumpPatchCertificateEpoch();
	// Replacing a mesh under the same geometry identity also invalidates
	// geometry-owned signal tables and their cached per-position normals.
	InvalidateSignalBakes();

	// And we're done!
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: Finished deserialization", bDoubleSided );
}

void TriangleMeshGeometryIndexed::ComputeVertexNormals()
{
    smsUncertainNormalOrientation=true;
    m_bProvablyOpenSheet=false;
    m_bZeroVolumeSheet=false;
    m_bShapeOpenSheet=false;
    openGraphAxis=-1;
    openRefusalPair[0]=openRefusalPair[1]=size_t(-1);
    openGraphCoincidentPairs.clear();
	GeometricUtilities::BumpPatchCertificateEpoch();
	pNormals.clear();
	pNormals.reserve( pPoints.size() );
	CalculateVertexNormals( indexedtris, pNormals, pPoints );
	// The bakes' per-position orientations are accumulated from these
	// normals, so a recompute stales them.
	InvalidateSignalBakes();
}

// Derivatives for a triangle in barycentric parameterization.
//
// Parameterize the triangle by p(u,v) = v0 + u*(v1-v0) + v*(v2-v0) with
// (u, v) in the standard barycentric domain {(u,v) : u,v >= 0, u+v <= 1}.
// Then dp/du and dp/dv are the triangle edge vectors e1 = v1-v0 and
// e2 = v2-v0 respectively — constant across the triangle, so the triangle
// is actually a flat patch in parameter space.
//
// For smooth-shaded triangles the normal is barycentric-interpolated:
//   N_raw(u,v) = n0 + u*(n1-n0) + v*(n2-n0)
//   N(u,v)     = N_raw / |N_raw|
// Differentiating the normalized expression gives:
//   dN/du = (dN_raw/du - N*dot(N, dN_raw/du)) / |N_raw|
//
// The returned (dpdu, dpdv, n) may be LEFT-HANDED depending on the
// triangle winding vs its per-vertex normals.  We flip dpdv's sign (and
// dndv accordingly) if needed so the frame is always right-handed, per
// docs/GEOMETRY_DERIVATIVES.md.
static SurfaceDerivatives ComputeTriangleDerivatives(
	const PointerTriangle& tri,
	const Vector3& objSpaceNormal,
	const Point3& objSpacePoint,
	const Scalar barU,
	const Scalar barV,
	bool hasVertexNormals )
{
	SurfaceDerivatives sd;
	sd.valid = true;

	// Strategy (mirrors TriangleMeshGeometryIndexedSpecializations.h):
	// invert the 2×2 UV Jacobian using the stored per-vertex texture
	// coordinates so derivatives come out in the stored parameterisation.
	// For TessellateToMesh-produced meshes (sphere, torus, ellipsoid,
	// cylinder) those UVs ARE the analytical shape's own (u,v), so the
	// frame is aligned with the analytical parameterisation and scales
	// match the analytical |dpdu|, |dpdv|.
	//
	// Fall back to the barycentric-edge frame when UVs are missing or
	// degenerate (|det|<ε: pole triangle or collinear-UV triangle).
	const Vector3 e1 = Vector3Ops::mkVector3( *tri.pVertices[1], *tri.pVertices[0] );
	const Vector3 e2 = Vector3Ops::mkVector3( *tri.pVertices[2], *tri.pVertices[0] );

	bool useUVJacobian = false;
	Scalar uvDet = 0.0;
	Scalar duA = 0.0, duB = 0.0, dvA = 0.0, dvB = 0.0;
	if( tri.pCoords[0] && tri.pCoords[1] && tri.pCoords[2] ) {
		duA = tri.pCoords[1]->x - tri.pCoords[0]->x;
		duB = tri.pCoords[2]->x - tri.pCoords[0]->x;
		dvA = tri.pCoords[1]->y - tri.pCoords[0]->y;
		dvB = tri.pCoords[2]->y - tri.pCoords[0]->y;
		uvDet = duA * dvB - duB * dvA;
		if( fabs( uvDet ) > NEARZERO ) {
			useUVJacobian = true;
		}
	}

	Vector3 dpdu_raw, dpdv_raw;
	if( useUVJacobian ) {
		const Scalar invDet = 1.0 / uvDet;
		dpdu_raw = Vector3(
			( e1.x * dvB - e2.x * dvA ) * invDet,
			( e1.y * dvB - e2.y * dvA ) * invDet,
			( e1.z * dvB - e2.z * dvA ) * invDet );
		dpdv_raw = Vector3(
			( e2.x * duA - e1.x * duB ) * invDet,
			( e2.y * duA - e1.y * duB ) * invDet,
			( e2.z * duA - e1.z * duB ) * invDet );
	} else {
		dpdu_raw = e1;
		dpdv_raw = e2;
	}

	// Project dpdu, dpdv into the shading-normal tangent plane so
	// dpdu·n ≈ 0 even for smooth-shaded triangles whose shading normal
	// differs from the face normal.  (See docs/GEOMETRY_DERIVATIVES.md.)
	const Scalar dpdu_dot_n = Vector3Ops::Dot( dpdu_raw, objSpaceNormal );
	const Scalar dpdv_dot_n = Vector3Ops::Dot( dpdv_raw, objSpaceNormal );
	sd.dpdu = Vector3(
		dpdu_raw.x - objSpaceNormal.x * dpdu_dot_n,
		dpdu_raw.y - objSpaceNormal.y * dpdu_dot_n,
		dpdu_raw.z - objSpaceNormal.z * dpdu_dot_n );
	sd.dpdv = Vector3(
		dpdv_raw.x - objSpaceNormal.x * dpdv_dot_n,
		dpdv_raw.y - objSpaceNormal.y * dpdv_dot_n,
		dpdv_raw.z - objSpaceNormal.z * dpdv_dot_n );

	if( hasVertexNormals && tri.pNormals[0] && tri.pNormals[1] && tri.pNormals[2] ) {
		// Raw normal-diff derivatives (in barycentric (A, B)).
		const Vector3 dNraw_dA = Vector3(
			tri.pNormals[1]->x - tri.pNormals[0]->x,
			tri.pNormals[1]->y - tri.pNormals[0]->y,
			tri.pNormals[1]->z - tri.pNormals[0]->z );
		const Vector3 dNraw_dB = Vector3(
			tri.pNormals[2]->x - tri.pNormals[0]->x,
			tri.pNormals[2]->y - tri.pNormals[0]->y,
			tri.pNormals[2]->z - tri.pNormals[0]->z );

		// Apply the same UV-Jacobian inversion to the normal derivatives
		// as to dpdu/dpdv, so the resulting dN/du, dN/dv live in the
		// SAME parameterisation as dpdu/dpdv.
		Vector3 dNraw_du, dNraw_dv;
		if( useUVJacobian ) {
			const Scalar invDet = 1.0 / uvDet;
			dNraw_du = Vector3(
				( dNraw_dA.x * dvB - dNraw_dB.x * dvA ) * invDet,
				( dNraw_dA.y * dvB - dNraw_dB.y * dvA ) * invDet,
				( dNraw_dA.z * dvB - dNraw_dB.z * dvA ) * invDet );
			dNraw_dv = Vector3(
				( dNraw_dB.x * duA - dNraw_dA.x * duB ) * invDet,
				( dNraw_dB.y * duA - dNraw_dA.y * duB ) * invDet,
				( dNraw_dB.z * duA - dNraw_dA.z * duB ) * invDet );
		} else {
			dNraw_du = dNraw_dA;
			dNraw_dv = dNraw_dB;
		}

		// Reconstruct |N_raw| at this (u,v) using the interpolated
		// unnormalized normal. The caller-supplied objSpaceNormal is
		// already normalized, so we can use the raw barycentric
		// interpolation (using dNraw_dA, dNraw_dB, the original basis —
		// barU/barV are in barycentric space, not (u,v) space).
		const Vector3 Nraw = Vector3(
			tri.pNormals[0]->x + barU * dNraw_dA.x + barV * dNraw_dB.x,
			tri.pNormals[0]->y + barU * dNraw_dA.y + barV * dNraw_dB.y,
			tri.pNormals[0]->z + barU * dNraw_dA.z + barV * dNraw_dB.z );
		const Scalar len = Vector3Ops::Magnitude( Nraw );
		const Scalar invLen = (len > NEARZERO) ? 1.0 / len : 0.0;

		// dN/d* = projection perpendicular to N, divided by |N_raw|.
		// Use the caller-supplied objSpaceNormal (which is the shading
		// normal the RISE runtime attached to this hit), not the
		// reconstructed N, so that our dn/d* is rigorously perpendicular
		// to the same normal a downstream consumer will use.
		const Scalar dot_u = Vector3Ops::Dot( objSpaceNormal, dNraw_du );
		const Scalar dot_v = Vector3Ops::Dot( objSpaceNormal, dNraw_dv );
		sd.dndu = Vector3(
			(dNraw_du.x - objSpaceNormal.x * dot_u) * invLen,
			(dNraw_du.y - objSpaceNormal.y * dot_u) * invLen,
			(dNraw_du.z - objSpaceNormal.z * dot_u) * invLen );
		sd.dndv = Vector3(
			(dNraw_dv.x - objSpaceNormal.x * dot_v) * invLen,
			(dNraw_dv.y - objSpaceNormal.y * dot_v) * invLen,
			(dNraw_dv.z - objSpaceNormal.z * dot_v) * invLen );
	} else {
		// Face-normal mesh: flat face, no per-vertex normal variation
		sd.dndu = Vector3( 0, 0, 0 );
		sd.dndv = Vector3( 0, 0, 0 );
	}

	// Handedness: (dpdu × dpdv) · n must be > 0.  If the triangle
	// winding gave the wrong sign w.r.t. the shading normal, flip dpdv
	// and dndv.
	const Vector3 cross = Vector3Ops::Cross( sd.dpdu, sd.dpdv );
	if( Vector3Ops::Dot( cross, objSpaceNormal ) < 0.0 ) {
		sd.dpdv = Vector3( -sd.dpdv.x, -sd.dpdv.y, -sd.dpdv.z );
		sd.dndv = Vector3( -sd.dndv.x, -sd.dndv.y, -sd.dndv.z );
	}

	sd.uv = Point2( barU, barV );
	return sd;
}

// Compute barycentric coords of a point in a triangle (ignoring
// out-of-plane component).  Returns (u, v) such that
//   p ≈ v0 + u*(v1-v0) + v*(v2-v0).
// `inside` is set to true iff (u,v) is inside the triangle (with
// tolerance epsTol).  The "containment" test is the only way to
// disambiguate which triangle a point belongs to.
static void BarycentricCoords(
	const Point3& v0, const Point3& v1, const Point3& v2,
	const Point3& p,
	Scalar& u, Scalar& v, bool& inside,
	const Scalar epsTol = 1e-4 )
{
	const Vector3 e1 = Vector3Ops::mkVector3( v1, v0 );
	const Vector3 e2 = Vector3Ops::mkVector3( v2, v0 );
	const Vector3 pv = Vector3Ops::mkVector3( p, v0 );
	const Scalar a = Vector3Ops::Dot( e1, e1 );
	const Scalar b = Vector3Ops::Dot( e1, e2 );
	const Scalar c = Vector3Ops::Dot( e2, e2 );
	const Scalar d = Vector3Ops::Dot( e1, pv );
	const Scalar e = Vector3Ops::Dot( e2, pv );
	const Scalar det = a*c - b*b;
	if( std::fabs( det ) < NEARZERO ) {
		u = 0; v = 0; inside = false;
		return;
	}
	u = (c*d - b*e) / det;
	v = (a*e - b*d) / det;
	inside = (u >= -epsTol) && (v >= -epsTol) && (u + v <= 1.0 + epsTol);
}

SurfaceDerivatives TriangleMeshGeometryIndexed::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	// Walk the triangle list to find which triangle contains the point.
	// This is O(n_triangles) — fine for tests and rare one-off queries.
	// SMS and other frequent callers should read derivatives directly
	// from the RayIntersectionGeometric record populated during
	// IntersectRay (stage 1.1 of the SMS work), avoiding this walk.
	const Scalar epsTol = 1e-3;

	const PointerTriangle* bestTri = 0;
	Scalar bestU = 0, bestV = 0;
	Scalar bestOutOfPlane = RISE_INFINITY;

	for( MyPointerTriangleList::const_iterator it = ptr_polygons.begin();
		it != ptr_polygons.end(); ++it )
	{
		const PointerTriangle& tri = *it;
		Scalar u = 0, v = 0;
		bool inside = false;
		BarycentricCoords( *tri.pVertices[0], *tri.pVertices[1], *tri.pVertices[2],
			objSpacePoint, u, v, inside, epsTol );
		if( !inside ) continue;

		// Verify the point is actually on the triangle's plane (not just
		// inside its prism).
		const Vector3 e1 = Vector3Ops::mkVector3( *tri.pVertices[1], *tri.pVertices[0] );
		const Vector3 e2 = Vector3Ops::mkVector3( *tri.pVertices[2], *tri.pVertices[0] );
		Vector3 faceN = Vector3Ops::Cross( e1, e2 );
		const Scalar faceNLen = Vector3Ops::Magnitude( faceN );
		if( faceNLen < NEARZERO ) continue;
		faceN = faceN * (1.0 / faceNLen);
		const Vector3 delta = Vector3Ops::mkVector3( objSpacePoint, *tri.pVertices[0] );
		const Scalar oop = std::fabs( Vector3Ops::Dot( faceN, delta ) );
		if( oop < bestOutOfPlane ) {
			bestTri = &tri;
			bestU = u;
			bestV = v;
			bestOutOfPlane = oop;
		}
	}

	if( !bestTri ) {
		// Fallback: no triangle found containing the point.  Return a
		// reasonable tangent frame from the normal so callers don't NaN.
		SurfaceDerivatives sd;
		OrthonormalBasis3D onb;
		onb.CreateFromW( objSpaceNormal );
		sd.dpdu = onb.u();
		sd.dpdv = onb.v();
		sd.dndu = Vector3( 0, 0, 0 );
		sd.dndv = Vector3( 0, 0, 0 );
		sd.uv = Point2( 0, 0 );
		sd.valid = false;
		return sd;
	}

	return ComputeTriangleDerivatives( *bestTri, objSpaceNormal, objSpacePoint,
		bestU, bestV, !bUseFaceNormals );
}

//////////////////////////////////////////////////////////////////////
// ISurfaceSignalProvider -- PHASE-3 geometry-derived shading signals
// on the mesh family (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §7).
//
// The shape of this, and why it differs from the SDF family's:
//
//   * an SDF ANSWERS PER HIT from a live field, so its provider is a
//     pure const function and any radius is a legal knob;
//   * a mesh has no field, so it answers from a BAKE -- which commits
//     to one radius, must be built exactly once, and must not exist at
//     all until a material actually reads it (draft previews shade no
//     materials, §7.3).
//
// So this half is: validate the radius precondition, find-or-build
// under the cache's lock, then interpolate the table barycentrically
// over the hit triangle the intersector stamped.  Everything expensive
// is behind the find; everything per-sample after the first is three
// float reads and a lerp.
//////////////////////////////////////////////////////////////////////

void TriangleMeshGeometryIndexed::InvalidateSignalBakes()
{
	// EVERYTHING derived from the vertex data -- the tables and the
	// per-position orientations they were traced along -- is dropped inside
	// the cache's ONE lock acquisition.  There is deliberately no tail after
	// this call: an earlier shape cleared a geometry-owned normal array here,
	// unlocked, where it raced a concurrent find-or-build reading that same
	// array.
	const bool bDroppedSomething = m_signalBakes.Invalidate();

	// DEBUG contract guard, the shape DisplacedGeometry::Realize() uses.
	// Conditioned on having actually dropped something, and that condition is
	// the point rather than a softening: every mesh CONSTRUCTION path
	// (BeginIndexedTriangles / DoneIndexedTriangles) invalidates an EMPTY
	// cache on a geometry no render can reach yet, which is harmless and must
	// not fire.  Dropping a POPULATED cache mid-render is the real violation
	// -- a table a worker thread may be interpolating right now.
	assert( ( !bDroppedSomething || g_renderParallelDepth.load( std::memory_order_seq_cst ) == 0 ) &&
		"TriangleMeshGeometryIndexed::InvalidateSignalBakes() dropped live per-vertex bakes during the "
		"parallel render — vertex mutation is a between-frames operation" );
	(void)bDroppedSomething;	// release builds compile the assert out

	// EXPRESSION MEMO: the L1 memo caches this provider's ANSWERS keyed on
	// (provider, hit, radius).  Dropping the bakes changes those answers
	// behind an unchanged key, so the memo must go with them
	// (Utilities/ExpressionMemo.h).
	//
	// AFTER the drop, never before, and this site needs no companion bump
	// on the way in (unlike the three seams in Scene, ObjectManager and
	// RayCaster, which each do work of their own that evaluates painters).
	// A bump ahead of the mutation would open a window another thread can
	// fall into: it adopts the NEW generation, misses (its tables were just
	// cleared), asks the provider -- which is still holding the OLD bake --
	// and inserts that stale answer stamped with the new generation, where
	// nothing will ever invalidate it.  Bumping last means every table
	// filled from the old bake carries the old generation and is dropped on
	// its owner's next look.  (The window needs a concurrent reader, which
	// the assert above says must not exist during a parallel render; the
	// ordering costs nothing, so it is not left to rest on that.)
	ExpressionMemo::Invalidate();
}

void TriangleMeshGeometryIndexed::BuildVertexNormals( std::vector<Vector3>& out ) const
{
	// Called by MeshSignalBakeCache::FindOrBuild from inside its lock, on the
	// first miss of a cache generation.  Writes into the CACHE's array and
	// keeps nothing of its own, so there is no second copy of this state for
	// an invalidation to have to chase.
	out.assign( pPoints.size(), Vector3( 0, 0, 0 ) );
	if( pPoints.empty() ) {
		return;
	}

	// ONE orientation per POSITION, accumulated over every incident corner.
	//
	// The mesh's own `pNormals` cannot be used directly: it is indexed
	// INDEPENDENTLY of positions (a position shared by two smoothing groups
	// carries two normals) and is empty entirely on a face-normal mesh.
	// Accumulating the AUTHORED corner normal where there is one, and the
	// face normal where there is not, gives a per-position orientation that
	// agrees with what the renderer treats as "outward" on this mesh -- which
	// matters more than any intrinsic definition, because a mesh wound
	// inward would otherwise have its whole AO bake trace into the solid.
	const Vertex* pBase = &pPoints[0];
	for( MyPointerTriangleList::const_iterator it = ptr_polygons.begin(); it != ptr_polygons.end(); ++it ) {
		const PointerTriangle& tri = *it;

		const Vector3 e1 = Vector3Ops::mkVector3( *tri.pVertices[1], *tri.pVertices[0] );
		const Vector3 e2 = Vector3Ops::mkVector3( *tri.pVertices[2], *tri.pVertices[0] );
		// NOT normalized: the cross product's magnitude is twice the
		// triangle's area, which is exactly the weight a vertex-normal
		// accumulation wants (a sliver contributes less than the face it
		// slivers off).
		const Vector3 faceN = Vector3Ops::Cross( e1, e2 );

		for( int k = 0; k < 3; ++k ) {
			const size_t idx = (size_t)( tri.pVertices[k] - pBase );
			if( idx >= out.size() ) {
				continue;
			}
			if( tri.pNormals[k] ) {
				const Vector3& an = *tri.pNormals[k];
				out[idx] = out[idx] + an;
			} else {
				out[idx] = out[idx] + faceN;
			}
		}
	}

	// Normalize; a vertex that accumulated to zero (opposed contributions,
	// or no incident triangle at all) is LEFT at zero, which the bake reads
	// as "unorientable" and answers with the neutral value rather than an
	// invented direction.
	for( size_t i = 0; i < out.size(); ++i ) {
		const Vector3& v = out[i];
		const Scalar len2 = v.x*v.x + v.y*v.y + v.z*v.z;
		if( len2 > NEARZERO ) {
			const Scalar inv = Scalar(1) / std::sqrt( len2 );
			out[i] = Vector3( v.x*inv, v.y*inv, v.z*inv );
		} else {
			out[i] = Vector3( 0, 0, 0 );
		}
	}
}

bool TriangleMeshGeometryIndexed::MakeBakeInput( const Scalar radiusFraction,
	const std::vector<Vector3>& vertexNormals, MeshSignalBake::Input& out ) const
{
	// Called by MeshSignalBakeCache::FindOrBuild from inside its lock, and
	// only on a cache MISS -- which keeps this whole function off the
	// per-sample path.  `vertexNormals` is the cache-owned array
	// BuildVertexNormals() filled; it outlives the build.
	if( pPoints.empty() || ptr_polygons.empty() || !pPtrBVH ) {
		return false;
	}
	if( vertexNormals.size() != pPoints.size() ) {
		return false;
	}

	// The radius is a FRACTION of this geometry's own object-space
	// bounding-box diagonal -- the shared convention `scaleHint` and the SDF
	// estimators already use (design doc §9).  Because the bake is
	// geometry-owned it is SHARED by every instance of this mesh, so this
	// object-space reading is also the only one that can be correct for all
	// of them: `thickness(0.05)` means "5 % of the object" on each instance,
	// and the world distance it corresponds to scales with the instance
	// (§5.2's instancing paragraph).
	const Scalar diagonal = SurfaceCurvature::ScaleHintFromBoundingBox( GenerateBoundingBox() );
	if( !RISE::IsFiniteDouble( static_cast<double>( diagonal ) ) || !( diagonal > Scalar(0) ) ) {
		return false;
	}

	out.pVertices     = &pPoints;
	out.pNormals      = &vertexNormals;
	out.maxDistance   = radiusFraction * diagonal;
	out.originEpsilon = MeshSignalBake::kOriginEpsilonFraction * diagonal;
	out.pOccluder     = this;
	return true;
}

bool TriangleMeshGeometryIndexed::AnyHitWithin( const Point3& origin, const Vector3& unitDir, const Scalar maxDist ) const
{
	if( !pPtrBVH ) {
		return false;
	}
	// BOTH faces.  A bake ray is not a camera ray: an occlusion ray that
	// grazes the far side of a fold, and every thickness ray (which starts
	// inside the solid and exits through a back face), would be invisible to
	// a front-faces-only query.
	//
	// THROUGH THIS CLASS'S OWN ENTRY POINT, never `pPtrBVH` directly, and
	// that is load-bearing under RISE_ENABLE_MAILBOXING (on by default on
	// every platform): the per-ray mailbox id is bumped in the class's
	// intersect methods, not in the BVH.  Reaching past them makes every
	// bake ray share one id, so the SECOND ray onward finds every triangle
	// already stamped and skips it -- a silent, total loss of hits that
	// reads as "this mesh is unoccluded and infinitely thick everywhere",
	// i.e. exactly like a correctly-neutral answer.
	return IntersectRay_IntersectionOnly( Ray( origin, unitDir ), maxDist, true, true );
}

bool TriangleMeshGeometryIndexed::NearestHitWithin( const Point3& origin, const Vector3& unitDir,
	const Scalar maxDist, Scalar& outDist ) const
{
	if( !pPtrBVH ) {
		return false;
	}
	// Same mailbox rule as AnyHitWithin: go through this class's own
	// IntersectRay, which bumps the per-ray mailbox id.  `range` seeded to
	// maxDist doubles as the closest-hit bound, so the traversal never
	// considers anything past the query radius.
	RayIntersectionGeometric ri( Ray( origin, unitDir ), nullRasterizerState );
	ri.range = maxDist;
	IntersectRay( ri, true, true, false );
	if( !ri.bHit || !( ri.range >= Scalar(0) ) || ri.range > maxDist ) {
		return false;
	}
	outDist = ri.range;
	return true;
}

bool TriangleMeshGeometryIndexed::LookupBakedSignal(
	const MeshSignalBake::Kind kind,
	const SurfaceSignalInfo& hit,
	const Scalar radiusFraction,
	const bool bRadiusIsConstant,
	Scalar& outValue ) const
{
	// §7.1's CONSTANT-RADIUS PRECONDITION.  A bake commits to one radius; a
	// computed one cannot be answered from it, and answering it from the
	// nearest available table anyway is the silent wrong-scale substitution
	// the design forbids.  Refuse, and let the caller read the neutral
	// value -- the mask is then visibly absent rather than plausibly wrong.
	if( !bRadiusIsConstant ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( static_cast<double>( radiusFraction ) ) || !( radiusFraction > Scalar(0) ) ) {
		return false;
	}
	// No triangle stamped: this hit did not come from a mesh intersector
	// (a hand-built record, or a provider mismatch).  Nothing honest to
	// interpolate.
	if( hit.primId < 0 || (size_t)hit.primId >= ptr_polygons.size() || pPoints.empty() ) {
		return false;
	}

	// Find-or-build.  On a HIT this is one lock and a linear scan of at most
	// eight radii; the input assembly (and with it the per-position normal
	// build) happens only on a MISS, inside the cache's own lock, via the
	// IInputSource callback this class implements.
	//
	// ONE shared_ptr copy for the whole query -- taken here, released at
	// return.  It is what keeps the table alive across the interpolation
	// below even if another thread invalidates the cache in between (see
	// MeshSignalBakeCache::TableRef).  Deliberately NOT re-fetched per tap:
	// the steady-state cost of this feature stays one refcount pair per
	// provider query, not one per table read.
	const MeshSignalBakeCache::TableRef table = m_signalBakes.FindOrBuild( kind, radiusFraction, *this );
	if( !table ) {
		return false;
	}

	const PointerTriangle& tri = ptr_polygons[ (size_t)hit.primId ];
	const Vertex* pBase = &pPoints[0];
	const size_t i0 = (size_t)( tri.pVertices[0] - pBase );
	const size_t i1 = (size_t)( tri.pVertices[1] - pBase );
	const size_t i2 = (size_t)( tri.pVertices[2] - pBase );

	Scalar v = Scalar(0);
	if( !InterpolateMeshSignal( *table, i0, i1, i2, hit.baryA, hit.baryB, v ) ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
		return false;
	}
	if( v < Scalar(0) ) v = Scalar(0);
	if( v > Scalar(1) ) v = Scalar(1);
	outValue = v;
	return true;
}

bool TriangleMeshGeometryIndexed::ComputeOcclusion( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const
{
	return LookupBakedSignal( MeshSignalBake::eOcclusion, hit, radiusFraction, bRadiusIsConstant, outValue );
}

bool TriangleMeshGeometryIndexed::ComputeThickness( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const
{
	return LookupBakedSignal( MeshSignalBake::eThickness, hit, radiusFraction, bRadiusIsConstant, outValue );
}

bool TriangleMeshGeometryIndexed::ComputeConvexity( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const
{
	return LookupBakedSignal( MeshSignalBake::eConvexity, hit, radiusFraction, bRadiusIsConstant, outValue );
}
