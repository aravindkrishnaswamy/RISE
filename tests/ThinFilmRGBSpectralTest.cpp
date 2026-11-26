//////////////////////////////////////////////////////////////////////
//
//  ThinFilmRGBSpectralTest.cpp - Regression for the dispersion-correct
//    RGB (preview + OIDN albedo) thin-film path (Finding #2,
//    docs/THIN_FILM_INTERFERENCE.md §8).
//
//    THE BUG (pre-fix): the RGB-preview / albedo integrators
//    (ThinFilm::ReflectanceConductorRGB / FresnelAvgConductorRGB)
//    integrated the interference reflectance over 380..780 nm but read
//    the optical constants ONCE, at their .v[0] REPRESENTATIVE value
//    (~555 nm) — built N0/N1/Ns OUTSIDE the wavelength loop.  For a
//    wavelength-VARYING (file-based, e.g. Ti / TiO2) n/k stack that
//    drops the dispersion: every integration wavelength saw the 555 nm
//    indices.  The hero-wavelength SPECTRAL path already sampled per-λ
//    and was correct; only the RGB path was wrong.
//
//    THE FIX: ReflectanceConductorRGBSpectral / FresnelAvgConductorRGBSpectral
//    rebuild the complex indices (and Snell invariant) INSIDE the λ loop
//    from a caller-supplied `stackAt(nm, ...)` functor, sampling the
//    optical constants at EACH integration wavelength via
//    IScalarPainter::GetValueAtNM.  The constant-n/k entry points are
//    now thin wrappers that pass a constant-returning functor, so they
//    are BIT-IDENTICAL to the former implementation.
//
//    Assertions:
//      (a) DISPERSION MATTERS — the per-λ Ti/TiO2 RGB reflectance differs
//          MEASURABLY (>1% in at least one channel) from the OLD
//          constant-.v[0](555 nm) ReflectanceConductorRGB, proving the
//          fix changes the answer for REAL dispersive data (teeth).
//      (b) ORACLE MATCH — ReflectanceConductorRGBSpectral matches an
//          INDEPENDENT, hand-rolled per-λ CMF integral (same band/step/
//          von-Kries white) to a tight tolerance, for both a dispersive
//          Ti/TiO2 stack and across several thicknesses/angles.
//      (c) BIT-IDENTITY ON UNIFORM STACK — a wavelength-INDEPENDENT
//          (non-dispersive) stack gives BIT-IDENTICAL RGB old-vs-new for
//          BOTH ReflectanceConductorRGB and FresnelAvgConductorRGB
//          (so the canonical scenes / ThinFilmBRDFTest Test E don't move).
//      (d) PAINTER-FUNCTOR PATH — the production functor pattern (a real
//          IScalarPainter whose GetValueAtNM interpolates the file) feeds
//          ReflectanceConductorRGBSpectral and equals the direct-table
//          oracle, exercising the exact code the BSDF sites run.
//
//    Optical-constant data: the real colors/thinfilm/substrates/Ti.{n,k}
//    + colors/thinfilm/oxides/TiO2.{n,k} files, loaded via the P1-C
//    header-only loader (tests/thinfilm/OpticalConstants.h), whose
//    interpolation is bit-for-bit the production
//    PiecewiseLinearScalarPainter convention (the painter that
//    `scalar_painter { file ... }` constructs).
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
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/ThinFilm.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"

// Header-only P1-C optical-constant loader (no renderer dependency; same
// piecewise-linear + flat-clamp interpolation as PiecewiseLinearScalarPainter).
#include "thinfilm/OpticalConstants.h"

