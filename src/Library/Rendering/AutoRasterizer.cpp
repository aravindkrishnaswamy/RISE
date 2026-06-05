//////////////////////////////////////////////////////////////////////
//
//  AutoRasterizer.cpp - Implements the auto-routing integrator
//    dispatcher.  See AutoRasterizer.h for the architecture rationale.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 4, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AutoRasterizer.h"
#include "../RISE_API.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IPixelFilter.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Utilities/SMSConfig.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Lowercase scene-language spelling of an integrator choice; used
	//! only for the diagnostic log line that surfaces the runtime pick.
	const char* IntegratorName( AutoIntegratorChoice c )
	{
		switch( c ) {
			case AutoIntegratorChoice::PT:   return "pt";
			case AutoIntegratorChoice::BDPT: return "bdpt";
			case AutoIntegratorChoice::VCM:  return "vcm";
			case AutoIntegratorChoice::Auto:
			default:                         return "auto";
		}
	}

	//! Tier-1 signal: does any visible object carry a transmissive /
	//! dielectric material (`IMaterial::CouldLightPassThrough()`)?  That
	//! is the surface kind — glass, water, gems — that bends light into
	//! refractive caustics.  One early-out object enumeration (stops at
	//! the first match).  Mirrors the `MediaScan` idiom in
	//! LightSampler::Prepare (return false = stop, true = continue).
	bool SceneHasTransmissiveMaterial( const IScene& scene )
	{
		struct TransmissiveScan : public IEnumCallback<IObject>
		{
			bool found;
			TransmissiveScan() : found( false ) {}
			bool operator()( const IObject& obj )
			{
				const IMaterial* mat = obj.GetMaterial();
				if( mat && mat->CouldLightPassThrough() ) {
					found = true;
					return false;   // one is enough — stop enumeration
				}
				return true;        // keep scanning
			}
		};

		const IObjectManager* objs = scene.GetObjects();
		if( !objs ) {
			return false;
		}
		TransmissiveScan scan;
		objs->EnumerateObjects( scan );
		return scan.found;
	}

	//! Tier-1 signal: does the scene have at least one positional
	//! (point / spot / omni) delta light?  These are the small / point
	//! sources able to concentrate refracted energy into a sharp caustic;
	//! `IsPositionalLight()` returns false for directional and ambient
	//! lights (and there is no entry for area / mesh emitters or env-IBL
	//! in the hack-light list at all — those are objects-with-emitter and
	//! the global radiance map respectively, deliberately NOT counted as
	//! caustic-prone here, which is what keeps area-lit dielectric scenes
	//! on PT/BDPT instead of over-routing them to VCM).
	bool SceneHasPositionalLight( const IScene& scene )
	{
		const ILightManager* lights = scene.GetLights();
		if( !lights ) {
			return false;
		}
		const ILightManager::LightsList& list = lights->getLights();
		for( ILightManager::LightsList::const_iterator it = list.begin(); it != list.end(); ++it ) {
			const ILightPriv* l = *it;
			if( l && l->IsPositionalLight() ) {
				return true;
			}
		}
		return false;
	}
}

AutoRasterizer::AutoRasterizer(
	IRayCaster* caster,
	ISampling2D* samples,
	IPixelFilter* filter,
	const AutoIntegratorChoice integrator,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const bool useZSobol,
	const ProgressiveConfig& progressiveConfig,
	FrameStore* frameStore
	) :
	Rasterizer( frameStore )
	,mCaster( caster )
	,mSamples( samples )
	,mFilter( filter )
	,mPinned( integrator )
	,mOidnDenoise( oidnDenoise )
	,mOidnQuality( oidnQuality )
	,mOidnDevice( oidnDevice )
	,mOidnPrefilter( oidnPrefilter )
	,mGuiding( guidingConfig )
	,mAdaptive( adaptiveConfig )
	,mStability( stabilityConfig )
	,mUseZSobol( useZSobol )
	,mProgressive( progressiveConfig )
	,mDelegate( 0 )
	,mResolved( AutoIntegratorChoice::Auto )
{
	// Hold the shared build inputs alive for the wrapper's lifetime —
	// the delegate is built lazily and may not exist for many frames
	// after construction, so the caster / samples / filter must survive
	// past the Job's post-factory safe_release of its own local refs
	// (mirrors how the concrete rasterizers addref these via their
	// constructor + SubSampleRays).
	if( mCaster )  mCaster->addref();
	if( mSamples ) mSamples->addref();
	if( mFilter )  mFilter->addref();
}

AutoRasterizer::~AutoRasterizer()
{
	// The base ~Rasterizer releases mFrameStore + the buffered outs.
	safe_release( mDelegate );
	safe_release( mCaster );
	safe_release( mSamples );
	safe_release( mFilter );
}

