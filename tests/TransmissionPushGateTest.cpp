//////////////////////////////////////////////////////////////////////
//
//  TransmissionPushGateTest.cpp - Red-proof and regression guard for
//    DL-111 (DielectricSPF / PerfectRefractorSPF /
//    SubSurfaceScatteringSPF transition the IOR stack on a transmission
//    lobe whose direction is never checked against the GEOMETRIC
//    horizon) and DL-112 (TranslucentSPF's entry front-reflection lobe
//    DROPS below-horizon samples instead of renormalizing, with an
//    unoriented axis).
//
//  DL-111 -- THE PATTERN (DL-68's, in three other files)
//
//    A lobe that PUSHES (or carries a POPPED) `IORStack` is built from
//    the SHADING normal, which a bump / normal map or `GlintModifier`
//    (up to 60 deg) moves off the surface, so the continuation can
//    travel to the side OPPOSITE the one the membership change claims.
//    A later hit on the same object is then misclassified as the
//    opposite crossing -- the DL-03 / DL-45 / DL-68 failure mode.  In
//    each of the three files the COMPANION Fresnel / reflection lobe in
//    the same function IS gated against a geometric normal, which is
//    what makes the omission legible.
//
//    The invariant every sub-test below asserts is one line:
//
//      a scattered ray whose `ior_stack` differs from the caller's
//      must satisfy  Dot(dir, geomN) < 0,
//
//    with `geomN` the RAY-ANCHORED true geometric normal (flipped to
//    oppose `ri.ray.Dir()`), recovered through `HasTrueGeomSide()`.
//    That single form covers BOTH crossings: on entry `geomN` is the
//    outward normal and the transmission must go in; on exit `geomN` is
//    the inward normal and the transmission must go out.  It is the
//    exact complement of the reflection lobes' existing
//    `Dot(dir, geomN) > 0` gate.
//
//  DL-111 -- THE FIX (and why it is NOT DL-68's clip, mostly)
//
//    These are DELTA Snell lobes: there is no lobe to renormalize.  The
//    ruling adopted (see docs/DL111_DL112_TRANSMISSION_PUSH_GATES.md
//    for the full derivation) is to RE-DERIVE THE REFRACTION ABOUT THE
//    TRUE GEOMETRIC NORMAL when the shading normal's Snell result is
//    geometrically impossible -- the treatment these same functions
//    ALREADY apply to their own mandatory (TIR) reflections.  It
//    preserves energy exactly, keeps the event a genuine refraction
//    obeying Snell's law at the true interface with the same eta (so a
//    dispersion fan stays a fan), and CANNOT fail the gate: a
//    refraction about N always lands on the far side of N.
//
//    `DielectricSPF`'s `scattering` / HG warp is the one non-delta
//    case, and there DL-68's construction applies verbatim: the warp is
//    azimuthally uniform about the Snell axis at fixed polar angle, so
//    clipping the AZIMUTH to the valid arc (`GeometricUtilities::
//    PerturbClipped`) is exact and costs no extra canonical draw.
//
//  DL-112 -- THE PATTERN
//
//    `TranslucentSPF`'s entry front-reflection lobe already HAS its
//    geometric gate and makes no stack claim, so nothing is
//    misclassified; the residual is ENERGY (the gate DROPS the sample
//    rather than resampling, so the lobe integrates to P(valid) < 1
//    under tilt) and AXIS (the lobe axis is the raw `n`, never oriented
//    into `geomN`'s hemisphere, so P(valid) is not bounded below by
//    0.5).  Sub-test 8 measures the emitted energy directly: 0.3 (the
//    reflectance painter's value) at every tilt after the fix,
//    0.3*(1+cos(phi))/2 before it.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `pushgates`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/Materials/PerfectRefractorSPF.h"
#include "../src/Library/Materials/SubSurfaceScatteringSPF.h"
#include "../src/Library/Materials/TranslucentSPF.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"
#include "../src/Library/Utilities/Reference.h"

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

	//! The ray-anchored TRUE geometric normal -- the reference every
	//! crossing test in this file uses.  Transcribed from the same
	//! recovery `TranslucentSPF` performs (DL-70's
	//! `HasTrueGeomSide()` / raw-field fallback), so that the test
	//! states the invariant in terms of the intersection record alone
	//! and never asks the SPF what it thinks the normal is.
	Vector3 RayAnchoredGeomN( const RayIntersectionGeometric& ri )
	{
		const Vector3 n = ri.onb.w();
		const Vector3 trueGeom = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
		const Vector3 raw = ( Vector3Ops::SquaredModulus(trueGeom) > Scalar(1e-12) ) ? trueGeom : n;
		return ( Vector3Ops::Dot( raw, ri.ray.Dir() ) < 0 ) ? raw : -raw;
	}

	//! Fixtures.  Same four shapes DL-68's `TranslucentEntryHorizonTest`
	//! uses, reproduced here so the two suites can be read (and can
	//! drift) independently.

	//! A CLOSED object (analytic-primitive convention: the reported
	//! geometric normal is the object's true outward direction and is NOT
	//! flipped toward the ray) struck from OUTSIDE, shading normal tilted
	//! `tiltDeg` off outward in the XZ plane.
	RayIntersectionGeometric MakeClosedEntry( Scalar tiltDeg )
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

	//! The same closed object, struck from INSIDE (interior ray
	//! travelling outward).  The analytic convention keeps the reported
	//! geometric normal outward.
	RayIntersectionGeometric MakeClosedExit( Scalar tiltDeg )
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

	//! A FLAT DOUBLE-SIDED sheet struck on its BACK face: the geometry
	//! has already flipped BOTH normals toward the incoming ray and
	//! recorded that in `bGeomNormalOrientedToRay`.  True outward is +Z;
	//! the ray travels +Z.
	RayIntersectionGeometric MakeDoubleSidedBackEntry( Scalar tiltDeg )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
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

	//! The FRONT-face companion: the same physical sheet (true outward
	//! +Z), struck from the other side.  A genuine front-face hit needs
	//! no flip, so the flag is false.
	RayIntersectionGeometric MakeDoubleSidedFrontEntry( Scalar tiltDeg )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

		Ray inRay( Point3(0,0,2), Vector3(0,0,-1) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );

		ri.bHit = true;
		ri.range = 2.0;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = n;
		ri.onb.CreateFromW( n );
		ri.vGeomNormal = Vector3(0,0,1);
		ri.bGeomNormalOrientedToRay = false;
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	//! A double-sided sheet struck from INSIDE (the second interface of
	//! a double-sided slab).  The geometry flips both normals toward the
	//! ray: the ray travels +Z (outward), so the reported normals face
	//! -Z while the true outward direction is +Z.
	RayIntersectionGeometric MakeDoubleSidedExit( Scalar tiltDeg )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		const Vector3 nReported( sin(tiltRad), 0, -cos(tiltRad) );

		Ray inRay( Point3(0,0,-2), Vector3(0,0,1) );   // travelling OUT
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );

		ri.bHit = true;
		ri.range = 2.0;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = nReported;
		ri.onb.CreateFromW( nReported );
		ri.vGeomNormal = Vector3(0,0,-1);
		ri.bGeomNormalOrientedToRay = true;
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	//! The fixture that can actually reach a WRONG-SIDE **delta** Snell
	//! refraction, and the derivation of why the simpler ones cannot.
	//!
	//! Work in the plane, angles measured from the true outward geometric
	//! normal, positive toward +x.  Write `phi` for the shading-normal
	//! tilt and `delta` for the ray's own angle off the geometric normal
	//! (the arrival direction on entry, the departure direction on exit).
	//! The incidence angle in the SHADING frame is |delta - phi|, and the
	//! refracted direction sits at `thetaT` from the shading normal on
	//! the same side as the ray, so the emitted direction's angle off the
	//! geometric normal is
	//!
	//!     phi + sign(delta - phi) * thetaT,
	//!
	//! and the crossing test `Dot(t, geomN) < 0` (entry) / `> 0` (exit) is
	//! exactly `|phi + sign(delta-phi)*thetaT| < 90 deg`.
	//!
	//! When the ray refracts into a DENSER medium, `thetaT < |delta-phi|`,
	//! so the emitted direction is angularly BETWEEN the incoming ray and
	//! the shading normal's far side -- both of which are already inside
	//! the (convex) crossing half-space -- and the test cannot fail.
	//!
	//! **THAT ARGUMENT HAS A PREMISE, and the review's P2-1 found it is
	//! not always true**: it assumes the tilted shading normal still
	//! OPPOSES the incoming ray, `Dot(d, n_s) < 0`, i.e. that the incoming
	//! ray is inside the half-space the transmitted direction is being
	//! compared against.  `Optics::CalculateRefractedRay` FLIPS the normal
	//! internally to restore its own sign convention, so once the tilt
	//! carries `n_s` past the grazing ray (`|phi| + |delta| > 90` with
	//! opposite signs -- a bump / normal map or `GlintModifier` at a
	//! silhouette; `ReliefModifier` is explicitly NOT a horizon clamp) the
	//! refraction is built about `-n_s`, whose far side is the side the
	//! ray CAME FROM, and a DENSER-medium ENTRY lands wrong-side too.
	//! Closed form here at (delta 89, tilt -30), air->glass 1.5:
	//! `t = (-0.911, 0, +0.412)`, above the surface.  Sub-test 2c sweeps
	//! it; **entering glass from air at a bump-mapped silhouette was 100%
	//! broken pre-fix** (`DielectricSPF` emitted NOTHING -- both lobes
	//! wrong-side -- and `PerfectRefractorSPF` pushed 64/64 rays the wrong
	//! way).
	//!
	//! So the two reachable families are:
	//!   * NEGATIVE tilt (`Dot(d, n_s) > 0`): any index pair, sub-test 2c;
	//!   * POSITIVE tilt with `thetaT > |delta-phi|`, i.e. refraction into
	//!     a RARER medium -- an ordinary glass->air EXIT, or an ENTRY into
	//!     a bubble (an `ior 1.0` object inside a glass block) -- with
	//!     `phi` and `thetaT` ADDING: sub-test 2b.
	//!
	//! `bExit == false` builds the entry shape (ray arriving from angle
	//! `deltaDeg`), `true` the exit shape (interior ray leaving at
	//! `deltaDeg`).  `tiltDeg` is SIGNED: positive tilts the shading
	//! normal the same way the ray leans, negative the other way (which is
	//! what produces the back-facing `Dot(d, n_s) > 0` configuration).
	//! Analytic-primitive convention: no double-sided flip.
	RayIntersectionGeometric MakeObliqueHit( Scalar deltaDeg, Scalar tiltDeg, bool bExit )
	{
		const Scalar d = deltaDeg * PI / 180.0;
		const Scalar f = tiltDeg  * PI / 180.0;
		const Vector3 n( sin(f), 0, cos(f) );
		const Vector3 dir = bExit
			? Vector3(  sin(d), 0,  cos(d) )     // interior ray leaving
			: Vector3( -sin(d), 0, -cos(d) );    // exterior ray arriving

		Ray inRay( Point3( -dir.x*2.0, -dir.y*2.0, -dir.z*2.0 ), dir );
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

	//! A SMOOTH-SHADED closed mesh: the interpolated shading normal
	//! deviates from the flat face's geometric normal by a few degrees,
	//! ray arriving obliquely.  No bump map, no glint -- the ordinary
	//! production case.
	RayIntersectionGeometric MakeSmoothShadedEntry( Scalar deviationDeg )
	{
		const Scalar devRad = deviationDeg * PI / 180.0;
		const Vector3 faceNormal( 0, 0, 1 );
		const Vector3 shading( sin(devRad), 0, cos(devRad) );

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

	IORStack MakeOutsideStack( const IObject* obj, Scalar ambient = 1.0 )
	{
		IORStack stack( ambient );
		stack.SetCurrentObject( obj );
		return stack;
	}

	IORStack MakeInsideStack( const IObject* obj, Scalar ior )
	{
		IORStack stack( 1.0 );
		stack.SetCurrentObject( obj );
		stack.push( ior );
		stack.SetCurrentObject( obj );
		return stack;
	}

	//! One census result: how many emitted rays transitioned the stack,
	//! and how many of those went the wrong way.
	struct Census
	{
		unsigned int transitioned;
		unsigned int wrongSide;
		unsigned int reflections;
		unsigned int reflectionsWrongSide;
		Census() : transitioned(0), wrongSide(0), reflections(0), reflectionsWrongSide(0) {}
	};

	//! Tally one scattered-ray container against the invariant.  A ray
	//! is "transitioning" when it carries its own `ior_stack` whose
	//! depth differs from the caller's (a push or a pop); the Fresnel
	//! lobes that merely COPY the caller's stack are excluded by the
	//! depth comparison, and tallied separately as reflections.
	void Tally( const ScatteredRayContainer& scattered,
		const RayIntersectionGeometric& ri, const IORStack& callerStack,
		Census& c )
	{
		const Vector3 geomN = RayAnchoredGeomN( ri );
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			const ScatteredRay& s = scattered[i];
			const Vector3 dir = s.ray.Dir();
			const bool transitioned = ( s.ior_stack != 0 )
				&& ( s.ior_stack->containsCurrent() != callerStack.containsCurrent() );
			if( transitioned ) {
				c.transitioned++;
				if( Vector3Ops::Dot( dir, geomN ) >= 0 ) c.wrongSide++;
			} else if( s.type == ScatteredRay::eRayReflection ) {
				c.reflections++;
				if( Vector3Ops::Dot( dir, geomN ) <= 0 ) c.reflectionsWrongSide++;
			}
		}
	}

	//! DielectricSPF plus the painters it owns.
	struct DielectricRig
	{
		IScalarPainter*  tau;
		IScalarPainter*  ior;
		IScalarPainter*  scat;
		DielectricSPF*   spf;

		DielectricRig( Scalar scattering, bool disperse, bool hg, Scalar iorValue = 1.5 )
		{
			tau  = new UniformScalarPainter( 0.9 ); tau->addref();
			ior  = disperse
				? static_cast<IScalarPainter*>( new RGBScalarPainter( 1.50, 1.52, 1.55 ) )
				: static_cast<IScalarPainter*>( new UniformScalarPainter( iorValue ) );
			ior->addref();
			scat = new UniformScalarPainter( scattering ); scat->addref();
			spf  = new DielectricSPF( *tau, *ior, *scat, hg, 0, 0, 0, 0 ); spf->addref();
		}
		~DielectricRig() { spf->release(); scat->release(); ior->release(); tau->release(); }
	};

	struct RefractorRig
	{
		UniformColorPainter* refractivity;
		IScalarPainter*      ior;
		PerfectRefractorSPF* spf;

		RefractorRig( bool disperse, Scalar iorValue = 1.5 )
		{
			refractivity = new UniformColorPainter( RISEPel(1,1,1) ); refractivity->addref();
			ior = disperse
				? static_cast<IScalarPainter*>( new RGBScalarPainter( 1.50, 1.52, 1.55 ) )
				: static_cast<IScalarPainter*>( new UniformScalarPainter( iorValue ) );
			ior->addref();
			spf = new PerfectRefractorSPF( *refractivity, *ior ); spf->addref();
		}
		~RefractorRig() { spf->release(); ior->release(); refractivity->release(); }
	};

	struct SSSRig
	{
		UniformScalarPainter*     ior;
		SubSurfaceScatteringSPF*  spf;

		SSSRig()
		{
			ior = new UniformScalarPainter( 1.4 ); ior->addref();
			// bAbsorbBackFace = false: the standalone non-absorbing
			// fallback is the branch that emits the exit refraction at
			// all (shipped SSS materials set true and return early --
			// DL-51 records the same scoping).
			spf = new SubSurfaceScatteringSPF( *ior, 0.0, 0.0, false ); spf->addref();
		}
		~SSSRig() { spf->release(); ior->release(); }
	};

	struct TranslucentRig
	{
		UniformColorPainter*  front;
		UniformColorPainter*  trans;
		UniformScalarPainter* ext;
		UniformScalarPainter* N;
		UniformScalarPainter* scat;
		TranslucentSPF*       spf;

		TranslucentRig()
		{
			front = new UniformColorPainter( RISEPel(0.3,0.3,0.3) ); front->addref();
			trans = new UniformColorPainter( RISEPel(0.4,0.4,0.4) ); trans->addref();
			ext   = new UniformScalarPainter( 0.2 ); ext->addref();
			N     = new UniformScalarPainter( 1.0 ); N->addref();
			scat  = new UniformScalarPainter( 0.0 ); scat->addref();
			spf   = new TranslucentSPF( *front, *trans, *ext, *N, *scat ); spf->addref();
		}
		~TranslucentRig()
		{
			spf->release(); scat->release(); N->release();
			ext->release(); trans->release(); front->release();
		}
	};

	//! A sampler that counts how many canonical numbers were drawn, so
	//! the fixed-dimension-budget invariant
	//! (`ISampler::HasFixedDimensionBudget()`) can be asserted directly:
	//! no SPF this row touches may vary its draw count with geometry.
	class CountingSampler : public ISampler
	{
	public:
		CountingSampler( const RandomNumberGenerator& rng ) : inner( rng ), count( 0 ) {}
		Scalar Get1D() { count++; return inner.Get1D(); }
		Point2 Get2D() { count += 2; return inner.Get2D(); }
		void   StartStream( int d ) { inner.StartStream(d); }
		bool   HasFixedDimensionBudget() const { return inner.HasFixedDimensionBudget(); }

		IndependentSampler inner;
		unsigned int count;
	};
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: DielectricSPF -- entering, closed object, tilt sweep.
//////////////////////////////////////////////////////////////////////
static void TestDielectricEntry()
{
	std::cout << "Sub-test 1: DielectricSPF entering transmission, closed object" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();

	// `scattering` sweep: 1e6 is the documented delta pass-through (the
	// Phong warp is switched off entirely at >= 1e6), 100 a narrow warp,
	// 1.0 a wide one.  0.0 is "maximally DIFFUSE transmission" per
	// CLAUDE.md, the widest warp of all.
	struct Pipe { const char* name; Scalar scattering; bool disperse; bool hg; int kind; };
	// kind: 0 = RGB Scatter(), 1 = ScatterNM()
	const Pipe pipes[] = {
		{ "RGB delta   (scat 1e6)", 1000000.0, false, false, 0 },
		{ "RGB narrow  (scat 100)",     100.0, false, false, 0 },
		{ "RGB wide    (scat 1)",         1.0, false, false, 0 },
		{ "RGB diffuse (scat 0)",         0.0, false, false, 0 },
		{ "RGB disperse(scat 1e6)", 1000000.0, true,  false, 0 },
		{ "RGB disperse(scat 1)",         1.0, true,  false, 0 },
		{ "RGB HG      (g 0.8)",          0.8, false, true,  0 },
		{ "NM  delta   (scat 1e6)", 1000000.0, false, false, 1 },
		{ "NM  wide    (scat 1)",         1.0, false, false, 1 },
	};
	const int kNumPipes = 9;
	const unsigned int kTrials = 4096;

	for( int p = 0; p < kNumPipes; p++ ) {
		for( int t = 0; t < kNumTilts; t++ ) {
			DielectricRig rig( pipes[p].scattering, pipes[p].disperse, pipes[p].hg );
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeClosedEntry( kTiltAnglesDeg[t] );
			IORStack stack = MakeOutsideStack( obj );

			Census c;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( pipes[p].kind == 1 ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else                     rig.spf->Scatter( ri, sampler, scattered, stack );
				Tally( scattered, ri, stack, c );
			}

			char msg[400];
			snprintf( msg, sizeof(msg), "DielectricSPF entry %s tilt %.0f: %u/%u pushed rays NOT into the solid",
				pipes[p].name, (double)kTiltAnglesDeg[t], c.wrongSide, c.transitioned );
			EXPECT( c.wrongSide == 0, msg );
			if( c.wrongSide ) {
				std::cout << "      (census: transitioned " << c.transitioned
				          << ", wrong-side " << c.wrongSide
				          << ", frac " << (double)c.wrongSide/(double)r_max(1u,c.transitioned) << ")" << std::endl;
			}
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2: DielectricSPF -- exiting, closed object, tilt sweep.
//////////////////////////////////////////////////////////////////////
static void TestDielectricExit()
{
	std::cout << "Sub-test 2: DielectricSPF exiting transmission, closed object" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();

	struct Pipe { const char* name; Scalar scattering; int kind; };
	const Pipe pipes[] = {
		{ "RGB delta (scat 1e6)", 1000000.0, 0 },
		{ "RGB wide  (scat 1)",         1.0, 0 },
		{ "NM  delta (scat 1e6)", 1000000.0, 1 },
	};
	const int kNumPipes = 3;
	const unsigned int kTrials = 4096;

	for( int p = 0; p < kNumPipes; p++ ) {
		for( int t = 0; t < kNumTilts; t++ ) {
			DielectricRig rig( pipes[p].scattering, false, false );
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeClosedExit( kTiltAnglesDeg[t] );
			IORStack stack = MakeInsideStack( obj, 1.5 );

			Census c;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( pipes[p].kind == 1 ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else                     rig.spf->Scatter( ri, sampler, scattered, stack );
				Tally( scattered, ri, stack, c );
			}

			char msg[400];
			snprintf( msg, sizeof(msg), "DielectricSPF exit %s tilt %.0f: %u/%u popped rays NOT out of the solid",
				pipes[p].name, (double)kTiltAnglesDeg[t], c.wrongSide, c.transitioned );
			EXPECT( c.wrongSide == 0, msg );
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2b: the DELTA refraction at a refractive silhouette.
//
//  The only configuration a pure Snell lobe can get wrong (see
//  MakeObliqueHit's derivation): refraction into a RARER medium, with
//  the shading tilt and the refracted deviation ADDING.  Two physical
//  instances, both ordinary: an `ior 1.5` object's glass->air EXIT at a
//  grazing angle, and the ENTRY into a bubble (an `ior 1.0` object
//  inside a glass block, ambient 1.6).  `scattering` is the delta
//  pass-through 1e6 throughout, so nothing but Snell is involved.
//////////////////////////////////////////////////////////////////////
static void TestDeltaRefractionAtSilhouette()
{
	std::cout << "Sub-test 2b: DELTA refraction at a refractive silhouette (into a rarer medium)" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 256;
	const Scalar deltas[] = { 70.0, 80.0, 85.0 };
	const Scalar tilts[]  = { 30.0, 45.0, 60.0 };

	struct Row { const char* name; int spf; bool bExit; Scalar objIOR; Scalar ambient; };
	const Row rows[] = {
		{ "Dielectric       glass->air exit", 0, true,  1.5, 1.0 },
		{ "Dielectric       bubble entry",    0, false, 1.0, 1.6 },
		{ "PerfectRefractor glass->air exit", 1, true,  1.5, 1.0 },
		{ "PerfectRefractor bubble entry",    1, false, 1.0, 1.6 },
		{ "SSS              exit",            2, true,  1.4, 1.0 },
	};
	const int kNumRows = 5;

	for( int r = 0; r < kNumRows; r++ ) {
		for( int di = 0; di < 3; di++ ) {
			for( int ti = 0; ti < 3; ti++ ) {
				RayIntersectionGeometric ri = MakeObliqueHit( deltas[di], tilts[ti], rows[r].bExit );
				IORStack stack = rows[r].bExit
					? MakeInsideStack( obj, rows[r].objIOR )
					: MakeOutsideStack( obj, rows[r].ambient );

				RandomNumberGenerator rng( 777 );
				IndependentSampler sampler( rng );

				DielectricRig dr( 1000000.0, false, false, rows[r].objIOR );
				RefractorRig  rr( false, rows[r].objIOR );
				SSSRig        sr;

				Census c;
				for( unsigned int trial = 0; trial < kTrials; trial++ ) {
					ScatteredRayContainer scattered;
					switch( rows[r].spf ) {
					case 0:  dr.spf->Scatter( ri, sampler, scattered, stack ); break;
					case 1:  rr.spf->Scatter( ri, sampler, scattered, stack ); break;
					default: sr.spf->Scatter( ri, sampler, scattered, stack ); break;
					}
					Tally( scattered, ri, stack, c );
				}

				char msg[400];
				snprintf( msg, sizeof(msg), "%s delta %.0f tilt %.0f: %u/%u transitioning rays on the wrong side",
					rows[r].name, (double)deltas[di], (double)tilts[ti], c.wrongSide, c.transitioned );
				EXPECT( c.wrongSide == 0, msg );
			}
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2c (review P2-1): the DENSER-medium entry is NOT safe.
//
//  Sub-test 2b's derivation (and the slice's original doc and source
//  comments) claimed that refraction into a DENSER medium can never
//  land wrong-side, because the transmitted direction is angularly
//  BETWEEN the incoming ray and the shading normal's far side, both of
//  which are already inside the convex crossing half-space.  That
//  argument silently assumes the tilted shading normal still OPPOSES
//  the incoming ray (`Dot(d, n_s) < 0`).  It need not:
//  `Optics::CalculateRefractedRay` flips the normal internally to
//  restore that convention, so when a bump / normal map / `GlintModifier`
//  tilts the shading normal PAST the grazing incoming ray -- the
//  bump-mapped silhouette of ANY refractive object, and `ReliefModifier`
//  is explicitly NOT A HORIZON CLAMP -- the refraction is built about
//  `-n_s`, whose far side is the side the ray CAME FROM.
//
//  Closed form for the fixture below (geomN = +Z, ray arriving at
//  `delta` off -Z, shading normal tilted `tilt`, air -> glass 1.5):
//  at delta 89 / tilt -30 the transmitted direction is
//  (-0.911, 0, +0.412) -- above the surface, on an ordinary air->glass
//  ENTRY at `ior 1.5`.  All nine (delta, tilt) cells below are
//  wrong-side pre-fix, and the companion Fresnel reflection is wrong-side
//  too, so `DielectricSPF` emitted NOTHING at all in those cells (total
//  emitted energy 0 instead of 1).
//
//  Sub-test 2b sweeps only POSITIVE tilts, which is why it never saw
//  this; that is the coverage hole this row closes.
//////////////////////////////////////////////////////////////////////
static void TestDenserEntryAtBackFacingShadingNormal()
{
	std::cout << "Sub-test 2c (P2-1): DENSER-medium entry at a back-facing shading normal" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 64;
	const Scalar deltas[] = { 80.0, 85.0, 89.0 };
	const Scalar tilts[]  = { -30.0, -45.0, -60.0 };

	struct Row { const char* name; int spf; };
	const Row rows[] = {
		{ "Dielectric       air->glass entry", 0 },
		{ "PerfectRefractor air->glass entry", 1 },
	};

	for( int r = 0; r < 2; r++ ) {
		for( int di = 0; di < 3; di++ ) {
			for( int ti = 0; ti < 3; ti++ ) {
				// `bExit = false`: an ENTRY into the denser medium.
				RayIntersectionGeometric ri = MakeObliqueHit( deltas[di], tilts[ti], false );
				IORStack stack = MakeOutsideStack( obj, 1.0 );

				RandomNumberGenerator rng( 777 );
				IndependentSampler sampler( rng );

				DielectricRig dr( 1000000.0, false, false, 1.5 );
				RefractorRig  rr( false, 1.5 );

				Census c;
				Scalar energy = 0;
				for( unsigned int trial = 0; trial < kTrials; trial++ ) {
					ScatteredRayContainer scattered;
					if( rows[r].spf == 0 ) dr.spf->Scatter( ri, sampler, scattered, stack );
					else                   rr.spf->Scatter( ri, sampler, scattered, stack );
					Tally( scattered, ri, stack, c );
					for( unsigned int i = 0; i < scattered.Count(); i++ ) {
						energy += scattered[i].kray[0];
					}
				}
				energy /= (Scalar)kTrials;

				char msg[400];
				snprintf( msg, sizeof(msg), "%s delta %.0f tilt %.0f: %u/%u transitioning rays on the wrong side",
					rows[r].name, (double)deltas[di], (double)tilts[ti], c.wrongSide, c.transitioned );
				EXPECT( c.wrongSide == 0, msg );

				snprintf( msg, sizeof(msg), "%s delta %.0f tilt %.0f: %u/%u reflection rays on the wrong side",
					rows[r].name, (double)deltas[di], (double)tilts[ti], c.reflectionsWrongSide, c.reflections );
				EXPECT( c.reflectionsWrongSide == 0, msg );

				// Fresnel + transmission must still partition unity: the
				// dielectric's `tau` is 1 on entry and `PerfectRefractor`'s
				// refractivity is white, so both lobes' kray sum to
				// (1-ref) + ref = 1 per trial when nothing is dropped.
				snprintf( msg, sizeof(msg), "%s delta %.0f tilt %.0f: total emitted energy %.4f, expected 1.0000",
					rows[r].name, (double)deltas[di], (double)tilts[ti], (double)energy );
				EXPECT( fabs( energy - 1.0 ) < 1e-6, msg );
			}
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 3: DielectricSPF -- double-sided sheet, both faces, and the
//  double-sided EXIT (the second interface of a double-sided slab).
//////////////////////////////////////////////////////////////////////
static void TestDielectricDoubleSided()
{
	std::cout << "Sub-test 3: DielectricSPF on a double-sided sheet (front / back / exit)" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 2048;

	struct Row { const char* name; int fixture; bool inside; };
	// fixture: 0 = front-face entry, 1 = back-face entry, 2 = exit
	const Row rows[] = {
		{ "front-face entry", 0, false },
		{ "back-face entry",  1, false },
		{ "exit",             2, true  },
	};

	for( int r = 0; r < 3; r++ ) {
		// Tilt 0 is the load-bearing cell here: any wrong-side count at
		// tilt 0 is a pure double-sided-flip defect, with no shading
		// tilt involved at all.
		for( int t = 0; t < kNumTilts; t++ ) {
			DielectricRig rig( 1000000.0, false, false );
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri =
				( rows[r].fixture == 0 ) ? MakeDoubleSidedFrontEntry( kTiltAnglesDeg[t] ) :
				( rows[r].fixture == 1 ) ? MakeDoubleSidedBackEntry( kTiltAnglesDeg[t] )
				                         : MakeDoubleSidedExit( kTiltAnglesDeg[t] );
			IORStack stack = rows[r].inside ? MakeInsideStack( obj, 1.5 ) : MakeOutsideStack( obj );

			Census c;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				rig.spf->Scatter( ri, sampler, scattered, stack );
				Tally( scattered, ri, stack, c );
			}

			char msg[400];
			snprintf( msg, sizeof(msg), "DielectricSPF double-sided %s tilt %.0f: %u/%u transitioning rays on the wrong side",
				rows[r].name, (double)kTiltAnglesDeg[t], c.wrongSide, c.transitioned );
			EXPECT( c.wrongSide == 0, msg );

			// The Fresnel companion must stay on the side the ray came
			// from, and must actually be EMITTED.  On a double-sided
			// exit hit the pre-fix `nEff`-anchored geometric reference
			// points the wrong way, so the reflection lobe was dropped
			// outright (non-TIR) -- deterministic energy loss, the same
			// family of defect with the opposite sign.
			snprintf( msg, sizeof(msg), "DielectricSPF double-sided %s tilt %.0f: reflection lobe emitted %u times (expected %u)",
				rows[r].name, (double)kTiltAnglesDeg[t], c.reflections, kTrials );
			EXPECT( c.reflections == kTrials, msg );

			snprintf( msg, sizeof(msg), "DielectricSPF double-sided %s tilt %.0f: %u/%u reflection rays on the wrong side",
				rows[r].name, (double)kTiltAnglesDeg[t], c.reflectionsWrongSide, c.reflections );
			EXPECT( c.reflectionsWrongSide == 0, msg );
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 4: DielectricSPF -- smooth-shaded mesh (no bump map).
//////////////////////////////////////////////////////////////////////
static void TestDielectricSmoothShaded()
{
	std::cout << "Sub-test 4: DielectricSPF on a smooth-shaded mesh" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const Scalar devs[] = { 2.0, 8.0, 20.0 };
	const unsigned int kTrials = 4096;

	for( int d = 0; d < 3; d++ ) {
		for( int s = 0; s < 2; s++ ) {
			const Scalar scattering = ( s == 0 ) ? 1000000.0 : 1.0;
			DielectricRig rig( scattering, false, false );
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeSmoothShadedEntry( devs[d] );
			IORStack stack = MakeOutsideStack( obj );

			Census c;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				rig.spf->Scatter( ri, sampler, scattered, stack );
				Tally( scattered, ri, stack, c );
			}

			char msg[400];
			snprintf( msg, sizeof(msg), "DielectricSPF smooth-shaded dev %.0f scat %.0f: %u/%u pushed rays NOT into the solid",
				(double)devs[d], (double)scattering, c.wrongSide, c.transitioned );
			EXPECT( c.wrongSide == 0, msg );
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 5: PerfectRefractorSPF -- entry + exit, RGB / dispersive / NM.
//////////////////////////////////////////////////////////////////////
static void TestPerfectRefractor()
{
	std::cout << "Sub-test 5: PerfectRefractorSPF transmission gate" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 1024;

	struct Pipe { const char* name; bool disperse; int kind; };
	const Pipe pipes[] = {
		{ "RGB",       false, 0 },
		{ "RGB disp",  true,  0 },
		{ "NM",        false, 1 },
	};

	for( int p = 0; p < 3; p++ ) {
		for( int side = 0; side < 2; side++ ) {          // 0 = entering, 1 = exiting
			for( int t = 0; t < kNumTilts; t++ ) {
				RefractorRig rig( pipes[p].disperse );
				RandomNumberGenerator rng( 777 );
				IndependentSampler sampler( rng );

				RayIntersectionGeometric ri = ( side == 0 )
					? MakeClosedEntry( kTiltAnglesDeg[t] )
					: MakeClosedExit( kTiltAnglesDeg[t] );
				IORStack stack = ( side == 0 )
					? MakeOutsideStack( obj )
					: MakeInsideStack( obj, 1.5 );

				Census c;
				for( unsigned int trial = 0; trial < kTrials; trial++ ) {
					ScatteredRayContainer scattered;
					if( pipes[p].kind == 1 ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
					else                     rig.spf->Scatter( ri, sampler, scattered, stack );
					Tally( scattered, ri, stack, c );
				}

				char msg[400];
				snprintf( msg, sizeof(msg), "PerfectRefractorSPF %s %s tilt %.0f: %u/%u transitioning rays on the wrong side",
					pipes[p].name, (side==0?"entry":"exit"), (double)kTiltAnglesDeg[t],
					c.wrongSide, c.transitioned );
				EXPECT( c.wrongSide == 0, msg );
			}
		}
	}

	// Double-sided sheet, all three hit shapes.
	for( int f = 0; f < 3; f++ ) {
		for( int t = 0; t < kNumTilts; t++ ) {
			RefractorRig rig( false );
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri =
				( f == 0 ) ? MakeDoubleSidedFrontEntry( kTiltAnglesDeg[t] ) :
				( f == 1 ) ? MakeDoubleSidedBackEntry( kTiltAnglesDeg[t] )
				           : MakeDoubleSidedExit( kTiltAnglesDeg[t] );
			IORStack stack = ( f == 2 ) ? MakeInsideStack( obj, 1.5 ) : MakeOutsideStack( obj );

			Census c;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				rig.spf->Scatter( ri, sampler, scattered, stack );
				Tally( scattered, ri, stack, c );
			}

			char msg[400];
			snprintf( msg, sizeof(msg), "PerfectRefractorSPF double-sided fixture %d tilt %.0f: %u/%u transitioning rays on the wrong side",
				f, (double)kTiltAnglesDeg[t], c.wrongSide, c.transitioned );
			EXPECT( c.wrongSide == 0, msg );
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 6: SubSurfaceScatteringSPF -- exit refraction gate.
//////////////////////////////////////////////////////////////////////
static void TestSubSurfaceExit()
{
	std::cout << "Sub-test 6: SubSurfaceScatteringSPF exit refraction gate" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 1024;

	for( int kind = 0; kind < 2; kind++ ) {              // 0 = RGB, 1 = NM
		for( int t = 0; t < kNumTilts; t++ ) {
			SSSRig rig;
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeClosedExit( kTiltAnglesDeg[t] );
			IORStack stack = MakeInsideStack( obj, 1.4 );

			Census c;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( kind == 1 ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else            rig.spf->Scatter( ri, sampler, scattered, stack );
				Tally( scattered, ri, stack, c );
			}

			char msg[400];
			snprintf( msg, sizeof(msg), "SubSurfaceScatteringSPF %s exit tilt %.0f: %u/%u popped rays NOT out of the solid",
				(kind==0?"RGB":"NM "), (double)kTiltAnglesDeg[t], c.wrongSide, c.transitioned );
			EXPECT( c.wrongSide == 0, msg );
		}
	}

	// Double-sided exit shape too.
	for( int t = 0; t < kNumTilts; t++ ) {
		SSSRig rig;
		RandomNumberGenerator rng( 777 );
		IndependentSampler sampler( rng );

		RayIntersectionGeometric ri = MakeDoubleSidedExit( kTiltAnglesDeg[t] );
		IORStack stack = MakeInsideStack( obj, 1.4 );

		Census c;
		for( unsigned int trial = 0; trial < kTrials; trial++ ) {
			ScatteredRayContainer scattered;
			rig.spf->Scatter( ri, sampler, scattered, stack );
			Tally( scattered, ri, stack, c );
		}

		char msg[400];
		snprintf( msg, sizeof(msg), "SubSurfaceScatteringSPF double-sided exit tilt %.0f: %u/%u popped rays NOT out of the solid",
			(double)kTiltAnglesDeg[t], c.wrongSide, c.transitioned );
		EXPECT( c.wrongSide == 0, msg );
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 7: the emitted transmission still obeys Snell's law.
//
//  The DL-111 ruling re-derives the refraction about the TRUE geometric
//  normal when the shading normal's answer is geometrically impossible.
//  That keeps the event a genuine refraction with the same eta -- which
//  this sub-test pins directly by recomputing Snell about BOTH
//  candidate normals and requiring the emitted direction to match one
//  of them.  A "mirror the Snell result across the geometric plane"
//  treatment (rejected option (a)) would fail this; so would a
//  "reflect instead" treatment (rejected option (c)).
//////////////////////////////////////////////////////////////////////
static void TestTransmissionObeysSnell()
{
	std::cout << "Sub-test 7: emitted transmission obeys Snell about the shading or the geometric normal" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 512;

	for( int t = 0; t < kNumTilts; t++ ) {
		RefractorRig rig( false );
		RandomNumberGenerator rng( 777 );
		IndependentSampler sampler( rng );

		RayIntersectionGeometric ri = MakeClosedEntry( kTiltAnglesDeg[t] );
		IORStack stack = MakeOutsideStack( obj );
		const Vector3 geomN = RayAnchoredGeomN( ri );

		Vector3 tShading = ri.ray.Dir();
		Vector3 tGeom    = ri.ray.Dir();
		const bool okS = Optics::CalculateRefractedRay( ri.onb.w(), 1.0, 1.5, tShading );
		const bool okG = Optics::CalculateRefractedRay( geomN,      1.0, 1.5, tGeom );

		unsigned int mismatches = 0, seen = 0;
		for( unsigned int trial = 0; trial < kTrials; trial++ ) {
			ScatteredRayContainer scattered;
			rig.spf->Scatter( ri, sampler, scattered, stack );
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( scattered[i].ior_stack == 0 ) continue;
				if( scattered[i].ior_stack->containsCurrent() == stack.containsCurrent() ) continue;
				seen++;
				const Vector3 d = scattered[i].ray.Dir();
				const bool matchS = okS && Vector3Ops::Magnitude( d - tShading ) < 1e-9;
				const bool matchG = okG && Vector3Ops::Magnitude( d - tGeom    ) < 1e-9;
				if( !matchS && !matchG ) mismatches++;
			}
		}

		char msg[300];
		snprintf( msg, sizeof(msg), "PerfectRefractorSPF tilt %.0f: %u/%u emitted transmissions match neither Snell direction",
			(double)kTiltAnglesDeg[t], mismatches, seen );
		EXPECT( mismatches == 0, msg );
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 8 (DL-112): TranslucentSPF entry front-reflection lobe --
//  emitted energy is the painter's reflectance at EVERY tilt.
//////////////////////////////////////////////////////////////////////
static void TestTranslucentFrontLobeEnergy()
{
	std::cout << "Sub-test 8 (DL-112): translucent entry front-reflection lobe energy" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 32768;

	// `front.kray` IS the throughput multiplier the integrator applies
	// (f*cos/pdf), so the estimator of the lobe's total reflected energy
	// is simply the MEAN of kray over trials, counting a non-emitted
	// lobe as zero.  The painter is 0.3, so the answer is 0.3 at every
	// tilt once the lobe renormalizes into its valid region.  Pre-fix it
	// reads 0.3 * P(valid) = 0.3 * (1 + cos(phi))/2.
	for( int kind = 0; kind < 2; kind++ ) {              // 0 = RGB, 1 = NM
		for( int t = 0; t < kNumTilts; t++ ) {
			TranslucentRig rig;
			RandomNumberGenerator rng( 777 );
			IndependentSampler sampler( rng );

			RayIntersectionGeometric ri = MakeClosedEntry( kTiltAnglesDeg[t] );
			IORStack stack = MakeOutsideStack( obj );
			const Vector3 geomN = RayAnchoredGeomN( ri );

			Scalar energy = 0;
			unsigned int wrongSide = 0;
			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				ScatteredRayContainer scattered;
				if( kind == 1 ) rig.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				else            rig.spf->Scatter( ri, sampler, scattered, stack );
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					if( scattered[i].type != ScatteredRay::eRayDiffuse ) continue;
					energy += ( kind == 1 ) ? scattered[i].krayNM : scattered[i].kray[0];
					if( Vector3Ops::Dot( scattered[i].ray.Dir(), geomN ) <= 0 ) wrongSide++;
				}
			}
			energy /= (Scalar)kTrials;

			char msg[300];
			snprintf( msg, sizeof(msg), "TranslucentSPF %s front lobe tilt %.0f: emitted energy %.5f, expected 0.30000",
				(kind==0?"RGB":"NM "), (double)kTiltAnglesDeg[t], (double)energy );
			EXPECT( fabs( energy - 0.3 ) < 0.002, msg );

			snprintf( msg, sizeof(msg), "TranslucentSPF %s front lobe tilt %.0f: %u below-horizon directions emitted",
				(kind==0?"RGB":"NM "), (double)kTiltAnglesDeg[t], wrongSide );
			EXPECT( wrongSide == 0, msg );
		}
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 9 (DL-112): the front-face Pdf integrates to 1.
//
//  An independent spherical quadrature of `TranslucentSPF::Pdf`'s
//  front-face branch -- the analogue of `TranslucentTiltedExitTest`'s
//  exit-branch quadrature.  Pre-fix this reads P(valid) = (1+cos phi)/2
//  rather than 1 (the branch's own comment says so in as many words);
//  post-fix it is 1 at every tilt, which is what makes the lobe's
//  density an honest MIS partner.
//////////////////////////////////////////////////////////////////////
static void TestTranslucentFrontPdfNormalization()
{
	std::cout << "Sub-test 9 (DL-112): translucent front-face Pdf spherical quadrature" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();

	const int nTheta = 300, nPhi = 600;
	for( int t = 0; t < kNumTilts; t++ ) {
		TranslucentRig rig;
		RayIntersectionGeometric ri = MakeClosedEntry( kTiltAnglesDeg[t] );
		IORStack stack = MakeOutsideStack( obj );

		Scalar total = 0;
		const Scalar dTheta = PI / (Scalar)nTheta;
		const Scalar dPhi   = TWO_PI / (Scalar)nPhi;
		for( int i = 0; i < nTheta; i++ ) {
			const Scalar th = ( (Scalar)i + 0.5 ) * dTheta;
			const Scalar st = sin(th), ct = cos(th);
			for( int j = 0; j < nPhi; j++ ) {
				const Scalar ph = ( (Scalar)j + 0.5 ) * dPhi;
				const Vector3 w( st*cos(ph), st*sin(ph), ct );
				total += rig.spf->Pdf( ri, w, stack ) * st * dTheta * dPhi;
			}
		}

		char msg[300];
		snprintf( msg, sizeof(msg), "TranslucentSPF front Pdf tilt %.0f: integrates to %.5f, expected 1.0",
			(double)kTiltAnglesDeg[t], (double)total );
		EXPECT( fabs( total - 1.0 ) < 0.02, msg );
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 10: fixed sampler-dimension budget.
//
//  `ISampler::HasFixedDimensionBudget()` (ISampler.h) means a bounce's
//  draw count must not depend on geometry: a variable count shifts
//  every later Get1D() in the bounce's phase in a tilt-correlated way.
//  None of the fixes in this row may introduce one, so assert
//  min == max draws across the whole tilt sweep, per SPF and per pipe --
//  the `TranslucentSamplerDimensionCountTest` pattern.
//////////////////////////////////////////////////////////////////////
static void TestDimensionBudget()
{
	std::cout << "Sub-test 10: fixed sampler-dimension budget across the tilt sweep" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 256;

	struct Row { const char* name; int spf; int kind; Scalar scattering; bool inside; };
	// spf: 0 = Dielectric, 1 = PerfectRefractor, 2 = SSS, 3 = Translucent
	const Row rows[] = {
		{ "Dielectric RGB entry delta",  0, 0, 1000000.0, false },
		{ "Dielectric RGB entry wide",   0, 0,       1.0, false },
		{ "Dielectric NM  entry delta",  0, 1, 1000000.0, false },
		{ "Dielectric RGB exit  wide",   0, 0,       1.0, true  },
		{ "PerfectRefractor RGB entry",  1, 0,       0.0, false },
		{ "PerfectRefractor NM  exit",   1, 1,       0.0, true  },
		{ "SSS RGB exit",                2, 0,       0.0, true  },
		{ "SSS NM  exit",                2, 1,       0.0, true  },
		{ "Translucent RGB entry",       3, 0,       0.0, false },
		{ "Translucent NM  entry",       3, 1,       0.0, false },
	};
	const int kNumRows = 10;

	for( int r = 0; r < kNumRows; r++ ) {
		unsigned int minDraws = 0xFFFFFFFFu, maxDraws = 0;
		for( int t = 0; t < kNumTilts; t++ ) {
			RayIntersectionGeometric ri = rows[r].inside
				? MakeClosedExit( kTiltAnglesDeg[t] )
				: MakeClosedEntry( kTiltAnglesDeg[t] );
			IORStack stack = rows[r].inside
				? MakeInsideStack( obj, ( rows[r].spf == 2 ) ? 1.4 : 1.5 )
				: MakeOutsideStack( obj );

			RandomNumberGenerator rng( 777 );
			CountingSampler sampler( rng );

			DielectricRig  dr( rows[r].scattering, false, false );
			RefractorRig   rr( false );
			SSSRig         sr;
			TranslucentRig tr;

			for( unsigned int trial = 0; trial < kTrials; trial++ ) {
				const unsigned int before = sampler.count;
				ScatteredRayContainer scattered;
				switch( rows[r].spf ) {
				case 0: if( rows[r].kind ) dr.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				        else               dr.spf->Scatter( ri, sampler, scattered, stack ); break;
				case 1: if( rows[r].kind ) rr.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				        else               rr.spf->Scatter( ri, sampler, scattered, stack ); break;
				case 2: if( rows[r].kind ) sr.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				        else               sr.spf->Scatter( ri, sampler, scattered, stack ); break;
				default:if( rows[r].kind ) tr.spf->ScatterNM( ri, sampler, 550.0, scattered, stack );
				        else               tr.spf->Scatter( ri, sampler, scattered, stack ); break;
				}
				const unsigned int drew = sampler.count - before;
				if( drew < minDraws ) minDraws = drew;
				if( drew > maxDraws ) maxDraws = drew;
			}
		}

		char msg[300];
		snprintf( msg, sizeof(msg), "%s: draw count varies (min %u, max %u) across the tilt sweep",
			rows[r].name, minDraws, maxDraws );
		EXPECT( minDraws == maxDraws, msg );
	}

	obj->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 11: GeometricUtilities::PerturbClipped contract.
//
//  The shared azimuth-arc construction DL-68 derived, exposed for
//  DielectricSPF's `scattering` warp.  Three properties: (i) the result
//  is a unit vector at the requested polar angle from the axis;
//  (ii) it always satisfies the clip; (iii) with the clip inactive the
//  half-arc is exactly PI, so the draw covers the full circle.
//////////////////////////////////////////////////////////////////////
static void TestPerturbClippedContract()
{
	std::cout << "Sub-test 11: GeometricUtilities::PerturbClipped contract" << std::endl;

	RandomNumberGenerator rng( 991 );
	const Scalar tilts[] = { 0.0, 30.0, 60.0, 89.0 };
	const Scalar alphas[] = { 0.05, 0.3, 0.8, 1.4 };

	for( int t = 0; t < 4; t++ ) {
		const Scalar phi = tilts[t] * PI / 180.0;
		// clipN tilted `phi` off the axis, in the XZ plane.
		const Vector3 axis( 0, 0, 1 );
		const Vector3 clipN( sin(phi), 0, cos(phi) );

		for( int a = 0; a < 4; a++ ) {
			unsigned int bad = 0, offAngle = 0, notUnit = 0;
			Scalar minHalf = PI*2, maxHalf = 0;
			for( int k = 0; k < 4000; k++ ) {
				const Scalar u2 = rng.CanonicalRandom();
				Scalar half = 0;
				const Vector3 w = GeometricUtilities::PerturbClipped(
					axis, alphas[a], clipN, u2, &half );
				if( fabs( Vector3Ops::Magnitude(w) - 1.0 ) > 1e-9 ) notUnit++;
				if( fabs( Vector3Ops::Dot(w,axis) - cos(alphas[a]) ) > 1e-9 ) offAngle++;
				if( Vector3Ops::Dot( w, clipN ) < -1e-12 ) bad++;
				if( half < minHalf ) minHalf = half;
				if( half > maxHalf ) maxHalf = half;
			}

			char msg[300];
			snprintf( msg, sizeof(msg), "PerturbClipped tilt %.0f alpha %.2f: %u non-unit, %u off-angle, %u below the clip",
				(double)tilts[t], (double)alphas[a], notUnit, offAngle, bad );
			EXPECT( notUnit == 0 && offAngle == 0 && bad == 0, msg );

			if( t == 0 ) {
				snprintf( msg, sizeof(msg), "PerturbClipped tilt 0 alpha %.2f: half-arc %.6f, expected PI",
					(double)alphas[a], (double)minHalf );
				EXPECT( fabs( minHalf - PI ) < 1e-12 && fabs( maxHalf - PI ) < 1e-12, msg );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 12 (review P1-1): the DL-111 re-derivation's own TIR
//  fallback must leave `refracted` in the crossing half-space.
//
//  `DielectricSPF::GenerateScatteredRay`'s re-derivation runs when the
//  SHADING normal's Snell result is geometrically impossible.  If the
//  TRUE interface total-internally-reflects at that incidence, the
//  re-derivation fails and the transmission lobe is dropped (`ref = 1`)
//  -- but the `scattering` warp block further down runs
//  UNCONDITIONALLY, so it hands `GeometricUtilities::PerturbClipped` an
//  axis that is still the wrong-side SHADING refraction.  That trips
//  PerturbClipped's fail-loud precondition, whose own comment calls
//  itself "unreachable from production": a global-lock console + file
//  write on the per-sample scatter path.
//
//  The configurations are exactly the ones that reach it: a glass->air
//  EXIT whose geometric incidence is past the 41.8 deg critical angle
//  while the tilted shading frame's incidence is inside it, so the
//  shading refraction succeeds (and lands wrong-side) and the geometric
//  one TIRs.  `scattering` is the DESCRIPTOR DEFAULT 10000, which makes
//  `alpha > 0` and so fires the warp -- the delta 1e6 rows elsewhere in
//  this file do not, which is why no existing row sees this.
//
//  Counted through an ILogPrinter rather than by scraping the log file,
//  matching CsgOperandTransformTest / CstSourceInstanceTest.
//////////////////////////////////////////////////////////////////////
namespace
{
	class PerturbClippedViolationCounter
		: public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
	{
	public:
		PerturbClippedViolationCounter() : mCount( 0 ) {}

		void Print( const RISE::LogEvent& event ) override
		{
			const std::string msg( event.szMessage );
			if( msg.find( "PerturbClipped:: precondition violated" ) != std::string::npos ) {
				std::lock_guard<std::mutex> lk( mMutex );
				mCount++;
			}
		}
		void Flush() override {}

		unsigned int Count() const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			return mCount;
		}
		void Reset()
		{
			std::lock_guard<std::mutex> lk( mMutex );
			mCount = 0;
		}

	protected:
		~PerturbClippedViolationCounter() override {}

	private:
		mutable std::mutex mMutex;
		unsigned int       mCount;
	};
}

static void TestRederivationTIRLeavesValidWarpAxis()
{
	std::cout << "Sub-test 12 (P1-1): the re-derivation's TIR fallback leaves a valid warp axis" << std::endl;

	PerturbClippedViolationCounter* counter = new PerturbClippedViolationCounter();
	counter->addref();
	GlobalLogPriv()->AddPrinter( counter );

	StubObject* obj = new StubObject(); obj->addref();
	const unsigned int kTrials = 64;

	// (delta, tilt) pairs -- the ray's angle off the TRUE geometric
	// normal and the shading-normal tilt, both in the same plane and
	// the same sense (MakeObliqueHit's convention).
	struct Cell { Scalar deltaDeg; Scalar tiltDeg; };
	const Cell cells[] = {
		{ 80.0, 45.0 }, { 80.0, 60.0 }, { 85.0, 45.0 }, { 85.0, 60.0 }, { 70.0, 30.0 }
	};
	const int kNumCells = 5;

	unsigned int totalViolations = 0;
	for( int c = 0; c < kNumCells; c++ ) {
		RayIntersectionGeometric ri = MakeObliqueHit( cells[c].deltaDeg, cells[c].tiltDeg, /*bExit*/ true );
		IORStack stack = MakeInsideStack( obj, 1.5 );

		RandomNumberGenerator rng( 777 );
		IndependentSampler sampler( rng );
		// The shipped descriptor default for `scattering` (Job.cpp /
		// the dielectric chunk descriptor), not the delta 1e6 the
		// other rows use.
		DielectricRig rig( 10000.0, false, false, 1.5 );

		counter->Reset();
		for( unsigned int trial = 0; trial < kTrials; trial++ ) {
			ScatteredRayContainer scattered;
			rig.spf->Scatter( ri, sampler, scattered, stack );
		}
		const unsigned int violations = counter->Count();
		totalViolations += violations;

		char msg[300];
		snprintf( msg, sizeof(msg), "Dielectric exit delta %.0f tilt %.0f scat 10000: %u/%u PerturbClipped precondition violations",
			(double)cells[c].deltaDeg, (double)cells[c].tiltDeg, violations, kTrials );
		EXPECT( violations == 0, msg );
	}

	char msg[200];
	snprintf( msg, sizeof(msg), "total PerturbClipped precondition violations across the 5 cells: %u (expected 0)",
		totalViolations );
	EXPECT( totalViolations == 0, msg );

	obj->release();
	GlobalLogPriv()->RemoveAllPrinters();
	counter->release();
}

int main()
{
	std::cout << "=== TransmissionPushGateTest (DL-111 / DL-112) ===" << std::endl;

	TestDielectricEntry();
	TestDielectricExit();
	TestDeltaRefractionAtSilhouette();
	TestDenserEntryAtBackFacingShadingNormal();
	TestDielectricDoubleSided();
	TestDielectricSmoothShaded();
	TestPerfectRefractor();
	TestSubSurfaceExit();
	TestTransmissionObeysSnell();
	TestTranslucentFrontLobeEnergy();
	TestTranslucentFrontPdfNormalization();
	TestDimensionBudget();
	TestPerturbClippedContract();
	TestRederivationTIRLeavesValidWarpAxis();

	std::cout << std::endl << "Checks: " << checks << "  Failures: " << failed << std::endl;
	return failed ? 1 : 0;
}
