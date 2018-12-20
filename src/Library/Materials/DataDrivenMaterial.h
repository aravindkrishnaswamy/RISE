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

			virtual ~DataDrivenMaterial( )
			{
				safe_release( pBSDF );
			}

		public:
			DataDrivenMaterial( const char* filename )
			{
				pBSDF = new DataDrivenBSDF( filename );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pBSDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return 0; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

		};
	}
}

#endif
