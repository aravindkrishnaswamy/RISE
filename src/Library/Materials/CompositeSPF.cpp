//////////////////////////////////////////////////////////////////////
//
//  CompositeSPF.cpp - Implementation of the Composite SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CompositeSPF.h"

using namespace RISE;
using namespace RISE::Implementation;

CompositeSPF::CompositeSPF( 
	const ISPF& top_, 
	const ISPF& bottom_, 
	const unsigned int max_recur_ ,
	const unsigned int max_reflection_recursion_,		// maximum level of reflection recursion
	const unsigned int max_refraction_recursion_,		// maximum level of refraction recursion
	const unsigned int max_diffuse_recursion_,			// maximum level of diffuse recursion
	const unsigned int max_translucent_recursion_,		// maximum level of translucent recursion
	const Scalar thickness_								// thickness between the materials
	) :
  top( top_ ),
  bottom( bottom_ ),
  max_recur( max_recur_ ),
  max_reflection_recursion( max_reflection_recursion_ ),
  max_refraction_recursion( max_refraction_recursion_ ),
  max_diffuse_recursion( max_diffuse_recursion_ ),
  max_translucent_recursion( max_translucent_recursion_ ),
  thickness( thickness_ )
{
	top.addref();
	bottom.addref();
}

CompositeSPF::~CompositeSPF( )
{
	top.release();
	bottom.release();
}

bool CompositeSPF::ShouldScatteredRayBePropagated(
	const ScatteredRay::ScatRayType type,
	const unsigned int steps
	) const
{
	switch( type )
	{
	case ScatteredRay::eRayReflection:
		return (steps<max_reflection_recursion);
		break;
	case ScatteredRay::eRayRefraction:
		return (steps<max_refraction_recursion);
		break;
	case ScatteredRay::eRayDiffuse:
		return (steps<max_diffuse_recursion);
		break;
	case ScatteredRay::eRayTranslucent:
		return (steps<max_translucent_recursion);
		break;
	default:
		return false;
		break;
	};

	return false;
}

void CompositeSPF::ProcessTopLayer(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RISEPel& importance,									///< [in] Importance from prevous pass
				const RandomNumberGenerator& random,				///< Random number generator for the MC process
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const unsigned int steps,									///< [in] Number of steps taken in the random walk process
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const
{
	if( steps >= max_recur ) {
		return;
	}

	ScatteredRayContainer scat_top;
	top.Scatter( ri, random, scat_top, ior_stack );

	for( unsigned int i=0; i<scat_top.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_top[i].ray.dir, ri.onb.w() ) >= 0 ) {
			// Exits from the top, so its all good
			scat_top[i].kray = scat_top[i].kray * importance;
			scattered.AddScatteredRay( scat_top[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_top[i].type, steps ) ) {
				// We must pass it off to the bottom
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.dir = Vector3Ops::Normalize(scat_top[i].ray.dir);
				my_ri.ray.Advance( thickness );
				ProcessBottomLayer( my_ri, scat_top[i].kray*importance, random, scattered, steps+1, ior_stack );
			}
		}
	}
}

void CompositeSPF::ProcessBottomLayer(
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RISEPel& importance,									///< [in] Importance from prevous pass
		const RandomNumberGenerator& random,				///< Random number generator for the MC process
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const unsigned int steps,									///< [in] Number of steps taken in the random walk process
		const IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
{
	if( steps >= max_recur ) {
		return;
	}

	ScatteredRayContainer scat_bottom;
	bottom.Scatter( ri, random, scat_bottom, ior_stack );

	for( unsigned int i=0; i<scat_bottom.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_bottom[i].ray.dir,ri. onb.w() ) <= 0 ) {
			// Exits from the bottom, so its all good
			scat_bottom[i].kray = scat_bottom[i].kray * importance;
			scattered.AddScatteredRay( scat_bottom[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_bottom[i].type, steps ) ) {
				// We must pass it back to the top
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.dir = Vector3Ops::Normalize(scat_bottom[i].ray.dir);
				my_ri.ray.Advance( thickness );
				ProcessTopLayer( my_ri, scat_bottom[i].kray*importance, random, scattered, steps+1, ior_stack );
			}
		}
	}
}

