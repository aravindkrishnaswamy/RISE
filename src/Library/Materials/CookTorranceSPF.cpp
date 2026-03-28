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
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );

	ScatteredRay	diffuse;
	ScatteredRay	specular;
	diffuse.type = ScatteredRay::eRayDiffuse;
	specular.type = ScatteredRay::eRayReflection;

	OrthonormalBasis3D	myonb = ri.onb;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	specular.ray = diffuse.ray;

	// Both diffuse and specular use cosine-weighted hemisphere sampling
	const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), myonb.w() );
	const Scalar cosinePdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;

	// Compute the weight
	const RISEPel fresnel = Optics::CalculateConductorReflectance( ri.ray.Dir(), myonb.w(), RISEPel(1,1,1), pIOR.GetColor(ri), pExtinction.GetColor(ri) );

	if( ColorMath::MaxValue(fresnel) > 0 ) {
		RISEPel factor = CookTorranceBRDF::ComputeFactor<RISEPel>( diffuse.ray.Dir(), ri, myonb.w(), pMasking.GetColor(ri) );
		ColorMath::Clamp( factor, 0, 1 );
		if( ColorMath::MaxValue( factor ) > 0 ) {
			specular.kray = pSpecular.GetColor(ri) * fresnel * factor;
			specular.pdf = cosinePdf;
			specular.isDelta = false;
			scattered.AddScatteredRay( specular );
		}
	}

	diffuse.kray = pDiffuse.GetColor(ri);
	diffuse.pdf = cosinePdf;
	diffuse.isDelta = false;
	scattered.AddScatteredRay( diffuse );
}

void CookTorranceSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );

	ScatteredRay	diffuse;
	ScatteredRay	specular;
	diffuse.type = ScatteredRay::eRayDiffuse;
	specular.type = ScatteredRay::eRayReflection;

	OrthonormalBasis3D	myonb = ri.onb;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	specular.ray = diffuse.ray;

	// Both use cosine-weighted hemisphere sampling
	const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), myonb.w() );
	const Scalar cosinePdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;

	// Compute the weight
	const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.Dir(), myonb.w(), 1.0, pIOR.GetColorNM(ri,nm), pExtinction.GetColorNM(ri,nm) );

	if( fresnel > 0 ) {
		const Scalar factor = CookTorranceBRDF::ComputeFactor( diffuse.ray.Dir(), ri, myonb.w(), pMasking.GetColorNM(ri,nm) );

		if( factor > 0 ) {
			specular.krayNM = pSpecular.GetColorNM(ri,nm) * fresnel * factor;
			specular.pdf = cosinePdf;
			specular.isDelta = false;
			scattered.AddScatteredRay( specular );
		}
	}

	diffuse.krayNM = pDiffuse.GetColorNM(ri,nm);
	diffuse.pdf = cosinePdf;
	diffuse.isDelta = false;
	scattered.AddScatteredRay( diffuse );
}

Scalar CookTorranceSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	// Both diffuse and specular components use cosine-weighted hemisphere sampling
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, myonb.w() );
	if( cosTheta <= 0 ) {
		return 0;
	}

	return cosTheta * INV_PI;
}

Scalar CookTorranceSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	// Both diffuse and specular components use cosine-weighted hemisphere sampling
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, myonb.w() );
	if( cosTheta <= 0 ) {
		return 0;
	}

	return cosTheta * INV_PI;
}
