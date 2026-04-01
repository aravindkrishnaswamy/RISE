//////////////////////////////////////////////////////////////////////
//
//  PathGuidingField.h - Wrapper around Intel OpenPGL for learned
//    incident radiance distributions used in path guiding.
//
//    PathGuidingConfig is always available (controls the pipeline).
//    PathGuidingField is only available when RISE_ENABLE_OPENPGL is
//    defined at compile time.
//
//    The guiding field is trained over multiple low-spp passes,
//    then queried during the final render to provide a learned
//    sampling distribution at each non-delta surface vertex.
//    The guided distribution is combined with the BSDF via
//    one-sample MIS.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATH_GUIDING_FIELD_
#define PATH_GUIDING_FIELD_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	/// Configuration for path guiding.  Always compiled in so the
	/// parser/Job/API pipeline works regardless of the OpenPGL flag.
	struct PathGuidingConfig
	{
		bool			enabled;				///< Master switch
		unsigned int	trainingIterations;		///< Number of training passes before final render
		unsigned int	trainingSPP;			///< Samples per pixel during each training pass
		Scalar			alpha;					///< MIS blending weight: P(sample from guide)
		unsigned int	maxGuidingDepth;		///< Max eye subpath bounce depth for guided sampling
		bool			completePathGuiding;	///< Experimental BDPT complete-path recorder/guide
		bool			completePathStrategySelection;	///< Experimental BDPT strategy selection
		unsigned int	completePathStrategySamples;	///< Techniques to evaluate per path

		PathGuidingConfig() :
		enabled( false ),
		trainingIterations( 4 ),
		trainingSPP( 4 ),
		alpha( 0.5 ),
		maxGuidingDepth( 3 ),
		completePathGuiding( false ),
		completePathStrategySelection( false ),
		completePathStrategySamples( 2 )
		{
		}
	};
}

#ifdef RISE_ENABLE_OPENPGL

