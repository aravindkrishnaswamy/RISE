//////////////////////////////////////////////////////////////////////
//
//  TranslucentMaterial.h - Defines a material that is partially
//  transparent (like a lampshade)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSLUCENT_MATERIAL_
#define TRANSLUCENT_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "TranslucentBSDF.h"
#include "TranslucentSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentMaterial :
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			TranslucentBSDF*				pBRDF;
			TranslucentSPF*					pSPF;

			virtual ~TranslucentMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			TranslucentMaterial( const IPainter& rF, const IPainter& T, const IPainter& ext, const IPainter& N_, const IPainter& scat )
			{
				pBRDF = new TranslucentBSDF( rF, T, N_ );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new TranslucentSPF( rF, T, ext, N_, scat );
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

