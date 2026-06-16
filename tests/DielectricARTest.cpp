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
#include "../src/Library/Materials/DielectricSPF.h"

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

	// ---------------------------------------------------------------
	// PART 2 - DielectricSPF INTEGRATION (ScatterNM reflection weight).
	// ---------------------------------------------------------------
	// Reference-counted painters (protected dtor) must be heap + release().
	UniformScalarPainter* tau  = new UniformScalarPainter( 1.0 );    tau->addref();
	UniformScalarPainter* ior  = new UniformScalarPainter( kSapph ); ior->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 1.0e6 );  scat->addref();  // huge => no scatter cone

	DielectricSPF* spfAR   = new DielectricSPF( *tau, *ior, *scat, false, kMgF2, 0.0, kArD ); spfAR->addref();
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

	spfAR->release();
	spfBare->release();
	tau->release(); ior->release(); scat->release();

	std::cout << "=== DielectricARTest: " << s_pass << " passed, " << s_fail << " failed ===\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
