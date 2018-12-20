//////////////////////////////////////////////////////////////////////
//
//  DielectricSPF.cpp - Implementation of dielectric SPF
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
#include "DielectricSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

DielectricSPF::DielectricSPF( 
	const IPainter& tau_, 
	const IPainter& ri, 
	const IPainter& s,
	const bool hg
	) : 
  tau( tau_ ),
  rIndex( ri ),
  scat( s ),
  bHG( hg )
{
	tau.addref();
	rIndex.addref();
	scat.addref();
}

DielectricSPF::~DielectricSPF( )
{
	tau.release();
	rIndex.release();
	scat.release();
}

//! Returns true if there was reflection
Scalar DielectricSPF::GenerateScatteredRay( 
	ScatteredRay& dielectric,									///< [out] Scattered dielectric ray
	ScatteredRay& fresnel,										///< [out] Scattered fresnel or reflected ray
	bool& bDielectric,											///< [out] Dielectric ray exists?
	bool& bFresnel,												///< [out] Fresnel ray exists?
	const bool bFromInside,
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& random,										///< [in] Two canonical random numbers
	const Scalar scatfunc,
	const Scalar rIndex,
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	dielectric.type = ScatteredRay::eRayRefraction;
	fresnel.type = ScatteredRay::eRayReflection;

	Vector3	refracted = ri.ray.dir;
	Scalar		ref=0;

	bDielectric = bFresnel = true;

	if( bFromInside )
	{
		if( Optics::CalculateRefractedRay( -ri.onb.w(), rIndex, ior_stack?ior_stack->top():1.0, refracted ) ) {
			if( ior_stack ) {
				dielectric.ior_stack = new IORStack( *ior_stack );
				dielectric.ior_stack->pop();
				GlobalLog()->PrintNew( dielectric.ior_stack, __FILE__, __LINE__, "ior stack" );
			}
			ref = Optics::CalculateDielectricReflectance( ri.ray.dir, refracted, -ri.onb.w(), rIndex, ior_stack?ior_stack->top():1.0 );
		} else {
			// We're still in the material
			ref = 1.0;
		}
	}
	else
	{
		if( Optics::CalculateRefractedRay( ri.onb.w(), ior_stack?ior_stack->top():1.0, rIndex, refracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.dir, refracted, ri.onb.w(), ior_stack?ior_stack->top():1.0, rIndex );
			if( ior_stack ) {
				dielectric.ior_stack = new IORStack( *ior_stack );
				dielectric.ior_stack->push( rIndex );
				GlobalLog()->PrintNew( dielectric.ior_stack, __FILE__, __LINE__, "ior stack" );
			}
		} else {
			ref = 1.0;
		}
	}

	// reflect ray
	{
		if( bFromInside ) {
			if( ior_stack ) {
				fresnel.ior_stack = new IORStack( *ior_stack );
				GlobalLog()->PrintNew( fresnel.ior_stack, __FILE__, __LINE__, "ior stack" );
			}
			fresnel.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, ri.onb.w() ) );
		} else {
			fresnel.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, -ri.onb.w() ) );
		}
	}

	// refracted ray
	{
		dielectric.ray = Ray( ri.ptIntersection, refracted );

		Scalar alpha = 0;

		if( bHG ) {
			if( scatfunc<1 ) {
				const Scalar& g = scatfunc;
				const Scalar inner = (1.0 - g*g) / (1 - g + 2*g*random.x);
				alpha = acos( (1/(2.0*g)) * (1 + g*g - inner*inner) );
			}
		} else {
			if( scatfunc < 1000000.0 ) {
				alpha = acos( pow(random.x, 1.0 / (scatfunc+1.0)) );
			}
		}

		// Use the warping function for a Phong based PDF
		if( alpha > 0 && alpha < PI_OV_TWO ) {
			dielectric.ray.dir = GeometricUtilities::Perturb(
				dielectric.ray.dir,
				alpha,
				TWO_PI * random.y
				);
		}

		if( !bFromInside && Vector3Ops::Dot(dielectric.ray.dir, ri.onb.w()) > -NEARZERO ) {
			bDielectric = false;
		} else if( bFromInside && Vector3Ops::Dot(dielectric.ray.dir, ri.onb.w()) < NEARZERO ) {
			bDielectric = false;
		}
	}

	return ref;
}


