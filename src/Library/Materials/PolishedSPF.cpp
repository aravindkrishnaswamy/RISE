//////////////////////////////////////////////////////////////////////
//
//  PolishedSPF.cpp - Implementation of the polished SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PolishedSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

PolishedSPF::PolishedSPF( 
	const IPainter& Rd_, 
	const IPainter& tau_, 
	const IPainter& Nt_,
	const IPainter& s,
	const bool hg
	) : 
  Rd( Rd_ ), 
  tau( tau_ ),
  Nt( Nt_ ), 
  scat( s ),
  bHG( hg )
{
	Rd.addref();
	tau.addref();
	Nt.addref();
	scat.addref();
}

PolishedSPF::~PolishedSPF( )
{
	Rd.release();
	tau.release();
	Nt.release();
	scat.release();
}

Scalar PolishedSPF::GenerateScatteredRayFromPolish(
	ScatteredRay& dielectric,
	const Vector3 normal,										///< [in] Normal
	const Vector3 reflected,									///< [in] Reflected ray
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& random,										///< [in] Random numbers
	const Scalar scatfunc,
	const Scalar ior,
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	Vector3	vRefracted = ri.ray.dir;
	const Vector3	vIn = vRefracted;
    
	Scalar		Rs = 0.0;
	if( Optics::CalculateRefractedRay( normal, ior_stack?ior_stack->top():1.0, ior, vRefracted ) ) {
		Rs = Optics::CalculateDielectricReflectance( vIn, vRefracted, normal, ior_stack?ior_stack->top():1.0, ior );
	}

	Vector3	rv = reflected;

	// Generate one reflected ray from the polish
	Scalar alpha = 0;

	if( bHG ) {
		if( scatfunc<1 ) {
			const Scalar& g = scatfunc;
			do {
				const Scalar inner = (1.0 - g*g) / (1 - g + 2*g*random.x);
				alpha = acos( (1/(2.0*g)) * (1 + g*g - inner*inner) );
			} while( alpha < 0 || alpha > PI_OV_TWO );
		}
	} else {
		if( scatfunc < 1000000.0 ) {
			alpha = acos( pow(random.x, 1.0 / (scatfunc+1.0)) );
		}
	}

	// Use the warping function for a Phong based PDF
	if( alpha > 0 ) {
		rv = GeometricUtilities::Perturb(
			rv,
			alpha,
			TWO_PI * random.y
			);
	}

	dielectric.ray.Set( ri.ptIntersection, rv );

	return Rs;
}

void PolishedSPF::Scatter( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	dielectric;
	dielectric.type = ScatteredRay::eRayReflection;
	RISEPel Rs;

	RISEPel scattering = scat.GetColor(ri);
	RISEPel ior = Nt.GetColor(ri);

	const Vector3 n = Vector3Ops::Dot(ri.vNormal, ri.ray.dir)>0 ? -ri.vNormal : ri.vNormal;
	const Vector3 rv = Optics::CalculateReflectedRay( ri.ray.dir, n );

	if( scattering[0] == scattering[1] && scattering[1] == scattering[2] &&
		ior[0] == ior[1] && ior[1] == ior[2] )
	{
		Rs = GenerateScatteredRayFromPolish( dielectric, n, rv, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), scattering[0], ior[0], ior_stack );
		dielectric.kray = tau.GetColor(ri) * Rs;

		if( Vector3Ops::Dot( dielectric.ray.dir, ri.onb.w() ) > 0.0 ) {
			scattered.AddScatteredRay( dielectric );
		}
	}
	else 
	{
		Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );
		for( int i=0; i<3; i++ ) {
			Rs[i] = GenerateScatteredRayFromPolish( dielectric, n, rv, ri, ptrand, scattering[i], ior[i], ior_stack );
			dielectric.kray = 0;
			dielectric.kray[i] = tau.GetColor(ri)[i] * Rs[i];

			if( Vector3Ops::Dot( dielectric.ray.dir, ri.onb.w() ) > 0.0 ) {
				scattered.AddScatteredRay( dielectric );
			}
		}
	}

	if( ColorMath::MinValue(Rs) < 1.0 )
	{
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;

		// Generate a reflected ray with a cosine distribution
		diffuse.kray = Rd.GetColor(ri) * (1.0-Rs);
 		diffuse.ray.Set( 
			ri.ptIntersection,
			GeometricUtilities::CreateDiffuseVector( ri.onb, Point2(random.CanonicalRandom(),random.CanonicalRandom()) )
			);

		if( Vector3Ops::Dot( diffuse.ray.dir, ri.onb.w() ) > 0.0 ) {
			scattered.AddScatteredRay( diffuse );
		}
	}
}

void PolishedSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	dielectric;
	dielectric.type = ScatteredRay::eRayReflection;

	const Vector3 n = Vector3Ops::Dot(ri.vNormal, ri.ray.dir)>0 ? -ri.vNormal : ri.vNormal;
	const Vector3 rv = Optics::CalculateReflectedRay( ri.ray.dir, n );

	Scalar Rs = GenerateScatteredRayFromPolish( dielectric, n, rv, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), scat.GetColorNM(ri,nm), Nt.GetColorNM(ri,nm), ior_stack );
	dielectric.krayNM = tau.GetColorNM(ri,nm) * Rs;


	if( Vector3Ops::Dot( dielectric.ray.dir, ri.onb.w() ) > 0.0 ) {
		scattered.AddScatteredRay( dielectric );
	}

	if( Rs < 1.0 )
	{
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;

		// Generate a reflected ray with a cosine distribution
		diffuse.krayNM = Rd.GetColorNM(ri,nm) * (1.0-Rs);
		diffuse.ray.Set( 
			ri.ptIntersection,
			GeometricUtilities::CreateDiffuseVector( ri.onb, Point2(random.CanonicalRandom(),random.CanonicalRandom()) )
			);

		if( Vector3Ops::Dot( diffuse.ray.dir, ri.onb.w() ) > 0.0 ) {
			scattered.AddScatteredRay( diffuse );
		}
	}
}

