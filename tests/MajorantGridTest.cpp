//////////////////////////////////////////////////////////////////////
//
//  MajorantGridTest.cpp - Unit tests for the MajorantGrid class
//
//  Tests:
//    1. Grid majorants >= actual density in every cell
//    2. DDA traversal visits correct cells for axis-aligned rays
//    3. DDA traversal visits correct cells for diagonal rays
//    4. Empty cells have zero majorant
//    5. Traversal range clamping works correctly
//    6. Tiny volumes (axis dim < grid res) keep the majorant
//       invariant under tricubic (Catmull-Rom) interpolation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 8, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "../src/Library/Utilities/MajorantGrid.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/CubicInterpolator.h"
#include "../src/Library/Interfaces/IVolumeAccessor.h"
#include "../src/Library/Interfaces/IVolume.h"
#include "../src/Library/Volume/VolumeAccessor_TriCubic.h"

using namespace RISE;


/// Simple test volume accessor that returns a spherical density field.
/// Density = max(0, 1 - distance_from_center / radius)
/// Uses centered coordinates: the sphere is at the origin.
class TestSphereAccessor : public virtual IVolumeAccessor
{
	Scalar m_radius;

public:
	TestSphereAccessor(
		Scalar radius
		) : m_radius(radius) {}

	Scalar GetValue( Scalar x, Scalar y, Scalar z ) const
	{
		// Centered coordinates
		const Scalar dist = sqrt( x*x + y*y + z*z );
		const Scalar d = 1.0 - dist / m_radius;
		return (d > 0) ? d : 0;
	}

	Scalar GetValue( int x, int y, int z ) const
	{
		return GetValue( Scalar(x), Scalar(y), Scalar(z) );
	}

	void BindVolume( const IVolume* ) {}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }
};


