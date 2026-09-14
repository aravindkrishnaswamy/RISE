//////////////////////////////////////////////////////////////////////
//
//  TranslucentEntryHorizonTest.cpp - Red-proof and regression guard for
//    DL-68: `TranslucentSPF::Scatter`/`ScatterNM`'s ENTERING branch
//    sampled its transmission (`trans`) lobe as a Phong `cos^N` lobe
//    about the FLIPPED SHADING normal and pushed the IOR stack
//    UNCONDITIONALLY -- with no geometric check that the sampled
//    direction actually crosses to the far side of the surface.  Under
//    a tilted shading normal (bump / normal map / glint modifier, or
//    simply a smooth-shaded mesh's interpolated normal) a real fraction
//    of those rays travel back out the side they arrived from while the
//    stack now says "inside".  This is the ENTRY-PUSH mirror of the
//    EXIT-POP defect DL-45 fixed a few lines below it.
//
//    The same missing check is present on the exit branch's own
//    interior BACKSCATTER `trans` lobe, whose contract is the opposite
//    membership claim ("stays inside this object, no stack change") --
//    a wrong-hemisphere sample there leaves the object WITHOUT popping.
//    Sub-test 4 covers that sibling.
//
//  THE GEOMETRIC REFERENCE (and why it is NOT DL-45's `geomNRaw`)
//
//    A transmission lobe's defining property is that it continues
//    THROUGH the surface, i.e. it must leave on the opposite side from
//    the one the incoming ray arrived on.  That is the RAY-ANCHORED
//    reference `geomN` (the true geometric normal flipped to oppose
//    `ri.ray.Dir()`), exactly the reference the entry FRONT (reflection)
//    lobe already gates against -- the transmission half-space is its
//    exact complement.  On a closed object this is identical to
//    "-(true outward normal)", which is what DL-68's ledger row
//    prescribes and what sub-tests 1/3 measure; the two differ only on
//    an OPEN double-sided sheet struck from its back face (sub-test 2),
//    where the object's "inside" is undefined but "through the sheet"
//    still is -- and there the unflipped reference would send the
//    transmission straight back at the light.  The interior backscatter
//    lobe (sub-test 4) is the same reference with the opposite sign:
//    it must stay on the side the interior ray arrived from, `+geomN`.
//
//  THE FIX (TranslucentSPF.cpp, `SampleClippedPhong`)
//
//    An EXACT, unconditional TWO-DRAW construction of a `cos^N` lobe
//    conditioned on a half-space -- no rejection (which would break
//    `ISampler::HasFixedDimensionBudget()`, see
//    TranslucentSamplerDimensionCountTest), and valid for EVERY N, not
//    just the plain-cosine N=1 case DL-45's Malley disk remap covers:
//
//      theta  from the unclipped marginal, cos(theta) = u1^(1/(N+1));
//      psi    UNIFORM on the valid azimuth ARC at that theta, which the
//             constraint dot(w,clipN)>0 makes cos(psi) > -cot(theta)cot(phi)
//             in the frame whose +u axis carries clipN's tangential part.
//
//    Given theta, the clipped Phong density is constant in psi, so the
//    conditional draw is exactly uniform on that arc; the resulting
//    solid-angle density is closed form,
//
//      q(w) = (N+1) * cos^N(theta) / (2 * halfArc(theta)),
//      halfArc(theta) = PI                       if cot(theta)cot(phi) >= 1
//                     = acos(-cot(theta)cot(phi)) otherwise,
//
//    which reduces to the pre-existing (N+1)cos^N/(2*PI) whenever the
//    clip is inactive.  Note this is NOT DL-45's cos/pi/P(valid) shape:
//    it renormalizes PER THETA RING rather than globally, because the
//    theta marginal is left at the unclipped one.  Both are exact; this
//    one extends to N != 1, which the ledger row records as the open
//    part of the derivation.  Energy in the clipped-away region is
//    RENORMALIZED into the valid region (matching DL-45's choice), not
//    dropped: every trial emits exactly one transmission ray.
//
//  RED-PROOF
//
//    Sub-test 1 (closed analytic fixture, isotropic N=1, RGB, 8192
//    trials/tilt) reproduces the ledger row's own census: the fraction
//    of pushed `trans` rays NOT geometrically into the solid tracks
//    (1-cos(tilt))/2 -- 0 at 0 deg, ~0.067 at 30 deg, ~0.25 at 60 deg,
//    ~0.49 at 89 deg.  The fix drives every one of those counts to 0.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `dl68`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/TranslucentSPF.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int checks = 0;
static int failed = 0;

#define EXPECT( cond, msg ) do { \
	checks++; \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

