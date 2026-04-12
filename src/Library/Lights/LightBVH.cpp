//////////////////////////////////////////////////////////////////////
//
//  LightBVH.cpp - Light Bounding Volume Hierarchy implementation.
//
//  See LightBVH.h for algorithm documentation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LightBVH.h"
#include "LightSampler.h"
#include "../Interfaces/ILight.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IEmitter.h"
#include <algorithm>
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  OrientationCone
// ============================================================

OrientationCone OrientationCone::FullSphere()
{
	OrientationCone c;
	c.axis = Vector3( 0, 1, 0 );
	c.halfAngle = PI;
	return c;
}

OrientationCone OrientationCone::FromDirection(
	const Vector3& dir
	)
{
	OrientationCone c;
	c.axis = Vector3Ops::Normalize( dir );
	c.halfAngle = 0;
	return c;
}

OrientationCone OrientationCone::Merge(
	const OrientationCone& a,
	const OrientationCone& b
	)
{
	// If either is already full sphere, result is full sphere
	if( a.halfAngle >= PI ) {
		return FullSphere();
	}
	if( b.halfAngle >= PI ) {
		return FullSphere();
	}

	// Angle between the two axes
	const Scalar cosD = Vector3Ops::Dot( a.axis, b.axis );
	const Scalar thetaD = acos( std::max( Scalar(-1), std::min( Scalar(1), cosD ) ) );

	// Check if one cone contains the other
	if( thetaD + b.halfAngle <= a.halfAngle + Scalar(1e-10) ) {
		return a;
	}
	if( thetaD + a.halfAngle <= b.halfAngle + Scalar(1e-10) ) {
		return b;
	}

	// Merged half-angle
	const Scalar thetaO = (a.halfAngle + thetaD + b.halfAngle) * Scalar(0.5);
	if( thetaO >= PI ) {
		return FullSphere();
	}

	// Rotate a.axis toward b.axis by (thetaO - a.halfAngle)
	const Scalar rotAngle = thetaO - a.halfAngle;

	// Rotation axis is cross(a.axis, b.axis), normalized
	Vector3 rotAxis = Vector3Ops::Cross( a.axis, b.axis );
	const Scalar rotAxisLen = Vector3Ops::Magnitude( rotAxis );

	if( rotAxisLen < Scalar(1e-12) )
	{
		// Axes are (anti-)parallel.  If antiparallel, we already
		// handled containment above, so the merged cone is full sphere
		// if they point opposite.
		if( cosD < 0 ) {
			return FullSphere();
		}
		// Same direction: result is the larger cone
		return (a.halfAngle >= b.halfAngle) ? a : b;
	}

	rotAxis = rotAxis * (Scalar(1) / rotAxisLen);

	// Rodrigues rotation of a.axis around rotAxis by rotAngle
	const Scalar cosR = cos( rotAngle );
	const Scalar sinR = sin( rotAngle );
	const Scalar dotRA = Vector3Ops::Dot( rotAxis, a.axis );
	const Vector3 crossRA = Vector3Ops::Cross( rotAxis, a.axis );

	OrientationCone result;
	result.axis = Vector3(
		a.axis.x * cosR + crossRA.x * sinR + rotAxis.x * dotRA * (1 - cosR),
		a.axis.y * cosR + crossRA.y * sinR + rotAxis.y * dotRA * (1 - cosR),
		a.axis.z * cosR + crossRA.z * sinR + rotAxis.z * dotRA * (1 - cosR)
	);
	result.axis = Vector3Ops::Normalize( result.axis );
	result.halfAngle = thetaO;

	return result;
}

// ============================================================
//  LightBVH
// ============================================================

LightBVH::LightBVH()
{
}

LightBVH::~LightBVH()
{
}