AutoIntegratorChoice AutoRasterizer::SelectIntegrator( const IScene* scene ) const
{
	// Tier 0 — an explicit author pin always wins and skips static
	// analysis entirely (and Phase 4's probe).
	switch( mPinned ) {
		case AutoIntegratorChoice::PT:   mResolveReason = "author pin"; return AutoIntegratorChoice::PT;
		case AutoIntegratorChoice::BDPT: mResolveReason = "author pin"; return AutoIntegratorChoice::BDPT;
		case AutoIntegratorChoice::VCM:  mResolveReason = "author pin"; return AutoIntegratorChoice::VCM;
		case AutoIntegratorChoice::Auto:
		default:                         break;   // -> Tier-1 static analysis
	}

	// Tier 1 — cheap, conservative static best-guess over the assembled
	// scene (docs/AUTO_RASTERIZER_DESIGN.md §5).  Two early-out scans give
	// the only two signals the routing keys on; everything else defaults
	// to PT, the matrix's converged-bulk winner.  `scene` is the assembled
	// scene because resolution runs at the first render-time entry (not at
	// construction); Phase 4 replaces this body with the probe.
	if( !scene ) {
		// Defensive: the render path always passes a real scene, but a
		// null here must not crash the dispatcher — fall back to PT.
		mResolveReason = "no scene (defaulted)";
		return AutoIntegratorChoice::PT;
	}

	const bool hasTransmissive = SceneHasTransmissiveMaterial( *scene );
	const bool hasPositional   = SceneHasPositionalLight( *scene );

	// VCM — only where refractive caustics are plausible: a transmissive /
	// dielectric surface (glass / water / gem) AND a positional point/spot
	// source to concentrate the refracted energy.  This is deliberately
	// COARSE (a dielectric scene without strong caustics will still lean
	// VCM here; the Phase-4 probe is what refines it back off VCM).  The
	// positional-light requirement is what spares the dielectric-but-
	// area-lit diffuse/indirect scenes — jewel_vault, cloister, alchemists,
	// env_only, prism all carry glass yet have NO point light, so they
	// correctly stay off VCM.  Validated against the Phase-1 matrix: this
	// hits all four point/spot-lit dielectric-caustic scenes (pool_caustics,
	// glass_pavilion, diamond_teapot, torus_chain) with no false positives.
	if( hasTransmissive && hasPositional ) {
		mResolveReason = "dielectric + positional light";
		return AutoIntegratorChoice::VCM;
	}

	// Everything else -> PT.  Note in particular that the strong-indirect /
	// BDPT regime is NOT routed here.  It is not statically separable from
	// the PT-efficient glossy/diffuse bulk via cheap IScene signals: the
	// BDPT-winning gi_spheres is byte-identical in every reachable signal
	// to the PT-winning ggx_showcase (both enclosed, single area light, no
	// point/env/glass/fog — they differ only in diffuse-vs-glossy
	// reflectance, which IScene exposes no accessor for).  Routing on that
	// would mis-send the converged glossy bulk to BDPT, the expensive
	// mistake.  So BDPT detection is deferred to the Phase-4 σ²·T probe;
	// the static tier conservatively defaults to PT.  (Full analysis +
	// matrix hit/miss table in AUTO_RASTERIZER_DESIGN.md §5.)
	mResolveReason = "no caustic/strong-indirect signal";
	return AutoIntegratorChoice::PT;
}

IRasterizer* AutoRasterizer::BuildDelegate( AutoIntegratorChoice choice ) const
{
	IRasterizer* d = 0;
	FrameStore* fs = GetFrameStore();   // current canonical store (may be null)

	switch( choice )
	{
	case AutoIntegratorChoice::BDPT:
		{
			// Per-integrator specifics use the canonical defaults — the
			// single source of truth shared with the bdpt_pel_rasterizer
			// parser + Job lazy-build (option (i) param population).
			BDPTPelDefaults bd;
			RISE_API_CreateBDPTPelRasterizer( &d, mCaster, mSamples, mFilter,
				bd.maxEyeDepth, bd.maxLightDepth,
				mOidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
				mGuiding, mAdaptive, mStability, mUseZSobol, fs );
		}
		break;

	case AutoIntegratorChoice::VCM:
		{
			VCMPelDefaults vd;
			RISE_API_CreateVCMPelRasterizer( &d, mCaster, mSamples, mFilter,
				vd.maxEyeDepth, vd.maxLightDepth, vd.mergeRadius, vd.enableVC, vd.enableVM,
				mOidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
				mGuiding, mAdaptive, mStability, mUseZSobol, fs );
		}
		break;

	case AutoIntegratorChoice::PT:
	case AutoIntegratorChoice::Auto:
	default:
		{
			// Default SMSConfig == SMS disabled, which is exactly what a
			// bare pathtracing_pel_rasterizer builds.  The factory ignores
			// the remaining SMS fields when enabled == false.
			SMSConfig sms;
			RISE_API_CreatePathTracingPelRasterizer( &d, mCaster, mSamples, mFilter,
				sms.enabled, sms.maxIterations, sms.threshold, sms.maxChainDepth, sms.biased,
				sms.bernoulliTrials, sms.multiTrials, sms.photonCount, sms.twoStage,
				sms.useLevenbergMarquardt, sms.seedingMode, sms.targetBounces,
				mOidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
				mGuiding, mAdaptive, mStability, mUseZSobol, fs );
		}
		break;
	}

	return d;
}

