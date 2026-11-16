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
#include "../Interfaces/IScalarPainter.h"
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
			//! Concrete pointer so the editor's `SetRadEx` / `SetN`
			//! reach `PhongEmitter`'s setters (the base IEmitter
			//! interface doesn't expose them).
			PhongEmitter*				pEmitter;

			virtual ~PhongLuminaireMaterial()
			{
				pMaterial.release();
				safe_release( pEmitter );
			}

		public:
			PhongLuminaireMaterial( const IPainter& radEx, const Scalar scale, const IScalarPainter& N, const IMaterial& pMaterial_ ) :
			pMaterial( pMaterial_ )
			{
				pMaterial.addref();

				pEmitter = new PhongEmitter( radEx, scale, N );
				GlobalLog()->PrintNew( pEmitter, __FILE__, __LINE__, "pEmitter" );
			}

			const IMaterial& GetBaseMaterial() const { return pMaterial; }

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pMaterial.GetBSDF(); };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pMaterial.GetSPF(); };

			//! DL-05: GetSPF() forwards the wrapped material's SPF verbatim,
			//! so a delta pass-through it emits (a thin weave's gap) is
			//! emitted here too -- forward the capability with it
			//! (IMaterial::HasDeltaPassThrough).
			inline bool HasDeltaPassThrough() const { return pMaterial.HasDeltaPassThrough(); }
			//! DL-471: GetBSDF()/GetSPF() forward the base material verbatim.
			inline unsigned int ConnectionScatterTypes() const { return pMaterial.ConnectionScatterTypes(); }
			inline bool HasConnectionTypeSplit() const { return pMaterial.HasConnectionTypeSplit(); }
			//! Forward scattering/interior/medium/PDF traits of the wrapped base material
			//! so refractive, volumetric, SSS, graded-index, and full-sphere luminaires
			//! participate identically in IORStackSeeding, SMS, NEE, and MIS.
			inline bool CouldLightPassThrough() const { return pMaterial.CouldLightPassThrough(); }
			inline bool IsVolumetric() const { return pMaterial.IsVolumetric(); }
			inline ISubSurfaceDiffusionProfile* GetDiffusionProfile() const { return pMaterial.GetDiffusionProfile(); }
			inline const RandomWalkSSSParams* GetRandomWalkSSSParams() const { return pMaterial.GetRandomWalkSSSParams(); }
			inline bool GetRandomWalkSSSParamsNM( const Scalar nm, RandomWalkSSSParams& params_out ) const { return pMaterial.GetRandomWalkSSSParamsNM( nm, params_out ); }
			inline SpecularInfo GetSpecularInfo( const RayIntersectionGeometric& ri, const IORStack& ior_stack ) const { return pMaterial.GetSpecularInfo( ri, ior_stack ); }
			inline SpecularInfo GetSpecularInfoNM( const RayIntersectionGeometric& ri, const IORStack& ior_stack, const Scalar nm ) const { return pMaterial.GetSpecularInfoNM( ri, ior_stack, nm ); }
			inline const IScalarPainter* GetGradedIORField() const { return pMaterial.GetGradedIORField(); }
			inline Scalar Pdf( const Vector3& wo, const RayIntersectionGeometric& ri, const IORStack& ior_stack ) const { return pMaterial.Pdf( wo, ri, ior_stack ); }
			inline Scalar PdfNM( const Vector3& wo, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack& ior_stack ) const { return pMaterial.PdfNM( wo, ri, nm, ior_stack ); }
			inline bool ScattersFullSphere() const { return pMaterial.ScattersFullSphere(); }


			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return pEmitter; };

			//! Read-back + rebind for the interactive editor.  Material
			//! forwards to the PhongEmitter — the wrapped base material
			//! is intentionally NOT editable through this surface (users
			//! edit it via its own row in the panel).
			inline const IPainter&       GetRadEx() const { return pEmitter->GetRadEx(); }
			inline const IScalarPainter& GetN()     const { return pEmitter->GetN(); }
			inline void SetRadEx( const IPainter& v )     { pEmitter->SetRadEx( v ); }
			inline void SetN( const IScalarPainter& v )   { pEmitter->SetN( v ); }

			//! Rescales the emission (backs `> modify material <name>
			//! scale`).  PhongEmitter holds its scale as a const member
			//! with no setter, so we REBUILD it at the new scale, reusing
			//! the existing radiance-exitance painter AND the Phong-N
			//! exponent painter.  Hold references on both across the swap
			//! so they survive the old emitter's release.  Mirrors
			//! LambertianLuminaireMaterial::SetEmissionScale; runs before
			//! a render — no threading concern.
			/// \return TRUE always (this material is a luminaire)
			bool SetEmissionScale( const Scalar scale )
			{
				const IPainter&       radEx = pEmitter->GetRadEx();
				const IScalarPainter& phongN = pEmitter->GetN();
				radEx.addref();
				phongN.addref();

				safe_release( pEmitter );

				pEmitter = new PhongEmitter( radEx, scale, phongN );
				GlobalLog()->PrintNew( pEmitter, __FILE__, __LINE__, "pEmitter" );

				// PhongEmitter's ctor addrefs both painters; drop the
				// temporary references that bridged the rebuild.
				radEx.release();
				phongN.release();

				return true;
			}
		};
	}
}

#endif

