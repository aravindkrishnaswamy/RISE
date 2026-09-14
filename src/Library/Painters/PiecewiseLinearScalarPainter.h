//////////////////////////////////////////////////////////////////////
//
//  PiecewiseLinearScalarPainter.h - A scalar painter that stores
//    spectral data as (nm, value) sample pairs and linearly
//    interpolates between samples at evaluation time.
//
//  Used for spectral IOR files (e.g. colors/linear.ior), spectral
//  absorption / scattering curves authored as 2-column files.
//
//  Construction takes a vector of (nm, value) pairs; the parser
//  loads these from a 2-column whitespace-separated text file.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIECEWISE_LINEAR_SCALAR_PAINTER_
#define PIECEWISE_LINEAR_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include <vector>
#include <algorithm>

namespace RISE
{
	namespace Implementation
	{
		class PiecewiseLinearScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		public:
			struct Sample { Scalar nm; Scalar value; };

		protected:
			//! Samples sorted by ascending `nm`.  Loaded from a file by
			//! the parser; we copy in (no shared mutable state).
			std::vector<Sample> samples;

			//! DL-29: RGB-aware evaluation, computed ONCE at construction
			//! (the curve is immutable afterward) by integrating the curve
			//! against the CIE 1931 CMFs under the shared D65-normalised
			//! reference illuminant (`RGBIlluminantSpectrum::
			//! ReferenceIlluminant`) -- the same forward model
			//! docs/SPECTRAL_ILLUMINANT_CONVENTION.md documents for going
			//! from an authored spectrum to RGB:
			//!   rgb = M_XYZ->709 . (int S.D65.cmf dλ) / (int D65.ȳ dλ)
			//! Computed out-of-line in the .cpp (DL-80: ColorUtils.h ->
			//! Color.h -> SpectralPacket.h needs IFunction1D/XYZFromNM
			//! declared in an order only `pch.h` guarantees; keeping this
			//! header free of that chain keeps it safely includable by a
			//! standalone translation unit).  Before this fix `GetValuesAt`
			//! broadcast one 555 nm sample into all three channels --
			//! correct for a constant curve, but silently grey for a curve
			//! (e.g. a measured `tau` absorption/transmission spectrum)
			//! that genuinely varies red-to-blue.
			ScalarTriple cachedRGB;

			//! True when `cachedRGB`'s three channels differ by more than
			//! floating-point noise.  A CMF integration of an EXACTLY flat
			//! curve does not, in general, land on bit-identical R/G/B
			//! (the XYZ->Rec709 matrix multiply and the D65 weighting are
			//! not symmetric under a constant integrand at the ULP level)
			//! -- snapping both `cachedRGB` and this flag to exact-uniform
			//! in that case keeps `ScalarTriple::IsUniform()`'s STRICT `==`
			//! contract intact for a flat curve (the parser's single-
			//! scalar-slot validation and any other exact-equality
			//! consumer), while still reporting genuine variation for a
			//! curve that actually has some (e.g. `colors/linear.ior`,
			//! whose 380nm/720nm samples differ).
			bool bHasPerChannelVariation;

			virtual ~PiecewiseLinearScalarPainter() {}

			//! Wavelength at which `GetValuesAt` reports the value for
			//! RGB rendering.  555 nm picks the luminance-peak point
			//! which matches what RGB integrators use as a default
			//! representative wavelength.  Per-channel rendering paths
			//! that need three values evaluate three times via
			//! `GetValueAtNM` instead.
			static constexpr Scalar kRepresentativeNm = Scalar( 555.0 );

			Scalar EvalAtNM( Scalar nm ) const
			{
				if( samples.empty() ) return Scalar( 0 );
				if( samples.size() == 1 ) return samples[0].value;
				if( nm <= samples.front().nm ) return samples.front().value;
				if( nm >= samples.back().nm  ) return samples.back().value;
				// Binary search for the upper bound.
				const auto it = std::lower_bound(
					samples.begin(), samples.end(), nm,
					[]( const Sample& s, Scalar n ) { return s.nm < n; } );
				// `it` points to the sample with `nm` >= query; previous is < query.
				const Sample& hi = *it;
				const Sample& lo = *(it - 1);
				// Guard against duplicate-nm samples (parser input may
				// have them).  Without this, divide-by-zero produces
				// NaN.  Identical-nm samples have undefined ordering
				// in the value axis; return the lower value as a
				// well-defined convention.
				if( hi.nm == lo.nm ) return lo.value;
				const Scalar t = ( nm - lo.nm ) / ( hi.nm - lo.nm );
				return lo.value + t * ( hi.value - lo.value );
			}

			//! DL-29 forward model, run once at construction.  Defined in
			//! PiecewiseLinearScalarPainter.cpp (see cachedRGB's doc
			//! comment for why this is out-of-line).
			void ComputeCachedRGB();

		public:
			explicit PiecewiseLinearScalarPainter( std::vector<Sample> s )
				: samples( std::move( s ) ),
				  bHasPerChannelVariation( false )
			{
				// Defensive sort — the parser already supplies sorted
				// samples but a programmatic construction site might not.
				std::sort( samples.begin(), samples.end(),
					[]( const Sample& a, const Sample& b ) {
						return a.nm < b.nm;
					} );

				ComputeCachedRGB();
			}

			//! Structural introspection: the parsed (nm, value) samples.
			const std::vector<Sample>& GetSamples() const { return samples; }

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				return cachedRGB;
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar nm
				) const override
			{
				return EvalAtNM( nm );
			}

			bool HasPerChannelVariation() const override { return bHasPerChannelVariation; }
		};
	}
}

#endif
