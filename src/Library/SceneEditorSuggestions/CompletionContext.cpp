//////////////////////////////////////////////////////////////////////
//
//  CompletionContext.cpp - Implementation.  Single forward scan of
//    the buffer up to the caret, tracking brace depth and active
//    chunk keyword.  Braces are on their own lines per the RISE
//    grammar invariant, which keeps the counter simple.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CompletionContext.h"

#include <cctype>
#include <algorithm>

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		namespace
		{
			// Trim leading whitespace and return the offset of the first
			// non-whitespace character on the line.
			std::size_t FirstNonSpace( const std::string& s, std::size_t begin, std::size_t end )
			{
				while( begin < end && ( s[begin] == ' ' || s[begin] == '\t' ) ) {
					++begin;
				}
				return begin;
			}

			// Strip trailing whitespace and return the offset past the last
			// non-whitespace character.
			std::size_t PastLastNonSpace( const std::string& s, std::size_t begin, std::size_t end )
			{
				while( end > begin && ( s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ) ) {
					--end;
				}
				return end;
			}

			bool IsBraceOnlyLine( const std::string& s, std::size_t begin, std::size_t end, char brace )
			{
				std::size_t fns = FirstNonSpace( s, begin, end );
				std::size_t plns = PastLastNonSpace( s, fns, end );
				return plns == fns + 1 && s[fns] == brace;
			}

			// '#' starts a line comment, not a directive — it is handled
			// separately below so it resolves to Scope::InComment (which
			// is semantically what the user sees).  Directives proper
			// begin with '>', '!', '~', or one of the multi-char keywords
			// FOR / ENDFOR / define / undef.
			bool IsDirectiveLine( const std::string& s, std::size_t begin, std::size_t end )
			{
				std::size_t fns = FirstNonSpace( s, begin, end );
				if( fns >= end ) return false;
				char c = s[fns];
				if( c == '>' || c == '!' || c == '~' ) return true;
				// Keywords: define, undef, FOR, ENDFOR
				static const char* kDirectives[] = { "define", "undef", "FOR", "ENDFOR" };
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

			std::string LineChunkKeyword( const std::string& s, std::size_t begin, std::size_t end )
			{
				// A chunk keyword line is a single token followed by no
				// other visible content.  Extract it.
				std::size_t fns = FirstNonSpace( s, begin, end );
				std::size_t plns = PastLastNonSpace( s, fns, end );
				if( plns <= fns ) return std::string();
				// Reject if there's any whitespace between fns and plns
				for( std::size_t i = fns; i < plns; ++i ) {
					if( s[i] == ' ' || s[i] == '\t' ) return std::string();
				}
				return s.substr( fns, plns - fns );
			}
		}

		CompletionContext ResolveCompletionContext( const std::string& bufferText, std::size_t cursorByteOffset )
		{
			CompletionContext ctx;
			if( cursorByteOffset > bufferText.size() ) {
				cursorByteOffset = bufferText.size();
			}

			int          braceDepth     = 0;
			std::string  currentKeyword;
			std::string  pendingKeyword; // keyword from the last non-blank line seen; becomes currentKeyword on {
			bool         inBlockComment = false;

			std::size_t pos = 0;
			std::size_t caretLineStart = 0;

			while( pos <= cursorByteOffset ) {
				// Find end of line
				std::size_t lineEnd = pos;
				while( lineEnd < bufferText.size() && bufferText[lineEnd] != '\n' ) {
					++lineEnd;
				}

				// If the caret is inside this line, remember the line start
				// and stop the outer scan — we'll classify the caret's
				// position within this line below.
				if( cursorByteOffset >= pos && cursorByteOffset <= lineEnd ) {
					caretLineStart = pos;
					break;
				}

				if( inBlockComment ) {
					// Look for */ on this line
					std::size_t close = bufferText.find( "*/", pos );
					if( close != std::string::npos && close < lineEnd ) {
						inBlockComment = false;
					}
				} else {
					std::size_t fns = FirstNonSpace( bufferText, pos, lineEnd );
					// Detect start of block comment (ignore same-line close for
					// simplicity — scenes don't typically put /* and */ on one
					// non-comment line)
					if( fns + 1 < lineEnd && bufferText[fns] == '/' && bufferText[fns + 1] == '*' ) {
						inBlockComment = true;
					} else if( IsBraceOnlyLine( bufferText, pos, lineEnd, '{' ) ) {
						braceDepth++;
						currentKeyword = pendingKeyword;
					} else if( IsBraceOnlyLine( bufferText, pos, lineEnd, '}' ) ) {
						braceDepth = std::max( 0, braceDepth - 1 );
						if( braceDepth == 0 ) currentKeyword.clear();
					} else if( IsDirectiveLine( bufferText, pos, lineEnd ) ) {
						// don't update pendingKeyword
					} else {
						std::string kw = LineChunkKeyword( bufferText, pos, lineEnd );
						if( !kw.empty() ) {
							pendingKeyword = kw;
						}
					}
				}

				pos = lineEnd + 1;  // skip newline
			}

			// Classify the caret's line.
			std::size_t caretLineEnd = cursorByteOffset;
			while( caretLineEnd < bufferText.size() && bufferText[caretLineEnd] != '\n' ) {
				++caretLineEnd;
			}

			if( inBlockComment ) {
				ctx.scope = Scope::InComment;
			} else if( IsDirectiveLine( bufferText, caretLineStart, caretLineEnd ) ) {
				ctx.scope = Scope::InDirective;
			} else {
				// Determine if the caret line starts with # before the caret (line comment)
				std::size_t fns = FirstNonSpace( bufferText, caretLineStart, cursorByteOffset );
				if( fns < cursorByteOffset && bufferText[fns] == '#' ) {
					ctx.scope = Scope::InComment;
				} else if( braceDepth == 0 ) {
					ctx.scope           = Scope::SceneRoot;
					ctx.partialTokenStart = fns;
					ctx.partialToken      = bufferText.substr( fns, cursorByteOffset - fns );
				} else {
					ctx.chunkKeyword = currentKeyword;

					// Determine if the caret is before the first whitespace
					// on the line (param-name position) or after (param-value).
					std::size_t firstWs = fns;
					while( firstWs < cursorByteOffset && bufferText[firstWs] != ' ' && bufferText[firstWs] != '\t' ) {
						++firstWs;
					}

					if( firstWs >= cursorByteOffset ) {
						// No whitespace between fns and caret — param-name position
						ctx.scope             = Scope::InBlockParamName;
						ctx.partialTokenStart = fns;
						ctx.partialToken      = bufferText.substr( fns, cursorByteOffset - fns );
					} else {
						ctx.scope            = Scope::InBlockParamValue;
						ctx.paramNameOnLine  = bufferText.substr( fns, firstWs - fns );

						// Skip whitespace to the start of the value portion.
						std::size_t afterName = firstWs;
						while( afterName < cursorByteOffset && ( bufferText[afterName] == ' ' || bufferText[afterName] == '\t' ) ) {
							++afterName;
						}

						// Count how many whitespace-separated tokens precede
						// the caret within the value portion.
						std::size_t tokenStart = afterName;
						std::size_t count      = 0;
						for( std::size_t i = afterName; i < cursorByteOffset; ++i ) {
							if( bufferText[i] == ' ' || bufferText[i] == '\t' ) {
								count++;
								// advance past any run of whitespace
								while( i + 1 < cursorByteOffset && ( bufferText[i+1] == ' ' || bufferText[i+1] == '\t' ) ) {
									++i;
								}
								tokenStart = i + 1;
							}
						}

						ctx.valueTokenIndex   = count;
						ctx.partialTokenStart = tokenStart;
						ctx.partialToken      = bufferText.substr( tokenStart, cursorByteOffset - tokenStart );
					}
				}
			}

			return ctx;
		}
	}
}
