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
	IRasterImage& target,
	const Rect* region
	) const
{
	// XYZ -> RISEPel conversion happens here, exactly once per pixel.
	// The implicit `RISEPel(XYZPel)` constructor invokes
	// `ColorUtils::XYZtoRec709RGB` (post Stage-B colour-space migration:
	// RISEPel = Rec709RGBPel, no Bradford adapt — Rec.709 is D65 and
	// the integrator's XYZ is D65-referred).
	//
	// The film is the SAME un-adapted XYZ(D65)->Rec709(D65) matrix the JH
	// LUT generator's rec709 target ends with, but it is NOT the
	// generator's whole forward model and must not be: the generator
	// integrates sigmoid x D65 x cmf and divides by INT D65*ybar, because it
	// is defining what a REFLECTANCE means (reflectance under D65).  The
	// film integrates radiance x cmf and divides by INT ybar, with no
	// illuminant weighting at all, because the D65 shape rides in the
	// SOURCES (Stage C: RGBIlluminantSpectrum = sigmoid x D65norm is what
	// every emitter, light and radiance map now emits -- slice 2).  Putting
	// D65 in BOTH places would apply it twice.  The consequence callers
	// rely on: a D65norm-shaped source of unit scale resolves here to
	// exactly (1, 1, 1), while a FLAT unit spectrum resolves to
	// (1.20485, 0.94827, 0.90894) -- equal-energy chromaticity, not D65.
	// Physical spectra (BioSpec, blackbody, measured SPDs) are absolute
	// radiances and come through this same un-weighted integral unchanged.
	// See docs/SPECTRAL_ILLUMINANT_CONVENTION.md.
	//
	// Pre-Stage-B revisions dispatched on a `bIntegratorMode`
	// flag to a matrix-only `IntegratorXYZtoROMMRGB`, which broke
	// physically-grounded scenes — that path was eliminated by the
	// Stage A colour-space migration (`IntegratorXYZto*` no longer
	// exists).
	unsigned int startX = 0, startY = 0, endX = width ? width-1 : 0, endY = height ? height-1 : 0;
	if( width == 0 || height == 0 ) return;
	if( region ) {
		if( region->left > region->right || region->top > region->bottom
			|| region->left >= width || region->top >= height ) return;
		startX = region->left;
		startY = region->top;
		endX = r_min( region->right, width-1 );
		endY = r_min( region->bottom, height-1 );
	}
	for( unsigned int y=startY; y<=endY; y++ ) {
		for( unsigned int x=startX; x<=endX; x++ ) {
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
	// Inverse of Resolve: subtract the previously-written XYZ-resolved
	// RISEPel value.  Mirror Resolve's conversion choice so the round-
	// trip cancels exactly.
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
