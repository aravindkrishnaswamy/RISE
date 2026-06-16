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
//    Most controls are disabled by default so that existing scenes
//    produce identical output unless a control is explicitly set.
//    Exceptions: `maxVolumeBounce` defaults to 64 (not unlimited) and
//    `useLightBVH` defaults to true.
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
		unsigned int	maxVolumeBounce;		///< Maximum volume scatter bounces (64 default, matches PT)

		//
		// Light sampling
		//

		bool			useLightBVH;			///< Use light BVH for importance-weighted many-light selection (true = enabled by default)

		//
		// Optimal MIS (Kondapaneni et al. 2019)
		//
		// When enabled, the path tracer runs training iterations to
		// estimate second-moment statistics for the NEE and BSDF
		// sampling techniques, then uses variance-minimizing weights
		// instead of the power heuristic for direct illumination.
		//

		bool			optimalMIS;				///< Enable optimal MIS weight training (false = disabled)
		unsigned int	optimalMISTrainingIterations;	///< Number of training passes (default 4)
		unsigned int	optimalMISTileSize;		///< Tile size for spatial binning (default 16)

		//
		// Transparent (Fresnel-attenuated) shadow rays
		//
		// When enabled, NEE shadow rays pass STRAIGHT through perfect-
		// specular transmissive dielectric interfaces (glass / sapphire /
		// water), multiplying the per-interface Fresnel TRANSMITTANCE
		// (1 - F) into the light contribution instead of being fully
		// occluded.  Any non-dielectric (opaque, rough, diffuse, mirror)
		// hit still fully blocks.  This is an APPROXIMATION: it ignores
		// refractive bending and internal multi-bounce, but recovers
		// direct lighting on surfaces under a thin transparent shell at a
		// fraction of the brute-force refracted-BSDF noise.  Honoured by
		// the unidirectional path tracers ONLY (PT pel + PT spectral);
		// BDPT / VCM / MLT shadow tests stay binary.
		// Field appended at the END of the struct to preserve the
		// pass-by-const-ref layout for existing callers.
		//
		bool			transparentShadows;	///< Fresnel-transmittance shadow rays through specular dielectrics (false = binary occlusion)

		StabilityConfig() :
		  directClamp( 0 ),
		  indirectClamp( 0 ),
		  filterGlossy( 0 ),
		  rrMinDepth( 3 ),
		  rrThreshold( 0.05 ),
		  maxDiffuseBounce( UINT_MAX ),
		  maxGlossyBounce( UINT_MAX ),
		  maxTransmissionBounce( UINT_MAX ),
		  maxTranslucentBounce( UINT_MAX ),
		  maxVolumeBounce( 64 ),
		  useLightBVH( true ),
		  optimalMIS( false ),
		  optimalMISTrainingIterations( 4 ),
		  optimalMISTileSize( 16 ),
		  transparentShadows( false )
		{
		}
	};
}

#endif
