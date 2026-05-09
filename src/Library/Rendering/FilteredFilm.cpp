//////////////////////////////////////////////////////////////////////
//
//  FilteredFilm.cpp - Implementation of the FilteredFilm class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FilteredFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

FilteredFilm::FilteredFilm(
	const unsigned int w,
	const unsigned int h
	) :
width( w ),
height( h ),
pixels( w * h )
{
	rowMutexes.resize( height );
	for( unsigned int i=0; i<height; i++ ) {
		rowMutexes[i] = new RMutex();
	}
}

FilteredFilm::~FilteredFilm()
{
	for( unsigned int i=0; i<rowMutexes.size(); i++ ) {
		delete rowMutexes[i];
		rowMutexes[i] = 0;
	}
}

void FilteredFilm::Splat(
	const Scalar screenX,
	const Scalar screenY,
	const XYZPel& color,
	const IPixelFilter& filter
	)
{
	Scalar halfW, halfH;
	filter.GetFilterSupport( halfW, halfH );

	// Compute the range of pixels affected by this sample
	const int minPX = static_cast<int>( floor(screenX - halfW) );
	const int maxPX = static_cast<int>( floor(screenX + halfW) );
	const int minPY = static_cast<int>( floor(screenY - halfH) );
	const int maxPY = static_cast<int>( floor(screenY + halfH) );

	// Clamp to image bounds
	const int x0 = minPX < 0 ? 0 : minPX;
	const int x1 = maxPX >= static_cast<int>(width) ? static_cast<int>(width) - 1 : maxPX;
	const int y0 = minPY < 0 ? 0 : minPY;
	const int y1 = maxPY >= static_cast<int>(height) ? static_cast<int>(height) - 1 : maxPY;

	// Splat to each affected pixel, locking one row at a time
	for( int py = y0; py <= y1; py++ )
	{
		const Scalar dy = screenY - static_cast<Scalar>(py);

		rowMutexes[py]->lock();

		for( int px = x0; px <= x1; px++ )
		{
			const Scalar dx = screenX - static_cast<Scalar>(px);
			const Scalar w = filter.EvaluateFilter( dx, dy );

			if( w != 0.0 )
			{
				FilteredPixel& pixel = pixels[py * width + px];
				pixel.colorSum = pixel.colorSum + color * w;
				pixel.weightSum += w;
			}
		}

		rowMutexes[py]->unlock();
	}
}

void FilteredFilm::Resolve(
	IRasterImage& target
	) const
{
	// XYZ -> ROMM RGB conversion happens here, exactly once per pixel.
	// The implicit `RISEPel(XYZPel)` constructor invokes the standard
	// `ColorUtils::XYZtoROMMRGB` (Bradford D65 -> D50 chromatic adapt
	// + gamut clip + matrix).  This matches the JH LUT generator's
	// post-2026-05 forward model (`tools/JakobHanikaLUTGen.cpp::
	// IntegrateToROMM`), so JH-uplifted spectra and physical spectra
	// (BioSpec, blackbody, measured SPDs) both round-trip through the
	// same conversion.  An earlier interim revision dispatched on a
	// `bIntegratorMode` flag to a matrix-only `IntegratorXYZtoROMMRGB`,
	// which broke physically-grounded scenes (BioSpec under blackbody
	// rendered lavender because the unadapted D65 XYZ shoved blue
	// through the `mxXYZD50toROMM` matrix's third row).  Removing the
	// flag and aligning the LUT with the standard pipeline fixes
	// both classes of scene at once.
	for( unsigned int y=0; y<height; y++ ) {
		for( unsigned int x=0; x<width; x++ ) {
			const FilteredPixel& pixel = pixels[y * width + x];

			if( fabs(pixel.weightSum) > 1e-10 ) {
				const XYZPel resolvedXYZ = pixel.colorSum * (1.0 / pixel.weightSum);
				target.SetPEL( x, y, RISEColor( RISEPel( resolvedXYZ ), 1.0 ) );
			}
		}
	}
}

void FilteredFilm::Unresolve(
	IRasterImage& target
	) const
{
	// Inverse of Resolve: subtract the standard-XYZ-resolved ROMM RGB
	// value previously written.  Mirror Resolve's conversion choice
	// so the round-trip cancels exactly.
	for( unsigned int y=0; y<height; y++ ) {
		for( unsigned int x=0; x<width; x++ ) {
			const FilteredPixel& pixel = pixels[y * width + x];

			if( fabs(pixel.weightSum) > 1e-10 ) {
				RISEColor existing = target.GetPEL( x, y );
				const XYZPel resolvedXYZ = pixel.colorSum * (1.0 / pixel.weightSum);
				const RISEPel resolved = RISEPel( resolvedXYZ );
				RISEColor combined( existing.base - resolved, existing.a );
				target.SetPEL( x, y, combined );
			}
		}
	}
}

void FilteredFilm::Clear()
{
	for( unsigned int i=0; i<pixels.size(); i++ ) {
		pixels[i].colorSum = XYZPel( 0, 0, 0 );
		pixels[i].weightSum = 0;
	}
}
