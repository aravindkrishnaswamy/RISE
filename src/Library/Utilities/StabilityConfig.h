//////////////////////////////////////////////////////////////////////
//
//  StabilityConfig.h - Configuration for production stability
//    controls.
//
//    Production path tracers expose knobs for taming difficult or
//    noisy scenes without requiring source edits.  This struct
//    carries all such controls through the rendering pipeline,
//    shared by both the unidirectional PT and BDPT integrators.
//
//    All controls are disabled by default so that existing scenes
//    produce identical output unless a control is explicitly set.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STABILITY_CONFIG_
#define STABILITY_CONFIG_

#include "../Interfaces/IReference.h"
#include <climits>

namespace RISE
{
	struct StabilityConfig
	{
		//
		// Sample clamping
		//

		Scalar			directClamp;			///< Max luminance for direct lighting contributions (0 = disabled)
		Scalar			indirectClamp;			///< Max luminance for indirect contributions (0 = disabled)

		//
		// Glossy filtering
		//

		Scalar			filterGlossy;			///< Per-bounce roughness increase for glossy BSDFs (0 = disabled)

		//
		// Russian roulette tuning
		//

		unsigned int	rrMinDepth;				///< Minimum path depth before RR activates
		Scalar			rrThreshold;			///< Throughput floor to prevent runaway RR compensation

		//
		// Per-type bounce limits
		//

		unsigned int	maxDiffuseBounce;		///< Maximum diffuse bounces (UINT_MAX = unlimited)
		unsigned int	maxGlossyBounce;		///< Maximum glossy/reflection bounces (UINT_MAX = unlimited)
		unsigned int	maxTransmissionBounce;	///< Maximum refraction/transmission bounces (UINT_MAX = unlimited)
		unsigned int	maxTranslucentBounce;	///< Maximum translucent bounces (UINT_MAX = unlimited)

		StabilityConfig() :
		  directClamp( 0 ),
		  indirectClamp( 0 ),
		  filterGlossy( 0 ),
		  rrMinDepth( 3 ),
		  rrThreshold( 0.05 ),
		  maxDiffuseBounce( UINT_MAX ),
		  maxGlossyBounce( UINT_MAX ),
		  maxTransmissionBounce( UINT_MAX ),
		  maxTranslucentBounce( UINT_MAX )
		{
		}
	};
}

#endif
