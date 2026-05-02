//////////////////////////////////////////////////////////////////////
//
//  RasterizerIntrospection.cpp - Descriptor-driven rasterizer
//    introspection.  The list of editable rows is sourced from the
//    chunk descriptor that the parser uses to LOAD the rasterizer
//    (`bdpt_pel_rasterizer`, `vcm_spectral_rasterizer`, etc.) — same
//    single source of truth `CameraIntrospection` and
//    `LightIntrospection` use.  Adding a parameter to the parser's
//    `Describe()` automatically surfaces it in the panel.
//
//    Read-back consults the per-rasterizer `Job::RasterizerParams`
//    snapshot via `IJob::GetRasterizerParameter`.  Write-back goes
//    through `IJob::SetRasterizerParameter`, which mutates the
//    snapshot and re-instantiates the rasterizer from the modified
//    snapshot — preserving every other parameter the scene file set.
//
//    Params the snapshot doesn't yet cover (the scene-config
//    structs — radiance_map / pixel_filter / SMS / path_guiding /
//    adaptive / stability / progressive) are surfaced read-only:
//    descriptor metadata still drives the row, but the value column
//    reads as empty until the snapshot grows to cover them.  The
//    snapshot covers the most-used scalar / bool / vec params today;
//    expanding it is purely additive.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RasterizerIntrospection.h"
#include "ChunkDescriptorRegistry.h"
#include "../Job.h"
#include "../Parsers/ChunkDescriptor.h"

#include <cstdio>
#include <string>

using namespace RISE;

namespace {

CameraProperty MakeReadOnlyRow( const char* name, const String& value, const char* description )
{
	CameraProperty p;
	p.name        = String( name );
	p.value       = value;
	p.description = String( description );
	p.kind        = ValueKind::String;
	p.editable    = false;
	return p;
}

}  // namespace

std::vector<CameraProperty> RasterizerIntrospection::Inspect( const IJob& job, const String& typeName )
{
	std::vector<CameraProperty> rows;

	rows.push_back( MakeReadOnlyRow(
		"Type", typeName,
		"The rasterizer chunk-name registered with the job (matches the chunk keyword in the .RISEscene file)." ) );

	{
		const std::string active = job.GetActiveRasterizerName();
		const bool isActive = ( active == std::string( typeName.c_str() ) );
		rows.push_back( MakeReadOnlyRow(
			"Active", String( isActive ? "yes" : "no" ),
			"Whether this rasterizer is the one the next Render click will use.  Click any other entry in the Rasterizer list to swap." ) );
	}

	const ChunkDescriptor* desc = DescriptorForKeyword( typeName );
	if( !desc ) {
		rows.push_back( MakeReadOnlyRow(
			"Status", String( "(no descriptor)" ),
			"Rasterizer chunk keyword is not registered with the parser.  Out-of-tree rasterizer types fall back to a minimal row set." ) );
		return rows;
	}

	// Iterate descriptor parameters in declaration order — the natural
	// display order, matching what users see in their scene files.
	for( const ParameterDescriptor& p : desc->parameters ) {
		// `defaultshader` is filtered: it's a Reference param that
		// would need a presets dropdown of registered shaders, plus
		// rebuild-with-new-shader plumbing (the rasterizer-rebuild
		// path uses `params.shader` — settable but the panel-row UX
		// for it deserves its own Phase 5 polish).  For now surface
		// it as a separate read-only "Default Shader" footer below.
		if( p.name == "defaultshader" ) continue;

		CameraProperty cp;
		cp.name        = String( p.name.c_str() );
		cp.kind        = p.kind;
		cp.description = String( p.description.c_str() );
		cp.presets     = p.presets;
		cp.unitLabel   = String( p.unitLabel.c_str() );

		// Read the current value.  Job::GetRasterizerParameter returns
		// "" for params the snapshot doesn't yet cover (config-struct
		// nested params).  Surface those read-only with an explicit
		// "(scene-file only)" rather than the descriptor's default
		// hint — using the default would be a silent factual lie
		// when the scene file actually set a non-default value the
		// snapshot didn't capture.  The default-hint is mentioned in
		// the description so the user can still see what the parser's
		// fallback would be.
		const std::string current = job.GetRasterizerParameter( typeName.c_str(), p.name.c_str() );
		if( !current.empty() ) {
			cp.value    = String( current.c_str() );
			cp.editable = true;
		} else {
			cp.value    = String( "(scene-file only)" );
			cp.editable = false;
			if( !p.defaultValueHint.empty() ) {
				std::string desc = p.description;
				desc += "  (Parser default: ";
				desc += p.defaultValueHint;
				desc += ")";
				cp.description = String( desc.c_str() );
			}
		}
		rows.push_back( cp );
	}

	// Read-only footer: the rasterizer's default shader.  Pulled out
	// of the descriptor loop above so the user can see the shader at
	// a glance without it pretending to be runtime-editable.
	{
		const Job* concreteJob = dynamic_cast<const Job*>( &job );
		const Job::RasterizerParams* params = concreteJob
			? concreteJob->GetRasterizerParams( std::string( typeName.c_str() ) )
			: 0;
		if( params ) {
			rows.push_back( MakeReadOnlyRow(
				"Default Shader", String( params->shader.c_str() ),
				"The shader bound at scene-load time.  Phase 5 will surface this as a presets dropdown so it can be re-bound without re-instantiating the rasterizer." ) );
		}
	}

	return rows;
}
