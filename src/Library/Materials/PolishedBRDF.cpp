//////////////////////////////////////////////////////////////////////
//
//  PolishedBRDF.cpp - Implements a polished BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  WARNING!!!  This is experimental and unverified code
//             from ggLibrary, DO NOT USE!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PolishedBRDF.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

PolishedBRDF::PolishedBRDF( const IPainter& reflectance, const IPainter& phongN_, const IPainter& Nt_ ) : 
  pReflectance( reflectance ), phongN( phongN_ ), Nt( Nt_ )
{
	pReflectance.addref();
	phongN.addref();
	Nt.addref();
}

PolishedBRDF::~PolishedBRDF()
{
	pReflectance.release();
	phongN.release();
	Nt.release();
}

Scalar ComputeRs( const Vector3& v, const Vector3& n, const Scalar ior )
{
	Vector3	t = v;

	if( Optics::CalculateRefractedRay( n, 1.0, ior, t ) ) {					// t becomes refracted vector
		return Optics::CalculateDielectricReflectance( v, t, n, 1.0, ior );
	}

	return 1.0;
}

RISEPel PolishedBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	// Side-of-surface decision uses the GEOMETRIC normal (front/back is
	// face-orientation, PBRT 4e §10.1.1).  The flipped normal carries
	// the SHADING normal so the Fresnel angular dependence stays in the
	// BSDF frame, matching Mitsuba 3 `dielectric.cpp`.  ComputeRs goes
	// through Optics::CalculateRefractedRay/DielectricReflectance which
	// are sign-invariant under n→-n, so the visible Rs change is small;
	// the geometric form is the documented convention and is robust
	// against future code that consumes `n` directly.
	const bool bBackface = Vector3Ops::Dot( ri.vGeomNormal, -vLightIn ) < NEARZERO;
	const Vector3 n = bBackface ? -ri.vNormal : ri.vNormal;

	// Geometric-horizon gate (ray-anchored; see GGXBRDF::value /
	// LambertianBRDF::ShouldReflect): a GlintModifier-tilted shading
	// normal can validate a light direction that is still below the
	// true geometric surface -- this file had NO horizon check at all
	// before this gate.  geomN is anchored to ri.ray.Dir(), so a tilt
	// cannot flip its orientation; the view-direction half is
	// tautologically satisfied (r = -ri.ray.Dir(), so Dot(r,geomN) > 0
	// always by construction) -- only vLightIn can actually reject.
	// Degenerate vGeomNormal falls back to n (gate is a no-op).
	{
		const Vector3 v = Vector3Ops::Normalize( vLightIn );
		const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
			return RISEPel(0,0,0);
		}
	}

	const RISEPel ior = Nt.GetColor(ri);
	if( ior[0] == ior[1] && ior[1] == ior[2] ) {
		Scalar		Rs = ComputeRs( -vLightIn, n, ior[0] );
		return pReflectance.GetColor(ri) * INV_PI * (1.0 - Rs);
	}

	// Otherwise we have to do it for each pel
	RISEPel Rs;
	for( int i=0; i<3; i++ ) {
		Rs[i] = ComputeRs( -vLightIn, n, ior[i] );
	}

	return pReflectance.GetColor(ri) * INV_PI * (1.0 - Rs);
}

Scalar PolishedBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Side-of-surface decision uses the GEOMETRIC normal (front/back is
	// face-orientation, PBRT 4e §10.1.1).  The flipped normal carries
	// the SHADING normal so the Fresnel angular dependence stays in the
	// BSDF frame, matching Mitsuba 3 `dielectric.cpp`.  ComputeRs goes
	// through Optics::CalculateRefractedRay/DielectricReflectance which
	// are sign-invariant under n→-n, so the visible Rs change is small;
	// the geometric form is the documented convention and is robust
	// against future code that consumes `n` directly.
	const bool bBackface = Vector3Ops::Dot( ri.vGeomNormal, -vLightIn ) < NEARZERO;
	const Vector3 n = bBackface ? -ri.vNormal : ri.vNormal;

	// Geometric-horizon gate (mirrors value()'s gate above).
	{
		const Vector3 v = Vector3Ops::Normalize( vLightIn );
		const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
			return 0;
		}
	}

	Scalar		Rs = ComputeRs( -vLightIn, n, Nt.GetColorNM(ri,nm) );
	return pReflectance.GetColorNM(ri,nm) * INV_PI * (1.0 - Rs);
}

RISEPel PolishedBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Diffuse base scaled by transmitted Fresnel (1 - Rs) plus the
	// polished specular layer contributing Rs.  Compute Rs at the view
	// direction with the camera ray.
	const Vector3& n = ri.onb.w();
	const RISEPel ior = Nt.GetColor( ri );
	RISEPel Rs;
	for( int i = 0; i < 3; ++i ) {
		Rs[i] = ComputeRs( ri.ray.Dir(), n, ior[i] );
	}
	return pReflectance.GetColor( ri ) * ( RISEPel(1,1,1) - Rs ) + Rs;
}