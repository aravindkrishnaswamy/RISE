//////////////////////////////////////////////////////////////////////
//
//  InteractivePelRasterizer.cpp
//
//  All defaults are "minimum-cost preview": no GI, no path guiding,
//  no adaptive sampling, no OIDN denoiser, 1 SPP.  We rely on the
//  default ctors of PathGuidingConfig / AdaptiveSamplingConfig /
//  StabilityConfig, all of which produce "disabled" state.  The
//  zsobol flag is left false: low-discrepancy ordering buys little
//  at 1 SPP.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "InteractivePelRasterizer.h"
#include "BlockRasterizeSequence.h"
#include "MortonRasterizeSequence.h"
#include "ScanlineRasterizeSequence.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/ISampling2D.h"
#include "../RasterImages/RasterImage.h"
#include "../RISE_API.h"
#include "../Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

InteractivePelRasterizer::InteractivePelRasterizer( IRayCaster* pCaster, const Config& cfg )
: PixelBasedRasterizerHelper( pCaster )
, PixelBasedPelRasterizer(
    pCaster,
    PathGuidingConfig(),       // disabled by default
    AdaptiveSamplingConfig(),  // maxSamples=0 == disabled
    StabilityConfig(),         // default stability bounds
    /*useZSobol*/false
  )
, mCfg( cfg )
, mIdleMode( false )
, mPolishKernel( 0 )
, mPolishCaster( 0 )
, mSavedPreviewCaster( 0 )
{
}

InteractivePelRasterizer::~InteractivePelRasterizer()
{
	// If a polish pass was active when destroyed (shouldn't happen
	// in practice — the controller always restores after the pass —
	// but be defensive), make sure pCaster points back to the
	// preview caster before the base destructor releases it.
	if( mSavedPreviewCaster ) {
		safe_release( pCaster );
		pCaster = mSavedPreviewCaster;
		mSavedPreviewCaster = 0;
	}
	safe_release( mPolishKernel );
	safe_release( mPolishCaster );
}

void InteractivePelRasterizer::SetPolishRayCaster( IRayCaster* polishCaster )
{
	if( mPolishCaster == polishCaster ) return;
	safe_release( mPolishCaster );
	mPolishCaster = polishCaster;
	if( mPolishCaster ) {
		mPolishCaster->addref();
	}
}

void InteractivePelRasterizer::SetIdleMode( bool idle ) const
{
	mIdleMode = idle;
	// The actual switch from 1-pass to multi-pass progressive lives
	// in the SceneEditController's render loop driver (Phase 2): it
	// inspects IsIdleMode() before each RasterizeScene call and
	// configures the rasterizer's progressiveConfig accordingly.
	// Doing it here would be order-dependent on the controller's
	// own state and risk thrashing.
}

void InteractivePelRasterizer::SetSampleCount( unsigned int n )
{
	if( n <= 1 ) {
		// 1-SPP — clear pSampling so per-pixel integration falls
		// back to the single-ray path.  Leave progressiveConfig
		// alone; without pSampling the progressive path is skipped.
		safe_release( pSampling );
		pSampling = 0;

		// If we'd swapped to the polish caster, restore the preview
		// caster.  pCaster currently holds an addref'd polish caster;
		// release that and adopt the saved preview caster (whose
		// refcount we preserved at swap time).
		if( mSavedPreviewCaster ) {
			safe_release( pCaster );
			pCaster = mSavedPreviewCaster;
			mSavedPreviewCaster = 0;
		}
		return;
	}

	// Multi-SPP polish.  Lazy-init a 2D sampling kernel; reuse on
	// subsequent polish calls (just reset numSamples in case the
	// caller asked for a different count).
	if( !mPolishKernel ) {
		// MultiJittered sample dimensions are the kernel's spatial
		// extent (pixels); 1.0 × 1.0 means the samples spread over
		// one full pixel.  Sample COUNT is independent and set via
		// SetNumSamples below.
		RISE_API_CreateMultiJitteredSampling2D( &mPolishKernel, 1.0, 1.0 );
	}
	if( mPolishKernel ) {
		mPolishKernel->SetNumSamples( n );
	}

	safe_release( pSampling );
	pSampling = mPolishKernel;
	if( pSampling ) {
		pSampling->addref();
	}

	// Disable progressive so we get exactly ONE pass at n SPP rather
	// than splitting into multiple progressive sub-passes.
	ProgressiveConfig cfg;
	cfg.enabled = false;
	cfg.samplesPerPass = n;
	SetProgressiveConfig( cfg );

	// Swap to the polish ray caster (higher max-recursion: one
	// bounce of glossy / reflected / refracted rays vs. preview's
	// primary-only).  Idempotent: if we've already swapped, leave
	// the existing state in place.
	if( mPolishCaster && !mSavedPreviewCaster ) {
		mSavedPreviewCaster = pCaster;   // keep its refcount on the saved slot
		pCaster = mPolishCaster;
		pCaster->addref();
	}
}

void InteractivePelRasterizer::PrepareImageForNewRender( IRasterImage& /*img*/, const Rect* /*pRect*/ ) const
{
	// Intentionally empty.  The default impl clears to a random
	// pastel and fires OutputIntermediateImage; both produce visible
	// flashes during the interactive cancel-restart loop.  We want
	// the previous frame's pixels to stay on screen until the new
	// tiles overwrite them, so we skip the clear.  Our viewport sink
	// also ignores OutputIntermediateImage (it only dispatches at
	// end-of-pass), so skipping the notification is harmless.
}

void InteractivePelRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	// Inherit everything the production base configures (stability,
	// optimal MIS, path guiding, etc.) so the fast-preview render
	// produces images consistent with what production would produce
	// for non-expensive shader ops.
	PixelBasedPelRasterizer::PrepareRuntimeContext( rc );

	// Then signal "this is the interactive preview path" so shader
	// ops that have a fast-preview branch take it.  See
	// RuntimeContext::bFastPreview for the contract.
	rc.bFastPreview = true;
}

IRasterizeSequence* InteractivePelRasterizer::CreateDefaultRasterSequence( unsigned int tileEdge ) const
{
	// BlockRasterizeSequence's `type` argument:
	//   0 = centre-out (sort by distance from image centre)
	//   1 = random shuffle
	//   2 = top-left
	switch( mCfg.tileOrder )
	{
	case TileOrder_Random:
		return new BlockRasterizeSequence( tileEdge, tileEdge, 1 );
	case TileOrder_Scanline:
		// Scanline goes left-to-right, top-to-bottom — closest
		// available is BlockRasterizeSequence type 2 (top-left
		// distance), which gives a roughly scanline-ish order.
		return new BlockRasterizeSequence( tileEdge, tileEdge, 2 );
	case TileOrder_CenterOut:
	default:
		return new BlockRasterizeSequence( tileEdge, tileEdge, 0 );
	}
}
