//////////////////////////////////////////////////////////////////////
//
//  TranslucentLobeConsistencyTest.cpp - Red-proof and regression guard
//    for the translucent "one function per side" family: DL-157
//    (`TranslucentBSDF::value`/`valueNM` describe none of the lobes
//    `TranslucentSPF::Scatter`/`ScatterNM` actually sample except the
//    entry front reflection), DL-41 (`TranslucentSPF::Pdf`/`PdfNM`
//    cover neither Phong lobe and apply no selection probability) and
//    DL-38 (the stateful Beer-extinction / scattering-split factors the
//    sampler charges never reach a reverse / NEE evaluation).
//
//  THE RULING BEING GATED (one function per side).  For every lobe the
//  SPF can emit on a given side of the surface:
//
//    ENTRY  (!ior_stack.containsCurrent())
//      E1  front reflection  -- clipped COSINE about OrientedLobeAxis(n, geomN),
//                               clipped to Dot(w, geomN) > 0,  kray = ref
//      E2  transmission      -- clipped PHONG(N) about OrientedLobeAxis(n,-geomN),
//                               clipped to Dot(w,-geomN) > 0,  kray = tau
//    EXIT   (ior_stack.containsCurrent())
//      X1  diffuse exit      -- clipped COSINE about OrientedExitNormal(n, geomNRaw),
//                               clipped to Dot(w, geomNRaw) > 0, kray = B*(1-s)
//      X2  interior backscatter -- clipped PHONG(N) about OrientedLobeAxis(n, geomN),
//                               clipped to Dot(w, geomN) > 0,   kray = B*s
//
//    with B = exp(-ext * |ri.ray.origin - ri.ptIntersection|) the Beer
//    extinction over the interior segment and s the scattering split.
//
//    (1) `value`/`valueNM` must return exactly `kray_I * p_I(w) / |cos(w,n)|`
//        for that lobe's direction, so that the two techniques which MIS
//        together -- a BSDF-sampled continuation carrying `kray`, and
//        NEE / a BDPT connection paying `value * cos / pdf` -- estimate
//        the SAME integral.  Gate 1 below.
//    (2) `Pdf`/`PdfNM` must report the density of what `Scatter` +
//        `ScatteredRayContainer::RandomlySelect` actually generate --
//        i.e. `sum_I q_I p_I(w)` with `q_I = MaxValue(kray_I)/sum_J ...`
//        (`krayNM` on the NM pipe), covering BOTH Phong lobes.  Gates 2
//        (mass) and 3 (shape) below.
//
//  WHY THE MASS GATE IS NOT ENOUGH (DL-98/DL-99's own lesson).  The
//  pre-fix `Pdf` returns ONE normalized cosine lobe, so it integrates to
//  1.0 over the sphere and the mass gate is GREEN against a measured
//  emission probability of 1.0 -- while the sampler is really drawing
//  from a two-lobe mixture.  Gate 3 (total variation against a histogram
//  of directions the real sampler + RandomlySelect produced) is what
//  actually fails there.  Gate 4 pins the DL-74 partition side condition
//  (`Pdf(w) > 0` wherever `value(w) != 0`), which the pre-fix pair also
//  violates over the whole transmission half-space.
//
//  MEASURED RED (see the commit message for the verbatim run).
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `dl157`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
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
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Materials/CompositeMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"

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
	const Scalar kTiltAnglesDeg[] = { 0, 30, 45, 60, 75, 89 };
	const int    kNumTilts        = 6;
	const Scalar kProbeNM         = 550.0;

	//! One TranslucentMaterial plus the painters it owns.  Constructed
	//! through the MATERIAL (not the BSDF/SPF directly) so this file
	//! compiles unchanged against both sides of the fix -- the BSDF's own
	//! constructor gains the extinction / scattering painters it needs to
	//! price the interior lobes.
	struct Rig
	{
		UniformColorPainter*  ref;
		UniformColorPainter*  tau;
		UniformScalarPainter* ext;
		IScalarPainter*       N;
		UniformScalarPainter* scat;
		TranslucentMaterial*  mat;

		//! `NV < 0` selects a PER-CHANNEL exponent (`RGBScalarPainter(5,10,15)`).
		//! That is not decoration: it is the ONLY way into `Scatter`'s
		//! three-ray branch, where the Phong lobe is emitted once per colour
		//! channel with a single-channel `kray`, `LobeSet` really does hold
		//! four lobes, and `RandomlySelect`'s CDF weighs `MaxValue` of a
		//! single-channel RISEPel -- the DL-98/DL-99 reduction-order trap.
		//! Every rig in the first draft of this file built `N` from a
		//! `UniformScalarPainter`, so that whole branch was untested.
		Rig( const RISEPel& refC, const RISEPel& tauC, Scalar extV, Scalar NV, Scalar scatV )
		{
			ref  = new UniformColorPainter( refC );  ref->addref();
			tau  = new UniformColorPainter( tauC );  tau->addref();
			ext  = new UniformScalarPainter( extV ); ext->addref();
			N    = ( NV < 0 ) ? static_cast<IScalarPainter*>( new RGBScalarPainter( 5, 10, 15 ) )
			                  : static_cast<IScalarPainter*>( new UniformScalarPainter( NV ) );
			N->addref();
			scat = new UniformScalarPainter( scatV );scat->addref();
			mat  = new TranslucentMaterial( *ref, *tau, *ext, *N, *scat );
			mat->addref();
		}
		~Rig()
		{
			safe_release( mat );
			safe_release( scat );
			safe_release( N );
			safe_release( ext );
			safe_release( tau );
			safe_release( ref );
		}
		ISPF*  spf()  const { return mat->GetSPF(); }
		IBSDF* bsdf() const { return mat->GetBSDF(); }
	};

	//! A CLOSED translucent object (analytic-primitive convention: the
	//! reported geometric normal IS the object's true outward direction,
	//! never flipped toward the ray) struck from OUTSIDE, shading normal
	//! tilted `tiltDeg` off outward in the XZ plane.
	RayIntersectionGeometric MakeClosedEntry( Scalar tiltDeg, Scalar segLen )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

		Ray inRay( Point3(0,0,segLen), Vector3(0,0,-1) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = segLen;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = n;
		ri.onb.CreateFromW( n );
		ri.vGeomNormal = Vector3(0,0,1);
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	//! The same object struck from INSIDE (the ray travels outward), with
	//! `segLen` the interior path length the Beer extinction is charged
	//! over.
	RayIntersectionGeometric MakeClosedExit( Scalar tiltDeg, Scalar segLen )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

		Ray inRay( Point3(0,0,-segLen), Vector3(0,0,1) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = segLen;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = n;
		ri.onb.CreateFromW( n );
		ri.vGeomNormal = Vector3(0,0,1);
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	//! DL-157 review P1: an OPEN double-sided sheet struck on its BACK
	//! FACE FIRST -- a camera ray hitting the back of a
	//! `clippedplane_geometry` (whose `doublesided` DEFAULTS TO TRUE), or
	//! of a double-sided `trianglemesh`.  The geometry has already flipped
	//! BOTH reported normals toward the ray and says so
	//! (`bGeomNormalOrientedToRay`), and marks the surface as having no
	//! interior (`bOpenSheet`, DL-96).  True outward is +Z; the ray
	//! travels +Z.
	//!
	//! This record is the one where the SIDE and the RAY disagree and BOTH
	//! are telling the truth: the stack says NOT INSIDE (the walk really
	//! never entered anything) and the ray is LEAVING relative to the
	//! authored outward normal (it really did strike the back face).  A
	//! frame derived from the side alone inverts here; `Scatter`'s own
	//! frame is ray-anchored, so the lobe set must be too on the entry
	//! side.
	RayIntersectionGeometric MakeOpenSheetBackFaceFirst( Scalar tiltDeg, Scalar segLen )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		// The REPORTED (already flipped) shading normal: -Z tilted in XZ.
		const Vector3 nReported( sin(tiltRad), 0, -cos(tiltRad) );

		Ray inRay( Point3(0,0,-segLen), Vector3(0,0,1) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = segLen;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = nReported;
		ri.onb.CreateFromW( nReported );
		ri.vGeomNormal = Vector3(0,0,-1);        // flipped toward the ray
		ri.bGeomNormalOrientedToRay = true;      // ... and it says so
		ri.bOpenSheet = true;                    // two legitimate faces
		// A `clippedplane_geometry`: a PLANE cannot enclose a volume, so
		// this geometry really does certify "no interior" and a stackless
		// caller can use it.  The MESH twin below cannot.
		ri.bProvablyNoInterior = true;
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	//! The same back-face-first hit on a double-sided MESH open sheet.
	//! Identical in every respect except that a mesh can never certify
	//! "no interior" (`bOpenSheet` there means UNCERTIFIED), so a
	//! stackless caller has only the ray anchor and reads this as an
	//! EXIT.  That is DL-223's recorded stackless residual; the STACKED
	//! entry point -- which is every production integrator path -- is
	//! correct here and is gated.
	RayIntersectionGeometric MakeMeshSheetBackFaceFirst( Scalar tiltDeg, Scalar segLen )
	{
		RayIntersectionGeometric ri = MakeOpenSheetBackFaceFirst( tiltDeg, segLen );
		ri.bProvablyNoInterior = false;
		return ri;
	}

	//! The FRONT-face control on the same open sheet: identical geometry,
	//! ray travelling -Z, no flip.  The side and the ray agree here, so
	//! this row is expected to be green on both sides of the fix and is
	//! what separates "the back-face record is broken" from "open sheets
	//! are broken".
	RayIntersectionGeometric MakeOpenSheetFrontFace( Scalar tiltDeg, Scalar segLen )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

		Ray inRay( Point3(0,0,segLen), Vector3(0,0,-1) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = segLen;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = n;
		ri.onb.CreateFromW( n );
		ri.vGeomNormal = Vector3(0,0,1);
		ri.bOpenSheet = true;
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	IORStack MakeOutsideStack( const IObject* obj )
	{
		IORStack stack( 1.0 );
		stack.SetCurrentObject( obj );
		return stack;
	}

	IORStack MakeInsideStack( const IObject* obj )
	{
		IORStack stack( 1.0 );
		stack.SetCurrentObject( obj );
		stack.push( 1.0 );
		stack.SetCurrentObject( obj );
		return stack;
	}

	//! Which intersection record a gate is driven on.  `kEntry`/`kExit`
	//! are the closed-solid pair; `kSheetFront`/`kSheetBack` are the open
	//! double-sided sheet, whose BACK-face record is the one DL-157's P1
	//! review found broken (side says entry, ray says leaving, both true).
	enum RecordKind { kEntry, kExit, kSheetFront, kSheetBack, kMeshSheetBack, kDSExit, kOtherEnclosure };
	const int kNumRecordKinds = 7;

	const char* RecordName( RecordKind k )
	{
		switch( k ) {
		case kEntry:      return "entry      ";
		case kExit:       return "exit       ";
		case kSheetFront: return "sheetFront ";
		case kSheetBack:  return "planeBack  ";
		case kMeshSheetBack: return "meshBack   ";
		case kDSExit:     return "dsMeshExit ";
		default:          return "otherEnclos";
		}
	}

	//! A CLOSED but UNCERTIFIED double-sided mesh hit from INSIDE (a
	//! genuine interior exit).  Review round 2's P1-2 record: the
	//! geometry flips both reported normals toward the outward-travelling
	//! ray AND stamps `bOpenSheet`, because
	//! `TriangleMeshGeometryIndexed` sets that flag whenever DL-143's
	//! weld could not CERTIFY watertightness -- which, per DL-143's own
	//! audit, is every glTF asset it examined, all of them closed solids.
	//! `bProvablyNoInterior` stays false, because a mesh can never prove
	//! it.  This is the record that a `bOpenSheet`-based stackless rule
	//! got wrong.
	RayIntersectionGeometric MakeDoubleSidedMeshExit( Scalar tiltDeg, Scalar segLen )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		// Reported (already flipped toward the outward-travelling ray).
		const Vector3 nReported( sin(tiltRad), 0, -cos(tiltRad) );

		Ray inRay( Point3(0,0,-segLen), Vector3(0,0,1) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = segLen;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = nReported;
		ri.onb.CreateFromW( nReported );
		ri.vGeomNormal = Vector3(0,0,-1);        // flipped toward the ray
		ri.bGeomNormalOrientedToRay = true;
		ri.bOpenSheet = true;                    // UNCERTIFIED, not open
		ri.bProvablyNoInterior = false;          // ... and a mesh cannot certify
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	RayIntersectionGeometric MakeRecord( RecordKind k, Scalar tiltDeg, Scalar segLen )
	{
		switch( k ) {
		case kEntry:      return MakeClosedEntry( tiltDeg, segLen );
		case kExit:       return MakeClosedExit( tiltDeg, segLen );
		case kSheetFront: return MakeOpenSheetFrontFace( tiltDeg, segLen );
		case kSheetBack:  return MakeOpenSheetBackFaceFirst( tiltDeg, segLen );
		case kMeshSheetBack: return MakeMeshSheetBackFaceFirst( tiltDeg, segLen );
		case kDSExit:     return MakeDoubleSidedMeshExit( tiltDeg, segLen );
		// `kOtherEnclosure` is an ordinary ENTRY hit on our object; what
		// differs is the STACK (see MakeRecordStack).
		default:          return MakeClosedEntry( tiltDeg, segLen );
		}
	}

	//! `kExit` and `kDSExit` are interior exits; every other record is an
	//! ENTRY by the stack.  `kOtherEnclosure` is the one that separates
	//! "the stack is non-empty" from "the stack contains US": the walk is
	//! inside a DIFFERENT object (water, say) and entering ours, so
	//! `containsCurrent()` must read false even though the stack has a
	//! frame on it.
	IORStack MakeRecordStack( RecordKind k, const IObject* obj, const IObject* other )
	{
		if( k == kExit || k == kDSExit ) return MakeInsideStack( obj );
		if( k == kOtherEnclosure ) {
			IORStack stack( 1.0 );
			stack.SetCurrentObject( other );
			stack.push( 1.33 );          // we are inside `other`
			stack.SetCurrentObject( obj );   // ... and now hitting ours
			return stack;
		}
		return MakeOutsideStack( obj );
	}

	//! Uniform-ish spherical grid used by gates 2/3/4.
	struct SphereGrid
	{
		static const int kNTheta = 48;
		static const int kNPhi   = 96;
		static const int kSub    = 3;   //!< sub-samples per axis per cell

		//! Cell (it, ip) sub-sample (st, sp): direction + solid angle share.
		static Vector3 Dir( int it, int ip, int st, int sp, Scalar& dOmega )
		{
			// cos(theta) uniform in [-1,1], phi uniform in [0,2pi):
			// each (it,ip,st,sp) cell then carries an EQUAL solid angle.
			const Scalar u = ( it + ( st + Scalar(0.5) ) / kSub ) / Scalar(kNTheta);
			const Scalar v = ( ip + ( sp + Scalar(0.5) ) / kSub ) / Scalar(kNPhi);
			const Scalar cosT = Scalar(1) - Scalar(2)*u;
			const Scalar sinT = sqrt( r_max( Scalar(0), Scalar(1) - cosT*cosT ) );
			const Scalar phi  = TWO_PI * v;
			dOmega = ( Scalar(4)*PI ) / ( Scalar(kNTheta)*kNPhi*kSub*kSub );
			return Vector3( sinT*cos(phi), sinT*sin(phi), cosT );
		}

		static int CellOf( const Vector3& w )
		{
			const Scalar cosT = r_max( Scalar(-1), r_min( Scalar(1), w.z ) );
			int it = (int)( ( Scalar(1) - cosT ) * Scalar(0.5) * kNTheta );
			if( it < 0 ) it = 0;
			if( it >= kNTheta ) it = kNTheta-1;
			Scalar phi = atan2( w.y, w.x );
			if( phi < 0 ) phi += TWO_PI;
			int ip = (int)( phi / TWO_PI * kNPhi );
			if( ip < 0 ) ip = 0;
			if( ip >= kNPhi ) ip = kNPhi-1;
			return it*kNPhi + ip;
		}
	};
}

//////////////////////////////////////////////////////////////////////
//  Gate 1 -- DL-157 / DL-38:  E[kray] == E[value * |cos| / pdf]
//            per LOBE, over the SPF's own draws.
//////////////////////////////////////////////////////////////////////

//! `bStacked` selects the ENTRY POINT under test.  Review round 2's P2-2:
//! gate 1 originally drove only the STACKLESS `value(wo, ri)`, while PT's
//! NEE (`pMisIorStack`), `PathVertexEval::EvalBSDFAtVertex` and BDPT's
//! zero-exitance row all call `valueStateful(wo, ri, &stack)` -- so the
//! entry point every production integrator uses had no gate at all, and
//! that hole is what let a mis-attributed red-proof stand for a round.
//! Both entry points are now driven on every record.
static void Gate1( const Rig& rig, const IObject* obj, const IObject* other,
	RecordKind kind, bool bNM, bool bStacked, Scalar segLen, const char* label )
{
	const int kTrials = 40000;
	const bool bExit = ( kind == kExit || kind == kDSExit );

	std::cout << "  -- Gate 1 " << label << " " << RecordName(kind)
	          << " (" << (bNM?"NM":"RGB") << ", " << (bStacked?"STACKED value":"stackless value")
	          << ")" << std::endl;

	for( int t = 0; t < kNumTilts; t++ )
	{
		RayIntersectionGeometric ri = MakeRecord( kind, kTiltAnglesDeg[t], segLen );
		IORStack stack = MakeRecordStack( kind, obj, other );

		RandomNumberGenerator rng( 4242u + (unsigned)t );
		Implementation::IndependentSampler sampler( rng );

		// index 0 = the cosine lobe (eRayDiffuse), 1 = the Phong lobe
		// (eRayTranslucent).
		double sumKray[2] = {0,0}, sumBsdf[2] = {0,0};
		long   nEmit[2]   = {0,0};

		for( int i = 0; i < kTrials; i++ )
		{
			ScatteredRayContainer scattered;
			if( bNM ) rig.spf()->ScatterNM( ri, sampler, kProbeNM, scattered, stack );
			else      rig.spf()->Scatter( ri, sampler, scattered, stack );

			for( unsigned int j = 0; j < scattered.Count(); j++ )
			{
				const ScatteredRay& s = scattered[j];
				if( s.pdf <= 0 ) continue;
				const int k = ( s.type == ScatteredRay::eRayDiffuse ) ? 0 : 1;
				const Vector3 wo = Vector3Ops::Normalize( s.ray.Dir() );
				const double cosO = fabs( Vector3Ops::Dot( wo, ri.vNormal ) );
				if( bNM ) {
					sumKray[k] += s.krayNM;
					sumBsdf[k] += ( bStacked
						? rig.bsdf()->valueStatefulNM( wo, ri, kProbeNM, &stack )
						: rig.bsdf()->valueNM( wo, ri, kProbeNM ) ) * cosO / s.pdf;
				} else {
					// REDUCE BY THE LOBE'S OWN CHANNEL, not by MaxValue of
					// the two sides independently.  `value` is a SUM over
					// the lobe set, and in `Scatter`'s per-channel-exponent
					// branch the Phong lobe is emitted once per colour
					// channel with a single-channel `kray` -- so at lobe
					// i's own direction the OTHER two channel-lobes also
					// have support, and `MaxValue(value)` can pick a
					// channel the emitted ray does not carry.  Channel
					// `argmax(kray)` is exactly lobe i's own term (the
					// channel-lobes contribute to disjoint channels), and
					// for every non-per-channel lobe `value[c] =
					// kray[c] * fOverKray`, so argmax(kray) == argmax(value)
					// and this is numerically identical to the MaxValue
					// form there.  Measured: without it the per-channel rig
					// reads 0.665 / 0.737 -- which is the DL-69
					// aggregate-over-per-lobe pairing, reproduced inside a
					// test that exists to catch exactly that.
					int cBest = 0;
					for( int c = 1; c < 3; c++ ) {
						if( s.kray[c] > s.kray[cBest] ) cBest = c;
					}
					sumKray[k] += s.kray[cBest];
					sumBsdf[k] += ( bStacked
						? rig.bsdf()->valueStateful( wo, ri, &stack )
						: rig.bsdf()->value( wo, ri ) )[cBest] * cosO / s.pdf;
				}
				nEmit[k]++;
			}
		}

		for( int k = 0; k < 2; k++ )
		{
			if( nEmit[k] == 0 ) continue;
			const double mk = sumKray[k] / (double)kTrials;
			const double mb = sumBsdf[k] / (double)kTrials;
			const double ratio = ( mb > 0 ) ? mk/mb : 0.0;
			const char* lobe = bExit
				? ( k==0 ? "exit      " : "backscatter" )
				: ( k==0 ? "frontrefl " : "transmit   " );
			std::cout << "    " << RecordName(kind)
			          << " tilt " << std::setw(2) << (int)kTiltAnglesDeg[t]
			          << " " << lobe
			          << "  E[kray]=" << std::fixed << std::setprecision(6) << mk
			          << "  E[val*cos/pdf]=" << mb
			          << "  ratio=" << ratio
			          << "  (n=" << nEmit[k] << ")" << std::endl;
			char buf[256];
			snprintf( buf, sizeof(buf), "%s %s %s %s tilt %d %s ratio=%.6f (want 1)",
				label, RecordName(kind), bNM?"NM":"RGB", bStacked?"STACKED":"stackless",
				(int)kTiltAnglesDeg[t], lobe, ratio );
			// DL-223's recorded stackless residual: a double-sided MESH
			// open sheet struck back-face-first cannot be distinguished
			// from that mesh's own interior EXIT without a stack, because
			// a mesh can never certify "no interior" (`bOpenSheet` there
			// means UNCERTIFIED -- review round 2, P1-2).  The STACKED
			// entry point, which is every production integrator path, is
			// gated normally; the stackless one is PRINTED and bounded so
			// its closure is as visible as a regression.
			if( kind == kMeshSheetBack && !bStacked ) {
				char rbuf[320];
				snprintf( rbuf, sizeof(rbuf),
					"%s KNOWN-RESIDUAL (DL-223, stackless mesh open sheet): %s",
					label, buf );
				EXPECT( ratio > 0.5 && ratio < 2.5, rbuf );
			} else {
				EXPECT( fabs( ratio - 1.0 ) < 0.005, buf );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
//  Gates 2/3/4 -- DL-41: the aggregate Pdf is the density of what
//  Scatter + RandomlySelect actually generate.
//////////////////////////////////////////////////////////////////////

static void Gate234( const Rig& rig, const IObject* obj, const IObject* other,
	RecordKind kind, bool bNM, Scalar segLen, const char* label )
{
	const int kTrials = 200000;

	std::cout << "  -- Gates 2/3/4 " << label << " " << RecordName(kind)
	          << " (" << (bNM?"NM":"RGB") << ")" << std::endl;

	for( int t = 0; t < kNumTilts; t++ )
	{
		RayIntersectionGeometric ri = MakeRecord( kind, kTiltAnglesDeg[t], segLen );
		IORStack stack = MakeRecordStack( kind, obj, other );

		// ---- quadrature of Pdf, and the value>0 / Pdf>0 partition ----
		const int kCells = SphereGrid::kNTheta * SphereGrid::kNPhi;
		std::vector<double> expected( kCells, 0.0 );
		double mass = 0;
		long   partitionViolations = 0;

		for( int it = 0; it < SphereGrid::kNTheta; it++ ) {
			for( int ip = 0; ip < SphereGrid::kNPhi; ip++ ) {
				double cell = 0;
				for( int st = 0; st < SphereGrid::kSub; st++ ) {
					for( int sp = 0; sp < SphereGrid::kSub; sp++ ) {
						Scalar dW = 0;
						const Vector3 w = SphereGrid::Dir( it, ip, st, sp, dW );
						const Scalar p = bNM
							? rig.spf()->PdfNM( ri, w, kProbeNM, stack )
							: rig.spf()->Pdf( ri, w, stack );
						// Gate 4 pairs the STACKED `Pdf` above with the
						// STACKED `value`: comparing a stacked density's
						// support against a STACKLESS value's would be a
						// cross-entry-point check, which is P2-1's own
						// mis-attribution in miniature.
						const double v = bNM
							? rig.bsdf()->valueStatefulNM( w, ri, kProbeNM, &stack )
							: ColorMath::MaxValue( rig.bsdf()->valueStateful( w, ri, &stack ) );
						if( v > 1e-12 && p <= 0 ) partitionViolations++;
						cell += p * dW;
						mass += p * dW;
					}
				}
				expected[it*SphereGrid::kNPhi + ip] = cell;
			}
		}

		// ---- histogram of what the sampler + RandomlySelect produce ----
		//
		// A total-variation distance over 4608 cells at 200000 draws has a
		// LARGE pure-multinomial floor -- roughly `0.5*sqrt(2K/(pi N))`,
		// i.e. ~0.06 here -- so a fixed tolerance either sits below the
		// noise (and fails on a correct density, which the first draft of
		// this file did at 0.035) or is set so loose it stops discriminating.
		// The floor is therefore MEASURED rather than assumed: the same
		// draws are also split into two independent halves, and
		// `TVD(halfA, halfB)` is a sample of exactly that noise at half the
		// count.  For pure noise `TVD(full, exact) ~ C/sqrt(N)` and
		// `TVD(halfA, halfB) ~ 2C/sqrt(N)`, so the model-vs-sampler distance
		// must come in at about HALF the halves' distance; the gate allows
		// 0.75 of it (a 50 % margin) plus a small absolute slack for the
		// grid's own discretisation of a sharp lobe.  Pre-fix this reads
		// 0.54-0.60 against a floor near 0.10, i.e. it fails by 5x.
		RandomNumberGenerator rng( 9090u + (unsigned)t );
		Implementation::IndependentSampler sampler( rng );
		std::vector<double> observed( kCells, 0.0 );
		std::vector<double> halfA( kCells, 0.0 );
		std::vector<double> halfB( kCells, 0.0 );
		long selected = 0, selA = 0, selB = 0;

		for( int i = 0; i < kTrials; i++ )
		{
			ScatteredRayContainer scattered;
			if( bNM ) rig.spf()->ScatterNM( ri, sampler, kProbeNM, scattered, stack );
			else      rig.spf()->Scatter( ri, sampler, scattered, stack );
			const ScatteredRay* pS = scattered.RandomlySelect( sampler.Get1D(), bNM );
			if( !pS ) continue;
			const Vector3 wo = Vector3Ops::Normalize( pS->ray.Dir() );
			const int cell = SphereGrid::CellOf( wo );
			observed[cell] += 1.0;
			selected++;
			if( (i & 1) == 0 ) { halfA[cell] += 1.0; selA++; }
			else               { halfB[cell] += 1.0; selB++; }
		}

		const double emitProb = (double)selected / (double)kTrials;

		double tvd = 0, tvdNoise = 0;
		if( selected > 0 && selA > 0 && selB > 0 ) {
			for( int c = 0; c < kCells; c++ ) {
				tvd      += fabs( observed[c]/(double)selected - expected[c]/r_max(1e-12,mass) );
				tvdNoise += fabs( halfA[c]/(double)selA - halfB[c]/(double)selB );
			}
			tvd *= 0.5;
			tvdNoise *= 0.5;
		}
		const double tvdGate = 0.75*tvdNoise + 0.004;

		std::cout << "    tilt " << std::setw(2) << (int)kTiltAnglesDeg[t]
		          << "  int(Pdf)=" << std::fixed << std::setprecision(5) << mass
		          << "  emitted=" << emitProb
		          << "  TVD=" << std::setprecision(5) << tvd
		          << "  (noiseFloor=" << tvdNoise << " gate=" << tvdGate << ")"
		          << "  partitionViolations=" << partitionViolations << std::endl;

		char buf[256];
		snprintf( buf, sizeof(buf), "%s %s %s tilt %d: int(Pdf)=%.5f vs emitted=%.5f",
			label, RecordName(kind), bNM?"NM":"RGB", (int)kTiltAnglesDeg[t], mass, emitProb );
		EXPECT( fabs( mass - emitProb ) < 0.02, buf );

		snprintf( buf, sizeof(buf), "%s %s %s tilt %d: TVD(sampler,Pdf)=%.5f vs gate %.5f (noise floor %.5f)",
			label, RecordName(kind), bNM?"NM":"RGB", (int)kTiltAnglesDeg[t], tvd, tvdGate, tvdNoise );
		EXPECT( tvd <= tvdGate, buf );

		snprintf( buf, sizeof(buf), "%s %s %s tilt %d: %ld directions with value>0 and Pdf==0",
			label, RecordName(kind), bNM?"NM":"RGB", (int)kTiltAnglesDeg[t], partitionViolations );
		EXPECT( partitionViolations == 0, buf );
	}
}

//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
//  Gate 5 -- DL-222: `TranslucentSPF::EvaluateKrayNM` is the HWSS
//  companion weight, and it must equal what `ScatterNM` itself stamps
//  when the SAME wavelength is the hero.
//
//  PT's and BDPT's HWSS companion lanes ask an SPF what a ray sampled at
//  the hero wavelength would have weighed at a companion one; an SPF that
//  declines (returns < 0) sends the caller to DL-125's
//  `value*cos/pdf` fallback.  `TranslucentSPF` used to decline.  The
//  check here is the strongest available statement of correctness: drive
//  `ScatterNM` at wavelength `nm` as the HERO and compare its own
//  `krayNM` per lobe against what `EvaluateKrayNM` answers for that lobe
//  at the same `nm` -- they must be identical, because they are the same
//  quantity asked two ways.
//////////////////////////////////////////////////////////////////////

static void GateKrayNM( const IObject* obj, const IObject* other )
{
	std::cout << std::endl << "[F] Gate 5: EvaluateKrayNM == ScatterNM's own krayNM (DL-222)"
	          << std::endl;

	Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.35, 10.0, 0.3 );
	const Scalar kNMs[] = { 430.0, 550.0, 660.0 };

	for( int k = 0; k < kNumRecordKinds; k++ )
	{
		const RecordKind kind = (RecordKind)k;
		for( int t = 0; t < kNumTilts; t++ )
		{
			RayIntersectionGeometric ri = MakeRecord( kind, kTiltAnglesDeg[t], 2.0 );
			IORStack stack = MakeRecordStack( kind, obj, other );
			RandomNumberGenerator rng( 31337u + (unsigned)t );
			Implementation::IndependentSampler sampler( rng );

			for( int w = 0; w < 3; w++ )
			{
				ScatteredRayContainer scattered;
				rig.spf()->ScatterNM( ri, sampler, kNMs[w], scattered, stack );
				for( unsigned int j = 0; j < scattered.Count(); j++ )
				{
					const ScatteredRay& sr = scattered[j];
					const Scalar asked = rig.spf()->EvaluateKrayNM(
						ri, Vector3Ops::Normalize( sr.ray.Dir() ), sr.type, kNMs[w], stack );
					char buf[256];
					snprintf( buf, sizeof(buf),
						"[F] %s tilt %d nm %.0f type %d: EvaluateKrayNM=%.9f vs ScatterNM krayNM=%.9f",
						RecordName(kind), (int)kTiltAnglesDeg[t], (double)kNMs[w],
						(int)sr.type, (double)asked, (double)sr.krayNM );
					EXPECT( asked >= 0 && fabs( asked - sr.krayNM ) <= 1e-12, buf );
				}
			}
		}
	}
	std::cout << "    " << checks << " checks so far; every emitted lobe matched to 1e-12"
	          << std::endl;
}

//////////////////////////////////////////////////////////////////////
//  Gate 6 -- DL-157 review P2-1: `CompositeMaterial` must forward the
//  full-sphere capability, and must forward it from WHICHEVER LAYER'S
//  BSDF it actually presents.
//
//  `CompositeMaterial::GetBSDF()` returns the TOP material's BSDF when
//  it has one, else the BOTTOM's -- and that is the `value()`
//  `LightSampler`'s NEE arms will call.  `FabricMaterial` and
//  `CoatedMaterial` both forward `ScattersFullSphere()`; `CompositeMaterial`
//  did not, so `composite { top = translucent }` (which is
//  `mat_wax_gold` in scenes/Tests/Materials/composite_material.RISEscene)
//  presented a transmitting `TranslucentBSDF` to a `bFullSphere == false`
//  NEE and lost the whole transmissive half-space -- the exact
//  under-reading DL-157's own `ScattersFullSphere` derivation names.
//
//  The CONTROL is the other order.  A Lambertian TOP over a translucent
//  BOTTOM presents the LAMBERTIAN BSDF, which does not transmit, so the
//  flag must stay FALSE there -- an OR over both layers would grant it
//  and light that material's back faces at full weight, which is the
//  failure mode `IMaterial::ScattersFullSphere`'s own doc warns about.
//////////////////////////////////////////////////////////////////////

static void GateCompositeFullSphere()
{
	std::cout << std::endl << "[G] Gate 6: CompositeMaterial forwards ScattersFullSphere (DL-157 P2-1)"
	          << std::endl;

	UniformColorPainter* c = new UniformColorPainter( RISEPel(0.5,0.4,0.3) ); c->addref();
	UniformScalarPainter* z = new UniformScalarPainter( 0.0 ); z->addref();
	UniformScalarPainter* n10 = new UniformScalarPainter( 10.0 ); n10->addref();
	UniformScalarPainter* s3 = new UniformScalarPainter( 0.3 ); s3->addref();

	TranslucentMaterial* tr = new TranslucentMaterial( *c, *c, *z, *n10, *s3 ); tr->addref();
	LambertianMaterial*  lm = new LambertianMaterial( *c ); lm->addref();

	EXPECT( tr->ScattersFullSphere(),
		"[G] translucent_material claims the full-sphere capability (DL-157)" );
	EXPECT( !lm->ScattersFullSphere(),
		"[G] lambertian_material does NOT claim it (control)" );

	CompositeMaterial* topTrans = new CompositeMaterial( *tr, *lm, 4, 2, 2, 2, 2, 0.1, *z );
	topTrans->addref();
	CompositeMaterial* topLamb  = new CompositeMaterial( *lm, *tr, 4, 2, 2, 2, 2, 0.1, *z );
	topLamb->addref();

	std::cout << "    composite{top=translucent} GetBSDF()==translucent's: "
	          << ( topTrans->GetBSDF() == tr->GetBSDF() ? "yes" : "no" )
	          << "  ScattersFullSphere=" << ( topTrans->ScattersFullSphere() ? "true" : "false" )
	          << std::endl;
	std::cout << "    composite{top=lambertian}  GetBSDF()==lambertian's:  "
	          << ( topLamb->GetBSDF() == lm->GetBSDF() ? "yes" : "no" )
	          << "  ScattersFullSphere=" << ( topLamb->ScattersFullSphere() ? "true" : "false" )
	          << std::endl;

	EXPECT( topTrans->GetBSDF() == tr->GetBSDF(),
		"[G] composite{top=translucent} presents the TRANSLUCENT BSDF" );
	EXPECT( topTrans->ScattersFullSphere(),
		"[G] ... so it must claim the full-sphere capability too" );
	EXPECT( topLamb->GetBSDF() == lm->GetBSDF(),
		"[G] composite{top=lambertian} presents the LAMBERTIAN BSDF" );
	EXPECT( !topLamb->ScattersFullSphere(),
		"[G] ... so it must NOT claim it, even though its BOTTOM layer does" );

	topLamb->release(); topTrans->release(); lm->release(); tr->release();
	s3->release(); n10->release(); z->release(); c->release();
}

int main()
{
	std::cout << "TranslucentLobeConsistencyTest (DL-157 / DL-41 / DL-38)" << std::endl;
	std::cout << "=======================================================" << std::endl;

	StubObject* obj = new StubObject();
	obj->addref();
	// A SECOND object, so `kOtherEnclosure` can put a frame on the stack
	// that is not ours -- the record that separates "the stack is
	// non-empty" from "the stack contains US".
	StubObject* other = new StubObject();
	other->addref();

	// Chromatic reflectance and transmittance (DL-98/DL-99 review lesson
	// (3): grey inputs hide a MaxValue reduction-order error), the
	// documented `N 10 / scattering 0.3` of the DL-157 evidence table,
	// and TWO extinction settings -- 0 (the pre-existing tables' value)
	// and 0.35 over a unit interior segment, which is what makes the
	// exit/backscatter split's Beer factor (DL-38) visible at all.
	//
	// EVERY gate-1 row is run through BOTH entry points -- the stackless
	// `value(wo, ri)` and the STACKED `valueStateful(wo, ri, &stack)` that
	// PT's NEE, `EvalBSDFAtVertex` and BDPT's zero-exitance row actually
	// call (review round 2, P2-2).
	{
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.0, 10.0, 0.3 );
		std::cout << std::endl << "[A] ext=0, ref=(.5,.3,.2) tau=(.4,.6,.3) N=10 scat=.3" << std::endl;
		for( int m = 0; m < 2; m++ ) {
			const bool bNM = ( m == 1 );
			for( int st = 0; st < 2; st++ ) {
				Gate1( rig, obj, other, kEntry, bNM, st==1, 2.0, "[A]" );
				Gate1( rig, obj, other, kExit,  bNM, st==1, 2.0, "[A]" );
			}
		}
		Gate234( rig, obj, other, kEntry, false, 2.0, "[A]" );
		Gate234( rig, obj, other, kEntry, true,  2.0, "[A]" );
		Gate234( rig, obj, other, kExit,  false, 2.0, "[A]" );
		Gate234( rig, obj, other, kExit,  true,  2.0, "[A]" );
	}

	{
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.35, 10.0, 0.3 );
		std::cout << std::endl << "[B] ext=0.35 over a 2.0 interior segment (DL-38's Beer factor)"
		          << std::endl;
		for( int st = 0; st < 2; st++ ) {
			Gate1( rig, obj, other, kExit, false, st==1, 2.0, "[B]" );
			Gate1( rig, obj, other, kExit, true,  st==1, 2.0, "[B]" );
		}
		Gate234( rig, obj, other, kExit, false, 2.0, "[B]" );
		Gate234( rig, obj, other, kExit, true,  2.0, "[B]" );
	}

	{
		// A low Phong exponent widens the transmission / backscatter lobe
		// so the clipped-arc renormalization is exercised over a large
		// solid angle rather than a narrow cap.
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.0, 1.0, 0.6 );
		std::cout << std::endl << "[C] N=1 (wide Phong lobes), scat=0.6" << std::endl;
		for( int st = 0; st < 2; st++ ) {
			Gate1( rig, obj, other, kEntry, false, st==1, 2.0, "[C]" );
			Gate1( rig, obj, other, kExit,  false, st==1, 2.0, "[C]" );
		}
		Gate234( rig, obj, other, kEntry, false, 2.0, "[C]" );
		Gate234( rig, obj, other, kExit,  false, 2.0, "[C]" );
	}

	{
		// [D] REVIEW P2-2 (round 1): a PER-CHANNEL Phong exponent, the only
		// way into `Scatter`'s three-ray branch -- four lobes in the set, a
		// single-channel `kray` on each Phong ray, and `RandomlySelect`
		// weighing `MaxValue` of that.  RGB only: `ScatterNM` has no
		// per-channel branch (it evaluates `GetValueAtNM` once).
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.35, -1.0, 0.3 );
		std::cout << std::endl << "[D] per-channel N=(5,10,15), ext=0.35, chromatic ref/tau" << std::endl;
		for( int st = 0; st < 2; st++ ) {
			Gate1( rig, obj, other, kEntry, false, st==1, 2.0, "[D]" );
			Gate1( rig, obj, other, kExit,  false, st==1, 2.0, "[D]" );
		}
		Gate234( rig, obj, other, kEntry, false, 2.0, "[D]" );
		Gate234( rig, obj, other, kExit,  false, 2.0, "[D]" );
	}

	{
		// [E] REVIEW round 1 P1 / round 2 P1-2: the records where the SIDE
		// and the RAY disagree, and the record where they must NOT be
		// confused.
		//
		//   sheetFront  -- open sheet, front face: side and ray agree.
		//   planeBack   -- a CLIPPED PLANE struck BACK FACE FIRST: the
		//                  stack says not-inside (true, it never entered)
		//                  and the ray says leaving (true, it hit the back
		//                  face).  `clippedplane_geometry`'s `doublesided`
		//                  DEFAULTS TO TRUE.  A plane PROVES it has no
		//                  interior, so even the stackless path is right.
		//   meshBack    -- the same hit on a double-sided MESH, which can
		//                  never prove it.  STACKED is gated; stackless is
		//                  DL-223's recorded residual.
		//   dsMeshExit  -- a CLOSED but UNCERTIFIED double-sided mesh hit
		//                  from inside.  It also carries `bOpenSheet`
		//                  (that flag means "uncertified", not "open"), so
		//                  it is the record that a `bOpenSheet`-based
		//                  stackless rule priced as an ENTRY.
		//   otherEnclos -- an ordinary entry while the walk is inside a
		//                  DIFFERENT object: `containsCurrent()` must read
		//                  false against a NON-EMPTY stack.
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.0, 10.0, 0.3 );
		std::cout << std::endl << "[E] open sheet (front / BACK-FACE-FIRST), uncertified closed mesh "
		          << "EXIT, and entry from inside another enclosure" << std::endl;
		for( int m = 0; m < 2; m++ ) {
			const bool bNM = ( m == 1 );
			for( int st = 0; st < 2; st++ ) {
				Gate1( rig, obj, other, kSheetFront,     bNM, st==1, 2.0, "[E]" );
				Gate1( rig, obj, other, kSheetBack,      bNM, st==1, 2.0, "[E]" );
				Gate1( rig, obj, other, kMeshSheetBack,  bNM, st==1, 2.0, "[E]" );
				Gate1( rig, obj, other, kDSExit,         bNM, st==1, 2.0, "[E]" );
				Gate1( rig, obj, other, kOtherEnclosure, bNM, st==1, 2.0, "[E]" );
			}
		}
		Gate234( rig, obj, other, kSheetFront,     false, 2.0, "[E]" );
		Gate234( rig, obj, other, kSheetBack,      false, 2.0, "[E]" );
		Gate234( rig, obj, other, kMeshSheetBack,  false, 2.0, "[E]" );
		Gate234( rig, obj, other, kDSExit,         false, 2.0, "[E]" );
		Gate234( rig, obj, other, kOtherEnclosure, false, 2.0, "[E]" );
	}

	GateKrayNM( obj, other );
	GateCompositeFullSphere();

	other->release();
	obj->release();

	std::cout << std::endl << "Passed: " << (checks-failed) << "  Failed: " << failed << std::endl;
	return failed ? 1 : 0;
}
