//////////////////////////////////////////////////////////////////////
//
//  CompletePathGuide.h - Experimental recorder for BDPT complete-path
//    statistics. This records whole successful paths into coarse
//    technique/spatial buckets so we can measure whether reusable
//    complete-path structure exists before changing sampling.
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPLETE_PATH_GUIDE_
#define COMPLETE_PATH_GUIDE_

#include "../Interfaces/IReference.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Math3D.h"
#include <stdint.h>
#include <unordered_map>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class CompletePathGuide :
			public virtual IReference,
			public virtual Reference
		{
		public:
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

			virtual ~CompletePathGuide();

			uint64_t MakeBucketKey(
				unsigned int s,
				unsigned int t,
				const Point3& eyePosition,
				const Point3* pLightPosition,
				bool hitsEmitter
				) const;

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

			void BeginIteration();

			void AddSample(
				unsigned int s,
				unsigned int t,
				const Point3& eyePosition,
				const Point3* pLightPosition,
				const RISEPel& contribution,
				bool hitsEmitter,
				bool needsSplat
				);

			void EndIteration();

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
