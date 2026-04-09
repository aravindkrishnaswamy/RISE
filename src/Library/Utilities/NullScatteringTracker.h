//////////////////////////////////////////////////////////////////////
//
//  NullScatteringTracker.h - Null-scattering path integral utilities
//    for delta tracking with explicit PDF computation
//
//  In the null-scattering formulation (Miller, Georgiev, Jarosz,
//  SIGGRAPH 2019), delta tracking is reinterpreted as sampling from
//  a path space augmented with fictitious null-scattering events.
//  The PDF of a sampled distance t along a ray through a medium
//  with majorant sigma_bar is:
//
//    For scatter at t:   p(t) = sigma_t(t) * T_bar(0,t)
//    For no scatter:     p(>tEnd) = T_bar(0,tEnd)
//
//  where T_bar is the majorant transmittance:
//    T_bar(0,t) = exp(-integral_0^t sigma_bar(s) ds)
//
//  With a majorant grid, sigma_bar is piecewise constant per cell,
//  so the integral is a sum over DDA segments.
//
//  These PDFs enable MIS between delta tracking and equiangular
//  sampling (Phase 4).
//
//  References:
//    - Miller, Georgiev, Jarosz, "A Null-Scattering Path Integral
//      Formulation of Light Transport", SIGGRAPH 2019
//    - Novak et al., "Monte Carlo Methods for Volumetric Light
//      Transport Simulation", Computer Graphics Forum 37(2), 2018
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 8, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef NULL_SCATTERING_TRACKER_
#define NULL_SCATTERING_TRACKER_

#include "Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/ColorMath.h"
#include "MajorantGrid.h"
#include "Ray.h"

namespace RISE
{
	namespace NullScatteringTracker
	{
		/// Evaluate the delta tracking PDF at a given distance along
		/// a ray through a majorant grid.
		///
		/// For a scatter event at distance t:
		///   pdf = sigma_t(t) * T_bar(0,t)
		/// where T_bar(0,t) = exp(-integral majorant along [0,t])
		///
		/// For no scatter (transmission through entire range):
		///   pdf = T_bar(0,tEnd)
		///
		/// The majorant integral is computed via DDA traversal,
		/// summing majorant * segment_length for each cell.
		inline Scalar EvaluateDeltaTrackingPdf(
			const Ray& ray,
			const Scalar t,
			const bool scattered,
			const Scalar sigma_t_at_t,
			const Scalar tEnd,
			const MajorantGrid& grid
			)
		{
			// Accumulate the majorant optical depth from 0 to t
			// (or to tEnd if not scattered)
			const Scalar targetDist = scattered ? t : tEnd;
			Scalar majorantOpticalDepth = 0;

			struct OpticalDepthAccumulator
			{
				Scalar targetDist;
				Scalar* pOpticalDepth;

				bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
				{
					// Clamp cell exit to target distance
					const Scalar tExit = fmin( tCellExit, targetDist );
					if( tExit <= tCellEntry )
						return true;  // Skip zero-length cells (DDA ties), continue traversal

					*pOpticalDepth += cellMajorant * (tExit - tCellEntry);
					return (tExit < targetDist);
				}
			};

			OpticalDepthAccumulator acc;
			acc.targetDist = targetDist;
			acc.pOpticalDepth = &majorantOpticalDepth;

			grid.TraverseRay( ray, 0.0, targetDist + 1e-6, acc );

			const Scalar T_bar = exp( -majorantOpticalDepth );

			if( scattered )
				return sigma_t_at_t * T_bar;
			else
				return T_bar;
		}
	}
}

#endif
