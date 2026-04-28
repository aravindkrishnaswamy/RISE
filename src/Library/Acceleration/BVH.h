//////////////////////////////////////////////////////////////////////
//
//  BVH.h - Templated 2-wide BVH with SAH binned top-down construction.
//
//  Phase 1 partial scope (per docs/BVH_ACCELERATION_PLAN.md §5):
//    - SAH binned builder, single-threaded, recursive.
//    - Single-ray stack traversal, fixed depth 64.
//    - Float-stored AABBs in nodes (conservative pad from double).
//    - Double-precision ray-triangle intersection at leaves
//      (delegated to TreeElementProcessor::RayElementIntersection).
//    - No SBVH spatial splits, no compressed nodes, no refit yet.
//      Those land in follow-up sessions.
//
//  Header-only template, mirroring the existing Octree<T>/BSPTreeSAH<T>
//  pattern.  Element type T is specialized via TreeElementProcessor<T>
//  in TriangleMeshGeometry{,Indexed}Specializations.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_BVH_
#define RISE_BVH_

#include "AccelerationConfig.h"
#include "../TreeElementProcessor.h"
#include "../Utilities/Reference.h"
#include "../Utilities/BoundingBox.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Intersection/RayIntersection.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/RTime.h"
#include "../Utilities/Profiling.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cfloat>

// Phase 3 SIMD selection.  Apple Silicon and arm64 Android (Galaxy Fold)
// route to NEON 128-bit; x86_64 with AVX2 routes to SSE2/SSE4.1 intrinsics
// (AVX2 not actually needed for 4-wide single-precision — SSE2 is enough,
// but we use AVX2 detection as the gate because that's what the rest of
// RISE assumes for "modern x86").  Anything else falls back to scalar.
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#  include <arm_neon.h>
#  define RISE_BVH_HAVE_NEON 1
#elif defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64)
#  include <emmintrin.h>
#  if defined(__SSE4_1__)
#    include <smmintrin.h>
#  endif
#  define RISE_BVH_HAVE_SSE 1
#endif

namespace RISE
{
	//////////////////////////////////////////////////////////////////////
	//
	// BVH<Element> — top-down SAH binned BVH2 over `Element` primitives.
	//
	// Storage:
	//   - `nodes` is the tree, packed depth-first.  Internal nodes carry
	//     a pair of float AABBs (their two children) + the index of
	//     `leftChild`; right child is always at leftChild+1.
	//   - Leaves store [primStart, primStart + primCount) into `prims`.
	//   - `prims` is the reordered primitive array (no duplication;
	//     SBVH spatial splits would change this in a future phase).
	//
	// Float vs double:
	//   - Internal traversal does float ray-vs-AABB tests.  AABBs are
	//     conservatively rounded outward by 1 ULP at construction so
	//     tests can never reject a valid hit purely due to fp32 loss.
	//   - Leaf intersection runs in `Scalar` (double) via the
	//     TreeElementProcessor — no precision loss for the geometry
	//     reported back to the caller.
	//
	//////////////////////////////////////////////////////////////////////
	template< class Element >
	class BVH : public Implementation::Reference
	{
	public:
		// Public node layout — flat, no virtuals, dense packing.
		struct Node
		{
			float    bboxMin[3];     // AABB lower bound (conservative float)
			float    bboxMax[3];     // AABB upper bound
			uint32_t firstPrimOrLeft;// leaf: primitive start index;
			                         // internal: left-child node index
			uint16_t primCount;      // 0 = internal, >0 = leaf
			uint8_t  splitAxis;      // 0/1/2 for x/y/z (internal only)
			uint8_t  pad;            // align to 32 B (room for refit cost later)
		};

		// Phase 2 float-filter data per leaf primitive.  Stored as
		// (P0, e1, e2) — what Möller-Trumbore needs.  36 bytes/triangle,
		// adjacent to `prims`, indexed identically (so prims[i] and
		// fastFilter[i] describe the same primitive after build-time
		// reordering).  Empty iff the element processor doesn't supply
		// `GetFloatTriangleVertices` (i.e. non-triangle primitives).
		struct TriangleFilterData
		{
			float p0[3];   // first vertex (in float)
			float e1[3];   // edge1 = v1 - v0
			float e2[3];   // edge2 = v2 - v0
		};

		// Phase 3 wide-BVH node layout (BVH4 SoA).  Up to 4 children per
		// internal node, child AABBs stored in struct-of-arrays so a single
		// SIMD load fetches all 4 mins (or maxes) on each axis for a
		// 4-wide ray-vs-AABB test.
		//
		// Encoding:
		//   - children[i]:
		//       primCount[i] > 0  → leaf, value is firstPrim into prims[]
		//       primCount[i] == 0 → internal, value is BVH4 node index
		//                           (or -1 for unused slot when numChildren < 4)
		//   - numChildren in 0..4.  Unused bbox slots are initialised to
		//     (+INF, -INF) so the ray-vs-AABB test always rejects them
		//     without needing a length check.
		struct alignas(16) BVH4Node
		{
			float    bboxMinX[4];
			float    bboxMinY[4];
			float    bboxMinZ[4];
			float    bboxMaxX[4];
			float    bboxMaxY[4];
			float    bboxMaxZ[4];
			int32_t  children[4];
			uint16_t primCount[4];
			uint8_t  numChildren;
			uint8_t  splitAxis;     // dominant split axis from BVH2 build
			uint8_t  pad[2];
		};

	protected:
		std::vector<Node>                    nodes;
		std::vector<Element>                 prims;
		std::vector<TriangleFilterData>      fastFilter;  // Phase 2; empty = filter off
		bool                                 hasFastFilter;
		std::vector<BVH4Node>                nodes4;      // Phase 3; empty = BVH4 off
		bool                                 useBVH4;
		BoundingBox                          overallBox;
		const TreeElementProcessor<Element>& ep;
		AccelerationConfig                   cfg;

		// Tier C3 (2026-04-27): SAH cost recorded once at build, used as
		// the denominator in `SAHDegradationRatio()`.  Refit() preserves
		// topology but bboxes can grow as keyframed vertices move; if
		// they grow far enough that traversal expected-cost more than
		// doubles, refit is no longer adequate and the caller should
		// rebuild from polygon data.  See Refit() warning + caller
		// rebuild fallback in TriangleMeshGeometryIndexed::UpdateVertices.
		Scalar                               originalSAH = 0;

		virtual ~BVH() {}

	public:
		BVH( const TreeElementProcessor<Element>& ep_,
		     const std::vector<Element>&          inputPrims,
		     const BoundingBox&                   overallBoxHint,
		     const AccelerationConfig&            cfg_ )
			: hasFastFilter( false ),
			  useBVH4( false ),
			  overallBox( overallBoxHint ),
			  ep( ep_ ),
			  cfg( cfg_ )
		{
			// Plain SAH binned BVH2 builder.  An SBVH spatial-split
			// alternative was investigated in Tier 1 §1 and excised in
			// Tier B (2026-04-27) — see docs/BVH_RETROSPECTIVE.md.
			// Decision: with closest-hit-native intersection and BVH4
			// collapse already in place, plain SAH extracts most of
			// the locatable benefit on this codebase's geometry;
			// spatial splits added build/memory cost with no
			// statistically significant rasterize win at any
			// duplication budget tested (0.05 / 0.10 / 0.30) on the
			// 7.2M-tri xyzdragon stress mesh.
			Build( inputPrims );

			BuildFastFilter();
			BuildBVH4();

			// Tier C3: snapshot the SAH cost of the freshly-built tree.
			// Refit-driven keyframe animation can grow per-node bboxes
			// arbitrarily; SAHDegradationRatio() compares against this
			// to detect "refit no longer adequate" cases.
			originalSAH = ComputeSAH();
		}

		size_t numNodes()   const { return nodes.size(); }
		size_t numNodes4()  const { return nodes4.size(); }
		size_t numPrims()   const { return prims.size(); }
		bool   FastFilterEnabled() const { return hasFastFilter; }
		bool   BVH4Enabled() const { return useBVH4; }
		const BoundingBox& GetBBox() const { return overallBox; }

		//////////////////////////////////////////////////////////////////
		//  SAHDegradationRatio (Tier C3 — refit safeguard).
		//
		//  Returns currentSAH / originalSAH.  The original SAH was
		//  captured at construction (a freshly-built balanced tree);
		//  Refit() preserves topology but per-node bboxes can grow as
		//  keyframed vertices move.  If they grow far enough that the
		//  ratio exceeds ~2.0, ray-traversal expected cost has more
		//  than doubled vs the original tree — the refit-only path is
		//  no longer adequate, and the caller should rebuild from
		//  polygon data instead.
		//
		//  Returns 1.0 if originalSAH is unset (e.g. mesh deserialized
		//  from a v3 cache that pre-dates Tier C3 — they get the safe
		//  default rather than a spurious "rebuild now" signal).
		//////////////////////////////////////////////////////////////////
		Scalar SAHDegradationRatio() const
		{
			if( originalSAH <= 0 ) return 1.0;
			const Scalar cur = ComputeSAH();
			if( cur <= 0 ) return 1.0;
			return cur / originalSAH;
		}

