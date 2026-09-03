//////////////////////////////////////////////////////////////////////
//
//  GLTFSheenImportTest.cpp - docs/CLOTH_FABRIC_DESIGN.md §7(B) / §14
//    regression: KHR_materials_sheen now imports onto `fabric_material`
//    instead of warn-and-skip, mirroring GLTFClearcoatImportTest.cpp's
//    `KHR_materials_clearcoat` -> `coated_material` coverage.
//
//  Fixtures (hand-authored, not third-party Khronos-corpus):
//    scenes/Tests/Geometry/assets/SheenQuad.gltf -- pbrMetallicRoughness
//      + KHR_materials_sheen (sheenColorFactor [0.9,0.6,0.1],
//      sheenRoughnessFactor 0.3), no textures.
//    scenes/Tests/Geometry/assets/SheenRoughnessTextureQuad.gltf --
//      sheenColorFactor [1,1,1] (white, no colour-factor scaling to
//      confound the readback) + sheenRoughnessFactor 1.0 +
//      sheenRoughnessTexture pointing at SheenRoughnessAlpha.png (a
//      2x2 RGBA PNG, alpha = 128 everywhere) -- exercises the ALPHA-
//      channel routing (`sheenRoughness = sheenRoughnessFactor *
//      texture.a`, per the KHR_materials_sheen spec).
//    scenes/Tests/Geometry/assets/ClearcoatSheenQuad.gltf -- BOTH
//      KHR_materials_clearcoat and KHR_materials_sheen on the same
//      material: proves sheen wins the wrap (clearcoat cannot compose
//      over a fabric_material result -- `coated_material`'s substrate
//      allowlist is the same three scattering classes FabricMaterial's
//      own is, and FabricMaterial is none of those) and that the drop
//      is SAID, not silent.
//
//  The imported material is a LIVE object built directly through the
//  Job:: API (Job::AddFabricMaterial / Job::AddPBRMetallicRoughnessMaterial),
//  read back via `dynamic_cast` to the concrete C++ type -- the same
//  idiom GLTFClearcoatImportTest.cpp / CoatedMaterialChunkTest.cpp use.
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
#include "../src/Library/Materials/FabricMaterial.h"
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

//! GLTFClearcoatImportTest.cpp's own idiom: capture log lines containing
//! `needle` -- "the diagnostic IS the feature; the log is where it lands".
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

//! GLTFClearcoatImportTest.cpp's own MakeProbe idiom -- a fixed, otherwise-
//! inert shading point; only the pieces `IScalarPainter`/`IPainter`
//! `GetValuesAt`/`GetColor` actually read for a uniform/constant painter
//! matter here.
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

static bool LoadFixture( Job*& pJob, const char* fixture, const char* prefix, const char* tmpName )
{
	std::string body =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.3 0.4 1.0\n}\n\n"
		"gltf_import\n{\n\tfile " + std::string( fixture ) + "\n\tname_prefix " + std::string( prefix ) + "\n}\n\n";

	const std::string tmp = TempPath( tmpName );
	{ std::ofstream o( tmp.c_str(), std::ios::binary ); o << body; }

	pJob = new Job();
	const bool loaded = pJob->LoadAsciiSceneViaCst( tmp.c_str() );
	std::remove( tmp.c_str() );
	return loaded;
}

