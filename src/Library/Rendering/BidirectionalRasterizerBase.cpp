//////////////////////////////////////////////////////////////////////
//
//  BidirectionalRasterizerBase.cpp - Shared splat-film plumbing
//    for BDPTRasterizerBase and VCMRasterizerBase.
//
//    Previously duplicated in two places.  See the header for the
//    inheritance layout and rationale.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BidirectionalRasterizerBase.h"
#include "FilteredFilm.h"
#include "FrameStore.h"  // FrameStoreBulkBracket RAII guard (Flush* splat composition)
#include "../RasterImages/RasterImage.h"

using namespace RISE;
using namespace RISE::Implementation;

BidirectionalRasterizerBase::BidirectionalRasterizerBase(
	IRayCaster* pCaster_,
	const StabilityConfig& stabilityCfg,
	RISE::Implementation::FrameStore* frameStore
	) :
	PixelBasedRasterizerHelper( pCaster_ ),
	pSplatFilm( 0 ),
	pScratchImage( 0 ),
	mSplatTotalSamples( 1.0 ),
	mTotalAdaptiveSamples( 0 ),
	mActiveSplatRegion( 0, 0, 0, 0 ),
	mHasActiveSplatRegion( false ),
	stabilityConfig( stabilityCfg )
{
}

void BidirectionalRasterizerBase::ConfigureSplatRegion(
	const Rect* region,
	unsigned int width,
	unsigned int height
	) const
{
	mHasActiveSplatRegion = false;
	if( !region || width == 0 || height == 0 ||
		region->left > region->right || region->top > region->bottom ||
		region->left >= width || region->top >= height ) return;
	mActiveSplatRegion = Rect(
		region->top,
		region->left,
		r_min( region->bottom, height - 1 ),
		r_min( region->right, width - 1 ) );
	mHasActiveSplatRegion = true;
}

BidirectionalRasterizerBase::~BidirectionalRasterizerBase()
{
	safe_release( pSplatFilm );
	safe_release( pScratchImage );
}

void BidirectionalRasterizerBase::AddAdaptiveSamples( uint64_t count ) const
{
	mTotalAdaptiveSamples.fetch_add( count, std::memory_order_relaxed );
}

uint64_t BidirectionalRasterizerBase::RegionalPixelCount(
	unsigned int width,
	unsigned int height,
	const Rect* region
	)
{
	if( !region || width == 0 || height == 0 ||
		region->left > region->right || region->top > region->bottom ||
		region->left >= width || region->top >= height ) {
		return static_cast<uint64_t>( width ) * height;
	}
	const unsigned int right = r_min( region->right, width - 1 );
	const unsigned int bottom = r_min( region->bottom, height - 1 );
	return
		static_cast<uint64_t>( right - region->left + 1 ) *
		static_cast<uint64_t>( bottom - region->top + 1 );
}

Scalar BidirectionalRasterizerBase::RegionalSplatSPP(
	Scalar fullFrameSPP,
	unsigned int width,
	unsigned int height,
	const Rect* region
	)
{
	if( width == 0 || height == 0 ) return fullFrameSPP;
	const Scalar regionArea = static_cast<Scalar>(
		RegionalPixelCount( width, height, region ) );
	const Scalar fullArea = static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	return fullFrameSPP * regionArea / fullArea;
}

Scalar BidirectionalRasterizerBase::GetEffectiveSplatSPP(
	unsigned int width,
	unsigned int height
	) const
{
	const uint64_t totalSamples = mTotalAdaptiveSamples.load( std::memory_order_relaxed );
	if( totalSamples > 0 && width > 0 && height > 0 ) {
		const Scalar avgSPP =
			static_cast<Scalar>( totalSamples ) /
			( static_cast<Scalar>( width ) * static_cast<Scalar>( height ) );
		return avgSPP * GetSplatSampleScale();
	}
	if( mHasActiveSplatRegion && width > 0 && height > 0 ) {
		return RegionalSplatSPP( mSplatTotalSamples, width, height, &mActiveSplatRegion );
	}
	return mSplatTotalSamples;
}

