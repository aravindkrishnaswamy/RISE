//////////////////////////////////////////////////////////////////////
//
//  SMSShaderOp.h - Specular Manifold Sampling shader operation.
//
//    At each non-specular surface hit, samples a light and attempts
//    to find a valid specular path connecting the shading point to
//    the light through any intervening glass/mirror surfaces using
//    Newton iteration on the specular constraint manifold.
//
//    This is the unidirectional path tracer counterpart of the SMS
//    strategy in BDPTIntegrator — it enables caustic rendering in
//    the standard rendering pipeline.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SMS_SHADER_OP_
#define SMS_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ManifoldSolver.h"

namespace RISE
{
	namespace Implementation
	{
		class SMSShaderOp :
			public virtual IShaderOp,
			public virtual Reference
		{
		protected:
			ManifoldSolver*		pSolver;

			virtual ~SMSShaderOp();

		public:
			SMSShaderOp( const ManifoldSolverConfig& config );

			//! Tells the shader to apply shade to the given intersection point
			void PerformOperation(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack,
				const ScatteredRayContainer* pScat
				) const;

			//! Tells the shader to apply shade to the given intersection point for the given wavelength
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

			//! Asks if the shader op needs SPF data
			bool RequireSPF() const { return false; }
		};
	}
}

#endif
