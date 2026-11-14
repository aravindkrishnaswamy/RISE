//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeThinFilmDispersionTest.cpp - Contract test for the
//    ABI v18 half of the Blender native bridge: Principled BSDF
//    "Thin Film Thickness" / "Thin Film IOR" (Blender 4.2+) and
//    "Transmission Dispersion Scale" / "Abbe Number" (Blender 5.3,
//    OpenPBR 1.1.1), i.e. `rise_blender_material`'s
//    `thin_film_*` / `dispersion_abbe` fields and the code in
//    src/Blender/native/rise_blender_bridge.cpp that consumes them
//    (`register_cauchy_ior_painter`, the DIELECTRIC branch of
//    `create_surface_material`, `add_thin_film_ggx_base`).
//
//  WHY THIS TEST INCLUDES A .cpp.  Same reasoning as
//  BlenderBridgeFabricTest.cpp's own banner: the functions under test
//  are file-local to the bridge, so the test compiles the shipping
//  translation unit directly.
//
//  RED-PROOF.  Written against the v17 header/.cpp, every reference to
//  the new fields fails to compile; once they exist the checks below
//  read back the CONSTRUCTED RISE state (the dielectric's IOR painter
//  and AR layers, the GGX material's Fresnel mode / film painters /
//  diffuse / specular) rather than trusting a return code.
//
//  The Python half (socket reading, Abbe/scale folding, the F82-tint
//  conductor solve) is covered by
//  src/Blender/addons/rise_renderer/test_hair_export.py
//  (`PrincipledMathTest`).
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>

#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/Materials/DielectricMaterial.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"

using RISE::Implementation::CoatedMaterial;
using RISE::Implementation::DielectricMaterial;
using RISE::Implementation::FabricMaterial;
using RISE::Implementation::GGXMaterial;
using RISE::Implementation::LambertianLuminaireMaterial;

static int g_checks   = 0;
static int g_failures = 0;

