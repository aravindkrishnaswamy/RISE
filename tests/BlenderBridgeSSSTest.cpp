//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeSSSTest.cpp - Contract test for the SUBSURFACE half
//    of the Blender native bridge (ABI v13): the new
//    `RISE_BLENDER_MATERIAL_RANDOMWALK_SSS` model and
//    `rise_blender_material`'s `subsurface_absorption` /
//    `subsurface_scattering` / `subsurface_ior` / `subsurface_g` /
//    `subsurface_roughness` fields, and the
//    `add_randomwalk_sss_material` function that consumes them, in
//    src/Blender/native/rise_blender_bridge.cpp.  Debt DL-186
//    (docs/DEBT_LEDGER.md); the mapping contract (including the
//    Radius/Scale -> sigma_a/sigma_s conversion, performed in
//    exporter.py, not here) is
//    docs/BLENDER_MATERIAL_TRANSLATION.md "Coat and Subsurface".
//
//  WHY THIS TEST INCLUDES A .cpp.  Same reasoning as
//  BlenderBridgeFabricTest.cpp's own banner.
//
//  RED-PROOF HISTORY: this file was first written against a
//  `rise_blender_bridge.h` / `.cpp` with no `RISE_BLENDER_MATERIAL_-
//  RANDOMWALK_SSS` enumerator, no `subsurface_*` fields, and no
//  `add_randomwalk_sss_material` function at all -- every reference
//  below failed to COMPILE.  Once the header/.cpp gained the model,
//  the five fields, and the function, this file compiles and the
//  checks below exercise the real behaviour.
//
//  WHAT THIS FILE PROBES.  `RandomWalkSSSMaterial` bakes its
//  absorption/scattering/ior/g PAINTERS down to a plain
//  `RandomWalkSSSParams` snapshot at construction time
//  (RandomWalkSSSMaterial.h), and `IMaterial::GetRandomWalkSSSParams()`
//  exposes that snapshot directly -- a far more precise probe than a
//  BRDF response comparison (the walk itself, not just the surface
//  Fresnel term, is what SSS actually models).  Every check below
//  reads that struct back rather than inferring the conversion from a
//  rendered response.
//
//  The six groups:
//
//    1. ABI VERSION 13 and the model/field additions (shared with
//       BlenderBridgeCoatTest.cpp's own group 1 for the field-order
//       half; this file only re-checks the model enumerator).
//    2. A random-walk SSS material registers as a REAL
//       RandomWalkSSSMaterial and its GetRandomWalkSSSParams() reflects
//       the ABI's raw fields exactly (absorption/scattering/ior/g
//       forwarded verbatim -- exporter.py did the physical conversion
//       before the fields ever reach this struct).
//    3. A chromatic (non-grey) subsurface_absorption/scattering triple
//       survives per-channel -- the money test for "the inline `r g b`
//       literal really becomes a per-channel IScalarPainter, not a
//       flattened scalar".
//    4. SSS + emission keep both.
//    5. Missing subsurface_absorption/subsurface_scattering is a
//       fatal failure (mirrors sheen/coat's required-slot convention).
//    6. A material with `model == RISE_BLENDER_MATERIAL_PBR_METALLIC_-
//       ROUGHNESS` (the ordinary case) is completely unaffected by the
//       existence of the RANDOMWALK_SSS branch -- consistency pin.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>

// The unit under test.  See the banner for why this is a .cpp include.
#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Materials/RandomWalkSSSMaterial.h"

using RISE::Implementation::LambertianLuminaireMaterial;
using RISE::Implementation::RandomWalkSSSMaterial;

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

//! A minimal, valid RANDOMWALK_SSS `rise_blender_material` fixture.
//! Every field not set here stays at its memset-zero default.
rise_blender_material SssFixture( const std::string& tag )
{
	static std::deque<std::string> nameStorage;
	auto keep = [&]( const std::string& s ) -> const char* {
		nameStorage.push_back( s );
		return nameStorage.back().c_str();
	};

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = keep( tag );
	mat.model = RISE_BLENDER_MATERIAL_RANDOMWALK_SSS;
	mat.double_sided = 1;
	mat.emissive_scale = 1.0;
	mat.subsurface_absorption = "0.02 0.05 0.10";
	mat.subsurface_scattering = "1.5 2.0 2.5";
	mat.subsurface_ior = 1.4;
	mat.subsurface_g = 0.0;
	mat.subsurface_roughness = 0.05;
	return mat;
}

const RandomWalkSSSMaterial* RandomWalkOf( RISE::IJobPriv& job, const char* name )
{
	RISE::IMaterial* material = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	return dynamic_cast<const RandomWalkSSSMaterial*>( material );
}

// ============================================================
//  1. The ABI's model enumerator
// ============================================================

