//////////////////////////////////////////////////////////////////////
//
//  LightBVH.h - Light Bounding Volume Hierarchy for importance-
//  weighted many-light sampling.
//
//  Builds an acceleration structure over all emitters (non-mesh
//  lights and mesh luminaries) that enables O(log N) importance-
//  weighted light selection with a fully evaluable PDF for MIS.
//
//  CONSTRUCTION:
//  Build() takes the LightSampler's lightEntries vector and the
//  luminaries list.  For each entry it extracts a bounding box,
//  an orientation cone (axis + half-angle bounding the emitter
//  normals), and a scalar power (exitance).  A top-down recursive
//  partitioning builds a binary BVH stored in a flat preorder
//  array.  At each level the axis with the largest centroid extent
//  is chosen, and the primitives are split at the power-weighted
//  centroid midpoint.  Leaves contain exactly one light.
//
//  ORIENTATION CONE:
//  An OrientationCone describes the bounding set of emission
//  directions within a subtree.  It has a central axis (unit
//  vector) and a half-angle (radians).  PI means full-sphere
//  (isotropic, e.g. point lights).  Two cones are merged
//  following Conty & Kulla (2018): the merged cone is the
//  smallest cone containing both input cones.
//
//  IMPORTANCE FUNCTION:
//  At an internal node for a shading point p:
//
//    importance = power * geometric_factor * orientation_factor
//
//  geometric_factor = 1 / max(distSq(p, closestPointOnBBox), eps)
//  If p is inside the bbox, geometric_factor = 1/eps (max).
//
//  orientation_factor:
//    For full-sphere cones: 1.0.
//    Otherwise: cos(max(0, theta - coneHalfAngle)) where theta
//    is the angle between -d (d = direction from bbox center to p)
//    and the cone axis.  This bounds the maximum cosine-weighted
//    emission toward p.
//
//  TRAVERSAL (Sample):
//  From the root, at each internal node compute importance for
//  the left and right children.  Stochastically select one child
//  proportional to its importance, accumulating the product of
//  selection probabilities.  A single uniform random number is
//  used, rescaled at each level to preserve stratification (the
//  "random number recycling" technique from pbrt-v4).  At a
//  leaf, the selected light index and overall PDF are returned.
//
//  PDF EVALUATION (Pdf):
//  For MIS, the probability that the BVH would select a given
//  light from a given shading point must be evaluable.  Pdf()
//  walks from the root to the target leaf, computing the same
//  importance ratios and multiplying the branch probability at
//  each level.  Each leaf covers a contiguous range in the BVH
//  permutation, so child membership is determined by comparing
//  the target's BVH position against each child's range.
//
//  INTEGRATION WITH LightSampler:
//  The BVH replaces the alias table for NEE light selection when
//  enabled.  SampleLight (BDPT emission) continues to use the
//  alias table for power-proportional selection without spatial
//  bias.  The BVH's evaluable PDF enables full MIS with BSDF
//  sampling, unlike RIS which must disable MIS.
//
//  Reference: Conty & Kulla, "Importance Sampling of Many Lights
//  with Adaptive Tree Splitting", 2018.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LIGHT_BVH_
#define LIGHT_BVH_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/BoundingBox.h"
#include "../Rendering/LuminaryManager.h"
#include <vector>

namespace RISE
{
	class ILightPriv;

	namespace Implementation
	{
		/// Bounding cone of emission directions.
		/// axis is a unit vector; halfAngle is in radians.
		/// halfAngle >= PI means full sphere (isotropic).
		struct OrientationCone
		{
			Vector3	axis;
			Scalar	halfAngle;

			/// Merge two orientation cones into the smallest
			/// bounding cone that contains both.
			/// Follows Conty & Kulla (2018).
			static OrientationCone Merge(
				const OrientationCone& a,
				const OrientationCone& b
				);

			/// Create a full-sphere (isotropic) cone.
			static OrientationCone FullSphere();

