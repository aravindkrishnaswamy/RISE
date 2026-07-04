//////////////////////////////////////////////////////////////////////
//
//  InMemoryRasterizerOutput.cpp - see InMemoryRasterizerOutput.h.
//
//////////////////////////////////////////////////////////////////////

#include "InMemoryRasterizerOutput.h"

#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRasterImageWriter.h"
#include "../RISE_API.h"
#include "../Utilities/Color/Color.h"           // eColorSpace_sRGB
#include "../Utilities/MemoryBuffer.h"

using namespace RISE;
using namespace RISE::Agent;

InMemoryRasterizerOutput::InMemoryRasterizerOutput()
	: mWidth( 0 )
	, mHeight( 0 )
	, mHasImage( false )
{
}

InMemoryRasterizerOutput::~InMemoryRasterizerOutput()
{
}

void InMemoryRasterizerOutput::OutputIntermediateImage( const IRasterImage&, const Rect* )
{
	// Headless final-image sink: intermediate scanlines are not retained
	// (matches FileRasterizerOutput, which ignores intermediates too).
}

void InMemoryRasterizerOutput::OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int )
{
	// Capture the WHOLE image (region ignored -- file-style semantics).
	// Overwrites any earlier frame so mPixels holds the last full image.
	mWidth  = pImage.GetWidth();
	mHeight = pImage.GetHeight();
	mPixels.resize( static_cast<std::size_t>( mWidth ) * mHeight );
	for( unsigned int y = 0; y < mHeight; ++y ) {
		for( unsigned int x = 0; x < mWidth; ++x ) {
			mPixels[ static_cast<std::size_t>( y ) * mWidth + x ] = pImage.GetPEL( x, y );
		}
	}
	mHasImage = true;
}

void InMemoryRasterizerOutput::MeanChannels( double& r, double& g, double& b ) const
{
	r = g = b = 0.0;
	if( !mHasImage || mPixels.empty() ) {
		return;
	}
	double sr = 0.0, sg = 0.0, sb = 0.0;
	for( const RISEColor& c : mPixels ) {
		sr += static_cast<double>( c.base.r );
		sg += static_cast<double>( c.base.g );
		sb += static_cast<double>( c.base.b );
	}
	const double n = static_cast<double>( mPixels.size() );
	r = sr / n;
	g = sg / n;
	b = sb / n;
}

namespace
{
	//! Shared PNG-encode tail for ToPng / ToPngDownscaled: write `w`x`h`
	//! pixels from `pels` (row-major) through the tree's PNGWriter (sRGB
	//! Integerize + libpng) into an in-memory buffer.  Returns an empty
	//! vector on a writer-creation failure or zero dims.
	std::vector<unsigned char> EncodePng( const std::vector<RISEColor>& pels,
	                                      unsigned int w, unsigned int h )
	{
		std::vector<unsigned char> out;
		if( w == 0 || h == 0 ) return out;

		// `new` yields refcount 1 (Reference starts at 1) -- that is our
		// owning ref; PNGWriter addrefs it internally.  Do NOT add an extra
		// addref here or the buffer leaks.
		Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer();

		IRasterImageWriter* writer = nullptr;
		if( !RISE_API_CreatePNGWriter( &writer, *buffer, /*bpp=*/8, eColorSpace_sRGB ) || !writer ) {
			safe_release( buffer );
			return out;
		}

		writer->BeginWrite( w, h );
		for( unsigned int y = 0; y < h; ++y ) {
			for( unsigned int x = 0; x < w; ++x ) {
				writer->WriteColor( pels[ static_cast<std::size_t>( y ) * w + x ], x, y );
			}
		}
		writer->EndWrite();   // flushes the encoded PNG bytes into `buffer`

		// MemoryBuffer::setBytes grows exactly (Resize to cursor+amount), so
		// the write cursor is the authoritative encoded length; Size() may
		// equal it but the cursor is the safest source of truth.
		const unsigned int nBytes = buffer->getCurPos();
		const char* p = buffer->Pointer();
		if( p && nBytes > 0 ) {
			out.assign(
				reinterpret_cast<const unsigned char*>( p ),
				reinterpret_cast<const unsigned char*>( p ) + nBytes );
		}

		safe_release( writer );
		safe_release( buffer );
		return out;
	}
}

