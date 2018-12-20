//////////////////////////////////////////////////////////////////////
//
//  MAXMaterial.h - 3D Studio MAX nateruak, that calls gets MAX
//    to do the computations for us
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 5, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef MAX_MATERIAL_H
#define MAX_MATERIAL_H

#include "MAXBSDF.h"
#include "MAXSPF.h"

class MAXMaterial : public virtual RISE::IMaterial, public virtual RISE::Implementation::Reference
{
protected:
	MAXBSDF*				pBSDF;
	MAXSPF*					pSPF;

	virtual ~MAXMaterial( )
	{
		safe_release( pBSDF );
		safe_release( pSPF );
	}

public:
	MAXMaterial( 
		RiseRenderer* renderer,
		RiseRendererParams* rparams 
		)
	{
		pBSDF = new MAXBSDF( renderer, rparams );
		RISE::GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

		pSPF = new MAXSPF( *pBSDF );
		RISE::GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "pSPF" );
	}

	/// \return The BRDF for this material.  NULL If there is no BRDF
	inline RISE::IBSDF* GetBSDF() const {			return pBSDF; };

	/// \return The SPF for this material.  NULL If there is no SPF
	inline RISE::ISPF* GetSPF() const {			return pSPF; };

	/// \return The emission properties for this material.  NULL If there is not an emitter
	inline RISE::IEmitter* GetEmitter() const {	return 0; };

};

#endif

