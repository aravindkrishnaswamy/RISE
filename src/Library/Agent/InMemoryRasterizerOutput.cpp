//////////////////////////////////////////////////////////////////////
//
//  InMemoryRasterizerOutput.cpp - see InMemoryRasterizerOutput.h.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "InMemoryRasterizerOutput.h"

#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRasterImageWriter.h"
#include "../RISE_API.h"
#include "../Rendering/DisplayTransform.h"       // DISPLAY_TRANSFORM enum (encode-time tone-curve parity with the CLI PNG path)
#include "../Rendering/DisplayTransformWriter.h" // exposure + tone-curve stage the CLI file-output pipeline applies before sRGB
#include "../Rendering/FrameStore.h"
#include "../Utilities/Color/Color.h"           // eColorSpace_sRGB
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/MemoryBuffer.h"

#include <cmath>   // P3: std::round for the downscale dims rounding rule
#include <limits>

using namespace RISE;
using namespace RISE::Agent;

InMemoryRasterizerOutput::InMemoryRasterizerOutput()
	: mWidth( 0 )
	, mHeight( 0 )
	, mHasImage( false )
	, mExposureEV( 0.0 )
	, mDisplayTransform( eDisplayTransform_None )   // identity until SetDisplayTransform is called
	, mColorSpace( eColorSpace_sRGB )               // matches this sink's pre-fix hardcoded default until SetOutputColorSpace is called
	, mFrameStore( 0 )
{
}

InMemoryRasterizerOutput::~InMemoryRasterizerOutput()
{
	safe_release( mFrameStore );
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
	CapturePerception_();
}

void InMemoryRasterizerOutput::OnRasterizerFrameStoreChanged(
	Implementation::FrameStore* framestore )
{
	if( framestore ) framestore->addref();
	safe_release( mFrameStore );
	mFrameStore = framestore;
}

void InMemoryRasterizerOutput::CapturePerception_()
{
	// A new completed image replaces the entire observation.  Release the
	// prior compact planes when this render did not request compatible AOVs.
	auto clearSidecar = [this]() {
		std::vector<unsigned char>().swap( mPerceptionAlbedo );
		std::vector<unsigned char>().swap( mPerceptionNormal );
		std::vector<unsigned char>().swap( mPerceptionDepth );
		mPerceptionInfo = PerceptionInfo();
	};
	clearSidecar();
	if( !mFrameStore || !mHasImage || mFrameStore->Width() != mWidth || mFrameStore->Height() != mHeight ) return;

	using namespace RISE::FrameStoreOutput;
	const Channel<RISEPel>* albedo = mFrameStore->GetChannel<ChannelId::Albedo>();
	const Channel<Vector3>* normal = mFrameStore->GetChannel<ChannelId::Normal>();
	const Channel<float>* depth = mFrameStore->GetChannel<ChannelId::Depth>();
	if( !albedo || !normal || !depth ) return;

	const size_t pixels = static_cast<size_t>( mWidth ) * mHeight;
	double depthMin = std::numeric_limits<double>::max();
	double depthMax = 0.0;
	unsigned int validDepth = 0;
	for( size_t i = 0; i < pixels; ++i ) {
		const double d = static_cast<double>( depth->Data()[i] );
		if( RISE::IsFiniteDouble( d ) && d > 0.0 ) {
			if( d < depthMin ) depthMin = d;
			if( d > depthMax ) depthMax = d;
			++validDepth;
		}
	}
	if( validDepth == 0 ) depthMin = depthMax = 0.0;

	mPerceptionAlbedo.resize( pixels * 3 );
	mPerceptionNormal.resize( pixels * 3 );
	mPerceptionDepth.resize( pixels );
	const double logMin = depthMin > 0.0 ? std::log( depthMin ) : 0.0;
	const double logMax = depthMax > depthMin ? std::log( depthMax ) : logMin;
	const double invLogRange = logMax > logMin ? 1.0 / ( logMax - logMin ) : 0.0;
	auto displayByte = []( double linear ) -> unsigned char {
		if( !RISE::IsFiniteDouble( linear ) || linear <= 0.0 ) return 0;
		if( linear >= 1.0 ) return 255;
		double encoded = ColorUtils::SRGBTransferFunction( linear );
		if( encoded < 0.0 ) encoded = 0.0;
		if( encoded > 1.0 ) encoded = 1.0;
		return static_cast<unsigned char>( encoded * 255.0 + 0.5 );
	};
	for( size_t i = 0; i < pixels; ++i ) {
		const RISEPel& a = albedo->Data()[i];
		mPerceptionAlbedo[i * 3 + 0] = displayByte( a.r );
		mPerceptionAlbedo[i * 3 + 1] = displayByte( a.g );
		mPerceptionAlbedo[i * 3 + 2] = displayByte( a.b );

		const double d = static_cast<double>( depth->Data()[i] );
		const bool hit = RISE::IsFiniteDouble( d ) && d > 0.0;
		if( hit ) {
			const Vector3& n = normal->Data()[i];
			mPerceptionNormal[i * 3 + 0] = displayByte( n.x * 0.5 + 0.5 );
			mPerceptionNormal[i * 3 + 1] = displayByte( n.y * 0.5 + 0.5 );
			mPerceptionNormal[i * 3 + 2] = displayByte( n.z * 0.5 + 0.5 );
			double t = invLogRange > 0.0 ? ( std::log( d ) - logMin ) * invLogRange : 0.0;
			if( t < 0.0 ) t = 0.0;
			if( t > 1.0 ) t = 1.0;
			mPerceptionDepth[i] = static_cast<unsigned char>( ( 1.0 - t ) * 255.0 + 0.5 );
		} else {
			mPerceptionNormal[i * 3 + 0] = 0;
			mPerceptionNormal[i * 3 + 1] = 0;
			mPerceptionNormal[i * 3 + 2] = 0;
			mPerceptionDepth[i] = 0;
		}
	}

	mPerceptionInfo.available = true;
	mPerceptionInfo.sourceWidth = mWidth;
	mPerceptionInfo.sourceHeight = mHeight;
	mPerceptionInfo.validDepthPixels = validDepth;
	mPerceptionInfo.depthMin = depthMin;
	mPerceptionInfo.depthMax = depthMax;
	mPerceptionInfo.persistentBytes =
		static_cast<std::uint64_t>( mPerceptionAlbedo.size()
			+ mPerceptionNormal.size() + mPerceptionDepth.size() );
	// Planned capture owns 28 B/pixel in the float tap, 52 B/pixel in
	// FrameStore's typed AOV channels, and the compact retained planes above.
	mPerceptionInfo.auxiliaryPeakBytes = mPerceptionInfo.persistentBytes
		+ static_cast<std::uint64_t>( pixels ) * 80u;
}

