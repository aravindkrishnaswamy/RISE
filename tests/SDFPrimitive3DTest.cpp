//////////////////////////////////////////////////////////////////////
//
//  SDFPrimitive3DTest.cpp - Unit tests for SDFPrimitive3D
//
//  Tests:
//    1. Output is in [0, 1] range
//    2. Sphere: origin has density 1
//    3. Sphere: far away has density 0
//    4. Box: center has density 1, outside has density 0
//    5. Torus: on ring has density 1
//    6. Shell mode: center of sphere is hollow
//    7. Noise displacement changes output
//    8. Different primitive types produce different outputs
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>

#include "../src/Library/Noise/SDFPrimitives.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"

using namespace RISE;
using namespace RISE::Implementation;

static bool IsClose( double a, double b, double tol = 1e-6 )
{
	return fabs( a - b ) < tol;
}

bool TestOutputRange()
{
	std::cout << "  Test 1: Output range [0,1]..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0, 0, 0, 0, 0, *interp );

	bool passed = true;
	for( int i = -20; i < 20; i++ ) {
		for( int j = -5; j < 5; j++ ) {
			Scalar val = sdf->Evaluate( i * 0.1, j * 0.1, 0 );
			if( val < -1e-10 || val > 1.0 + 1e-6 ) {
				std::cout << "    FAIL: out of range " << val << std::endl;
				passed = false;
				break;
			}
		}
		if( !passed ) break;
	}

	sdf->release();
	interp->release();
	if( passed ) std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestSphereCenter()
{
	std::cout << "  Test 2: Sphere center density=1..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0, 0, 0, 0, 0, *interp );

	Scalar val = sdf->Evaluate( 0, 0, 0 );
	bool passed = val > 0.9;
	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: center density=" << val << std::endl;
	else
		std::cout << "    PASSED (density=" << val << ")" << std::endl;
	return passed;
}

bool TestSphereAxisSymmetry()
{
	std::cout << "  Test 3: Sphere axis symmetry..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0, 0, 0, 0, 0, *interp );

	const Scalar vx = sdf->Evaluate( 0.25, 0.0, 0.0 );
	const Scalar vy = sdf->Evaluate( 0.0, 0.25, 0.0 );
	const Scalar vz = sdf->Evaluate( 0.0, 0.0, 0.25 );
	bool passed = IsClose( vx, vy, 1e-6 ) && IsClose( vx, vz, 1e-6 );

	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: vx=" << vx << " vy=" << vy << " vz=" << vz << std::endl;
	else
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestSphereFarAway()
{
	std::cout << "  Test 4: Sphere far away density=0..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0, 0, 0, 0, 0, *interp );

	Scalar val = sdf->Evaluate( 10, 10, 10 );
	bool passed = val < 0.01;
	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: far density=" << val << std::endl;
	else
		std::cout << "    PASSED (density=" << val << ")" << std::endl;
	return passed;
}

bool TestBoxCenter()
{
	std::cout << "  Test 5: Box center dense, outside sparse..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Box, 0.5, 0.5, 0.5, 0, 0, 0, *interp );

	Scalar center = sdf->Evaluate( 0, 0, 0 );
	Scalar outside = sdf->Evaluate( 5, 5, 5 );
	bool passed = center > 0.9 && outside < 0.01;
	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: center=" << center << " outside=" << outside << std::endl;
	else
		std::cout << "    PASSED (center=" << center << " outside=" << outside << ")" << std::endl;
	return passed;
}

bool TestBoxSignSymmetry()
{
	std::cout << "  Test 6: Box sign symmetry..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Box, 0.5, 0.25, 0.75, 0, 0, 0, *interp );

	const Scalar a = sdf->Evaluate( 0.2, -0.1, 0.3 );
	const Scalar b = sdf->Evaluate( -0.2, 0.1, -0.3 );
	bool passed = IsClose( a, b, 1e-6 );

	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: a=" << a << " b=" << b << std::endl;
	else
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestTorusRing()
{
	std::cout << "  Test 7: Torus on ring density high..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	// Major radius 1.0, minor radius 0.3, torus in XZ plane
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Torus, 1.0, 0.3, 0, 0, 0, 0, *interp );

	// On the ring at (1, 0, 0)
	Scalar onRing = sdf->Evaluate( 1.0, 0, 0 );
	// Far from ring
	Scalar farAway = sdf->Evaluate( 5, 5, 5 );
	bool passed = onRing > 0.5 && farAway < 0.01;
	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: onRing=" << onRing << " far=" << farAway << std::endl;
	else
		std::cout << "    PASSED (onRing=" << onRing << " far=" << farAway << ")" << std::endl;
	return passed;
}

bool TestCylinderBasic()
{
	std::cout << "  Test 8: Cylinder center dense, outside sparse..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Cylinder, 0.5, 1.0, 0, 0, 0, 0, *interp );

	const Scalar center = sdf->Evaluate( 0, 0, 0 );
	const Scalar outsideRadial = sdf->Evaluate( 2, 0, 0 );
	const Scalar outsideHeight = sdf->Evaluate( 0, 2, 0 );
	bool passed = center > 0.9 && outsideRadial < 0.05 && outsideHeight < 0.05;

	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: center=" << center
			<< " outsideRadial=" << outsideRadial
			<< " outsideHeight=" << outsideHeight << std::endl;
	else
		std::cout << "    PASSED" << std::endl;
	return passed;
}

bool TestShellMode()
{
	std::cout << "  Test 9: Shell mode - center is hollow..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	// Sphere with shell thickness 0.1
	SDFPrimitive3D* sdf = new SDFPrimitive3D( eSDF_Sphere, 1.0, 0, 0, 0.1, 0, 0, *interp );

	Scalar center = sdf->Evaluate( 0, 0, 0 );		// Far inside = hollow
	Scalar surface = sdf->Evaluate( 1.0, 0, 0 );	// On surface = dense

	bool passed = surface > center;
	sdf->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: surface=" << surface << " center=" << center << std::endl;
	else
		std::cout << "    PASSED (surface=" << surface << " center=" << center << ")" << std::endl;
	return passed;
}

bool TestNoiseDisplacement()
{
	std::cout << "  Test 10: Noise displacement changes output..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sdfNoNoise = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0, 0, 0, 0, 0, *interp );
	SDFPrimitive3D* sdfNoise = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0, 0, 0, 0.2, 2.0, *interp );

	int differCount = 0;
	for( int i = 0; i < 30; i++ ) {
		Scalar x = i * 0.1 - 1.5;
		if( !IsClose( sdfNoNoise->Evaluate(x, 0, 0), sdfNoise->Evaluate(x, 0, 0), 1e-4 ) )
			differCount++;
	}

	bool passed = differCount > 10;
	sdfNoNoise->release();
	sdfNoise->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/30 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/30 differ)" << std::endl;
	return passed;
}

