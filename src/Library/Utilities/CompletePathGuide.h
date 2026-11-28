//////////////////////////////////////////////////////////////////////
//
//  CompletePathGuide.h - Experimental recorder for BDPT complete-path
//    statistics. Records whole successful paths into coarse
//    technique/spatial buckets so we can measure whether reusable
//    complete-path structure exists before changing sampling.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPLETE_PATH_GUIDE_
#define COMPLETE_PATH_GUIDE_

#include "../Interfaces/IReference.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Threads/Threads.h"
#include <stdint.h>
#include <unordered_map>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		// Aggregates per-iteration and cross-iteration BDPT (s, t) path
		// energy (Rec.709 luminance) into a coarse 16^3 spatial grid.
		// Mutating calls synchronize on `mutex`; QueryStrategyWeight() is
		// unlocked and must only be called between iterations.
		class CompletePathGuide :
			public virtual IReference,
			public virtual Reference
		{
		public:
			// Per-(s, t) technique sample count and summed luminance energy.
			struct TechniqueStats
			{
				size_t	count;
				Scalar	energy;

				TechniqueStats() :
					count( 0 ),
					energy( 0 )
				{
				}
			};

			// Summary metrics logged at EndIteration() for the most recent pass.
			struct IterationSummary
			{
				size_t	sampleCount;
				Scalar	totalEnergy;
				size_t	uniqueBucketCount;
				size_t	persistentBucketCount;
				unsigned int topTechniqueS;
				unsigned int topTechniqueT;
				Scalar	topTechniqueEnergy;
				Scalar	topBucketEnergyShare;

				IterationSummary() :
					sampleCount( 0 ),
					totalEnergy( 0 ),
					uniqueBucketCount( 0 ),
					persistentBucketCount( 0 ),
					topTechniqueS( 0 ),
					topTechniqueT( 0 ),
					topTechniqueEnergy( 0 ),
					topBucketEnergyShare( 0 )
				{
				}
			};

		protected:
			struct BucketStats
			{
				size_t	count;
				Scalar	energy;

				BucketStats() :
					count( 0 ),
					energy( 0 )
				{
				}
			};

			Point3								boundsMin;
			Point3								boundsMax;
			unsigned int						maxLightDepth;
			unsigned int						maxEyeDepth;
			bool								collecting;
			size_t								iteration;
			IterationSummary					lastSummary;
			std::vector<TechniqueStats>			iterationTechniqueStats;
			std::vector<TechniqueStats>			aggregateTechniqueStats;
			std::unordered_map<uint64_t, BucketStats> iterationBuckets;
			std::unordered_map<uint64_t, BucketStats> aggregateBuckets;
			RMutex								mutex;

			virtual ~CompletePathGuide();

			// Packs (s, t, hitsEmitter, quantized eye/light 16^3 voxels) into
			// the low 33 bits of a uint64_t bucket key (s, t masked to 4 bits).
			uint64_t MakeBucketKey(
				unsigned int s,
				unsigned int t,
				const Point3& eyePosition,
				const Point3* pLightPosition,
				bool hitsEmitter
				) const;

			// Flat row-major index into (maxLightDepth+1) x (maxEyeDepth+1) stats tables.
			static inline size_t TechniqueIndex(
				const unsigned int maxLightDepth,
				const unsigned int maxEyeDepth,
				const unsigned int s,
				const unsigned int t
				)
			{
				(void)maxEyeDepth;
				return s + (maxLightDepth + 1) * t;
			}

		public:
			CompletePathGuide(
				const Point3& sceneBoundsMin,
				const Point3& sceneBoundsMax,
				unsigned int maxLightDepth_,
				unsigned int maxEyeDepth_
				);

			// Resets per-iteration counters and enables sample collection.
			void BeginIteration();

			// Records a non-splat, positive-luminance complete (s, t) path.
			// Thread-safe; skips paths when not collecting or needsSplat is set.
			void AddSample(
				unsigned int s,
				unsigned int t,
				const Point3& eyePosition,
				const Point3* pLightPosition,
				const RISEPel& contribution,
				bool hitsEmitter,
				bool needsSplat
				);

			// Finalizes collection, computes summary stats, and logs a report.
			void EndIteration();

			// Lock-free heuristic weight sqrt(bucketEnergy + globalEnergy)
			// from aggregate stats. Call only when no iteration is mutating.
			Scalar QueryStrategyWeight(
				unsigned int s,
				unsigned int t,
				const Point3& eyePosition,
				const Point3* pLightPosition,
				bool hitsEmitter
				) const;

			bool IsCollectingTrainingSamples() const { return collecting; }
			const IterationSummary& GetLastSummary() const { return lastSummary; }
		};
	}
}

#endif
