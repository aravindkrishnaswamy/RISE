//////////////////////////////////////////////////////////////////////
//
//  LambertianLuminaireMaterial.h - Defines a material that 
//  emits light uniformly over its surface, and regardless of the 
//  incoming angle, however it expects another material to 
//  do all the rest of the work
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LAMBERTIAN_LUMINAIRE_MATERIAL_
#define LAMBERTIAN_LUMINAIRE_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/ILog.h"
#include "LambertianEmitter.h"

namespace RISE
{
	namespace Implementation
	{
		class LambertianLuminaireMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			const IMaterial&			pMaterial;			/// Base material without emission
			LambertianEmitter*			pEmitter;			/// Emission properties

			virtual ~LambertianLuminaireMaterial()
			{
				pMaterial.release();

				safe_release( pEmitter );
			}

		public:
			LambertianLuminaireMaterial( const IPainter& radEx, const Scalar scale, const IMaterial& pMaterial_ ) : 
			pMaterial( pMaterial_ )
			{
				pMaterial.addref();

				pEmitter = new LambertianEmitter( radEx, scale );
				GlobalLog()->PrintNew( pEmitter, __FILE__, __LINE__, "pEmitter" );
			}
			
			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pMaterial.GetBSDF(); };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pMaterial.GetSPF(); };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return pEmitter; };
		};
	}
}

#endif