void BidirectionalRasterizerBase::SplatContributionToFilm(
	const Scalar fx,
	const Scalar fy,
	const RISEPel& contribution,
	const unsigned int imageWidth,
	const unsigned int imageHeight
	) const
{
	if( !pSplatFilm ) {
		return;
	}

	if( pPixelFilter )
	{
		// Spread through the configured reconstruction kernel.
		// SplatFilm::SplatFiltered short-circuits to a point splat
		// if the filter's half-width is <= 0.501 (box).
		pSplatFilm->SplatFiltered( fx, fy, contribution, *pPixelFilter );
	}
	else
	{
		// No filter — round to nearest pixel, still better than the
		// old truncation which introduced a half-pixel bias.
		unsigned int sx = 0, sy = 0;
		if( SplatFilm::NearestPixel( fx, fy, imageWidth, imageHeight, sx, sy ) ) {
			pSplatFilm->Splat( sx, sy, contribution );
		}
	}
}

IRasterImage& BidirectionalRasterizerBase::GetIntermediateOutputImage(
	IRasterImage& primary
	) const
{
	if( !pSplatFilm && !pFilteredFilm ) {
		return primary;
	}

	const unsigned int w = primary.GetWidth();
	const unsigned int h = primary.GetHeight();

	// Lazy allocation: only pay the scratch cost once we actually need
	// to compose splats or filtered film.
	//
	// L8 round 10 — also reallocate when the dims have CHANGED since
	// the previous render.  Pre-fix the dim-check was missing, so a
	// resize-then-render sequence kept the OLD-dim scratch image
	// alive and the `SetPEL(x, y, ...)` loop wrote out-of-bounds past
	// its `RISEColor[oldW * oldH]` buffer.  At smaller-new-dim that's
	// silent corruption; at larger-new-dim it's a `EXC_BAD_ACCESS`
	// crash on the first row past `oldH` (the user-reported
	// segmentation fault on render-after-resize).  `RISERasterImage`
	// doesn't expose a resize helper, so a discard + re-alloc is the
	// simplest correct fix; the scratch buffer is short-lived
	// (one render) so the per-render alloc cost is acceptable.
	if( pScratchImage &&
	    ( pScratchImage->GetWidth()  != w ||
	      pScratchImage->GetHeight() != h ) )
	{
		safe_release( pScratchImage );
	}
	if( !pScratchImage ) {
		pScratchImage = new RISERasterImage( w, h, RISEColor( 0, 0, 0, 0 ) );
	}

	// Copy the current primary image into the scratch buffer.
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			pScratchImage->SetPEL( x, y, primary.GetPEL( x, y ) );
		}
	}

	// Overlay the filter-reconstructed eye-subpath image (approach C)
	// on top of the per-pixel box accumulation, then add t=1 splats.
	// Order matters: FilteredFilm::Resolve OVERWRITES pixels where
	// weightSum > 0, so it must go first; SplatFilm::Resolve ADDs on
	// top.  Primary is never mutated.
	if( pFilteredFilm ) {
		pFilteredFilm->Resolve( *pScratchImage, ActiveSplatRegion() );
	}
	if( pSplatFilm ) {
		pSplatFilm->Resolve( *pScratchImage, GetEffectiveSplatSPP( w, h ), ActiveSplatRegion() );
	}

	return *pScratchImage;
}

IRasterImage& BidirectionalRasterizerBase::ResolveSplatIntoScratch(
	const IRasterImage& src
	) const
{
	const unsigned int w = src.GetWidth();
	const unsigned int h = src.GetHeight();
	// L8 round 10 — sibling of the dim-check fix in
	// `GetIntermediateOutputImage` above.  Same `pScratchImage`
	// cached pointer, same `SetPEL(x, y, ...)` loop, same
	// out-of-bounds-on-resize bug if the dim-check is missing.
	if( pScratchImage &&
	    ( pScratchImage->GetWidth()  != w ||
	      pScratchImage->GetHeight() != h ) )
	{
		safe_release( pScratchImage );
	}
	if( !pScratchImage ) {
		pScratchImage = new RISERasterImage( w, h, RISEColor( 0, 0, 0, 0 ) );
	}
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			pScratchImage->SetPEL( x, y, src.GetPEL( x, y ) );
		}
	}
	pSplatFilm->Resolve( *pScratchImage, GetEffectiveSplatSPP( w, h ), ActiveSplatRegion() );
	return *pScratchImage;
}