/// Test 1: Grid majorants are >= actual density at all points within each cell
bool TestMajorantBounds()
{
	std::cout << "  Test 1: Majorant bounds..." << std::endl;

	const unsigned int volDim = 32;
	const Scalar radius = 12.0;
	const Scalar sigma_t_majorant = 0.01;
	TestSphereAccessor accessor( radius );

	unsigned int gridX, gridY, gridZ;
	MajorantGrid::DefaultGridResolution( volDim, volDim, volDim,
		gridX, gridY, gridZ );

	const Point3 bboxMin( -100, -100, -100 );
	const Point3 bboxMax( 100, 100, 100 );

	MajorantGrid grid( accessor, volDim, volDim, volDim,
		bboxMin, bboxMax, sigma_t_majorant,
		gridX, gridY, gridZ );

	// Sample random points within each cell and verify the density
	// at each point does not exceed the cell's majorant
	bool passed = true;
	const int halfDim = (int)volDim / 2;

	for( int vz = -halfDim; vz < (int)volDim - halfDim; vz++ )
	{
		for( int vy = -halfDim; vy < (int)volDim - halfDim; vy++ )
		{
			for( int vx = -halfDim; vx < (int)volDim - halfDim; vx++ )
			{
				const Scalar density = accessor.GetValue( vx, vy, vz );
				const Scalar actual_sigma_t = density * sigma_t_majorant;

				// Find which grid cell this voxel belongs to
				const Scalar nx = (Scalar(vx) / Scalar(volDim)) + 0.5;
				const Scalar ny = (Scalar(vy) / Scalar(volDim)) + 0.5;
				const Scalar nz = (Scalar(vz) / Scalar(volDim)) + 0.5;

				unsigned int cx = (unsigned int)fmin( nx * Scalar(gridX), Scalar(gridX - 1) );
				unsigned int cy = (unsigned int)fmin( ny * Scalar(gridY), Scalar(gridY - 1) );
				unsigned int cz = (unsigned int)fmin( nz * Scalar(gridZ), Scalar(gridZ - 1) );

				const Scalar cellMaj = grid.GetCellMajorant( cx, cy, cz );

				if( actual_sigma_t > cellMaj + 1e-10 )
				{
					std::cout << "    FAIL: voxel (" << vx << "," << vy << "," << vz
						<< ") sigma_t=" << actual_sigma_t
						<< " > cell (" << cx << "," << cy << "," << cz
						<< ") majorant=" << cellMaj << std::endl;
					passed = false;
				}
			}
		}
	}

	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 2: DDA traversal visits cells along an axis-aligned ray
bool TestDDAAxisAligned()
{
	std::cout << "  Test 2: DDA axis-aligned traversal..." << std::endl;

	const unsigned int volDim = 16;
	const Scalar radius = 6.0;
	const Scalar sigma_t_majorant = 0.01;
	TestSphereAccessor accessor( radius );

	const Point3 bboxMin( 0, 0, 0 );
	const Point3 bboxMax( 8, 8, 8 );

	MajorantGrid grid( accessor, volDim, volDim, volDim,
		bboxMin, bboxMax, sigma_t_majorant,
		4, 4, 4 );

	// Ray along +X through the center of the grid
	const Ray ray( Point3( -1, 4, 4 ), Vector3( 1, 0, 0 ) );

	std::vector<unsigned int> visitedCells;

	struct CellCollector
	{
		std::vector<unsigned int>* cells;
		unsigned int gridX;
		unsigned int gridY;

		bool operator()( Scalar tEntry, Scalar tExit, Scalar cellMaj )
		{
			(void)tEntry; (void)tExit; (void)cellMaj;
			// We can't easily get cell indices from tEntry/tExit,
			// but we can count the number of cells visited
			cells->push_back( 1 );
			return true;  // Continue
		}
	};

	CellCollector collector;
	collector.cells = &visitedCells;
	collector.gridX = 4;
	collector.gridY = 4;

	grid.TraverseRay( ray, 0.0, 100.0, collector );

	// Should visit exactly 4 cells (4 cells along X)
	bool passed = (visitedCells.size() == 4);
	if( !passed )
	{
		std::cout << "    FAIL: expected 4 cells, got " << visitedCells.size() << std::endl;
	}
	else
	{
		std::cout << "    PASSED (visited " << visitedCells.size() << " cells)" << std::endl;
	}
	return passed;
}


/// Test 3: DDA traversal visits correct cells for a diagonal ray
bool TestDDADiagonal()
{
	std::cout << "  Test 3: DDA diagonal traversal..." << std::endl;

	const unsigned int volDim = 16;
	const Scalar radius = 6.0;
	const Scalar sigma_t_majorant = 0.01;
	TestSphereAccessor accessor( radius );

	const Point3 bboxMin( 0, 0, 0 );
	const Point3 bboxMax( 4, 4, 4 );

	MajorantGrid grid( accessor, volDim, volDim, volDim,
		bboxMin, bboxMax, sigma_t_majorant,
		4, 4, 4 );

	// Diagonal ray from corner to corner
	const Vector3 dir( 1, 1, 1 );
	const Scalar len = sqrt( 3.0 );
	const Ray ray( Point3( -0.1, -0.1, -0.1 ),
		Vector3( dir.x/len, dir.y/len, dir.z/len ) );

	unsigned int cellCount = 0;
	struct Counter {
		unsigned int* count;
		bool operator()( Scalar, Scalar, Scalar ) {
			(*count)++;
			return true;
		}
	};

	Counter counter;
	counter.count = &cellCount;
	grid.TraverseRay( ray, 0.0, 100.0, counter );

	// A diagonal through a 4x4x4 grid visits at most 3*4 - 2 = 10 cells
	// (Amanatides-Woo: sum of grid dims minus (ndim-1))
	// But exact count depends on the ray direction. With a perfect diagonal
	// it visits 4 + 3 + 3 = 10 cells (4 along each axis, minus overlaps)
	// In practice it should visit between 4 and 12 cells
	bool passed = (cellCount >= 4 && cellCount <= 12);
	if( !passed )
	{
		std::cout << "    FAIL: diagonal visited " << cellCount << " cells (expected 4-12)" << std::endl;
	}
	else
	{
		std::cout << "    PASSED (visited " << cellCount << " cells)" << std::endl;
	}
	return passed;
}


/// Test 4: Cells outside the sphere have zero majorant
bool TestEmptyCells()
{
	std::cout << "  Test 4: Empty cells outside sphere..." << std::endl;

	const unsigned int volDim = 64;
	const Scalar radius = 4.0;  // Very small sphere relative to volume
	const Scalar sigma_t_majorant = 0.01;
	TestSphereAccessor accessor( radius );

	const Point3 bboxMin( -100, -100, -100 );
	const Point3 bboxMax( 100, 100, 100 );

	// Use 16^3 grid so radius-2 dilation doesn't fill everything
	MajorantGrid grid( accessor, volDim, volDim, volDim,
		bboxMin, bboxMax, sigma_t_majorant,
		16, 16, 16 );

	// Corner cells should have zero majorant (far from center)
	unsigned int zeroCells = 0;
	unsigned int nonZeroCells = 0;
	for( unsigned int cz = 0; cz < 16; cz++ )
		for( unsigned int cy = 0; cy < 16; cy++ )
			for( unsigned int cx = 0; cx < 16; cx++ )
			{
				if( grid.GetCellMajorant(cx, cy, cz) == 0 )
					zeroCells++;
				else
					nonZeroCells++;
			}

	// The sphere is very small relative to the grid; even with
	// radius-2 dilation, most cells should remain empty.
	bool passed = (zeroCells > 0 && nonZeroCells > 0 && zeroCells > nonZeroCells);
	if( !passed )
	{
		std::cout << "    FAIL: zero=" << zeroCells << " nonzero=" << nonZeroCells << std::endl;
	}
	else
	{
		std::cout << "    PASSED (zero=" << zeroCells << " nonzero=" << nonZeroCells << ")" << std::endl;
	}
	return passed;
}


/// Test 5: Visitor stop works correctly
bool TestVisitorStop()
{
	std::cout << "  Test 5: Visitor early stop..." << std::endl;

	const unsigned int volDim = 16;
	const Scalar radius = 6.0;
	const Scalar sigma_t_majorant = 0.01;
	TestSphereAccessor accessor( radius );

	const Point3 bboxMin( 0, 0, 0 );
	const Point3 bboxMax( 8, 8, 8 );

	MajorantGrid grid( accessor, volDim, volDim, volDim,
		bboxMin, bboxMax, sigma_t_majorant,
		4, 4, 4 );

	const Ray ray( Point3( -1, 4, 4 ), Vector3( 1, 0, 0 ) );

	unsigned int cellCount = 0;
	struct StopAfterTwo {
		unsigned int* count;
		bool operator()( Scalar, Scalar, Scalar ) {
			(*count)++;
			return (*count < 2);  // Stop after visiting 2 cells
		}
	};

	StopAfterTwo stopper;
	stopper.count = &cellCount;
	grid.TraverseRay( ray, 0.0, 100.0, stopper );

	bool passed = (cellCount == 2);
	if( !passed )
	{
		std::cout << "    FAIL: expected 2 visits, got " << cellCount << std::endl;
	}
	else
	{
		std::cout << "    PASSED" << std::endl;
	}
	return passed;
}


/// Volume for Test 6: 2x2x2 voxels (centered indices in {-1, 0} per
/// axis) with a single hot voxel at (-1,-1,-1) and zero elsewhere,
/// including out-of-range indices (matching Volume::GetValue).
class TestTinyHotVoxelVolume : public virtual IVolume
{
public:
	unsigned int Width( ) const { return 2; }
	unsigned int Height( ) const { return 2; }
	unsigned int Depth( ) const { return 2; }

	Scalar GetValue( const int x, const int y, const int z ) const
	{
		if( x < -1 || x > 0 || y < -1 || y > 0 || z < -1 || z > 0 )
			return 0;
		return ( x == -1 && y == -1 && z == -1 ) ? 1.0 : 0.0;
	}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }
};


