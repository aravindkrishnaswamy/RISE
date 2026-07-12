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
	const IScalarPainter& roughness
	) :
  pReflectance( &reflectance ),
  pRoughness( &roughness )
{
	pReflectance->addref();
	pRoughness->addref();
}
OrenNayarSPF::~OrenNayarSPF( )
{
	safe_release( pReflectance );
	safe_release( pRoughness );
}

void OrenNayarSPF::SetReflectance( const IPainter& v )
{
	v.addref();
	safe_release( pReflectance );
	pReflectance = &v;
}

void OrenNayarSPF::SetRoughness( const IScalarPainter& v )
{
	v.addref();
	safe_release( pRoughness );
	pRoughness = &v;
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

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a sampled direction that validates
	// against the (tilted) shading normal can still point below the
	// geometric surface -- the continuation ray then tunnels into the solid
	// (a zero-weight ray would still be traced).  Oriented to myonb.w()
	// (the normal the lobe is sampled around).  Degenerate vGeomNormal
	// (SquaredModulus guard, matches GlintModifier.cpp) falls back to the
	// shading normal, making the gate a no-op.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, myonb.w() ) >= 0 ) ? geomNRaw : -geomNRaw;

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );

	// Compute the weight
	RISEPel L1, L2;
	const ScalarTriple r = pRoughness->GetValuesAt(ri);
	const RISEPel roughness( r.v[0], r.v[1], r.v[2] );
	OrenNayarBRDF::ComputeFactor<RISEPel>( L1, L2, diffuse.ray.Dir(), ri, ri.onb.w(), roughness );

	const RISEPel rho = pReflectance->GetColor(ri);
	diffuse.kray = L1*rho + (L2*rho*rho);

	// Set the sampling PDF: cosine-weighted hemisphere = cos(theta) / pi
	diffuse.pdf = fabs( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) ) * INV_PI;
	diffuse.isDelta = false;

	if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) > 0 ) {
		scattered.AddScatteredRay( diffuse );
	}
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

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a sampled direction that validates
	// against the (tilted) shading normal can still point below the
	// geometric surface -- the continuation ray then tunnels into the solid
	// (a zero-weight ray would still be traced).  Oriented to myonb.w()
	// (the normal the lobe is sampled around).  Degenerate vGeomNormal
	// (SquaredModulus guard, matches GlintModifier.cpp) falls back to the
	// shading normal, making the gate a no-op.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, myonb.w() ) >= 0 ) ? geomNRaw : -geomNRaw;

	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	
	// Compute the weight
	Scalar L1=0, L2=0;
	OrenNayarBRDF::ComputeFactor( L1, L2, diffuse.ray.Dir(), ri, myonb.w(), pRoughness->GetValueAtNM(ri,nm) );

	const Scalar rho = pReflectance->GetColorNM(ri,nm);
	diffuse.krayNM = L1*rho + (L2*rho*rho);

	// Set the sampling PDF: cosine-weighted hemisphere = cos(theta) / pi
	diffuse.pdf = fabs( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) ) * INV_PI;
	diffuse.isDelta = false;

	if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) > 0 ) {
		scattered.AddScatteredRay( diffuse );
	}
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

	// Geometric-horizon gate (MIS consistency with Scatter's sampler-side
	// gate): a wo the sampler can no longer emit contributes zero density.
	const Vector3 nEff = bFrontFace ? ri.onb.w() : -ri.onb.w();
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : nEff;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, nEff ) >= 0 ) ? geomNRaw : -geomNRaw;

	return (cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0) ? cosTheta * INV_PI : 0;
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

