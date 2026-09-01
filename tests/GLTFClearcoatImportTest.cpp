//////////////////////////////////////////////////////////////////////
//
//  GLTFClearcoatImportTest.cpp - docs/WETNESS_COAT_DESIGN.md sec 13
//    Phase 2 item 9 (2026-09-01) regression: KHR_materials_clearcoat
//    now imports onto `coated_material` instead of warn-and-skip.
//
//  Fixture: scenes/Tests/Geometry/assets/ClearcoatQuad.gltf, a small
//  hand-authored (not third-party Khronos-corpus) JSON glTF -- a single
//  two-triangle quad with an embedded data-URI buffer and ONE material
//  carrying `pbrMetallicRoughness` + `KHR_materials_clearcoat`
//  (clearcoatFactor 1.0, clearcoatRoughnessFactor 0.3).  Self-contained:
//  no external .bin, no RISE_MEDIA_PATH beyond the repo-root convention
//  every other GLTF test here already uses.
//
//  The imported material is a LIVE object built directly through the
//  Job:: API (Job::AddCoatedMaterial / Job::AddPBRMetallicRoughnessMaterial),
//  not a CST chunk the document text carries -- `gltf_import` stays a
//  single chunk in the scene text regardless of how many materials it
//  synthesizes underneath.  So this test reads the live material
//  manager (CoatedMaterialChunkTest.cpp's own idiom: `dynamic_cast` to
//  confirm the concrete C++ type, not string-matching), not
//  `AgentSession::ReadDocument()`.
//
//  What this proves, per the exit gate ("a clearcoat glTF scene
//  renders NON-BLACK"):
//    A  the imported material is a `CoatedMaterial`, not a bare GGX --
//       the layer actually landed, not a silent warn-and-skip;
//    B  its base is a `GGXMaterial` -- `pbr_metallic_roughness_material`
//       resolves to one at scene-build time, and that is exactly what
//       `CoatedMaterial::IsSupportedSubstrate` allowlists it for;
//    C  `coat_weight`/`coat_roughness` read back the authored
//       clearcoat_factor (1.0) and clearcoat_roughness_factor SQUARED
//       (0.3^2 = 0.09) -- the perceptual-roughness -> GGX-alpha
//       conversion item 9 is required to apply, not the raw 0.3;
//    D  the imported scene DERIVES and RENDERS NON-BLACK.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

//! P2-2 regression harness (2026-09-01 review round): captures log lines
//! containing `needle` -- ProceduralMeshTest.cpp's own `SkinLogCapture`
//! idiom ("the diagnostic IS the feature; the log is where it lands").
//! Never removed from the global log's printer list -- this is a short-
//! lived, one-test-per-process binary, the same convention that file's
//! captures use.
class NeedleLogCapture : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	explicit NeedleLogCapture( std::string needle ) : mNeedle( std::move( needle ) ) {}
	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( mNeedle ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMatches.push_back( msg );
		}
	}
	void Flush() override {}
	int MatchCount() const { std::lock_guard<std::mutex> lk( mMutex ); return (int)mMatches.size(); }
protected:
	~NeedleLogCapture() override {}
private:
	std::string                mNeedle;
	mutable std::mutex         mMutex;
	std::vector<std::string>   mMatches;
};

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
	return dir + name;
}

//! A fixed, otherwise-inert shading point -- `CoatedMaterialChunkTest.cpp`'s
//! own MakeProbe idiom, only the pieces `IScalarPainter::GetValuesAt`
//! actually reads matter for a UniformScalarPainter (the concrete type
//! Job::AddCoatedMaterial resolves an inline numeric literal into).
static RayIntersectionGeometric MakeProbe()
{
	Ray inRay( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) );
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

