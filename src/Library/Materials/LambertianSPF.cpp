//////////////////////////////////////////////////////////////////////
//
//  LambertianSPF.cpp - Implementation of the lambertian SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LambertianSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

LambertianSPF::LambertianSPF( const IPainter& ref ) :
  reflectance( ref )
{
	reflectance.addref();
}

LambertianSPF::~LambertianSPF( )
{
	reflectance.release();
}

void LambertianSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );

	ScatteredRay	diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO )
	{
		OrthonormalBasis3D	myonb = ri.onb;
		myonb.FlipW();				
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	} else {
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( ri.onb, ptrand ) );
	}

	diffuse.kray = reflectance.GetColor(ri);

	// Set the sampling PDF: cosine-weighted hemisphere = cos(theta) / pi
	diffuse.pdf = fabs( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) ) * INV_PI;
	diffuse.isDelta = false;

	scattered.AddScatteredRay( diffuse );
}

void LambertianSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );

	ScatteredRay	diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;

	// Generate a reflected ray randomly with a cosine distribution
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO )
	{
		OrthonormalBasis3D	myonb = ri.onb;
		myonb.FlipW();				
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	} else {
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( ri.onb, ptrand ) );
	}
	
	diffuse.krayNM = reflectance.GetColorNM(ri, nm);

	// Set the sampling PDF: cosine-weighted hemisphere = cos(theta) / pi
	diffuse.pdf = fabs( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) ) * INV_PI;
	diffuse.isDelta = false;

	scattered.AddScatteredRay( diffuse );
}

Scalar LambertianSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	// Cosine-weighted hemisphere PDF = cos(theta) / pi
	// Use the geometric normal to determine which hemisphere we're scattering into
	const bool bFrontFace = Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) <= NEARZERO;
	const Scalar cosTheta = bFrontFace ?
		Vector3Ops::Dot( wo, ri.onb.w() ) :
		-Vector3Ops::Dot( wo, ri.onb.w() );
	return (cosTheta > 0) ? cosTheta * INV_PI : 0;
}

Scalar LambertianSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	// Lambertian PDF is wavelength-independent
	return Pdf( ri, wo, ior_stack );
}

