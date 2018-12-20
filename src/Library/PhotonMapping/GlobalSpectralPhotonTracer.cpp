//////////////////////////////////////////////////////////////////////
//
//  GlobalSpectralPhotonTracer.cpp - Implementation of the GlobalSpectralPhotonTracer class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 8, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GlobalSpectralPhotonTracer.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

#define ENABLE_MAX_RECURSION

GlobalSpectralPhotonTracer::GlobalSpectralPhotonTracer( 
	const unsigned int maxR,					///< [in] Maximum recursion depth
	const Scalar ext,							///< [in] Minimum importance for a photon before extinction
	const Scalar nm_begin_,						///< [in] Wavelength to start shooting photons at
	const Scalar nm_end_,						///< [in] Wavelength to end shooting photons at
	const unsigned int num_wavelengths_,		///< [in] Number of wavelengths to shoot photons at
	const bool useiorstack,						///< [in] Should we use an ior stack ?
	const bool branch,							///< [in] Should the tracer branch or only follow one path?
	const Scalar powerscale,
	const unsigned int temporal_samples,
	const bool regenerate
	) : 
  SpectralPhotonTracer<GlobalSpectralPhotonMap>( nm_begin_, nm_end_, num_wavelengths_, useiorstack, powerscale, temporal_samples, regenerate ),
  nMaxRecursions( maxR ),
  dExtinction( ext ),
  bBranch( branch )
{
}

GlobalSpectralPhotonTracer::~GlobalSpectralPhotonTracer( )
{
}

void GlobalSpectralPhotonTracer::TracePhoton( 
	const Ray& ray, 
	const Scalar power, 
	const Scalar nm,
	bool bStorePhoton, 
	GlobalSpectralPhotonMap& pPhotonMap,
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

	if( power < dExtinction )
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

			pSPF->ScatterNM( ri.geometric, random, nm, scattered, ior_stack );
		
			bool bDiffuseComponentAvailable = false;
			for( unsigned int i=0; i<scattered.Count(); i++ ) {
				ScatteredRay& scat = scattered[i];
				if( scat.type==ScatteredRay::eRayDiffuse ) {
					bDiffuseComponentAvailable = true;
					break;
				}
			}

			if( bDiffuseComponentAvailable && bStorePhoton ) {
				pPhotonMap.Store( power, nm, ri.geometric.ptIntersection, -ray.dir );
			}

			if( bBranch ) {
				for( unsigned int i=0; i<scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];
					scat.ray.Advance( 1e-8 );
					TracePhoton( scat.ray, power*scat.krayNM, nm, scat.type==ScatteredRay::eRayDiffuse, pPhotonMap, scat.ior_stack?scat.ior_stack:ior_stack );
				}
			} else {
				ScatteredRay* pScat = scattered.RandomlySelect( random.CanonicalRandom(), true );
				if( pScat ) {
					pScat->ray.Advance( 1e-8 );
					TracePhoton( pScat->ray, power*pScat->krayNM, nm, pScat->type==ScatteredRay::eRayDiffuse, pPhotonMap, pScat->ior_stack?pScat->ior_stack:ior_stack );
				}
			}
		}
	}

	// If there was no hit then the photon just got ejected into space!

	numRecursions--;
}


