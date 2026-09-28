//////////////////////////////////////////////////////////////////////
//
//  IScalarPainter.h - Declaration of the IScalarPainter interface.
//
//  A scalar painter stores a PHYSICAL SCALAR quantity at every
//  surface point.  Distinct from IPainter (which represents colors
//  with colorspace semantics + JH spectral uplift), an IScalarPainter
//  represents a wavelength-dependent or wavelength-independent
//  physical scalar — IOR, scattering coefficient, roughness,
//  absorption, Phong exponent, etc. — and carries no colorspace.
//
//  Why a separate interface
//
//    Overloading IPainter as a scalar container forces every scalar
//    parameter through `GetColorNM`, which JH-uplifts the underlying
//    RGB.  JH uplift is designed for albedos in [0, 1]: it returns
//    nonsense (clamped / inverted) for physical scalars like IOR=1.5
//    or scattering=1e6.  Symptom: spectral renders of any material
//    with a numeric IOR / scattering / roughness silently mis-shade.
//    See docs/ISCALARPAINTER_REFACTOR.md for the full diagnosis.
//
//    `IScalarPainter` never goes through JH uplift.  Implementations
//    return the authored physical value directly.
//
//  Three axes of variation
//
//    Painters implement the interface freely along any of:
//      - Wavelength: SellmeierScalarPainter, PolynomialScalarPainter,
//                    PiecewiseLinearScalarPainter, Function1DScalarPainter.
//      - Spatial:    TextureScalarPainter, Function2DScalarPainter,
//                    PerlinScalarPainter, WorleyScalarPainter.
//      - Channel:    RGBScalarPainter (per-channel triple).
//    Composition: ScaledScalarPainter, MultiplyScalarPainter, AddScalarPainter.
//
//  Conventions
//
//    Materials that use a single scalar (e.g. roughness) read
//    `GetValuesAt(ri).v[0]`.  The parser's descriptor system
//    rejects per-channel painters in single-scalar slots, so this
//    is never an accidental selection of "red channel" — the user
//    must explicitly author a single-valued painter for those slots.
//
//    Wavelength-independent painters get `GetValueAtNM` for free
//    via the default implementation (returns `GetValuesAt.v[0]`).
//    Only wavelength-varying painters override it.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISCALARPAINTER_
#define ISCALARPAINTER_

#include "../Utilities/Math3D/Math3D.h"
#include "IReference.h"

namespace RISE
{
	class RayIntersectionGeometric;

	//! Representative wavelength of each RGB channel, in nanometres.
	//!
	//! THE CONVENTION for turning a WAVELENGTH-parameterised physical
	//! scalar into the `ScalarTriple` an RGB render consumes: evaluate
	//! the curve AT THESE THREE WAVELENGTHS.  Nothing else — no CMF
	//! integration, no illuminant weighting, no colorspace matrix, no
	//! gamut mapping.
	//!
	//! Why not a colorimetric integral.  A colorimetric RGB is the right
	//! answer only for a slot the renderer consumes LINEARLY as a
	//! transmittance or reflectance (`tau`).  Most `IScalarPainter`
	//! slots are not that: `ior` / `ext` / `film_ior` / `film_extinction`
	//! feed a per-channel Fresnel evaluation, `absorption` /
	//! `scattering` / `extinction` are per-channel RATES that then go
	//! through `exp()`, `roughness` and Phong exponents parameterise a
	//! lobe.  `f(integral of n) != integral of f(n)` for all of those,
	//! and the painter cannot know which slot it was bound to.  Sampling
	//! the curve at a representative wavelength per channel is the same
	//! approximation the renderer already makes everywhere else on the
	//! RGB path (it is exactly what `DielectricSPF`'s RGB dispersion
	//! loop does with its three per-channel iors), it is EXACT for a
	//! flat curve, it never clamps, and it is monotone in the curve.
	//!
	//! Limits, stated once here so no caller has to rediscover them: the
	//! triple is NOT colorimetric (a curve and its metamer do not give
	//! the same triple); it is exact only AT these three wavelengths;
	//! and a Beer's-law tint built from it matches the spectral render
	//! only approximately, and only near unit optical depth.  Concrete
	//! case: gold's measured conductor n/k reflectance, sampled at
	//! this convention's three wavelengths, reads approximately
	//! `(1.000, 0.883, 0.412)`; a colorimetric (CMF-integrated) RGB of
	//! the same curve reads approximately `(1.000, 0.730, 0.326)` —
	//! visibly LESS saturated under this convention (a higher green and
	//! blue relative to red) than the colorimetric answer, because a
	//! discrete 3-point sample misses spectral structure a CMF integral
	//! averages over.  This is expected, not a defect: see "why not a
	//! colorimetric integral" above for why the colorimetric answer is
	//! the wrong target for most of this interface's slots in the first
	//! place.

