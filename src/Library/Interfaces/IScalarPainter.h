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
//    Composition: ScaledScalarPainter, MultiplyScalarPainter.
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
	//!    value (e.g. value at 555 nm for piecewise/function-1D, value
	//!    at the d-line 587.6 nm for Sellmeier, the authored
	//!    `(r, g, b)` triple for `RGBScalarPainter`).
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
	};
}

#endif
