//////////////////////////////////////////////////////////////////////
//
//  StandardShader.cpp - Implementation of the StandardShader class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 16, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "StandardShader.h"
#include "../Utilities/Optics.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/IndependentSampler.h"
#include "DirectLightingShaderOp.h"
#include "DistributionTracingShaderOp.h"

using namespace RISE;
using namespace RISE::Implementation;

StandardShader::StandardShader(
	const std::vector<IShaderOp*>& shaderops_
	) :
  shaderops( shaderops_ ),
  bComputeSPF( false ),
  bHasDirectLightingOp( false ),
  bHasBsdfContinuationOp( false )
{
	// DL-171/DL-209: resolve the chain-composition facts ONCE, from this
	// shader's own op list -- see RAY_STATE::chainHasNEEOp's doc.  No
	// `break` on the first match: all three flags must be checked over
	// the WHOLE list.
	for( std::vector<IShaderOp*>::const_iterator i=shaderops.begin(); i!=shaderops.end(); i++ ) {
		if( (*i)->RequireSPF() ) {
			bComputeSPF = true;
		}
		if( dynamic_cast<DirectLightingShaderOp*>( *i ) ) {
			bHasDirectLightingOp = true;
		}
		if( dynamic_cast<DistributionTracingShaderOp*>( *i ) ) {
			bHasBsdfContinuationOp = true;
		}
	}
}

StandardShader::~StandardShader( )
{
}

void StandardShader::Shade( 
			const RuntimeContext& rc,					///< [in] The runtime context
			const RayIntersection& ri,					///< [in] Intersection information 
			const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
			const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
			RISEPel& c,									///< [out] RISEPel value at the point
			const IORStack& ior_stack					///< [in] Index of refraction stack
			) const
{
	const IScene* pScene = caster.GetAttachedScene();

	if( !pScene ) {
		return;
	}

	const ISPF* pSPF = ri.pMaterial?ri.pMaterial->GetSPF():0;

	ScatteredRayContainer scattered;
	if( pSPF && bComputeSPF ) {
		IndependentSampler fallbackSampler( rc.random );
		ISampler& scatterSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
		pSPF->Scatter( ri.geometric, scatterSampler, scattered, ior_stack );
	}

	// DL-171/DL-209: stamp THIS shader's own resolved chain composition
	// onto a local copy of `rs` -- `DirectLightingShaderOp` and
	// `DistributionTracingShaderOp` read it back to decide whether a
	// competing strategy actually exists in this specific op list (see
	// RAY_STATE::chainHasNEEOp's doc).  A fresh copy per call, not a
	// mutation of the shared op instances, because a NAMED op (e.g. the
	// ubiquitous "DefaultDirectLighting" preset) can be referenced by
	// several DIFFERENT StandardShader instances with different siblings.
	IRayCaster::RAY_STATE rs2 = rs;
	rs2.chainHasNEEOp = bHasDirectLightingOp;
	rs2.chainHasBsdfContinuationOp = bHasBsdfContinuationOp;

	// Iterate through the shader ops and accumulate the results
	std::vector<IShaderOp*>::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		RISEPel cthis = c;
		(*i)->PerformOperation( rc, ri, caster, rs2, cthis, ior_stack, pSPF?&scattered:0 );
		c = c + cthis;
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar StandardShader::ShadeNM( 
	const RuntimeContext& rc,					///< [in] The runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack& ior_stack					///< [in] Index of refraction stack
	) const
{
	const IScene* pScene = caster.GetAttachedScene();

	if( !pScene || !ri.pMaterial ) {
		return 0;
	}

	const ISPF* pSPF = ri.pMaterial->GetSPF();

	ScatteredRayContainer scattered;
	if( pSPF ) {
		IndependentSampler fallbackSampler( rc.random );
		ISampler& scatterSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
		pSPF->ScatterNM( ri.geometric, scatterSampler, nm, scattered, ior_stack );
	}

	Scalar c = 0;

	// DL-171/DL-209 -- see Shade's identical construction above.
	IRayCaster::RAY_STATE rs2 = rs;
	rs2.chainHasNEEOp = bHasDirectLightingOp;
	rs2.chainHasBsdfContinuationOp = bHasBsdfContinuationOp;

	// Iterate through the shader ops and accumulate the results
	std::vector<IShaderOp*>::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		c += (*i)->PerformOperationNM( rc, ri, caster, rs2, c, nm, ior_stack, pSPF?&scattered:0 );
	}

	return c;
}

void StandardShader::ShadeHWSS(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	Scalar c[SampledWavelengths::N],
	SampledWavelengths& swl,
	const IORStack& ior_stack
	) const
{
	const IScene* pScene = caster.GetAttachedScene();

	if( !pScene || !ri.pMaterial ) {
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
			c[i] = 0;
		return;
	}

	const ISPF* pSPF = ri.pMaterial->GetSPF();

	// Scatter using the hero wavelength only — companions share
	// the hero's geometric direction through PerformOperationHWSS.
	ScatteredRayContainer scattered;
	if( pSPF ) {
		IndependentSampler fallbackSampler( rc.random );
		ISampler& scatterSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
		pSPF->ScatterNM( ri.geometric, scatterSampler, swl.HeroLambda(), scattered, ior_stack );
	}

	// Initialize per-wavelength accumulators
	Scalar caccum[SampledWavelengths::N];
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		caccum[i] = 0;

	// DL-171/DL-209 -- see Shade's identical construction above.  The
	// default `PerformOperationHWSS` (neither legacy op overrides it)
	// dispatches to `PerformOperationNM` per wavelength, which is where
	// this is actually read.
	IRayCaster::RAY_STATE rs2 = rs;
	rs2.chainHasNEEOp = bHasDirectLightingOp;
	rs2.chainHasBsdfContinuationOp = bHasBsdfContinuationOp;

	// Iterate through shader ops, dispatching to PerformOperationHWSS
	std::vector<IShaderOp*>::const_iterator it, e;
	for( it=shaderops.begin(), e=shaderops.end(); it!=e; it++ )
	{
		Scalar opResult[SampledWavelengths::N];
		(*it)->PerformOperationHWSS( rc, ri, caster, rs2, caccum, swl,
			ior_stack, pSPF?&scattered:0, opResult );
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
			caccum[i] += opResult[i];
	}

	for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		c[i] = caccum[i];
}

//! Tells the shader to reset itself
void StandardShader::ResetRuntimeData() const
{
	std::vector<IShaderOp*>::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		(*i)->ResetRuntimeData();
	}
}


