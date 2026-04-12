//////////////////////////////////////////////////////////////////////
//
//  SampledWavelengths.h - Hero Wavelength Spectral Sampling (HWSS)
//    wavelength bundle.
//
//  Implements the equidistant spectral stratification scheme from
//  Wilkie et al., "Hero Wavelength Spectral Sampling" (EGSR 2014).
//
//  A SampledWavelengths object carries 4 wavelengths per path:
//    - lambda[0]: the hero wavelength, sampled uniformly from
//      [lambda_min, lambda_max].  This wavelength drives all
//      directional decisions (BSDF sampling, NEE direction, light
//      selection).
//    - lambda[1..3]: companion wavelengths placed at equidistant
//      spectral offsets with wrap-around:
//        lambda_i = lambda_min + fmod(lambda_hero - lambda_min
//                   + i * Delta, lambda_max - lambda_min)
//      where Delta = (lambda_max - lambda_min) / 4.
//
//  The equidistant placement ensures that the 4 wavelengths form
//  a stratified sample of the visible spectrum regardless of the
//  hero's position.  The uniform PDF for each wavelength is
//  1 / (lambda_max - lambda_min).
//
//  Secondary wavelength termination:
//  At specular dispersive interfaces (wavelength-dependent IOR with
//  delta distribution), companion wavelengths are terminated because
//  they would follow different geometric paths (Snell's law depends
//  on IOR which depends on wavelength).  After termination, only the
//  hero survives and the path degenerates to single-wavelength
//  transport for the dispersive segment.  For rough dispersive BSDFs,
//  MIS over wavelengths remains valid and companions are not
//  terminated.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SAMPLED_WAVELENGTHS_
#define SAMPLED_WAVELENGTHS_

#include <cmath>
#include "../Math3D/Math3D.h"

namespace RISE
{
	/// Carries 4 stratified wavelengths per path for Hero Wavelength
	/// Spectral Sampling (HWSS).  Value type — copy/assign freely.
	struct SampledWavelengths
	{
		static const unsigned int N = 4;

		Scalar		lambda[N];			///< Wavelengths in nm.  Index 0 is the hero.
		Scalar		pdf[N];				///< Sampling PDF for each wavelength (uniform).
		bool		terminated[N];		///< True if this wavelength has been terminated.

		SampledWavelengths()
		{
			for( unsigned int i = 0; i < N; i++ )
			{
				lambda[i] = 0;
				pdf[i] = 0;
				terminated[i] = false;
			}
		}

		/// Sample 4 equidistant wavelengths from [lambda_min, lambda_max].
		///
		/// @param u        Uniform random number in [0, 1) for hero placement.
		/// @param lambda_min  Start of wavelength range (nm), e.g. 380.
		/// @param lambda_max  End of wavelength range (nm), e.g. 780.
		/// @return A SampledWavelengths with hero at index 0 and 3 companions
		///         at equidistant spectral offsets with wrap-around.
		static SampledWavelengths SampleEquidistant(
			const Scalar u,
			const Scalar lambda_min,
			const Scalar lambda_max
			)
		{
			SampledWavelengths swl;

			const Scalar range = lambda_max - lambda_min;
			const Scalar delta = range / static_cast<Scalar>( N );
			const Scalar invRange = 1.0 / range;

			// Hero wavelength: uniform sample within the range
			swl.lambda[0] = lambda_min + u * range;
			swl.pdf[0] = invRange;
			swl.terminated[0] = false;

			// Companion wavelengths at equidistant offsets with wrap-around
			for( unsigned int i = 1; i < N; i++ )
			{
				Scalar offset = swl.lambda[0] - lambda_min + static_cast<Scalar>( i ) * delta;
				swl.lambda[i] = lambda_min + std::fmod( offset, range );
				swl.pdf[i] = invRange;
				swl.terminated[i] = false;
			}

			return swl;
		}

		/// Returns the hero wavelength (index 0).  The hero is never terminated.
		inline Scalar HeroLambda() const
		{
			return lambda[0];
		}

		/// Terminate all companion wavelengths (indices 1..N-1).
		/// Called at specular dispersive interfaces where companions
		/// would follow different geometric paths.
		/// The hero (index 0) is never terminated.
		inline void TerminateSecondary()
		{
			for( unsigned int i = 1; i < N; i++ )
			{
				terminated[i] = true;
			}
		}

		/// Returns true if all companion wavelengths have been terminated,
		/// leaving only the hero active.
		inline bool SecondaryTerminated() const
		{
			for( unsigned int i = 1; i < N; i++ )
			{
				if( !terminated[i] )
				{
					return false;
				}
			}
			return true;
		}

		/// Returns the number of active (non-terminated) wavelengths.
		inline unsigned int NumActive() const
		{
			unsigned int count = 0;
			for( unsigned int i = 0; i < N; i++ )
			{
				if( !terminated[i] )
				{
					count++;
				}
			}
			return count;
		}
	};
}

#endif
