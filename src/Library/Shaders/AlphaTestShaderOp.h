//////////////////////////////////////////////////////////////////////
//
//  AlphaTestShaderOp.h - Legacy explicit alpha-mask shader operation.
//
//  Implements shader-chain cutout transparency (used
//  almost exclusively for foliage / grates / chain-link textures).
//  At hit time, samples alpha from the supplied painter; if
//  alpha < cutoff, the ray continues past the surface as if it never
//  hit -- the next surface behind shades into this pixel instead.
//  If alpha >= cutoff, this op is a no-op and the next op in the
//  shader pipeline handles the surface normally.
//
//  Legacy explicit shader-chain compatibility only. Imported glTF/Blender
//  coverage now lives on IMaterial and is tested by the sampled transport
//  traversal before shading (DL-214). Do not install this op on the same
//  imported material: that would apply coverage twice. Explicit authored
//  shader-op chains retain their original legacy dispatch semantics.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ALPHA_TEST_SHADER_OP_
#define ALPHA_TEST_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AlphaTestShaderOp :
			public virtual IShaderOp,
			public virtual Reference
		{
		protected:
			virtual ~AlphaTestShaderOp();

			const IPainter& alphaPainter;
			const Scalar    cutoff;

		public:
			AlphaTestShaderOp(
				const IPainter& alpha_painter,
				const Scalar    cutoff_
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

			bool RequireSPF() const { return false; }
		};
	}
}

#endif
