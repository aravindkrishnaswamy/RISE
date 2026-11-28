//////////////////////////////////////////////////////////////////////
//
//  PathTransportUtilities.h - Shared stateless transport utilities
//  for unidirectional (PT) and bidirectional (BDPT/VCM/MLT) integrators:
//    - Sampler stream layouts for PT volume walks and HWSS SMS lanes
//    - Russian roulette (Veach §10.4.1):
//        rrProb = min(1, currentThroughput / max(prevThroughput, threshold))
//      Caller divides throughput by survivalProb on survival.
//    - Contribution clamping (RISEPel max-channel / Scalar abs)
//    - Per-type bounce limit tracking (diffuse, glossy, transmission, translucent)
//    - OpenPGL path guiding MIS, partition weights, and RIS helpers
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
#include "FiniteMath.h"
#include "StabilityConfig.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IMaterial.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	namespace PathTransportUtilities
	{
		inline bool IsPositiveFiniteDensity( const Scalar density )
		{
			// Density support is exact: every finite positive value is legal.
			// An epsilon cutoff would discard rare events and their compensating
			// weights, biasing the estimator.
			return RISE::IsFiniteDouble( density ) && density > 0;
		}

		// Scalar NM analog tracking uses the same deterministic transmittance
		// T_det for the physical factor and the no-event atom P_0.  They cancel
		// exactly, leaving only the reciprocal of the explicitly selected
		// distance-technique mass (1 for pure delta tracking, 0.5 for its half
		// of the delta-tracking/equiangular mixture).
		inline Scalar NMNoEventSurvivalWeight( const Scalar techniqueMass )
		{
			return IsPositiveFiniteDensity(techniqueMass) ?
				Scalar(1.0)/techniqueMass : Scalar(0.0);
		}

        // A single-label BSDF keeps that label when the guide replaces its
        // direction. Mixed-label aggregate guide draws retain the DL-483 pin.
        inline ScatteredRay::ScatRayType GuidedSingleLabelType(const IMaterial* material)
        {
            return material && material->ConnectionScatterTypes() == (1u << ScatteredRay::eRayReflection)
                ? ScatteredRay::eRayReflection : ScatteredRay::eRayDiffuse;
        }

		//////////////////////////////////////////////////////////////////////
		// PTVolumeWalkStream (DL-247 / DL-283; checked by SobolDimensionBudgetTest).
		//
		// Dedicated stream per (wavelength lane, volume scatterIndex) in
		// [4096, 8192): `4096 + 1024 * lane + (scatterIndex % 1024)` for
		// lanes 0..3. Keeps open-ended volume walks and HWSS lanes from
		// overrunning the vertex stream (`16 + depth`, stride 32) into the
		// surface hand-off's streams. Ceilings: `max_volume_bounce <= 1024`
		// and PT main-loop depth `<= 4080`. Streams lie in SobolSampler's
		// Owen-permuted wrap region (past stream 256).
		//////////////////////////////////////////////////////////////////////
		static const int kPTVolumeWalkStreamBase = 4096;
		static const unsigned int kPTVolumeWalkLaneStride = 1024;

		inline int PTVolumeWalkStream( const unsigned int lane, const unsigned int scatterIndex )
		{
			return kPTVolumeWalkStreamBase + static_cast<int>(
				lane * kPTVolumeWalkLaneStride + ( scatterIndex % kPTVolumeWalkLaneStride ) );
		}

		//////////////////////////////////////////////////////////////////////
		// PTSMSLaneStream (DL-453; checked by SobolDimensionBudgetTest).
		//
		// Dedicated forked stream (`SobolSampler::ForkStream`) per HWSS-lane
		// SMS evaluation (extended or legacy) at PT `depth` and `lane` 0..3,
		// preventing per-lane NEE+SMS from overrunning the vertex stream.
		// Occupies [141312, 145408), past BDPT/VCM's deep-walk block (141266);
		// `depth` wraps mod 1024. RGB/NM run SMS once per vertex on the vertex stream.
		//////////////////////////////////////////////////////////////////////
		static const int kPTSMSLaneStreamBase = 141312;
		static const unsigned int kPTSMSLanes = 4;
		static const unsigned int kPTSMSLaneDepthCap = 1024;
		static const int kPTSMSLaneStreamEnd = kPTSMSLaneStreamBase +
			static_cast<int>( kPTSMSLanes * kPTSMSLaneDepthCap );

		inline int PTSMSLaneStream( const unsigned int depth, const unsigned int lane )
		{
			return kPTSMSLaneStreamBase + static_cast<int>(
				kPTSMSLanes * ( depth % kPTSMSLaneDepthCap ) + ( lane % kPTSMSLanes ) );
		}

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

		/// Exact probability that the ordinary continuation roulette survives.
		/// Kept separate from the random decision so NEE can evaluate the density
		/// of its counterfactual march strategy before that strategy is sampled.
		inline Scalar RussianRouletteSurvivalProbability(
			unsigned int pathDepth,
			unsigned int rrMinDepth,
			Scalar rrThreshold,
			Scalar currentThroughputMax,
			Scalar prevThroughputMax
			)
		{
			if( pathDepth < rrMinDepth ) return 1.0;
			return r_min( Scalar(1.0),
				currentThroughputMax / r_max( prevThroughputMax, rrThreshold ) );
		}

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
				const Scalar rrProb = RussianRouletteSurvivalProbability(
					pathDepth, rrMinDepth, rrThreshold,
					currentThroughputMax, prevThroughputMax );
				if( randomSample >= rrProb ) {
					result.terminate = true;
				} else if( rrProb > 0 && rrProb < 1.0 ) {
					result.survivalProb = rrProb;
				}
			}
			return result;
		}

		//////////////////////////////////////////////////////////////////////
		// Contribution Clamping (RISEPel via MaxValue, Scalar via fabs)
		//////////////////////////////////////////////////////////////////////

		inline Scalar ClampMagnitude( const RISEPel& v )
		{
			return ColorMath::MaxValue( v );
		}

		inline Scalar ClampMagnitude( const Scalar v )
		{
			return fabs( v );
		}

		/// Clamp a contribution so that its magnitude does not exceed
		/// 'limit'.  Returns the original value when limit <= 0
		/// (disabled) or when the contribution is already within
		/// bounds.  T is deduced as either RISEPel or Scalar.
		template<class T>
		inline T ClampContribution(
			const T& contrib,
			const Scalar limit
			)
		{
			if( limit <= 0 ) {
				return contrib;
			}
			const Scalar m = ClampMagnitude( contrib );
			if( m > limit ) {
				return contrib * (limit / m);
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

		/// Resolve a replaced direction's medium without changing its proposal.
		/// A crossing continues on the incoming ray's geometric side and takes
		/// the selected SPF transition. A reflection keeps the input stack.
		/// Call only for replacements; ordinary SPF continuations keep their
		/// original state. Exact RIS candidate-0 reuse also keeps that state.
		/// The sign test is invariant to ray-oriented normal flips. A missing
		/// geometric normal falls back to the sampling frame; tangent sides
		/// cannot establish a transition and retain the input stack.
		/// Returned pointers are borrowed until the scattered container dies.
		inline const IORStack* GuidedContinuationIORStack(
			const ScatteredRay& selected,
			const IORStack& current,
			const RayIntersection& ri,
			const Vector3& direction,
			IORStack& generatedStack
			)
		{
			if( !selected.ior_stack ) {
				Vector3 normal = ri.geometric.vGeomNormal;
				if( Vector3Ops::Dot( normal, normal ) <= NEARZERO ) normal = ri.geometric.onb.w();
				const Scalar before = Vector3Ops::Dot( ri.geometric.ray.Dir(), normal );
				const Scalar after = Vector3Ops::Dot( direction, normal );
				if( (before > 0 && after > 0) || (before < 0 && after < 0) ) {
					if( ri.pMaterial ) {
						SpecularInfo info = ri.pMaterial->GetSpecularInfo(ri.geometric, current);
						if( info.canRefract || info.hasInterior ) {
							generatedStack = current;
							generatedStack.SetCurrentObject( ri.pObject );
							const bool bEntering = !generatedStack.containsCurrent();
							if( bEntering ) {
								generatedStack.push( info.canRefract ? info.ior : generatedStack.top() );
							} else {
								generatedStack.pop();
							}
							return &generatedStack;
						}
					}
				}
				return &current;
			}
			const Vector3& original = selected.ray.Dir();
			if( direction.x == original.x && direction.y == original.y && direction.z == original.z ) {
				return selected.ior_stack;
			}
			Vector3 normal = ri.geometric.vGeomNormal;
			if( Vector3Ops::Dot( normal, normal ) <= NEARZERO ) normal = ri.geometric.onb.w();
			const Scalar before = Vector3Ops::Dot( ri.geometric.ray.Dir(), normal );
			const Scalar after = Vector3Ops::Dot( direction, normal );
			return ((before > 0 && after > 0) || (before < 0 && after < 0))
				? selected.ior_stack : &current;
		}

#ifdef RISE_ENABLE_OPENPGL
		//////////////////////////////////////////////////////////////////////
		// Path Guiding MIS Helpers
		//
		// One-sample MIS blend: combinedPdf = alpha * guidePdf + (1-alpha) * bsdfPdf.
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

		/// Actual one-sample guiding-mixture density for a selected technique.
		/// A zero guide density is valid on a phase/BSDF-selected direction and
		/// still leaves (1-alpha)*p_phase support.  A guide-selected sample with
		/// zero density is instead an explicit null sample; silently falling back
		/// to a pre-drawn phase direction would add an unevaluable failure mass.
		inline Scalar GuidingSelectedMixturePdf(
			const Scalar alpha,
			const Scalar guidePdf,
			const Scalar phasePdf,
			const bool guideTechniqueSelected
			)
		{
			if( !RISE::IsFiniteDouble( alpha ) || alpha < 0 || alpha > 1 ||
				!RISE::IsFiniteDouble( guidePdf ) || guidePdf < 0 ||
				!RISE::IsFiniteDouble( phasePdf ) || phasePdf < 0 ||
				( guideTechniqueSelected && guidePdf == 0 ) ) {
				return 0;
			}
			const Scalar mixturePdf = GuidingCombinedPdf( alpha, guidePdf, phasePdf );
			return IsPositiveFiniteDensity( mixturePdf )
				? mixturePdf : 0;
		}

		//////////////////////////////////////////////////////////////////////
		// Guided continuation partition (DL-67; docs/DEBT_LEDGER.md).
		//
		// Combines BSDF (Scatter + RandomlySelect, weight kray_I/q_I * W_b/(1-a))
		// and guide (weight f*cos/g * W_g/a) via the deterministic partition:
		//   W_g(w) = a g(w) / (a g(w) + (1 - a) p_agg(w)),  W_b = 1 - W_g
		// with `p_agg = ISPF::Pdf()`. Unbiased when: (1) technique choice `a` is
		// realization-independent; (2) IBSDF::value matches SPF kray; (3) 0 < a < 1
		// wherever BSDF owns delta mass (`GuidingOneSampleProbability`); (4) post-
		// choice continuation state is technique-independent (not met under per-type
		// bounce caps for mixed-label guide draws).
		//////////////////////////////////////////////////////////////////////

		/// Guide partition weight `W_g(w)` (0 when `guidePdf <= 0`).
		inline Scalar GuidingPartitionGuideWeight(
			const Scalar mixA,
			const Scalar guidePdf,
			const Scalar aggregatePdf
			)
		{
			const Scalar num = mixA * ( guidePdf > 0 ? guidePdf : Scalar( 0 ) );
			const Scalar den = num + ( 1.0 - mixA ) * ( aggregatePdf > 0 ? aggregatePdf : Scalar( 0 ) );
			return den > 0 ? num / den : Scalar( 0 );
		}

		/// BSDF partition weight `W_b(w) = 1 - W_g(w)` for a non-delta lobe.
		/// Returns 1 when `aggregatePdf <= 0` so unrepresented lobes stay un-partitioned.
		inline Scalar GuidingPartitionBsdfWeight(
			const Scalar mixA,
			const Scalar guidePdf,
			const Scalar aggregatePdf
			)
		{
			if( aggregatePdf <= 0 ) {
				return 1;
			}
			const Scalar num = ( 1.0 - mixA ) * aggregatePdf;
			const Scalar den = num + mixA * ( guidePdf > 0 ? guidePdf : Scalar( 0 ) );
			return den > 0 ? num / den : Scalar( 1 );
		}

		/// Upper clamp on one-sample guide probability `a` (DL-67; docs/DEBT_LEDGER.md):
		/// keeps `0 < a < 1` so delta lobes (priced `kray/q/(1-a)`) are never dropped
		/// and their variance amplification `1/(1-a)` stays <= 10x.
		static const Scalar kGuidingMaxOneSampleProbability = 0.9;

		/// The clamped one-sample firing probability: 0 for a non-positive
		/// (or NaN) alpha, `kGuidingMaxOneSampleProbability` above it.
		inline Scalar GuidingOneSampleProbability( const Scalar alpha )
		{
			if( !( alpha > 0 ) ) {
				return 0;
			}
			return alpha < kGuidingMaxOneSampleProbability ?
				alpha : kGuidingMaxOneSampleProbability;
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
		// Draws N candidates from the 50/50 BSDF/guide proposal mixture:
		//   target       = avgBsdfEval * cosTheta * ((1-p)/(2*PI) + p*incomingRadPdf)
		//   proposalPdf  = 0.5 * (bsdfPdf + guidePdf)
		//   effectivePdf = risTarget * (N / sumWeights)
		//////////////////////////////////////////////////////////////////////

		/// RIS candidate.  One candidate is drawn from the BSDF, one
		/// from the guiding distribution.  Both are evaluated under
		/// the target function for RIS selection.
		///
		/// T is the BSDF evaluation type: RISEPel for RGB rendering,
		/// Scalar for spectral (NM) rendering.  Callers write either
		/// `GuidingRISCandidate<RISEPel>` or `GuidingRISCandidate<Scalar>`.
		template<class T>
		struct GuidingRISCandidate
		{
			Vector3		direction;
			T			bsdfEval;
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
		/// Templated on T so the same body serves both RGB and NM
		/// candidate arrays.  T is deduced from the pointer type.
		///
		/// \param candidates   Array of N candidates
		/// \param count        Number of candidates
		/// \param xi           Uniform random sample in [0, 1)
		/// \param effectivePdf Output: effective PDF for selected candidate
		/// \return Index of the selected candidate
		template<class T>
		inline unsigned int GuidingRISSelectCandidate(
			const GuidingRISCandidate<T>* candidates,
			unsigned int count,
			Scalar xi,
			Scalar& effectivePdf
			)
		{
			Scalar maxWeight = 0;
			for( unsigned int i = 0; i < count; i++ ) {
				const Scalar weight = candidates[i].risWeight;
				if( IsPositiveFiniteDensity(weight) ) {
					maxWeight = r_max(maxWeight,weight);
				}
			}

			if( !IsPositiveFiniteDensity(maxWeight) ) {
				effectivePdf = 0;
				return 0;
			}

			// Scale by the largest weight before summing.  This preserves every
			// finite positive RIS atom without an epsilon support cutoff and keeps
			// both the selection sum and the effective-density ratio finite when
			// the unscaled weights are uniformly tiny or would overflow in sum.
			Scalar scaledSum = 0;
			for( unsigned int i = 0; i < count; i++ ) {
				const Scalar weight = candidates[i].risWeight;
				if( IsPositiveFiniteDensity(weight) ) {
					scaledSum += weight/maxWeight;
				}
			}
			const Scalar threshold = xi * scaledSum;
			Scalar cumulative = 0;
			unsigned int selected = 0;
			for( unsigned int i = 0; i < count; i++ ) {
				const Scalar weight = candidates[i].risWeight;
				if( IsPositiveFiniteDensity(weight) ) {
					cumulative += weight/maxWeight;
					if( cumulative >= threshold ) {
						selected = i;
						break;
					}
				}
			}

			effectivePdf = (candidates[selected].risTarget/maxWeight) *
				(static_cast<Scalar>( count )/scaledSum);
			if( !IsPositiveFiniteDensity(effectivePdf) ) {
				effectivePdf = 0;
			}
			return selected;
		}
#endif
	}
}

#endif
