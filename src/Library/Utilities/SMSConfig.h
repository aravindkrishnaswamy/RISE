//////////////////////////////////////////////////////////////////////
//
//  SMSConfig.h - Parser-facing configuration for Specular Manifold
//    Sampling (SMS).
//
//    SMS (Zeltner et al. 2020, "Specular Manifold Sampling for Rendering
//    High-Frequency Caustics and Glints") seeds a Newton solver on a
//    sequence of specular vertices to connect a shading point to a
//    light through a specular chain that ordinary path/NEE sampling
//    almost never hits.  It is a targeted integrator add-on, not a
//    free upgrade: on scenes without the hard caustics or glints it
//    is designed to resolve, SMS only adds cost and a second source
//    of variance.
//
//    This struct bundles the SMS knobs the scene parser exposes for
//    PT, BDPT, VCM and their spectral variants.  It is a narrow view
//    of the richer `ManifoldSolverConfig` (see ManifoldSolver.h);
//    Job.cpp translates between them at the boundary.
//
//    Default-constructed state is "SMS off".  Users opt in per-scene
//    with `sms_enabled true`.  When enabled, the remaining defaults
//    are a safe production preset:
//      - `biased = true` — skip the Bernoulli unbiased PDF estimator.
//        Biased mode is dramatically faster and the bias is usually
//        invisible on production caustics.  `biased = false` is for
//        convergence studies and ground-truth references.
//      - `maxIterations = 20` — Newton iteration cap per solve.
//        Converges well under 20 on well-conditioned chains; anything
//        higher is usually a symptom of a seed on a bad basin, not a
//        threshold worth raising.
//      - `threshold = 1e-5` — Newton ||C|| convergence threshold.
//        Tight enough that the reconstructed chain is visually
//        indistinguishable from the true manifold; loose enough that
//        typical smooth dielectrics converge in ~5 iterations.
//      - `maxChainDepth = 30` — hard stop on specular vertex count.
//        SMS work is exponential in chain length, so this is both a
//        safety rail and a performance cap.
//      - `bernoulliTrials = 100` — only used when `biased == false`.
//        Unused otherwise; keep the default to make opt-in to
//        unbiased mode predictable.
//      - `multiTrials = 1` — independent Newton solves per eval.
//        Value 1 reproduces the single-seed Snell-traced solve.
//        Increase to uncover caustic paths on bumpy / displaced
//        surfaces where seeds land in different basins of attraction.
//      - `photonCount = 0` — photon-aided seeding off.  Setting > 0
//        builds an SMSPhotonMap at scene-prep time whose photon
//        landing points feed additional Newton seeds; enables SMS on
//        caustics the deterministic Snell seed misses entirely, at
//        the cost of a photon pass and per-query kd-tree lookups.
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
	struct SMSConfig
	{
		bool			enabled;			///< Master switch; default false (SMS off unless the scene opts in)
		unsigned int	maxIterations;		///< Newton iteration limit per solve (default 20)
		Scalar			threshold;			///< Newton ||C|| convergence threshold (default 1e-5)
		unsigned int	maxChainDepth;		///< Maximum specular vertices in a chain (default 30)
		bool			biased;				///< Skip Bernoulli unbiased PDF estimation (default true — biased mode, dramatically faster and usually visually identical on production caustics)
		unsigned int	bernoulliTrials;	///< Max trials for unbiased PDF estimation (default 100, ignored when biased == true)
		unsigned int	multiTrials;		///< Independent Newton solves per evaluation (Zeltner 2020); default 1 = single-solve Snell seed.  >1 uncovers separate basins on bumpy surfaces at proportional cost.
		unsigned int	photonCount;		///< Photon-aided seed budget; default 0 = off.  >0 builds an SMSPhotonMap for seeds on caustics the deterministic seed misses.
		bool			twoStage;			///< Two-stage Newton solver (Zeltner 2020 §5).  When enabled, Newton first runs on a smoothed reference surface (smoothing=1: underlying analytical base, no displacement) to escape the C1-discontinuity plateau on Phong-shaded triangle meshes, then refines on the actual surface (smoothing=0).  Default false; opt-in via `sms_two_stage TRUE`.  No-op when the specular geometry doesn't expose a smoothing-aware analytical query.  See `docs/SMS_TWO_STAGE_SOLVER.md`.

		SMSConfig() :
		  enabled( false ),
		  maxIterations( 20 ),
		  threshold( 1e-5 ),
		  maxChainDepth( 30 ),
		  biased( true ),
		  bernoulliTrials( 100 ),
		  multiTrials( 1 ),
		  photonCount( 0 ),
		  twoStage( false )
		{
		}
	};
}

#endif
