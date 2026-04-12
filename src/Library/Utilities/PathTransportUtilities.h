//////////////////////////////////////////////////////////////////////
//
//  PathTransportUtilities.h - Shared transport decision utilities
//
//  Pure helper functions used by both the unidirectional path tracer
//  (PathTracingShaderOp) and the bidirectional path tracer
//  (BDPTIntegrator).  Each function is a stateless decision or
//  computation that was previously duplicated across both integrators.
//
//  RUSSIAN ROULETTE:
//    The survival probability is:
//      rrProb = min(1, currentThroughput / max(prevThroughput, threshold))
//    where currentThroughput is the throughput magnitude at the current
//    vertex and prevThroughput is the throughput at the previous vertex.
//    The threshold prevents runaway compensation on low-energy paths.
//    This formula is used identically in PT and BDPT (Veach thesis
//    Section 10.4.1).  The caller is responsible for dividing throughput
//    by survivalProb after the call.
//
//  CONTRIBUTION CLAMPING:
//    Clamp the maximum channel of an RGB contribution (or absolute
//    value of a scalar spectral contribution) to a user-specified
//    limit.  Used for both direct and indirect sample clamping.
//
//  PER-TYPE BOUNCE LIMITS:
//    Enforce independent depth limits for diffuse, glossy, transmission,
//    and translucent ray types.  Two variants: one for the PT's
//    RAY_STATE propagation model, one for BDPT's standalone counters.
//
//  PATH GUIDING HELPERS (requires RISE_ENABLE_OPENPGL):
//    One-sample MIS blending between BSDF and guiding distributions,
//    plus RIS-based candidate selection.  The RIS helpers draw N=2
//    candidates (one from BSDF, one from the guide), evaluate a
//    target function at each, and select proportional to RIS weight.
//    The adaptive alpha scaling that drives how much weight the
//    guiding distribution receives lives in the rasterizers; see
//    PixelBasedPelRasterizer.cpp and BDPTRasterizerBase.cpp.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 2, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATH_TRANSPORT_UTILITIES_
#define PATH_TRANSPORT_UTILITIES_

#include "Math3D/Math3D.h"
#include "Color/ColorMath.h"
#include "StabilityConfig.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/ISPF.h"

namespace RISE
{
	namespace PathTransportUtilities
	{
		//////////////////////////////////////////////////////////////////////
		// Russian Roulette
		//////////////////////////////////////////////////////////////////////

		/// Result of a Russian roulette evaluation.  The caller
		/// multiplies throughput by (1 / survivalProb) when the path
		/// survives.  When terminate is true, the path should be killed.
		struct RussianRouletteResult
		{
			bool	terminate;		///< True if the path should be killed
			Scalar	survivalProb;	///< 1.0 if RR was not applied (depth < rrMinDepth)
		};

		/// Pure Russian roulette decision function.
		///
		/// Does NOT modify throughput — the caller is responsible for
		/// the (1 / survivalProb) compensation.
		///
		/// \param pathDepth           Current path depth (bounces from origin)
		/// \param rrMinDepth          Minimum depth before RR activates
		/// \param rrThreshold         Throughput floor to prevent runaway compensation
		/// \param currentThroughputMax  MaxValue(currentThroughput) or fabs(throughputNM)
		/// \param prevThroughputMax    MaxValue(prevThroughput) or importance at parent
		/// \param randomSample        Pre-drawn uniform [0,1) sample
		/// \return RussianRouletteResult with terminate flag and survival probability
		inline RussianRouletteResult EvaluateRussianRoulette(
			unsigned int pathDepth,
			unsigned int rrMinDepth,
			Scalar rrThreshold,
			Scalar currentThroughputMax,
			Scalar prevThroughputMax,
			Scalar randomSample
			)
		{
			RussianRouletteResult result;
			result.terminate = false;
			result.survivalProb = 1.0;

			if( pathDepth >= rrMinDepth )
			{
				const Scalar rrProb = r_min( Scalar(1.0),
					currentThroughputMax / r_max( prevThroughputMax, rrThreshold ) );
				if( randomSample >= rrProb ) {
					result.terminate = true;
				} else if( rrProb > 0 && rrProb < 1.0 ) {
					result.survivalProb = rrProb;
				}
			}
			return result;
		}

