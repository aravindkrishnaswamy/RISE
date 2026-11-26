//////////////////////////////////////////////////////////////////////
//
//  SuggestionEngine.h - Public entry point for scene-editor
//    suggestions.  Stateless, thread-safe pure function from
//    (buffer, cursor, mode) to a ranked list of Suggestion values.
//    Platform adapters (Qt / AppKit) wrap this to display the
//    context menu and inline completion.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCENE_EDITOR_SUGGESTIONS_SUGGESTIONENGINE_
#define SCENE_EDITOR_SUGGESTIONS_SUGGESTIONENGINE_

#include <cstddef>
#include <string>
#include <vector>

#include "../Parsers/ChunkDescriptor.h"

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		enum class SuggestionKind
		{
			ChunkKeyword,
			Parameter,
			EnumValue,
			BoolLiteral,
			Reference
		};

		enum class SuggestionMode
		{
			ContextMenu,         // builds the full list grouped by category
			InlineCompletion     // filters by the partial token at the caret and tags the unambiguous hit
		};

		struct Suggestion
		{
			std::string    insertText;
			std::string    displayText;
			std::string    description;
			SuggestionKind kind                    = SuggestionKind::ChunkKeyword;
			ChunkCategory  category                = ChunkCategory::Painter;
			bool           isUnambiguousCompletion = false;
		};

		class SuggestionEngine
		{
		public:
			std::vector<Suggestion> GetSuggestions(
				const std::string& bufferText,
				std::size_t        cursorByteOffset,
				SuggestionMode     mode ) const;
		};
	}
}

#endif
