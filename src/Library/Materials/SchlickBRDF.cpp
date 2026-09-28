//////////////////////////////////////////////////////////////////////
//
//  SchlickBRDF.cpp - Implements the Schlick BRDF
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
#include "SchlickBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"
#include "../Utilities/GeometricUtilities.h"
#include "SchlickMasking.h"

using namespace RISE;
using namespace RISE::Implementation;

SchlickBRDF::SchlickBRDF(
	const IPainter& diffuse,
	const IPainter& specular,
	const IScalarPainter& roughness,
	const IScalarPainter& isotropy
	) :
  pDiffuse( &diffuse ),
  pSpecular( &specular ),
  pRoughness( &roughness ),
  pIsotropy( &isotropy )
{
	pDiffuse->addref();
	pSpecular->addref();
	pRoughness->addref();
	pIsotropy->addref();
}

SchlickBRDF::~SchlickBRDF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pRoughness );
	safe_release( pIsotropy );
}

void SchlickBRDF::SetDiffuse( const IPainter& v )       { v.addref(); safe_release( pDiffuse );   pDiffuse   = &v; }
void SchlickBRDF::SetSpecular( const IPainter& v )      { v.addref(); safe_release( pSpecular );  pSpecular  = &v; }
void SchlickBRDF::SetRoughness( const IScalarPainter& v ){ v.addref(); safe_release( pRoughness ); pRoughness = &v; }
void SchlickBRDF::SetIsotropy( const IScalarPainter& v ) { v.addref(); safe_release( pIsotropy );  pIsotropy  = &v; }

namespace
{
	//! The direction-pair quantities every lane shares (DL-225 split
	//! them out of the old templated ComputeFactor so each channel can
	//! carry its own masking lane).
	struct SchlickPairGeometry
	{
		Scalar nv, nl;				//!< cosines to the shading normal
		Scalar vx, vy, lx, ly;		//!< tangential components in (onb.u, onb.v)
		Scalar t;					//!< n.h
		Scalar w;					//!< onb.v . normalize(h - t n): Schlick's A argument
		Scalar fresnel;				//!< (1 - h.l)^5
	};

	//! Returns false (zero BRDF) outside either hemisphere or below the
	//! geometric horizon.
	bool SchlickPair(
		const Vector3& vLightIn,
		const RayIntersectionGeometric& ri,
		const OrthonormalBasis3D& onb,
		SchlickPairGeometry& g
		)
	{
		const Vector3& n = onb.w();
		const Vector3 l = Vector3Ops::Normalize(vLightIn); // light vector
		const Vector3 v = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

		g.nv = Vector3Ops::Dot(n,v);
		g.nl = Vector3Ops::Dot(n,l);

		if( (g.nv < NEARZERO) || (g.nl < NEARZERO) ) {
			return false;
		}

		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  This is a DEFENSIVE check (a valid exterior hit
		// already satisfies it) rather than a literal sampler-consistency one
		// -- NEE's light direction isn't sampler-drawn -- but it guards
		// against the same tilt pathology.  Degenerate vGeomNormal falls
		// back to the shading normal (gate is a no-op).
		// (v is tautologically inside the gate: v = -ri.ray.Dir() and geomN is
		// ray-anchored, so Dot(v,geomN) > 0 always holds -- see LambertianBRDF.cpp:57-60.)
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( l, geomN ) <= 0 || Vector3Ops::Dot( v, geomN ) <= 0 ) {
			return false;
		}

		const Vector3 h = Vector3Ops::Normalize(l+v);
		g.t = Vector3Ops::Dot(n,h);
		const Scalar hl = Vector3Ops::Dot(h,l);
		g.fresnel = ::pow(1-hl,5);
		g.w = Vector3Ops::Dot(onb.v(),Vector3Ops::Normalize(h-(g.t*n)));
		g.vx = Vector3Ops::Dot(v,onb.u());
		g.vy = Vector3Ops::Dot(v,onb.v());
		g.lx = Vector3Ops::Dot(l,onb.u());
		g.ly = Vector3Ops::Dot(l,onb.v());
		return true;
	}