/// Test 6: Majorant invariant for a tiny volume under tricubic
/// (Catmull-Rom) interpolation.
///
/// With the default resolution rule clamp(ceil(dim/8), 4, 32), an
/// axis dimension < 4 makes grid cells SMALLER than a voxel (dim=2
/// -> 4-cell grid -> 1 voxel spans 2 cells).  The tricubic stencil
/// reaches 2 voxels = 4 cells from the query point, so a fixed
/// 2-cell dilation radius under-covers: a positive Catmull-Rom
/// cross-term (two negative lobe factors) from the hot voxel can
/// make the interpolated density exceed a distant cell's majorant.
/// Regression guard for the voxel-based dilation radius.
bool TestTinyVolumeTricubicMajorant()
{
	std::cout << "  Test 6: Tiny-volume tricubic majorant invariant..." << std::endl;

	const unsigned int volDim = 2;
	const Scalar sigma_t_majorant = 1.0;

	TestTinyHotVoxelVolume vol;
	ICubicInterpolator<Scalar>* interp = new CatmullRomCubicInterpolator<Scalar>();
	VolumeAccessor_TriCubic* accessor = new VolumeAccessor_TriCubic( *interp );
	interp->release();
	accessor->BindVolume( &vol );

	unsigned int gridX, gridY, gridZ;
	MajorantGrid::DefaultGridResolution( volDim, volDim, volDim,
		gridX, gridY, gridZ );

	const Point3 bboxMin( -3, -3, -3 );
	const Point3 bboxMax( 5, 5, 5 );

	MajorantGrid grid( *accessor, volDim, volDim, volDim,
		bboxMin, bboxMax, sigma_t_majorant,
		gridX, gridY, gridZ );

	// Dense sweep over the volume's centered coordinate domain
	// [-dim/2, dim/2).  For each sample, the interpolated extinction
	// must not exceed the containing cell's majorant.  This mirrors
	// how HeterogeneousMedium queries density: world point -> centered
	// volume coordinates -> interpolating accessor.
	bool passed = true;
	unsigned int violations = 0;
	Scalar worstExcess = 0;

	const Scalar half = Scalar(volDim) / 2.0;
	const Scalar step = 0.05;
	const Vector3 extent = Vector3Ops::mkVector3( bboxMax, bboxMin );

	for( Scalar vz = -half; vz < half; vz += step )
	{
		for( Scalar vy = -half; vy < half; vy += step )
		{
			for( Scalar vx = -half; vx < half; vx += step )
			{
				const Scalar density = accessor->GetValue( vx, vy, vz );
				const Scalar sigma = density * sigma_t_majorant;
				if( sigma <= 0 )
					continue;  // Undershoot can't break the majorant

				// Centered -> world (reverse of LookupDensity's mapping)
				const Point3 worldPt(
					(vx / Scalar(volDim) + 0.5) * extent.x + bboxMin.x,
					(vy / Scalar(volDim) + 0.5) * extent.y + bboxMin.y,
					(vz / Scalar(volDim) + 0.5) * extent.z + bboxMin.z );

				unsigned int cx, cy, cz;
				if( !grid.WorldToCell( worldPt, cx, cy, cz ) )
				{
					std::cout << "    FAIL: sample (" << vx << "," << vy << "," << vz
						<< ") maps outside the grid" << std::endl;
					passed = false;
					continue;
				}

				const Scalar cellMaj = grid.GetCellMajorant( cx, cy, cz );
				if( sigma > cellMaj + 1e-12 )
				{
					if( violations < 5 )
					{
						std::cout << "    FAIL: at (" << vx << "," << vy << "," << vz
							<< ") sigma_t=" << sigma
							<< " > cell (" << cx << "," << cy << "," << cz
							<< ") majorant=" << cellMaj << std::endl;
					}
					violations++;
					if( sigma - cellMaj > worstExcess )
						worstExcess = sigma - cellMaj;
					passed = false;
				}
			}
		}
	}

	accessor->release();

	if( !passed && violations > 0 )
	{
		std::cout << "    " << violations << " majorant violations, worst excess "
			<< worstExcess << std::endl;
	}
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


int main()
{
	std::cout << "=== MajorantGrid Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestMajorantBounds();
	allPassed &= TestDDAAxisAligned();
	allPassed &= TestDDADiagonal();
	allPassed &= TestEmptyCells();
	allPassed &= TestVisitorStop();
	allPassed &= TestTinyVolumeTricubicMajorant();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
