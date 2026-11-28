//////////////////////////////////////////////////////////////////////
//
//  CoatedBRDF.h - Closed-form layered response for `coated_material`
//  (docs/WETNESS_COAT_DESIGN.md §7.1-7.5).
//
//  Model: coverage fraction `c = coat_weight` blends an air-exposed base
//  and a dielectric-coated base:
//    f = c * f_coat + (c * K(cos_i, cos_o) + (1 - c)) * f_base
//    K = T(cos_i) T(cos_o) A_in A_out / (eta^2 (1 - r_i R A_rt))
//  where `f_coat` is GGX + dielectric Fresnel + Kulla-Conty MS via
//  `MicrofacetEnergyLUT` (coat's own MS), while `1/(1 - r_i R A_rt)`
//  handles coat<->substrate recycling (producing spectral wet darkening
//  and chroma boost from transport; no wet-exponent parameter).
//
//  Load-bearing invariants:
//  - Substrate frame dispatch (`SubstrateModel`): Lambertian uses outer
//    directions (exact); Oren-Nayar and GGX evaluate inside the coat's
//    refracted frame with recycled field M (cosine or lobe reservoir);
//    fabric/weave use the outer-frame approximation.
//  - Transmission (`bBaseFullSphere`, captured once from the substrate's
//    IMaterial::ScattersFullSphere()): opposite-hemisphere pairs forward
//    `f_base * (K_t*c + (1-c))` using |n.l| and the same `rec` factor,
//    with no `f_coat` term. Delta gap rays in CoatedSPF omit `rec`.
//    Non-transmitting bases early-out unchanged. `hemisphericalAlbedo`
//    and `albedo` remain reflectance-only.
//  - `SubstrateAlbedo` reads view-independent `hemisphericalAlbedo`
//    (never `albedo`) to preserve reciprocity.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COATED_BRDF_
#define COATED_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class GGXBRDF;

		class CoatedBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		public:
			//! Coat parameters resolved at one shading point, in one
			//! colour regime.  `tint` carries the RGB triple on the
			//! RGB path and (tintNM, tintNM, tintNM) on the spectral
			//! path, so the shared code below reads one type.
			struct CoatParams
			{
				Scalar	weight;			///< coat_weight, clamped to [0,1]
				Scalar	eta;			///< coat_ior / ri.ambientIOR, clamped >= 1
				Scalar	alpha;			///< GGX alpha, clamped >= CoatedLayer::kMinCoatAlpha
				Scalar	thickness;		///< world length
				Scalar	absorption;		///< 1/length
				RISEPel	tint;			///< per-traversal transmission colour
				//! Whether the coat is tinted at all, decided once from authored RGB
				//! so both pipes agree (spectral uplift of white is slightly < 1.0;
				//! see IPainter::IsUntintedWhite / CoatedLayer::PassTransmittance).
				bool	tinted;
				Scalar	ri;				///< internal diffuse Fresnel reflectance for `eta`
				Scalar	re;				///< external diffuse Fresnel average for `eta`
				Scalar	ambient;		///< DL-388: absolute index of the outside medium (eta * ambient is the coat's)
			};

			//! DL-388: how the substrate is evaluated under the coat.  Picked
			//! once, from the substrate BSDF's class (CoatedMaterial's
			//! allowlist is closed).  See CoatedBRDF.cpp's "SUBSTRATE IN THE
			//! COAT'S FRAME" block for the derivation.
			enum SubstrateModel
			{
				//! Lambertian: direction-independent, so the outer and the
				//! refracted frame agree and the pre-DL-388 expression is
				//! kept verbatim (exact; bit-identical).
				eSubstrateLambertian,
				//! Oren-Nayar: refracted frame; the recycled field priced as
				//! a cosine reservoir (the Lambertian recycling factor).
				eSubstrateRefractedCosine,
				//! GGX: refracted frame; the recycled field priced through
				//! the substrate's own first-bounce return g(mu) (a lobe
				//! reservoir; exact for a single separable lobe).
				eSubstrateRefractedLobe,
				//! fabric / weave: the pre-DL-388 outer-frame model,
				//! unchanged (DL-417).
				eSubstrateOuterFrame
			};

			inline SubstrateModel GetSubstrateModel() const { return substrateModel; }

			//! DL-388: the substrate's shading record INSIDE the coat: a copy
			//! of `ri` whose view direction is the refraction of the outer
			//! view, whose frame is the ray-facing `onb`, and whose ambient
			//! index is the coat's (a substrate reading `ambientIOR`, e.g. a
			//! GGX conductor, sees the medium it is actually buried in).
			RayIntersectionGeometric MakeSubstrateRecord(
				const RayIntersectionGeometric& ri,
				const OrthonormalBasis3D& onb,
				const CoatParams& cp
				) const;

			//! DL-388: fraction of CoatedSPF's substrate branch that samples
			//! the substrate in the refracted frame (the rest samples it at
			//! the outer directions, as before).  0 for every model but
			//! eSubstrateRefractedLobe, so Lambertian / Oren-Nayar / fabric /
			//! weave sampling is unchanged.
			Scalar RefractedSampleFraction() const;

			//! Share of CoatedSPF's substrate branch sampled from a cosine lobe
			//! about the macro normal to cover the broad recycled term M (nonzero
			//! only for eSubstrateRefractedLobe).
			Scalar RecycledSampleFraction() const;

			CoatedBRDF(
				const IBSDF& base,						///< [in] Substrate BSDF (allowlisted -- see CoatedMaterial)
				const IScalarPainter& coatWeight,
				const IScalarPainter& coatIOR,
				const IScalarPainter& coatRoughness,
				const IScalarPainter& coatThickness,
				const IScalarPainter& coatAbsorption,
				const IPainter& coatTint,
				const bool recyclingCompensation = true,	///< [in] see kRecycling note below
				//! [in] Substrate material's `ScattersFullSphere()` (IBSDF lacks this
				//! flag; captured once at construction from CoatedMaterial).
				const bool baseScattersFullSphere = false,
				//! [in] Optional tangent-space normal map applied only to the coat's
				//! GGX lobe (NULL = unchanged frame). Decoded per NormalMap.cpp and
				//! scaled by `coatNormalScale`. Addref'd; construction-time only.
				const IPainter* coatNormal = 0,
				const Scalar coatNormalScale = Scalar(1),
                const bool auditedDiffuseBase = false
				);

			bool SingleDiffuseLabel() const;
            bool valueByScatterType(const Vector3& wo,const RayIntersectionGeometric& ri,
                const IORStack*,RISEPel out[5]) const {
                if(!SingleDiffuseLabel()) return false;
                for(int t=0;t<5;++t) out[t]=RISEPel(0,0,0);
                out[ScatteredRay::eRayDiffuse]=value(wo,ri); return true;
            }
            bool valueByScatterTypeNM(const Vector3& wo,const RayIntersectionGeometric& ri,
                Scalar nm,const IORStack*,Scalar out[5]) const {
                if(!SingleDiffuseLabel()) return false;
                for(int t=0;t<5;++t) out[t]=0;
                out[ScatteredRay::eRayDiffuse]=valueNM(wo,ri,nm); return true;
            }
            virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
			virtual bool    hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const;
			virtual bool    hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const;

			//! Resolve the coat parameters at `ri`.  `nm < 0` selects
			//! the RGB regime (painters read via GetValuesAt /
			//! GetColor); `nm >= 0` selects the spectral one.  Shared
			//! by CoatedBRDF and CoatedSPF so the sampler's Fresnel /
			//! alpha can never drift from the evaluator's.
			void ResolveCoat( const RayIntersectionGeometric& ri, const Scalar nm, CoatParams& out ) const;

			//! The substrate as seen by the layer.  CoatedSPF needs
			//! both to build its mixture.
			inline const IBSDF& GetBase() const { return *pBase; }

			//! Resolves the coat-lobe shading frame from ray-facing `baseOnb`
			//! (returns `baseOnb` unchanged if `pCoatNormal` is null). When a
			//! separate shader tangent is present, decodes against `ri.normalMapOnb`
			//! UV axes and the ray-facing normal (matching NormalMap).

			OrthonormalBasis3D ResolveCoatFrame(
				const RayIntersectionGeometric& ri,
				const OrthonormalBasis3D& baseOnb
				) const;

			//! DL-23: does the SUBSTRATE scatter over the full sphere?
			//! `CoatedSPF` reads it back through here so the sampler and
			//! the evaluator are gated on literally the same bit -- one
			//! copy of the state, mirroring `FabricBRDF::
			//! BaseScattersFullSphere`'s identical contract.
			inline bool BaseScattersFullSphere() const { return bBaseFullSphere; }

			//! View-independent substrate reflectance R for recycling (reads
			//! `hemisphericalAlbedo{,NM}`, never `albedo`, to preserve reciprocity).
			RISEPel SubstrateAlbedo( const RayIntersectionGeometric& ri ) const;
			Scalar  SubstrateAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! Read-back for the interactive editor / snapshot clone.
			inline const IScalarPainter& GetCoatWeight()     const { return *pCoatWeight; }
			inline const IScalarPainter& GetCoatIOR()        const { return *pCoatIOR; }
			inline const IScalarPainter& GetCoatRoughness()  const { return *pCoatRoughness; }
			inline const IScalarPainter& GetCoatThickness()  const { return *pCoatThickness; }
			inline const IScalarPainter& GetCoatAbsorption() const { return *pCoatAbsorption; }
			inline const IPainter&       GetCoatTint()       const { return *pCoatTint; }
			//! DL-192.  NULL = no coat-normal perturbation bound (the
			//! pre-DL-192 / default state).
			inline const IPainter*       GetCoatNormal()      const { return pCoatNormal; }
			inline Scalar                GetCoatNormalScale() const { return coatNormalScale; }

			//! Interactive editor rebinds (addref-before-release; CoatedSPF reads
			//! shared state directly from this object, so no SPF forwarder needed).
			void SetCoatWeight( const IScalarPainter& v );
			void SetCoatIOR( const IScalarPainter& v );
			void SetCoatRoughness( const IScalarPainter& v );
			void SetCoatThickness( const IScalarPainter& v );
			void SetCoatAbsorption( const IScalarPainter& v );
			void SetCoatTint( const IPainter& v );

			//! False disables interreflection recycling (Weidlich-Wilkie single
			//! bounce); always true in production, exposed for furnace unit tests.
			inline bool GetRecyclingCompensation() const { return bRecycling; }

		protected:
			virtual ~CoatedBRDF();

			const IBSDF*			pBase;
			const IScalarPainter*	pCoatWeight;
			const IScalarPainter*	pCoatIOR;
			const IScalarPainter*	pCoatRoughness;
			const IScalarPainter*	pCoatThickness;
			const IScalarPainter*	pCoatAbsorption;
			const IPainter*			pCoatTint;
			const bool				bRecycling;
			bool bAuditedDiffuseBase;
			bool					bBaseFullSphere;	///< DL-23: see the ctor parameter and CoatedBRDF.cpp's transmission section
			const IPainter*			pCoatNormal;		///< DL-192: optional coat-lobe-only normal map; NULL = none
			Scalar					coatNormalScale;	///< DL-192: xy scale on the decoded tangent-space normal
			SubstrateModel			substrateModel;		///< DL-388: see SubstrateModel
			const GGXBRDF*			pGGXBase;			///< DL-388: pBase as a GGXBRDF when substrateModel is the lobe model (not owned; same object)

			//! DL-388: value()/valueNM() for the two refracted-frame models
			//! (reflection half-space; the gates are already passed).
			RISEPel ValueRefracted( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const OrthonormalBasis3D& onb,
				const Vector3& v, const Vector3& r ) const;
			Scalar  ValueRefractedNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const OrthonormalBasis3D& onb,
				const Vector3& v, const Vector3& r, const Scalar nm ) const;
		};
	}
}

#endif
