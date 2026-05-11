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
	stabilityConfig( stabilityCfg )
{
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

Scalar BidirectionalRasterizerBase::GetEffectiveSplatSPP(
	unsigned int width,
	unsigned int height
	) const
{
	const uint64_t totalSamples = mTotalAdaptiveSamples.load( std::memory_order_relaxed );
	if( totalSamples > 0 && width > 0 && height > 0 ) {
		const Scalar avgSPP =
			static_cast<Scalar>( totalSamples ) /
			static_cast<Scalar>( width * height );
		return avgSPP * GetSplatSampleScale();
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
		const Scalar rx = fx + Scalar( 0.5 );
		const Scalar ry = fy + Scalar( 0.5 );
		if( rx < 0 || ry < 0 ) return;
		const unsigned int sx = static_cast<unsigned int>( rx );
		const unsigned int sy = static_cast<unsigned int>( ry );
		if( sx < imageWidth && sy < imageHeight ) {
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
		pFilteredFilm->Resolve( *pScratchImage );
	}
	if( pSplatFilm ) {
		pSplatFilm->Resolve( *pScratchImage, GetEffectiveSplatSPP( w, h ) );
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
	pSplatFilm->Resolve( *pScratchImage, GetEffectiveSplatSPP( w, h ) );
	return *pScratchImage;
}
