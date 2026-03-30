//////////////////////////////////////////////////////////////////////
//
//  CompletePathGuide.cpp - Experimental recorder for BDPT complete
//  path statistics.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CompletePathGuide.h"
#include "../Interfaces/ILog.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static const unsigned int kCompletePathGuideGridResolution = 16;

	inline Scalar CompletePathEnergy( const RISEPel& contribution )
	{
		return contribution[0] * Scalar( 0.2126 ) +
			contribution[1] * Scalar( 0.7152 ) +
			contribution[2] * Scalar( 0.0722 );
	}

	inline uint64_t QuantizePositionComponent(
		const Scalar value,
		const Scalar minValue,
		const Scalar maxValue
		)
	{
		if( maxValue - minValue <= NEARZERO ) {
			return 0;
		}

		const Scalar normalized =
			r_min( Scalar( 0.999999 ),
				r_max( Scalar( 0.0 ),
					(value - minValue) / (maxValue - minValue) ) );
		return static_cast<uint64_t>( normalized * kCompletePathGuideGridResolution );
	}
}

CompletePathGuide::CompletePathGuide(
	const Point3& sceneBoundsMin,
	const Point3& sceneBoundsMax,
	unsigned int maxLightDepth_,
	unsigned int maxEyeDepth_
	) :
  boundsMin( sceneBoundsMin ),
  boundsMax( sceneBoundsMax ),
  maxLightDepth( maxLightDepth_ ),
  maxEyeDepth( maxEyeDepth_ ),
  collecting( false ),
  iteration( 0 ),
  lastSummary(),
  iterationTechniqueStats( (maxLightDepth + 1) * (maxEyeDepth + 1) ),
  aggregateTechniqueStats( (maxLightDepth + 1) * (maxEyeDepth + 1) ),
  iterationBuckets(),
  aggregateBuckets()
{
	iterationBuckets.reserve( 1 << 15 );
	aggregateBuckets.reserve( 1 << 16 );
}

CompletePathGuide::~CompletePathGuide()
{
}

uint64_t CompletePathGuide::MakeBucketKey(
	unsigned int s,
	unsigned int t,
	const Point3& eyePosition,
	const Point3* pLightPosition,
	bool hitsEmitter
	) const
{
	const uint64_t eyeX = QuantizePositionComponent( eyePosition.x, boundsMin.x, boundsMax.x );
	const uint64_t eyeY = QuantizePositionComponent( eyePosition.y, boundsMin.y, boundsMax.y );
	const uint64_t eyeZ = QuantizePositionComponent( eyePosition.z, boundsMin.z, boundsMax.z );

	uint64_t lightX = 0;
	uint64_t lightY = 0;
	uint64_t lightZ = 0;
	if( pLightPosition )
	{
		lightX = QuantizePositionComponent( pLightPosition->x, boundsMin.x, boundsMax.x );
		lightY = QuantizePositionComponent( pLightPosition->y, boundsMin.y, boundsMax.y );
		lightZ = QuantizePositionComponent( pLightPosition->z, boundsMin.z, boundsMax.z );
	}

	uint64_t key = 0;
	key |= (static_cast<uint64_t>( s ) & 0x0f);
	key |= (static_cast<uint64_t>( t ) & 0x0f) << 4;
	key |= (hitsEmitter ? uint64_t( 1 ) : uint64_t( 0 )) << 8;
	key |= (eyeX & 0x0f) << 9;
	key |= (eyeY & 0x0f) << 13;
	key |= (eyeZ & 0x0f) << 17;
	key |= (lightX & 0x0f) << 21;
	key |= (lightY & 0x0f) << 25;
	key |= (lightZ & 0x0f) << 29;
	return key;
}

void CompletePathGuide::BeginIteration()
{
	mutex.lock();
	iteration++;
	collecting = true;
	lastSummary = IterationSummary();
	std::fill(
		iterationTechniqueStats.begin(),
		iterationTechniqueStats.end(),
		TechniqueStats() );
	iterationBuckets.clear();
	mutex.unlock();
}

