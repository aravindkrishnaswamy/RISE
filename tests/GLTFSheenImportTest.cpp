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
//      material: proves a `coated_material` now composes OVER the
//      `fabric_material` sheen result (DL-23, docs/DEBT_LEDGER.md,
//      closed 2026-09-14, lifted `coated_material`'s substrate-allowlist
//      refusal of a `FabricMaterial`), matching glTF's own base -> sheen
//      -> clearcoat layer order, instead of the old sheen-wins /
//      clearcoat-warn-and-skip behaviour.
//    scenes/Tests/Geometry/assets/SheenOnlyOfClearcoatSheenQuad.gltf --
//      a render-only CONTROL: byte-identical geometry/base-color/sheen
//      to ClearcoatSheenQuad.gltf with KHR_materials_clearcoat omitted,
//      isolating the coat lobe's own contribution in the A/B render
//      comparison below.
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
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
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

//! Clearcoat + sheen on the same material (DL-23, docs/DEBT_LEDGER.md,
//! closed 2026-09-14; importer wiring closed by this fix,
//! docs/IMPROVEMENTS.md "Clearcoat over `fabric_material`"):
//! `CoatedMaterial::IsSupportedSubstrate` now accepts a `FabricMaterial`,
//! so the importer builds a `coated_material` wrapping the sheen's
//! `fabric_material` result instead of warning-and-dropping the coat --
//! matching glTF's own base -> sheen -> clearcoat layer order.  This test
//! used to assert the OLD sheen-wins behaviour (a bare `FabricMaterial`,
//! clearcoat dropped with a logged warning); it now asserts the NEW one.
static void TestClearcoatSheenComposesCoatOverFabric()
{
	std::printf( "-- clearcoat + sheen: coated_material now wraps the fabric (sheen) result --\n" );

	// This exact log line no longer fires for this combination -- the
	// coat is composed, not dropped -- so absence of the OLD drop
	// message is itself part of the regression, not just a positive
	// assertion of the new structure.
	NeedleLogCapture* pDropWarnLog = new NeedleLogCapture( "so the clearcoat layer is skipped" );
	RISE::GlobalLogPriv()->AddPrinter( pDropWarnLog );

	Job* pJob = nullptr;
	const bool loaded = LoadFixture( pJob, "scenes/Tests/Geometry/assets/ClearcoatSheenQuad.gltf",
	                                  "ccs", "gltf_clearcoat_sheen.RISEscene" );
	Check( loaded, "the clearcoat+sheen fixture scene parses and derives -- is "
	               "scenes/Tests/Geometry/assets/ClearcoatSheenQuad.gltf committed?" );
	if( loaded ) {
		IMaterial* mat = pJob->GetMaterials()->GetItem( "ccs.mat.0" );
		Check( mat != nullptr, "the imported material `ccs.mat.0` is registered despite the "
		                       "clearcoat+sheen combination" );

		CoatedMaterial* coated = mat ? dynamic_cast<CoatedMaterial*>( mat ) : nullptr;
		Check( coated != nullptr,
		       "A MONEY: the FINAL material is a `CoatedMaterial` -- the clearcoat layer now "
		       "composes over the sheen result per glTF's base -> sheen -> clearcoat layering, "
		       "instead of being warned-and-dropped" );

		FabricMaterial* fabric = coated ? dynamic_cast<FabricMaterial*>( const_cast<IMaterial*>( &coated->GetBase() ) ) : nullptr;
		Check( fabric != nullptr,
		       "B MONEY: the coated material's `base` is a `FabricMaterial` -- the coat wraps "
		       "the sheen layer's OWN result, not the bare PBR base underneath it" );

		if( fabric ) {
			GGXMaterial* base = dynamic_cast<GGXMaterial*>( const_cast<IMaterial*>( &fabric->GetBase() ) );
			Check( base != nullptr,
			       "the fabric material's own base is the bare PBR GGXMaterial -- the full chain "
			       "is coat(fabric(ggx)), matching glTF's base -> sheen -> clearcoat order" );

			const RayIntersectionGeometric probe = MakeProbe();
			const RISEPel sheenColor = fabric->GetSheenColor().GetColor( probe );
			Check( std::fabs( sheenColor.r - 0.9 ) < 1e-3 &&
			       std::fabs( sheenColor.g - 0.6 ) < 1e-3 &&
			       std::fabs( sheenColor.b - 0.1 ) < 1e-3,
			       "the sheen layer still reads back the authored sheenColorFactor "
			       "(0.9, 0.6, 0.1) now that it is wrapped by the coat -- got (" +
			       std::to_string( sheenColor.r ) + ", " + std::to_string( sheenColor.g ) + ", " +
			       std::to_string( sheenColor.b ) + ")" );
		}

		if( coated ) {
			const RayIntersectionGeometric probe = MakeProbe();

			const double coatWeight = coated->GetCoatWeight().GetValuesAt( probe ).v[0];
			Check( std::fabs( coatWeight - 1.0 ) < 1e-6,
			       "C MONEY: coat_weight reads back the authored clearcoat_factor (1.0) over "
			       "the sheen result -- got " + std::to_string( coatWeight ) );

			const double coatRough = coated->GetCoatRoughness().GetValuesAt( probe ).v[0];
			Check( std::fabs( coatRough - 0.09 ) < 1e-6,
			       "C MONEY: coat_roughness reads back clearcoat_roughness_factor SQUARED "
			       "(0.3^2 = 0.09) -- the SAME mapping the clearcoat-only branch uses, shared "
			       "via BuildClearcoatWrap -- got " + std::to_string( coatRough ) );

			const double coatIor = coated->GetCoatIOR().GetValuesAt( probe ).v[0];
			Check( std::fabs( coatIor - 1.5 ) < 1e-6,
			       "C: coat_ior is fixed at 1.5 per the glTF KHR_materials_clearcoat spec, "
			       "unchanged by composing over a fabric substrate -- got " + std::to_string( coatIor ) );

			const RISEPel coatTint = coated->GetCoatTint().GetColor( probe );
			Check( std::fabs( coatTint.r - 1.0 ) < 1e-6 &&
			       std::fabs( coatTint.g - 1.0 ) < 1e-6 &&
			       std::fabs( coatTint.b - 1.0 ) < 1e-6,
			       "C: coat_tint is the untinted default (1,1,1) -- glTF's KHR_materials_clearcoat "
			       "has no tint concept -- got (" + std::to_string( coatTint.r ) + ", " +
			       std::to_string( coatTint.g ) + ", " + std::to_string( coatTint.b ) + ")" );
		}

		Check( pDropWarnLog->MatchCount() == 0,
		       "MONEY: the OLD 'clearcoat layer is skipped, keeping sheen' warning no longer "
		       "fires for this combination -- the coat is composed, not dropped" );
	}
	if( pJob ) pJob->release();

	// ---- D: a render of the combined material shows a specular coat
	// highlight a sheen-only control (byte-identical geometry/base-
	// color/sheen, no clearcoat extension) lacks.  The scene aligns the
	// directional light with the camera/surface-normal axis (`direction
	// 0 0 1`, quad facing +Z, camera at (0,0,4) looking down -Z) so the
	// flat quad's SPECULAR mirror direction points straight at the
	// camera everywhere on the surface -- the coat's own sharp GGX lobe
	// (alpha = coat_roughness = 0.09, versus the substrate's rougher
	// alpha = roughnessFactor^2 = 0.25) then adds a materially brighter,
	// tighter highlight on top of whatever the sheen-only control already
	// returns from the identical substrate.  n=3 renders per scene
	// (RISE's renders are NOT wall-clock seeded -- see the
	// rise-render-seeding memory note -- so three renders in one process
	// really do draw three independent Monte-Carlo estimates); mean and
	// sample standard deviation are printed and gated with margin.
	auto RenderMeanSD = [&]( const char* fixture, const char* prefix, const char* tmpName,
	                          double& outMean, double& outSD ) {
		std::vector<double> samples;
		for( int i = 0; i < 3; ++i ) {
			Job* pRJob = nullptr;
			char tmpNameI[128];
			std::snprintf( tmpNameI, sizeof( tmpNameI ), "%s_%d", tmpName, i );
			std::string body =
				"RISE ASCII SCENE 7\n"
				"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
				"pathtracing_pel_rasterizer\n{\n\tsamples 24\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
				"film\n{\n\twidth 24\n\theight 24\n}\n\n"
				"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
				"directional_light\n{\n\tname key\n\tpower 8.0\n\tcolor 1 1 1\n\tdirection 0 0 1\n}\n\n"
				"gltf_import\n{\n\tfile " + std::string( fixture ) + "\n\tname_prefix " + std::string( prefix ) + "\n}\n\n";
			const std::string tmp = TempPath( tmpNameI );
			{ std::ofstream o( tmp.c_str(), std::ios::binary ); o << body; }
			pRJob = new Job();
			const bool ldd = pRJob->LoadAsciiSceneViaCst( tmp.c_str() );
			std::remove( tmp.c_str() );
			Check( ldd, std::string( "render-comparison fixture `" ) + fixture + "` parses and derives" );
			if( ldd ) {
				std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pRJob );
				Agent::AgentRenderParams rp;
				rp.width = 24; rp.height = 24; rp.samples = 24;
				const Agent::AgentRenderResult rr = sess->Render( rp );
				Check( rr.ok, std::string( "render-comparison fixture `" ) + fixture + "` renders" );
				if( rr.ok ) samples.push_back( rr.meanR + rr.meanG + rr.meanB );
				sess.reset();
			} else {
				pRJob->release();
			}
		}
		outMean = 0.0;
		for( double s : samples ) outMean += s;
		outMean /= (double)samples.size();
		double var = 0.0;
		for( double s : samples ) var += ( s - outMean ) * ( s - outMean );
		outSD = samples.size() > 1 ? std::sqrt( var / (double)( samples.size() - 1 ) ) : 0.0;
	};

	double combinedMean = 0.0, combinedSD = 0.0;
	RenderMeanSD( "scenes/Tests/Geometry/assets/ClearcoatSheenQuad.gltf", "ccsr",
	              "gltf_ccs_render.RISEscene", combinedMean, combinedSD );
	double sheenOnlyMean = 0.0, sheenOnlySD = 0.0;
	RenderMeanSD( "scenes/Tests/Geometry/assets/SheenOnlyOfClearcoatSheenQuad.gltf", "shor",
	              "gltf_shonly_render.RISEscene", sheenOnlyMean, sheenOnlySD );

	std::printf( "   D: combined (coat+sheen) mean = %.6f (sd %.6f), sheen-only control mean = "
	             "%.6f (sd %.6f), n=3 each\n",
	             combinedMean, combinedSD, sheenOnlyMean, sheenOnlySD );

	Check( combinedMean > sheenOnlyMean + 2.0 * ( combinedSD + sheenOnlySD ),
	       "D MONEY: the combined coat+sheen render is measurably BRIGHTER than the byte-"
	       "identical sheen-only control under camera-aligned lighting (a coat specular "
	       "highlight the sheen-only import lacks), by more than 2 combined standard "
	       "deviations -- combined " + std::to_string( combinedMean ) + " (sd " +
	       std::to_string( combinedSD ) + ") vs sheen-only " + std::to_string( sheenOnlyMean ) +
	       " (sd " + std::to_string( sheenOnlySD ) + ")" );
}

