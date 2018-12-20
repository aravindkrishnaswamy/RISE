//////////////////////////////////////////////////////////////////////
//
//  PerfectRefractorMaterial.h - A material which 
//  refracts the incoming vector perfectly... 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  This is NOT a PHYSICALLY BASED material!
//  Its really only for test purposes
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PERFECT_REFRACTOR_MATERIAL_
#define PERFECT_REFRACTOR_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/ILog.h"
#include "PerfectRefractorSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class PerfectRefractorMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			PerfectRefractorSPF*	pSPF;

			virtual ~PerfectRefractorMaterial( )
			{
				safe_release( pSPF );
			}

		public:
			PerfectRefractorMaterial( const IPainter& ref, const IPainter& Nt_ )
			{
				pSPF = new PerfectRefractorSPF( ref, Nt_ );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return 0; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };
		};
	}
}

#endif

