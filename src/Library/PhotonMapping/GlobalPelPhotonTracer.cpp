//////////////////////////////////////////////////////////////////////
//
//  GlobalPelPhotonTracer.cpp - Implementation of the GlobalPelPhotonTracer
//    class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 14, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GlobalPelPhotonTracer.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

#define ENABLE_MAX_RECURSION

GlobalPelPhotonTracer::GlobalPelPhotonTracer(
	const unsigned int maxR, 
	const Scalar ext, 
	const bool branch,
	const bool dononmeshlights,					///< Should we trace non-mesh lights?
	const bool useiorstack,						///< [in] Should we use an ior stack ?
	const Scalar powerscale,
	const unsigned int temporal_samples,
	const bool regenerate
	) : 
  PhotonTracer<GlobalPelPhotonMap>( dononmeshlights, useiorstack, powerscale, temporal_samples, regenerate ),
  nMaxRecursions( maxR ),
  dExtinction( ext ),
  bBranch( branch )
{
}

GlobalPelPhotonTracer::~GlobalPelPhotonTracer( )
{
}


void GlobalPelPhotonTracer::TracePhoton( 
	const Ray& ray,
	const RISEPel& power,
	GlobalPelPhotonMap& pPhotonMap,
	const bool bStorePhoton,
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	static unsigned int		numRecursions = 0;

#ifdef ENABLE_MAX_RECURSION
	if( numRecursions > nMaxRecursions )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "FORCED RECURSION TERMINATION" );
#endif
		return;
	}
#endif

	if( ColorMath::MaxValue(power) < dExtinction )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "PHOTON BECAME EXTINCT :(" );
#endif
		return;
	}

	numRecursions++;

	// Cast the ray into the scene
	RayIntersection	ri( ray, nullRasterizerState );
	Vector3Ops::NormalizeMag(ri.geometric.ray.dir);
	pScene->GetObjects()->IntersectRay( ri, true, true, false );

	if( ri.geometric.bHit )
	{
		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Set the current object on the IOR stack
		if( ior_stack ) {
			ior_stack->SetCurrentObject( ri.pObject );
		}

		ISPF* pSPF = ri.pMaterial->GetSPF();

		if( pSPF )
		{
			// Get information from the material as to what to do
			ScatteredRayContainer		scattered;

			pSPF->Scatter( ri.geometric, random, scattered, ior_stack );

			bool bDiffuseComponentAvailable = false;
			for( unsigned int i=0; i<scattered.Count(); i++ ) {
				ScatteredRay& scat = scattered[i];
				if( scat.type==ScatteredRay::eRayDiffuse ) {
					bDiffuseComponentAvailable = true;
					break;
				}
			}

			if( bDiffuseComponentAvailable && bStorePhoton ) {
				pPhotonMap.Store( power, ri.geometric.ptIntersection, ri.geometric.vNormal, -ray.dir );
			}

			if( bBranch ) {
				for( unsigned int i=0; i<scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];
					scat.ray.Advance( 1e-8 );
					TracePhoton( scat.ray, power*scat.kray, pPhotonMap, scat.type==ScatteredRay::eRayDiffuse, scat.ior_stack?scat.ior_stack:ior_stack );
				}
			} else {
				ScatteredRay* pScat = scattered.RandomlySelect( random.CanonicalRandom(), false );
				if( pScat ) {
					pScat->ray.Advance( 1e-8 );
					TracePhoton( pScat->ray, power*pScat->kray, pPhotonMap, pScat->type==ScatteredRay::eRayDiffuse, pScat->ior_stack?pScat->ior_stack:ior_stack );
				}
			}
		}
	}

	// If there was no hit then the photon just got ejected into space!

	numRecursions--;
}

