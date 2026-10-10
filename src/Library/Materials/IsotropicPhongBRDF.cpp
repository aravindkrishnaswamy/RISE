//////////////////////////////////////////////////////////////////////
//
//  IsotropicPhongBRDF.cpp - Implements the phong BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../Interfaces/ISPF.h"
#include "IsotropicPhongBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

IsotropicPhongBRDF::IsotropicPhongBRDF( const IPainter& rd, const IPainter& rs, const IScalarPainter& exp ) :
  pRd( &rd ),
  pRs( &rs ),
  pExponent( &exp )
{
	pRd->addref();
	pRs->addref();
	pExponent->addref();
}

IsotropicPhongBRDF::~IsotropicPhongBRDF( )
{
	safe_release( pRd );
	safe_release( pRs );
	safe_release( pExponent );
}

void IsotropicPhongBRDF::SetRd( const IPainter& v )
{
	v.addref();
	safe_release( pRd );
	pRd = &v;
}

void IsotropicPhongBRDF::SetRs( const IPainter& v )
{
	v.addref();
	safe_release( pRs );
	pRs = &v;
}

void IsotropicPhongBRDF::SetExponent( const IScalarPainter& v )
{
	v.addref();
	safe_release( pExponent );
	pExponent = &v;
}

template< class T >
static void ComputeDiffuseSpecularFactors( 
	T& diffuse,
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri,
	const T& exp 
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Vector3& n = ri.onb.w();

	Scalar	nr = Vector3Ops::Dot(n,r);
	const Scalar	nv = Vector3Ops::Dot(n,v);

	if( (nr <= -NEARZERO) &&		// viewer is in front
		(nv <= -NEARZERO) )			// light is in front
	{
		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  Anchored directly to the incoming ray
		// (ri.ray.Dir()), NOT to n or -n -- see LambertianBRDF::ShouldReflect
		// for the branch-independence argument (r = -ri.ray.Dir(), so
		// "Dot(r,geomN) > 0" below is exactly "Dot(geomN,ri.ray.Dir()) < 0").
		// -- i.e. that condition holds TAUTOLOGICALLY for any r = -ri.ray.Dir(),
		// leaving only "Dot(v,geomN) > 0" (the light half) as a real constraint.
		// Degenerate vGeomNormal falls back to the raw n (gate is a no-op).
		{
			const Vector3 nEff = -n;
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : nEff;
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
			if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
				return;
			}
		}

		nr = -nr;
		Point3 incident = Point3Ops::mkPoint3(Point3( r.x, r.y, r.z ), ((nr*-2.0)*n));
		Scalar sd = Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), v );
		if( sd > 0 ) {
			specular = pow( sd, exp ) * ((exp+2.0)/TWO_PI);
		}

		diffuse = INV_PI;
	}
	else if( (nr >= NEARZERO) &&	// viewer is behind
			 (nv >= NEARZERO) )		// light is behind
	{
		// Geometric-horizon gate (mirrors the branch above; effective
		// normal here is +n).
		{
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : n;
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
			if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
				return;
			}
		}

		Point3 incident = Point3Ops::mkPoint3( Point3( r.x, r.y, r.z ), ((nr*-2.0)*n));
		Scalar sd = Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), -v );
		if( sd > 0 ) {
			specular = pow( sd, exp ) * ((exp+2.0)/TWO_PI);
		}

		diffuse = INV_PI;
	}
}

RISEPel IsotropicPhongBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel diffuseFactor, specularFactor;
	const ScalarTriple e = pExponent->GetValuesAt(ri);
	const RISEPel exp( e.v[0], e.v[1], e.v[2] );
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, exp );

	return ((ReflectanceColor( *pRd, ri ) * diffuseFactor) + (ReflectanceColor( *pRs, ri )*specularFactor));
}

Scalar IsotropicPhongBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar diffuseFactor=0, specularFactor=0;
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, pExponent->GetValueAtNM(ri,nm) );

	return ((ReflectanceColorNM( *pRd, ri, nm ) * diffuseFactor) + (ReflectanceColorNM( *pRs, ri, nm )*specularFactor));
}

RISEPel IsotropicPhongBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Conventional Phong reflectance: Rd + Rs (the normalized lobe
	// integrates to ≈ Rs over the hemisphere).
	return ReflectanceColor( *pRd, ri ) + ReflectanceColor( *pRs, ri );
}


