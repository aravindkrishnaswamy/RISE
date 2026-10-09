//////////////////////////////////////////////////////////////////////
//
//  AdvancedShader.cpp - Implementation of the AdvancedShader class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 1, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AdvancedShader.h"
#include "../Utilities/Optics.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/IndependentSampler.h"
#include "DirectLightingShaderOp.h"
#include "DistributionTracingShaderOp.h"
#include "PathTracingShaderOp.h"
#include "ReflectionShaderOp.h"
#include "RefractionShaderOp.h"
#include "AreaLightShaderOp.h"
#include "EmissionShaderOp.h"
#include "FinalGatherShaderOp.h"
#include "GlobalPelPhotonMapShaderOp.h"
#include "GlobalSpectralPhotonMapShaderOp.h"
#include "CausticPelPhotonMapShaderOp.h"
#include "CausticSpectralPhotonMapShaderOp.h"
#include "TranslucentPelPhotonMapShaderOp.h"
#include "ShadowPhotonMapShaderOp.h"
#include "SSS/SubSurfaceScatteringShaderOp.h"
#include "SSS/DonnerJensenSkinSSSShaderOp.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace {
    // These exact native providers overwrite their result independently of
    // the incoming accumulator, including their early-return paths. Unknown
    // providers and input-dependent alpha/transparency ops retain provenance.
    bool SMSIndependentShaderResult(const IShaderOp& op) {
        const auto& type=typeid(op);
        return type==typeid(PathTracingShaderOp)
            || type==typeid(ReflectionShaderOp)
            || type==typeid(RefractionShaderOp)
            || type==typeid(AreaLightShaderOp)
            || type==typeid(EmissionShaderOp)
            || type==typeid(FinalGatherShaderOp)
            || type==typeid(GlobalPelPhotonMapShaderOp)
            || type==typeid(GlobalSpectralPhotonMapShaderOp)
            || type==typeid(CausticPelPhotonMapShaderOp)
            || type==typeid(CausticSpectralPhotonMapShaderOp)
            || type==typeid(TranslucentPelPhotonMapShaderOp)
            || type==typeid(ShadowPhotonMapShaderOp)
            || type==typeid(DirectLightingShaderOp)
            || type==typeid(DistributionTracingShaderOp)
            || type==typeid(SubSurfaceScatteringShaderOp)
            || type==typeid(DonnerJensenSkinSSSShaderOp);
    }
    bool SMSReplacesShaderResult(char operation) {return operation=='=' || operation=='e';}
}

void AdvancedShader::ResolveChainFlagsForDepth(
	unsigned int depth,
	bool& outHasDirectLightingOp,
	bool& outHasBsdfContinuationOp
	) const
{
	outHasDirectLightingOp = false;
	outHasBsdfContinuationOp = false;
	for( ShadeOpListType::const_iterator i=shaderops.begin(); i!=shaderops.end(); i++ ) {
		if( depth < i->nMinDepth || depth > i->nMaxDepth ) {
			continue;
		}
		if( dynamic_cast<DirectLightingShaderOp*>( i->pShaderOp ) ) {
			outHasDirectLightingOp = true;
		}
		if( dynamic_cast<DistributionTracingShaderOp*>( i->pShaderOp ) ) {
			outHasBsdfContinuationOp = true;
		}
	}
}

AdvancedShader::AdvancedShader(
	const ShadeOpListType& shaderops_
	) : 
  shaderops( shaderops_ ),
  bComputeSPF( false )
{
	for( ShadeOpListType::const_iterator i=shaderops.begin(); i!=shaderops.end(); i++ ) {
		if( i->pShaderOp->RequireSPF() ) {
			bComputeSPF = true;
			break;
		}
	}
}

AdvancedShader::~AdvancedShader( )
{
}