	//! Z A m(v) m(l) / (4 pi nv nl) for ONE lane.
	//!
	//! DL-178 introduced Schlick 1994 Eq.31, G(c) = c/(r+(1-r)c), with
	//! nl*nv cancelled analytically.  DL-225: Eq.31 alone lets rho_d
	//! exceed 1 near grazing at low roughness (5.6 at r .005); the masking
	//! is now min(Eq.31, the Smith projected-area bound of Schlick's own
	//! Z*A distribution), which is exactly Eq.31 wherever Eq.31 is already
	//! inside that bound -- see SchlickMasking.h.  The masking enters as
	//! its denominators c/m(c) so the grazing limit stays finite.
	Scalar SchlickLaneFactor( const SchlickPairGeometry& g, const Scalar r, const Scalar p )
	{
		const Scalar sqr_t = g.t*g.t;
		const Scalar zdem = (r*sqr_t + 1.0) - sqr_t;
		const Scalar Z = r / (zdem*zdem);

		const Scalar sqr_p = p*p;
		const Scalar sqr_w = g.w*g.w;
		const Scalar A = sqrt(p/(sqr_p-sqr_p*sqr_w+sqr_w));

		SchlickMasking::Lane lane;
		SchlickMasking::Prepare( lane, r, p );
		const Scalar dv = SchlickMasking::MaskDen( lane, g.nv, g.vx, g.vy );
		const Scalar dl = SchlickMasking::MaskDen( lane, g.nl, g.lx, g.ly );
		return (Z*A)/(4.0*PI*dv*dl);
	}

	//! Per-channel factor, evaluating each distinct (r, p) lane once.
	RISEPel SchlickChannelFactors( const SchlickPairGeometry& g, const RISEPel& r, const RISEPel& p )
	{
		RISEPel out;
		for( int ch = 0; ch < 3; ch++ ) {
			int reuse = -1;
			for( int prev = 0; prev < ch; prev++ ) {
				if( r[prev] == r[ch] && p[prev] == p[ch] ) { reuse = prev; break; }
			}
			out[ch] = ( reuse >= 0 ) ? out[reuse] : SchlickLaneFactor( g, r[ch], p[ch] );
		}
		return out;
	}
}

RISEPel SchlickBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	ScalarTriple rt = pRoughness->GetValuesAt(ri);
	const ScalarTriple it = pIsotropy->GetValuesAt(ri);

	// DL-65 (sibling of DL-62): widen by the same glossy-filter amount
	// SchlickSPF::Scatter/ScatterNM/Pdf/PdfNM already apply to their
	// sampling/density roughness -- NEE evaluation and BSDF-sampled
	// continuation must agree on which surface roughness is being
	// rendered at this hit.
	if( ri.glossyFilterWidth > 0 ) {
		for( int ch = 0; ch < 3; ch++ ) {
			rt.v[ch] = r_min( rt.v[ch] + ri.glossyFilterWidth, Scalar(1.0) );
		}
	}

	const RISEPel rPel( rt.v[0], rt.v[1], rt.v[2] );
	const RISEPel iPel( it.v[0], it.v[1], it.v[2] );

	// Flip to the ray-facing frame, mirroring SchlickSPF's FlipW (same
	// condition), so value() agrees with Scatter()/Pdf() on back-face hits.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	SchlickPairGeometry g;
	if( !SchlickPair( vLightIn, ri, myonb, g ) ) {
		return RISEPel(0,0,0);
	}
	const RISEPel factor = SchlickChannelFactors( g, rPel, iPel );
	if( ColorMath::MaxValue(factor) > 0 ) {
		const RISEPel rho = pSpecular->GetColor(ri);
		return (pDiffuse->GetColor(ri)*INV_PI) + ((rho + (RISEPel(1.0,1.0,1.0)-rho)*g.fresnel) * factor);
	}

	return RISEPel(0,0,0);
}

Scalar SchlickBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Same ray-facing flip as value() above.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	// DL-65: same glossy-filter widening as value() above.
	Scalar roughnessNM = pRoughness->GetValueAtNM(ri,nm);
	if( ri.glossyFilterWidth > 0 ) {
		roughnessNM = r_min( roughnessNM + ri.glossyFilterWidth, Scalar(1.0) );
	}
	SchlickPairGeometry g;
	if( !SchlickPair( vLightIn, ri, myonb, g ) ) {
		return 0;
	}
	const Scalar factor = SchlickLaneFactor( g, roughnessNM, pIsotropy->GetValueAtNM(ri,nm) );
	if( factor > 0 ) {
		const Scalar rho = GuardedGetColorNM( *pSpecular, ri, nm );
		return (GuardedGetColorNM( *pDiffuse, ri, nm )*INV_PI) + (rho + (1.0-rho)*g.fresnel) * factor;
	}

	return 0;
}

