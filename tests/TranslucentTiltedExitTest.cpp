//////////////////////////////////////////////////////////////////////
//
//  TranslucentTiltedExitTest.cpp - Regression guard for DL-45:
//    TranslucentSPF's diffuse EXIT re-emission (RGB Scatter() and
//    ScatterNM(), "coming out the other side") sampled unconditionally
//    around the (possibly bump/glint-tilted) shading normal and
//    unconditionally popped the IOR stack -- with no geometric-horizon
//    check at all, unlike the entry-branch front (reflection) lobe a
//    few lines above it.  Under a tilted shading normal, a real
//    fraction of "exit" samples pointed back INTO the solid while the
//    stack said outside.  DL-03's production PT-guiding fixture
//    (TranslucentGuidedStackProbe.h) recorded this directly: 1021/4096
//    unsubstituted RGB/NM exit samples at a 60-degree tilt.
//
//  THE FIX (TranslucentSPF.cpp)
//
//    Resample (rejection sampling against the SAME cosine-around-n
//    distribution) until the direction is ALSO geometrically valid
//    (dot(wo,geomNRaw)>0, using the object's actual unflipped outward
//    direction -- see the long comment in Scatter()), and report the
//    density of that ACTUAL restricted procedure: a properly
//    NORMALIZED (integrates to 1 over its own support) conditional PDF
//    cos(theta)/pi / P(valid), where P(valid) = (1+cos(phi))/2 and phi
//    is the angle between the shading normal and the true outward
//    direction (`ExitValidFraction`'s closed-form derivation).  This is
//    explicitly NOT the "reject and silently lose the energy" policy
//    the row's recipe rules out -- every emitted exit sample is valid
//    by construction, and Pdf()/PdfNM() report the matching normalized
//    density rather than the unclipped one.
//
//  COVERAGE (recipe: "aaligned and tilted shading normals... enclosing
//    IOR, scattering endpoints... a unit-energy directional integral")
//
//    Sub-test 1 -- sampled-direction validity: at 7 tilt angles (0 to
//      89 degrees) x extinction {0, 1} x scattering {0, 0.5}, every
//      emitted diffuse EXIT ray (the one carrying a non-null ior_stack)
//      across thousands of trials must satisfy dot(dir, trueOutward) >
//      0.  RED-PROOF (pre-fix): nonzero tilt produces a real fraction
//      of inward exits (matches DL-03's 1021/4096 at 60 degrees).
//    Sub-test 2 -- Pdf()/PdfNM() at a sampled exit direction must equal
//      the closed-form cos(theta)/pi / ExitValidFraction, and must be
//      EXACTLY 0 for a wo that fails the geometric-horizon gate even
//      though it is within the shading hemisphere.
//    Sub-test 3 -- unit-energy directional integral: numerically
//      integrating Pdf()/PdfNM() over the full sphere (fine theta/phi
//      quadrature) must recover 1.0 within quadrature tolerance, at
//      every tilt angle -- the defining property of a NORMALIZED
//      density that an unrenormalized reject-based policy would fail
//      (it would integrate to P(valid) < 1 instead).
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `translucent`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/TranslucentSPF.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int failed = 0;

#define EXPECT( cond, msg ) do { \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

// True outward direction used throughout: vGeomNormal is set explicitly
// to (0,0,1) on every fixture below, independent of tilt or ray
// direction (matching how real closed-surface geometry, e.g.
// SphereGeometry, reports it -- see docs/DL45... / TranslucentSPF.cpp's
// comment on `geomNRaw`).
static const Vector3 kTrueOutward( 0, 0, 1 );

