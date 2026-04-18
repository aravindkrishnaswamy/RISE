//////////////////////////////////////////////////////////////////////
//
//  GerstnerWavePainter.h - Sum-of-sines water-wave painter.  A bank
//  of N sine waves derived from wind parameters and a deterministic
//  seed; Evaluate(u,v) returns the sum of per-wave heights at the
//  given UV and current `time`.  Intended to drive a DisplacedGeometry
//  via the IFunction2D hook.
//
//  Scalar-only (height) variant.  Full Gerstner/trochoidal waves also
//  move vertices horizontally in the direction of travel to sharpen
//  peaks and flatten troughs — that requires a vector-valued
//  displacement, which DisplacedGeometry does not yet support.  Until
//  it does, peak sharpening is approximated by the amplitude spectrum
//  (wider wavelength_range + amplitude_power > 1 gives steeper crests).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GERSTNER_WAVE_PAINTER_
#define GERSTNER_WAVE_PAINTER_

#include "Painter.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class GerstnerWavePainter : public Painter
		{
		protected:
			virtual ~GerstnerWavePainter();

			const IPainter&		a;
			const IPainter&		b;

			struct Wave
			{
				Scalar	amplitude;
				Scalar	frequency;		// k = 2*pi / wavelength
				Scalar	angularSpeed;	// omega = dispersion_speed * sqrt(g*k)
				Scalar	dirX;
				Scalar	dirY;
				Scalar	phase;			// initial phase [0, 2*pi)
			};

			// Factory parameters (retained for RegenerateData after seed changes).
			unsigned int	numWaves;
			Scalar			medianWavelength;
			Scalar			wavelengthRange;	// multiplicative: span is [median/range, median*range]
			Scalar			medianAmplitude;
			Scalar			amplitudePower;		// A_i = medianAmplitude * (lambda_i/medianWavelength)^power
			Scalar			windDirX;
			Scalar			windDirY;
			Scalar			directionalSpread;	// radians; per-wave direction jitters in [-spread, +spread]
			Scalar			dispersionSpeed;	// multiplies sqrt(g*k); lets users tune motion without touching `time`
			unsigned int	seed;

			// Derived state (built by GenerateWaves()).
			std::vector<Wave>	waves;
			Scalar				totalAmplitude;		// Sum of |A_i| — used to normalize height for color interp.

			Scalar			m_time;					// keyframeable

			void GenerateWaves();

		public:
			GerstnerWavePainter(
				const IPainter&		a_,
				const IPainter&		b_,
				const unsigned int	numWaves_,
				const Scalar		medianWavelength_,
				const Scalar		wavelengthRange_,
				const Scalar		medianAmplitude_,
				const Scalar		amplitudePower_,
				const Scalar		windDirX_,
				const Scalar		windDirY_,
				const Scalar		directionalSpread_,
				const Scalar		dispersionSpeed_,
				const unsigned int	seed_,
				const Scalar		time_ );

			GerstnerWavePainter( const GerstnerWavePainter& ) = delete;
			GerstnerWavePainter& operator=( const GerstnerWavePainter& ) = delete;

			RISEPel GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar  GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// IFunction2D — the hook DisplacedGeometry calls per vertex.
			Scalar  Evaluate( const Scalar x, const Scalar y ) const;

			// Keyframable — `time` is the only animated parameter in v1.
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData();
		};
	}
}

#endif
