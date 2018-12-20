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
				const IPainter& roughness,
				const IPainter& isotropy
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
		};
	}
}

#endif