static void Check( const bool ok, const std::string& what )
{
	++g_checks;
	if( !ok ) {
		++g_failures;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

namespace {

class JobHolder
{
public:
	JobHolder() : p( 0 ) { RISE::RISE_CreateJobPriv( &p ); }
	~JobHolder() { RISE::safe_release( p ); }
	RISE::IJobPriv& operator*() const { return *p; }
	bool Valid() const { return p != 0; }
private:
	JobHolder( const JobHolder& );
	JobHolder& operator=( const JobHolder& );
	RISE::IJobPriv* p;
};

const char* Keep( const std::string& s )
{
	static std::deque<std::string> storage;
	storage.push_back( s );
	return storage.back().c_str();
}

RISE::RayIntersectionGeometric MakeProbe( const double thetaDeg = 30.0 )
{
	const double th = thetaDeg * 3.14159265358979323846 / 180.0;
	const RISE::Vector3 inDir( std::sin( th ), 0, -std::cos( th ) );
	RISE::Ray inRay( RISE::Point3( std::sin( th ), 0, 1.0 ), inDir );
	RISE::RasterizerState rs = { 0, 0 };
	RISE::RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = RISE::Point3( 0, 0, 0 );
	ri.vNormal = RISE::Vector3( 0, 0, 1 );
	ri.vGeomNormal = RISE::Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( RISE::Vector3( 0, 0, 1 ) );
	ri.ptCoord = RISE::Point2( 0.5, 0.5 );
	ri.ptCoord1 = ri.ptCoord;
	return ri;
}

//! Mirror direction of MakeProbe's incident ray: probes the specular peak.
RISE::Vector3 MirrorOut( const double thetaDeg = 30.0 )
{
	const double th = thetaDeg * 3.14159265358979323846 / 180.0;
	return RISE::Vector3( std::sin( th ), 0, std::cos( th ) );
}

RISE::RISEPel Respond( RISE::IJobPriv& job, const char* matName, const double thetaDeg = 30.0 )
{
	RISE::IMaterial* m = job.GetMaterials() ? job.GetMaterials()->GetItem( matName ) : 0;
	if( !m || !m->GetBSDF() ) return RISE::RISEPel( -1, -1, -1 );
	RISE::RayIntersectionGeometric ri = MakeProbe( thetaDeg );
	return m->GetBSDF()->value( MirrorOut( thetaDeg ), ri );
}

template< class T >
const T* MaterialAs( RISE::IJobPriv& job, const char* name )
{
	RISE::IMaterial* material = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	return dynamic_cast<const T*>( material );
}

rise_blender_material DielectricFixture( const std::string& tag, const char* iorLiteral )
{
	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = Keep( tag );
	mat.model = RISE_BLENDER_MATERIAL_DIELECTRIC;
	mat.double_sided = 1;
	mat.tau_painter_name = "1 1 1";
	mat.ior_painter_name = iorLiteral;
	mat.scatter_painter_name = "1000000";
	return mat;
}

rise_blender_material PbrFixture( RISE::IJobPriv& job, const std::string& tag )
{
	const std::string baseName  = tag + "_base";
	const std::string metalName = tag + "_metal";
	const std::string roughName = tag + "_rough";
	double baseColor[3] = { 0.6, 0.2, 0.2 };
	double metal[3]     = { 0.0, 0.0, 0.0 };
	double rough[3]     = { 0.4, 0.4, 0.4 };
	job.AddUniformColorPainter( baseName.c_str(), baseColor, "Rec709RGB_Linear" );
	job.AddUniformColorPainter( metalName.c_str(), metal, "Rec709RGB_Linear" );
	job.AddUniformColorPainter( roughName.c_str(), rough, "Rec709RGB_Linear" );

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = Keep( tag );
	mat.model = RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS;
	mat.double_sided = 1;
	mat.base_color_painter_name = Keep( baseName );
	mat.metallic_painter_name = Keep( metalName );
	mat.roughness_painter_name = Keep( roughName );
	mat.emissive_scale = 1.0;
	return mat;
}

// Independent statement of the OpenPBR/Cauchy construction.
void CauchyReference( const double n_d, const double abbe, double& A, double& B )
{
	const double invF = 1.0 / ( 486.1 * 486.1 );
	const double invC = 1.0 / ( 656.3 * 656.3 );
	const double invD = 1.0 / ( 587.6 * 587.6 );
	B = ( n_d - 1.0 ) / ( abbe * ( invF - invC ) );
	A = n_d - B * invD;
}

// ============================================================

void TestAbiVersionAndLayout()
{
	std::cout << "Test: ABI version 18 and append-only v18 fields" << std::endl;
	Check( RISE_BLENDER_API_VERSION == 18, "RISE_BLENDER_API_VERSION is 18" );
	Check( rise_blender_api_version() == RISE_BLENDER_API_VERSION,
		"rise_blender_api_version() reports the compiled-in constant" );
	Check( offsetof( rise_blender_material, thin_film_thickness ) >
	       offsetof( rise_blender_material, alpha_threshold ),
		"thin_film_thickness is appended after the last v17 field" );
	Check( offsetof( rise_blender_material, thin_film_ior ) > offsetof( rise_blender_material, thin_film_thickness ),
		"thin_film_ior follows thin_film_thickness" );
	Check( offsetof( rise_blender_material, thin_film_conductor ) > offsetof( rise_blender_material, thin_film_ior ),
		"thin_film_conductor follows thin_film_ior" );
	Check( offsetof( rise_blender_material, thin_film_dielectric_ior ) > offsetof( rise_blender_material, thin_film_conductor ),
		"thin_film_dielectric_ior follows thin_film_conductor" );
	Check( offsetof( rise_blender_material, thin_film_metal_f0 ) > offsetof( rise_blender_material, thin_film_dielectric_ior ),
		"thin_film_metal_f0 follows thin_film_dielectric_ior" );
	Check( offsetof( rise_blender_material, thin_film_metal_tint ) > offsetof( rise_blender_material, thin_film_metal_f0 ),
		"thin_film_metal_tint follows thin_film_metal_f0" );
	Check( offsetof( rise_blender_material, dispersion_abbe ) > offsetof( rise_blender_material, thin_film_metal_tint ),
		"dispersion_abbe is the last field" );
}

void TestDielectricWithoutNewFieldsIsUnchanged()
{
	std::cout << "Test: a v17-shaped dielectric (no film, no dispersion) is unchanged" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = DielectricFixture( "plain_glass", "1.5" );
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), "plain dielectric registered" );
	const DielectricMaterial* d = MaterialAs<DielectricMaterial>( *job, "plain_glass" );
	Check( d != 0, "plain dielectric is a DielectricMaterial" );
	if( !d ) return;
	Check( d->GetARLayerCount() == 0, "no AR layer without a thin film" );
	Check( !d->GetIOR().HasPerChannelVariation(), "IOR is not per-channel without dispersion" );
	Check( (*job).GetScalarPainters()->GetItem( "plain_glass::ior_cauchy" ) == 0,
		"no Cauchy painter is registered without dispersion" );
	RISE::RayIntersectionGeometric ri = MakeProbe();
	Check( std::fabs( d->GetIOR().GetValueAtNM( ri, 450.0 ) - 1.5 ) < 1e-12 &&
	       std::fabs( d->GetIOR().GetValueAtNM( ri, 650.0 ) - 1.5 ) < 1e-12,
		"non-dispersive IOR is flat at 1.5" );
}

void TestDispersionBuildsCauchyCurve()
{
	std::cout << "Test: dispersion_abbe builds a Cauchy IOR pinned at the d line" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	const double n_d = 1.5168, abbe = 64.17;		// BK7-like
	rise_blender_material mat = DielectricFixture( "bk7", "1.5168" );
	mat.dispersion_abbe = abbe;
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "dispersive dielectric registered: " ) + err );
	const DielectricMaterial* d = MaterialAs<DielectricMaterial>( *job, "bk7" );
	Check( d != 0, "dispersive dielectric is a DielectricMaterial" );
	if( !d ) return;

	Check( (*job).GetScalarPainters()->GetItem( "bk7::ior_cauchy" ) == &d->GetIOR(),
		"the material's IOR IS the registered Cauchy painter" );

	RISE::RayIntersectionGeometric ri = MakeProbe();
	const RISE::IScalarPainter& ior = d->GetIOR();
	const double nF = ior.GetValueAtNM( ri, 486.1 );
	const double nD = ior.GetValueAtNM( ri, 587.6 );
	const double nC = ior.GetValueAtNM( ri, 656.3 );
	// 1 nm piecewise-linear sampling: off-node error <= 0.75 B / lambda^4
	// (about 6e-7 for BK7); allow 1e-6 absolute.
	Check( std::fabs( nD - n_d ) < 1e-6, "n(587.6 nm) == n_d" );
	const double recoveredAbbe = ( nD - 1.0 ) / ( nF - nC );
	Check( std::fabs( recoveredAbbe - abbe ) / abbe < 2e-3, "(n_d - 1) / (n_F - n_C) recovers the Abbe number" );
	Check( nF > nD && nD > nC, "normal dispersion: index falls with wavelength" );

	double A = 0, B = 0;
	CauchyReference( n_d, abbe, A, B );
	double worst = 0.0;
	for( double nm = 380.0; nm <= 780.0; nm += 7.3 ) {
		worst = std::max( worst, std::fabs( ior.GetValueAtNM( ri, nm ) - ( A + B / ( nm * nm ) ) ) );
	}
	Check( worst < 1e-6, "the painter tracks A + B/lambda^2 across the visible band" );

	// RGB rasterizers read per-channel IOR (DL-82): blue > green > red.
	const RISE::ScalarTriple rgb = ior.GetValuesAt( ri );
	Check( rgb[2] > rgb[1] && rgb[1] > rgb[0], "per-channel RGB IOR is ordered B > G > R" );
	Check( ior.HasPerChannelVariation(), "the dispersive IOR reports per-channel variation" );
}

