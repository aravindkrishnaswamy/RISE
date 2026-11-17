//////////////////////////////////////////////////////////////////////
//
//  EmissionShaderOp.cpp - Implementation of the EmissionShaderOp class
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
#include "EmissionShaderOp.h"
#include "../Lights/LightSampler.h"
#include "../Utilities/OptimalMISAccumulator.h"
#include "../Utilities/MISWeights.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/IGeometry.h"		// CanBeAreaLight(): SDF emitters get full BSDF weight

using namespace RISE;
using namespace RISE::Implementation;

EmissionShaderOp::EmissionShaderOp(
	)
{
}

EmissionShaderOp::~EmissionShaderOp( )
{
}

//! Tells the shader to apply shade to the given intersection point
void EmissionShaderOp::PerformOperation(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [in/out] Resultant color from op
	const IORStack& ior_stack,			///< [in] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);

	// Only do stuff on a normal pass or on final gather
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return;
	}

	IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;

	if( pEmitter && rs.considerEmission ) {
		// Emitter takes the GEOMETRIC normal — emitters define their
		// emissive hemisphere relative to the actual surface face
		// (LambertianEmitter: `Dot(out, N) > 0` is a side-of-surface
		// test that must be unaffected by bump perturbation).
		c = pEmitter->emittedRadiance( ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vGeomNormal );
		// DL-633: light solo blacks out every other emitter for a marked ray
		// (`emitterHitFollowsLightTable`), before any geometry or MIS test, as
		// the path tracer's own PART 1 does.  (The frozen legacy chain's rays
		// are left as they were.)
		if( rs.emitterHitFollowsLightTable ) {
			const LightSampler* pSoloLS = caster.GetLightSampler();
			if( pSoloLS && pSoloLS->IsSoloActive() && !pSoloLS->IsSoloTargetLuminary( ri.pObject ) ) {
				c = RISEPel( 0.0 );
				return;
			}
		}

		// MIS weight for BSDF-sampled emission.
		// When bsdfPdf > 0, this ray was traced via BSDF importance sampling.
		// We weight the emission by w_bsdf = p_bsdf² / (p_bsdf² + p_light²).
		// When bsdfPdf == 0 (view ray or delta), full weight (no MIS).
		// What p_light is depends on who produced the ray: see the two
		// branches on `rs.emitterHitFollowsLightTable` below (DL-633).
		// An emitter on geometry that cannot be uniformly area-sampled (CanBeAreaLight()
		// false, OR NO geometry at all -- e.g. a csg_object, see LuminaryManager::
		// AddToLuminaryList) is NOT in the NEE light set (LuminaryManager skips it), so
		// the light-sampling strategy's pdf for this BSDF hit is ZERO -> the emission
		// must take FULL weight.  Skipping the block leaves it unweighted (and un-zeroed
		// under RIS, so it is not lost).
		//
		// CRASH FIX (2026-07-31 fix round 2): this used to read
		// `( !pEmitGeom || pEmitGeom->CanBeAreaLight() )` -- INVERTED for the null-geometry
		// case, so a null pEmitGeom (a csg_object -- see LuminaryManager.cpp) evaluated
		// emitterNeeSampleable = TRUE, entered this block, and called
		// `ri.pObject->GetArea()`, which (pre this fix round) null-deref'd inside
		// Object::GetArea() -> pGeometry->GetArea().  Object::GetArea() now has its own
		// base-layer null guard (Object.cpp) and returns 0 for a null-geometry object, so
		// `area > 0` below would ALSO have caught this and left the emission correctly
		// unweighted -- but the semantics here must independently match the
		// CanBeAreaLight()==false precedent (null geometry is a STRICTLY STRONGER case of
		// "cannot be uniformly area-sampled", not a "sampleable" default), so the
		// condition is corrected to treat null the same as CanBeAreaLight()==false: NOT
		// NEE-sampleable, full BSDF weight, and no GetArea() call is even reached.
		const IGeometry* pEmitGeom = ri.pObject ? ri.pObject->GetGeometry() : 0;
		const bool emitterNeeSampleable = ( pEmitGeom && pEmitGeom->CanBeAreaLight() );
		// DL-74 (docs/DL74_ENV_NEE_GUIDING_PARTITION.md): `p_bsdf` below is
		// the MIS PARTNER, not the training denominator, so it reads
		// `MisPartnerPdf()`.  The two differ for RayCaster's volume
		// phase-scatter continuation under volume guiding (`bsdfPdf` = the
		// guided mixture the direction was drawn from, `bsdfMisPdf` = the
		// raw phase pdf the matching NEE arm weights against); using the
		// guided one here broke that pair's partition of unity.  The gate
		// admits EITHER density being positive, matching
		// RayCasterEnvEscapeMISWeight; a zero partner keeps full weight.
		const Scalar misPartnerPdf = rs.MisPartnerPdf();
		if( ( rs.bsdfPdf > 0 || misPartnerPdf > 0 ) && ri.pObject && emitterNeeSampleable )
		{
			const Scalar area = ri.pObject->GetArea();
			if( area > 0 )
			{
				// cos at light surface — solid-angle <-> area Jacobian
				// uses the GEOMETRIC normal (Veach §8.2 / PBRT 4e §13.6.4).
				const Scalar cosLight = fabs( Vector3Ops::Dot( ri.geometric.ray.Dir(), ri.geometric.vGeomNormal ) );
				if( cosLight > 0 )
				{
					const Scalar dist = Vector3Ops::Magnitude(
						Vector3Ops::mkVector3( ri.geometric.ptIntersection, ri.geometric.ray.origin ) );
					const Scalar p_bsdf = misPartnerPdf;
					if( !rs.emitterHitFollowsLightTable ) {
						// A ray from the legacy shader-op chain.  That chain is frozen
						// and keeps its weight as it was: the partner density leaves
						// out the light-selection probability its NEE op applies, so
						// a hit is under-weighted when the light table has more than
						// one entry, and a hit light sampling could not have made is
						// weighted as if it could (docs/DEBT_LEDGER.md, "Known issues
						// in frozen code").
						const Scalar p_light = (dist * dist) / (area * cosLight);
						if( p_bsdf > 0 ) {
							const Scalar w_bsdf = (p_bsdf * p_bsdf) / (p_bsdf * p_bsdf + p_light * p_light);
							c = c * w_bsdf;
						}
					} else {
						// DL-633: a ray from the path tracer's subsurface continuation
						// or RayCaster's volume continuation, shaded here because the
						// hit object's shader does not name `pathtracing_shaderop`.
						// Weight it as the path tracer weights its own emitter hits
						// (PathTracingIntegrator.cpp PART 1): the partner density
						// carries the probability light sampling gave THIS emitter
						// at the producing vertex, which is zero when the luminary is
						// not in the light table, when the ray returned to the object
						// that vertex's light sampling excluded, or with no light
						// sampler; a zero keeps full weight; and light RIS takes a
						// selectable emitter whole (its NEE weight is 1).
						const LightSampler* pLS = caster.GetLightSampler();
						const bool bSelfHit = rs.pNEESelfObject && ri.pObject == rs.pNEESelfObject;
						const Scalar pdfSelect = ( pLS && !bSelfHit ) ? pLS->CachedPdfSelectLuminary(
							*ri.pObject, ri.geometric.ray.origin, ri.geometric.ray.Dir() ) : Scalar( 0 );
						const Scalar p_light = pdfSelect * (dist * dist) / (area * cosLight);
						if( p_bsdf > 0 && p_light > 0 ) {
							if( pLS->IsRISActive() ) {
								c = c * Scalar( 0 );
							} else if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() ) {
								// The NEE partner uses the trained weight once it is ready.
								c = c * MISWeights::OptimalMIS2Weight( p_bsdf, p_light,
									rc.pOptimalMIS->GetAlpha( ri.geometric.rast.x, ri.geometric.rast.y ) );
							} else {
								const Scalar w_bsdf = (p_bsdf * p_bsdf) / (p_bsdf * p_bsdf + p_light * p_light);
								c = c * w_bsdf;
							}
						}
					}
				}
			}
		}

	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function