		//////////////////////////////////////////////////////////////////////
		// Contribution Clamping
		//////////////////////////////////////////////////////////////////////

		/// Clamp an RGB contribution so that its maximum channel does not
		/// exceed 'limit'.  Returns the original value when limit <= 0
		/// (disabled) or when the contribution is already within bounds.
		inline RISEPel ClampContribution(
			const RISEPel& contrib,
			const Scalar limit
			)
		{
			if( limit <= 0 ) {
				return contrib;
			}
			const Scalar maxVal = ColorMath::MaxValue( contrib );
			if( maxVal > limit ) {
				return contrib * (limit / maxVal);
			}
			return contrib;
		}

		/// Scalar variant for the spectral (NM) path.
		inline Scalar ClampContributionNM(
			const Scalar contrib,
			const Scalar limit
			)
		{
			if( limit <= 0 ) {
				return contrib;
			}
			const Scalar absVal = fabs( contrib );
			if( absVal > limit ) {
				return contrib * (limit / absVal);
			}
			return contrib;
		}

		//////////////////////////////////////////////////////////////////////
		// Per-Type Bounce Limits
		//////////////////////////////////////////////////////////////////////

		/// Full version for the unidirectional PT: propagates per-type
		/// bounce counters and glossy filter width from parent RAY_STATE
		/// to child RAY_STATE, then checks whether the scatter type
		/// exceeds its configured limit.
		///
		/// \return True if the bounce limit for this scatter type is exceeded
		inline bool PropagateBounceLimits(
			const IRayCaster::RAY_STATE& parent,
			IRayCaster::RAY_STATE& child,
			const ScatteredRay& scat,
			const StabilityConfig* pConfig
			)
		{
			child.diffuseBounces      = parent.diffuseBounces;
			child.glossyBounces       = parent.glossyBounces;
			child.transmissionBounces = parent.transmissionBounces;
			child.translucentBounces  = parent.translucentBounces;
			child.glossyFilterWidth   = parent.glossyFilterWidth;

			if( !pConfig ) {
				return false;
			}

			// Accumulate glossy filter width for non-delta glossy bounces.
			if( scat.type == ScatteredRay::eRayReflection && !scat.isDelta &&
				pConfig->filterGlossy > 0 )
			{
				child.glossyFilterWidth = parent.glossyFilterWidth + pConfig->filterGlossy;
			}

			switch( scat.type )
			{
				case ScatteredRay::eRayDiffuse:
					child.diffuseBounces++;
					return child.diffuseBounces > pConfig->maxDiffuseBounce;
				case ScatteredRay::eRayReflection:
					child.glossyBounces++;
					return child.glossyBounces > pConfig->maxGlossyBounce;
				case ScatteredRay::eRayRefraction:
					child.transmissionBounces++;
					return child.transmissionBounces > pConfig->maxTransmissionBounce;
				case ScatteredRay::eRayTranslucent:
					child.translucentBounces++;
					return child.translucentBounces > pConfig->maxTranslucentBounce;
				default:
					return false;
			}
		}

		/// Standalone version for BDPT subpath generators: increments
		/// the matching per-type counter and returns true if the
		/// configured limit for that type is exceeded.
		///
		/// Unlike PropagateBounceLimits, this does not touch RAY_STATE
		/// or glossy filter width — BDPT tracks bounce counts in local
		/// variables within its subpath generation loops.
		///
		/// \return True if the bounce limit for this scatter type is exceeded
		inline bool ExceedsBounceLimitForType(
			ScatteredRay::ScatRayType scatType,
			unsigned int& diffuseBounces,
			unsigned int& glossyBounces,
			unsigned int& transmissionBounces,
			unsigned int& translucentBounces,
			const StabilityConfig& config
			)
		{
			switch( scatType )
			{
				case ScatteredRay::eRayDiffuse:
					return (++diffuseBounces > config.maxDiffuseBounce);
				case ScatteredRay::eRayReflection:
					return (++glossyBounces > config.maxGlossyBounce);
				case ScatteredRay::eRayRefraction:
					return (++transmissionBounces > config.maxTransmissionBounce);
				case ScatteredRay::eRayTranslucent:
					return (++translucentBounces > config.maxTranslucentBounce);
				default:
					return false;
			}
		}

