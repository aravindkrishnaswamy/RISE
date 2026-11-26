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

		//! NOTE: a Spatial-BVH (SBVH) builder lived here in Tier 1 §1
		//! with two extra fields (`buildSBVH`, `sbvhDuplicationBudget`).
		//! Both were excised in Tier B (2026-04-27) after measurement
		//! showed no statistically significant rasterize-time benefit
		//! at any duplication budget on the canonical 7.2M-tri xyzdragon
		//! stress mesh.  See docs/BVH_RETROSPECTIVE.md → Tier B for
		//! the data and decision.  If a future codebase change shifts
		//! the tradeoff (e.g. very different scene geometry, or
		//! algorithmic changes that make leaf-overlap a bigger cost
		//! factor), reintroduce by reverting that commit and the
		//! BVH.h SBVH block.
	};
}

#endif
