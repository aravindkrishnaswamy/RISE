//////////////////////////////////////////////////////////////////////
//
//  MajorantGrid.cpp - Implementation of the majorant grid with
//    3D-DDA ray traversal for null-scattering volume rendering
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 8, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MajorantGrid.h"
#include <math.h>

using namespace RISE;


MajorantGrid::MajorantGrid(
	IVolumeAccessor& accessor,
	unsigned int volWidth,
	unsigned int volHeight,
	unsigned int volDepth,
	const Point3& bboxMin,
	const Point3& bboxMax,
	const Scalar sigma_t_majorant,
	unsigned int gridResX,
	unsigned int gridResY,
	unsigned int gridResZ
	) :
  m_data( 0 ),
  m_gridX( gridResX ),
  m_gridY( gridResY ),
  m_gridZ( gridResZ ),
  m_bboxMin( bboxMin ),
  m_bboxExtent( Vector3Ops::mkVector3( bboxMax, bboxMin ) ),
  m_cellSize( m_bboxExtent.x / Scalar(gridResX),
              m_bboxExtent.y / Scalar(gridResY),
              m_bboxExtent.z / Scalar(gridResZ) ),
  m_invCellSize( Scalar(gridResX) / m_bboxExtent.x,
                 Scalar(gridResY) / m_bboxExtent.y,
                 Scalar(gridResZ) / m_bboxExtent.z )
{
	const unsigned int totalCells = m_gridX * m_gridY * m_gridZ;
	m_data = new Scalar[totalCells];

	// Initialize all cells to zero
	for( unsigned int i = 0; i < totalCells; i++ )
		m_data[i] = 0;

	// Iterate over ALL voxels in the volume and assign each to
	// its grid cell.  This approach is simpler and more robust
	// than mapping cell bounds to voxel ranges, because the
	// volume uses centered coordinates:
	//   vol_x = ((world_x - bboxMin.x) / extent.x - 0.5) * volWidth
	// Centered integer indices run from -volWidth/2 to volWidth/2-1.
	//
	// For each voxel, we:
	//   1. Compute its centered coordinate
	//   2. Map to normalized [0,1] within the AABB
	//   3. Determine which grid cell it falls in
	//   4. Update that cell's max density

	const int halfW = (int)volWidth / 2;
	const int halfH = (int)volHeight / 2;
	const int halfD = (int)volDepth / 2;

	for( int vz = -halfD; vz < (int)volDepth - halfD; vz++ )
	{
		// Normalized z: map centered coord back to [0,1]
		const Scalar nz = (Scalar(vz) / Scalar(volDepth)) + 0.5;
		const unsigned int cz = (unsigned int)fmin( nz * Scalar(m_gridZ), Scalar(m_gridZ - 1) );

		for( int vy = -halfH; vy < (int)volHeight - halfH; vy++ )
		{
			const Scalar ny = (Scalar(vy) / Scalar(volHeight)) + 0.5;
			const unsigned int cy = (unsigned int)fmin( ny * Scalar(m_gridY), Scalar(m_gridY - 1) );

			for( int vx = -halfW; vx < (int)volWidth - halfW; vx++ )
			{
				const Scalar nx = (Scalar(vx) / Scalar(volWidth)) + 0.5;
				const unsigned int cx = (unsigned int)fmin( nx * Scalar(m_gridX), Scalar(m_gridX - 1) );

				// Query density at this voxel (centered coordinates)
				const Scalar d = accessor.GetValue( vx, vy, vz );
				const unsigned int cellIdx = CellIndex( cx, cy, cz );
				const Scalar cellMajorant = d * sigma_t_majorant;
				if( cellMajorant > m_data[cellIdx] )
				{
					m_data[cellIdx] = cellMajorant;
				}
			}
		}
	}

	// Dilation passes: account for interpolation stencil overlap.
	//
	// Trilinear interpolation uses a 2^3 stencil centered on the
	// query point, so a lookup near a cell boundary can depend on
	// voxels from the adjacent cell.  Tricubic (Catmull-Rom) uses a
	// 4^3 stencil that can reach 2 cells away from the query point,
	// AND the non-convex basis can overshoot local extrema.
	//
	// Two dilation passes (2-cell radius) ensure each cell's majorant
	// covers all voxels that can contribute to any interpolation query
	// within that cell, for both trilinear and tricubic accessors.
	//
	// The Catmull-Rom basis functions at mu=0.5 are [-1/16, 9/16,
	// 9/16, -1/16].  Let P = 9/8 (sum of positive weights) and
	// N = 1/8 (sum of absolute negative weights).  In the 3D
	// tensor product, a coefficient w_i*w_j*w_k is positive when
	// an even number of factors are negative (0 or 2).  The exact
	// worst-case amplification with voxels in [0,M] is the sum
	// of all positive 3D coefficients:
	//   P^3 + 3*P*N^2 = (9/8)^3 + 3*(9/8)*(1/8)^2
	//                  = 729/512 + 27/512 = 756/512 ~= 1.4766
	// achieved by setting voxels at positive-coefficient positions
	// to M and the rest to 0.
	//
	// The cost of two passes is negligible (one-time grid build).
	{
		const unsigned int nCells = m_gridX * m_gridY * m_gridZ;

		for( unsigned int pass = 0; pass < 2; pass++ )
		{
			Scalar* dilated = new Scalar[nCells];
			for( unsigned int i = 0; i < nCells; i++ )
				dilated[i] = m_data[i];

			for( unsigned int z = 0; z < m_gridZ; z++ )
			{
				for( unsigned int y = 0; y < m_gridY; y++ )
				{
					for( unsigned int x = 0; x < m_gridX; x++ )
					{
						Scalar maxVal = m_data[CellIndex( x, y, z )];

						// Check 26-neighborhood
						for( int dz = -1; dz <= 1; dz++ )
						{
							const int nz2 = (int)z + dz;
							if( nz2 < 0 || nz2 >= (int)m_gridZ ) continue;
							for( int dy = -1; dy <= 1; dy++ )
							{
								const int ny2 = (int)y + dy;
								if( ny2 < 0 || ny2 >= (int)m_gridY ) continue;
								for( int dx = -1; dx <= 1; dx++ )
								{
									const int nx2 = (int)x + dx;
									if( nx2 < 0 || nx2 >= (int)m_gridX ) continue;
									const Scalar neighborVal = m_data[CellIndex(
										(unsigned int)nx2, (unsigned int)ny2, (unsigned int)nz2 )];
									if( neighborVal > maxVal )
										maxVal = neighborVal;
								}
							}
						}

						dilated[CellIndex( x, y, z )] = maxVal;
					}
				}
			}

			delete[] m_data;
			m_data = dilated;
		}

		// Safety factor for Catmull-Rom tricubic overshoot:
		// P^3 + 3*P*N^2 = 756/512 ~= 1.4766 (see derivation above).
		const Scalar kTricubicOvershootFactor = 756.0 / 512.0;
		for( unsigned int i = 0; i < nCells; i++ )
			m_data[i] *= kTricubicOvershootFactor;
	}
}


