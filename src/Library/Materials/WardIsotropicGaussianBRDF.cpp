//////////////////////////////////////////////////////////////////////
//
//  WardIsotropicGaussianBRDF.cpp
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../Interfaces/ISPF.h"
#include "WardIsotropicGaussianBRDF.h"
#include "WardSelectionQuadrature.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

WardIsotropicGaussianBRDF::WardIsotropicGaussianBRDF(
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IScalarPainter& alpha_
	) :
  pDiffuse( &diffuse_ ),
  pSpecular( &specular_ ),
  pAlpha( &alpha_ )
{
	pDiffuse->addref();
	pSpecular->addref();
	pAlpha->addref();
}

WardIsotropicGaussianBRDF::~WardIsotropicGaussianBRDF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pAlpha );
}

void WardIsotropicGaussianBRDF::SetDiffuse( const IPainter& v )      { v.addref(); safe_release( pDiffuse );  pDiffuse  = &v; }
void WardIsotropicGaussianBRDF::SetSpecular( const IPainter& v )     { v.addref(); safe_release( pSpecular ); pSpecular = &v; }
void WardIsotropicGaussianBRDF::SetAlpha( const IScalarPainter& v )  { v.addref(); safe_release( pAlpha );    pAlpha    = &v; }

template< class T >
static void ComputeFactors( 
    T& diffuse, 
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const OrthonormalBasis3D& onb,
	const T& alpha,
	const T& rs
	)
{
	const Vector3 n = onb.w();
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) && (nv >= NEARZERO) ) {
		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  This is a DEFENSIVE check (a valid exterior hit
		// already satisfies it) rather than a literal sampler-consistency one
		// -- NEE's light direction isn't sampler-drawn -- but it guards
		// against the same tilt pathology.  Degenerate vGeomNormal falls
		// back to the shading normal (gate is a no-op).
		// (r is tautologically inside the gate: r = -ri.ray.Dir() and geomN is
		// ray-anchored, so Dot(r,geomN) > 0 always holds -- see LambertianBRDF.cpp:57-60.)
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
			return;
		}

		diffuse = INV_PI;

		const Vector3 h = WardSelection::ReconstructHalf(v+r);
		const Scalar hn = Vector3Ops::Dot(n,h);

        specular = WardSelection::SpecularKernel(Vector3Ops::Dot(h,onb.u()),
            Vector3Ops::Dot(h,onb.v()),hn,Vector3Ops::Dot(h,r),alpha,alpha,rs);
	}
}

RISEPel WardIsotropicGaussianBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel d, s;
	const ScalarTriple at = pAlpha->GetValuesAt(ri);
	const RISEPel a( at.v[0], at.v[1], at.v[2] );

	// Flip to the ray-facing frame, mirroring WardIsotropicGaussianSPF's
	// FlipW (same condition), so value() agrees with Scatter()/Pdf() on
	// back-face hits -- ComputeFactors' geomN gate orients to whatever n
	// it's given, so the flip propagates through automatically.
	OrthonormalBasis3D onb = ri.onb;
	if(Vector3Ops::Dot(ri.ray.Dir(),onb.w())>NEARZERO) onb.FlipW();
	const RISEPel rs = ReflectanceColor( *pSpecular, ri );
	ComputeFactors<RISEPel>( d, s, vLightIn, ri, onb, a, rs );

	// DL-310: the diffuse term is coupled to the specular lobe's albedo
	// bound -- see WardSelection::CoupledDiffuse.
	const RISEPel rd = ReflectanceColor( *pDiffuse, ri );
	const RISEPel rdCoupled( WardSelection::CoupledDiffuse( rd[0], rs[0] ),
		WardSelection::CoupledDiffuse( rd[1], rs[1] ), WardSelection::CoupledDiffuse( rd[2], rs[2] ) );
	return d*rdCoupled + s;
}

Scalar WardIsotropicGaussianBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar d=0, s=0;

	// Same ray-facing flip as value() above.
	OrthonormalBasis3D onb = ri.onb;
	if(Vector3Ops::Dot(ri.ray.Dir(),onb.w())>NEARZERO) onb.FlipW();
	const Scalar rsNM = ReflectanceColorNM(*pSpecular,ri,nm);
	ComputeFactors<Scalar>( d, s, vLightIn, ri, onb, pAlpha->GetValueAtNM(ri,nm), rsNM );

	return d*WardSelection::CoupledDiffuse( ReflectanceColorNM( *pDiffuse, ri, nm ), rsNM ) + s;
}

