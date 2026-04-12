//////////////////////////////////////////////////////////////////////
//
//  GGXMaterial.h - Material wrapper combining GGXBRDF and GGXSPF.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GGX_MATERIAL_
#define GGX_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "GGXBRDF.h"
#include "GGXSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class GGXMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			GGXBRDF*		pBRDF;
			GGXSPF*			pSPF;

			virtual ~GGXMaterial()
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			GGXMaterial(
				const IPainter& diffuse,
				const IPainter& specular,
				const IPainter& alphaX,
				const IPainter& alphaY,
				const IPainter& ior,
				const IPainter& ext
				)
			{
				pBRDF = new GGXBRDF( diffuse, specular, alphaX, alphaY, ior, ext );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new GGXSPF( diffuse, specular, alphaX, alphaY, ior, ext );
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
