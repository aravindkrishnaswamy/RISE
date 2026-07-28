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

#include <math.h>

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
			// floor (not int-cast truncation) so negative coordinates map to
			// the cell that contains them -- heterogeneous volumes address
			// with centered, signed coordinates, and int(-0.5) would land in
			// voxel 0 instead of voxel -1.
			//
			// Note this is containing-cell (floor) selection, matching the
			// TRI/TriCubic cell convention and the HeterogeneousMedium DDA --
			// NOT round-to-nearest-knot (that would be floor(x + 0.5) and
			// would shift results half a voxel against the other accessors).
			return pVolume->GetValue( int(floor(x)), int(floor(y)), int(floor(z)) );
		}

		Scalar GetValue( int x, int y, int z )const 
		{
			return pVolume->GetValue( x, y, z );
		}
	};
}

#endif

