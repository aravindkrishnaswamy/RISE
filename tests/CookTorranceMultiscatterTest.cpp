//////////////////////////////////////////////////////////////////////
//
//  CookTorranceMultiscatterTest.cpp - Regression guard for the 2026-06
//    tinted-multiscatter energy-compensation fix in the Cook-Torrance
//    BRDF/SPF.
//
//    THE FIX (commits at 4 sites: CookTorranceBRDF.cpp value ~L127 /
//    valueNM ~L168, CookTorranceSPF.cpp Scatter ~L203 / ScatterNM ~L356):
//    the Kulla-Conty multiscatter tail must apply the specular tint
//    INSIDE the nonlinear ComputeFms,
//        F_ms = ComputeFms(specColor * F_avg, Eavg)            (NEW, correct)
//    NOT outside it,
//        F_ms = specColor * ComputeFms(F_avg, Eavg)            (OLD, over-bright)
//    where ComputeFms(F) = F^2 * Eavg / (1 - F*(1-Eavg)) is NONLINEAR.
//    The tinted per-bounce reflectance specColor*F_avg compounds across the
//    geometric series of bounces (matching the single-scatter lobe
//    specColor*fresnel); pulling specColor outside the nonlinear Fms makes
//    the multiscatter tail TOO BRIGHT for tinted (specColor<1) rough
//    conductors.  The two forms are EQUAL only at specColor==1.
//
//    There was previously NO Cook-Torrance test in tests/.  This file
//    constructs CookTorranceBRDF / CookTorranceSPF DIRECTLY with uniform-
//    value painters (mirroring ThinFilmBRDFTest's Test G structure, which
//    guards the identical fix in the GGX material).
//
//    METHODOLOGY (matches ThinFilmBRDFTest's Test G) — the production build
//    is -O3 -ffast-math -flto, so a hand-reconstruction of the FULL
//    single+multi model drifts from the compiled value by FP-reassociation
//    noise.  We therefore:
//      * pin CORRECTNESS on the FULL value/valueNM (singleScatter + NEW
//        multi), where the single-scatter is NOT subtracted, to the
//        ~few-1e-3 fast-math reconstruction floor; and
//      * measure TEETH on the ISOLATED multiscatter term (full minus the
//        reconstructed single-scatter lobe) — that isolation is noisier but
//        only needs to discriminate NEW (≈0) from OLD (>5% away), which the
//        large OLD/NEW gap easily clears for specColor<1.
//
//    Cook-Torrance is SIMPLER than the thin-film GGX: the single-scatter
//    Fresnel is the plain conductor reflectance (no Airy stack), the
//    multiscatter F_avg is the SUBSTRATE hemispherical conductor average
//    (ComputeFresnelAvg over the substrate ior/ext), and the lobe uses an
//    ISOTROPIC single roughness (pMasking), so the reconstruction is direct.
//
//    Assertions (per rs in {0.25, 0.5, 1.0}, varying the specular painter):
//      A. CORRECTNESS PIN (RGB value() AND spectral valueNM()) —
//         value == singleScatter + ComputeFms(specColor*F_avg)*f_ms to the
//         fast-math floor.  Reconstruction uses the SAME MicrofacetUtils /
//         MicrofacetEnergyLUT / Optics calls the BRDF uses, with the NM-path
//         specColor read from pSpecular->GetColorNM (the JH-uplifted value,
//         NOT rs) exactly as the library reads it.
//      B. TEETH — the isolated multiscatter term sits >5% from the OLD
//         specColor*ComputeFms(F_avg)*f_ms and >=4x closer to NEW, for
//         rs=0.25 and rs=0.5 (so the test would FAIL against the pre-fix
//         code).  The measured "% from OLD vs % from NEW" deltas are printed.
//      C. PHYSICS DIRECTION — tinted multiscatter DECREASES: NEW < OLD for
//         rs<1 (in BOTH the RGB and NM paths).
//      D. UNTINTED IDENTITY — at rs==1, NEW == OLD == measured to epsilon
//         (the fix is byte-identical on white materials).
//      E. BRDF/SPF CONSISTENCY — the BRDF value() multiscatter term equals
//         the SPF multiscatter kray reconstruction (both route the SAME
//         ComputeFms(specColor*F_avg) — audit-by-bug-pattern's RGB/NM and
//         BRDF/SPF twins).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Materials/CookTorranceBRDF.h"
#include "../src/Library/Materials/CookTorranceSPF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	// A titanium-like absorbing conductor substrate (the same substrate
	// numbers ThinFilmBRDFTest uses), so the hemispherical F_avg fed to the
	// multiscatter tail is well into [0,1) (NOT saturated to ~1, where
	// ComputeFms's nonlinearity would be invisible).
	const Scalar kSubN = 2.74;	// substrate n (Ti-ish)
	const Scalar kSubK = 3.79;	// substrate k (absorbing)

	// Build an RI whose incoming ray travels along rayDir (INTO the
	// surface) with geometric normal +Z.  Mirrors ThinFilmBRDFTest::MakeRI
	// and GGXFresnelModeTest.
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

	// A deterministic sampler replaying a fixed list of 1D draws, then a
	// constant tail.  Used to STEER CookTorranceSPF::Scatter / ScatterNM
	// into the MULTISCATTER lobe (uLobe past pDiffuse+pSpec).
	class ScriptedSampler : public ISampler
	{
		const Scalar*	seq;
		unsigned int	n;
		unsigned int	idx;
		Scalar			tail;
	public:
		ScriptedSampler( const Scalar* s, unsigned int count, Scalar tailVal )
			: seq( s ), n( count ), idx( 0 ), tail( tailVal ) {}

		Scalar Get1D()
		{
			if( idx < n ) return seq[idx++];
			return tail;
		}
		Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
		void StartStream( int /*streamIndex*/ ) {}
	};

	// Owns one Cook-Torrance painter stack at a given base specular tint
	// (rs) and roughness (alpha), with NO diffuse (so value()/valueNM() is
	// purely single-scatter + multiscatter — the diffuse INV_PI term would
	// otherwise pollute the isolation).
	struct Stack
	{
		UniformColorPainter*	diffuse;
		UniformColorPainter*	specular;
		UniformScalarPainter*	masking;	// roughness (isotropic)
		UniformScalarPainter*	ior;
		UniformScalarPainter*	ext;

		Stack( Scalar alpha, Scalar rs )
		{
			diffuse  = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) ); diffuse->addref();
			specular = new UniformColorPainter( RISEPel( rs, rs, rs ) );    specular->addref();
			masking  = new UniformScalarPainter( alpha ); masking->addref();
			ior      = new UniformScalarPainter( kSubN ); ior->addref();
			ext      = new UniformScalarPainter( kSubK ); ext->addref();
		}
		~Stack()
		{
			diffuse->release(); specular->release();
			masking->release(); ior->release(); ext->release();
		}
		CookTorranceBRDF* MakeBRDF() const
		{
			CookTorranceBRDF* b = new CookTorranceBRDF( *diffuse, *specular, *masking, *ior, *ext );
			b->addref();
			return b;
		}
		CookTorranceSPF* MakeSPF() const
		{
			CookTorranceSPF* s = new CookTorranceSPF( *diffuse, *specular, *masking, *ior, *ext );
			s->addref();
			return s;
		}
	};

	// ---- Shared fixed geometry (matches Test G's family) -------------
	// A moderate light direction wi and a small-angle view direction; both
	// nv>0 and nr>0 so the single-scatter factor is non-zero AND the
	// multiscatter gate (cosWo>0 && cosWi>0) is active.
	const Vector3 g_v( std::sin( Scalar( 0.4 ) ), 0, std::cos( Scalar( 0.4 ) ) );	// wi (light)
	Vector3 MakeViewDir()
	{
		const Scalar to = 0.15;
		return Vector3( std::sin( to ) * std::cos( 1.0 ), std::sin( to ) * std::sin( 1.0 ), std::cos( to ) );
	}

	// Roughness: rough enough that the multiscatter tail carries real
	// weight (Eavg well below 1) — matches Test G's alpha=0.50.
	const Scalar kAlpha = 0.50;

	// Reconstruct the single-scatter "factor" = D*G/(4 nr nv) EXACTLY as
	// CookTorranceBRDF::ComputeFactor does (isotropic GGX_D + separable
	// Smith G), through the SAME onb the library uses.
	Scalar ReconstructFactor( const Vector3& v, const RayIntersectionGeometric& ri, Scalar alpha )
	{
		const Vector3 n = ri.onb.w();
		const Vector3 vN = Vector3Ops::Normalize( v );
		const Vector3 r  = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Scalar nr = Vector3Ops::Dot( n, r );
		const Scalar nv = Vector3Ops::Dot( n, vN );
		if( !( nr >= NEARZERO && nv >= NEARZERO ) ) return 0.0;
		const Vector3 h = Vector3Ops::Normalize( vN + r );
		const Scalar hn = Vector3Ops::Dot( n, h );
		const Scalar D = MicrofacetUtils::GGX_D<Scalar>( alpha, hn );
		const Scalar G = MicrofacetUtils::GGX_G( alpha, nv, nr );
		return D * G / ( 4.0 * nr * nv );
	}

	// f_ms = (1-Ess_o)(1-Ess_i)/(PI(1-Eavg)), reconstructed EXACTLY as the
	// BRDF's multiscatter block (Ess at cosWo/cosWi, both LUT-looked-up at
	// alpha).
	Scalar Reconstruct_f_ms( const Vector3& v, const RayIntersectionGeometric& ri, Scalar alpha, Scalar Eavg )
	{
		const Vector3 n = ri.onb.w();
		const Scalar cosWo = Vector3Ops::Dot( n, Vector3Ops::Normalize( -ri.ray.Dir() ) );
		const Scalar cosWi = Vector3Ops::Dot( n, Vector3Ops::Normalize( v ) );
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosWo, alpha );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );
		return ( 1.0 - Ess_o ) * ( 1.0 - Ess_i ) / ( PI * ( 1.0 - Eavg ) );
	}
}

