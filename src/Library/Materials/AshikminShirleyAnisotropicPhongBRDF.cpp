//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleyAnisotropicPhongBRDF.cpp - Implementation of the class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 10, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AshikminShirleyAnisotropicPhongBRDF.h"
#include "../Utilities/Optics.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

AshikminShirleyAnisotropicPhongBRDF::AshikminShirleyAnisotropicPhongBRDF( const IScalarPainter& Nu_, const IScalarPainter& Nv_, const IPainter& Rd_, const IPainter& Rs_ ) :
  pNu( &Nu_ ), pNv( &Nv_ ), pRd( &Rd_ ), pRs( &Rs_ )
{
	pNu->addref();
	pNv->addref();
	pRd->addref();
	pRs->addref();
}

AshikminShirleyAnisotropicPhongBRDF::~AshikminShirleyAnisotropicPhongBRDF( )
{
	safe_release( pNu );
	safe_release( pNv );
	safe_release( pRd );
	safe_release( pRs );
}

void AshikminShirleyAnisotropicPhongBRDF::SetNu( const IScalarPainter& v ) { v.addref(); safe_release( pNu ); pNu = &v; }
void AshikminShirleyAnisotropicPhongBRDF::SetNv( const IScalarPainter& v ) { v.addref(); safe_release( pNv ); pNv = &v; }
void AshikminShirleyAnisotropicPhongBRDF::SetRd( const IPainter& v )       { v.addref(); safe_release( pRd ); pRd = &v; }
void AshikminShirleyAnisotropicPhongBRDF::SetRs( const IPainter& v )       { v.addref(); safe_release( pRs ); pRs = &v; }

template< class T >
void AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors(
	T& diffuse,
	T& specular,
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const Vector3& n,
	const Vector3& u,
	const Vector3& v,
	const T& NU,
	const T& NV,
	const T& Rs
	)
{
	diffuse = specular = 0;

	Vector3 k1 = Vector3Ops::Normalize(vLightIn);
	Vector3 k2 = Vector3Ops::Normalize(-ri.ray.Dir());
	Vector3 h = Vector3Ops::Normalize(k1+k2);

	const Vector3& k = k2;

	const Scalar hdotk = Vector3Ops::Dot(h,k);
	Scalar ndotk1 = Vector3Ops::Dot(n,k1);
	Scalar ndotk2 = Vector3Ops::Dot(n,k2);

	if( ndotk2 < NEARZERO ) {
		return;
	}

	// Geometric-horizon gate: a GlintModifier-tilted shading normal can
	// validate light/view directions that are still below the true geometric
	// surface.  This is a DEFENSIVE check, not a sampler-consistency one --
	// NEE's light direction isn't sampler-drawn, but a valid exterior hit
	// already satisfies it, and rejecting here guards against the same tilt
	// pathology the sampler-side gate targets.  Degenerate vGeomNormal falls
	// back to the shading normal (gate is a no-op).
	// (k2 is tautologically inside the gate: k2 = -ri.ray.Dir() and geomN is
	// ray-anchored, so Dot(k2,geomN) > 0 always holds -- see LambertianBRDF.cpp:57-60.)
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( k1, geomN ) <= 0 || Vector3Ops::Dot( k2, geomN ) <= 0 ) {
		return;
	}

	if( ndotk1 < NEARZERO ) {
		ndotk1 = 0;
	}

	Scalar	fromK1 = 1.0 - pow(1.0 - ((ndotk1)*0.5), 5.0);
	Scalar	fromK2 = 1.0 - pow(1.0 - ((ndotk2)*0.5), 5.0);

	static const Scalar energyConservation = 28.0 / (23.0 * PI);

	diffuse = energyConservation * fromK1 * fromK2;

	// Compute specular
	const T NuNvFactor = sqrt((NU+1.0)*(NV+1.0));
	const T rhoSconst = NuNvFactor / (8.0 * PI);
	const T fresnel = Optics::CalculateFresnelReflectanceSchlick( Rs, hdotk );
	
	const Scalar hn = Vector3Ops::Dot(h,n);
	const Scalar hu = Vector3Ops::Dot(h,u);
	const Scalar hv = Vector3Ops::Dot(h,v);

	// h -> n alignment (mirror-reflection direction, hn -> +-1, e.g. viewing
	// straight down a near-normal-incidence specular highlight) drives
	// 1-hn^2 -> 0.  NU*hu^2 + NV*hv^2 -> 0 at the same rate (hu, hv -> 0),
	// so the un-guarded ratio is a 0/0 indeterminate that evaluates to NaN
	// in floating point, poisoning `num = pow(hn, exponent)` and every
	// downstream kray/pdf that consumes this BRDF value.  In the limit
	// pow(hn~=1, anything) ~= 1, so the exact exponent value at hn=+-1
	// doesn't matter -- 0 is the well-defined limit RISE already uses for
	// the identical formula in AshikminShirleySpecularPdf
	// (AshikminShirleyAnisotropicPhongSPF.cpp); mirror that guard here.
	const Scalar sin_theta_h_sq = 1.0 - hn*hn;
	T exponent;
	if( sin_theta_h_sq > NEARZERO ) {
		exponent = (( NU*hu*hu ) + ( NV*hv*hv )) / sin_theta_h_sq;
	} else {
		exponent = 0;
	}

	// hn < 0 means the half-vector lies in the back hemisphere relative to
	// the (possibly GlintModifier-tilted) shading normal `n` -- reachable
	// now that the geomN gate above is ray-anchored (decoupled from n)
	// rather than n-anchored: k1 and k2 can each individually satisfy the
	// TRUE geometric-horizon gate while their vector sum still drags h to
	// the back side of a heavily tilted n.  pow(hn, exponent) is
	// mathematically undefined for a negative base with the general
	// non-integer exponent computed above; under -ffast-math this is NOT
	// guaranteed to produce a clean NaN (see docs/skills -- ffast-math has
	// historically no reliable infinity/NaN sentinel (pre-2026-07-29; macOS
	// pairs -fno-finite-math-only now, so the predicates work) -- but the
	// design below stands on its own: reject invalid inputs at the value
	// layer, don't rely on isnan/isinf downstream).  The Ashikmin-Shirley
	// half-vector lobe is only defined for hn (== cos(theta_h)) in [0,1];
	// hn <= 0 has no valid reflection through this half-vector, so the
	// specular contribution is 0 -- mirrors the sin_theta_h_sq guard above.
	T num;
	if( hn > 0 ) {
		num = pow( hn, exponent );
	} else {
		num = 0;
	}
	const Scalar den = (hdotk) * r_max( ndotk1, ndotk2 );

	specular = rhoSconst*(num/den)*fresnel;
}

