//////////////////////////////////////////////////////////////////////
//
//  PainterVolumeAccessorTest.cpp - Unit tests for VolumeAccessor_Painter
//    and the interpolating volume accessors (TRI / NNB / TriCubic)
//
//  Tests:
//    1. Uniform painter produces constant density
//    2. Spatially varying painter produces correct density gradient
//    3. Density values are clamped to [0, 1]
//    4. MajorantGrid integration (majorants >= actual density)
//    5. Color-to-scalar conversion modes
//    6. TRI reproduces exact voxel values at integer coordinates
//    7. TRI reproduces a linear ramp at signed fractional coordinates
//    8. TRI is continuous across integer-coordinate knots
//    9. TRI stays inside the convex hull of the 8 cell corners
//   10. NNB maps signed coordinates to the containing cell (floor)
//   11. TriCubic (Catmull-Rom) reproduces knots and is continuous
//       across integer coordinates at signed positions
//   12. TriCubic (UniformBSpline) reproduces a linear ramp at signed
//       coordinates (linear precision of the DVR 'b' accessor)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 12, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Volume/VolumeAccessor_Painter.h"
#include "../src/Library/Volume/VolumeAccessor_TRI.h"
#include "../src/Library/Volume/VolumeAccessor_NNB.h"
#include "../src/Library/Volume/VolumeAccessor_TriCubic.h"
#include "../src/Library/Utilities/CubicInterpolator.h"
#include "../src/Library/Utilities/MajorantGrid.h"

using namespace RISE;


/// Minimal test painter that returns a fixed color.
class TestUniformPainter : public virtual IPainter
{
	RISEPel m_color;

public:
	TestUniformPainter( const RISEPel& color ) : m_color( color ) {}

	RISEPel GetColor( const RayIntersectionGeometric& ) const { return m_color; }
	Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar ) const { return 0; }
	SpectralPacket GetSpectrum( const RayIntersectionGeometric& ) const { return SpectralPacket(400,700,1); }
	Scalar Evaluate( const Scalar, const Scalar ) const { return 0; }
	IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
	void SetIntermediateValue( const IKeyframeParameter& ) {}
	void RegenerateData() {}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }
};


/// Test painter that returns a gradient based on world-space X.
/// Color ramps from (0,0,0) to (1,1,1) as ptIntersection.x goes
/// from xMin to xMax.
class TestGradientPainter : public virtual IPainter
{
	Scalar m_xMin;
	Scalar m_xMax;

public:
	TestGradientPainter( Scalar xMin, Scalar xMax )
		: m_xMin( xMin ), m_xMax( xMax ) {}

	RISEPel GetColor( const RayIntersectionGeometric& ri ) const
	{
		Scalar t = (ri.ptIntersection.x - m_xMin) / (m_xMax - m_xMin);
		if( t < 0.0 ) t = 0.0;
		if( t > 1.0 ) t = 1.0;
		return RISEPel( t, t, t );
	}

	Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const
	{
		return GetColor( ri )[0];
	}

	SpectralPacket GetSpectrum( const RayIntersectionGeometric& ) const { return SpectralPacket(400,700,1); }
	Scalar Evaluate( const Scalar, const Scalar ) const { return 0; }
	IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
	void SetIntermediateValue( const IKeyframeParameter& ) {}
	void RegenerateData() {}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }
};


/// Test painter that returns out-of-range values for clamping test.
class TestOutOfRangePainter : public virtual IPainter
{
public:
	RISEPel GetColor( const RayIntersectionGeometric& ) const
	{
		return RISEPel( -0.5, 1.5, 2.0 );
	}

	Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar ) const { return 0; }
	SpectralPacket GetSpectrum( const RayIntersectionGeometric& ) const { return SpectralPacket(400,700,1); }
	Scalar Evaluate( const Scalar, const Scalar ) const { return 0; }
	IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
	void SetIntermediateValue( const IKeyframeParameter& ) {}
	void RegenerateData() {}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }
};