RISEPel SchlickBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Deterministic half-vector quadrature of the corrected (DL-178 Eq.31,
	// DL-225 bounded) directional reflectance, for the noise-free
	// OIDN/preview AOV only. Sampling
	// p_h=t*Z/pi cancels the sharp Z peak analytically; uniform azimuth
	// leaves A in the weight. Substitute xi=u^2 (Jacobian 2u) to
	// remove the 1/sqrt(xi) endpoint at grazing. No transport clamp.
	OrthonormalBasis3D onb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(),onb.w()) > NEARZERO ) onb.FlipW();
	const Vector3 v = Vector3Ops::Normalize(-ri.ray.Dir());
	const Scalar nv = Vector3Ops::Dot(v,onb.w());
	if( nv <= 0 ) return RISEPel(0,0,0);
	const Vector3& rawG = Vector3Ops::SquaredModulus(ri.vGeomNormal)>Scalar(1e-12)
		? ri.vGeomNormal : onb.w();
	const Vector3 g = Vector3Ops::Dot(rawG,ri.ray.Dir())<0 ? rawG : -rawG;
	const Scalar vx=Vector3Ops::Dot(v,onb.u()), vy=Vector3Ops::Dot(v,onb.v());
	const Scalar gx=Vector3Ops::Dot(g,onb.u()), gy=Vector3Ops::Dot(g,onb.v());
	const Scalar gz=Vector3Ops::Dot(g,onb.w()), vg=Vector3Ops::Dot(v,g);
	const RISEPel rd=pDiffuse->GetColor(ri), rho=pSpecular->GetColor(ri);
	ScalarTriple rough=pRoughness->GetValuesAt(ri);
	const ScalarTriple iso=pIsotropy->GetValuesAt(ri);
	const int nt=16, np=32;
	Scalar cp[np],sp[np];
	for(int j=0;j<np;++j) { const Scalar ph=TWO_PI*(j+0.5)/np; cp[j]=cos(ph);sp[j]=sin(ph); }
	RISEPel result(0,0,0);
	Scalar m0[3]={0,0,0}, m5[3]={0,0,0}, effectiveR[3];
	for(int ch=0;ch<3;++ch) {
		const Scalar r=ri.glossyFilterWidth>0 ? r_min(rough.v[ch]+ri.glossyFilterWidth,Scalar(1)) : rough.v[ch];
		const Scalar p=iso.v[ch];
		effectiveR[ch]=r;
		int reuse=-1;
		for(int prev=0;prev<ch;++prev) if(effectiveR[prev]==r && iso.v[prev]==p) { reuse=prev; break; }
		if(reuse>=0) { m0[ch]=m0[reuse];m5[ch]=m5[reuse]; } else {
		// DL-225: the same bounded masking value() evaluates.
		SchlickMasking::Lane lane;
		SchlickMasking::Prepare(lane,r,p);
		const Scalar denV=SchlickMasking::MaskDen(lane,nv,vx,vy);
		Scalar az[np];
		for(int j=0;j<np;++j) az[j]=sqrt(p/(p*p+(1-p*p)*sp[j]*sp[j]));
		for(int i=0;i<nt;++i) {
			const Scalar u=(i+0.5)/nt, x=u*u;
			const Scalar t=sqrt(x/(r+(1-r)*x)), st=sqrt(1-t*t);
			for(int j=0;j<np;++j) {
				const Scalar hx=st*cp[j],hy=st*sp[j];
				const Scalar hv=hx*vx+hy*vy+t*nv, nl=2*hv*t-nv;
				if(hv<=0 || nl<=0 || 2*hv*(hx*gx+hy*gy+t*gz)-vg<=0) continue;
				const Scalar f=1-hv, f2=f*f, F=f2*f2*f;
				const Scalar lx=2*hv*hx-vx, ly=2*hv*hy-vy;
				const Scalar weight=2*u*az[j]*hv*nl
					/(t*denV*SchlickMasking::MaskDen(lane,nl,lx,ly));
				m0[ch]+=weight;m5[ch]+=weight*F;
			}
		}
		m0[ch]/=nt*np;m5[ch]/=nt*np;
		}
		const Scalar reflected=rd[ch]*(1+gz)*0.5+rho[ch]*m0[ch]+(1-rho[ch])*m5[ch];
		// IBSDF::albedo is a bounded auxiliary estimate. Authored additive
		// Rd plus specular can exceed one; transport still evaluates it.
		result[ch]=r_max(Scalar(0),r_min(Scalar(1),reflected));
	}
	return result;
}
