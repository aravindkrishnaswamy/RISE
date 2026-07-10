//////////////////////////////////////////////////////////////////////
//
//  SchemaGen.cpp - descriptor -> JSON-Schema-ish emitter (see SchemaGen.h).
//
//  The mapping from ValueKind to a JSON-Schema-ish "type" is deliberately
//  coarse (this is a first-pass filter, not the parser -- the parser is
//  always authoritative, docs/agentic-redesign/50-agentic-surface.md §5):
//    Bool                 -> "boolean"
//    UInt                 -> "integer"
//    Double               -> "number"
//    DoubleVec3/4, Mat4   -> "array" (of number, with a fixed length hint)
//    String / Filename    -> "string"
//    Enum                 -> "string" + an `enum` array
//    Reference            -> "string" + a `references` array of category names
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SchemaGen.h"

#include "Json.h"
#include "../SceneEditor/ChunkDescriptorRegistry.h"
#include "../SceneEditorSuggestions/SceneGrammar.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"

#include <set>
#include <string>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//! Append `s` as a JSON string literal (quotes included).  Slice
			//! 0c FACTORED the escaper into the shared JSON codec
			//! (JsonAppendEscapedString) -- this is now a thin forwarder so
			//! SchemaGen and the JSON-RPC transport share ONE correct RFC-8259
			//! escaper (no drift possible).
			void AppendJsonString( std::string& out, const std::string& s )
			{
				JsonAppendEscapedString( out, s );
			}

			//! The JSON-Schema-ish "type" token for a ValueKind.
			const char* TypeFor( ValueKind k )
			{
				switch( k ) {
					case ValueKind::Bool:       return "boolean";
					case ValueKind::UInt:       return "integer";
					case ValueKind::Double:     return "number";
					case ValueKind::DoubleVec3:
					case ValueKind::DoubleVec4:
					case ValueKind::DoubleMat4: return "array";
					case ValueKind::String:
					case ValueKind::Filename:
					case ValueKind::Enum:
					case ValueKind::Reference:  return "string";
				}
				return "string";
			}

			//! Fixed array length for the vector/matrix kinds (0 = not an array).
			int ArrayLenFor( ValueKind k )
			{
				switch( k ) {
					case ValueKind::DoubleVec3: return 3;
					case ValueKind::DoubleVec4: return 4;
					case ValueKind::DoubleMat4: return 16;
					default:                    return 0;
				}
			}

			//! A stable lowercase name for a ChunkCategory (for a Reference
			//! param's `references` array).  Mirrors the descriptor category
			//! namespace the design keys references on (§2.5).
			const char* CategoryName( ChunkCategory c )
			{
				switch( c ) {
					case ChunkCategory::Painter:          return "painter";
					case ChunkCategory::Function:         return "function";
					case ChunkCategory::Material:         return "material";
					case ChunkCategory::Camera:           return "camera";
					case ChunkCategory::Film:             return "film";
					case ChunkCategory::Geometry:         return "geometry";
					case ChunkCategory::Modifier:         return "modifier";
					case ChunkCategory::Medium:           return "medium";
					case ChunkCategory::Object:           return "object";
					case ChunkCategory::ShaderOp:         return "shaderop";
					case ChunkCategory::Shader:           return "shader";
					case ChunkCategory::Rasterizer:       return "rasterizer";
					case ChunkCategory::RasterizerOutput: return "rasterizer_output";
					case ChunkCategory::Light:            return "light";
					case ChunkCategory::PhotonMap:        return "photon_map";
					case ChunkCategory::PhotonGather:     return "photon_gather";
					case ChunkCategory::IrradianceCache:  return "irradiance_cache";
					case ChunkCategory::Animation:        return "animation";
					case ChunkCategory::SceneVariant:     return "scene_variant";
				}
				return "unknown";
			}

			//! Emit one ParameterDescriptor as a JSON object into `out`.
			void EmitParameter( std::string& out, const ParameterDescriptor& p )
			{
				out += '{';
				out += "\"type\":";
				AppendJsonString( out, TypeFor( p.kind ) );

				const int arrLen = ArrayLenFor( p.kind );
				if( arrLen > 0 ) {
					out += ",\"items\":{\"type\":\"number\"}";
					out += ",\"minItems\":";
					out += std::to_string( arrLen );
					out += ",\"maxItems\":";
					out += std::to_string( arrLen );
				}

				if( p.kind == ValueKind::Enum && !p.enumValues.empty() ) {
					out += ",\"enum\":[";
					for( size_t i = 0; i < p.enumValues.size(); ++i ) {
						if( i ) out += ',';
						AppendJsonString( out, p.enumValues[i] );
					}
					out += ']';
				}

				if( p.kind == ValueKind::Reference && !p.referenceCategories.empty() ) {
					out += ",\"references\":[";
					for( size_t i = 0; i < p.referenceCategories.size(); ++i ) {
						if( i ) out += ',';
						AppendJsonString( out, CategoryName( p.referenceCategories[i] ) );
					}
					out += ']';
				}

				out += ",\"required\":";
				out += ( p.required ? "true" : "false" );

				out += ",\"repeatable\":";
				out += ( p.repeatable ? "true" : "false" );

				if( !p.description.empty() ) {
					out += ",\"description\":";
					AppendJsonString( out, p.description );
				}
				if( !p.defaultValueHint.empty() ) {
					out += ",\"default\":";
					AppendJsonString( out, p.defaultValueHint );
				}
				if( !p.unitLabel.empty() ) {
					out += ",\"unit\":";
					AppendJsonString( out, p.unitLabel );
				}
				out += '}';
			}

			//! Emit a whole ChunkDescriptor as a JSON object into `out`.
			void EmitChunk( std::string& out, const ChunkDescriptor& d )
			{
				out += '{';
				out += "\"keyword\":";
				AppendJsonString( out, d.keyword );
				out += ",\"category\":";
				AppendJsonString( out, CategoryName( d.category ) );
				if( !d.description.empty() ) {
					out += ",\"description\":";
					AppendJsonString( out, d.description );
				}
				out += ",\"properties\":{";
				for( size_t i = 0; i < d.parameters.size(); ++i ) {
					if( i ) out += ',';
					AppendJsonString( out, d.parameters[i].name );
					out += ':';
					EmitParameter( out, d.parameters[i] );
				}
				out += "}}";
			}
		}

		std::string SchemaGenForChunk( const std::string& keyword )
		{
			const ChunkDescriptor* d = DescriptorForKeyword( String( keyword.c_str() ) );
			if( !d ) {
				std::string out;
				out += "{\"error\":";
				AppendJsonString( out, "unknown chunk type '" + keyword + "'" );
				out += '}';
				return out;
			}
			std::string out;
			EmitChunk( out, *d );
			return out;
		}

		std::string SchemaGenAll()
		{
			// SchemaGenAll enumerates via the SceneGrammar singleton;
			// SchemaGenForChunk resolves via DescriptorForKeyword (a separate
			// process-lifetime static).  Both derive from the same
			// CreateAllChunkParsers() factory output, so per-chunk and
			// whole-grammar schemas cannot drift.
			const SceneEditorSuggestions::SceneGrammar& g = SceneEditorSuggestions::SceneGrammar::Instance();
			const std::vector<const ChunkDescriptor*>& all = g.AllChunks();

			// Dedupe by keyword.  SceneGrammar's mDescriptors has one entry
			// PER REGISTERED PARSER, not per distinct chunk (see
			// SceneGrammar.cpp's constructor comment): a legacy alias like
			// `mis_pathtracing_shaderop` shares its parser CLASS with the
			// canonical `pathtracing_shaderop` entry, and Describe() reports
			// the canonical keyword on both -- so `all` contains the same
			// `d->keyword` twice.  Emitting it twice here is harmless for
			// Anthropic/OpenAI (tool results ride as opaque strings) but is
			// a hard failure for Gemini, whose protobuf-Struct backend
			// rejects duplicate map keys outright (HTTP 400).  First
			// occurrence wins (matches SceneGrammar's own registration
			// order, which lists the canonical keyword before its aliases).
			std::set<std::string> seen;

			std::string out;
			out += '{';
			bool first = true;
			for( const ChunkDescriptor* d : all ) {
				if( !d ) continue;
				if( !seen.insert( d->keyword ).second ) continue;
				if( !first ) out += ',';
				first = false;
				AppendJsonString( out, d->keyword );
				out += ':';
				EmitChunk( out, *d );
			}
			out += '}';
			return out;
		}
	}
}
