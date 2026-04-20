//////////////////////////////////////////////////////////////////////
//
//  BDPTIntegrator.h - Core bidirectional path tracing algorithm.
//
//    CONTEXT:
//    Standard unidirectional path tracing only discovers light paths
//    from the camera side.  For scenes where important light paths
//    are hard to find from the eye (caustics, small luminaries, SSS
//    materials), convergence is very slow.  BDPT generates subpaths
//    from both the camera and light sources, then connects them with
//    all possible (s,t) strategies, weighted by MIS to minimize
//    variance.
//
//    ARCHITECTURE:
//    BDPTIntegrator is a standalone algorithm class (not a rasterizer)
//    so it can be reused by both BDPTRasterizer (pixel-based) and
//    MLTRasterizer (Markov chain-based).  The public API is:
//
//    1. GenerateLightSubpath / GenerateEyeSubpath:
//       Trace a subpath from a sampled light or camera ray, storing
//       vertices with throughput (alpha), forward/reverse PDFs, and
//       delta flags.  Each vertex's throughput is the cumulative
//       path contribution from the subpath origin to that vertex.
//
//    2. ConnectAndEvaluate:
//       Evaluate a single (s,t) strategy — connect lightVerts[s-1]
//       to eyeVerts[t-1], check visibility, evaluate BSDFs, compute
//       the geometric term, and return the unweighted contribution.
//       Special cases: s=0 (eye path hits emitter), s=1 (next event
//       estimation), and t=1 (light path connects to the camera,
//       with the camera stored as eye vertex 0).
//
//    3. MISWeight:
//       Balance heuristic weight computed by walking along the full
//       path and accumulating ratios of forward/reverse PDFs at each
//       vertex, following Veach's thesis Section 10.2.1.
//
//    DIRECTION CONVENTIONS:
//    In RISE, BSDF::value(vLightIn, ri) expects:
//      - vLightIn (wi): direction AWAY from surface toward light
//      - ri.ray.Dir(): direction TOWARD surface (incoming viewer ray)
//    EvalBSDFAtVertex adapts to this by negating wo to build ri.
//    EvalPdfAtVertex negates wi (since SPF::Pdf expects the incoming
//    ray as ri.ray.Dir() and the outgoing direction as wo).
//
//    THROUGHPUT CONVENTION:
//    beta (path throughput) accumulates f*|cos|/pdf for non-delta
//    interactions, and kray directly for delta interactions.  The
//    stored vertex.throughput is the value of beta at that vertex,
//    representing the measurement contribution from the subpath
//    origin to that point.  The full path contribution for (s,t) is:
//      C = alpha_light * f_light * G * f_eye * alpha_eye
//    where alpha_light = lightVerts[s-1].throughput and similarly
//    for the eye side.
//
//    REFERENCES:
//    - Veach, E. "Robust Monte Carlo Methods for Light Transport
//      Simulation." PhD Thesis, Stanford, 1997. Chapters 8-10.
//    - Lafortune & Willems. "Bi-Directional Path Tracing." 1993.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_INTEGRATOR_
#define BDPT_INTEGRATOR_