	//!
	//! The values are the sRGB / Rec.709 primaries' dominant
	//! wavelengths.  `DielectricSPF`'s RGB dispersion loop reads the
	//! same array (it used to carry a private copy named
	//! `kARChannelNM`); keep the two in one place so a painter's triple
	//! and the SPF's per-channel refraction always describe the same
	//! three wavelengths.
	namespace ScalarPainterRGB
	{
		const Scalar kChannelNM[3] = { Scalar( 611.0 ), Scalar( 549.0 ), Scalar( 465.0 ) };

		//! Index into `kChannelNM` used when a wavelength-varying
		//! painter has to report ONE value (a single-scalar material
		//! slot): green, the luminance-dominant channel and the middle
		//! of the three samples.
		const unsigned int kSingleSampleChannel = 1u;
	}

	//! Three scalar values at a point, no colorspace.
	//!
	//! For RGB rendering, the three slots are interpreted as
	//! R / G / B channel values when the consumer is per-channel-aware
	//! (e.g. RGB dispersion); for materials that take a single scalar
	//! the canonical read is `.v[0]` and the parser's descriptor
	//! enforces that the bound painter is single-valued.
	struct ScalarTriple
	{
		Scalar v[3];

		ScalarTriple() : v{ Scalar(0), Scalar(0), Scalar(0) } {}
		explicit ScalarTriple( Scalar x ) : v{ x, x, x } {}
		ScalarTriple( Scalar r, Scalar g, Scalar b ) : v{ r, g, b } {}

		Scalar  operator[]( unsigned int i ) const { return v[i]; }
		Scalar& operator[]( unsigned int i )       { return v[i]; }

		//! True if all three components are exactly equal.
		//! Used by the parser to validate that a painter bound to a
		//! single-scalar slot does not have per-channel variation.
		//! Strict `==` is intentional — `RGBScalarPainter` constructed
		//! with three equal literals (e.g. inline `1.5 1.5 1.5`) has
		//! bit-identical channel values; the only path that creates
		//! "almost equal" channels is colorspace conversion of an
		//! `IPainter` (which `IScalarPainter` deliberately avoids).
		bool IsUniform() const
		{
			return v[0] == v[1] && v[1] == v[2];
		}
	};