static bool IsClose( double a, double b, double tol = 1e-6 )
{
	return fabs( a - b ) < tol;
}


/// Test 1: Uniform painter produces constant density at all coordinates
bool TestUniformDensity()
{
	std::cout << "  Test 1: Uniform painter density..." << std::endl;

	TestUniformPainter painter( RISEPel( 0.5, 0.5, 0.5 ) );
	const Point3 bboxMin( -10, -10, -10 );
	const Point3 bboxMax( 10, 10, 10 );
	const unsigned int res = 16;

	VolumeAccessor_Painter* accessor = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'l' );

	bool passed = true;
	const int half = (int)res / 2;

	// Sample at several centered coordinates
	for( int z = -half; z < (int)res - half; z += 4 )
	{
		for( int y = -half; y < (int)res - half; y += 4 )
		{
			for( int x = -half; x < (int)res - half; x += 4 )
			{
				const Scalar d = accessor->GetValue( x, y, z );
				if( !IsClose( d, 0.5, 1e-4 ) )
				{
					std::cout << "    FAIL: at (" << x << "," << y << "," << z
						<< ") density=" << d << " expected 0.5" << std::endl;
					passed = false;
				}
			}
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 2: Gradient painter produces monotonically varying density
bool TestGradientDensity()
{
	std::cout << "  Test 2: Gradient painter density..." << std::endl;

	const Point3 bboxMin( 0, 0, 0 );
	const Point3 bboxMax( 100, 100, 100 );
	const unsigned int res = 32;

	TestGradientPainter painter( bboxMin.x, bboxMax.x );
	VolumeAccessor_Painter* accessor = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'l' );

	bool passed = true;

	// Sample along the X axis at y=0, z=0 (centered coords)
	Scalar prevDensity = -1.0;
	const int half = (int)res / 2;

	for( int x = -half; x < (int)res - half; x++ )
	{
		const Scalar d = accessor->GetValue( x, 0, 0 );
		if( d < prevDensity - 1e-6 )
		{
			std::cout << "    FAIL: density not monotonic at x=" << x
				<< " d=" << d << " prev=" << prevDensity << std::endl;
			passed = false;
		}
		prevDensity = d;
	}

	// Check boundary values: leftmost should be near 0, rightmost near 1
	const Scalar dLeft = accessor->GetValue( -half, 0, 0 );
	const Scalar dRight = accessor->GetValue( (int)res - half - 1, 0, 0 );

	if( dLeft > 0.1 )
	{
		std::cout << "    FAIL: left boundary density=" << dLeft << " (expected near 0)" << std::endl;
		passed = false;
	}

	if( dRight < 0.9 )
	{
		std::cout << "    FAIL: right boundary density=" << dRight << " (expected near 1)" << std::endl;
		passed = false;
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED (left=" << dLeft << " right=" << dRight << ")" << std::endl;
	return passed;
}


/// Test 3: Out-of-range values are clamped to [0, 1]
bool TestClamping()
{
	std::cout << "  Test 3: Density clamping..." << std::endl;

	TestOutOfRangePainter painter;
	const Point3 bboxMin( 0, 0, 0 );
	const Point3 bboxMax( 10, 10, 10 );
	const unsigned int res = 8;

	// Test all three modes
	VolumeAccessor_Painter* accL = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'l' );
	VolumeAccessor_Painter* accM = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'm' );
	VolumeAccessor_Painter* accR = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'r' );

	const Scalar dL = accL->GetValue( 0, 0, 0 );
	const Scalar dM = accM->GetValue( 0, 0, 0 );
	const Scalar dR = accR->GetValue( 0, 0, 0 );

	bool passed = true;

	if( dL < 0.0 || dL > 1.0 )
	{
		std::cout << "    FAIL: luminance mode density=" << dL << " not in [0,1]" << std::endl;
		passed = false;
	}

	if( dM < 0.0 || dM > 1.0 )
	{
		std::cout << "    FAIL: max mode density=" << dM << " not in [0,1]" << std::endl;
		passed = false;
	}

	if( dR < 0.0 || dR > 1.0 )
	{
		std::cout << "    FAIL: red mode density=" << dR << " not in [0,1]" << std::endl;
		passed = false;
	}

	accL->release();
	accM->release();
	accR->release();
	if( passed )
		std::cout << "    PASSED (l=" << dL << " m=" << dM << " r=" << dR << ")" << std::endl;
	return passed;
}


