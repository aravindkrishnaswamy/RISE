//////////////////////////////////////////////////////////////////////
//
//  PerfectReflectorSPF.cpp - Implementation of the perfect
//  reflector SPF
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
#include "PerfectReflectorSPF.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

PerfectReflectorSPF::PerfectReflectorSPF( const IPainter& R_ ) :
  reflectivity( R_ )
{
	reflectivity.addref();
}

PerfectReflectorSPF::~PerfectReflectorSPF( )
{
	reflectivity.release();
}

void PerfectReflectorSPF::Scatter( 
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RandomNumberGenerator& random,				///< [in] Random number generator
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
{
	ScatteredRay specular;
	specular.type = ScatteredRay::eRayReflection;

	specular.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, ri.onb.w() ) );
	specular.kray = reflectivity.GetColor(ri);
	
	if( Vector3Ops::Dot( specular.ray.dir, ri.onb.w() ) > 0.0 ) {
		scattered.AddScatteredRay( specular );
	}
}

void PerfectReflectorSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay specular;
	specular.type = ScatteredRay::eRayReflection;

	specular.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.dir, ri.onb.w() ) );
	specular.krayNM = reflectivity.GetColorNM(ri,nm);
	
	if( Vector3Ops::Dot( specular.ray.dir, ri.onb.w() ) > 0.0 ) {
		scattered.AddScatteredRay( specular );
	}
}

