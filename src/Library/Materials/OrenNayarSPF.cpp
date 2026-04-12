//////////////////////////////////////////////////////////////////////
//
//  OrenNayarSPF.cpp - Implementation of the Oren-Nayar SPF
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
#include "OrenNayarSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

OrenNayarSPF::OrenNayarSPF(
	const IPainter& reflectance, 
	const IPainter& roughness
	) :
  pReflectance( reflectance ),
  pRoughness( roughness )
{
	pReflectance.addref();
	pRoughness.addref();
}
OrenNayarSPF::~OrenNayarSPF( )
{
	pReflectance.release();
	pRoughness.release();
}

void OrenNayarSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );

	ScatteredRay	diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;

	OrthonormalBasis3D	myonb = ri.onb;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();				
	}

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );

	// Compute the weight
	RISEPel L1, L2;
	OrenNayarBRDF::ComputeFactor<RISEPel>( L1, L2, diffuse.ray.Dir(), ri, ri.onb.w(), pRoughness.GetColor(ri) );

	const RISEPel rho = pReflectance.GetColor(ri);
	diffuse.kray = L1*rho + (L2*rho*rho);

	// Set the sampling PDF: cosine-weighted hemisphere = cos(theta) / pi
	diffuse.pdf = fabs( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) ) * INV_PI;
	diffuse.isDelta = false;

	scattered.AddScatteredRay( diffuse );
}

void OrenNayarSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );

	ScatteredRay	diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;

	OrthonormalBasis3D	myonb = ri.onb;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();				
	}

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	
	// Compute the weight
	Scalar L1=0, L2=0;
	OrenNayarBRDF::ComputeFactor( L1, L2, diffuse.ray.Dir(), ri, myonb.w(), pRoughness.GetColorNM(ri,nm) );

	const Scalar rho = pReflectance.GetColorNM(ri,nm);
	diffuse.krayNM = L1*rho + (L2*rho*rho);

	// Set the sampling PDF: cosine-weighted hemisphere = cos(theta) / pi
	diffuse.pdf = fabs( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) ) * INV_PI;
	diffuse.isDelta = false;

	scattered.AddScatteredRay( diffuse );
}

Scalar OrenNayarSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	// Cosine-weighted hemisphere PDF = cos(theta) / pi
	// Same as Lambertian since the sampling strategy is identical
	const bool bFrontFace = Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) <= NEARZERO;
	const Scalar cosTheta = bFrontFace ?
		Vector3Ops::Dot( wo, ri.onb.w() ) :
		-Vector3Ops::Dot( wo, ri.onb.w() );
	return (cosTheta > 0) ? cosTheta * INV_PI : 0;
}

Scalar OrenNayarSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	// Oren-Nayar sampling PDF is wavelength-independent
	return Pdf( ri, wo, ior_stack );
}

