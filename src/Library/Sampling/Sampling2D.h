//////////////////////////////////////////////////////////////////////
//
//  Sampling2D.h - Contains helper implementation for ISampling2D
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SAMPLING_2D_
#define SAMPLING_2D_

#include "../Interfaces/ISampling2D.h"

//
// 2D sampling interface helper
//
namespace RISE
{
	namespace Implementation
	{
		class Sampling2D : public virtual ISampling2D
		{
		protected:
			Scalar			dSpaceWidth;
			Scalar			dSpaceHeight;
			unsigned int	numSamples;

			Sampling2D( ){numSamples=0;}
			virtual ~Sampling2D( ){};

		public:	
			void SetNumSamples( const unsigned int num_samples ){ numSamples = num_samples; }
			unsigned int GetNumSamples(){ return numSamples; }
		};
	}
}

#endif