void TestAbiModelEnumerator()
{
	std::cout << "Test: RISE_BLENDER_MATERIAL_RANDOMWALK_SSS is a distinct v13 model value" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 13, "RISE_BLENDER_API_VERSION is 13" );
	Check( RISE_BLENDER_MATERIAL_RANDOMWALK_SSS == 4,
		"RISE_BLENDER_MATERIAL_RANDOMWALK_SSS is appended after RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS (3)" );
	Check( RISE_BLENDER_MATERIAL_RANDOMWALK_SSS != RISE_BLENDER_MATERIAL_LAMBERT &&
	       RISE_BLENDER_MATERIAL_RANDOMWALK_SSS != RISE_BLENDER_MATERIAL_GGX &&
	       RISE_BLENDER_MATERIAL_RANDOMWALK_SSS != RISE_BLENDER_MATERIAL_DIELECTRIC &&
	       RISE_BLENDER_MATERIAL_RANDOMWALK_SSS != RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS,
		"the new model value collides with none of the pre-v13 ones" );
}

// ============================================================
//  2. A real RandomWalkSSSMaterial with the raw fields forwarded
// ============================================================

void TestRegistersRealRandomWalkSSSMaterial()
{
	std::cout << "Test: model == RANDOMWALK_SSS registers a real RandomWalkSSSMaterial with the ABI's raw values" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = SssFixture( "sss_basic" );

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "SSS material registered: " ) + err );

	const RandomWalkSSSMaterial* rw = RandomWalkOf( *job, "sss_basic" );
	Check( rw != 0, "MONEY: the final name IS a real RandomWalkSSSMaterial" );
	if( !rw ) return;

	const RISE::RandomWalkSSSParams* params = rw->GetRandomWalkSSSParams();
	Check( params != 0, "GetRandomWalkSSSParams() returns a real snapshot" );
	if( !params ) return;

	Check( std::fabs( params->ior - 1.4 ) < 1.0e-6, "ior forwarded verbatim from subsurface_ior" );
	Check( std::fabs( params->g - 0.0 ) < 1.0e-6, "g forwarded verbatim from subsurface_g" );
	Check( params->maxBounces == 64, "maxBounces is the bridge's fixed convention (64)" );

	Check( std::fabs( params->sigma_a.r - 0.02 ) < 1.0e-6 &&
	       std::fabs( params->sigma_a.g - 0.05 ) < 1.0e-6 &&
	       std::fabs( params->sigma_a.b - 0.10 ) < 1.0e-6,
		"sigma_a forwarded verbatim (per channel) from subsurface_absorption" );
	Check( std::fabs( params->sigma_s.r - 1.5 ) < 1.0e-6 &&
	       std::fabs( params->sigma_s.g - 2.0 ) < 1.0e-6 &&
	       std::fabs( params->sigma_s.b - 2.5 ) < 1.0e-6,
		"sigma_s forwarded verbatim (per channel) from subsurface_scattering" );
}

// ============================================================
//  3. A chromatic absorption/scattering triple survives per channel
//     (money test for the inline "r g b" literal parsing path)
// ============================================================

void TestChromaticCoefficientsSurvivePerChannel()
{
	std::cout << "Test: a strongly chromatic subsurface_absorption/scattering triple is NOT flattened to grey" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = SssFixture( "sss_chroma" );
	mat.subsurface_absorption = "0.01 0.50 2.00";	// deliberately far apart per channel
	mat.subsurface_scattering = "3.00 0.75 0.10";

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), "chromatic SSS material registered" );

	const RandomWalkSSSMaterial* rw = RandomWalkOf( *job, "sss_chroma" );
	Check( rw != 0, "the chromatic material IS a real RandomWalkSSSMaterial" );
	if( !rw ) return;
	const RISE::RandomWalkSSSParams* params = rw->GetRandomWalkSSSParams();
	Check( params != 0, "GetRandomWalkSSSParams() returns a real snapshot" );
	if( !params ) return;

	Check( params->sigma_a.r < params->sigma_a.g && params->sigma_a.g < params->sigma_a.b,
		"MONEY: sigma_a channels stay in strictly increasing order (R < G < B), matching the "
		"authored literal -- not flattened to a single (max/avg) scalar" );
	Check( params->sigma_s.r > params->sigma_s.g && params->sigma_s.g > params->sigma_s.b,
		"MONEY: sigma_s channels stay in strictly decreasing order (R > G > B), matching the "
		"authored literal" );
}

// ============================================================
//  4. SSS + emission keep BOTH
// ============================================================

