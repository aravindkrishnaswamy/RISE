//////////////////////////////////////////////////////////////////////
//
//  HaltonPointsSampling2D.h - Creates a set of Halton points
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HALTONPOINTS_SAMPLING_2D_H
#define HALTONPOINTS_SAMPLING_2D_H

#include "Sampling2D.h"
#include "../Utilities/Reference.h"
#include "HaltonPoints.h"

namespace RISE
{
	namespace Implementation
	{
		class HaltonPointsSampling2D : 
			public virtual Sampling2D, 
			public virtual Reference
		{
		protected:
			~HaltonPointsSampling2D( );

			mutable MultiHalton mh;

		public:
			HaltonPointsSampling2D( Scalar dSpaceWidth_, Scalar dSpaceHeight_ );

			void GenerateSamplePoints( const RandomNumberGenerator&, SamplesList2D& samplePoints ) const;

			ISampling2D* Clone( ) const;
		};
	}
}

#endif
