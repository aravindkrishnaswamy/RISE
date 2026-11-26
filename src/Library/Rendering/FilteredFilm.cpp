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
#include "../Interfaces/ICamera.h"
#include "../Utilities/CheckedSize.h"
#include <algorithm>
#include <mutex>

using namespace RISE;
using namespace RISE::Implementation;

std::size_t FilteredFilm::CheckedPixelCount(unsigned int w, unsigned int h)
{
	const auto count = RISE::Implementation::CheckedPixelCount<FilteredPixel>(w, h);
	if( count ) RISE::Implementation::CheckedPixelCount<std::unique_ptr<RMutex>>(h, 1);
	return count;
}

FilteredFilm::FilteredFilm(
	const unsigned int w,
	const unsigned int h
	) :
width( w ),
height( h ),
pixels( CheckedPixelCount(w, h) )
{
	if( pixels.empty() ) return;
	rowMutexes.resize( height );
	for( auto& mutex : rowMutexes ) mutex = std::make_unique<RMutex>();
}

FilteredFilm::~FilteredFilm() = default;

void FilteredFilm::Splat(
	const Scalar screenX,
	const Scalar screenY,
	const XYZPel& color,
	const IPixelFilter& filter,
	const Scalar sampleAlpha
	)
{
	if( width == 0 || height == 0 || !std::isfinite(screenX) || !std::isfinite(screenY) ) return;
	Scalar halfW, halfH;
	filter.GetFilterSupport( halfW, halfH );
	if( !std::isfinite(halfW) || !std::isfinite(halfH) || halfW < 0 || halfH < 0 ) return;

	// Compute the range of pixels affected by this sample.  (cx, cy) is
	// the position in pixel-CENTRE units: pixel (px, py) has its centre
	// at (px + 0.5, py + 0.5) (DL-368, ICamera.h's RasterConvention).
	const Scalar cx = screenX - RasterConvention::kPixelCentre;
	const Scalar cy = screenY - RasterConvention::kPixelCentre;
	// Intersect before narrowing. Off-image samples retain their existing
	// footprint semantics, while huge finite supports never overflow an int.
	const Scalar firstX = std::max(Scalar(0), std::floor(cx - halfW));
	const Scalar lastX = std::min(static_cast<Scalar>(width - 1), std::floor(cx + halfW));
	const Scalar firstY = std::max(Scalar(0), std::floor(cy - halfH));
	const Scalar lastY = std::min(static_cast<Scalar>(height - 1), std::floor(cy + halfH));
	if( firstX > lastX || firstY > lastY ) return;
	const auto x0 = static_cast<unsigned int>(firstX);
	const auto x1 = static_cast<unsigned int>(lastX);
	const auto y0 = static_cast<unsigned int>(firstY);
	const auto y1 = static_cast<unsigned int>(lastY);

	// Splat to each affected pixel, locking one row at a time
	for( unsigned int py = y0; py <= y1; py++ )
	{
		const Scalar dy = cy - static_cast<Scalar>(py);

		std::lock_guard<RMutex> rowLock(*rowMutexes[py]);

		for( unsigned int px = x0; px <= x1; px++ )
		{
			const Scalar dx = cx - static_cast<Scalar>(px);
			const Scalar w = filter.EvaluateFilter( dx, dy );

			if( w != 0.0 )
			{
				FilteredPixel& pixel = pixels[static_cast<std::size_t>(py) * width + px];
				pixel.colorSum = pixel.colorSum + color * w;
				pixel.weightSum += w;
				pixel.alphaSum += sampleAlpha * w;
			}
		}
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
	// Splat() writes a row under rowMutexes[row] while a preview
	// composition may resolve concurrently: snapshot each row's span under
	// that lock, release it, then write the target (no lock held across
	// the target, one row lock at a time).
	std::vector<FilteredPixel> row( static_cast<std::size_t>( endX - startX ) + 1 );
	for( unsigned int y=startY; y<=endY; y++ ) {
		CopyRow( y, startX, row );
		for( unsigned int x=startX; x<=endX; x++ ) {
			const FilteredPixel& pixel = row[x - startX];

			if( fabs(pixel.weightSum) > 1e-10 ) {
				const XYZPel resolvedXYZ = pixel.colorSum * (1.0 / pixel.weightSum);
				if( fabs(pixel.alphaSum - pixel.weightSum) <= 1e-10 ) {
					target.SetPEL( x, y, RISEColor( RISEPel( resolvedXYZ ), 1.0 ) );
				} else {
					Scalar alpha = pixel.alphaSum / pixel.weightSum;
					if( alpha < 0.0 ) alpha = 0.0;
					else if( alpha > 1.0 ) alpha = 1.0;
					if( alpha > 1e-6 ) {
						target.SetPEL( x, y, RISEColor( RISEPel( resolvedXYZ * (1.0 / alpha) ), alpha ) );
					} else {
						target.SetPEL( x, y, RISEColor( RISEPel( 0, 0, 0 ), 0.0 ) );
					}
				}
			}
		}
	}
}

void FilteredFilm::CopyRow(
	const unsigned int y,
	const unsigned int startX,
	std::vector<FilteredPixel>& row
	) const
{
	std::lock_guard<RMutex> rowLock( *rowMutexes[y] );
	const FilteredPixel* src = &pixels[static_cast<std::size_t>( y ) * width + startX];
	std::copy( src, src + row.size(), row.begin() );
}

void FilteredFilm::Unresolve(
	IRasterImage& target
	) const
{
	// Inverse of Resolve: subtract the previously-written XYZ-resolved
	// RISEPel value.  Mirror Resolve's conversion choice so the round-
	// trip cancels exactly.
	if( width == 0 || height == 0 ) return;
	std::vector<FilteredPixel> row( width );
	for( unsigned int y=0; y<height; y++ ) {
		CopyRow( y, 0, row );
		for( unsigned int x=0; x<width; x++ ) {
			const FilteredPixel& pixel = row[x];

			if( fabs(pixel.weightSum) > 1e-10 ) {
				RISEColor existing = target.GetPEL( x, y );
				const XYZPel resolvedXYZ = pixel.colorSum * (1.0 / pixel.weightSum);
				RISEPel resolved( 0, 0, 0 );
				if( fabs(pixel.alphaSum - pixel.weightSum) <= 1e-10 ) {
					resolved = RISEPel( resolvedXYZ );
				} else {
					Scalar alpha = pixel.alphaSum / pixel.weightSum;
					if( alpha < 0.0 ) alpha = 0.0;
					else if( alpha > 1.0 ) alpha = 1.0;
					if( alpha > 1e-6 ) {
						resolved = RISEPel( resolvedXYZ * (1.0 / alpha) );
					}
				}
				RISEColor combined( existing.base - resolved, existing.a );
				target.SetPEL( x, y, combined );
			}
		}
	}
}

void FilteredFilm::Clear()
{
	for( auto& pixel : pixels ) {
		pixel.colorSum = XYZPel( 0, 0, 0 );
		pixel.weightSum = 0;
		pixel.alphaSum = 0;
	}
}
