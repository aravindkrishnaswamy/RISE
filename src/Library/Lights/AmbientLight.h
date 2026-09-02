//////////////////////////////////////////////////////////////////////
//
//  AmbientLight.h - Ambient light is a hacky light that just
//  returns a constant value of illumination.  It is used only in the
//  ray tracer since the ray tracer is not capable of global illumination
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 01, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef AMBIENT_LIGHT_
#define AMBIENT_LIGHT_

#include "../Interfaces/ILightPriv.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/RGBSpectra.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Transformable.h"
#include "../Animation/KeyframableHelper.h"

namespace RISE
{
	namespace Implementation
	{
		class AmbientLight : 
			public virtual ILightPriv, 
			public virtual Transformable, 
			public virtual Reference
		{
		protected:
			Scalar		radiantEnergy;
			RISEPel		cColor;

			//! `cColor` uplifted as a RADIANCE SOURCE (Stage C slice 2).
			//! See PointLight::cSpectrum for the convention and for the
			//! staleness argument (colour is writable only through
			//! `SetIntermediateValue( kColorID )`).
			RGBIlluminantSpectrum	cSpectrum;

			//! Rebuild `cSpectrum` from `cColor`.  Call after ANY write to
			//! `cColor`.
			void RefreshSpectrum()
			{
				// See DirectionalLight::RefreshSpectrum: FromRGB's scale
				// comes from the max channel outside the maxc>1e-9 guard,
				// so a keyframe-spline overshoot into negative cColor
				// would flip the scale's sign.  Clamp a local copy;
				// cColor is left untouched for emissionColor()/RGB.
				RISEPel c = cColor;
				ColorMath::EnsurePositve( c );
				cSpectrum = RGBIlluminantSpectrum::FromRGB( c );
			}

			virtual ~AmbientLight( ){};

		public:
			AmbientLight( Scalar radiantEnergy_, const RISEPel& c  ) : radiantEnergy( radiantEnergy_ ), cColor( c )
			{
				RefreshSpectrum();
			}

			inline bool CanGeneratePhotons() const override
			{
				return false;
			}

			inline RISEPel radiantExitance() const override
			{
				return RISEPel(0,0,0);
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const override
			{
				return (cColor * radiantEnergy);
			}

			//! Spectral twin of `emittedRadiance` (Stage C slice 2): the
			//! cached illuminant spectrum at `nm`, times the energy.
			inline Scalar emittedRadianceNM( const Vector3& /*vLightOut*/, const Scalar nm ) const override
			{
				return cSpectrum.Eval( nm ) * radiantEnergy;
			}

			inline Point3 position() const override
			{
				return Point3( 0, 0, 0 );
			}

			inline RISEPel   emissionColor() const override  { return cColor; }
			inline Scalar    emissionEnergy() const override { return radiantEnergy; }
			inline LightType lightType() const override      { return LightType::Ambient; }

			inline Ray generateRandomPhoton( const Point3& ptrand ) const override
			{
				return Ray();
			}

			inline Scalar pdfDirection( const Vector3& ) const override
			{
				return 0;
			}

			//! `bFullSphereReceiver` (residual wave 2 item D) is a no-op
			//! here: ambient light applies no cosine gate at all (it
			//! evaluates `brdf.value` straight along the normal), so
			//! there is nothing for the flag to flip.
			//!
			//! `bVolumeReceiver` (residual-ledger item 10 of
			//! docs/PT_ENV_MIS_DOUBLECOUNT.md) is likewise a no-op, and
			//! for the same reason: there is no receiver cosine and no
			//! hemisphere rejection here to remove.  The math below is
			//! deliberately UNCHANGED at a medium scatter vertex.  What
			//! it means there is worth stating plainly, because it is
			//! odd rather than wrong: at such a vertex
			//! `MediumTransport::EvaluateInScattering{,NM}` has set
			//! `ri.vNormal == wo`, so `brdf.value( ri.vNormal, ri )`
			//! evaluates the PHASE FUNCTION at `p(wo, wo)` -- pure
			//! back-scatter.  An ambient light is a directionless
			//! constant with no incident direction of its own, so no
			//! choice of evaluation direction is more defensible than
			//! any other; back-scatter is as meaningful as the hack
			//! admits, and for the isotropic phase (the common case) the
			//! choice does not matter at all since `p` is constant.
			//! Anisotropic phase functions will read the wrong lobe --
			//! an accepted limitation of an ambient light, not a defect
			//! this parameter can fix.  Part B of the same fix likewise
			//! skips ambient: it is a constant with no ray to attenuate.
			inline void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool, RISEPel& amount, const bool = false, const bool = false ) const override
			{
				amount = cColor * radiantEnergy * brdf.value( ri.vNormal, ri );
			}

			//! Per-wavelength evaluation: sample the light's own illuminant
			//! spectrum at `nm` and multiply by the per-NM BSDF.  Matches
			//! the RGB version but uses brdf.valueNM, preserving the
			//! surface's spectral character (which the older
			//! Luminance(amount_RGB) projection collapsed to white because
			//! amount_RGB had the per-NM BSDF replaced by its RGB
			//! equivalent).  Stage C slice 2 replaced the remaining
			//! Rec.709 luma projection of `cColor` with the real spectrum,
			//! so a coloured ambient is no longer spectrally grey.
			inline Scalar ComputeDirectLightingNM(
				const RayIntersectionGeometric& ri,
				const IRayCaster&,
				const IBSDF& brdf,
				const bool,
				const Scalar nm,
				const bool = false,				///< bFullSphereReceiver: no-op, see the RGB override above
				const bool = false				///< bVolumeReceiver: no-op, see the RGB override above
				) const override
			{
				return cSpectrum.Eval( nm ) * radiantEnergy * brdf.valueNM( ri.vNormal, ri, nm );
			}

			// No light-specific state to refresh; the base composition is all an
			// ambient light needs.  Declared explicitly (rather than left to
			// inheritance) only to keep the parity with the other two lights
			// obvious to the next reader.
			inline void	FinalizeTransformations( const Matrix4& parentWorld ) override { Transformable::FinalizeTransformations( parentWorld ); };
			using Transformable::FinalizeTransformations;

			// For keyframamble interface
			// Keyframe parameter IDs.  The IDs MUST match between
			// `KeyframeFromParameters` (allocator) and
			// `SetIntermediateValue` (consumer) — historically this
			// class shipped with the allocator using 1000/1001 and
			// the consumer using 100/101, so every ambient color /
			// energy edit silently no-op'd while reporting success
			// (the keyframe got built, but the consumer's switch
			// fell through and changed nothing).  Match PointLight's
			// 100/101 convention now that the divergence is fixed.
			static const unsigned int kColorID  = 100;
			static const unsigned int kEnergyID = 101;

			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override
			{
				IKeyframeParameter* p = 0;

				// Check the name and see if its something we recognize
				if( name == "color" ) {
					double d[3];
					if( ParseStrictVec3( value, d ) ) {
						p = new Parameter<RISEPel>( RISEPel(d), kColorID );
					}
				} else if( name == "energy" ) {
					Scalar parsed;
					if( !ParseStrictScalar( value, parsed ) ) {
						return 0;
					}
					p = new Parameter<Scalar>( parsed, kEnergyID );
				} else {
					return Transformable::KeyframeFromParameters( name, value );
				}

				GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
				return p;
			}

			void SetIntermediateValue( const IKeyframeParameter& val ) override
			{
				switch( val.getID() )
				{
				case kColorID:
					{
						cColor = *(RISEPel*)val.getValue();
						RefreshSpectrum();		// keep the cached illuminant spectrum in step
					}
					break;
				case kEnergyID:
					{
						radiantEnergy = *(Scalar*)val.getValue();
					}
					break;
				}

				Transformable::SetIntermediateValue( val );
			}
		};
	}
}

#endif
