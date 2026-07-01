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

std::vector<unsigned char> InMemoryRasterizerOutput::ToPng() const
{
	std::vector<unsigned char> out;
	if( !mHasImage || mWidth == 0 || mHeight == 0 ) {
		return out;   // nothing captured yet -> empty (documented contract)
	}

	// Reuse the tree's PNGWriter (sRGB Integerize gamma encode + libpng
	// container) targeting an in-memory MemoryBuffer instead of a file.
	// We do NOT hand-roll gamma or the PNG bytes: 8-bit sRGB matches what
	// a `png_painter` / file PNG output produces.
	// `new` yields refcount 1 (Reference starts at 1) -- that is our owning
	// ref; PNGWriter addrefs it internally.  Do NOT add an extra addref here
	// or the buffer leaks.
	Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer();

	IRasterImageWriter* writer = nullptr;
	if( !RISE_API_CreatePNGWriter( &writer, *buffer, /*bpp=*/8, eColorSpace_sRGB ) || !writer ) {
		safe_release( buffer );
		return out;
	}

	writer->BeginWrite( mWidth, mHeight );
	for( unsigned int y = 0; y < mHeight; ++y ) {
		for( unsigned int x = 0; x < mWidth; ++x ) {
			writer->WriteColor( mPixels[ static_cast<std::size_t>( y ) * mWidth + x ], x, y );
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
