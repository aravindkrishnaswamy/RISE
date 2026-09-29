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
//  RGB REPORTING (DL-29, docs/DEBT_LEDGER.md).  `GetValuesAt` returns
//  the curve evaluated at the three representative wavelengths
//  `ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm -- the same
//  three `DielectricSPF`'s RGB dispersion loop refracts against, so a
//  measured `ior` curve and the SPF's per-channel Fresnel describe one
//  set of wavelengths rather than two.  Read that constant's doc
//  comment (Interfaces/IScalarPainter.h) for why a colorimetric CMF
//  integration would be WRONG here: most slots this painter feeds
//  (`ior`, `ext`, absorption/scattering rates, roughness) are consumed
//  NON-LINEARLY, and the painter cannot know which slot it was bound
//  to.  The convention is exact on a flat curve, never clamps, and is
//  monotone in the curve; it is not colorimetric, and a Beer's-law tint
//  built from it matches the spectral render only approximately.
//
//  Before DL-29 this class broadcast a single 555 nm sample into all
//  three channels, so a measured `tau` curve rendered exactly grey
//  under the RGB rasterizers and `HasPerChannelVariation()` always
//  reported false -- which also kept `DielectricSPF`/`PolishedSPF`/
//  `PerfectRefractorSPF` off their dispersion path for a measured `ior`
//  file.
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

			//! The curve at `ScalarPainterRGB::kChannelNM`, computed
			//! ONCE at construction (the samples are immutable
			//! afterward).  For a single-scalar-slot view (see
			//! `MakeSingleScalarSlotView`) this is instead the green
			//! sample broadcast to all three channels.
			ScalarTriple cachedRGB;

			//! `cachedRGB`'s three channels differ.  EXACT comparison,
			//! deliberately: the three values come from evaluating one
			//! curve, so a flat curve gives bit-identical samples and
			//! needs no tolerance, and `ScalarTriple::IsUniform()`'s
			//! strict-`==` contract stays intact without a fuzzy
			//! snap-to-uniform step that could fold a real (small)
			//! gradient into "flat".
			bool bHasPerChannelVariation;

			//! True for the view `MakeSingleScalarSlotView` hands a
			//! material slot that reads only `.v[0]`.  Only affects
			//! `cachedRGB` / `bHasPerChannelVariation`; `GetValueAtNM`
			//! is the same curve either way, so the spectral renderers
			//! see no difference at all.
			bool bSingleSlotView;

			virtual ~PiecewiseLinearScalarPainter() {}

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

		public:
			explicit PiecewiseLinearScalarPainter(
				std::vector<Sample> s,
				bool bSingleSlotView_ = false
				)
				: samples( std::move( s ) ),
				  bHasPerChannelVariation( false ),
				  bSingleSlotView( bSingleSlotView_ )
			{
				// Defensive sort — the parser already supplies sorted
				// samples but a programmatic construction site might not.
				std::sort( samples.begin(), samples.end(),
					[]( const Sample& a, const Sample& b ) {
						return a.nm < b.nm;
					} );

				if( bSingleSlotView ) {
					cachedRGB = ScalarTriple( EvalAtNM(
						ScalarPainterRGB::kChannelNM[ ScalarPainterRGB::kSingleSampleChannel ] ) );
					bHasPerChannelVariation = false;
				} else {
					cachedRGB = ScalarTriple(
						EvalAtNM( ScalarPainterRGB::kChannelNM[0] ),
						EvalAtNM( ScalarPainterRGB::kChannelNM[1] ),
						EvalAtNM( ScalarPainterRGB::kChannelNM[2] ) );
					bHasPerChannelVariation = !cachedRGB.IsUniform();
				}
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

			//! DL-29: a measured 2-column file bound to a single-scalar
			//! material slot keeps loading -- the slot gets the curve's
			//! green (549 nm) sample, not whichever channel lands at
			//! `.v[0]`, and the spectral path is untouched.  See the
			//! base declaration in Interfaces/IScalarPainter.h.
			IScalarPainter* MakeSingleScalarSlotView() const override
			{
				if( bSingleSlotView ) return nullptr;	// already one
				return new PiecewiseLinearScalarPainter( samples, true );
			}

			//! DL-292: the value depends on the wavelength only, never on
			//! the hit (IScalarPainter::IsPositionIndependent).
			bool IsPositionIndependent() const override { return true; }
		};
	}
}

#endif