Scalar LightBVH::DistSqToAABB(
	const Point3& p,
	const BoundingBox& bbox
	)
{
	Scalar distSq = 0;

	// X axis
	if( p.x < bbox.ll.x ) {
		const Scalar d = bbox.ll.x - p.x;
		distSq += d * d;
	} else if( p.x > bbox.ur.x ) {
		const Scalar d = p.x - bbox.ur.x;
		distSq += d * d;
	}

	// Y axis
	if( p.y < bbox.ll.y ) {
		const Scalar d = bbox.ll.y - p.y;
		distSq += d * d;
	} else if( p.y > bbox.ur.y ) {
		const Scalar d = p.y - bbox.ur.y;
		distSq += d * d;
	}

	// Z axis
	if( p.z < bbox.ll.z ) {
		const Scalar d = bbox.ll.z - p.z;
		distSq += d * d;
	} else if( p.z > bbox.ur.z ) {
		const Scalar d = p.z - bbox.ur.z;
		distSq += d * d;
	}

	return distSq;
}

Scalar LightBVH::NodeImportance(
	const LightBVHNode& node,
	const Point3& shadingPoint
	) const
{
	if( node.power <= 0 ) {
		return 0;
	}

	// Distance-based importance
	static const Scalar minDistSq = Scalar(1e-4);
	const Scalar distSq = DistSqToAABB( shadingPoint, node.bounds );
	const Scalar geometricFactor = Scalar(1) / std::max( distSq, minDistSq );

	// Orientation-based importance
	Scalar orientationFactor = Scalar(1);

	if( node.coneHalfAngle < PI - Scalar(1e-6) )
	{
		const Point3 center = node.bounds.GetCenter();
		Vector3 d = Vector3(
			shadingPoint.x - center.x,
			shadingPoint.y - center.y,
			shadingPoint.z - center.z
		);
		const Scalar dLen = Vector3Ops::Magnitude( d );

		if( dLen > Scalar(1e-10) )
		{
			d = d * (Scalar(1) / dLen);

			// Emission goes from light toward shading point, so we
			// check the angle between the emission direction (d, from
			// light center toward shading point) and the cone axis.
			// The cone axis represents the average emission direction.
			const Scalar cosTheta = Vector3Ops::Dot( d, node.coneAxis );
			const Scalar theta = acos( std::max( Scalar(-1), std::min( Scalar(1), cosTheta ) ) );
			const Scalar thetaPrime = std::max( Scalar(0), theta - node.coneHalfAngle );

			orientationFactor = std::max( Scalar(0), cos( thetaPrime ) );
		}
	}

	return node.power * geometricFactor * orientationFactor;
}

// ============================================================
//  Build
// ============================================================

void LightBVH::Build(
	const std::vector<LightEntry>& lightEntries,
	const LuminaryManager::LuminariesList& luminaries
	)
{
	nodes.clear();
	bvhOrder.clear();
	lightToBvhPos.clear();

	const unsigned int nLights = (unsigned int)lightEntries.size();
	if( nLights == 0 ) {
		return;
	}

	// Collect per-light build primitives
	std::vector<BuildPrimitive> prims( nLights );

	for( unsigned int i = 0; i < nLights; i++ )
	{
		const LightEntry& entry = lightEntries[i];
		BuildPrimitive& prim = prims[i];
		prim.origIndex = i;
		prim.power = entry.exitance;

		if( entry.pLight )
		{
			// Non-mesh light (point, spot, directional)
			prim.bounds = BoundingBox( entry.position, entry.position );
			prim.bounds.EnsureBoxHasVolume();

			// Query orientation from light interface
			prim.cone.axis = entry.pLight->emissionDirection();
			prim.cone.halfAngle = entry.pLight->emissionConeHalfAngle();
		}
		else
		{
			// Mesh luminary
			if( entry.lumIndex < (unsigned int)luminaries.size() )
			{
				const IObject* pObj = luminaries[entry.lumIndex].pLum;
				prim.bounds = pObj->getBoundingBox();
				prim.bounds.EnsureBoxHasVolume();
			}
			else
			{
				prim.bounds = BoundingBox( entry.position, entry.position );
				prim.bounds.EnsureBoxHasVolume();
			}

			// Mesh luminaries: conservative full-sphere cone
			// (correct but suboptimal — normals vary across the surface)
			prim.cone = OrientationCone::FullSphere();
		}

		prim.centroid = prim.bounds.GetCenter();
	}

	// Reserve nodes (upper bound: 2*N - 1 for a full binary tree)
	nodes.reserve( 2 * nLights );
	bvhOrder.reserve( nLights );

	unsigned int bvhPos = 0;
	BuildRecursive( prims, 0, nLights, bvhPos );

	// Build inverse permutation
	lightToBvhPos.resize( nLights, 0 );
	for( unsigned int i = 0; i < (unsigned int)bvhOrder.size(); i++ )
	{
		lightToBvhPos[bvhOrder[i]] = i;
	}
}

