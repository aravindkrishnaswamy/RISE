//////////////////////////////////////////////////////////////////////
//
//  PoissonDiskSampling2D.h - ensures that no sample is too close
//  together
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


#ifndef POISSON_DISK_SAMPLING_2D_
#define POISSON_DISK_SAMPLING_2D_

#include "Sampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Generates random samples but with the guarantee that no sample is 
	// too close together
	//
	namespace Implementation
	{
		class PoissonDiskSampling2D : public virtual Sampling2D, public virtual Reference
		{
		protected:
			Scalar						dMinimumSeperation;
			virtual ~PoissonDiskSampling2D( );

		public:
			PoissonDiskSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight, Scalar _dSep );

			bool TooClose( SamplesList2D& samplePoints, const Point2& v ) const;
			virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const;

			virtual ISampling2D* Clone( ) const;
		};
	}
}

#endif