//! P1 fix (post-DL-18-review sibling, 2026-09-17): KHR_materials_sheen
//! combined with a non-black emissiveFactor on the SAME material used
//! to fail `AddFabricMaterial` outright ("the substrate is a
//! luminaire"), because the PBR base built for the fabric wrap baked
//! `emissivePainter` in unconditionally.  This proves the fix keeps
//! BOTH: the final material carries a real emitter (via
//! `AddLambertianLuminaireMaterial`) wrapping a genuine `FabricMaterial`
//! sheen layer, whose own PBR base carries no emitter.
static void TestSheenWithEmissionCombines()
{
	std::printf( "-- sheen + emission on the same material: keep BOTH (P1 fix) --\n" );

	Job* pJob = nullptr;
	const bool loaded = LoadFixture( pJob, "scenes/Tests/Geometry/assets/SheenEmissiveQuad.gltf",
	                                  "she", "gltf_sheen_emissive.RISEscene" );
	Check( loaded, "the sheen+emissive fixture scene parses and derives -- is "
	               "scenes/Tests/Geometry/assets/SheenEmissiveQuad.gltf committed?" );
	if( !loaded ) { if( pJob ) pJob->release(); return; }

	// GLTFSceneImporter::MaterialName( prefix, idx ) = "<prefix>.mat.<idx>".
	IMaterial* mat = pJob->GetMaterials()->GetItem( "she.mat.0" );
	Check( mat != nullptr, "the imported material `she.mat.0` is registered "
	                       "(pre-fix, add_material failed outright for this combination)" );

	if( mat ) {
		Check( mat->GetEmitter() != nullptr,
		       "MONEY: the final material has a real emitter -- emission was NOT dropped" );

		LambertianLuminaireMaterial* lum = dynamic_cast<LambertianLuminaireMaterial*>( mat );
		Check( lum != nullptr,
		       "MONEY: the final material is a `LambertianLuminaireMaterial` -- the outer "
		       "emissive wrapper the fix re-attaches once the fabric wrap is a legal substrate" );

		Check( dynamic_cast<FabricMaterial*>( mat ) == nullptr,
		       "the final material is NOT itself a FabricMaterial -- that lives one layer down" );
	}

	FabricMaterial* fabric = pJob->GetMaterials()->GetItem( "she.mat.0__sheen_emit_base" )
		? dynamic_cast<FabricMaterial*>( pJob->GetMaterials()->GetItem( "she.mat.0__sheen_emit_base" ) )
		: nullptr;
	Check( fabric != nullptr,
	       "MONEY: the fabric (sheen) layer is registered under the `__sheen_emit_base` "
	       "intermediate name and IS a live FabricMaterial -- the sheen lobe really landed" );
	if( fabric ) {
		Check( fabric->GetEmitter() == nullptr,
		       "the fabric (sheen) layer itself carries NO emitter -- a legal fabric_material "
		       "result, not itself the luminaire" );

		GGXMaterial* base = dynamic_cast<GGXMaterial*>( const_cast<IMaterial*>( &fabric->GetBase() ) );
		Check( base != nullptr, "the fabric material's base is a GGXMaterial" );
		Check( base != nullptr && base->GetEmitter() == nullptr,
		       "MONEY: the PBR base does NOT carry the emitter -- it stays a legal "
		       "fabric_material substrate (FabricMaterial::IsSupportedSubstrate refuses "
		       "GetEmitter() != 0)" );

		const RayIntersectionGeometric probe = MakeProbe();
		const RISEPel sheenColor = fabric->GetSheenColor().GetColor( probe );
		Check( std::fabs( sheenColor.r - 0.9 ) < 1e-3 &&
		       std::fabs( sheenColor.g - 0.6 ) < 1e-3 &&
		       std::fabs( sheenColor.b - 0.1 ) < 1e-3,
		       "the sheen layer still reads back the authored sheenColorFactor (0.9, 0.6, 0.1) "
		       "with emission also present" );
	}

	Check( pJob->GetScene() != nullptr, "the imported scene derives a live IScene" );
	{
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "the sheen+emissive scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0,
		       "the sheen+emissive glTF scene renders NON-BLACK" );
		sess.reset();
	}

	pJob->release();
}

int main()
{
	std::printf( "=== GLTFSheenImportTest ===\n" );
	TestBasicSheenImport();
	TestSheenRoughnessTextureAlphaRouting();
	TestClearcoatSheenComposesCoatOverFabric();
	TestSheenWithEmissionCombines();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
