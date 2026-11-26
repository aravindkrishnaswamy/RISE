//////////////////////////////////////////////////////////////////////
//
//  DielectricARTest.cpp - Phase-3 piece: the data-based anti-reflective
//    (AR) thin-film COATING on the dielectric crystal (docs/
//    THIN_FILM_INTERFERENCE.md; the MING-37.06 watch hero).
//
//    A real watch sapphire crystal is AR-coated so its 7.7%/surface bare
//    Fresnel glare drops to ~0.5%, with the characteristic purple bloom.
//    DielectricSPF now models this physically: when arThickness > 0 the
//    bare air<->medium Fresnel is replaced by the single-film Airy
//    reflectance of an (ambient / AR-film / substrate) stack via the
//    Phase-1/2 evaluator ThinFilm::ReflectanceConductor.
//
//    This test pins BOTH halves:
//      PART 1 - the DATA: the air / MgF2 (n=1.38) quarter-wave / sapphire
//               (n=1.768) reflectance spectrum is the textbook AR curve
//               (R(550)~0.14%, mean visible ~0.5%, ~14x below bare, energy
//               in [0,1], reduces reflection at every visible wavelength).
//      PART 2 - the INTEGRATION: DielectricSPF::ScatterNM's reflection-ray
//               weight equals that AR reflectance when the coating is on,
//               equals the bare air/sapphire Fresnel when it is off
//               (back-compat), and AR is strictly dimmer than bare.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>

#include "../src/Library/Utilities/ThinFilm.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what, double got = 0.0, double want = 0.0 )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what
				<< "  (got " << std::setprecision( 6 ) << got
				<< ", want " << want << ")\n";
		}
	}

	// Air / MgF2 quarter-wave / sapphire AR stack constants.
	const Scalar kAir   = 1.0;
	const Scalar kMgF2  = 1.38;		// MgF2 real index (~constant across visible)
	const Scalar kSapph = 1.768;	// sapphire (Al2O3 o-ray) at the d-line
	const Scalar kArD   = 99.6;		// quarter-wave at 550 nm: 550/(4*1.38)

	// Single-layer AR as ambient->substrate arrays (for the array-based ctor).
	const Scalar kN1[1] = { kMgF2 };
	const Scalar kK1[1] = { 0.0 };
	const Scalar kT1[1] = { kArD };

	// Neutral 2-layer broadband AR (quarter/quarter @520 nm), ambient->substrate:
	// MgF2 then Al2O3 (the sapphire-matching layer).  Flattens R(lambda) across
	// the visible -> a faint COLOUR-NEUTRAL reflection (no single-layer bloom).
	const Scalar kAl2O3 = 1.66;
	const Scalar kN2[2] = { kMgF2, kAl2O3 };
	const Scalar kK2[2] = { 0.0, 0.0 };
	const Scalar kT2[2] = { 94.2, 78.3 };

	// Bare single-surface Fresnel reflectance at normal incidence, air->sapphire.
	Scalar BareNormalR()
	{
		const Scalar r = ( kAir - kSapph ) / ( kAir + kSapph );
		return r * r;
	}

	// A trivial deterministic sampler (the dielectric only draws for the
	// optional Phong/HG scatter cone, which is disabled here via a huge
	// scattering coefficient, so the value is irrelevant).
	class FixedSampler : public ISampler
	{
	public:
		Scalar Get1D() { return 0.5; }
		Point2 Get2D() { return Point2( 0.5, 0.5 ); }
		void StartStream( int /*streamIndex*/ ) {}
	};

	// RI whose incoming ray travels along rayDir (INTO the surface) with the
	// geometric normal +Z.  Mirrors ThinFilmBRDFTest::MakeRI.
	RayIntersectionGeometric MakeRI( const Vector3& rayDir )
	{
		Ray inRay( Point3( 0, 0, 1 ), rayDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		return ri;
	}

	// The reflection-lobe weight produced by an SPF scatter.
	Scalar ReflectionWeight( const ScatteredRayContainer& sc )
	{
		for( unsigned int i = 0; i < sc.Count(); ++i ) {
			if( sc[i].type == ScatteredRay::eRayReflection ) {
				return sc[i].krayNM;
			}
		}
		return -1.0;
	}
}

