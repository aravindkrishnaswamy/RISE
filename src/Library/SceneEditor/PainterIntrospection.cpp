//////////////////////////////////////////////////////////////////////
//
//  PainterIntrospection.cpp - See PainterIntrospection.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "PainterIntrospection.h"
#include "ChunkDescriptorRegistry.h"

namespace RISE
{

namespace
{
	// Mirrors SceneEditController.cpp's anonymous-namespace
	// AgentReadFirstParamValue -- kept as an intentional small
	// duplicate rather than hoisting a Cst-kernel-wide shared helper
	// for one caller; see the doc comment on
	// SceneEditController::CaptureAgentPriorParamValue_ for the
	// original.  Reads the FIRST occurrence of `pname` on `chunk`.
	std::string ReadFirstParamValue( const RISE::Cst::NodeRef& chunk, const char* pname, bool* outPresent )
	{
		if( outPresent ) *outPresent = false;
		if( !chunk ) return std::string();
		for( const auto& kid : chunk->kids )
		{
			if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
			std::string nm, val;
			bool inVal = false;
			for( const auto& tk : kid->kids )
			{
				if( !tk ) continue;
				if( !inVal && tk->kind == RISE::Cst::NodeKind::Token && tk->role == "pname" && nm.empty() ) { nm = tk->text; continue; }
				if( !inVal && tk->kind == RISE::Cst::NodeKind::Token && tk->role == "pvalue" ) inVal = true;
				if( inVal ) val += tk->text;
			}
			if( nm == pname )
			{
				if( outPresent ) *outPresent = true;
				return val;
			}
		}
		return std::string();
	}
}   // anonymous namespace

std::vector<CameraProperty> PainterIntrospection::Inspect(
	const RISE::Cst::Document* doc, const String& painterName )
{
	std::vector<CameraProperty> out;
	if( !doc || painterName.size() <= 1 ) return out;

	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, painterName.c_str(), nullptr, "painter", /*uniqueFallback=*/false );
	if( id == 0 ) return out;
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return out;

	// Defensive kind check: DocFindByNameAnyRole's roleKindSuffix only
	// narrows when the bare name is AMBIGUOUS (>1 chunk shares it); a
	// single cross-category match resolves regardless of kind.  Painter
	// names come from the painter managers, but a name can exist in a
	// manager without a matching CST painter chunk (dual-registration /
	// programmatic painters) while some OTHER category's chunk carries
	// the same name -- refuse rather than introspect the wrong chunk.
	{
		const std::string& role = chunk->role;
		const std::string suffix = "_painter";
		const bool isPainterChunk = ( role == "painter" ) ||
			( role.size() > suffix.size()
			  && role.compare( role.size() - suffix.size(), suffix.size(), suffix ) == 0 );
		if( !isPainterChunk ) return out;
	}

	const String keyword( chunk->role.c_str() );
	const ChunkDescriptor* cd = DescriptorForKeyword( keyword );

	// Leading read-only identity row -- always present even if the
	// keyword has no registered descriptor, so the panel isn't blank.
	{
		CameraProperty row;
		row.name        = String( "type" );
		row.kind         = ValueKind::String;
		row.value        = keyword;
		row.description  = String( "Painter chunk keyword" );
		row.editable     = false;
		out.push_back( row );
	}
	if( !cd ) return out;

	for( const ParameterDescriptor& p : cd->parameters )
	{
		if( p.name == "name" ) continue;      // covered by the identity row above; renaming is out of scope
		if( p.repeatable ) continue;          // see header doc -- occ-0 editing doesn't fit a repeated param

		bool present = false;
		const std::string raw = ReadFirstParamValue( chunk, p.name.c_str(), &present );

		CameraProperty row;
		row.name        = String( p.name.c_str() );
		row.kind         = p.kind;
		row.value        = present ? String( raw.c_str() ) : String( p.defaultValueHint.c_str() );
		row.description  = String( p.description.c_str() );
		row.editable     = true;   // routed through ApplyAgentParamEdit -- see header doc
		row.unitLabel    = String( p.unitLabel.c_str() );
		for( const ParameterPreset& pr : p.presets )
		{
			ParameterPreset copy;
			copy.label = pr.label;
			copy.value = pr.value;
			row.presets.push_back( copy );
		}
		out.push_back( row );
	}
	return out;
}

}   // namespace RISE
