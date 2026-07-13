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
  pReflectivity( &R_ )
{
	pReflectivity->addref();
}

PerfectReflectorSPF::~PerfectReflectorSPF( )
{
	safe_release( pReflectivity );
}

void PerfectReflectorSPF::SetReflectance( const IPainter& R )
{
	R.addref();
	safe_release( pReflectivity );
	pReflectivity = &R;
}

void PerfectReflectorSPF::Scatter( 
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		ISampler& sampler,				///< [in] Sampler
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const IORStack& ior_stack								///< [in/out] Index of refraction stack
		) const
{
	OrthonormalBasis3D myonb = ri.onb;

	// Ensure normal faces the incoming ray (matches GGXSPF / the rest of the
	// geometric-horizon sweep).  This file previously reflected around the
	// raw, un-flipped ri.onb.w(), which silently swallowed every back-face
	// hit (a mirror seen from behind rendered as a black facet).
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const Vector3 n = myonb.w();

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a reflection direction that
	// validates against the (tilted) shading normal can still point below
	// the geometric surface -- the continuation ray then tunnels into the
	// solid.  Degenerate vGeomNormal (SquaredModulus guard, matches
	// GlintModifier.cpp) falls back to the shading normal, making the gate
	// a no-op.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, n ) >= 0 ) ? geomNRaw : -geomNRaw;

	ScatteredRay specular;
	specular.type = ScatteredRay::eRayReflection;
	specular.isDelta = true;
	specular.pdf = 1.0;

	specular.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), n ) );
	specular.kray = pReflectivity->GetColor(ri);

	if( Vector3Ops::Dot( specular.ray.Dir(), geomN ) <= 0 ) {
		// A perfect mirror has no companion lobe to carry the energy -- there
		// is nothing else in the container, so dropping here would be total,
		// deterministic energy loss (a mirror facet that returns nothing is a
		// black hole).  Re-derive the reflection direction about the TRUE
		// geometric normal instead of the shading normal.  This is guaranteed
		// to satisfy the gate (no re-check needed): for a ray arriving against
		// geomN, dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0.
		specular.ray.SetDir( Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
	}

	scattered.AddScatteredRay( specular );
}

void PerfectReflectorSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;

	// Ensure normal faces the incoming ray (mirrors Scatter()).
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const Vector3 n = myonb.w();

	// Geometric-horizon gate (mirrors Scatter()).
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, n ) >= 0 ) ? geomNRaw : -geomNRaw;

	ScatteredRay specular;
	specular.type = ScatteredRay::eRayReflection;
	specular.isDelta = true;
	specular.pdf = 1.0;

	specular.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), n ) );
	specular.krayNM = pReflectivity->GetColorNM(ri,nm);

	if( Vector3Ops::Dot( specular.ray.Dir(), geomN ) <= 0 ) {
		// Mandatory (no companion lobe) -- see Scatter() for the rationale.
		specular.ray.SetDir( Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
	}

	scattered.AddScatteredRay( specular );
}

Scalar PerfectReflectorSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return 0;
}

Scalar PerfectReflectorSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return 0;
}

