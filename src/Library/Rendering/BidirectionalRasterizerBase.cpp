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
#include "../RasterImages/RasterImage.h"

using namespace RISE;
using namespace RISE::Implementation;

BidirectionalRasterizerBase::BidirectionalRasterizerBase(
	IRayCaster* pCaster_,
	const StabilityConfig& stabilityCfg
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
	if( !pSplatFilm ) {
		return primary;
	}

	const unsigned int w = primary.GetWidth();
	const unsigned int h = primary.GetHeight();

	// Lazy allocation: only pay the scratch cost once a splat film
	// actually needs composition.
	if( !pScratchImage ) {
		pScratchImage = new RISERasterImage( w, h, RISEColor( 0, 0, 0, 0 ) );
	}

	// Copy the current primary image into the scratch buffer.
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			pScratchImage->SetPEL( x, y, primary.GetPEL( x, y ) );
		}
	}

	// Resolve splats into the scratch copy (primary is untouched).
	pSplatFilm->Resolve( *pScratchImage, GetEffectiveSplatSPP( w, h ) );

	return *pScratchImage;
}

IRasterImage& BidirectionalRasterizerBase::ResolveSplatIntoScratch(
	const IRasterImage& src
	) const
{
	const unsigned int w = src.GetWidth();
	const unsigned int h = src.GetHeight();
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
