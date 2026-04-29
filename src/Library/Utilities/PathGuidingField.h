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
//    The guided distribution is combined with the BSDF via either
//    one-sample MIS (original, default) or Resampled Importance
//    Sampling (RIS), selectable via GuidingSamplingType.
//
//    RIS draws two candidates (one from BSDF, one from the guiding
//    distribution) and selects proportional to a target function
//    that accounts for both the BSDF value and the learned incident
//    radiance.  This produces lower variance than one-sample MIS at
//    modest additional cost.
//
//    The guiding alpha (blend probability) is adaptively scaled per
//    training iteration using a variance-aware approach inspired by
//    Rath et al. 2020.  The coefficient of variation (CoV) of
//    indirect sample energy measures how directionally concentrated
//    the illumination is; scenes with high CoV benefit most from
//    guiding.  Training also tracks indirectSampleEnergySquaredSum
//    for this purpose.  See PixelBasedPelRasterizer.cpp and
//    BDPTRasterizerBase.cpp for the full adaptive alpha logic.
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
	/// Selects the directional sampling strategy used when path
	/// guiding is active.  The enum lives outside PathGuidingConfig
	/// so it can be referenced by RuntimeContext without pulling in
	/// the full config header.
	enum GuidingSamplingType
	{
		eGuidingOneSampleMIS = 0,	///< One-sample MIS blend (original)
		eGuidingRIS          = 1	///< Resampled Importance Sampling
	};

	/// Configuration for path guiding.  Always compiled in so the
	/// parser/Job/API pipeline works regardless of the OpenPGL flag.
	struct PathGuidingConfig
	{
		bool			enabled;				///< Master switch
		unsigned int	trainingIterations;		///< Number of training passes before final render
		unsigned int	trainingSPP;			///< Samples per pixel during each training pass
		bool			combineTrainingIterations;	///< If true, accumulate every training iteration's rendered pixels into the final image weighted by its SPP (Müller 2017 §5).  When false (legacy behaviour) training-iteration pixels are discarded — fine only when total final SPP ≫ training SPP.  At low final SPP, discarding wastes the work and trial-to-trial training non-determinism dominates the variance budget.
		bool			online;					///< If true, the training-iteration loop IS the entire render — no separate final pass, every sample feeds both the field and the image.  Implies combineTrainingIterations.  Best for low-SPP regimes where the train-then-render two-phase pattern wastes too much budget on discarded training pixels (Vorba 2014 / NASG 2024 style).  Total SPP = pathguiding_iterations × pathguiding_spp; the scene's `samples` parameter is ignored in this mode.
		unsigned int	warmupIterations;		///< Number of training iterations to render with alpha=0 (pure BSDF sampling) before switching to the configured alpha.  Samples still feed the field — but because they're produced by unguided BDPT, their pixels are statistically clean to keep in the final image when combine/online is on.  This avoids the bias regression where naive online combine mixes early pixels-from-untrained-field into the output.  Useful primarily when `online` is true; ignored when `combineTrainingIterations` is false.
		Scalar			alpha;					///< MIS blending weight: P(sample from guide)
		bool			learnedAlpha;			///< Per-cell Adam-learned α (Müller 2017 v2 / Tom94's practical-path-guiding).  When true the per-vertex α used in one-sample MIS is `alpha · 2 · σ(θ_cell)`, with θ_cell updated by Adam on the deferred KL gradient at path completion.  Neutral at low SPP (32), ~2% mean-σ² reduction at 256 SPP.  When false, falls back to fixed `alpha`.
		unsigned int	maxGuidingDepth;		///< Max eye subpath bounce depth for guided sampling
		unsigned int	maxLightGuidingDepth;	///< Max light subpath bounce depth for guided sampling (0 = disabled)
		GuidingSamplingType	samplingType;		///< Directional sampling strategy
		unsigned int	risCandidates;			///< Reserved for future N>2 RIS; currently only N=2 is implemented
		bool			completePathGuiding;	///< Experimental BDPT complete-path recorder/guide
		bool			completePathStrategySelection;	///< Experimental BDPT strategy selection
		unsigned int	completePathStrategySamples;	///< Techniques to evaluate per path

		PathGuidingConfig() :
		enabled( false ),
		trainingIterations( 4 ),
		trainingSPP( 4 ),
		combineTrainingIterations( true ),
		online( false ),
		warmupIterations( 0 ),
		alpha( 0.5 ),
		learnedAlpha( true ),
		maxGuidingDepth( 3 ),
		maxLightGuidingDepth( 0 ),
		samplingType( eGuidingOneSampleMIS ),
		risCandidates( 2 ),
		completePathGuiding( false ),
		completePathStrategySelection( false ),
		completePathStrategySamples( 2 )
		{
		}
	};
}

#ifdef RISE_ENABLE_OPENPGL