static void TestBasicClearcoatImport()
{
	std::printf( "-- basic clearcoat import (A/B/C/D) --\n" );

	const char* kFixture = "scenes/Tests/Geometry/assets/ClearcoatQuad.gltf";

	std::string body =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.3 0.4 1.0\n}\n\n"
		"gltf_import\n{\n\tfile " + std::string( kFixture ) + "\n\tname_prefix cc\n}\n\n";

	const std::string tmp = TempPath( "gltf_clearcoat.RISEscene" );
	{ std::ofstream o( tmp.c_str(), std::ios::binary ); o << body; }

	Job* pJob = new Job();
	const bool loaded = pJob->LoadAsciiSceneViaCst( tmp.c_str() );
	Check( loaded, "the clearcoat fixture scene parses and derives -- is "
	               "scenes/Tests/Geometry/assets/ClearcoatQuad.gltf committed, and is "
	               "the test running from the repo root?" );
	if( !loaded ) { pJob->release(); std::remove( tmp.c_str() ); return; }

	// GLTFSceneImporter::MaterialName( prefix, idx ) = "<prefix>.mat.<idx>".
	// One material in the fixture, prefix "cc" from `name_prefix cc` above.
	IMaterial* mat = pJob->GetMaterials()->GetItem( "cc.mat.0" );
	Check( mat != nullptr, "the imported material `cc.mat.0` is registered" );

	CoatedMaterial* coated = mat ? dynamic_cast<CoatedMaterial*>( mat ) : nullptr;
	Check( coated != nullptr,
	       "A MONEY: the imported clearcoat material is a live `CoatedMaterial` -- item 9's "
	       "re-target actually fired, not a silent warn-and-skip that leaves a bare GGX" );

	if( coated ) {
		GGXMaterial* base = dynamic_cast<GGXMaterial*>( const_cast<IMaterial*>( &coated->GetBase() ) );
		Check( base != nullptr,
		       "B: the coated material's `base` is a `GGXMaterial` -- "
		       "`pbr_metallic_roughness_material` resolves to one at scene-build time, and "
		       "that is exactly the substrate CoatedMaterial::IsSupportedSubstrate allowlists" );

		const RayIntersectionGeometric probe = MakeProbe();
		const double coatWeight = coated->GetCoatWeight().GetValuesAt( probe ).v[0];
		Check( std::fabs( coatWeight - 1.0 ) < 1e-6,
		       "C MONEY: coat_weight reads back the authored clearcoat_factor (1.0) -- got " +
		       std::to_string( coatWeight ) );

		const double coatRough = coated->GetCoatRoughness().GetValuesAt( probe ).v[0];
		Check( std::fabs( coatRough - 0.09 ) < 1e-6,
		       "C MONEY: coat_roughness reads back clearcoat_roughness_factor SQUARED "
		       "(0.3^2 = 0.09), the perceptual-roughness -> GGX-alpha conversion item 9 "
		       "requires -- NOT the raw 0.3 -- got " + std::to_string( coatRough ) );

		const double coatIor = coated->GetCoatIOR().GetValuesAt( probe ).v[0];
		Check( std::fabs( coatIor - 1.5 ) < 1e-6,
		       "C: coat_ior is fixed at 1.5 per the glTF KHR_materials_clearcoat spec "
		       "(it carries no IOR field of its own) -- got " + std::to_string( coatIor ) );
	}

	// ---- D: the imported scene derives and renders non-black.
	Check( pJob->GetScene() != nullptr, "D: the imported scene derives a live IScene" );
	{
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "D: the clearcoat scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0,
		       "D MONEY: the clearcoat glTF scene renders NON-BLACK (the exit-gate's own numeric claim)" );
		sess.reset();
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//! P2-2 regression (2026-09-01 review round): clearcoat combined with
//! `KHR_materials_unlit` on the SAME material registers as a
//! `LambertianLuminaireMaterial`, not a PBR/GGX base -- `coated_material`'s
//! substrate allowlist correctly refuses a luminaire (coating a light
//! source is not a modelled configuration), so the clearcoat layer must be
//! DROPPED for this combination.  Before the fix this drop was silent (the
//! diff that re-targeted clearcoat onto `coated_material` deleted the
//! previously-unconditional warn-and-skip without restoring it on this
//! branch); this test proves both halves: (a) the material still imports
//! cleanly as the unlit fallback, not a crash or a refused import, and
//! (b) the drop is SAID -- a warning naming the material and the
//! combination is actually logged, not silently swallowed.
static void TestClearcoatSkippedOnUnlit()
{
	std::printf( "-- P2-2: clearcoat + unlit warns and skips, not silent --\n" );

	const char* kFixture = "scenes/Tests/Geometry/assets/ClearcoatUnlitQuad.gltf";
	std::string body =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.3 0.4 1.0\n}\n\n"
		"gltf_import\n{\n\tfile " + std::string( kFixture ) + "\n\tname_prefix ccu\n}\n\n";

	const std::string tmp = TempPath( "gltf_clearcoat_unlit.RISEscene" );
	{ std::ofstream o( tmp.c_str(), std::ios::binary ); o << body; }

	NeedleLogCapture* pWarnLog = new NeedleLogCapture( "declares KHR_materials_clearcoat" );
	RISE::GlobalLogPriv()->AddPrinter( pWarnLog );

	Job* pJob = new Job();
	const bool loaded = pJob->LoadAsciiSceneViaCst( tmp.c_str() );
	Check( loaded, "the clearcoat+unlit fixture scene parses and derives -- is "
	               "scenes/Tests/Geometry/assets/ClearcoatUnlitQuad.gltf committed?" );
	if( loaded ) {
		IMaterial* mat = pJob->GetMaterials()->GetItem( "ccu.mat.0" );
		Check( mat != nullptr, "the imported material `ccu.mat.0` is registered despite the "
		                       "clearcoat+unlit combination" );
		if( mat ) {
			Check( dynamic_cast<CoatedMaterial*>( mat ) == nullptr,
			       "P2-2 MONEY: the material is NOT a CoatedMaterial -- clearcoat correctly did NOT "
			       "wrap the unlit luminaire fallback (coating a luminaire is not a modelled "
			       "configuration)" );
		}
		Check( pWarnLog->MatchCount() >= 1,
		       "P2-2 MONEY: the drop was SAID -- a warning naming the clearcoat+unlit combination "
		       "was actually logged, not silently swallowed" );
	}
	if( pJob ) pJob->release();
	std::remove( tmp.c_str() );
}

int main()
{
	std::printf( "=== GLTFClearcoatImportTest ===\n" );
	TestBasicClearcoatImport();
	TestClearcoatSkippedOnUnlit();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
