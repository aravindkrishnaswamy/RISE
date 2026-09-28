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
			bool          bScattersFullSphere;   //!< DL-157 P2-1 / DL-24; see the ctor

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
				if( top.GetSPF() && bottom.GetSPF() ) {
					// DL-24 (2026-09-28): the composite prices its own
					// transport.  Both layers' BSDFs go INTO the SPF --
					// they are what its layered evaluator connects
					// through -- and the BSDF this material presents is
					// that SAME evaluator (CompositeBSDF), so NEE, BDPT/VCM
					// connections and the BSDF-sampled continuation
					// estimate one integral (DL-157's one function per
					// side).
					//
					// This SUPERSEDES the pre-DL-24 "top-wins" BSDF, which
					// presented ONE layer's closed form while the walk
					// sampled the stack: under a dielectric coat (whose
					// GetBSDF() is 0) NEE priced the BARE substrate --
					// unattenuated by the coat, and on top of the walk's own
					// substrate transport -- which read 1.37 in a
					// white-furnace BDPT render and ignored the coat's
					// Fresnel under a delta light entirely.
					//
					// DL-157 P2-1's rule is unchanged in principle -- the
					// full-sphere capability follows the BSDF actually
					// presented -- and the presented BSDF now never prices
					// a direction on the far side of the stack (that
					// transport is sampled by the walk as delta-tagged
					// rays, see CompositeSPF.h), so the capability is
					// false.  Pre-DL-24 `composite { top = translucent }`
					// claimed it because it presented the bare
					// TranslucentBSDF, pricing a transmission the walk over
					// an opaque bottom (mat_wax_gold's gold Lambertian)
					// never produces.
					CompositeSPF* pComposite = new CompositeSPF( *top.GetSPF(), *bottom.GetSPF(),
						max_recur, max_reflection_recursion, max_refraction_recursion, max_diffuse_recursion,
						max_translucent_recursion, thickness, extinction, top.GetBSDF(), bottom.GetBSDF() );
					pSPF = pComposite;
					if( pComposite->HasLayeredValue() ) {
						// Null composition (DL-126) is preserved: with no
						// BSDF on either layer there is nothing to evaluate,
						// every walked exit is delta-tagged, and the
						// material presents no BSDF, exactly as before.
						pBRDF = new CompositeBSDF( *pComposite, top.GetBSDF() ? top.GetBSDF() : bottom.GetBSDF() );
						GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "CompositeBSDF" );
					}
					bScattersFullSphere = false;
				} else {
					// Only one layer can scatter: that layer IS the
					// material (the pre-DL-24 behaviour, kept verbatim).
					if( top.GetBSDF() ) {
						pBRDF = top.GetBSDF();
						pBRDF->addref();
						bScattersFullSphere = top.ScattersFullSphere();
					} else if( bottom.GetBSDF() ) {
						pBRDF = bottom.GetBSDF();
						pBRDF->addref();
						bScattersFullSphere = bottom.ScattersFullSphere();
					}

					if( top.GetSPF() ) {
						pSPF = top.GetSPF();
						pSPF->addref();
					} else if( bottom.GetSPF() ){
						pSPF = bottom.GetSPF();
						pSPF->addref();
					}
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

			//! DL-157 P2-1 / DL-24: the answer of the BSDF `GetBSDF()`
			//! actually returns -- see the constructor's derivation.
			inline bool ScattersFullSphere() const { return bScattersFullSphere; }
		};
	}
}

#endif