// Flush-time splat composition, shared by VCM and (since DL-458) BDPT.
// Both algorithms create pSplatFilm in PreRenderSetup, which runs once per
// still render AND once per animation frame, so composing the splats here
// (rather than inline in a RasterizeScene override) is what makes an
// animation frame match a still render.
//
// L6d-2a — the pSplatFilm splats resolve DIRECTLY into the
// rasterizer's canonical store (which `img` aliases when the VFS is
// bound, or the persistent internal RISERasterImage otherwise).  This
// fixes the L6f-flagged splat-less-canonical regression for direct
// FrameStore observers (post-L6e-2 bound VFS): once the splat resolve
// has run on `mFrameStore`, the rasterizer-driven `MarkFrameComplete`
// (post-L6f, fired inside `PixelBasedRasterizerHelper::FlushToOutputs`)
// dispatches `OnFrameComplete` to observers reading the splatted
// final.  CLI file outputs were already correct via the legacy
// IRasterizerOutput chain (which received the composited scratch);
// they're now correct AND see the same canonical content the
// FrameStore observers do.
//
// `FlushPreDenoisedToOutputs` (2026-06-10 revision): OIDN's denoise
// pass runs on `mFrameStore` AFTER `FlushPreDenoisedToOutputs`
// returns and BEFORE `FlushDenoisedToOutputs` is called, and OIDN
// must NOT see splats (BDPT's docs/OIDN.md decision: splatted
// accumulation is incompatible with OIDN's per-pixel-independent-
// noise assumption).  The original L6d-2a compromise composited
// splats into a separate scratch image for the legacy
// IRasterizerOutput chain and accepted a splat-less canonical for
// `OnPreDenoiseComplete` observers — but post-L8 the CLI file
// outputs ARE canonical-FrameStore observers (FileEncoderObserver),
// so the plain (non-_denoised) file silently lost ALL t=1
// light-tracing energy whenever denoise was enabled (the default).
// VCM's balance heuristic routes most direct lighting through t=1
// on many scenes, so plain outputs came out several times too dim
// (torus-arealight floor at 0.36x of PT).  The revised flush
// resolves the splat film into the canonical, dispatches the flush
// (legacy chain AND Mark observers see the complete image), then
// `SplatFilm::Unresolve`s the identical per-pixel values so OIDN's
// subsequent input is splat-free again to ~1 ulp.
//
// Mutation safety: `pSplatFilm->Resolve(target, spp)` is ADDITIVE.
// pSplatFilm is reallocated fresh per render in `PreRenderSetup`
// (VCMRasterizerBase::PreRenderSetup, BDPTRasterizerBase::PreRenderSetup), so accumulated splats don't leak
// across renders.  For animation: `PreRenderSetup` runs PER FRAME
// from `RenderFrameOfAnimation`, followed by an inter-frame
// `pImage->Clear()` in `RasterizeSceneAnimation` that
// wipes the splat-mutated buffer before the next frame's per-pixel
// writes start.  Net: per-render Resolve is called exactly once on
// `mFrameStore` (via either FlushTo or FlushDenoised — the
// non-OIDN/OIDN paths are mutually exclusive in
// `PixelBasedRasterizerHelper::RasterizeScene`).  If a future
// refactor of `RenderFrameOfAnimation`'s setup ordering moves the
// PreRenderSetup OUT of the per-frame loop, this invariant breaks
// and splats from frame N would be replayed onto frame N+1.
//
// Bracketing: `FrameStoreBulkBracket` (L6e-1.1 RAII) protects
// concurrent direct readers against torn writes during the
// per-pixel splat add.  The bracket releases BEFORE the rasterizer's
// `FlushTo/Denoised` calls `mFrameStore->MarkFrameComplete`
// (post-L6f), so async observers that wake on the Mark see the
// splatted final without holding the bracket — no observer-side
// deadlock risk.
//
// Behavioural drift: `GetLastRenderedImage()` (read-back of the
// rasterizer's persistent buffer) now returns content WITH splats
// post-render in non-bound mode (was splat-less pre-fix).  No
// in-tree consumer of `GetLastRenderedImage` for VCM today; flagged
// here for the next reader who adds one.

