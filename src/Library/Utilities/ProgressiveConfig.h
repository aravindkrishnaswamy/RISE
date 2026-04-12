//////////////////////////////////////////////////////////////////////
//
//  ProgressiveConfig.h - Configuration for multi-pass progressive
//    rendering with adaptive convergence.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PROGRESSIVE_CONFIG_
#define PROGRESSIVE_CONFIG_

namespace RISE
{
	struct ProgressiveConfig
	{
		bool			enabled;			///< Enable progressive multi-pass rendering
		unsigned int	samplesPerPass;		///< SPP per progressive pass (default: 8)

		ProgressiveConfig() :
		  enabled( true ),
		  samplesPerPass( 32 )
		{}
	};
}

#endif
