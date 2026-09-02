//////////////////////////////////////////////////////////////////////
//
//  DirectionalLight.h - Declaration of the DirectionalLight class.
//    This is light infinitely far away such that all the light rays
//    are coming parallel.
//
//    --- Direction convention (READ THIS BEFORE AUTHORING SCENES) ---
//
//    `vDirection` (the chunk's `direction` parameter) is the vector
//    pointing FROM any surface TO the light source -- i.e., it
//    describes WHERE THE LIGHT IS, not where it shines.  A surface
//    is lit when `N · vDirection > 0`.  See ComputeDirectLighting
//    in the .cpp.
//
//    To put a light "above-front-right" of a camera that's at +Z
//    looking at the origin, use direction (0.4, 0.4, 0.7) (positive
//    Z so it lights camera-facing surfaces with normals near +Z).
//    The opposite sign ((-0.4, -0.4, -0.7)) puts the light behind
//    the asset and renders the camera-facing side dark.
//
//    Importers from foreign formats that use the "shine direction"
//    convention (glTF KHR_lights_punctual, Unity, Unreal) MUST
//    negate before passing through to AddDirectionalLight.  See
//    docs/SCENE_CONVENTIONS.md §1 for the full rationale and the
//    historical bugs this caused.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 15, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIRECTIONAL_LIGHT_
#define DIRECTIONAL_LIGHT_

#include "../Interfaces/ILightPriv.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/RGBSpectra.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Transformable.h"

namespace RISE
{
	namespace Implementation
	{
		class DirectionalLight : public virtual ILightPriv, public virtual Transformable, public virtual Reference
		{
		protected:
			Scalar		radiantEnergy;
			RISEPel		cColor;
			Vector3		vDirection;

			//! `cColor` uplifted as a RADIANCE SOURCE (Stage C slice 2).
			//! See PointLight::cSpectrum for the convention and for the
			//! staleness argument (colour is writable only through
			//! `SetIntermediateValue( COLOR_ID )`).
			RGBIlluminantSpectrum	cSpectrum;

			//! Rebuild `cSpectrum` from `cColor`.  Call after ANY write to
			//! `cColor`.
			void RefreshSpectrum();

			virtual ~DirectionalLight( );

		public:

			inline bool CanGeneratePhotons() const
			{
				return false;
			}

			inline RISEPel radiantExitance() const
			{
				return RISEPel(0,0,0);
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const
			{
				return (cColor * radiantEnergy);
			}

			//! Spectral twin of `emittedRadiance` (Stage C slice 2): the
			//! cached illuminant spectrum at `nm`, times the energy.
			inline Scalar emittedRadianceNM( const Vector3& /*vLightOut*/, const Scalar nm ) const
			{
				return cSpectrum.Eval( nm ) * radiantEnergy;
			}

			inline Point3 position() const
			{
				return Point3( 0, 0, 0 );
			}

			inline RISEPel   emissionColor() const  { return cColor; }
			inline Scalar    emissionEnergy() const { return radiantEnergy; }
			inline LightType lightType() const      { return LightType::Directional; }
			//! ILight's default `emissionDirection()` returns
			//! `Vector3(0,1,0)`.  Without this override, the introspection
			//! panel reads stale (0,1,0) regardless of edits, AND
			//! `SetLightProperty("direction")` undo silently drops the
			//! entry because `ReadLightParam` captures the wrong prev
			//! value.  Rendering was always correct (ComputeDirectLighting
			//! reads `vDirection` directly).  Detected by
			//! SceneEditorLightFullCoverageTest's `dir direction.x/.z`
			//! checks; before this override the magnitude check passed
			//! by coincidence (|(0,1,0)| = 1) while the components were
			//! wrong.
			inline Vector3   emissionDirection() const { return vDirection; }

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
			{
				return Ray();
			}

			inline Scalar pdfDirection( const Vector3& ) const
			{
				return 0;
			}

			DirectionalLight( Scalar radiantEnergy_, const RISEPel& c, const Vector3& vDir );

			//! `bFullSphereReceiver` (residual wave 2 item D,
			//! 2026-08-27; docs/HAIR_FUR_DESIGN.md section 4.1 /
			//! HairBSDF.h section 5's "KNOWN REMAINING SIBLING" entry,
			//! now closed): the one full-sphere-NEE gate
			//! `IMaterial::ScattersFullSphere()` did not reach when it
			//! landed (commit 1472ae57).  A directional light has no
			//! MIS partner (same as the delta-position row LightSampler
			//! itself gates), so when true this simply restores the
			//! below-horizon direct term at full weight via `fabs`
			//! instead of rejecting it -- see the .cpp.
			//!
			//! `bVolumeReceiver` (residual-ledger item 10 of
			//! docs/PT_ENV_MIS_DOUBLECOUNT.md, 2026-08-27): this is the
			//! light that item names.  At a MEDIUM SCATTER vertex
			//! `ri.vNormal` is `wo`, not a normal, so both the `fDot`
			//! factor and its `<= 0` rejection are dropped and the light
			//! delivers RADIANCE ONLY -- the phase function, handed in as
			//! `brdf`, carries the entire angular term.  Takes precedence
			//! over `bFullSphereReceiver`.  Full derivation in the .cpp.
			void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, RISEPel& amount, const bool bFullSphereReceiver = false, const bool bVolumeReceiver = false ) const;

			//! Per-wavelength direct-lighting evaluation.  Mirrors the
			//! RGB ComputeDirectLighting (cosine, shadow check) but
			//! uses brdf.valueNM(direction, ri, nm) so the surface's
			//! spectral character is preserved.  Light color projected
			//! to luminance per the JH-flat-E convention.
			//! `bFullSphereReceiver` / `bVolumeReceiver`: see the RGB
			//! overload above.
			Scalar	ComputeDirectLightingNM(
				const RayIntersectionGeometric& ri,
				const IRayCaster& pCaster,
				const IBSDF& brdf,
				const bool bReceivesShadows,
				const Scalar nm,
				const bool bFullSphereReceiver = false,
				const bool bVolumeReceiver = false
				) const;

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
		};
	}
}

#endif