bool TestDifferentTypes()
{
	std::cout << "  Test 11: Different types produce different outputs..." << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();
	SDFPrimitive3D* sphere = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0.5, 0.5, 0, 0, 0, *interp );
	SDFPrimitive3D* box = new SDFPrimitive3D( eSDF_Box, 0.5, 0.5, 0.5, 0, 0, 0, *interp );
	SDFPrimitive3D* torus = new SDFPrimitive3D( eSDF_Torus, 0.5, 0.2, 0, 0, 0, 0, *interp );

	int differCount = 0;
	for( int i = 0; i < 20; i++ ) {
		Scalar x = i * 0.15 - 1.5;
		Scalar y = i * 0.1 - 1.0;
		Scalar s = sphere->Evaluate( x, y, 0 );
		Scalar b = box->Evaluate( x, y, 0 );
		Scalar t = torus->Evaluate( x, y, 0 );
		if( !IsClose(s, b, 1e-4) || !IsClose(s, t, 1e-4) ) differCount++;
	}

	bool passed = differCount > 10;
	sphere->release();
	box->release();
	torus->release();
	interp->release();
	if( !passed )
		std::cout << "    FAIL: only " << differCount << "/20 differ" << std::endl;
	else
		std::cout << "    PASSED (" << differCount << "/20 differ)" << std::endl;
	return passed;
}

int main()
{
	std::cout << "=== SDFPrimitive3D Tests ===" << std::endl;
	bool allPassed = true;

	allPassed &= TestOutputRange();
	allPassed &= TestSphereCenter();
	allPassed &= TestSphereAxisSymmetry();
	allPassed &= TestSphereFarAway();
	allPassed &= TestBoxCenter();
	allPassed &= TestBoxSignSymmetry();
	allPassed &= TestTorusRing();
	allPassed &= TestCylinderBasic();
	allPassed &= TestShellMode();
	allPassed &= TestNoiseDisplacement();
	allPassed &= TestDifferentTypes();

	std::cout << std::endl;
	if( allPassed )
		std::cout << "ALL TESTS PASSED" << std::endl;
	else
		std::cout << "SOME TESTS FAILED" << std::endl;

	return allPassed ? 0 : 1;
}