void TestDispersionRejectsNonNumericIor()
{
	std::cout << "Test: dispersion with a non-literal IOR fails loudly" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double v[3] = { 1.5, 1.5, 1.5 };
	(*job).AddUniformColorPainter( "named_ior", v, "Rec709RGB_Linear" );
	rise_blender_material mat = DielectricFixture( "bad_glass", "named_ior" );
	mat.dispersion_abbe = 30.0;
	char err[256] = { 0 };
	Check( !add_material( *job, mat, err, sizeof( err ) ), "a named-painter IOR with dispersion is refused" );
	Check( std::string( err ).find( "numeric IOR" ) != std::string::npos, "the refusal names the cause" );

	rise_blender_material trailing = DielectricFixture( "bad_glass2", "1.5x" );
	trailing.dispersion_abbe = 30.0;
	std::memset( err, 0, sizeof( err ) );
	Check( !add_material( *job, trailing, err, sizeof( err ) ), "a literal with trailing garbage is refused" );
}

void TestDielectricThinFilmIsArLayer()
{
	std::cout << "Test: a thin film on the dielectric becomes one AR layer" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = DielectricFixture( "coated_glass", "1.5" );
	mat.thin_film_thickness = 380.0;
	mat.thin_film_ior = 1.33;
	mat.dispersion_abbe = 40.0;
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "film+dispersion dielectric registered: " ) + err );
	const DielectricMaterial* d = MaterialAs<DielectricMaterial>( *job, "coated_glass" );
	Check( d != 0, "is a DielectricMaterial" );
	if( !d ) return;
	Check( d->GetARLayerCount() == 1, "exactly one AR layer" );
	if( d->GetARLayerCount() == 1 ) {
		Check( std::fabs( d->GetARLayerN()[0] - 1.33 ) < 1e-12, "AR layer n is the film IOR" );
		Check( d->GetARLayerK()[0] == 0.0, "AR layer is transparent (k = 0)" );
		Check( std::fabs( d->GetARLayerT()[0] - 380.0 ) < 1e-12, "AR layer thickness is the film thickness (nm)" );
	}
	Check( d->GetIOR().HasPerChannelVariation(), "film and dispersion compose (IOR still dispersive)" );
}

void TestPbrWithoutFilmStaysPbr()
{
	std::cout << "Test: a PBR material with no film is not routed to thin-film GGX" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }
	rise_blender_material mat = PbrFixture( *job, "plain_pbr" );
	mat.thin_film_dielectric_ior = 1.5;		// ignored: thickness is 0
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), "plain PBR registered" );
	const GGXMaterial* g = MaterialAs<GGXMaterial>( *job, "plain_pbr" );
	Check( g != 0, "PBR builds a GGXMaterial" );
	if( g ) Check( g->GetFresnelMode() != RISE::eFresnelThinFilmConductor, "no thin-film Fresnel without a film" );
}

