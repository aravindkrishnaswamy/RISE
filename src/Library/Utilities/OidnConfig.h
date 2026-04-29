//////////////////////////////////////////////////////////////////////
//
//  OidnConfig.h - Configuration for the Intel OIDN denoiser quality
//    preset.
//
//    Carried alongside the existing `bool oidnDenoise` on/off flag
//    through the rasterizer factory chain.  Independent axis: the
//    bool drives whether denoise runs at all, this enum decides which
//    OIDN quality preset is used when it does.  See docs/OIDN.md
//    (OIDN-P0-1) for the auto heuristic semantics and tuning history.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 29, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OIDN_CONFIG_
#define OIDN_CONFIG_

namespace RISE
{
	/// OIDN quality preset selector for the post-process denoiser.
	///
	/// `Auto` picks one of the explicit presets per render using the
	/// heuristic `r = render_seconds / megapixels`:
	///   r < 3   → Fast
	///   r < 20  → Balanced
	///   r >= 20 → High
	///
	/// Explicit values force the preset regardless of render time.
	/// Replaces OIDN's `DEFAULT` constant on the public surface
	/// (DEFAULT was just an alias for HIGH; Auto is more useful).
	enum class OidnQuality
	{
		Auto = 0,
		High,
		Balanced,
		Fast
	};

	/// OIDN device backend selector.  Default is `Auto`, which asks
	/// OIDN to pick the fastest available device (Metal on Apple
	/// Silicon, CUDA / SYCL / HIP on supported workstations).  If
	/// the requested GPU backend is unavailable the denoiser falls
	/// back to CPU automatically.
	///
	///   Auto - prefer GPU, silently fall back to CPU.
	///   CPU  - force CPU.
	///   GPU  - prefer GPU, fall back to CPU with a warning so
	///          explicit requests aren't silently downgraded.
	///
	/// The actually-selected device type is logged on first denoise
	/// (e.g. "OIDN: creating Metal device (one-time per rasterizer)")
	/// so it's easy to confirm GPU acceleration is on after a render.
	enum class OidnDevice
	{
		Auto = 0,
		CPU,
		GPU
	};

	/// OIDN aux-buffer (albedo / normal) source mode.  Default is
	/// `Fast` — a separate 4-spp first-hit retrace pass after the
	/// main render, with `cleanAux=true` on the OIDN filter.  This
	/// is cheap (one extra ray cast per pixel) and accurate for
	/// most scenes, but the retrace returns white at NULL-BSDF
	/// surfaces (perfect glass / mirror), which forces OIDN to
	/// treat refracted / reflected detail as noise.
	///
	/// `Accurate` instead accumulates albedo + normal *during* the
	/// path trace, recording at the first vertex where the shader's
	/// scatter was non-delta (probabilistic per-sample detection
	/// via `ScatteredRay::isDelta`).  Glass / mirror surfaces are
	/// walked through naturally; rough dielectrics record at the
	/// rough surface or behind it depending on each sample's
	/// scatter, averaging to a Fresnel-weighted mix that matches
	/// the beauty.  Costs ~2× warm-cache denoise time because OIDN
	/// runs three filter passes (one prefilter each for albedo /
	/// normal, then beauty) — opt-in for users who care about
	/// caustic / reflection preservation.
	///
	/// MLT integrators ignore this knob — they always use the
	/// `Fast` retrace (their splat-film accumulation pattern is
	/// incompatible with inline accumulation).  See docs/OIDN.md
	/// (OIDN-P1-1) for the full design and per-integrator coverage.
	enum class OidnPrefilter
	{
		Fast = 0,
		Accurate
	};
}

#endif
