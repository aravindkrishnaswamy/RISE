//////////////////////////////////////////////////////////////////////
//
//  SplatFilm.cpp - Implementation of the SplatFilm class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SplatFilm.h"
#include "ThreadLocalSplatBuffer.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include "../Utilities/CheckedSize.h"

using namespace RISE;
using namespace RISE::Implementation;

std::size_t SplatFilm::CheckedPixelCount(unsigned int w, unsigned int h)
{
	constexpr std::uint64_t indexDomain = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
	constexpr std::size_t indexLimit = std::numeric_limits<std::size_t>::max() < indexDomain ?
		std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(indexDomain);
	const auto count = RISE::Implementation::CheckedPixelCount<SplatPixel>(w, h, indexLimit);
	if( count ) RISE::Implementation::CheckedPixelCount<std::unique_ptr<RMutex>>(h, 1);
	return count;
}

SplatFilm::SplatFilm(
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

SplatFilm::~SplatFilm() = default;

void SplatFilm::Splat(
	const unsigned int x,
	const unsigned int y,
	const RISEPel& contribution
	)
{
	if( x >= width || y >= height ) {
		return;
	}

	// Route through the per-thread sparse buffer.  This turns a
	// per-splat mutex acquire+release into a per-row acquire that
	// amortizes over all splats the thread produces for that row
	// before FlushInto() is called (typically at end-of-tile).
	//
	// Lazy-bind on first call per thread per film.
	ThreadLocalSplatBuffer& buf = GetThreadLocalSplatBuffer();
	if( buf.GetBoundFilm() != this ) {
		buf.Bind( this, width, height );
	}
	buf.Splat( x, y, contribution );
}

void SplatFilm::SplatFiltered(
	const Scalar screenX,
	const Scalar screenY,
	const RISEPel& contribution,
	const IPixelFilter& filter
	)
{
	if( width == 0 || height == 0 ) return;
	Scalar halfW, halfH;
	filter.GetFilterSupport( halfW, halfH );
	// Invalid extents have no defined footprint. Validate before either the
	// box shortcut or any floating-to-integer conversion.
	if( !std::isfinite(halfW) || !std::isfinite(halfH) || halfW < 0 || halfH < 0 ) return;

	// Box-filter fast path — half-width ≤ 0.501 means the filter
	// footprint is exactly one pixel with weight 1, so a point
	// splat at the nearest integer pixel is equivalent.  Matches
	// the threshold used by PixelBasedRasterizerHelper::UseFilteredFilm
	// so behaviour is consistent across the codebase.
	if( halfW <= 0.501 && halfH <= 0.501 )
	{
		// The pixel containing the splat (centres at i + 0.5, DL-368);
		// NearestPixel is the film-membership rule (DL-294).
		unsigned int ix = 0, iy = 0;
		if( NearestPixel( screenX, screenY, width, height, ix, iy ) ) {
			Splat( ix, iy, contribution );
		}
		return;
	}

	// Preserve the centre-value heuristic for existing reconstruction
	// filters: the base EvaluateFilter implementation returns zero, so a
	// missing override would otherwise drop every splat. Nonpositive-centre
	// kernels use the legacy point fallback, with one warning per process.
	if( filter.EvaluateFilter( 0, 0 ) <= 0 )
	{
		static std::atomic<bool> warned{ false };
		bool expected = false;
		if( warned.compare_exchange_strong( expected, true ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SplatFilm::SplatFiltered:: pixel filter advertises support "
				"(%.3f x %.3f) but EvaluateFilter returned 0 at the centre. "
				"Falling back to point splats — filter subclass probably needs "
				"an EvaluateFilter override.",
				halfW, halfH );
		}
		unsigned int ix = 0, iy = 0;
		if( NearestPixel( screenX, screenY, width, height, ix, iy ) ) {
			Splat( ix, iy, contribution );
		}
		return;
	}

	// DL-294: film membership.  The camera's world-to-raster
	// projection no longer clips to its nominal [0,W) x [0,H) film
	// (BDPTCameraUtilities::InRasterGuardBand); the film decides, in
	// the SAME convention the eye subpaths are sampled in and the box
	// fast path above rounds in: a splat is on the film iff its
	// nearest pixel centre is.  Without this a splat just outside the
	// film would be renormalised onto the edge pixels below.
	{
		unsigned int hx = 0, hy = 0;
		if( !NearestPixel( screenX, screenY, width, height, hx, hy ) ) {
			return;
		}
	}

	// Filter footprint expansion.  Mirrors FilteredFilm::Splat so
	// the two accumulators agree on which pixels receive weight
	// from a given fractional sample position.  (cx, cy) is the
	// position measured in pixel-CENTRE units: pixel (px, py) has its
	// centre at (px + 0.5, py + 0.5) (DL-368), i.e. at integer (cx, cy).
	const Scalar cx = screenX - RasterConvention::kPixelCentre;
	const Scalar cy = screenY - RasterConvention::kPixelCentre;
	// Intersect in floating point first. Even DBL_MAX-sized finite support
	// on a small film must not be narrowed before clipping to the film.
	const Scalar firstX = std::max(Scalar(0), std::floor(cx - halfW));
	const Scalar lastX = std::min(static_cast<Scalar>(width - 1), std::floor(cx + halfW));
	const Scalar firstY = std::max(Scalar(0), std::floor(cy - halfH));
	const Scalar lastY = std::min(static_cast<Scalar>(height - 1), std::floor(cy + halfH));
	if( firstX > lastX || firstY > lastY ) return;
	// The nonempty intersection proves every converted value lies in
	// [0, axis - 1], including axes above the signed-int range.
	const auto x0 = static_cast<unsigned int>(firstX);
	const auto x1 = static_cast<unsigned int>(lastX);
	const auto y0 = static_cast<unsigned int>(firstY);
	const auto y1 = static_cast<unsigned int>(lastY);

	// Two-pass per-splat normalization.
	//
	// SplatFilm::Resolve divides every pixel by a SCALAR global
	// sampleCount — it does not track a per-pixel weight sum like
	// FilteredFilm does.  That means the DISCRETE sum
	//     Σ_i EvaluateFilter(cx - px_i, cy - py_i)
	// over the affected pixels must equal 1.0 for every fractional
	// splat position, otherwise each splat deposits more or less
	// than one sample's worth of energy and the resolved image is
	// brightness-biased.  A continuous unit-integral kernel does
	// NOT guarantee this — for example a tent with width 1.7 or a
	// truncated Gaussian both have ∫K = 1 but their discrete
	// footprint sums oscillate with sub-pixel position.
	//
	// Compute the sum over the affected pixels, then deposit contribution
	// times each cached weight divided by that sum. Under the kernel
	// contract this targets the original total contribution, subject to
	// floating-point rounding. Evaluated weights are reused by the deposit
	// pass; typical footprints use the stack buffer below, wider ones a
	// scoped vector.
	Scalar weightSum = 0;
	const std::size_t footprintWidth = static_cast<std::size_t>(x1) - x0 + 1;
	const std::size_t footprintHeight = static_cast<std::size_t>(y1) - y0 + 1;
	const auto footprintArea = RISE::Implementation::CheckedPixelCount<Scalar>(footprintWidth, footprintHeight);
	// Stack buffer for typical 4×4 Mitchell footprint; heap for the
	// rare case of a very wide filter (box width 10+ etc.).
	Scalar weightsStack[64];
	Scalar* weights = weightsStack;
	std::vector<Scalar> weightsHeap;
	if( footprintArea > sizeof(weightsStack) / sizeof(weightsStack[0]) ) {
		weightsHeap.resize( footprintArea );
		weights = weightsHeap.data();
	}
	{
		std::size_t k = 0;
		for( unsigned int py = y0; py <= y1; py++ )
		{
			const Scalar dy = cy - static_cast<Scalar>( py );
			for( unsigned int px = x0; px <= x1; px++, k++ )
			{
				const Scalar dx = cx - static_cast<Scalar>( px );
				const Scalar w = filter.EvaluateFilter( dx, dy );
				weights[k] = w;
				weightSum += w;
			}
		}
	}

	// Pathological: all weights zero within the (clamped) footprint.
	// Can happen at an image corner for a sample just outside the
	// visible rect; also the "broken filter" case already caught
	// above.  Drop silently — there is no pixel this splat could
	// contribute to.
	if( weightSum <= 0 ) {
		return;
	}

	const Scalar invWeightSum = static_cast<Scalar>( 1.0 ) / weightSum;

	// Route through Splat() so TLS buffer + row-mutex batching
	// work unchanged.  A 4×4 Mitchell footprint produces 16
	// buffered records per filtered splat; they batch by row at
	// flush time just like any other contributions.
	std::size_t k = 0;
	for( unsigned int py = y0; py <= y1; py++ )
	{
		for( unsigned int px = x0; px <= x1; px++, k++ )
		{
			const Scalar w = weights[k];
			if( w != 0.0 ) {
				Splat( px,
				       py,
				       contribution * ( w * invWeightSum ) );
			}
		}
	}
}

void SplatFilm::BatchCommit(
	const BatchRecord* records,
	std::size_t count
	)
{
	if( !records || count == 0 || width == 0 || height == 0 ) {
		return;
	}

	// Records are sorted by pixelIndex → rows group together.
	// Walk, acquiring each row's mutex exactly once.
	std::size_t i = 0;
	while( i < count ) {
		const uint32_t firstIdx = records[i].pixelIndex;
		const unsigned int row   = firstIdx / width;
		if( row >= height ) {
			i++;
			continue;
		}

		// Find end of this row's group (records[j].y == row).
		std::size_t j = i;
		while( j < count && (records[j].pixelIndex / width) == row ) {
			j++;
		}

		// Single lock acquisition for all splats in this row.
		std::lock_guard<RMutex> rowLock(*rowMutexes[row]);
		for( std::size_t k = i; k < j; k++ ) {
			const uint32_t idx = records[k].pixelIndex;
			if( idx < pixels.size() ) {
				SplatPixel& pixel = pixels[idx];
				pixel.color   = pixel.color + records[k].color;
				pixel.weight += 1.0;
			}
		}

		i = j;
	}
}

void SplatFilm::ApplyResolved(
	IRasterImage& target,
	const Scalar sampleCount,
	const Rect* region,
	const bool subtract
	) const
{
	if( sampleCount <= 0 || width == 0 || height == 0 ) {
		return;
	}

	const Scalar invSamples = 1.0 / sampleCount;
	unsigned int startX = 0, startY = 0, endX = width - 1, endY = height - 1;
	if( region ) {
		if( region->left > region->right || region->top > region->bottom ||
			region->left >= width || region->top >= height ) return;
		startX = region->left;
		startY = region->top;
		endX = r_min( region->right, width - 1 );
		endY = r_min( region->bottom, height - 1 );
	}

	// Writers (BatchCommit) mutate a row under rowMutexes[row] while a
	// preview composition may resolve concurrently.  Snapshot each row's
	// span under that same lock, release it, then write the target: no row
	// lock is held across the target's SetPEL/GetPEL (lock-free for every
	// in-tree target) and only one row lock is ever held at a time.
	std::vector<SplatPixel> row( static_cast<std::size_t>( endX - startX ) + 1 );
	for( unsigned int y=startY; y<=endY; y++ ) {
		{
			std::lock_guard<RMutex> rowLock( *rowMutexes[y] );
			const SplatPixel* src = &pixels[static_cast<std::size_t>( y ) * width + startX];
			std::copy( src, src + row.size(), row.begin() );
		}
		for( unsigned int x=startX; x<=endX; x++ ) {
			const SplatPixel& pixel = row[x - startX];
			if( pixel.weight > 0 ) {
				// Scale the accumulated splat by the total sample count and
				// add it to (or, for Unresolve, subtract it from) the target.
				// IRasterImage stores straight RGB + coverage alpha; when
				// alpha is fractional (transparent-background silhouette),
				// unpremultiply the pixel-averaged splat so premultiplied
				// outputs do not attenuate it by alpha a second time.
				const RISEColor existing = target.GetPEL( x, y );
				RISEPel splatPel = pixel.color * invSamples;
				if( existing.a > 1e-6 && existing.a < 1.0 ) {
					splatPel = splatPel * (1.0 / existing.a);
				}
				const RISEColor combined( subtract ? existing.base - splatPel : existing.base + splatPel, existing.a );
				target.SetPEL( x, y, combined );
			}
		}
	}
}

void SplatFilm::Resolve(
	IRasterImage& target,
	const Scalar sampleCount,
	const Rect* region
	) const
{
	ApplyResolved( target, sampleCount, region, false );
}

void SplatFilm::Unresolve(
	IRasterImage& target,
	const Scalar sampleCount,
	const Rect* region
	) const
{
	ApplyResolved( target, sampleCount, region, true );
}

void SplatFilm::Clear()
{
	for( auto& pixel : pixels ) {
		pixel.color = RISEPel( 0, 0, 0 );
		pixel.weight = 0;
	}
}

void SplatFilm::FlushCallingThreadBuffer()
{
	ThreadLocalSplatBuffer& buf = GetThreadLocalSplatBuffer();
	if( buf.GetBoundFilm() == this ) {
		buf.FlushBound();
	}
}