/// Test 4: MajorantGrid built from painter accessor has valid majorant bounds
bool TestMajorantGridIntegration()
{
	std::cout << "  Test 4: MajorantGrid integration..." << std::endl;

	const Point3 bboxMin( -50, -50, -50 );
	const Point3 bboxMax( 50, 50, 50 );
	const unsigned int res = 16;
	const Scalar sigma_t_majorant = 0.01;

	TestGradientPainter painter( bboxMin.x, bboxMax.x );
	VolumeAccessor_Painter* accessor = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'l' );

	unsigned int gridX, gridY, gridZ;
	MajorantGrid::DefaultGridResolution( res, res, res, gridX, gridY, gridZ );

	MajorantGrid grid( *accessor, res, res, res,
		bboxMin, bboxMax, sigma_t_majorant,
		gridX, gridY, gridZ );

	// Verify every voxel's sigma_t does not exceed its cell's majorant
	bool passed = true;
	const int half = (int)res / 2;

	for( int vz = -half; vz < (int)res - half; vz++ )
	{
		for( int vy = -half; vy < (int)res - half; vy++ )
		{
			for( int vx = -half; vx < (int)res - half; vx++ )
			{
				const Scalar density = accessor->GetValue( vx, vy, vz );
				const Scalar actual_sigma_t = density * sigma_t_majorant;

				const Scalar nx = (Scalar(vx) / Scalar(res)) + 0.5;
				const Scalar ny = (Scalar(vy) / Scalar(res)) + 0.5;
				const Scalar nz = (Scalar(vz) / Scalar(res)) + 0.5;

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

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 5: Color-to-scalar conversion modes produce expected results
bool TestColorToScalarModes()
{
	std::cout << "  Test 5: Color-to-scalar modes..." << std::endl;

	// Color (0.2, 0.6, 0.3)
	// Luminance = 0.2126*0.2 + 0.7152*0.6 + 0.0722*0.3 = 0.04252 + 0.42912 + 0.02166 = 0.4933
	// Max = 0.6
	// Red = 0.2
	TestUniformPainter painter( RISEPel( 0.2, 0.6, 0.3 ) );
	const Point3 bboxMin( 0, 0, 0 );
	const Point3 bboxMax( 10, 10, 10 );
	const unsigned int res = 8;

	VolumeAccessor_Painter* accL = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'l' );
	VolumeAccessor_Painter* accM = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'm' );
	VolumeAccessor_Painter* accR = new VolumeAccessor_Painter( painter, res, res, res, bboxMin, bboxMax, 'r' );

	const Scalar dL = accL->GetValue( 0, 0, 0 );
	const Scalar dM = accM->GetValue( 0, 0, 0 );
	const Scalar dR = accR->GetValue( 0, 0, 0 );

	bool passed = true;

	const Scalar expectedLum = 0.2126 * 0.2 + 0.7152 * 0.6 + 0.0722 * 0.3;

	if( !IsClose( dL, expectedLum, 1e-3 ) )
	{
		std::cout << "    FAIL: luminance=" << dL << " expected " << expectedLum << std::endl;
		passed = false;
	}

	if( !IsClose( dM, 0.6, 1e-4 ) )
	{
		std::cout << "    FAIL: max=" << dM << " expected 0.6" << std::endl;
		passed = false;
	}

	if( !IsClose( dR, 0.2, 1e-4 ) )
	{
		std::cout << "    FAIL: red=" << dR << " expected 0.2" << std::endl;
		passed = false;
	}

	accL->release();
	accM->release();
	accR->release();
	if( passed )
		std::cout << "    PASSED (l=" << dL << " m=" << dM << " r=" << dR << ")" << std::endl;
	return passed;
}


//////////////////////////////////////////////////////////////////////
// Interpolating accessor tests (TRI / NNB / TriCubic)
//
// These guard against two historical defects in the accessors'
// GetValue( Scalar, Scalar, Scalar ):
//   (a) a reversed z blend in VolumeAccessor_TRI (front/back planes
//       swapped, producing knot discontinuities as z crossed integers)
//   (b) modf/int-cast fraction extraction, which yields negative
//       fractions and mis-identified cells for negative coordinates —
//       and heterogeneous volumes address with centered, SIGNED
//       coordinates.
//////////////////////////////////////////////////////////////////////

/// Synthetic volume whose voxel value is a linear ramp over the
/// signed (centered) voxel indices: f(x,y,z) = a + bx + cy + dz.
/// Defined for ALL indices so tests never touch a boundary.
class TestRampVolume : public virtual IVolume
{
	Scalar m_a, m_b, m_c, m_d;

public:
	TestRampVolume( Scalar a, Scalar b, Scalar c, Scalar d )
		: m_a( a ), m_b( b ), m_c( c ), m_d( d ) {}

	unsigned int Width( ) const { return 64; }
	unsigned int Height( ) const { return 64; }
	unsigned int Depth( ) const { return 64; }

	Scalar GetValue( const int x, const int y, const int z ) const
	{
		return m_a + m_b * Scalar(x) + m_c * Scalar(y) + m_d * Scalar(z);
	}

	Scalar Evaluate( Scalar x, Scalar y, Scalar z ) const
	{
		return m_a + m_b * x + m_c * y + m_d * z;
	}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }
};