void CompletePathGuide::AddSample(
	unsigned int s,
	unsigned int t,
	const Point3& eyePosition,
	const Point3* pLightPosition,
	const RISEPel& contribution,
	bool hitsEmitter,
	bool needsSplat
	)
{
	if( !collecting || needsSplat ) {
		return;
	}

	const Scalar energy = CompletePathEnergy( contribution );
	if( energy <= NEARZERO ) {
		return;
	}

	mutex.lock();
	lastSummary.sampleCount++;
	lastSummary.totalEnergy += energy;

	const size_t techIndex = TechniqueIndex( maxLightDepth, maxEyeDepth, s, t );
	if( techIndex < iterationTechniqueStats.size() )
	{
		iterationTechniqueStats[techIndex].count++;
		iterationTechniqueStats[techIndex].energy += energy;
		aggregateTechniqueStats[techIndex].count++;
		aggregateTechniqueStats[techIndex].energy += energy;
	}

	const uint64_t key = MakeBucketKey( s, t, eyePosition, pLightPosition, hitsEmitter );
	BucketStats& iterBucket = iterationBuckets[key];
	iterBucket.count++;
	iterBucket.energy += energy;

	BucketStats& aggBucket = aggregateBuckets[key];
	aggBucket.count++;
	aggBucket.energy += energy;
	mutex.unlock();
}

void CompletePathGuide::EndIteration()
{
	mutex.lock();
	collecting = false;
	lastSummary.uniqueBucketCount = iterationBuckets.size();
	lastSummary.persistentBucketCount = aggregateBuckets.size();

	Scalar topBucketEnergy = 0;
	for( std::unordered_map<uint64_t, BucketStats>::const_iterator it = iterationBuckets.begin();
		it != iterationBuckets.end();
		++it )
	{
		topBucketEnergy = r_max( topBucketEnergy, it->second.energy );
	}
	lastSummary.topBucketEnergyShare =
		lastSummary.totalEnergy > NEARZERO ?
			topBucketEnergy / lastSummary.totalEnergy :
			0;

	for( unsigned int t = 0; t <= maxEyeDepth; t++ )
	{
		for( unsigned int s = 0; s <= maxLightDepth; s++ )
		{
			const TechniqueStats& stats =
				iterationTechniqueStats[TechniqueIndex( maxLightDepth, maxEyeDepth, s, t )];
			if( stats.energy > lastSummary.topTechniqueEnergy )
			{
				lastSummary.topTechniqueEnergy = stats.energy;
				lastSummary.topTechniqueS = s;
				lastSummary.topTechniqueT = t;
			}
		}
	}

	GlobalLog()->PrintEx( eLog_Event,
		"CompletePathGuide:: Iteration %zu recorded %zu complete paths (energy %.6f, %zu unique buckets, %zu persistent buckets, top technique s=%u t=%u energy %.6f, top bucket share %.3f)",
		iteration,
		lastSummary.sampleCount,
		lastSummary.totalEnergy,
		lastSummary.uniqueBucketCount,
		lastSummary.persistentBucketCount,
		lastSummary.topTechniqueS,
		lastSummary.topTechniqueT,
		lastSummary.topTechniqueEnergy,
		lastSummary.topBucketEnergyShare );
	mutex.unlock();
}

Scalar CompletePathGuide::QueryStrategyWeight(
	unsigned int s,
	unsigned int t,
	const Point3& eyePosition,
	const Point3* pLightPosition,
	bool hitsEmitter
	) const
{
	const size_t techIndex = TechniqueIndex( maxLightDepth, maxEyeDepth, s, t );
	const Scalar globalEnergy =
		techIndex < aggregateTechniqueStats.size() ?
			aggregateTechniqueStats[techIndex].energy :
			0;

	Scalar bucketEnergy = 0;
	const std::unordered_map<uint64_t, BucketStats>::const_iterator it =
		aggregateBuckets.find( MakeBucketKey( s, t, eyePosition, pLightPosition, hitsEmitter ) );
	if( it != aggregateBuckets.end() ) {
		bucketEnergy = it->second.energy;
	}

	const Scalar weight = bucketEnergy + globalEnergy;
	return weight > 0 ? std::sqrt( weight ) : 0;
}
