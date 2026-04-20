//////////////////////////////////////////////////////////////////////
//
//  PathTracingShaderOp.h - Unified MIS path tracing shader
//    operation with integrated SMS (Specular Manifold Sampling).
//
//    Combines next event estimation (NEE), BSDF sampling, emission,
//    and optional SMS into a single shader op with proper MIS
//    weighting to prevent double-counting.
//
//    At each diffuse surface hit:
//    1. Emission: if surface is an emitter, add emission (MIS weighted)
//    2. NEE: sample lights, shadow test, contribute with MIS weight
//       - If blocked by glass and SMS enabled: attempt SMS
//    3. BSDF sampling: trace scattered ray, recurse
//       - If hits emitter: contribute with MIS weight w_bsdf
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATHTRACING_SHADER_OP_
#define PATHTRACING_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/StabilityConfig.h"

namespace RISE
{
	namespace Implementation
	{
		class PathTracingIntegrator;

		class PathTracingShaderOp :
			public virtual IShaderOp,
			public virtual Reference
		{
		protected:
			virtual ~PathTracingShaderOp();

			PathTracingIntegrator*	pIntegrator;	///< Shared iterative integrator
			const bool				bSMSEnabled;	///< Whether SMS is active (for emission suppression)

		public:
			PathTracingShaderOp(
				const ManifoldSolverConfig& smsConfig,
				const StabilityConfig& stabilityCfg
				);

			void PerformOperation(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack,
				const ScatteredRayContainer* pScat
				) const;

			Scalar PerformOperationNM(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				const Scalar caccum,
				const Scalar nm,
				const IORStack& ior_stack,
				const ScatteredRayContainer* pScat
				) const;

			/// HWSS override: evaluates the wavelength bundle with
			/// material-aware fallback strategy.
			///
			/// For materials with IBSDF and no SSS: hero wavelength
			/// drives directional decisions via ScatterNM(hero);
			/// companions evaluate throughput via valueNM(lambda_i).
			///
			/// Fallback cases (see material audit in plan):
			/// - SPF-only materials (GetBSDF()==NULL): per-wavelength
			/// - SSS materials: SSS component per-wavelength, surface HWSS
			/// - Dispersive specular (isDelta + varying IOR): terminate
			///   companions
			void PerformOperationHWSS(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				const Scalar caccum[SampledWavelengths::N],
				SampledWavelengths& swl,
				const IORStack& ior_stack,
				const ScatteredRayContainer* pScat,
				Scalar result[SampledWavelengths::N]
				) const;

			bool RequireSPF() const { return true; }
			bool HandlesEmission() const { return true; }
		};
	}
}

#endif
