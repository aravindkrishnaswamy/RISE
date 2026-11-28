//////////////////////////////////////////////////////////////////////
//
//  ChunkParserRegistry.h - Factory exposing one instance of every
//    chunk parser the scene grammar supports.  Shared by
//    Cst::DeriveToJob (which uses the entries to populate its
//    dispatch map) and by SceneEditorSuggestions (which walks the
//    entries to enumerate ChunkDescriptors for the suggestion engine
//    and the syntax highlighters).
//
//    The definition of CreateAllChunkParsers lives in
//    ChunkParserRegistry.cpp alongside the concrete chunk parser
//    classes; this header provides the declaration for external
//    consumers.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CHUNK_PARSER_REGISTRY_
#define CHUNK_PARSER_REGISTRY_

#include <memory>
#include <string>
#include <vector>

#include "IAsciiChunkParser.h"

namespace RISE
{
	struct ChunkParserEntry
	{
		// Dispatch key used by Cst::DeriveToJob and SceneGrammar to build
		// their name->parser map (allows keyword aliases such as
		// `mis_pathtracing_shaderop` to share a parser class with a
		// different Describe().keyword).
		std::string                        keyword;
		std::unique_ptr<IAsciiChunkParser> parser;
	};

	// Creates one instance of every chunk parser type the scene grammar
	// supports.  Entries are returned in canonical registration order
	// (ChunkParserRegistry.cpp).  Ownership of each parser
	// transfers to the caller; when the returned vector goes out of
	// scope all parsers are destroyed.
	[[nodiscard]] std::vector<ChunkParserEntry> CreateAllChunkParsers();

	// Legacy-deprecation Phase 1 (2026-10-09): a deprecated keyword ALIAS that
	// shares its descriptor with a SUPPORTED canonical keyword (today only
	// `mis_pathtracing_shaderop` -> `pathtracing_shaderop`) cannot carry the
	// descriptor flag, so DeriveToJob asks here by DISPATCH keyword.  Returns the
	// replacement hint, or nullptr for every other keyword.
	[[nodiscard]] const char* DeprecatedChunkAliasReplacement( const std::string& keyword );

	namespace Implementation
	{
		namespace ChunkParsers
		{
			// The `hair_geometry` grow-mode-only parameter list -- the
			// production source of truth `HairGeometryAsciiChunkParser::
			// Finalize` (ChunkParserRegistry.cpp) refuses in `file` mode.
			// Declared here (residual wave 2 item C, 2026-08-27) so a test
			// can assert against the REAL list instead of maintaining its
			// own byte-copied mirror that could silently drift from it --
			// see HairFileImportTest.cpp's drift guard.
			const char* const* HairGeometryGrowOnlyParameters( unsigned int& n );
		}
	}
}

#endif
