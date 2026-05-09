//////////////////////////////////////////////////////////////////////
//
//  FrameSink.cpp - Implementation of the IRasterizerOutput →
//  FrameStore adapter.
//
//  The pixel-copy path uses FrameStore::CopyTileFromRasterImage
//  for each affected tile so the per-tile shared_mutex is held
//  during the write — keeping the data-race-free contract from
//  L1.  After the copy, the appropriate Mark* method on the
//  FrameStore fires the matching observer callback.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FrameSink.h"
#include "FrameStore.h"
#include "../Interfaces/IRasterImage.h"

#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

FrameSink::FrameSink( FrameStore* store )
	: store_( store )
{
	if ( store_ ) store_->addref();
}

FrameSink::~FrameSink()
{
	if ( store_ ) store_->release();
}

// ─── IRasterizerOutput methods ────────────────────────────────────

void FrameSink::OutputIntermediateImage(
	const IRasterImage& pImage,
	const Rect*         pRegion )
{
	// Mirrors the legacy FileRasterizerOutput::OutputIntermediateImage
	// no-op (FileRasterizerOutput.h:8 "Region updates are not
	// supported by the file rasterizer output").  GUI viewport sinks
	// will subclass FrameSink (or compose differently) when they
	// want partial-tile updates routed to OnTileComplete observers.
	(void)pImage;
	(void)pRegion;
}

void FrameSink::OutputImage(
	const IRasterImage& pImage,
	const Rect*         pRegion,
	const unsigned int  frame )
{
	if ( !store_ ) return;
	CopyImageIntoStore( pImage, pRegion );
	store_->MarkFrameComplete( frame );
}

void FrameSink::OutputPreDenoisedImage(
	const IRasterImage& pImage,
	const Rect*         pRegion,
	const unsigned int  frame )
{
	if ( !store_ ) return;
	CopyImageIntoStore( pImage, pRegion );
	store_->MarkPreDenoiseComplete( frame );
}

void FrameSink::OutputDenoisedImage(
	const IRasterImage& pImage,
	const Rect*         pRegion,
	const unsigned int  frame )
{
	if ( !store_ ) return;
	CopyImageIntoStore( pImage, pRegion );
	store_->MarkDenoiseComplete( frame );
}

void FrameSink::SetCameraExposureCompensationEV( Scalar ev )
{
	if ( !store_ ) return;
	// Update FrameStore metadata so encoders can read this on
	// the next OnFrameComplete dispatch.  This matches the legacy
	// FileRasterizerOutput::SetCameraExposureCompensationEV path
	// where the per-frame EV from the camera is stacked with any
	// scene-declared exposure_compensation at write time.
	store_->MutableMeta().cameraExposureEV = static_cast<double>( ev );
}

// ─── CopyImageIntoStore ───────────────────────────────────────────
void FrameSink::CopyImageIntoStore(
	const IRasterImage& src,
	const Rect*         /*region*/ )
{
	// Determine the affected pixel range.  The `region` parameter
	// is ignored for the Phase-1 file-output path: legacy
	// FileRasterizerOutput's OutputImage also ignores it (writes
	// the full image).  GUI sinks that want partial-region
	// efficiency will subclass.
	const unsigned int srcW = src.GetWidth();
	const unsigned int srcH = src.GetHeight();
	const size_t       te   = store_->TileEdge();
	const size_t       tcX  = store_->TileCountX();
	const size_t       tcY  = store_->TileCountY();

	// Iterate every tile of the FrameStore that overlaps [0, srcW) ×
	// [0, srcH).  CopyTileFromRasterImage takes a srcRect that
	// describes "which part of `src` to read for this tile"; we
	// pass identity-mapped coords so source(x, y) → dest(x, y).
	const size_t ty1 = std::min<size_t>( tcY,
		( static_cast<size_t>( srcH ) + te - 1 ) / te );
	const size_t tx1 = std::min<size_t>( tcX,
		( static_cast<size_t>( srcW ) + te - 1 ) / te );

	for ( size_t ty = 0; ty < ty1; ++ty ) {
		for ( size_t tx = 0; tx < tx1; ++tx ) {
			const unsigned int dstX0 = static_cast<unsigned int>( tx * te );
			const unsigned int dstY0 = static_cast<unsigned int>( ty * te );
			const unsigned int dstX1 = static_cast<unsigned int>(
				std::min( ( tx + 1 ) * te,
				          static_cast<size_t>( store_->Width() ) ) );
			const unsigned int dstY1 = static_cast<unsigned int>(
				std::min( ( ty + 1 ) * te,
				          static_cast<size_t>( store_->Height() ) ) );
			const Rect srcRect( dstY0, dstX0, dstY1, dstX1 );
			store_->CopyTileFromRasterImage( tx, ty, src, srcRect );
		}
	}
}
