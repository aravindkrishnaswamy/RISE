//////////////////////////////////////////////////////////////////////
//
//  MixMaterial.h - Physically-based bounded N-ary material mixer.
//
//  Implements Part I of docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md and
//  all Review Round 3 (2026-11-16) P1 contracts (P1-1, P1-2, P1-3/§3,
//  P1-4, P1-6):
//    - Weights are response coefficients (never normalized when S < 1;
//      shortfall 1 - S absorbs as `none`; S > 1 clamps by dividing by S
//      and logs once; negative/non-finite weights clamp to 0 and log once).
//    - Optional pass-through weight `w_clear` acts as alpha coverage
//      `a = (1 - w_clear) * ownAlpha`, registered with `AnyAlphaMaterials`,
//      `HasAlphaCoverage`, and unified `RayIntersection::HitCoverage()`.
//    - Each child may carry an optional `IRayIntersectionModifier*` that
//      replaces the object's modifier on the hit record when that leaf is
//      selected (weights are always evaluated on the pre-modifier hit).
//    - Per-hit stochastic leaf draw from the active `ISampler*` (Round 3
//      §3), resolved ONCE after BVH traversal + `CompleteShadingSignals`
//      so weights calling `osl_ao()` / `proximity()` / `mesh_attr3()`
//      evaluate safely with full scene signals.
//    - Stamps `ri.mixConnectible` (`q = P(connectible | mix accepted)`)
//      for VCM photon-merge thinning alongside `v.isConnectible = false`
//      on absorber leaves (Round 3 P1-1).
//    - Keeps `ri.pUnresolvedMix` for noise-free expected-value denoiser
//      albedo & normal guides (Round 3 P1-6).
//    - Refuses nested mixes and containment-tracking / refracting children
//      at construction (`MIX §I.3`).
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MIX_MATERIAL_
#define MIX_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Utilities/Reference.h"
#include <atomic>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		struct MixMaterialChild
		{
			const IMaterial*                pMaterial;  ///< Nullable (`none` black absorber)
			const IScalarPainter*           pWeight;    ///< Required non-null scalar weight
			const IRayIntersectionModifier* pModifier;  ///< Optional per-leaf modifier (replaces object modifier)
		};

		class MixMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			static const unsigned int     kStackWeightCap = 16;

			std::vector<MixMaterialChild> children;
			const IScalarPainter*         pPassthroughWeight;

			mutable std::atomic<bool>     loggedInvalidWeight;
			mutable std::atomic<bool>     loggedSumOverOne;

			virtual ~MixMaterial();

		public:
			//! Evaluates sanitized bounded weights (all >= 0, sum + wClear <= 1)
			//! into caller-provided `outWeights` (length == children.size()) so
			//! hot-path queries and MaterialPreview avoid heap allocations for <= 16 children.
			void EvaluateWeights(
				const RayIntersectionGeometric& ri,
				Scalar* outWeights,
				Scalar& outWClear
				) const;
			//! Validates all children before constructing; caller should prefer
			//! `MixMaterial::ValidateChildren` or `RISE_API_CreateMixMaterial`.
			static bool ValidateChildren(
				const IMaterial* const* pChildren,
				const IScalarPainter* const* pWeights,
				const unsigned int numChildren
				);

			MixMaterial(
				const IMaterial* const* pChildren,
				const IScalarPainter* const* pWeights,
				const IRayIntersectionModifier* const* pModifiers,
				const unsigned int numChildren,
				const IScalarPainter* pPassthroughWeight_
				);

			// Unresolved mix exposes no BSDF/SPF/Emitter of its own; hits
			// resolve post-traversal to one concrete leaf (or nullptr absorber).
			IBSDF* GetBSDF() const override { return 0; }
			ISPF* GetSPF() const override { return 0; }
			IEmitter* GetEmitter() const override { return 0; }

			bool IsUnresolvedMix() const override { return true; }

			//! Conservative per-object capabilities (live OR over children so
			//! interactive rebinds like `WeaveMaterial::SetGap()` stay fresh).
			//! Pass-through is deliberately excluded from `CouldLightPassThrough`
			//! (MIX §I.3) so transparent mixes do not force VCM under Auto.
			bool CouldLightPassThrough() const override;
			bool HasDeltaPassThrough() const override;
			bool HasSubsurfaceLeaf() const override;
			bool HasRandomWalkSSSLeaf() const override;
			bool IsHyperspectralSkin() const override;
			bool CoversWavelengthRange( const Scalar nmBegin, const Scalar nmEnd ) const override;

			//! Round 3 P1-2: true iff mix has pass-through, own alpha, or any
			//! child with alpha coverage.
			bool HasAlphaCoverage() const override;

			//! Mix-level coverage `(1 - w_clear) * ownAlpha` (used directly by
			//! subsurface entry probes and emitter alpha; composed with the
			//! resolved leaf's alpha in `RayIntersection::HitCoverage()`).
			Scalar AlphaCoverage( const RayIntersectionGeometric& ri ) const override;

			//! MIX §I.3: a mix is never a specular caster.
			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric&,
				const IORStack&
				) const override { return SpecularInfo(); }

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric&,
				const IORStack&,
				const Scalar
				) const override { return SpecularInfo(); }

			//! Post-traversal stochastic leaf selection (Round 3 §3).
			void ResolveMixHit( RayIntersection& ri, ISampler* pSampler ) const override;

			//! Round 3 P1-6: noise-free expected-value denoiser albedo & normal
			//! guides across all leaves (each leaf's own modifier applied to N0).
			void EvaluateExpectedGuides(
				const RayIntersection& ri,
				const bool bClayOverride,
				const IBSDF* pClayBRDF,
				RISEPel& outAlbedo,
				Vector3& outNormal
				) const override;

			//! Part 0 + MIX §I.3: forward path dependence from own alpha, children,
			//! weights, passthrough weight, and child modifiers; also sets
			//! `PathDependence::kVaryingSSSMix` when an SSS child has a spatially
			//! varying weight (refused in BDPT/VCM).
			unsigned int GetPathDependence() const override;

			// Read-back accessors for tests, introspection, and cloning.
			unsigned int GetChildCount() const { return static_cast<unsigned int>( children.size() ); }
			const MixMaterialChild& GetChild( const unsigned int i ) const { return children[i]; }
			const IScalarPainter* GetPassthroughWeight() const { return pPassthroughWeight; }
		};
	}
}

#endif
