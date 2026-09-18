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
#include "../src/Library/Materials/TranslucentMaterial.h"

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
		UniformScalarPainter* N;
		UniformScalarPainter* scat;
		TranslucentMaterial*  mat;

		Rig( const RISEPel& refC, const RISEPel& tauC, Scalar extV, Scalar NV, Scalar scatV )
		{
			ref  = new UniformColorPainter( refC );  ref->addref();
			tau  = new UniformColorPainter( tauC );  tau->addref();
			ext  = new UniformScalarPainter( extV ); ext->addref();
			N    = new UniformScalarPainter( NV );   N->addref();
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

static void Gate1( const Rig& rig, const IObject* obj, bool bExit, bool bNM,
	Scalar segLen, const char* label )
{
	const int kTrials = 40000;

	std::cout << "  -- Gate 1 " << label << " (" << (bNM?"NM":"RGB") << ")" << std::endl;

	for( int t = 0; t < kNumTilts; t++ )
	{
		RayIntersectionGeometric ri = bExit
			? MakeClosedExit( kTiltAnglesDeg[t], segLen )
			: MakeClosedEntry( kTiltAnglesDeg[t], segLen );
		IORStack stack = bExit ? MakeInsideStack( obj ) : MakeOutsideStack( obj );

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
					sumBsdf[k] += rig.bsdf()->valueNM( wo, ri, kProbeNM ) * cosO / s.pdf;
				} else {
					sumKray[k] += ColorMath::MaxValue( s.kray );
					sumBsdf[k] += ColorMath::MaxValue( rig.bsdf()->value( wo, ri ) ) * cosO / s.pdf;
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
			std::cout << "    tilt " << std::setw(2) << (int)kTiltAnglesDeg[t]
			          << " " << lobe
			          << "  E[kray]=" << std::fixed << std::setprecision(6) << mk
			          << "  E[val*cos/pdf]=" << mb
			          << "  ratio=" << ratio
			          << "  (n=" << nEmit[k] << ")" << std::endl;
			char buf[256];
			snprintf( buf, sizeof(buf), "%s %s tilt %d %s ratio=%.6f (want 1)",
				label, bNM?"NM":"RGB", (int)kTiltAnglesDeg[t], lobe, ratio );
			EXPECT( fabs( ratio - 1.0 ) < 0.005, buf );
		}
	}
}

//////////////////////////////////////////////////////////////////////
//  Gates 2/3/4 -- DL-41: the aggregate Pdf is the density of what
//  Scatter + RandomlySelect actually generate.
//////////////////////////////////////////////////////////////////////

