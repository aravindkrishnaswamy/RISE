//////////////////////////////////////////////////////////////////////
//
//  RefractionShaderOp.cpp - Implementation of the RefractionShaderOp class
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
#include "RefractionShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

RefractionShaderOp::RefractionShaderOp(
	)
{
}

RefractionShaderOp::~RefractionShaderOp( )
{
}

//////////////////////////////////////////////////////////////////////
// DL-171: this op's own continuation's MIS partner -- see
// `ReflectionShaderOp`'s identical twin for the full rationale (the
// materials this op is historically paired with are pure-delta
// refractors, but the gate is `scat.isDelta`, not an assumption, in
// case a non-delta `eRayRefraction` lobe is ever routed through here).
static Scalar RefractionMisPartner(
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
	return aggregatePdf > 0 ? aggregatePdf : scat.pdf;
}

static Scalar RefractionMisPartnerNM(
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
void RefractionShaderOp::PerformOperation(
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
			if( scat.type==ScatteredRay::eRayRefraction )
			{
				// Cast and add!
				RISEPel	refractedPixel( 0.0 );
				Ray ray( scat.ray );
				ray.Advance( 1e-8 );

				IRayCaster::RAY_STATE rs2;

				// eta^2 basic-radiance factor (debt 30).  The legacy
				// shader-op chain is a RADIANCE-mode walk exactly like the
				// integrators, and a refraction lobe is the one lobe that
				// always changes medium, so this is the site where the
				// factor bites hardest: before 2026-09-12 a luminaire seen
				// through glass under `pixelpel_rasterizer` read n^2 too
				// bright.  kray carries Fresnel and Beer's law only --
				// Interfaces/ISPF.h.
				const Scalar etaScale = RadianceEtaScale( ior_stack, scat.ior_stack );

				rs2.depth = rs.depth+1;
				rs2.importance = rs.importance * ColorMath::MaxValue(scat.kray) * etaScale;
				rs2.considerEmission = true;
				rs2.type = IRayCaster::RAY_STATE::eRaySpecular;
				// DL-171: this continuation's own MIS partner -- see
				// `RefractionMisPartner`'s doc above.
				if( pSPFR ) {
					rs2.bsdfPdf = RefractionMisPartner( *pSPFR, ri.geometric, scat, ior_stack );
					rs2.bsdfMisPdf = rs2.bsdfPdf;
				}

				caster.CastRay( rc, ri.geometric.rast, ray, refractedPixel, rs2, 0, ri.pRadianceMap, scat.ior_stack ? *scat.ior_stack : ior_stack );
				c = c + (refractedPixel * scat.kray * etaScale);
			}
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar RefractionShaderOp::PerformOperationNM(
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
			if( scat.type==ScatteredRay::eRayRefraction )
			{
				// Cast and add!
				Scalar	refracted = 0.0;
				Ray ray( scat.ray );
				ray.Advance( 1e-8 );

				IRayCaster::RAY_STATE rs2;

				// Same eta^2 factor as the Pel twin above.  The stack this
				// ray carries was pushed with the WAVELENGTH's IOR by
				// DielectricSPF::ScatterNM, so a dispersive medium gets a
				// per-wavelength factor here for free.
				const Scalar etaScale = RadianceEtaScale( ior_stack, scat.ior_stack );

				rs2.depth = rs.depth+1;
				rs2.importance = rs.importance * scat.krayNM * etaScale;
				rs2.considerEmission = true;
				rs2.type = IRayCaster::RAY_STATE::eRaySpecular;
				// DL-171: see `RefractionMisPartnerNM`'s doc above.
				if( pSPFR ) {
					rs2.bsdfPdf = RefractionMisPartnerNM( *pSPFR, ri.geometric, scat, nm, ior_stack );
					rs2.bsdfMisPdf = rs2.bsdfPdf;
				}

				caster.CastRayNM( rc, ri.geometric.rast, ray, refracted, rs2, nm, 0, ri.pRadianceMap, scat.ior_stack ? *scat.ior_stack : ior_stack );
				c = c + (refracted * scat.krayNM * etaScale);
			}
		}
	}

	return c;
}
