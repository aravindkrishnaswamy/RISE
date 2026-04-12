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

namespace RISE
{
	namespace MISWeights
	{
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