void DielectricSPF::DoSingleRGBComponent( 
	 const RayIntersectionGeometric& ri,						///< [in] Geometric intersection details for point of intersection
	 const Point2& random,										///< [in] Two canonical random numbers
	 ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	 const IORStack* const ior_stack,							///< [in/out] Index of refraction stack
	 const int oneofthree,
	 const Scalar newIOR,
	 const Scalar scattering,
	 const Scalar cosine
	 ) const
{
	ScatteredRay dielectric;
	ScatteredRay fresnel;

	bool		bFromInside = false;

	if( cosine < NEARZERO ) {
		// We are coming from the inside of the object
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		bFromInside = true;

		if( oneofthree ) {
			dielectric.kray[oneofthree-1] = pow(tau.GetColor(ri)[oneofthree-1],distance);
		} else {
			dielectric.kray = ColorMath::pow(tau.GetColor(ri),distance);
		}
	} else {
		if( oneofthree ) {
			dielectric.kray[oneofthree-1] = 1.0;
		} else {
			dielectric.kray = RISEPel(1.0,1.0,1.0);	
		}
	}

	bool bDielectric, bFresnel;
	Scalar ref = GenerateScatteredRay( dielectric, fresnel, bDielectric, bFresnel, bFromInside, ri, random, scattering, newIOR, ior_stack );

	if( bDielectric && ref < 1.0 ) {
		if( oneofthree ) {
			dielectric.kray[oneofthree-1] = dielectric.kray[oneofthree-1] * (1.0-ref);
		} else {
			dielectric.kray = dielectric.kray * (1.0-ref);
		}

		scattered.AddScatteredRay( dielectric );
	}

	if( bFresnel ) {
		if( oneofthree ) {
			fresnel.kray[oneofthree-1] = ref;
		} else {
			fresnel.kray = RISEPel(ref,ref,ref);
		}

		scattered.AddScatteredRay( fresnel );
	}
}


void DielectricSPF::Scatter( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	Scalar		cosine = -Vector3Ops::Dot(ri.onb.w(), ri.ray.dir);
	
	const RISEPel ior = rIndex.GetColor(ri);
	const RISEPel scattering = scat.GetColor(ri);

	// Check to see if we have any dispersion
	const bool disperse = (ior[0] != ior[1]) || (ior[1] != ior[2]) || (scattering[0] != scattering[1]) || (scattering[1] != scattering[2]);

	if( !disperse ) {
		// No dispersion
		DoSingleRGBComponent( ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), scattered, ior_stack, false, ior[0], scattering[0], cosine );
	} else {
		// We have dispersion, so we must process each component seperately
		Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );
		for( int i=0; i<3; i++ ) {
			DoSingleRGBComponent( ri, ptrand, scattered, ior_stack, i+1, ior[i], scattering[i], cosine );
		}
	}
}

void DielectricSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay dielectric;
	ScatteredRay fresnel;

	Scalar		cosine = -Vector3Ops::Dot(ri.onb.w(), ri.ray.dir);
	bool		bFromInside = false;

	if( cosine < NEARZERO ) {
		// We are coming from the inside of the object
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		bFromInside = true;

		dielectric.krayNM = pow( tau.GetColorNM(ri,nm), distance );
	} else {
		dielectric.krayNM = 1.0;
	}

	bool bDielectric, bFresnel;
	const Scalar ref = GenerateScatteredRay( dielectric, fresnel, bDielectric, bFresnel, bFromInside, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), scat.GetColorNM(ri,nm), rIndex.GetColorNM(ri,nm), ior_stack );
	
	if( bDielectric && ref < 1.0 ) {
		dielectric.krayNM = dielectric.krayNM * (1.0-ref);
		scattered.AddScatteredRay( dielectric );
	}

	if( bFresnel && ref ) {
		fresnel.krayNM = ref;
		scattered.AddScatteredRay( fresnel );
	}
}