void AdvancedShader::Shade(
	const RuntimeContext& rc,					///< [in] The runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [out] RISEPel value at the point
	const IORStack& ior_stack				///< [in] Index of refraction stack
			) const
{
	const IScene* pScene = caster.GetAttachedScene();
	
	if( !pScene ) {
		return;
	}

    const bool incomingReference=rc.smsReferenceRadiance;
    SMSReferenceRadianceScope shaderReturn(rc);
    if(c[0]!=0 || c[1]!=0 || c[2]!=0) rc.smsReferenceRadiance=incomingReference;

	const ISPF* pSPF = ri.pMaterial?ri.pMaterial->GetSPF():0;

	ScatteredRayContainer scattered;
	if( pSPF && bComputeSPF ) {
		IndependentSampler fallbackSampler( rc.random );
		ISampler& scatterSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
		pSPF->Scatter( ri.geometric, scatterSampler, scattered, ior_stack );
	}

	// DL-171/DL-209: resolve THIS call's depth-filtered chain composition
	// -- see ResolveChainFlagsForDepth's doc.
	IRayCaster::RAY_STATE rs2 = rs;
	ResolveChainFlagsForDepth( rs.depth, rs2.chainHasNEEOp, rs2.chainHasBsdfContinuationOp );

	// Iterate through the shader ops and accumulate the results
	ShadeOpListType::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		const SHADE_OP& op = *i;
		if( rs.depth >= op.nMinDepth && rs.depth <= op.nMaxDepth ) {
			RISEPel cthis = c;
            const bool accumulatedReference=rc.smsReferenceRadiance;
            if(accumulatedReference && SMSIndependentShaderResult(*op.pShaderOp)) rc.smsReferenceRadiance=false;
			op.pShaderOp->PerformOperation( rc, ri, caster, rs2, cthis, ior_stack, pSPF?&scattered:0 );
            if(!SMSReplacesShaderResult(op.operation)) rc.smsReferenceRadiance=accumulatedReference || rc.smsReferenceRadiance;
			switch( op.operation ) {
				default:
				case 'a':
				case '+':
					c = c + cthis;
					break;
				case 's':
				case '-':
					c = c - cthis;
					break;
				case 'm':
				case '*':
					c = c * cthis;
					break;
				case 'd':
				case '/':
				case '\\':
					c = c * ColorMath::invert(cthis);
					break;
				case '=':
				case 'e':
					c = cthis;
					break;
				// We need to make up a few more
			}
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar AdvancedShader::ShadeNM( 
	const RuntimeContext& rc,					///< [in] The runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack& ior_stack				///< [in] Index of refraction stack
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
    SMSReferenceRadianceScope shaderReturn(rc);

	// DL-171/DL-209 -- see Shade's identical construction above.
	IRayCaster::RAY_STATE rs2 = rs;
	ResolveChainFlagsForDepth( rs.depth, rs2.chainHasNEEOp, rs2.chainHasBsdfContinuationOp );

	// Iterate through the shader ops and accumulate the results
	ShadeOpListType::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		const SHADE_OP& op = *i;
		if( rs.depth >= op.nMinDepth && rs.depth <= op.nMaxDepth ) {
            const bool accumulatedReference=rc.smsReferenceRadiance;
            if(accumulatedReference && SMSIndependentShaderResult(*op.pShaderOp)) rc.smsReferenceRadiance=false;
			const Scalar cthis = op.pShaderOp->PerformOperationNM( rc, ri, caster, rs2, c, nm, ior_stack, pSPF?&scattered:0 );
            if(!SMSReplacesShaderResult(op.operation)) rc.smsReferenceRadiance=accumulatedReference || rc.smsReferenceRadiance;

			switch( op.operation ) {
				default:
				case 'a':
				case '+':
					c += cthis;
					break;
				case 's':
				case '-':
					c -= cthis;
					break;
				case 'm':
				case '*':
					c *= cthis;
					break;
				case 'd':
				case '/':
				case '\\':
					c /= cthis;
					break;
				case '=':
				case 'e':
					c = cthis;
					break;
				// We need to make up a few more
			}
		}
	}

	return c;
}

void AdvancedShader::ShadeHWSS(
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

	// Scatter using the hero wavelength only
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

	// DL-171/DL-209 -- see Shade's identical construction above.
	IRayCaster::RAY_STATE rs2 = rs;
	ResolveChainFlagsForDepth( rs.depth, rs2.chainHasNEEOp, rs2.chainHasBsdfContinuationOp );

	// Iterate through shader ops with depth filtering and blend modes
	ShadeOpListType::const_iterator it, e;
	for( it=shaderops.begin(), e=shaderops.end(); it!=e; it++ )
	{
		const SHADE_OP& op = *it;
		if( rs.depth >= op.nMinDepth && rs.depth <= op.nMaxDepth )
		{
			Scalar opResult[SampledWavelengths::N];
			op.pShaderOp->PerformOperationHWSS( rc, ri, caster, rs2, caccum, swl,
				ior_stack, pSPF?&scattered:0, opResult );

			for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
			{
				switch( op.operation ) {
					default:
					case 'a':
					case '+':
						caccum[i] += opResult[i];
						break;
					case 's':
					case '-':
						caccum[i] -= opResult[i];
						break;
					case 'm':
					case '*':
						caccum[i] *= opResult[i];
						break;
					case 'd':
					case '/':
					case '\\':
						if( opResult[i] != 0 )
							caccum[i] /= opResult[i];
						break;
					case '=':
					case 'e':
						caccum[i] = opResult[i];
						break;
				}
			}
		}
	}

	for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		c[i] = caccum[i];
}

//! Tells the shader to reset itself
void AdvancedShader::ResetRuntimeData() const
{
	ShadeOpListType::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		i->pShaderOp->ResetRuntimeData();
	}
}