//! Basic sheen import: sheenColorFactor + sheenRoughnessFactor, no textures.
static void TestBasicSheenImport()
{
	std::printf( "-- basic sheen import (fabric_material, sheen_color, sheen_roughness) --\n" );

	Job* pJob = nullptr;
	const bool loaded = LoadFixture( pJob, "scenes/Tests/Geometry/assets/SheenQuad.gltf",
	                                  "sh", "gltf_sheen_basic.RISEscene" );
	Check( loaded, "the sheen fixture scene parses and derives -- is "
	               "scenes/Tests/Geometry/assets/SheenQuad.gltf committed, and is the test "
	               "running from the repo root?" );
	if( !loaded ) { if( pJob ) pJob->release(); return; }

	// GLTFSceneImporter::MaterialName( prefix, idx ) = "<prefix>.mat.<idx>".
	IMaterial* mat = pJob->GetMaterials()->GetItem( "sh.mat.0" );
	Check( mat != nullptr, "the imported material `sh.mat.0` is registered" );

	FabricMaterial* fabric = mat ? dynamic_cast<FabricMaterial*>( mat ) : nullptr;
	Check( fabric != nullptr,
	       "MONEY: the imported sheen material is a live `FabricMaterial` -- the layer "
	       "actually landed, not a silent warn-and-skip" );

	if( fabric ) {
		GGXMaterial* base = dynamic_cast<GGXMaterial*>( const_cast<IMaterial*>( &fabric->GetBase() ) );
		Check( base != nullptr,
		       "the fabric material's `base` is a `GGXMaterial` -- "
		       "`pbr_metallic_roughness_material` resolves to one at scene-build time, and "
		       "that is exactly the substrate FabricMaterial::IsSupportedSubstrate allowlists" );

		const RayIntersectionGeometric probe = MakeProbe();

		const RISEPel sheenColor = fabric->GetSheenColor().GetColor( probe );
		Check( std::fabs( sheenColor.r - 0.9 ) < 1e-3 &&
		       std::fabs( sheenColor.g - 0.6 ) < 1e-3 &&
		       std::fabs( sheenColor.b - 0.1 ) < 1e-3,
		       "MONEY: sheen_color reads back the authored sheenColorFactor (0.9, 0.6, 0.1) -- got (" +
		       std::to_string( sheenColor.r ) + ", " + std::to_string( sheenColor.g ) + ", " +
		       std::to_string( sheenColor.b ) + ")" );

		const double sheenRough = fabric->GetSheenRoughness().GetValuesAt( probe ).v[0];
		Check( std::fabs( sheenRough - 0.3 ) < 1e-6,
		       "MONEY: sheen_roughness reads back the authored sheenRoughnessFactor (0.3) -- got " +
		       std::to_string( sheenRough ) );
	}

	Check( pJob->GetScene() != nullptr, "the imported scene derives a live IScene" );
	{
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "the sheen scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0,
		       "MONEY: the sheen glTF scene renders NON-BLACK" );
		sess.reset();
	}

	pJob->release();
}

//! sheenRoughnessTexture's ALPHA channel: sheenRoughness = factor * texture.a.
static void TestSheenRoughnessTextureAlphaRouting()
{
	std::printf( "-- sheenRoughnessTexture ALPHA-channel routing --\n" );

	Job* pJob = nullptr;
	const bool loaded = LoadFixture( pJob, "scenes/Tests/Geometry/assets/SheenRoughnessTextureQuad.gltf",
	                                  "shr", "gltf_sheen_rough_tex.RISEscene" );
	Check( loaded, "the sheen-roughness-texture fixture scene parses and derives -- is "
	               "scenes/Tests/Geometry/assets/SheenRoughnessTextureQuad.gltf (and its "
	               "SheenRoughnessAlpha.png sidecar) committed?" );
	if( !loaded ) { if( pJob ) pJob->release(); return; }

	IMaterial* mat = pJob->GetMaterials()->GetItem( "shr.mat.0" );
	Check( mat != nullptr, "the imported material `shr.mat.0` is registered" );

	FabricMaterial* fabric = mat ? dynamic_cast<FabricMaterial*>( mat ) : nullptr;
	Check( fabric != nullptr, "the imported material is a live `FabricMaterial`" );

	if( fabric ) {
		const RayIntersectionGeometric probe = MakeProbe();
		const double sheenRough = fabric->GetSheenRoughness().GetValuesAt( probe ).v[0];
		// SheenRoughnessAlpha.png is alpha=128 everywhere (128/255 =
		// 0.501960...); sheenRoughnessFactor is 1.0, so the expected
		// value is the bare alpha fraction.  Tolerance is generous
		// (1e-2) to absorb the texture-sampler's bilinear/point-filter
		// footprint and 8-bit quantization, while still discriminating
		// "the alpha channel was actually read" (~0.502) from "the RGB
		// channel was read instead" (RGB is fully opaque white, 1.0) or
		// "the texture was ignored and the factor alone was used" (1.0).
		const double expected = 128.0 / 255.0;
		Check( std::fabs( sheenRough - expected ) < 1e-2,
		       "MONEY: sheen_roughness reads the sheenRoughnessTexture's ALPHA channel "
		       "(128/255 = " + std::to_string( expected ) + ") scaled by sheenRoughnessFactor "
		       "(1.0) -- got " + std::to_string( sheenRough ) + " (RGB-channel or factor-only "
		       "fallback would read ~1.0)" );
	}

	Check( pJob->GetScene() != nullptr, "the imported scene derives a live IScene" );
	{
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "the sheen-roughness-texture scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0,
		       "the sheen-roughness-texture glTF scene renders NON-BLACK" );
		sess.reset();
	}

	pJob->release();
}

