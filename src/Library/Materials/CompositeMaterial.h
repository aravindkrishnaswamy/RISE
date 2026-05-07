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
		//! BRDF wrapper for the Khronos additive layered composition.
		//! Mirrors `CompositeSPF`'s additive branch so direct lighting
		//! (NEE) and forward path-tracing (Scatter) agree on the same
		//! `f_combined = f_top + f_base · (1 − topAlbedo)` per-vertex.
		//!
		//! Default opt-out: when `top.GetLayerAlbedo()` returns zero
		//! (every BSDF except sheen, today), this wrapper falls through
		//! to evaluating only the top BRDF — preserving the historical
		//! `CompositeMaterial::pBRDF = top.GetBSDF()` behaviour for
		//! dielectric / GGX / translucent composites whose composition
		//! lives in `CompositeSPF`'s random walk.  The base BRDF is
		//! engaged ONLY when the top layer opts in via the additive
		//! composition; otherwise the random walk on the SPF side is
		//! the source of truth.
		class CompositeBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			IBSDF& topBRDF;
			IBSDF& bottomBRDF;

			virtual ~CompositeBRDF()
			{
				topBRDF.release();
				bottomBRDF.release();
			}

		public:
			CompositeBRDF( IBSDF& top_, IBSDF& bottom_ )
				: topBRDF( top_ ), bottomBRDF( bottom_ )
			{
				topBRDF.addref();
				bottomBRDF.addref();
			}

			RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const override
			{
				const RISEPel f_top = topBRDF.value( vLightIn, ri );
				if( topBRDF.UsesAdditiveLayering() ) {
					const RISEPel topAlbedo = topBRDF.GetLayerAlbedo( ri );
					const RISEPel f_base = bottomBRDF.value( vLightIn, ri );
					return f_top + f_base * (RISEPel( 1, 1, 1 ) - topAlbedo);
				}
				return f_top;
			}

			Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const override
			{
				const Scalar f_top = topBRDF.valueNM( vLightIn, ri, nm );
				if( topBRDF.UsesAdditiveLayering() ) {
					const Scalar topAlbedo = topBRDF.GetLayerAlbedoNM( ri, nm );
					const Scalar f_base = bottomBRDF.valueNM( vLightIn, ri, nm );
					return f_top + f_base * (Scalar(1) - topAlbedo);
				}
				return f_top;
			}

			RISEPel albedo( const RayIntersectionGeometric& ri ) const override
			{
				// Combined directional-hemispherical reflectance for the
				// OIDN albedo AOV.  Use the same additive form so the
				// AOV reflects what's actually rendered.  IBSDF::albedo
				// contracts the result to [0, 1] per channel so OIDN
				// can run with cleanAux=true; the per-channel clamp
				// below is a defensive cap in case any downstream BSDF
				// returns an over-conservative `albedo()` (the formula
				// is energy-bounded when each layer's `albedo()`
				// reports its actual directional reflectance, but
				// IBSDF::albedo's default returns RISEPel(1,1,1) which
				// would compound the way it did before SheenBRDF::albedo
				// switched to GetLayerAlbedo).
				if( topBRDF.UsesAdditiveLayering() ) {
					const RISEPel topA  = topBRDF.albedo( ri );
					const RISEPel topL  = topBRDF.GetLayerAlbedo( ri );
					const RISEPel baseA = bottomBRDF.albedo( ri );
					RISEPel a = topA + baseA * (RISEPel( 1, 1, 1 ) - topL);
					for( int c = 0; c < 3; ++c ) {
						if( a[c] < 0 ) a[c] = 0;
						if( a[c] > 1 ) a[c] = 1;
					}
					return a;
				}
				return topBRDF.albedo( ri );
			}

			// Layer-albedo on the composite is the top layer's directly
			// — a CompositeBRDF wrapped in another composite would
			// surface the OUTERMOST sheen lobe to the next-up composite,
			// matching the conceptual "stack of additive layers" model.
			bool UsesAdditiveLayering() const override { return topBRDF.UsesAdditiveLayering(); }

			RISEPel GetLayerAlbedo( const RayIntersectionGeometric& ri ) const override
			{
				return topBRDF.GetLayerAlbedo( ri );
			}

			Scalar GetLayerAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm ) const override
			{
				return topBRDF.GetLayerAlbedoNM( ri, nm );
			}
		};

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
				const Scalar thickness,								// thickness between the materials
				const IPainter& extinction							// extinction coefficient for absorption between layers
				) :
			pBRDF( 0 ),
			pSPF( 0 ),
			pEmitter( 0 )
			{
				// BRDF: wrap both layers in a CompositeBRDF that does
				// the Khronos additive composition when the top opts
				// in via GetLayerAlbedo.  When only one side has a
				// BRDF (or neither), fall back to the legacy single-
				// BRDF behaviour.
				if( top.GetBSDF() && bottom.GetBSDF() ) {
					pBRDF = new CompositeBRDF( *top.GetBSDF(), *bottom.GetBSDF() );
				} else if( top.GetBSDF() ) {
					pBRDF = top.GetBSDF();
					pBRDF->addref();
				} else if( bottom.GetBSDF() ) {
					pBRDF = bottom.GetBSDF();
					pBRDF->addref();
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
		};
	}
}

#endif
