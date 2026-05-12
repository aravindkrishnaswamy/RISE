//////////////////////////////////////////////////////////////////////
//
//  VCMLightVertexStore.cpp - Persistent light-vertex store for VCM.
//
//    Holds a flat array of LightVertex objects populated during the
//    VCM light pass, balanced into a left-balanced KD-tree for
//    fixed-radius queries during the eye pass.
//
//    The KD-tree algorithm is a lightweight clone of
//    PhotonMapCore<T>'s BalanceSegment + LocateAllPhotons from
//    src/Library/PhotonMapping/PhotonMap.h.  The clone keeps the
//    exact same index arithmetic, splitting-axis selection, and
//    recursive traversal so that a side-by-side comparison against
//    PhotonMapCore<Photon> on the same input produces identical
//    tree layout and query results (verified by
//    tests/VCMLightVertexStoreTest.cpp).
//
//    Rationale for the clone: PhotonMapCore inherits from
//    IPhotonMap + IReference + IPhotonTracer and has a protected
//    constructor that requires a tracer pointer.  Instantiating it
//    on a pure data type (LightVertex) would require wrestling
//    with those virtuals.  The ~150-line clone is cleaner.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "VCMLightVertexStore.h"
#include "../Utilities/BoundingBox.h"
#include "../Utilities/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Comparators for std::nth_element — mirror PhotonMapCore's
	// less_than_X/Y/Z lambdas.  The stored LightVertex type exposes
	// ptPosition as its first field (required so the balance
	// algorithm can index by axis).
	inline bool LessThanX( const LightVertex& a, const LightVertex& b ) { return a.ptPosition.x < b.ptPosition.x; }
	inline bool LessThanY( const LightVertex& a, const LightVertex& b ) { return a.ptPosition.y < b.ptPosition.y; }
	inline bool LessThanZ( const LightVertex& a, const LightVertex& b ) { return a.ptPosition.z < b.ptPosition.z; }

	//
	// Left-balanced KD-tree median computation.
	// Lifted byte-for-byte from PhotonMapCore::BalanceSegment so
	// the VCM store builds trees with identical memory layout.
	//
	inline int ComputeMedian( const int from, const int to )
	{
		int median = 1;
		while( ( 4 * median ) <= ( to - from + 1 ) ) {
			median += median;
		}
		if( ( 3 * median ) <= ( to - from + 1 ) ) {
			median += median;
			median += from - 1;
		} else {
			median = to - median + 1;
		}
		return median;
	}

	//
	// Recursive balance of a [from, to] index range.  BoundingBox
	// is passed by reference and mutated in place so each split
	// sees the tightened box extents, matching PhotonMapCore's
	// tmp-swap pattern.
	//
	void BalanceSegment(
		std::vector<LightVertex>& verts,
		BoundingBox& bbox,
		const int from,
		const int to
		)
	{
		if( to - from <= 0 ) {
			return;
		}

		unsigned char axis = 2;
		const Vector3& extents = bbox.GetExtents();
		if( extents.x > extents.y && extents.x > extents.z ) {
			axis = 0;
		} else if( extents.y > extents.z ) {
			axis = 1;
		}

		const int median = ComputeMedian( from, to );

		// Note: this uses the correct end-exclusive iterator
		// begin()+to+1 so the element at index 'to' is included in
		// the partition.  PhotonMapCore::BalanceSegment uses
		// begin()+to (end-exclusive of (to-1)), which silently drops
		// the last element at each level.  The clone fixes that so
		// every stored vertex has its splitting plane correctly set
		// and queries never miss it.  The unit test validates query
		// correctness against a brute-force ground truth rather than
		// against PhotonMapCore directly.
		switch( axis )
		{
		case 0:
			std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanX );
			break;
		case 1:
			std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanY );
			break;
		case 2:
		default:
			std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanZ );
			break;
		}

		verts[median].plane = axis;

		{
			// Left segment: tighten the upper bound.
			const Scalar tmp = bbox.ur[axis];
			bbox.ur[axis] = verts[median].ptPosition[axis];
			BalanceSegment( verts, bbox, from, median - 1 );
			bbox.ur[axis] = tmp;
		}

		{
			// Right segment: tighten the lower bound.
			const Scalar tmp = bbox.ll[axis];
			bbox.ll[axis] = verts[median].ptPosition[axis];
			BalanceSegment( verts, bbox, median + 1, to );
			bbox.ll[axis] = tmp;
		}
	}

	//
	// Fixed-radius query.  Straight translation of
	// PhotonMapCore::LocateAllPhotons.  'maxDist' is a SQUARED
	// radius (matches the PhotonMap convention).
	//
	void LocateAllInRadiusSq(
		const std::vector<LightVertex>& verts,
		const Point3& loc,
		const Scalar maxDistSq,
		std::vector<LightVertex>& out,
		const int from,
		const int to
		)
	{
		if( to - from < 0 ) {
			return;
		}

		const int median = ComputeMedian( from, to );

		const Vector3 v = Vector3Ops::mkVector3( loc, verts[median].ptPosition );
		const Scalar distanceToVertexSq = Vector3Ops::SquaredModulus( v );

		if( distanceToVertexSq < maxDistSq ) {
			out.push_back( verts[median] );
		}

		const int axis = verts[median].plane;
		const Scalar planeDelta = loc[axis] - verts[median].ptPosition[axis];
		const Scalar planeDeltaSq = planeDelta * planeDelta;

		// Pruning: if the query sphere doesn't cross the splitting
		// plane, only the nearer subtree can contain hits.
		// Otherwise we must visit both sides.  The original
		// PhotonMapCore code uses strict '<' on both comparisons,
		// which leaves the tangent case (planeDeltaSq == maxDistSq)
		// visiting NEITHER subtree and losing every vertex behind
		// the tangent node.  On a regular grid queried at an
		// integer coordinate this drops exactly those vertices
		// whose perpendicular distance to a splitting plane
		// matches the query radius.  The fix is to put '<=' on
		// the recurse-both-sides branch so tangent fires the
		// conservative path.
		if( planeDeltaSq > maxDistSq ) {
			if( planeDelta <= 0 ) {
				LocateAllInRadiusSq( verts, loc, maxDistSq, out, from, median - 1 );
			} else {
				LocateAllInRadiusSq( verts, loc, maxDistSq, out, median + 1, to );
			}
		} else {
			LocateAllInRadiusSq( verts, loc, maxDistSq, out, from, median - 1 );
			LocateAllInRadiusSq( verts, loc, maxDistSq, out, median + 1, to );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// LightVertexStore
//////////////////////////////////////////////////////////////////////

LightVertexStore::LightVertexStore() :
	mBuilt( false )
{
}

LightVertexStore::~LightVertexStore()
{
}

void LightVertexStore::Clear()
{
	mVertices.clear();
	mBuilt = false;
}

void LightVertexStore::Reserve( const std::size_t capacity )
{
	mVertices.reserve( capacity );
}

void LightVertexStore::Append( const LightVertex& v )
{
	mVertices.push_back( v );
	mBuilt = false;
}

void LightVertexStore::Concat( std::vector<LightVertex>&& localBuffer )
{
	// Always insert — never move-assign the whole source over mVertices,
	// even when mVertices.empty() is true.
	//
	// Why: the "empty" fast path historically did
	//   mVertices = std::move(localBuffer)
	// which deallocates mVertices' existing capacity buffer (allocated
	// by the caller via Reserve) and steals localBuffer's buffer.  In
	// the GUI build that deallocation occasionally crashed inside
	// libsystem_malloc on the second+ animation frame (PAC-tagged
	// indirect branch to an invalid address during
	// xzm_segment_group_free_chunk).  The pattern — deallocate a large
	// main-thread buffer between frames from code running on a Swift
	// cooperative-pool thread — seems to hit an allocator edge case.
	//
	// insert() preserves the Reserve'd capacity (zero reallocation when
	// the pre-reserved size is sufficient), does an element-wise move
	// of the POD-ish LightVertex structs, and — crucially — never
	// frees the destination's buffer.  That sidesteps the crash.
	if( localBuffer.empty() ) {
		mBuilt = false;
		return;
	}
	mVertices.insert(
		mVertices.end(),
		std::make_move_iterator( localBuffer.begin() ),
		std::make_move_iterator( localBuffer.end() ) );
	localBuffer.clear();
	mBuilt = false;
}

void LightVertexStore::BuildKDTree()
{
	if( mVertices.empty() ) {
		mBuilt = true;
		return;
	}

	// Build the enclosing bounding box before we start splitting.
	BoundingBox bbox(
		Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
		Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
	for( std::vector<LightVertex>::const_iterator i = mVertices.begin(); i != mVertices.end(); ++i ) {
		bbox.Include( i->ptPosition );
	}

	BalanceSegment( mVertices, bbox, 0, static_cast<int>( mVertices.size() ) - 1 );
	mBuilt = true;
}

void LightVertexStore::Query(
	const Point3& center,
	const Scalar radiusSq,
	std::vector<LightVertex>& out
	) const
{
	if( !mBuilt || mVertices.empty() || radiusSq <= 0 ) {
		return;
	}
	LocateAllInRadiusSq(
		mVertices,
		center,
		radiusSq,
		out,
		0,
		static_cast<int>( mVertices.size() ) - 1 );
}

//////////////////////////////////////////////////////////////////////
// Parallel KD-tree build.
//
// Strategy:
//   - Compute the root median serially; partition the array around it.
//   - Recurse on the two disjoint halves in parallel via the thread pool.
//   - Below a cutoff threshold, drop into the existing serial BalanceSegment.
//
// Correctness invariant:
//   Every recursion operates on a disjoint index range of mVertices
//   and its own copy of the bbox.  The shared vector is resized once
//   up front; no thread appends or removes elements after that.  All
//   writes are to cells owned by the recursing task.  Therefore no
//   locking on mVertices is needed.
//
//   The tree layout (which vertex lives at which array slot) is
//   determined entirely by:
//     1) the sequence of median-index computations (deterministic),
//     2) std::nth_element's output for a given comparator (deterministic
//        though partition order of non-median elements is
//        implementation-defined — but LocateAllInRadiusSq only reads
//        each element via the tree traversal so partition order of
//        non-medians within a subtree does NOT affect query results).
//   So BuildKDTreeParallel produces a tree that is functionally
//   equivalent to BuildKDTree (not byte-identical in non-median slots,
//   but identical-for-queries).  The unit test asserts query-equivalence
//   against a brute-force oracle rather than memcmp.
//////////////////////////////////////////////////////////////////////
namespace
{
	// Copy of BalanceSegment but queues sub-ranges into the pool
	// until they fall below the cutoff.  outstanding tracks the
	// number of pending subtree tasks so the caller can wait.
	void BalanceSegmentParallel(
		std::vector<LightVertex>& verts,
		BoundingBox bbox,               // by-value: each task has its own
		const int from,
		const int to,
		const std::size_t cutoff,
		ThreadPool& pool,
		std::atomic<unsigned int>& outstanding,
		std::mutex& doneMut,
		std::condition_variable& doneCv
		)
	{
		if( to - from <= 0 ) {
			return;
		}

		if( static_cast<std::size_t>( to - from + 1 ) <= cutoff ) {
			// Small enough — drop to the existing serial path.
			BalanceSegment( verts, bbox, from, to );
			return;
		}

		unsigned char axis = 2;
		const Vector3& extents = bbox.GetExtents();
		if( extents.x > extents.y && extents.x > extents.z ) {
			axis = 0;
		} else if( extents.y > extents.z ) {
			axis = 1;
		}

		const int median = ComputeMedian( from, to );

		switch( axis )
		{
		case 0: std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanX ); break;
		case 1: std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanY ); break;
		default: std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanZ ); break;
		}

		verts[median].plane = axis;

		// Prepare child bboxes (by value — each task owns its own).
		BoundingBox leftBox  = bbox;
		BoundingBox rightBox = bbox;
		leftBox.ur[axis]  = verts[median].ptPosition[axis];
		rightBox.ll[axis] = verts[median].ptPosition[axis];

		// Fire off the left subtree as a pool task, recurse on right
		// in this thread so we always keep one active path of work.
		outstanding.fetch_add( 1, std::memory_order_relaxed );
		pool.Submit( [&verts, leftBox, from, median, cutoff, &pool, &outstanding, &doneMut, &doneCv] {
			BalanceSegmentParallel( verts, leftBox, from, median - 1, cutoff,
				pool, outstanding, doneMut, doneCv );
			if( outstanding.fetch_sub( 1, std::memory_order_acq_rel ) == 1 ) {
				{
					std::lock_guard<std::mutex> lk( doneMut );
				}
				doneCv.notify_all();
			}
		} );

		// Right subtree on the current task.
		BalanceSegmentParallel( verts, rightBox, median + 1, to, cutoff,
			pool, outstanding, doneMut, doneCv );
	}
}

