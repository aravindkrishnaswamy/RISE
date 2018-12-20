//////////////////////////////////////////////////////////////////////
//
//  WardIsotropicGaussianMaterial.h - Implements Gred Ward's isotropic
//    gaussian material as described in his paper "Measuring and Modelling
//    Anisotropic Reflection" available here:
//    http://radsite.lbl.gov/radiance/papers/sg92/paper.html
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WARD_ISOTROPIC_GAUSSIAN_MATERIAL_
#define WARD_ISOTROPIC_GAUSSIAN_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "WardIsotropicGaussianBRDF.h"
#include "WardIsotropicGaussianSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class WardIsotropicGaussianMaterial :
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			WardIsotropicGaussianBRDF*				pBRDF;
			WardIsotropicGaussianSPF*				pSPF;

			virtual ~WardIsotropicGaussianMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			WardIsotropicGaussianMaterial( 
				const IPainter& diffuse,
				const IPainter& specular,
				const IPainter& alpha
				)
			{
				pBRDF = new WardIsotropicGaussianBRDF( diffuse, specular, alpha );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new WardIsotropicGaussianSPF( diffuse, specular, alpha );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "SPF" );
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
