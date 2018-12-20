//////////////////////////////////////////////////////////////////////
//
//  ScatteredRayContainer.cpp - Implements the scattered ray container
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 6, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../Interfaces/ISPF.h"

using namespace RISE;

ScatteredRayContainer::ScatteredRayContainer() :
  freeidx( 0 )
{
}

ScatteredRayContainer::~ScatteredRayContainer()
{
}

bool ScatteredRayContainer::AddScatteredRay( ScatteredRay& ray )
{
	if( freeidx >= 6 ) {
		return false;
	}

	memcpy( (void*)&rays[freeidx], (void*)&ray, sizeof( ScatteredRay ) );
	freeidx++;

	ray.delete_stack = false;
	return true;
}

//! From the rays stored, randomly returns one given a value
ScatteredRay* ScatteredRayContainer::RandomlySelect(
		const double random,										///< [in] Random number to use in ray selection
		const bool bNM												///< [in] Should the spectral values be used when selecting?
		) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( freeidx == 1 ) {
		return &rays[0];
	}

	if( freeidx == 2 ) {
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[6] = {0};
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
		cdf[i] = total + prob;
		total += prob;
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( random < (cdf[i]/total) ) {
				return &rays[i];
			}
		}
	}

	return 0;
}

//! From the rays stored, randomly returns a non diffuse ray
ScatteredRay* ScatteredRayContainer::RandomlySelectNonDiffuse(
	const double random,										///< [in] Random number to use in ray selection
	const bool bNM												///< [in] Should the spectral values be used when selecting?
	) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type!=ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse) )
	{
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[6] = {0};
	bool valid[6];
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		valid[i] = rays[i].type!=ScatteredRay::eRayDiffuse;
		if( valid[i] ) {
			const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
			cdf[i] = total + prob;
			total += prob;
		}
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( valid[i] ) {
				if( random < (cdf[i]/total) ) {
					return &rays[i];
				}
			}
		}
	}


	return 0;
}


//! From the rays stored, randomly returns a diffuse ray
ScatteredRay* ScatteredRayContainer::RandomlySelectDiffuse(
	const double random,										///< [in] Random number to use in ray selection
	const bool bNM												///< [in] Should the spectral values be used when selecting?
	) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type==ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse) )
	{
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[6] = {0};
	bool valid[6];
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		valid[i] = rays[i].type==ScatteredRay::eRayDiffuse;
		if( valid[i] ) {
			const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
			cdf[i] = total + prob;
			total += prob;
		}
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( valid[i] ) {
				if( random < (cdf[i]/total) ) {
					return &rays[i];
				}
			}
		}
	}

	return 0;
}