// ============================================================
//  Test A/B/C/D: specColor INSIDE the Kulla-Conty Fms — RGB value().
// ============================================================
//  Per rs, value() = diffuse(0) + singleScatter + multiScatter, where
//    singleScatter = specColor * fresnel * factor
//      fresnel = Optics::CalculateConductorReflectance(ray.Dir(), n, 1, ior, ext)
//      factor  = D*G/(4 nr nv)            (ReconstructFactor)
//    multiScatter = ComputeFms(specColor*F_avg, Eavg) * f_ms   (NEW)
//      F_avg   = ComputeFresnelAvg(n, 1, ior, ext)   (SUBSTRATE conductor avg)
//      f_ms    = (1-Ess_o)(1-Ess_i)/(PI(1-Eavg))     (Reconstruct_f_ms)
//  We pin correctness on the FULL value() (no subtraction), and TEETH on
//  the isolated multiscatter (full minus reconstructed single-scatter).
static bool TestRGBSpecColorInsideMultiscatter()
{
	std::cout << "--- Test A (RGB value()): specColor INSIDE Kulla-Conty Fms ---\n";
	const int startFail = s_fail;

	const Scalar alpha = kAlpha;
	const Scalar Eavg  = MicrofacetEnergyLUT::LookupEavg( alpha );

	const Vector3 v = g_v;
	const Vector3 r = MakeViewDir();
	RayIntersectionGeometric ri = MakeRI( -r );
	const Vector3 n = ri.onb.w();

	const Scalar factor = ReconstructFactor( v, ri, alpha );
	const Scalar f_ms   = Reconstruct_f_ms( v, ri, alpha, Eavg );

	// Single-scatter Fresnel (substrate conductor; per-channel — uniform
	// ior/ext so all three channels are equal).  Use the SAME call the BRDF
	// uses, on the SAME view ray and normal.
	const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
		ri.ray.Dir(), n, RISEPel( 1, 1, 1 ), RISEPel( kSubN, kSubN, kSubN ), RISEPel( kSubK, kSubK, kSubK ) );
	// Substrate hemispherical Fresnel average feeding the multiscatter tail.
	const RISEPel Favg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>(
		n, RISEPel( 1, 1, 1 ), RISEPel( kSubN, kSubN, kSubN ), RISEPel( kSubK, kSubK, kSubK ) );

	std::cout << "  alpha=" << alpha << " Eavg=" << std::fixed << std::setprecision( 4 ) << Eavg
			  << " factor=" << factor << " f_ms=" << f_ms
			  << " fresnel.r=" << fresnel.r << " Favg.r=" << Favg.r << "\n";

	const Scalar rsList[] = { 0.25, 0.5, 1.0 };
	double maxRelFullNew = 0.0;
	int    teethCount = 0;
	int    rsLt1Count = 0;
	bool   identityRs1 = false;
	bool   directionOk = true;

	for( int q = 0; q < 3; ++q )
	{
		const Scalar rs = rsList[q];
		Stack stk( alpha, rs );
		CookTorranceBRDF* brdf = stk.MakeBRDF();

		// RGB specColor is the verbatim painter colour (no JH uplift).
		const RISEPel specColor = stk.specular->GetColor( ri );

		const RISEPel full = brdf->value( v, ri );	// diffuse(0) + single + multi

		// Reconstructed single-scatter lobe.
		const RISEPel singleScatter = specColor * fresnel * factor;

		// NEW (correct): specColor INSIDE the nonlinear Fms.
		const RISEPel fmsNew  = MicrofacetEnergyLUT::ComputeFms<RISEPel>( specColor * Favg, Eavg );
		const RISEPel multiNew = fmsNew * f_ms;
		// OLD (buggy): specColor pulled OUTSIDE the nonlinear Fms.
		const RISEPel fmsOld  = MicrofacetEnergyLUT::ComputeFms<RISEPel>( Favg, Eavg );
		const RISEPel multiOld = specColor * fmsOld * f_ms;

		// (A) correctness on the FULL value() (no subtraction noise).
		const RISEPel predFullNew = singleScatter + multiNew;
		const Scalar  dFull = std::fabs( full.r - predFullNew.r ) + std::fabs( full.g - predFullNew.g ) + std::fabs( full.b - predFullNew.b );
		const Scalar  magFull = r_max( ColorMath::MaxValue( full ), Scalar( 1e-12 ) );
		const double  relFullNew = dFull / ( 3.0 * magFull );
		if( relFullNew > maxRelFullNew ) maxRelFullNew = relFullNew;

		// (B) teeth on the ISOLATED multiscatter (use the .r channel; all
		// channels equal under uniform painters).
		const Scalar multi = full.r - singleScatter.r;
		const double relMultiNew = std::fabs( multi - multiNew.r ) / r_max( std::fabs( multiNew.r ), Scalar( 1e-12 ) );
		const double relMultiOld = std::fabs( multi - multiOld.r ) / r_max( std::fabs( multiOld.r ), Scalar( 1e-12 ) );

		std::cout << "  rs=" << std::fixed << std::setprecision( 2 ) << rs
				  << "  full.r=" << std::scientific << std::setprecision( 4 ) << full.r
				  << " (relFullNew " << relFullNew << ")"
				  << "  multi=" << multi << "  NEW=" << multiNew.r << " (rel " << relMultiNew << ")"
				  << "  OLD=" << multiOld.r << " (rel " << relMultiOld << ")\n";

		if( rs < 0.999 ) {
			++rsLt1Count;
			if( relMultiOld > 0.05 && relMultiNew < relMultiOld * 0.25 ) ++teethCount;
			if( !( multiNew.r < multiOld.r ) ) directionOk = false;
		} else {
			const double relNewOld = std::fabs( multiNew.r - multiOld.r ) / r_max( std::fabs( multiNew.r ), Scalar( 1e-12 ) );
			identityRs1 = ( relNewOld < 1e-2 ) && ( relMultiNew < 1.5e-2 );
		}

		brdf->release();
	}

	Check( maxRelFullNew < 3e-3,
		"RGB value() == singleScatter + ComputeFms(specColor*F_avg)*f_ms for all rs (full-model fast-math floor)" );
	Check( teethCount == rsLt1Count && rsLt1Count >= 2,
		"RGB TEETH: isolated multiscatter is >5% from OLD specColor*ComputeFms(F_avg) and >=4x closer to NEW, for every rs<1" );
	Check( directionOk,
		"RGB physics direction: tinted multiscatter DECREASES (NEW < OLD) for rs<1" );
	Check( identityRs1,
		"RGB (untinted) rs==1 multiscatter unchanged: NEW == OLD == measured" );

	return s_fail == startFail;
}

