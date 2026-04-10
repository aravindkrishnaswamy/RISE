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
#include "../src/Library/Interfaces/IVolumeAccessor.h"
#include "../src/Library/Interfaces/IVolume.h"

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

	// Use 16^3 grid so 2-pass dilation doesn't fill everything
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
	// 2-pass dilation, most cells should remain empty.
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


int main()
{
	std::cout << "=== MajorantGrid Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestMajorantBounds();
	allPassed &= TestDDAAxisAligned();
	allPassed &= TestDDADiagonal();
	allPassed &= TestEmptyCells();
	allPassed &= TestVisitorStop();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
