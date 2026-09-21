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
	const IORStack& ior_stack,								///< [in/out] Index of refraction stack
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
	pScene->GetObjects()->IntersectRay( ri, true, true, false );

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

		// Set the current object on the IOR stack
		ior_stack.SetCurrentObject( ri.pObject );

		// P2-2 (DL-39 over-generalisation, review round 3): is THIS hit a
		// translucent interior EXIT?  `TranslucentPelPhotonMap::
		// RadianceEstimate` is the Jensen estimator --
		// `sum(power_i) * brdf.value(...) / (pi*r^2)` -- so the stored
		// quantity must be the flux ARRIVING at the surface; the gather
		// applies the surface's own BSDF itself.  Depositing the diffuse
		// lobe's kray is right ONLY where that kray is a TRANSPORT
		// attenuation, which it is exactly at a translucent exit (Beer
		// extinction times (1-scattering), the fraction of the interior
		// segment's flux that reaches the boundary).  Everywhere else the
		// diffuse kray is a REFLECTANCE: a Lambertian wall's eRayDiffuse
		// kray IS its albedo (LambertianSPF.cpp), and TranslucentSPF's own
		// ENTERING branch uses `pRefFront`, also a reflectance -- storing
		// those makes the gather read `power * albedo^2`.
		//
		// `hasInterior` (DL-46) + `containsCurrent()` is exactly
		// TranslucentSPF::Scatter's own entry-vs-exit test.  Capture it
		// BEFORE the trace loop below: a recursive TracePhoton call that
		// inherits THIS stack (`scat.ior_stack` null) re-points its
		// current object, so the answer is no longer available afterwards.
		bool bTranslucentExit = false;
		if( ri.pMaterial ) {
			const SpecularInfo info =
				ri.pMaterial->GetSpecularInfo( ri.geometric, ior_stack );
			bTranslucentExit = info.valid && info.hasInterior && ior_stack.containsCurrent();
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
		pSPF->Scatter( ri.geometric, samplerWrapper, scattered, ior_stack );

			// DL-39: at a translucent interior EXIT the deposited flux
			// must be the diffuse exit lobe's own (Beer-attenuated) kray,
			// not "incoming power minus whatever got traced further".  The
			// original `power*(1-accum_scattered)` formula assumed the
			// SPF's non-diffuse (traced) kray plus its diffuse kray always
			// summed to exactly 1 -- true only when nothing is absorbed.
			// TranslucentSPF's diffuse exit lobe is type eRayDiffuse,
			// which the trace-selection `if` below never matches, so
			// `accum_scattered` never contained it: at an interior exit
			// with scattering=0 (no backscatter `trans` ray either)
			// accum_scattered stayed exactly 0 and the old code deposited
			// the FULL incoming power, discarding the Beer extinction the
			// SPF had already folded into the diffuse ray's own kray.
			// TranslucentSPF emits at most one eRayDiffuse ray per
			// Scatter()/ScatterNM() call, so summing them is that lobe's
			// transport weight.
			//
			// P2-2 (review round 3): that rule is specific to the
			// translucent exit lobe and must NOT be generalised to every
			// diffuse hit -- see `bTranslucentExit` above for why (the
			// Jensen gather needs ARRIVING flux, and a non-exit diffuse
			// kray is a reflectance, not a transport attenuation).  At
			// every other hit the deposit stays exactly what it was before
			// DL-39: incoming power less whatever was traced onward, which
			// for an ordinary diffuse surface (nothing traced) is the full
			// arriving power the estimator wants.
			RISEPel accum_scattered;
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
					TracePhoton( scat.ray, power*scat.kray*PathVertexEval::ImportanceShadingNormalFactor( ri.geometric.vNormal, ri.geometric.vGeomNormal, -ray.Dir(), scat.ray.Dir() ), scat.type==ScatteredRay::eRayTranslucent, pPhotonMap, scat.ior_stack?*scat.ior_stack:ior_stack, depth+1 );
					if( bFromTranslucent ) {
						accum_scattered = accum_scattered + scat.kray;
					}
				} else if( scat.type==ScatteredRay::eRayDiffuse ) {
					diffuse_deposit = diffuse_deposit + scat.kray;
				}
			}

			// Only deposit if the photon came from a translucent surface,
			if( bFromTranslucent ) {
				pPhotonMap.Store(
					bTranslucentExit ? power*diffuse_deposit
						: power*(RISEPel(1,1,1)-accum_scattered),
					ri.geometric.ptIntersection );
			}
		}
	}

	// If there was no hit then the photon just got ejected into space!
}



