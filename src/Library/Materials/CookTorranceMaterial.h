//////////////////////////////////////////////////////////////////////
//
//  CookTorranceMaterial.h - 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COOKTORRANCE_MATERIAL_
#define COOKTORRANCE_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "CookTorranceBRDF.h"
#include "CookTorranceSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class CookTorranceMaterial : 
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			CookTorranceBRDF*				pBRDF;
			CookTorranceSPF*				pSPF;

			virtual ~CookTorranceMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			CookTorranceMaterial(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& masking,
				const IScalarPainter& ior,
				const IScalarPainter& ext
				)
			{
				pBRDF = new CookTorranceBRDF( diffuse, specular, masking, ior, ext );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new CookTorranceSPF( diffuse, specular, masking, ior, ext );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			//! Read-back + rebind for the interactive editor.  Material
			//! forwards to BOTH BRDF and SPF in lockstep.
			inline const IPainter&       GetDiffuse()    const { return pBRDF->GetDiffuse(); }
			inline const IPainter&       GetSpecular()   const { return pBRDF->GetSpecular(); }
			inline const IScalarPainter& GetMasking()    const { return pBRDF->GetMasking(); }
			inline const IScalarPainter& GetIOR()        const { return pBRDF->GetIOR(); }
			inline const IScalarPainter& GetExtinction() const { return pBRDF->GetExtinction(); }
			inline void SetDiffuse( const IPainter& v )        { pBRDF->SetDiffuse( v );    pSPF->SetDiffuse( v ); }
			inline void SetSpecular( const IPainter& v )       { pBRDF->SetSpecular( v );   pSPF->SetSpecular( v ); }
			inline void SetMasking( const IScalarPainter& v )  { pBRDF->SetMasking( v );    pSPF->SetMasking( v ); }
			inline void SetIOR( const IScalarPainter& v )      { pBRDF->SetIOR( v );        pSPF->SetIOR( v ); }
			inline void SetExtinction( const IScalarPainter& v ){ pBRDF->SetExtinction( v ); pSPF->SetExtinction( v ); }
		};
	}
}

#endif
