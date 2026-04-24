//////////////////////////////////////////////////////////////////////
//
//  NameIndex.cpp - Single-pass scan of the scene buffer collecting
//    every (name, chunkKeyword, category) triple.  Looks up the
//    category via SceneGrammar.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "NameIndex.h"
#include "SceneGrammar.h"

#include <algorithm>

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		namespace
		{
			std::size_t FirstNonSpace( const std::string& s, std::size_t begin, std::size_t end )
			{
				while( begin < end && ( s[begin] == ' ' || s[begin] == '\t' ) ) ++begin;
				return begin;
			}

			std::size_t PastLastNonSpace( const std::string& s, std::size_t begin, std::size_t end )
			{
				while( end > begin && ( s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ) ) --end;
				return end;
			}
		}

		namespace
		{
			// True when the line starts a directive (#, >, !, ~, FOR, ENDFOR,
			// define, undef).  These should never contribute to the name
			// index — a `name X` that appears on such a line is either a
			// comment or a directive-payload, not a live chunk definition.
			bool IsDirectiveOrComment( const std::string& s, std::size_t fns, std::size_t end )
			{
				if( fns >= end ) return true;
				const char c = s[fns];
				if( c == '#' || c == '>' || c == '!' || c == '~' ) return true;
				// Multi-char keywords: FOR / ENDFOR / define / undef.
				static const char* kDirectives[] = { "FOR", "ENDFOR", "define", "undef" };
				for( std::size_t i = 0; i < sizeof(kDirectives)/sizeof(kDirectives[0]); ++i ) {
					std::size_t len = 0;
					while( kDirectives[i][len] ) ++len;
					if( fns + len <= end && s.compare( fns, len, kDirectives[i] ) == 0 ) {
						std::size_t after = fns + len;
						if( after == end || s[after] == ' ' || s[after] == '\t' ) return true;
					}
				}
				return false;
			}
		}

		NameIndex::NameIndex( const std::string& bufferText )
		{
			const SceneGrammar& grammar = SceneGrammar::Instance();

			int         braceDepth     = 0;
			std::string pendingKeyword;
			std::string currentKeyword;
			std::string currentName;
			bool        inBlockComment = false;

			std::size_t pos = 0;
			while( pos < bufferText.size() ) {
				std::size_t lineEnd = pos;
				while( lineEnd < bufferText.size() && bufferText[lineEnd] != '\n' ) ++lineEnd;

				std::size_t fns  = FirstNonSpace( bufferText, pos, lineEnd );
				std::size_t plns = PastLastNonSpace( bufferText, fns, lineEnd );

				if( plns > fns ) {
					if( inBlockComment ) {
						// Look for a closing */ anywhere on this line.
						std::size_t close = bufferText.find( "*/", pos );
						if( close != std::string::npos && close < lineEnd ) {
							inBlockComment = false;
						}
						pos = lineEnd + 1;
						continue;
					}

					// Start of a block comment — treat this line and
					// everything until the matching */ as comment.  We don't
					// handle the rare case of /* and */ on the same non-
					// comment line (the parser itself treats those as
					// comment-only, matching the scene grammar).
					if( fns + 1 < plns && bufferText[fns] == '/' && bufferText[fns + 1] == '*' ) {
						std::size_t close = bufferText.find( "*/", fns + 2 );
						if( close == std::string::npos || close >= lineEnd ) {
							inBlockComment = true;
						}
						pos = lineEnd + 1;
						continue;
					}

					// # line comment or top-level directive — skip entirely.
					if( IsDirectiveOrComment( bufferText, fns, plns ) ) {
						pos = lineEnd + 1;
						continue;
					}

					// Brace-only lines
					if( plns == fns + 1 && bufferText[fns] == '{' ) {
						braceDepth++;
						currentKeyword = pendingKeyword;
						currentName.clear();
					} else if( plns == fns + 1 && bufferText[fns] == '}' ) {
						if( braceDepth > 0 ) braceDepth--;
						if( braceDepth == 0 && !currentKeyword.empty() && !currentName.empty() ) {
							const ChunkDescriptor* d = grammar.FindChunk( currentKeyword );
							DefinedName dn;
							dn.name         = currentName;
							dn.chunkKeyword = currentKeyword;
							dn.category     = d ? d->category : ChunkCategory::Painter;
							mNames.push_back( dn );
						}
						if( braceDepth == 0 ) currentKeyword.clear();
					} else if( braceDepth == 0 ) {
						// Candidate chunk keyword line: a single bare token.
						bool single = true;
						for( std::size_t i = fns; i < plns; ++i ) {
							if( bufferText[i] == ' ' || bufferText[i] == '\t' ) { single = false; break; }
						}
						if( single ) {
							pendingKeyword = bufferText.substr( fns, plns - fns );
						}
					} else {
						// Parameter line inside a block.  If it's a `name X`
						// line, capture the value.
						std::size_t firstWs = fns;
						while( firstWs < plns && bufferText[firstWs] != ' ' && bufferText[firstWs] != '\t' ) {
							++firstWs;
						}
						std::string paramName = bufferText.substr( fns, firstWs - fns );
						if( paramName == "name" && firstWs < plns ) {
							std::size_t valueStart = firstWs;
							while( valueStart < plns && ( bufferText[valueStart] == ' ' || bufferText[valueStart] == '\t' ) ) ++valueStart;
							currentName = bufferText.substr( valueStart, plns - valueStart );
						}
					}
				}

				pos = lineEnd + 1;
			}
		}

		std::vector<const DefinedName*> NameIndex::FilterByCategories( const std::vector<ChunkCategory>& cats ) const
		{
			std::vector<const DefinedName*> out;
			for( std::vector<DefinedName>::const_iterator it = mNames.begin(); it != mNames.end(); ++it ) {
				for( std::vector<ChunkCategory>::const_iterator c = cats.begin(); c != cats.end(); ++c ) {
					if( it->category == *c ) {
						out.push_back( &(*it) );
						break;
					}
				}
			}
			return out;
		}
	}
}