void TestPbrDielectricThinFilm()
{
	std::cout << "Test: a dielectric-substrate thin film builds a thinfilm GGX base" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	auto build = [&]( const std::string& tag, const double thickness ) {
		rise_blender_material mat = PbrFixture( *job, tag );
		mat.thin_film_thickness = thickness;
		mat.thin_film_ior = 1.33;
		mat.thin_film_dielectric_ior = 1.5;
		mat.thin_film_conductor = 0;
		char err[256] = { 0 };
		Check( add_material( *job, mat, err, sizeof( err ) ), "thin-film PBR registered (" + tag + "): " + err );
	};
	build( "film300", 300.0 );
	build( "film520", 520.0 );

	const GGXMaterial* g = MaterialAs<GGXMaterial>( *job, "film300" );
	Check( g != 0, "thin-film base is a GGXMaterial" );
	if( !g ) return;
	Check( g->GetFresnelMode() == RISE::eFresnelThinFilmConductor, "Fresnel mode is thinfilm" );
	RISE::RayIntersectionGeometric ri = MakeProbe();
	Check( g->GetFilmIOR() && std::fabs( g->GetFilmIOR()->GetValueAtNM( ri, 550.0 ) - 1.33 ) < 1e-12,
		"film IOR painter carries Thin Film IOR" );
	Check( g->GetFilmThickness() && std::fabs( g->GetFilmThickness()->GetValueAtNM( ri, 550.0 ) - 300.0 ) < 1e-9,
		"film thickness painter carries the thickness in nm" );
	Check( std::fabs( g->GetIOR().GetValueAtNM( ri, 550.0 ) - 1.5 ) < 1e-12, "substrate n is thin_film_dielectric_ior" );
	Check( std::fabs( g->GetExtinction().GetValueAtNM( ri, 550.0 ) ) < 1e-12, "dielectric substrate has k = 0" );
	const RISE::RISEPel diffuse = g->GetDiffuse().GetColor( ri );
	Check( std::fabs( diffuse[0] - 0.6 ) < 1e-9 && std::fabs( diffuse[1] - 0.2 ) < 1e-9,
		"dielectric substrate keeps Base Color as its diffuse" );
	Check( std::fabs( g->GetAlphaX().GetValuesAt( ri )[0] - 0.16 ) < 1e-9, "alpha is roughness^2 (0.4^2)" );

	// Interference: the specular response's colour shifts with thickness.
	const RISE::RISEPel a = Respond( *job, "film300" );
	const RISE::RISEPel b = Respond( *job, "film520" );
	Check( a[0] > 0 && b[0] > 0, "both films have a positive specular response" );
	const double ratioA = a[2] / a[0];
	const double ratioB = b[2] / b[0];
	std::cout << "    film300 " << a[0] << " " << a[1] << " " << a[2] << "  film520 " << b[0] << " " << b[1] << " " << b[2] << std::endl;
	Check( std::fabs( ratioA - ratioB ) > 0.02, "the reflected hue depends on film thickness (interference)" );
}

//! RISE's own Jakob-Hanika reflectance uplift of an RGB colour at `nm`.
double Uplift( RISE::IJobPriv& job, const std::string& name, const double rgb[3], const double nm )
{
	double c[3] = { rgb[0], rgb[1], rgb[2] };
	if( !job.GetPainters()->GetItem( name.c_str() ) ) {
		job.AddUniformColorPainter( name.c_str(), c, "Rec709RGB_Linear" );
	}
	RISE::RayIntersectionGeometric ri = MakeProbe();
	return RISE::ReflectanceColorNM( *job.GetPainters()->GetItem( name.c_str() ), ri, nm );
}

