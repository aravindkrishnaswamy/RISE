//////////////////////////////////////////////////////////////////////
//
//  CompositeMaterial.h - Defines a material that is composed of
//    two other materials
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPOSITE_MATERIAL_
#define COMPOSITE_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "CompositeSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class CompositeMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			IBSDF*						pBRDF;
			ISPF*						pSPF;
			IEmitter*					pEmitter;

			virtual ~CompositeMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
				safe_release( pEmitter );
			}

		public:
			CompositeMaterial( 
				const IMaterial& top, 
				const IMaterial& bottom, 
				const unsigned int max_recur,
				const unsigned int max_reflection_recursion,		// maximum level of reflection recursion
				const unsigned int max_refraction_recursion,		// maximum level of refraction recursion
				const unsigned int max_diffuse_recursion,			// maximum level of diffuse recursion
				const unsigned int max_translucent_recursion,		// maximum level of translucent recursion
				const Scalar thickness								// thickness between the materials
				) : 
			pBRDF( 0 ), 
			pSPF( 0 ), 
			pEmitter( 0 )
			{
				if( top.GetBSDF() ) {
					pBRDF = top.GetBSDF();
					pBRDF->addref();
				} else if( bottom.GetBSDF() ) {
					pBRDF = bottom.GetBSDF();
					pBRDF->addref();
				}

				if( top.GetSPF() && bottom.GetSPF() ) {
					pSPF = new CompositeSPF( *top.GetSPF(), *bottom.GetSPF(), max_recur, max_reflection_recursion, max_refraction_recursion, max_diffuse_recursion, max_translucent_recursion, thickness );
				} else if( top.GetSPF() ) {
					pSPF = top.GetSPF();
					pSPF->addref();
				} else if( bottom.GetSPF() ){
					pSPF = bottom.GetSPF();
					pSPF->addref();
				}

				if( top.GetEmitter() ) {
					pEmitter = top.GetEmitter();
					pEmitter->addref();
				} else if( bottom.GetEmitter() ) {
					pEmitter = bottom.GetEmitter();
					pEmitter->addref();
				}
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return pEmitter; };
		};
	}
}

#endif