#include <openpgl/openpgl.h>
#include "../Interfaces/IReference.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		/// Thread-local guiding distribution handle for query-phase sampling.
		/// Each rendering thread should own one of these.
		struct GuidingDistributionHandle
		{
			PGLSurfaceSamplingDistribution dist;

			GuidingDistributionHandle() : dist( 0 ) {}
			~GuidingDistributionHandle()
			{
				if( dist ) {
					pglReleaseSurfaceSamplingDistribution( dist );
					dist = 0;
				}
			}
		};

		/// Thread-local volume guiding distribution handle.
		struct GuidingVolumeDistributionHandle
		{
			PGLVolumeSamplingDistribution dist;

			GuidingVolumeDistributionHandle() : dist( 0 ) {}
			~GuidingVolumeDistributionHandle()
			{
				if( dist ) {
					pglReleaseVolumeSamplingDistribution( dist );
					dist = 0;
				}
			}
		};

		/// Wrapper around Intel OpenPGL.  Owns the device, field, and
		/// sample storage.  Thread-safe for queries after training.
		class PathGuidingField :
			public virtual IReference,
			public virtual Reference
		{
		protected:
			PGLDevice				device;
			PGLField				field;
			PGLSampleStorage		sampleStorage;
			bool					trained;
			bool					collectingTraining;
			PathGuidingConfig		config;
			mutable size_t			sampleCount;
			mutable size_t			zeroValueSampleCount;
			mutable size_t			volumeSampleCount;
			mutable size_t			zeroValueVolumeSampleCount;
			mutable Scalar			sampleEnergy;
			mutable Scalar			directSampleEnergy;

			virtual ~PathGuidingField();

		public:
			PathGuidingField(
				const PathGuidingConfig& cfg,
				const Point3& boundsMin,
				const Point3& boundsMax
				);

			//
			// Training API — Begin/End must be serialized per iteration.
			// OpenPGL sample storage itself supports concurrent sample adds,
			// so AddSample/AddZeroValueSample/AddPathSegments may be called
			// from multiple render threads during an active training pass.
			//
			void BeginTrainingIteration();

			void AddSample(
				const Point3& position,
				const Vector3& direction,
				Scalar distance,
				Scalar pdf,
				Scalar luminance,
				bool isDirect
				);

			void AddZeroValueSample(
				const Point3& position,
				const Vector3& direction
				);

			void AddPathSegments(
				PGLPathSegmentStorage pathSegments,
				bool useNEEMiWeights,
				bool guideDirectLight,
				bool rrAffectsDirectContribution
				);

			void EndTrainingIteration();

			//
			// Query API — thread-safe after training
			//

			/// Allocate a per-thread distribution handle.
			/// Caller owns the returned pointer.
			GuidingDistributionHandle* NewDistributionHandle() const;

			/// Initialize the distribution at the given surface point.
			/// Returns false if no guiding data is available here.
			bool InitDistribution(
				GuidingDistributionHandle& handle,
				const Point3& position,
				Scalar sample1D
				) const;

			/// Apply cosine product to improve guiding quality.
			void ApplyCosineProduct(
				GuidingDistributionHandle& handle,
				const Vector3& normal
				) const;

			/// Sample a direction from the guiding distribution.
			Vector3 Sample(
				GuidingDistributionHandle& handle,
				const Point2& xi,
				Scalar& pdf
				) const;

			/// Evaluate the guiding PDF for a given direction.
			Scalar Pdf(
				GuidingDistributionHandle& handle,
				const Vector3& direction
				) const;

			//
			// Volume guiding API
			//

			/// Record a volume scatter training sample.
			void AddVolumeSample(
				const Point3& position,
				const Vector3& direction,
				Scalar distance,
				Scalar pdf,
				Scalar luminance,
				bool isDirect
				);

			/// Record a zero-value volume sample (guides away from dark regions).
			void AddZeroValueVolumeSample(
				const Point3& position,
				const Vector3& direction
				);

			/// Initialize a volume sampling distribution at the given scatter point.
			/// Returns false if no guiding data is available.
			bool InitVolumeDistribution(
				GuidingVolumeDistributionHandle& handle,
				const Point3& position,
				Scalar sample1D
				) const;

			/// Apply Henyey-Greenstein product to the volume distribution.
			/// This improves guiding quality for anisotropic phase functions.
			void ApplyHGProduct(
				GuidingVolumeDistributionHandle& handle,
				const Vector3& wo,
				Scalar meanCosine
				) const;

			/// Sample a direction from the volume guiding distribution.
			Vector3 SampleVolume(
				GuidingVolumeDistributionHandle& handle,
				const Point2& xi,
				Scalar& pdf
				) const;

			/// Evaluate the volume guiding PDF for a given direction.
			Scalar PdfVolume(
				GuidingVolumeDistributionHandle& handle,
				const Vector3& direction
				) const;

			/// Get volume sample statistics from last training iteration.
			size_t GetLastAddedVolumeSampleCount() const { return volumeSampleCount; }

			bool IsTrained() const { return trained; }
			bool IsCollectingTrainingSamples() const { return collectingTraining; }
			size_t GetLastAddedSurfaceSampleCount() const { return sampleCount; }
			size_t GetLastAddedZeroValueSurfaceSampleCount() const { return zeroValueSampleCount; }
			Scalar GetLastAddedSurfaceSampleEnergy() const { return sampleEnergy; }
			Scalar GetLastAddedDirectSurfaceSampleEnergy() const { return directSampleEnergy; }
			Scalar GetLastAddedIndirectSurfaceSampleEnergy() const
			{
				return sampleEnergy > directSampleEnergy ?
					sampleEnergy - directSampleEnergy :
					0;
			}

			Scalar GetAlpha() const { return config.alpha; }
		};
	}
}

#endif // RISE_ENABLE_OPENPGL

#endif // PATH_GUIDING_FIELD_
