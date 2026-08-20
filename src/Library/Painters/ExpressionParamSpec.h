//////////////////////////////////////////////////////////////////////
//
//  ExpressionParamSpec.h - Parses the UI-metadata-carrying form of a
//  chunk `param` line:
//
//    <name> <value> [min <a>] [max <b>] [step <s>] [label "<text>"]
//
//  This is the S1 building block for the property-panel work in the
//  procedural-texture-expressiveness arc (doc 88 S1 item 4): the
//  compiler ignores this metadata entirely (ExpressionEval's
//  Builder::AddParam only ever sees `name`/`value`), it exists purely
//  for a future inspector to render a param as a slider/field with a
//  sensible range instead of a bare text box.  Wired into two chunk
//  parsers as of S2: the `expression_painter` chunk and the
//  `expression` form of `scalar_painter` (both in
//  ChunkParserRegistry.cpp, via BuildExpressionProgramFromChunkFields
//  in ExpressionPainter.h/.cpp), both of which parse `param` lines with
//  this scanner.  expression_function2d still calls the plain
//  `<name> <number>` scanner and does not carry this metadata.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXPRESSION_PARAM_SPEC_
#define EXPRESSION_PARAM_SPEC_

#include <string>
#include <cstdlib>
#include <cstddef>
#include "../Utilities/Math3D/Math3D.h"	// Scalar

namespace RISE
{
	namespace Implementation
	{
		//! One parsed `param` line, metadata included.  `value` and
		//! `name` are always populated on success; the optional fields
		//! are only meaningful when their `has*` flag is set.
		struct ParamSpec
		{
			std::string	name;
			Scalar		value;

			bool		hasMin;  Scalar min;
			bool		hasMax;  Scalar max;
			bool		hasStep; Scalar step;
			bool		hasLabel; std::string label;

			ParamSpec() : value(0), hasMin(false), min(0), hasMax(false), max(0), hasStep(false), step(0), hasLabel(false) {}
		};

		namespace ParamSpecDetail
		{
			inline bool IsSpace( char c ) { return c==' ' || c=='\t' || c=='\n' || c=='\r'; }

			//! Skips leading whitespace in `line` starting at `i`.
			inline void SkipSpace( const std::string& line, size_t& i )
			{
				while( i < line.size() && IsSpace(line[i]) ) ++i;
			}

			//! Reads one whitespace-delimited token starting at `i` (which
			//! must not itself be whitespace); advances `i` past it.
			inline std::string ReadToken( const std::string& line, size_t& i )
			{
				const size_t start = i;
				while( i < line.size() && !IsSpace(line[i]) ) ++i;
				return line.substr( start, i - start );
			}

			//! Reads a double-quoted string starting at `line[i]=='"'`;
			//! advances `i` past the closing quote.  No escape sequences
			//! (label text is a display string, not code).  Returns false
			//! (offset at the opening quote) if the closing quote is missing.
			inline bool ReadQuoted( const std::string& line, size_t& i, std::string& out, ptrdiff_t& errOffset )
			{
				const size_t start = i;
				++i;	// opening quote
				const size_t contentStart = i;
				while( i < line.size() && line[i] != '"' ) ++i;
				if( i >= line.size() ) { errOffset = (ptrdiff_t)start; return false; }
				out = line.substr( contentStart, i - contentStart );
				++i;	// closing quote
				return true;
			}

			inline bool ParseNumberToken( const std::string& tok, Scalar& out )
			{
				if( tok.empty() ) return false;
				const char* start = tok.c_str();
				char* end = 0;
				const double v = strtod( start, &end );
				if( end != start + tok.size() ) return false;
				out = (Scalar)v;
				return true;
			}
		}

		//! Parses `line` as `<name> <value> [min <minval>] [max <maxval>]
		//! [step <stepval>] [label "<text>"]`, in any order after the first two positional
		//! fields, each keyword at most once.  On success returns true and
		//! fills `out`.  On failure returns false, fills `outError` with a
		//! human-readable message, and sets `outErrorOffset` to the byte
		//! offset within `line` the problem was found at (or -1 when not
		//! localized to one position).
		inline bool ParseParamSpecLine( const std::string& line, ParamSpec& out, std::string& outError, ptrdiff_t& outErrorOffset )
		{
			using namespace ParamSpecDetail;
			out = ParamSpec();
			outErrorOffset = -1;

			size_t i = 0;
			SkipSpace( line, i );
			if( i >= line.size() ) { outError = "empty param line"; return false; }
			const size_t nameOff = i;
			out.name = ReadToken( line, i );
			if( out.name.empty() ) { outError = "missing param name"; outErrorOffset = (ptrdiff_t)nameOff; return false; }

			SkipSpace( line, i );
			if( i >= line.size() ) { outError = "param `" + out.name + "` is missing its value"; outErrorOffset = (ptrdiff_t)i; return false; }
			const size_t valOff = i;
			const std::string valTok = ReadToken( line, i );
			if( !ParseNumberToken( valTok, out.value ) ) {
				outError = "param `" + out.name + "` value `" + valTok + "` is not a number";
				outErrorOffset = (ptrdiff_t)valOff;
				return false;
			}

			for( ;; ) {
				SkipSpace( line, i );
				if( i >= line.size() ) break;
				const size_t kwOff = i;
				const std::string kw = ReadToken( line, i );
				if( kw == "min" || kw == "max" || kw == "step" ) {
					SkipSpace( line, i );
					if( i >= line.size() ) { outError = "`" + kw + "` is missing its number"; outErrorOffset = (ptrdiff_t)i; return false; }
					const size_t numOff = i;
					const std::string numTok = ReadToken( line, i );
					Scalar v;
					if( !ParseNumberToken( numTok, v ) ) { outError = "`" + kw + " " + numTok + "` is not a number"; outErrorOffset = (ptrdiff_t)numOff; return false; }
					if( kw == "min" )  { if( out.hasMin )  { outError = "`min` specified twice"; outErrorOffset = (ptrdiff_t)kwOff; return false; } out.hasMin = true; out.min = v; }
					if( kw == "max" )  { if( out.hasMax )  { outError = "`max` specified twice"; outErrorOffset = (ptrdiff_t)kwOff; return false; } out.hasMax = true; out.max = v; }
					if( kw == "step" ) { if( out.hasStep ) { outError = "`step` specified twice"; outErrorOffset = (ptrdiff_t)kwOff; return false; } out.hasStep = true; out.step = v; }
					continue;
				}
				if( kw == "label" ) {
					SkipSpace( line, i );
					if( i >= line.size() || line[i] != '"' ) { outError = "`label` expects a \"quoted string\""; outErrorOffset = (ptrdiff_t)i; return false; }
					if( out.hasLabel ) { outError = "`label` specified twice"; outErrorOffset = (ptrdiff_t)kwOff; return false; }
					std::string text;
					ptrdiff_t qErr = -1;
					if( !ReadQuoted( line, i, text, qErr ) ) { outError = "unterminated \"label\" string"; outErrorOffset = qErr; return false; }
					out.hasLabel = true; out.label = text;
					continue;
				}
				outError = "unknown param keyword `" + kw + "`";
				outErrorOffset = (ptrdiff_t)kwOff;
				return false;
			}

			if( out.hasMin && out.hasMax && !( out.min < out.max ) ) {
				outError = "param `" + out.name + "`: min must be less than max";
				outErrorOffset = (ptrdiff_t)nameOff;
				return false;
			}
			return true;
		}
	}
}

#endif
