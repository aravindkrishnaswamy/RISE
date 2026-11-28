//////////////////////////////////////////////////////////////////////
//
//  SMSConfig.h - Parser-facing configuration for Specular Manifold
//    Sampling (Zeltner et al. 2020).
//
//  Narrow scene-language view translated to ManifoldSolverConfig in
//  Job.cpp for PT, BDPT, VCM and their spectral variants.  Off by
//  default (`enabled = false`); when enabled via `sms_enabled true`,
//  defaults to biased single-seed Snell mode (20 Newton iters,
//  threshold 1e-5, maxChainDepth 30).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SMS_CONFIG_
#define SMS_CONFIG_

#include "../Interfaces/IReference.h"

namespace RISE
{
	/// Parser-facing mirror of Implementation::ManifoldSolverConfig::SeedingMode.
	enum class SMSSeedingMode {
		Snell   = 0,   ///< Legacy: Snell-traced seed shading-point->light.
		Uniform = 1    ///< Mitsuba-faithful: uniform-area on each caustic caster.
	};

	struct SMSConfig
	{
		bool			enabled;			///< Master switch (default false; opt-in via `sms_enabled true`)
		unsigned int	maxIterations;		///< Newton iteration cap per solve (default 20)
		Scalar			threshold;			///< Newton ||C|| convergence threshold (default 1e-5)
		unsigned int	maxChainDepth;		///< Hard cap on specular vertices in a chain (default 30)
		bool			biased;				///< Skip Bernoulli unbiased PDF estimator (default true; false for unbiased reference)
		unsigned int	bernoulliTrials;	///< Max Bernoulli trials when biased == false (default 100)
		unsigned int	multiTrials;		///< Independent Newton solves per eval (default 1; >1 explores multiple basins on bumpy surfaces)
		unsigned int	photonCount;		///< Photon-aided seed count (default 0 = off; >0 builds SMSPhotonMap)
		unsigned int	maxPhotonSeedsPerShadingPoint;		///< Per-shading-point photon seed cap (0 = unlimited; default 16)
		bool			twoStage;			///< Two-stage Newton solver (Zeltner 2020 §5; default false, opt-in via `sms_two_stage`). See docs/SMS_TWO_STAGE_SOLVER.md.
		bool			useLevenbergMarquardt;	///< LM diagonal damping in NewtonSolve (default false; opt-in for heavy-displacement meshes). See docs/SMS_LEVENBERG_MARQUARDT.md.
		SMSSeedingMode	seedingMode;		///< Seeding strategy (default Snell; Uniform required for geometric Bernoulli 1/p). See docs/SMS.md.
		unsigned int	targetBounces;		///< Required specular chain length (default 0 = unrestricted; recommended for Uniform seeding, e.g. 2 for a glass shell)
		bool			extended;			///< Extended SMS (`sms_extended`, default false; ignores biased/seedingMode; inert on composite scenes). See docs/SMS_EXTENDED_DESIGN.md.

		SMSConfig() :
		  enabled( false ),
		  maxIterations( 20 ),
		  threshold( 1e-5 ),
		  maxChainDepth( 30 ),
		  biased( true ),
		  bernoulliTrials( 100 ),
		  multiTrials( 1 ),
		  photonCount( 0 ),
		  maxPhotonSeedsPerShadingPoint( 16 ),
		  twoStage( false ),
		  useLevenbergMarquardt( false ),
		  seedingMode( SMSSeedingMode::Snell ),
		  targetBounces( 0 ),
		  extended( false )
		{
		}
	};
}

#endif