// ============================================================
//  Test A/B/C/D twin: specColor INSIDE the Fms — spectral valueNM().
// ============================================================
//  IMPORTANT: the NM path multiplies by specColor = pSpecular->GetColorNM
//  (the JH-uplifted spectral value of the RGB tint at nm, NONLINEAR in rs),
//  NOT rs itself.  We query that exact specColor and use it in BOTH the
//  single-scatter reconstruction AND the NEW/OLD formulas, or the
//  reconstruction is inconsistent (the trap Test G called out).
static bool TestNMSpecColorInsideMultiscatter()
{
	std::cout << "\n--- Test B (spectral valueNM()): specColor INSIDE Kulla-Conty Fms ---\n";
	const int startFail = s_fail;

	const Scalar alpha = kAlpha;
	const Scalar Eavg  = MicrofacetEnergyLUT::LookupEavg( alpha );

	const Vector3 v = g_v;
	const Vector3 r = MakeViewDir();
	RayIntersectionGeometric ri = MakeRI( -r );
	const Vector3 n = ri.onb.w();

	const Scalar factor = ReconstructFactor( v, ri, alpha );
	const Scalar f_ms   = Reconstruct_f_ms( v, ri, alpha, Eavg );

	const Scalar rsList[] = { 0.25, 0.5, 1.0 };
	const Scalar nms[]    = { 450.0, 560.0, 650.0 };	// a few hero wavelengths

	double maxRelFullNew = 0.0;
	int    teethCount = 0;
	int    tintedCells = 0;	// cells whose QUERIED specColor is < 1 (the nonlinearity bites)
	int    untintedCells = 0;	// cells whose QUERIED specColor ≈ 1 (NEW must equal OLD)
	int    skippedDegenerate = 0;	// cells where the multiscatter term is below the noise floor
	bool   identityUntinted = true;	// AND across untinted (specColor≈1) cells
	bool   directionOk = true;

	for( int q = 0; q < 3; ++q )
	{
		const Scalar rs = rsList[q];
		Stack stk( alpha, rs );
		CookTorranceBRDF* brdf = stk.MakeBRDF();

		for( int w = 0; w < 3; ++w )
		{
			const Scalar nm = nms[w];

			// Per-lambda quantities (substrate ior/ext are constant in nm
			// here, but query them through the SAME nm-path the BRDF uses).
			const Scalar iorVal = stk.ior->GetValueAtNM( ri, nm );
			const Scalar extVal = stk.ext->GetValueAtNM( ri, nm );
			const Scalar fresnel = Optics::CalculateConductorReflectance<Scalar>(
				ri.ray.Dir(), n, 1.0, iorVal, extVal );
			const Scalar Favg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );

			// The EXACT per-lambda specColor the library multiplies in
			// (JH-uplifted; NOT rs).
			const Scalar specColor = stk.specular->GetColorNM( ri, nm );

			const Scalar full = brdf->valueNM( v, ri, nm );	// diffuse(0) + single + multi

			const Scalar singleScatter = specColor * fresnel * factor;

			// NEW (correct): specColor INSIDE the nonlinear Fms.
			const Scalar fmsNew  = MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * Favg, Eavg );
			const Scalar multiNew = fmsNew * f_ms;
			// OLD (buggy): specColor OUTSIDE.
			const Scalar fmsOld  = MicrofacetEnergyLUT::ComputeFms<Scalar>( Favg, Eavg );
			const Scalar multiOld = specColor * fmsOld * f_ms;

			// DEGENERATE-CELL GUARD (adversarial concern (e)): the JH uplift
			// of white (1,1,1) is NOT a flat spectrum — at some wavelengths
			// (e.g. the rs=1 deep-red corner) uplift(white) collapses toward
			// ~0, so BOTH single- and multi-scatter shrink to ~1e-5 and the
			// fast-math single-scatter subtraction is pure noise.  Such a cell
			// carries no signal about the specColor-inside-Fms direction, so
			// skip it.  Require the NEW multiscatter term to be a non-trivial
			// fraction of the full value before we measure anything.
			if( multiNew < 1e-4 || full < 1e-3 ) {
				++skippedDegenerate;
				std::cout << "  rs=" << std::fixed << std::setprecision( 2 ) << rs
						  << " nm=" << std::setprecision( 0 ) << nm
						  << " specColor=" << std::setprecision( 4 ) << specColor
						  << "  [skipped: multiscatter below noise floor]\n";
				continue;
			}

			// (A) correctness on the FULL valueNM().
			const Scalar predFullNew = singleScatter + multiNew;
			const double relFullNew = std::fabs( full - predFullNew ) / r_max( std::fabs( full ), Scalar( 1e-12 ) );
			if( relFullNew > maxRelFullNew ) maxRelFullNew = relFullNew;

			// (B) teeth on the ISOLATED multiscatter.
			const Scalar multi = full - singleScatter;
			const double relMultiNew = std::fabs( multi - multiNew ) / r_max( std::fabs( multiNew ), Scalar( 1e-12 ) );
			const double relMultiOld = std::fabs( multi - multiOld ) / r_max( std::fabs( multiOld ), Scalar( 1e-12 ) );

			std::cout << "  rs=" << std::fixed << std::setprecision( 2 ) << rs
					  << " nm=" << std::setprecision( 0 ) << nm
					  << " specColor=" << std::setprecision( 4 ) << specColor
					  << "  full=" << std::scientific << std::setprecision( 3 ) << full
					  << " (relFullNew " << relFullNew << ")"
					  << "  multi=" << multi << " NEW=" << multiNew << " (rel " << relMultiNew << ")"
					  << " OLD=" << multiOld << " (rel " << relMultiOld << ")\n";

			// Classify by the QUERIED specColor (the physical driver of the
			// nonlinearity), NOT the nominal rs: uplift(white) at some nm is
			// itself < 1, which is a genuine tinted case.
			if( specColor < 0.95 ) {
				++tintedCells;
				if( relMultiOld > 0.05 && relMultiNew < relMultiOld * 0.25 ) ++teethCount;
				if( !( multiNew < multiOld ) ) directionOk = false;
			} else {
				++untintedCells;
				const double relNewOld = std::fabs( multiNew - multiOld ) / r_max( std::fabs( multiNew ), Scalar( 1e-12 ) );
				if( !( relNewOld < 1e-2 && relMultiNew < 1.5e-2 ) ) identityUntinted = false;
			}
		}

		brdf->release();
	}

	std::cout << "  tintedCells=" << tintedCells << " untintedCells=" << untintedCells
			  << " skippedDegenerate=" << skippedDegenerate << "\n";

	Check( maxRelFullNew < 3e-3,
		"NM valueNM() == singleScatter + ComputeFms(specColor*F_avg)*f_ms for all measured (rs, lambda) (fast-math floor)" );
	Check( teethCount == tintedCells && tintedCells >= 2,
		"NM TEETH: isolated multiscatter is >5% from OLD and >=4x closer to NEW, for every tinted (specColor<1) cell" );
	Check( directionOk,
		"NM physics direction: tinted multiscatter DECREASES (NEW < OLD) for specColor<1" );
	Check( untintedCells >= 1 && identityUntinted,
		"NM (untinted, specColor≈1) multiscatter unchanged: NEW == OLD == measured" );

	return s_fail == startFail;
}

