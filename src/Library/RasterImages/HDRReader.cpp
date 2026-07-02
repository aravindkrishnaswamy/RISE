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
#include <stdio.h>					// for sscanf
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

	// Next, skip past comments until we reach the portion that tells us the dimensions of the image
	char buf[1024] = {0};
	unsigned int numRead = 0;
	while( (numRead = pReadBuffer.getLine( buf, 1024 )) ) {
		// Check to see if this line contains the format string
		if( strncmp( buf, "FORMAT", 6 ) == 0 ) {
			break;
		}
	}

	if( numRead == 0 ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Something went wrong looking for FORMAT string" );
		return false;
	}

	// Check if the format string is ok
	const char * szFormat = "FORMAT=32-bit_rle_rgbe";
	if( strncmp( buf, szFormat, strlen(szFormat) ) != 0 ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: The FORMAT is NOT 32-bit_rle_rgbe" );
	}

	// Now look for the -Y or +Y
	while( (numRead = pReadBuffer.getLine( buf, 1024 )) ) {
		// Check to see if this line contains the format string
		if( strncmp( buf, "-Y", 2 ) == 0 ||
			strncmp( buf, "+Y", 2 ) == 0 ) {
			break;
		}
	}

	if( numRead == 0 ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Something went wrong looking for image dimensions" );
		return false;
	}

	bool bFlippedY = false;
	if( strncmp( buf, "+Y", 2 ) == 0 ) {
		bFlippedY = true;
	}

	// Find the X
	bool bFlippedX = false;
	{
		int cnt=0;
		while( buf[cnt] != 'X' ) {
			cnt++;
		}

		if( buf[cnt-1] == '-' ) {
			// Flip the X
			bFlippedX = true;
		}
	}

	// Read the dimensions
	char dummy[32] = {0};
	sscanf( buf, "%s %u %s %u", dummy, &height, dummy, &width );

	if( !width || !height ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Something is wrong with image dimensions" );
		return false;
	}

	// The -X (horizontally flipped) scanline order walks the write cursor
	// backwards off the row (start is at the row's left edge with step -1);
	// this decoder only supports the standard +X order.  Reject -X up front
	// with a clear message -- the pre-fix code decoded it into out-of-bounds
	// indices, and the bounds guards below would otherwise reject it with a
	// misleading "malformed data" error.
	if( bFlippedX ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: -X (horizontally flipped) scanline order is not supported" );
		return false;
	}

	// Guard the destination-buffer sizing against a header-driven integer
	// overflow.  width and height come from an unbounded sscanf %u, so the
	// historical 32-bit product `width*height*4` can wrap to a tiny
	// allocation that the RLE decode loop below then writes past (heap OOB
	// write).  Compute the size in 64-bit and cap at a sane ceiling.
	static const unsigned long long kMaxImageBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB ceiling
	const unsigned long long nBytes64 = (unsigned long long)width * (unsigned long long)height * 4ULL;
	if( nBytes64 > kMaxImageBytes ) {
		GlobalLog()->Print( eLog_Error, "HDRReader: Image dimensions too large" );
		return false;
	}
	const size_t nBytes = (size_t)nBytes64;

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
		int step = bFlippedX ? -1 : 1;

		// Start by reading the first four bytes of every scanline
		pReadBuffer.getBytes( col, 4 );

		// This is Radiance's new RLE scheme
		// Header of 0x2, 0x2 is RLE encoding
		if( col[0] == 2 && col[1] == 2 && col[2] >= 0 ) {

			// Each component is run length encoded seperately
			// This will naturally lead to better compression
			for( char component = 0; component < 4; component++ ) {
				unsigned int x = 0;
				int pos = start;

				// Keep going until the end of the scanline
				while( x < width ){

					// Check to see if we have a run
					unsigned char num = pReadBuffer.getUChar();
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
						value = pReadBuffer.getUChar();
					}

					// A zero-length code makes no forward progress; on a
					// truncated / crafted file getUChar() returns 0 forever,
					// so treat it as malformed rather than spinning.
					if( runLen == 0 ) {
						return RejectMalformed();
					}

					// The run must not exceed the pixels remaining in this
					// scanline (keeps pos within [start, start+width)).
					if( x + runLen > width ) {
						return RejectMalformed();
					}

					for( unsigned int i=0; i<runLen; i++ ) {
						const long long idx = (long long)component + (long long)pos*4;
						if( idx < 0 || idx >= nBytesSigned ) {
							return RejectMalformed();
						}
						pBuffer[idx] = bRun ? value : pReadBuffer.getUChar();
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
				if( x > 0 ){
					pReadBuffer.getBytes( col, 4 );
				}

				// Check for the RLE header for this scanline
				if( col[0] == 1 && col[1] == 1 && col[2] == 1 ) {

					// Do the run

					int num = ((int) col[3])&0xFF;

					// A count of zero would underflow the `x += num-1`
					// accounting below (x is unsigned) and repeats nothing;
					// treat it as malformed.
					if( num == 0 ) {
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
