//////////////////////////////////////////////////////////////////////
//
//  OptimalMISAccumulator.h - Tiled second-moment accumulator for
//  optimal MIS weights in path tracing direct illumination.
//
//  ALGORITHM:
//  During training iterations, for each NEE or BSDF-sampled emitter
//  hit, the accumulator records (f/p_i)^2 — the squared single-
//  technique estimator value — into the tile containing the current
//  pixel, where i is the technique that generated the sample
//  direction.  After training, Solve() computes per-tile optimal
//  alpha:
//
//    alpha = M_nee / (M_nee + M_bsdf)
//
//  where M_i = E_{x~p_i}[(f/p_i)^2] = mean of f^2/p_i^2 over
//  samples drawn by technique i.  This equals the second moment of
//  technique i's single-technique estimator.  The OTHER technique's
//  moment goes in the numerator because the optimal coefficient is
//  1/M_i, so lower M means more weight.  This alpha is then used by
//  OptimalMIS2Weight() from MISWeights.h:
//
//    w_bsdf(x) = alpha * p_bsdf / (alpha * p_bsdf + (1-alpha) * p_nee)
//    w_nee(x)  = (1-alpha) * p_nee / ((1-alpha) * p_nee + alpha * p_bsdf)
//
//  CORRECT MOMENT ESTIMATION:
//  Each technique must estimate its OWN second moment from its OWN
//  samples.  If technique i draws direction x ~ p_i, then:
//
//    M_i = E_{x~p_i}[(f(x)/p_i(x))^2]
//        = integral f^2/p_i dx
//
//  and (f(x)/p_i(x))^2 = f^2/p_i^2 is an unbiased single-sample
//  estimator.  We do NOT need the other technique's PDF during
//  accumulation.  Mixing samples from different proposals without
//  IS correction would produce biased moment estimates.
//
//  SPATIAL BINNING:
//  Using per-pixel statistics would require too many samples for
//  convergence.  Instead, statistics are accumulated per tile
//  (default 16x16 pixels), providing spatial adaptivity while
//  maintaining adequate sample counts.
//
//  THREAD SAFETY:
//  Multiple threads may accumulate concurrently.  Per-tile counters
//  use std::atomic for lock-free thread safety.
//
//  REFERENCE:
//  Kondapaneni, I., Vevoda, P., Grittmann, P., Skalicky, T.,
//  Vorba, J., Krivanek, J. "Optimal Multiple Importance Sampling."
//  ACM Trans. Graph. (Proc. SIGGRAPH) 38(4), 2019.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OPTIMAL_MIS_ACCUMULATOR_
#define OPTIMAL_MIS_ACCUMULATOR_

#include "../Interfaces/IReference.h"
#include "Reference.h"
#include "Math3D/Math3D.h"
#include <vector>
#include <atomic>

namespace RISE
{
	namespace Implementation
	{
		/// Identifies which sampling technique generated a sample direction.
		enum SamplingTechnique
		{
			kTechniqueNEE = 0,		///< Direction was drawn by next event estimation (light sampling)
			kTechniqueBSDF = 1		///< Direction was drawn by BSDF sampling
		};

