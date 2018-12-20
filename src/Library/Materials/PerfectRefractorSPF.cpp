//////////////////////////////////////////////////////////////////////
//
//  PerfectRefractorSPF.cpp - Implementation of the perfect
//  refractor SPF
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
#include "PerfectRefractorSPF.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

PerfectRefractorSPF::PerfectRefractorSPF(
	const IPainter& ref, 
	const IPainter& Nt_ 
	) :
  refractivity( ref ),
  Nt( Nt_ )
{
	refractivity.addref();
	Nt.addref();
}

PerfectRefractorSPF::~PerfectRefractorSPF( )
{
	refractivity.release();
	Nt.release();
}

void PerfectRefractorSPF::DoSingleRGBComponent( 
	 const RayIntersectionGeometric& ri,						///< [in] Geometric intersection details for point of intersection
	 ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	 const IORStack* const ior_stack,							///< [in/out] Index of refraction stack
	 const int oneofthree,
	 const Scalar newIOR,
	 const Scalar cosine
	 ) const
{
	ScatteredRay specular;
	ScatteredRay fresnel;

	fresnel.type = ScatteredRay::eRayReflection;
	specular.type = ScatteredRay::eRayRefraction;

	Vector3	vRefracted = ri.ray.dir;

	Scalar ref = 0;
	if( cosine < NEARZERO )
	{
		// Going in 
		if( Optics::CalculateRefractedRay( ri.onb.w(), ior_stack?ior_stack->top():1.0, newIOR, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.dir, vRefracted, ri.onb.w(), ior_stack?ior_stack->top():1.0, newIOR );
			if( ior_stack ) {
				specular.ior_stack = new IORStack( *ior_stack );
				specular.ior_stack->push( newIOR );
				GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );
			}
		} else {
			// TIR, so reflect
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, ri.onb.w() ) );
		}
	}
	else
	{
		if( ior_stack ) {
			specular.ior_stack = new IORStack( *ior_stack );
			specular.ior_stack->pop();
			GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );
		}

		// Coming out, IOR becomes air
		if( Optics::CalculateRefractedRay( -ri.onb.w(), newIOR, specular.ior_stack?specular.ior_stack->top():1.0, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.dir, vRefracted, -ri.onb.w(), newIOR, specular.ior_stack?specular.ior_stack->top():1.0 );
		} else {
			// TIR, so reflect
			// We're still in the material
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			if( ior_stack ) {
				fresnel.ior_stack = new IORStack( *ior_stack );
				GlobalLog()->PrintNew( fresnel.ior_stack, __FILE__, __LINE__, "ior stack" );
			}
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, -ri.onb.w() ) );
		}
	}
 
	if( ref < 1.0 ) {
		specular.ray.Set( ri.ptIntersection, vRefracted );
		if( oneofthree ) {
			specular.kray[oneofthree-1] = refractivity.GetColor(ri)[oneofthree-1] * ((1.0-ref));
		} else {
			specular.kray = refractivity.GetColor(ri) * (1.0-ref);
		}

		scattered.AddScatteredRay( specular );
	}

	if( ref > 0.0 ) {
		if( oneofthree ) {
			fresnel.kray[oneofthree-1] = ref;
		} else {
			fresnel.kray = RISEPel(ref,ref,ref);
		}
		
		scattered.AddScatteredRay( fresnel );
	}
}


void PerfectRefractorSPF::Scatter( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	Scalar		cosine = Vector3Ops::Dot( ri.onb.w(), ri.ray.dir );
	
	const RISEPel ior = Nt.GetColor(ri);

	// Check to see if we have any dispersion
	if( (ior[0] == ior[1]) && (ior[1] == ior[2]) ) {
		// No dispersion
		DoSingleRGBComponent( ri, scattered, ior_stack, false, ior[0], cosine );
	} else {
		// We have dispersion, so we must process each component seperately
		for( int i=0; i<3; i++ ) {
			DoSingleRGBComponent( ri, scattered, ior_stack, i+1, ior[i], cosine );
		}
	}
}

void PerfectRefractorSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay specular;
	ScatteredRay fresnel;

	fresnel.type = ScatteredRay::eRayReflection;
	specular.type = ScatteredRay::eRayRefraction;

	Scalar		cosine = Vector3Ops::Dot( ri.onb.w(), ri.ray.dir );
	Vector3	vRefracted = ri.ray.dir;

	Scalar newIOR = Nt.GetColorNM(ri,nm);

	Scalar ref = 0;
	if( cosine < NEARZERO )
	{
		// Going in 
		if( Optics::CalculateRefractedRay(ri. onb.w(), ior_stack?ior_stack->top():1.0, newIOR, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.dir, vRefracted, ri.onb.w(), ior_stack?ior_stack->top():1.0, newIOR );
			if( ior_stack ) {
				specular.ior_stack = new IORStack( *ior_stack );
				specular.ior_stack->push( newIOR );
				GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );
			}
		} else {
			// TIR, so reflect
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, ri.onb.w() ) );
		}
	}
	else
	{
		if( ior_stack ) {
			specular.ior_stack = new IORStack( *ior_stack );
			specular.ior_stack->pop();
			GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );
		}

		// Coming out, IOR becomes whatever was there before
		if( Optics::CalculateRefractedRay( -ri.onb.w(), newIOR, ior_stack?ior_stack->top():1.0, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.dir, vRefracted, -ri.onb.w(), newIOR, ior_stack?ior_stack->top():1.0 );
		} else {
			// TIR, so reflect
			// We're still in the material
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			if( ior_stack ) {
				fresnel.ior_stack = new IORStack( *ior_stack );
				GlobalLog()->PrintNew( fresnel.ior_stack, __FILE__, __LINE__, "ior stack" );
			}
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, -ri.onb.w() ) );
		}
	}
 
	if( ref < 1.0 ) {
		specular.ray.Set( ri.ptIntersection, vRefracted );
		specular.krayNM = refractivity.GetColorNM(ri,nm) * (1.0-ref);

		scattered.AddScatteredRay( specular );
	}

	if( ref > 0.0 ) {
		fresnel.krayNM = ref;

		scattered.AddScatteredRay( fresnel );
	}
}

