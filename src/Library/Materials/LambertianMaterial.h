//////////////////////////////////////////////////////////////////////
//
//  LambertianMaterial.h - Defines a material that is a perfect
//  lambertian reflector (ie, light is reflected in all directions
//  with a cosine distribution and with the same intensity)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LAMBERTIAN_MATERIAL_
#define LAMBERTIAN_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "LambertianBRDF.h"
#include "LambertianSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class LambertianMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			LambertianBRDF*				pBRDF;
			LambertianSPF*				pSPF;

			virtual ~LambertianMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			LambertianMaterial( const IPainter& ref )
			{
				pBRDF = new LambertianBRDF( ref );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new LambertianSPF( ref );
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