void TestPbrConductorThinFilm()
{
	std::cout << "Test: a metal under a film gets per-wavelength n, k from the uplifted F0 / tint" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	const double f0[3] = { 0.95, 0.64, 0.54 };		// copper-ish Base Color
	const double tint[3] = { 1.0, 0.9, 0.8 };
	rise_blender_material mat = PbrFixture( *job, "film_metal" );
	mat.thin_film_thickness = 250.0;
	mat.thin_film_ior = 2.0;
	mat.thin_film_conductor = 1;
	for( int c = 0; c < 3; ++c ) { mat.thin_film_metal_f0[c] = f0[c]; mat.thin_film_metal_tint[c] = tint[c]; }
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "conductor thin film registered: " ) + err );
	const GGXMaterial* g = MaterialAs<GGXMaterial>( *job, "film_metal" );
	Check( g != 0, "is a GGXMaterial" );
	if( !g ) return;
	RISE::RayIntersectionGeometric ri = MakeProbe();
	const RISE::RISEPel diffuse = g->GetDiffuse().GetColor( ri );
	Check( diffuse[0] == 0.0 && diffuse[1] == 0.0 && diffuse[2] == 0.0, "conductor substrate has a black diffuse" );
	const RISE::RISEPel spec = g->GetSpecular().GetColor( ri );
	Check( spec[0] == 1.0 && spec[1] == 1.0 && spec[2] == 1.0, "conductor specular tint is white" );
	Check( (*job).GetScalarPainters()->GetItem( "film_metal::tf_n" ) == &g->GetIOR() &&
	       (*job).GetScalarPainters()->GetItem( "film_metal::tf_k" ) == &g->GetExtinction(),
		"the substrate is the registered per-wavelength n / k pair" );

	// At sample wavelengths the painters ARE Cycles' estimate of the
	// uplifted spectra, and the conductor reproduces the uplifted F0.
	double worstNk = 0.0, worstF0 = 0.0;
	for( double nm = 400.0; nm <= 750.0; nm += 25.0 ) {
		double nWant = 0, kWant = 0;
		cycles_thin_film_metal_nk( Uplift( *job, "ref_f0", f0, nm ), Uplift( *job, "ref_tint", tint, nm ), nWant, kWant );
		const double n = g->GetIOR().GetValueAtNM( ri, nm );
		const double k = g->GetExtinction().GetValueAtNM( ri, nm );
		worstNk = std::max( worstNk, std::max( std::fabs( n - nWant ), std::fabs( k - kWant ) ) );
		const double F0 = ( ( n - 1 ) * ( n - 1 ) + k * k ) / ( ( n + 1 ) * ( n + 1 ) + k * k );
		worstF0 = std::max( worstF0, std::fabs( F0 - std::min( 0.999, Uplift( *job, "ref_f0", f0, nm ) ) ) );
	}
	Check( worstNk < 1e-9, "n(lambda), k(lambda) are Cycles' estimate at each sample wavelength" );
	Check( worstF0 < 1e-9, "the substrate's normal-incidence reflectance is the uplifted F0 spectrum" );
}

void TestCyclesMetalEstimate()
{
	std::cout << "Test: cycles_thin_film_metal_nk reproduces Cycles' estimate, clamps included" << std::endl;
	// Reference values hand-evaluated (Python, outside this code) in
	// Cycles' order of operations: kernel/svm/closure.h first clamps the
	// tint, `f82 = min(specular_tint, 1)`; bsdf_util.h's fresnel_f82 then
	// gives g = saturate(Schlick(F0, 1/7) * f82) with the UNCLAMPED F0;
	// bsdf_microfacet.h's F82_TINT-with-film branch applies Gulbrandsen
	// with r = min(F0, 0.999).  A tint above 1 therefore behaves exactly
	// like tint 1 (k stays positive, F(0) = r).
	struct Ref { double f0, tint, n, k; };
	const Ref refs[] = {
		{ 0.7, 1.3, 1.96062848, 4.16849998 },		// == tint 1.0
		{ 0.7, 1.0, 1.96062848, 4.16849998 },
		{ 0.9, 1.1, 2.09026388, 8.60585989 },
		{ 1.0, 1.0, 0.00050025, 0.99999987 },		// white metal: g = Schlick(1) = 1
		{ 0.5, 0.6, 3.41718376, 2.79749132 },
	};
	for( const Ref& ref : refs ) {
		double n = 0, k = 0;
		cycles_thin_film_metal_nk( ref.f0, ref.tint, n, k );
		std::cout << "    f0 " << ref.f0 << " tint " << ref.tint << " -> n " << n << " k " << k << std::endl;
		Check( std::fabs( n - ref.n ) < 1e-6 && std::fabs( k - ref.k ) < 1e-6,
			"matches Cycles' (n, k) for f0 " + std::to_string( ref.f0 ) + ", tint " + std::to_string( ref.tint ) );
	}

	// With g <= 1 always, F(0) = min(F0, 0.999) for every tint.
	for( double r : { 0.04, 0.3, 0.7, 0.95 } ) {
		for( double tint : { 0.5, 1.0, 1.5 } ) {
			double n = 0, k = 0;
			cycles_thin_film_metal_nk( r, tint, n, k );
			const double F0 = ( ( n - 1 ) * ( n - 1 ) + k * k ) / ( ( n + 1 ) * ( n + 1 ) + k * k );
			Check( std::fabs( F0 - r ) < 1e-9, "F(0) = F0 for any tint" );
		}
	}
}

void TestThinFilmMissingSubstrateFails()
{
	std::cout << "Test: a dielectric film without a substrate IOR is refused" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }
	rise_blender_material mat = PbrFixture( *job, "film_nosub" );		// memset: dielectric ior 0
	mat.thin_film_thickness = 300.0;
	mat.thin_film_ior = 1.33;
	char err[256] = { 0 };
	Check( !add_material( *job, mat, err, sizeof( err ) ), "a zero substrate IOR refuses the material" );
	Check( std::string( err ).find( "thin_film_dielectric_ior" ) != std::string::npos, "the refusal names the field" );
}

