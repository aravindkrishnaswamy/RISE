//////////////////////////////////////////////////////////////////////
//
//  OrenNayarMaterial.h - 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ORENNAYAR_MATERIAL_
#define ORENNAYAR_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "OrenNayarBRDF.h"
#include "OrenNayarSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class OrenNayarMaterial : 
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			OrenNayarBRDF*				pBRDF;
			OrenNayarSPF*				pSPF;

			virtual ~OrenNayarMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			OrenNayarMaterial(
				const IPainter& reflectance, 
				const IPainter& roughness
				)
			{
				pBRDF = new OrenNayarBRDF( reflectance, roughness );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new OrenNayarSPF( reflectance, roughness );
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