// ============================================================
//  Test E: BRDF value() multiscatter == SPF multiscatter kray.
// ============================================================
//  The SPF's multiscatter lobe (CookTorranceSPF::ScatterNM) emits
//    krayNM = F_ms * (1-Ess_o)(1-Ess_i) / ((1-Eavg) * pMSSelect)
//  where F_ms = ComputeFms(specColor*F_avg, Eavg).  Multiplying back by
//  pMSSelect and dividing by PI recovers the BRDF's multiscatter term
//  F_ms*f_ms.  Since the SPF picks the lobe stochastically, we STEER the
//  scripted sampler past pDiffuse(=0)+pSpec into the multiscatter lobe,
//  then reconstruct pMSSelect from the same painter weights the SPF uses.
//  This is the BRDF/SPF twin guard: both must apply specColor INSIDE Fms.
static bool TestBRDFvsSPFMultiscatter()
{
	std::cout << "\n--- Test E: BRDF value() multiscatter == SPF kray multiscatter ---\n";
	const int startFail = s_fail;

	const Scalar alpha = kAlpha;
	const Scalar Eavg  = MicrofacetEnergyLUT::LookupEavg( alpha );
	const Scalar nm    = 560.0;

	const Vector3 v = g_v;
	const Vector3 r = MakeViewDir();
	RayIntersectionGeometric ri = MakeRI( -r );
	const Vector3 n = ri.onb.w();

	const Scalar rsList[] = { 0.25, 0.5, 1.0 };
	double maxRel = 0.0;
	int    compared = 0;

	for( int q = 0; q < 3; ++q )
	{
		const Scalar rs = rsList[q];
		Stack stk( alpha, rs );
		CookTorranceSPF* spf = stk.MakeSPF();

		// SPF lobe-selection weights (ScatterNM): wd=GetColorNM(diffuse)=0,
		// ws=GetColorNM(spec).  H6 (2026-07): wms is now DIRECTION-AWARE --
		// ws*(1-Ess(cosWi,alpha)), not ws*(1-Eavg) -- see MicrofacetEnergyLUT.h
		// header comment above MSLobeZ/SampleMSCosTheta/MSPdf and
		// GGXSPF.cpp/CookTorranceSPF.cpp's "H6:" comments.  pMSSelect = wms/total.
		const Scalar wd = stk.diffuse->GetColorNM( ri, nm );	// 0
		const Scalar ws = stk.specular->GetColorNM( ri, nm );
		const Scalar cosWi = Vector3Ops::Dot( Vector3Ops::Normalize( -ri.ray.Dir() ), n );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );
		const Scalar wms = ws * ( 1.0 - Ess_i );
		const Scalar total = wd + ws + wms;
		const Scalar pDiffuseSelect = ( total > 1e-10 ) ? wd / total : 1.0;
		const Scalar pSpecSelect    = ( total > 1e-10 ) ? ws / total : 0.0;
		const Scalar pMSSelect = 1.0 - pDiffuseSelect - pSpecSelect;
		const Scalar msZ = MicrofacetEnergyLUT::MSLobeZ( alpha );

		// Drive uLobe into the MULTISCATTER branch: uLobe must be >=
		// pDiffuseSelect + pSpecSelect.  First draw = uLobe, then two draws
		// for the (H6) MS-lobe direction sampler.  Use 0.999 (safely past
		// the spec cutoff) then mid draws.
		const Scalar script[] = { 0.999, 0.5, 0.5 };
		ScriptedSampler smp( script, 3, 0.5 );
		ScatteredRayContainer sc;
		IORStack ist( 1.0 );
		spf->ScatterNM( ri, smp, nm, sc, ist );

		// Find the multiscatter ray (type eRayDiffuse with diffuse painter
		// == 0, so the only diffuse-typed ray here is the multiscatter one).
		int idx = -1;
		for( unsigned int k = 0; k < sc.Count(); ++k )
			if( sc[k].type == ScatteredRay::eRayDiffuse ) { idx = (int) k; break; }
		if( idx < 0 ) { spf->release(); continue; }

		// H6: krayNM = f_ms*cosWo/(msPdf(cosWo)*pMSSelect), the honest f/pdf
		// estimator (was the hard-coded (1-Eavg)-only closed form).  The SPF
		// samples wo via the MS-lobe importance sampler, so Ess_o is at the
		// SAMPLED cosTheta, not the BRDF's view cosWo — recompute Ess/msPdf at
		// the SPF's actual wo to match (the specColor-in-Fms factor is
		// wo-independent).
		const Vector3 wo = Vector3Ops::Normalize( sc[idx].ray.Dir() );
		const Scalar cosThetaWo = Vector3Ops::Dot( wo, n );
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosThetaWo, alpha );
		const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosThetaWo, alpha, msZ );

		// Reconstruct F_ms the SPF SHOULD have used (NEW form): specColor
		// INSIDE.  specColor in the SPF is GetColorNM (== ws here).
		const Scalar iorVal = stk.ior->GetValueAtNM( ri, nm );
		const Scalar extVal = stk.ext->GetValueAtNM( ri, nm );
		const Scalar Favg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
		const Scalar fmsNew = MicrofacetEnergyLUT::ComputeFms<Scalar>( ws * Favg, Eavg );
		const Scalar f_ms = fmsNew * ( 1.0 - Ess_o ) * ( 1.0 - Ess_i ) * INV_PI / ( 1.0 - Eavg );
		const Scalar krayExpected = ( msPdfHere > 1e-14 ) ?
			f_ms * cosThetaWo / ( msPdfHere * pMSSelect ) : Scalar(0);

		const Scalar krayGot = sc[idx].krayNM;
		const double rel = std::fabs( krayGot - krayExpected ) / r_max( std::fabs( krayExpected ), Scalar( 1e-12 ) );
		if( rel > maxRel ) maxRel = rel;
		++compared;

		std::cout << "  rs=" << std::fixed << std::setprecision( 2 ) << rs
				  << " pMSSelect=" << std::setprecision( 4 ) << pMSSelect
				  << "  kray got=" << std::scientific << std::setprecision( 4 ) << krayGot
				  << " expected(NEW)=" << krayExpected << " rel=" << rel << "\n";

		spf->release();
	}

	Check( compared >= 3, "SPF multiscatter exercised for all rs" );
	Check( maxRel < 1e-9,
		"SPF ScatterNM multiscatter kray == ComputeFms(specColor*F_avg)*... (BRDF/SPF twin, specColor INSIDE Fms)" );

	return s_fail == startFail;
}

