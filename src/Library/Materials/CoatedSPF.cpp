//////////////////////////////////////////////////////////////////////
//
//  CoatedSPF.cpp - Mixture importance sampler for `coated_material`.
//    See CoatedSPF.h for the estimator and its three consequences.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CoatedSPF.h"
#include "CoatedLayer.h"
#include "../Utilities/MicrofacetUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

CoatedSPF::CoatedSPF( const CoatedBRDF& brdf, const ISPF& baseSPF ) :
  pBRDF( &brdf ),
  pBaseSPF( &baseSPF )
{
	pBRDF->addref();
	pBaseSPF->addref();
}

CoatedSPF::~CoatedSPF()
{
	safe_release( pBRDF );
	safe_release( pBaseSPF );
}

namespace
{
	//! Ray-facing shading frame; identical construction to
	//! GGXSPF::Scatter's FlipW and to CoatedBRDF's, so evaluator and
	//! sampler share one frame.
	inline RISE::OrthonormalBasis3D RayFacingONB( const RISE::RayIntersectionGeometric& ri )
	{
		RISE::OrthonormalBasis3D onb = ri.onb;
		if( RISE::Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
			onb.FlipW();
		}
		return onb;
	}

	//! Ray-anchored geometric normal for the horizon gate.
	inline RISE::Vector3 GeomNormal( const RISE::RayIntersectionGeometric& ri, const RISE::Vector3& n )
	{
		const RISE::Vector3& raw = ( RISE::Vector3Ops::SquaredModulus( ri.vGeomNormal ) > RISE::Scalar(1e-12) )
			? ri.vGeomNormal : n;
		return ( RISE::Vector3Ops::Dot( raw, ri.ray.Dir() ) < 0 ) ? raw : -raw;
	}
}

Scalar CoatedSPF::PdfImpl(
	const RayIntersectionGeometric& ri,
	const Vector3& woIn,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n  = onb.w();
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 wo = Vector3Ops::Normalize( woIn );

	if( Vector3Ops::Dot( wi, n ) <= 0 ) {
		return 0;
	}
	if( Vector3Ops::Dot( wo, n ) <= 0 ) {
		return 0;
	}
	// Same geometric-horizon rejection the sampler applies, so a
	// direction Scatter can no longer emit carries zero density (MIS
	// consistency -- see GGXSPF::Pdf).
	if( Vector3Ops::Dot( wo, GeomNormal( ri, n ) ) <= 0 ) {
		return 0;
	}

	CoatedBRDF::CoatParams cp;
	pBRDF->ResolveCoat( ri, nm, cp );

	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	const Scalar pCoat = cp.weight * CoatedLayer::Fresnel( cosWi, cp.eta );

	const Scalar qCoat = MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, onb, cp.alpha, cp.alpha );
	const Scalar qBase = ( nm < 0 )
		? pBaseSPF->Pdf( ri, wo, ior_stack )
		: pBaseSPF->PdfNM( ri, wo, nm, ior_stack );

	return r_max( Scalar(0), pCoat * qCoat + ( Scalar(1) - pCoat ) * qBase );
}

Scalar CoatedSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return PdfImpl( ri, wo, Scalar(-1), ior_stack );
}

Scalar CoatedSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return PdfImpl( ri, wo, nm, ior_stack );
}