MajorantGrid::~MajorantGrid()
{
	delete[] m_data;
	m_data = 0;
}


bool MajorantGrid::IntersectBBox(
	const Ray& ray,
	Scalar& tEntry,
	Scalar& tExit
	) const
{
	// Slab method for ray-AABB intersection
	const Point3 bboxMax(
		m_bboxMin.x + m_bboxExtent.x,
		m_bboxMin.y + m_bboxExtent.y,
		m_bboxMin.z + m_bboxExtent.z );

	Scalar tMin = -RISE_INFINITY;
	Scalar tMax = RISE_INFINITY;

	// X slab
	if( fabs( ray.Dir().x ) > 1e-20 )
	{
		const Scalar invD = 1.0 / ray.Dir().x;
		Scalar t0 = (m_bboxMin.x - ray.origin.x) * invD;
		Scalar t1 = (bboxMax.x - ray.origin.x) * invD;
		if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
		tMin = fmax( tMin, t0 );
		tMax = fmin( tMax, t1 );
		if( tMin > tMax ) return false;
	}
	else
	{
		if( ray.origin.x < m_bboxMin.x || ray.origin.x > bboxMax.x )
			return false;
	}

	// Y slab
	if( fabs( ray.Dir().y ) > 1e-20 )
	{
		const Scalar invD = 1.0 / ray.Dir().y;
		Scalar t0 = (m_bboxMin.y - ray.origin.y) * invD;
		Scalar t1 = (bboxMax.y - ray.origin.y) * invD;
		if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
		tMin = fmax( tMin, t0 );
		tMax = fmin( tMax, t1 );
		if( tMin > tMax ) return false;
	}
	else
	{
		if( ray.origin.y < m_bboxMin.y || ray.origin.y > bboxMax.y )
			return false;
	}

	// Z slab
	if( fabs( ray.Dir().z ) > 1e-20 )
	{
		const Scalar invD = 1.0 / ray.Dir().z;
		Scalar t0 = (m_bboxMin.z - ray.origin.z) * invD;
		Scalar t1 = (bboxMax.z - ray.origin.z) * invD;
		if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
		tMin = fmax( tMin, t0 );
		tMax = fmin( tMax, t1 );
		if( tMin > tMax ) return false;
	}
	else
	{
		if( ray.origin.z < m_bboxMin.z || ray.origin.z > bboxMax.z )
			return false;
	}

	// Clamp entry to 0 (ray origin inside box)
	tEntry = fmax( tMin, 0.0 );
	tExit = tMax;

	return tEntry < tExit;
}