		//////////////////////////////////////////////////////////////////
		//  Refit (Tier 1 §3 animation support).
		//
		//  Walk nodes[] in reverse index order (children-before-parents
		//  by construction in BuildRecursive), recomputing
		//  each node's AABB from either its referenced prims (leaves) or
		//  its child AABBs (internal nodes).  After the BVH2 nodes are
		//  refit, re-derive the float-filter triangle data and the BVH4
		//  collapse from scratch so traversal sees the new geometry.
		//
		//  Topology (which nodes are children of which) is preserved.
		//  Caller is responsible for ensuring vertex updates don't
		//  change connectivity — that's the case for keyframed painter
		//  notifications on DisplacedGeometry whose detail is fixed.
		//
		//  Returns refit duration in milliseconds for the caller's bench
		//  reporting.
		//////////////////////////////////////////////////////////////////
		unsigned int Refit()
		{
			Timer t; t.start();
			if( nodes.empty() ) {
				BuildFastFilter();  // no-op on empty
				BuildBVH4();        // no-op on empty
				t.stop();
				return (unsigned int)t.getInterval();
			}

			BoundingBox newOverall(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );

			// Highest index first — children always have larger index
			// than parents in our build order.
			for( int32_t i = (int32_t)nodes.size() - 1; i >= 0; --i ) {
				Node& n = nodes[ (size_t)i ];
				if( n.primCount > 0 ) {
					// Leaf: AABB = union of referenced prim bboxes.
					BoundingBox b(
						Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
						Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
					const uint32_t end = n.firstPrimOrLeft + n.primCount;
					for( uint32_t p = n.firstPrimOrLeft; p < end; ++p ) {
						const BoundingBox primBox = ep.GetElementBoundingBox( prims[p] );
						b.Include( primBox.ll );
						b.Include( primBox.ur );
					}
					SetNodeBox( n, b );
				} else {
					// Internal: AABB = union of child AABBs.  Read from
					// already-refit children (they have larger indices).
					const Node& l = nodes[ n.firstPrimOrLeft     ];
					const Node& r = nodes[ n.firstPrimOrLeft + 1 ];
					n.bboxMin[0] = std::fmin( l.bboxMin[0], r.bboxMin[0] );
					n.bboxMin[1] = std::fmin( l.bboxMin[1], r.bboxMin[1] );
					n.bboxMin[2] = std::fmin( l.bboxMin[2], r.bboxMin[2] );
					n.bboxMax[0] = std::fmax( l.bboxMax[0], r.bboxMax[0] );
					n.bboxMax[1] = std::fmax( l.bboxMax[1], r.bboxMax[1] );
					n.bboxMax[2] = std::fmax( l.bboxMax[2], r.bboxMax[2] );
				}
			}

			// Update overallBox from root.
			newOverall.ll.x = nodes[0].bboxMin[0];
			newOverall.ll.y = nodes[0].bboxMin[1];
			newOverall.ll.z = nodes[0].bboxMin[2];
			newOverall.ur.x = nodes[0].bboxMax[0];
			newOverall.ur.y = nodes[0].bboxMax[1];
			newOverall.ur.z = nodes[0].bboxMax[2];
			overallBox = newOverall;

			// Re-derive filter triangle data + BVH4 collapse from the
			// refit BVH2.  These walks are O(N) and parallel-friendly —
			// dominated by per-prim float-vertex extraction (filter)
			// and a single tree walk (collapse).
			BuildFastFilter();
			BuildBVH4();

			t.stop();
			const unsigned int ms = (unsigned int)t.getInterval();

			// Tier C3: SAH-degradation log.  We don't *force* a rebuild
			// from inside Refit (BVH doesn't own the input prim list,
			// only the caller does) — instead we emit a warning at
			// 2.0× and let the caller decide based on
			// SAHDegradationRatio().
			const Scalar ratio = SAHDegradationRatio();
			if( ratio > 2.0 ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"BVH:: Refit %u nodes (%u prims) in %u ms — "
					"SAH degraded %.2fx vs original tree, caller may "
					"want to rebuild from polygon data",
					(unsigned)nodes.size(), (unsigned)prims.size(), ms,
					(double)ratio );
			} else {
				GlobalLog()->PrintEx( eLog_Info,
					"BVH:: Refit %u nodes (%u prims) in %u ms (SAH ratio %.2fx)",
					(unsigned)nodes.size(), (unsigned)prims.size(), ms,
					(double)ratio );
			}
			return ms;
		}

		//////////////////////////////////////////////////////////////////
		//  Serialization (Tier 1 §2 — `.risemesh` v3 BVH cache).
		//
		//  Writes the BVH2 representation only (nodes + DFS prim-index
		//  ordering).  BVH4 collapse and float-filter precompute are
		//  derived at load time from the BVH2 + the input prims, so the
		//  on-disk format stays compatible across Phase 3+ traversal
		//  changes.  See docs/BVH_ACCELERATION_PLAN.md §4.5 (deferred
		//  but partially-realised here for the Tier 1 scope).
		//
		//  Format:
		//    "BVH2"           4 bytes magic
		//    uint32 version   = 1
		//    uint32 numNodes
		//    Node[numNodes]   raw bytes (POD, 32 bytes each)
		//    uint32 numPrims
		//    uint32[numPrims] DFS-ordered indices into the caller's
		//                     original primitive list
		//    Scalar overallBox.ll[3], overallBox.ur[3]
		//
		//  primIdxFn maps each `prims[i]` back to its index in the
		//  caller's authoritative ordering (since BVH<Element>::prims
		//  is reordered by the SAH partition).  For
		//  TriangleMeshGeometryIndexed, `prims[i] - &ptr_polygons[0]`.
		//////////////////////////////////////////////////////////////////
		template< class IndexOf >
		void Serialize( IWriteBuffer& buffer, const IndexOf& primIdxFn ) const
		{
			buffer.setBytes( "BVH2", 4 );
			buffer.setUInt( 1u );  // version
			buffer.setUInt( (unsigned int)nodes.size() );
			if( !nodes.empty() ) {
				buffer.setBytes( nodes.data(),
					(unsigned int)( nodes.size() * sizeof( Node ) ) );
			}
			buffer.setUInt( (unsigned int)prims.size() );
			for( const Element& p : prims ) {
				buffer.setUInt( (unsigned int)primIdxFn( p ) );
			}
			buffer.setDouble( overallBox.ll.x );
			buffer.setDouble( overallBox.ll.y );
			buffer.setDouble( overallBox.ll.z );
			buffer.setDouble( overallBox.ur.x );
			buffer.setDouble( overallBox.ur.y );
			buffer.setDouble( overallBox.ur.z );
		}

		// Deserialize: returns true on success, false if magic mismatches
		// or version is unsupported.  `primAt(i)` should return the
		// caller's primitive at index i.  After a successful load,
		// the BVH4 collapse + filter precompute are re-run via the
		// post-build hooks (BuildBVH4 + BuildFastFilter), so a v3
		// `.risemesh` benefits from any improvement in those layers
		// without re-baking.
		template< class PrimAtFn >
		bool Deserialize( IReadBuffer& buffer, uint32_t numInputPrims, const PrimAtFn& primAt )
		{
			char magic[5] = {0};
			if( !buffer.getBytes( magic, 4 ) ) return false;
			if( magic[0]!='B' || magic[1]!='V' || magic[2]!='H' || magic[3]!='2' ) return false;

			const unsigned int ver = buffer.getUInt();
			if( ver != 1u ) return false;

			const unsigned int nNodes = buffer.getUInt();
			nodes.resize( nNodes );
			if( nNodes > 0 ) {
				if( !buffer.getBytes( nodes.data(), (unsigned int)( nNodes * sizeof( Node ) ) ) ) {
					nodes.clear();
					return false;
				}
			}

			const unsigned int nPrims = buffer.getUInt();
			prims.resize( nPrims );
			for( unsigned int i = 0; i < nPrims; ++i ) {
				const unsigned int idx = buffer.getUInt();
				if( idx >= numInputPrims ) {
					nodes.clear(); prims.clear();
					return false;
				}
				prims[i] = primAt( idx );
			}

			overallBox.ll.x = buffer.getDouble();
			overallBox.ll.y = buffer.getDouble();
			overallBox.ll.z = buffer.getDouble();
			overallBox.ur.x = buffer.getDouble();
			overallBox.ur.y = buffer.getDouble();
			overallBox.ur.z = buffer.getDouble();

			// Run post-build hooks (filter precompute + BVH4 collapse).
			BuildFastFilter();
			BuildBVH4();
			return true;
		}

