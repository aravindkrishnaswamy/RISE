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
	pReadBuffer.getBytes( szSigFromBuf, strlen(szSignature) );

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

	pBuffer = new unsigned char[width*height*4];
	GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
	memset( pBuffer, 0, width*height*4 );

	// Do the RLE compression stuff
	// Some of the RLE decoding stuff comes from ggLibrary
	char col[4] = {0};
	for( unsigned int y = 0; y < height; y++ ) {
		int start = bFlippedY ? (height-y-1)*width : y*width;
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
					if( num <= 128) {
						// No run, just values, just just read all the values
						for( int i=0; i<num; i++ ) {
							pBuffer[component+pos*4] = pReadBuffer.getUChar();
							pos += step;
						}
					} else {
						// We have a run, so get the value and set all the values
						// for this run
                        unsigned char value = pReadBuffer.getUChar();
                        num -= 128;
                        for( int i=0; i<num; i++ ) {
                            pBuffer[component+pos*4] = value;
                            pos += step;
						}
					}
                    x += num;
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
					unsigned char r = pBuffer[(pos-step)*4];
					unsigned char g = pBuffer[(pos-step)*4+1];
					unsigned char b = pBuffer[(pos-step)*4+2];
					unsigned char e = pBuffer[(pos-step)*4+3];

					for( int i=0; i<num; i++ ) {
                        pBuffer[pos*4] = r;
                        pBuffer[pos*4+1] = g;
                        pBuffer[pos*4+2] = b;
                        pBuffer[pos*4+3] = e;
                        pos += step;
					}

                    x += num-1;
				} else {

					// No runs here, so just read the data thats there
					pBuffer[pos*4] = buf[0];
					pBuffer[pos*4+1] = buf[1];
					pBuffer[pos*4+2] = buf[2];
					pBuffer[pos*4+3] = buf[3];
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
