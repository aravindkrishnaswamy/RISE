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
#include "../Interfaces/ILog.h"
#include "CompositeSPF.h"
#include "CompositeEmitter.h"

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
			bool          bScattersFullSphere;   //!< DL-157 P2-1; see the ctor

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
				const Scalar thickness,								// thickness between the materials
				const IScalarPainter& extinction					// extinction coefficient for absorption between layers (physical scalar)
				) :
			pBRDF( 0 ), 
			pSPF( 0 ), 
			pEmitter( 0 ),
			bScattersFullSphere( false )
			{
				// DL-157 P2-1 (2026-09-18): the full-sphere capability is
				// taken from WHICHEVER MATERIAL'S BSDF this composite ends
				// up presenting, because that is the `value()` NEE will
				// call and the capability's own safety condition is stated
				// about `value()` (IMaterial.h, and LightSampler.cpp's
				// FULL-SPHERE NEE block).  An OR over both layers would be
				// wrong in one direction that matters: a Lambertian top
				// over a translucent bottom presents the LAMBERTIAN BSDF,
				// which does not transmit, and granting the flag there
				// would light its back faces at full weight.
				//
				// The case this fixes is the other order --
				// `composite { top = translucent }`, which is
				// `mat_wax_gold` in scenes/Tests/Materials/composite_material.RISEscene:
				// `GetBSDF()` hands NEE a transmitting `TranslucentBSDF`
				// while `ScattersFullSphere()` said false, so the three NEE
				// arms broke at `cosSurface <= 0` over exactly the
				// half-space DL-157 had just taught that BSDF to price.
				// `CompositeSPF::Pdf` is still DL-24's documented 50/50
				// placeholder, so the MIS partition through a composite is
				// not closed by this -- it is closer, and no longer
				// systematically under-reading the transmissive half.
				if( top.GetBSDF() ) {
					pBRDF = top.GetBSDF();
					pBRDF->addref();
					bScattersFullSphere = top.ScattersFullSphere();
				} else if( bottom.GetBSDF() ) {
					pBRDF = bottom.GetBSDF();
					pBRDF->addref();
					bScattersFullSphere = bottom.ScattersFullSphere();
				}

				if( top.GetSPF() && bottom.GetSPF() ) {
					pSPF = new CompositeSPF( *top.GetSPF(), *bottom.GetSPF(), max_recur, max_reflection_recursion, max_refraction_recursion, max_diffuse_recursion, max_translucent_recursion, thickness, extinction );
				} else if( top.GetSPF() ) {
					pSPF = top.GetSPF();
					pSPF->addref();
				} else if( bottom.GetSPF() ){
					pSPF = bottom.GetSPF();
					pSPF->addref();
				}

				if( top.GetEmitter() && bottom.GetEmitter() ) {
					pEmitter = new CompositeEmitter( *top.GetEmitter(), *bottom.GetEmitter(), extinction, thickness );
					GlobalLog()->PrintNew( pEmitter, __FILE__, __LINE__, "CompositeEmitter" );
				} else if( top.GetEmitter() ) {
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

			//! DL-157 P2-1: the answer of whichever layer's BSDF `GetBSDF()`
			//! actually returned -- see the constructor's derivation.
			inline bool ScattersFullSphere() const { return bScattersFullSphere; }
		};
	}
}

#endif
