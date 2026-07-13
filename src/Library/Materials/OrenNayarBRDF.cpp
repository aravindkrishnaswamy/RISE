//////////////////////////////////////////////////////////////////////
//
//  OrenNayarBRDF.cpp - Implements the lambertian BRDF
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
#include "OrenNayarBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

OrenNayarBRDF::OrenNayarBRDF(
	const IPainter& reflectance,
	const IScalarPainter& roughness
	) :
  pReflectance( &reflectance ),
  pRoughness( &roughness )
{
	pReflectance->addref();
	pRoughness->addref();
}

OrenNayarBRDF::~OrenNayarBRDF( )
{
	safe_release( pReflectance );
	safe_release( pRoughness );
}

void OrenNayarBRDF::SetReflectance( const IPainter& v )
{
	v.addref();
	safe_release( pReflectance );
	pReflectance = &v;
}

void OrenNayarBRDF::SetRoughness( const IScalarPainter& v )
{
	v.addref();
	safe_release( pRoughness );
	pRoughness = &v;
}

template< class T >
void OrenNayarBRDF::ComputeFactor( 
	T& L1, 
	T& L2, 
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const Vector3& n, 
	const T& roughness 
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) &&	(nv >= NEARZERO) ) {
		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  This is a DEFENSIVE check (a valid exterior hit
		// already satisfies it) rather than a literal sampler-consistency one
		// -- NEE's light direction isn't sampler-drawn -- but it guards
		// against the same tilt pathology; the early return leaves L1/L2 at
		// the caller's zero-init, identical to the guard falling through.
		// Degenerate vGeomNormal falls back to the shading normal (gate is a
		// no-op).
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
			return;
		}

		const T sqr_r = roughness*roughness;
		const Scalar cos_phi_diff = Vector3Ops::Dot(
			Vector3Ops::Normalize(r-(n*nr)),
			Vector3Ops::Normalize(v-(n*nv))
			);

		const Scalar theta_i = acos(nv);
		const Scalar theta_r = acos(nr);

		const Scalar alpha = r_max(theta_i,theta_r);
		const Scalar beta = r_min(theta_i,theta_r);

		const T C1 = 1.0 - 0.5*(sqr_r / (sqr_r + 0.33));
		const T C2 = 0.45 * (sqr_r / (sqr_r + 0.09)) *	(sin(alpha) - ((cos_phi_diff >= 0)? 0 : pow(2.0*beta/PI,3.0)));
		const Scalar t = (4.0*alpha*beta/(PI*PI));
		const T C3 = 0.125 * (sqr_r / (sqr_r + 0.09)) * (t*t);
		L1 = ( C1 + cos_phi_diff * C2 * tan(beta) + (1.0 - fabs(cos_phi_diff)) * C3 * tan((alpha+beta)/2.0) );
		const Scalar u = (2.0*beta)/PI;
		L2 = 0.17 * (sqr_r / (sqr_r + 0.13)) * (1.0 - cos_phi_diff * (u*u));
	}
}

RISEPel OrenNayarBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel L1, L2;
	const ScalarTriple r = pRoughness->GetValuesAt(ri);
	const RISEPel roughness( r.v[0], r.v[1], r.v[2] );

	// Flip to the ray-facing frame, mirroring OrenNayarSPF::Scatter's FlipW
	// (same condition), so value() agrees with Scatter()/Pdf() on back-face
	// hits -- ComputeFactor's geomN gate orients to whatever n it's given,
	// so the flip propagates through automatically.
	const Vector3 n = ( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) ? -ri.onb.w() : ri.onb.w();
	ComputeFactor<RISEPel>( L1, L2, vLightIn, ri, n, roughness );
	const RISEPel rho = pReflectance->GetColor(ri);

	return (L1*INV_PI*rho) + (L2*INV_PI*(rho*rho));
}

Scalar OrenNayarBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar L1=0, L2=0;

	// Same ray-facing flip as value() above.
	const Vector3 n = ( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) ? -ri.onb.w() : ri.onb.w();
	ComputeFactor<Scalar>( L1, L2, vLightIn, ri, n, pRoughness->GetValueAtNM(ri,nm) );
	const Scalar rho = pReflectance->GetColorNM(ri,nm);

	return (L1*INV_PI*rho) + (L2*INV_PI*(rho*rho));
}

RISEPel OrenNayarBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Oren-Nayar is energy-conserving: total reflectance ≈ Rd
	// regardless of roughness.  The L1 / L2 terms only redistribute
	// directional scattering shape.
	return pReflectance->GetColor( ri );
}

// Explicit instantiation so other TUs (OrenNayarSPF.cpp) can link to the
// scalar overload without seeing the template body.  The RISEPel flavour
// is instantiated implicitly through OrenNayarBRDF::value above.
template void OrenNayarBRDF::ComputeFactor<Scalar>(
	Scalar&, Scalar&,
	const Vector3&, const RayIntersectionGeometric&,
	const Vector3&, const Scalar& );