using namespace RISE;

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

	// Repo root: probe a few candidate roots for the optical-constant data
	// (mirrors ThinFilmAnodizeSwatchTest::ResolveRepoRoot).  An explicit
	// RISE_MEDIA_PATH wins if set.  Returns "." if nothing resolves (the
	// loader then reports the missing-data failure loudly).
	std::string ResolveRepoRoot()
	{
		const char* env = std::getenv( "RISE_MEDIA_PATH" );
		if( env && *env ) {
			std::string probe = std::string( env ) + "/colors/thinfilm/substrates/Ti.n";
			FILE* f = std::fopen( probe.c_str(), "r" );
			if( f ) { std::fclose( f ); return env; }
		}
		static const char* kCandidates[] = { ".", "..", "../..", "../../..", "../../../.." };
		for( const char* cand : kCandidates ) {
			std::string probe = std::string( cand ) + "/colors/thinfilm/substrates/Ti.n";
			FILE* f = std::fopen( probe.c_str(), "r" );
			if( f ) { std::fclose( f ); return cand; }
		}
		return ".";
	}

	// A minimal file-backed IScalarPainter: GetValueAtNM interpolates the
	// loaded (nm,value) table exactly like the production
	// PiecewiseLinearScalarPainter; GetValuesAt reports the 555 nm
	// representative (the same kRepresentativeNm contract).  This is the
	// production functor pattern the BSDF sites run, so test (d) exercises
	// the real path end-to-end.
	class TableScalarPainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
		ThinFilmReference::SpectralTable table;
	protected:
		virtual ~TableScalarPainter() {}
	public:
		explicit TableScalarPainter( const ThinFilmReference::SpectralTable& t ) : table( t ) {}

		ScalarTriple GetValuesAt( const RayIntersectionGeometric& /*ri*/ ) const override
		{
			return ScalarTriple( Scalar( table.EvalAtNM( 555.0 ) ) );
		}
		Scalar GetValueAtNM( const RayIntersectionGeometric& /*ri*/, Scalar nm ) const override
		{
			return Scalar( table.EvalAtNM( double( nm ) ) );
		}
		bool HasPerChannelVariation() const override { return false; }
	};

	// Maximum absolute per-channel difference between two RISEPels.
	double MaxChanDiff( const RISEPel& a, const RISEPel& b )
	{
		return r_max( r_max( std::fabs( a.r - b.r ), std::fabs( a.g - b.g ) ),
		              std::fabs( a.b - b.b ) );
	}

	// Maximum relative per-channel difference (guarded denominator).
	double MaxChanRelDiff( const RISEPel& a, const RISEPel& b )
	{
		const double dr = std::fabs( a.r - b.r ) / r_max( std::fabs( a.r ), 1e-9 );
		const double dg = std::fabs( a.g - b.g ) / r_max( std::fabs( a.g ), 1e-9 );
		const double db = std::fabs( a.b - b.b ) / r_max( std::fabs( a.b ), 1e-9 );
		return r_max( r_max( dr, dg ), db );
	}

	// INDEPENDENT hand-rolled per-λ oracle for the §8 RGB albedo-basis
	// integral.  Deliberately a SEPARATE implementation from ThinFilm.h:
	// same band (380..780), same kRGBIntegrationSamples midpoint
	// quadrature, same equal-energy relative tristimulus + von-Kries-to-D65
	// white + XYZ->Rec.709, but written here independently so a coincidental
	// bug in ThinFilm.h that this oracle DOESN'T share would diverge.  The
	// stack (n,k per λ) is supplied by `stackAt`, exactly the functor the
	// renderer passes.
	template<class StackFn>
	RISEPel OracleReflectanceRGB( Scalar cosThetaI, Scalar thickness_nm, const StackFn& stackAt )
	{
		const Scalar loNm = Scalar( 380 );
		const Scalar hiNm = Scalar( 780 );
		const int    N    = ThinFilm::kRGBIntegrationSamples;
		const Scalar step = ( hiNm - loNm ) / Scalar( N );

		Scalar Xn = 0, Yn = 0, Zn = 0;
		Scalar Xe = 0, Ye = 0, Ze = 0;

		for( int s = 0; s < N; ++s ) {
			const Scalar nm = loNm + ( Scalar( s ) + Scalar( 0.5 ) ) * step;
			XYZPel cmf;
			if( !ColorUtils::XYZFromNM( cmf, nm ) ) {
				continue;
			}
			Scalar n0, k0, n1, k1, n2, k2;
			stackAt( nm, n0, k0, n1, k1, n2, k2 );

			// Independent unpolarized reflectance via the public single-
			// wavelength hot-path entry (NOT the RGB integrator), so the
			// oracle does not reuse ReflectanceConductorRGBSpectral's loop.
			Scalar R = ThinFilm::ReflectanceConductor( cosThetaI, nm, n0, k0, n1, k1, thickness_nm, n2, k2 );
			if( R < 0 ) R = 0;
			if( R > 1 ) R = 1;

			Xn += R * cmf.X * step;  Yn += R * cmf.Y * step;  Zn += R * cmf.Z * step;
			Xe += cmf.X * step;      Ye += cmf.Y * step;      Ze += cmf.Z * step;
		}

		const Scalar relX = ( Xe > 0 ) ? Xn / Xe : Scalar( 0 );
		const Scalar relY = ( Ye > 0 ) ? Yn / Ye : Scalar( 0 );
		const Scalar relZ = ( Ze > 0 ) ? Zn / Ze : Scalar( 0 );

		const Rec709RGBPel whiteRGB( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) );
		const XYZPel whiteXYZ = ColorUtils::Rec709RGBtoXYZ( whiteRGB );

		XYZPel xyz;
		xyz.X = relX * whiteXYZ.X;
		xyz.Y = relY * whiteXYZ.Y;
		xyz.Z = relZ * whiteXYZ.Z;

		RISEPel rgb = ColorUtils::XYZtoRec709RGB( xyz );
		if( rgb.r < 0 ) rgb.r = 0;
		if( rgb.g < 0 ) rgb.g = 0;
		if( rgb.b < 0 ) rgb.b = 0;
		return rgb;
	}
}

