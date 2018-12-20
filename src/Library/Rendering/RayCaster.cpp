//////////////////////////////////////////////////////////////////////
//
//  RayCaster.cpp - Implementation of the RayCaster class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayCaster.h"
#include "LuminaryManager.h"
#include "../Utilities/RandomNumbers.h"

#define ENABLE_MAX_RECURSION
#define ENABLE_MIN_IMPORTANCE

//#define ENABLE_TERMINATION_MESSAGES

//#define RUSSIAN_ROULETTE_TERMINATION
//static const Scalar RR_GAMMA_THRESHOLD = 0.2;
//static const Scalar OV_RR_GAMMA_THRESHOLD = 1.0 / RR_GAMMA_THRESHOLD;

using namespace RISE;
using namespace RISE::Implementation;

RayCaster::RayCaster( 
	const bool seeRadianceMap,
	const unsigned int maxR,
	const Scalar minI,
	const IShader& pDefaultShader_,
	const bool showLuminaires,
	const bool useiorstack,
	const bool chooseonlyoneluminaire
	) : 
  pScene( 0 ),
  pDefaultShader( pDefaultShader_ ),
  pLuminaryManager( 0 ),
  pLumSampling( 0 ),
  bConsiderRMapAsBackground( seeRadianceMap ),
  nMaxRecursions( maxR ),
  dMinImportance( minI ),
  bShowLuminaires( showLuminaires ),
  bIORStack( useiorstack ),
  bChooseOnlyOneLuminaire( chooseonlyoneluminaire )
{
	pDefaultShader.addref();
}

RayCaster::~RayCaster( )
{
	safe_release( pScene );

	pDefaultShader.release();

	safe_release( pLumSampling );
	safe_release( pLuminaryManager );
}

void RayCaster::AttachScene( const IScene* pScene_ )
{
	if( pScene == pScene_ ) {
		return;
	}

	if( pScene_ ) {
		safe_release( pScene );

		pScene = pScene_;
		pScene->addref();

		safe_release( pLuminaryManager );

		pLuminaryManager = new LuminaryManager( bChooseOnlyOneLuminaire );
		GlobalLog()->PrintNew( pLuminaryManager, __FILE__, __LINE__, "luminary manager" );
		pLuminaryManager->AttachScene( pScene );

		if( pLumSampling ) {
			pLuminaryManager->SetLuminaireSampling( pLumSampling );
		}
	}
}


bool RayCaster::CastRay( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
			) const
{
	if( bIORStack ) {
		IORStack ior_stack( 1.0 );
		return CastRay( rc, rast, ray, c, rs, distance, pRadianceMap, &ior_stack );
	}

	return CastRay( rc, rast, ray, c, rs, distance, pRadianceMap, 0 );
}

bool RayCaster::CastRay( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
			const IORStack* const ior_stack						///< [in/out] Index of refraction stack
			) const
{
#ifdef ENABLE_MAX_RECURSION
	if( rs.depth > nMaxRecursions )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "FORCED RECURSION TERMINATION" );
#endif

		return false;
	}
#endif

#ifdef ENABLE_MIN_IMPORTANCE
	if( rs.importance < dMinImportance )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "NOT IMPORTANT ENOUGH" );
#endif
		return false;
	}
#endif

#ifdef RUSSIAN_ROULETTE_TERMINATION
	if( rs.importance < RR_GAMMA_THRESHOLD ) {
		if( random.CanonicalRandom() > (rs.importance*OV_RR_GAMMA_THRESHOLD) ) {
			return false;
		}
	}
#endif

	bool bReturn = false;

	// Cast the ray into the scene
	RayIntersection	ri( ray, rast );
	pScene->GetObjects()->IntersectRay( ri, true, true, false );

	bool bHit = ri.geometric.bHit;

	if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
		if( ri.pMaterial && ri.pMaterial->GetEmitter() ) {
			bHit = bShowLuminaires;
		}
	}

	if( bHit )
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

		// Apply shade by calling the appropriate shader
		if( ri.pShader ) {
			ri.pShader->Shade( rc, ri, *this, rs, c, ior_stack );
		} else {
			pDefaultShader.Shade( rc, ri, *this, rs, c, ior_stack );
		}

		if( distance ) {
			*distance = ri.geometric.range;
		}

		bReturn = true;
	} else if( pRadianceMap ) {
		c = pRadianceMap->GetRadiance( ray, rast );
	} else if( pScene->GetGlobalRadianceMap() ) {
		c = pScene->GetGlobalRadianceMap()->GetRadiance( ray, rast );

		if( distance && bConsiderRMapAsBackground ) {
			*distance = INFINITY;
		}

		bReturn = bConsiderRMapAsBackground;
	}

	// After everything else is done, apply atmospherics if its there
	if( pScene->GetGlobalAtmosphere() && (bHit||pRadianceMap||pScene->GetGlobalRadianceMap()) ) {
		c = pScene->GetGlobalAtmosphere()->ApplyAtmospherics( ray, ri.geometric.ptIntersection, rast, c, !bHit );
	}


	return bReturn;
}