RISEPel AshikminShirleyAnisotropicPhongBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	// rsCol = Rs painter SAMPLED at this point — distinct from the
	// `pRs` MEMBER (the painter pointer itself).
	RISEPel	rsCol = pRs->GetColor(ri);
	RISEPel	OMRs = RISEPel(1.0,1.0,1.0) - rsCol;

	RISEPel diffuseFactor, specularFactor;
	const ScalarTriple nu = pNu->GetValuesAt(ri);
	const ScalarTriple nv = pNv->GetValuesAt(ri);
	const RISEPel NUp( nu.v[0], nu.v[1], nu.v[2] );
	const RISEPel NVp( nv.v[0], nv.v[1], nv.v[2] );

	// Flip to the ray-facing frame, mirroring AshikminShirleyAnisotropicPhongSPF's
	// FlipW (same condition), so value() agrees with Scatter()/Pdf() on
	// back-face hits -- the raw ri.onb.w() made ndotk2 negative there and
	// silently dropped every back-face NEE sample via the helper's early-out.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, myonb.w(), myonb.u(), myonb.v(), NUp, NVp, rsCol );

	const RISEPel diffuse = (pRd->GetColor(ri) * OMRs * diffuseFactor);
	// specularFactor already contains Fresnel F(h·k) = Rs + (1-Rs)(1-cos)^5,
	// so no extra Rs multiplication is needed (per Ashikmin-Shirley 2000 paper)
	const RISEPel specular = specularFactor;

	return diffuse + specular;
}

Scalar AshikminShirleyAnisotropicPhongBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar	rsCol = pRs->GetColorNM(ri,nm);
	const Scalar	OMRs = 1.0 - rsCol;

	Scalar diffuseFactor, specularFactor;

	// Same ray-facing flip as value() above.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, myonb.w(), myonb.u(), myonb.v(), pNu->GetValueAtNM(ri,nm), pNv->GetValueAtNM(ri,nm), rsCol );

	const Scalar diffuse = (pRd->GetColorNM(ri,nm) * OMRs * diffuseFactor);
	// specularFactor already contains Fresnel — no extra Rs multiplication
	const Scalar specular = specularFactor;

	return diffuse + specular;
}

RISEPel AshikminShirleyAnisotropicPhongBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// AS-2000 factors as `Rd·(1-Rs)·diffuse_factor + spec_factor` where
	// the spec lobe carries Fresnel(F0=Rs) and integrates to ≈ Rs.
	// Total: Rd·(1-Rs) + Rs — symmetric in the diffuse/spec coupling.
	const RISEPel rsCol = pRs->GetColor( ri );
	return pRd->GetColor( ri ) * ( RISEPel(1,1,1) - rsCol ) + rsCol;
}
