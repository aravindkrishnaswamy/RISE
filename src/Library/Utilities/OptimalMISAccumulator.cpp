//////////////////////////////////////////////////////////////////////
//
//  OptimalMISAccumulator.cpp - Implementation of the tiled
//  second-moment accumulator for optimal MIS weights.
//
//  See OptimalMISAccumulator.h for the algorithm overview and
//  references.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OptimalMISAccumulator.h"
#include "Log/Log.h"
#include <cmath>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

OptimalMISAccumulator::OptimalMISAccumulator() :
	imageWidth( 0 ),
	imageHeight( 0 ),
	tileSize( 16 ),
	tilesX( 0 ),
	tilesY( 0 ),
	minSamplesPerTile( 32 ),
	alphaClampMin( 0.05 ),
	alphaClampMax( 0.95 ),
	bSolved( false )
{
}

OptimalMISAccumulator::~OptimalMISAccumulator()
{
}

void OptimalMISAccumulator::Initialize(
	unsigned int width,
	unsigned int height,
	const Config& config
	)
{
	imageWidth = width;
	imageHeight = height;
	tileSize = config.tileSize > 0 ? config.tileSize : 16;
	minSamplesPerTile = config.minSamplesPerTile;
	alphaClampMin = config.alphaClampMin;
	alphaClampMax = config.alphaClampMax;

	tilesX = (width + tileSize - 1) / tileSize;
	tilesY = (height + tileSize - 1) / tileSize;

	const unsigned int numTiles = tilesX * tilesY;
	tiles.clear();
	tiles.resize( numTiles );
	solvedAlpha.clear();
	solvedAlpha.resize( numTiles, 0.5 );
	bSolved = false;

	GlobalLog()->PrintEx( eLog_Event,
		"OptimalMISAccumulator:: Initialized %ux%u image, %ux%u tiles (tile size %u)",
		width, height, tilesX, tilesY, tileSize );
}

void OptimalMISAccumulator::Reset()
{
	for( unsigned int i = 0; i < tiles.size(); i++ ) {
		tiles[i].sumMomentNee.store( 0.0 );
		tiles[i].sumMomentBsdf.store( 0.0 );
		tiles[i].countNee.store( 0 );
		tiles[i].countBsdf.store( 0 );
	}
	bSolved = false;
}

void OptimalMISAccumulator::AccumulateCount(
	unsigned int px,
	unsigned int py,
	SamplingTechnique technique
	)
{
	if( px >= imageWidth || py >= imageHeight ) {
		return;
	}

	const unsigned int idx = TileIndex( px, py );
	if( technique == kTechniqueNEE ) {
		tiles[idx].countNee.fetch_add( 1, std::memory_order_relaxed );
	} else {
		tiles[idx].countBsdf.fetch_add( 1, std::memory_order_relaxed );
	}
}

void OptimalMISAccumulator::Accumulate(
	unsigned int px,
	unsigned int py,
	Scalar f2,
	Scalar samplingPdf,
	SamplingTechnique technique
	)
{
	if( px >= imageWidth || py >= imageHeight ) {
		return;
	}
	if( f2 <= 0 || samplingPdf <= 0 ) {
		return;
	}

	const unsigned int idx = TileIndex( px, py );

	// The second moment of technique i's estimator is:
	//   M_i = E_{x~p_i}[(f(x)/p_i(x))^2]
	//
	// So from a sample x drawn by technique i, the unbiased estimator
	// for M_i is (f/p_i)^2 = f^2 / p_i^2.  Only hits (f > 0)
	// contribute to the sum; the count includes all samples (hits +
	// misses) and is managed by AccumulateCount().
	const double pdf = static_cast<double>( samplingPdf );
	const double moment = static_cast<double>( f2 ) / (pdf * pdf);

	// Atomic fetch-add for lock-free thread safety.
	// std::atomic<double> supports fetch_add in C++20; for C++11/14/17
	// compatibility we use a compare-exchange loop.
	if( technique == kTechniqueNEE )
	{
		double expected = tiles[idx].sumMomentNee.load( std::memory_order_relaxed );
		while( !tiles[idx].sumMomentNee.compare_exchange_weak(
			expected, expected + moment, std::memory_order_relaxed ) ) {}
	}
	else
	{
		double expected = tiles[idx].sumMomentBsdf.load( std::memory_order_relaxed );
		while( !tiles[idx].sumMomentBsdf.compare_exchange_weak(
			expected, expected + moment, std::memory_order_relaxed ) ) {}
	}
}