		//////////////////////////////////////////////////////////////////
		//  BuildFastFilter: precompute float Möller-Trumbore edges for
		//  every leaf primitive that the element processor can supply
		//  vertices for.  Called once at construction after the main
		//  build (so prims[] is in its final reordered layout).
		//////////////////////////////////////////////////////////////////
		void BuildFastFilter()
		{
			if( prims.empty() ) {
				hasFastFilter = false;
				return;
			}

			// Probe the first prim — if processor doesn't supply float
			// vertices, the whole filter is disabled.
			float v0[3], v1[3], v2[3];
			if( !ep.GetFloatTriangleVertices( prims[0], v0, v1, v2 ) ) {
				hasFastFilter = false;
				return;
			}

			fastFilter.resize( prims.size() );
			for( size_t i = 0; i < prims.size(); ++i ) {
				if( !ep.GetFloatTriangleVertices( prims[i], v0, v1, v2 ) ) {
					// Heterogeneous primitive set (some triangle-like,
					// some not) — disable filter wholesale rather than
					// per-prim, simpler to reason about.
					fastFilter.clear();
					hasFastFilter = false;
					return;
				}
				TriangleFilterData& fd = fastFilter[i];
				fd.p0[0] = v0[0]; fd.p0[1] = v0[1]; fd.p0[2] = v0[2];
				fd.e1[0] = v1[0] - v0[0]; fd.e1[1] = v1[1] - v0[1]; fd.e1[2] = v1[2] - v0[2];
				fd.e2[0] = v2[0] - v0[0]; fd.e2[1] = v2[1] - v0[1]; fd.e2[2] = v2[2] - v0[2];
			}
			hasFastFilter = true;

			GlobalLog()->PrintEx( eLog_Info,
				"BVH:: Float filter enabled (%u triangles precomputed, %u bytes)",
				(unsigned)fastFilter.size(),
				(unsigned)( fastFilter.size() * sizeof( TriangleFilterData ) ) );
		}

		//////////////////////////////////////////////////////////////////
		//  BuildBVH4: collapse the BVH2 into a BVH4 by absorbing each
		//  internal child's two children into the parent slot.  Result:
		//  every BVH4 internal node has 2..4 children.  Empty child slots
		//  have bbox initialized to (+INF, -INF) so the ray-vs-AABB test
		//  always rejects without a length check.
		//
		//////////////////////////////////////////////////////////////////
		void BuildBVH4()
		{
			// Refit-safe: BuildBVH4 is called on every Refit() (after the
			// BVH2 nodes are updated in place), so we MUST start from an
			// empty nodes4.  Without this clear, push_back below appends
			// the new collapse to the trailing edge of the previous tree;
			// nodes4[0] then still points at the stale pre-refit root and
			// traversal silently intersects against old vertex bounds.
			// (Caught by adversarial review, 2026-04-27.)
			nodes4.clear();

			if( nodes.empty() ) {
				useBVH4 = false;
				return;
			}

			// Reserve generously — collapse roughly halves node count.
			nodes4.reserve( nodes.size() / 2 + 1 );
			nodes4.push_back( BVH4Node{} );
			InitEmptyBVH4Node( nodes4[0] );

			// If root is a leaf, BVH4 just has one leaf-only node.  We
			// represent it as a BVH4 node with one child = leaf.
			const Node& rootBVH2 = nodes[0];
			if( rootBVH2.primCount > 0 ) {
				FillBVH4ChildAsLeaf( nodes4[0], 0, rootBVH2 );
				nodes4[0].numChildren = 1;
				nodes4[0].splitAxis   = 0;
				useBVH4 = true;
				GlobalLog()->PrintEx( eLog_Info,
					"BVH:: BVH4 collapsed (%u BVH2 nodes -> %u BVH4 nodes, single leaf root)",
					(unsigned)nodes.size(), (unsigned)nodes4.size() );
				return;
			}

			CollapseRecursive( /*bvh4Idx=*/0, /*bvh2NodeIdx=*/0 );
			useBVH4 = true;

			GlobalLog()->PrintEx( eLog_Info,
				"BVH:: BVH4 collapsed (%u BVH2 nodes -> %u BVH4 nodes, %.1fx fewer)",
				(unsigned)nodes.size(), (unsigned)nodes4.size(),
				(float)nodes.size() / std::max<size_t>(1, nodes4.size()) );
		}

	protected:
		static inline void InitEmptyBVH4Node( BVH4Node& n )
		{
			for( int i = 0; i < 4; ++i ) {
				n.bboxMinX[i] =  FLT_MAX;
				n.bboxMinY[i] =  FLT_MAX;
				n.bboxMinZ[i] =  FLT_MAX;
				n.bboxMaxX[i] = -FLT_MAX;
				n.bboxMaxY[i] = -FLT_MAX;
				n.bboxMaxZ[i] = -FLT_MAX;
				n.children[i]  = -1;
				n.primCount[i] = 0;
			}
			n.numChildren = 0;
			n.splitAxis   = 0;
		}

		// Copy bbox from BVH2 source node into slot `i` of BVH4 destination.
		static inline void CopyBBoxToSlot( BVH4Node& dst, int i, const Node& src )
		{
			dst.bboxMinX[i] = src.bboxMin[0];
			dst.bboxMinY[i] = src.bboxMin[1];
			dst.bboxMinZ[i] = src.bboxMin[2];
			dst.bboxMaxX[i] = src.bboxMax[0];
			dst.bboxMaxY[i] = src.bboxMax[1];
			dst.bboxMaxZ[i] = src.bboxMax[2];
		}

		// Slot a BVH2 leaf into BVH4 child slot `i`.
		static inline void FillBVH4ChildAsLeaf( BVH4Node& dst, int i, const Node& src )
		{
			CopyBBoxToSlot( dst, i, src );
			dst.children[i]  = (int32_t)src.firstPrimOrLeft;
			dst.primCount[i] = src.primCount;
		}

		// Recursive collapse.  Caller has already allocated nodes4[bvh4Idx]
		// and initialized it via InitEmptyBVH4Node.  This populates that
		// node's children from the BVH2 subtree rooted at bvh2NodeIdx.
		void CollapseRecursive( uint32_t bvh4Idx, uint32_t bvh2NodeIdx )
		{
			const Node& n = nodes[bvh2NodeIdx];
			// Caller is responsible for ensuring n is internal.
			const uint32_t lIdx = n.firstPrimOrLeft;
			const uint32_t rIdx = lIdx + 1;
			const Node& l = nodes[lIdx];
			const Node& r = nodes[rIdx];

			// Decide candidates: try to absorb internal children's children.
			// Each candidate is either a BVH2 leaf (becomes a BVH4 leaf-child)
			// or a BVH2 internal (becomes a BVH4 internal-child via recursion).
			uint32_t candidateNodeIdx[4];
			int      numCandidates = 0;

			// Process L
			if( l.primCount > 0 ) {
				candidateNodeIdx[numCandidates++] = lIdx;
			} else {
				const uint32_t llIdx = l.firstPrimOrLeft;
				const uint32_t lrIdx = llIdx + 1;
				candidateNodeIdx[numCandidates++] = llIdx;
				candidateNodeIdx[numCandidates++] = lrIdx;
			}
			// Process R
			if( r.primCount > 0 ) {
				candidateNodeIdx[numCandidates++] = rIdx;
			} else {
				const uint32_t rlIdx = r.firstPrimOrLeft;
				const uint32_t rrIdx = rlIdx + 1;
				candidateNodeIdx[numCandidates++] = rlIdx;
				candidateNodeIdx[numCandidates++] = rrIdx;
			}

			// IMPORTANT: nodes4 grows via push_back inside this loop body
			// (when an internal child is encountered).  Each push_back may
			// reallocate the underlying buffer and invalidate any reference
			// previously taken into nodes4.  We use index-based access
			// (nodes4[bvh4Idx].field) throughout — never a long-lived
			// `BVH4Node&` reference — so a stale-reference bug cannot
			// hide.  Carry the splitAxis/numChildren state in locals,
			// then write through nodes4[bvh4Idx] freshly on each line.
			nodes4[bvh4Idx].numChildren = (uint8_t)numCandidates;
			nodes4[bvh4Idx].splitAxis   = n.splitAxis;

			for( int i = 0; i < numCandidates; ++i ) {
				const uint32_t bvh2Cand = candidateNodeIdx[i];
				const Node& cand = nodes[bvh2Cand];

				// Capture bbox into locals before any push_back to avoid
				// reading stale memory if the slot we'd write to is moved.
				const float bxn = cand.bboxMin[0], byn = cand.bboxMin[1], bzn = cand.bboxMin[2];
				const float bxp = cand.bboxMax[0], byp = cand.bboxMax[1], bzp = cand.bboxMax[2];

				if( cand.primCount > 0 ) {
					// Leaf child — no recursion, no further push_backs.
					nodes4[bvh4Idx].bboxMinX[i] = bxn;
					nodes4[bvh4Idx].bboxMinY[i] = byn;
					nodes4[bvh4Idx].bboxMinZ[i] = bzn;
					nodes4[bvh4Idx].bboxMaxX[i] = bxp;
					nodes4[bvh4Idx].bboxMaxY[i] = byp;
					nodes4[bvh4Idx].bboxMaxZ[i] = bzp;
					nodes4[bvh4Idx].children[i]  = (int32_t)cand.firstPrimOrLeft;
					nodes4[bvh4Idx].primCount[i] = cand.primCount;
				} else {
					// Internal child — allocate BVH4 slot first, then recurse.
					const uint32_t childBVH4Idx = (uint32_t)nodes4.size();
					nodes4.push_back( BVH4Node{} );           // may reallocate
					InitEmptyBVH4Node( nodes4.back() );
					CollapseRecursive( childBVH4Idx, bvh2Cand );  // grows nodes4 further

					// All push_backs are done for this slot — write through index.
					nodes4[bvh4Idx].bboxMinX[i] = bxn;
					nodes4[bvh4Idx].bboxMinY[i] = byn;
					nodes4[bvh4Idx].bboxMinZ[i] = bzn;
					nodes4[bvh4Idx].bboxMaxX[i] = bxp;
					nodes4[bvh4Idx].bboxMaxY[i] = byp;
					nodes4[bvh4Idx].bboxMaxZ[i] = bzp;
					nodes4[bvh4Idx].children[i]  = (int32_t)childBVH4Idx;
					nodes4[bvh4Idx].primCount[i] = 0;
				}
			}
		}

