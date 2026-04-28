//////////////////////////////////////////////////////////////////////
//
//  AccelerationConfig.h - Tuning parameters for the BVH builder.
//
//  Phase 1 partial delivery (per docs/BVH_ACCELERATION_PLAN.md §5):
//    - SAH binned BVH2 only.  SBVH spatial splits, refit, and
//      compressed nodes are deferred to follow-up sessions.
//    - All fields explicit; no defaults in the header per project
//      convention (see memory/feedback_no_default_params.md).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_ACCELERATION_CONFIG_
#define RISE_ACCELERATION_CONFIG_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	struct AccelerationConfig
	{
		//! Maximum triangles per BVH leaf.  Smaller = deeper tree, faster
		//! traversal at cost of more nodes; larger = shallower, fewer
		//! nodes, more leaf intersection work per ray.  Typical range
		//! 2-8.  Set explicitly per build.
		unsigned int   maxLeafSize;

		//! Number of SAH bins per axis during top-down construction.
		//! 32 is the common production default; 16 is faster to build
		//! at slightly worse quality.
		unsigned int   binCount;

		//! Surface-area heuristic constants.  C_trav = node traversal
		//! cost relative to a single leaf intersection; C_inter = leaf
		//! intersection cost.  Production-typical: C_trav=1.0, C_inter=1.0
		//! (the SAH cost is dimensionless after normalization).  The
		//! ratio is what matters; classical values 1.0/1.0 work well.
		Scalar         sahTraversalCost;
		Scalar         sahIntersectionCost;

		//! Both-sided geometry flag.  Propagated to the leaf intersection
		//! call so a polygon with no back-face culling is hit from either
		//! side.  This was previously plumbed via the mesh's bDoubleSided
		//! field; AccelerationConfig holds the canonical copy now so the
		//! BVH builder doesn't need to ask the mesh.
		bool           doubleSided;

		//! Tier 1 §1 — Build a Spatial-BVH (SBVH) instead of plain SAH BVH.
		//! When enabled, the builder tries both object-split and spatial-
		//! split SAH at each node and picks whichever has lower cost,
		//! subject to the duplication budget.  Spatial splits duplicate
		//! straddling primitives across the split, with each duplicate's
		//! bbox clipped against the split plane.
		//!
		//! Spatial splits typically improve traversal speed by 10–30 %
		//! on non-uniform geometry (where centroid-based object splits
		//! produce overlapping child bboxes) at the cost of a longer
		//! build (2–3× the plain-SAH builder) and modestly more memory
		//! (proportional to duplication budget).
		bool           buildSBVH;

		//! Maximum allowed duplication of primitive references during
		//! SBVH construction, expressed as a fraction of the input
		//! primitive count.  E.g. 0.30 = at most 30 % more references
		//! than triangles.  Once exceeded, the builder reverts to
		//! object-split-only for all subsequent decisions.  Ignored
		//! when buildSBVH is false.
		Scalar         sbvhDuplicationBudget;
	};
}

#endif
