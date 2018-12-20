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

using namespace RISE;
using namespace RISE::Implementation;

StandardShader::StandardShader(
	const std::vector<IShaderOp*>& shaderops_
	) : 
  shaderops( shaderops_ ), 
  bComputeSPF( false )
{
	for( std::vector<IShaderOp*>::const_iterator i=shaderops.begin(); i!=shaderops.end(); i++ ) {
		if( (*i)->RequireSPF() ) {
			bComputeSPF = true;
			break;
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
	std::vector<IShaderOp*>::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		RISEPel cthis = c;
		(*i)->PerformOperation( rc, ri, caster, rs, cthis, ior_stack, pSPF?&scattered:0 );
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
	std::vector<IShaderOp*>::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		c += (*i)->PerformOperationNM( rc, ri, caster, rs, c, nm, ior_stack, pSPF?&scattered:0 );
	}

	return c;
}

//! Tells the shader to reset itself
void StandardShader::ResetRuntimeData() const
{
	std::vector<IShaderOp*>::const_iterator i, e;
	for( i=shaderops.begin(), e=shaderops.end(); i!=e; i++ ) {
		(*i)->ResetRuntimeData();
	}
}