// ============================================================
//  H6 (2026-07): multiscatter-lobe estimator reformulation tests.
// ============================================================
//  These three tests validate MicrofacetEnergyLUT.h's MSLobeZ / MSPdf /
//  SampleMSCosTheta DIRECTLY (no SPF/BRDF construction needed) -- they are
//  the "pdf integrates to 1", "histogram matches pdf", and "kray collapses
//  to a bounded constant" checks from the H6 design note.  A fourth test
//  exercises the actual GGXSPF/CookTorranceSPF end-to-end at grazing
//  incidence, reproducing the original defect's regime (alpha~0.02,
//  cosWi->0) and checking the reported kray no longer blows up.

namespace
{
	// Composite Simpson's rule for f over [a,b] with nSeg (even) intervals.
	template< class F >
	Scalar SimpsonIntegrate( F f, Scalar a, Scalar b, int nSeg )
	{
		if( nSeg % 2 != 0 ) ++nSeg;
		const Scalar h = ( b - a ) / nSeg;
		Scalar sum = f( a ) + f( b );
		for( int i = 1; i < nSeg; ++i )
		{
			const Scalar x = a + i * h;
			sum += ( i % 2 == 0 ) ? 2.0 * f( x ) : 4.0 * f( x );
		}
		return sum * h / 3.0;
	}
}

