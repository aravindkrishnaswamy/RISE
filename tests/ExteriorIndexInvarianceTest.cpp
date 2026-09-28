//////////////////////////////////////////////////////////////////////
//
//  ExteriorIndexInvarianceTest.cpp - Regression guard for DL-290:
//    boundary models outside SSS price an immersed interface against
//    the live exterior (`RayIntersectionGeometric::ambientIOR`), not
//    against air.
//
//  THE DEFECT (same pattern as DL-49, outside SSS)
//
//    "An absolute index against 1 where a relative index is meant":
//      - Cook-Torrance conductor Fresnel and its Kulla-Conty F_avg passed
//        incident index 1 / RISEPel(1,1,1) (GGX has read ambientIOR since
//        G6; Cook-Torrance never did);
//      - the Chiang hair BCSDF read `ior` as the fibre-vs-surrounding
//        ratio the model is written in, but was handed the ABSOLUTE
//        painter value;
//      - the weave fibre lobes (FrDielectric of the fibre index) did the
//        same;
//      - BioSpec skin's outside / stratum-corneum boundary passed a literal
//        1.0 for the outside index, on entry and on exit;
//      - the four SMS evaluation rigs in ManifoldSolver.cpp built their
//        receiver record without stamping `ambientIOR`, although the IOR
//        stack was in hand, so a G6 receiver (GGX conductor) priced air.
//
//  THE INVARIANT (reference-free, DL-49's)
//
//    Scale every index by one factor s: exterior 1 -> s, every material
//    index n -> s*n (a conductor's k too).  Every Snell direction and
//    every Fresnel term of a RELATIVE index is unchanged, so every BSDF
//    value, every sampled direction and weight and every rendered pixel
//    must be unchanged.
//
//  WHAT EACH PART PROVES
//
//    Part A -- deterministic, function level:
//      A1  cooktorrance_material: value / valueNM / albedo scale
//          invariant; matched index (n == exterior, k = 0) reflects
//          exactly nothing.
//      A2  cooktorrance SPF, seeded twins: identical directions, krays
//          and pdfs (RGB and NM); matched index emits zero throughput.
//      A3  hair_material: value / valueNM / albedo scale invariant;
//          seeded SPF twins identical; an opaque fibre whose index matches
//          its surroundings has no R-lobe highlight.
//      A4  weave_material: value / valueNM scale invariant; seeded SPF
//          twins identical.
//      A5  biospec_skin_material: seeded SPF twins (RGB and NM) agree,
//          including a twin whose outside interface is index MATCHED.
//      A6  ManifoldSolver::ComputeTrialContribution{,NM}: the receiver
//          record carries the stack top as `ambientIOR` (and air with no
//          stack), and the NM twin hands the stack to the BSDF as the RGB
//          twin does.
//    Part B -- rendered, end to end (DL-49's black-room harness):
//      subject, spherical luminaire and pinhole camera inside a black
//      absorbing room, in air versus inside an ideal non-reflecting
//      enclosure of index 1.5 with every material index scaled by 1.5.
//      Rows: Lambertian control, Cook-Torrance (PT, BDPT, PT spectral),
//      hair (PT), weave (PT), BioSpec skin (PT), and four SMS rows (a GGX
//      conductor floor lit ONLY through a mirror by a shaded point light,
//      snell and uniform seeding, RGB and spectral).  The enclosed/air
//      image-mean ratio must be 1.
//    Usage: [--unit-only] [--trials K (default 4)] [--only <label substring>]
//
//  Author: RISE debt-cleanup, slice `debt-dl290`
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	int passCount = 0;
	int failCount = 0;

	void Check( const bool condition, const std::string& label )
	{
		if( condition ) {
			++passCount;
		} else {
			++failCount;
			std::cout << "  FAILED: " << label << std::endl;
		}
	}

	const Scalar kScale = 1.5;

	bool RelClose( const Scalar a, const Scalar b, const Scalar tol )
	{
		if( !std::isfinite( a ) || !std::isfinite( b ) ) return false;
		const Scalar m = std::fmax( std::fabs( a ), std::fabs( b ) );
		return std::fabs( a - b ) <= tol * m || std::fabs( a - b ) <= Scalar( 1e-300 );
	}
	bool RelClosePel( const RISEPel& a, const RISEPel& b, const Scalar tol )
	{
		return RelClose( a[0], b[0], tol ) && RelClose( a[1], b[1], tol ) && RelClose( a[2], b[2], tol );
	}
	Scalar RelDiff( const Scalar a, const Scalar b )
	{
		const Scalar m = std::fmax( std::fabs( a ), std::fabs( b ) );
		return m > 0 ? std::fabs( a - b ) / m : 0;
	}

	class TestSampler : public ISampler
	{
		RandomNumberGenerator rng;
	public:
		explicit TestSampler( const unsigned int seed ) : rng( seed ) {}
		Scalar Get1D() { return rng.CanonicalRandom(); }
		Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
	};

	//! A hit at the origin with shading/geometric normal +Z, a tangent
	//! frame (u = +X), a mid-surface UV (hair reads h = 2v - 1 from it), and
	//! the viewer ray arriving from `viewFrom` (a unit vector on the +Z side).
	RayIntersectionGeometric MakeRI( const Scalar exteriorIOR, const Vector3& viewFrom )
	{
		const Vector3 v = Vector3Ops::Normalize( viewFrom );
		RayIntersectionGeometric ri( Ray( Point3( v.x * 2, v.y * 2, v.z * 2 ), -v ), nullRasterizerState );
		ri.bHit = true;
		ri.range = 2.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromWU( Vector3( 0, 0, 1 ), Vector3( 1, 0, 0 ) );
		ri.ptCoord = Point2( 0.37, 0.64 );
		ri.ptCoord1 = ri.ptCoord;
		ri.ambientIOR = exteriorIOR;
		return ri;
	}

	const Vector3 kViews[] = {
		Vector3( 0.0, 0.0, 1.0 ), Vector3( 0.5, 0.2, 0.84 ), Vector3( -0.8, 0.3, 0.52 ), Vector3( 0.9, -0.1, 0.2 ) };
	const Vector3 kLights[] = {
		Vector3( 0.1, 0.0, 0.99 ), Vector3( -0.6, 0.2, 0.77 ), Vector3( 0.7, 0.5, 0.5 ), Vector3( -0.3, -0.9, 0.3 ),
		Vector3( 0.2, 0.3, -0.93 ) };	// the last is BELOW the surface: hair and weave transmit

	//////////////////////////////////////////////////////////////////
	// Materials, loaded through the real scene language
	//////////////////////////////////////////////////////////////////
	std::string Num( const Scalar v )
	{
		std::ostringstream s;
		s << std::setprecision( 17 ) << v;
		return s.str();
	}

	//! Cook-Torrance conductor: n = 1.5, k = 0.5 in air (a contrast chosen
	//! so the exterior matters: F0 is 0.077 against 1.0 but 0.191 when the
	//! scaled twin is priced against air).
	std::string CookTorranceChunk( const std::string& name, const Scalar s, const Scalar n, const Scalar k, const bool blackDiffuse )
	{
		return "cooktorrance_material\n{\n\tname " + name + "\n\trd " + ( blackDiffuse ? "black" : "grey" ) +
			"\n\trs white\n\tfacets 0.25\n\tior " + Num( n * s ) + "\n\textinction " + Num( k * s ) + "\n}\n\n";
	}
	std::string HairChunk( const std::string& name, const Scalar s, const Scalar sigmaA )
	{
		return "hair_material\n{\n\tname " + name + "\n\tsigma_a " + Num( sigmaA ) +
			"\n\tbeta_m 0.3\n\tbeta_n 0.3\n\talpha 2\n\tior " + Num( 1.55 * s ) + "\n}\n\n";
	}
	std::string WeaveChunk( const std::string& name, const Scalar s )
	{
		return "weave_material\n{\n\tname " + name + "\n\twarp_color reddish\n\tweft_color bluish\n\twarp_ior " +
			Num( 1.46 * s ) + "\n\tweft_ior " + Num( 1.60 * s ) + "\n}\n\n";
	}
	//! BioSpec skin.  Every layer index scales with the exterior.  The dermis
	//! index is 2.0 (not tissue-like) and the papillary layer 0.2 cm thick for
	//! one reason: the dermis Rayleigh term prices collagen (a hardcoded
	//! absolute 1.5) against the dermis index, an INTRA-tissue constant a
	//! uniform scaling cannot carry.  With these values the Rayleigh
	//! probability saturates to 1 in both twins at every visible wavelength
	//! (optical depth >= 24), so the only index-dependent physics left is
	//! what the invariant tests.
	std::string SkinChunk( const std::string& name, const Scalar s, const Scalar iorSC )
	{
		return "biospec_skin_material\n{\n\tname " + name + "\n\tior_SC " + Num( iorSC * s ) + "\n\tior_epidermis " +
			Num( 1.4 * s ) + "\n\tior_papillary_dermis " + Num( 2.0 * s ) + "\n\tior_reticular_dermis " + Num( 2.0 * s ) +
			"\n\tthickness_papillary_dermis 0.2\n\tmelanosomes_in_epidermis 0.019\n}\n\n";
	}

	std::string PainterPreamble()
	{
		return "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n"
			"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.3 0.25 0.2\n}\n\n"
			"uniformcolor_painter\n{\n\tname reddish\n\tcolor 0.7 0.3 0.2\n}\n\n"
			"uniformcolor_painter\n{\n\tname bluish\n\tcolor 0.2 0.3 0.7\n}\n\n";
	}

	std::string WriteScene( const std::string& text, const std::string& tag )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/tmp/dl290_invariance_%s_%d.RISEscene", tag.c_str(), static_cast<int>( ::getpid() ) );
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return std::string();
		ofs << text;
		ofs.close();
		return std::string( path );
	}

	struct MaterialJob
	{
		IJobPriv* job;
		std::string path;
		MaterialJob() : job( nullptr ) {}
		~MaterialJob() { safe_release( job ); if( !path.empty() ) std::remove( path.c_str() ); }
		bool Load( const std::string& materials )
		{
			path = WriteScene( "RISE ASCII SCENE 7\n" + PainterPreamble() + materials, "materials" );
			if( path.empty() ) return false;
			if( !RISE_CreateJobPriv( &job ) || !job ) return false;
			return job->LoadAsciiSceneViaCst( path.c_str() );
		}
		const IMaterial* Get( const char* name ) const
		{
			return job && job->GetMaterials() ? job->GetMaterials()->GetItem( name ) : nullptr;
		}
	};

	//! value / valueNM / albedo of `a` at exterior 1 against `b` at exterior s.
	//! Returns the worst relative difference; checks each within `tol`.
	Scalar CompareBSDF( const IBSDF& a, const IBSDF& b, const Scalar extA, const Scalar extB,
		const Scalar tol, const std::string& label, const bool withAlbedo )
	{
		Scalar worst = 0;
		bool allOk = true, anyNonZero = false;
		for( const Vector3& view : kViews ) {
			const RayIntersectionGeometric riA = MakeRI( extA, view );
			const RayIntersectionGeometric riB = MakeRI( extB, view );
			for( const Vector3& l : kLights ) {
				const Vector3 light = Vector3Ops::Normalize( l );
				const RISEPel fa = a.value( light, riA ), fb = b.value( light, riB );
				for( const Scalar nm : { 450.0, 550.0, 650.0 } ) {
					const Scalar ga = a.valueNM( light, riA, nm ), gb = b.valueNM( light, riB, nm );
					worst = std::fmax( worst, RelDiff( ga, gb ) );
					if( !RelClose( ga, gb, tol ) ) allOk = false;
					if( ga > 0 ) anyNonZero = true;
				}
				for( int c = 0; c < 3; ++c ) worst = std::fmax( worst, RelDiff( fa[c], fb[c] ) );
				if( !RelClosePel( fa, fb, tol ) ) allOk = false;
				if( ColorMath::MaxValue( fa ) > 0 ) anyNonZero = true;
			}
			if( withAlbedo ) {
				const RISEPel aa = a.albedo( riA ), ab = b.albedo( riB );
				for( int c = 0; c < 3; ++c ) worst = std::fmax( worst, RelDiff( aa[c], ab[c] ) );
				if( !RelClosePel( aa, ab, tol ) ) allOk = false;
			}
		}
		std::cout << "    " << label << ": worst relative difference " << std::setprecision( 6 ) << worst << std::endl;
		Check( anyNonZero, label + ": (sanity) the configuration reflects" );
		Check( allOk, label + ": value/valueNM" + ( withAlbedo ? "/albedo" : "" ) + " scale invariant" );
		return worst;
	}

	//! Seeded SPF twins: the same random stream through `a` at exterior 1 and
	//! `b` at exterior s.  Every emitted ray must agree in count, direction,
	//! kray / krayNM and pdf.  Returns the fraction of calls that agree.
	Scalar CompareSPF( const ISPF& a, const ISPF& b, const Scalar extA, const Scalar extB,
		const bool bNM, const unsigned int calls, const Scalar tol, const std::string& label )
	{
		unsigned int agree = 0, emitted = 0;
		for( unsigned int i = 0; i < calls; ++i ) {
			const Vector3& view = kViews[i % 4];
			const RayIntersectionGeometric riA = MakeRI( extA, view );
			const RayIntersectionGeometric riB = MakeRI( extB, view );
			const IORStack stackA( extA ), stackB( extB );
			TestSampler sa( 290000 + i ), sb( 290000 + i );
			ScatteredRayContainer ca, cb;
			const Scalar nm = 400.0 + 300.0 * ( ( i * 7919 ) % 1000 ) / 1000.0;
			if( bNM ) {
				a.ScatterNM( riA, sa, nm, ca, stackA );
				b.ScatterNM( riB, sb, nm, cb, stackB );
			} else {
				a.Scatter( riA, sa, ca, stackA );
				b.Scatter( riB, sb, cb, stackB );
			}
			bool same = ca.Count() == cb.Count();
			for( unsigned int r = 0; same && r < ca.Count(); ++r ) {
				const ScatteredRay& x = ca[r];
				const ScatteredRay& y = cb[r];
				const Vector3 dx = x.ray.Dir(), dy = y.ray.Dir();
				same = std::fabs( dx.x - dy.x ) < 1e-9 && std::fabs( dx.y - dy.y ) < 1e-9 && std::fabs( dx.z - dy.z ) < 1e-9
					&& ( bNM ? RelClose( x.krayNM, y.krayNM, tol ) : RelClosePel( x.kray, y.kray, tol ) )
					&& RelClose( x.pdf, y.pdf, tol );
			}
			if( ca.Count() > 0 ) ++emitted;
			if( same ) ++agree;
		}
		const Scalar frac = Scalar( agree ) / Scalar( calls );
		std::cout << "    " << label << ": " << agree << "/" << calls << " seeded twins agree (" << emitted << " emitted)" << std::endl;
		Check( emitted > calls / 10, label + ": (sanity) the SPF emits" );
		return frac;
	}

	//////////////////////////////////////////////////////////////////
	// A1/A2 -- Cook-Torrance
	//////////////////////////////////////////////////////////////////
	void TestCookTorrance( const MaterialJob& mj )
	{
		std::cout << "A1/A2: cooktorrance_material conductor Fresnel is a relative-index interface" << std::endl;
		const IMaterial* air = mj.Get( "ct_air" );
		const IMaterial* scaled = mj.Get( "ct_scaled" );
		const IMaterial* matched = mj.Get( "ct_matched" );
		Check( air && scaled && matched && air->GetBSDF() && air->GetSPF(), "A1: Cook-Torrance materials load" );
		if( !( air && scaled && matched && air->GetBSDF() && air->GetSPF() ) ) return;
		CompareBSDF( *air->GetBSDF(), *scaled->GetBSDF(), 1.0, kScale, 1e-9, "A1 cooktorrance BSDF", true );

		// Matched index (n == exterior, k = 0, black diffuse): no interface.
		// The conductor Fresnel then reads (cos - a)^2-type cancellations,
		// so "nothing" is rounding-level, not a literal 0.  The NM value
		// also carries the black diffuse painter's Jakob-Hanika uplift
		// residual (~2.5e-5 / pi); it is removed by differencing against a
		// twin whose SPECULAR colour is black too (same diffuse residual,
		// no specular term at any index).
		const IMaterial* matchedNoSpec = mj.Get( "ct_matched_nospec" );
		Check( matchedNoSpec && matchedNoSpec->GetBSDF(), "A1: matched no-specular twin loads" );
		if( !( matchedNoSpec && matchedNoSpec->GetBSDF() ) ) return;
		Scalar maxMatched = 0, maxMatchedNM = 0, maxAirControl = 0;
		for( const Vector3& view : kViews ) {
			const RayIntersectionGeometric riM = MakeRI( 1.5, view );
			const RayIntersectionGeometric riAir = MakeRI( 1.0, view );
			for( const Vector3& l : kLights ) {
				const Vector3 light = Vector3Ops::Normalize( l );
				maxMatched = std::fmax( maxMatched, ColorMath::MaxValue( matched->GetBSDF()->value( light, riM ) ) );
				maxMatchedNM = std::fmax( maxMatchedNM, std::fabs( matched->GetBSDF()->valueNM( light, riM, 550.0 ) -
					matchedNoSpec->GetBSDF()->valueNM( light, riM, 550.0 ) ) );
				maxAirControl = std::fmax( maxAirControl, ColorMath::MaxValue( matched->GetBSDF()->value( light, riAir ) ) );
			}
		}
		std::cout << "    matched index (1.5 in 1.5, k=0): max value RGB " << maxMatched << "  NM specular " << maxMatchedNM
			<< "  (same material in air: " << maxAirControl << ")" << std::endl;
		Check( maxAirControl > 0.05, "A1: (sanity) the matched material reflects in air" );
		Check( maxMatched <= 1e-12 * maxAirControl, "A1: matched index reflects nothing (value, rounding level)" );
		Check( maxMatchedNM <= 1e-12 * maxAirControl, "A1: matched index reflects nothing (valueNM specular, rounding level)" );

		const Scalar rgb = CompareSPF( *air->GetSPF(), *scaled->GetSPF(), 1.0, kScale, false, 2048, 1e-9, "A2 cooktorrance SPF RGB" );
		const Scalar nm = CompareSPF( *air->GetSPF(), *scaled->GetSPF(), 1.0, kScale, true, 2048, 1e-9, "A2 cooktorrance SPF NM" );
		Check( rgb == 1.0, "A2: cooktorrance SPF RGB seeded twins identical" );
		Check( nm == 1.0, "A2: cooktorrance SPF NM seeded twins identical" );

		// Matched index: every emitted ray carries zero throughput.
		Scalar maxKray = 0;
		unsigned int emitted = 0;
		for( unsigned int i = 0; i < 1024; ++i ) {
			const RayIntersectionGeometric ri = MakeRI( 1.5, kViews[i % 4] );
			const IORStack stack( 1.5 );
			TestSampler s( 7000 + i ), s2( 9000 + i );
			ScatteredRayContainer c, c2;
			matched->GetSPF()->Scatter( ri, s, c, stack );
			matched->GetSPF()->ScatterNM( ri, s2, 550.0, c2, stack );
			for( unsigned int r = 0; r < c.Count(); ++r ) { maxKray = std::fmax( maxKray, ColorMath::MaxValue( c[r].kray ) ); ++emitted; }
			for( unsigned int r = 0; r < c2.Count(); ++r ) { maxKray = std::fmax( maxKray, c2[r].krayNM ); ++emitted; }
		}
		std::cout << "    matched index SPF: " << emitted << " rays, max kray " << maxKray << std::endl;
		Check( emitted > 100 && maxKray <= 1e-12, "A2: matched index SPF emits zero throughput (RGB and NM, rounding level)" );

		// The aggregate density does not read the Fresnel (lobe selection is
		// by specular colour): a consistency pin that the Pdf replay stays
		// equal between twins.
		bool pdfOk = true;
		for( const Vector3& view : kViews ) {
			const RayIntersectionGeometric riA = MakeRI( 1.0, view ), riB = MakeRI( kScale, view );
			const IORStack sA( 1.0 ), sB( kScale );
			for( const Vector3& l : kLights ) {
				const Vector3 d = Vector3Ops::Normalize( l );
				if( !RelClose( air->GetSPF()->Pdf( riA, d, sA ), scaled->GetSPF()->Pdf( riB, d, sB ), 1e-12 ) ) pdfOk = false;
				if( !RelClose( air->GetSPF()->PdfNM( riA, d, 550.0, sA ), scaled->GetSPF()->PdfNM( riB, d, 550.0, sB ), 1e-12 ) ) pdfOk = false;
			}
		}
		Check( pdfOk, "A2: cooktorrance Pdf/PdfNM twins equal (consistency pin)" );
	}

	//////////////////////////////////////////////////////////////////
	// A3 -- hair
	//////////////////////////////////////////////////////////////////
	void TestHair( const MaterialJob& mj )
	{
		std::cout << "A3: hair_material eta is the fibre-vs-surrounding ratio" << std::endl;
		const IMaterial* air = mj.Get( "hair_air" );
		const IMaterial* scaled = mj.Get( "hair_scaled" );
		const IMaterial* opaque = mj.Get( "hair_opaque_matched" );
		Check( air && scaled && opaque && air->GetBSDF() && air->GetSPF(), "A3: hair materials load" );
		if( !( air && scaled && opaque && air->GetBSDF() && air->GetSPF() ) ) return;
		CompareBSDF( *air->GetBSDF(), *scaled->GetBSDF(), 1.0, kScale, 1e-9, "A3 hair BSDF", true );
		const Scalar rgb = CompareSPF( *air->GetSPF(), *scaled->GetSPF(), 1.0, kScale, false, 2048, 1e-9, "A3 hair SPF RGB" );
		const Scalar nm = CompareSPF( *air->GetSPF(), *scaled->GetSPF(), 1.0, kScale, true, 2048, 1e-9, "A3 hair SPF NM" );
		Check( rgb == 1.0, "A3: hair SPF RGB seeded twins identical" );
		Check( nm == 1.0, "A3: hair SPF NM seeded twins identical" );

		// An opaque fibre (sigma_a huge: every transmissive order dies) whose
		// index MATCHES its surroundings (1.55 inside 1.55) has no surface
		// reflection left: the R lobe is Fresnel-weighted and the relative
		// index is 1.  The same fibre in air keeps its white highlight.
		Scalar maxMatched = 0, maxAir = 0;
		for( const Vector3& view : kViews ) {
			const RayIntersectionGeometric riM = MakeRI( 1.55, view ), riA = MakeRI( 1.0, view );
			for( const Vector3& l : kLights ) {
				const Vector3 light = Vector3Ops::Normalize( l );
				maxMatched = std::fmax( maxMatched, ColorMath::MaxValue( opaque->GetBSDF()->value( light, riM ) ) );
				maxAir = std::fmax( maxAir, ColorMath::MaxValue( opaque->GetBSDF()->value( light, riA ) ) );
			}
		}
		std::cout << "    opaque fibre, matched index: max value " << maxMatched << "  (same fibre in air: " << maxAir << ")" << std::endl;
		Check( maxAir > 1e-3, "A3: (sanity) the opaque fibre has a highlight in air" );
		Check( maxMatched < 1e-9 * maxAir, "A3: matched-index opaque fibre has no R-lobe highlight" );
	}

	//////////////////////////////////////////////////////////////////
	// A4 -- weave
	//////////////////////////////////////////////////////////////////
	void TestWeave( const MaterialJob& mj )
	{
		std::cout << "A4: weave_material fibre Fresnel is a relative-index interface" << std::endl;
		const IMaterial* air = mj.Get( "weave_air" );
		const IMaterial* scaled = mj.Get( "weave_scaled" );
		Check( air && scaled && air->GetBSDF() && air->GetSPF(), "A4: weave materials load" );
		if( !( air && scaled && air->GetBSDF() && air->GetSPF() ) ) return;
		CompareBSDF( *air->GetBSDF(), *scaled->GetBSDF(), 1.0, kScale, 1e-9, "A4 weave BSDF", false );
		const Scalar rgb = CompareSPF( *air->GetSPF(), *scaled->GetSPF(), 1.0, kScale, false, 2048, 1e-9, "A4 weave SPF RGB" );
		const Scalar nm = CompareSPF( *air->GetSPF(), *scaled->GetSPF(), 1.0, kScale, true, 2048, 1e-9, "A4 weave SPF NM" );
		Check( rgb == 1.0, "A4: weave SPF RGB seeded twins identical" );
		Check( nm == 1.0, "A4: weave SPF NM seeded twins identical" );
	}

	//////////////////////////////////////////////////////////////////
	// A5 -- BioSpec skin
	//////////////////////////////////////////////////////////////////
	void TestSkin( const MaterialJob& mj )
	{
		std::cout << "A5: biospec_skin_material outside boundary is against the live exterior" << std::endl;
		struct Pair { const char* air; const char* scaled; Scalar ext; const char* tag; };
		// The second pair's outer interface is index MATCHED: stratum
		// corneum 1.0 in air, twinned with 1.5 inside 1.5.
		const Pair pairs[] = { { "skin_air", "skin_scaled", kScale, "generic" }, { "skin_air_matched", "skin_scaled_matched", kScale, "matched outer interface" } };
		for( const Pair& p : pairs ) {
			const IMaterial* a = mj.Get( p.air );
			const IMaterial* b = mj.Get( p.scaled );
			Check( a && b && a->GetSPF() && b->GetSPF(), std::string( "A5: skin materials load (" ) + p.tag + ")" );
			if( !( a && b && a->GetSPF() && b->GetSPF() ) ) continue;
			// Seeded twins agree except where a sampler draw lands inside a
			// last-bit difference of two Fresnel/Snell evaluations (a 1.5x
			// scaled index is not the same float operation), after which the
			// two random walks consume the stream differently: >= 99.5 %.
			const Scalar rgb = CompareSPF( *a->GetSPF(), *b->GetSPF(), 1.0, p.ext, false, 8192, 1e-9, std::string( "A5 skin SPF RGB, " ) + p.tag );
			const Scalar nm = CompareSPF( *a->GetSPF(), *b->GetSPF(), 1.0, p.ext, true, 8192, 1e-9, std::string( "A5 skin SPF NM, " ) + p.tag );
			Check( rgb >= 0.995, std::string( "A5: skin SPF RGB seeded twins agree (" ) + p.tag + ")" );
			Check( nm >= 0.995, std::string( "A5: skin SPF NM seeded twins agree (" ) + p.tag + ")" );
		}
	}

	//////////////////////////////////////////////////////////////////
	// A6 -- SMS evaluation rigs
	//////////////////////////////////////////////////////////////////
	class RecordingBSDF : public virtual IBSDF, public virtual Reference
	{
	public:
		mutable Scalar lastAmbient = -1;
		mutable const IORStack* lastStack = nullptr;
		mutable int statefulCalls = 0;
		RecordingBSDF() {}
		RISEPel value( const Vector3&, const RayIntersectionGeometric& ri ) const override { lastAmbient = ri.ambientIOR; return RISEPel( 1, 1, 1 ); }
		Scalar valueNM( const Vector3&, const RayIntersectionGeometric& ri, const Scalar ) const override { lastAmbient = ri.ambientIOR; return 1.0; }
		RISEPel valueStateful( const Vector3& v, const RayIntersectionGeometric& ri, const IORStack* p ) const override
		{ ++statefulCalls; lastStack = p; return value( v, ri ); }
		Scalar valueStatefulNM( const Vector3& v, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* p ) const override
		{ ++statefulCalls; lastStack = p; return valueNM( v, ri, nm ); }
		RISEPel albedo( const RayIntersectionGeometric& ) const override { return RISEPel( 1, 1, 1 ); }
	protected:
		virtual ~RecordingBSDF() {}
	};

	void TestSMSRigs()
	{
		std::cout << "A6: SMS evaluation rigs stamp the receiver's exterior index" << std::endl;
		// An empty scene: the chain's external segments are then trivially
		// unoccluded, and the receiver BSDF is reached.
		const std::string path = WriteScene( "RISE ASCII SCENE 7\n" + PainterPreamble(), "sms_empty" );
		IJobPriv* job = nullptr;
		const bool loaded = !path.empty() && RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "A6: empty scene loads" );
		if( !loaded ) { safe_release( job ); return; }
		StandardShader* shader = new StandardShader( std::vector<IShaderOp*>() );
		RayCaster* caster = new RayCaster( false, 8, *shader, false );
		caster->AttachScene( job->GetScene() );
		ManifoldSolver* solver = new ManifoldSolver( ManifoldSolverConfig() );
		RecordingBSDF* bsdf = new RecordingBSDF();

		ManifoldResult mr;
		mr.valid = true;
		ManifoldVertex mv;
		mv.position = Point3( 0.3, 1.0, 0.0 );
		mv.normal = Vector3( 0, -1, 0 );
		mv.geomNormal = Vector3( 0, -1, 0 );
		mr.specularChain.push_back( mv );
		LightSample ls;
		ls.position = Point3( 0.6, 0.2, 0.0 );
		ls.normal = Vector3( 0, 1, 0 );
		ls.Le = RISEPel( 1, 1, 1 );
		ls.isDelta = true;
		ls.pLight = nullptr;

		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 1, 0 ) );
		const Point3 pos( 0, 0, 0 );
		const Vector3 wo = Vector3Ops::Normalize( Vector3( -0.3, 1.0, 0.1 ) );
		const IORStack water( 1.33 );
		const IORStack glassy( 1.5 );

		struct Case { const IORStack* stack; Scalar expected; const char* tag; };
		const Case cases[] = { { &water, 1.33, "stack top 1.33" }, { &glassy, 1.5, "stack top 1.5" }, { nullptr, 1.0, "no stack (air)" } };
		for( const Case& c : cases ) {
			Vector3 dir;
			RISEPel contrib;
			bsdf->lastAmbient = -1; bsdf->lastStack = nullptr; bsdf->statefulCalls = 0;
			solver->ComputeTrialContribution( pos, Vector3( 0, 1, 0 ), Vector3( 0, 1, 0 ), onb, wo, bsdf, ls, mr, *caster,
				dir, contrib, true, nullptr, c.stack );
			std::cout << "    RGB " << c.tag << ": receiver ambientIOR " << bsdf->lastAmbient << std::endl;
			Check( bsdf->lastAmbient == c.expected, std::string( "A6: ComputeTrialContribution receiver ambientIOR, " ) + c.tag );
			Check( bsdf->statefulCalls == 1 && bsdf->lastStack == c.stack, std::string( "A6: ComputeTrialContribution hands the stack to the BSDF, " ) + c.tag );

			Scalar contribNM = 0;
			bsdf->lastAmbient = -1; bsdf->lastStack = nullptr; bsdf->statefulCalls = 0;
			solver->ComputeTrialContributionNM( pos, Vector3( 0, 1, 0 ), Vector3( 0, 1, 0 ), onb, wo, bsdf, ls, mr, *caster,
				550.0, dir, contribNM, true, nullptr, c.stack );
			std::cout << "    NM  " << c.tag << ": receiver ambientIOR " << bsdf->lastAmbient << " (stateful calls " << bsdf->statefulCalls << ")" << std::endl;
			Check( bsdf->lastAmbient == c.expected, std::string( "A6: ComputeTrialContributionNM receiver ambientIOR, " ) + c.tag );
			Check( bsdf->statefulCalls == 1 && bsdf->lastStack == c.stack, std::string( "A6: ComputeTrialContributionNM hands the stack to the BSDF, " ) + c.tag );
		}
		safe_release( bsdf );
		safe_release( solver );
		safe_release( caster );
		safe_release( shader );
		safe_release( job );
		std::remove( path.c_str() );
	}

	//////////////////////////////////////////////////////////////////
	// Part B -- rendered scale invariance
	//////////////////////////////////////////////////////////////////
	class CapturingRasterizerOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
		CapturingRasterizerOutput() {}
	protected:
		virtual ~CapturingRasterizerOutput() {}
	public:
		virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
		virtual void OutputImage( const IRasterImage& image, const Rect*, const unsigned int ) override
		{
			pixels.resize( size_t( image.GetWidth() ) * image.GetHeight() );
			for( unsigned int y = 0; y < image.GetHeight(); y++ )
				for( unsigned int x = 0; x < image.GetWidth(); x++ )
					pixels[size_t( y ) * image.GetWidth() + x] = image.GetPEL( x, y );
		}
	};

	enum class Model { Lambertian, CookTorrance, Hair, Weave, Skin, SMSMirror, SMSGlass };
	enum class Integrator { PT, BDPT, PTSpectral };

	const char* ModelName( Model m )
	{
		switch( m ) {
		case Model::Lambertian: return "lambertian_control";
		case Model::CookTorrance: return "cooktorrance_conductor";
		case Model::Hair: return "hair";
		case Model::Weave: return "weave";
		case Model::Skin: return "biospec_skin";
		case Model::SMSMirror: return "sms_ggx_conductor_via_mirror";
		case Model::SMSGlass: return "sms_lambertian_via_glass_sphere";
		}
		return "unknown";
	}
	const char* IntegratorName( Integrator i )
	{
		return i == Integrator::PT ? "PT" : ( i == Integrator::BDPT ? "BDPT" : "PT-spectral" );
	}

	//! exterior == 1 builds the air scene; exterior > 1 wraps everything in an
	//! ideal enclosure of that index and scales every material index by it.
	std::string BuildScene( Model model, Integrator integrator, Scalar exterior, unsigned int samples, const char* smsSeeding )
	{
		const Scalar s = exterior;
		std::ostringstream o;
		o << std::setprecision( 17 );
		o << "RISE ASCII SCENE 7\n";
		o << "film\n{\n\twidth 32\n\theight 32\n}\n\n";
		if( model == Model::SMSMirror || model == Model::SMSGlass ) {
			o << "pinhole_camera\n{\n\tlocation 0 2.2 3.4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n";
		} else {
			o << "pinhole_camera\n{\n\tlocation 0 0 4.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n";
		}
		o << PainterPreamble();
		o << "lambertian_material\n{\n\tname black_base\n\treflectance black\n}\n\n";
		switch( model ) {
		case Model::Lambertian:
			o << "lambertian_material\n{\n\tname subject\n\treflectance grey\n}\n\n";
			break;
		case Model::CookTorrance:
			o << CookTorranceChunk( "subject", s, 1.5, 0.5, false );
			break;
		case Model::Hair:
			o << HairChunk( "subject", s, 0.4 );
			break;
		case Model::Weave:
			o << WeaveChunk( "subject", s );
			break;
		case Model::Skin:
			o << SkinChunk( "subject", s, 1.55 );
			break;
		case Model::SMSMirror:
			// A GGX conductor floor (n = 1.5, k = 0.5 in air; scaled with the
			// exterior) lit ONLY through a mirror: the point light sits in an
			// open-topped black box, so its only way to the floor is up to
			// the mirror and back down -- a light-mirror-floor chain only SMS
			// can connect (a delta light seen through a delta mirror).
			o << "ggx_material\n{\n\tname subject\n\trd black\n\trs white\n\talphax 0.3\n\talphay 0.3\n\tior " << 1.5 * s
			  << "\n\textinction " << 0.5 * s << "\n}\n\n";
			o << "perfectreflector_material\n{\n\tname mirror_mat\n\treflectance white\n}\n\n";
			break;
		case Model::SMSGlass:
			// NOT GATED -- the recorded DL-290 residual.  A Lambertian floor
			// (no index anywhere at the receiver) under a glass sphere
			// caster: the only index-dependent quantities are the SMS
			// chain's own etaI/etaT, which the seed walk
			// (BuildSeedChain / SnellContinueChain) still starts at air.
			o << "lambertian_material\n{\n\tname subject\n\treflectance grey\n}\n\n";
			o << "perfectrefractor_material\n{\n\tname glass_mat\n\tior " << 1.5 * s << "\n\trefractance white\n}\n\n";
			break;
		}
		if( model == Model::SMSGlass ) {
			o << "clippedplane_geometry\n{\n\tname floor_geo\n\tpta -3 0 -3\n\tptb -3 0 3\n\tptc 3 0 3\n\tptd 3 0 -3\n}\n\n";
			o << "standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial subject\n}\n\n";
			o << "sphere_geometry\n{\n\tname ball_geo\n\tradius 0.4\n}\n\n";
			o << "standard_object\n{\n\tname ball\n\tgeometry ball_geo\n\tmaterial glass_mat\n\tposition 0 0.6 0\n}\n\n";
			o << "omni_light\n{\n\tname point\n\tpower 40\n\tcolor 1 1 1\n\tposition 0 1.5 0\n}\n\n";
		} else if( model == Model::SMSMirror ) {
			o << "clippedplane_geometry\n{\n\tname floor_geo\n\tpta -3 0 -3\n\tptb -3 0 3\n\tptc 3 0 3\n\tptd 3 0 -3\n}\n\n";
			o << "standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial subject\n}\n\n";
			o << "clippedplane_geometry\n{\n\tname mirror_geo\n\tpta -1.2 2.5 -1.2\n\tptb 1.2 2.5 -1.2\n\tptc 1.2 2.5 1.2\n\tptd -1.2 2.5 1.2\n}\n\n";
			o << "standard_object\n{\n\tname mirror_obj\n\tgeometry mirror_geo\n\tmaterial mirror_mat\n}\n\n";
			// The shade: an open-topped black box around the light.
			o << "box_geometry\n{\n\tname shade_wall_geo\n\twidth 0.05\n\theight 0.5\n\tdepth 0.5\n}\n\n";
			o << "box_geometry\n{\n\tname shade_wall2_geo\n\twidth 0.5\n\theight 0.5\n\tdepth 0.05\n}\n\n";
			o << "box_geometry\n{\n\tname shade_floor_geo\n\twidth 0.5\n\theight 0.05\n\tdepth 0.5\n}\n\n";
			o << "standard_object\n{\n\tname shade_a\n\tgeometry shade_wall_geo\n\tmaterial black_base\n\tposition 0.25 1.2 0\n}\n\n";
			o << "standard_object\n{\n\tname shade_b\n\tgeometry shade_wall_geo\n\tmaterial black_base\n\tposition -0.25 1.2 0\n}\n\n";
			o << "standard_object\n{\n\tname shade_c\n\tgeometry shade_wall2_geo\n\tmaterial black_base\n\tposition 0 1.2 0.25\n}\n\n";
			o << "standard_object\n{\n\tname shade_d\n\tgeometry shade_wall2_geo\n\tmaterial black_base\n\tposition 0 1.2 -0.25\n}\n\n";
			o << "standard_object\n{\n\tname shade_e\n\tgeometry shade_floor_geo\n\tmaterial black_base\n\tposition 0 0.95 0\n}\n\n";
			o << "omni_light\n{\n\tname point\n\tpower 40\n\tcolor 1 1 1\n\tposition 0 1.2 0\n}\n\n";
		} else {
			o << "lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tmaterial black_base\n\tscale 6\n}\n\n";
			o << "sphere_geometry\n{\n\tname subject_geo\n\tradius 1\n}\n\n";
			o << "standard_object\n{\n\tname subject_obj\n\tgeometry subject_geo\n\tmaterial subject\n}\n\n";
			// BioSpec skin has no evaluable BSDF (null GetBSDF, DL-126), so
			// no NEE reaches it: the light is found only by BSDF-sampled
			// continuations, and a large emitter keeps that row's noise
			// within its band at a practical sample count.
			o << "sphere_geometry\n{\n\tname light_geo\n\tradius " << ( model == Model::Skin ? 1.5 : 0.4 ) << "\n}\n\n";
			o << "standard_object\n{\n\tname light_obj\n\tgeometry light_geo\n\tmaterial lum\n\tposition 2 2.5 2.5\n}\n\n";
		}
		// DL-49's black absorbing room, and the ideal non-reflecting index
		// enclosure OUTSIDE it: no path ever reaches the enclosure wall, so
		// its only effect is the exterior index the IOR stack is seeded
		// with.  Unlike DL-49's harness the enclosure is present on BOTH
		// sides -- at index 1 (air) in the air scene -- because SMS
		// uniform-area seeding enumerates every specular CASTER shape, and
		// an enclosure on one side only changed that set: the uniform rows
		// read 0.988 (a 20-sd offset under common random numbers) with the
		// enclosure absent from the air scene, 1.000 with it present.
		o << "sphere_geometry\n{\n\tname room_geo\n\tradius 20\n}\n\n";
		o << "standard_object\n{\n\tname room\n\tgeometry room_geo\n\tmaterial black_base\n}\n\n";
		{
			o << "perfectrefractor_material\n{\n\tname enclosure_mat\n\tior " << exterior << "\n\trefractance white\n}\n\n"
			  << "box_geometry\n{\n\tname enclosure_geo\n\twidth 60\n\theight 60\n\tdepth 60\n}\n\n"
			  << "standard_object\n{\n\tname enclosure\n\tgeometry enclosure_geo\n\tmaterial enclosure_mat\n}\n\n";
		}
		o << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		const std::string sms = smsSeeding
			? std::string( "\n\tsms_enabled TRUE\n\tsms_seeding " ) + smsSeeding + "\n\tsms_max_chain_depth 2\n\tsms_target_bounces 1"
			: std::string();
		switch( integrator ) {
		case Integrator::PT:
			o << "pathtracing_pel_rasterizer\n{\n\tsamples " << samples
			  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\tpathguiding FALSE\n\tadaptive_max_samples 0" << sms << "\n}\n\n";
			break;
		case Integrator::BDPT:
			o << "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 6\n\tmax_light_depth 6\n\tsamples " << samples
			  << "\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
			break;
		case Integrator::PTSpectral:
			o << "pathtracing_spectral_rasterizer\n{\n\tsamples " << samples
			  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE" << sms << "\n}\n\n";
			break;
		}
		o << "file_rasterizeroutput\n{\n\tpattern rendered/dl290_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return o.str();
	}

	//! Renders once; returns the RGB-sum image mean, or a negative value on failure.
	double RenderMean( const std::string& path, unsigned int seed, Scalar expectedExterior, bool checkSeed,
		const Point3& camera, const std::string& label )
	{
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) return -1;
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) { safe_release( job ); return -1; }
		if( checkSeed ) {
			IORStack stack( 1.0 );
			IORStackSeeding::SeedFromPoint( stack, camera, *job->GetScene() );
			Check( std::fabs( stack.top() - expectedExterior ) < 1e-12, label + ": camera's seeded exterior index" );
		}
		job->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "dl290 capture" );
		job->GetRasterizer()->AddRasterizerOutput( cap );
		std::srand( seed );
		const bool rendered = job->Rasterize();
		double mean = -1;
		if( rendered && !cap->pixels.empty() ) {
			double sum = 0;
			bool finite = true;
			for( const RISEColor& c : cap->pixels ) {
				const double v = ( c.base.r + c.base.g + c.base.b ) * c.a;
				if( !std::isfinite( v ) ) { finite = false; break; }
				sum += v;
			}
			if( finite ) mean = sum / double( cap->pixels.size() );
		}
		safe_release( cap );
		safe_release( job );
		return mean;
	}

	struct Stats { double mean, sd; };
	Stats Summarize( const std::vector<double>& v )
	{
		double m = 0;
		for( double x : v ) m += x;
		m /= double( v.size() );
		double ss = 0;
		for( double x : v ) ss += ( x - m ) * ( x - m );
		return Stats{ m, v.size() > 1 ? std::sqrt( ss / double( v.size() - 1 ) ) : 0.0 };
	}

	void TestRenderedInvariance( const unsigned int trials, const std::string& only )
	{
		std::cout << "B: rendered scale invariance, air (1, n) vs enclosed (1.5, 1.5 n), n=" << trials << " per side" << std::endl;
		struct Row { Model model; Integrator integrator; unsigned int samples; double band; const char* sms; bool gated = true; };
		// Bands: several times the measured sd of the ratio (common random
		// numbers per pair) and far below the pre-fix deviations of the
		// same rows; both recorded in docs/DL49_SSS_EXTERIOR_INDEX.md §10.
		const Row rows[] = {
			{ Model::Lambertian,   Integrator::PT,         16,  0.02, nullptr },
			{ Model::CookTorrance, Integrator::PT,         32,  0.02, nullptr },
			{ Model::CookTorrance, Integrator::BDPT,       16,  0.02, nullptr },
			{ Model::CookTorrance, Integrator::PTSpectral, 32,  0.03, nullptr },
			{ Model::Hair,         Integrator::PT,         32,  0.03, nullptr },
			{ Model::Hair,         Integrator::BDPT,       16,  0.03, nullptr },
			{ Model::Weave,        Integrator::PT,         32,  0.03, nullptr },
			{ Model::Weave,        Integrator::BDPT,       16,  0.03, nullptr },
			{ Model::Skin,         Integrator::PT,         256, 0.015, nullptr },
			{ Model::Skin,         Integrator::BDPT,       512, 0.02, nullptr },
			{ Model::SMSMirror,    Integrator::PT,         16,  0.03, "snell" },
			{ Model::SMSMirror,    Integrator::PT,         16,  0.03, "uniform" },
			{ Model::SMSMirror,    Integrator::PTSpectral, 16,  0.05, "snell" },
			{ Model::SMSMirror,    Integrator::PTSpectral, 16,  0.05, "uniform" },
			{ Model::SMSGlass,     Integrator::PT,         16,  0.03, "snell", false },
		};
		unsigned int seed = 290000;
		for( const Row& row : rows ) {
			const std::string label = std::string( "B: " ) + ModelName( row.model ) + "/" + IntegratorName( row.integrator ) +
				( row.sms ? std::string( "/sms-" ) + row.sms : std::string() );
			if( !only.empty() && label.find( only ) == std::string::npos ) continue;
			const Point3 camera = ( row.model == Model::SMSMirror || row.model == Model::SMSGlass ) ? Point3( 0, 2.2, 3.4 ) : Point3( 0, 0, 4.5 );
			const std::string airPath = WriteScene( BuildScene( row.model, row.integrator, 1.0, row.samples, row.sms ), "air" );
			const std::string scaledPath = WriteScene( BuildScene( row.model, row.integrator, kScale, row.samples, row.sms ), "scaled" );
			Check( !airPath.empty() && !scaledPath.empty(), label + ": scene files written" );
			std::vector<double> air, scaled;
			bool allValid = true;
			for( unsigned int t = 0; t < trials; ++t ) {
				const unsigned int pairSeed = seed++;
				const double a = RenderMean( airPath, pairSeed, 1.0, t == 0, camera, label + " air" );
				const double sc = RenderMean( scaledPath, pairSeed, kScale, t == 0, camera, label + " enclosed" );
				if( !( a > 0 ) || !( sc > 0 ) ) allValid = false;
				air.push_back( a );
				scaled.push_back( sc );
			}
			std::remove( airPath.c_str() );
			std::remove( scaledPath.c_str() );
			Check( allValid, label + ": every render finite and non-black" );
			if( !allValid ) continue;
			const Stats sa = Summarize( air ), ss = Summarize( scaled );
			const double ratio = ss.mean / sa.mean;
			const double ratioSd = ratio * std::sqrt( ( sa.sd / sa.mean ) * ( sa.sd / sa.mean ) / trials +
				( ss.sd / ss.mean ) * ( ss.sd / ss.mean ) / trials );
			std::cout << std::setprecision( 6 ) << "    " << label.substr( 3 )
				<< " spp=" << row.samples << ": air " << sa.mean << " +/- " << sa.sd
				<< "  enclosed " << ss.mean << " +/- " << ss.sd
				<< "  ratio " << ratio << " +/- " << ratioSd << " (band " << row.band << ")" << std::endl;
			if( row.gated ) {
				Check( std::fabs( ratio - 1.0 ) < row.band, label + ": enclosed/air image mean ratio within band of 1" );
			} else {
				std::cout << "      (NOT GATED: recorded DL-290 residual -- SMS chain etas seed their walk at air)" << std::endl;
			}
		}
	}
}