		//////////////////////////////////////////////////////////////////////
		// MIS Utilities
		//////////////////////////////////////////////////////////////////////

		/// Power heuristic weight: w = pa^2 / (pa^2 + pb^2)
		inline Scalar PowerHeuristic(
			const Scalar pa,
			const Scalar pb
			)
		{
			const Scalar pa2 = pa * pa;
			return pa2 / (pa2 + pb * pb);
		}

#ifdef RISE_ENABLE_OPENPGL
		//////////////////////////////////////////////////////////////////////
		// Path Guiding MIS Helpers
		//
		// One-sample MIS blending between a BSDF-sampled direction and a
		// guiding-distribution-sampled direction.  The combined PDF is:
		//   combinedPdf = alpha * guidePdf + (1 - alpha) * bsdfPdf
		// where alpha is the probability of sampling from the guide.
		//
		// These helpers are intentionally minimal: they compute the PDF
		// combination and the sampling decision, leaving BSDF/PDF
		// evaluation at each call site.  This keeps the abstraction thin
		// enough that Stage 8's RIS-based replacement can swap them out
		// without large refactoring.
		//////////////////////////////////////////////////////////////////////

		/// Computes the one-sample MIS combined PDF for a guiding blend.
		///
		/// \param alpha     Probability of sampling from the guide distribution
		/// \param guidePdf  PDF value from the guiding distribution
		/// \param bsdfPdf   PDF value from the BSDF/SPF sampling
		/// \return Combined PDF under one-sample MIS
		inline Scalar GuidingCombinedPdf(
			Scalar alpha,
			Scalar guidePdf,
			Scalar bsdfPdf
			)
		{
			return alpha * guidePdf + (1.0 - alpha) * bsdfPdf;
		}

		/// Determines whether to sample from the guiding distribution
		/// or keep the BSDF-sampled direction, given a uniform random
		/// number on [0, 1).
		///
		/// \param alpha  Probability of sampling from the guide
		/// \param xi     Uniform random sample
		/// \return True if the sample should come from the guide distribution
		inline bool ShouldUseGuidedSample(
			Scalar alpha,
			Scalar xi
			)
		{
			return xi < alpha;
		}

		//////////////////////////////////////////////////////////////////////
		// RIS-Based Path Guiding Helpers
		//
		// Resampled Importance Sampling draws N candidates from a mixture
		// of the BSDF and guiding distributions, evaluates a target
		// function at each, and selects one proportional to its RIS
		// weight.  This produces lower variance than one-sample MIS at
		// modest additional cost.
		//
		// The target function follows the Cycles formulation:
		//   target = avgBsdfEval * ((1-p) / (2*PI) + p * incomingRadPdf)
		// where p is the guiding sampling probability and
		// incomingRadPdf is the raw learned radiance PDF from OpenPGL
		// (before cosine product).
		//
		// The proposal PDF for each candidate is:
		//   proposalPdf = 0.5 * (bsdfPdf + guidePdf)
		// reflecting equal probability of drawing from either source.
		//
		// After selection, the effective PDF for the chosen candidate is:
		//   effectivePdf = risTarget * (N / sumWeights)
		// which is the properly normalized RIS estimator.
		//////////////////////////////////////////////////////////////////////

		/// RGB RIS candidate.  One candidate is drawn from the BSDF,
		/// one from the guiding distribution.  Both are evaluated under
		/// the target function for RIS selection.
		struct GuidingRISCandidate
		{
			Vector3		direction;
			RISEPel		bsdfEval;
			Scalar		bsdfPdf;
			Scalar		guidePdf;
			Scalar		incomingRadPdf;
			Scalar		cosTheta;
			Scalar		risTarget;
			Scalar		risPdf;
			Scalar		risWeight;
			bool		valid;
		};

		/// Spectral (NM) RIS candidate.
		struct GuidingRISCandidateNM
		{
			Vector3		direction;
			Scalar		bsdfEvalNM;
			Scalar		bsdfPdf;
			Scalar		guidePdf;
			Scalar		incomingRadPdf;
			Scalar		cosTheta;
			Scalar		risTarget;
			Scalar		risPdf;
			Scalar		risWeight;
			bool		valid;
		};

