//////////////////////////////////////////////////////////////////////
//
//  SuggestionEngine.cpp - Generates ranked suggestions from the
//    resolved completion context.  Filters by the partial token,
//    ranks exact-prefix matches above subword, and marks the
//    single top-tier candidate as the "ghost-text" completion
//    when in InlineCompletion mode.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SuggestionEngine.h"
#include "SceneGrammar.h"
#include "CompletionContext.h"
#include "NameIndex.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		namespace
		{
			enum class MatchTier { None = 0, Fuzzy = 1, Subword = 2, ExactPrefix = 3 };

			MatchTier RankMatch( const std::string& candidate, const std::string& partial )
			{
				if( partial.empty() ) return MatchTier::ExactPrefix;
				if( candidate.size() < partial.size() ) return MatchTier::None;

				// Exact prefix (case-sensitive — scene grammar is case-sensitive)
				if( candidate.compare( 0, partial.size(), partial ) == 0 ) return MatchTier::ExactPrefix;

				// Subword: partial matches a snake-case word start somewhere inside
				// (e.g. "bt" matches "branching_threshold")
				if( partial.size() >= 2 ) {
					std::size_t idx = 0;
					for( std::size_t i = 0; i < candidate.size() && idx < partial.size(); ++i ) {
						bool isStart = ( i == 0 ) || candidate[i-1] == '_';
						if( isStart && candidate[i] == partial[idx] ) {
							++idx;
						}
					}
					if( idx == partial.size() ) return MatchTier::Subword;
				}

				// Fuzzy: all chars of partial appear in order somewhere in candidate
				std::size_t idx = 0;
				for( std::size_t i = 0; i < candidate.size() && idx < partial.size(); ++i ) {
					if( candidate[i] == partial[idx] ) ++idx;
				}
				if( idx == partial.size() ) return MatchTier::Fuzzy;

				return MatchTier::None;
			}

			struct Ranked
			{
				Suggestion suggestion;
				MatchTier  tier;
			};

			// Collects the set of parameter names already written in the
			// block that contains `cursorByteOffset`.  Walks backward from
			// the caret to the most recent unclosed '{' line, then forward
			// from that line to the caret's line, reading the first token
			// of every non-empty, non-comment, non-directive line.  Used
			// by the suggestion engine to suppress re-suggesting a
			// non-repeatable parameter that has already been authored.
			std::set<std::string> CollectAuthoredParamNamesInBlock(
				const std::string& bufferText,
				std::size_t        cursorByteOffset )
			{
				std::set<std::string> out;
				if( cursorByteOffset > bufferText.size() ) cursorByteOffset = bufferText.size();

				// Walk line-by-line tracking depth.  When we reach depth 1
				// (inside the enclosing block), collect parameter-name
				// tokens.  Skip comments and directive lines.
				int         braceDepth     = 0;
				bool        inBlockComment = false;
				std::size_t pos            = 0;
				std::size_t enteringBlockAt = std::string::npos; // start collecting after we re-enter depth 1

				while( pos < cursorByteOffset ) {
					std::size_t lineEnd = pos;
					while( lineEnd < cursorByteOffset && bufferText[lineEnd] != '\n' ) ++lineEnd;

					// Find first non-space and past last non-space.
					std::size_t fns = pos;
					while( fns < lineEnd && ( bufferText[fns] == ' ' || bufferText[fns] == '\t' ) ) ++fns;
					std::size_t plns = lineEnd;
					while( plns > fns && ( bufferText[plns - 1] == ' ' || bufferText[plns - 1] == '\t' || bufferText[plns - 1] == '\r' ) ) --plns;

					if( plns <= fns ) { pos = lineEnd + 1; continue; }

					if( inBlockComment ) {
						std::size_t close = bufferText.find( "*/", pos );
						if( close != std::string::npos && close < lineEnd ) inBlockComment = false;
						pos = lineEnd + 1;
						continue;
					}
					if( fns + 1 < plns && bufferText[fns] == '/' && bufferText[fns + 1] == '*' ) {
						std::size_t close = bufferText.find( "*/", fns + 2 );
						if( close == std::string::npos || close >= lineEnd ) inBlockComment = true;
						pos = lineEnd + 1;
						continue;
					}
					// Skip comment / directive lines.
					const char c0 = bufferText[fns];
					if( c0 == '#' || c0 == '>' || c0 == '!' || c0 == '~' ) { pos = lineEnd + 1; continue; }
					// FOR/ENDFOR/define/undef as first token
					static const char* kSkipPrefixes[] = { "FOR", "ENDFOR", "define", "undef" };
					bool skip = false;
					for( std::size_t i = 0; i < 4; ++i ) {
						std::size_t len = 0; while( kSkipPrefixes[i][len] ) ++len;
						if( fns + len <= plns && bufferText.compare( fns, len, kSkipPrefixes[i] ) == 0 ) {
							std::size_t after = fns + len;
							if( after == plns || bufferText[after] == ' ' || bufferText[after] == '\t' ) { skip = true; break; }
						}
					}
					if( skip ) { pos = lineEnd + 1; continue; }

					// Brace-only lines adjust depth.
					if( plns == fns + 1 && bufferText[fns] == '{' ) {
						braceDepth++;
						if( braceDepth == 1 ) { enteringBlockAt = pos; out.clear(); }
					} else if( plns == fns + 1 && bufferText[fns] == '}' ) {
						if( braceDepth > 0 ) braceDepth--;
						if( braceDepth == 0 ) { out.clear(); enteringBlockAt = std::string::npos; }
					} else if( braceDepth >= 1 && enteringBlockAt != std::string::npos ) {
						// Capture the first whitespace-separated token on the line.
						std::size_t firstWs = fns;
						while( firstWs < plns && bufferText[firstWs] != ' ' && bufferText[firstWs] != '\t' ) ++firstWs;
						if( firstWs > fns ) {
							out.insert( bufferText.substr( fns, firstWs - fns ) );
						}
					}

					pos = lineEnd + 1;
				}

				return out;
			}

			bool RankedLess( const Ranked& a, const Ranked& b )
			{
				if( a.tier != b.tier ) return a.tier > b.tier;  // higher tier first
				return a.suggestion.displayText < b.suggestion.displayText;
			}

			void EmitChunkSuggestion(
				const std::string&        keyword,
				const ChunkDescriptor&    desc,
				const std::string&        partial,
				std::vector<Ranked>&      out )
			{
				// Use the grammar's registered keyword (which may be an
				// alias like mis_pathtracing_shaderop) rather than
				// desc.keyword (which is always the primary class's
				// keyword, shared by alias entries).  Without this, the
				// scene-root menu would list pathtracing_shaderop twice
				// and never expose the alias as an insertable choice.
				MatchTier t = RankMatch( keyword, partial );
				if( t == MatchTier::None ) return;
				Ranked r;
				r.suggestion.insertText  = keyword;
				r.suggestion.displayText = keyword;
				r.suggestion.description = desc.description;
				r.suggestion.kind        = SuggestionKind::ChunkKeyword;
				r.suggestion.category    = desc.category;
				r.tier                   = t;
				out.push_back( r );
			}

			// Produces a placeholder value text for a parameter of the
			// given kind.  Used to seed the line a context-menu insertion
			// produces, so the user sees a complete `name <value>` line
			// rather than just the bare parameter name.
			std::string MakeValuePlaceholder( const ParameterDescriptor& p )
			{
				if( !p.defaultValueHint.empty() ) {
					return p.defaultValueHint;
				}
				switch( p.kind ) {
				case ValueKind::Bool:       return "FALSE";
				case ValueKind::UInt:       return "0";
				case ValueKind::Double:     return "0.0";
				case ValueKind::DoubleVec3: return "0 0 0";
				case ValueKind::Filename:   return "<filename>";
				case ValueKind::Enum:
					return p.enumValues.empty() ? std::string() : p.enumValues.front();
				case ValueKind::Reference:  return "<name>";
				case ValueKind::String:     return "<value>";
				}
				return "<value>";
			}

			void EmitParameterSuggestion(
				const ParameterDescriptor& p,
				ChunkCategory              cat,
				const std::string&         partial,
				SuggestionMode             mode,
				std::vector<Ranked>&       out )
			{
				MatchTier t = RankMatch( p.name, partial );
				if( t == MatchTier::None ) return;
				Ranked r;
				// In ContextMenu mode (right-click → insert whole line)
				// embed a placeholder value so the user sees `samples 1`
				// or `branching_threshold 0.5` rather than a bare
				// parameter name.  In InlineCompletion mode the user
				// is typing the parameter's name and expects to
				// complete only that token; the placeholder would
				// land mid-stream and require backspacing.
				const std::string placeholder =
					( mode == SuggestionMode::ContextMenu ) ? MakeValuePlaceholder( p ) : std::string();
				r.suggestion.insertText  = placeholder.empty() ? p.name : ( p.name + " " + placeholder );
				r.suggestion.displayText = p.name;
				r.suggestion.description = p.description;
				r.suggestion.kind        = SuggestionKind::Parameter;
				r.suggestion.category    = cat;
				r.tier                   = t;
				out.push_back( r );
			}

			void EmitValueSuggestions(
				const ParameterDescriptor& p,
				const std::string&         bufferText,
				const std::string&         partial,
				std::vector<Ranked>&       out )
			{
				if( p.kind == ValueKind::Bool ) {
					for( const char* v : { "TRUE", "FALSE" } ) {
						MatchTier t = RankMatch( v, partial );
						if( t == MatchTier::None ) continue;
						Ranked r;
						r.suggestion.insertText  = v;
						r.suggestion.displayText = v;
						r.suggestion.kind        = SuggestionKind::BoolLiteral;
						r.tier                   = t;
						out.push_back( r );
					}
				} else if( p.kind == ValueKind::Enum ) {
					for( std::vector<std::string>::const_iterator it = p.enumValues.begin(); it != p.enumValues.end(); ++it ) {
						MatchTier t = RankMatch( *it, partial );
						if( t == MatchTier::None ) continue;
						Ranked r;
						r.suggestion.insertText  = *it;
						r.suggestion.displayText = *it;
						r.suggestion.kind        = SuggestionKind::EnumValue;
						r.tier                   = t;
						out.push_back( r );
					}
				} else if( p.kind == ValueKind::Reference ) {
					NameIndex ni( bufferText );
					std::vector<const DefinedName*> defs = ni.FilterByCategories( p.referenceCategories );
					for( std::vector<const DefinedName*>::const_iterator it = defs.begin(); it != defs.end(); ++it ) {
						MatchTier t = RankMatch( (*it)->name, partial );
						if( t == MatchTier::None ) continue;
						Ranked r;
						r.suggestion.insertText  = (*it)->name;
						r.suggestion.displayText = (*it)->name;
						r.suggestion.description = std::string( "From " ) + (*it)->chunkKeyword;
						r.suggestion.kind        = SuggestionKind::Reference;
						r.suggestion.category    = (*it)->category;
						r.tier                   = t;
						out.push_back( r );
					}
				}
			}
		}

		std::vector<Suggestion> SuggestionEngine::GetSuggestions(
			const std::string& bufferText,
			std::size_t        cursorByteOffset,
			SuggestionMode     mode ) const
		{
			CompletionContext ctx = ResolveCompletionContext( bufferText, cursorByteOffset );

			// Determine the partial string to filter by.  In ContextMenu
			// mode we present the full list; in InlineCompletion we filter
			// to what the user has typed.
			const std::string filter = ( mode == SuggestionMode::InlineCompletion ) ? ctx.partialToken : std::string();

			std::vector<Ranked> ranked;
			const SceneGrammar& grammar = SceneGrammar::Instance();

			switch( ctx.scope ) {
			case Scope::SceneRoot: {
				// Walk the parallel keyword / descriptor arrays so alias
				// entries (mis_pathtracing_shaderop) generate a distinct
				// suggestion with the alias as insertText, even though
				// their backing descriptor is shared with the primary
				// keyword.
				const std::vector<std::string>& kws = grammar.AllChunkKeywords();
				const std::vector<const ChunkDescriptor*>& descs = grammar.AllChunks();
				for( std::size_t i = 0; i < kws.size() && i < descs.size(); ++i ) {
					EmitChunkSuggestion( kws[i], *descs[i], filter, ranked );
				}
				break;
			}
			case Scope::InBlockParamName: {
				const ChunkDescriptor* d = grammar.FindChunk( ctx.chunkKeyword );
				if( d ) {
					// Suppress already-present non-repeatable parameters so
					// the editor does not offer a second `samples` / `name`
					// / etc. line after one has been authored.  The collector
					// deliberately includes the first-token of the caret's
					// current line, so when the user is editing an existing
					// `name foo` line from the start, the still-attached
					// trailing "name" already-authored token prevents the
					// same name being suggested twice.  Repeatable params
					// (cp, shaderop, …) are unaffected.
					const std::set<std::string> already = CollectAuthoredParamNamesInBlock( bufferText, cursorByteOffset );
					for( std::vector<ParameterDescriptor>::const_iterator it = d->parameters.begin(); it != d->parameters.end(); ++it ) {
						if( !it->repeatable && already.find( it->name ) != already.end() ) {
							continue;
						}
						EmitParameterSuggestion( *it, d->category, filter, mode, ranked );
					}
				}
				break;
			}
			case Scope::InBlockParamValue: {
				const ChunkDescriptor* d = grammar.FindChunk( ctx.chunkKeyword );
				if( d ) {
					const ParameterDescriptor* p = 0;
					for( std::vector<ParameterDescriptor>::const_iterator it = d->parameters.begin(); it != d->parameters.end(); ++it ) {
						if( it->name == ctx.paramNameOnLine ) { p = &(*it); break; }
					}
					if( p ) {
						EmitValueSuggestions( *p, bufferText, filter, ranked );
					}
				}
				break;
			}
			case Scope::InComment:
			case Scope::InDirective:
				break;
			}

			std::sort( ranked.begin(), ranked.end(), RankedLess );

			// Mark the unambiguous inline-completion hit: exactly one top-tier
			// candidate with ExactPrefix match and non-empty partial token.
			if( mode == SuggestionMode::InlineCompletion && !ctx.partialToken.empty() ) {
				if( !ranked.empty() && ranked[0].tier == MatchTier::ExactPrefix ) {
					bool exactlyOne = ( ranked.size() == 1 || ranked[1].tier != MatchTier::ExactPrefix );
					if( exactlyOne ) {
						ranked[0].suggestion.isUnambiguousCompletion = true;
					}
				}
			}

			std::vector<Suggestion> out;
			out.reserve( ranked.size() );
			for( std::vector<Ranked>::const_iterator it = ranked.begin(); it != ranked.end(); ++it ) {
				out.push_back( it->suggestion );
			}
			return out;
		}
	}
}