// ---- H6 Test 1: msPdf hemisphere-integrates to 1 -------------------
static bool TestMSPdfIntegratesToOne()
{
	std::cout << "\n--- H6 Test 1: MSPdf integrates to 1 over the hemisphere ---\n";
	const int startFail = s_fail;

	const Scalar alphas[] = { 0.01, 0.02, 0.1, 0.5 };
	bool allOk = true;

	for( Scalar alpha : alphas )
	{
		const Scalar Z = MicrofacetEnergyLUT::MSLobeZ( alpha );
		auto pdfOfCos = [&]( Scalar c ) { return MicrofacetEnergyLUT::MSPdf( c, alpha, Z ); };
		// hemisphereIntegral = integral over solid angle = 2*PI * integral_0^1 pdf(cos) dcos
		// (azimuth uniform; cos itself is the sampling variable, matching the
		// codebase's cosTheta*INV_PI convention for cosine-hemisphere pdfs).
		const Scalar integCos = SimpsonIntegrate( pdfOfCos, Scalar(0.0), Scalar(1.0), 20000 );
		const Scalar hemiInteg = TWO_PI * integCos;
		const double rel = std::fabs( hemiInteg - 1.0 );

		std::cout << "  alpha=" << std::fixed << std::setprecision( 3 ) << alpha
				  << "  Z=" << std::setprecision( 6 ) << Z
				  << "  hemisphereIntegral=" << std::setprecision( 8 ) << hemiInteg
				  << "  |1-integral|=" << std::scientific << std::setprecision( 3 ) << rel << "\n";

		if( rel > 1e-4 ) allOk = false;
	}

	Check( allOk, "MSPdf hemisphere-integrates to 1.0 (within 1e-4) for alpha in {0.01,0.02,0.1,0.5}" );
	return s_fail == startFail;
}