		/// Compute the RIS target function value.
		///
		/// In Cycles the BSDF evaluation already includes the cosine
		/// factor; in RISE the cosine is separate.  We include
		/// cosTheta here so that the target function matches the
		/// throughput numerator (bsdfEval * cosTheta), preventing
		/// fireflies from the cos/target mismatch.
		///
		/// \param avgBsdfEval     Scalar measure of the BSDF evaluation (max channel)
		/// \param cosTheta        Absolute cosine between scattered dir and surface normal
		/// \param incomingRadPdf  Incoming radiance PDF from OpenPGL (pre-cosine-product)
		/// \param blendP          Guiding sampling probability (typically 0.5)
		/// \return Target function value
		inline Scalar GuidingRISTarget(
			Scalar avgBsdfEval,
			Scalar cosTheta,
			Scalar incomingRadPdf,
			Scalar blendP
			)
		{
			return avgBsdfEval * cosTheta *
				((1.0 - blendP) * (0.5 * INV_PI) + blendP * incomingRadPdf);
		}

		/// Compute the RIS proposal PDF (equal-weight mixture of BSDF
		/// and guide).  The result is positive whenever at least one
		/// component is positive, so a BSDF candidate is valid even
		/// when the guide has zero support for that direction.
		///
		/// \param bsdfPdf   BSDF sampling PDF
		/// \param guidePdf  Guiding distribution sampling PDF (may be zero)
		/// \return Proposal PDF
		inline Scalar GuidingRISProposalPdf(
			Scalar bsdfPdf,
			Scalar guidePdf
			)
		{
			return 0.5 * (bsdfPdf + guidePdf);
		}

		/// Select one candidate from an array proportional to RIS
		/// weights.  Returns the selected index and outputs the
		/// effective PDF for the chosen candidate.
		///
		/// When all candidates are invalid (sumWeights == 0), returns
		/// the first candidate index with effectivePdf = 0, which
		/// produces zero throughput at the call site (safe).
		///
		/// \param candidates   Array of N candidates
		/// \param count        Number of candidates
		/// \param xi           Uniform random sample in [0, 1)
		/// \param effectivePdf Output: effective PDF for selected candidate
		/// \return Index of the selected candidate
		inline unsigned int GuidingRISSelectCandidate(
			const GuidingRISCandidate* candidates,
			unsigned int count,
			Scalar xi,
			Scalar& effectivePdf
			)
		{
			Scalar sumWeights = 0;
			for( unsigned int i = 0; i < count; i++ ) {
				sumWeights += candidates[i].risWeight;
			}

			if( sumWeights <= NEARZERO ) {
				effectivePdf = 0;
				return 0;
			}

			const Scalar threshold = xi * sumWeights;
			Scalar cumulative = 0;
			unsigned int selected = 0;
			for( unsigned int i = 0; i < count; i++ ) {
				cumulative += candidates[i].risWeight;
				if( cumulative >= threshold ) {
					selected = i;
					break;
				}
			}

			effectivePdf = candidates[selected].risTarget *
				(static_cast<Scalar>( count ) / sumWeights);
			return selected;
		}

		/// Spectral (NM) variant of RIS candidate selection.
		inline unsigned int GuidingRISSelectCandidateNM(
			const GuidingRISCandidateNM* candidates,
			unsigned int count,
			Scalar xi,
			Scalar& effectivePdf
			)
		{
			Scalar sumWeights = 0;
			for( unsigned int i = 0; i < count; i++ ) {
				sumWeights += candidates[i].risWeight;
			}

			if( sumWeights <= NEARZERO ) {
				effectivePdf = 0;
				return 0;
			}

			const Scalar threshold = xi * sumWeights;
			Scalar cumulative = 0;
			unsigned int selected = 0;
			for( unsigned int i = 0; i < count; i++ ) {
				cumulative += candidates[i].risWeight;
				if( cumulative >= threshold ) {
					selected = i;
					break;
				}
			}

			effectivePdf = candidates[selected].risTarget *
				(static_cast<Scalar>( count ) / sumWeights);
			return selected;
		}
#endif
	}
}

#endif
