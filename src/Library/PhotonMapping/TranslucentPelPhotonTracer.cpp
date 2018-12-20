//////////////////////////////////////////////////////////////////////
//
//  TranslucentPelPhotonTracer.cpp - Implementation of the 
//    TranslucentPelPhotonTracer class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 19, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TranslucentPelPhotonTracer.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

#define ENABLE_MAX_RECURSION

TranslucentPelPhotonTracer::TranslucentPelPhotonTracer(
	const unsigned int maxR, 
	const Scalar ext,
	const bool reflect, 
	const bool refract, 
	const bool direct_translucent,
	const bool dononmeshlights,
	const bool useiorstack,						///< [in] Should we use an ior stack ?
	const Scalar powerscale,
	const unsigned int temporal_samples,
	const bool regenerate
	) : 
  PhotonTracer<TranslucentPelPhotonMap>( dononmeshlights, useiorstack, powerscale, temporal_samples, regenerate ),
  nMaxRecursions( maxR ),
  dExtinction( ext ),
  bTraceReflections( reflect ),
  bTraceRefractions( refract ),
  bTraceDirectTranslucent( direct_translucent )
{
}

TranslucentPelPhotonTracer::~TranslucentPelPhotonTracer( )
{
}


void TranslucentPelPhotonTracer::TracePhoton( 
	const Ray& ray, 
	const RISEPel& power, 
	const bool bFromTranslucent,
	TranslucentPelPhotonMap& pPhotonMap,
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

		ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;

		if( pSPF )
		{
			// Get information from the material as to what to do
			ScatteredRayContainer		scattered;

			pSPF->Scatter( ri.geometric, random, scattered, ior_stack );
		
			RISEPel accum_scattered;
			for( unsigned int i=0; i<scattered.Count(); i++ ) {
				ScatteredRay& scat = scattered[i];
				// Trace all rays
				scat.ray.Advance( 1e-8 );
				bool bTraceTranslucent = true;
				if( !bTraceDirectTranslucent && numRecursions==1 ) {
					bTraceTranslucent = false;
				}

				if( (scat.type==ScatteredRay::eRayTranslucent && bTraceTranslucent) ||
					(scat.type==ScatteredRay::eRayReflection && bTraceReflections) ||
					(scat.type==ScatteredRay::eRayRefraction && bTraceRefractions) ) {
					TracePhoton( scat.ray, power*scat.kray, scat.type==ScatteredRay::eRayTranslucent, pPhotonMap, ior_stack );
					if( bFromTranslucent ) {
						accum_scattered = accum_scattered + scat.kray;
					}
				}
			}

			// Only deposit if the photon came from a translucent surface, 
			if( bFromTranslucent ) {
				pPhotonMap.Store( power*(RISEPel(1,1,1)-accum_scattered), ri.geometric.ptIntersection );
			}
		}
	}

	// If there was no hit then the photon just got ejected into space!

	numRecursions--;
}