void LightVertexStore::BuildKDTreeParallel()
{
	if( mVertices.empty() ) {
		mBuilt = true;
		return;
	}

	// Enclosing bbox — single-threaded (cheap).
	BoundingBox bbox(
		Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
		Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
	for( std::vector<LightVertex>::const_iterator i = mVertices.begin(); i != mVertices.end(); ++i ) {
		bbox.Include( i->ptPosition );
	}

	ThreadPool& pool = GlobalThreadPool();
	const std::size_t N = mVertices.size();

	// Cutoff: keep ~8 tasks per worker so there's slack for work
	// stealing while not over-decomposing for tiny inputs.
	const std::size_t cutoff = std::max<std::size_t>(
		4096,
		N / ( pool.NumWorkers() * 8 ) );

	// Small input — skip the pool.
	if( N <= cutoff ) {
		BalanceSegment( mVertices, bbox, 0, static_cast<int>( N ) - 1 );
		mBuilt = true;
		return;
	}

	std::atomic<unsigned int> outstanding( 1 );
	std::mutex doneMut;
	std::condition_variable doneCv;

	pool.Submit( [this, bbox, N, cutoff, &pool, &outstanding, &doneMut, &doneCv] {
		BalanceSegmentParallel( mVertices, bbox, 0, static_cast<int>( N ) - 1, cutoff,
			pool, outstanding, doneMut, doneCv );
		if( outstanding.fetch_sub( 1, std::memory_order_acq_rel ) == 1 ) {
			{
				std::lock_guard<std::mutex> lk( doneMut );
			}
			doneCv.notify_all();
		}
	} );

	std::unique_lock<std::mutex> lk( doneMut );
	doneCv.wait( lk, [&outstanding] {
		return outstanding.load( std::memory_order_acquire ) == 0;
	} );

	mBuilt = true;
}

Scalar LightVertexStore::ComputeBBoxDiagonal() const
{
	if( mVertices.empty() ) {
		return 0;
	}

	Point3 mn = mVertices[0].ptPosition;
	Point3 mx = mn;
	for( std::size_t i = 1; i < mVertices.size(); i++ ) {
		const Point3& p = mVertices[i].ptPosition;
		if( p.x < mn.x ) mn.x = p.x;  if( p.x > mx.x ) mx.x = p.x;
		if( p.y < mn.y ) mn.y = p.y;  if( p.y > mx.y ) mx.y = p.y;
		if( p.z < mn.z ) mn.z = p.z;  if( p.z > mx.z ) mx.z = p.z;
	}

	const Vector3 d = Vector3Ops::mkVector3( mx, mn );
	return Vector3Ops::Magnitude( d );
}

Scalar LightVertexStore::ComputeBBoxSurfaceArea() const
{
	if( mVertices.empty() ) {
		return 0;
	}

	Point3 mn = mVertices[0].ptPosition;
	Point3 mx = mn;
	for( std::size_t i = 1; i < mVertices.size(); i++ ) {
		const Point3& p = mVertices[i].ptPosition;
		if( p.x < mn.x ) mn.x = p.x;  if( p.x > mx.x ) mx.x = p.x;
		if( p.y < mn.y ) mn.y = p.y;  if( p.y > mx.y ) mx.y = p.y;
		if( p.z < mn.z ) mn.z = p.z;  if( p.z > mx.z ) mx.z = p.z;
	}

	const Scalar dx = mx.x - mn.x;
	const Scalar dy = mx.y - mn.y;
	const Scalar dz = mx.z - mn.z;
	return Scalar( 2 ) * ( dx * dy + dy * dz + dz * dx );
}

namespace
{
	// Rec. 709 luminance — matches RISEPelToNMProxy in VCMIntegrator.cpp
	// so the clamp ranks photons consistently with the spectral merge
	// path's projection.
	inline Scalar LightVertexLuminance( const RISEPel& t )
	{
		return Scalar( 0.2126 ) * t[0]
		     + Scalar( 0.7152 ) * t[1]
		     + Scalar( 0.0722 ) * t[2];
	}
}

void LightVertexStore::ClampOutlierThroughputs(
	const Scalar percentile,
	const Scalar multiplier
	)
{
	if( mVertices.empty() ) {
		return;
	}
	if( multiplier <= 0 || percentile <= 0 || percentile >= 1 ) {
		return;
	}

	// Build a flat luminance array for std::nth_element.  Using the
	// store's mVertices directly would let nth_element reorder photons,
	// breaking later index-based access; the auxiliary copy is the
	// only correct option.
	const std::size_t n = mVertices.size();
	std::vector<Scalar> lums;
	lums.reserve( n );
	for( std::size_t i = 0; i < n; i++ ) {
		lums.push_back( LightVertexLuminance( mVertices[i].throughput ) );
	}

	// Find the percentile via nth_element (O(n) average).  Clamp the
	// index to [0, n-1] so percentile=1.0 doesn't run off the end.
	std::size_t k = static_cast<std::size_t>( percentile * static_cast<double>( n ) );
	if( k >= n ) k = n - 1;
	std::nth_element( lums.begin(), lums.begin() + k, lums.end() );
	const Scalar pctValue = lums[k];

	// All photons effectively dark — nothing to clamp.  Skip the
	// rescale loop to avoid divide-by-near-zero amplification.
	if( pctValue <= NEARZERO ) {
		return;
	}

	const Scalar threshold = multiplier * pctValue;

	// Single pass to rescale outliers.  Color is preserved (uniform
	// scale) so chromaticity stays the same; only the magnitude is
	// capped.  Vertices already at or below threshold are untouched.
	for( std::size_t i = 0; i < n; i++ ) {
		const Scalar lum = LightVertexLuminance( mVertices[i].throughput );
		if( lum > threshold ) {
			const Scalar scale = threshold / lum;
			mVertices[i].throughput = mVertices[i].throughput * scale;
		}
	}
}
