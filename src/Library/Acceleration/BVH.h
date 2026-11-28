//////////////////////////////////////////////////////////////////////
//
//  BVH.h - Templated SAH-binned BVH2 / 4-wide BVH4 SIMD accelerator
//    with opt-in bit-identical parallel build and in-place refit.
//
//  Float-stored AABBs (conservatively padded outward by 1 ULP from
//  double) drive traversal; leaves intersect in double precision via
//  TreeElementProcessor<Element>.
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
#include "../Utilities/ThreadPool.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cfloat>

// SIMD selection: NEON on ARM, SSE2/SSE4.1 on x86_64, else scalar fallback.
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
	//! Top-down SAH-binned BVH2 collapsed into 4-wide SoA BVH4.
	//! Internal traversal tests 1-ULP outward-padded float AABBs; leaf intersection
	//! runs in double precision via TreeElementProcessor<Element>.
	template< class Element >
	class BVH : public Implementation::Reference
	{
	public:
		// 32-byte flat BVH2 node: right child is always at firstPrimOrLeft + 1.
		struct Node
		{
			float    bboxMin[3];     // conservative float lower bound
			float    bboxMax[3];     // conservative float upper bound
			uint32_t firstPrimOrLeft;// leaf: prims[] start; internal: left-child index
			uint16_t primCount;      // 0 = internal, >0 = leaf
			uint8_t  splitAxis;      // 0/1/2 for x/y/z (internal only)
			uint8_t  pad;            // align to 32 B
		};

		// Conservative float Möller-Trumbore pre-filter data (36 B/tri, parallel to prims[]).
		struct TriangleFilterData
		{
			float p0[3];   // first vertex
			float e1[3];   // v1 - v0
			float e2[3];   // v2 - v0
		};

		// 4-wide SoA BVH4 node: primCount[i] > 0 => leaf (children[i] = firstPrim),
		// primCount[i] == 0 => internal (children[i] = nodes4 index, or -1 if unused).
		// Unused slots store (+FLT_MAX, -FLT_MAX) and are masked by numChildren.
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
		std::vector<TriangleFilterData>      fastFilter;  // empty = float pre-filter off
		bool                                 hasFastFilter;
		std::vector<BVH4Node>                nodes4;      // empty = BVH4 off
		bool                                 useBVH4;
		BoundingBox                          overallBox;
		const TreeElementProcessor<Element>& ep;
		AccelerationConfig                   cfg;

		// Build-time SAH cost baseline for SAHDegradationRatio() (>2x signals rebuild).
		Scalar                               originalSAH = 0;

		virtual ~BVH() {}

	public:
		struct BuildOptions
		{
			bool parallel = false;	//!< build subtrees on GlobalThreadPool(); bit-identical to serial
			bool quiet = false;		//!< suppress Info log lines (e.g. per-frame rebuild)
		};

		BVH( const TreeElementProcessor<Element>& ep_,
		     const std::vector<Element>&          inputPrims,
		     const BoundingBox&                   overallBoxHint,
		     const AccelerationConfig&            cfg_,
		     const BuildOptions&                  options )
			: hasFastFilter( false ),
			  useBVH4( false ),
			  overallBox( overallBoxHint ),
			  ep( ep_ ),
			  cfg( cfg_ )
		{
			Rebuild( inputPrims, options );
		}

		//! Full rebuild over a new primitive list, reusing existing vector storage.
		void Rebuild( const std::vector<Element>& inputPrims, const BuildOptions& options )
		{
			hasFastFilter = false;
			useBVH4 = false;
			if( inputPrims.empty() ) overallBox = BoundingBox( Point3( 0, 0, 0 ), Point3( 0, 0, 0 ) );
			Build( inputPrims, options );
			BuildFastFilter( !options.quiet );
			BuildBVH4();
			originalSAH = ComputeSAH();
		}

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
			Build( inputPrims );

			BuildFastFilter();
			BuildBVH4();

			originalSAH = ComputeSAH();
		}

		size_t numNodes()   const { return nodes.size(); }
		size_t numNodes4()  const { return nodes4.size(); }
		size_t numPrims()   const { return prims.size(); }
		bool   FastFilterEnabled() const { return hasFastFilter; }
		bool   BVH4Enabled() const { return useBVH4; }
		const BoundingBox& GetBBox() const { return overallBox; }

		//! Ratio currentSAH / originalSAH (1.0 if baseline unset); > ~2.0 signals
		//! that in-place refit has degraded traversal and caller should rebuild.
		Scalar SAHDegradationRatio() const
		{
			if( originalSAH <= 0 ) return 1.0;
			const Scalar cur = ComputeSAH();
			if( cur <= 0 ) return 1.0;
			return cur / originalSAH;
		}

		//! In-place AABB refit preserving topology (reverse-index bottom-up pass),
		//! then rebuilds float filter and BVH4 collapse. Returns duration in ms.
		unsigned int Refit( const bool logSummary = true )
		{
			const auto tStart = std::chrono::steady_clock::now();
			if( nodes.empty() ) {
				BuildFastFilter();  // no-op on empty
				BuildBVH4();        // no-op on empty
				const auto tEnd = std::chrono::steady_clock::now();
				return (unsigned int)ElapsedMilliseconds(tStart, tEnd);
			}

			BoundingBox newOverall(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );

			// Reverse index order: children always have larger indices than parents.
			for( int32_t i = (int32_t)nodes.size() - 1; i >= 0; --i ) {
				Node& n = nodes[ (size_t)i ];
				if( n.primCount > 0 ) {
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

			newOverall.ll.x = nodes[0].bboxMin[0];
			newOverall.ll.y = nodes[0].bboxMin[1];
			newOverall.ll.z = nodes[0].bboxMin[2];
			newOverall.ur.x = nodes[0].bboxMax[0];
			newOverall.ur.y = nodes[0].bboxMax[1];
			newOverall.ur.z = nodes[0].bboxMax[2];
			overallBox = newOverall;

			BuildFastFilter( logSummary );
			BuildBVH4();

			const auto tEnd = std::chrono::steady_clock::now();
			const unsigned int ms = (unsigned int)ElapsedMilliseconds(tStart, tEnd);

			if( !logSummary ) return ms;
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

		//! Serializes BVH2 ("BVH2", v1, nodes, DFS prim indices via primIdxFn, overallBox).
		//! BVH4 and float-filter tables are re-derived on Deserialize().
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

		//! Deserializes BVH2 and rebuilds float filter + BVH4 collapse; returns false on bad header/index.
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

			BuildFastFilter();
			BuildBVH4();
			return true;
		}

		//! Precomputes float Möller-Trumbore edges for all prims[] if ep supplies float vertices.
		void BuildFastFilter( const bool logSummary = true )
		{
			if( prims.empty() ) {
				hasFastFilter = false;
				return;
			}

			float v0[3], v1[3], v2[3];
			if( !ep.GetFloatTriangleVertices( prims[0], v0, v1, v2 ) ) {
				hasFastFilter = false;
				return;
			}

			fastFilter.resize( prims.size() );
			for( size_t i = 0; i < prims.size(); ++i ) {
				if( !ep.GetFloatTriangleVertices( prims[i], v0, v1, v2 ) ) {
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

			if( logSummary ) GlobalLog()->PrintEx( eLog_Info,
				"BVH:: Float filter enabled (%u triangles precomputed, %u bytes)",
				(unsigned)fastFilter.size(),
				(unsigned)( fastFilter.size() * sizeof( TriangleFilterData ) ) );
		}

		//! Collapses BVH2 into 4-wide BVH4 (clearing nodes4 first so Refit() stays safe).
		void BuildBVH4()
		{
			nodes4.clear();

			if( nodes.empty() ) {
				useBVH4 = false;
				return;
			}

			nodes4.reserve( nodes.size() / 2 + 1 );
			nodes4.push_back( BVH4Node{} );
			InitEmptyBVH4Node( nodes4[0] );

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

		// Recursive collapse into pre-allocated nodes4[bvh4Idx]. Uses index-based
		// access throughout because recursive push_back may reallocate nodes4.
		void CollapseRecursive( uint32_t bvh4Idx, uint32_t bvh2NodeIdx )
		{
			const Node& n = nodes[bvh2NodeIdx];
			const uint32_t lIdx = n.firstPrimOrLeft;
			const uint32_t rIdx = lIdx + 1;
			const Node& l = nodes[lIdx];
			const Node& r = nodes[rIdx];

			uint32_t candidateNodeIdx[4];
			int      numCandidates = 0;

			if( l.primCount > 0 ) {
				candidateNodeIdx[numCandidates++] = lIdx;
			} else {
				const uint32_t llIdx = l.firstPrimOrLeft;
				const uint32_t lrIdx = llIdx + 1;
				candidateNodeIdx[numCandidates++] = llIdx;
				candidateNodeIdx[numCandidates++] = lrIdx;
			}
			if( r.primCount > 0 ) {
				candidateNodeIdx[numCandidates++] = rIdx;
			} else {
				const uint32_t rlIdx = r.firstPrimOrLeft;
				const uint32_t rrIdx = rlIdx + 1;
				candidateNodeIdx[numCandidates++] = rlIdx;
				candidateNodeIdx[numCandidates++] = rrIdx;
			}

			nodes4[bvh4Idx].numChildren = (uint8_t)numCandidates;
			nodes4[bvh4Idx].splitAxis   = n.splitAxis;

			for( int i = 0; i < numCandidates; ++i ) {
				const uint32_t bvh2Cand = candidateNodeIdx[i];
				const Node& cand = nodes[bvh2Cand];

				const float bxn = cand.bboxMin[0], byn = cand.bboxMin[1], bzn = cand.bboxMin[2];
				const float bxp = cand.bboxMax[0], byp = cand.bboxMax[1], bzp = cand.bboxMax[2];

				if( cand.primCount > 0 ) {
					nodes4[bvh4Idx].bboxMinX[i] = bxn;
					nodes4[bvh4Idx].bboxMinY[i] = byn;
					nodes4[bvh4Idx].bboxMinZ[i] = bzn;
					nodes4[bvh4Idx].bboxMaxX[i] = bxp;
					nodes4[bvh4Idx].bboxMaxY[i] = byp;
					nodes4[bvh4Idx].bboxMaxZ[i] = bzp;
					nodes4[bvh4Idx].children[i]  = (int32_t)cand.firstPrimOrLeft;
					nodes4[bvh4Idx].primCount[i] = cand.primCount;
				} else {
					const uint32_t childBVH4Idx = (uint32_t)nodes4.size();
					nodes4.push_back( BVH4Node{} );
					InitEmptyBVH4Node( nodes4.back() );
					CollapseRecursive( childBVH4Idx, bvh2Cand );

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
		//! 4-wide SIMD ray-vs-AABB kernel: returns 4-bit hit mask and writes
		//! entry-t per lane (FLT_MAX on miss).
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
		void Build( const std::vector<Element>& inputPrims, const BuildOptions& options = BuildOptions() )
		{
			const auto tStart = std::chrono::steady_clock::now();

			if( !options.quiet ) GlobalLog()->PrintEx( eLog_Info,
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
			const auto boxRange = [&]( size_t begin, size_t end, BoundingBox& rb, BoundingBox& rcb ) {
				for( size_t i = begin; i < end; ++i ) {
					BoundingBox b = ep.GetElementBoundingBox( prims[i] );
					primBox[i]      = b;
					// Centroid = midpoint of bbox.  Done component-wise — no
					// existing op for "midpoint between two Point3" in this codebase.
					primCentroid[i] = Point3( ( b.ll.x + b.ur.x ) * 0.5,
					                          ( b.ll.y + b.ur.y ) * 0.5,
					                          ( b.ll.z + b.ur.z ) * 0.5 );
					rb.Include( b.ll );
					rb.Include( b.ur );
					rcb.Include( primCentroid[i] );
				}
			};
			const bool parallel = options.parallel && prims.size() >= kParallelBuildMinimum &&
				Implementation::GlobalThreadPool().NumWorkers() > 1;
			if( !parallel ) {
				boxRange( 0, prims.size(), rootBox, rootCentroidBox );
			} else {
				// Min / max reductions: the union is exact whatever the order.
				const unsigned int chunks = 64;
				std::vector<BoundingBox> rb( chunks, rootBox ), rcb( chunks, rootCentroidBox );
				Implementation::GlobalThreadPool().ParallelFor( chunks, [&]( unsigned int c ) {
					boxRange( prims.size() * c / chunks, prims.size() * ( c + 1 ) / chunks, rb[c], rcb[c] );
				} );
				for( unsigned int c = 0; c < chunks; ++c ) {
					if( rb[c].ll.x <= rb[c].ur.x ) { rootBox.Include( rb[c].ll ); rootBox.Include( rb[c].ur ); }
					if( rcb[c].ll.x <= rcb[c].ur.x ) { rootCentroidBox.Include( rcb[c].ll ); rootCentroidBox.Include( rcb[c].ur ); }
				}
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

			if( !parallel ) {
				BuildRecursive( nodes, 0, idx, 0, (uint32_t)prims.size(),
				                primBox, primCentroid, rootBox, rootCentroidBox );
			} else {
				BuildParallel( idx, primBox, primCentroid, rootBox, rootCentroidBox );
			}

			// Reorder prims by the now-finalized index array.  After
			// this, leaves' [firstPrimOrLeft, firstPrimOrLeft+primCount)
			// range indexes directly into `prims`.
			std::vector<Element> reordered( prims.size() );
			for( size_t i = 0; i < idx.size(); ++i ) {
				reordered[i] = prims[ idx[i] ];
			}
			prims.swap( reordered );

			const auto tEnd = std::chrono::steady_clock::now();
			if( !options.quiet ) GlobalLog()->PrintEx( eLog_Info,
				"BVH:: Built %u nodes (%u leaves) in %u ms",
				(unsigned)nodes.size(),
				(unsigned)CountLeaves(),
				(unsigned)ElapsedMilliseconds(tStart, tEnd) );
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
			std::vector<Node>&             out,
			uint32_t                       nodeIdx,
			std::vector<uint32_t>&         idx,
			uint32_t                       first,
			uint32_t                       last,
			const std::vector<BoundingBox>& primBox,
			const std::vector<Point3>&      primCentroid,
			const BoundingBox&              parentBox,
			const BoundingBox&              centroidBox )
		{
			uint32_t mid = 0;
			BoundingBox leftCBox, rightCBox;
			if( !SplitNode( out[nodeIdx], idx, first, last, primBox, primCentroid, centroidBox, mid, leftCBox, rightCBox ) ) {
				return;
			}
			// Allocate two children, contiguous (Node[leftIdx], Node[leftIdx+1]).
			const uint32_t leftIdx = (uint32_t)out.size();
			out.push_back( Node{} );
			out.push_back( Node{} );
			out[nodeIdx].firstPrimOrLeft = leftIdx;
			out[nodeIdx].primCount       = 0;
			// (splitAxis was set by SplitNode)

			BuildRecursive( out, leftIdx,     idx, first, mid,  primBox, primCentroid, parentBox, leftCBox );
			BuildRecursive( out, leftIdx + 1, idx, mid,   last, primBox, primCentroid, parentBox, rightCBox );
		}

		// One node of the build: its tight box, and either a leaf (false) or
		// the SAH split of idx[first..last) in place, with the child ranges'
		// centroid boxes (true; `node.splitAxis` holds the axis).
		bool SplitNode(
			Node&                           node,
			std::vector<uint32_t>&          idx,
			uint32_t                        first,
			uint32_t                        last,
			const std::vector<BoundingBox>& primBox,
			const std::vector<Point3>&      primCentroid,
			const BoundingBox&              centroidBox,
			uint32_t&                       midOut,
			BoundingBox&                    leftCBoxOut,
			BoundingBox&                    rightCBoxOut )
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
			SetNodeBox( node, tightBox );

			// Leaf-termination conditions.
			if( count <= cfg.maxLeafSize ) {
				MakeLeaf( node, first, count );
				return false;
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
				MakeLeaf( node, first, count );
				return false;
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
				MakeLeaf( node, first, count );
				return false;
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

			node.splitAxis = axis;
			midOut = mid;
			leftCBoxOut = leftCBox;
			rightCBoxOut = rightCBox;
			return true;
		}

		// Parallel build: splits top levels serially, builds disjoint subtrees on
		// GlobalThreadPool(), and splices them in exact serial DFS node order.
		static const size_t kParallelBuildMinimum = 65536;	// smaller lists build serially
		static const uint32_t kParallelTaskMinimum = 16384;	// smallest subtree given its own task

		struct TopNode
		{
			Node node{};
			bool split = false;
			int children[2] = { -1, -1 };	// TopNode indices when split
			int task = -1;					// local tree index when a pool task builds it
			uint32_t first = 0, last = 0;
			BoundingBox centroidBox;
		};

		void BuildParallel(
			std::vector<uint32_t>&          idx,
			const std::vector<BoundingBox>& primBox,
			const std::vector<Point3>&      primCentroid,
			const BoundingBox&              rootBox,
			const BoundingBox&              rootCentroidBox )
		{
			const unsigned int workers = Implementation::GlobalThreadPool().NumWorkers();
			unsigned int maxDepth = 0;
			while( ( 1u << maxDepth ) < 4u * workers && maxDepth < 12 ) ++maxDepth;

			std::vector<TopNode> top;
			std::vector<int> tasks;	// TopNode index per local tree
			top.push_back( TopNode{} );
			top[0].first = 0; top[0].last = (uint32_t)prims.size(); top[0].centroidBox = rootCentroidBox;
			(void)rootBox;
			// Breadth-agnostic serial split of the top levels (each split touches
			// only its own range of idx, so the order of splits is immaterial).
			std::vector<std::pair<int, unsigned int>> work( 1, std::make_pair( 0, 0u ) );
			while( !work.empty() ) {
				const int t = work.back().first;
				const unsigned int depth = work.back().second;
				work.pop_back();
				if( depth >= maxDepth || top[t].last - top[t].first < 2 * kParallelTaskMinimum ) {
					top[t].task = (int)tasks.size();
					tasks.push_back( t );
					continue;
				}
				uint32_t mid = 0;
				BoundingBox lc, rc;
				const uint32_t first = top[t].first, last = top[t].last;
				const BoundingBox cbox = top[t].centroidBox;
				Node node{};
				if( !SplitNode( node, idx, first, last, primBox, primCentroid, cbox, mid, lc, rc ) ) {
					top[t].node = node;	// a leaf
					continue;
				}
				top[t].node = node;
				top[t].split = true;
				const int l = (int)top.size();
				top.push_back( TopNode{} );
				top.push_back( TopNode{} );
				top[t].children[0] = l; top[t].children[1] = l + 1;
				top[l].first = first; top[l].last = mid; top[l].centroidBox = lc;
				top[l + 1].first = mid; top[l + 1].last = last; top[l + 1].centroidBox = rc;
				work.push_back( std::make_pair( l + 1, depth + 1 ) );
				work.push_back( std::make_pair( l, depth + 1 ) );
			}

			std::vector<std::vector<Node>> local( tasks.size() );
			Implementation::GlobalThreadPool().ParallelFor( (unsigned int)tasks.size(), [&]( unsigned int k ) {
				const TopNode& t = top[ tasks[k] ];
				std::vector<Node>& out = local[k];
				out.reserve( 2 * ( t.last - t.first ) );
				out.push_back( Node{} );
				BuildRecursive( out, 0, idx, t.first, t.last, primBox, primCentroid, t.centroidBox, t.centroidBox );
			} );

			size_t total = 1;
			for( const TopNode& t : top ) if( t.split ) total += 2;
			for( const std::vector<Node>& l : local ) total += l.size() - 1;
			nodes.resize( total );
			uint32_t cursor = 1;
			Place( top, local, 0, 0, cursor );
		}

		// Writes TopNode t at nodes[slot] and its descendants from `cursor`,
		// in the serial build's order: the child pair, then the left
		// subtree's descendants, then the right's.
		void Place( const std::vector<TopNode>& top, const std::vector<std::vector<Node>>& local,
			int t, uint32_t slot, uint32_t& cursor )
		{
			const TopNode& tn = top[t];
			if( tn.task >= 0 ) {
				const std::vector<Node>& l = local[ tn.task ];
				const uint32_t base = cursor;	// local index i >= 1 goes to base + i - 1
				Node root = l[0];
				if( root.primCount == 0 ) root.firstPrimOrLeft = root.firstPrimOrLeft - 1 + base;
				nodes[slot] = root;
				for( size_t i = 1; i < l.size(); ++i ) {
					Node n = l[i];
					if( n.primCount == 0 ) n.firstPrimOrLeft = n.firstPrimOrLeft - 1 + base;
					nodes[ base + i - 1 ] = n;
				}
				cursor += (uint32_t)( l.size() - 1 );
				return;
			}
			nodes[slot] = tn.node;
			if( !tn.split ) return;
			const uint32_t left = cursor;
			cursor += 2;
			nodes[slot].firstPrimOrLeft = left;
			nodes[slot].primCount = 0;
			Place( top, local, tn.children[0], left, cursor );
			Place( top, local, tn.children[1], left + 1, cursor );
		}

		static void MakeLeaf( Node& node, uint32_t first, uint32_t count )
		{
			node.firstPrimOrLeft = first;
			node.primCount       = (uint16_t)count;
			node.splitAxis       = 0;
		}
		uint32_t CountLeaves() const
		{
			uint32_t n = 0;
			for( const Node& node : nodes ) if( node.primCount != 0 ) ++n;
			return n;
		}

		// SAH cost of the current tree normalized by root surface area (0 if empty).
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

		// Conservative float AABB store: rounds finite in-range doubles outward by
		// 1 ULP via std::nextafter (overflowing/unbounded bounds clamp to +/-FLT_MAX).
		static inline void SetNodeBox( Node& n, const BoundingBox& b )
		{
			n.bboxMin[0] = std::nextafter( (float)b.ll.x, -FLT_MAX );
			n.bboxMin[1] = std::nextafter( (float)b.ll.y, -FLT_MAX );
			n.bboxMin[2] = std::nextafter( (float)b.ll.z, -FLT_MAX );
			n.bboxMax[0] = std::nextafter( (float)b.ur.x,  FLT_MAX );
			n.bboxMax[1] = std::nextafter( (float)b.ur.y,  FLT_MAX );
			n.bboxMax[2] = std::nextafter( (float)b.ur.z,  FLT_MAX );
		}

		// Conservative float Möller-Trumbore pre-filter (EPS = 1e-5f): skips clear misses;
		// passing candidates are certified in double precision by ep.RayElementIntersection.
		static inline bool MollerTrumboreFloat(
			const float origin[3],
			const float dir[3],
			const TriangleFilterData& f,
			float       currentBest,
			float&      tOut )
		{
			constexpr float EPS = 1e-5f;

			const float pvec[3] = {
				dir[1] * f.e2[2] - dir[2] * f.e2[1],
				dir[2] * f.e2[0] - dir[0] * f.e2[2],
				dir[0] * f.e2[1] - dir[1] * f.e2[0]
			};

			const float det = f.e1[0] * pvec[0] + f.e1[1] * pvec[1] + f.e1[2] * pvec[2];

			if( std::fabs( det ) < 1e-20f ) return false;

			const float invDet = 1.0f / det;

			const float tvec[3] = {
				origin[0] - f.p0[0],
				origin[1] - f.p0[1],
				origin[2] - f.p0[2]
			};

			const float u = ( tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2] ) * invDet;
			if( u < -EPS || u > 1.0f + EPS ) return false;

			const float qvec[3] = {
				tvec[1] * f.e1[2] - tvec[2] * f.e1[1],
				tvec[2] * f.e1[0] - tvec[0] * f.e1[2],
				tvec[0] * f.e1[1] - tvec[1] * f.e1[0]
			};

			const float v = ( dir[0] * qvec[0] + dir[1] * qvec[1] + dir[2] * qvec[2] ) * invDet;
			if( v < -EPS || u + v > 1.0f + EPS ) return false;

			const float t = ( f.e2[0] * qvec[0] + f.e2[1] * qvec[1] + f.e2[2] * qvec[2] ) * invDet;
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

		// Optional epOverride lets callers (e.g. ObjectManager::IntersectOcclusionRay)
		// reuse this BVH under a different TreeElementProcessor, resolved once per leaf.
		inline bool Bvh4Leaf_IntersectionOnly(
			const Ray& ray, Scalar dHowFar,
			uint32_t firstPrim, uint32_t primCnt,
			const float origin[3], const float dir[3],
			bool bHitFrontFaces, bool bHitBackFaces,
			const TreeElementProcessor<Element>* epOverride = nullptr ) const
		{
			const TreeElementProcessor<Element>& useEp = epOverride ? *epOverride : ep;
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
				if( useEp.RayElementIntersection_IntersectionOnly(
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

			// thread_local dynamic stack avoids fixed-depth overflow on deep/clustered trees.
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
		// `epOverride`: see Bvh4Leaf_IntersectionOnly above.
		bool IntersectRay4_IntersectionOnly(
			const Ray& ray, Scalar dHowFar,
			bool bHitFrontFaces, bool bHitBackFaces,
			const TreeElementProcessor<Element>* epOverride = nullptr ) const
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
								bHitFrontFaces, bHitBackFaces,
								epOverride ) ) {
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
		//
		// `epOverride`: nullptr (every existing call site) uses the
		// bound `ep` -- the same behaviour as before this parameter
		// existed, for one pointer compare per CALL (resolved once, above
		// the traversal).  A non-null override traverses this SAME
		// tree under a different TreeElementProcessor for one call --
		// see ObjectManager::IntersectOcclusionRay (geometry-presence
		// query that must NOT honour DoesCastShadows(), unlike the
		// shadow-ray query this method otherwise serves).
		bool IntersectRay_IntersectionOnly(
			const Ray& ray,
			Scalar     dHowFar,
			bool       bHitFrontFaces,
			bool       bHitBackFaces,
			const TreeElementProcessor<Element>* epOverride = nullptr ) const
		{
			if( useBVH4 && !nodes4.empty() ) {
				return IntersectRay4_IntersectionOnly( ray, dHowFar,
				                                       bHitFrontFaces, bHitBackFaces,
				                                       epOverride );
			}
			if( nodes.empty() ) return false;

			const TreeElementProcessor<Element>& useEp = epOverride ? *epOverride : ep;

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
						if( useEp.RayElementIntersection_IntersectionOnly(
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

		//! Bounded-radius closest-point query over BVH2 `nodes` (nearest-AABB-first).
		//! Keeps query `p` in full Scalar precision against outward-padded float node
		//! boxes so pruning is conservative (exact wrt brute force). Not re-entrant
		//! within the same (Element, PrimDistFn) instantiation due to thread_local stack;
		//! `useElementBoxTest` pre-filters leaf elements in Scalar. Optional diagnostic
		//! counters are incremented (not reset).
		template< class PrimDistFn >
		bool ClosestPointDistance(
			const Point3& p,
			const Scalar  maxDist,
			PrimDistFn&&  primDist,
			Scalar&       outDist,
			const bool    useElementBoxTest = false,
			std::size_t*  outNodesVisited = 0,
			std::size_t*  outCandidatesVisited = 0 ) const
		{
			if( nodes.empty() || prims.empty() ) return false;
			if( !( maxDist > Scalar( 0 ) ) ) return false;

			Scalar best  = maxDist;
			bool   found = false;

			struct Entry { uint32_t node; Scalar dist; };
			static thread_local std::vector<Entry> stack;
			stack.clear();

			const Scalar rootDist = PointBoxDistanceF( p, nodes[0].bboxMin, nodes[0].bboxMax );
			if( !( rootDist < best ) ) return false;
			stack.push_back( Entry{ 0u, rootDist } );

			while( !stack.empty() ) {
				const Entry e = stack.back();
				stack.pop_back();
				if( !( e.dist < best ) ) continue;
				if( outNodesVisited ) ++( *outNodesVisited );

				const Node& node = nodes[ e.node ];

				if( node.primCount > 0 ) {
					const uint32_t end = node.firstPrimOrLeft + node.primCount;
					for( uint32_t i = node.firstPrimOrLeft; i < end; ++i ) {
						if( useElementBoxTest ) {
							const BoundingBox eb = ep.GetElementBoundingBox( prims[i] );
							const Scalar ebDist = PointBoxDistance( p, eb.ll, eb.ur );
							if( !( ebDist < best ) ) continue;
						}
						if( outCandidatesVisited ) ++( *outCandidatesVisited );
						const Scalar d = primDist( prims[i], p );
						if( d < best ) {
							best  = d;
							found = true;
							if( best <= Scalar( 0 ) ) {
								outDist = Scalar( 0 );
								return true;
							}
						}
					}
				} else {
					const uint32_t leftIdx  = node.firstPrimOrLeft;
					const uint32_t rightIdx = leftIdx + 1;
					const Scalar dL = PointBoxDistanceF( p, nodes[leftIdx].bboxMin,  nodes[leftIdx].bboxMax );
					const Scalar dR = PointBoxDistanceF( p, nodes[rightIdx].bboxMin, nodes[rightIdx].bboxMax );
					// Push farther child first so nearer pops first and tightens `best`.
					if( dL <= dR ) {
						if( dR < best ) stack.push_back( Entry{ rightIdx, dR } );
						if( dL < best ) stack.push_back( Entry{ leftIdx,  dL } );
					} else {
						if( dL < best ) stack.push_back( Entry{ leftIdx,  dL } );
						if( dR < best ) stack.push_back( Entry{ rightIdx, dR } );
					}
				}
			}

			if( !found ) return false;
			outDist = best;
			return true;
		}

		//! Visits every leaf element whose containing BVH2 node (and element box if
		//! `useElementBoxTest`) contains `p`. Conservative & NaN-safe over-admission;
		//! not re-entrant within one instantiation.
		template< class VisitFn >
		void ForEachContainingPoint(
			const Point3& p,
			VisitFn&&     visit,
			const bool    useElementBoxTest = false,
			std::size_t*  outNodesVisited = 0,
			std::size_t*  outCandidatesVisited = 0 ) const
		{
			if( nodes.empty() || prims.empty() ) return;

			static thread_local std::vector<uint32_t> stack;
			stack.clear();

			if( !PointInBoxF( p, nodes[0].bboxMin, nodes[0].bboxMax ) ) return;
			stack.push_back( 0u );

			while( !stack.empty() ) {
				const uint32_t ni = stack.back();
				stack.pop_back();
				if( outNodesVisited ) ++( *outNodesVisited );
				const Node& node = nodes[ni];

				if( node.primCount > 0 ) {
					const uint32_t end = node.firstPrimOrLeft + node.primCount;
					for( uint32_t i = node.firstPrimOrLeft; i < end; ++i ) {
						if( useElementBoxTest ) {
							const BoundingBox eb = ep.GetElementBoundingBox( prims[i] );
							if( !PointInBox( p, eb.ll, eb.ur ) ) continue;
						}
						if( outCandidatesVisited ) ++( *outCandidatesVisited );
						visit( prims[i], p );
					}
				} else {
					const uint32_t leftIdx  = node.firstPrimOrLeft;
					const uint32_t rightIdx = leftIdx + 1;
					if( PointInBoxF( p, nodes[leftIdx].bboxMin, nodes[leftIdx].bboxMax ) ) {
						stack.push_back( leftIdx );
					}
					if( PointInBoxF( p, nodes[rightIdx].bboxMin, nodes[rightIdx].bboxMax ) ) {
						stack.push_back( rightIdx );
					}
				}
			}
		}

		//! Visits every leaf element whose box lies within `radius` of `p`.
		template< class VisitFn >
		void ForEachNearPoint(
			const Point3& p,
			const Scalar  radius,
			VisitFn&&     visit,
			const bool    useElementBoxTest = false ) const
		{
			if( nodes.empty() || prims.empty() ) return;
			static thread_local std::vector<uint32_t> stack;
			stack.clear();
			if( PointBoxDistanceF( p, nodes[0].bboxMin, nodes[0].bboxMax ) > radius ) return;
			stack.push_back( 0u );
			while( !stack.empty() ) {
				const uint32_t ni = stack.back();
				stack.pop_back();
				const Node& node = nodes[ni];
				if( node.primCount > 0 ) {
					const uint32_t end = node.firstPrimOrLeft + node.primCount;
					for( uint32_t i = node.firstPrimOrLeft; i < end; ++i ) {
						if( useElementBoxTest ) {
							const BoundingBox eb = ep.GetElementBoundingBox( prims[i] );
							if( PointBoxDistance( p, eb.ll, eb.ur ) > radius ) continue;
						}
						visit( prims[i], p );
					}
				} else {
					const uint32_t leftIdx  = node.firstPrimOrLeft;
					const uint32_t rightIdx = leftIdx + 1;
					if( PointBoxDistanceF( p, nodes[leftIdx].bboxMin, nodes[leftIdx].bboxMax ) <= radius ) {
						stack.push_back( leftIdx );
					}
					if( PointBoxDistanceF( p, nodes[rightIdx].bboxMin, nodes[rightIdx].bboxMax ) <= radius ) {
						stack.push_back( rightIdx );
					}
				}
			}
		}

	protected:
		//! Conservative point-to-node-AABB distance (0 if inside): keeps `p` in Scalar
		//! and widens outward-padded float bounds to Scalar so distance is never overstated.
		static Scalar PointBoxDistanceF( const Point3& p, const float lo[3], const float hi[3] )
		{
			Scalar sum = Scalar( 0 );
			const Scalar pc[3] = { p.x, p.y, p.z };
			for( int k = 0; k < 3; ++k ) {
				const Scalar v  = pc[k];
				const Scalar l  = (Scalar)lo[k];
				const Scalar h  = (Scalar)hi[k];
				const Scalar dk = ( v < l ) ? ( l - v ) : ( ( v > h ) ? ( v - h ) : Scalar( 0 ) );
				sum += dk * dk;
			}
			return std::sqrt( sum );
		}

		//! Exact Scalar point-to-AABB distance for element-level pre-tests.
		static Scalar PointBoxDistance( const Point3& p, const Point3& lo, const Point3& hi )
		{
			const Scalar dx = ( p.x < lo.x ) ? ( lo.x - p.x ) : ( ( p.x > hi.x ) ? ( p.x - hi.x ) : Scalar( 0 ) );
			const Scalar dy = ( p.y < lo.y ) ? ( lo.y - p.y ) : ( ( p.y > hi.y ) ? ( p.y - hi.y ) : Scalar( 0 ) );
			const Scalar dz = ( p.z < lo.z ) ? ( lo.z - p.z ) : ( ( p.z > hi.z ) ? ( p.z - hi.z ) : Scalar( 0 ) );
			return std::sqrt( dx*dx + dy*dy + dz*dz );
		}

		//! Conservative, NaN-safe (over-admitting) point-in-float-AABB test.
		static bool PointInBoxF( const Point3& p, const float lo[3], const float hi[3] )
		{
			const Scalar pc[3] = { p.x, p.y, p.z };
			for( int k = 0; k < 3; ++k ) {
				const Scalar l = (Scalar)lo[k];
				const Scalar h = (Scalar)hi[k];
				if( pc[k] < l || pc[k] > h ) return false;
			}
			return true;
		}

		//! Exact, NaN-safe Scalar point-in-AABB test for element pre-tests.
		static bool PointInBox( const Point3& p, const Point3& lo, const Point3& hi )
		{
			if( p.x < lo.x || p.x > hi.x ) return false;
			if( p.y < lo.y || p.y > hi.y ) return false;
			if( p.z < lo.z || p.z > hi.z ) return false;
			return true;
		}
	};
}

#endif