void TestSSSWithEmissionKeepsBoth()
{
	std::cout << "Test: Emission Strength on a Subsurface material keeps BOTH the walk and the emitter" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double emitColor[3] = { 1.0, 0.8, 0.6 };
	(*job).AddUniformColorPainter( "emit_color_sss", emitColor, "Rec709RGB_Linear" );

	rise_blender_material mat = SssFixture( "sss_emit" );
	mat.emission_painter_name = "emit_color_sss";
	mat.emissive_scale = 3.0;

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "SSS+emission material registered: " ) + err );

	RISE::IMaterial* finalMat = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "sss_emit" ) : 0;
	Check( finalMat != 0, "the final material is registered" );
	if( finalMat ) {
		Check( finalMat->GetEmitter() != 0,
			"MONEY: the final material has a real emitter -- emission was NOT dropped" );
		Check( dynamic_cast<LambertianLuminaireMaterial*>( finalMat ) != 0,
			"the final material is a LambertianLuminaireMaterial (the outer emissive wrapper)" );
	}

	const RandomWalkSSSMaterial* sssBase = RandomWalkOf( *job, "sss_emit::sssbase" );
	Check( sssBase != 0, "the random-walk SSS layer is registered under the `::sssbase` intermediate name" );
	Check( sssBase != 0 && sssBase->GetEmitter() == 0,
		"the SSS layer itself carries NO emitter (RandomWalkSSSMaterial::GetEmitter() is always 0)" );
}

// ============================================================
//  5. Missing subsurface_absorption/subsurface_scattering is fatal
// ============================================================

void TestMissingCoefficientsIsFatal()
{
	std::cout << "Test: missing subsurface_absorption/subsurface_scattering fails add_material" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = SssFixture( "sss_missing" );
	mat.subsurface_absorption = 0;	// NULL -- not just empty

	char err[256] = { 0 };
	Check( !add_material( *job, mat, err, sizeof( err ) ), "a NULL subsurface_absorption fails add_material" );
	Check( err[0] != '\0', "a fatal failure names the reason in error_message" );
	Check( (*job).GetMaterials()->GetItem( "sss_missing" ) == 0,
		"nothing is left registered under the final name after the failure" );

	rise_blender_material mat2 = SssFixture( "sss_missing2" );
	mat2.subsurface_scattering = "";	// empty string

	char err2[256] = { 0 };
	Check( !add_material( *job, mat2, err2, sizeof( err2 ) ), "an empty subsurface_scattering fails add_material" );
}

// ============================================================
//  6. An ordinary PBR material is unaffected by the new model
// ============================================================

void TestPbrMaterialUnaffected()
{
	std::cout << "Test: an ordinary PBR_METALLIC_ROUGHNESS material is unaffected by RANDOMWALK_SSS's existence" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double baseColor[3] = { 0.5, 0.5, 0.5 };
	double metal[3]     = { 0.0, 0.0, 0.0 };
	double rough[3]     = { 0.4, 0.4, 0.4 };
	(*job).AddUniformColorPainter( "pbr_ctrl_base", baseColor, "Rec709RGB_Linear" );
	(*job).AddUniformColorPainter( "pbr_ctrl_metal", metal, "Rec709RGB_Linear" );
	(*job).AddUniformColorPainter( "pbr_ctrl_rough", rough, "Rec709RGB_Linear" );

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = "pbr_ctrl";
	mat.model = RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS;
	mat.double_sided = 1;
	mat.base_color_painter_name = "pbr_ctrl_base";
	mat.metallic_painter_name = "pbr_ctrl_metal";
	mat.roughness_painter_name = "pbr_ctrl_rough";
	mat.emissive_scale = 1.0;
	// subsurface_* stays at its memset-zero default (NULL / 0.0) --
	// irrelevant to a PBR_METALLIC_ROUGHNESS-model payload.

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), "ordinary PBR control material registered" );
	Check( RandomWalkOf( *job, "pbr_ctrl" ) == 0, "the PBR control is NOT a RandomWalkSSSMaterial" );

	RISE::IMaterial* m = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "pbr_ctrl" ) : 0;
	Check( m != 0 && m->GetBSDF() != 0, "the PBR control has a real BSDF" );
}

} // namespace

int main()
{
	std::cout << "=== Blender bridge subsurface test (DL-186) ===" << std::endl;

	TestAbiModelEnumerator();
	TestRegistersRealRandomWalkSSSMaterial();
	TestChromaticCoefficientsSurvivePerChannel();
	TestSSSWithEmissionKeepsBoth();
	TestMissingCoefficientsIsFatal();
	TestPbrMaterialUnaffected();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;

	if( g_failures == 0 ) {
		std::cout << "BlenderBridgeSSSTest: PASSED" << std::endl;
		return 0;
	}

	std::cout << "BlenderBridgeSSSTest: FAILED" << std::endl;
	return 1;
}
