//////////////////////////////////////////////////////////////////////
//
//  PainterVolumeAccessorTest.cpp - Unit tests for VolumeAccessor_Painter
//
//  Tests:
//    1. Uniform painter produces constant density
//    2. Spatially varying painter produces correct density gradient
//    3. Density values are clamped to [0, 1]
//    4. MajorantGrid integration (majorants >= actual density)
//    5. Color-to-scalar conversion modes
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


int main()
{
	std::cout << "=== PainterVolumeAccessor Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestUniformDensity();
	allPassed &= TestGradientDensity();
	allPassed &= TestClamping();
	allPassed &= TestMajorantGridIntegration();
	allPassed &= TestColorToScalarModes();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
