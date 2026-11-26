//////////////////////////////////////////////////////////////////////
//
//  SceneGrammar.h - Aggregates ChunkDescriptors from the parser
//    registry and exposes lookup / enumeration APIs for the
//    suggestion engine, syntax highlighters, and any future
//    consumer of the scene grammar.  Built lazily on first call
//    to Instance().  Thread-safe via C++11 magic-statics.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCENE_EDITOR_SUGGESTIONS_SCENEGRAMMAR_
#define SCENE_EDITOR_SUGGESTIONS_SCENEGRAMMAR_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../Parsers/ChunkDescriptor.h"
#include "../Parsers/ChunkParserRegistry.h"

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		class SceneGrammar
		{
		public:
			static const SceneGrammar& Instance();

			// Lookup by chunk keyword.  Returns nullptr when the
			// keyword is not a valid RISE scene chunk.
			const ChunkDescriptor* FindChunk( const std::string& keyword ) const;

			// Every chunk keyword known to the grammar (e.g. for
			// the scene-root context menu and syntax highlighters).
			const std::vector<std::string>& AllChunkKeywords() const { return mKeywords; }

			// Every descriptor pointer known to the grammar.
			const std::vector<const ChunkDescriptor*>& AllChunks() const { return mDescriptors; }

			// All descriptors in a given category, in canonical order.
			std::vector<const ChunkDescriptor*> ChunksInCategory( ChunkCategory c ) const;

			// Human-readable plural noun for a category, for use as
			// a submenu header in context menus.
			const char* CategoryDisplayName( ChunkCategory c ) const;

		private:
			SceneGrammar();
			SceneGrammar( const SceneGrammar& );              // deleted
			SceneGrammar& operator=( const SceneGrammar& );   // deleted

			std::vector<ChunkParserEntry>        mParsers;       // owns the parsers; descriptors live inside them
			std::vector<std::string>             mKeywords;      // one per parser entry
			std::vector<const ChunkDescriptor*>  mDescriptors;   // matches mKeywords order
			std::map<std::string, const ChunkDescriptor*> mByKeyword;
		};
	}
}

#endif
