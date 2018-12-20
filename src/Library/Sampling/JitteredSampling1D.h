//////////////////////////////////////////////////////////////////////
//
//  JitteredSampling1D.h - Samples along a uniform line
//                
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 28, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef JITTEREDSAMPLING_1D_
#define JITTEREDSAMPLING_1D_

#include "Sampling1D.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RandomNumbers.h"

namespace RISE
{
	//
	// Generates samples along a uniformly spaced grid
	//
	namespace Implementation
	{
		class JitteredSampling1D : 
			public virtual Sampling1D, 
			public virtual Reference
		{
		protected:
			virtual ~JitteredSampling1D( );

		public:
			JitteredSampling1D( Scalar dSpace_ );
			virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList1D& samplePoints ) const;
			virtual ISampling1D* Clone( ) const;
		};
	}
}

#endif
