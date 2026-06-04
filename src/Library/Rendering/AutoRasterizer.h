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
//    Spectral sibling (`auto_spectral_rasterizer`) is the immediate
//    Phase-1b follow-up — it delegates to the `*_spectral_` variants and
//    carries the spectral-core params; see the design doc §3.  Adding it
//    is a parallel class (or a domain flag) plus a BuildDelegate switch
//    over the spectral factories.
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
#include <mutex>

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

		private:
			//! Phase-1 selection (Tier 0 only): honour the pin, else PT.
			//! Phase 4 replaces this body with the render-time probe over
			//! `scene` (kept in the signature precisely so that drop-in is
			//! a body change, not a surgery on the call sites).
			AutoIntegratorChoice SelectIntegrator( const IScene* scene ) const;

			//! Build the concrete delegate for `choice` using the stored
			//! inputs + canonical per-integrator defaults.  Returns a
			//! refcount-1 IRasterizer (caller owns), or null on failure.
			IRasterizer* BuildDelegate( AutoIntegratorChoice choice ) const;

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

			// Lazily-resolved delegate.  `mutable` because RasterizeScene
			// (and its siblings) are `const` per the IRasterizer contract
			// but are the genuine render-time entry where resolution must
			// happen (the base class already uses `mutable` for state
			// reached from those const methods).
			mutable IRasterizer*			mDelegate;
			mutable AutoIntegratorChoice	mResolved;
			mutable std::once_flag			mResolveOnce;
		};
	}
}

#endif
