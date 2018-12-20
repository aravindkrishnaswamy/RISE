//////////////////////////////////////////////////////////////////////
//
//  VolumeAccessor_TRI.h - Trilinear interpolator for volumes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef _VolumeAccessor_TRI
#define _VolumeAccessor_TRI

#include "VolumeAccessorHelper.h"

#include <math.h>

namespace RISE
{
	class VolumeAccessor_TRI :
		public virtual VolumeAccessorHelper
	{
	protected:
		virtual ~VolumeAccessor_TRI( ){}

	public:
		VolumeAccessor_TRI( ){};

		Scalar GetValue( Scalar x, Scalar y, Scalar z )const 
		{
			Scalar ulo, vlo, wlo;

			// trilinear interpolation
			// Extract the integer and decimal components of the value at the three axis
			Scalar	ut = modf( x, &ulo );
			Scalar	vt = modf( y, &vlo );
			Scalar	wt = modf( z, &wlo );

			int	xlo = int(ulo);
			int	xhi = xlo+1;
			int	ylo = int(vlo);
			int	yhi = ylo+1;
			int	zlo = int(wlo);
			int	zhi = zlo+1;

			// Thus our final value is computed from these low and high values
			Scalar	llA = pVolume->GetValue( xlo, ylo, zlo );
			Scalar	lhA = pVolume->GetValue( xhi, ylo, zlo );
			Scalar	hlA = pVolume->GetValue( xlo, yhi, zlo );
			Scalar	hhA = pVolume->GetValue( xhi, yhi, zlo );

			Scalar	llB = pVolume->GetValue( xlo,  ylo, zhi );
			Scalar	lhB = pVolume->GetValue( xhi, ylo, zhi );
			Scalar	hlB = pVolume->GetValue( xlo, yhi, zhi );
			Scalar	hhB = pVolume->GetValue( xhi, yhi, zhi );
			
			Scalar	omut = 1.0 - ut;
			Scalar	omvt = 1.0 - vt;
			Scalar	omwt = 1.0 - wt;

			// And the final voxel value just becomes a linear combination of these values...
			Scalar front = (llA * (omut * omvt) 
				+ hlA * (omut * vt)
				+ lhA * (ut * omvt) 
				+ hhA * (ut * vt)); 

			Scalar back = (llB * (omut * omvt) 
				+ hlB * (omut * vt)
				+ lhB * (ut * omvt) 
				+ hhB * (ut * vt));

			return (front * wt + back * omwt);
		}

		Scalar GetValue( int x, int y, int z )const 
		{
			return GetValue( (Scalar)x, (Scalar)y, (Scalar)z );
		}
	};
}

#endif

