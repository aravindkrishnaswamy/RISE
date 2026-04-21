//////////////////////////////////////////////////////////////////////
//
//  SMSPhotonMap.h - Photon-aided seeding for Specular Manifold
//    Sampling.
//
//    One-time pass shot at scene-preparation time (via the rasterizer
//    hook PixelBasedRasterizerHelper::PreRenderSetup).  Tracer walks
//    the classical photon-mapping loop (sample light -> sample
//    emission direction -> refract / reflect through specular chain
//    -> store on first diffuse hit) but stores a custom SMSPhoton
//    record that remembers the FIRST specular-caster entry point,
//    not just the diffuse-hit radiance contribution.
//
//    Queried at render time by ManifoldSolver::EvaluateAtShadingPoint:
//    a fixed-radius kd-tree lookup "photons whose diffuse-landing
//    position is within R of this shading point" returns a set of
//    PROVEN-good SMS seeds (each photon's stored entryPoint).  This
//    replaces the uniform-random-surface seeding strategy that was
//    O(N) per shading point and scaled poorly with caster complexity.
//
//    The kd-tree code is a focused clone of VCMLightVertexStore —
//    same left-balanced median construction, same fixed-radius
//    traversal with the tangent-case fix.  We duplicate rather than
//    template that code because SMSPhoton's semantic is different
//    enough (it's a seed record, not a density-estimation vertex)
//    that sharing would complicate both.
//
//    Thread-safety:
//      - Build() is called once from a single thread (the rasterizer's
//        PreRenderSetup).  It may internally parallelize via
//        GlobalThreadPool().ParallelFor.
//      - After BuildKDTree() returns, the store is read-only;
//        QuerySeeds() is safe from every render worker.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-20
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SMS_PHOTON_MAP_
#define SMS_PHOTON_MAP_

#include "SMSPhoton.h"
#include <vector>

namespace RISE
{
	class IScene;
	class IRayCaster;
	class IObject;

	namespace Implementation
	{
		class SMSPhotonMap
		{
		public:
			SMSPhotonMap();
			~SMSPhotonMap();

			/// Shoot numPhotons photons from every light in the scene,
			/// walk each one through the specular chain, and store a
			/// record on the first diffuse landing.  Builds the kd-tree
			/// at the end.  After this call the store is read-only.
			///
			/// \param scene          Scene with its acceleration structure already built.
			/// \param numPhotons     Requested emission budget; actual stored count depends on how many photons land on diffuse after a specular bounce.
			/// \return               Number of photons actually stored (== queryable seeds).
			unsigned int Build(
				const IScene& scene,
				const unsigned int numPhotons
				);

			/// Fixed-radius query: appends every stored photon whose
			/// landing position is within radiusSq of center.
			/// \param center         World-space query position (typically a shading point).
			/// \param radiusSq       SQUARED search radius.
			/// \param out            Appended-to output vector (not cleared).
			void QuerySeeds(
				const Point3& center,
				const Scalar radiusSq,
				std::vector<SMSPhoton>& out
				) const;

			/// Returns the number of photons stored.
			std::size_t Size() const { return mPhotons.size(); }

			/// Returns the kd-tree built flag.  Safe callers only query
			/// when this is true.
			bool IsBuilt() const { return mBuilt; }

			/// Auto-radius: half of the average nearest-neighbor distance
			/// between stored photon landings — VCM's 0.01 * median_segment
			/// analog for SMS.  Returns 0 when the map is empty.  Cached
			/// after first call.
			Scalar GetAutoRadius() const;

			/// Clear everything — drops the kd-tree.  Re-Build() to use
			/// again.
			void Clear();

		private:
			std::vector<SMSPhoton>	mPhotons;
			bool					mBuilt;
			mutable Scalar			mCachedAutoRadius;	///< -1 until computed
		};
	}
}

#endif