//! Clearcoat + sheen on the same material: sheen wins the wrap (final
//! `matName` is a FabricMaterial, not a CoatedMaterial), and the clearcoat
//! drop is SAID via a warning naming both extensions.
static void TestClearcoatSheenOrdering()
{
	std::printf( "-- clearcoat + sheen: sheen wraps, clearcoat warn-and-skips --\n" );

	NeedleLogCapture* pWarnLog = new NeedleLogCapture( "declares KHR_materials_clearcoat" );
	RISE::GlobalLogPriv()->AddPrinter( pWarnLog );

	Job* pJob = nullptr;
	const bool loaded = LoadFixture( pJob, "scenes/Tests/Geometry/assets/ClearcoatSheenQuad.gltf",
	                                  "ccs", "gltf_clearcoat_sheen.RISEscene" );
	Check( loaded, "the clearcoat+sheen fixture scene parses and derives -- is "
	               "scenes/Tests/Geometry/assets/ClearcoatSheenQuad.gltf committed?" );
	if( loaded ) {
		IMaterial* mat = pJob->GetMaterials()->GetItem( "ccs.mat.0" );
		Check( mat != nullptr, "the imported material `ccs.mat.0` is registered despite the "
		                       "clearcoat+sheen combination" );

		FabricMaterial* fabric = mat ? dynamic_cast<FabricMaterial*>( mat ) : nullptr;
		Check( fabric != nullptr,
		       "MONEY: the FINAL material is a `FabricMaterial` -- sheen wins the wrap per "
		       "glTF's base -> sheen -> clearcoat layering, since coated_material cannot "
		       "compose over a fabric_material result" );

		if( mat ) {
			Check( dynamic_cast<CoatedMaterial*>( mat ) == nullptr,
			       "the final material is NOT a CoatedMaterial -- clearcoat did not wrap "
			       "(or get wrapped by) anything here" );
		}

		if( fabric ) {
			GGXMaterial* base = dynamic_cast<GGXMaterial*>( const_cast<IMaterial*>( &fabric->GetBase() ) );
			Check( base != nullptr,
			       "the fabric material's base is the bare PBR GGXMaterial (clearcoat did "
			       "NOT insert itself between the PBR base and the sheen layer)" );
		}

		Check( pWarnLog->MatchCount() >= 1,
		       "MONEY: the clearcoat drop was SAID -- a warning naming the clearcoat+sheen "
		       "combination was actually logged, not silently swallowed" );
	}
	if( pJob ) pJob->release();
}

int main()
{
	std::printf( "=== GLTFSheenImportTest ===\n" );
	TestBasicSheenImport();
	TestSheenRoughnessTextureAlphaRouting();
	TestClearcoatSheenOrdering();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