void OptimalMISAccumulator::Solve()
{
	unsigned int validTiles = 0;
	unsigned int fallbackTiles = 0;

	for( unsigned int i = 0; i < tiles.size(); i++ ) {
		const unsigned int nNee = tiles[i].countNee.load();
		const unsigned int nBsdf = tiles[i].countBsdf.load();
		const bool haveNee = (nNee >= minSamplesPerTile);
		const bool haveBsdf = (nBsdf >= minSamplesPerTile);

		if( !haveNee && !haveBsdf ) {
			// No data for either technique — balance heuristic
			solvedAlpha[i] = 0.5;
			fallbackTiles++;
			continue;
		}

		if( haveNee && !haveBsdf ) {
			// NEE has data but BSDF couldn't produce enough emitter
			// hits — BSDF is poor at finding lights in this tile.
			// Favor NEE (alpha toward clampMin).
			solvedAlpha[i] = alphaClampMin;
			validTiles++;
			continue;
		}

		if( !haveNee && haveBsdf ) {
			// BSDF has data but NEE doesn't — unusual, but favor BSDF.
			solvedAlpha[i] = alphaClampMax;
			validTiles++;
			continue;
		}

		// Both techniques have enough attempts — but check whether
		// each actually produced nonzero contributions.  A technique
		// with many attempts but zero accumulated moment has no
		// evidence of finding light; treating M_i=0 as zero variance
		// would incorrectly over-trust it.
		const double rawNee = tiles[i].sumMomentNee.load();
		const double rawBsdf = tiles[i].sumMomentBsdf.load();

		if( rawNee <= 0 && rawBsdf <= 0 ) {
			// Neither technique found any light — balance heuristic
			solvedAlpha[i] = 0.5;
			fallbackTiles++;
			continue;
		}

		if( rawNee <= 0 ) {
			// NEE never found light despite enough attempts — favor BSDF
			solvedAlpha[i] = alphaClampMax;
			validTiles++;
			continue;
		}

		if( rawBsdf <= 0 ) {
			// BSDF never found light despite enough attempts — favor NEE
			solvedAlpha[i] = alphaClampMin;
			validTiles++;
			continue;
		}

		const double Mnee = rawNee / static_cast<double>( nNee );
		const double Mbsdf = rawBsdf / static_cast<double>( nBsdf );
		const double denom = Mnee + Mbsdf;

		// alpha = M_nee / (M_nee + M_bsdf)
		//
		// From Kondapaneni et al. 2019: the optimal per-technique
		// coefficient is alpha_i = 1 / M_i.  Normalizing
		// for two techniques:
		//
		//   alpha_bsdf / (alpha_bsdf + alpha_nee)
		//     = (1/M_bsdf) / (1/M_bsdf + 1/M_nee)
		//     = M_nee / (M_nee + M_bsdf)
		//
		// The technique with LOWER second moment (lower variance)
		// gets MORE weight.  So the BSDF alpha uses M_nee in the
		// numerator — the OTHER technique's second moment.
		Scalar alpha = static_cast<Scalar>( Mnee / denom );

		// Clamp to avoid degenerate weights
		alpha = std::max( alphaClampMin, std::min( alphaClampMax, alpha ) );

		solvedAlpha[i] = alpha;
		validTiles++;
	}

	bSolved = true;

	GlobalLog()->PrintEx( eLog_Event,
		"OptimalMISAccumulator:: Solved %u tiles (%u valid, %u fallback to balance heuristic)",
		static_cast<unsigned int>( tiles.size() ), validTiles, fallbackTiles );
}

Scalar OptimalMISAccumulator::GetAlpha(
	unsigned int px,
	unsigned int py
	) const
{
	if( !bSolved || px >= imageWidth || py >= imageHeight ) {
		return 0.5;
	}
	return solvedAlpha[TileIndex( px, py )];
}