unsigned int LightBVH::BuildRecursive(
	std::vector<BuildPrimitive>& prims,
	unsigned int start,
	unsigned int end,
	unsigned int& bvhPos
	)
{
	const unsigned int nPrims = end - start;

	// Allocate a node
	const unsigned int nodeIdx = (unsigned int)nodes.size();
	nodes.push_back( LightBVHNode() );

	LightBVHNode& node = nodes[nodeIdx];
	node.bvhStart = bvhPos;
	node.nLights = nPrims;

	// Compute bounding box, total power, and merged orientation cone
	node.bounds = prims[start].bounds;
	node.power = prims[start].power;
	OrientationCone mergedCone = prims[start].cone;

	for( unsigned int i = start + 1; i < end; i++ )
	{
		node.bounds.Include( prims[i].bounds );
		node.power += prims[i].power;
		mergedCone = OrientationCone::Merge( mergedCone, prims[i].cone );
	}

	node.coneAxis = mergedCone.axis;
	node.coneHalfAngle = mergedCone.halfAngle;

	if( nPrims == 1 )
	{
		// Leaf node
		node.lightIndex = prims[start].origIndex;
		node.childOffset = 0;

		// Record BVH order
		bvhOrder.push_back( prims[start].origIndex );
		bvhPos++;

		return nodeIdx;
	}

	// Interior node: find split axis and position
	// Use axis with largest centroid extent
	BoundingBox centroidBounds(
		prims[start].centroid,
		prims[start].centroid
	);
	for( unsigned int i = start + 1; i < end; i++ ) {
		centroidBounds.Include( prims[i].centroid );
	}

	const Vector3 extent = centroidBounds.GetExtents();
	int splitAxis = 0;
	if( extent.y > extent.x ) {
		splitAxis = 1;
	}
	if( extent.z > (splitAxis == 0 ? extent.x : extent.y) ) {
		splitAxis = 2;
	}

	// Check for degenerate case (all centroids coincide)
	const Scalar axisExtent = (splitAxis == 0) ? extent.x :
		(splitAxis == 1) ? extent.y : extent.z;

	unsigned int mid;

	if( axisExtent < Scalar(1e-10) )
	{
		// Degenerate: split in half
		mid = start + nPrims / 2;
	}
	else
	{
		// Power-weighted centroid midpoint
		Scalar weightedSum = 0;
		Scalar totalPower = 0;
		for( unsigned int i = start; i < end; i++ )
		{
			const Scalar coord = (splitAxis == 0) ? prims[i].centroid.x :
				(splitAxis == 1) ? prims[i].centroid.y : prims[i].centroid.z;
			weightedSum += coord * prims[i].power;
			totalPower += prims[i].power;
		}

		const Scalar splitPos = (totalPower > 0) ?
			(weightedSum / totalPower) :
			((splitAxis == 0) ? centroidBounds.GetCenter().x :
			 (splitAxis == 1) ? centroidBounds.GetCenter().y :
			 centroidBounds.GetCenter().z);

		// Partition around split position
		auto midIt = std::partition(
			prims.begin() + start,
			prims.begin() + end,
			[splitAxis, splitPos]( const BuildPrimitive& p ) {
				const Scalar coord = (splitAxis == 0) ? p.centroid.x :
					(splitAxis == 1) ? p.centroid.y : p.centroid.z;
				return coord < splitPos;
			}
		);

		mid = (unsigned int)std::distance( prims.begin(), midIt );

		// If partition puts everything on one side, force a balanced split
		if( mid == start || mid == end ) {
			mid = start + nPrims / 2;
		}
	}

	// Build left child (immediately follows this node in preorder).
	// Note: the 'node' reference is invalidated after this call
	// because BuildRecursive pushes to 'nodes', potentially
	// reallocating the vector.
	BuildRecursive( prims, start, mid, bvhPos );

	// Record right child offset (re-index into nodes since the
	// vector may have been reallocated by the left-child build).
	nodes[nodeIdx].childOffset = (unsigned int)nodes.size();

	BuildRecursive( prims, mid, end, bvhPos );

	return nodeIdx;
}

