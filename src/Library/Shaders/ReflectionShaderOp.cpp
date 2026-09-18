//////////////////////////////////////////////////////////////////////
//
//  ReflectionShaderOp.cpp - Implementation of the ReflectionShaderOp class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ReflectionShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

ReflectionShaderOp::ReflectionShaderOp(
	)
{
}

ReflectionShaderOp::~ReflectionShaderOp( )
{
}

//////////////////////////////////////////////////////////////////////
// DL-171: this op's own continuation's MIS partner.
//
// The RULING for this row is that Reflection/Refraction are DELTA
// continuations, so the partner is 0 (DL-74's rule) and both
// `EmissionShaderOp` (weight 1) and `LightSampler`'s NEE arms
// (`Pdf()==0` at a delta-only material) already agree with that.  That
// premise is true for the materials this op is historically paired
// with (`polished_material`, `dielectric_material`'s mirror lobe) but
// is NOT true in general: `GGXSPF::Scatter`/`ScatterNM` emit their
// ROUGH specular lobe with `type = eRayReflection` and `isDelta =
// false` (verified in source), so a scene that attaches a `ggx_material`
// to a `standard_shader`/`advanced_shader` op chain using this op
// (rather than the usual `pathtracing_*_rasterizer` path) would have
// a real, non-delta lobe pass through here silently un-weighted.  Gate
// on `scat.isDelta` rather than assuming it: 0 for a genuine delta
// lobe (unchanged), the material's own aggregate density -- the SAME
// function `LightSampler`'s NEE arm evaluates -- for a non-delta one.
static Scalar ReflectionMisPartner(
	const ISPF& spf,
	const RayIntersectionGeometric& ri,
	const ScatteredRay& scat,
	const IORStack& ior_stack
	)
{
	if( scat.isDelta ) {
		return 0;
	}
	const Scalar aggregatePdf = spf.Pdf( ri, scat.ray.Dir(), ior_stack );
	// DL-41 guard -- see `DistributionTracingShaderOp`'s twin.
	return aggregatePdf > 0 ? aggregatePdf : scat.pdf;
}

static Scalar ReflectionMisPartnerNM(
	const ISPF& spf,
	const RayIntersectionGeometric& ri,
	const ScatteredRay& scat,
	const Scalar nm,
	const IORStack& ior_stack
	)
{
	if( scat.isDelta ) {
		return 0;
	}
	const Scalar aggregatePdf = spf.PdfNM( ri, scat.ray.Dir(), nm, ior_stack );
	return aggregatePdf > 0 ? aggregatePdf : scat.pdf;
}

//! Tells the shader to apply shade to the given intersection point
void ReflectionShaderOp::PerformOperation(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [in/out] Resultant color from op
	const IORStack& ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);

	// Only do stuff on a normal pass
	if( !rc.IsNormalShadingPass() ) {
		return;
	}

	const ISPF* pSPFR = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;

	if( pScat ) {
		const ScatteredRayContainer& scattered = *pScat;
		for( unsigned int i=0; i<scattered.Count(); i++ ) {
			const ScatteredRay& scat = scattered[i];
			// NO eta^2 factor here (debt 30), and not by omission: this op
			// only ever consumes eRayReflection lobes, and a reflection
			// stays in the medium it started in.  DielectricSPF's
			// from-inside reflection branch allocates an UNCHANGED COPY of
			// the stack rather than leaving ior_stack null, so
			// RadianceEtaScale would return exactly 1 on every ray that
			// reaches this loop.  Its sibling RefractionShaderOp, which
			// consumes the lobe that does change medium, applies it.
			if( scat.type==ScatteredRay::eRayReflection )
			{
				// Cast and add!
				RISEPel	reflectedPixel( 0.0 );
				Ray ray( scat.ray );
				ray.Advance( 1e-8 );

				IRayCaster::RAY_STATE rs2;

				rs2.depth = rs.depth+1;
				rs2.importance = rs.importance * ColorMath::MaxValue(scat.kray);
				rs2.considerEmission = true;
				rs2.type = IRayCaster::RAY_STATE::eRaySpecular;
				// DL-171: this continuation's own MIS partner (see
				// `ReflectionMisPartner`'s doc above) -- 0 for the
				// ordinary delta lobe, the aggregate density for a
				// non-delta one (e.g. a misconfigured GGX reflection).
				if( pSPFR ) {
					rs2.bsdfPdf = ReflectionMisPartner( *pSPFR, ri.geometric, scat, ior_stack );
					rs2.bsdfMisPdf = rs2.bsdfPdf;
				}

				caster.CastRay( rc, ri.geometric.rast, ray, reflectedPixel, rs2, 0, ri.pRadianceMap, scat.ior_stack ? *scat.ior_stack : ior_stack );
				c = c + (reflectedPixel * scat.kray);
			}
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar ReflectionShaderOp::PerformOperationNM(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar caccum,						///< [in] Current value for wavelength
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack& ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	Scalar c=0;

	// Only do stuff on a normal pass
	if( !rc.IsNormalShadingPass() ) {
		return 0;
	}

	const ISPF* pSPFR = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;

	if( pScat ) {
		const ScatteredRayContainer& scattered = *pScat;
		for( unsigned int i=0; i<pScat->Count(); i++ ) {
			const ScatteredRay& scat = scattered[i];
			// NO eta^2 factor here (debt 30), and not by omission: this op
			// only ever consumes eRayReflection lobes, and a reflection
			// stays in the medium it started in.  DielectricSPF's
			// from-inside reflection branch allocates an UNCHANGED COPY of
			// the stack rather than leaving ior_stack null, so
			// RadianceEtaScale would return exactly 1 on every ray that
			// reaches this loop.  Its sibling RefractionShaderOp, which
			// consumes the lobe that does change medium, applies it.
			if( scat.type==ScatteredRay::eRayReflection )
			{
				// Cast and add!
				Scalar	reflected = 0.0;
				Ray ray( scat.ray );
				ray.Advance( 1e-8 );

				IRayCaster::RAY_STATE rs2;

				rs2.depth = rs.depth+1;
				rs2.importance = rs.importance * scat.krayNM;
				rs2.considerEmission = true;
				rs2.type = IRayCaster::RAY_STATE::eRaySpecular;
				// DL-171: see `ReflectionMisPartnerNM`'s doc above.
				if( pSPFR ) {
					rs2.bsdfPdf = ReflectionMisPartnerNM( *pSPFR, ri.geometric, scat, nm, ior_stack );
					rs2.bsdfMisPdf = rs2.bsdfPdf;
				}

				caster.CastRayNM( rc, ri.geometric.rast, ray, reflected, rs2, nm, 0, ri.pRadianceMap, scat.ior_stack ? *scat.ior_stack : ior_stack );
				c = c + (reflected * scat.krayNM);
			}
		}
	}

	return c;
}
