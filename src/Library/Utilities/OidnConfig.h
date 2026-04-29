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
}

#endif