//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
bool RayCaster::CastRayNM( 
	const RuntimeContext& rc,							///< [in] The runtime context
	const RasterizerState& rast,						///< [in] Current state of the rasterizer
	const Ray& ray,										///< [in] Ray to cast
	Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
	const RAY_STATE& rs,								///< [in] The ray state
	const Scalar nm,									///< [in] Wavelength to cast
	Scalar* distance,									///< [in] If there was a hit, how far?
	const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
	) const
{
	if( bIORStack ) {
		IORStack ior_stack( 1.0 );
		return CastRayNM( rc, rast, ray, c, rs, nm, distance, pRadianceMap, &ior_stack );
	}

	return CastRayNM( rc, rast, ray, c, rs, nm, distance, pRadianceMap, 0 );
}

//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
bool RayCaster::CastRayNM( 
    const RuntimeContext& rc,							///< [in] The runtime context
	const RasterizerState& rast,						///< [in] Current state of the rasterizer
	const Ray& ray,										///< [in] Ray to cast
	Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
	const RAY_STATE& rs,								///< [in] The ray state
	const Scalar nm,									///< [in] Wavelength to cast
	Scalar* distance,									///< [in] If there was a hit, how far?
	const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
	const IORStack* const ior_stack						///< [in/out] Index of refraction stack
	) const
{
#ifdef ENABLE_MAX_RECURSION
	if( rs.depth > nMaxRecursions )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "FORCED RECURSION TERMINATION" );
#endif
		return false;
	}
#endif

#ifdef ENABLE_MIN_IMPORTANCE
	if( rs.importance < dMinImportance )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "NOT IMPORTANT ENOUGH" );
#endif
		return false;
	}
#endif

#ifdef RUSSIAN_ROULETTE_TERMINATION
	if( rs.importance < RR_GAMMA_THRESHOLD ) {
		if( random.CanonicalRandom() > (rs.importance*OV_RR_GAMMA_THRESHOLD) ) {
			return false;
		}
	}
#endif

	// Cast the ray into the scene
	RayIntersection	ri( ray, rast );
	pScene->GetObjects()->IntersectRay( ri, true, true, false );

	bool bHit = ri.geometric.bHit;

	if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
		if( ri.pMaterial && ri.pMaterial->GetEmitter() ) {
			bHit = bShowLuminaires;
		}
	}

	bool bReturn = false;

	if( bHit ) {
		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Set the current object on the IOR stack
		if( ior_stack ) {
			ior_stack->SetCurrentObject( ri.pObject );
		}

		// Apply shade by calling the appropriate shader
		if( ri.pShader ) {
			c = ri.pShader->ShadeNM( rc, ri, *this, rs, nm, ior_stack );
		} else {
			c = pDefaultShader.ShadeNM( rc, ri, *this, rs, nm, ior_stack );
		}

		if( distance ) {
			*distance = ri.geometric.range;
		}

		bReturn = true;
	} else if( pRadianceMap ) {
		// If there is a radiance map, return the radiance value
		// Transform the world co-ordinates to the texture lookup
		c = pRadianceMap->GetRadianceNM( ray, rast, nm );
	} else if( pScene->GetGlobalRadianceMap() ) {
		c = pScene->GetGlobalRadianceMap()->GetRadianceNM( ray, rast, nm );

		if( distance && bConsiderRMapAsBackground ) {
			*distance = INFINITY;
		}

		bReturn = bConsiderRMapAsBackground;
	}

	// After everything else is done, apply atmospherics if its there
	if( pScene->GetGlobalAtmosphere() && (bHit||pRadianceMap||pScene->GetGlobalRadianceMap()) ) {
		c = pScene->GetGlobalAtmosphere()->ApplyAtmosphericsNM( ray, ri.geometric.ptIntersection, rast, nm, c, !bHit );
	}

	return bReturn;
}

bool RayCaster::CastShadowRay( const Ray& ray, const Scalar dHowFar ) const
{
	if( !pScene ) {
		GlobalLog()->PrintSourceError( "RayCaster::CastRay_IntersectionOnly:: No scene", __FILE__, __LINE__ );
		return false;
	}
	
	return pScene->GetObjects()->IntersectShadowRay( ray, dHowFar, true, true );
}

//! Sets the luminaire sampler
void RayCaster::SetLuminaireSampling(
	ISampling2D* pLumSam							///< [in] Kernel to use for luminaire sampling
	)
{
	safe_release( pLumSampling );

	if( pLumSam ) {
		pLumSampling = pLumSam;
		pLumSampling->addref();
	}
}

