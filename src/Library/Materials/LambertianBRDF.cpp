//////////////////////////////////////////////////////////////////////
//
//  LambertianBRDF.cpp - Implements the lambertian BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LambertianBRDF.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

LambertianBRDF::LambertianBRDF( const IPainter& reflectance ) :
  pReflectance( &reflectance )
{
	pReflectance->addref();
}

LambertianBRDF::~LambertianBRDF( )
{
	safe_release( pReflectance );
}

void LambertianBRDF::SetReflectance( const IPainter& reflectance )
{
	// Order matters: addref the new painter BEFORE releasing the old
	// one, so a self-rebind (SetReflectance(*pReflectance)) doesn't
	// destroy the painter mid-swap.  Even though SceneEditController
	// parks the render thread around this call, the addref-then-
	// release ordering keeps the function safe in isolation.
	reflectance.addref();
	safe_release( pReflectance );
	pReflectance = &reflectance;
}

static bool ShouldReflect( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n )
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr <= -NEARZERO) &&		// viewer is in front	
		(nv <= -NEARZERO) ) {		// light is in front
		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  Oriented to this branch's effective normal
		// (-n: both cosines are negative against the raw n here).  Degenerate
		// vGeomNormal falls back to the shading normal (gate is a no-op).
		const Vector3 nEff = -n;
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : nEff;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, nEff ) >= 0 ) ? geomNRaw : -geomNRaw;
		return Vector3Ops::Dot( v, geomN ) > 0 && Vector3Ops::Dot( r, geomN ) > 0;
	} else if( (nr >= NEARZERO) &&	// viewer is behind
				(nv >= NEARZERO) ) {	// light is behind
		// Geometric-horizon gate (mirrors the branch above; effective
		// normal here is +n).
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, n ) >= 0 ) ? geomNRaw : -geomNRaw;
		return Vector3Ops::Dot( v, geomN ) > 0 && Vector3Ops::Dot( r, geomN ) > 0;
	}
   
	return false;
}

RISEPel LambertianBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	if( ShouldReflect( vLightIn, ri, ri.onb.w() ) ) {
		return pReflectance->GetColor(ri) * INV_PI;
	}
   
	return RISEPel(0,0,0);
}

Scalar LambertianBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	if( ShouldReflect( vLightIn, ri, ri.onb.w() ) ) {
		return pReflectance->GetColorNM(ri,nm) * INV_PI;
	}

	return 0;
}

RISEPel LambertianBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Exact: ∫ f cos θ dω = Rd for a Lambertian.
	return pReflectance->GetColor( ri );
}
