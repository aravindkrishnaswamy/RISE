//////////////////////////////////////////////////////////////////////
//
//  MISWeights.h - Multiple Importance Sampling weight functions
//
//  Provides weight computation functions beyond the standard power
//  heuristic already in PathTransportUtilities.h.  Each function
//  takes PDF values and returns a normalized weight in [0, 1].
//
//  BALANCE HEURISTIC:
//    w(pa, pb) = pa / (pa + pb)
//    Veach's simplest MIS weight.  Provably never increases variance
//    by more than a factor of N (number of techniques) versus the
//    optimal single-technique estimator.
//
//  OPTIMAL MIS (2-TECHNIQUE CASE):
//    Kondapaneni et al. SIGGRAPH 2019 showed that variance-minimizing
//    weights for 2 techniques reduce to an alpha-weighted balance
//    heuristic:
//      w_a(x) = alpha * pa / (alpha * pa + (1-alpha) * pb)
//    where alpha = M_b / (M_a + M_b) and M_i = E[f^2 / p_i] is the
//    second moment of technique i.  When alpha = 0.5, this reduces
//    to the standard balance heuristic.  The alpha is estimated from
//    training data by the OptimalMISAccumulator class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MIS_WEIGHTS_
#define MIS_WEIGHTS_

#include "Math3D/Math3D.h"
#include "FiniteMath.h"

namespace RISE
{
	namespace Implementation
	{
		struct VolumeEmissionPivotState;
	}

	/// Phase-B thermal-volume MIS state for the segment launched by a vertex.
	/// This deliberately does not live in public IRayCaster::RAY_STATE: adding
	/// fields there would consume its historical tail padding, whose bytes are
	/// uninitialized in callers compiled against the old inline constructor.
	struct VolumeEmissionSegmentState
	{
		bool competitionAvailable;
		bool continuationSingular;
		const Implementation::VolumeEmissionPivotState* pivots;
		Scalar directionPdf;
		Scalar logBoundarySurvival;
		Scalar distanceOffset;

		VolumeEmissionSegmentState(
			const bool competitionAvailable_ = false,
			const bool continuationSingular_ = false,
			const Implementation::VolumeEmissionPivotState* pivots_ = 0,
			const Scalar directionPdf_ = 0.0,
			const Scalar logBoundarySurvival_ = 0.0,
			const Scalar distanceOffset_ = 0.0 ) :
			competitionAvailable( competitionAvailable_ ),
			continuationSingular( continuationSingular_ ),
			pivots( pivots_ ),
			directionPdf( directionPdf_ ),
			logBoundarySurvival( logBoundarySurvival_ ),
			distanceOffset( distanceOffset_ )
		{}
	};

	/// Preserve the originating strategy while crossing one exact null
	/// boundary.  No-event probabilities accumulate in log space so a long
	/// enclosure chain cannot silently lose its proposal density to an
	/// intermediate product underflow.
	inline VolumeEmissionSegmentState AdvanceVolumeEmissionSegmentState(
		const VolumeEmissionSegmentState& state,
		const Scalar noEventProbability,
		const Scalar segmentDistance
		)
	{
		Scalar nextLogSurvival = -RISE_INFINITY;
		Scalar nextDistanceOffset = RISE_INFINITY;
		if( RISE::IsFiniteDouble(state.logBoundarySurvival) &&
			RISE::IsFiniteDouble(noEventProbability) &&
			RISE::IsFiniteDouble(state.distanceOffset) &&
			RISE::IsFiniteDouble(segmentDistance) &&
			noEventProbability > 0.0 && noEventProbability <= 1.0 &&
			state.distanceOffset >= 0.0 && segmentDistance >= 0.0 ) {
			nextLogSurvival = state.logBoundarySurvival + log(noEventProbability);
			nextDistanceOffset = state.distanceOffset + segmentDistance;
		}
		return VolumeEmissionSegmentState(
			state.competitionAvailable, state.continuationSingular,
			state.pivots, state.directionPdf, nextLogSurvival,
			nextDistanceOffset );
	}

