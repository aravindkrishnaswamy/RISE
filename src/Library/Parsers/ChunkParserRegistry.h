//////////////////////////////////////////////////////////////////////
//
//  ChunkParserRegistry.h - Factory exposing one instance of every
//    chunk parser the scene grammar supports.  Shared by
//    AsciiSceneParser (which uses the entries to populate its
//    dispatch map) and by SceneEditorSuggestions (which walks the
//    entries to enumerate ChunkDescriptors for the suggestion engine
//    and the syntax highlighters).
//
//    The definition of CreateAllChunkParsers lives in
//    AsciiSceneParser.cpp because that is where the concrete chunk
//    parser classes are defined; all this header provides is the
//    declaration so consumers outside that translation unit can call
//    it.
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
		// During Phase 1 migration, `keyword` is the authoritative dispatch
		// key — AsciiSceneParser uses it to build its name->parser map, and
		// the legacy alias `mis_pathtracing_shaderop` relies on the entry
		// carrying a different keyword than the parser's own Describe().
		// Once every parser has a populated Describe() (Phase 1c) this
		// field becomes redundant with Describe().keyword for non-alias
		// entries and may be removed in favour of a separate alias table.
		std::string                        keyword;
		std::unique_ptr<IAsciiChunkParser> parser;
	};

	// Creates one instance of every chunk parser type the scene grammar
	// supports.  Entries are returned in canonical order (as they appear
	// in AsciiSceneParser's old inline block).  Ownership of each parser
	// transfers to the caller; when the returned vector goes out of
	// scope all parsers are destroyed.
	[[nodiscard]] std::vector<ChunkParserEntry> CreateAllChunkParsers();

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
