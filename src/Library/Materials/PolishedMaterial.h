//////////////////////////////////////////////////////////////////////
//
//  PolishedMaterial.h - A polished material is a diffuse substrate
//  with a thin dielectric covering
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POLISHED_MATERIAL_
#define POLISHED_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "LambertianBRDF.h"
#include "PolishedSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class PolishedMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			LambertianBRDF*	pBRDF;
			PolishedSPF*	pSPF;

			virtual ~PolishedMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			PolishedMaterial( 
				const IPainter& Rd_, 
				const IPainter& tau_, 
				const IPainter& Nt_,
				const IPainter& s,
				const bool hg
				)
			{
				pBRDF = new LambertianBRDF( Rd_ );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new PolishedSPF( Rd_, tau_, Nt_, s, hg );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {		return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };
		};
	}
}

#endif