namespace
{
	const Scalar kTiltAnglesDeg[] = { 0, 15, 30, 45, 60, 75, 89 };
	const int kNumTilts = 7;

	//! Closed-form solid-angle density of the clipped Phong lobe the fix
	//! samples -- implemented here INDEPENDENTLY of TranslucentSPF.cpp so
	//! that "stored pdf == this" is a cross-check, not a tautology.
	Scalar ClippedPhongDensity( const Vector3& axis, const Vector3& clipN,
		const Scalar N, const Vector3& w )
	{
		const Scalar cosTheta = Vector3Ops::Dot( w, axis );
		if( cosTheta <= 0 ) return 0;
		if( Vector3Ops::Dot( w, clipN ) <= 0 ) return 0;

		const Scalar cosPhi = r_max( Scalar(0), r_min( Scalar(1), Vector3Ops::Dot(axis,clipN) ) );
		const Scalar sinPhi = sqrt( r_max( Scalar(0), Scalar(1) - cosPhi*cosPhi ) );
		const Scalar sinTheta = sqrt( r_max( Scalar(0), Scalar(1) - cosTheta*cosTheta ) );

		Scalar half = PI;
		const Scalar denom = sinTheta * sinPhi;
		if( denom > Scalar(1e-12) ) {
			const Scalar num = cosTheta * cosPhi;
			if( num < denom ) {
				half = acos( -num/denom );
			}
		}
		return (N+Scalar(1)) * pow( cosTheta, N ) / ( Scalar(2) * half );
	}

	//! The lobe axis and clip normal the fix uses for the ENTERING
	//! transmission lobe, recomputed here from the fixture's own fields.
	void EntryLobeFrame( const RayIntersectionGeometric& ri, Vector3& axis, Vector3& clipN )
	{
		const Vector3 n = ri.onb.w();
		const Vector3 trueGeom = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
		const Vector3 geomNRaw = ( Vector3Ops::SquaredModulus(trueGeom) > Scalar(1e-12) ) ? trueGeom : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		clipN = -geomN;                                  // "through the surface"
		axis  = ( Vector3Ops::Dot( n, clipN ) >= 0 ) ? n : -n;
	}

	//! Same, for the exit branch's interior BACKSCATTER lobe (must stay
	//! on the side the interior ray arrived from).
	void BackscatterLobeFrame( const RayIntersectionGeometric& ri, Vector3& axis, Vector3& clipN )
	{
		const Vector3 n = ri.onb.w();
		const Vector3 trueGeom = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
		const Vector3 geomNRaw = ( Vector3Ops::SquaredModulus(trueGeom) > Scalar(1e-12) ) ? trueGeom : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		clipN = geomN;                                   // stays inside
		axis  = ( Vector3Ops::Dot( n, geomN ) >= 0 ) ? n : -n;
	}
}

//////////////////////////////////////////////////////////////////////
//  Fixtures
//////////////////////////////////////////////////////////////////////

