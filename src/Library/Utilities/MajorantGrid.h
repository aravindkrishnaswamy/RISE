//////////////////////////////////////////////////////////////////////
//
//  MajorantGrid.h - Low-resolution 3D grid of per-cell majorant
//    extinction values with Amanatides-Woo 3D-DDA ray traversal
//
//  For heterogeneous media, a single global majorant leads to
//  excessive null collisions in low-density regions.  This grid
//  decomposes the volume bounding box into cells, each storing the
//  maximum possible extinction within that cell.  Delta tracking
//  and ratio tracking then use the local cell majorant instead of
//  the global one, dramatically reducing null collisions.
//
//  The TraverseRay method implements Amanatides & Woo's 3D-DDA
//  algorithm (Eurographics 1987) and calls a visitor functor for
//  each cell along the ray.  The visitor pattern allows the same
//  traversal to serve both delta tracking (SampleDistance) and
//  ratio tracking (EvalTransmittance) without duplicating DDA code.
//
//  Grid resolution is derived from the volume dimensions:
//    gridRes = clamp( ceil(volDim / 8), 4, 32 ) per axis
//  For a 64^3 volume this gives an 8^3 grid (512 cells), each
//  covering 8^3 voxels.
//
//  References:
//    - Amanatides, Woo, "A Fast Voxel Traversal Algorithm for
//      Ray Tracing", Eurographics 1987
//    - Miller, Georgiev, Jarosz, "A Null-Scattering Path Integral
//      Formulation of Light Transport", SIGGRAPH 2019
//    - Fong et al., "Production Volume Rendering", SIGGRAPH 2017
//      Course Notes (majorant grid strategy)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 8, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MAJORANT_GRID_
#define MAJORANT_GRID_

#include "Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Ray.h"
#include "../Interfaces/IVolumeAccessor.h"

namespace RISE
{
	/// \brief Low-resolution grid of per-cell majorant extinction values
	///
	/// Each cell stores the maximum extinction (sigma_t * density) that
	/// can occur anywhere within that cell.  This enables delta tracking
	/// and ratio tracking to use tight local majorants instead of a
	/// single global majorant for the entire volume.
	class MajorantGrid
	{
	protected:
		Scalar* m_data;						///< Flat array of per-cell majorants [gridX * gridY * gridZ]
		unsigned int m_gridX;				///< Grid resolution along X
		unsigned int m_gridY;				///< Grid resolution along Y
		unsigned int m_gridZ;				///< Grid resolution along Z
		Point3 m_bboxMin;					///< World-space AABB minimum corner
		Vector3 m_bboxExtent;				///< World-space AABB extent (max - min)
		Vector3 m_cellSize;					///< World-space size of each cell
		Vector3 m_invCellSize;				///< 1.0 / cellSize (for fast coordinate conversion)

		/// Flatten 3D cell index to 1D array index
		unsigned int CellIndex(
			const unsigned int cx,
			const unsigned int cy,
			const unsigned int cz
			) const
		{
			return cz * m_gridX * m_gridY + cy * m_gridX + cx;
		}

	public:
		/// Build the majorant grid from a volume accessor.
		///
		/// Iterates through all voxels in each cell to find the maximum
		/// density, then scales by MaxValue(max_sigma_t) to produce the
		/// majorant extinction for that cell.
		MajorantGrid(
			IVolumeAccessor& accessor,			///< [in] Density field accessor
			unsigned int volWidth,				///< [in] Volume width in voxels
			unsigned int volHeight,				///< [in] Volume height in voxels
			unsigned int volDepth,				///< [in] Volume depth in voxels
			const Point3& bboxMin,				///< [in] World-space AABB minimum
			const Point3& bboxMax,				///< [in] World-space AABB maximum
			const Scalar sigma_t_majorant,		///< [in] Global majorant (MaxValue(max_sigma_t))
			unsigned int gridResX,				///< [in] Grid resolution X
			unsigned int gridResY,				///< [in] Grid resolution Y
			unsigned int gridResZ				///< [in] Grid resolution Z
			);

		~MajorantGrid();