void TestThinFilmComposesWithCoatAndEmission()
{
	std::cout << "Test: thin-film base composes with coat and deferred emission" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double e[3] = { 2.0, 2.0, 2.0 };
	(*job).AddUniformColorPainter( "film_coat_emit_e", e, "Rec709RGB_Linear" );
	rise_blender_material mat = PbrFixture( *job, "film_coat" );
	mat.thin_film_thickness = 400.0;
	mat.thin_film_ior = 1.4;
	mat.thin_film_dielectric_ior = 1.5;
	mat.coat_weight = 1.0;
	mat.coat_roughness = 0.1;
	mat.coat_ior = 1.5;
	mat.emission_painter_name = "film_coat_emit_e";
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "film + coat + emission registered: " ) + err );

	const GGXMaterial* base = MaterialAs<GGXMaterial>( *job, "film_coat::pbrbase" );
	Check( base != 0 && base->GetFresnelMode() == RISE::eFresnelThinFilmConductor,
		"the intermediate base is the thin-film GGX" );
	if( base ) Check( base->GetEmitter() == 0, "emission is NOT baked into the coat substrate" );
	Check( MaterialAs<CoatedMaterial>( *job, "film_coat::coatbase" ) != 0, "a coated layer wraps the film base" );
	const LambertianLuminaireMaterial* outer = MaterialAs<LambertianLuminaireMaterial>( *job, "film_coat" );
	Check( outer != 0 && outer->GetEmitter() != 0, "emission is re-attached at the outer layer" );
}

void TestLowAbbeInterpolationBound()
{
	std::cout << "Test: 1 nm sampling holds the documented interpolation bound at a low Abbe number" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	// V = 8 is far more dispersive than any optical glass; the bound
	// 0.75 B / lambda^4 (h = 1 nm) is largest at the 300 nm end.
	const double n_d = 1.5, abbe = 8.0;
	rise_blender_material mat = DielectricFixture( "flint8", "1.5" );
	mat.dispersion_abbe = abbe;
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "abbe-8 dielectric registered: " ) + err );
	const DielectricMaterial* d = MaterialAs<DielectricMaterial>( *job, "flint8" );
	if( !d ) { Check( false, "is a DielectricMaterial" ); return; }

	double A = 0, B = 0;
	CauchyReference( n_d, abbe, A, B );
	const double bound = 0.75 * B / std::pow( 300.0, 4.0 );
	RISE::RayIntersectionGeometric ri = MakeProbe();
	double worst = 0.0;
	for( double nm = 300.5; nm < 900.0; nm += 1.0 ) {		// mid-cell: the worst case
		worst = std::max( worst, std::fabs( d->GetIOR().GetValueAtNM( ri, nm ) - ( A + B / ( nm * nm ) ) ) );
	}
	std::cout << "    worst mid-cell error " << worst << " vs bound " << bound << std::endl;
	Check( worst <= bound * 1.0001, "mid-cell interpolation error is within 0.75 B / 300^4" );
	Check( bound < 3.5e-6, "the bound itself is below 3.5e-6 at abbe 8" );
}

void TestNonPhysicalDispersionRefused()
{
	std::cout << "Test: a Cauchy curve that goes non-positive in the band is refused" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }
	rise_blender_material mat = DielectricFixture( "absurd", "1.5" );
	mat.dispersion_abbe = 0.1;			// n(900 nm) ~ -2.85
	char err[256] = { 0 };
	Check( !add_material( *job, mat, err, sizeof( err ) ), "abbe 0.1 is refused" );
	Check( std::string( err ).find( "positive" ) != std::string::npos, "the refusal says why" );
}

void TestDispersiveDielectricWithEmission()
{
	std::cout << "Test: a dispersive emissive dielectric registers (wrapped base name)" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }
	double e[3] = { 1.0, 0.5, 0.2 };
	(*job).AddUniformColorPainter( "glow_e", e, "Rec709RGB_Linear" );
	rise_blender_material mat = DielectricFixture( "glowglass", "1.45" );
	mat.dispersion_abbe = 30.0;
	mat.emission_painter_name = "glow_e";
	mat.emissive_scale = 1.0;
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "emissive dispersive dielectric registered: " ) + err );
	RISE::IMaterial* outer = (*job).GetMaterials()->GetItem( "glowglass" );
	Check( outer != 0 && outer->GetEmitter() != 0, "the outer material emits" );
}

void TestThinFilmEmissionBakedWithoutWraps()
{
	std::cout << "Test: with no sheen/coat, emission is baked straight into the film base" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }
	double e[3] = { 3.0, 3.0, 3.0 };
	(*job).AddUniformColorPainter( "film_emit_e", e, "Rec709RGB_Linear" );
	rise_blender_material mat = PbrFixture( *job, "film_emit" );
	mat.thin_film_thickness = 350.0;
	mat.thin_film_ior = 1.4;
	mat.thin_film_dielectric_ior = 1.5;
	mat.emission_painter_name = "film_emit_e";
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "emissive film registered: " ) + err );
	const GGXMaterial* g = MaterialAs<GGXMaterial>( *job, "film_emit" );
	Check( g != 0 && g->GetFresnelMode() == RISE::eFresnelThinFilmConductor, "the registered material is the film GGX itself" );
	if( g ) Check( g->GetEmitter() != 0, "and it carries the emitter" );
}