/// Synthetic volume with deterministic pseudo-random voxel values in
/// [0, 1], defined for all signed indices.
class TestHashVolume : public virtual IVolume
{
public:
	unsigned int Width( ) const { return 64; }
	unsigned int Height( ) const { return 64; }
	unsigned int Depth( ) const { return 64; }

	Scalar GetValue( const int x, const int y, const int z ) const
	{
		unsigned int h = (unsigned int)x * 73856093u ^
			(unsigned int)y * 19349663u ^
			(unsigned int)z * 83492791u;
		h ^= h >> 13;
		h *= 0x5bd1e995u;
		h ^= h >> 15;
		return Scalar( h & 0xffffffu ) / Scalar( 0xffffffu );
	}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }
};


/// Test 6: TRI reproduces exact voxel values at integer coordinates.
/// Catches the reversed z blend: at a whole-number z the old code
/// returned the z+1 plane instead of the z plane.
bool TestTRIIntegerReproduction()
{
	std::cout << "  Test 6: TRI integer-coordinate voxel reproduction..." << std::endl;

	TestHashVolume vol;
	VolumeAccessor_TRI* accessor = new VolumeAccessor_TRI();
	accessor->BindVolume( &vol );

	bool passed = true;

	for( int z = -5; z <= 5; z += 2 )
	{
		for( int y = -5; y <= 5; y += 2 )
		{
			for( int x = -5; x <= 5; x += 2 )
			{
				const Scalar expected = vol.GetValue( x, y, z );
				const Scalar got = accessor->GetValue( Scalar(x), Scalar(y), Scalar(z) );
				if( !IsClose( got, expected, 1e-9 ) )
				{
					std::cout << "    FAIL: at (" << x << "," << y << "," << z
						<< ") got " << got << " expected " << expected << std::endl;
					passed = false;
				}
			}
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 7: TRI reproduces a linear ramp exactly at signed fractional
/// coordinates.  Discriminates the z-blend reversal (the z slope
/// inverts within each cell).  NOTE: this test alone does NOT catch
/// the negative-fraction modf defect -- affine weights that sum to 1
/// reproduce a linear ramp exactly even when individual weights fall
/// outside [0,1] (verified against a z-fixed/modf-broken mutant).
/// Tests 8 and 9 are the modf-defect guards; keep them if pruning.
bool TestTRISignedRampReproduction()
{
	std::cout << "  Test 7: TRI signed-coordinate ramp reproduction..." << std::endl;

	TestRampVolume vol( 0.5, 0.25, -0.125, 0.375 );
	VolumeAccessor_TRI* accessor = new VolumeAccessor_TRI();
	accessor->BindVolume( &vol );

	bool passed = true;

	const Scalar coords[] = { -4.75, -3.5, -2.25, -1.9, -0.5, -0.1, 0.0, 0.3, 1.5, 2.6, 4.25 };
	const int n = sizeof(coords) / sizeof(coords[0]);

	for( int k = 0; k < n; k++ )
	{
		for( int j = 0; j < n; j++ )
		{
			for( int i = 0; i < n; i++ )
			{
				const Scalar x = coords[i], y = coords[j], z = coords[k];
				const Scalar expected = vol.Evaluate( x, y, z );
				const Scalar got = accessor->GetValue( x, y, z );
				if( !IsClose( got, expected, 1e-9 ) )
				{
					std::cout << "    FAIL: at (" << x << "," << y << "," << z
						<< ") got " << got << " expected " << expected << std::endl;
					passed = false;
				}
			}
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 8: TRI is continuous across integer-coordinate knots.
/// Approaching a whole-number coordinate from below and above must
/// agree to O(epsilon).  The old z blend jumped by a full voxel
/// difference at every integer z.
bool TestTRIKnotContinuity()
{
	std::cout << "  Test 8: TRI knot continuity..." << std::endl;

	TestHashVolume vol;
	VolumeAccessor_TRI* accessor = new VolumeAccessor_TRI();
	accessor->BindVolume( &vol );

	bool passed = true;
	const Scalar eps = 1e-7;
	const Scalar tol = 1e-4;	// values are in [0,1]; gradient is O(1)

	// Walk each axis across several signed integer knots, holding the
	// other two axes at fractional positions.
	const int knots[] = { -3, -1, 0, 1, 4 };
	const int nKnots = sizeof(knots) / sizeof(knots[0]);

	for( int axis = 0; axis < 3; axis++ )
	{
		for( int k = 0; k < nKnots; k++ )
		{
			Scalar lo[3] = { 0.4, -1.7, 2.3 };
			Scalar hi[3] = { 0.4, -1.7, 2.3 };
			lo[axis] = Scalar(knots[k]) - eps;
			hi[axis] = Scalar(knots[k]) + eps;

			const Scalar below = accessor->GetValue( lo[0], lo[1], lo[2] );
			const Scalar above = accessor->GetValue( hi[0], hi[1], hi[2] );

			if( fabs( below - above ) > tol )
			{
				std::cout << "    FAIL: axis " << axis << " knot " << knots[k]
					<< " below=" << below << " above=" << above << std::endl;
				passed = false;
			}
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 9: TRI never leaves the convex hull of the 8 surrounding
/// voxel values.  Negative modf fractions produced weights outside
/// [0,1] and thus extrapolation.
bool TestTRIConvexHull()
{
	std::cout << "  Test 9: TRI convex-hull (no extrapolation) bounds..." << std::endl;

	TestHashVolume vol;
	VolumeAccessor_TRI* accessor = new VolumeAccessor_TRI();
	accessor->BindVolume( &vol );

	bool passed = true;

	const Scalar coords[] = { -4.6, -3.25, -1.5, -0.75, -0.2, 0.35, 1.8, 3.9 };
	const int n = sizeof(coords) / sizeof(coords[0]);

	for( int k = 0; k < n; k++ )
	{
		for( int j = 0; j < n; j++ )
		{
			for( int i = 0; i < n; i++ )
			{
				const Scalar x = coords[i], y = coords[j], z = coords[k];
				const int xlo = (int)floor( x ), ylo = (int)floor( y ), zlo = (int)floor( z );

				Scalar mn = 1e30, mx = -1e30;
				for( int dz = 0; dz <= 1; dz++ )
					for( int dy = 0; dy <= 1; dy++ )
						for( int dx = 0; dx <= 1; dx++ )
						{
							const Scalar v = vol.GetValue( xlo + dx, ylo + dy, zlo + dz );
							if( v < mn ) mn = v;
							if( v > mx ) mx = v;
						}

				const Scalar got = accessor->GetValue( x, y, z );
				if( got < mn - 1e-9 || got > mx + 1e-9 )
				{
					std::cout << "    FAIL: at (" << x << "," << y << "," << z
						<< ") value " << got << " outside corner hull ["
						<< mn << ", " << mx << "]" << std::endl;
					passed = false;
				}
			}
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 10: NNB maps signed coordinates to the containing cell.
/// int-cast truncation sent everything in (-1, 0) to voxel 0 instead
/// of voxel -1 (and likewise for every negative cell).
bool TestNNBSignedCoordinates()
{
	std::cout << "  Test 10: NNB signed-coordinate cell selection..." << std::endl;

	TestHashVolume vol;
	VolumeAccessor_NNB* accessor = new VolumeAccessor_NNB();
	accessor->BindVolume( &vol );

	bool passed = true;

	struct { Scalar in; int cell; } cases[] = {
		{ -0.5, -1 }, { -0.001, -1 }, { -1.999, -2 }, { -2.3, -3 },
		{ 0.0, 0 }, { 0.4, 0 }, { 0.999, 0 }, { 2.7, 2 }, { -4.0, -4 },
	};
	const int n = sizeof(cases) / sizeof(cases[0]);

	for( int i = 0; i < n; i++ )
	{
		const Scalar expected = vol.GetValue( cases[i].cell, cases[i].cell, cases[i].cell );
		const Scalar got = accessor->GetValue( cases[i].in, cases[i].in, cases[i].in );
		if( !IsClose( got, expected, 1e-12 ) )
		{
			std::cout << "    FAIL: coord " << cases[i].in << " expected cell "
				<< cases[i].cell << " value " << expected << " got " << got << std::endl;
			passed = false;
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 11: TriCubic (Catmull-Rom) reproduces voxel values at signed
/// integer coordinates and is continuous across integer knots at
/// signed positions.  Negative modf fractions handed the cubic a
/// parameter in (-1, 0) around a mis-based footprint.  Part (b)
/// continuity is what discriminates that historical defect; part (a)
/// knot reproduction passed even the old code (modf at exact integers
/// yields fraction -0.0 with a correct base) and is kept as a guard
/// against future footprint off-by-ones or plane swaps.
bool TestTriCubicSignedCoordinates()
{
	std::cout << "  Test 11: TriCubic signed knots and continuity..." << std::endl;

	TestHashVolume vol;
	ICubicInterpolator<Scalar>* interp = new CatmullRomCubicInterpolator<Scalar>();
	VolumeAccessor_TriCubic* accessor = new VolumeAccessor_TriCubic( *interp );
	interp->release();
	accessor->BindVolume( &vol );

	bool passed = true;

	// (a) Knot reproduction: Catmull-Rom passes through its control
	// points, so integer coordinates must return the voxel exactly.
	for( int z = -4; z <= 4; z += 2 )
	{
		for( int y = -3; y <= 3; y += 3 )
		{
			for( int x = -5; x <= 5; x += 5 )
			{
				const Scalar expected = vol.GetValue( x, y, z );
				const Scalar got = accessor->GetValue( Scalar(x), Scalar(y), Scalar(z) );
				if( !IsClose( got, expected, 1e-9 ) )
				{
					std::cout << "    FAIL: knot (" << x << "," << y << "," << z
						<< ") got " << got << " expected " << expected << std::endl;
					passed = false;
				}
			}
		}
	}

	// (b) Continuity across signed integer knots.
	const Scalar eps = 1e-7;
	const Scalar tol = 1e-4;
	const int knots[] = { -3, -1, 0, 2 };
	const int nKnots = sizeof(knots) / sizeof(knots[0]);

	for( int axis = 0; axis < 3; axis++ )
	{
		for( int k = 0; k < nKnots; k++ )
		{
			Scalar lo[3] = { -0.6, 1.3, -2.4 };
			Scalar hi[3] = { -0.6, 1.3, -2.4 };
			lo[axis] = Scalar(knots[k]) - eps;
			hi[axis] = Scalar(knots[k]) + eps;

			const Scalar below = accessor->GetValue( lo[0], lo[1], lo[2] );
			const Scalar above = accessor->GetValue( hi[0], hi[1], hi[2] );

			if( fabs( below - above ) > tol )
			{
				std::cout << "    FAIL: axis " << axis << " knot " << knots[k]
					<< " below=" << below << " above=" << above << std::endl;
				passed = false;
			}
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


/// Test 12: TriCubic with the UniformBSpline interpolator (the DVR 'b'
/// accessor) reproduces a linear ramp at signed coordinates.  B-spline
/// does not interpolate its control points, but it has linear
/// precision, so a linear ramp is an exact expectation -- this
/// exercises the shared floor-based fraction/footprint path under the
/// second interpolator variant.
bool TestTriCubicBSplineLinearPrecision()
{
	std::cout << "  Test 12: TriCubic B-spline linear precision..." << std::endl;

	TestRampVolume vol( -0.25, 0.375, 0.0625, -0.5 );
	ICubicInterpolator<Scalar>* interp = new UniformBSplineCubicInterpolator<Scalar>();
	VolumeAccessor_TriCubic* accessor = new VolumeAccessor_TriCubic( *interp );
	interp->release();
	accessor->BindVolume( &vol );

	bool passed = true;

	const Scalar coords[] = { -4.75, -2.25, -1.9, -0.5, 0.0, 0.3, 1.5, 3.25 };
	const int n = sizeof(coords) / sizeof(coords[0]);

	for( int k = 0; k < n; k++ )
	{
		for( int j = 0; j < n; j++ )
		{
			for( int i = 0; i < n; i++ )
			{
				const Scalar x = coords[i], y = coords[j], z = coords[k];
				const Scalar expected = vol.Evaluate( x, y, z );
				const Scalar got = accessor->GetValue( x, y, z );
				if( !IsClose( got, expected, 1e-9 ) )
				{
					std::cout << "    FAIL: at (" << x << "," << y << "," << z
						<< ") got " << got << " expected " << expected << std::endl;
					passed = false;
				}
			}
		}
	}

	accessor->release();
	if( passed )
		std::cout << "    PASSED" << std::endl;
	return passed;
}


int main()
{
	std::cout << "=== PainterVolumeAccessor Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestUniformDensity();
	allPassed &= TestGradientDensity();
	allPassed &= TestClamping();
	allPassed &= TestMajorantGridIntegration();
	allPassed &= TestColorToScalarModes();
	allPassed &= TestTRIIntegerReproduction();
	allPassed &= TestTRISignedRampReproduction();
	allPassed &= TestTRIKnotContinuity();
	allPassed &= TestTRIConvexHull();
	allPassed &= TestNNBSignedCoordinates();
	allPassed &= TestTriCubicSignedCoordinates();
	allPassed &= TestTriCubicBSplineLinearPrecision();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
