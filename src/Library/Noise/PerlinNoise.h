//////////////////////////////////////////////////////////////////////
//
//  PerlinNoise.h - Defines perlin noise functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PERLIN_NOISE_
#define PERLIN_NOISE_

#include "../Interfaces/IFunction1D.h"
#include "../Interfaces/IFunction2D.h"
#include "InterpolatedNoise.h"

namespace RISE
{
	namespace Implementation
	{
		static const Scalar frequency_lut[32] = 
		{
			pow( 2.0, 0.0 ),
			pow( 2.0, 1.0 ),
			pow( 2.0, 2.0 ),
			pow( 2.0, 3.0 ),
			pow( 2.0, 4.0 ),
			pow( 2.0, 5.0 ),
			pow( 2.0, 6.0 ),
			pow( 2.0, 7.0 ),
			pow( 2.0, 8.0 ),
			pow( 2.0, 9.0 ),
			pow( 2.0, 10.0 ),
			pow( 2.0, 11.0 ),
			pow( 2.0, 12.0 ),
			pow( 2.0, 13.0 ),
			pow( 2.0, 14.0 ),
			pow( 2.0, 15.0 ),
			pow( 2.0, 16.0 ),
			pow( 2.0, 17.0 ),
			pow( 2.0, 18.0 ),
			pow( 2.0, 19.0 ),
			pow( 2.0, 20.0 ),	
			pow( 2.0, 21.0 ),
			pow( 2.0, 22.0 ),
			pow( 2.0, 23.0 ),
			pow( 2.0, 24.0 ),
			pow( 2.0, 25.0 ),
			pow( 2.0, 26.0 ),
			pow( 2.0, 27.0 ),
			pow( 2.0, 28.0 ),
			pow( 2.0, 29.0 ),
			pow( 2.0, 30.0 ),
			pow( 2.0, 31.0 )
		};

		class PerlinNoise1D : public virtual IFunction1D, public virtual Reference
		{
		protected:
			virtual ~PerlinNoise1D();

			InterpolatedNoise1D*		noise;
			Scalar						persistence;
			int							numOctaves;
			int							n;
			Scalar*						pAmplitudesLUT;

		public:
			PerlinNoise1D( const RealSimpleInterpolator& interp, const Scalar persistence_, const int numOctaves_ );

			virtual Scalar Evaluate( const Scalar variable ) const;
		};

		class PerlinNoise2D : public virtual IFunction2D, public virtual Reference
		{
		protected:
			virtual ~PerlinNoise2D();

			InterpolatedNoise2D*		noise;
			Scalar						persistence;
			int							numOctaves;
			int							n;
			Scalar*						pAmplitudesLUT;

		public:
			PerlinNoise2D( const RealSimpleInterpolator& interp, const Scalar persistence_, const int numOctaves_ );

			virtual Scalar Evaluate( const Scalar x, const Scalar y ) const;
		};

		class PerlinNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~PerlinNoise3D();

			InterpolatedNoise3D*		noise;
			Scalar						persistence;
			int							numOctaves;
			int							n;
			Scalar*						pAmplitudesLUT;

		public:
			PerlinNoise3D( const RealSimpleInterpolator& interp, const Scalar persistence_, const int numOctaves_ );

			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
