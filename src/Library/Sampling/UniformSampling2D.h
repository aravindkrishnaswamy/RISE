//////////////////////////////////////////////////////////////////////
//
//  UniformSampling2D.h - Samples along a uniform grid
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 30, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef UNIFORMSAMPLING_2D_
#define UNIFORMSAMPLING_2D_

#include "Sampling2D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Generates samples along a uniformly spaced grid
	//
	namespace Implementation
	{
		class UniformSampling2D : public virtual Sampling2D, public virtual Reference
		{
		protected:
			virtual ~UniformSampling2D( );

		public:
			UniformSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight );

			virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const;

			virtual ISampling2D* Clone( ) const;
		};
	}
}

#endif