int main()
{
	std::cout << "=== DielectricARTest: data-based AR coating on the dielectric ===\n";

	// ---------------------------------------------------------------
	// PART 1 - the AR reflectance DATA (air / MgF2 lambda/4 / sapphire).
	// ---------------------------------------------------------------
	const Scalar bare = BareNormalR();
	Check( std::fabs( bare - 0.0770 ) < 0.001, "bare air/sapphire normal R ~= 7.7%", bare, 0.077 );

	const Scalar Rar550 = ThinFilm::ReflectanceConductor( 1.0, 550.0, kAir, 0.0, kMgF2, 0.0, kArD, kSapph, 0.0 );
	Check( Rar550 < 0.003, "AR R(550nm) < 0.3% (design minimum)", Rar550, 0.003 );
	Check( Rar550 < bare * 0.10, "AR R(550nm) < 10% of bare", Rar550, bare );

	// Mean visible reflectance and per-wavelength reduction vs bare.
	Scalar sum = 0.0; int cnt = 0; bool allBelowBare = true; bool allInUnit = true;
	for( int nm = 430; nm <= 670; nm += 20 ) {
		const Scalar R = ThinFilm::ReflectanceConductor( 1.0, (Scalar)nm, kAir, 0.0, kMgF2, 0.0, kArD, kSapph, 0.0 );
		sum += R; ++cnt;
		if( R >= bare ) allBelowBare = false;
		if( R < 0.0 || R > 1.0 ) allInUnit = false;
	}
	const Scalar mean = sum / cnt;
	Check( mean < 0.012, "AR mean visible R < 1.2%", mean, 0.012 );
	Check( mean < bare / 8.0, "AR mean visible R >= 8x below bare", mean, bare / 8.0 );
	Check( allBelowBare, "AR reduces R at EVERY visible wavelength (normal)", 0, 0 );
	Check( allInUnit, "AR R in [0,1] across the visible band", 0, 0 );

	// Energy in [0,1] across a fan of incidence angles too.
	bool angleUnit = true;
	for( int a = 1; a <= 10; ++a ) {
		const Scalar mu = (Scalar)a / 10.0;
		const Scalar R = ThinFilm::ReflectanceConductor( mu, 550.0, kAir, 0.0, kMgF2, 0.0, kArD, kSapph, 0.0 );
		if( R < 0.0 || R > 1.0 ) angleUnit = false;
	}
	Check( angleUnit, "AR R in [0,1] across incidence angles", 0, 0 );

	// AR stays below bare at moderate oblique angles (where the dome is seen).
	bool obliqueBelow = true;
	for( double mu = 1.0; mu >= 0.6 - 1e-9; mu -= 0.2 ) {
		const Scalar Rar  = ThinFilm::ReflectanceConductor( mu, 550.0, kAir, 0.0, kMgF2, 0.0, kArD, kSapph, 0.0 );
		// Bare air/sapphire Fresnel at this angle (unpolarized), for comparison.
		const Scalar Rbar = ThinFilm::ReflectanceConductor( mu, 550.0, kAir, 0.0, kSapph, 0.0, 0.0, kSapph, 0.0 );
		if( Rar >= Rbar ) obliqueBelow = false;
	}
	Check( obliqueBelow, "AR < bare at moderate oblique angles (mu=1.0..0.6)", 0, 0 );

	// --- Revert-proof: the legacy single-film AR convenience (RISE_API + IJob)
	// must accept EVERY arithmetic spelling of the three film scalars -- all-double,
	// all-int (0,0,0), AND MIXED (e.g. an integer extinction, (1.38, 0, 99.6)) --
	// unambiguously.  A fixed Scalar + int overload pair left mixed spellings with
	// no best match; the SFINAE arithmetic template fixes that.  Each call below is
	// a COMPILE-TIME proof: if the template is removed or mis-constrained, the call
	// is ambiguous / ill-formed and this test fails to BUILD.  NO-COATING: a
	// thickness <= 0 forwards nLayers=0 (bare Fresnel), matching the parser's
	// if(ar_thick>0) legacy handling and the ctor's nLayers==0 contract.  (A
	// *behavioural* no-coating check isn't feasible: DielectricSPF::ScatterNM is
	// stochastic and it doesn't override the deterministic EvaluateKrayNM, so no
	// deterministic reflectance readout exists through the constructed material.)
	{
		IScalarPainter *tauP = nullptr, *iorP = nullptr, *scatP = nullptr;
		RISE_API_CreateUniformScalarPainter( &tauP,  1.0 );
		RISE_API_CreateUniformScalarPainter( &iorP,  kSapph );
		RISE_API_CreateUniformScalarPainter( &scatP, 100000.0 );

		IMaterial *mAllD=nullptr, *mNoD=nullptr, *mAllI=nullptr, *mMixK=nullptr, *mMixT=nullptr;
		const bool a = RISE_API_CreateDielectricMaterial( &mAllD, *tauP,*iorP,*scatP, false, kMgF2, 0.0, kArD );   // all-double coating
		const bool b = RISE_API_CreateDielectricMaterial( &mNoD,  *tauP,*iorP,*scatP, false, 0.0, 0.0, 0.0 );      // all-double no-coating
		const bool c = RISE_API_CreateDielectricMaterial( &mAllI, *tauP,*iorP,*scatP, false, 0, 0, 0 );           // all-int (was ambiguous vs array)
		const bool d = RISE_API_CreateDielectricMaterial( &mMixK, *tauP,*iorP,*scatP, false, kMgF2, 0, kArD );    // MIXED integer extinction
		const bool e = RISE_API_CreateDielectricMaterial( &mMixT, *tauP,*iorP,*scatP, false, 0, 0.0, 0 );         // MIXED no-coating
		Check( a && mAllD, "AR all-double coating (MgF2,0.0,lambda/4) builds", 0, 0 );
		Check( b && mNoD,  "AR all-double no-coating (0.0,0.0,0.0) builds", 0, 0 );
		Check( c && mAllI, "AR all-int (0,0,0) builds unambiguously", 0, 0 );
		Check( d && mMixK, "AR MIXED integer extinction (MgF2,0,lambda/4) builds unambiguously", 0, 0 );
		Check( e && mMixT, "AR MIXED (0,0.0,0) no-coating builds unambiguously", 0, 0 );
		if( mAllD ) mAllD->release();
		if( mNoD )  mNoD->release();
		if( mAllI ) mAllI->release();
		if( mMixK ) mMixK->release();
		if( mMixT ) mMixT->release();
		if( tauP )  tauP->release();
		if( iorP )  iorP->release();
		if( scatP ) scatP->release();

		// IJob mirrors it: taking the member-template's address at a MIXED
		// (Scalar,int,Scalar) signature is a compile-time proof (no Job instance
		// needed) that IJob's convenience handles mixed spellings too.
		bool (IJob::*jobMixed)( const char*, const char*, const char*, const char*, const bool,
		                        const Scalar, const int, const Scalar )
			= &IJob::AddDielectricMaterial;
		Check( jobMixed != 0, "IJob::AddDielectricMaterial handles MIXED (Scalar,int,Scalar)", 0, 0 );
	}

	// ---------------------------------------------------------------
	// PART 1b - the N-LAYER stack path (ReflectanceConductorStack).
	// ---------------------------------------------------------------
	// (a) nFilms==1 must reproduce the Airy single-film result exactly (the
	//     legacy single-layer AR path now routes through the stack evaluator).
	bool stackMatchesAiry = true;
	for( int nm = 430; nm <= 670; nm += 20 ) {
		const ThinFilm::Complex f1[1] = { ThinFilm::Complex( kMgF2, 0.0 ) };
		const Scalar Rstack = ThinFilm::ReflectanceConductorStack(
			1.0, (Scalar)nm, ThinFilm::Complex( kAir, 0.0 ), f1, kT1, 1, ThinFilm::Complex( kSapph, 0.0 ) );
		const Scalar Rairy = ThinFilm::ReflectanceConductor( 1.0, (Scalar)nm, kAir, 0.0, kMgF2, 0.0, kArD, kSapph, 0.0 );
		if( std::fabs( Rstack - Rairy ) > 1e-9 ) stackMatchesAiry = false;
	}
	Check( stackMatchesAiry, "TMM stack (nFilms=1) == Airy single-film across visible", 0, 0 );

	// (b) the 2-layer broadband stack is COLOUR-NEUTRAL: a single MgF2 layer is
	//     minimised only at green, so its visible reflectance SWINGS (magenta
	//     "purple bloom"); the QQ pair flattens R(lambda).  Pin: the 2-layer's
	//     visible spread is far below the single layer's, and near-neutral.
	Scalar r1min = 1e9, r1max = -1e9, r2min = 1e9, r2max = -1e9, r2sum = 0.0; int nc = 0;
	for( int nm = 430; nm <= 670; nm += 20 ) {
		const Scalar R1 = ThinFilm::ReflectanceConductor( 1.0, (Scalar)nm, kAir, 0.0, kMgF2, 0.0, kArD, kSapph, 0.0 );
		const ThinFilm::Complex f2[2] = { ThinFilm::Complex( kN2[0], 0.0 ), ThinFilm::Complex( kN2[1], 0.0 ) };
		const Scalar R2 = ThinFilm::ReflectanceConductorStack(
			1.0, (Scalar)nm, ThinFilm::Complex( kAir, 0.0 ), f2, kT2, 2, ThinFilm::Complex( kSapph, 0.0 ) );
		r1min = std::min( r1min, R1 ); r1max = std::max( r1max, R1 );
		r2min = std::min( r2min, R2 ); r2max = std::max( r2max, R2 ); r2sum += R2; ++nc;
	}
	const Scalar spread1 = r1max - r1min, spread2 = r2max - r2min;
	Check( spread2 < spread1 * 0.5, "2-layer AR spectrally FLATTER than single-layer (neutral, not bloom)", spread2, spread1 );
	Check( spread2 < 0.005, "2-layer AR visible spread < 0.5% (near-neutral)", spread2, 0.005 );
	Check( ( r2sum / nc ) < bare / 5.0, "2-layer AR mean R well below bare (faint)", r2sum / nc, bare / 5.0 );

	// ---------------------------------------------------------------
	// PART 2 - DielectricSPF INTEGRATION (ScatterNM reflection weight).
	// ---------------------------------------------------------------
	// Reference-counted painters (protected dtor) must be heap + release().
	UniformScalarPainter* tau  = new UniformScalarPainter( 1.0 );    tau->addref();
	UniformScalarPainter* ior  = new UniformScalarPainter( kSapph ); ior->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 1.0e6 );  scat->addref();  // huge => no scatter cone

	DielectricSPF* spfAR   = new DielectricSPF( *tau, *ior, *scat, false, kN1, kK1, kT1, 1 ); spfAR->addref();
	DielectricSPF* spfBare = new DielectricSPF( *tau, *ior, *scat, false );                   spfBare->addref();

	RayIntersectionGeometric ri = MakeRI( Vector3( 0, 0, -1 ) );   // normal, from air
	FixedSampler samp;

	const int nmList[3] = { 450, 550, 650 };
	for( int i = 0; i < 3; ++i ) {
		const Scalar nm = (Scalar)nmList[i];

		IORStack stkAR( kAir );
		ScatteredRayContainer scAR;
		spfAR->ScatterNM( ri, samp, nm, scAR, stkAR );
		const Scalar refAR = ReflectionWeight( scAR );
		const Scalar expR  = ThinFilm::ReflectanceConductor( 1.0, nm, kAir, 0.0, kMgF2, 0.0, kArD, kSapph, 0.0 );
		Check( refAR >= 0.0 && std::fabs( refAR - expR ) < 1e-6,
			"SPF AR reflection weight == ThinFilm AR reflectance", refAR, expR );

		IORStack stkB( kAir );
		ScatteredRayContainer scB;
		spfBare->ScatterNM( ri, samp, nm, scB, stkB );
		const Scalar refB = ReflectionWeight( scB );
		Check( refB >= 0.0 && std::fabs( refB - bare ) < 1e-4,
			"SPF bare (AR-off) reflection weight == air/sapphire Fresnel", refB, bare );

		Check( refAR >= 0.0 && refB > 0.0 && refAR < refB * 0.2,
			"SPF AR reflection weight << bare (coating fires)", refAR, refB );
	}

	// ---------------------------------------------------------------
	// PART 2b - MULTI-LAYER AR through the SPF (the new wiring).  The SPF must
	// evaluate the FULL stack, not just the first layer: its reflection weight
	// equals ReflectanceConductorStack for the 2-layer coating.
	// ---------------------------------------------------------------
	DielectricSPF* spf2 = new DielectricSPF( *tau, *ior, *scat, false, kN2, kK2, kT2, 2 ); spf2->addref();
	for( int i = 0; i < 3; ++i ) {
		const Scalar nm = (Scalar)nmList[i];
		IORStack stk2( kAir );
		ScatteredRayContainer sc2;
		spf2->ScatterNM( ri, samp, nm, sc2, stk2 );
		const Scalar ref2 = ReflectionWeight( sc2 );
		const ThinFilm::Complex f2[2] = { ThinFilm::Complex( kN2[0], 0.0 ), ThinFilm::Complex( kN2[1], 0.0 ) };
		const Scalar exp2 = ThinFilm::ReflectanceConductorStack(
			1.0, nm, ThinFilm::Complex( kAir, 0.0 ), f2, kT2, 2, ThinFilm::Complex( kSapph, 0.0 ) );
		Check( ref2 >= 0.0 && std::fabs( ref2 - exp2 ) < 1e-6,
			"SPF 2-layer AR reflection weight == ThinFilm stack reflectance", ref2, exp2 );
	}

	// OBLIQUE incidence (from outside): the SPF must feed the true cosI to the
	// stack, not assume normal incidence.  ~40deg -> cosI = cos(0.7).
	{
		const Scalar nm = 550.0;
		const Vector3 obl( std::sin( 0.7 ), 0.0, -std::cos( 0.7 ) );   // into +Z surface
		RayIntersectionGeometric riO = MakeRI( obl );
		IORStack stkO( kAir );
		ScatteredRayContainer scO;
		spf2->ScatterNM( riO, samp, nm, scO, stkO );
		const Scalar refO = ReflectionWeight( scO );
		const Scalar cosI = std::fabs( std::cos( 0.7 ) );
		const ThinFilm::Complex f2[2] = { ThinFilm::Complex( kN2[0], 0.0 ), ThinFilm::Complex( kN2[1], 0.0 ) };
		const Scalar expO = ThinFilm::ReflectanceConductorStack(
			cosI, nm, ThinFilm::Complex( kAir, 0.0 ), f2, kT2, 2, ThinFilm::Complex( kSapph, 0.0 ) );
		Check( refO >= 0.0 && std::fabs( refO - expO ) < 1e-6,
			"SPF 2-layer AR oblique weight == stack at cosI<1", refO, expO );
	}

	// FROM INSIDE (sapphire->air): exercises the layer-order REVERSAL -- the
	// ray meets the substrate-adjacent layer first, so the stack (and its
	// endpoints) reverse.  bFromInside is driven by ior_stack.containsCurrent(),
	// so seed the stack with the current object on top.  (The IObject* is used
	// only as an opaque identity key by IORStack; it is never dereferenced.)
	{
		const Scalar nm = 550.0;
		const IObject* fakeObj = reinterpret_cast<const IObject*>( 0x1 );
		IORStack stkI( kAir );					// air = the exit medium (bottom)
		stkI.SetCurrentObject( fakeObj );
		stkI.push( kSapph );					// sapphire on top => containsCurrent()
		RayIntersectionGeometric riI = MakeRI( Vector3( 0, 0, 1 ) );   // travelling outward
		ScatteredRayContainer scI;
		spf2->ScatterNM( riI, samp, nm, scI, stkI );
		const Scalar refI = ReflectionWeight( scI );
		// REVERSED stack: sapphire-adjacent layer first, N0=sapphire, Ns=air.
		const ThinFilm::Complex f2rev[2] = { ThinFilm::Complex( kN2[1], 0.0 ), ThinFilm::Complex( kN2[0], 0.0 ) };
		const Scalar t2rev[2] = { kT2[1], kT2[0] };
		const Scalar expI = ThinFilm::ReflectanceConductorStack(
			1.0, nm, ThinFilm::Complex( kSapph, 0.0 ), f2rev, t2rev, 2, ThinFilm::Complex( kAir, 0.0 ) );
		Check( refI >= 0.0 && std::fabs( refI - expI ) < 1e-6,
			"SPF 2-layer AR from-inside weight == REVERSED stack (layer-order flip)", refI, expI );
	}

	spf2->release();

	spfAR->release();
	spfBare->release();
	tau->release(); ior->release(); scat->release();

	std::cout << "=== DielectricARTest: " << s_pass << " passed, " << s_fail << " failed ===\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