			/// Create a cone from a single direction (zero half-angle).
			static OrientationCone FromDirection(
				const Vector3& dir
				);
		};

		/// Forward declaration — LightEntry is defined in LightSampler.h
		/// (public struct in the Implementation namespace)
		struct LightEntry;

		/// A single node in the light BVH, stored in a flat preorder array.
		///
		/// Interior nodes: left child is at index (thisIndex + 1),
		/// right child is at index (childOffset).  nLights > 1.
		///
		/// Leaf nodes: lightIndex is the index into the original
		/// lightEntries array.  nLights == 1.
		struct LightBVHNode
		{
			BoundingBox		bounds;			///< AABB of all emitters in subtree
			Vector3			coneAxis;		///< Orientation cone central axis
			Scalar			coneHalfAngle;	///< Orientation cone half-angle (radians)
			Scalar			power;			///< Total exitance of all emitters in subtree
			unsigned int	childOffset;	///< Interior: index of right child.  Leaf: unused.
			unsigned int	lightIndex;		///< Leaf: index into lightEntries.  Interior: unused.
			unsigned int	nLights;		///< Number of lights in subtree (1 = leaf)
			unsigned int	bvhStart;		///< Start index in BVH-order permutation
		};

		/// Light BVH for importance-weighted many-light sampling.
		class LightBVH
		{
		public:
			LightBVH();
			~LightBVH();

			/// Build the BVH from the LightSampler's light entries.
			/// Must be called once after lightEntries is populated.
			void Build(
				const std::vector<LightEntry>& lightEntries,
				const LuminaryManager::LuminariesList& luminaries
				);

			/// Select one light by importance-weighted stochastic
			/// traversal from the root.
			/// \return Index into lightEntries of the selected light
			unsigned int Sample(
				const Point3& shadingPoint,
				const Vector3& shadingNormal,
				Scalar xi,
				Scalar& pdf
				) const;

			/// Evaluate the probability that Sample() would select
			/// the given light from the given shading point.
			/// \return Selection probability in [0, 1]
			Scalar Pdf(
				unsigned int lightIdx,
				const Point3& shadingPoint,
				const Vector3& shadingNormal
				) const;

			/// \return Number of lights in the BVH
			unsigned int GetLightCount() const { return (unsigned int)bvhOrder.size(); }

			/// \return True if the BVH has been built and is usable
			bool IsBuilt() const { return !nodes.empty(); }

			/// \return Number of nodes in the BVH
			unsigned int NumNodes() const { return (unsigned int)nodes.size(); }

		protected:
			/// Flat preorder array of BVH nodes
			std::vector<LightBVHNode>	nodes;

			/// Permutation: bvhOrder[i] = original lightEntries index
			/// for the i-th leaf in BVH traversal order
			std::vector<unsigned int>	bvhOrder;

			/// Inverse permutation: lightToBvhPos[origIdx] = position
			/// in bvhOrder (for PDF evaluation)
			std::vector<unsigned int>	lightToBvhPos;

			/// Compute the importance of a node for a given shading point.
			/// Used by both Sample() and Pdf().
			Scalar NodeImportance(
				const LightBVHNode& node,
				const Point3& shadingPoint
				) const;

			/// Compute the squared distance from a point to the closest
			/// point on an AABB.  Returns 0 if the point is inside.
			static Scalar DistSqToAABB(
				const Point3& p,
				const BoundingBox& bbox
				);

			/// Primitive data collected during build
			struct BuildPrimitive
			{
				BoundingBox		bounds;
				OrientationCone	cone;
				Scalar			power;
				Point3			centroid;
				unsigned int	origIndex;	///< Index into lightEntries
			};

			/// Recursive build helper.
			/// \return Index of the created node in the nodes array.
			unsigned int BuildRecursive(
				std::vector<BuildPrimitive>& prims,
				unsigned int start,
				unsigned int end,
				unsigned int& bvhPos
				);
		};
	}
}

#endif
