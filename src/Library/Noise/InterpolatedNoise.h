//////////////////////////////////////////////////////////////////////
//
//  InterpolatedNoise.h - Contains 1D and 2D interpolated noise
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments:  The noise functions here were taken from a Perlin Noise
//  tutorial, available here:
//  http://freespace.virgin.net/hugo.elias/models/m_perlin.htm
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef INTERPOLATED_NOISE_
#define INTERPOLATED_NOISE_

#include "SmoothedNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Reference.h"
#include <math.h>

namespace RISE
{
	namespace Implementation
	{
		class InterpolatedNoise1D : public virtual IFunction1D, public virtual Reference
		{
		protected:
			SmoothedNoise1D*				SmoothedNoise1;
			const RealSimpleInterpolator&	interp;

		public:
			InterpolatedNoise1D( const RealSimpleInterpolator& interp_ );
			virtual ~InterpolatedNoise1D();
			virtual Scalar Evaluate( const Scalar x ) const;
		};

		class InterpolatedNoise2D : public virtual IFunction2D, public virtual Reference
		{
		protected:
			SmoothedNoise2D*				SmoothedNoise2;
			const RealSimpleInterpolator&	interp;

			virtual ~InterpolatedNoise2D();

		public:
			InterpolatedNoise2D( const RealSimpleInterpolator& interp_ );
			virtual Scalar Evaluate( const Scalar x, const Scalar y ) const;
		};

		class InterpolatedNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			SmoothedNoise3D*				SmoothedNoise3;
			const RealSimpleInterpolator&	interp;

			virtual ~InterpolatedNoise3D();

		public:
			InterpolatedNoise3D( const RealSimpleInterpolator& interp_ );
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