// DL-481: a verbatim copy of ComputeDiffuseSpecularFactors for
// valueByScatterType alone (a second caller can change how LTO inlines it
// into value(), moving cap-free renders by ~1e-15).
template< class T >
static void ComputeDiffuseSpecularFactorsSplit( 
	T& diffuse,
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri,
	const T& exp 
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Vector3& n = ri.onb.w();

	Scalar	nr = Vector3Ops::Dot(n,r);
	const Scalar	nv = Vector3Ops::Dot(n,v);

	if( (nr <= -NEARZERO) &&		// viewer is in front
		(nv <= -NEARZERO) )			// light is in front
	{
		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  Anchored directly to the incoming ray
		// (ri.ray.Dir()), NOT to n or -n -- see LambertianBRDF::ShouldReflect
		// for the branch-independence argument (r = -ri.ray.Dir(), so
		// "Dot(r,geomN) > 0" below is exactly "Dot(geomN,ri.ray.Dir()) < 0").
		// -- i.e. that condition holds TAUTOLOGICALLY for any r = -ri.ray.Dir(),
		// leaving only "Dot(v,geomN) > 0" (the light half) as a real constraint.
		// Degenerate vGeomNormal falls back to the raw n (gate is a no-op).
		{
			const Vector3 nEff = -n;
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : nEff;
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
			if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
				return;
			}
		}

		nr = -nr;
		Point3 incident = Point3Ops::mkPoint3(Point3( r.x, r.y, r.z ), ((nr*-2.0)*n));
		Scalar sd = Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), v );
		if( sd > 0 ) {
			specular = pow( sd, exp ) * ((exp+2.0)/TWO_PI);
		}

		diffuse = INV_PI;
	}
	else if( (nr >= NEARZERO) &&	// viewer is behind
			 (nv >= NEARZERO) )		// light is behind
	{
		// Geometric-horizon gate (mirrors the branch above; effective
		// normal here is +n).
		{
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : n;
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
			if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
				return;
			}
		}

		Point3 incident = Point3Ops::mkPoint3( Point3( r.x, r.y, r.z ), ((nr*-2.0)*n));
		Scalar sd = Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), -v );
		if( sd > 0 ) {
			specular = pow( sd, exp ) * ((exp+2.0)/TWO_PI);
		}

		diffuse = INV_PI;
	}
}

//////////////////////////////////////////////////////////////////////
// DL-481 (IBSDF::valueByScatterType): the connection value split by the
// SPF's lobe labels -- the diffuse lobe eRayDiffuse, the glossy lobe
// eRayReflection, each priced by its own f cos / p weight in the SPF.
// value() is left untouched (its code generation is what cap-free
// renders are bit-identical to); this re-evaluates the same two terms.
//////////////////////////////////////////////////////////////////////

bool IsotropicPhongBRDF::valueByScatterType( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const IORStack* /*pIORStack*/, RISEPel out[5] ) const
{
	for( int k = 0; k < 5; k++ ) out[k] = RISEPel( 0, 0, 0 );
	RISEPel diffuseFactor, specularFactor;
	const ScalarTriple e = pExponent->GetValuesAt(ri);
	const RISEPel exp( e.v[0], e.v[1], e.v[2] );
	ComputeDiffuseSpecularFactorsSplit( diffuseFactor, specularFactor, vLightIn, ri, exp );
	out[ScatteredRay::eRayDiffuse] = ReflectanceColor( *pRd, ri ) * diffuseFactor;
	out[ScatteredRay::eRayReflection] = ReflectanceColor( *pRs, ri ) * specularFactor;
	return true;
}

bool IsotropicPhongBRDF::valueByScatterTypeNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* /*pIORStack*/, Scalar out[5] ) const
{
	for( int k = 0; k < 5; k++ ) out[k] = 0;
	Scalar diffuseFactor=0, specularFactor=0;
	ComputeDiffuseSpecularFactorsSplit( diffuseFactor, specularFactor, vLightIn, ri, pExponent->GetValueAtNM(ri,nm) );
	out[ScatteredRay::eRayDiffuse] = ReflectanceColorNM( *pRd, ri, nm ) * diffuseFactor;
	out[ScatteredRay::eRayReflection] = ReflectanceColorNM( *pRs, ri, nm ) * specularFactor;
	return true;
}