static void Gate234( const Rig& rig, const IObject* obj, bool bExit, bool bNM,
	Scalar segLen, const char* label )
{
	const int kTrials = 200000;

	std::cout << "  -- Gates 2/3/4 " << label << " (" << (bNM?"NM":"RGB") << ")" << std::endl;

	for( int t = 0; t < kNumTilts; t++ )
	{
		RayIntersectionGeometric ri = bExit
			? MakeClosedExit( kTiltAnglesDeg[t], segLen )
			: MakeClosedEntry( kTiltAnglesDeg[t], segLen );
		IORStack stack = bExit ? MakeInsideStack( obj ) : MakeOutsideStack( obj );

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
						const double v = bNM
							? rig.bsdf()->valueNM( w, ri, kProbeNM )
							: ColorMath::MaxValue( rig.bsdf()->value( w, ri ) );
						if( v > 1e-12 && p <= 0 ) partitionViolations++;
						cell += p * dW;
						mass += p * dW;
					}
				}
				expected[it*SphereGrid::kNPhi + ip] = cell;
			}
		}

		// ---- histogram of what the sampler + RandomlySelect produce ----
		RandomNumberGenerator rng( 9090u + (unsigned)t );
		Implementation::IndependentSampler sampler( rng );
		std::vector<double> observed( kCells, 0.0 );
		long selected = 0;

		for( int i = 0; i < kTrials; i++ )
		{
			ScatteredRayContainer scattered;
			if( bNM ) rig.spf()->ScatterNM( ri, sampler, kProbeNM, scattered, stack );
			else      rig.spf()->Scatter( ri, sampler, scattered, stack );
			const ScatteredRay* pS = scattered.RandomlySelect( sampler.Get1D(), bNM );
			if( !pS ) continue;
			const Vector3 wo = Vector3Ops::Normalize( pS->ray.Dir() );
			observed[ SphereGrid::CellOf( wo ) ] += 1.0;
			selected++;
		}

		const double emitProb = (double)selected / (double)kTrials;

		double tvd = 0;
		if( selected > 0 ) {
			for( int c = 0; c < kCells; c++ ) {
				tvd += fabs( observed[c]/(double)selected - expected[c]/r_max(1e-12,mass) );
			}
			tvd *= 0.5;
		}

		std::cout << "    tilt " << std::setw(2) << (int)kTiltAnglesDeg[t]
		          << "  int(Pdf)=" << std::fixed << std::setprecision(5) << mass
		          << "  emitted=" << emitProb
		          << "  TVD=" << std::setprecision(5) << tvd
		          << "  partitionViolations=" << partitionViolations << std::endl;

		char buf[256];
		snprintf( buf, sizeof(buf), "%s %s tilt %d: int(Pdf)=%.5f vs emitted=%.5f",
			label, bNM?"NM":"RGB", (int)kTiltAnglesDeg[t], mass, emitProb );
		EXPECT( fabs( mass - emitProb ) < 0.02, buf );

		snprintf( buf, sizeof(buf), "%s %s tilt %d: TVD(sampler,Pdf)=%.5f",
			label, bNM?"NM":"RGB", (int)kTiltAnglesDeg[t], tvd );
		EXPECT( tvd < 0.035, buf );

		snprintf( buf, sizeof(buf), "%s %s tilt %d: %ld directions with value>0 and Pdf==0",
			label, bNM?"NM":"RGB", (int)kTiltAnglesDeg[t], partitionViolations );
		EXPECT( partitionViolations == 0, buf );
	}
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "TranslucentLobeConsistencyTest (DL-157 / DL-41 / DL-38)" << std::endl;
	std::cout << "=======================================================" << std::endl;

	StubObject* obj = new StubObject();
	obj->addref();

	// Chromatic reflectance and transmittance (DL-98/DL-99 review lesson
	// (3): grey inputs hide a MaxValue reduction-order error), the
	// documented `N 10 / scattering 0.3` of the DL-157 evidence table,
	// and TWO extinction settings -- 0 (the pre-existing tables' value)
	// and 0.35 over a unit interior segment, which is what makes the
	// exit/backscatter split's Beer factor (DL-38) visible at all.
	{
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.0, 10.0, 0.3 );
		std::cout << std::endl << "[A] ext=0, ref=(.5,.3,.2) tau=(.4,.6,.3) N=10 scat=.3" << std::endl;
		Gate1  ( rig, obj, false, false, 2.0, "entry" );
		Gate1  ( rig, obj, false, true,  2.0, "entry" );
		Gate1  ( rig, obj, true,  false, 2.0, "exit " );
		Gate1  ( rig, obj, true,  true,  2.0, "exit " );
		Gate234( rig, obj, false, false, 2.0, "entry" );
		Gate234( rig, obj, false, true,  2.0, "entry" );
		Gate234( rig, obj, true,  false, 2.0, "exit " );
		Gate234( rig, obj, true,  true,  2.0, "exit " );
	}

	{
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.35, 10.0, 0.3 );
		std::cout << std::endl << "[B] ext=0.35 over a 2.0 interior segment (DL-38's Beer factor)"
		          << std::endl;
		Gate1  ( rig, obj, true,  false, 2.0, "exit " );
		Gate1  ( rig, obj, true,  true,  2.0, "exit " );
		Gate234( rig, obj, true,  false, 2.0, "exit " );
		Gate234( rig, obj, true,  true,  2.0, "exit " );
	}

	{
		// A low Phong exponent widens the transmission / backscatter lobe
		// so the clipped-arc renormalization is exercised over a large
		// solid angle rather than a narrow cap.
		Rig rig( RISEPel(0.5,0.3,0.2), RISEPel(0.4,0.6,0.3), 0.0, 1.0, 0.6 );
		std::cout << std::endl << "[C] N=1 (wide Phong lobes), scat=0.6" << std::endl;
		Gate1  ( rig, obj, false, false, 2.0, "entry" );
		Gate1  ( rig, obj, true,  false, 2.0, "exit " );
		Gate234( rig, obj, false, false, 2.0, "entry" );
		Gate234( rig, obj, true,  false, 2.0, "exit " );
	}

	obj->release();

	std::cout << std::endl << "Passed: " << (checks-failed) << "  Failed: " << failed << std::endl;
	return failed ? 1 : 0;
}
