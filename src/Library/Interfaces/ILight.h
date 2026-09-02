//////////////////////////////////////////////////////////////////////
//
//  ILight.h - Interface of the Light class.  This class represents
//  lights that are specific to computer graphics (ie. hack lights)
//  Some examples of these kinds of lights are infinite point lights, 
//  infinite point, spot lights, and directional lights.  
//
//  Note that Ambient light is also a subclass
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 23, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILIGHT_
#define ILIGHT_

#include "IReference.h"
#include "IKeyframable.h"
#include "IBSDF.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	class IRayCaster;

	//! This is a 'hacky' light that are not physically based, such as point lights
	//! spot lights or directional lights
	class ILight :
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		virtual ~ILight(){};
		ILight(){};

	public:

		//! Asks if the light can generate photons for the purpose of photon mapping
		virtual bool CanGeneratePhotons() const = 0;

		//! Toggle photon-mapping eligibility at runtime.  Used by the
		//! interactive editor's `shootphotons` row.  Default impl is a
		//! no-op so out-of-tree subclasses still link AND so light
		//! kinds that don't carry photon-mapping storage
		//! (AmbientLight / DirectionalLight) silently absorb the edit
		//! — `CanGeneratePhotons()` returns false unconditionally for
		//! those kinds so the editor doesn't surface the row in the
		//! first place.  Concrete photon-capable types
		//! (PointLight / SpotLight) override to write their stored
		//! `bShootPhotons` flag.
		virtual void SetCanGeneratePhotons( bool /*b*/ ) {}

		//! Asks the light for its radiant exitance
		virtual RISEPel radiantExitance() const = 0;

		//! Asks the light for its emitted radiance in a particular direction
		virtual RISEPel emittedRadiance( const Vector3& vLightOut ) const = 0;

		//! Per-wavelength emitted radiance in a particular direction
		//! (Stage C slice 2, docs/SPECTRAL_ILLUMINANT_CONVENTION.md).
		//!
		//! Every NM consumer of a non-mesh light used to take
		//! `emittedRadiance()` and project it to a Rec.709 luma scalar
		//! (`0.2126 r + 0.7152 g + 0.0722 b`), used unchanged at every
		//! wavelength — so a coloured point / spot / directional light was
		//! spectrally GREY in NM renders, and even a white one landed on
		//! the flat-spectrum chromaticity (1.20, 0.95, 0.91) rather than
		//! on white.  An RGB-authored light source means "reflectance-
		//! under-D65 times D65", so its spectrum is the JH sigmoid times
		//! the reference illuminant, and the film resolves that back to
		//! the authored RGB exactly.
		//!
		//! Default: uplift `emittedRadiance(vLightOut)` per call.  In-tree
		//! lights override with a spectrum cached at construction (and
		//! rebuilt on every colour mutation) so no LUT lookup happens on
		//! the hot path.  Declared out-of-line (defined in PointLight.cpp)
		//! to keep the Jakob-Hanika LUT header out of every translation
		//! unit that includes ILight.h.
		virtual Scalar emittedRadianceNM( const Vector3& vLightOut, const Scalar nm ) const;

		//! Returns the light's world-space position (for spatial importance estimation)
		virtual Point3 position() const = 0;

		//! Asks the light to generate a random emitted photon
		virtual Ray generateRandomPhoton( const Point3& ptrand ) const = 0;

		//! Returns the solid angle PDF for the directional sampling used by generateRandomPhoton
		virtual Scalar pdfDirection( const Vector3& dir ) const = 0;

		//! Is this a positional (point/spot) light suitable for equiangular sampling?
		//! Returns false for directional and ambient lights.
		virtual bool IsPositionalLight() const { return false; }

		//! Returns the primary emission direction (unit vector).
		//! Default is (0,1,0); overridden by directional lights (e.g. spot).
		virtual Vector3 emissionDirection() const { return Vector3(0,1,0); }

		//! Returns the half-angle (radians) of the emission cone.
		//! PI means full-sphere (isotropic, e.g. point lights).
		virtual Scalar emissionConeHalfAngle() const { return PI; }

		//! Read-back accessors for the editable properties surfaced
		//! through `LightIntrospection`.  Default impls return
		//! sentinel values (white / unit / zero) so subclasses that
		//! haven't opted in yet still link.  Concrete light types
		//! override to return their stored color / energy fields.
		virtual RISEPel emissionColor() const { return RISEPel( 1, 1, 1 ); }
		virtual Scalar  emissionEnergy() const { return Scalar( 1 ); }

		//! Discriminator for `LightIntrospection`'s per-type rendering.
		//! Each concrete light type overrides to identify itself; the
		//! introspection layer dispatches on this rather than RTTI to
		//! keep the per-type cases declarative.  Default = Unknown so
		//! a third-party ILight subclass falls back to the common
		//! property set (position / energy / color).
		enum class LightType
		{
			Unknown      = 0,
			Point        = 1,
			Spot         = 2,
			Directional  = 3,
			Ambient      = 4,
		};
		virtual LightType lightType() const { return LightType::Unknown; }

		//! Spot-light-specific: the world-space target the cone is
		//! aimed at (`SpotLight::ptTarget`).  PRECONDITION for the
		//! returned value to be meaningful: `lightType() ==
		//! LightType::Spot`.  Default impl returns origin so callers
		//! that bypass the discriminator don't crash, but the value
		//! is a sentinel — not a real position.  `LightIntrospection`
		//! only surfaces the `target` row when the discriminator
		//! says Spot, so the panel never displays the sentinel.
		virtual Point3 emissionTarget() const { return Point3( 0, 0, 0 ); }

		//! Spot-light-specific: inner / outer cone full angles in
		//! radians.  Defaults match an isotropic point light (full
		//! sphere both).  `emissionConeHalfAngle()` already covers
		//! the outer half-angle for non-spot lights, but
		//! `LightIntrospection` needs both inner AND outer for the
		//! spot-light-specific UI.
		virtual Scalar emissionInnerAngle() const { return PI; }
		virtual Scalar emissionOuterAngle() const { return PI; }

		//! Computes direct lighting
		//!
		//! `bFullSphereReceiver` (residual wave 2 item D, 2026-08-27;
		//! the sibling this file's `EvaluateDirectLighting` FULL-SPHERE
		//! NEE block comment named as owed) closes the one full-sphere-
		//! NEE gap `IMaterial::ScattersFullSphere()` did not reach: this
		//! virtual, not `LightSampler`'s own inline delta-light site, is
		//! what a groom lit by a `directional_light` goes through (see
		//! `EvaluateDirectLighting`'s Step-1 zero-exitance pass), so the
		//! capability has to be threaded down to here too.  Defaulted
		//! `false` so every pre-existing call site (any out-of-tree
		//! light, and `LightManager::ComputeDirectLighting`'s own
		//! forwarding loop, which has no material to ask) keeps its
		//! exact prior behaviour without being touched.  Only
		//! `DirectionalLight` reads it (see its own override); every
		//! other concrete light no-ops it, either because it applies no
		//! cosine gate at all (`AmbientLight`) or because its own NEE
		//! site is never reached through this virtual for a delta-
		//! position light (`PointLight` / `SpotLight` -- LightSampler's
		//! proportional-selection path evaluates those inline, already
		//! capability-gated there; only Step 1's zero-exitance sweep
		//! (ambient, directional) and BDPT's mirroring s==1 row call
		//! this virtual at all).
		//! `bVolumeReceiver` (residual-ledger item 10 of
		//! docs/PT_ENV_MIS_DOUBLECOUNT.md, 2026-08-27): the receiver is
		//! a MEDIUM SCATTER vertex, not a surface.  Same defaulted-tail
		//! shape as `bFullSphereReceiver` above, for the same reason --
		//! every pre-existing call site keeps its exact prior behaviour
		//! untouched.
		//!
		//! WHY IT EXISTS.  `MediumTransport::EvaluateInScattering{,NM}`
		//! reuses this surface-shaped interface at a phase-function
		//! vertex by synthesising a `RayIntersectionGeometric` whose
		//! `vNormal` is the OUTGOING direction `wo`.  That synthetic
		//! normal is not a surface normal and carries no geometric
		//! meaning, so any `Dot(vToLight, ri.vNormal)` computed against
		//! it is meaningless -- and the `<= 0` rejection that usually
		//! accompanies it throws away half the sphere at a vertex that
		//! scatters over the WHOLE sphere.
		//!
		//! THE CONTRACT WHEN TRUE: deliver RADIANCE ONLY.  No receiver
		//! cosine, no hemisphere rejection.  Derivation: the medium
		//! in-scattering integrand is
		//!     Ls(x, wo) = sigma_s(x) * INT_{S^2} p(wi, wo) Li(x, wi) dwi
		//! -- an integral over the FULL sphere whose only angular factor
		//! is the phase function `p`.  There is no `|cos theta|` because
		//! there is no surface to project onto: the surface form's cosine
		//! is the projected-area Jacobian dA_perp/dA of a surface patch,
		//! and a scattering volume element has no patch and no
		//! orientation.  The caller already supplies `p` as the `brdf`
		//! (`MediumScatterBSDF::value` returns `p(vLightIn, wo)`), and
		//! multiplies by `sigma_s` outside, so this virtual's entire job
		//! at such a vertex is `Li` -- i.e. emitted radiance times
		//! visibility.  This is exactly what `LightSampler`'s own Step-2
		//! and Step-3 rows already do inline: they test `isVolumeScatter`
		//! and force `cosSurface = 1.0` (LightSampler.cpp's delta-light,
		//! mesh-luminary and environment rows).  Step 1 -- the
		//! zero-exitance sweep that reaches this virtual -- did not, which
		//! is the defect this parameter closes.
		//!
		//! PRECEDENCE: `bVolumeReceiver` OVERRIDES `bFullSphereReceiver`.
		//! `bFullSphereReceiver` replaces the signed cosine with `|cos|`
		//! (a full-sphere-scattering SURFACE still has a real normal and a
		//! real projected-area factor); `bVolumeReceiver` removes the
		//! cosine altogether.  "No cosine at all" beats "unsigned cosine",
		//! so when both are true the volume rule wins.
		//!
		//! IMPLEMENTOR NOTE: every concrete light MUST honour this if it
		//! applies a receiver cosine or a hemisphere gate, even one that
		//! today is unreachable through this virtual at a volume vertex
		//! (`PointLight` / `SpotLight` -- see their overrides).  A
		//! defaulted flag that silently means the wrong thing at one
		//! implementor is the sibling-site bug pattern
		//! docs/skills/audit-by-bug-pattern.md exists to prevent.
		virtual void ComputeDirectLighting(
			const RayIntersectionGeometric& ri,				///< [in] Geometric intersection details at point to compute lighting information
			const IRayCaster& pCaster,						///< [in] The ray caster to use for occlusion testing
			const IBSDF& brdf,								///< [in] BRDF of the object
			const bool bReceivesShadows,					///< [in] Should shadow checking be performed?
			RISEPel& amount,								///< [out] Amount of lighting
			const bool bFullSphereReceiver = false,			///< [in] When true, use |cos| instead of the signed cosine (a full-sphere-scattering receiver, e.g. hair); see IMaterial::ScattersFullSphere()
			const bool bVolumeReceiver = false				///< [in] When true, the receiver is a phase-function (medium scatter) vertex: NO receiver cosine and NO hemisphere rejection; overrides bFullSphereReceiver.  See the block comment above.
			) const = 0;

		//! Per-wavelength direct-lighting contribution at wavelength
		//! `nm`.  Used by the spectral integrators (LightSampler /
		//! BDPTIntegrator NM path) to evaluate a non-mesh (delta-
		//! position or delta-direction) light contribution with the
		//! per-NM BSDF (`brdf.valueNM`) rather than the RGB BSDF.
		//!
		//! (Dated 2026-09-02.) The spectral character comes from BOTH the
		//! BSDF and the light: since Stage C slice 2
		//! (docs/SPECTRAL_ILLUMINANT_CONVENTION.md), a light's emission is
		//! the reference D65-shaped illuminant spectrum, not a flat
		//! scalar -- concrete lights (`PointLight` / `SpotLight` /
		//! `DirectionalLight` / `AmbientLight`) cache an
		//! `RGBIlluminantSpectrum` built from their colour, rebuilt on
		//! every colour write, and evaluate it at `nm` here.  A flat
		//! scalar reused at every wavelength would resolve to the
		//! flat-spectrum chromaticity (1.20, 0.95, 0.91) on this film
		//! instead of round-tripping to the authored RGB colour; the
		//! stale "Illuminant E projection via `ColorMath::Luminance`"
		//! description this comment used to carry is what Stage C
		//! replaced.  This is the per-NM analog of the RGB
		//! `ComputeDirectLighting`.
		//!
		//! Default impl falls back to running the RGB version and
		//! uplifting the resulting RGB as an ILLUMINANT at @a nm — the
		//! best available answer for an out-of-tree light type, since
		//! `amount` is a computed RADIANCE and so round-trips through the
		//! film back to the RGB result.  (Before Stage C slice 2 this
		//! projected to Rec.709 luma and discarded the wavelength, which
		//! tinted the result by the flat-spectrum chromaticity.)
		//! Concrete RISE light types (Ambient / Directional / Point /
		//! Spot) override this to multiply per-NM BSDF correctly.
		virtual Scalar ComputeDirectLightingNM(
			const RayIntersectionGeometric& ri,				///< [in] Geometric intersection details at point to compute lighting information
			const IRayCaster& pCaster,						///< [in] The ray caster to use for occlusion testing
			const IBSDF& brdf,								///< [in] BSDF of the object (per-NM eval via valueNM)
			const bool bReceivesShadows,					///< [in] Should shadow checking be performed?
			const Scalar nm,								///< [in] Wavelength (nm) at which to evaluate
			const bool bFullSphereReceiver = false,			///< [in] See the RGB ComputeDirectLighting's doc
			const bool bVolumeReceiver = false				///< [in] See the RGB ComputeDirectLighting's doc
			) const;
	};
}

#include "IRayCaster.h"

#endif