void TestThinFilmUnderSheen()
{
	std::cout << "Test: sheen wraps the thin-film base" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }
	double sheen[3] = { 0.8, 0.8, 0.8 };
	(*job).AddUniformColorPainter( "film_sheen_c", sheen, "Rec709RGB_Linear" );
	rise_blender_material mat = PbrFixture( *job, "film_sheen" );
	mat.thin_film_thickness = 300.0;
	mat.thin_film_ior = 1.33;
	mat.thin_film_dielectric_ior = 1.5;
	mat.sheen_color_painter_name = "film_sheen_c";
	mat.sheen_roughness = 0.5;
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "film + sheen registered: " ) + err );
	const GGXMaterial* base = MaterialAs<GGXMaterial>( *job, "film_sheen::pbrbase" );
	Check( base != 0 && base->GetFresnelMode() == RISE::eFresnelThinFilmConductor, "the sheen substrate is the film GGX" );
	Check( MaterialAs<FabricMaterial>( *job, "film_sheen" ) != 0, "the final material is the fabric (sheen) wrap" );
}

void TestThinFilmDielectricTintAndLiterals()
{
	std::cout << "Test: dielectric film tint is the specular painter; numeric literals are accepted" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }
	double tint[3] = { 1.2, 1.0, 0.8 };
	(*job).AddUniformColorPainter( "film_tint_c", tint, "Rec709RGB_Linear" );

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = "film_literal";
	mat.model = RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS;
	mat.double_sided = 1;
	mat.base_color_painter_name = "0.3 0.4 0.5";		// literal, as a direct API caller may pass
	mat.thin_film_dielectric_ior = 1.5;
	mat.metallic_painter_name = "0";
	mat.roughness_painter_name = ".3";
	mat.specular_color_painter_name = "film_tint_c";
	mat.emissive_scale = 1.0;
	mat.thin_film_thickness = 450.0;
	mat.thin_film_ior = 1.33;
	mat.thin_film_dielectric_ior = 1.5;
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "literal-input film registered: " ) + err );
	const GGXMaterial* g = MaterialAs<GGXMaterial>( *job, "film_literal" );
	if( !g ) { Check( false, "is a GGXMaterial" ); return; }
	RISE::RayIntersectionGeometric ri = MakeProbe();
	Check( std::fabs( g->GetAlphaX().GetValuesAt( ri )[0] - 0.09 ) < 1e-9, "alpha = .3^2 from a numeric roughness" );
	const RISE::RISEPel diffuse = g->GetDiffuse().GetColor( ri );
	Check( std::fabs( diffuse[0] - 0.3 ) < 1e-9 && std::fabs( diffuse[2] - 0.5 ) < 1e-9, "diffuse from a colour literal" );
	const RISE::RISEPel spec = g->GetSpecular().GetColor( ri );
	Check( std::fabs( spec[0] - 1.2 ) < 1e-9 && std::fabs( spec[2] - 0.8 ) < 1e-9, "the film tint is the specular painter" );
}