void InMemoryRasterizerOutput::AdoptCoherentSnapshot(
	std::vector<RISEColor>&& pixels, unsigned int width, unsigned int height )
{
	// The caller already did the coherent, tile-locked copy -- adopt the
	// buffer wholesale (no per-pixel GetPEL loop, no re-read).  Move so the
	// caller's buffer is emptied, not copied.
	mPixels = std::move( pixels );
	mWidth  = width;
	mHeight = height;
	// Defensive: guarantee mPixels holds exactly width*height (the encode
	// paths index by y*mWidth+x).  When the caller honoured the contract
	// this is a no-op; a short buffer pads with default-black rather than
	// letting ToPng index past the end.
	mPixels.resize( static_cast<std::size_t>( mWidth ) * mHeight );
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

bool InMemoryRasterizerOutput::GetPixelColor( unsigned int x, unsigned int y, RISEColor& out ) const
{
	if( !mHasImage || x >= mWidth || y >= mHeight ) return false;
	out = mPixels[ static_cast<std::size_t>( y ) * mWidth + x ];
	return true;
}

void InMemoryRasterizerOutput::SetDisplayTransform( double exposureEV, int displayTransform )
{
	mExposureEV = exposureEV;
	// Clamp to the known DISPLAY_TRANSFORM range [none .. hable]; an unknown
	// value degrades to identity (none) rather than indexing DisplayTransforms::
	// Apply's switch default (which is ACES) on a caller typo.
	mDisplayTransform =
		( displayTransform >= eDisplayTransform_None && displayTransform <= eDisplayTransform_Hable )
		? displayTransform : eDisplayTransform_None;
}

void InMemoryRasterizerOutput::SetOutputColorSpace( int colorSpace )
{
	// Clamp to the known COLOR_SPACE range [sRGB .. ProPhotoRGB]; an
	// unknown value degrades to sRGB (this sink's original hardcoded
	// behaviour) rather than indexing PNGWriter's COLOR_SPACE switch with
	// a caller typo -- same defensive shape as SetDisplayTransform above.
	mColorSpace =
		( colorSpace >= eColorSpace_sRGB && colorSpace <= eColorSpace_ProPhotoRGB )
		? colorSpace : eColorSpace_sRGB;
}

namespace
{
	//! Shared PNG-encode tail for ToPng / ToPngDownscaled: write `w`x`h`
	//! pixels from `pels` (row-major) through the tree's PNGWriter (the
	//! REQUESTED `colorSpace`'s Integerize + libpng) into an in-memory
	//! buffer.  Returns an empty vector on a writer-creation failure or
	//! zero dims.
	//!
	//! When `displayTransform != eDisplayTransform_None` OR `exposureEV != 0`,
	//! the PNGWriter is wrapped in the SAME `DisplayTransformWriter` the CLI
	//! `file_rasterizeroutput` LDR pipeline uses (FrameEncoders.cpp gate) --
	//! exposure multiply by 2^EV then the selected tone curve, applied to the
	//! LINEAR pels BEFORE the inner PNGWriter's colour-space Integerize.  This
	//! is what makes an agent beauty render byte-identical to a CLI PNG render
	//! of the same scene; the identity default (0 EV, curve none, sRGB) reproduces
	//! the raw linear->sRGB bytes this sink emitted before the display-transform
	//! stage was added (and is what the objectmap identity sink relies on).
	//!
	//! External review P2 fix: `colorSpace` (a `COLOR_SPACE` enum value) was
	//! previously hardcoded to `eColorSpace_sRGB` here regardless of what the
	//! scene's `file_rasterizeroutput` declared -- now threaded through from
	//! InMemoryRasterizerOutput::mColorSpace (see SetOutputColorSpace).
	std::vector<unsigned char> EncodePng( const std::vector<RISEColor>& pels,
	                                      unsigned int w, unsigned int h,
	                                      double exposureEV, int displayTransform,
	                                      int colorSpace )
	{
		std::vector<unsigned char> out;
		if( w == 0 || h == 0 ) return out;

		// `new` yields refcount 1 (Reference starts at 1) -- that is our
		// owning ref; PNGWriter addrefs it internally.  Do NOT add an extra
		// addref here or the buffer leaks.
		Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer();

		IRasterImageWriter* writer = nullptr;
		if( !RISE_API_CreatePNGWriter( &writer, *buffer, /*bpp=*/8, static_cast<COLOR_SPACE>( colorSpace ) ) || !writer ) {
			safe_release( buffer );
			return out;
		}

		// Interpose the exposure + tone-curve stage when non-identity, exactly
		// mirroring the LDR-only gate in FrameEncoders.cpp (tone curve != none
		// OR total EV != 0).  The wrapper addref's `writer`; we keep our own
		// owning ref on `writer` and release both at the end.
		const DISPLAY_TRANSFORM dt = static_cast<DISPLAY_TRANSFORM>( displayTransform );
		IRasterImageWriter* effective = writer;
		Implementation::DisplayTransformWriter* dtw = nullptr;
		if( dt != eDisplayTransform_None || exposureEV != 0.0 ) {
			dtw = new Implementation::DisplayTransformWriter(
				*writer, static_cast<Scalar>( exposureEV ), dt );
			effective = dtw;
		}

		effective->BeginWrite( w, h );
		for( unsigned int y = 0; y < h; ++y ) {
			for( unsigned int x = 0; x < w; ++x ) {
				effective->WriteColor( pels[ static_cast<std::size_t>( y ) * w + x ], x, y );
			}
		}
		effective->EndWrite();   // flushes the encoded PNG bytes into `buffer`
		safe_release( dtw );      // no-op when identity (dtw stayed null)

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
	return EncodePng( mPixels, mWidth, mHeight, mExposureEV, mDisplayTransform, mColorSpace );
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
		return EncodePng( mPixels, mWidth, mHeight, mExposureEV, mDisplayTransform, mColorSpace );
	}

	// Aspect-preserving box-filter downscale.  Compute the new dims from
	// the SAME scale factor + the SAME rounding rule for both axes --
	// P3 fix: the prior code truncated each axis independently
	// (`static_cast<unsigned int>` on a positive double floors), which lets
	// the two axes round in different directions (e.g. one axis's
	// fractional part is 0.49 and floors down, the other's is 0.51 and
	// would have rounded up) and drift the encoded aspect ratio away from
	// the source's.  std::round applied identically to both axes keeps the
	// nearest-integer error symmetric; min-1-clamp still guards a
	// degenerate 1-pixel-tall/wide source from dividing by zero below.
	const double scale = static_cast<double>( maxEdge ) / static_cast<double>( longEdge );
	unsigned int newW = static_cast<unsigned int>( std::round( scale * mWidth ) );
	unsigned int newH = static_cast<unsigned int>( std::round( scale * mHeight ) );
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
	return EncodePng( down, newW, newH, mExposureEV, mDisplayTransform, mColorSpace );
}

std::vector<unsigned char> InMemoryRasterizerOutput::ToPerceptionPng(
	unsigned int maxEdge,
	unsigned int& outWidth,
	unsigned int& outHeight,
	PerceptionInfo& outInfo ) const
{
	outWidth = outHeight = 0;
	outInfo = mPerceptionInfo;
	std::vector<unsigned char> out;
	if( !mPerceptionInfo.available || !mHasImage || mWidth == 0 || mHeight == 0 ) return out;

	const unsigned int nativeLong = 2u * ( mWidth >= mHeight ? mWidth : mHeight );
	double scale = 1.0;
	if( maxEdge > 0 && maxEdge < nativeLong ) {
		scale = static_cast<double>( maxEdge ) / static_cast<double>( nativeLong );
	}
	unsigned int panelW = static_cast<unsigned int>( std::round( scale * mWidth ) );
	unsigned int panelH = static_cast<unsigned int>( std::round( scale * mHeight ) );
	if( panelW < 1 ) panelW = 1;
	if( panelH < 1 ) panelH = 1;
	outWidth = panelW * 2;
	outHeight = panelH * 2;

	Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer();
	IRasterImageWriter* writer = nullptr;
	if( !RISE_API_CreatePNGWriter( &writer, *buffer, 8, eColorSpace_sRGB ) || !writer ) {
		safe_release( buffer );
		outWidth = outHeight = 0;
		return out;
	}

	const Scalar exposureMul = std::pow( Scalar( 2 ), static_cast<Scalar>( mExposureEV ) );
	const DISPLAY_TRANSFORM dt = static_cast<DISPLAY_TRANSFORM>( mDisplayTransform );
	auto decodeByte = []( unsigned char b ) -> double {
		return ColorUtils::SRGBTransferFunctionInverse( static_cast<double>( b ) / 255.0 );
	};

	writer->BeginWrite( outWidth, outHeight );
	for( unsigned int ay = 0; ay < outHeight; ++ay ) {
		const unsigned int panelY = ay >= panelH ? 1u : 0u;
		const unsigned int dy = ay - panelY * panelH;
		const unsigned int sy0 = static_cast<unsigned int>( ( static_cast<double>( dy ) * mHeight ) / panelH );
		unsigned int sy1 = static_cast<unsigned int>( ( static_cast<double>( dy + 1 ) * mHeight ) / panelH );
		if( sy1 <= sy0 ) sy1 = sy0 + 1;
		if( sy1 > mHeight ) sy1 = mHeight;

		for( unsigned int ax = 0; ax < outWidth; ++ax ) {
			const unsigned int panelX = ax >= panelW ? 1u : 0u;
			const unsigned int dx = ax - panelX * panelW;
			const unsigned int sx0 = static_cast<unsigned int>( ( static_cast<double>( dx ) * mWidth ) / panelW );
			unsigned int sx1 = static_cast<unsigned int>( ( static_cast<double>( dx + 1 ) * mWidth ) / panelW );
			if( sx1 <= sx0 ) sx1 = sx0 + 1;
			if( sx1 > mWidth ) sx1 = mWidth;

			double r = 0.0, g = 0.0, b = 0.0;
			unsigned int count = 0;
			const unsigned int panel = panelY * 2 + panelX;
			for( unsigned int sy = sy0; sy < sy1; ++sy ) {
				for( unsigned int sx = sx0; sx < sx1; ++sx ) {
					const size_t i = static_cast<size_t>( sy ) * mWidth + sx;
					if( panel == 0 ) {
						const RISEPel& c = mPixels[i].base;
						r += c.r; g += c.g; b += c.b;
					} else if( panel == 1 ) {
						r += decodeByte( mPerceptionAlbedo[i * 3 + 0] );
						g += decodeByte( mPerceptionAlbedo[i * 3 + 1] );
						b += decodeByte( mPerceptionAlbedo[i * 3 + 2] );
					} else if( panel == 2 ) {
						r += decodeByte( mPerceptionNormal[i * 3 + 0] );
						g += decodeByte( mPerceptionNormal[i * 3 + 1] );
						b += decodeByte( mPerceptionNormal[i * 3 + 2] );
					} else {
						const double d = decodeByte( mPerceptionDepth[i] );
						r += d; g += d; b += d;
					}
					++count;
				}
			}
			if( count > 0 ) {
				const double inv = 1.0 / static_cast<double>( count );
				r *= inv; g *= inv; b *= inv;
			}
			RISEPel mapped( r, g, b );
			if( panel == 0 ) {
				mapped.r *= exposureMul;
				mapped.g *= exposureMul;
				mapped.b *= exposureMul;
				mapped = DisplayTransforms::Apply( dt, mapped );
			}
			writer->WriteColor( RISEColor( mapped, 1.0 ), ax, ay );
		}
	}
	writer->EndWrite();

	const unsigned int nBytes = buffer->getCurPos();
	const char* bytes = buffer->Pointer();
	if( bytes && nBytes > 0 ) {
		out.assign( reinterpret_cast<const unsigned char*>( bytes ),
			reinterpret_cast<const unsigned char*>( bytes ) + nBytes );
	}
	safe_release( writer );
	safe_release( buffer );
	return out;
}
