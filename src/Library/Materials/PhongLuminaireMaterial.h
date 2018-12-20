//////////////////////////////////////////////////////////////////////
//
//  PhongLuminaireMaterial.h - Defines a material that 
//  emits light using the phong model over its surface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PHONG_LUMINAIRE_MATERIAL_
#define PHONG_LUMINAIRE_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/ILog.h"
#include "PhongEmitter.h"

namespace RISE
{
	namespace Implementation
	{
		class PhongLuminaireMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			const IMaterial&			pMaterial;
			IEmitter*					pEmitter;
			
			virtual ~PhongLuminaireMaterial()
			{
				pMaterial.release();
				safe_release( pEmitter );
			}

		public:
			PhongLuminaireMaterial( const IPainter& radEx, const Scalar scale, const IPainter& N, const IMaterial& pMaterial_ ) : 
			pMaterial( pMaterial_ )
			{
				pMaterial.addref();

				pEmitter = new PhongEmitter( radEx, scale, N );
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