// ============================================================
//  Sample
// ============================================================

unsigned int LightBVH::Sample(
	const Point3& shadingPoint,
	const Vector3& shadingNormal,
	Scalar xi,
	Scalar& pdf
	) const
{
	pdf = Scalar(1);

	if( nodes.empty() ) {
		pdf = 0;
		return 0;
	}

	unsigned int nodeIdx = 0;

	while( nodes[nodeIdx].nLights > 1 )
	{
		const LightBVHNode& node = nodes[nodeIdx];

		// Left child is at nodeIdx + 1, right child at node.childOffset
		const unsigned int leftIdx = nodeIdx + 1;
		const unsigned int rightIdx = node.childOffset;

		const Scalar impL = NodeImportance( nodes[leftIdx], shadingPoint );
		const Scalar impR = NodeImportance( nodes[rightIdx], shadingPoint );
		const Scalar total = impL + impR;

		if( total <= 0 )
		{
			// No importance — fall through to first leaf in subtree.
			// This shouldn't happen with nonzero-power lights.
			pdf = 0;
			return bvhOrder[node.bvhStart];
		}

		const Scalar probL = impL / total;

		if( xi < probL )
		{
			// Select left child
			pdf *= probL;
			xi = xi / probL;  // Rescale for next level
			nodeIdx = leftIdx;
		}
		else
		{
			// Select right child
			pdf *= (Scalar(1) - probL);
			xi = (xi - probL) / (Scalar(1) - probL);  // Rescale
			nodeIdx = rightIdx;
		}
	}

	return nodes[nodeIdx].lightIndex;
}

// ============================================================
//  Pdf
// ============================================================

Scalar LightBVH::Pdf(
	unsigned int lightIdx,
	const Point3& shadingPoint,
	const Vector3& shadingNormal
	) const
{
	if( nodes.empty() || lightIdx >= (unsigned int)lightToBvhPos.size() ) {
		return 0;
	}

	const unsigned int targetBvhPos = lightToBvhPos[lightIdx];

	Scalar pdf = Scalar(1);
	unsigned int nodeIdx = 0;

	while( nodes[nodeIdx].nLights > 1 )
	{
		const LightBVHNode& node = nodes[nodeIdx];
		const unsigned int leftIdx = nodeIdx + 1;
		const unsigned int rightIdx = node.childOffset;

		const Scalar impL = NodeImportance( nodes[leftIdx], shadingPoint );
		const Scalar impR = NodeImportance( nodes[rightIdx], shadingPoint );
		const Scalar total = impL + impR;

		if( total <= 0 ) {
			return 0;
		}

		const Scalar probL = impL / total;

		// Determine which child contains the target light
		const LightBVHNode& leftNode = nodes[leftIdx];
		const bool inLeft = (targetBvhPos >= leftNode.bvhStart &&
			targetBvhPos < leftNode.bvhStart + leftNode.nLights);

		if( inLeft )
		{
			pdf *= probL;
			nodeIdx = leftIdx;
		}
		else
		{
			pdf *= (Scalar(1) - probL);
			nodeIdx = rightIdx;
		}
	}

	return pdf;
}