#include "../Interfaces/IReference.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/ICamera.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Lights/LightSampler.h"
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/CompletePathGuide.h"
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/BSSRDFSampling.h"
#include "BDPTVertex.h"
#include "../Utilities/RandomNumbers.h"
#include <atomic>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		/// Encapsulates the full BDPT algorithm: subpath generation,
		/// connection, and MIS weight computation.
		class BDPTIntegrator :
			public virtual IReference,
			public virtual Reference
		{
		protected:
			unsigned int		maxEyeDepth;
			unsigned int		maxLightDepth;
			const LightSampler*	pLightSampler;
			ManifoldSolver*		pManifoldSolver;
			StabilityConfig		stabilityConfig;

#ifdef RISE_ENABLE_OPENPGL
			PathGuidingField*	pGuidingField;
			PathGuidingField*	pLightGuidingField;	///< Separate field for light subpath guiding (Option B)
			CompletePathGuide*	pCompletePathGuide;
			Scalar				guidingAlpha;
			unsigned int		maxGuidingDepth;
			unsigned int		maxLightGuidingDepth;
			GuidingSamplingType	guidingSamplingType;
			unsigned int		guidingRISCandidates;
			bool				completePathStrategySelectionEnabled;
			unsigned int		completePathStrategySampleCount;
			mutable std::atomic<unsigned long long> strategySelectionPathCount;
			mutable std::atomic<unsigned long long> strategySelectionCandidateCount;
			mutable std::atomic<unsigned long long> strategySelectionEvaluatedCount;

		public:
			struct GuidingTrainingStats
			{
				Scalar	totalEnergy;
				Scalar	firstSurfaceConnectionEnergy;
				Scalar	deepEyeConnectionEnergy;
				size_t	firstSurfaceConnectionCount;
				size_t	deepEyeConnectionCount;

				GuidingTrainingStats() :
					totalEnergy( 0 ),
					firstSurfaceConnectionEnergy( 0 ),
					deepEyeConnectionEnergy( 0 ),
					firstSurfaceConnectionCount( 0 ),
					deepEyeConnectionCount( 0 )
				{
				}
			};

		protected:
			mutable GuidingTrainingStats guidingTrainingStats;
#endif

			virtual ~BDPTIntegrator();

		public:
			BDPTIntegrator(
				unsigned int maxEye,
				unsigned int maxLight,
				const StabilityConfig& stabilityCfg
				);

			void SetLightSampler( const LightSampler* pSampler );
			void SetManifoldSolver( ManifoldSolver* pSolver );

#ifdef RISE_ENABLE_OPENPGL
			void SetGuidingField( PathGuidingField* pField, PathGuidingField* pLightField, Scalar alpha, unsigned int maxDepth, unsigned int maxLightDepth, GuidingSamplingType samplingType, unsigned int risCandidates );
			void SetCompletePathGuide(
				CompletePathGuide* pGuide,
				bool enableStrategySelection = false,
				unsigned int strategySamples = 0 );
			void ResetGuidingTrainingStats() const;
			const GuidingTrainingStats& GetGuidingTrainingStats() const;
			void ResetStrategySelectionStats() const;
			void GetStrategySelectionStats(
				unsigned long long& pathCount,
				unsigned long long& candidateCount,
				unsigned long long& evaluatedCount
				) const;
#endif

			unsigned int GetMaxEyeDepth() const { return maxEyeDepth; }
			unsigned int GetMaxLightDepth() const { return maxLightDepth; }

			/// Result of connecting a single (s,t) strategy
			struct ConnectionResult
			{
				RISEPel		contribution;	///< Unweighted path contribution
				Scalar		misWeight;		///< MIS weight for this (s,t) strategy
				Point2		rasterPos;		///< Pixel position for splatting (valid if needsSplat)
				unsigned int s;				///< Number of light subpath vertices
				unsigned int t;				///< Number of eye subpath vertices
				bool		needsSplat;		///< True for light-side camera connections (active case: t==1)
				bool		valid;			///< False if connection is invalid
				RISEPel		guidingLocalContribution;	///< Contribution at eye vertex before eye-prefix throughput
				unsigned int guidingEyeVertexIndex;	///< Eye vertex receiving guidingLocalContribution
				bool		guidingUseDirectContribution;	///< Store guidingLocalContribution as OpenPGL directContribution
				bool		guidingValid;	///< True if the result contributes to an eye-path training segment

				ConnectionResult() :
				contribution( RISEPel( 0, 0, 0 ) ),
				misWeight( 0 ),
				rasterPos( Point2( 0, 0 ) ),
				s( 0 ),
				t( 0 ),
				needsSplat( false ),
				valid( false ),
				guidingLocalContribution( RISEPel( 0, 0, 0 ) ),
				guidingEyeVertexIndex( 0 ),
				guidingUseDirectContribution( false ),
				guidingValid( false )
				{
				}
		};

			/// Generates a light subpath starting from a sampled light source.
			/// \return Number of vertices stored
			///
			/// Subpaths produced by these generators can fan out into
			/// multiple branches when a multi-lobe delta vertex is
			/// encountered with surviving throughput above the scene's
			/// `branching_threshold` (see StabilityConfig.h).  All branch
			/// vertex arrays are appended sequentially into the flat
			/// `vertices` vector; `subpathStarts` delimits them:
			/// branch i occupies `vertices[subpathStarts[i] ..
			/// subpathStarts[i+1])`.  For a subpath with no split the
			/// output is `subpathStarts = {0, N}` — a single branch.
			/// Return value is the number of branches.
			///
			/// `branchingThresholdOverride` semantics match GenerateEyeSubpath:
			/// pass a value in [0, 1] to override StabilityConfig.branching-
			/// Threshold on this call, or a negative value to use the config
			/// default.  Callers that do NOT loop over `subpathStarts` on
			/// the light side (currently BDPT-Pel, MLT, MMLT, spectral
			/// rasterizers; VCM-Pel is the only consumer that handles
			/// multi-branch light subpaths) should pass 1.0 to keep the
			/// light subpath single-branch.
			///
			/// `maxBouncesOverride` lets MMLT cap the surface-bounce loop
			/// below the integrator's `maxLightDepth` member when the
			/// chain only needs a short subpath.  The default UINT_MAX
			/// preserves the historical PSSMLT/BDPT behaviour bit-for-bit
			/// (min(maxLightDepth, UINT_MAX) == maxLightDepth).  Volume
			/// bounces are still allowed up to `stabilityConfig.maxVolumeBounce`
			/// on top of the surface cap.
			unsigned int GenerateLightSubpath(
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices,
				std::vector<uint32_t>& subpathStarts,
				const RandomNumberGenerator& random,
				Scalar branchingThresholdOverride,
				unsigned int maxBouncesOverride = UINT_MAX
				) const;

			/// Generates an eye subpath from a camera ray.
			/// \return Number of vertices stored
			///
			/// `branchingThresholdOverride` controls whether threshold-
			/// gated splitting fires on this call.  Pass a value in
			/// [0, 1] to override StabilityConfig.branchingThreshold
			/// (e.g. 1.0 to disable branching for this caller).  Pass
			/// any negative value to use the config default.  Callers
			/// that do NOT loop over `subpathStarts` must pass 1.0 to
			/// avoid silently-wrong MIS evaluation across branch
			/// boundaries; MLT / MLT-spectral / MMLT always pass 1.0
			/// because PSSMLT's primary-sample vector can't mutate
			/// split choices.  BDPT-Pel, BDPT-Spectral, VCM-Pel,
			/// VCM-Spectral, and the VCM photon-store build all loop
			/// over branches.
			///
			/// See GenerateLightSubpath for `maxBouncesOverride` semantics.
			unsigned int GenerateEyeSubpath(
				const RuntimeContext& rc,
				const Ray& cameraRay,
				const Point2& screenPos,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices,
				std::vector<uint32_t>& subpathStarts,
				Scalar branchingThresholdOverride,
				unsigned int maxBouncesOverride = UINT_MAX
				) const;

			/// Connects and evaluates a single (s,t) strategy.
			///
			/// THREAD-SAFETY CONTRACT: although this method is `const`,
			/// it transiently mutates `pdfRev` on up to four endpoint
			/// vertices in `lightVerts`/`eyeVerts` via `const_cast` (and
			/// restores them before returning) so MISWeight can read the
			/// connection-aware reverse PDFs.  CALLERS MUST OWN their
			/// own subpath vectors per thread — sharing the same
			/// `lightVerts`/`eyeVerts` across concurrent threads is a
			/// data race.  All in-tree callers (BDPTRasterizerBase,
			/// MLTRasterizer, MLTSpectralRasterizer, the upcoming MMLT)
			/// build the vectors per worker thread on the stack, so the
			/// contract holds; the `const` qualifier is preserved
			/// because the BDPTIntegrator instance itself has no
			/// observable state mutation.
			ConnectionResult ConnectAndEvaluate(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				unsigned int s,		///< Number of light subpath vertices used
				unsigned int t,		///< Number of eye subpath vertices used
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera
				) const;

			/// Evaluates all valid (s,t) strategies and returns results.
			std::vector<ConnectionResult> EvaluateAllStrategies(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera,
				ISampler* pSampler
				) const;

			/// Single-strategy entry point for MMLT.  Each MMLT chain
			/// evaluates ONE (s,t) per mutation (not all strategies like
			/// PSSMLT), so the chain density adapts to per-strategy
			/// luminance rather than the summed luminance — this is what
			/// fixes the dim-strategy starvation that limits PSSMLT
			/// convergence on SDS scenes.
			///
			/// Semantically a thin wrapper around ConnectAndEvaluate that
			/// also fills in cr.s and cr.t (mirrors the inner loop body
			/// of EvaluateAllStrategies).  It deliberately does NOT
			/// invoke the directional/ambient-light deterministic branch
			/// or the OpenPGL strategy-selection branch that
			/// EvaluateAllStrategies adds AROUND its (s,t) loop — those
			/// have no per-strategy analog and MMLT treats their inputs
			/// (zero-exitance lights, path guiding) as out of scope, the
			/// same restriction PBRT v3 MMLT documents.
			///
			/// Inherits the same per-thread subpath ownership contract
			/// as ConnectAndEvaluate (see above).
			ConnectionResult ConnectAndEvaluateForMMLT(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				unsigned int s,
				unsigned int t,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera
				) const;

			/// Computes MIS weight using the balance heuristic (power=1).
			Scalar MISWeight(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				unsigned int s,
				unsigned int t
				) const;

			//////////////////////////////////////////////////////////////////
			// Spectral (NM) variants -- single wavelength, scalar throughput
			//////////////////////////////////////////////////////////////////

			/// Result of connecting a single (s,t) strategy at a specific wavelength
			struct ConnectionResultNM
			{
				Scalar		contribution;
				Scalar		misWeight;
				Point2		rasterPos;
				unsigned int s;				///< Number of light subpath vertices
				bool		needsSplat;
				bool		valid;

				ConnectionResultNM() :
				contribution( 0 ),
				misWeight( 0 ),
				rasterPos( Point2( 0, 0 ) ),
				s( 0 ),
				needsSplat( false ),
				valid( false )
				{
				}
			};

			/// NM light subpath.  `branchingThresholdOverride`: negative
			/// (e.g. -1) means "use stabilityConfig.branchingThreshold";
			/// any value in [0,1] overrides the config.  Callers that
			/// cannot handle multi-branch output (BDPT-spectral, MLT-
			/// spectral) must pass 1.0 to force a single chain.
			unsigned int GenerateLightSubpathNM(
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices,
				std::vector<uint32_t>& subpathStarts,
				const Scalar nm,
				const RandomNumberGenerator& random,
				const Scalar branchingThresholdOverride
				) const;

			/// NM eye subpath.  See GenerateLightSubpathNM for the
			/// branchingThresholdOverride semantics.
			unsigned int GenerateEyeSubpathNM(
				const RuntimeContext& rc,
				const Ray& cameraRay,
				const Point2& screenPos,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices,
				std::vector<uint32_t>& subpathStarts,
				const Scalar nm,
				const Scalar branchingThresholdOverride
				) const;

			ConnectionResultNM ConnectAndEvaluateNM(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				unsigned int s,
				unsigned int t,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera,
				const Scalar nm
				) const;

			std::vector<ConnectionResultNM> EvaluateAllStrategiesNM(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera,
				const Scalar nm
				) const;

			/// Evaluates SMS strategies for specular caustic paths (RGB).
			/// For each non-delta eye vertex, traces toward emitters to find
			/// specular object chains, then uses ManifoldSolver to find valid
			/// specular paths.
			std::vector<ConnectionResult> EvaluateSMSStrategies(
				const std::vector<BDPTVertex>& eyeVerts,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera,
				ISampler& sampler
				) const;

			/// Spectral variant of EvaluateSMSStrategies.
			std::vector<ConnectionResultNM> EvaluateSMSStrategiesNM(
				const std::vector<BDPTVertex>& eyeVerts,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera,
				ISampler& sampler,
				const Scalar nm
				) const;

		protected:
			/// Helper: evaluates the BSDF at a surface vertex for given
			/// incoming and outgoing directions.
			RISEPel EvalBSDFAtVertex(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo
				) const;

			/// Helper: evaluates the SPF PDF at a surface vertex
			Scalar EvalPdfAtVertex(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo
				) const;

			/// Helper: checks if a connection between two points is unoccluded.
			bool IsVisible(
				const IRayCaster& caster,
				const Point3& p1,
				const Point3& p2
				) const;

			/// NM helper: evaluates the BSDF at a surface vertex for a single wavelength
			Scalar EvalBSDFAtVertexNM(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo,
				const Scalar nm
				) const;

			/// NM helper: evaluates the SPF PDF at a vertex for a single wavelength
			Scalar EvalPdfAtVertexNM(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo,
				const Scalar nm
				) const;

			/// NM helper: evaluates emitter radiance for a specific wavelength
			Scalar EvalEmitterRadianceNM(
				const BDPTVertex& vertex,
				const Vector3& outDir,
				const Scalar nm
				) const;

			/// Helper: evaluate transmittance along a connection edge
			/// between two points, accounting for participating media.
			///
			/// Connection edge transmittance: the connection between light
			/// and eye subpath endpoints passes through potentially multiple
			/// media.  We evaluate Tr by walking the connection segment and
			/// accumulating per-segment Beer-Lambert transmittance.
			/// This Tr multiplies the connection contribution but is NOT
			/// included in MIS PDFs (see note on transmittance cancellation
			/// in the MISWeight documentation).
			RISEPel EvalConnectionTransmittance(
				const Point3& p1,
				const Point3& p2,
				const IScene& scene,
				const IRayCaster& caster,
				const IObject* pStartMediumObject,
				const IMedium* pStartMedium
				) const;

			/// Spectral variant of EvalConnectionTransmittance
			Scalar EvalConnectionTransmittanceNM(
				const Point3& p1,
				const Point3& p2,
				const IScene& scene,
				const IRayCaster& caster,
				const Scalar nm,
				const IObject* pStartMediumObject,
				const IMedium* pStartMedium
				) const;

			// BSSRDF sampling is provided by the shared utility
			// BSSRDFSampling::SampleEntryPoint() in
			// ../Utilities/BSSRDFSampling.h

		public:
			/// Recompute throughputNM for each vertex in a stored subpath
			/// at a companion wavelength.  Used by HWSS BDPT to re-evaluate
			/// spectral transport along the hero's geometric path.
			///
			/// Walks the stored vertex array and adjusts throughputNM by
			/// the ratio of BSDF values at the companion vs hero wavelength.
			/// Delta vertices use a ratio of 1.0 (exact for non-dispersive
			/// specular interactions).  Light endpoint emission is re-evaluated
			/// at the companion wavelength.
			///
			/// @param verts        Vertex array (modified in place)
			/// @param isLightPath  True for light subpath, false for eye subpath
			/// @param heroNM       Hero wavelength (nm)
			/// @param companionNM  Companion wavelength (nm)
			/// @param scene        Scene reference
			/// @param caster       Ray caster reference
			void RecomputeSubpathThroughputNM(
				std::vector<BDPTVertex>& verts,
				bool isLightPath,
				Scalar heroNM,
				Scalar companionNM,
				const IScene& scene,
				const IRayCaster& caster
				) const;

			/// Checks whether any delta vertex in the given subpath has
			/// wavelength-dependent IOR (dispersion).  If so, returns
			/// true — the caller should terminate companion wavelengths.
			///
			/// Walks the stored vertices, reconstructs a minimal
			/// RayIntersectionGeometric from vertex geometry, and
			/// compares GetSpecularInfoNM at hero vs companion wavelength.
			static bool HasDispersiveDeltaVertex(
				const std::vector<BDPTVertex>& verts,
				Scalar heroNM,
				Scalar companionNM
				);
		};
	}
}

#endif
