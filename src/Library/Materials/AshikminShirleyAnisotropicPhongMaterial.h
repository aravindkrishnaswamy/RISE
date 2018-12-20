//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleyAnisotropicPhongMaterial.h - Defines
//  of the Shirley / Ashikhmin Anisotropic Phong material
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ASHIKMINSHIRLEY_ANISOTROPIC_PHONG_MATERIAL_
#define ASHIKMINSHIRLEY_ANISOTROPIC_PHONG_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/ILog.h"
#include "AshikminShirleyAnisotropicPhongBRDF.h"
#include "AshikminShirleyAnisotropicPhongSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class AshikminShirleyAnisotropicPhongMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			AshikminShirleyAnisotropicPhongBRDF*	pBRDF;
			AshikminShirleyAnisotropicPhongSPF*		pSPF;

			virtual ~AshikminShirleyAnisotropicPhongMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			AshikminShirleyAnisotropicPhongMaterial(
				const IPainter& Nu_, 
				const IPainter& Nv_, 
				const IPainter& Rd_, 
				const IPainter& Rs_ 
				)
			{
				pBRDF = new AshikminShirleyAnisotropicPhongBRDF( Nu_, Nv_, Rd_, Rs_ );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new AshikminShirleyAnisotropicPhongSPF( Nu_, Nv_, Rd_, Rs_ );
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