// Builds an "inside, about to exit" fixture with the shading normal
// tilted by `tiltDeg` off the true outward direction (rotation in the
// XZ plane), and vGeomNormal fixed at the true outward direction.
static RayIntersectionGeometric MakeTiltedExitIntersection( Scalar tiltDeg )
{
	const Scalar tiltRad = tiltDeg * PI / 180.0;
	const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

	Ray inRay( Point3(0,0,-1), Vector3(0,0,1) );  // arbitrary; exit branch's
	                                               // geometric gate no longer
	                                               // depends on ray.Dir().
	RasterizerState rs = {0,0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = n;
	ri.onb.CreateFromW( n );
	ri.vGeomNormal = kTrueOutward;
	ri.ptCoord = Point2(0.5,0.5);

	return ri;
}

static IORStack MakeInsideStack( const IObject* obj )
{
	IORStack stack( 1.0 );
	stack.SetCurrentObject( obj );
	stack.push( 1.33 );  // nonzero enclosing IOR (water) -- DL-45's recipe
	                      // asks for enclosing-IOR coverage; TranslucentSPF's
	                      // exit direction/pdf logic doesn't consult the
	                      // numeric IOR at all, but the fixture should look
	                      // like a real nested scenario, not just air.
	stack.SetCurrentObject( obj );
	return stack;
}

namespace
{
	const Scalar kTiltAnglesDeg[] = { 0, 15, 30, 45, 60, 75, 89 };
	const int kNumTilts = 7;
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: sampled exit directions are always geometrically valid.
//////////////////////////////////////////////////////////////////////
static void TestSampledDirectionsAreOutward()
{
	std::cout << "Sub-test 1: sampled exit directions vs. tilt/extinction/scattering" << std::endl;

	const Scalar extinctions[] = { 0.0, 1.0 };
	const Scalar scatterings[] = { 0.0, 0.5 };

	StubObject* obj = new StubObject();  obj->addref();
	RandomNumberGenerator rng( 424242 );
	IndependentSampler sampler( rng );

	for( int t = 0; t < kNumTilts; t++ ) {
		for( int e = 0; e < 2; e++ ) {
			for( int s = 0; s < 2; s++ ) {
				UniformColorPainter* front = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  front->addref();
				UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  trans->addref();
				UniformScalarPainter* ext = new UniformScalarPainter( extinctions[e] );  ext->addref();
				UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
				UniformScalarPainter* scat = new UniformScalarPainter( scatterings[s] );  scat->addref();
				TranslucentSPF* spf = new TranslucentSPF( *front, *trans, *ext, *phongN, *scat );
				spf->addref();

				RayIntersectionGeometric ri = MakeTiltedExitIntersection( kTiltAnglesDeg[t] );
				IORStack stack = MakeInsideStack( obj );

				unsigned int exitSamples = 0, inwardExits = 0;
				const unsigned int kTrials = 4096;
				for( unsigned int trial = 0; trial < kTrials; trial++ ) {
					for( unsigned int spectral = 0; spectral < 2; spectral++ ) {
						ScatteredRayContainer scattered;
						if( spectral ) {
							spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
						} else {
							spf->Scatter( ri, sampler, scattered, stack );
						}
						for( unsigned int i = 0; i < scattered.Count(); i++ ) {
							if( scattered[i].ior_stack != 0 && scattered[i].type == ScatteredRay::eRayDiffuse ) {
								exitSamples++;
								if( Vector3Ops::Dot( scattered[i].ray.Dir(), kTrueOutward ) <= 0 ) {
									inwardExits++;
								}
							}
						}
					}
				}

				char label[256];
				std::snprintf( label, sizeof(label),
					"tilt=%g ext=%g scat=%g: exit samples=%u inward=%u",
					(double)kTiltAnglesDeg[t], (double)extinctions[e], (double)scatterings[s],
					exitSamples, inwardExits );
				std::cout << "  " << label << std::endl;
				EXPECT( exitSamples > 0, ( std::string(label) + " -- at least one exit sample emitted" ).c_str() );
				EXPECT( inwardExits == 0, ( std::string(label) + " -- no exit sample points geometrically inward" ).c_str() );

				spf->release();
				scat->release(); phongN->release(); ext->release(); trans->release(); front->release();
			}
		}
	}
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2: Pdf()/PdfNM() formula and geometric-horizon zeroing.
//////////////////////////////////////////////////////////////////////
static void TestPdfFormula()
{
	std::cout << "Sub-test 2: Pdf/PdfNM formula + geometric-horizon zeroing" << std::endl;

	StubObject* obj = new StubObject();  obj->addref();
	UniformColorPainter* front = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  trans->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.2 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.0 );  scat->addref();
	TranslucentSPF* spf = new TranslucentSPF( *front, *trans, *ext, *phongN, *scat );
	spf->addref();

	RandomNumberGenerator rng( 99 );
	IndependentSampler sampler( rng );

	for( int t = 0; t < kNumTilts; t++ ) {
		const Scalar tiltRad = kTiltAnglesDeg[t] * PI / 180.0;
		const Scalar cosPhi = cos(tiltRad);
		const Scalar pValid = r_max( Scalar(1e-4), (1.0+cosPhi)*0.5 );

		RayIntersectionGeometric ri = MakeTiltedExitIntersection( kTiltAnglesDeg[t] );
		IORStack stack = MakeInsideStack( obj );

		// Sample a real exit direction and check Pdf() against it.
		ScatteredRayContainer scattered;
		spf->Scatter( ri, sampler, scattered, stack );
		bool checked = false;
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			if( scattered[i].ior_stack != 0 && scattered[i].type == ScatteredRay::eRayDiffuse ) {
				const Vector3 wo = scattered[i].ray.Dir();
				const Scalar cosTheta = Vector3Ops::Dot( wo, ri.onb.w() );
				const Scalar expectedPdf = (cosTheta*INV_PI) / pValid;
				const Scalar actualPdf = spf->Pdf( ri, wo, stack );
				char label[160];
				std::snprintf( label, sizeof(label), "tilt=%g: Pdf(sampled exit dir) matches closed form", (double)kTiltAnglesDeg[t] );
				EXPECT( fabs(actualPdf - expectedPdf) < 1e-6 * r_max(Scalar(1),fabs(expectedPdf)), label );
				EXPECT( fabs(scattered[i].pdf - actualPdf) < 1e-9, "stored front.pdf matches re-evaluated Pdf()" );
				checked = true;
				break;
			}
		}
		EXPECT( checked, "found a real exit sample to check Pdf() against" );

		// A wo that IS in the shading hemisphere (cosTheta>0) but fails the
		// geometric-horizon gate (dot(wo,trueOutward)<=0) must be exactly 0
		// -- only reachable at nonzero tilt, since at tilt=0 the shading
		// hemisphere and the outward hemisphere coincide.  Construct wo by
		// rotating n (in the plane spanned by n and the true outward
		// direction, both in the XZ plane by construction) by angle
		// eps = (91-tilt) degrees toward the perpendicular direction that
        // points AWAY from the true outward direction: dot(wo,n)=cos(eps)
		// (>0 for eps<90) and dot(wo,Z)=cos(tilt+eps); choosing eps so that
		// tilt+eps=91 deg guarantees the latter is just past the true
		// horizon while the former stays comfortably positive, for any
		// tilt in (45,90).
		if( kTiltAnglesDeg[t] > 45 ) {
			const Scalar epsRad = (91.0 - kTiltAnglesDeg[t]) * PI / 180.0;
			const Vector3 ePerp( cos(tiltRad), 0, -sin(tiltRad) );
			const Vector3 wo = ri.onb.w()*cos(epsRad) + ePerp*sin(epsRad);
			const Scalar cosThetaShading = Vector3Ops::Dot( wo, ri.onb.w() );
			const Scalar cosThetaGeom = Vector3Ops::Dot( wo, kTrueOutward );
			char geomLabel[160];
			std::snprintf( geomLabel, sizeof(geomLabel), "tilt=%g: constructed wo is shading-valid, geometrically-inward (fixture sanity)", (double)kTiltAnglesDeg[t] );
			EXPECT( cosThetaShading > 0 && cosThetaGeom <= 0, geomLabel );
			const Scalar p = spf->Pdf( ri, wo, stack );
			char label[160];
			std::snprintf( label, sizeof(label), "tilt=%g: Pdf==0 for shading-valid but geometrically-inward wo", (double)kTiltAnglesDeg[t] );
			EXPECT( p == 0, label );
		}
	}

	spf->release();
	scat->release(); phongN->release(); ext->release(); trans->release(); front->release();
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 3: unit-energy directional integral of Pdf() over the full
//  sphere, at every tilt angle.
//////////////////////////////////////////////////////////////////////
static void TestUnitEnergyIntegral()
{
	std::cout << "Sub-test 3: unit-energy directional integral of Pdf()" << std::endl;

	StubObject* obj = new StubObject();  obj->addref();
	UniformColorPainter* front = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  trans->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.2 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.0 );  scat->addref();
	TranslucentSPF* spf = new TranslucentSPF( *front, *trans, *ext, *phongN, *scat );
	spf->addref();

	IORStack stack = MakeInsideStack( obj );

	const int kThetaSteps = 300;
	const int kPhiSteps = 600;

	for( int t = 0; t < kNumTilts; t++ ) {
		RayIntersectionGeometric ri = MakeTiltedExitIntersection( kTiltAnglesDeg[t] );

		Scalar integral = 0;
		for( int ti = 0; ti < kThetaSteps; ti++ ) {
			const Scalar theta0 = PI * Scalar(ti) / kThetaSteps;
			const Scalar theta1 = PI * Scalar(ti+1) / kThetaSteps;
			const Scalar thetaMid = 0.5*(theta0+theta1);
			const Scalar dTheta = theta1 - theta0;
			const Scalar sinThetaMid = sin(thetaMid);
			for( int pi_ = 0; pi_ < kPhiSteps; pi_++ ) {
				const Scalar phi = TWO_PI * (Scalar(pi_)+0.5) / kPhiSteps;
				const Scalar dPhi = TWO_PI / kPhiSteps;
				const Vector3 wo = ri.onb.u()*sin(thetaMid)*cos(phi)
					+ ri.onb.v()*sin(thetaMid)*sin(phi)
					+ ri.onb.w()*cos(thetaMid);
				const Scalar pdf = spf->Pdf( ri, wo, stack );
				integral += pdf * sinThetaMid * dTheta * dPhi;
			}
		}

		char label[160];
		std::snprintf( label, sizeof(label), "tilt=%g: integral(Pdf dOmega) == 1", (double)kTiltAnglesDeg[t] );
		std::cout << "  " << label << " -> " << integral << std::endl;
		// Quadrature (300x600 cells) is generous for a smooth cosine-like
		// integrand except right at the tilt-induced horizon discontinuity;
		// 1% relative tolerance comfortably separates "normalized" (this
		// fix, ~1.0) from "unrenormalized reject" (would integrate to
		// P(valid), e.g. 0.75 at 60 deg, 0.5 at 90 deg -- both well outside
		// this band).
		EXPECT( fabs(integral - 1.0) < 0.01, label );
	}

	spf->release();
	scat->release(); phongN->release(); ext->release(); trans->release(); front->release();
	obj->release();
}

int main()
{
	GlobalLog();

	TestSampledDirectionsAreOutward();
	TestPdfFormula();
	TestUnitEnergyIntegral();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	} else {
		std::cout << failed << " CHECK(S) FAILED" << std::endl;
		return 1;
	}
}