//! A vanishing film over a metal should reproduce the metal: the film base,
//! thinned to 0.01 nm (the 0.1 nm cutoff is the exporter's, so the native
//! thinfilm path still runs), is compared with RISE's `conductor` mode given Cycles' own
//! per-channel n, k for the same Base Color -- i.e. what Cycles renders
//! with no film.  The film path's substrate is the per-wavelength
//! spectrum (uplifted F0 / tint), so a saturated metal keeps its colour.
//! (Handing the per-channel n, k to the film path as an "r g b" literal
//! instead -- an earlier revision -- badly desaturated a saturated metal.)
void TestVanishingFilmMetalKeepsColour()
{
	std::cout << "Test: a vanishing film over a metal reproduces the metal's colour" << std::endl;
	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	struct Metal { const char* name; double f0[3]; double rgbBand; };
	const Metal metals[] = {
		{ "grey", { 0.6, 0.6, 0.6 }, 0.002 },
		{ "gold", { 1.0, 0.766, 0.336 }, 0.04 },
		{ "red",  { 0.9, 0.1, 0.1 }, 0.16 },
	};
	double black[3] = { 0, 0, 0 }, white[3] = { 1, 1, 1 };
	(*job).AddUniformColorPainter( "resid_black", black, "Rec709RGB_Linear" );
	(*job).AddUniformColorPainter( "resid_white", white, "Rec709RGB_Linear" );
	for( const Metal& m : metals ) {
		char nLit[128], kLit[128];
		double n[3], k[3];
		for( int c = 0; c < 3; ++c ) cycles_thin_film_metal_nk( m.f0[c], 1.0, n[c], k[c] );
		std::snprintf( nLit, sizeof( nLit ), "%.9g %.9g %.9g", n[0], n[1], n[2] );
		std::snprintf( kLit, sizeof( kLit ), "%.9g %.9g %.9g", k[0], k[1], k[2] );
		const std::string conductorName = std::string( "resid_cond_" ) + m.name;
		const std::string filmName = std::string( "resid_film_" ) + m.name;
		Check( (*job).AddGGXMaterial( conductorName.c_str(), "resid_black", "resid_white", ".16", ".16", nLit, kLit, "conductor" ),
			std::string( "conductor reference registered: " ) + m.name );

		rise_blender_material mat = PbrFixture( *job, filmName );
		mat.roughness_painter_name = ".4";				// alpha .16, matching the reference
		mat.thin_film_thickness = 0.01;			// the exporter's 0.1 nm cutoff is not native
		mat.thin_film_ior = 1.33;
		mat.thin_film_conductor = 1;
		for( int c = 0; c < 3; ++c ) { mat.thin_film_metal_f0[c] = m.f0[c]; mat.thin_film_metal_tint[c] = 1.0; }
		char err[256] = { 0 };
		Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "vanishing film registered: " ) + err );

		const RISE::RISEPel ref = Respond( *job, conductorName.c_str() );
		const RISE::RISEPel film = Respond( *job, filmName.c_str() );
		double worst = 0.0;
		std::cout << "    " << m.name << " RGB film/conductor per channel:";
		for( int c = 0; c < 3; ++c ) {
			const double ratio = ref[c] > 0 ? film[c] / ref[c] : 0.0;
			worst = std::max( worst, std::fabs( ratio - 1.0 ) );
			std::cout << " " << ratio;
		}
		std::cout << std::endl;
		// RGB (Pel) path: GGXBRDF's thinfilm branch integrates the film's
		// spectral reflectance back to RGB per call ("PREVIEW-grade; the
		// spectral path is authoritative"), so a saturated metal keeps a
		// residual there.  Measured: grey 0.02%, gold 3.3%, red 14.7% (its
		// 0.1 green channel); the spectral check below is exact to 2.3e-4.  Bands guard against it growing.
		Check( worst <= m.rgbBand, std::string( "Pel vanishing-film residual within its pinned band: " ) + m.name );

		// Spectral path: per wavelength, the vanishing film equals a
		// conductor built from the SAME n(lambda), k(lambda) painters.
		const std::string specRef = std::string( "resid_specref_" ) + m.name;
		const std::string nName = filmName + "::tf_n", kName = filmName + "::tf_k";
		Check( (*job).AddGGXMaterial( specRef.c_str(), "resid_black", "resid_white", ".16", ".16",
			nName.c_str(), kName.c_str(), "conductor" ), "spectral reference registered" );
		RISE::IMaterial* filmMat = (*job).GetMaterials()->GetItem( filmName.c_str() );
		RISE::IMaterial* refMat = (*job).GetMaterials()->GetItem( specRef.c_str() );
		double worstNM = 0.0;
		if( filmMat && refMat ) {
			RISE::RayIntersectionGeometric ri = MakeProbe();
			for( double nm = 400.0; nm <= 720.0; nm += 20.0 ) {
				const double a = filmMat->GetBSDF()->valueNM( MirrorOut(), ri, nm );
				const double b = refMat->GetBSDF()->valueNM( MirrorOut(), ri, nm );
				worstNM = std::max( worstNM, b > 0 ? std::fabs( a / b - 1.0 ) : 1.0 );
			}
		}
		std::cout << "    " << m.name << " spectral worst |film/conductor - 1| = " << worstNM << std::endl;
		Check( worstNM < 2e-3, std::string( "spectral vanishing film equals the per-wavelength conductor: " ) + m.name );
	}
}

} // namespace

int main()
{
	std::cout << "BlenderBridgeThinFilmDispersionTest" << std::endl;
	TestAbiVersionAndLayout();
	TestDielectricWithoutNewFieldsIsUnchanged();
	TestDispersionBuildsCauchyCurve();
	TestDispersionRejectsNonNumericIor();
	TestDielectricThinFilmIsArLayer();
	TestPbrWithoutFilmStaysPbr();
	TestPbrDielectricThinFilm();
	TestPbrConductorThinFilm();
	TestThinFilmMissingSubstrateFails();
	TestThinFilmComposesWithCoatAndEmission();
	TestLowAbbeInterpolationBound();
	TestNonPhysicalDispersionRefused();
	TestDispersiveDielectricWithEmission();
	TestThinFilmEmissionBakedWithoutWraps();
	TestThinFilmUnderSheen();
	TestThinFilmDielectricTintAndLiterals();
	TestVanishingFilmMetalKeepsColour();
	TestCyclesMetalEstimate();
	std::cout << ( g_checks - g_failures ) << " passed, " << g_failures << " failed" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
