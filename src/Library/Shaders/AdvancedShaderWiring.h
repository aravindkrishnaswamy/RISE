//////////////////////////////////////////////////////////////////////
//
//  AdvancedShaderWiring.h - Shared helper for the per-material
//    `advanced_shader` op chain an alpha-aware shader op (cutout or
//    stochastic blend) needs to replace the renderer's always-on
//    default shader for that material.
//
//  Legacy helper for explicitly authored alpha shader-op chains. glTF and
//  Blender now call IJob::SetMaterialAlpha instead; their transport alpha
//  works before shader dispatch. The Blender bridge preserves its historical
//  shader name with ordinary emission/direct ops only, without this helper.
//
//  WHY THE SHAPE IS FIXED.  `alpha_test_shaderop` / `transparency_-
//  shaderop` REPLACE the running accumulator (the `=` operation)
//  rather than adding to it -- an ADDITIVE `standard_shader` would
//  produce wrong cutout/blend semantics, since the accumulator must
//  hold either the surface's own emission+BSDF response (opaque case)
//  or the background colour reached by continuing the ray (cutout /
//  blend case), never their sum.  The two preceding ops
//  (`DefaultEmission`, `DefaultDirectLighting`) fill the accumulator
//  with the ordinary emission + direct-lighting response so the
//  alpha-aware op has something to keep or discard.
//
//  This helper remains specific to legacy shader dispatch. The shared
//  material coverage path is documented in docs/ALPHA_COVERAGE.md.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ADVANCED_SHADER_WIRING_
#define ADVANCED_SHADER_WIRING_

#include "../Interfaces/IJob.h"

namespace RISE
{
	namespace Utilities
	{
		//! Registers an `advanced_shader` named `shaderName` running
		//! `[DefaultEmission +, DefaultDirectLighting +, opName =]` --
		//! the shape every alpha-aware (cutout / blend) material needs.
		//! `opName` must already be registered (an `alpha_test_shaderop`
		//! or a `transparency_shaderop`).  Depth range is unrestricted
		//! (0..100), matching both existing callers.
		//!
		//! \return TRUE on success; on FALSE, nothing is registered and
		//!         the caller is responsible for its own diagnostic
		//!         (the two existing callers each have their own
		//!         differently-worded failure message).
		inline bool WireAlphaAdvancedShader(
			IJob& job,
			const char* shaderName,
			const char* opName
			)
		{
			const char* ops[] = { "DefaultEmission", "DefaultDirectLighting", opName };
			const unsigned int minDepth[] = { 0, 0, 0 };
			const unsigned int maxDepth[] = { 100, 100, 100 };
			const char operations[] = { '+', '+', '=' };
			return job.AddAdvancedShader( shaderName, 3, ops, minDepth, maxDepth, operations );
		}
	}
}

#endif
