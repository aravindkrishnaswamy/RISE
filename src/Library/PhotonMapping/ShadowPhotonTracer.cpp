//////////////////////////////////////////////////////////////////////
//
//  ShadowPhotonTracer.cpp - Implementation of the ShadowPhotonTracer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ShadowPhotonTracer.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

ShadowPhotonTracer::ShadowPhotonTracer(
	const unsigned int temporal_samples,
	const bool regenerate
	) : 
  PhotonTracer<ShadowPhotonMap>( false, false, 1.0, temporal_samples, regenerate )
{
}

ShadowPhotonTracer::~ShadowPhotonTracer( )
{
}

void ShadowPhotonTracer::TracePhoton( 
	const Ray& ray,
	const bool bOccluded,
	ShadowPhotonMap& pPhotonMap
	) const
{
	// Cast the ray into the scene
	RayIntersection	ri( ray, nullRasterizerState );
	ri.geometric.ray = ray;
	Vector3Ops::NormalizeMag(ri.geometric.ray.dir);
	pScene->GetObjects()->IntersectRay( ri, true, false, false );

	if( ri.geometric.bHit )
	{
		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Only store if the surface is facing the right direction
		pPhotonMap.Store( ri.geometric.ptIntersection, bOccluded );

		// Continue
		Ray r = ray;
		r.Advance( ri.geometric.range + 1e-8 );
		TracePhoton( r, true, pPhotonMap );
	}

	// If there was no hit then the photon just got ejected into space!
}