// openpgl ships headers that trip -Wdocumentation (empty @brief) and
// -Wshorten-64-to-32 (size_t→int) under our project warning level.
// Silence them at the include site so our build stays warning-clean.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#endif
#include <openpgl/openpgl.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "../Interfaces/IReference.h"
#include "../Utilities/Reference.h"
#include <mutex>
#include <unordered_map>

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
			mutable Scalar			indirectSampleEnergySquaredSum;	///< Sum of squared indirect energies for variance estimation

			// Per-cell adaptive alpha (Müller 2017 v2 / Practical Path
			// Guiding online-learning of bsdfSamplingFraction).  Each
			// spatial-cell id maps to an Adam state for the logit
			// θ where α = sigmoid(θ).  Updates apply Adam (β1=0.9,
			// β2=0.999, lr=0.01) on dL/dθ derived from the per-sample
			// KL gradient computed at path completion (deferred so f
			// = BSDF·cos·Li uses the actual incident radiance, not a
			// BSDF-only proxy).
			//
			// Map is NOT cleared on EndTrainingIteration.  OpenPGL's
			// kdtree only subdivides leaves (existing IDs become
			// orphaned interior-node ids — never reused); orphaned
			// entries bloat memory by at most O(field-cells) but
			// don't cause wrong queries.  The α we learn during early
			// iterations therefore carries forward into later
			// iterations and the final render — Tom94's prescribed
			// "frozen-α-at-render" recipe falls out for free.
			struct CellAdamState
			{
				float		theta;	///< logit; α = σ(θ).  Init: 0 → α = 0.5.
				float		m;		///< Adam 1st moment of dL/dθ
				float		v;		///< Adam 2nd moment of dL/dθ
				uint32_t	t;		///< Adam step count (for bias-correction)

				CellAdamState() : theta( 0.0f ), m( 0.0f ), v( 0.0f ), t( 0 ) {}
			};

			mutable std::mutex	cellAlphaMutex;
			mutable std::unordered_map<uint32_t, CellAdamState> cellState;

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

			/// Evaluate the incoming radiance PDF for a given direction.
			/// Unlike Pdf(), this returns the raw learned distribution
			/// PDF before any cosine product is applied.  Used by the
			/// RIS target function which needs the incident radiance
			/// estimate separately from the sampling PDF.
			Scalar IncomingRadiancePdf(
				GuidingDistributionHandle& handle,
				const Vector3& direction
				) const;

			//
			// Per-cell adaptive alpha — online KL-loss Adam over the
			// guide-vs-BSDF mixing weight, keyed on OpenPGL's spatial
			// cell id.  Müller 2017 v2 / Tom94's practical-path-guiding.
			//
			// Caller pattern:
			//   1. At guide sample time:
			//        cellId = GetCellId(handle)         — snapshot id
			//        alpha  = GetCellAlpha(handle)     — read live α
			//        ... use α in one-sample MIS, record (resultBefore,
			//        throughputBefore, bsdfPdf, guidePdf, combinedPdf,
			//        cellId) into a per-thread pending list ...
			//   2. After path completes:
			//        for each pending update:
			//          f = lum(deltaResult) * combinedPdf / lum(throughputBefore)
			//          UpdateCellAlpha(cellId, bsdfPdf, guidePdf, f,
			//                          combinedPdf, lr)
			// f from deltaResult uses the actual radiance flowing
			// through the chosen direction at this vertex, not a
			// BSDF-only proxy — this is what gives Adam a useful
			// gradient signal at low SPP.

			/// Stable spatial-cell id for the cell this handle is
			/// currently initialized at.  Caller should snapshot at
			/// sample time and pass to UpdateCellAlpha after the path
			/// completes (the handle itself is reused for later
			/// vertices, so reading GetId from it later returns the
			/// wrong cell).
			uint32_t GetCellId(
				const GuidingDistributionHandle& handle
				) const;

			/// Read the learned α at the cell where `handle` is
			/// currently initialized.  Returns 0.5 (neutral) when the
			/// cell has no updates yet.  Cheap (lock_guard over
			/// unordered_map lookup); safe to call on the hot path.
			Scalar GetCellAlpha(
				const GuidingDistributionHandle& handle
				) const;

			/// Adam step on the cell's logit θ after one path-traced
			/// sample has been drawn from the combined density.
			/// Pre-computed quantities expected:
			///   bsdfPdf, guidePdf, combinedPdf — same as sample time
			///   f          — |BSDF·cos·Li| at the sampled direction
			///                 (deferred path-completion estimate)
			/// Internal math:
			///   dL/dα = -(f / combinedPdf²) · (guidePdf - bsdfPdf)
			///   dL/dθ = dL/dα · α·(1-α)
			///   Adam:  m ← β₁·m + (1-β₁)·g
			///          v ← β₂·v + (1-β₂)·g²
			///          θ ← θ - lr · (m / (1-β₁ᵗ)) / (√(v / (1-β₂ᵗ)) + ε)
			/// θ clamped to ±8 so α stays in (~3.4e-4, ~0.9997).
			void UpdateCellAlpha(
				uint32_t cellId,
				Scalar bsdfPdf,
				Scalar guidePdf,
				Scalar f,
				Scalar combinedPdf,
				Scalar learningRate
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
			Scalar GetLastAddedIndirectSurfaceSampleEnergySquaredSum() const { return indirectSampleEnergySquaredSum; }

			Scalar GetAlpha() const { return config.alpha; }
		};
	}
}

#endif // RISE_ENABLE_OPENPGL

#endif // PATH_GUIDING_FIELD_