void BidirectionalRasterizerBase::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushToOutputs( img, rcRegion, frame );
		return;
	}
	// L6d-2a — splat-resolve into `img` directly.  `img` is the
	// rasterizer's `*pImage` from `RasterizeScene`'s
	// `AcquireRenderImage`; in bound mode it IS the FrameStore beauty
	// view.  Const-cast is honest: the const here is a parameter
	// declaration — the underlying buffer is the rasterizer's mutable
	// canonical state.
	IRasterImage& target = const_cast<IRasterImage&>( img );
	// Skip splat overlay when show_adaptive_map is on — the
	// authoritative output is the heatmap from the progressive
	// resolve.  See PixelBasedRasterizerHelper.h GetAdaptiveShowMap.
	if( !GetAdaptiveShowMap() ) {
		const Scalar splatSpp = GetEffectiveSplatSPP( target.GetWidth(), target.GetHeight() );
		FrameStoreBulkBracket bracket( mFrameStore, target );
		pSplatFilm->Resolve( target, splatSpp, ActiveSplatRegion() );
	}
	PixelBasedRasterizerHelper::FlushToOutputs( target, rcRegion, frame );
}

void BidirectionalRasterizerBase::FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushPreDenoisedToOutputs( img, rcRegion, frame );
		return;
	}
	// Resolve splats into the canonical for the DURATION of the flush,
	// then subtract them back out so OIDN's denoise input stays
	// splat-free.  The previous scratch-composite path
	// (ResolveSplatIntoScratch) fed only the legacy IRasterizerOutput
	// chain — bound-mode FrameStore observers (FileEncoderObserver,
	// the post-L8 CLI file outputs) read the CANONICAL at
	// MarkPreDenoiseComplete and wrote the plain (non-_denoised) file
	// WITHOUT the t=1 light-tracing strategy's energy.  Under VCM's
	// balance heuristic that strategy carries most direct lighting
	// (e.g. ~63% of floor-direct on a torus-arealight scene — plain
	// file at 0.36x of PT, 2026-06-10).  Observer dispatch is
	// synchronous (FrameStore::DispatchObservers is an in-thread loop
	// and FileEncoderObserver::WriteFile encodes inside the callback),
	// so the Unresolve below cannot race a file write.  Resolve and
	// Unresolve add/subtract the bitwise-identical per-pixel product,
	// so the canonical returns to its pre-flush content to ~1 ulp.
	IRasterImage& target = const_cast<IRasterImage&>( img );
	const Scalar splatSpp = GetEffectiveSplatSPP( target.GetWidth(), target.GetHeight() );
	{
		FrameStoreBulkBracket bracket( mFrameStore, target );
		pSplatFilm->Resolve( target, splatSpp, ActiveSplatRegion() );
	}
	PixelBasedRasterizerHelper::FlushPreDenoisedToOutputs( target, rcRegion, frame );
	{
		FrameStoreBulkBracket bracket( mFrameStore, target );
		pSplatFilm->Unresolve( target, splatSpp, ActiveSplatRegion() );
	}
}

void BidirectionalRasterizerBase::FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	// BDPT flow (which we mirror): the incoming image holds only the
	// DENOISED non-splat contributions; we must add the splat film on
	// top before writing the final denoised output so it matches what
	// BDPT produces.
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushDenoisedToOutputs( img, rcRegion, frame );
		return;
	}
	// L6d-2a — splat-resolve into `img` directly (same pattern as
	// FlushToOutputs above).  OIDN denoise has already mutated
	// `mFrameStore` to the denoised eye-subpath; we now overlay
	// VCM's t==1 splats ON TOP of the denoised content.
	IRasterImage& target = const_cast<IRasterImage&>( img );
	// Defensive: this branch is unreachable when show_adaptive_map is
	// on (the denoise gate above us short-circuits to FlushToOutputs),
	// but guard anyway to keep the contract symmetric with the other
	// two flush methods.
	if( !GetAdaptiveShowMap() ) {
		const Scalar splatSpp = GetEffectiveSplatSPP( target.GetWidth(), target.GetHeight() );
		FrameStoreBulkBracket bracket( mFrameStore, target );
		pSplatFilm->Resolve( target, splatSpp, ActiveSplatRegion() );
	}
	PixelBasedRasterizerHelper::FlushDenoisedToOutputs( target, rcRegion, frame );
}
