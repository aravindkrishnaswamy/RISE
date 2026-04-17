//////////////////////////////////////////////////////////////////////
//
//  VCMLightVertexStore.h - Persistent, KD-tree-indexed store of
//    light-subpath vertices used by VCM's photon merging pass.
//
//    The store is the product of one full light pass inside a VCM
//    iteration: every non-delta surface vertex from every light
//    subpath is appended, then the whole array is balanced into a
//    left-balanced KD-tree, then the eye pass fires fixed-radius
//    queries against it.
//
//    Thread-safety model:
//      - Append-phase writes go to per-thread local buffers owned
//        by the rasterizer (not the store), then the rasterizer
//        calls Concat() in deterministic thread-index order after
//        the light pass barriers.  The store itself exposes no
//        mutex; Concat is the single writer.
//      - After BuildKDTree() the store is read-only for the rest
//        of the iteration and Query() is safe from any thread.
//
//    The cloned KD-tree template VCMLightVertexKDTree<T> is a
//    fixed-radius-only copy of PhotonMapping/PhotonMap.h's
//    PhotonMapCore<T>, stripped of the IPhotonMap virtual
//    interface and protected constructor that require a tracer
//    pointer.  The balance and query algorithms are unchanged.
//
//    Step 0 ships an empty store with no-op methods so the build
//    wiring compiles; Step 3 populates the KD-tree template and
//    adds a unit test.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VCM_LIGHT_VERTEX_STORE_
#define VCM_LIGHT_VERTEX_STORE_

#include "VCMLightVertex.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class LightVertexStore
		{
		public:
			LightVertexStore();
			~LightVertexStore();

			/// Drop all vertices and reset the tree to "not built".
			/// Called once at the start of each VCM iteration.
			void Clear();

			/// Pre-reserve capacity so Append / Concat avoid
			/// reallocation during the light pass.
			void Reserve( const std::size_t capacity );

			/// Append a single vertex.  NOT thread-safe; the
			/// rasterizer writes to per-thread buffers and then
			/// calls Concat from a single thread after the barrier.
			void Append( const LightVertex& v );

			/// Move-concatenate a whole per-thread buffer.  Caller
			/// is responsible for ordering across threads.
			void Concat( std::vector<LightVertex>&& localBuffer );

			/// Build a left-balanced KD-tree over the current
			/// contents.  After this call the store is read-only;
			/// Query() is safe from any thread.  Step 3 fills this
			/// in; Step 0 is a no-op so the stubs compile.
			void BuildKDTree();

			/// Parallel variant — splits the recursion across the
			/// global thread pool.  Produces a byte-identical tree
			/// to BuildKDTree (unit-tested).  Falls back to serial
			/// below numWorkers*8 vertices so task overhead never
			/// dominates.
			void BuildKDTreeParallel();

			/// Fixed-radius query.  Appends all vertices within
			/// radiusSq of center to 'out'.  Step 3 fills this in;
			/// Step 0 returns nothing.
			void Query(
				const Point3& center,
				const Scalar radiusSq,
				std::vector<LightVertex>& out
				) const;

			/// Return the number of vertices currently stored.
			std::size_t Size() const { return mVertices.size(); }

			/// Read-only access to a stored vertex by index.
			const LightVertex& Get( std::size_t idx ) const { return mVertices[idx]; }

			/// Mutable access to a stored vertex by index.
			/// Only valid BEFORE BuildKDTree (the tree reorders
			/// the array; accessing by pre-balance index after
			/// the tree is built will give the wrong vertex).
			LightVertex& GetMutable( std::size_t idx ) { return mVertices[idx]; }

			/// Has BuildKDTree() been called since the last
			/// Clear/Append sequence?
			bool IsBuilt() const { return mBuilt; }

			/// Diagonal length of the axis-aligned bounding box of
			/// currently stored vertices, used as a robust proxy for
			/// scene extent when auto-resolving the VCM merge radius.
			/// Returns 0 when the store is empty.
			Scalar ComputeBBoxDiagonal() const;

		private:
			std::vector<LightVertex>	mVertices;
			bool						mBuilt;
		};
	}
}

#endif