		/// Get majorant for a specific cell
		Scalar GetCellMajorant(
			unsigned int cx,
			unsigned int cy,
			unsigned int cz
			) const
		{
			return m_data[CellIndex( cx, cy, cz )];
		}

		/// Grid dimensions
		unsigned int GetGridX() const { return m_gridX; }
		unsigned int GetGridY() const { return m_gridY; }
		unsigned int GetGridZ() const { return m_gridZ; }

		/// Cell size
		const Vector3& GetCellSize() const { return m_cellSize; }

		/// Bounding box
		const Point3& GetBBoxMin() const { return m_bboxMin; }
		const Vector3& GetBBoxExtent() const { return m_bboxExtent; }

		/// Get the cell indices containing a world-space point.
		/// Returns false if the point is outside the grid.
		bool WorldToCell(
			const Point3& worldPt,
			unsigned int& cx,
			unsigned int& cy,
			unsigned int& cz
			) const
		{
			const Scalar nx = (worldPt.x - m_bboxMin.x) * m_invCellSize.x;
			const Scalar ny = (worldPt.y - m_bboxMin.y) * m_invCellSize.y;
			const Scalar nz = (worldPt.z - m_bboxMin.z) * m_invCellSize.z;

			if( nx < 0 || ny < 0 || nz < 0 ) return false;

			cx = (unsigned int)nx;
			cy = (unsigned int)ny;
			cz = (unsigned int)nz;

			if( cx >= m_gridX ) cx = m_gridX - 1;
			if( cy >= m_gridY ) cy = m_gridY - 1;
			if( cz >= m_gridZ ) cz = m_gridZ - 1;

			return true;
		}

