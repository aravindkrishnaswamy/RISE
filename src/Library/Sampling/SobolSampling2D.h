//////////////////////////////////////////////////////////////////////
//
//  SobolSampling2D.h - Owen-scrambled Sobol (0,2)-net for 2D pixel
//    sample positions.
//
//    Generates numSamples 2D points in [0, dSpaceWidth] x
//    [0, dSpaceHeight] using Sobol dimensions 0 and 1 with
//    Owen scrambling for randomization.  The scramble seed is
//    derived from the RandomNumberGenerator so that different
//    pixels get different scrambled sequences.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 27, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SOBOL_SAMPLING_2D_H
#define SOBOL_SAMPLING_2D_H

#include "Sampling2D.h"
#include "../Utilities/Reference.h"
#include "SobolSequence.h"

namespace RISE
{
	namespace Implementation
	{
		class SobolSampling2D :
			public virtual Sampling2D,
			public virtual Reference
		{
		protected:
			virtual ~SobolSampling2D();

		public:
			SobolSampling2D( Scalar dSpaceWidth_, Scalar dSpaceHeight_ );

			void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const;

			ISampling2D* Clone() const;
		};
	}
}

#endif