void AutoRasterizer::EnsureResolved( const IScene* scene ) const
{
	std::call_once( mResolveOnce, [this, scene]() {
		const AutoIntegratorChoice choice = SelectIntegrator( scene );
		mResolved = choice;
		mDelegate = BuildDelegate( choice );

		if( !mDelegate ) {
			GlobalLog()->PrintEasyError(
				"AutoRasterizer:: failed to build the chosen delegate rasterizer" );
			return;
		}

		// Replay the buffered render state onto the freshly-built
		// delegate.  The base Rasterizer stored these (the file output /
		// viewport sink via AddRasterizerOutput, the progress callback)
		// BEFORE the delegate existed; the delegate is the object that
		// actually renders into them.
		{
			std::lock_guard<std::mutex> lock( outsMutex );
			for( IRasterizerOutput* ro : outs ) {
				mDelegate->AddRasterizerOutput( ro );
			}
		}
		mDelegate->SetProgressCallback( pProgressFunc );

		// Progressive config can't ride RISE_API_SetRasterizerProgressiveRendering
		// on the wrapper (that down-casts to PixelBasedRasterizerHelper, which the
		// wrapper is not) — apply it to the delegate, which IS one.
		RISE_API_SetRasterizerProgressiveRendering(
			mDelegate, mProgressive.enabled, mProgressive.samplesPerPass );

		GlobalLog()->PrintEx( eLog_Event,
			"AutoRasterizer:: integrator '%s' -> delegating to '%s' (%s)",
			IntegratorName( mPinned ), IntegratorName( choice ),
			mResolveReason.empty() ? "default" : mResolveReason.c_str() );
	} );
}

void AutoRasterizer::SyncDelegateFrameStore() const
{
	if( !mDelegate ) {
		return;
	}
	FrameStore* mine = GetFrameStore();
	if( mDelegate->GetFrameStore() == mine ) {
		return;   // already in sync (the common case)
	}
	// The delegate is always an in-tree Rasterizer; push our current
	// FrameStore so a late SetFrameStore (deferred Job push when the
	// camera dims weren't known at construction, or a viewport resize)
	// reaches the object that writes pixels.  SetFrameStore balances the
	// refcount internally (addref new, release old).
	Rasterizer* r = dynamic_cast<Rasterizer*>( mDelegate );
	if( r ) {
		r->SetFrameStore( mine );
	}
}

void AutoRasterizer::AttachToScene( const IScene* pScene )
{
	EnsureResolved( pScene );
	SyncDelegateFrameStore();
	if( mDelegate ) {
		mDelegate->AttachToScene( pScene );
	}
}

void AutoRasterizer::DetachFromScene( const IScene* pScene )
{
	if( mDelegate ) {
		mDelegate->DetachFromScene( pScene );
	}
}

unsigned int AutoRasterizer::PredictTimeToRasterizeScene(
	const IScene& pScene,
	const ISampling2D& pSampling,
	unsigned int* pActualTime
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	if( mDelegate ) {
		return mDelegate->PredictTimeToRasterizeScene( pScene, pSampling, pActualTime );
	}
	if( pActualTime ) {
		*pActualTime = 0;
	}
	return 0;
}

void AutoRasterizer::RasterizeScene(
	const IScene& pScene,
	const Rect* pRect,
	IRasterizeSequence* pRasterSequence
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	if( mDelegate ) {
		mDelegate->RasterizeScene( pScene, pRect, pRasterSequence );
	}
}

void AutoRasterizer::RasterizeSceneAnimation(
	const IScene& pScene,
	const Scalar time_start,
	const Scalar time_end,
	const unsigned int num_frames,
	const bool do_fields,
	const bool invert_fields,
	const Rect* pRect,
	const unsigned int* specificFrame,
	IRasterizeSequence* pRasterSequence
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	if( mDelegate ) {
		mDelegate->RasterizeSceneAnimation( pScene, time_start, time_end, num_frames,
			do_fields, invert_fields, pRect, specificFrame, pRasterSequence );
	}
}
