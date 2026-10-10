//////////////////////////////////////////////////////////////////////
//
//  DeprecatedMaterialWarningTest.cpp - DL-323 follow-through
//    (debt-deprecate, 2026-10-02): the legacy non-physically-based
//    material chunks are DEPRECATED (not retrofitted) -- they keep
//    parsing, deriving and rendering exactly as before, and a scene load
//    logs ONE warning per deprecated chunk TYPE naming the modern
//    replacement.
//
//  What it pins
//  ------------
//    [registry]  legacy-deprecation Phase 1 (2026-10-09): the FROZEN set
//                (legacy, unsupported -- the seven legacy BRDF materials
//                promoted, plus the shader-op pipeline, photon maps, MLT,
//                composite, ...) and the DEPRECATED-only set are exactly
//                kLegacyTierTable's; frozen implies deprecated; each
//                carries a non-empty replacement hint (the seven materials'
//                name a registered SUPPORTED material keyword); their
//                descriptions are prefixed so every consumer of
//                `description` shows the notice.  translucent, the SSS
//                materials, the research skin models, datadriven, bezier
//                patch, the delta lights and pathtracing_shaderop are NOT
//                flagged.
//    [warn]      DeriveToJob called with warnDeprecated=true (only
//                Job::LoadAsciiSceneViaCst does) logs exactly ONE eLog_Warning per
//                deprecated keyword per derive (two schlick chunks -> one
//                warning that says "2 chunk(s)"), none for modern chunks,
//                and the deprecation never becomes a derive diagnostic
//                (the load still succeeds with zero diagnostics).  A
//                second derive on a fresh Job warns again (per LOAD).
//    [schema]    the agent's read_schema JSON carries
//                `"deprecated":true` + `"replacement"` for a deprecated
//                chunk and neither for a modern one.
//    [identity]  the chunk still derives into a real material (the
//                Job's material manager holds it) -- deprecation changed
//                no behaviour.  The bit-identical-render proof is
//                DeprecatedMaterialRenderIdentityTest.
//
//  Author: Claude (Sonnet 5.5)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Agent/SchemaGen.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "CstRenderEquivalence.h"      // Job

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

using namespace RISE;
using namespace RISE::Cst;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); } }

//! Records (severity, text) of every message containing "DEPRECATED" or "UNSUPPORTED" (frozen).
class DeprecationLog : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	DeprecationLog() {}
	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( "DEPRECATED" ) != std::string::npos || msg.find( "UNSUPPORTED" ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMsgs.push_back( msg );
			mTypes.push_back( event.eType );
		}
	}
	void Flush() override {}
	void Reset() { std::lock_guard<std::mutex> lk( mMutex ); mMsgs.clear(); mTypes.clear(); }
	//! Number of captured messages mentioning `needle`.
	int Count( const std::string& needle ) const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		int n = 0;
		for( const std::string& m : mMsgs ) if( m.find( needle ) != std::string::npos ) ++n;
		return n;
	}
	size_t Total() const { std::lock_guard<std::mutex> lk( mMutex ); return mMsgs.size(); }
	//! First captured message mentioning `needle` ("" if none).
	std::string First( const std::string& needle ) const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		for( const std::string& m : mMsgs ) if( m.find( needle ) != std::string::npos ) return m;
		return std::string();
	}
	bool AllWarnings() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		if( mTypes.empty() ) return false;
		for( RISE::LOG_ENUM t : mTypes ) if( t != RISE::eLog_Warning ) return false;
		return true;
	}
protected:
	~DeprecationLog() override {}
private:
	mutable std::mutex mMutex;
	std::vector<std::string> mMsgs;
	std::vector<RISE::LOG_ENUM> mTypes;
};

static const std::string HDR = "RISE ASCII SCENE 7\n";

static std::string Scene( const std::string& body ) { return HDR +
	"uniformcolor_painter\n{\nname pnt\ncolor 0.5 0.5 0.5\n}\n"
	"uniformcolor_painter\n{\nname pnt_t\ncolor 0.3 0.3 0.3\n}\n" + body; }

