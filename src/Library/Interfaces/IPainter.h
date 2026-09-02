//////////////////////////////////////////////////////////////////////
//
//  IPainter.h - Declaration of the abstract interface IPainter
//  which is what all painters must implement
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPAINTER_
#define IPAINTER_

#include "../Utilities/Color/Color.h"
#include "IFunction2D.h"
#include "IKeyframable.h"

namespace RISE
{
	class RayIntersectionGeometric;

	//! Spectral-uplift role for an RGB-source painter (Landing 3).
	//! Determines whether the input RGB is treated as a bounded
	//! reflectance, an unbounded radiance, or an illuminant.  Default
	//! is Albedo (the common case for baseColor and similar inputs).
	enum SpectrumKind
	{
		eSpectrumKind_Albedo      = 0,	///< rgb ∈ [0, 1]; baseColor / sheen / transmission_color etc.
		eSpectrumKind_Unbounded   = 1,	///< rgb ≥ 0 with components possibly > 1; emissive / HDR sources
		eSpectrumKind_Illuminant  = 2	///< rgb ≥ 0; pre-multiplied by the reference SPD matching the LUT target's whitepoint (D65 post Stage B colour-space migration, D50 historically)
	};

	//! A painter determines the spectral properties of a material.  It takes
	//! intersection information and from that derives the spectral properties
	//! either as an RISEPel or as the amplitude of a specific wavelength
	class IPainter : 
		public virtual IFunction2D,
		public virtual IKeyframable
	{
	protected:
		IPainter() {};
		virtual ~IPainter(){};

	public:
		//!
		//! This is what a painter essentially does.  It takes
		//! some parameters as to the current RayEngineState, and returns
		//! some color, how this is done is left to the individual
		//! painters
		//!
		/// \return The computed color as an RISEPel
		virtual RISEPel GetColor(
			const RayIntersectionGeometric& ri					///< [in] Geometric intersection details 
			) const = 0;

		//! 
		//! This function is almost the same as the above one, however it returns
		//! the response of the painter to a particular wavelength of light.  Note
		//! that currently only the spectral painter class can do anything of use
		//! with this.  The default implementation returns 0
		//!
		/// \return The intensity for the particular wavelength
		virtual Scalar GetColorNM( 
			const RayIntersectionGeometric& ri,					///< [in] Geometric intersection details 
			const Scalar nm										///< [in] Wavelength to process
			) const = 0;

		//!
		//! This function is also similar to the above ones, however it returns the entire spectrum
		//! rather than just the value at the particular wavelength
		//!
		/// \return The computed color as a spectral packet
		virtual SpectralPacket GetSpectrum(
			const RayIntersectionGeometric& ri 					///< [in] Geometric intersection details
			) const = 0;

		//!
		//! Returns the painter's alpha (opacity) at the given hit, in [0, 1].
		//! Default 1.0 (fully opaque) for painters whose representation is
		//! RGB-only (most painters).  Texture painters that load an RGBA
		//! image override this to return the per-pixel A channel without
		//! the premultiplication that `GetColor` applies.  Used by the
		//! glTF importer's alpha-mask / alphaMode = BLEND wiring (Phase 4)
		//! so the test / blend factor reads the actual alpha rather than
		//! `max(R, G, B)` of the premultiplied baseColor.
		//!
		//! \return Alpha in [0, 1]; default 1 (opaque)
		virtual Scalar GetAlpha(
			const RayIntersectionGeometric& /*ri*/				///< [in] Geometric intersection details
			) const { return Scalar(1); }
	};

	//! An authored colour is "untinted white" iff its minimum RGB
	//! component is >= 1 - 1e-6 (same epsilon as
	//! CoatedBRDF::ResolveCoat's `out.tinted` decision).  A multiplicative
	//! slot at authored white must be a BIT-EXACT no-op on the spectral
	//! pipe too, so that the NM path and the RGB path agree exactly.
	//! `GetColorNM` runs the Jakob-Hanika uplift, whose sigmoid reaches
	//! white only asymptotically: authored white samples to 1 - epsilon
	//! (measured 0.99999970 at 660 nm, 0.99999015 at 550 nm), and
	//! epsilon varies with wavelength -- so without this guard an
	//! untinted slot would attenuate throughput slightly, and
	//! wavelength-dependently, on every bounce.
	//!
	//! HISTORICAL: before Stage C (2026-09-02,
	//! docs/SPECTRAL_ILLUMINANT_CONVENTION.md) the LUT was trained under
	//! a flat E illuminant, under which white was not representable at
	//! all and its uplift COLLAPSED above ~620 nm (1.28e-5 at 660 nm) --
	//! an ~8-10 % red loss per bounce rather than an epsilon.  That is
	//! the measurement the guard was introduced for; the collapse is
	//! gone, the exactness requirement is not.  (Measured curve +
	//! rationale: CoatedLayer.h `PassTransmittance`, ~line 302;
	//! precedent decision: CoatedBRDF::ResolveCoat, ~line 107.)
	inline bool IsUntintedWhite( const RISEPel& c )
	{
		const Scalar minc = r_min( r_min( c[0], c[1] ), c[2] );
		return minc >= Scalar(1) - Scalar(1e-6);
	}

	//! Guarded spectral sample of a multiplicative painter slot: returns
	//! exactly 1.0 when the AUTHORED (un-uplifted) colour is untinted
	//! white, else the raw `GetColorNM` sample.  See `IsUntintedWhite`.
	//!
	//! Precondition: the bound painter must be an eSpectrumKind_Albedo
	//! source.  The floor-only `minc >= 1 - 1e-6` check is EXACT only
	//! because `RGBAlbedoSpectrum::FromRGB` clamps its input to [0, 1]
	//! before the LUT lookup (src/Library/Utilities/Color/RGBSpectra.h:39-40,
	//! clamp itself performed by RGBToSpectrumTable::operator(), documented
	//! at RGBToSpectrumTable.h:93) -- so `minc >= 1 - 1e-6` implies every
	//! channel is already at the clamp ceiling and uplifts as exact white.
	//! An Unbounded or Illuminant-kind painter does NOT clamp (see
	//! RGBUnboundedSpectrum::FromRGB / RGBIlluminantSpectrum::FromRGB in the
	//! same header) -- binding one to a guarded slot would break this
	//! assumption and must not be done without revisiting the guard.
	inline Scalar GuardedGetColorNM( const IPainter& p, const RayIntersectionGeometric& ri, const Scalar nm )
	{
		if( IsUntintedWhite( p.GetColor( ri ) ) ) {
			return Scalar(1);
		}
		return p.GetColorNM( ri, nm );
	}
}

#include "../Intersection/RayIntersectionGeometric.h"

#endif
