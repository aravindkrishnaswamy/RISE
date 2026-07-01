//////////////////////////////////////////////////////////////////////
//
//  SchemaGen.h - descriptor -> JSON-Schema-ish emitter (Facet 5, the
//    agentic surface -- slice 0a; the `read_schema` backing).
//
//    Walks the live ChunkDescriptor registry (via DescriptorForKeyword /
//    SceneGrammar -- charter L6: the descriptor IS the schema) and emits
//    a JSON object describing a chunk's parameters (name, type from
//    ValueKind, enum from enumValues, description, default from
//    defaultValueHint, required).  This is the agent's "what is the
//    grammar" reference (docs/agentic-redesign/50-agentic-surface.md
//    §2.2.1 `read_schema`).
//
//    Emitted with a small local string builder (the tree has no general
//    JSON library yet -- slice 0c adds the shared JSON-RPC codec); the
//    local emitter is minimal but correct on escaping.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_SCHEMAGEN_
#define RISE_AGENT_SCHEMAGEN_

#include <string>

namespace RISE
{
	namespace Agent
	{
		//! JSON Schema (as a string) for ONE chunk keyword.  On an unknown
		//! keyword returns an error object: `{"error":"unknown chunk ..."}`
		//! (never throws, never returns empty -- a caller can test for the
		//! `"error"` key).
		std::string SchemaGenForChunk( const std::string& keyword );

		//! JSON Schema for the WHOLE grammar: a top-level object keyed by
		//! chunk keyword, each value the same per-chunk schema
		//! SchemaGenForChunk emits.
		std::string SchemaGenAll();
	}
}

#endif
