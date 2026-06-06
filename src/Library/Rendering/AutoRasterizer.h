//////////////////////////////////////////////////////////////////////
//
//  AutoRasterizer.h - The auto-routing integrator dispatcher.
//
//    A thin `IRasterizer` wrapper that, at the first render-time entry
//    point, selects one of the concrete PT / BDPT / VCM rasterizers and
//    delegates every IRasterizer call to it.  It composes the existing
//    integrators — it contains NO integrator algorithm of its own.
//
//    Phase 1 (this file): selection is Tier-0 only — honour an explicit
//    author pin (`integrator pt|bdpt|vcm`), else fall back to PT (the
//    matrix's default-and-converged-bulk winner).  See
//    docs/AUTO_RASTERIZER_DESIGN.md §3 for the architecture and §1 for
//    the routing policy.
//
//    WHY LATE (render-time) RESOLUTION, not construction-time:
//    Phase 4 adds a render-time PROBE — a cheap pre-render of the
//    *assembled* scene to pick the integrator per-scene.  The assembled
//    scene only exists at `RasterizeScene` time, so the dispatcher
//    defers building its delegate until then (guarded by std::call_once)
//    rather than at parse/construction.  The wrapper stores everything
//    needed to build ANY of the three delegates and resolves exactly one
//    lazily; Phase 4 replaces the body of `SelectIntegrator` with the
//    probe and is free to build candidate delegates via `BuildDelegate`.
//    A parse-time-only dispatch could not host that probe, which is why
//    this is the foundation rather than a switch in Job::SetAutoRasterizer.
//
//    The base `Implementation::Rasterizer` already buffers the output
//    sinks (`outs`), the progress callback, and the FrameStore for us;
//    they are added before the delegate exists, so the dispatcher
//    REPLAYS them onto the freshly-built delegate at resolution time.
//
//    Spectral sibling (`auto_spectral_rasterizer`) is implemented as a
//    DOMAIN FLAG on this same class (Phase 1b, 2026-06-05): `mSpectral`
//    switches `BuildDelegate` to the `*_spectral_` factories and the
//    wrapper carries a `SpectralConfig` (range/bins/samples/HWSS) in place
//    of path-guiding / optimal-MIS.  The decision logic (SelectIntegrator
//    / RunProbe / ProbeCandidate) is shared verbatim, so there is ONE
//    source of truth for routing across both domains; see design doc §3.1.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 4, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef AUTO_RASTERIZER_
#define AUTO_RASTERIZER_

#include "Rasterizer.h"
#include "../Utilities/RasterizerDefaults.h"      // AutoIntegratorChoice
#include "../Utilities/OidnConfig.h"
#include "../Utilities/PathGuidingField.h"        // PathGuidingConfig
#include "../Utilities/AdaptiveSamplingConfig.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/ProgressiveConfig.h"
#include "../Utilities/SpectralConfig.h"        // SpectralConfig (auto_spectral domain)
#include <mutex>
#include <string>

namespace RISE
{
	class IRayCaster;
	class IPixelFilter;

	namespace Implementation
	{
		class AutoRasterizer : public Rasterizer
		{
		public:
			//! Construct the dispatcher.  The caster / samples / filter
			//! are the integrator-agnostic inputs every concrete factory
			//! shares; the configs are the universally-meaningful ones.
			//! Per-integrator specifics use the canonical built-in
			//! defaults at delegate-build time (param-population option
			//! (i)).  Nothing renders until the first render-time entry.
			AutoRasterizer(
				IRayCaster* caster,								///< [in] Shared ray caster (integrator-agnostic)
				ISampling2D* samples,							///< [in] Pixel sampler
				IPixelFilter* filter,							///< [in] Pixel reconstruction filter
				const AutoIntegratorChoice integrator,			///< [in] Author pin (Auto = dispatcher decides -> PT in Phase 1)
				const bool oidnDenoise,							///< [in] OIDN denoise (forwarded to the delegate)
				const OidnQuality oidnQuality,					///< [in] OIDN quality preset
				const OidnDevice oidnDevice,					///< [in] OIDN device backend
				const OidnPrefilter oidnPrefilter,				///< [in] OIDN aux source mode
				const PathGuidingConfig& guidingConfig,			///< [in] Path-guiding configuration
				const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
				const StabilityConfig& stabilityConfig,			///< [in] Production stability controls
				const bool useZSobol,							///< [in] Z-order Sobol sampler toggle
				const ProgressiveConfig& progressiveConfig,		///< [in] Progressive multi-pass configuration
				const bool probeEnabled,						///< [in] Enable the Tier-2 render-time probe (Phase 4; gated on activation-spp)
				const bool spectral,							///< [in] Domain: false = Pel (RGB), true = spectral (delegate to *_spectral_)
				const SpectralConfig& spectralConfig,			///< [in] Spectral-core params (range/bins/samples/HWSS); used only when spectral==true
				FrameStore* frameStore							///< [in] Canonical FrameStore (may be null until Job pushes one)
				);

