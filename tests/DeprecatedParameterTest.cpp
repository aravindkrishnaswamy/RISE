//////////////////////////////////////////////////////////////////////
//
//  DeprecatedParameterTest.cpp - Legacy-deprecation Phase 1 deferred
//    (docs/LEGACY_DEPRECATION_ASSESSMENT.md §10.1): parameter-level
//    ParameterDescriptor::deprecated flag, wired like the chunk-level
//    one: one warning per load, read_schema reports it, and the agent
//    refuses to insert it. Applied to the frozen per-object
//    radiance_map parameter.
//
//  Author: Gemini
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Job.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/Agent/SchemaGen.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

using namespace RISE;
using namespace RISE::Cst;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); } }

static const ParameterDescriptor* FindParam( const ChunkDescriptor& d, const std::string& name )
{
	for( const auto& p : d.parameters ) {
		if( p.name == name ) return &p;
	}
	return nullptr;
}

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	return dir + name;
}

static Job* LoadScene( const char* text, const std::string& path )
{
	{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		std::remove( path.c_str() );
		return nullptr;
	}
	return pJob;
}

int main()
{
	std::printf( "=== DeprecatedParameterTest ===\n" );

	// 1. Descriptor check
	std::printf( "[descriptor] ParameterDescriptor::deprecated flag on per-object radiance_map\n" );
	{
		const ChunkDescriptor* so = RISE::DescriptorForKeyword( String( "standard_object" ) );
		Check( so != nullptr, "standard_object descriptor exists" );
		if( so ) {
			const ParameterDescriptor* p = FindParam( *so, "radiance_map" );
			Check( p != nullptr, "standard_object has radiance_map parameter" );
			Check( p && p->deprecated, "standard_object.radiance_map is marked deprecated" );
			Check( p && p->replacement.find( "radiance_map" ) != std::string::npos, "standard_object.radiance_map has replacement hint" );
			const ParameterDescriptor* mat = FindParam( *so, "material" );
			Check( mat && !mat->deprecated, "standard_object.material is NOT deprecated" );
		}

		const ChunkDescriptor* csg = RISE::DescriptorForKeyword( String( "csg_object" ) );
		Check( csg != nullptr, "csg_object descriptor exists" );
		if( csg ) {
			const ParameterDescriptor* p = FindParam( *csg, "radiance_map" );
			Check( p != nullptr, "csg_object has radiance_map parameter" );
			Check( p && p->deprecated, "csg_object.radiance_map is marked deprecated" );
			Check( p && p->replacement.find( "radiance_map" ) != std::string::npos, "csg_object.radiance_map has replacement hint" );
		}

		const ChunkDescriptor* r = RISE::DescriptorForKeyword( String( "pathtracing_pel_rasterizer" ) );
		Check( r != nullptr, "pathtracing_pel_rasterizer descriptor exists" );
		if( r ) {
			const ParameterDescriptor* p = FindParam( *r, "radiance_map" );
			Check( p != nullptr, "pathtracing_pel_rasterizer has radiance_map parameter" );
			Check( p && !p->deprecated, "rasterizer radiance_map is NOT deprecated" );
		}
	}

	// 2. Schema check (read_schema)
	std::printf( "[schema] read_schema reports deprecated parameter and replacement\n" );
	{
		const std::string soSchema = SchemaGenForChunk( "standard_object" );
		Check( soSchema.find( "\"radiance_map\":" ) != std::string::npos, "standard_object schema mentions radiance_map" );
		// Check that radiance_map has "deprecated":true within its parameter definition
		const size_t pos = soSchema.find( "\"radiance_map\":" );
		if( pos != std::string::npos ) {
			const size_t endPos = soSchema.find( '}', pos );
			const std::string paramDef = soSchema.substr( pos, endPos - pos + 1 );
			Check( paramDef.find( "\"deprecated\":true" ) != std::string::npos, "standard_object.radiance_map has \"deprecated\":true in schema" );
			Check( paramDef.find( "\"replacement\":" ) != std::string::npos, "standard_object.radiance_map has \"replacement\" in schema" );
		}

		const std::string csgSchema = SchemaGenForChunk( "csg_object" );
		const size_t csgPos = csgSchema.find( "\"radiance_map\":" );
		if( csgPos != std::string::npos ) {
			const size_t endPos = csgSchema.find( '}', csgPos );
			const std::string paramDef = csgSchema.substr( csgPos, endPos - csgPos + 1 );
			Check( paramDef.find( "\"deprecated\":true" ) != std::string::npos, "csg_object.radiance_map has \"deprecated\":true in schema" );
		}

		const std::string rSchema = SchemaGenForChunk( "pathtracing_pel_rasterizer" );
		const size_t rPos = rSchema.find( "\"radiance_map\":" );
		if( rPos != std::string::npos ) {
			const size_t endPos = rSchema.find( '}', rPos );
			const std::string paramDef = rSchema.substr( rPos, endPos - rPos + 1 );
			Check( paramDef.find( "\"deprecated\":true" ) == std::string::npos, "rasterizer radiance_map does NOT have \"deprecated\":true" );
		}
	}

	// 3. Agent refusal check
	std::printf( "[agent] Agent refuses to insert deprecated parameters\n" );
	{
		std::string outKind, outName, outParam;
		const std::string clause1 = CheckDeprecatedParamBanForInsert(
			"standard_object {\nname o1\nmaterial lb\nradiance_map pnt\n}\n",
			&outKind, &outName, &outParam );
		Check( !clause1.empty(), "CheckDeprecatedParamBanForInsert rejects standard_object with radiance_map" );
		Check( outKind == "standard_object", "outKind is standard_object" );
		Check( outName == "o1", "outName is o1" );
		Check( outParam == "radiance_map", "outParam is radiance_map" );
		Check( clause1.find( "radiance_map" ) != std::string::npos, "refusal clause mentions radiance_map" );

		const std::string clauseClean = CheckDeprecatedParamBanForInsert(
			"standard_object {\nname o2\nmaterial lb\n}\n",
			&outKind, &outName, &outParam );
		Check( clauseClean.empty(), "CheckDeprecatedParamBanForInsert accepts standard_object without deprecated params" );

		const std::string clausePatch = CheckDeprecatedParamBanForPatch(
			"standard_object {\nname o1\nmaterial lb\n}\n",
			"o1", "standard_object", "radiance_map", "pnt" );
		Check( !clausePatch.empty(), "CheckDeprecatedParamBanForPatch rejects patching radiance_map on standard_object" );
		Check( clausePatch.find( "radiance_map" ) != std::string::npos, "patch refusal clause mentions radiance_map" );

		const std::string clausePatchClean = CheckDeprecatedParamBanForPatch(
			"standard_object {\nname o1\nmaterial lb\n}\n",
			"o1", "standard_object", "material", "lb2" );
		Check( clausePatchClean.empty(), "CheckDeprecatedParamBanForPatch accepts patching valid parameter" );
	}

	// 4. AgentSession execution check
	std::printf( "[session] AgentSession::InsertChunk and ProposePatch reject deprecated parameters\n" );
	{
		const std::string tmp = TempPath( "deprecated_param_test.RISEscene" );
		const char* baseScene =
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\toidn_denoise false\n}\n"
			"film\n{\n\twidth 16\n\theight 16\n}\n"
			"pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n"
			"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\n\tname lb\n\treflectance pnt\n}\n"
			"sphere_geometry\n{\n\tname sg\n\tradius 1.0\n}\n"
			"standard_object\n{\n\tname o1\n\tgeometry sg\n\tmaterial lb\n}\n";

		Job* pJob = LoadScene( baseScene, tmp );
		Check( pJob != nullptr, "base scene fixture loaded cleanly" );
		if( pJob ) {
			AgentSession::SetBuildPlanGateDefaultEnabled( false );
			std::unique_ptr<AgentSession> sess = AgentSession::WrapJob( pJob );
			Check( sess != nullptr, "AgentSession wraps the Job" );

			// insert_chunk with deprecated param
			AgentChunkResult r = sess->InsertChunk( "standard_object\n{\n\tname bad_obj\n\tgeometry sg\n\tmaterial lb\n\tradiance_map pnt\n}\n" );
			Check( !r.applied, "InsertChunk with radiance_map is not applied" );
			Check( r.status == "rejected", "InsertChunk with radiance_map status is rejected" );
			Check( r.message.find( "radiance_map" ) != std::string::npos, "InsertChunk refusal message names radiance_map" );

			// insert_chunk clean
			AgentChunkResult rGood = sess->InsertChunk( "standard_object\n{\n\tname good_obj\n\tgeometry sg\n\tmaterial lb\n}\n" );
			Check( rGood.applied, "InsertChunk without deprecated param is applied" );

			// propose_patch with deprecated param
			AgentSetPatch patch;
			patch.target = "o1";
			patch.kind   = "standard_object";
			patch.param  = "radiance_map";
			patch.value  = "pnt";
			AgentPatchResult pr = sess->ProposePatch( patch );
			Check( !pr.applied, "ProposePatch with radiance_map is not applied" );
			Check( pr.status == "rejected", "ProposePatch with radiance_map status is rejected" );
			Check( pr.message.find( "radiance_map" ) != std::string::npos, "ProposePatch refusal message names radiance_map" );

			// propose_patch clean
			patch.param = "casts_shadows";
			patch.value = "FALSE";
			AgentPatchResult prGood = sess->ProposePatch( patch );
			Check( prGood.applied, "ProposePatch with casts_shadows is applied" );

			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	std::printf( "%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