// ---- H6 Test 2: histogram of SampleMSCosTheta matches MSPdf ---------
static bool TestMSHistogramVsPdf()
{
	std::cout << "\n--- H6 Test 2: SampleMSCosTheta histogram vs MSPdf ---\n";
	const int startFail = s_fail;

	const Scalar alphas[] = { 0.02, 0.5 };
	const int nBins = 20;
	const long nSamples = 300000;
	bool allOk = true;

	for( Scalar alpha : alphas )
	{
		const Scalar Z = MicrofacetEnergyLUT::MSLobeZ( alpha );

		long hist[nBins] = { 0 };
		for( long s = 0; s < nSamples; ++s )
		{
			const Scalar u1 = GlobalRNG().CanonicalRandom();
			const Scalar c = MicrofacetEnergyLUT::SampleMSCosTheta( alpha, u1 );
			int bin = (int) ( c * nBins );
			if( bin < 0 ) bin = 0;
			if( bin >= nBins ) bin = nBins - 1;
			++hist[bin];
		}

		bool alphaOk = true;
		for( int b = 0; b < nBins; ++b )
		{
			const Scalar lo = Scalar(b) / nBins;
			const Scalar hi = Scalar(b+1) / nBins;
			// Analytic bin probability mass: integral_lo^hi 2*PI*MSPdf(cos) dcos.
			auto pdfOfCos = [&]( Scalar c ) { return MicrofacetEnergyLUT::MSPdf( c, alpha, Z ); };
			const Scalar analyticP = TWO_PI * SimpsonIntegrate( pdfOfCos, lo, hi, 40 );
			const Scalar empiricalP = Scalar( hist[b] ) / Scalar( nSamples );

			// Generous statistical tolerance: 5-sigma binomial + a 1% floor
			// (guards near-zero-probability bins where 5-sigma itself -> 0).
			const Scalar sigma = sqrt( r_max( Scalar(0.0), analyticP * ( 1.0 - analyticP ) / nSamples ) );
			const Scalar tol = r_max( Scalar(0.01), Scalar(5.0) * sigma );
			const bool binOk = std::fabs( empiricalP - analyticP ) <= tol;
			if( !binOk ) alphaOk = false;
		}

		std::cout << "  alpha=" << std::fixed << std::setprecision( 3 ) << alpha
				  << "  nSamples=" << nSamples << "  bins " << ( alphaOk ? "all within tolerance" : "SOME OUT OF TOLERANCE" ) << "\n";
		if( !alphaOk )
		{
			for( int b = 0; b < nBins; ++b )
			{
				const Scalar lo = Scalar(b) / nBins;
				const Scalar hi = Scalar(b+1) / nBins;
				auto pdfOfCos = [&]( Scalar c ) { return MicrofacetEnergyLUT::MSPdf( c, alpha, Z ); };
				const Scalar analyticP = TWO_PI * SimpsonIntegrate( pdfOfCos, lo, hi, 40 );
				const Scalar empiricalP = Scalar( hist[b] ) / Scalar( nSamples );
				std::cout << "    bin[" << lo << "," << hi << "] analytic=" << analyticP << " empirical=" << empiricalP << "\n";
			}
		}

		if( !alphaOk ) allOk = false;
	}

	Check( allOk, "SampleMSCosTheta's empirical histogram matches MSPdf's analytic bin mass (5-sigma binomial tolerance)" );
	return s_fail == startFail;
}

// ---- H6 Test 3: MS-lobe kray collapses to a bounded constant --------
//  Reproduces the design note's derivation directly:
//    f_ms = F_ms*(1-Ess_o)*(1-Ess_i)/(PI*(1-Eavg))     (BRDF value; UNCHANGED)
//    kray = f_ms * cosWo / (MSPdf(cosWo) * pMSSelect)
//  With F_ms and pMSSelect held at 1 (they are cosWo-independent constants
//  that only rescale the whole expression), kray must reduce to EXACTLY
//  Z*(1-Ess_i)/(1-Eavg) for EVERY probe cosWo -- this is the algebraic
//  cancellation that eliminates the old estimator's grazing-incidence
//  blow-up.  Checked at alphaEff in {0.01,0.02,0.1,0.5} x grazing-to-normal
//  cosWi in {0.999,0.5,0.05,0.005}, each against 7 probe cosWo spanning the
//  full [0,1] range including both LUT end-caps.
static bool TestMSKrayClosedFormConstant()
{
	std::cout << "\n--- H6 Test 3: MS-lobe kray == closed-form constant (grazing cosWi incl.) ---\n";
	const int startFail = s_fail;

	const Scalar alphas[] = { 0.01, 0.02, 0.1, 0.5 };
	const Scalar cosWis[]  = { 0.999, 0.5, 0.05, 0.005 };
	const Scalar cosWos[]  = { 0.001, 0.01, 0.05, 0.2, 0.5, 0.8, 0.999 };

	double maxRel = 0.0;
	int nChecked = 0;

	for( Scalar alpha : alphas )
	{
		const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
		const Scalar Z = MicrofacetEnergyLUT::MSLobeZ( alpha );

		for( Scalar cosWi : cosWis )
		{
			const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );
			const Scalar closedForm = Z * ( 1.0 - Ess_i ) / ( 1.0 - Eavg );

			for( Scalar cosWo : cosWos )
			{
				const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosWo, alpha );
				const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosWo, alpha, Z );
				// f_ms with a synthetic F_ms=1 (material-dependent Fresnel
				// factors are cosWo-independent and drop straight through).
				const Scalar f_ms = ( 1.0 - Ess_o ) * ( 1.0 - Ess_i ) * INV_PI / ( 1.0 - Eavg );
				const Scalar reducedKray = ( msPdfHere > 1e-14 ) ? f_ms * cosWo / msPdfHere : Scalar(-1);

				if( reducedKray < 0 ) continue;	// msPdf underflowed at this probe; skip

				const double rel = std::fabs( reducedKray - closedForm ) / r_max( std::fabs( closedForm ), Scalar( 1e-12 ) );
				if( rel > maxRel ) maxRel = rel;
				++nChecked;
			}
		}
		std::cout << "  alpha=" << std::fixed << std::setprecision( 3 ) << alpha
				  << "  Eavg=" << std::setprecision( 5 ) << Eavg
				  << "  Z=" << std::setprecision( 6 ) << Z << "\n";
	}

	std::cout << "  checked " << nChecked << " (alpha,cosWi,cosWo) combinations, maxRel=" << std::scientific << std::setprecision( 3 ) << maxRel << "\n";

	Check( nChecked >= 100, "kray closed-form check exercised across the full alpha x cosWi x cosWo grid" );
	Check( maxRel < 1e-6, "MS-lobe reduced kray == Z*(1-Ess_i)/(1-Eavg) within 1e-6 relative, for ALL probe cosWo including grazing cosWi" );

	return s_fail == startFail;
}