		protected:
			virtual ~AutoRasterizer();

		public:
			//
			// IRasterizer surface the `Rasterizer` base does NOT provide.
			// Each resolves the delegate (once) then forwards.
			//
			virtual void AttachToScene( const IScene* pScene );
			virtual void DetachFromScene( const IScene* pScene );
			virtual unsigned int PredictTimeToRasterizeScene(
				const IScene& pScene,
				const ISampling2D& pSampling,
				unsigned int* pActualTime
				) const;
			virtual void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const;
			virtual void RasterizeSceneAnimation(
				const IScene& pScene,
				const Scalar time_start,
				const Scalar time_end,
				const unsigned int num_frames,
				const bool do_fields,
				const bool invert_fields,
				const Rect* pRect,
				const unsigned int* specificFrame,
				IRasterizeSequence* pRasterSequence
				) const;

			//! The integrator the dispatcher resolved to.  `Auto` until
			//! the first render-time entry runs selection; the concrete
			//! pick (PT/BDPT/VCM) thereafter.  For diagnostics / a future
			//! UI "Auto -> VCM" surfacing.
			AutoIntegratorChoice ResolvedIntegrator() const { return mResolved; }

			//! Total wall-clock seconds the Tier-2 probe spent rendering
			//! candidate integrators (0 if the probe didn't run).  Exposed
			//! so the §6.2 resolution/cost sweep can read the REAL in-process
			//! probe cost directly instead of log-scraping.  Valid after the
			//! first render-time entry.
			double LastProbeSeconds() const { return mLastProbeSeconds; }
			//! Number of candidate renders the probe issued (0 if it didn't run).
			unsigned int LastProbeRenders() const { return mLastProbeRenders; }

		private:
			//! Render-time probe tunables.  Read from `GlobalOptions` at
			//! selection time (so the Phase-4 resolution/cost sweep can
			//! vary them without recompiling) with the Phase-3 experiment
			//! values as defaults.  See AUTO_RASTERIZER_DESIGN.md §6.1.
			struct ProbeConfig
			{
				unsigned int spp;				///< probe samples-per-pixel (default 4)
				unsigned int scale;				///< resolution divisor: 2=half, 4=quarter, 8=eighth (default 4 — §6.2)
				double       tauCaustic;		///< median-lum VCM/PT ratio gate 1/2 -> caustic candidate (default 1.30)
				double       tauReach;			///< mean-lum VCM/PT (transport-reach) gate 2/2 -> VCM (default 1.50; rejects the jewel_vault over-fire)
				double       tauBdpt;			///< σ²·T PT/BDPT ratio -> BDPT (default 1.35)
				unsigned int varianceRenders;	///< sub-renders for the per-pixel σ² estimate (default 2)
				unsigned int activationSpp;		///< production spp at/above which the probe runs (default from §6.2 sweep)
			};

			//! One candidate's probe outcome.  `medianLum` is the
			//! firefly-robust caustic signal (single render); `meanVar`
			//! is the mean per-pixel cross-sub-render variance (needs
			//! `varianceRenders >= 2`); `rasSeconds` is the summed
			//! probe-render wall time (the T in σ²·T).
			struct ProbeResult
			{
				bool   valid;
				double medianLum;
				double meanLum;		///< mean per-pixel luminance (the μ in σ/μ; brightness-normalizer for the PT-struggling discriminator)
				double meanVar;
				double rasSeconds;
			};

			//! Integrator selection.  Tier 0: an explicit author pin always
			//! wins.  Tier 2 (Phase 4): when the probe is enabled and the
			//! production sample count clears the activation-spp gate, run
			//! the gated/short-circuited render-time probe over the
			//! assembled `scene` (RunProbe) and take its verdict.  Tier 1
			//! (Phase 2, the fallback when the probe is inactive): a cheap,
			//! conservative static analysis — route VCM where refractive
			//! caustics are plausible (a transmissive/dielectric surface AND
			//! a positional point/spot source), default everything else to
			//! PT.  Sets `mResolveReason` to the one-line explanation logged
			//! at resolution.
			AutoIntegratorChoice SelectIntegrator( const IScene* scene ) const;

			//! Read the probe tunables from GlobalOptions (Phase-3 defaults).
			ProbeConfig ReadProbeConfig() const;

			//! Tier-2 decision tree (AUTO_RASTERIZER_DESIGN.md §6.1/§6.2).  Caustic
			//! check FIRST, short-circuited: only when (dielectric ∧ ¬env-IBL)
			//! probe PT+VCM and route VCM iff BOTH the median gate
			//! median-lum(VCM)/median-lum(PT) > τ_caustic AND the transport-reach
			//! gate mean-lum(VCM)/mean-lum(PT) > τ_reach hold — the reach gate
			//! rejects the jewel_vault over-fire (median fires on a converging
			//! dielectric scene, but PT reaches the same total energy → not a real
			//! caustic).  Else the BDPT check: probe PT+BDPT and route BDPT iff
			//! σ²·T(PT)/σ²·T(BDPT) > τ_bdpt, else PT.  Sets mResolveReason.
			AutoIntegratorChoice RunProbe(
				const IScene* scene, const ProbeConfig& cfg,
				bool dielectric, bool envIbl ) const;

