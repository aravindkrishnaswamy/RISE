//////////////////////////////////////////////////////////////////////
//
//  FabricBRDF.h - Closed-form combined response for `fabric_material`:
//  energy-compensated Charlie sheen over a restricted substrate
//  (docs/CLOTH_FABRIC_DESIGN.md 9.2).
//
//  Model (Kulla-Conty adding-doubling coupling, reciprocal & energy-exact
//  for a white Lambertian base where N == 1):
//    f(l,v)     = sheenColor * D_Charlie(a, n.h) * V_Charlie(a, n.l, n.v) / N
//                 + f_base(l,v) * scale(l,v)
//    scale(l,v) = (1 - m*Ehat(a, |n.v|)) * (1 - m*Ehat(a, |n.l|))
//                 / (1 - m*EhatMean(a))
//  with m = max3(sheenColor), Ehat = min(E, 1), and symmetric normaliser
//  N = max(1, E(a, n.v), E(a, n.l)) (SheenDirectionalAlbedo.h). The
//  1/(1 - m*EhatMean) denominator intentionally brightens the base away
//  from grazing to return energy intercepted at grazing.
//
//  Load-bearing invariants:
//  - Roughness floor kMinSheenAlpha = 0.04 (smallest alpha with
//    max_{mu >= 0.03} E <= 1; below mu = 0.03, N bounds the Estevez-Kulla
//    grazing overshoot so global max rho <= 1.0075 on the 64x64 warped LUT).
//    `sheen_material`'s 1e-3 floor is deliberately left unchanged.
//  - `weave_rotation` rotates the SUBSTRATE's ONB via `WeaveRotatedRI`
//    identically across value/valueNM/Pdf/PdfNM/Scatter/ScatterNM (not
//    hemisphericalAlbedo). The Charlie lobe itself is isotropic.
//  - `alpha`, `m`, and `weaveAngle` are read achromatically in both RGB
//    and NM paths so hero/companion PDFs and tangent frames cannot drift.
//  - Transmission (`BaseScattersFullSphere()`): opposite-hemisphere pairs
//    forward `f_base(l,v) * scale(l,v)` with |n.l| and sheen = 0 (two-sided
//    fuzz layer); non-transmitting substrates early-out unchanged.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FABRIC_BRDF_
#define FABRIC_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include "../Utilities/MicrofacetUtils.h"
#include <cmath>
#include <optional>

namespace RISE
{
	namespace Implementation
	{
		//! Lazily copies `ri` with its ONB rotated about `w` by `weave_rotation`
		//! (when `|angle| >= 1e-9`, matching MicrofacetUtils::RotateTangent).
		//! Copying is required so the shared hit record stays immutable.
		class WeaveRotatedRI
		{
		public:
			WeaveRotatedRI( const RayIntersectionGeometric& ri, const Scalar angle ) :
			  pRef( &ri )
			{
				if( std::fabs( angle ) >= Scalar( 1e-9 ) ) {
					storage.emplace( ri );
					storage->onb = MicrofacetUtils::RotateTangent( ri.onb, angle );
					pRef = &( *storage );
				}
			}

			//! Non-copyable: `pRef` may alias `storage`.
			WeaveRotatedRI( const WeaveRotatedRI& ) = delete;
			WeaveRotatedRI& operator=( const WeaveRotatedRI& ) = delete;

			inline const RayIntersectionGeometric& Get() const { return *pRef; }

		private:
			std::optional<RayIntersectionGeometric>	storage;
			const RayIntersectionGeometric*			pRef;
		};

		class FabricBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		public:
			//! Sheen roughness clamp.  See the header banner for the
			//! measurement that pins the lower bound.
			static const Scalar kMinSheenAlpha;		///< 0.04
			static const Scalar kMaxSheenAlpha;		///< 1.0

