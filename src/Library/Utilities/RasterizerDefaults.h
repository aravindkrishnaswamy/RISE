//////////////////////////////////////////////////////////////////////
//
//  RasterizerDefaults.h - Single source of truth for production
//    rasterizer defaults.
//
//  Shared by ChunkParserRegistry (`Finalize` fallbacks and `Describe`
//  default hints) and `Job::InstantiateRasterizerWithDefaults` so all
//  three surfaces stay in agreement.  Nested `*Config` structs
//  (PixelFilterConfig, SMSConfig, etc.) remain canonical via their own
//  default constructors.  Interactive viewport preview
//  (InteractivePelRasterizer) separately overrides numPixelSamples = 1.
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
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>

namespace RISE
{
	// Auto-rasterizer integrator selector (docs/AUTO_RASTERIZER_DESIGN.md),
	// shared across API, parser, IJob, and AutoRasterizer.  `Auto` lets the
	// dispatcher choose; PT / BDPT / VCM pin the delegate explicitly.
	enum class AutoIntegratorChoice
	{
		Auto = 0,   ///< Dispatcher decides (Phase 1 -> PT).
		PT,         ///< Author pin: unidirectional path tracing.
		BDPT,       ///< Author pin: bidirectional path tracing.
		VCM         ///< Author pin: vertex connection and merging.
	};

	enum class RasterSequenceKind
	{
		Morton,
		Block,
		Hilbert,
		Scanline,
		RasterizerDefault
	};

	struct ResolvedRasterSequence
	{
		RasterSequenceKind kind = RasterSequenceKind::Morton;
		unsigned int tileSize = 32;
		unsigned int blockWidth = 0;
		unsigned int blockHeight = 0;
		unsigned int blockOrder = 0;
		unsigned int hilbertDepth = 0;
		std::uint32_t shuffleSeed = 0;
		bool hasShuffleSeed = false;
	};

	template <typename NextRandom>
	ResolvedRasterSequence ResolveRasterSequence(
		const int authoredType,
		const std::string& authoredOptions,
		NextRandom nextRandom )
	{
		ResolvedRasterSequence resolved;
		const auto bindShuffleSeed = [&]( ResolvedRasterSequence& sequence ) {
			if( sequence.blockOrder != 1u ) return;
			const double unit = nextRandom();
			const double span = static_cast<double>(
				std::numeric_limits<std::uint32_t>::max()) + 1.0;
			sequence.shuffleSeed = static_cast<std::uint32_t>(unit * span);
			sequence.hasShuffleSeed = true;
		};

		switch( authoredType ) {
		case 0:
			resolved.kind = RasterSequenceKind::Scanline;
			return resolved;
		case 1: {
			unsigned int width = 0, height = 0, order = 0;
			if( std::sscanf(authoredOptions.c_str(),"%u %u %u",
				&width,&height,&order) != 3 || width == 0u || height == 0u || order > 8u ) {
				return resolved;
			}
			resolved.kind = RasterSequenceKind::Block;
			resolved.blockWidth = width;
			resolved.blockHeight = height;
			resolved.blockOrder = order;
			bindShuffleSeed(resolved);
			return resolved;
		}
		case 2: {
			unsigned int depth = 0;
			if( std::sscanf(authoredOptions.c_str(),"%u",&depth) != 1 || depth == 0u ) {
				return resolved;
			}
			resolved.kind = RasterSequenceKind::Hilbert;
			resolved.hilbertDepth = depth;
			return resolved;
		}
		case 3:
			if( nextRandom() < 0.1 ) {
				resolved.kind = RasterSequenceKind::Hilbert;
				resolved.hilbertDepth = 4u;
			} else {
				resolved.kind = RasterSequenceKind::Block;
				resolved.blockWidth = 64u;
				resolved.blockHeight = 64u;
				resolved.blockOrder = static_cast<unsigned int>(
					std::floor(nextRandom()*8.999999));
				bindShuffleSeed(resolved);
			}
			return resolved;
		case 4:
		default: {
			unsigned int tileSize = 32u;
			if( !authoredOptions.empty() ) {
				unsigned int parsed = 0;
				if( std::sscanf(authoredOptions.c_str(),"%u",&parsed) == 1 && parsed > 0u ) {
					tileSize = parsed;
				}
			}
			resolved.tileSize = tileSize;
			return resolved;
		}
		}
	}

	// Universal production defaults (numPixelSamples = 32, oidnDenoise = true).
	// Subclasses override where behaviour differs (e.g. PixelPel samples = 1,
	// MLT oidnDenoise = false).  Interactive preview uses a separate 1-spp path.
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

	// `auto_rasterizer` defaults (docs/AUTO_RASTERIZER_DESIGN.md); delegate
	// specifics come from the per-integrator default structs above.
	struct AutoRasterizerDefaults : BaseRasterizerDefaults
	{
		AutoIntegratorChoice integrator = AutoIntegratorChoice::Auto;

		//! Opt-in render-time probe (default false; gated by GlobalOptions
		//! `auto_probe_activation_spp` — falls back to static routing below it).
		bool probeEnabled = false;
	};

	// MLT defaults: oidnDenoise is off because splat-film accumulation is
	// incompatible with OIDN (scenes may opt in with `oidn_denoise true`).
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
