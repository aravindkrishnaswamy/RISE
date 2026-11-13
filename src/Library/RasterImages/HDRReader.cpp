//////////////////////////////////////////////////////////////////////
//
//  HDRReader.cpp - Implementation of the HDRReader class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 17, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <charconv>
#include <sstream>
#include <string_view>
#include <string.h>					// for strlen and strncmp
#include "HDRReader.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

HDRReader::HDRReader( IReadBuffer& buffer ) :
  pReadBuffer( buffer ),
  pBuffer( 0 )
{
	pReadBuffer.addref();
}

HDRReader::~HDRReader( )
{
	pReadBuffer.release();

	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}

bool HDRReader::BeginRead( unsigned int& width, unsigned int& height )
{
	// Verify the RADIANCE signature
	static const char * szSignature = "#?RADIANCE";
	char szSigFromBuf[11] = {0};

	pReadBuffer.seek( IBuffer::START, 0 );
	pReadBuffer.getBytes( szSigFromBuf, static_cast<unsigned int>(strlen(szSignature)) );

	if( strcmp(szSignature, szSigFromBuf) != 0 ) {
		GlobalLog()->PrintSourceError( "HDRReader::BeginRead:: RADIANCE signature not found!", __FILE__, __LINE__ );
		return false;
	}

	// Read bounded prefixes and consume the rest of a long physical line
	// so an ancillary line's continuation cannot become a header field.
	char buf[1024];
	bool longLine = false;
	const auto readHeaderLine = [&]() {
		const int length = pReadBuffer.getLine( buf, sizeof(buf) );
		longLine = length == sizeof(buf) && buf[length-1] != '\n';
		if( longLine ) {
			char discarded[1024];
			int n;
			do {
				n = pReadBuffer.getLine( discarded, sizeof(discarded) );
			} while( n == sizeof(discarded) && discarded[n-1] != '\n' );
		}
		return std::string_view( buf, length > 0 ? static_cast<size_t>( length ) : 0 );
	};
	std::string_view line;
	do {
		line = readHeaderLine();
	} while( !line.empty() && line.substr( 0, 7 ) != "FORMAT=" );

	const auto last = line.find_last_not_of( " \t\r\n" );
	if( longLine || last == std::string_view::npos ||
		line.substr( 0, last + 1 ) != "FORMAT=32-bit_rle_rgbe" ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Missing or unsupported FORMAT" );
		return false;
	}

	do {
		line = readHeaderLine();
	} while( !line.empty() && line.substr( 0, 2 ) != "-Y" && line.substr( 0, 2 ) != "+Y" );

	if( line.empty() || longLine ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Invalid image resolution line" );
		return false;
	}

	std::istringstream resolution{ std::string( line ) };
	std::string yAxis, heightToken, xAxis, widthToken, trailing;
	const auto readDimension = []( const std::string& token, unsigned int& value ) {
		const char* end = token.data() + token.size();
		const auto parsed = std::from_chars( token.data(), end, value );
		return parsed.ec == std::errc{} && parsed.ptr == end && value != 0;
	};
	if( !(resolution >> yAxis >> heightToken >> xAxis >> widthToken) || (resolution >> trailing) ||
		(yAxis != "-Y" && yAxis != "+Y") || (xAxis != "-X" && xAxis != "+X") ||
		!readDimension( heightToken, height ) || !readDimension( widthToken, width ) ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Something is wrong with image dimensions" );
		return false;
	}
	const bool bFlippedY = yAxis == "+Y";

	// The -X (horizontally flipped) scanline order walks the write cursor
	// backwards off the row (start is at the row's left edge with step -1);
	// this decoder only supports the standard +X order.  Reject -X up front
	// with a clear message -- the pre-fix code decoded it into out-of-bounds
	// indices, and the bounds guards below would otherwise reject it with a
	// misleading "malformed data" error.
	if( xAxis == "-X" ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: -X (horizontally flipped) scanline order is not supported" );
		return false;
	}

	// Cap the pixel count before multiplying by the four bytes per pixel.
	static const unsigned long long kMaxImageBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB ceiling
	const auto pixels = static_cast<unsigned long long>( width ) * height;
	if( pixels > kMaxImageBytes / 4 ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Image dimensions too large" );
		return false;
	}
	const size_t nBytes = static_cast<size_t>( pixels * 4 );

	pBuffer = new unsigned char[nBytes];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	memset( pBuffer, 0, nBytes );

	// Every RLE write below is bounds-checked against this signed byte count.
	// The Radiance RLE run lengths are file-controlled and can advance the
	// write cursor arbitrarily; without a clamp a crafted run walks off the
	// buffer (heap OOB write) independent of the allocation-overflow above.
	// On any out-of-range write (or a zero-length run, which makes no forward
	// progress and would spin forever on a truncated file) we reject the
	// whole file rather than corrupt the heap.
	const long long nBytesSigned = (long long)nBytes;
	auto RejectMalformed = [this]() -> bool {
		GlobalLog()->Print( eLog_Error, "HDRReader: Malformed / out-of-bounds RLE scanline data; rejecting file" );
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
		return false;
	};

	// Do the RLE compression stuff
	// Some of the RLE decoding stuff comes from ggLibrary
	char col[4] = {0};
	for( unsigned int y = 0; y < height; y++ ) {
		int start = bFlippedY ? (height-y-1)*width : y*width;
		// step is always +1 here (bFlippedX / -X order was rejected above);
		// the `idx < 0` / `pos-step < 0` guards below are defensive only.
		const int step = 1;

		// Start by reading the first four bytes of every scanline
		if( !pReadBuffer.getBytes( col, 4 ) ) {
			return RejectMalformed();
		}

		// This is Radiance's new RLE scheme
		// Header of 0x2, 0x2 is RLE encoding
		// Radiance's per-channel RLE is defined for this scanline width range.
		if( width >= 8 && width <= 32767 && col[0] == 2 && col[1] == 2 &&
			(static_cast<unsigned char>( col[2] ) & 0x80u) == 0 ) {
			const auto scanlineWidth = (static_cast<unsigned int>( col[2] ) << 8) |
				static_cast<unsigned char>( col[3] );
			if( scanlineWidth != width ) {
				return RejectMalformed();
			}

			// Each component is run length encoded seperately
			// This will naturally lead to better compression
			for( char component = 0; component < 4; component++ ) {
				unsigned int x = 0;
				int pos = start;

				// Keep going until the end of the scanline
				while( x < width ){

					// Check to see if we have a run
					unsigned char num;
					if( !pReadBuffer.getBytes( &num, 1 ) ) {
						return RejectMalformed();
					}
					bool bRun = false;
					unsigned int runLen = 0;
					unsigned char value = 0;
					if( num <= 128 ) {
						// No run, just raw values to read one at a time
						runLen = num;
					} else {
						// A run: read the value once and repeat it
						bRun = true;
						runLen = num - 128;
						if( !pReadBuffer.getBytes( &value, 1 ) ) {
							return RejectMalformed();
						}
					}

					// A zero-length code makes no forward progress.
					if( runLen == 0 ) {
						return RejectMalformed();
					}

					// The run must not exceed the pixels remaining in this
					// scanline (keeps pos within [start, start+width)).
					if( x + runLen > width ) {
						return RejectMalformed();
					}

					unsigned char literals[128];
					if( !bRun && !pReadBuffer.getBytes( literals, runLen ) ) {
						return RejectMalformed();
					}
					for( unsigned int i=0; i<runLen; i++ ) {
						const long long idx = (long long)component + (long long)pos*4;
						if( idx < 0 || idx >= nBytesSigned ) {
							return RejectMalformed();
						}
						pBuffer[idx] = bRun ? value : literals[i];
						pos += step;
					}
					x += runLen;
				}
			}
		} else {
			// This is the old Radiance RLE scheme
			// IIts a bit simpler
			// All it contains is either runs or raw data, runs have their header, which we
			// check for right away.
			int pos = start;
			for( unsigned int x=0; x<width; x++ ) {
				if( x > 0 && !pReadBuffer.getBytes( col, 4 ) ) {
					return RejectMalformed();
				}

				// Check for the RLE header for this scanline
				if( col[0] == 1 && col[1] == 1 && col[2] == 1 ) {

					// Do the run

					int num = ((int) col[3])&0xFF;

					// A count of zero would underflow the `x += num-1`
					// accounting below (x is unsigned) and repeats nothing;
					// treat it as malformed.
					if( num == 0 || x == 0 ) {
						return RejectMalformed();
					}

					// The run must not exceed the pixels remaining in this
					// scanline (parity with the new-RLE `x + runLen > width`
					// clamp).  Without this a long run would corrupt later
					// scanlines in-buffer before the global end-check below
					// stopped it.  x < width here, so `width - x` >= 1.
					if( (unsigned int)num > width - x ) {
						return RejectMalformed();
					}

					// The run repeats the PREVIOUS pixel; bounds-check that
					// back-reference (at the first pixel of a scanline it can
					// point before the buffer on a crafted file).
					const long long prev = (long long)(pos-step)*4;
					if( prev < 0 || prev+3 >= nBytesSigned ) {
						return RejectMalformed();
					}
					unsigned char r = pBuffer[prev];
					unsigned char g = pBuffer[prev+1];
					unsigned char b = pBuffer[prev+2];
					unsigned char e = pBuffer[prev+3];

					for( int i=0; i<num; i++ ) {
						const long long idx = (long long)pos*4;
						if( idx < 0 || idx+3 >= nBytesSigned ) {
							return RejectMalformed();
						}
						pBuffer[idx] = r;
						pBuffer[idx+1] = g;
						pBuffer[idx+2] = b;
						pBuffer[idx+3] = e;
						pos += step;
					}

					x += num-1;
				} else {

					// No runs here, so just store the 4 bytes just read into
					// `col` (the current pixel's RGBE).  NOTE: the pre-fix code
					// wrote `buf` here -- the header/dimension line buffer --
					// so old-RLE raw pixels decoded to stale header text; that
					// was a latent correctness bug, fixed to `col`.
					const long long idx = (long long)pos*4;
					if( idx < 0 || idx+3 >= nBytesSigned ) {
						return RejectMalformed();
					}
					pBuffer[idx] = col[0];
					pBuffer[idx+1] = col[1];
					pBuffer[idx+2] = col[2];
					pBuffer[idx+3] = col[3];
					pos += step;
				}
			}
		}
	}

	bufW = width;

	return true;
}

void HDRReader::ReadColor( RISEColor& c, const unsigned int x, const unsigned int y )
{
	if( pBuffer ) {
		// The HDR format is always Rec709 with non-linearity in e?  Yea that kinda makes sense
		// If we used some other format like ROMM, there wouldn't be enough bits in the r, g & b
		// What we need is 16 bit RGBE to pull this off
		Rec709RGBPel	p;

		const unsigned char RGBEBase = 128;

		const unsigned int idx = y*bufW*4+x*4;

		unsigned char r = pBuffer[idx];
		unsigned char g = pBuffer[idx+1];
		unsigned char b = pBuffer[idx+2];
		unsigned char e = pBuffer[idx+3];

		if( e == 0 ) {
			p = RGB::Black;
		} else {
			double v = ldexp( 1.0, (int) e-(RGBEBase+8) );
			p.r = v * (r + 0.5);
			p.g = v * (g + 0.5);
			p.b = v * (b + 0.5);
		}

		c.base = p;
		c.a = 1.0;			// No alpha channel in RGBE
	}
}

void HDRReader::EndRead( )
{
	if( pBuffer ) {
		GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
		delete [] pBuffer;
		pBuffer = 0;
	}
}