int main( int argc, char** argv )
{
	unsigned int trials = 4;
	bool unitOnly = false;
	std::string only;
	for( int i = 1; i < argc; ++i ) {
		const std::string a( argv[i] );
		if( a == "--unit-only" ) unitOnly = true;
		else if( a == "--only" && i + 1 < argc ) only = argv[++i];
		else if( a == "--trials" && i + 1 < argc ) trials = static_cast<unsigned int>( std::atoi( argv[++i] ) );
	}
	if( trials < 2 ) trials = 2;

	std::cout << "=== DL-290 exterior-index invariance (non-SSS boundary models) ===" << std::endl;
	{
		MaterialJob mj;
		const std::string mats =
			CookTorranceChunk( "ct_air", 1.0, 1.5, 0.5, false ) +
			CookTorranceChunk( "ct_scaled", kScale, 1.5, 0.5, false ) +
			CookTorranceChunk( "ct_matched", 1.0, 1.5, 0.0, true ) +
			"cooktorrance_material\n{\n\tname ct_matched_nospec\n\trd black\n\trs black\n\tfacets 0.25\n\tior 1.5\n\textinction 0\n}\n\n" +
			HairChunk( "hair_air", 1.0, 0.4 ) +
			HairChunk( "hair_scaled", kScale, 0.4 ) +
			HairChunk( "hair_opaque_matched", 1.0, 2000.0 ) +
			WeaveChunk( "weave_air", 1.0 ) +
			WeaveChunk( "weave_scaled", kScale ) +
			SkinChunk( "skin_air", 1.0, 1.55 ) +
			SkinChunk( "skin_scaled", kScale, 1.55 ) +
			SkinChunk( "skin_air_matched", 1.0, 1.0 ) +
			SkinChunk( "skin_scaled_matched", kScale, 1.0 );
		Check( mj.Load( mats ), "materials scene loads" );
		TestCookTorrance( mj );
		TestHair( mj );
		TestWeave( mj );
		TestSkin( mj );
	}
	TestSMSRigs();
	if( !unitOnly ) {
		TestRenderedInvariance( trials, only );
	}
	std::cout << "=== " << passCount << " passed, " << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
