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

using namespace RISE;
using namespace RISE::Implementation;

SplatFilm::SplatFilm(
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

SplatFilm::~SplatFilm()
{
	for( unsigned int i=0; i<rowMutexes.size(); i++ ) {
		delete rowMutexes[i];
		rowMutexes[i] = 0;
	}
}

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
	Scalar halfW, halfH;
	filter.GetFilterSupport( halfW, halfH );

	// Box-filter fast path — half-width ≤ 0.501 means the filter
	// footprint is exactly one pixel with weight 1, so a point
	// splat at the nearest integer pixel is equivalent.  Matches
	// the threshold used by PixelBasedRasterizerHelper::UseFilteredFilm
	// so behaviour is consistent across the codebase.
	if( halfW <= 0.501 && halfH <= 0.501 )
	{
		// Round to nearest pixel center.  Pixel centers live at
		// integer coordinates (matches BoxPixelFilter::warpOnScreen
		// which returns canonical.x + x - 0.5 for canonical ∈ [0,1)).
		// Casting floor(v + 0.5) is the portable way to do a
		// round-half-up for positive values.
		const Scalar rx = screenX + Scalar( 0.5 );
		const Scalar ry = screenY + Scalar( 0.5 );
		if( rx < 0 || ry < 0 ) return;
		const unsigned int ix = static_cast<unsigned int>( rx );
		const unsigned int iy = static_cast<unsigned int>( ry );
		Splat( ix, iy, contribution );
		return;
	}

	// Defensive fallback.  IPixelFilter::EvaluateFilter has a base
	// implementation that returns 0, so a filter that advertises a
	// support ≥ 0.5 via GetFilterSupport but forgot to override
	// EvaluateFilter would silently drop every splat.  We detect
	// this at the filter's own centre (where it should be strongly
	// non-zero for any unit-integral kernel) and fall back to a
	// single-pixel point splat if the filter looks broken.  Logs
	// once per process so the scene author sees the problem.
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
		const Scalar rx = screenX + Scalar( 0.5 );
		const Scalar ry = screenY + Scalar( 0.5 );
		if( rx < 0 || ry < 0 ) return;
		const unsigned int ix = static_cast<unsigned int>( rx );
		const unsigned int iy = static_cast<unsigned int>( ry );
		if( ix < width && iy < height ) {
			Splat( ix, iy, contribution );
		}
		return;
	}

	// Filter footprint expansion.  Mirrors FilteredFilm::Splat so
	// the two accumulators agree on which pixels receive weight
	// from a given fractional sample position.
	const int minPX = static_cast<int>( floor( screenX - halfW ) );
	const int maxPX = static_cast<int>( floor( screenX + halfW ) );
	const int minPY = static_cast<int>( floor( screenY - halfH ) );
	const int maxPY = static_cast<int>( floor( screenY + halfH ) );

	const int x0 = minPX < 0 ? 0 : minPX;
	const int x1 = maxPX >= static_cast<int>( width ) ?
		static_cast<int>( width ) - 1 : maxPX;
	const int y0 = minPY < 0 ? 0 : minPY;
	const int y1 = maxPY >= static_cast<int>( height ) ?
		static_cast<int>( height ) - 1 : maxPY;

	if( x0 > x1 || y0 > y1 ) {
		return;
	}

	// Two-pass per-splat normalization.
	//
	// SplatFilm::Resolve divides every pixel by a SCALAR global
	// sampleCount — it does not track a per-pixel weight sum like
	// FilteredFilm does.  That means the DISCRETE sum
	//     Σ_i EvaluateFilter(px_i - screenX, py_i - screenY)
	// over the affected pixels must equal 1.0 for every fractional
	// splat position, otherwise each splat deposits more or less
	// than one sample's worth of energy and the resolved image is
	// brightness-biased.  A continuous unit-integral kernel does
	// NOT guarantee this — for example a tent with width 1.7 or a
	// truncated Gaussian both have ∫K = 1 but their discrete
	// footprint sums oscillate with sub-pixel position.
	//
	// The fix: compute Σ over the actual affected pixels first,
	// then deposit contribution*w/Σ at each pixel.  Every splat
	// then deposits exactly `contribution` total energy across its
	// footprint regardless of filter shape or sub-pixel alignment.
	//
	// Cost: one extra pass of 4x4 EvaluateFilter calls for the
	// default Mitchell-Netravali footprint (~16 multiply-adds on
	// top of the 16 we were already doing) — negligible compared
	// to the BDPT/MLT path-evaluation cost this helper gates.
	Scalar weightSum = 0;
	const int footprintArea = ( x1 - x0 + 1 ) * ( y1 - y0 + 1 );
	// Stack buffer for typical 4×4 Mitchell footprint; heap for the
	// rare case of a very wide filter (box width 10+ etc.).
	Scalar weightsStack[64];
	Scalar* weights = weightsStack;
	std::vector<Scalar> weightsHeap;
	if( footprintArea > static_cast<int>( sizeof(weightsStack) / sizeof(weightsStack[0]) ) ) {
		weightsHeap.resize( static_cast<std::size_t>( footprintArea ) );
		weights = weightsHeap.data();
	}
	{
		int k = 0;
		for( int py = y0; py <= y1; py++ )
		{
			const Scalar dy = screenY - static_cast<Scalar>( py );
			for( int px = x0; px <= x1; px++, k++ )
			{
				const Scalar dx = screenX - static_cast<Scalar>( px );
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
	int k = 0;
	for( int py = y0; py <= y1; py++ )
	{
		for( int px = x0; px <= x1; px++, k++ )
		{
			const Scalar w = weights[k];
			if( w != 0.0 ) {
				Splat( static_cast<unsigned int>( px ),
				       static_cast<unsigned int>( py ),
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
	if( !records || count == 0 ) {
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
		rowMutexes[row]->lock();
		for( std::size_t k = i; k < j; k++ ) {
			const uint32_t idx = records[k].pixelIndex;
			if( idx < pixels.size() ) {
				SplatPixel& pixel = pixels[idx];
				pixel.color   = pixel.color + records[k].color;
				pixel.weight += 1.0;
			}
		}
		rowMutexes[row]->unlock();

		i = j;
	}
}

void SplatFilm::Resolve(
	IRasterImage& target,
	const Scalar sampleCount
	) const
{
	if( sampleCount <= 0 ) {
		return;
	}

	const Scalar invSamples = 1.0 / sampleCount;

	for( unsigned int y=0; y<height; y++ ) {
		for( unsigned int x=0; x<width; x++ ) {
			const SplatPixel& pixel = pixels[y * width + x];

			if( pixel.weight > 0 ) {
				// Get the existing color in the target
				RISEColor existing = target.GetPEL( x, y );

				// Scale the accumulated splat by the total sample count and add
				RISEPel splatPel = pixel.color * invSamples;
				RISEColor combined( existing.base + splatPel, existing.a );

				target.SetPEL( x, y, combined );
			}
		}
	}
}

void SplatFilm::Unresolve(
	IRasterImage& target,
	const Scalar sampleCount
	) const
{
	if( sampleCount <= 0 ) {
		return;
	}

	const Scalar invSamples = 1.0 / sampleCount;

	for( unsigned int y=0; y<height; y++ ) {
		for( unsigned int x=0; x<width; x++ ) {
			const SplatPixel& pixel = pixels[y * width + x];

			if( pixel.weight > 0 ) {
				RISEColor existing = target.GetPEL( x, y );
				RISEPel splatPel = pixel.color * invSamples;
				RISEColor combined( existing.base - splatPel, existing.a );
				target.SetPEL( x, y, combined );
			}
		}
	}
}

void SplatFilm::Clear()
{
	for( unsigned int i=0; i<pixels.size(); i++ ) {
		pixels[i].color = RISEPel( 0, 0, 0 );
		pixels[i].weight = 0;
	}
}

void SplatFilm::FlushCallingThreadBuffer()
{
	ThreadLocalSplatBuffer& buf = GetThreadLocalSplatBuffer();
	if( buf.GetBoundFilm() == this ) {
		buf.FlushBound();
	}
}
