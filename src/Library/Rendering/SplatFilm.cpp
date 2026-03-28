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

	rowMutexes[y]->lock();

	SplatPixel& pixel = pixels[y * width + x];
	pixel.color = pixel.color + contribution;
	pixel.weight += 1.0;

	rowMutexes[y]->unlock();
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
