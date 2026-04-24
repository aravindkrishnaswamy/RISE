//////////////////////////////////////////////////////////////////////
//
//  IAsciiChunkParser.h - Interface for the per-chunk parsers used by
//    AsciiSceneParser.  Was defined inline in AsciiSceneParser.cpp
//    until it was extracted here so that consumers of the scene
//    grammar (the scene-editor suggestion engine, future validators)
//    can include it without pulling in the 8000-line scene parser
//    implementation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IASCII_CHUNKPARSER_
#define IASCII_CHUNKPARSER_

#include <vector>

#include "ChunkDescriptor.h"
#include "../Utilities/RString.h"

namespace RISE
{
	class IJob;

	class IAsciiChunkParser
	{
	protected:
		IAsciiChunkParser() {}

	public:
		typedef std::vector<String> ParamsList;

		virtual ~IAsciiChunkParser() {}

		virtual bool ParseChunk( const ParamsList& in, IJob& pJob ) const = 0;

		// Describes this chunk's grammar: keyword, category, parameter
		// metadata.  Every concrete chunk parser implements this via a
		// static descriptor, and SceneEditorSuggestions / the syntax
		// highlighters read it.  Pure-virtual so adding a new chunk
		// parser cannot silently surface as an empty grammar entry in
		// the suggestion engine — the build fails until Describe() is
		// implemented.  Returned reference must remain valid for the
		// parser's lifetime; consumers that need longer-lived copies
		// should copy the descriptor out at registry-build time.
		[[nodiscard]] virtual const ChunkDescriptor& Describe() const = 0;
	};
}

#endif
