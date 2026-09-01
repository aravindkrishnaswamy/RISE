//////////////////////////////////////////////////////////////////////
//
//  CoatedBRDF.h - The CLOSED-FORM combined layer response for
//    `coated_material` (docs/WETNESS_COAT_DESIGN.md 7.1, Phase 2
//    item 1).
//
//  This class is the point of Phase 2.  `composite_material` forwards
//  one sub-material's BSDF (3.2 defect 2) and `polished_material`
//  returns a bare LambertianBRDF (3.3), so before this triad EVERY
//  route to a coated surface in RISE mis-evaluated direct lighting --
//  NEE, BDPT vertex connections and MIS denominators all saw an
//  UNCOATED substrate.  `value` / `valueNM` here answer with the
//  actual layered response.
//
//  THE MODEL (7.3 mixture x 7.4 layer):
//
//    f  =  c * f_coat
//       +  ( c * K(cos_i, cos_o) + (1 - c) ) * f_base
//
//    K  =  T(cos_i) T(cos_o) A_in A_out / ( eta^2 (1 - r_i R A_rt) )
//
//  `c` is coat_weight, the sub-pixel COVERAGE fraction (7.3).  The
//  two branches of 7.5's three-term mixture reach the substrate
//  DIFFERENTLY -- the (1 - c) branch through air with no Fresnel
//  transmission and no recycling, the `c` branch through the coat with
//  both -- and collapsing them would turn `c` into a gloss knob rather
//  than a coverage fraction.  The algebra above keeps them distinct
//  while factoring the shared `f_base`.
//
//  `f_coat` is a GGX lobe (7.2 types coat_roughness as "GGX alpha on
//  the coat lobe") with dielectric Fresnel at the microfacet normal
//  plus a Kulla-Conty multiple-scattering tail via
//  `MicrofacetEnergyLUT` -- which is what 7.4 says that LUT is for
//  here: "the coat lobe's OWN multiple-scatter once coat_roughness is
//  non-trivial", NOT the coat<->substrate recycling (that is
//  CoatedLayer's closed-form `1/(1 - r_i R)`).
//
//  SPECTRAL WET DARKENING IS TRANSPORT, NOT AN EXPONENT (7.1's second
//  requirement).  `valueNM` evaluates the substrate at the hero
//  wavelength through `hemisphericalAlbedoNM` / `valueNM` and lets the
//  PER-CHANNEL
//  recycling `1/(1 - r_i R(lambda))` act.  Since the amplification is
//  largest where R(lambda) is largest, the Saunderson form of 2.1
//  falls out -- darkening AND chroma boost together, from transport.
//  There is deliberately no `substrate_wet_exponent` in the shipping
//  parameter set (Phase 2 item 4): an additional R^k would
//  double-count this very mechanism.
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
				//! Whether the coat is tinted AT ALL.  Decided ONCE
				//! from the authored (un-uplifted) RGB triple and used
				//! by both pipes, because a white painter's
				//! `GetColorNM` is NOT 1.0 across the band -- the
				//! Jakob-Hanika uplift collapses pure white off the red
				//! end (~1.3e-5 at 660 nm, measured).  Testing
				//! `tint < 1` per wavelength, as an earlier revision
				//! did, therefore made an UNTINTED coat opaque in the
				//! red on every spectral render while RGB stayed clean.
				//! Full measurement in CoatedLayer::PassTransmittance.
				bool	tinted;
				Scalar	ri;				///< internal diffuse Fresnel reflectance for `eta`
				Scalar	re;				///< external diffuse Fresnel average for `eta`
			};

			CoatedBRDF(
				const IBSDF& base,						///< [in] Substrate BSDF (allowlisted -- see CoatedMaterial)
				const IScalarPainter& coatWeight,
				const IScalarPainter& coatIOR,
				const IScalarPainter& coatRoughness,
				const IScalarPainter& coatThickness,
				const IScalarPainter& coatAbsorption,
				const IPainter& coatTint,
				const bool recyclingCompensation = true	///< [in] see kRecycling note below
				);

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

			//! The substrate's VIEW-INDEPENDENT reflectance -- the `R`
			//! that drives the recycling denominator.  Reads
			//! `IBSDF::hemisphericalAlbedo{,NM}`, never `albedo`:
			//! feeding a view-dependent reflectance into a term shared
			//! by both directions would make this BRDF non-reciprocal.
			//! See the long note at the definitions in CoatedBRDF.cpp.
			RISEPel SubstrateAlbedo( const RayIntersectionGeometric& ri ) const;
			Scalar  SubstrateAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! Read-back for the interactive editor / snapshot clone.
			inline const IScalarPainter& GetCoatWeight()     const { return *pCoatWeight; }
			inline const IScalarPainter& GetCoatIOR()        const { return *pCoatIOR; }
			inline const IScalarPainter& GetCoatRoughness()  const { return *pCoatRoughness; }
			inline const IScalarPainter& GetCoatThickness()  const { return *pCoatThickness; }
			inline const IScalarPainter& GetCoatAbsorption() const { return *pCoatAbsorption; }
			inline const IPainter&       GetCoatTint()       const { return *pCoatTint; }

			//! Rebind for the interactive editor's MaterialIntrospection.
			//! Unlike every other triad in Materials/, these do NOT need a
			//! matching forwarder into the SPF: CoatedSPF holds a
			//! reference to THIS object and reads every coat parameter
			//! back through it (CoatedSPF.h), so evaluator and sampler
			//! cannot drift by construction -- there is only one copy of
			//! the state to rebind.
			//!
			//! addref-before-release throughout, so a self-rebind
			//! (Set(X) where X is already bound) cannot destroy the
			//! painter mid-swap -- LambertianBRDF::SetReflectance's
			//! documented contract, and the same caller-side
			//! cancel-and-park gate against the render thread applies.
			void SetCoatWeight( const IScalarPainter& v );
			void SetCoatIOR( const IScalarPainter& v );
			void SetCoatRoughness( const IScalarPainter& v );
			void SetCoatThickness( const IScalarPainter& v );
			void SetCoatAbsorption( const IScalarPainter& v );
			void SetCoatTint( const IPainter& v );

			//! FALSE reduces the layer to plain Weidlich-Wilkie single
			//! bounce -- i.e. it DELETES 7.4's required interreflection
			//! compensation.  Not reachable from the scene language and
			//! never false in production; it exists so
			//! tests/LayeredWhiteFurnaceTest.cpp can run the red-proof
			//! pair (config 15) and MEASURE the ~45 % high-albedo loss
			//! 7.4 predicts, instead of asserting it from theory.
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
		};
	}
}

#endif
