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

using namespace RISE;
using namespace RISE::Implementation;

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
	const IORStack* const ior_stack				///< [in/out] Index of refraction stack
			) const
{
	const IScene* pScene = caster.GetAttachedScene();
	
	if( !pScene ) {
		return;
	}

	const ISPF* pSPF = ri.pMaterial?ri.pMaterial->GetSPF():0;

	ScatteredRayContainer scattered;
	if( pSPF && bComputeSPF ) {
		pSPF->Scatter( ri.geometric, rc.random, scattered, ior_stack );	
	}

	// Iterate through the shader ops and accumulate the results
	ShadeOpListType::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		const SHADE_OP& op = *i;
		if( rs.depth >= op.nMinDepth && rs.depth <= op.nMaxDepth ) {
			RISEPel cthis = c;
			op.pShaderOp->PerformOperation( rc, ri, caster, rs, cthis, ior_stack, pSPF?&scattered:0 );
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
	const IORStack* const ior_stack				///< [in/out] Index of refraction stack
	) const
{
	const IScene* pScene = caster.GetAttachedScene();
	
	if( !pScene || !ri.pMaterial ) {
		return 0;
	}

	const ISPF* pSPF = ri.pMaterial->GetSPF();

	ScatteredRayContainer scattered;
	if( pSPF ) {
		pSPF->ScatterNM( ri.geometric, rc.random, nm, scattered, ior_stack );	
	}

	Scalar c = 0;

	// Iterate through the shader ops and accumulate the results
	ShadeOpListType::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		const SHADE_OP& op = *i;
		if( rs.depth >= op.nMinDepth && rs.depth <= op.nMaxDepth ) {
			const Scalar cthis = op.pShaderOp->PerformOperationNM( rc, ri, caster, rs, c, nm, ior_stack, pSPF?&scattered:0 );

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

//! Tells the shader to reset itself
void AdvancedShader::ResetRuntimeData() const
{
	ShadeOpListType::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		i->pShaderOp->ResetRuntimeData();
	}
}