		/// Traverse a ray through the grid using 3D-DDA (Amanatides-Woo).
		///
		/// For each cell the ray passes through, calls:
		///   visitor( tCellEntry, tCellExit, cellMajorant )
		/// where distances are along the ray from its origin.
		///
		/// The visitor should return true to continue traversal, or
		/// false to stop early (e.g., when a scatter event is accepted).
		///
		/// \return true if the ray intersected the grid, false otherwise
		template<typename Visitor>
		bool TraverseRay(
			const Ray& ray,
			Scalar tStart,
			Scalar tEnd,
			Visitor& visitor
			) const
		{
			// Intersect ray with grid AABB
			Scalar tEntry, tExit;
			if( !IntersectBBox( ray, tEntry, tExit ) )
				return false;

			// Clamp to caller's range
			tEntry = fmax( tEntry, tStart );
			tExit = fmin( tExit, tEnd );
			if( tEntry >= tExit )
				return false;

			// Entry point in grid-local coordinates [0, gridDim]
			const Point3 entryPt = Point3Ops::mkPoint3( ray.origin, ray.Dir() * tEntry );
			Scalar fx = (entryPt.x - m_bboxMin.x) * m_invCellSize.x;
			Scalar fy = (entryPt.y - m_bboxMin.y) * m_invCellSize.y;
			Scalar fz = (entryPt.z - m_bboxMin.z) * m_invCellSize.z;

			// Clamp to valid cell range
			fx = fmax( 0.0, fmin( fx, Scalar(m_gridX) - 1e-6 ) );
			fy = fmax( 0.0, fmin( fy, Scalar(m_gridY) - 1e-6 ) );
			fz = fmax( 0.0, fmin( fz, Scalar(m_gridZ) - 1e-6 ) );

			int cx = (int)fx;
			int cy = (int)fy;
			int cz = (int)fz;

			// DDA step directions and increments
			const Vector3& d = ray.Dir();

			const int stepX = (d.x >= 0) ? 1 : -1;
			const int stepY = (d.y >= 0) ? 1 : -1;
			const int stepZ = (d.z >= 0) ? 1 : -1;

			// Distance along the ray to cross one cell in each axis
			const Scalar tDeltaX = (fabs(d.x) > 1e-20) ? fabs(m_cellSize.x / d.x) : RISE_INFINITY;
			const Scalar tDeltaY = (fabs(d.y) > 1e-20) ? fabs(m_cellSize.y / d.y) : RISE_INFINITY;
			const Scalar tDeltaZ = (fabs(d.z) > 1e-20) ? fabs(m_cellSize.z / d.z) : RISE_INFINITY;

			// Distance to the next cell boundary in each axis
			Scalar tMaxX, tMaxY, tMaxZ;
			if( fabs(d.x) > 1e-20 ) {
				const Scalar nextBoundary = m_bboxMin.x + Scalar(stepX > 0 ? cx + 1 : cx) * m_cellSize.x;
				tMaxX = tEntry + (nextBoundary - entryPt.x) / d.x;
			} else {
				tMaxX = RISE_INFINITY;
			}
			if( fabs(d.y) > 1e-20 ) {
				const Scalar nextBoundary = m_bboxMin.y + Scalar(stepY > 0 ? cy + 1 : cy) * m_cellSize.y;
				tMaxY = tEntry + (nextBoundary - entryPt.y) / d.y;
			} else {
				tMaxY = RISE_INFINITY;
			}
			if( fabs(d.z) > 1e-20 ) {
				const Scalar nextBoundary = m_bboxMin.z + Scalar(stepZ > 0 ? cz + 1 : cz) * m_cellSize.z;
				tMaxZ = tEntry + (nextBoundary - entryPt.z) / d.z;
			} else {
				tMaxZ = RISE_INFINITY;
			}

			// Walk cells via DDA
			Scalar tCellEntry = tEntry;
			static const unsigned int nMaxCells = 256;

			for( unsigned int i = 0; i < nMaxCells; i++ )
			{
				// Determine where we leave this cell
				const Scalar tCellExit = fmin( fmin( tMaxX, tMaxY ), fmin( tMaxZ, tExit ) );

				// Fetch the majorant for this cell
				const Scalar cellMaj = m_data[CellIndex(
					(unsigned int)cx, (unsigned int)cy, (unsigned int)cz )];

				// Call the visitor
				if( !visitor( tCellEntry, tCellExit, cellMaj ) )
					return true;  // Visitor requested stop (e.g., scatter accepted)

				// Advance to next cell
				if( tCellExit >= tExit )
					return true;  // Reached end of traversal range

				tCellEntry = tCellExit;

				// Step to the next cell along the axis with smallest tMax
				if( tMaxX <= tMaxY && tMaxX <= tMaxZ )
				{
					cx += stepX;
					if( cx < 0 || cx >= (int)m_gridX ) return true;
					tMaxX += tDeltaX;
				}
				else if( tMaxY <= tMaxZ )
				{
					cy += stepY;
					if( cy < 0 || cy >= (int)m_gridY ) return true;
					tMaxY += tDeltaY;
				}
				else
				{
					cz += stepZ;
					if( cz < 0 || cz >= (int)m_gridZ ) return true;
					tMaxZ += tDeltaZ;
				}
			}

			return true;  // Exceeded max cells
		}

	protected:
		/// Ray-AABB intersection (slab method).
		/// Same algorithm as HeterogeneousMedium::IntersectBBox.
		bool IntersectBBox(
			const Ray& ray,
			Scalar& tEntry,
			Scalar& tExit
			) const;

		/// Compute the default grid resolution for a given volume dimension.
		/// Resolution = clamp( ceil(volDim / 8), 4, 32 )
		static unsigned int DefaultGridRes( unsigned int volDim )
		{
			unsigned int res = (volDim + 7) / 8;  // ceil(volDim/8)
			if( res < 4 ) res = 4;
			if( res > 32 ) res = 32;
			return res;
		}

	public:
		/// Convenience: compute default grid resolution for a volume
		static void DefaultGridResolution(
			unsigned int volWidth,
			unsigned int volHeight,
			unsigned int volDepth,
			unsigned int& gridX,
			unsigned int& gridY,
			unsigned int& gridZ
			)
		{
			gridX = DefaultGridRes( volWidth );
			gridY = DefaultGridRes( volHeight );
			gridZ = DefaultGridRes( volDepth );
		}
	};
}

#endif