void CompositeSPF::ProcessTopLayerNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const Scalar importance,									///< [in] Importance from prevous pass
				const RandomNumberGenerator& random,				///< Random number generator for the MC process
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const unsigned int steps,									///< [in] Number of steps taken in the random walk process
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const
{
	if( steps >= max_recur ) {
		return;
	}

	ScatteredRayContainer scat_top;
	top.ScatterNM( ri, random, nm, scat_top, ior_stack );

	for( unsigned int i=0; i<scat_top.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_top[i].ray.dir, ri.onb.w() ) >= 0 ) {
			// Exits from the top, so its all good
			scat_top[i].krayNM *= importance;
			scattered.AddScatteredRay( scat_top[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_top[i].type, steps ) ) {
				// We must pass it off to the bottom
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.dir = Vector3Ops::Normalize(scat_top[i].ray.dir);
				my_ri.ray.Advance( thickness );
				ProcessBottomLayerNM( my_ri, scat_top[i].krayNM*importance, random, nm, scattered, steps+1, ior_stack );
			}
		}
	}
}

void CompositeSPF::ProcessBottomLayerNM(
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const Scalar importance,									///< [in] Importance from prevous pass
		const RandomNumberGenerator& random,				///< Random number generator for the MC process
		const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const unsigned int steps,									///< [in] Number of steps taken in the random walk process
		const IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
{
	if( steps >= max_recur ) {
		return;
	}

	ScatteredRayContainer scat_bottom;
	bottom.ScatterNM( ri, random, nm, scat_bottom, ior_stack );

	for( unsigned int i=0; i<scat_bottom.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_bottom[i].ray.dir, ri.onb.w() ) <= 0 ) {
			// Exits from the bottom, so its all good
			scat_bottom[i].krayNM *= importance;
			scattered.AddScatteredRay( scat_bottom[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_bottom[i].type, steps ) ) {
				// We must pass it back to the top
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.dir = Vector3Ops::Normalize(scat_bottom[i].ray.dir);
				my_ri.ray.Advance( thickness );
				ProcessTopLayerNM( my_ri, scat_bottom[i].krayNM*importance, random, nm, scattered, steps+1, ior_stack );
			}
		}
	}
}

void CompositeSPF::Scatter(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			const RandomNumberGenerator& random,				///< [in] Random number generator
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack* const ior_stack								///< [in/out] Index of refraction stack
			) const
{
	// We do a random walk process between the materials until the rays
	// either exit the bottom material from the bottom, or exit the
	// top material from the top
	if( Vector3Ops::Dot( ri.ray.dir, ri.onb.w() ) <= 0 ) {
		ProcessTopLayer( ri, RISEPel(1,1,1), random, scattered, 0, ior_stack );
	} else {
		ProcessBottomLayer( ri, RISEPel(1,1,1), random, scattered, 0, ior_stack );
	}

	for( unsigned int i=0; i<scattered.Count(); i++ ) {
		// To account for thicknesses
		scattered[i].ray.origin = ri.ptIntersection;
	}
}

void CompositeSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	// We do a random walk process between the materials until the rays
	// either exit the bottom material from the bottom, or exit the
	// top material from the top
	if( Vector3Ops::Dot( ri.ray.dir, ri.onb.w() ) <= 0 ) {
		ProcessTopLayerNM( ri, 1, random, nm, scattered, 0, ior_stack );
	} else {
		ProcessBottomLayerNM( ri, 1, random, nm, scattered, 0, ior_stack );
	}

	for( unsigned int i=0; i<scattered.Count(); i++ ) {
		// To account for thicknesses
		scattered[i].ray.origin = ri.ptIntersection;
	}
}

