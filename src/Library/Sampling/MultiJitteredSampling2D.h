//////////////////////////////////////////////////////////////////////
//
//  MultiJitteredSampling2D.h - Type of Latin Hypercubes sampling
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef MULTIJITTERED_SAMPLING_2D_
#define MULTIJITTERED_SAMPLING_2D_

#include "Sampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Generates multi jittered samples using the algorithm described
	//   by Chu et al. in Graphics Gems IV, page 373
	//
	namespace Implementation
	{
		class MultiJitteredSampling2D : public virtual Sampling2D, public virtual Reference
		{
		protected:
			virtual ~MultiJitteredSampling2D( );

		public:
			MultiJitteredSampling2D( Scalar dSpaceWidth_, Scalar dSpaceHeight_ );

			virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const;

			virtual ISampling2D* Clone( ) const;
		};
	}
}

#endif
