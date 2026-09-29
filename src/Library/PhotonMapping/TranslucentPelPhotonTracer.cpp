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
#include "../Utilities/PathVertexEval.h"
#include "TranslucentPelPhotonTracer.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/IndependentSampler.h"
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
	const bool shootFromNonMeshLights,
	const Scalar powerscale,
	const unsigned int temporal_samples,
	const bool regenerate,
	const bool shootFromMeshLights
	) :
  PhotonTracer<TranslucentPelPhotonMap>( shootFromNonMeshLights, powerscale, temporal_samples, regenerate, shootFromMeshLights ),
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
	const IORStack& ior_stack,								///< [in] Index of refraction stack (not modified; DL-315)
	const unsigned int depth								///< [in] Recursion depth (0 = primary photon emitted from the light)
	) const
{
#ifdef ENABLE_MAX_RECURSION
	if( depth > nMaxRecursions )
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

	// Cast the ray into the scene
	RayIntersection	ri( ray, nullRasterizerState );
	ri.geometric.ray.SetDir(Vector3Ops::Normalize(ri.geometric.ray.Dir()));
	IndependentSampler alphaSampler(random);
	pScene->GetObjects()->IntersectRaySampled(ri, alphaSampler);

	if( ri.geometric.bHit )
	{
		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// G6: stamp the ambient (incident-medium) IOR from the stack so the
		// GGX conductor Fresnel evaluated in SPF::Scatter sees the surrounding
		// medium (e.g. enamel glass) rather than hardcoded air.  Read BEFORE
		// SetCurrentObject so top() is the medium the photon was travelling
		// through.  Guard a non-positive stack top to air (1.0).
		{
			const Scalar ambIOR = ior_stack.top();
			ri.geometric.ambientIOR = ( ambIOR > 0.0 ) ? ambIOR : 1.0;
		}

		// DL-315: this hit's current object lives on a COPY; the caller's
		// stack is `const` and is no longer written through (IORStack's
		// pCurrentObject used to be `mutable`).
		IORStack hitStack( ior_stack );
		hitStack.SetCurrentObject( ri.pObject );

		// Separate incident-flux packets from translucent diffuse-exit
		// packets, whose SPF weight already includes Beer*(1-scattering).
		// Capture this pre-scatter state before recursive walks change the
		// stack's current object. The gather must not price that transport
		// attenuation again as a front-reflection material response.
		bool bTranslucentExit = false;
		if( ri.pMaterial ) {
			const SpecularInfo info =
				ri.pMaterial->GetSpecularInfo( ri.geometric, hitStack );
			bTranslucentExit = info.valid && info.hasInterior && hitStack.containsCurrent();
		}

		ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;

		if( pSPF )
		{
			// Get information from the material as to what to do
			ScatteredRayContainer		scattered;

			IndependentSampler samplerWrapper( random );
		// IMPORTANCE mode: NO eta^2 basic-radiance factor (debt 30).
		// A photon carries FLUX, and flux is conserved across a smooth
		// interface up to Fresnel -- it is RADIANCE that picks up
		// (eta_before/eta_after)^2, and only on the camera-rooted side.
		// Scaling here too would cancel the non-symmetry and put every
		// gather/merge that pairs a photon with an eye vertex back where
		// it was.  See docs/REFRACTIVE_RADIANCE_SCALING.md.
		pSPF->Scatter( ri.geometric, samplerWrapper, scattered, hitStack );

			// DL39: preserve the diffuse exit lobe's Beer-weighted packet,
			// rather than treating absorption as deposited flux. At ordinary
			// receivers the entire arriving packet is retained (DL280): a
			// sampled glossy continuation is not a flux partition to subtract
			// before the query BSDF is evaluated. DL39's historical pure-wall
			// control did not exercise that mixed-material subtraction.
			RISEPel diffuse_deposit;
			for( unsigned int i=0; i<scattered.Count(); i++ ) {
				ScatteredRay& scat = scattered[i];
				// Trace all rays
				scat.ray.Advance( 1e-8 );
				bool bTraceTranslucent = true;
				if( !bTraceDirectTranslucent && depth==0 ) {
					bTraceTranslucent = false;
				}

				if( (scat.type==ScatteredRay::eRayTranslucent && bTraceTranslucent) ||
					(scat.type==ScatteredRay::eRayReflection && bTraceReflections) ||
					(scat.type==ScatteredRay::eRayRefraction && bTraceRefractions) ) {
					TracePhoton( scat.ray, power*scat.kray*PathVertexEval::ImportanceShadingNormalFactor( ri.geometric.vNormal, ri.geometric.vGeomNormal, -ray.Dir(), scat.ray.Dir() ), scat.type==ScatteredRay::eRayTranslucent, pPhotonMap, scat.ior_stack?*scat.ior_stack:hitStack, depth+1 );
				} else if( scat.type==ScatteredRay::eRayDiffuse ) {
					diffuse_deposit = diffuse_deposit + scat.kray;
				}
			}

			// Only deposit if the photon came from a translucent surface,
			if( bFromTranslucent ) {
				pPhotonMap.Store(
					(bTranslucentExit ? power*diffuse_deposit : power) * (1 / ri.acceptedAlphaCoverage),
					ri.geometric.ptIntersection, -ri.geometric.ray.Dir(), bTranslucentExit );
			}
		}
	}

	// If there was no hit then the photon just got ejected into space!
}