void CoatedSPF::ScatterImpl(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n     = onb.w();
	const Vector3 geomN = GeomNormal( ri, n );
	const Vector3 wi    = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	if( cosWi <= 0 ) {
		return;
	}

	CoatedBRDF::CoatParams cp;
	pBRDF->ResolveCoat( ri, nm, cp );

	// 7.5's selection weight: coverage x Fresnel.  The complement goes
	// to the substrate branch, where the base SPF distributes it across
	// its own lobes by its own weights.
	const Scalar pCoat = cp.weight * CoatedLayer::Fresnel( cosWi, cp.eta );

	const unsigned int before = scattered.Count();
	Vector3 wo;
	ScatteredRay::ScatRayType lobeType = ScatteredRay::eRayReflection;
	bool sampled = false;

	if( sampler.Get1D() < pCoat )
	{
		// --- coat lobe: anisotropic-capable VNDF sampling at the
		//     isotropic coat alpha (7.2 gives the coat one roughness).
		const Scalar u1 = sampler.Get1D();
		const Scalar u2 = sampler.Get1D();
		const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso( wi, onb, cp.alpha, cp.alpha, u1, u2 );
		const Scalar wiDotM = Vector3Ops::Dot( wi, m );
		if( wiDotM > 0 ) {
			wo = Vector3Ops::Normalize( m * ( Scalar(2) * wiDotM ) - wi );
			// eRayReflection is the honest tag for a coat lobe, and
			// every modern integrator reads it correctly.
			//
			// KNOWN, NOT FIXED: FinalGatherShaderOp
			// (FinalGatherShaderOp.cpp:663) picks its continuation with
			// RandomlySelectDiffuse, so under that legacy shader-op the
			// coat branch's samples are DROPPED and the gather sees a
			// substrate-only response.  That is a pre-existing property
			// of the op -- it does the same to GGX's and Polished's
			// specular lobes -- and fixing it means changing the op's
			// selection policy, which is out of scope here and would
			// move every existing final-gather render.  Recorded so the
			// next reader does not rediscover it as a coated bug.
			lobeType = ScatteredRay::eRayReflection;
			sampled = true;
		}
	}
	else
	{
		// --- substrate branch: the base SPF chooses the direction (and
		//     the lobe tag, which downstream heuristics read).  It
		//     writes straight into the caller's container so any IOR-
		//     stack state it attaches is preserved; its `kray` and
		//     `pdf` are OVERWRITTEN below with the layered values.
		//
		//     Every material on CoatedMaterial's substrate allowlist
		//     (Lambertian, Oren-Nayar, GGX) emits AT MOST ONE ray per
		//     Scatter call -- the allowlist is what makes the
		//     single-sample estimator below well-posed.  The loop still
		//     rewrites every ray the base added so no stale base-weight
		//     ray can escape if that ever changes.
		if( nm < 0 ) {
			pBaseSPF->Scatter( ri, sampler, scattered, ior_stack );
		} else {
			pBaseSPF->ScatterNM( ri, sampler, nm, scattered, ior_stack );
		}
		if( scattered.Count() > before ) {
			wo = Vector3Ops::Normalize( scattered[before].ray.Dir() );
			lobeType = scattered[before].type;
			sampled = true;
		}
	}

	if( !sampled ) {
		// The substrate branch may have left rejected rays behind; it
		// only reaches here having added none.
		return;
	}

	const Scalar cosWo = Vector3Ops::Dot( wo, n );
	const bool   valid = ( cosWo > 0 ) && ( Vector3Ops::Dot( wo, geomN ) > 0 );

	const Scalar q = valid ? PdfImpl( ri, wo, nm, ior_stack ) : Scalar(0);

	if( !valid || q <= Scalar(1e-12) )
	{
		// Zero the THROUGHPUT of anything the base contributed, so a
		// bare-substrate weight cannot escape as if it were a coated
		// one.  The base's own `pdf` is deliberately LEFT ALONE: the
		// ray carries no energy either way, and a non-delta ray with
		// pdf == 0 is a live 0/0 hazard in any downstream MIS
		// denominator, where it would turn a zero contribution into a
		// NaN one.
		//
		// Unreachable for the allowlisted substrates -- Lambertian,
		// Oren-Nayar and GGX all gate their own samples against the
		// shading hemisphere AND the same ray-anchored geometric
		// horizon this function uses, so a direction that survives
		// their gate survives this one.  Kept as a guard against a
		// future substrate with a looser gate.
		for( unsigned int i = before; i < scattered.Count(); ++i ) {
			scattered[i].kray   = RISEPel( 0, 0, 0 );
			scattered[i].krayNM = 0;
		}
		return;
	}

	if( scattered.Count() > before )
	{
		// Substrate branch: rewrite in place.
		for( unsigned int i = before; i < scattered.Count(); ++i )
		{
			ScatteredRay& s = scattered[i];
			const Vector3 sw    = Vector3Ops::Normalize( s.ray.Dir() );
			const Scalar  scos  = Vector3Ops::Dot( sw, n );
			const Scalar  sq    = ( i == before ) ? q : PdfImpl( ri, sw, nm, ior_stack );

			if( scos <= 0 || sq <= Scalar(1e-12) ) {
				// Same reasoning as the block above: kill the
				// throughput, keep the base's density.
				s.kray = RISEPel( 0, 0, 0 );
				s.krayNM = 0;
				continue;
			}

			if( nm < 0 ) {
				s.kray = pBRDF->value( sw, ri ) * ( scos / sq );
			} else {
				s.krayNM = pBRDF->valueNM( sw, ri, nm ) * ( scos / sq );
			}
			s.pdf     = sq;
			s.isDelta = false;
		}
	}
	else
	{
		// Coat branch: build the ray.
		ScatteredRay coat;
		coat.type    = lobeType;
		coat.isDelta = false;
		coat.ray.Set( ri.ptIntersection, wo );
		coat.pdf     = q;
		if( nm < 0 ) {
			coat.kray = pBRDF->value( wo, ri ) * ( cosWo / q );
		} else {
			coat.krayNM = pBRDF->valueNM( wo, ri, nm ) * ( cosWo / q );
		}
		scattered.AddScatteredRay( coat );
	}
}

void CoatedSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, Scalar(-1), scattered, ior_stack );
}

void CoatedSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, nm, scattered, ior_stack );
}

// DELIBERATELY NO EvaluateKrayNM OVERRIDE.
//
// An earlier revision of this file had one.  It computed
// `valueNM(nm) * cos / PdfImpl(nm)` -- but `PdfImpl(nm)` is the
// mixture density at the COMPANION wavelength, while the ray it is
// being asked about was drawn from the density at the HERO wavelength.
// Those two coincide only when every coat and substrate painter is
// wavelength-independent; the moment one is not (a dispersive
// `coat_ior`, a spectral substrate reflectance) the estimator divides
// by the wrong density, which is a BIAS rather than an approximation.
//
// Falling through to the ISPF default (-1) is strictly better: it
// routes PathTracingIntegrator to `valueNM * cos / pS->pdf`, and
// `pS->pdf` is the density stored ON the sampled ray -- the true hero
// mixture pdf that actually drew it.  Since `Scatter` writes exactly
// that value and `value`/`valueNM` are the same closed form the
// integrator re-evaluates, the default path is exact.
//
// The general rule this is an instance of: override EvaluateKrayNM
// only when the SPF's lobes are NOT fully represented by the
// material's IBSDF (the contract in ISPF.h).  Here they are -- that is
// the entire point of Phase 2 -- so there is nothing for an override
// to add and one thing for it to get wrong.
