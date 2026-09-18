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
//  This op runs inside IShader::Shade(), which is reached through
//  `RayCaster::SelectShader(ri)` (`ri.pShader` if the OBJECT bound one
//  via `IJob::AddObject`'s `shader` parameter, else the rasterizer's
//  own default shader) at the sites `RayCaster::CastRay{,NM,HWSS}`
//  call it from.  DL-193 (docs/DEBT_LEDGER.md) corrected an earlier,
//  WRONG revision of this comment that claimed the modern path tracer
//  honours this mechanism: `PathTracingIntegrator.cpp` -- what
//  `pathtracing_pel_rasterizer` / `RISE_API_CreatePathTracingPelRasterizer`
//  actually run -- has NO reference to `SelectShader` or `ri.pShader`
//  anywhere in it; it evaluates emission/BSDF/NEE directly against
//  `ri.pMaterial`, entirely bypassing the shader-op pipeline this file
//  lives in.  Measured directly (DL-193's `BlenderBridgeAlphaTest.cpp`):
//  an alpha=0 CLIP material renders fully opaque under
//  `pathtracing_pel_rasterizer` and correctly cut-through under
//  `pixelpel_rasterizer` (the LEGACY direct-lighting-only rasterizer,
//  which DOES dispatch through `RayCaster::CastRay`/`SelectShader`) on
//  the IDENTICAL scene.  **`pixelpel_rasterizer` is therefore the ONLY
//  rasterizer this op's alpha mask reaches** -- BDPT, VCM, MLT, photon
//  tracers, AND the modern PT integrator all bypass the shader-op
//  pipeline and will treat every alpha-masked surface as fully opaque.
//  The importer does NOT currently introspect the active rasterizer to
//  warn at scene-load time (rasterizer / job / shader op manager
//  ordering doesn't make this trivial); users running a glTF MASK
//  asset through any integrator but `pixelpel_rasterizer` get silent
//  fully-opaque surfaces.  Document this caveat next to the rasterizer
//  chunks.  Filed as DL-214 (docs/DEBT_LEDGER.md): a general
//  architecture gap (this file's own mechanism reaching only one
//  rasterizer), not specific to the Blender bridge that found it.
//
//  If a future need arises to support alpha mask under integrators
//  that bypass shader-ops, the right architectural move is to promote
//  it to a hit-time geometry concern (a pre-commit hook on
//  IRayIntersectionModifier or directly in the intersector), not a
//  shader op.  Tracked in docs/GLTF_IMPORT.md §13 as a Phase 4
//  candidate.
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
