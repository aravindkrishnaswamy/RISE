//////////////////////////////////////////////////////////////////////
//
//  MISPathTracingShaderOp.h - Unified MIS path tracing shader
//    operation with integrated SMS (Specular Manifold Sampling).
//
//    Combines next event estimation (NEE), BSDF sampling, and SMS
//    into a single shader op with proper MIS weighting to prevent
//    double-counting.
//
//    At each diffuse surface hit:
//    1. NEE: sample a light, shadow test
//       - Unblocked: contribute with MIS weight w_nee
//       - Blocked: attempt SMS through specular chain
//    2. BSDF sampling: trace scattered ray, recurse
//       - If hits emitter (non-specular path): contribute with w_bsdf
//       - Specular bounces: continue tracing but don't count emission
//         (SMS handles caustic transport)
//
//    Replaces: DefaultDirectLighting + DefaultEmission + SMS + PT
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MIS_PATHTRACING_SHADER_OP_
#define MIS_PATHTRACING_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ManifoldSolver.h"

namespace RISE
{
	namespace Implementation
	{
		class MISPathTracingShaderOp :
			public virtual IShaderOp,
			public virtual Reference
		{
		protected:
			virtual ~MISPathTracingShaderOp();

			ManifoldSolver*		pSolver;		///< SMS solver (NULL if SMS disabled)
			const bool			bBranch;		///< Branch vs Russian Roulette
			const bool			bSMSEnabled;	///< Whether to attempt SMS for blocked NEE

		public:
			MISPathTracingShaderOp(
				const bool branch,
				const ManifoldSolverConfig& smsConfig
				);

			void PerformOperation(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack* const ior_stack,
				const ScatteredRayContainer* pScat
				) const;

			Scalar PerformOperationNM(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				const Scalar caccum,
				const Scalar nm,
				const IORStack* const ior_stack,
				const ScatteredRayContainer* pScat
				) const;

			bool RequireSPF() const { return true; }
		};
	}
}

#endif