	public:
		//////////////////////////////////////////////////////////////////
		//  Phase 3 ray-vs-4-AABB SIMD kernel.  Returns hit-mask in low 4
		//  bits; tEntry[0..3] is the entry-t for each child (sentinel
		//  +INF for non-hit lanes so caller can sort without re-checking).
		//////////////////////////////////////////////////////////////////
		static inline uint32_t RayBox4(
			const float origin[3], const float invDir[3], float currentBest,
			const BVH4Node& n, float tEntry[4] )
		{
#if defined(RISE_BVH_HAVE_NEON)
			const float32x4_t ox = vdupq_n_f32( origin[0] );
			const float32x4_t oy = vdupq_n_f32( origin[1] );
			const float32x4_t oz = vdupq_n_f32( origin[2] );
			const float32x4_t idx = vdupq_n_f32( invDir[0] );
			const float32x4_t idy = vdupq_n_f32( invDir[1] );
			const float32x4_t idz = vdupq_n_f32( invDir[2] );

			const float32x4_t bminx = vld1q_f32( n.bboxMinX );
			const float32x4_t bminy = vld1q_f32( n.bboxMinY );
			const float32x4_t bminz = vld1q_f32( n.bboxMinZ );
			const float32x4_t bmaxx = vld1q_f32( n.bboxMaxX );
			const float32x4_t bmaxy = vld1q_f32( n.bboxMaxY );
			const float32x4_t bmaxz = vld1q_f32( n.bboxMaxZ );

			const float32x4_t t1x = vmulq_f32( vsubq_f32( bminx, ox ), idx );
			const float32x4_t t2x = vmulq_f32( vsubq_f32( bmaxx, ox ), idx );
			const float32x4_t t1y = vmulq_f32( vsubq_f32( bminy, oy ), idy );
			const float32x4_t t2y = vmulq_f32( vsubq_f32( bmaxy, oy ), idy );
			const float32x4_t t1z = vmulq_f32( vsubq_f32( bminz, oz ), idz );
			const float32x4_t t2z = vmulq_f32( vsubq_f32( bmaxz, oz ), idz );

			float32x4_t tmin = vmaxq_f32( vmaxq_f32( vminq_f32( t1x, t2x ),
			                                          vminq_f32( t1y, t2y ) ),
			                              vminq_f32( t1z, t2z ) );
			float32x4_t tmax = vminq_f32( vminq_f32( vmaxq_f32( t1x, t2x ),
			                                          vmaxq_f32( t1y, t2y ) ),
			                              vmaxq_f32( t1z, t2z ) );

			tmin = vmaxq_f32( tmin, vdupq_n_f32( 0.0f ) );
			tmax = vminq_f32( tmax, vdupq_n_f32( currentBest ) );

			const uint32x4_t hitMask = vcleq_f32( tmin, tmax );

			// Replace non-hit lanes' tmin with +INF so caller can sort
			// without re-consulting the mask.
			const float32x4_t tInf  = vdupq_n_f32( FLT_MAX );
			const float32x4_t tOut  = vbslq_f32( hitMask, tmin, tInf );
			vst1q_f32( tEntry, tOut );

			// movemask emulation: AND with bit-per-lane mask then horizontal-add.
			static const uint32_t bitsArr[4] = { 1u, 2u, 4u, 8u };
			const uint32x4_t bits = vandq_u32( hitMask, vld1q_u32( bitsArr ) );
			return vaddvq_u32( bits );

#elif defined(RISE_BVH_HAVE_SSE)
			const __m128 ox = _mm_set1_ps( origin[0] );
			const __m128 oy = _mm_set1_ps( origin[1] );
			const __m128 oz = _mm_set1_ps( origin[2] );
			const __m128 idx = _mm_set1_ps( invDir[0] );
			const __m128 idy = _mm_set1_ps( invDir[1] );
			const __m128 idz = _mm_set1_ps( invDir[2] );

			const __m128 bminx = _mm_loadu_ps( n.bboxMinX );
			const __m128 bminy = _mm_loadu_ps( n.bboxMinY );
			const __m128 bminz = _mm_loadu_ps( n.bboxMinZ );
			const __m128 bmaxx = _mm_loadu_ps( n.bboxMaxX );
			const __m128 bmaxy = _mm_loadu_ps( n.bboxMaxY );
			const __m128 bmaxz = _mm_loadu_ps( n.bboxMaxZ );

			const __m128 t1x = _mm_mul_ps( _mm_sub_ps( bminx, ox ), idx );
			const __m128 t2x = _mm_mul_ps( _mm_sub_ps( bmaxx, ox ), idx );
			const __m128 t1y = _mm_mul_ps( _mm_sub_ps( bminy, oy ), idy );
			const __m128 t2y = _mm_mul_ps( _mm_sub_ps( bmaxy, oy ), idy );
			const __m128 t1z = _mm_mul_ps( _mm_sub_ps( bminz, oz ), idz );
			const __m128 t2z = _mm_mul_ps( _mm_sub_ps( bmaxz, oz ), idz );

			__m128 tmin = _mm_max_ps( _mm_max_ps( _mm_min_ps( t1x, t2x ),
			                                      _mm_min_ps( t1y, t2y ) ),
			                          _mm_min_ps( t1z, t2z ) );
			__m128 tmax = _mm_min_ps( _mm_min_ps( _mm_max_ps( t1x, t2x ),
			                                      _mm_max_ps( t1y, t2y ) ),
			                          _mm_max_ps( t1z, t2z ) );

			tmin = _mm_max_ps( tmin, _mm_setzero_ps() );
			tmax = _mm_min_ps( tmax, _mm_set1_ps( currentBest ) );

			const __m128 hitMaskF = _mm_cmple_ps( tmin, tmax );
			const __m128 tInf     = _mm_set1_ps( FLT_MAX );
			const __m128 tOut     = _mm_or_ps( _mm_and_ps( hitMaskF, tmin ),
			                                    _mm_andnot_ps( hitMaskF, tInf ) );
			_mm_storeu_ps( tEntry, tOut );
			return (uint32_t)_mm_movemask_ps( hitMaskF ) & 0xF;

#else
			// Scalar fallback — same arithmetic, four times.
			uint32_t mask = 0;
			for( int i = 0; i < 4; ++i ) {
				const float t1x = ( n.bboxMinX[i] - origin[0] ) * invDir[0];
				const float t2x = ( n.bboxMaxX[i] - origin[0] ) * invDir[0];
				const float t1y = ( n.bboxMinY[i] - origin[1] ) * invDir[1];
				const float t2y = ( n.bboxMaxY[i] - origin[1] ) * invDir[1];
				const float t1z = ( n.bboxMinZ[i] - origin[2] ) * invDir[2];
				const float t2z = ( n.bboxMaxZ[i] - origin[2] ) * invDir[2];
				float tmin = std::fmax( std::fmax( std::fmin( t1x, t2x ),
				                                    std::fmin( t1y, t2y ) ),
				                        std::fmin( t1z, t2z ) );
				float tmax = std::fmin( std::fmin( std::fmax( t1x, t2x ),
				                                    std::fmax( t1y, t2y ) ),
				                        std::fmax( t1z, t2z ) );
				tmin = std::fmax( tmin, 0.0f );
				tmax = std::fmin( tmax, currentBest );
				if( tmin <= tmax ) {
					tEntry[i] = tmin;
					mask |= ( 1u << i );
				} else {
					tEntry[i] = FLT_MAX;
				}
			}
			return mask;
#endif
		}

	protected:

		//////////////////////////////////////////////////////////////////
		//  Build: recursive top-down SAH with binning.
		//////////////////////////////////////////////////////////////////
		void Build( const std::vector<Element>& inputPrims )
		{
			Timer t; t.start();

			GlobalLog()->PrintEx( eLog_Info,
				"BVH:: Building over %u primitives (binCount=%u, maxLeafSize=%u)",
				(unsigned)inputPrims.size(), cfg.binCount, cfg.maxLeafSize );

			prims = inputPrims;
			nodes.clear();
			if( prims.empty() ) {
				return;
			}

			// Precompute per-primitive AABB and centroid.  These are
			// touched O(log N) times during build by partition-around-bin
			// — caching them avoids re-reading vertices through the
			// processor on every visit.
			std::vector<BoundingBox> primBox( prims.size() );
			std::vector<Point3>      primCentroid( prims.size() );
			BoundingBox  rootBox(  Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
			                       Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
			BoundingBox  rootCentroidBox(
			                       Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
			                       Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
			for( size_t i = 0; i < prims.size(); ++i ) {
				BoundingBox b = ep.GetElementBoundingBox( prims[i] );
				primBox[i]      = b;
				// Centroid = midpoint of bbox.  Done component-wise — no
				// existing op for "midpoint between two Point3" in this codebase.
				primCentroid[i] = Point3( ( b.ll.x + b.ur.x ) * 0.5,
				                          ( b.ll.y + b.ur.y ) * 0.5,
				                          ( b.ll.z + b.ur.z ) * 0.5 );
				rootBox.Include( b.ll );
				rootBox.Include( b.ur );
				rootCentroidBox.Include( primCentroid[i] );
			}

			// If caller passed a non-degenerate hint, prefer the actual
			// computed root box (it's tighter than what the caller may
			// know).  Either way we now own the authoritative bounds.
			overallBox = rootBox;

			// Reserve generously — typical BVH has ~2N-1 nodes.
			nodes.reserve( prims.size() * 2 );
			nodes.push_back( Node{} );  // root at index 0

			// Index array we partition in place during build.
			std::vector<uint32_t> idx( prims.size() );
			for( uint32_t i = 0; i < prims.size(); ++i ) idx[i] = i;

			BuildRecursive( 0, idx, 0, (uint32_t)prims.size(),
			                primBox, primCentroid, rootBox, rootCentroidBox );

			// Reorder prims by the now-finalized index array.  After
			// this, leaves' [firstPrimOrLeft, firstPrimOrLeft+primCount)
			// range indexes directly into `prims`.
			std::vector<Element> reordered( prims.size() );
			for( size_t i = 0; i < idx.size(); ++i ) {
				reordered[i] = prims[ idx[i] ];
			}
			prims.swap( reordered );

			t.stop();
			GlobalLog()->PrintEx( eLog_Info,
				"BVH:: Built %u nodes (%u leaves) in %u ms",
				(unsigned)nodes.size(),
				(unsigned)CountLeaves(),
				(unsigned)t.getInterval() );
		}

	protected:
		// Single bin in SAH binning.
		struct Bin
		{
			BoundingBox box;
			uint32_t    count;
			Bin()
				: box( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				       Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) )
				, count( 0 )
			{}
		};

		// Recursively build the subtree rooted at `nodeIdx` for the
		// primitives in idx[first..last).  centroidBox bounds those
		// centroids (used to drive bin placement).  primBox bounds
		// the primitives themselves (used for leaf AABB).
		void BuildRecursive(
			uint32_t                       nodeIdx,
			std::vector<uint32_t>&         idx,
			uint32_t                       first,
			uint32_t                       last,
			const std::vector<BoundingBox>& primBox,
			const std::vector<Point3>&      primCentroid,
			const BoundingBox&              parentBox,
			const BoundingBox&              centroidBox )
		{
			const uint32_t count = last - first;

			// Tighten parent box to the actual covered primitives.
			BoundingBox tightBox(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
			for( uint32_t i = first; i < last; ++i ) {
				const BoundingBox& b = primBox[ idx[i] ];
				tightBox.Include( b.ll );
				tightBox.Include( b.ur );
			}
			(void)parentBox;
			SetNodeBox( nodes[nodeIdx], tightBox );

			// Leaf-termination conditions.
			if( count <= cfg.maxLeafSize ) {
				MakeLeaf( nodeIdx, first, count );
				return;
			}

			// Pick split axis = longest centroid-box extent.
			Vector3 cExt = Vector3Ops::mkVector3( centroidBox.ur, centroidBox.ll );
			uint8_t axis = 0;
			if( cExt.y > cExt.x ) axis = 1;
			if( cExt.z > ((axis == 0) ? cExt.x : cExt.y) ) axis = 2;
			const Scalar cMin = AxisVal( centroidBox.ll, axis );
			const Scalar cMax = AxisVal( centroidBox.ur, axis );

			// Degenerate centroid box: all centroids coincide on this
			// axis — no useful split.  Fall back to a leaf.
			if( cMax - cMin < 1e-12 ) {
				MakeLeaf( nodeIdx, first, count );
				return;
			}

			// SAH binning.
			const uint32_t B = cfg.binCount;
			std::vector<Bin> bins( B );
			const Scalar invSpan = (Scalar)B / (cMax - cMin);

			for( uint32_t i = first; i < last; ++i ) {
				const Scalar c   = AxisVal( primCentroid[ idx[i] ], axis );
				int          bid = (int)( (c - cMin) * invSpan );
				if( bid < 0 )      bid = 0;
				if( bid >= (int)B ) bid = B - 1;
				bins[bid].count++;
				const BoundingBox& b = primBox[ idx[i] ];
				bins[bid].box.Include( b.ll );
				bins[bid].box.Include( b.ur );
			}

			// Sweep left-to-right and right-to-left to compute prefix
			// AABBs and counts at each potential split boundary.
			std::vector<BoundingBox> leftBox( B - 1, BoundingBox(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) ) );
			std::vector<BoundingBox> rightBox( B - 1, BoundingBox(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) ) );
			std::vector<uint32_t> leftCount( B - 1, 0 );
			std::vector<uint32_t> rightCount( B - 1, 0 );

			BoundingBox accBox(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
			uint32_t accCount = 0;
			for( uint32_t i = 0; i + 1 < B; ++i ) {
				// Empty-bin guard: an empty bin still has its sentinel-init bbox
				// of (INF, -INF) — including those values into accBox would
				// corrupt the prefix to span the universe and drive every
				// split cost to infinity.  Skip empty bins for bbox accumulation
				// (their count is 0 anyway, no contribution).
				if( bins[i].count > 0 ) {
					accBox.Include( bins[i].box.ll );
					accBox.Include( bins[i].box.ur );
				}
				accCount += bins[i].count;
				leftBox[i]   = accBox;
				leftCount[i] = accCount;
			}
			accBox = BoundingBox(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
			accCount = 0;
			for( int i = (int)B - 1; i >= 1; --i ) {
				if( bins[i].count > 0 ) {
					accBox.Include( bins[i].box.ll );
					accBox.Include( bins[i].box.ur );
				}
				accCount += bins[i].count;
				rightBox[i - 1]   = accBox;
				rightCount[i - 1] = accCount;
			}

			// Find best split by SAH cost.
			const Scalar parentArea = SurfaceArea( tightBox );
			const Scalar leafCost   = cfg.sahIntersectionCost * (Scalar)count;
			Scalar       bestCost   = leafCost;
			int          bestSplit  = -1;
			for( uint32_t i = 0; i + 1 < B; ++i ) {
				if( leftCount[i] == 0 || rightCount[i] == 0 ) continue;
				const Scalar cost =
					cfg.sahTraversalCost +
					cfg.sahIntersectionCost * (
						(SurfaceArea( leftBox[i] )  * (Scalar)leftCount[i]) +
						(SurfaceArea( rightBox[i] ) * (Scalar)rightCount[i])
					) / parentArea;
				if( cost < bestCost ) {
					bestCost  = cost;
					bestSplit = (int)i;
				}
			}

			// SAH says splitting is worse than a leaf — accept the leaf.
			if( bestSplit < 0 ) {
				MakeLeaf( nodeIdx, first, count );
				return;
			}

			// Partition idx[first..last) by bin.
			const Scalar splitVal = cMin + (cMax - cMin) * (Scalar)( bestSplit + 1 ) / (Scalar)B;
			uint32_t mid = first;
			for( uint32_t i = first; i < last; ++i ) {
				const Scalar c = AxisVal( primCentroid[ idx[i] ], axis );
				if( c < splitVal ) {
					std::swap( idx[i], idx[mid] );
					++mid;
				}
			}

			// Edge case: partitioning collapsed to one side (e.g.
			// exactly-on-boundary primitives).  Fall back to median.
			if( mid == first || mid == last ) {
				mid = first + count / 2;
				std::nth_element(
					idx.begin() + first,
					idx.begin() + mid,
					idx.begin() + last,
					[&]( uint32_t a, uint32_t b ){
						return AxisVal( primCentroid[a], axis ) <
						       AxisVal( primCentroid[b], axis );
					});
			}

			// Recompute child centroid boxes for the recursive call.
			BoundingBox leftCBox(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
			BoundingBox rightCBox(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
			for( uint32_t i = first; i < mid; ++i ) leftCBox.Include(  primCentroid[ idx[i] ] );
			for( uint32_t i = mid;   i < last; ++i ) rightCBox.Include( primCentroid[ idx[i] ] );

			// Allocate two children, contiguous (Node[leftIdx], Node[leftIdx+1]).
			const uint32_t leftIdx = (uint32_t)nodes.size();
			nodes.push_back( Node{} );
			nodes.push_back( Node{} );
			nodes[nodeIdx].firstPrimOrLeft = leftIdx;
			nodes[nodeIdx].primCount       = 0;
			nodes[nodeIdx].splitAxis       = axis;

			BuildRecursive( leftIdx,     idx, first, mid,  primBox, primCentroid, tightBox, leftCBox );
			BuildRecursive( leftIdx + 1, idx, mid,   last, primBox, primCentroid, tightBox, rightCBox );
		}

		void MakeLeaf( uint32_t nodeIdx, uint32_t first, uint32_t count )
		{
			nodes[nodeIdx].firstPrimOrLeft = first;
			nodes[nodeIdx].primCount       = (uint16_t)count;
			nodes[nodeIdx].splitAxis       = 0;
		}
		uint32_t CountLeaves() const
		{
			uint32_t n = 0;
			for( const Node& node : nodes ) if( node.primCount != 0 ) ++n;
			return n;
		}

		// Tier C3: SAH cost of the current tree, normalised by root-bbox
		// surface area.  See SAHDegradationRatio() for rationale.
		// Single O(N) walk; the typical caller invokes this after Refit
		// where the AABB walk has already warmed the cache, so the
		// amortised cost is small.  Empty tree returns 0.
		Scalar ComputeSAH() const
		{
			if( nodes.empty() ) return 0;
			BoundingBox rootBox(
				Point3( nodes[0].bboxMin[0], nodes[0].bboxMin[1], nodes[0].bboxMin[2] ),
				Point3( nodes[0].bboxMax[0], nodes[0].bboxMax[1], nodes[0].bboxMax[2] ) );
			const Scalar rootArea = SurfaceArea( rootBox );
			if( rootArea <= 0 ) return 0;

			Scalar cost = 0;
			for( const Node& n : nodes ) {
				BoundingBox nb(
					Point3( n.bboxMin[0], n.bboxMin[1], n.bboxMin[2] ),
					Point3( n.bboxMax[0], n.bboxMax[1], n.bboxMax[2] ) );
				const Scalar a = SurfaceArea( nb );
				if( n.primCount > 0 ) {
					cost += cfg.sahIntersectionCost * (Scalar)n.primCount * a;
				} else {
					cost += cfg.sahTraversalCost * a;
				}
			}
			return cost / rootArea;
		}

		static inline Scalar AxisVal( const Point3& p, uint8_t axis )
		{
			return ( axis == 0 ) ? p.x : ( axis == 1 ) ? p.y : p.z;
		}

		static inline Scalar SurfaceArea( const BoundingBox& b )
		{
			Vector3 e = Vector3Ops::mkVector3( b.ur, b.ll );
			if( e.x < 0 || e.y < 0 || e.z < 0 ) return 0.0;
			return 2.0 * ( e.x * e.y + e.y * e.z + e.z * e.x );
		}

		// Conservative AABB store: round outward by 1 ULP in each
		// component.  Guarantees the float box contains the true
		// double box, so the float traversal can never reject a hit
		// the double intersection would otherwise have caught.
		static inline void SetNodeBox( Node& n, const BoundingBox& b )
		{
			n.bboxMin[0] = std::nextafter( (float)b.ll.x, -FLT_MAX );
			n.bboxMin[1] = std::nextafter( (float)b.ll.y, -FLT_MAX );
			n.bboxMin[2] = std::nextafter( (float)b.ll.z, -FLT_MAX );
			n.bboxMax[0] = std::nextafter( (float)b.ur.x,  FLT_MAX );
			n.bboxMax[1] = std::nextafter( (float)b.ur.y,  FLT_MAX );
			n.bboxMax[2] = std::nextafter( (float)b.ur.z,  FLT_MAX );
		}

		// Phase 2 float Möller-Trumbore filter.  Conservative — pads
		// barycentric checks by EPS so near-edge hits pass through to
		// the double-precision certifier rather than being silently
		// rejected on float roundoff.  Returns true on potential hit
		// with a float t estimate; double-precision certifier (the
		// production RayElementIntersection) computes the authoritative
		// hit info if the filter approves and t is competitive.
		static inline bool MollerTrumboreFloat(
			const float origin[3],
			const float dir[3],
			const TriangleFilterData& f,
			float       currentBest,
			float&      tOut )
		{
			// Conservative epsilon: 1e-5 relative.  This covers most
			// double→float vertex roundoff (~1 ULP at fp32) plus the
			// barycentric arithmetic loss across cross-product chains.
			constexpr float EPS = 1e-5f;

			// pvec = dir × e2
			const float pvec[3] = {
				dir[1] * f.e2[2] - dir[2] * f.e2[1],
				dir[2] * f.e2[0] - dir[0] * f.e2[2],
				dir[0] * f.e2[1] - dir[1] * f.e2[0]
			};

			const float det = f.e1[0] * pvec[0] + f.e1[1] * pvec[1] + f.e1[2] * pvec[2];

			// Parallel-ray rejection.  Conservative threshold — but if a
			// ray is THAT close to parallel, the double-precision path
			// would also struggle and any "hit" would be an edge graze.
			if( std::fabs( det ) < 1e-20f ) return false;

			const float invDet = 1.0f / det;

			// tvec = origin - p0
			const float tvec[3] = {
				origin[0] - f.p0[0],
				origin[1] - f.p0[1],
				origin[2] - f.p0[2]
			};

			const float u = ( tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2] ) * invDet;
			if( u < -EPS || u > 1.0f + EPS ) return false;

			// qvec = tvec × e1
			const float qvec[3] = {
				tvec[1] * f.e1[2] - tvec[2] * f.e1[1],
				tvec[2] * f.e1[0] - tvec[0] * f.e1[2],
				tvec[0] * f.e1[1] - tvec[1] * f.e1[0]
			};

			const float v = ( dir[0] * qvec[0] + dir[1] * qvec[1] + dir[2] * qvec[2] ) * invDet;
			if( v < -EPS || u + v > 1.0f + EPS ) return false;

			const float t = ( f.e2[0] * qvec[0] + f.e2[1] * qvec[1] + f.e2[2] * qvec[2] ) * invDet;
			// Reject negative-t (behind ray) and beyond-currentBest hits.
			// Pad with same EPS so a triangle on the very edge of the
			// "useful" range passes through to certify.
			if( t < -EPS || t > currentBest + EPS ) return false;

			tOut = t;
			return true;
		}

		// Float ray-vs-AABB slab test.  Returns true if ray enters the
		// box within (0, currentBest], and writes the entry t to tEntry.
		// Standard branchless slab; uses 1/dir cached by caller.
		static inline bool RayBoxF(
			const float origin[3],
			const float invDir[3],
			float       currentBest,
			const float bboxMin[3],
			const float bboxMax[3],
			float&      tEntry )
		{
			float t1 = ( bboxMin[0] - origin[0] ) * invDir[0];
			float t2 = ( bboxMax[0] - origin[0] ) * invDir[0];
			float tmin = std::fmin( t1, t2 );
			float tmax = std::fmax( t1, t2 );

			t1 = ( bboxMin[1] - origin[1] ) * invDir[1];
			t2 = ( bboxMax[1] - origin[1] ) * invDir[1];
			tmin = std::fmax( tmin, std::fmin( t1, t2 ) );
			tmax = std::fmin( tmax, std::fmax( t1, t2 ) );

			t1 = ( bboxMin[2] - origin[2] ) * invDir[2];
			t2 = ( bboxMax[2] - origin[2] ) * invDir[2];
			tmin = std::fmax( tmin, std::fmin( t1, t2 ) );
			tmax = std::fmin( tmax, std::fmax( t1, t2 ) );

			if( tmax < 0.0f )         return false;
			if( tmin > currentBest )  return false;
			if( tmin > tmax )         return false;
			tEntry = std::fmax( tmin, 0.0f );
			return true;
		}

	public:
		//////////////////////////////////////////////////////////////////
		//  Traversal: single-ray, stack-based, ordered.  Phase 3
		//  dispatches to BVH4 SIMD kernel when collapse succeeded
		//  (useBVH4 == true).  Otherwise falls back to BVH2 traversal.
		//////////////////////////////////////////////////////////////////

	protected:
		// Helper: float ray data for traversal.  Caller passes origin/dir
		// in double; we cache in float and compute invDir.
		static inline void PrepRayFloat(
			const Ray& ray, float origin[3], float dir[3], float invDir[3] )
		{
			origin[0] = (float)ray.origin.x;
			origin[1] = (float)ray.origin.y;
			origin[2] = (float)ray.origin.z;
			dir[0] = (float)ray.Dir().x;
			dir[1] = (float)ray.Dir().y;
			dir[2] = (float)ray.Dir().z;
			for( int k = 0; k < 3; ++k ) {
				if( dir[k] == 0.0f ) invDir[k] = ( dir[k] >= 0 ) ?  FLT_MAX : -FLT_MAX;
				else                  invDir[k] = 1.0f / dir[k];
			}
		}

		// Phase 3 BVH4 leaf intersection — same closest-hit + filter
		// pattern as BVH2 leaves.  Inlined to avoid duplicating the
		// 3-overload boilerplate.
		// Cleanup §2: with the closest-hit guard now native to
		// TriangleMeshGeometry{,Indexed}::RayElementIntersection, the
		// per-element myRI copy/compare workaround is no longer needed.
		// Leaf intersection just calls the processor directly with `ri`
		// — saves the RayIntersectionGeometric/RayIntersection copy
		// (~120 bytes ea on 64-bit doubles) per leaf-prim test.

		inline void Bvh4Leaf_Geometric(
			RayIntersectionGeometric& ri,
			uint32_t firstPrim, uint32_t primCnt,
			const float origin[3], const float dir[3],
			bool bHitFrontFaces, bool bHitBackFaces ) const
		{
			const uint32_t end = firstPrim + primCnt;
			for( uint32_t i = firstPrim; i < end; ++i ) {
				if( hasFastFilter ) {
					float fT;
					if( !MollerTrumboreFloat( origin, dir, fastFilter[i],
					                          (float)ri.range, fT ) ) {
						continue;
					}
				}
				ep.RayElementIntersection( ri, prims[i],
				                           bHitFrontFaces, bHitBackFaces );
			}
		}

		inline void Bvh4Leaf_Full(
			RayIntersection& ri,
			uint32_t firstPrim, uint32_t primCnt,
			const float origin[3], const float dir[3],
			bool bHitFrontFaces, bool bHitBackFaces, bool bComputeExitInfo ) const
		{
			const uint32_t end = firstPrim + primCnt;
			for( uint32_t i = firstPrim; i < end; ++i ) {
				if( hasFastFilter ) {
					float fT;
					if( !MollerTrumboreFloat( origin, dir, fastFilter[i],
					                          (float)ri.geometric.range, fT ) ) {
						continue;
					}
				}
				ep.RayElementIntersection( ri, prims[i],
				                           bHitFrontFaces, bHitBackFaces,
				                           bComputeExitInfo );
			}
		}

		inline bool Bvh4Leaf_IntersectionOnly(
			const Ray& ray, Scalar dHowFar,
			uint32_t firstPrim, uint32_t primCnt,
			const float origin[3], const float dir[3],
			bool bHitFrontFaces, bool bHitBackFaces ) const
		{
			const uint32_t end = firstPrim + primCnt;
			const float currentBest = (float)dHowFar;
			for( uint32_t i = firstPrim; i < end; ++i ) {
				if( hasFastFilter ) {
					float fT;
					if( !MollerTrumboreFloat( origin, dir, fastFilter[i],
					                          currentBest, fT ) ) {
						continue;
					}
				}
				if( ep.RayElementIntersection_IntersectionOnly(
				        ray, dHowFar, prims[i],
				        bHitFrontFaces, bHitBackFaces ) ) {
					return true;
				}
			}
			return false;
		}

		// BVH4 traversal — geometric (closest hit, no exit info).
		void IntersectRay4(
			RayIntersectionGeometric& ri,
			bool bHitFrontFaces, bool bHitBackFaces ) const
		{
			float origin[3], dir[3], invDir[3];
			PrepRayFloat( ri.ray, origin, dir, invDir );

			// Bounded dynamic stack.  A fixed-size `stack[64]` was the
			// original here, on the assumption that BVH4 collapse halves
			// tree depth — but adversarial / clustered geometry with
			// 100M+ tris can push past that, and BVH4 pushes up to 3
			// deferred siblings per visit (so max-stack ≈ 3×depth).
			// `thread_local` keeps allocations to one warm-up grow per
			// worker thread, then steady-state zero heap traffic.
			// (Caught by adversarial review, 2026-04-27.)
			static thread_local std::vector<uint32_t> stack;
			stack.clear();
			stack.push_back( 0 );

			while( !stack.empty() ) {
				const uint32_t ni = stack.back();
				stack.pop_back();
				const BVH4Node& n = nodes4[ni];

				float tEntry[4];
				const float currentBest = (float)ri.range;
				const uint32_t rawMask = RayBox4( origin, invDir, currentBest, n, tEntry );
				// Mask off unused child slots — sentinel bboxes
				// (+INF, -INF) don't structurally fail the slab test.
				const uint32_t validMask = ( 1u << n.numChildren ) - 1u;
				const uint32_t mask      = rawMask & validMask;
				if( mask == 0 ) continue;

				// Process hit children.  For correctness, order doesn't
				// matter (closest-hit is checked at leaf intersection
				// against ri.range).  We do a simple ordered push: walk
				// children far-to-near so near pops first.  Index sort
				// of up to 4 entries — quick selection sort.
				int hitOrder[4];
				int numHits = 0;
				for( int i = 0; i < 4; ++i ) {
					if( mask & ( 1u << i ) ) hitOrder[numHits++] = i;
				}
				// Selection sort by tEntry descending (so smaller-t pops first).
				for( int a = 0; a < numHits - 1; ++a ) {
					int maxIdx = a;
					for( int b = a + 1; b < numHits; ++b ) {
						if( tEntry[ hitOrder[b] ] > tEntry[ hitOrder[maxIdx] ] ) maxIdx = b;
					}
					if( maxIdx != a ) std::swap( hitOrder[a], hitOrder[maxIdx] );
				}

				for( int h = 0; h < numHits; ++h ) {
					const int i = hitOrder[h];
					if( n.primCount[i] > 0 ) {
						// Leaf child — intersect immediately (no stack push).
						Bvh4Leaf_Geometric(
							ri,
							(uint32_t)n.children[i], (uint32_t)n.primCount[i],
							origin, dir,
							bHitFrontFaces, bHitBackFaces );
					} else {
						stack.push_back( (uint32_t)n.children[i] );
					}
				}
			}
		}

		// BVH4 traversal — full (closest hit + exit info).
		void IntersectRay4(
			RayIntersection& ri,
			bool bHitFrontFaces, bool bHitBackFaces, bool bComputeExitInfo ) const
		{
			float origin[3], dir[3], invDir[3];
			PrepRayFloat( ri.geometric.ray, origin, dir, invDir );

			// Bounded dynamic stack — see geometric overload above for
			// rationale.
			static thread_local std::vector<uint32_t> stack;
			stack.clear();
			stack.push_back( 0 );

			while( !stack.empty() ) {
				const uint32_t ni = stack.back();
				stack.pop_back();
				const BVH4Node& n = nodes4[ni];

				float tEntry[4];
				const float currentBest = (float)ri.geometric.range;
				const uint32_t rawMask = RayBox4( origin, invDir, currentBest, n, tEntry );
				const uint32_t validMask = ( 1u << n.numChildren ) - 1u;
				const uint32_t mask      = rawMask & validMask;
				if( mask == 0 ) continue;

				int hitOrder[4];
				int numHits = 0;
				for( int i = 0; i < 4; ++i ) {
					if( mask & ( 1u << i ) ) hitOrder[numHits++] = i;
				}
				for( int a = 0; a < numHits - 1; ++a ) {
					int maxIdx = a;
					for( int b = a + 1; b < numHits; ++b ) {
						if( tEntry[ hitOrder[b] ] > tEntry[ hitOrder[maxIdx] ] ) maxIdx = b;
					}
					if( maxIdx != a ) std::swap( hitOrder[a], hitOrder[maxIdx] );
				}

				for( int h = 0; h < numHits; ++h ) {
					const int i = hitOrder[h];
					if( n.primCount[i] > 0 ) {
						Bvh4Leaf_Full(
							ri,
							(uint32_t)n.children[i], (uint32_t)n.primCount[i],
							origin, dir,
							bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
					} else {
						stack.push_back( (uint32_t)n.children[i] );
					}
				}
			}
		}

		// BVH4 traversal — any-hit (returns on first hit within dHowFar).
		bool IntersectRay4_IntersectionOnly(
			const Ray& ray, Scalar dHowFar,
			bool bHitFrontFaces, bool bHitBackFaces ) const
		{
			float origin[3], dir[3], invDir[3];
			PrepRayFloat( ray, origin, dir, invDir );

			// Bounded dynamic stack — see geometric overload above for
			// rationale.
			static thread_local std::vector<uint32_t> stack;
			stack.clear();
			stack.push_back( 0 );
			const float currentBest = (float)dHowFar;

			while( !stack.empty() ) {
				const uint32_t ni = stack.back();
				stack.pop_back();
				const BVH4Node& n = nodes4[ni];

				float tEntry[4];
				const uint32_t rawMask = RayBox4( origin, invDir, currentBest, n, tEntry );
				const uint32_t validMask = ( 1u << n.numChildren ) - 1u;
				const uint32_t mask      = rawMask & validMask;
				if( mask == 0 ) continue;

				// Order doesn't matter for any-hit; just iterate hit
				// children in slot order.  Push internals, intersect
				// leaves immediately so we can early-exit.
				for( int i = 0; i < 4; ++i ) {
					if( !( mask & ( 1u << i ) ) ) continue;
					if( n.primCount[i] > 0 ) {
						if( Bvh4Leaf_IntersectionOnly(
								ray, dHowFar,
								(uint32_t)n.children[i], (uint32_t)n.primCount[i],
								origin, dir,
								bHitFrontFaces, bHitBackFaces ) ) {
							return true;
						}
					} else {
						stack.push_back( (uint32_t)n.children[i] );
					}
				}
			}
			return false;
		}

	public:
		// Full intersection (writes hit info on success).
		void IntersectRay(
			RayIntersectionGeometric& ri,
			bool                      bHitFrontFaces,
			bool                      bHitBackFaces ) const
		{
			if( useBVH4 && !nodes4.empty() ) {
				IntersectRay4( ri, bHitFrontFaces, bHitBackFaces );
				return;
			}
			if( nodes.empty() ) return;

			// Cache float-precision ray data for traversal.  Intersection
			// at leaves runs in Scalar against the original ri.ray.
			const float origin[3] = {
				(float)ri.ray.origin.x,
				(float)ri.ray.origin.y,
				(float)ri.ray.origin.z };
			float dir[3] = {
				(float)ri.ray.Dir().x,
				(float)ri.ray.Dir().y,
				(float)ri.ray.Dir().z };
			float invDir[3];
			for( int k = 0; k < 3; ++k ) {
				// Avoid division by zero — IEEE inf works for the slab
				// test (the comparisons just go the right way), but
				// some platforms trap on 0 division at fp32; we map to
				// FLT_MAX with the original sign instead.
				if( dir[k] == 0.0f ) invDir[k] = ( dir[k] >= 0 ) ?  FLT_MAX : -FLT_MAX;
				else                  invDir[k] = 1.0f / dir[k];
			}

			// Bounded dynamic stack — see IntersectRay4 (BVH4 path) above
			// for full rationale.  thread_local keeps allocations to one
			// warm-up grow per worker thread.
			static thread_local std::vector<uint32_t> stack;
			stack.clear();
			stack.push_back( 0 );

			while( !stack.empty() ) {
				const uint32_t  ni   = stack.back();
				stack.pop_back();
				const Node&     node = nodes[ni];

				// Ray-vs-this-node AABB.  Skip if no hit, or if the
				// closest hit so far is already in front of the box.
				const float currentBest = (float)ri.range;
				float tEntry;
				if( !RayBoxF( origin, invDir, currentBest,
				              node.bboxMin, node.bboxMax, tEntry ) )
					continue;

				if( node.primCount > 0 ) {
					// Leaf — call the production element-processor directly.
					// Cleanup §1+§2: closest-hit guard is now native in
					// TriangleMeshGeometry{,Indexed}::RayElementIntersection,
					// so the BSP-pattern myRI copy/compare workaround is no
					// longer needed.  Phase 2 float-filter gate stays.
					const uint32_t end = node.firstPrimOrLeft + node.primCount;
					for( uint32_t i = node.firstPrimOrLeft; i < end; ++i ) {
						if( hasFastFilter ) {
							float fT;
							if( !MollerTrumboreFloat( origin, dir, fastFilter[i],
							                          (float)ri.range, fT ) ) {
								continue;
							}
						}
						ep.RayElementIntersection( ri, prims[i],
						                           bHitFrontFaces, bHitBackFaces );
					}
				} else {
					// Internal — push children near-far order based on
					// ray direction sign on splitAxis.  Near goes on
					// top of stack so it pops first.
					const uint32_t leftIdx  = node.firstPrimOrLeft;
					const uint32_t rightIdx = leftIdx + 1;
					const bool dirNeg = ( dir[ node.splitAxis ] < 0.0f );
					if( dirNeg ) {
						stack.push_back( leftIdx );
						stack.push_back( rightIdx );
					} else {
						stack.push_back( rightIdx );
						stack.push_back( leftIdx );
					}
				}
			}
		}

		// Same with exit-info flag (delegates to the unified leaf
		// intersection that takes RayIntersection rather than the
		// geometric-only variant).
		void IntersectRay(
			RayIntersection& ri,
			bool             bHitFrontFaces,
			bool             bHitBackFaces,
			bool             bComputeExitInfo ) const
		{
			if( useBVH4 && !nodes4.empty() ) {
				IntersectRay4( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
				return;
			}
			if( nodes.empty() ) return;

			const float origin[3] = {
				(float)ri.geometric.ray.origin.x,
				(float)ri.geometric.ray.origin.y,
				(float)ri.geometric.ray.origin.z };
			float dir[3] = {
				(float)ri.geometric.ray.Dir().x,
				(float)ri.geometric.ray.Dir().y,
				(float)ri.geometric.ray.Dir().z };
			float invDir[3];
			for( int k = 0; k < 3; ++k ) {
				if( dir[k] == 0.0f ) invDir[k] = ( dir[k] >= 0 ) ?  FLT_MAX : -FLT_MAX;
				else                  invDir[k] = 1.0f / dir[k];
			}

			// Bounded dynamic stack — see IntersectRay4 (BVH4 path) above.
			static thread_local std::vector<uint32_t> stack;
			stack.clear();
			stack.push_back( 0 );

			while( !stack.empty() ) {
				const uint32_t  ni   = stack.back();
				stack.pop_back();
				const Node&     node = nodes[ni];
				const float     currentBest = (float)ri.geometric.range;
				float tEntry;
				if( !RayBoxF( origin, invDir, currentBest,
				              node.bboxMin, node.bboxMax, tEntry ) )
					continue;

				if( node.primCount > 0 ) {
					// Cleanup §1+§2: same closest-hit-native pattern as the
					// geometric overload above — call processor directly.
					const uint32_t end = node.firstPrimOrLeft + node.primCount;
					for( uint32_t i = node.firstPrimOrLeft; i < end; ++i ) {
						if( hasFastFilter ) {
							float fT;
							if( !MollerTrumboreFloat( origin, dir, fastFilter[i],
							                          (float)ri.geometric.range, fT ) ) {
								continue;
							}
						}
						ep.RayElementIntersection( ri, prims[i],
						                           bHitFrontFaces, bHitBackFaces,
						                           bComputeExitInfo );
					}
				} else {
					const uint32_t leftIdx  = node.firstPrimOrLeft;
					const uint32_t rightIdx = leftIdx + 1;
					const bool dirNeg = ( dir[ node.splitAxis ] < 0.0f );
					if( dirNeg ) {
						stack.push_back( leftIdx );
						stack.push_back( rightIdx );
					} else {
						stack.push_back( rightIdx );
						stack.push_back( leftIdx );
					}
				}
			}
		}

		// Boolean any-hit traversal.  Returns true on first leaf hit
		// within dHowFar.  Used by shadow-ray paths.
		bool IntersectRay_IntersectionOnly(
			const Ray& ray,
			Scalar     dHowFar,
			bool       bHitFrontFaces,
			bool       bHitBackFaces ) const
		{
			if( useBVH4 && !nodes4.empty() ) {
				return IntersectRay4_IntersectionOnly( ray, dHowFar,
				                                       bHitFrontFaces, bHitBackFaces );
			}
			if( nodes.empty() ) return false;

			const float origin[3] = {
				(float)ray.origin.x,
				(float)ray.origin.y,
				(float)ray.origin.z };
			float dir[3] = {
				(float)ray.Dir().x,
				(float)ray.Dir().y,
				(float)ray.Dir().z };
			float invDir[3];
			for( int k = 0; k < 3; ++k ) {
				if( dir[k] == 0.0f ) invDir[k] = ( dir[k] >= 0 ) ?  FLT_MAX : -FLT_MAX;
				else                  invDir[k] = 1.0f / dir[k];
			}

			// Bounded dynamic stack — see IntersectRay4 (BVH4 path) above.
			static thread_local std::vector<uint32_t> stack;
			stack.clear();
			stack.push_back( 0 );
			const float currentBest = (float)dHowFar;

			while( !stack.empty() ) {
				const uint32_t  ni   = stack.back();
				stack.pop_back();
				const Node&     node = nodes[ni];
				float tEntry;
				if( !RayBoxF( origin, invDir, currentBest,
				              node.bboxMin, node.bboxMax, tEntry ) )
					continue;

				if( node.primCount > 0 ) {
					// Phase 2 filter — same pattern, but here a "hit" from
					// the filter alone is NOT enough to return early
					// because the filter is conservative (might pass on
					// near-edge cases that the double cert rejects).
					// We still defer the verdict to the production
					// _IntersectionOnly call.  The filter's job is to
					// skip non-hits, not to grant hits.
					const uint32_t end = node.firstPrimOrLeft + node.primCount;
					for( uint32_t i = node.firstPrimOrLeft; i < end; ++i ) {
						if( hasFastFilter ) {
							float fT;
							if( !MollerTrumboreFloat( origin, dir, fastFilter[i],
							                          currentBest, fT ) ) {
								continue;
							}
						}
						if( ep.RayElementIntersection_IntersectionOnly(
						        ray, dHowFar, prims[i],
						        bHitFrontFaces, bHitBackFaces ) ) {
							return true;
						}
					}
				} else {
					const uint32_t leftIdx  = node.firstPrimOrLeft;
					const uint32_t rightIdx = leftIdx + 1;
					const bool dirNeg = ( dir[ node.splitAxis ] < 0.0f );
					if( dirNeg ) {
						stack.push_back( leftIdx );
						stack.push_back( rightIdx );
					} else {
						stack.push_back( rightIdx );
						stack.push_back( leftIdx );
					}
				}
			}
			return false;
		}
	};
}

#endif