//! A CLOSED translucent object (analytic primitive convention: the
//! reported geometric normal is the object's true outward direction and
//! is NOT flipped toward the ray) struck from outside, with the shading
//! normal tilted `tiltDeg` off that outward direction in the XZ plane.
static RayIntersectionGeometric MakeClosedEntry( Scalar tiltDeg )
{
	const Scalar tiltRad = tiltDeg * PI / 180.0;
	const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

	Ray inRay( Point3(0,0,2), Vector3(0,0,-1) );   // travelling INTO the solid
	RasterizerState rs = {0,0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 2.0;
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = n;
	ri.onb.CreateFromW( n );
	ri.vGeomNormal = Vector3(0,0,1);
	ri.ptCoord = Point2(0.5,0.5);
	return ri;
}

//! A FLAT DOUBLE-SIDED sheet struck on its BACK face: the geometry has
//! already flipped BOTH normals toward the incoming ray and recorded
//! that in `bGeomNormalOrientedToRay` (TriangleMeshGeometry when
//! bDoubleSided).  True outward is +Z; the ray travels +Z.
static RayIntersectionGeometric MakeDoubleSidedEntry( Scalar tiltDeg )
{
	const Scalar tiltRad = tiltDeg * PI / 180.0;
	// Reported (already flipped) shading normal: -Z tilted in XZ.
	const Vector3 nReported( sin(tiltRad), 0, -cos(tiltRad) );

	Ray inRay( Point3(0,0,-2), Vector3(0,0,1) );
	RasterizerState rs = {0,0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 2.0;
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = nReported;
	ri.onb.CreateFromW( nReported );
	ri.vGeomNormal = Vector3(0,0,-1);       // flipped toward the ray
	ri.bGeomNormalOrientedToRay = true;     // ... and it says so
	ri.ptCoord = Point2(0.5,0.5);
	return ri;
}

//! P2-b (review): the FRONT-face companion to `MakeDoubleSidedEntry` --
//! the same physical sheet (true outward +Z), but struck from the
//! OPPOSITE side with the ray REVERSED (MakeDoubleSidedEntry's ray
//! origin/direction both negated).  Per the real geometry contract
//! (`TriangleMeshGeometry.cpp`: `ri.bGeomNormalOrientedToRay =
//! bFlipGeomNormal`) a genuine FRONT-face hit needs NO flip -- the
//! reported normal is the true outward direction, tilted by `tiltDeg`,
//! and the flag is false.  Never exercised by a committed test before
//! this row even though it is the ROUTINE case (most double-sided-mesh
//! hits are front-face, not back-face).
static RayIntersectionGeometric MakeDoubleSidedFrontFaceEntry( Scalar tiltDeg )
{
	const Scalar tiltRad = tiltDeg * PI / 180.0;
	const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

	Ray inRay( Point3(0,0,2), Vector3(0,0,-1) );   // MakeDoubleSidedEntry's ray, reversed
	RasterizerState rs = {0,0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 2.0;
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = n;
	ri.onb.CreateFromW( n );
	ri.vGeomNormal = Vector3(0,0,1);        // true outward; no flip on a front-face hit
	ri.bGeomNormalOrientedToRay = false;
	ri.ptCoord = Point2(0.5,0.5);
	return ri;
}

//! A SMOOTH-SHADED closed mesh: the interpolated shading normal deviates
//! from the flat face's geometric normal by a few degrees, with the ray
//! arriving obliquely (the ordinary, un-bump-mapped production case).
static RayIntersectionGeometric MakeSmoothShadedSphereEntry( Scalar deviationDeg )
{
	const Scalar devRad = deviationDeg * PI / 180.0;
	const Vector3 faceNormal( 0, 0, 1 );
	const Vector3 shading( sin(devRad), 0, cos(devRad) );

	// Oblique arrival, still entering (dot(faceNormal, dir) < 0).
	const Vector3 dir = Vector3Ops::Normalize( Vector3( -0.5, 0.2, -1.0 ) );
	Ray inRay( Point3( -dir.x*2.0, -dir.y*2.0, -dir.z*2.0 ), dir );
	RasterizerState rs = {0,0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 2.0;
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = shading;
	ri.onb.CreateFromW( shading );
	ri.vGeomNormal = faceNormal;
	ri.ptCoord = Point2(0.5,0.5);
	return ri;
}

//! "Inside, about to exit" fixture for the interior-backscatter sibling:
//! analytic primitive, interior ray travelling outward, shading normal
//! tilted off the true outward direction.
static RayIntersectionGeometric MakeClosedExit( Scalar tiltDeg )
{
	const Scalar tiltRad = tiltDeg * PI / 180.0;
	const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

	Ray inRay( Point3(0,0,-2), Vector3(0,0,1) );   // travelling OUT of the solid
	RasterizerState rs = {0,0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 2.0;
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = n;
	ri.onb.CreateFromW( n );
	ri.vGeomNormal = Vector3(0,0,1);
	ri.ptCoord = Point2(0.5,0.5);
	return ri;
}

static IORStack MakeOutsideStack( const IObject* obj )
{
	IORStack stack( 1.0 );
	stack.SetCurrentObject( obj );
	return stack;
}

static IORStack MakeInsideStack( const IObject* obj )
{
	IORStack stack( 1.0 );
	stack.SetCurrentObject( obj );
	stack.push( 1.33 );
	stack.SetCurrentObject( obj );
	return stack;
}

namespace
{
	//! One TranslucentSPF plus the painters it owns, released together.
	struct SPFRig
	{
		UniformColorPainter*  front;
		UniformColorPainter*  trans;
		UniformScalarPainter* ext;
		IScalarPainter*       N;
		UniformScalarPainter* scat;
		TranslucentSPF*       spf;

		SPFRig( Scalar extinction, Scalar phongN, Scalar scattering, bool splitN )
		{
			front = new UniformColorPainter( RISEPel(0.3,0.3,0.3) ); front->addref();
			trans = new UniformColorPainter( RISEPel(0.4,0.4,0.4) ); trans->addref();
			ext   = new UniformScalarPainter( extinction );          ext->addref();
			N     = splitN
				? static_cast<IScalarPainter*>( new RGBScalarPainter( 1.0, 7.0, 30.0 ) )
				: static_cast<IScalarPainter*>( new UniformScalarPainter( phongN ) );
			N->addref();
			scat  = new UniformScalarPainter( scattering );          scat->addref();
			spf   = new TranslucentSPF( *front, *trans, *ext, *N, *scat ); spf->addref();
		}
		~SPFRig()
		{
			spf->release(); scat->release(); N->release();
			ext->release(); trans->release(); front->release();
		}
	};
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: entering transmission lobe on a CLOSED object.
//////////////////////////////////////////////////////////////////////
static void TestClosedEntryCensus()
{
	std::cout << "Sub-test 1: entering transmission lobe, closed object, tilt sweep" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();

	struct Pipe { const char* name; int kind; bool splitN; Scalar N; };
	// kind: 0 = RGB isotropic, 1 = RGB per-channel N, 2 = spectral (NM)
	const Pipe pipes[] = {
		{ "RGB  N=1",      0, false, 1.0  },
		{ "RGB  N=20",     0, false, 20.0 },
		{ "RGB  N=1/7/30", 1, true,  0.0  },
		{ "NM   N=1",      2, false, 1.0  },
		{ "NM   N=20",     2, false, 20.0 },
	};
	const int kNumPipes = 5;

	for( int p = 0; p < kNumPipes; p++ ) {
		for( int t = 0; t < kNumTilts; t++ ) {
			SPFRig rig( 0.2, pipes[p].N, 0.0, pipes[p].splitN );
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeClosedEntry( kTiltAnglesDeg[t] );
			IORStack stack = MakeOutsideStack( obj );

			Vector3 axis, clipN;
			EntryLobeFrame( ri, axis, clipN );

			const unsigned int kTrials = 8192;
			unsigned int pushed = 0, wrongSide = 0, pdfMismatch = 0;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( pipes[p].kind == 2 ) {
					rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				} else {
					rig.spf->Scatter( ri, sampler, scattered, stack );
				}
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					if( scattered[i].type != ScatteredRay::eRayTranslucent ) continue;
					if( scattered[i].ior_stack == 0 ) continue;   // entry lobe pushes
					pushed++;
					const Vector3 dir = scattered[i].ray.Dir();
					if( Vector3Ops::Dot( dir, clipN ) <= 0 ) {
						wrongSide++;
					} else {
						// N for this ray: the split-N pipe emits one ray per
						// channel, in channel order, with N = 1 / 7 / 30.
						Scalar rayN = pipes[p].N;
						if( pipes[p].splitN ) {
							const RISEPel k = scattered[i].kray;
							rayN = ( k[0] > 0 ) ? 1.0 : ( ( k[1] > 0 ) ? 7.0 : 30.0 );
						}
						const Scalar expected = ClippedPhongDensity( axis, clipN, rayN, dir );
						if( fabs( scattered[i].pdf - expected ) > 1e-9 * r_max( Scalar(1), expected ) ) {
							pdfMismatch++;
						}
					}
				}
			}

			char label[256];
			std::snprintf( label, sizeof(label),
				"%s tilt=%2g: pushed=%u wrongSide=%u (%.4f) pdfMismatch=%u",
				pipes[p].name, (double)kTiltAnglesDeg[t], pushed, wrongSide,
				pushed ? (double)wrongSide/(double)pushed : 0.0, pdfMismatch );
			std::cout << "  " << label << std::endl;
			EXPECT( pushed > 0, ( std::string(label) + " -- lobe emitted" ).c_str() );
			EXPECT( wrongSide == 0, ( std::string(label) + " -- every pushed ray enters the solid" ).c_str() );
			EXPECT( pdfMismatch == 0, ( std::string(label) + " -- stored pdf matches the closed form" ).c_str() );
		}
	}
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2: FLAT DOUBLE-SIDED sheet, back-face entry.
//
//  At tilt 0 this is a CONTROL (the reported shading normal is already
//  ray-facing, so the pre-fix `FlipW()` happened to land on the correct
//  side): it pins that the lobe is still EMITTED and still crosses the
//  sheet, i.e. that the fix did not repeat DL-45 round 3's "silent total
//  energy loss" by adopting the unflipped reference here.  At tilt 45 it
//  is a genuine red case.
//////////////////////////////////////////////////////////////////////
static void TestDoubleSidedEntry()
{
	std::cout << "Sub-test 2: double-sided sheet, back-face entry" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const Scalar tilts[] = { 0.0, 45.0, 60.0 };

	for( int t = 0; t < 3; t++ ) {
		for( int spectral = 0; spectral < 2; spectral++ ) {
			SPFRig rig( 0.2, 1.0, 0.0, false );
			RandomNumberGenerator rng( 31337 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeDoubleSidedEntry( tilts[t] );
			IORStack stack = MakeOutsideStack( obj );

			// The sheet's TRUE outward is +Z; the ray travels +Z, so
			// "through the sheet" is +Z and the recovered unflipped normal
			// must be +Z (fixture sanity -- this is what makes the
			// ray-anchored and unflipped references DISAGREE here).
			EXPECT( Vector3Ops::Dot( ri.UnflippedGeomNormal(), Vector3(0,0,1) ) > 0.99,
				"double-sided fixture recovers +Z as the true outward normal" );

			Vector3 axis, clipN;
			EntryLobeFrame( ri, axis, clipN );
			EXPECT( Vector3Ops::Dot( clipN, Vector3(0,0,1) ) > 0.99,
				"transmission half-space is 'through the sheet' (+Z), not '-trueOutward'" );

			const unsigned int kTrials = 4096;
			unsigned int pushed = 0, wrongSide = 0;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( spectral ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else           rig.spf->Scatter( ri, sampler, scattered, stack );
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					if( scattered[i].type != ScatteredRay::eRayTranslucent ) continue;
					if( scattered[i].ior_stack == 0 ) continue;
					pushed++;
					if( Vector3Ops::Dot( scattered[i].ray.Dir(), clipN ) <= 0 ) wrongSide++;
				}
			}

			char label[256];
			std::snprintf( label, sizeof(label), "double-sided %s tilt=%g: pushed=%u wrongSide=%u",
				spectral ? "NM " : "RGB", (double)tilts[t], pushed, wrongSide );
			std::cout << "  " << label << std::endl;
			EXPECT( pushed == kTrials, ( std::string(label) + " -- one transmission per trial (no silent loss)" ).c_str() );
			EXPECT( wrongSide == 0, ( std::string(label) + " -- every pushed ray crosses the sheet" ).c_str() );
		}
	}
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 3: SMOOTH-SHADED mesh (interpolated normal), oblique entry.
//////////////////////////////////////////////////////////////////////
static void TestSmoothShadedEntry()
{
	std::cout << "Sub-test 3: smooth-shaded mesh, oblique entry" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const Scalar deviations[] = { 2.0, 8.0, 20.0 };

	for( int d = 0; d < 3; d++ ) {
		for( int spectral = 0; spectral < 2; spectral++ ) {
			SPFRig rig( 0.2, 1.0, 0.0, false );
			RandomNumberGenerator rng( 20260914 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeSmoothShadedSphereEntry( deviations[d] );
			IORStack stack = MakeOutsideStack( obj );

			Vector3 axis, clipN;
			EntryLobeFrame( ri, axis, clipN );

			const unsigned int kTrials = 8192;
			unsigned int pushed = 0, wrongSide = 0;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( spectral ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else           rig.spf->Scatter( ri, sampler, scattered, stack );
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					if( scattered[i].type != ScatteredRay::eRayTranslucent ) continue;
					if( scattered[i].ior_stack == 0 ) continue;
					pushed++;
					if( Vector3Ops::Dot( scattered[i].ray.Dir(), clipN ) <= 0 ) wrongSide++;
				}
			}

			char label[256];
			std::snprintf( label, sizeof(label), "smooth-shaded %s dev=%g deg: pushed=%u wrongSide=%u",
				spectral ? "NM " : "RGB", (double)deviations[d], pushed, wrongSide );
			std::cout << "  " << label << std::endl;
			EXPECT( pushed > 0, ( std::string(label) + " -- lobe emitted" ).c_str() );
			EXPECT( wrongSide == 0, ( std::string(label) + " -- every pushed ray enters the solid" ).c_str() );
		}
	}
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 4 (sibling): interior BACKSCATTER lobe must stay inside.
//////////////////////////////////////////////////////////////////////
static void TestInteriorBackscatterStaysInside()
{
	std::cout << "Sub-test 4: exit-branch interior backscatter lobe (DL-68 sibling)" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();

	for( int splitN = 0; splitN < 2; splitN++ ) {
		for( int spectral = 0; spectral < 2; spectral++ ) {
			if( splitN && spectral ) continue;   // NM has no per-channel branch
			for( int t = 0; t < kNumTilts; t++ ) {
				SPFRig rig( 0.1, 1.0, 0.6, splitN != 0 );
				RandomNumberGenerator rng( 5150 );
				IndependentSampler sampler( rng );

				RayIntersectionGeometric ri = MakeClosedExit( kTiltAnglesDeg[t] );
				IORStack stack = MakeInsideStack( obj );

				Vector3 axis, clipN;
				BackscatterLobeFrame( ri, axis, clipN );

				const unsigned int kTrials = 4096;
				unsigned int back = 0, escaped = 0;
				for( unsigned int trial = 0; trial < kTrials; trial++ ) {
					ScatteredRayContainer scattered;
					if( spectral ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
					else           rig.spf->Scatter( ri, sampler, scattered, stack );
					for( unsigned int i = 0; i < scattered.Count(); i++ ) {
						if( scattered[i].type != ScatteredRay::eRayTranslucent ) continue;
						if( scattered[i].ior_stack != 0 ) continue;   // backscatter leaves the stack alone
						back++;
						if( Vector3Ops::Dot( scattered[i].ray.Dir(), clipN ) <= 0 ) escaped++;
					}
				}

				char label[256];
				std::snprintf( label, sizeof(label),
					"backscatter %s%s tilt=%2g: rays=%u escapedWithoutPop=%u",
					spectral ? "NM " : "RGB", splitN ? " splitN" : "",
					(double)kTiltAnglesDeg[t], back, escaped );
				std::cout << "  " << label << std::endl;
				EXPECT( back > 0, ( std::string(label) + " -- backscatter emitted" ).c_str() );
				EXPECT( escaped == 0, ( std::string(label) + " -- no backscatter ray leaves the solid" ).c_str() );
			}
		}
	}
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 5: the clipped-Phong density is NORMALIZED (integrates to 1
//  over the sphere) at every tilt and every N.
//////////////////////////////////////////////////////////////////////
static void TestDensityNormalization()
{
	std::cout << "Sub-test 5: unit-energy integral of the clipped-Phong density" << std::endl;

	const Scalar Ns[] = { 1.0, 7.0, 30.0 };
	const int kThetaSteps = 400;
	const int kPhiSteps = 800;

	for( int t = 0; t < kNumTilts; t++ ) {
		RayIntersectionGeometric ri = MakeClosedEntry( kTiltAnglesDeg[t] );
		Vector3 axis, clipN;
		EntryLobeFrame( ri, axis, clipN );
		OrthonormalBasis3D onb;
		onb.CreateFromW( axis );

		for( int k = 0; k < 3; k++ ) {
			Scalar integral = 0;
			for( int ti = 0; ti < kThetaSteps; ti++ ) {
				const Scalar theta0 = PI * Scalar(ti) / kThetaSteps;
				const Scalar theta1 = PI * Scalar(ti+1) / kThetaSteps;
				const Scalar thetaMid = 0.5*(theta0+theta1);
				const Scalar dTheta = theta1 - theta0;
				const Scalar sinThetaMid = sin(thetaMid);
				for( int pj = 0; pj < kPhiSteps; pj++ ) {
					const Scalar psi = TWO_PI * (Scalar(pj)+0.5) / kPhiSteps;
					const Scalar dPsi = TWO_PI / kPhiSteps;
					const Vector3 w = onb.u()*(sinThetaMid*cos(psi))
						+ onb.v()*(sinThetaMid*sin(psi))
						+ axis*cos(thetaMid);
					integral += ClippedPhongDensity( axis, clipN, Ns[k], w ) * sinThetaMid * dTheta * dPsi;
				}
			}
			char label[200];
			std::snprintf( label, sizeof(label), "tilt=%2g N=%g: integral(q dOmega)=%.5f",
				(double)kTiltAnglesDeg[t], (double)Ns[k], (double)integral );
			std::cout << "  " << label << std::endl;
			EXPECT( fabs(integral - 1.0) < 0.01, label );
		}
	}
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 6: chi-squared goodness of fit -- the SAMPLER really does
//  produce the density it reports (an exactness check the per-sample
//  "stored pdf == closed form" comparison cannot make).
//////////////////////////////////////////////////////////////////////
static void TestSampledDirectionChiSquared()
{
	std::cout << "Sub-test 6: chi-squared of sampled entry directions vs. the density" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const Scalar tilts[] = { 30.0, 60.0 };
	const Scalar Ns[] = { 1.0, 7.0 };

	for( int t = 0; t < 2; t++ ) {
		for( int k = 0; k < 2; k++ ) {
			SPFRig rig( 0.2, Ns[k], 0.0, false );
			RandomNumberGenerator rng( 987654321 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeClosedEntry( tilts[t] );
			IORStack stack = MakeOutsideStack( obj );
			Vector3 axis, clipN;
			EntryLobeFrame( ri, axis, clipN );
			OrthonormalBasis3D onb;
			onb.CreateFromW( axis );

			const int kMuBins = 16;
			const int kPsiBins = 12;
			const int kBins = kMuBins * kPsiBins;
			const int kSub = 24;
			const int kSamples = 400000;

			std::vector<Scalar> expectedProb( kBins, 0 );
			std::vector<bool> straddles( kBins, false );
			const Scalar dMu = Scalar(1) / kMuBins;
			const Scalar dPsi = TWO_PI / kPsiBins;
			const Scalar subArea = ( dMu / kSub ) * ( dPsi / kSub );
			for( int b = 0; b < kBins; b++ ) {
				const int mi = b / kPsiBins;
				const int pi_ = b % kPsiBins;
				Scalar prob = 0;
				bool anyZero = false, anyPositive = false;
				for( int sm = 0; sm < kSub; sm++ ) {
					const Scalar mu = mi*dMu + (sm+0.5)*dMu/kSub;
					const Scalar s = sqrt( r_max( Scalar(0), Scalar(1)-mu*mu ) );
					for( int sp = 0; sp < kSub; sp++ ) {
						const Scalar psi = pi_*dPsi + (sp+0.5)*dPsi/kSub;
						const Vector3 w = onb.u()*(s*cos(psi)) + onb.v()*(s*sin(psi)) + axis*mu;
						const Scalar q = ClippedPhongDensity( axis, clipN, Ns[k], w );
						if( q > 0 ) anyPositive = true; else anyZero = true;
						prob += q * subArea;
					}
				}
				expectedProb[b] = prob;
				// A bin the geometric horizon cuts through has a
				// DISCONTINUOUS integrand; its sub-quadrature carries a
				// first-order error that has nothing to do with the
				// sampler.  Exclude it (mirrors TranslucentTiltedExitTest
				// sub-test 4's own straddle exclusion).
				straddles[b] = anyZero && anyPositive;
			}

			std::vector<int> observed( kBins, 0 );
			int collected = 0;
			while( collected < kSamples ) {
				ScatteredRayContainer scattered;
				rig.spf->Scatter( ri, sampler, scattered, stack );
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					if( scattered[i].type != ScatteredRay::eRayTranslucent ) continue;
					if( scattered[i].ior_stack == 0 ) continue;
					const Vector3 w = scattered[i].ray.Dir();
					const Scalar mu = Vector3Ops::Dot( w, axis );
					if( mu <= 0 || mu >= 1 ) continue;
					Scalar psi = atan2( Vector3Ops::Dot(w,onb.v()), Vector3Ops::Dot(w,onb.u()) );
					if( psi < 0 ) psi += TWO_PI;
					int mi = int( mu / dMu );  if( mi >= kMuBins ) mi = kMuBins-1;
					int pi_ = int( psi / dPsi ); if( pi_ >= kPsiBins ) pi_ = kPsiBins-1;
					observed[ mi*kPsiBins + pi_ ]++;
					collected++;
				}
			}

			// Pool bins whose expected count is too small for the
			// chi-squared approximation, and skip bins the horizon cuts
			// through (their expected probability is a quadrature of a
			// discontinuous integrand).
			Scalar chi2 = 0;
			int dof = 0;
			for( int b = 0; b < kBins; b++ ) {
				if( straddles[b] ) continue;
				const Scalar e = expectedProb[b] * collected;
				if( e < 20 ) continue;
				const Scalar d = observed[b] - e;
				chi2 += d*d/e;
				dof++;
			}
			const Scalar reduced = dof > 0 ? chi2/dof : 0;
			char label[220];
			std::snprintf( label, sizeof(label),
				"tilt=%g N=%g: samples=%d bins=%d chi2/dof=%.3f",
				(double)tilts[t], (double)Ns[k], collected, dof, (double)reduced );
			std::cout << "  " << label << std::endl;
			EXPECT( dof > 40, ( std::string(label) + " -- enough usable bins" ).c_str() );
			// A correct sampler gives chi2/dof ~ 1; a mis-normalized or
			// mis-shaped one at 400k samples per configuration lands far
			// above this band.
			EXPECT( reduced < 1.6, ( std::string(label) + " -- sampled histogram matches the reported density" ).c_str() );
		}
	}
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 7 (P2-b): double-sided sheet, FRONT-face entry.  Never
//  exercised by a committed test even though it is the ROUTINE
//  double-sided-mesh case -- sub-test 2 above only covers the BACK
//  face.  Reviewer-measured base (pre-fix): 2014/4096 wrong-side at
//  89 deg tilt, matching (1-cos(phi))/2 like every other row.
//////////////////////////////////////////////////////////////////////
static void TestDoubleSidedFrontFaceEntry()
{
	std::cout << "Sub-test 7: double-sided sheet, FRONT-face entry (P2-b)" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const Scalar tilts[] = { 0.0, 45.0, 89.0 };

	for( int t = 0; t < 3; t++ ) {
		for( int spectral = 0; spectral < 2; spectral++ ) {
			SPFRig rig( 0.2, 1.0, 0.0, false );
			RandomNumberGenerator rng( 246813579 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeDoubleSidedFrontFaceEntry( tilts[t] );
			IORStack stack = MakeOutsideStack( obj );

			// Fixture sanity: a genuine front-face hit needs no un-flip --
			// the reported normal already IS the true outward direction.
			EXPECT( Vector3Ops::Dot( ri.UnflippedGeomNormal(), Vector3(0,0,1) ) > 0.99,
				"front-face fixture recovers +Z as the true outward normal (no flip)" );

			Vector3 axis, clipN;
			EntryLobeFrame( ri, axis, clipN );

			const unsigned int kTrials = 4096;
			unsigned int pushed = 0, wrongSide = 0;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( spectral ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else           rig.spf->Scatter( ri, sampler, scattered, stack );
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					if( scattered[i].type != ScatteredRay::eRayTranslucent ) continue;
					if( scattered[i].ior_stack == 0 ) continue;
					pushed++;
					if( Vector3Ops::Dot( scattered[i].ray.Dir(), clipN ) <= 0 ) wrongSide++;
				}
			}

			char label[256];
			std::snprintf( label, sizeof(label),
				"double-sided FRONT %s tilt=%g: pushed=%u wrongSide=%u (%.4f)",
				spectral ? "NM " : "RGB", (double)tilts[t], pushed, wrongSide,
				pushed ? (double)wrongSide/(double)pushed : 0.0 );
			std::cout << "  " << label << std::endl;
			EXPECT( pushed > 0, ( std::string(label) + " -- lobe emitted" ).c_str() );
			EXPECT( wrongSide == 0, ( std::string(label) + " -- every pushed ray enters the solid" ).c_str() );
		}
	}
	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 8 (P2-b): double-sided sheet, EXIT interior backscatter.
//  `MakeDoubleSidedEntry`'s own back-face-style bookkeeping
//  (`bGeomNormalOrientedToRay=true`, flipped `vGeomNormal`) paired with
//  an INSIDE stack instead of the entry-side Outside stack -- this
//  drives the EXIT branch's own un-flip recovery path, a genuinely
//  different code path from sub-test 4's non-flipped `MakeClosedExit`.
//  Reviewer-measured base (pre-fix): 2004/4096 escaping at 89 deg tilt.
//////////////////////////////////////////////////////////////////////
static void TestDoubleSidedExitBackscatter()
{
	std::cout << "Sub-test 8: double-sided sheet, EXIT interior backscatter (P2-b)" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const Scalar tilts[] = { 0.0, 45.0, 89.0 };

	for( int t = 0; t < 3; t++ ) {
		for( int spectral = 0; spectral < 2; spectral++ ) {
			SPFRig rig( 0.1, 1.0, 0.6, false );
			RandomNumberGenerator rng( 135792468 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeDoubleSidedEntry( tilts[t] );
			IORStack stack = MakeInsideStack( obj );

			Vector3 axis, clipN;
			BackscatterLobeFrame( ri, axis, clipN );

			const unsigned int kTrials = 4096;
			unsigned int back = 0, escaped = 0;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( spectral ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else           rig.spf->Scatter( ri, sampler, scattered, stack );
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					if( scattered[i].type != ScatteredRay::eRayTranslucent ) continue;
					if( scattered[i].ior_stack != 0 ) continue;   // backscatter leaves the stack alone
					back++;
					if( Vector3Ops::Dot( scattered[i].ray.Dir(), clipN ) <= 0 ) escaped++;
				}
			}

			char label[256];
			std::snprintf( label, sizeof(label),
				"double-sided EXIT %s tilt=%g: rays=%u escapedWithoutPop=%u",
				spectral ? "NM " : "RGB", (double)tilts[t], back, escaped );
			std::cout << "  " << label << std::endl;
			EXPECT( back > 0, ( std::string(label) + " -- backscatter emitted" ).c_str() );
			EXPECT( escaped == 0, ( std::string(label) + " -- no backscatter ray leaves the solid" ).c_str() );
		}
	}
	obj->release();
}

int main()
{
	std::cout << "TranslucentEntryHorizonTest (DL-68)" << std::endl;

	TestClosedEntryCensus();
	TestDoubleSidedEntry();
	TestSmoothShadedEntry();
	TestInteriorBackscatterStaysInside();
	TestDensityNormalization();
	TestSampledDirectionChiSquared();
	TestDoubleSidedFrontFaceEntry();
	TestDoubleSidedExitBackscatter();

	std::cout << "checks=" << checks << " failed=" << failed << std::endl;
	if( failed ) {
		std::cout << "TranslucentEntryHorizonTest FAILED" << std::endl;
		return 1;
	}
	std::cout << "TranslucentEntryHorizonTest PASSED" << std::endl;
	return 0;
}