	inline const VolumeEmissionSegmentState*& ActiveVolumeEmissionSegmentState()
	{
		static thread_local const VolumeEmissionSegmentState* pState = 0;
		return pState;
	}

	/// Synchronous, request-local propagation across recursive CastRay calls.
	/// The scope points at immutable stack storage and restores nesting exactly;
	/// a public/root call outside a scope receives the weight-one default.
	class VolumeEmissionSegmentStateScope
	{
	public:
		explicit VolumeEmissionSegmentStateScope(
			const VolumeEmissionSegmentState& state ) :
			state_( state ),
			pPrevious_( ActiveVolumeEmissionSegmentState() )
		{
			ActiveVolumeEmissionSegmentState() = &state_;
		}

		~VolumeEmissionSegmentStateScope()
		{
			ActiveVolumeEmissionSegmentState() = pPrevious_;
		}

		VolumeEmissionSegmentStateScope(
			const VolumeEmissionSegmentStateScope& ) = delete;
		VolumeEmissionSegmentStateScope& operator=(
			const VolumeEmissionSegmentStateScope& ) = delete;

	private:
		const VolumeEmissionSegmentState state_;
		const VolumeEmissionSegmentState* const pPrevious_;
	};

	inline VolumeEmissionSegmentState CurrentVolumeEmissionSegmentState()
	{
		const VolumeEmissionSegmentState* const pState =
			ActiveVolumeEmissionSegmentState();
		return pState ? *pState : VolumeEmissionSegmentState();
	}

	namespace MISWeights
	{
		/// Power-2 family weight with stable ratio scaling.  This is the
		/// NEE-side weight for thermal-volume emission when both pV and pMarch
		/// describe the same endpoint in scene-volume measure.
		inline Scalar VolumeEmissionNEEFamilyWeight(
			const Scalar pV,
			const Scalar pMarch
			)
		{
			if( !RISE::IsFiniteDouble(pV) || pV <= 0.0 ) return 0.0;
			if( !RISE::IsFiniteDouble(pMarch) || pMarch <= 0.0 ) return 1.0;
			if( pV >= pMarch ) {
				const Scalar ratio = pMarch/pV;
				return 1.0/(1.0+ratio*ratio);
			}
			const Scalar ratio = pV/pMarch;
			return ratio*ratio/(1.0+ratio*ratio);
		}

		/// March-side thermal-emission weight.  The two explicit state bits are
		/// the complete selection rule: camera/unsupported paths and sampled
		/// delta lobes remain weight 1; only an attempted competing NEE strategy
		/// against a non-singular continuation takes the complementary power-2
		/// family weight.
		inline Scalar VolumeEmissionMarchFamilyWeight(
			const Scalar pMarch,
			const Scalar pV,
			const bool competitionAvailable,
			const bool continuationSingular
			)
		{
			if( !competitionAvailable || continuationSingular ) return 1.0;
			return VolumeEmissionNEEFamilyWeight(pMarch,pV);
		}

		/// Convert a direction×distance march proposal to scene-volume measure.
		/// Boundary survival is proposal probability, distinct from physical
		/// transmittance.  Invalid or empty support fails closed to zero.
		inline Scalar VolumeEmissionMarchDensity(
			const Scalar directionPdf,
			const Scalar distancePdf,
			const Scalar distance,
			const Scalar boundarySurvival
			)
		{
			if( !RISE::IsFiniteDouble(directionPdf) ||
				!RISE::IsFiniteDouble(distancePdf) ||
				!RISE::IsFiniteDouble(distance) ||
				!RISE::IsFiniteDouble(boundarySurvival) ||
				directionPdf <= 0.0 || distancePdf <= 0.0 || distance <= 0.0 ||
				boundarySurvival <= 0.0 ) return 0.0;
			// Compose in exponent space so scene-unit changes cannot overflow or
			// underflow an intermediate product when the final density is still
			// representable.
			const Scalar logDensity = log(directionPdf) + log(distancePdf) +
				log(boundarySurvival) - 2.0*log(distance);
			const Scalar result = exp(logDensity);
			return RISE::IsFiniteDouble(result) && result > 0.0 ? result : 0.0;
		}