RISEPel WardIsotropicGaussianBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Conservative approximation: the bounded specular variant integrates
	// to at most Rs. Saturate only this OIDN AOV, whose contract is [0,1];
	// additive authored reflectances remain unchanged in transport.
	// DL-310: the coupled diffuse min(Rd, 1 - Rs) plus the specular bound.
	const RISEPel rd=ReflectanceColor( *pDiffuse, ri ), rs=ReflectanceColor( *pSpecular, ri );
	RISEPel result=rs;
	for(int ch=0;ch<3;++ch) result[ch]+=WardSelection::CoupledDiffuse(rd[ch],rs[ch]);
	for(int ch=0;ch<3;++ch) result[ch]=r_max(Scalar(0),r_min(Scalar(1),result[ch]));
	return result;
}


// DL-481: a verbatim copy of ComputeFactors for valueByScatterType alone.
// Calling ComputeFactors from a second function changed how LTO inlines
// it into value(), moving cap-free renders by ~1e-15; with its own copy
// value()'s code generation is the pre-DL-481 one.
template< class T >
static void ComputeFactorsSplit( 
    T& diffuse, 
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const OrthonormalBasis3D& onb,
	const T& alpha,
	const T& rs
	)
{
	const Vector3 n = onb.w();
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) && (nv >= NEARZERO) ) {
		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  This is a DEFENSIVE check (a valid exterior hit
		// already satisfies it) rather than a literal sampler-consistency one
		// -- NEE's light direction isn't sampler-drawn -- but it guards
		// against the same tilt pathology.  Degenerate vGeomNormal falls
		// back to the shading normal (gate is a no-op).
		// (r is tautologically inside the gate: r = -ri.ray.Dir() and geomN is
		// ray-anchored, so Dot(r,geomN) > 0 always holds -- see LambertianBRDF.cpp:57-60.)
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
			return;
		}

		diffuse = INV_PI;

		const Vector3 h = WardSelection::ReconstructHalf(v+r);
		const Scalar hn = Vector3Ops::Dot(n,h);

        specular = WardSelection::SpecularKernel(Vector3Ops::Dot(h,onb.u()),
            Vector3Ops::Dot(h,onb.v()),hn,Vector3Ops::Dot(h,r),alpha,alpha,rs);
	}
}

//////////////////////////////////////////////////////////////////////
// DL-481 (IBSDF::valueByScatterType): the connection value split by the
// SPF's lobe labels -- the diffuse lobe eRayDiffuse, the glossy lobe
// eRayReflection, each priced by its own f cos / p weight in the SPF.
// value() is left untouched (its code generation is what cap-free
// renders are bit-identical to); this re-evaluates the same two terms.
//////////////////////////////////////////////////////////////////////

bool WardIsotropicGaussianBRDF::valueByScatterType( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const IORStack* /*pIORStack*/, RISEPel out[5] ) const
{
	for( int k = 0; k < 5; k++ ) out[k] = RISEPel( 0, 0, 0 );
	RISEPel d, s;
	const ScalarTriple at = pAlpha->GetValuesAt(ri);
	const RISEPel a( at.v[0], at.v[1], at.v[2] );
	OrthonormalBasis3D onb = ri.onb;
	if(Vector3Ops::Dot(ri.ray.Dir(),onb.w())>NEARZERO) onb.FlipW();
	const RISEPel rs = ReflectanceColor( *pSpecular, ri );
	ComputeFactorsSplit<RISEPel>( d, s, vLightIn, ri, onb, a, rs );
	const RISEPel rd = ReflectanceColor( *pDiffuse, ri );
	const RISEPel rdCoupled( WardSelection::CoupledDiffuse( rd[0], rs[0] ),
		WardSelection::CoupledDiffuse( rd[1], rs[1] ), WardSelection::CoupledDiffuse( rd[2], rs[2] ) );
	out[ScatteredRay::eRayDiffuse] = d*rdCoupled;
	out[ScatteredRay::eRayReflection] = s;
	return true;
}

bool WardIsotropicGaussianBRDF::valueByScatterTypeNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* /*pIORStack*/, Scalar out[5] ) const
{
	for( int k = 0; k < 5; k++ ) out[k] = 0;
	Scalar d=0, s=0;
	OrthonormalBasis3D onb = ri.onb;
	if(Vector3Ops::Dot(ri.ray.Dir(),onb.w())>NEARZERO) onb.FlipW();
	const Scalar rsNM = ReflectanceColorNM(*pSpecular,ri,nm);
	ComputeFactorsSplit<Scalar>( d, s, vLightIn, ri, onb, pAlpha->GetValueAtNM(ri,nm), rsNM );
	out[ScatteredRay::eRayDiffuse] = d*WardSelection::CoupledDiffuse( ReflectanceColorNM( *pDiffuse, ri, nm ), rsNM );
	out[ScatteredRay::eRayReflection] = s;
	return true;
}