			//! Fabric parameters resolved at ONE shading point in ONE
			//! colour regime.  Shared by FabricBRDF and FabricSPF so the
			//! sampler's alpha / selection weight can never drift from
			//! the evaluator's.
			struct FabricParams
			{
				Scalar	alpha;			///< sheen roughness, clamped to [kMinSheenAlpha, kMaxSheenAlpha]
				Scalar	m;				///< max3(sheenColor), clamped to [0,1] -- the ACHROMATIC energy split
				RISEPel	tint;			///< sheen colour, RGB regime (undefined on the NM path)
				Scalar	tintNM;			///< sheen colour at the hero wavelength (undefined on the RGB path)
				Scalar	weaveAngle;		///< radians, for the substrate's frame
			};

			FabricBRDF(
				const IBSDF& base,					///< [in] Substrate BSDF (allowlisted -- see FabricMaterial)
				const IPainter& sheenColor,
				const IScalarPainter& sheenRoughness,
				const IScalarPainter& weaveRotation,
				//! [in] The substrate material's `ScattersFullSphere()`.
				//! Passed in rather than derived because `IBSDF` carries
				//! no such flag -- it is an `IMaterial` property, and
				//! `FabricMaterial` is the only thing that holds both.
				//! Captured once: the only substrate that can set it is a
				//! `weave_material`, whose `transmission` enum is not
				//! rebindable, and `base` is not rebindable here either.
				const bool baseScattersFullSphere,
				const bool baseSingleDiffuseLabel = false
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
            // With a diffuse-only substrate both mixture branches carry the
            // same label. Its density share is one, also under reversal.
            bool SingleDiffuseLabel() const { return bSingleDiffuseLabel; }
            bool valueByScatterType(const Vector3& wo,const RayIntersectionGeometric& ri,
                const IORStack*,RISEPel out[5]) const {
                if(!bSingleDiffuseLabel) return false;
                for(int t=0;t<5;++t) out[t]=RISEPel(0,0,0);
                out[ScatteredRay::eRayDiffuse]=value(wo,ri); return true;
            }
            bool valueByScatterTypeNM(const Vector3& wo,const RayIntersectionGeometric& ri,
                Scalar nm,const IORStack*,Scalar out[5]) const {
                if(!bSingleDiffuseLabel) return false;
                for(int t=0;t<5;++t) out[t]=0;
                out[ScatteredRay::eRayDiffuse]=valueNM(wo,ri,nm); return true;
            }

			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
			virtual bool    hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const;
			virtual bool    hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const;

			//! Resolves fabric parameters at `ri` (`nm < 0` RGB, `nm >= 0` NM).
			//! `alpha`, `m` (RGB max3), and `weaveAngle` are always read
			//! achromatically so hero/companion PDFs and substrate tangent frames
			//! stay identical across wavelengths; only tint varies spectrally.
			void ResolveFabric( const RayIntersectionGeometric& ri, const Scalar nm, FabricParams& out ) const;

			//! Single-direction fuzz transmittance `1 - m*Ehat(alpha, cosTheta)`
			//! (`Ehat = min(E, 1)`); also the closed-form base factor in `albedo`.
			static Scalar SheenTransmit( const Scalar alpha, const Scalar m, const Scalar cosTheta );

			//! Kulla-Conty base scaling:
			//!   (1 - m*Ehat(a, cosV)) * (1 - m*Ehat(a, cosL)) / (1 - m*EhatMean(a)).
			//! Symmetric in cosV/cosL (reciprocity); shared by `value` and `valueNM`.
			static Scalar BaseScaling( const Scalar alpha, const Scalar m,
			                           const Scalar cosThetaV, const Scalar cosThetaL );

			//! Symmetric normaliser `max(1, E(v), E(l))` on raw E; divides the
			//! sheen lobe near grazing where E > 1 while preserving reciprocity
			//! (identically 1 elsewhere).
			static Scalar SheenNormaliser( const Scalar alpha,
			                               const Scalar cosThetaV, const Scalar cosThetaL );

			//! The hemispherical mean of `SheenTransmit`, i.e.
			//! `1 - m*EhatMean(alpha)` -- the base factor in
			//! `hemisphericalAlbedo`'s closed form.  Retired the baked
			//! 2-D S(alpha, m) kernel table that round 4's `min` form
			//! needed (a `min` does not factor; a product does).
			static Scalar SheenTransmitMean( const Scalar alpha, const Scalar m );

			//! The sheen lobe's SELECTION weight / directional-albedo
			//! share, `m * Ehat(alpha, n.v)`, clamped to [0,1].
			//! FabricSPF uses it for both the branch pick and the mixture
			//! density, which is what makes the two agree by
			//! construction.  Uses `Ehat`, not raw E, so the weight
			//! cannot exceed 1 inside the grazing sliver.
			static Scalar SheenSelectWeight( const Scalar alpha, const Scalar m, const Scalar cosThetaV );

			//! The two lobe terms at one (l, v) pair, with the sheen's
			//! normaliser already folded in.  Colour-free: everything
			//! here depends on geometry and roughness only, so ONE body
			//! serves both the RGB and the spectral pipe and the two
			//! cannot drift (docs/skills/audit-by-bug-pattern.md's
			//! RGB/NM-twin discipline).
			struct FabricTerms
			{
				bool	valid;		///< false => the direction pair is gated off; both terms are 0
				//! Zero on transmission pairs (Charlie is reflection-only); consumers
				//! evaluate `tint * sheen + f_base * scaling` uniformly on both sides.
				Scalar	sheen;		///< D * V / max(1, E(v), E(l))
				Scalar	scaling;	///< the base scaling factor
			};

			//! Resolve the geometry + both lobe factors at `vLightIn`.
			//! Shared by `value`, `valueNM` and their `*WithParams`
			//! forms.
			FabricTerms ComputeTerms( const Vector3& vLightIn,
			                          const RayIntersectionGeometric& ri,
			                          const FabricParams& p ) const;

			//! `value` / `valueNM` with pre-resolved params and weave-rotated RI
			//! (avoids redundant painter lookups and RI copies inside FabricSPF).
			RISEPel ValueWithParams( const Vector3& vLightIn,
			                         const RayIntersectionGeometric& ri,
			                         const FabricParams& p,
			                         const RayIntersectionGeometric& weaveRi ) const;
			Scalar  ValueNMWithParams( const Vector3& vLightIn,
			                           const RayIntersectionGeometric& ri,
			                           const Scalar nm,
			                           const FabricParams& p,
			                           const RayIntersectionGeometric& weaveRi ) const;

			//! The substrate as seen by the layer.  FabricSPF needs it to
			//! evaluate the full mixture after sampling either branch.
			inline const IBSDF& GetBase() const { return *pBase; }

			//! Does the SUBSTRATE scatter over the full sphere (R8 P1.1)?
			//! `FabricSPF` reads it back through here so the sampler and
			//! the evaluator are gated on literally the same bit -- one
			//! copy of the state, `FabricSPF`'s standing contract.
			inline bool BaseScattersFullSphere() const { return bBaseFullSphere; }

			//! Read-back for the interactive editor / snapshot clone.
			inline const IPainter&       GetSheenColor()     const { return *pSheenColor; }
			inline const IScalarPainter& GetSheenRoughness() const { return *pSheenRoughness; }
			inline const IScalarPainter& GetWeaveRotation()  const { return *pWeaveRotation; }

			//! Interactive editor rebinds (addref-before-release; FabricSPF reads
			//! shared state directly from this object, so no SPF forwarder needed).
			void SetSheenColor( const IPainter& v );
			void SetSheenRoughness( const IScalarPainter& v );
			void SetWeaveRotation( const IScalarPainter& v );

		protected:
			virtual ~FabricBRDF();

			const IBSDF*			pBase;
			const IPainter*			pSheenColor;
			const IScalarPainter*	pSheenRoughness;
			const IScalarPainter*	pWeaveRotation;
			bool bSingleDiffuseLabel;
			bool					bBaseFullSphere;	///< see the ctor parameter and the header's transmission section
		};
	}
}

#endif
