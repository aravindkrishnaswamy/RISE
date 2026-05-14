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

		public:
			explicit PiecewiseLinearScalarPainter( std::vector<Sample> s )
				: samples( std::move( s ) )
			{
				// Defensive sort — the parser already supplies sorted
				// samples but a programmatic construction site might not.
				std::sort( samples.begin(), samples.end(),
					[]( const Sample& a, const Sample& b ) {
						return a.nm < b.nm;
					} );
			}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				const Scalar v = EvalAtNM( kRepresentativeNm );
				return ScalarTriple( v );
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar nm
				) const override
			{
				return EvalAtNM( nm );
			}

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
