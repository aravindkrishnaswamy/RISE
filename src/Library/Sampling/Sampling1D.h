//////////////////////////////////////////////////////////////////////
//
//  Sampling1D.h - Contains helper implementation for ISampling1D
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 23, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SAMPLING_1D_
#define SAMPLING_1D_

#include "../Interfaces/ISampling1D.h"

namespace RISE
{
	//
	// 1D sampling implementation helper
	//
	namespace Implementation
	{
		class Sampling1D : 
			public virtual ISampling1D
		{
		protected:
			Scalar			dSpace;
			unsigned int	numSamples;

			Sampling1D( ){numSamples=0;}
			virtual ~Sampling1D( ){};

		public:	
			void SetNumSamples( const unsigned int num_samples ){ numSamples = num_samples; }
			unsigned int GetNumSamples(){ return numSamples; }
		};
	}
}

#endif
