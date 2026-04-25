#ifndef PROCEDURAL_GRID_TEST_HELPERS_H
#define PROCEDURAL_GRID_TEST_HELPERS_H

#include "ProceduralTestHelpers.h"

namespace ProceduralGridTestHelpers
{
	template< typename EvalA, typename EvalB >
	inline bool AllCloseOnGrid3D(
		const int nx,
		const int ny,
		const int nz,
		const RISE::Scalar stepX,
		const RISE::Scalar stepY,
		const RISE::Scalar stepZ,
		const EvalA& evalA,
		const EvalB& evalB,
		const double tol = 1e-6 )
	{
		for( int ix = 0; ix < nx; ix++ ) {
			for( int iy = 0; iy < ny; iy++ ) {
				for( int iz = 0; iz < nz; iz++ ) {
					const RISE::Scalar x = ix * stepX;
					const RISE::Scalar y = iy * stepY;
					const RISE::Scalar z = iz * stepZ;
					if( !ProceduralTestHelpers::IsClose( evalA( x, y, z ), evalB( x, y, z ), tol ) ) {
						return false;
					}
				}
			}
		}
		return true;
	}

	template< typename EvalA, typename EvalB >
	inline int CountDifferentOnGrid3D(
		const int nx,
		const int ny,
		const int nz,
		const RISE::Scalar stepX,
		const RISE::Scalar stepY,
		const RISE::Scalar stepZ,
		const EvalA& evalA,
		const EvalB& evalB,
		const double tol = 1e-6 )
	{
		int differCount = 0;
		for( int ix = 0; ix < nx; ix++ ) {
			for( int iy = 0; iy < ny; iy++ ) {
				for( int iz = 0; iz < nz; iz++ ) {
					const RISE::Scalar x = ix * stepX;
					const RISE::Scalar y = iy * stepY;
					const RISE::Scalar z = iz * stepZ;
					if( !ProceduralTestHelpers::IsClose( evalA( x, y, z ), evalB( x, y, z ), tol ) ) {
						differCount++;
					}
				}
			}
		}
		return differCount;
	}
}

#endif