int main()
{
	std::cout << "=== Thin-Film RGB Spectral (dispersion) Test ===\n";
	GlobalLog();	// initialize the global log

	// ---- Load the real Ti / TiO2 optical constants --------------------
	const std::string repoRoot = ResolveRepoRoot();
	ThinFilmReference::IndexedMaterial ti, tio2;
	const std::string tiBase   = repoRoot + "/colors/thinfilm/substrates/Ti";
	const std::string tio2Base = repoRoot + "/colors/thinfilm/oxides/TiO2";
	const bool okTi   = ti.LoadFromBase( tiBase );
	const bool okTiO2 = tio2.LoadFromBase( tio2Base );
	std::cout << "[setup] Ti  " << ( okTi ? "loaded" : "MISSING" )
	          << " from " << tiBase << ".{n,k}\n";
	std::cout << "[setup] TiO2 " << ( okTiO2 ? "loaded" : "MISSING" )
	          << " from " << tio2Base << ".{n,k}\n";
	Check( okTi && okTiO2, "real Ti + TiO2 optical-constant files loaded" );
	if( !okTi || !okTiO2 ) {
		std::cout << "  FATAL: cannot run dispersion regression without the data files.\n";
		std::cout << "  (set RISE_MEDIA_PATH to the repo root, or run from the repo root)\n";
		std::cout << "\n=== ThinFilmRGBSpectralTest: " << s_pass << " passed, "
		          << ( s_fail + 1 ) << " failed ===\n";
		return 1;
	}

	// Print the band the data spans + a couple of sample points so the
	// dispersion is visible in the log.
	std::cout << "[setup] Ti.n band [" << ti.N().MinNm() << ", " << ti.N().MaxNm()
	          << "] nm; n(450)=" << ti.N().EvalAtNM( 450 )
	          << " n(650)=" << ti.N().EvalAtNM( 650 )
	          << "; TiO2.n n(450)=" << tio2.N().EvalAtNM( 450 )
	          << " n(650)=" << tio2.N().EvalAtNM( 650 ) << "\n";

	// Per-λ dispersive functor: air / TiO2(film) / Ti(substrate).
	auto dispersiveStack = [&]( Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2 ) {
		n0 = Scalar( 1 ); k0 = Scalar( 0 );
		n1 = Scalar( tio2.N().EvalAtNM( double( nm ) ) );
		k1 = Scalar( tio2.K().EvalAtNM( double( nm ) ) );
		n2 = Scalar( ti.N().EvalAtNM( double( nm ) ) );
		k2 = Scalar( ti.K().EvalAtNM( double( nm ) ) );
	};

	// The representative (555 nm) constants the OLD .v[0] path used.
	const Scalar n1_555 = Scalar( tio2.N().EvalAtNM( 555.0 ) );
	const Scalar k1_555 = Scalar( tio2.K().EvalAtNM( 555.0 ) );
	const Scalar n2_555 = Scalar( ti.N().EvalAtNM( 555.0 ) );
	const Scalar k2_555 = Scalar( ti.K().EvalAtNM( 555.0 ) );

	// ------------------------------------------------------------------
	// (a) DISPERSION MATTERS: per-λ Ti/TiO2 differs measurably from the
	//     OLD constant-555nm path.  Sweep thickness + angle; require a
	//     >1% per-channel delta in at least one (cosθ, d) cell, and report
	//     the maximum.
	// ------------------------------------------------------------------
	std::cout << "\n[a] dispersion-vs-constant(555nm) delta (per-channel, linear Rec.709):\n";
	{
		const Scalar cosines[]    = { 1.0, 0.7, 0.4 };
		const Scalar thicknesses[] = { 100.0, 200.0, 300.0, 450.0, 600.0 };
		double maxRel = 0.0, maxAbs = 0.0;
		Scalar atCos = 0, atThk = 0;
		for( Scalar c : cosines ) {
			for( Scalar d : thicknesses ) {
				const RISEPel disp = ThinFilm::ReflectanceConductorRGBSpectral( c, d, dispersiveStack );
				const RISEPel cst  = ThinFilm::ReflectanceConductorRGB(
					c, 1.0, 0.0, n1_555, k1_555, d, n2_555, k2_555 );
				const double rel = MaxChanRelDiff( disp, cst );
				const double abs = MaxChanDiff( disp, cst );
				if( rel > maxRel ) { maxRel = rel; atCos = c; atThk = d; }
				if( abs > maxAbs ) maxAbs = abs;
			}
		}
		// Show the worst cell explicitly.
		const RISEPel dispW = ThinFilm::ReflectanceConductorRGBSpectral( atCos, atThk, dispersiveStack );
		const RISEPel cstW  = ThinFilm::ReflectanceConductorRGB(
			atCos, 1.0, 0.0, n1_555, k1_555, atThk, n2_555, k2_555 );
		std::cout << "    worst cell cosθ=" << atCos << " d=" << atThk << "nm:"
		          << "  dispersive=(" << std::fixed << std::setprecision( 4 )
		          << dispW.r << "," << dispW.g << "," << dispW.b << ")"
		          << "  constant555=(" << cstW.r << "," << cstW.g << "," << cstW.b << ")"
		          << "  maxRel=" << std::setprecision( 3 ) << maxRel
		          << "  maxAbs=" << maxAbs << "\n";
		Check( maxRel > 0.01,
			"(a) per-λ Ti/TiO2 RGB differs >1% from constant-555nm (dispersion matters for real data)" );
	}

	// ------------------------------------------------------------------
	// (b) ORACLE MATCH: ReflectanceConductorRGBSpectral == independent
	//     per-λ oracle, for the dispersive stack, across angles/thickness.
	// ------------------------------------------------------------------
	std::cout << "\n[b] spectral integrator vs independent per-λ oracle (dispersive):\n";
	{
		const Scalar cosines[]    = { 1.0, 0.85, 0.6, 0.3 };
		const Scalar thicknesses[] = { 50.0, 150.0, 250.0, 400.0, 550.0 };
		double maxAbs = 0.0;
		for( Scalar c : cosines ) {
			for( Scalar d : thicknesses ) {
				const RISEPel got = ThinFilm::ReflectanceConductorRGBSpectral( c, d, dispersiveStack );
				const RISEPel ora = OracleReflectanceRGB( c, d, dispersiveStack );
				maxAbs = r_max( maxAbs, MaxChanDiff( got, ora ) );
			}
		}
		std::cout << "    max abs per-channel diff vs oracle = "
		          << std::scientific << std::setprecision( 3 ) << maxAbs << "\n";
		// The oracle differs only in HOW it computes the per-λ unpolarized R
		// (ReflectanceConductor's two AiryReflectanceForPol calls vs the RGB
		// integrator's inline pair) — algebraically identical; -ffast-math
		// reassociation is the only gap.  Tight tol.
		Check( maxAbs < 1e-9,
			"(b) ReflectanceConductorRGBSpectral matches independent per-λ oracle (tight)" );
	}

	// ------------------------------------------------------------------
	// (c) BIT-IDENTITY ON UNIFORM STACK: a non-dispersive stack gives
	//     BIT-IDENTICAL old-vs-new for BOTH RGB entry points.  This is the
	//     guard that the wrapper does not perturb existing scenes/tests
	//     (ThinFilmBRDFTest Test E, the canonical scenes).
	// ------------------------------------------------------------------
	std::cout << "\n[c] uniform-stack bit-identity (constant wrapper vs spectral-with-constant-functor):\n";
	{
		// A canonical uniform heat-tint stack (matches ThinFilmBRDFTest).
		const Scalar uN1 = 2.50, uK1 = 0.00, uN2 = 2.74, uK2 = 3.79;
		auto constStack = [&]( Scalar /*nm*/, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2 ) {
			n0 = Scalar( 1 ); k0 = Scalar( 0 );
			n1 = uN1; k1 = uK1;
			n2 = uN2; k2 = uK2;
		};
		const Scalar cosines[]    = { 1.0, 0.9, 0.65, 0.35, 0.1 };
		const Scalar thicknesses[] = { 80.0, 175.0, 260.0, 380.0, 540.0 };
		bool reflectBitIdentical = true;
		bool favgBitIdentical    = true;
		size_t cells = 0;
		for( Scalar c : cosines ) {
			for( Scalar d : thicknesses ) {
				++cells;
				// Single-scatter RGB: wrapper (constant n/k) vs Spectral with
				// a constant functor — must be byte-for-byte equal.
				const RISEPel wrap = ThinFilm::ReflectanceConductorRGB(
					c, 1.0, 0.0, uN1, uK1, d, uN2, uK2 );
				const RISEPel spec = ThinFilm::ReflectanceConductorRGBSpectral( c, d, constStack );
				if( !( wrap.r == spec.r && wrap.g == spec.g && wrap.b == spec.b ) ) {
					reflectBitIdentical = false;
				}
				// Multiscatter RGB hemispherical average: same bit-identity.
				const RISEPel favgWrap = ThinFilm::FresnelAvgConductorRGB(
					1.0, 0.0, uN1, uK1, d, uN2, uK2 );
				const RISEPel favgSpec = ThinFilm::FresnelAvgConductorRGBSpectral( d, constStack );
				if( !( favgWrap.r == favgSpec.r && favgWrap.g == favgSpec.g && favgWrap.b == favgSpec.b ) ) {
					favgBitIdentical = false;
				}
			}
		}
		std::cout << "    " << cells << " (cosθ,d) cells checked for exact == \n";
		Check( reflectBitIdentical,
			"(c) ReflectanceConductorRGB constant wrapper is BIT-IDENTICAL to spectral-with-constant-functor" );
		Check( favgBitIdentical,
			"(c) FresnelAvgConductorRGB constant wrapper is BIT-IDENTICAL to spectral-with-constant-functor" );
	}

	// ------------------------------------------------------------------
	// (d) PAINTER-FUNCTOR PATH: a real IScalarPainter whose GetValueAtNM
	//     interpolates the file feeds ReflectanceConductorRGBSpectral via
	//     the SAME functor shape the BSDF sites build, and equals the
	//     direct-table dispersive result.  This exercises the production
	//     code path (GetValueAtNM per-λ) end-to-end.
	// ------------------------------------------------------------------
	std::cout << "\n[d] production IScalarPainter functor path (GetValueAtNM per-λ):\n";
	{
		TableScalarPainter* pFilmN = new TableScalarPainter( tio2.N() ); pFilmN->addref();
		TableScalarPainter* pFilmK = new TableScalarPainter( tio2.K() ); pFilmK->addref();
		TableScalarPainter* pSubN  = new TableScalarPainter( ti.N() );   pSubN->addref();
		TableScalarPainter* pSubK  = new TableScalarPainter( ti.K() );   pSubK->addref();

		// A dummy intersection — the table painters ignore `ri`.
		Ray inRay( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );

		// The exact functor pattern the GGX BSDF sites build.
		auto painterStack = [&]( Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2 ) {
			n0 = Scalar( 1 ); k0 = Scalar( 0 );
			n1 = pFilmN->GetValueAtNM( ri, nm );
			k1 = pFilmK->GetValueAtNM( ri, nm );
			n2 = pSubN->GetValueAtNM( ri, nm );
			k2 = pSubK->GetValueAtNM( ri, nm );
		};

		double maxAbs = 0.0;
		const Scalar cosines[]    = { 1.0, 0.7, 0.4 };
		const Scalar thicknesses[] = { 120.0, 240.0, 360.0 };
		for( Scalar c : cosines ) {
			for( Scalar d : thicknesses ) {
				const RISEPel viaPainter = ThinFilm::ReflectanceConductorRGBSpectral( c, d, painterStack );
				const RISEPel viaTable   = ThinFilm::ReflectanceConductorRGBSpectral( c, d, dispersiveStack );
				maxAbs = r_max( maxAbs, MaxChanDiff( viaPainter, viaTable ) );
			}
		}
		std::cout << "    max abs per-channel diff painter-functor vs direct-table = "
		          << std::scientific << std::setprecision( 3 ) << maxAbs << "\n";
		// Same interpolation (TableScalarPainter::GetValueAtNM delegates to
		// SpectralTable::EvalAtNM), so this is exact.
		Check( maxAbs == 0.0,
			"(d) IScalarPainter GetValueAtNM functor path == direct-table dispersive result (exact)" );

		// Also confirm the F_avg (multiscatter) painter path runs and differs
		// from the constant-555nm F_avg (dispersion reaches the tail too).
		const Scalar dThk = 240.0;
		const RISEPel favgDisp = ThinFilm::FresnelAvgConductorRGBSpectral( dThk, painterStack );
		const RISEPel favgCst  = ThinFilm::FresnelAvgConductorRGB(
			1.0, 0.0, n1_555, k1_555, dThk, n2_555, k2_555 );
		std::cout << "    F_avg dispersive=(" << std::fixed << std::setprecision( 4 )
		          << favgDisp.r << "," << favgDisp.g << "," << favgDisp.b << ")"
		          << "  constant555=(" << favgCst.r << "," << favgCst.g << "," << favgCst.b << ")\n";
		Check( MaxChanRelDiff( favgDisp, favgCst ) > 0.01,
			"(d) FresnelAvgConductorRGBSpectral (multiscatter tail) is dispersion-sensitive too" );

		pFilmN->release(); pFilmK->release(); pSubN->release(); pSubK->release();
	}

	std::cout << "\n=== ThinFilmRGBSpectralTest: " << s_pass << " passed, "
	          << s_fail << " failed ===\n";
	return s_fail == 0 ? 0 : 1;
}
