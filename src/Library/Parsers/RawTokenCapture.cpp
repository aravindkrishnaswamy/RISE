//////////////////////////////////////////////////////////////////////
//
//  RawTokenCapture.cpp - implementation of the Phase 0 token capture.
//
//  See header for lexer rules (mirrors
//  docs/ROUND_TRIP_SAVE_PLAN.md §6.2).
//
//////////////////////////////////////////////////////////////////////

#include "RawTokenCapture.h"

namespace RISE
{
    namespace Implementation
    {
        namespace
        {
            // Whitespace per the existing AsciiCommandParser::TokenizeString
            // convention: spaces, tabs, and `\r` (the trailing carriage-return
            // left behind on CRLF files after `getline` strips only `\n`).
            // `\n` is NOT in this set because lines coming from `getline`
            // have already lost their `\n`.
            inline bool IsWhitespace( char c )
            {
                return c == ' ' || c == '\t' || c == '\r';
            }
        }

        RawTokenCapture::RawTokenCapture()
        {
        }

        RawTokenCapture::~RawTokenCapture()
        {
        }

        void RawTokenCapture::BeginScene()
        {
            Clear();
        }

        void RawTokenCapture::Clear()
        {
            mAllLines.clear();
        }

        void RawTokenCapture::RecordLine( std::size_t lineBeginOffset, const char* lineBytes )
        {
            RawLine rec;
            rec.lineBeginOffset = lineBeginOffset;
            rec.comment.present = false;
            rec.comment.byteBegin = 0;
            rec.comment.byteEnd = 0;
            TokenizeOneLine( rec, lineBeginOffset, lineBytes );
            mAllLines.push_back( std::move(rec) );
        }

        const std::vector<RawLine>& RawTokenCapture::AllLines() const
        {
            return mAllLines;
        }

        void RawTokenCapture::TokenizeOneLine( RawLine& out, std::size_t baseOffset, const char* line )
        {
            // Lexer per §6.2 of the design doc.  Single forward pass over
            // the line bytes; per-token bookkeeping kept simple because
            // the data this populates is consumed by code paths that don't
            // care about partial / incremental states.
            //
            // Notes:
            //   - lineEndOffset is the offset of the first byte AFTER
            //     the last content byte (excluding EOL).  Since we don't
            //     see the EOL bytes here (getline stripped them), it's
            //     baseOffset + strlen(line).
            //   - Comment span begins at the `#` and runs to lineEndOffset.
            //   - Quoted strings keep their surrounding quotes in `text`
            //     so the save engine can re-emit them verbatim.  Mode A
            //     splices the value range INCLUDING the quotes.
            //   - $(...) tracks paren depth so nested expressions are
            //     respected.  A line ending mid-expression (unbalanced)
            //     still terminates the token at end-of-line; this is a
            //     best-effort recovery for malformed input (the real
            //     parser will fail on the math evaluator anyway).

            std::size_t i = 0;
            while( line[i] != '\0' ) ++i;
            const std::size_t lineLen = i;
            out.lineEndOffset = baseOffset + lineLen;

            std::size_t pos = 0;
            while( pos < lineLen ) {
                // Skip leading whitespace.
                while( pos < lineLen && IsWhitespace(line[pos]) ) {
                    ++pos;
                }
                if( pos >= lineLen ) break;

                // Comment marker?
                if( line[pos] == '#' ) {
                    out.comment.byteBegin = baseOffset + pos;
                    out.comment.byteEnd   = baseOffset + lineLen;
                    out.comment.present   = true;
                    return;
                }

                const std::size_t tokBegin = pos;

                // R6-review P3-B: track whether the token started with a
                // `"` so we can suppress isSymbolic for `$` inside a
                // quoted literal (it's data, not a macro reference).
                bool isQuotedToken = false;

                // Quoted string.
                if( line[pos] == '"' ) {
                    isQuotedToken = true;
                    ++pos;
                    while( pos < lineLen && line[pos] != '"' ) {
                        ++pos;
                    }
                    if( pos < lineLen ) {
                        ++pos;  // consume closing quote
                    }
                }
                // Balanced $(...) expression.
                else if( line[pos] == '$' && pos + 1 < lineLen && line[pos+1] == '(' ) {
                    pos += 2;   // consume "$("
                    int depth = 1;
                    while( pos < lineLen && depth > 0 ) {
                        if( line[pos] == '(' ) ++depth;
                        else if( line[pos] == ')' ) --depth;
                        ++pos;
                    }
                }
                // Plain whitespace-delimited token.  R6-review P1-C: do
                // NOT terminate the token at a mid-token `#` — the
                // existing `AsciiCommandParser::TokenizeString` splits
                // only on whitespace, and treats `#` as comment-start
                // ONLY at a token boundary (after whitespace).  Matching
                // that contract keeps Phase 6.1's raw-token ↔ descriptor-
                // value alignment 1:1 with whatever the chunk parser sees.
                else {
                    while( pos < lineLen && !IsWhitespace(line[pos]) ) {
                        ++pos;
                    }
                }

                const std::size_t tokEnd = pos;
                if( tokEnd > tokBegin ) {
                    RawToken t;
                    t.text.assign( line + tokBegin, line + tokEnd );
                    t.byteBegin = baseOffset + tokBegin;
                    t.byteEnd   = baseOffset + tokEnd;
                    t.isSymbolic = false;
                    if( !isQuotedToken ) {
                        // P3-B: skip the `$` scan for quoted tokens — a
                        // string literal like `"price: $5"` is data, not
                        // a macro reference.
                        for( std::size_t k = 0; k < t.text.size(); ++k ) {
                            if( t.text[k] == '$' ) {
                                t.isSymbolic = true;
                                break;
                            }
                        }
                    }
                    out.tokens.push_back( std::move(t) );
                }
            }
        }
    }
}
