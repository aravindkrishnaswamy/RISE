//////////////////////////////////////////////////////////////////////
//
//  AdvancedShaderWiring.h - Shared helper for the per-material
//    `advanced_shader` op chain an alpha-aware shader op (cutout or
//    stochastic blend) needs to replace the renderer's always-on
//    default shader for that material.
//
//  Both `GLTFSceneImporter.cpp`'s `WireAlphaShader` (glTF alphaMode
//  MASK/BLEND) and `rise_blender_bridge.cpp`'s Blender Principled BSDF
//  Alpha bridging (DL-193, docs/DEBT_LEDGER.md) build the IDENTICAL
//  three-op chain -- `[Emission +, DirectLighting +, <op> =]` -- and
//  differed only in the alpha-aware op's own name.  This is that one
//  shared construction, extracted so the two importers cannot drift
//  apart on it.
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
//  See `AlphaTestShaderOp.h`'s own "integrator-compatibility caveat":
//  this chain runs inside `IShader::Shade()`, reached by the RayCaster
//  path the path tracer and the legacy direct shaders use -- BDPT /
//  VCM / MLT / photon tracers bypass it entirely and render every
//  alpha-masked/blended surface as fully opaque regardless of this
//  wiring.
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
