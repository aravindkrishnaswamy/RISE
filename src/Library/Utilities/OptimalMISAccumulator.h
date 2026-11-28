//////////////////////////////////////////////////////////////////////
//
//  OptimalMISAccumulator.h - Tiled second-moment accumulator for
//    optimal MIS direct-illumination weights (Kondapaneni et al.,
//    SIGGRAPH 2019).
//
//  Per tile (default 16x16 px), each technique estimates its own second
//  moment M_i = E_{x~p_i}[(f/p_i)^2] from its own samples only.  Solve()
//  computes alpha = M_nee / (M_nee + M_bsdf) for OptimalMIS2Weight() in
//  MISWeights.h (lower M_i -> higher weight; BSDF gets alpha, NEE gets
//  1 - alpha).  Per-tile counters use std::atomic for lock-free
//  concurrent accumulation.
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

			/// Increments sample count for the technique.  Must be called once per
			/// sample attempt (hits AND misses) so M_i averages over all samples.
			void AccumulateCount(
				unsigned int px,
				unsigned int py,
				SamplingTechnique technique
				);

			/// Accumulates (f/p_i)^2 for a hit (f > 0), where f2 is squared
			/// direct-light luminance and samplingPdf is the generating technique's
			/// PDF.  Does NOT increment the sample count (call AccumulateCount).
			void Accumulate(
				unsigned int px,
				unsigned int py,
				Scalar f2,
				Scalar samplingPdf,
				SamplingTechnique technique
				);

			/// Reads raw per-tile training sums (f/p_i)^2 and attempt counts
			/// (hits + misses) prior to Solve(), for direct test verification.
			void GetTileTraining(
				unsigned int px,
				unsigned int py,
				double& sumNee,
				double& sumBsdf,
				unsigned int& countNee_,
				unsigned int& countBsdf_
				) const;

			/// Solves for per-tile optimal alpha from accumulated statistics.
			/// Must be called after all training iterations are complete.
			void Solve();

			/// Returns solved BSDF alpha in [alphaClampMin, alphaClampMax] for
			/// the pixel's tile (valid after Solve(); NEE weight is 1 - alpha).
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