Scalar EmissionShaderOp::PerformOperationNM(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar caccum,						///< [in] Current value for wavelength
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack& ior_stack,			///< [in] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	Scalar c=0;

	// Only do stuff on a normal pass or on final gather
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return 0;
	}

	IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;

	if( pEmitter && rs.considerEmission ) {
		// Geometric normal: see RGB path above for rationale.
		c = pEmitter->emittedRadianceNM( ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vGeomNormal, nm );
		// DL-633: light solo, as in the RGB twin above.
		if( rs.emitterHitFollowsLightTable ) {
			const LightSampler* pSoloLS = caster.GetLightSampler();
			if( pSoloLS && pSoloLS->IsSoloActive() && !pSoloLS->IsSoloTargetLuminary( ri.pObject ) ) {
				return 0;
			}
		}

		// MIS weight for BSDF-sampled emission (spectral)
		// An emitter on geometry that cannot be uniformly area-sampled (CanBeAreaLight()
		// false, OR NO geometry at all -- e.g. a csg_object) is NOT in the NEE light set
		// (LuminaryManager skips it), so the light-sampling strategy's pdf for this BSDF
		// hit is ZERO -> the emission must take FULL weight.  Skipping the block leaves
		// it unweighted (and un-zeroed under RIS, so it is not lost).  See the RGB path
		// above (PerformOperation) for the crash-fix-round-2 rationale on why the null
		// case is `pEmitGeom && ...` (NOT NEE-sampleable) rather than the previous
		// inverted `!pEmitGeom || ...`.
		const IGeometry* pEmitGeom = ri.pObject ? ri.pObject->GetGeometry() : 0;
		const bool emitterNeeSampleable = ( pEmitGeom && pEmitGeom->CanBeAreaLight() );
		// DL-74 -- see the RGB twin above.
		const Scalar misPartnerPdf = rs.MisPartnerPdf();
		if( ( rs.bsdfPdf > 0 || misPartnerPdf > 0 ) && ri.pObject && emitterNeeSampleable )
		{
			const Scalar area = ri.pObject->GetArea();
			if( area > 0 )
			{
				const Scalar cosLight = fabs( Vector3Ops::Dot( ri.geometric.ray.Dir(), ri.geometric.vGeomNormal ) );
				if( cosLight > 0 )
				{
					const Scalar dist = Vector3Ops::Magnitude(
						Vector3Ops::mkVector3( ri.geometric.ptIntersection, ri.geometric.ray.origin ) );
					const Scalar p_bsdf = misPartnerPdf;
					if( !rs.emitterHitFollowsLightTable ) {
						// A ray from the legacy shader-op chain.  That chain is frozen
						// and keeps its weight as it was: the partner density leaves
						// out the light-selection probability its NEE op applies, so
						// a hit is under-weighted when the light table has more than
						// one entry, and a hit light sampling could not have made is
						// weighted as if it could (docs/DEBT_LEDGER.md, "Known issues
						// in frozen code").
						const Scalar p_light = (dist * dist) / (area * cosLight);
						if( p_bsdf > 0 ) {
							const Scalar w_bsdf = (p_bsdf * p_bsdf) / (p_bsdf * p_bsdf + p_light * p_light);
							c = c * w_bsdf;
						}
					} else {
						// DL-633: a ray from the path tracer's subsurface continuation
						// or RayCaster's volume continuation, shaded here because the
						// hit object's shader does not name `pathtracing_shaderop`.
						// Weight it as the path tracer weights its own emitter hits
						// (PathTracingIntegrator.cpp PART 1): the partner density
						// carries the probability light sampling gave THIS emitter
						// at the producing vertex, which is zero when the luminary is
						// not in the light table, when the ray returned to the object
						// that vertex's light sampling excluded, or with no light
						// sampler; a zero keeps full weight; and light RIS takes a
						// selectable emitter whole (its NEE weight is 1).
						const LightSampler* pLS = caster.GetLightSampler();
						const bool bSelfHit = rs.pNEESelfObject && ri.pObject == rs.pNEESelfObject;
						const Scalar pdfSelect = ( pLS && !bSelfHit ) ? pLS->CachedPdfSelectLuminary(
							*ri.pObject, ri.geometric.ray.origin, ri.geometric.ray.Dir() ) : Scalar( 0 );
						const Scalar p_light = pdfSelect * (dist * dist) / (area * cosLight);
						if( p_bsdf > 0 && p_light > 0 ) {
							if( pLS->IsRISActive() ) {
								c = c * Scalar( 0 );
							} else if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() ) {
								// The NEE partner uses the trained weight once it is ready.
								c = c * MISWeights::OptimalMIS2Weight( p_bsdf, p_light,
									rc.pOptimalMIS->GetAlpha( ri.geometric.rast.x, ri.geometric.rast.y ) );
							} else {
								const Scalar w_bsdf = (p_bsdf * p_bsdf) / (p_bsdf * p_bsdf + p_light * p_light);
								c = c * w_bsdf;
							}
						}
					}
				}
			}
		}
	}

	return c;
}