// ---- H6 Test 4: end-to-end SPF grazing-incidence kray sanity --------
//  Reproduces the ORIGINAL defect's regime directly through the real SPF:
//  alpha~0.02 (smooth), cosWi->0 (grazing, e.g. a glint-tilted shading
//  frame).  Before H6, kray was measured up to ~96 at selection probability
//  ~6e-4 on this regime; after H6 it must stay bounded and (for a FIXED
//  incident direction) constant across independently-sampled MS-lobe wo's.
static bool TestMSSPFGrazingKraySanity()
{
	std::cout << "\n--- H6 Test 4: CookTorranceSPF MS-lobe kray at grazing cosWi ---\n";
	const int startFail = s_fail;

	const Scalar alpha = 0.02;	// smooth: 1-Eavg is tiny, the old estimator's danger zone
	const Scalar rs = 0.9;
	const Scalar nm = 560.0;

	// A near-grazing incident ray: wi close to the tangent plane (cosWi small).
	const Vector3 wiDir = Vector3Ops::Normalize( Vector3( std::sin( Scalar(1.55) ), 0, std::cos( Scalar(1.55) ) ) );	// ~cosWi=0.02
	RayIntersectionGeometric ri = MakeRI( -wiDir );
	const Vector3 n = ri.onb.w();
	const Scalar cosWi = Vector3Ops::Dot( wiDir, n );
	std::cout << "  cosWi=" << std::fixed << std::setprecision( 4 ) << cosWi << " (grazing)\n";

	Stack stk( alpha, rs );
	CookTorranceSPF* spf = stk.MakeSPF();

	// Draw the MS lobe several times with DIFFERENT u1ms (different wo each
	// time) via a scripted sampler: first draw pins uLobe into the MS
	// branch, second/third draws are (u1ms,u2ms).
	const Scalar u1msProbes[] = { 0.05, 0.25, 0.5, 0.75, 0.95 };
	double maxKray = 0.0;
	double minKray = 1e300;
	int nDrawn = 0;

	for( Scalar u1ms : u1msProbes )
	{
		const Scalar script[] = { 0.999, u1ms, 0.37 };
		ScriptedSampler smp( script, 3, 0.5 );
		ScatteredRayContainer sc;
		IORStack ist( 1.0 );
		spf->ScatterNM( ri, smp, nm, sc, ist );

		int idx = -1;
		for( unsigned int k = 0; k < sc.Count(); ++k )
			if( sc[k].type == ScatteredRay::eRayDiffuse ) { idx = (int) k; break; }
		if( idx < 0 ) continue;

		const Scalar kray = sc[idx].krayNM;
		std::cout << "  u1ms=" << std::fixed << std::setprecision( 2 ) << u1ms
				  << "  krayNM=" << std::scientific << std::setprecision( 5 ) << kray << "\n";
		if( kray > maxKray ) maxKray = kray;
		if( kray < minKray ) minKray = kray;
		++nDrawn;
	}

	spf->release();

	Check( nDrawn >= 4, "MS lobe sampled successfully across multiple u1ms draws at grazing cosWi" );
	Check( maxKray < 5.0, "MS-lobe kray stays bounded (<5) at grazing cosWi on a smooth surface (was up to ~96 pre-H6)" );
	const double spread = ( minKray > 1e-12 ) ? ( maxKray - minKray ) / minKray : 0.0;
	Check( spread < 1e-4, "MS-lobe kray is CONSTANT (within 1e-4 relative) across independently-sampled wo's at fixed cosWi" );

	return s_fail == startFail;
}

int main()
{
	std::cout << "=== Cook-Torrance Multiscatter (tinted energy fix) Test ===\n";
	GlobalLog();	// initialize the global log

	TestRGBSpecColorInsideMultiscatter();
	TestNMSpecColorInsideMultiscatter();
	TestBRDFvsSPFMultiscatter();
	TestMSPdfIntegratesToOne();
	TestMSHistogramVsPdf();
	TestMSKrayClosedFormConstant();
	TestMSSPFGrazingKraySanity();

	std::cout << "\n=== CookTorranceMultiscatterTest: " << s_pass << " passed, " << s_fail << " failed ===\n";
	return s_fail == 0 ? 0 : 1;
}