			//! Probe one candidate integrator.  Renders it `1` time
			//! (median-lum only, `needVariance=false`) or `cfg.varianceRenders`
			//! times (for the per-pixel σ² estimate) at probe resolution +
			//! probe spp, into a scratch (non-canonical) target, and folds the
			//! read-back pixels into a ProbeResult.  Does NOT touch the real
			//! FrameStore or the real delegate.
			ProbeResult ProbeCandidate(
				const IScene* scene, AutoIntegratorChoice choice,
				const ProbeConfig& cfg, bool needVariance ) const;

			//! Build the concrete delegate for `choice` using the stored
			//! inputs + canonical per-integrator defaults, with the canonical
			//! sampler (`mSamples`) and FrameStore (`GetFrameStore()`).
			//! Returns a refcount-1 IRasterizer (caller owns), or null.
			IRasterizer* BuildDelegate( AutoIntegratorChoice choice ) const;

			//! Build the concrete delegate for `choice` with explicit
			//! sampler / FrameStore / denoise / guiding / adaptive overrides
			//! — the probe passes a low-spp cloned sampler, a null FrameStore
			//! (so a candidate render lands in the delegate's own internal
			//! image, read back via a capturing output, and never disturbs
			//! the canonical store), denoise OFF (the σ² signal lives in the
			//! raw per-pixel noise that a denoiser would erase), and OFF
			//! guiding/adaptive configs (no expensive training pass; uniform
			//! spp so the probe cost + variance are well-defined).
			IRasterizer* BuildDelegate(
				AutoIntegratorChoice choice,
				ISampling2D* samples, FrameStore* fs,
				bool oidnDenoise,
				const PathGuidingConfig& guiding,
				const AdaptiveSamplingConfig& adaptive ) const;

			//! Resolve + wire the delegate exactly once, at the first
			//! render-time entry.  Replays the buffered output sinks /
			//! progress callback / progressive config onto it.
			void EnsureResolved( const IScene* scene ) const;

			//! Keep the delegate's FrameStore in lock-step with ours so a
			//! late `SetFrameStore` (deferred Job push, camera resize)
			//! reaches the object that actually writes pixels.
			void SyncDelegateFrameStore() const;

			// Integrator-agnostic build inputs (addref'd; released in dtor).
			IRayCaster*					mCaster;
			ISampling2D*				mSamples;
			IPixelFilter*				mFilter;

			// The author pin (Auto = dispatcher decides).
			AutoIntegratorChoice		mPinned;

			// Universally-meaningful configs, forwarded to the delegate
			// factory at build time.
			bool						mOidnDenoise;
			OidnQuality					mOidnQuality;
			OidnDevice					mOidnDevice;
			OidnPrefilter				mOidnPrefilter;
			PathGuidingConfig			mGuiding;
			AdaptiveSamplingConfig		mAdaptive;
			StabilityConfig				mStability;
			bool						mUseZSobol;
			ProgressiveConfig			mProgressive;

			// Tier-2 probe master enable (Phase 4).  The probe additionally
			// requires the production sample count to clear the activation-spp
			// gate; see ReadProbeConfig / SelectIntegrator.
			bool						mProbeEnabled;

			// Domain selector (Phase 1b).  false = Pel (RGB) factories; true =
			// spectral factories.  The ONLY axis on which the two siblings
			// differ — the SelectIntegrator/RunProbe decision logic is shared.
			bool						mSpectral;
			// Spectral-core params (range / bins / samples / HWSS).  Carried
			// only for the spectral domain; default-constructed (unused) for
			// Pel.  BuildDelegate unpacks it into the *_spectral_ factory args.
			SpectralConfig				mSpectralConfig;

			// Lazily-resolved delegate.  `mutable` because RasterizeScene
			// (and its siblings) are `const` per the IRasterizer contract
			// but are the genuine render-time entry where resolution must
			// happen (the base class already uses `mutable` for state
			// reached from those const methods).
			mutable IRasterizer*			mDelegate;
			mutable AutoIntegratorChoice	mResolved;
			mutable std::once_flag			mResolveOnce;

			// Cost instrumentation for the §6.2 sweep (set by RunProbe; 0
			// when the probe is inactive).
			mutable double					mLastProbeSeconds;
			mutable unsigned int			mLastProbeRenders;

			// One-line, human-readable reason for the resolved choice
			// (e.g. "dielectric + positional light"), set by
			// SelectIntegrator and surfaced in the resolution log line for
			// diagnostics + the future UI "Auto -> VCM: <reason>" display.
			mutable std::string				mResolveReason;
		};
	}
}

#endif
