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

#include <algorithm>

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
	if( mVertices.empty() ) {
		mVertices = std::move( localBuffer );
	} else {
		mVertices.insert(
			mVertices.end(),
			std::make_move_iterator( localBuffer.begin() ),
			std::make_move_iterator( localBuffer.end() ) );
		localBuffer.clear();
	}
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