		class OptimalMISAccumulator :
			public virtual IReference,
			public virtual Reference
		{
		public:
			/// Configuration for the accumulator
			struct Config
			{
				unsigned int tileSize;				///< Tile size in pixels (default 16)
				unsigned int minSamplesPerTile;		///< Minimum samples before trusting alpha (default 32)
				Scalar alphaClampMin;				///< Minimum alpha value (default 0.05)
				Scalar alphaClampMax;				///< Maximum alpha value (default 0.95)
			};

		protected:
			/// Per-tile statistics.  Uses doubles for accumulation
			/// precision even when Scalar is float.
			///
			/// Each technique accumulates its own second moment:
			///   (f/p_i)^2 = f^2/p_i^2
			/// from its own samples only, with a separate count.
			struct TileStats
			{
				std::atomic<double>		sumMomentNee;		///< Sum of (f/p_nee)^2 from NEE samples
				std::atomic<double>		sumMomentBsdf;		///< Sum of (f/p_bsdf)^2 from BSDF samples
				std::atomic<unsigned int> countNee;			///< Number of NEE samples accumulated
				std::atomic<unsigned int> countBsdf;		///< Number of BSDF samples accumulated

				TileStats() :
					sumMomentNee( 0.0 ),
					sumMomentBsdf( 0.0 ),
					countNee( 0 ),
					countBsdf( 0 )
				{
				}

				// Atomic types are not copyable, so provide explicit copy
				TileStats( const TileStats& other ) :
					sumMomentNee( other.sumMomentNee.load() ),
					sumMomentBsdf( other.sumMomentBsdf.load() ),
					countNee( other.countNee.load() ),
					countBsdf( other.countBsdf.load() )
				{
				}
			};

			unsigned int			imageWidth;
			unsigned int			imageHeight;
			unsigned int			tileSize;
			unsigned int			tilesX;
			unsigned int			tilesY;
			unsigned int			minSamplesPerTile;
			Scalar					alphaClampMin;
			Scalar					alphaClampMax;

			std::vector<TileStats>	tiles;
			std::vector<Scalar>		solvedAlpha;		///< Per-tile solved alpha (valid after Solve())
			bool					bSolved;

			/// Maps pixel coordinates to tile index
			unsigned int TileIndex(
				unsigned int px,
				unsigned int py
				) const
			{
				const unsigned int tx = px / tileSize;
				const unsigned int ty = py / tileSize;
				return ty * tilesX + tx;
			}

		public:
			OptimalMISAccumulator();
			virtual ~OptimalMISAccumulator();

			/// Initializes the accumulator for a given image size.
			void Initialize(
				unsigned int width,
				unsigned int height,
				const Config& config
				);

			/// Resets all accumulated statistics.
			void Reset();

			/// Increments the sample count for the specified technique.
			///
			/// Must be called once per sample attempt (every NEE shadow
			/// ray, every BSDF scatter), regardless of whether the sample
			/// contributed non-zero radiance.  This ensures the second
			/// moment M_i = E_{x~p_i}[(f/p_i)^2] is averaged over ALL
			/// samples, not just hits.
			///
			/// \param px           Pixel x coordinate
			/// \param py           Pixel y coordinate
			/// \param technique    Which technique generated this sample
			void AccumulateCount(
				unsigned int px,
				unsigned int py,
				SamplingTechnique technique
				);

			/// Accumulates the second-moment contribution for a successful
			/// sample (f > 0).
			///
			/// Called during training for every emitter hit where the
			/// sampling PDF is available.  Does NOT increment the sample
			/// count — call AccumulateCount() separately for every sample
			/// attempt (including misses).
			///
			/// \param px           Pixel x coordinate
			/// \param py           Pixel y coordinate
			/// \param f2           Squared integrand magnitude (luminance^2
			///                     of the full direct-light contribution)
			/// \param samplingPdf  PDF of the technique that drew this direction
			/// \param technique    Which technique generated this sample
			void Accumulate(
				unsigned int px,
				unsigned int py,
				Scalar f2,
				Scalar samplingPdf,
				SamplingTechnique technique
				);

			/// Solves for per-tile optimal alpha from accumulated statistics.
			/// Must be called after all training iterations are complete.
			void Solve();

			/// Returns the solved alpha for the tile containing the given pixel.
			/// Valid only after Solve() has been called.
			///
			/// Alpha is the weight for the BSDF technique in the optimal
			/// MIS formula.  Use (1 - alpha) for the NEE technique weight.
			///
			/// \param px  Pixel x coordinate
			/// \param py  Pixel y coordinate
			/// \return Optimal alpha in [alphaClampMin, alphaClampMax]
			Scalar GetAlpha(
				unsigned int px,
				unsigned int py
				) const;

			/// Returns true after Solve() has been called.
			bool IsReady() const { return bSolved; }

			/// Returns the image dimensions this accumulator was initialized for.
			unsigned int GetWidth() const { return imageWidth; }
			unsigned int GetHeight() const { return imageHeight; }
		};
	}
}

#endif
