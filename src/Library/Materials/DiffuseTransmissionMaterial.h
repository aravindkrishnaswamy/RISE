//////////////////////////////////////////////////////////////////////
//
//  DiffuseTransmissionMaterial.h - Thin two-sided diffuse reflection +
//    diffuse transmission material (Blender/Cycles Translucent BSDF
//    plus optional front/back diffuse reflection).
//
//  Contract (docs/BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md §5.6 &
//  docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md P1-13):
//    * Thin sheet: NO interior IOR tracking (`hasInterior = false`,
//      `canRefract = false`).  Transmitted rays keep `ior_stack` unchanged.
//    * Both-directions hemisphere test: decides reflection vs
//      transmission from `Dot(n, -ray.Dir()) * Dot(n, vLightIn)` without
//      assuming the normal faces the incoming ray, so BDPT/VCM re-aimed
//      vertices evaluate reciprocally from either side.
//    * Strict energy conservation in BOTH RGB (Pel) and spectral
//      (NM/HWSS): at every RGB channel and at every individual wavelength
//      `nm`, if `R + T > 1`, both are scaled by `1 / (R + T)` so
//      `∫ f(wi, wo) |cos θo| dωo <= 1` even when spectral uplift of
//      saturated RGB colours with `R_rgb + T_rgb <= 1` would otherwise
//      exceed 1 at narrow wavelengths (Round 3 P1-13).
//    * Per-scatter-type evaluation (`ConnectionScatterTypes`,
//      `HasConnectionTypeSplit`, `valueByScatterType`,
//      `valueByScatterTypeNM`) separating `eRayDiffuse` (reflection) and
//      `eRayTranslucent` (transmission), plus `ScattersFullSphere() == true`
//      for full-sphere NEE and `EvaluateLobeFNM` / `EvaluateKrayNM` for
//      HWSS companion wavelengths.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIFFUSE_TRANSMISSION_MATERIAL_
#define DIFFUSE_TRANSMISSION_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class DiffuseTransmissionBSDF :
			public virtual IBSDF,
			public virtual Reference
		{
		protected:
			const IPainter* pReflectance;
			const IPainter* pTransmittance;

			virtual ~DiffuseTransmissionBSDF();

		public:
			DiffuseTransmissionBSDF( const IPainter& reflectance, const IPainter& transmittance );

			RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const override;
			Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const override;

			bool valueByScatterType(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri,
				const IORStack* pIORStack,
				RISEPel out[5]
				) const override;

			bool valueByScatterTypeNM(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri,
				const Scalar nm,
				const IORStack* pIORStack,
				Scalar out[5]
				) const override;

			RISEPel albedo( const RayIntersectionGeometric& ri ) const override;
			bool hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const override;
			bool hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const override;

			const IPainter& GetReflectance() const { return *pReflectance; }
			const IPainter& GetTransmittance() const { return *pTransmittance; }

			void SetReflectance( const IPainter& reflectance );
			void SetTransmittance( const IPainter& transmittance );
		};

		class DiffuseTransmissionSPF :
			public virtual ISPF,
			public virtual Reference
		{
		protected:
			const IPainter* pReflectance;
			const IPainter* pTransmittance;

			virtual ~DiffuseTransmissionSPF();

		public:
			DiffuseTransmissionSPF( const IPainter& reflectance, const IPainter& transmittance );

			void Scatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const override;

			void ScatterNM(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const override;

			Scalar Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const override;

			Scalar PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const override;

			Scalar EvaluateLobeFNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const override;

			using ISPF::EvaluateKrayNM;
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const override;

			void SetReflectance( const IPainter& reflectance );
			void SetTransmittance( const IPainter& transmittance );
		};

		class DiffuseTransmissionMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			DiffuseTransmissionBSDF* pBSDF;
			DiffuseTransmissionSPF*  pSPF;

			virtual ~DiffuseTransmissionMaterial();

		public:
			DiffuseTransmissionMaterial( const IPainter& reflectance, const IPainter& transmittance );

			inline IBSDF* GetBSDF() const override { return pBSDF; }
			inline ISPF*  GetSPF()  const override { return pSPF; }
			inline IEmitter* GetEmitter() const override { return 0; }

			inline bool CouldLightPassThrough() const override { return false; }

			inline unsigned int ConnectionScatterTypes() const override
			{
				return ( 1u << ScatteredRay::eRayDiffuse ) | ( 1u << ScatteredRay::eRayTranslucent );
			}

			inline bool HasConnectionTypeSplit() const override { return true; }

			inline bool ScattersFullSphere() const override { return true; }

			inline const IPainter& GetReflectance() const { return pBSDF->GetReflectance(); }
			inline const IPainter& GetTransmittance() const { return pBSDF->GetTransmittance(); }

			inline void SetReflectance( const IPainter& reflectance )
			{
				pBSDF->SetReflectance( reflectance );
				pSPF->SetReflectance( reflectance );
			}

			inline void SetTransmittance( const IPainter& transmittance )
			{
				pBSDF->SetTransmittance( transmittance );
				pSPF->SetTransmittance( transmittance );
			}

			inline unsigned int GetPathDependence() const override
			{
				return IMaterial::GetPathDependence() |
					pBSDF->GetReflectance().GetPathDependence() |
					pBSDF->GetTransmittance().GetPathDependence();
			}

			inline SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric&,
				const IORStack&
				) const override
			{
				SpecularInfo info;
				info.valid = true;
				info.isSpecular = false;
				info.canRefract = false;
				info.hasInterior = false;
				return info;
			}
		};
	}
}

#endif
