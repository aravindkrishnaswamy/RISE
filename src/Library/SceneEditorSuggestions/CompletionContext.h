//////////////////////////////////////////////////////////////////////
//
//  CompletionContext.h - Given a scene-buffer string and a cursor
//    byte offset, resolves what the user is currently typing and
//    inside what block, so the suggestion engine can propose the
//    right completions.  Pure function, no state.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCENE_EDITOR_SUGGESTIONS_COMPLETIONCONTEXT_
#define SCENE_EDITOR_SUGGESTIONS_COMPLETIONCONTEXT_

#include <cstddef>
#include <string>

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		enum class Scope
		{
			InComment,            // caret is inside /* ... */ or past # on its line — suggest nothing
			InDirective,          // caret is on a !/define/FOR/ENDFOR/> line — suggest nothing
			SceneRoot,            // caret is outside any chunk block — suggest chunk keywords
			InBlockParamName,     // caret is at the start of a parameter line — suggest parameter names
			InBlockParamValue     // caret is after the first whitespace on a parameter line — suggest values
		};

		struct CompletionContext
		{
			Scope       scope             = Scope::SceneRoot;
			std::string chunkKeyword;                // populated when scope is In*
			std::string paramNameOnLine;             // populated when scope == InBlockParamValue
			std::size_t valueTokenIndex   = 0;       // 0 = first token of value
			std::string partialToken;                // text between the last whitespace/line-start and the caret
			std::size_t partialTokenStart = 0;       // byte offset where partialToken begins
		};

		// Walk the buffer up to cursorByteOffset and resolve the
		// cursor's surrounding context.  O(n) in buffer size.
		CompletionContext ResolveCompletionContext(
			const std::string& bufferText,
			std::size_t        cursorByteOffset );
	}
}

#endif
