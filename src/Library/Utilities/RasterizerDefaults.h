//////////////////////////////////////////////////////////////////////
//
//  RasterizerDefaults.h - Single source of truth for the defaults
//    used by every production rasterizer.
//
//    Three independent code paths historically picked their own
//    defaults for "what value should this parameter take when the
//    user / scene file omits it?":
//
//      1. AsciiSceneParser chunk parsers' `Finalize` fallback —
//         `bag.GetUInt("samples", 32)` style.
//      2. AsciiSceneParser chunk parsers' `Describe` defaultValueHint
//         strings — what the GUI panel labels the default as.
//      3. Job::InstantiateRasterizerWithDefaults — the literals it
//         passes when the GUI lazy-builds a rasterizer with no
//         matching chunk in the scene file.
//
//    Plus the underlying `*Config` structs (PixelFilterConfig /
//    SMSConfig / etc.) which have their own in-class member
//    initializers that act as a fourth default surface for nested
//    config knobs.
//
//    Drift between these surfaces produced visible bugs: BDPT-pel
//    parser rendered at 32 spp with OIDN on, while clicking the
//    same rasterizer in the GUI panel without a chunk gave 1 spp
//    with OIDN off.  This header collapses surfaces 1-3 into one
//    set of structs, while the *Config* structs remain canonical
//    for nested-config defaults (consumed via default-construction).
//
//    Convention: every field on every *Defaults struct in this
//    header is the canonical value.  Parsers reference it via
//    `BDPTPelDefaults d; bag.GetUInt("samples", d.numPixelSamples)`,
//    descriptors reference it via `to_hint(d.numPixelSamples)`, and
//    Job's lazy-build branches default-construct the matching
//    struct and read its fields directly.  Tests assert all three
//    surfaces remain in agreement.
//
//    Interactive viewport preview (InteractivePelRasterizer) is a
//    separate code path that overrides `numPixelSamples` to 1 for
//    live-edit feedback — the production-quality default of 32 lets
//    a "click Render with no chunk" produce a real image instead of
//    one-sample noise.
//
//    Adding a new rasterizer: add a `*Defaults` struct here with
//    its parameter values, then have the parser / Job branch read
//    from it.  Adding a new parameter: add a field to the relevant
//    *Defaults struct, surface it in the parser's Finalize +
//    Describe, and the GUI panel + lazy-build path pick it up
//    automatically.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 5, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RASTERIZER_DEFAULTS_
#define RASTERIZER_DEFAULTS_

#include "OidnConfig.h"
#include <string>

namespace RISE
{
	//
	// Common base — every production rasterizer accepts these.  The
	// values here are the universal canonical defaults; per-rasterizer
	// structs override individual fields where their behaviour
	// genuinely differs (MLT overrides oidnDenoise, MLT overrides the
	// depth caps, etc.).
	//
	// numPixelSamples = 32: production-ready default.  The interactive
	//   viewport preview (InteractivePelRasterizer) is a separate code
	//   path that uses 1.  See PixelBasedRasterizerHelper.cpp.
	//
	// oidnDenoise = true: matches modern denoise-by-default expectation.
	//   MLT overrides to false because the splat-film accumulation
	//   pattern is incompatible with OIDN.
	//
	struct BaseRasterizerDefaults
	{
		unsigned int   numPixelSamples = 32;
		bool           showLuminaires  = true;
		bool           oidnDenoise     = true;
		OidnQuality    oidnQuality     = OidnQuality::Auto;
		OidnDevice     oidnDevice      = OidnDevice::Auto;
		OidnPrefilter  oidnPrefilter   = OidnPrefilter::Fast;
		std::string    defaultShader   = "global";
	};

	//
	// Legacy `pixelpel_rasterizer` — recursion-driven shader-op
	// integrator used by the pre-PT codepath.  Same base values plus
	// max_recursion / lum_samples / luminary_sampler.
	//
	struct PixelPelDefaults : BaseRasterizerDefaults
	{
		unsigned int   maxRecursion         = 10;
		unsigned int   numLumSamples        = 1;
		std::string    luminarySampler      = "none";
		double         luminarySamplerParam = 1.0;

		PixelPelDefaults()
		{
			// PixelPel was historically the only RGB rasterizer that
			// kept `samples = 1`.  Snap to that to avoid surprising
			// users who select pixelpel for a quick-look render.
			numPixelSamples = 1;
		}
	};

	//
	// Legacy `pixelintegratingspectral_rasterizer` — same recursion
	// model as PixelPel plus the RGB-to-SPD conversion knob.
	//
	struct PixelIntegratingSpectralDefaults : PixelPelDefaults
	{
		bool           integrateRGB = false;
	};

	struct PathTracingPelDefaults      : BaseRasterizerDefaults {};
	struct PathTracingSpectralDefaults : BaseRasterizerDefaults {};

	struct BDPTPelDefaults : BaseRasterizerDefaults
	{
		unsigned int   maxEyeDepth   = 8;
		unsigned int   maxLightDepth = 8;
	};
	struct BDPTSpectralDefaults : BDPTPelDefaults {};

	struct VCMPelDefaults : BaseRasterizerDefaults
	{
		unsigned int   maxEyeDepth   = 8;
		unsigned int   maxLightDepth = 8;
		double         mergeRadius   = 0.0;   // 0 = scene-auto
		bool           enableVC      = true;
		bool           enableVM      = true;
	};
	struct VCMSpectralDefaults : VCMPelDefaults {};

	//
	// MLT canonical — production values matching what scene authors
	// have historically gotten when they omit these fields.  OIDN is
	// off by default because the splat-film accumulation pattern is
	// incompatible with the denoiser; users who want denoised MLT
	// can opt in with `oidn_denoise true` per scene.
	//
	struct MLTDefaults : BaseRasterizerDefaults
	{
		unsigned int   maxEyeDepth        = 10;
		unsigned int   maxLightDepth      = 10;
		unsigned int   nBootstrap         = 100000;
		unsigned int   nChains            = 512;
		unsigned int   nMutationsPerPixel = 32;
		double         largeStepProb      = 0.3;

		MLTDefaults()
		{
			// Override the base default — MLT cannot use OIDN (see
			// the MLT parser's oidn_denoise comment for the splat-
			// film rationale).
			oidnDenoise = false;
		}
	};
	struct MLTSpectralDefaults : MLTDefaults {};

}

#endif
