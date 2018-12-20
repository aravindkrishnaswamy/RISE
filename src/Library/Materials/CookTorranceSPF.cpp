//////////////////////////////////////////////////////////////////////
//
//  CookTorranceSPF.cpp - Implementation of the Cook-Torrance SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CookTorranceSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

CookTorranceSPF::CookTorranceSPF(
	const IPainter& diffuse, 
	const IPainter& specular, 
	const IPainter& masking,
	const IPainter& ior,
	const IPainter& ext
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pMasking( masking ),
  pIOR( ior ),
  pExtinction( ext )
{
	pDiffuse.addref();
	pSpecular.addref();
	pMasking.addref();
	pIOR.addref();
	pExtinction.addref();
}
CookTorranceSPF::~CookTorranceSPF( )
{
	pDiffuse.release();
	pSpecular.release();
	pMasking.release();
	pIOR.release();
	pExtinction.release();
}

void CookTorranceSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );

	ScatteredRay	diffuse;
	ScatteredRay	specular;
	diffuse.type = ScatteredRay::eRayDiffuse;
	specular.type = ScatteredRay::eRayReflection;

	OrthonormalBasis3D	myonb = ri.onb;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.dir, ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();				
	}

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	specular.ray = diffuse.ray;

	// Compute the weight
	const RISEPel fresnel = Optics::CalculateConductorReflectance( ri.ray.dir, myonb.w(), RISEPel(1,1,1), pIOR.GetColor(ri), pExtinction.GetColor(ri) );

	if( ColorMath::MaxValue(fresnel) > 0 ) {
		RISEPel factor = CookTorranceBRDF::ComputeFactor<RISEPel>( diffuse.ray.dir, ri, myonb.w(), pMasking.GetColor(ri) );
		ColorMath::Clamp( factor, 0, 1 );
		if( ColorMath::MaxValue( factor ) > 0 ) {
			specular.kray = pSpecular.GetColor(ri) * fresnel * factor;
			scattered.AddScatteredRay( specular );
		}
	}

	diffuse.kray = pDiffuse.GetColor(ri);
	scattered.AddScatteredRay( diffuse );
}

void CookTorranceSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );

	ScatteredRay	diffuse;
	ScatteredRay	specular;
	diffuse.type = ScatteredRay::eRayDiffuse;
	specular.type = ScatteredRay::eRayReflection;

	OrthonormalBasis3D	myonb = ri.onb;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.dir, ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();				
	}

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	specular.ray = diffuse.ray;

	// Compute the weight
	const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.dir, myonb.w(), 1.0, pIOR.GetColorNM(ri,nm), pExtinction.GetColorNM(ri,nm) );

	if( fresnel > 0 ) {
		const Scalar factor = CookTorranceBRDF::ComputeFactor( diffuse.ray.dir, ri, myonb.w(), pMasking.GetColorNM(ri,nm) );

		if( factor > 0 ) {
			specular.krayNM = pSpecular.GetColorNM(ri,nm) * fresnel * factor;
			scattered.AddScatteredRay( specular );
		}
	}

	diffuse.krayNM = pDiffuse.GetColorNM(ri,nm);
	scattered.AddScatteredRay( diffuse );
}

