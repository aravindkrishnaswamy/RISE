//////////////////////////////////////////////////////////////////////
//
//  VolumeAccessor_TriCubic.h - Tricubic interpolator for volumes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 20, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef _VolumeAccessor_TriCubic
#define _VolumeAccessor_TriCubic

#include "../Interfaces/ICubicInterpolator.h"
#include "VolumeAccessorHelper.h"

#include <math.h>

namespace RISE
{
	class VolumeAccessor_TriCubic :
		public virtual VolumeAccessorHelper
	{
	protected:
		const ICubicInterpolator<Scalar>& interp;

		virtual ~VolumeAccessor_TriCubic( )
		{
			interp.release();
		}

	public:
		VolumeAccessor_TriCubic( const ICubicInterpolator<Scalar>& interp_ ) : 
		interp( interp_ )
		{
			interp.addref();
		};

		Scalar GetValue( Scalar x, Scalar y, Scalar z )const 
		{
				// Extract the integer and decimal components of the x, y co-ordinates
				double ulo, vlo, wlo;
				const double ut = modf( x, &ulo );
				const double vt = modf( y, &vlo );
				const double wt = modf( z, &wlo );

				int		xlo = int( ulo );
				int		ylo = int( vlo );
				int		zlo = int( wlo );

				// We need all the voxels around the primary
				Scalar voxels[4][4][4];
				{
					for( int z=0; z<4; z++ ) {
						for( int y=0; y<4; y++ ) {
							for( int x=0; x<4; x++ ) {
								int px = (xlo-1+x);
								int py = (ylo-1+y);
								int pz = (zlo-1+z);
								voxels[z][y][x] =  pVolume->GetValue( px, py, pz );
							}
						}
					}
				}

				// Now that we have all our voxels, run the cubic interpolator in one dimension to collapse it (we choose to collapase x)
				Scalar voxelcol[4][4];
				{
					for( int z=0; z<4; z++ ) {
						for( int y=0; y<4; y++ ) {
							voxelcol[z][y] = interp.InterpolateValues( voxels[z][y][0], voxels[z][y][1], voxels[z][y][2], voxels[z][y][3], ut );
						}
					}
				}

				// Then collapse the y dimension 
				Scalar voxelcol2[4];
				{
					for( int z=0; z<4; z++ ) {
						voxelcol2[z] = interp.InterpolateValues( voxelcol[z][0], voxelcol[z][1], voxelcol[z][2], voxelcol[z][3], vt );
					}
				}

				// The collapse the z dimension to get our value
				return interp.InterpolateValues( voxelcol2[0], voxelcol2[1], voxelcol2[2], voxelcol2[3], wt );
		}

		Scalar GetValue( int x, int y, int z )const 
		{
			return GetValue( (Scalar)x, (Scalar)y, (Scalar)z );
		}
	};
}

#endif