		/// Log-survival sibling used by a marched chain of exact null
		/// boundaries. Invalid/empty support fails closed to zero.
		inline Scalar VolumeEmissionMarchDensityFromLogSurvival(
			const Scalar directionPdf,
			const Scalar distancePdf,
			const Scalar distance,
			const Scalar logBoundarySurvival
			)
		{
			if( !RISE::IsFiniteDouble(directionPdf) ||
				!RISE::IsFiniteDouble(distancePdf) ||
				!RISE::IsFiniteDouble(distance) ||
				!RISE::IsFiniteDouble(logBoundarySurvival) ||
				directionPdf <= 0.0 || distancePdf <= 0.0 || distance <= 0.0 ||
				logBoundarySurvival > 0.0 ) return 0.0;
			const Scalar logDensity = log(directionPdf) + log(distancePdf) +
				logBoundarySurvival - 2.0*log(distance);
			const Scalar result = exp(logDensity);
			return RISE::IsFiniteDouble(result) && result > 0.0 ? result : 0.0;
		}

		/// Evaluate the arriving march density at a collision on the current
		/// segment.  The radius is measured from the originating vertex, not
		/// from the most recent null boundary.
		inline Scalar VolumeEmissionMarchDensityAtCollision(
			const VolumeEmissionSegmentState& state,
			const Scalar distancePdf,
			const Scalar segmentDistance
			)
		{
			if( !RISE::IsFiniteDouble(state.distanceOffset) ||
				!RISE::IsFiniteDouble(segmentDistance) ||
				state.distanceOffset < 0.0 || segmentDistance <= 0.0 ) return 0.0;
			return VolumeEmissionMarchDensityFromLogSurvival(
				state.directionPdf, distancePdf,
				state.distanceOffset+segmentDistance,
				state.logBoundarySurvival );
		}

		/// Balance heuristic weight: w = pa / (pa + pb)
		///
		/// \param pa  PDF of the technique being weighted
		/// \param pb  PDF of the competing technique
		/// \return Weight in [0, 1]; returns 0 when both PDFs are zero
		inline Scalar BalanceHeuristic(
			Scalar pa,
			Scalar pb
			)
		{
			const Scalar denom = pa + pb;
			if( denom <= 0 ) {
				return 0;
			}
			return pa / denom;
		}

		/// Optimal MIS weight for the 2-technique case.
		///
		/// This is the alpha-weighted balance heuristic from Kondapaneni
		/// et al. 2019.  Alpha controls the relative weight assigned to
		/// technique A vs technique B:
		///   alpha = 1   -> fully trust technique A (w = 1 when pa > 0)
		///   alpha = 0   -> fully trust technique B (w = 0)
		///   alpha = 0.5 -> standard balance heuristic
		///
		/// The alpha is typically computed from second-moment statistics:
		///   alpha = M_b / (M_a + M_b)
		/// where M_i = E[f^2 / p_i] averaged over the scene.
		///
		/// \param pa     PDF of the technique being weighted
		/// \param pb     PDF of the competing technique
		/// \param alpha  Mixture coefficient in [0, 1] for technique A
		/// \return Weight in [0, 1]
		inline Scalar OptimalMIS2Weight(
			Scalar pa,
			Scalar pb,
			Scalar alpha
			)
		{
			const Scalar wa = alpha * pa;
			const Scalar wb = (1.0 - alpha) * pb;
			const Scalar denom = wa + wb;
			if( denom <= 0 ) {
				return 0;
			}
			return wa / denom;
		}
	}
}

#endif
