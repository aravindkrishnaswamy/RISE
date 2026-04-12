//////////////////////////////////////////////////////////////////////
//
//  AdaptiveSamplingConfig.h - Configuration for adaptive sampling.
//
//    When enabled (maxSamples > 0), pixel-based rasterizers take
//    samples in batches and use Welford variance estimation to stop
//    sampling pixels whose relative error has dropped below the
//    threshold.  The existing samples-per-pixel setting serves as
//    both the minimum sample count and the batch size.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 29, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ADAPTIVE_SAMPLING_CONFIG_
#define ADAPTIVE_SAMPLING_CONFIG_

#include "../Interfaces/IReference.h"

namespace RISE
{
	struct AdaptiveSamplingConfig
	{
		unsigned int	maxSamples;		///< Maximum samples per pixel (0 = disabled)
		Scalar			threshold;		///< Relative error convergence threshold
		bool			showMap;		///< Output sample-count heatmap instead of color

		AdaptiveSamplingConfig() :
		  maxSamples( 0 ),
		  threshold( 0.01 ),
		  showMap( false )
		{
		}
	};
}

#endif
