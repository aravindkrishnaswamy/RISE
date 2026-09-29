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
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "PolishedBRDF.h"
#include "PolishedSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class PolishedMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			//! DL-285: the BSDF is the SPF's OWN reflectance function (one
			//! object, shared), not a separate model -- NEE, BDPT/VCM
			//! connections and the DL-67 guide draw price `value`, the
			//! sampled continuation prices `kray`, and those must be one
			//! function (DL-67 premise 2).  Before DL-285 this was a bare
			//! `LambertianBRDF(Rd)`.
			PolishedBRDF*	pBRDF;
			PolishedSPF*	pSPF;

			virtual ~PolishedMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			PolishedMaterial(
				const IPainter& Rd_,
				const IScalarPainter& tau_,
				const IScalarPainter& Nt_,
				const IScalarPainter& s,
				const bool hg
				)
			{
				pSPF = new PolishedSPF( Rd_, tau_, Nt_, s, hg );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

				pBRDF = &pSPF->GetBRDF();
				pBRDF->addref();
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {		return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const
			{
				return pSPF->GetSpecularInfo( ri, ior_stack );
			}

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack,
				const Scalar nm
				) const
			{
				return pSPF->GetSpecularInfoNM( ri, ior_stack, nm );
			}

			//! Read-back + rebind for the interactive editor's
			//! MaterialIntrospection.  diffuse_reflectance is an
			//! IPainter; transmittance / ior / scattering are
			//! IScalarPainter (physical scalar pipe).  Every setter
			//! rebinds the ONE painter set the shared PolishedBRDF owns,
			//! so BSDF and SPF cannot fall out of lockstep.
			inline const IPainter&       GetDiffuseReflectance() const { return pSPF->GetDiffuseReflectance(); }
			inline const IScalarPainter& GetTransmittance()      const { return pSPF->GetTransmittance(); }
			inline const IScalarPainter& GetIOR()                const { return pSPF->GetIOR(); }
			inline const IScalarPainter& GetScattering()         const { return pSPF->GetScattering(); }
			//! Baked HG-phase flag read-back for the snapshot clone
			//! (fixed at construction; no setter).
			inline bool                  GetHG()                 const { return pSPF->GetHG(); }
			inline void SetDiffuseReflectance( const IPainter& v ) { pSPF->SetDiffuseReflectance( v ); }
			inline void SetTransmittance( const IScalarPainter& v ) { pSPF->SetTransmittance( v ); }
			inline void SetIOR( const IScalarPainter& v )           { pSPF->SetIOR( v ); }
			inline void SetScattering( const IScalarPainter& v )    { pSPF->SetScattering( v ); }
		};
	}
}

#endif
