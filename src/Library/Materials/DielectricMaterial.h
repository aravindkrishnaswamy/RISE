//////////////////////////////////////////////////////////////////////
//
//  DielectricMaterial.h - Defines a dielectric material, which is
//  is basically like glass
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIELECTRIC_MATERIAL_
#define DIELECTRIC_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "DielectricSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class DielectricMaterial : 
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			DielectricSPF* pSPF;

			virtual ~DielectricMaterial( )
			{
				safe_release( pSPF );
			}

		public:
			DielectricMaterial( const IPainter& tau_, const IPainter& ri, const IPainter& scat, const bool hg )
			{
				pSPF = new DielectricSPF( tau_, ri, scat, hg );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {		return 0; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };
		};
	}
}

#endif