int main()
{
	std::printf( "DeprecatedMaterialWarningTest (DL-323 follow-through + legacy Phase 1 tiers)\n" );

	//----------------------------------------------------------------------
	// [registry]
	//----------------------------------------------------------------------
	std::printf( "[registry] the frozen and deprecated tiers are exactly the Phase 1 sets\n" );
	const std::set<std::string> legacyMaterials = {
		"cooktorrance_material", "isotropic_phong_material", "ashikminshirley_anisotropicphong_material",
		"schlick_material", "ward_isotropic_material", "ward_anisotropic_material", "polished_material",
	};
	std::set<std::string> expectedFrozen = legacyMaterials;
	for( const char* kw : { "pixelpel_rasterizer", "pixelintegratingspectral_rasterizer",
	                        "distributiontracing_shaderop", "finalgather_shaderop", "directlighting_shaderop", "arealight_shaderop",
	                        "simple_sss_shaderop", "diffusion_approximation_sss_shaderop", "donner_jensen_skin_sss_shaderop",
	                        "ambientocclusion_shaderop", "alpha_test_shaderop", "transparency_shaderop",
	                        "caustic_pel_photonmap", "caustic_spectral_photonmap", "global_pel_photonmap", "global_spectral_photonmap",
	                        "translucent_pel_photonmap", "shadow_photonmap", "caustic_pel_gather", "caustic_spectral_gather",
	                        "global_pel_gather", "global_spectral_gather", "translucent_pel_gather", "shadow_gather",
	                        "irradiance_cache", "mlt_rasterizer", "mlt_spectral_rasterizer", "composite_material",
	                        "phong_luminaire_material", "directvolumerendering_shader", "spectraldirectvolumerendering_shader" } )
		expectedFrozen.insert( kw );
	const std::set<std::string> expectedDeprecatedOnly = {
		"ambient_light", "iridescent_painter", "onb_pinhole_camera", "3dsmesh_geometry", "sms_shaderop",
	};
	std::set<std::string> flaggedFrozen, flaggedDeprecatedOnly;
	for( const RISE::ChunkParserEntry& e : RISE::CreateAllChunkParsers() ) {
		const ChunkDescriptor& d = e.parser->Describe();
		if( d.frozen ) {
			flaggedFrozen.insert( e.keyword );
			Check( d.deprecated, e.keyword + ": frozen implies deprecated" );
			Check( d.description.compare( 0, 19, "LEGACY, UNSUPPORTED" ) == 0, e.keyword + ": frozen description is prefixed with the notice" );
		} else if( d.deprecated ) {
			flaggedDeprecatedOnly.insert( e.keyword );
			Check( d.description.compare( 0, 10, "DEPRECATED" ) == 0, e.keyword + ": description is prefixed with the notice" );
		} else {
			Check( d.replacement.empty(), e.keyword + ": a supported chunk carries no replacement hint" );
		}
		if( d.deprecated ) Check( !d.replacement.empty(), e.keyword + ": has a replacement hint" );
	}
	Check( flaggedFrozen == expectedFrozen, "the FROZEN set is exactly the Phase 1 frozen chunk types" );
	Check( flaggedDeprecatedOnly == expectedDeprecatedOnly, "the DEPRECATED-only set is exactly the Phase 1 deprecated chunk types" );
	Check( RISE::DeprecatedChunkAliasReplacement( "mis_pathtracing_shaderop" ) != nullptr, "the mis_pathtracing_shaderop alias is deprecated by dispatch keyword" );
	Check( RISE::DeprecatedChunkAliasReplacement( "pathtracing_shaderop" ) == nullptr, "pathtracing_shaderop (infrastructure) is not" );
	for( const std::string& kw : legacyMaterials ) {
		const ChunkDescriptor* d = DescriptorForKeyword( String( kw.c_str() ) );
		Check( d != nullptr, kw + ": registered" );
		if( !d ) continue;
		Check( d->category == ChunkCategory::Material, kw + ": is a Material-category chunk" );
		// The hint must name a registered, SUPPORTED Material keyword (a legacy chunk must
		// never point at another legacy one, and never at nothing).
		bool names = false;
		for( const String& m : AllKeywordsForCategory( ChunkCategory::Material ) ) {
			const ChunkDescriptor* md = DescriptorForKeyword( m );
			if( md && !md->deprecated && d->replacement.find( m.c_str() ) != std::string::npos ) { names = true; break; }
		}
		Check( names, kw + ": the replacement hint names a registered supported material keyword" );
	}
	// Owner rulings: these stay SUPPORTED.
	for( const char* kw : { "translucent_material", "orennayar_material", "sheen_material",
	                        "datadriven_material", "lambertian_material", "perfectreflector_material",
	                        "perfectrefractor_material", "dielectric_material", "ggx_material", "pbr_metallic_roughness_material",
	                        "coated_material", "fabric_material", "weave_material", "hair_material", "subsurfacescattering_material",
	                        "randomwalk_sss_material", "donner_jensen_skin_bssrdf_material", "biospec_skin_material",
	                        "generic_human_tissue_material", "lambertian_luminaire_material", "bezierpatch_geometry",
	                        "omni_light", "spot_light", "directional_light", "pathtracing_shaderop", "standard_shader",
	                        "advanced_shader", "pathtracing_pel_rasterizer", "bdpt_pel_rasterizer", "vcm_pel_rasterizer",
	                        "rawmesh_geometry" } ) {
		const ChunkDescriptor* d = DescriptorForKeyword( String( kw ) );
		Check( d && !d->deprecated && !d->frozen, std::string( kw ) + ": SUPPORTED (neither deprecated nor frozen)" );
	}

	//----------------------------------------------------------------------
	// [warn] one warning per deprecated type per derive; never a diagnostic
	//----------------------------------------------------------------------
	std::printf( "[warn] DeriveToJob logs ONE warning per deprecated chunk type per load\n" );
	DeprecationLog* log = new DeprecationLog();
	RISE::GlobalLogPriv()->AddPrinter( log );
	log->release();   // AddPrinter addref'd; the log keeps it alive for the process

	const std::string mixed = Scene(
		"schlick_material\n{\nname s1\nrd pnt\nrs pnt\nroughness 0.2\n}\n"
		"schlick_material\n{\nname s2\nrd pnt\nrs pnt\nroughness 0.3\n}\n"
		"cooktorrance_material\n{\nname ct\nrd pnt\nrs pnt\nfacets 0.2\n}\n"
		"polished_material\n{\nname pm\nreflectance pnt\ntau 0.9\nior 1.5\n}\n"
		"ggx_material\n{\nname gg\nrd pnt\nrs pnt\nalphax 0.2\nalphay 0.2\n}\n"
		"translucent_material\n{\nname tr\nref pnt\ntau pnt_t\n}\n"
		"lambertian_material\n{\nname lb\nreflectance pnt\n}\n"
		"ambient_light\n{\nname amb\npower 0.1\ncolor 1 1 1\n}\n"
		"mis_pathtracing_shaderop\n{\nname ptop\n}\n" );

	for( int load = 0; load < 2; ++load ) {
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		Document d = ParseToCst( mixed );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );
		const std::string tag = std::string( "load " ) + std::to_string( load + 1 ) + ": ";
		Check( n > 0 && diags.empty(), tag + "the scene loads cleanly with ZERO derive diagnostics (a deprecation is not a failure)" );
		Check( log->Count( "`schlick_material` is LEGACY and UNSUPPORTED" ) == 1, tag + "schlick_material (2 chunks) warns exactly once" );
		Check( log->First( "`schlick_material` is LEGACY and UNSUPPORTED" ).find( "2 chunk(s)" ) != std::string::npos, tag + "...and the warning counts both chunks" );
		Check( log->Count( "`cooktorrance_material` is LEGACY and UNSUPPORTED" ) == 1, tag + "cooktorrance_material warns exactly once" );
		Check( log->Count( "`polished_material` is LEGACY and UNSUPPORTED" ) == 1, tag + "polished_material warns exactly once" );
		Check( log->Count( "`ambient_light` is DEPRECATED" ) == 1, tag + "ambient_light (deprecated, not frozen) warns exactly once" );
		Check( log->Count( "`mis_pathtracing_shaderop` is DEPRECATED" ) == 1, tag + "the mis_pathtracing_shaderop alias warns by its own keyword" );
		Check( log->Count( "`pathtracing_shaderop` is" ) == 0, tag + "...and never as the supported pathtracing_shaderop" );
		Check( log->Total() == 5, tag + "exactly five legacy messages in total (no warning for ggx / translucent / lambertian)" );
		Check( log->AllWarnings(), tag + "every deprecation message is severity eLog_Warning" );
		Check( log->First( "`cooktorrance_material` is LEGACY and UNSUPPORTED" ).find( "ggx_material" ) != std::string::npos, tag + "the cooktorrance warning names ggx_material" );
		Check( log->First( "`polished_material` is LEGACY and UNSUPPORTED" ).find( "coated_material" ) != std::string::npos, tag + "the polished warning names coated_material" );
		// [identity] deprecation changed no behaviour: every chunk is a real, resolvable material.
		Check( j->GetMaterials()->GetItem( "s1" ) != nullptr && j->GetMaterials()->GetItem( "ct" ) != nullptr
			&& j->GetMaterials()->GetItem( "pm" ) != nullptr, tag + "the deprecated chunks still derive into real materials" );
		j->release();
	}

	// Only a REAL scene load warns.  A staging / dry-run / gate / re-derive (every other DeriveToJob caller:
	// the agent's throwaway-Job derives, Job::DeriveEditedCstDocument_, the full re-derives) passes the default
	// warnDeprecated = false and must stay silent, or the notice repeats on every agent edit.
	log->Reset();
	{
		Job* j = new Job();
		std::vector<std::string> diags;
		Document d = ParseToCst( mixed );
		const int n = DeriveToJob( d, *j, &diags );   // the default: a staging / dry-run derive
		Check( n > 0 && diags.empty(), "[staging] a default DeriveToJob of a deprecated-chunk scene derives cleanly" );
		Check( log->Total() == 0, "[staging] ...and logs NO deprecation message (warnDeprecated defaults off)" );
		j->release();
	}
	{
		// ...while the real load entry point still warns, once per type.
		const char* tmpBase = std::getenv( "TMPDIR" );
		std::string dir = tmpBase ? tmpBase : "/tmp";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
		const std::string path = dir + "deprecated_material_warning_" + std::to_string( (long)::getpid() ) + ".RISEscene";
		{ std::ofstream o( path.c_str(), std::ios::binary ); o << mixed; }
		Job* j = new Job();
		log->Reset();
		const bool ok = j->LoadAsciiSceneViaCst( path.c_str() );
		std::remove( path.c_str() );
		Check( ok, "[load] Job::LoadAsciiSceneViaCst of the deprecated-chunk scene succeeds" );
		Check( log->Count( "`schlick_material` is LEGACY and UNSUPPORTED" ) == 1 && log->Count( "`cooktorrance_material` is LEGACY and UNSUPPORTED" ) == 1
			&& log->Count( "`polished_material` is LEGACY and UNSUPPORTED" ) == 1 && log->Total() == 5,
			"[load] a real scene load warns exactly once per deprecated type" );
		// A later staging derive on the SAME loaded Job's document is still silent (the notice was given at load).
		log->Reset();
		Job* j2 = new Job();
		std::vector<std::string> diags2;
		Document d2 = ParseToCst( mixed );
		DeriveToJob( d2, *j2, &diags2 );
		Check( log->Total() == 0, "[load] a staging derive after the load stays silent" );
		j2->release();
		j->release();
	}

	log->Reset();
	{
		Job* j = new Job();
		std::vector<std::string> diags;
		Document d = ParseToCst( Scene(
			"ggx_material\n{\nname gg\nrd pnt\nrs pnt\nalphax 0.2\nalphay 0.2\n}\n"
			"translucent_material\n{\nname tr\nref pnt\ntau pnt_t\n}\n" ) );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, true );
		Check( n > 0 && diags.empty(), "a scene of modern + no-replacement chunks loads cleanly" );
		Check( log->Total() == 0, "...and logs no deprecation message at all" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [schema] the agent's read_schema surface
	//----------------------------------------------------------------------
	std::printf( "[schema] read_schema carries the deprecation\n" );
	{
		const std::string ct = RISE::Agent::SchemaGenForChunk( "cooktorrance_material" );
		Check( ct.find( "\"deprecated\":true" ) != std::string::npos, "cooktorrance_material schema has \"deprecated\":true" );
		Check( ct.find( "\"replacement\":\"ggx_material" ) != std::string::npos, "...and a replacement hint starting at ggx_material" );
		const std::string gg = RISE::Agent::SchemaGenForChunk( "ggx_material" );
		Check( gg.find( "\"deprecated\":true" ) == std::string::npos && gg.find( "\"replacement\"" ) == std::string::npos,
			"ggx_material schema carries neither key" );
		Check( ct.find( "\"frozen\":true" ) != std::string::npos, "cooktorrance_material (promoted) schema has \"frozen\":true" );
		const std::string pp = RISE::Agent::SchemaGenForChunk( "pixelpel_rasterizer" );
		Check( pp.find( "\"frozen\":true" ) != std::string::npos && pp.find( "\"deprecated\":true" ) != std::string::npos,
			"pixelpel_rasterizer schema is frozen (and deprecated)" );
		const std::string am = RISE::Agent::SchemaGenForChunk( "ambient_light" );
		Check( am.find( "\"deprecated\":true" ) != std::string::npos && am.find( "\"frozen\"" ) == std::string::npos,
			"ambient_light schema is deprecated but NOT frozen" );
		Check( gg.find( "\"frozen\"" ) == std::string::npos, "ggx_material schema is not frozen" );
		const std::string tr = RISE::Agent::SchemaGenForChunk( "translucent_material" );
		Check( tr.find( "\"deprecated\":true" ) == std::string::npos, "translucent_material (no replacement) schema is not flagged" );
	}

	std::printf( "%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