	//! Pure physical-scalar painter (see file header for design).
	//!
	//! Contract between `GetValuesAt` and `GetValueAtNM`:
	//!  - For wavelength-INDEPENDENT painters (`UniformScalarPainter`,
	//!    `TextureScalarPainter`, `Function2DScalarPainter`, etc.):
	//!    `GetValuesAt(ri).v[i] == GetValueAtNM(ri, *)` for every i
	//!    and any wavelength.  The default `GetValueAtNM` impl
	//!    upholds this by returning `GetValuesAt(ri).v[0]`.
	//!  - For wavelength-VARYING painters (`SellmeierScalarPainter`,
	//!    `PolynomialScalarPainter`, `PiecewiseLinearScalarPainter`,
	//!    `Function1DScalarPainter`, `RGBScalarPainter`):
	//!    `GetValueAtNM(ri, nm)` varies with `nm`, while
	//!    `GetValuesAt(ri)` reports a per-implementation representative
	//!    value — the curve (or wrapped function, or dispersion formula)
	//!    sampled at `ScalarPainterRGB::kChannelNM` for
	//!    `PiecewiseLinearScalarPainter`, `SellmeierScalarPainter`,
	//!    `PolynomialScalarPainter`, and `Function1DScalarPainter` alike
	//!    (DL-29 established the convention on the first of these;
	//!    DL-82 brought the other three into line — see each class's own
	//!    header comment), the authored `(r, g, b)` triple for
	//!    `RGBScalarPainter`.
	class IScalarPainter :
		public virtual IReference
	{
	protected:
		IScalarPainter() {}
		virtual ~IScalarPainter() {}

	public:
		//! RGB-rendering query: per-channel scalar triple at the hit.
		//! Materials that need a single value read `.v[0]`; materials
		//! that need three (RGB dispersion) read all three.
		virtual ScalarTriple GetValuesAt(
			const RayIntersectionGeometric& ri
			) const = 0;

		//! Spectral-rendering query: scalar value at the hit and
		//! wavelength.  Default implementation calls `GetValuesAt`
		//! and returns the first component — correct for wavelength-
		//! independent painters; overridden by truly wavelength-
		//! varying painters.
		virtual Scalar GetValueAtNM(
			const RayIntersectionGeometric& ri,
			Scalar nm
			) const
		{
			(void) nm;
			return GetValuesAt( ri ).v[0];
		}

		//! Static authoring hint: does this painter have per-channel
		//! variation?  Used by the parser at material-construction
		//! time to reject per-channel painters in single-scalar slots.
		//! Default false; `RGBScalarPainter` overrides to true.
		//!
		//! This is a STATIC property of the painter type, not a
		//! per-hit query — a `TextureScalarPainter` returns false
		//! even though the texture's pixels happen to have R != G != B
		//! per-pixel (the texture stores a grayscale channel by
		//! contract).
		virtual bool HasPerChannelVariation() const { return false; }

		//! When this painter's per-channel triple is a SPECTRAL SAMPLING
		//! of one authored curve (the curve read at
		//! `ScalarPainterRGB::kChannelNM`) rather than three
		//! independently AUTHORED channel values, return a newly
		//! allocated painter — caller owns the reference — that reports
		//! the same curve through `GetValueAtNM` but a UNIFORM
		//! `GetValuesAt` triple (the green sample,
		//! `kSingleSampleChannel`) and `HasPerChannelVariation() ==
		//! false`.  That is the view a material slot which reads only
		//! `.v[0]` should bind.  Returns `nullptr` — the default, and
		//! the right answer for `RGBScalarPainter` and for an inline
		//! `r g b` triple — when there is no such curve, in which case
		//! the parser rejects the binding outright.
		//!
		//! The distinction is authoring intent, not arithmetic.  Someone
		//! who typed three different channel numbers into a
		//! single-scalar slot made a mistake and should be told so;
		//! someone who bound a measured 2-column spectral file did not,
		//! and their scene must keep loading (it did before the triple
		//! became per-channel) with the slot reading a well-defined
		//! single wavelength instead of whichever channel happens to sit
		//! at `.v[0]`.
		virtual IScalarPainter* MakeSingleScalarSlotView() const { return nullptr; }

		//! DL-09 (docs/DL09_GRADED_INDEX_INTERIOR_FACTOR.md): is this
		//! painter a genuine function of WORLD POSITION and nothing else
		//! the hit record carries -- so that it can be evaluated at an
		//! arbitrary interior point of a medium (a point that is not a hit
		//! on the painter's own object and so has no UV, normal or
		//! footprint) from a record carrying only `ptIntersection`?
		//!
		//! True only when the value really VARIES with position (a
		//! position-independent painter reports false: it needs no
		//! interior-segment factor) and is a single scalar (a per-channel
		//! triple would need to know which RGB channel a walk is carrying).
		//! The default is false, which is the conservative answer for
		//! every UV-, normal- or signal-driven form: `n` is undefined at an
		//! interior point for those, and a walk through such a medium
		//! keeps the pre-DL-09 accounting (see the derivation doc, §4 and
		//! §8, for why that is consistent on every completed path).
		virtual bool IsWorldPositionField() const { return false; }
	};
}

#endif