std::vector<unsigned char> InMemoryRasterizerOutput::ToPng() const
{
	if( !mHasImage || mWidth == 0 || mHeight == 0 ) {
		return std::vector<unsigned char>();   // nothing captured yet -> empty (documented contract)
	}
	// We do NOT hand-roll gamma or the PNG bytes: 8-bit sRGB matches what
	// a `png_painter` / file PNG output produces.
	return EncodePng( mPixels, mWidth, mHeight );
}

std::vector<unsigned char> InMemoryRasterizerOutput::ToPngDownscaled(
	unsigned int maxEdge, unsigned int& outWidth, unsigned int& outHeight ) const
{
	outWidth = 0;
	outHeight = 0;
	if( !mHasImage || mWidth == 0 || mHeight == 0 || maxEdge == 0 ) {
		return std::vector<unsigned char>();
	}

	const unsigned int longEdge = mWidth >= mHeight ? mWidth : mHeight;
	if( longEdge <= maxEdge ) {
		// Never upscale -- already within bounds, encode at native size
		// (identical bytes to ToPng()).
		outWidth  = mWidth;
		outHeight = mHeight;
		return EncodePng( mPixels, mWidth, mHeight );
	}

	// Aspect-preserving box-filter downscale.  Compute the new dims from
	// the same scale factor the constraint implies, floor-clamped to 1 so
	// a degenerate 1-pixel-tall/wide source never divides by zero below.
	const double scale = static_cast<double>( maxEdge ) / static_cast<double>( longEdge );
	unsigned int newW = static_cast<unsigned int>( scale * mWidth );
	unsigned int newH = static_cast<unsigned int>( scale * mHeight );
	if( newW < 1 ) newW = 1;
	if( newH < 1 ) newH = 1;

	std::vector<RISEColor> down( static_cast<std::size_t>( newW ) * newH );

	// For each destination pixel, average the axis-aligned source box that
	// maps to it (each source pixel contributes to exactly one destination
	// pixel's box -- a standard non-overlapping box filter, computed in
	// LINEAR space before ToPng's sRGB Integerize, matching MeanChannels'
	// rationale for why linear-space averaging is the physically correct
	// operation here).
	for( unsigned int dy = 0; dy < newH; ++dy ) {
		const unsigned int sy0 = static_cast<unsigned int>( ( static_cast<double>( dy )     * mHeight ) / newH );
		unsigned int sy1 = static_cast<unsigned int>( ( static_cast<double>( dy + 1 ) * mHeight ) / newH );
		if( sy1 <= sy0 ) sy1 = sy0 + 1;
		if( sy1 > mHeight ) sy1 = mHeight;

		for( unsigned int dx = 0; dx < newW; ++dx ) {
			const unsigned int sx0 = static_cast<unsigned int>( ( static_cast<double>( dx )     * mWidth ) / newW );
			unsigned int sx1 = static_cast<unsigned int>( ( static_cast<double>( dx + 1 ) * mWidth ) / newW );
			if( sx1 <= sx0 ) sx1 = sx0 + 1;
			if( sx1 > mWidth ) sx1 = mWidth;

			double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
			unsigned int n = 0;
			for( unsigned int sy = sy0; sy < sy1; ++sy ) {
				for( unsigned int sx = sx0; sx < sx1; ++sx ) {
					const RISEColor& c = mPixels[ static_cast<std::size_t>( sy ) * mWidth + sx ];
					r += static_cast<double>( c.base.r );
					g += static_cast<double>( c.base.g );
					b += static_cast<double>( c.base.b );
					a += static_cast<double>( c.a );
					++n;
				}
			}
			RISEColor& dst = down[ static_cast<std::size_t>( dy ) * newW + dx ];
			if( n > 0 ) {
				dst.base.r = r / n;
				dst.base.g = g / n;
				dst.base.b = b / n;
				dst.a      = a / n;
			}
		}
	}

	outWidth  = newW;
	outHeight = newH;
	return EncodePng( down, newW, newH );
}
