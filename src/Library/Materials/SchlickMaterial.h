//////////////////////////////////////////////////////////////////////
//
//  SchlickMaterial.h - 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCHLICK_MATERIAL_
#define SCHLICK_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "SchlickBRDF.h"
#include "SchlickSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class SchlickMaterial : 
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			SchlickBRDF*			pBRDF;
			SchlickSPF*				pSPF;

			virtual ~SchlickMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			SchlickMaterial(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& roughness,
				const IScalarPainter& isotropy
				)
			{
				pBRDF = new SchlickBRDF( diffuse, specular, roughness, isotropy );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new SchlickSPF( diffuse, specular, roughness, isotropy );
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
			inline const IPainter&       GetDiffuse()   const { return pBRDF->GetDiffuse(); }
			inline const IPainter&       GetSpecular()  const { return pBRDF->GetSpecular(); }
			inline const IScalarPainter& GetRoughness() const { return pBRDF->GetRoughness(); }
			inline const IScalarPainter& GetIsotropy()  const { return pBRDF->GetIsotropy(); }
			inline void SetDiffuse( const IPainter& v )       { pBRDF->SetDiffuse( v );   pSPF->SetDiffuse( v ); }
			inline void SetSpecular( const IPainter& v )      { pBRDF->SetSpecular( v );  pSPF->SetSpecular( v ); }
			inline void SetRoughness( const IScalarPainter& v ){ pBRDF->SetRoughness( v ); pSPF->SetRoughness( v ); }
			inline void SetIsotropy( const IScalarPainter& v ) { pBRDF->SetIsotropy( v );  pSPF->SetIsotropy( v ); }
		};
	}
}

#endif
