//////////////////////////////////////////////////////////////////////
//
//  DataDrivenMaterial.h - Defines a data driven material
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DATA_DRIVEN_MATERIAL_H
#define DATA_DRIVEN_MATERIAL_H

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "DataDrivenBSDF.h"
#include "DataDrivenSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class DataDrivenMaterial : 
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			DataDrivenBSDF*				pBSDF;
			DataDrivenSPF*				pSPF;				///< DL-325: samples pBSDF's own table

			virtual ~DataDrivenMaterial( )
			{
				safe_release( pSPF );
				safe_release( pBSDF );
			}

		public:
			DataDrivenMaterial( const char* filename )
			{
				pBSDF = new DataDrivenBSDF( filename );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

				// DL-325: a material whose BSDF is non-zero must sample it.  Without an SPF the
				// sampled function was identically 0 (PT: NEE only; BDPT: black).
				pSPF = new DataDrivenSPF( *pBSDF );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pBSDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

		};
	}
}

#endif
