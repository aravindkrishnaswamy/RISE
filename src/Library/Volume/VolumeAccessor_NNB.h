//////////////////////////////////////////////////////////////////////
//
//  VolumeAccessor_NNB.h - Nearest neighbour interpolator for volumes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	

#ifndef _VolumeAccessor_NNB
#define _VolumeAccessor_NNB

#include "VolumeAccessorHelper.h"

namespace RISE
{
	class VolumeAccessor_NNB : 
		public virtual VolumeAccessorHelper
	{
	protected:
		virtual ~VolumeAccessor_NNB( ){}

	public:
		VolumeAccessor_NNB( ){};

		Scalar GetValue( Scalar x, Scalar y, Scalar z ) const 
		{
			return pVolume->GetValue( int(x), int(y), int(z) );
		}

		Scalar GetValue( int x, int y, int z )const 
		{
			return pVolume->GetValue( x, y, z );
		}
	};
}

#endif

