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

AutoIntegratorChoice AutoRasterizer::SelectIntegrator( const IScene* /*scene*/ ) const
{
	// Phase 1 — Tier 0 only: honour an explicit author pin; everything
	// else (Auto / unset) falls back to PT, the matrix's default and
	// converged-bulk winner.  Phases 2-4 replace this body with static
	// scene analysis + a render-time probe over the assembled `scene`
	// (which is why `scene` is already threaded into the signature and
	// resolution runs at the first render-time entry, not construction).
	switch( mPinned ) {
		case AutoIntegratorChoice::PT:   return AutoIntegratorChoice::PT;
		case AutoIntegratorChoice::BDPT: return AutoIntegratorChoice::BDPT;
		case AutoIntegratorChoice::VCM:  return AutoIntegratorChoice::VCM;
		case AutoIntegratorChoice::Auto:
		default:                         return AutoIntegratorChoice::PT;
	}
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
			"AutoRasterizer:: integrator pin '%s' -> delegating to '%s' rasterizer",
			IntegratorName( mPinned ), IntegratorName( choice ) );
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
