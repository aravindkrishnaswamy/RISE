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
