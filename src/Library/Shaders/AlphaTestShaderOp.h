//////////////////////////////////////////////////////////////////////
//
//  AlphaTestShaderOp.h - Stochastic alpha-mask hit rejection.
//
//  Implements glTF 2.0 alphaMode = MASK (cutout transparency, used
//  almost exclusively for foliage / grates / chain-link textures).
//  At hit time, samples alpha from the supplied painter; if
//  alpha < cutoff, the ray continues past the surface as if it never
//  hit -- the next surface behind shades into this pixel instead.
//  If alpha >= cutoff, this op is a no-op and the next op in the
//  shader pipeline handles the surface normally.
//
//  --- Important integrator-compatibility caveat ---
//
//  This op runs inside IShader::Shade(), which is invoked by the
//  RayCaster path used by the path tracer (PT) and the legacy direct
//  shaders.  Integrators that bypass the shader-op pipeline -- BDPT,
//  VCM, MLT, photon tracers -- will NOT honour the alpha mask and
//  will treat every alpha-masked surface as fully opaque.  The
//  importer does NOT currently introspect the active rasterizer to
//  warn at scene-load time (rasterizer / job / shader op manager
//  ordering doesn't make this trivial); users running a glTF MASK
//  asset through one of these integrators get silent fully-opaque
//  surfaces.  Document this caveat next to the rasterizer chunks and
//  re-render with PT if the alpha cutout matters.
//
//  If a future need arises to support alpha mask under BDPT/VCM/MLT,
//  the right architectural move is to promote it to a hit-time
//  geometry concern (a pre-commit hook on IRayIntersectionModifier
//  or directly in the intersector), not a shader op.  Tracked in
//  docs/GLTF_IMPORT.md §13 as a Phase 4 candidate.
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
