//////////////////////////////////////////////////////////////////////
//
//  RawTokenCaptureTest.cpp - exercises the Phase 0 token capture
//    against the lexer rules pinned in
//    docs/ROUND_TRIP_SAVE_PLAN.md §6.2.
//
//  The capture has no parser-side state of its own; we feed it
//  hand-built lines + byte offsets and assert the tokens / spans
//  / symbolic flags it produces.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include "../src/Library/Parsers/RawTokenCapture.h"

using namespace RISE::Implementation;

namespace {

// Pretty-print a line's tokens for assertion failure messages.
std::string FormatTokens( const RawLine& ln )
{
    std::string out = "[";
    for( std::size_t i = 0; i < ln.tokens.size(); ++i ) {
        if( i > 0 ) out += ", ";
        out += "'" + ln.tokens[i].text + "'";
        if( ln.tokens[i].isSymbolic ) out += "*";
    }
    out += "]";
    return out;
}

void AssertTokenCount( const RawLine& ln, std::size_t expected, const char* test )
{
    if( ln.tokens.size() != expected ) {
        std::cerr << "FAIL " << test << ": expected " << expected
                  << " tokens, got " << ln.tokens.size()
                  << " — " << FormatTokens(ln) << std::endl;
        assert( false );
    }
}

void AssertTokenText( const RawLine& ln, std::size_t idx, const std::string& expected, const char* test )
{
    if( ln.tokens[idx].text != expected ) {
        std::cerr << "FAIL " << test << ": token[" << idx
                  << "] expected '" << expected
                  << "' got '" << ln.tokens[idx].text << "'" << std::endl;
        assert( false );
    }
}

void AssertTokenSymbolic( const RawLine& ln, std::size_t idx, bool expected, const char* test )
{
    if( ln.tokens[idx].isSymbolic != expected ) {
        std::cerr << "FAIL " << test << ": token[" << idx
                  << "] '" << ln.tokens[idx].text
                  << "' expected isSymbolic=" << (expected?"true":"false")
                  << " got " << (ln.tokens[idx].isSymbolic?"true":"false") << std::endl;
        assert( false );
    }
}

void AssertTokenSpan( const RawLine& ln, std::size_t idx,
                     std::size_t expectedBegin, std::size_t expectedEnd,
                     const char* test )
{
    if( ln.tokens[idx].byteBegin != expectedBegin || ln.tokens[idx].byteEnd != expectedEnd ) {
        std::cerr << "FAIL " << test << ": token[" << idx
                  << "] '" << ln.tokens[idx].text
                  << "' expected [" << expectedBegin << ", " << expectedEnd
                  << ") got [" << ln.tokens[idx].byteBegin << ", " << ln.tokens[idx].byteEnd
                  << ")" << std::endl;
        assert( false );
    }
}

// ----------------------------------------------------------------------

void TestPlainWhitespaceTokenization()
{
    std::cout << "TestPlainWhitespaceTokenization..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    // Offset 100 chosen arbitrarily to verify byte arithmetic uses baseOffset.
    cap.RecordLine( 100, "position 0 0 0" );
    const auto& lines = cap.AllLines();
    assert( lines.size() == 1 );
    const RawLine& ln = lines[0];
    AssertTokenCount( ln, 4, "TestPlainWhitespaceTokenization" );
    AssertTokenText( ln, 0, "position", "TestPlainWhitespaceTokenization" );
    AssertTokenText( ln, 1, "0", "TestPlainWhitespaceTokenization" );
    AssertTokenText( ln, 2, "0", "TestPlainWhitespaceTokenization" );
    AssertTokenText( ln, 3, "0", "TestPlainWhitespaceTokenization" );
    // No symbolic.
    for( const RawToken& t : ln.tokens ) {
        assert( !t.isSymbolic );
    }
    // Spans:
    //   "position" at column 0..7 (8 chars)
    //   "0"        at column 9..9 (1 char each, separated by single space)
    //   "0"        at column 11..11
    //   "0"        at column 13..13
    AssertTokenSpan( ln, 0, 100, 108, "TestPlainWhitespaceTokenization" );
    AssertTokenSpan( ln, 1, 109, 110, "TestPlainWhitespaceTokenization" );
    AssertTokenSpan( ln, 2, 111, 112, "TestPlainWhitespaceTokenization" );
    AssertTokenSpan( ln, 3, 113, 114, "TestPlainWhitespaceTokenization" );
    // Line metadata.
    assert( ln.lineBeginOffset == 100 );
    assert( ln.lineEndOffset == 100 + std::strlen("position 0 0 0") );
    assert( !ln.comment.present );
}

void TestDollarVarIsSymbolic()
{
    std::cout << "TestDollarVarIsSymbolic..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "position $x 0 0" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 4, "TestDollarVarIsSymbolic" );
    AssertTokenSymbolic( ln, 0, false, "TestDollarVarIsSymbolic" );
    AssertTokenSymbolic( ln, 1, true, "TestDollarVarIsSymbolic" );   // "$x"
    AssertTokenSymbolic( ln, 2, false, "TestDollarVarIsSymbolic" );
    AssertTokenSymbolic( ln, 3, false, "TestDollarVarIsSymbolic" );
    AssertTokenText( ln, 1, "$x", "TestDollarVarIsSymbolic" );
}

void TestBalancedDollarParenIsOneToken()
{
    std::cout << "TestBalancedDollarParenIsOneToken..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    // R2 §6 P2: $(...) must remain one token even when it contains spaces.
    // Without the balanced-paren rule a whitespace tokenizer would split
    // this into six tokens and Vec3 value-slot alignment would break.
    cap.RecordLine( 0, "position 0 $(i * 1.5) 0" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 4, "TestBalancedDollarParenIsOneToken" );
    AssertTokenText( ln, 0, "position", "TestBalancedDollarParenIsOneToken" );
    AssertTokenText( ln, 1, "0", "TestBalancedDollarParenIsOneToken" );
    AssertTokenText( ln, 2, "$(i * 1.5)", "TestBalancedDollarParenIsOneToken" );
    AssertTokenText( ln, 3, "0", "TestBalancedDollarParenIsOneToken" );
    AssertTokenSymbolic( ln, 2, true, "TestBalancedDollarParenIsOneToken" );
}

void TestNestedDollarParen()
{
    std::cout << "TestNestedDollarParen..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "x $(sin($(i*0.5))) y" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 3, "TestNestedDollarParen" );
    AssertTokenText( ln, 0, "x", "TestNestedDollarParen" );
    AssertTokenText( ln, 1, "$(sin($(i*0.5)))", "TestNestedDollarParen" );
    AssertTokenText( ln, 2, "y", "TestNestedDollarParen" );
    AssertTokenSymbolic( ln, 1, true, "TestNestedDollarParen" );
}

void TestQuotedString()
{
    std::cout << "TestQuotedString..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    // Quoted strings keep their quotes in the token text so the save
    // engine can re-emit them verbatim.
    cap.RecordLine( 0, "name \"my object\"" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 2, "TestQuotedString" );
    AssertTokenText( ln, 0, "name", "TestQuotedString" );
    AssertTokenText( ln, 1, "\"my object\"", "TestQuotedString" );
    AssertTokenSymbolic( ln, 1, false, "TestQuotedString" );
}

void TestTrailingComment()
{
    std::cout << "TestTrailingComment..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "position 0 0 0  # important value" );
    const RawLine& ln = cap.AllLines()[0];
    // Tokens stop at the `#` — comment is excluded.
    AssertTokenCount( ln, 4, "TestTrailingComment" );
    AssertTokenText( ln, 0, "position", "TestTrailingComment" );
    AssertTokenText( ln, 3, "0", "TestTrailingComment" );
    // Comment span includes the `#` and runs to lineEndOffset.
    assert( ln.comment.present );
    // The line: "position 0 0 0  # important value"
    //           0         1         2         3
    //           0123456789012345678901234567890123
    // `#` is at index 16.
    assert( ln.comment.byteBegin == 16 );
    assert( ln.comment.byteEnd == std::strlen("position 0 0 0  # important value") );
}

void TestCommentOnlyLine()
{
    std::cout << "TestCommentOnlyLine..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 50, "# this is a comment line" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 0, "TestCommentOnlyLine" );
    assert( ln.comment.present );
    assert( ln.comment.byteBegin == 50 );
}

void TestEmptyLine()
{
    std::cout << "TestEmptyLine..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 0, "TestEmptyLine" );
    assert( !ln.comment.present );
    assert( ln.lineEndOffset == 0 );
}

void TestWhitespaceOnlyLine()
{
    std::cout << "TestWhitespaceOnlyLine..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 200, "   \t  " );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 0, "TestWhitespaceOnlyLine" );
    assert( !ln.comment.present );
}

void TestCRLFLineEndingTrailingCR()
{
    std::cout << "TestCRLFLineEndingTrailingCR..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    // `getline` strips `\n` but leaves `\r` on CRLF files (POSIX behaviour).
    // The lexer must treat the trailing `\r` as whitespace so the last
    // token's byteEnd doesn't include it.
    cap.RecordLine( 0, "position 1 2 3\r" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 4, "TestCRLFLineEndingTrailingCR" );
    AssertTokenText( ln, 3, "3", "TestCRLFLineEndingTrailingCR" );
    AssertTokenSpan( ln, 3, 13, 14, "TestCRLFLineEndingTrailingCR" );
}

void TestChunkHeaderLineWithBraceOnSameLine()
{
    std::cout << "TestChunkHeaderLineWithBraceOnSameLine..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    // Some scene files put `{` on the chunk-name line:
    //   `standard_object {`
    // The lexer should produce two tokens.
    cap.RecordLine( 0, "standard_object {" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 2, "TestChunkHeaderLineWithBraceOnSameLine" );
    AssertTokenText( ln, 0, "standard_object", "TestChunkHeaderLineWithBraceOnSameLine" );
    AssertTokenText( ln, 1, "{", "TestChunkHeaderLineWithBraceOnSameLine" );
}

void TestSequenceOfLines()
{
    std::cout << "TestSequenceOfLines..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    // Three lines at offsets 0, 20, 40 (gaps account for whatever EOL
    // bytes the source file had).
    cap.RecordLine( 0, "standard_object" );
    cap.RecordLine( 20, "{" );
    cap.RecordLine( 40, "  name \"sphere\"" );
    const auto& lines = cap.AllLines();
    assert( lines.size() == 3 );
    assert( lines[0].lineBeginOffset == 0 );
    assert( lines[1].lineBeginOffset == 20 );
    assert( lines[2].lineBeginOffset == 40 );
    // The third line has leading whitespace; the `name` token starts at
    // offset 42 (40 + 2 leading spaces).
    AssertTokenSpan( lines[2], 0, 42, 46, "TestSequenceOfLines" );
}

void TestBeginSceneClearsState()
{
    std::cout << "TestBeginSceneClearsState..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "first" );
    assert( cap.AllLines().size() == 1 );
    cap.BeginScene();
    assert( cap.AllLines().size() == 0 );
    cap.RecordLine( 0, "second" );
    assert( cap.AllLines().size() == 1 );
    AssertTokenText( cap.AllLines()[0], 0, "second", "TestBeginSceneClearsState" );
}

void TestUnbalancedDollarParenRecoveryAtEOL()
{
    std::cout << "TestUnbalancedDollarParenRecoveryAtEOL..." << std::endl;
    // Malformed input: `$(` with no closing `)` before end of line.  The
    // lexer should not crash and should emit the open token text up to
    // end-of-line; the real parser will fail elsewhere (math evaluator).
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "x $(missing y" );
    const RawLine& ln = cap.AllLines()[0];
    // One way to lex this:
    //   token 0: "x"
    //   token 1: "$(missing y"  (rest of line consumed because depth > 0)
    // We accept either splitting behaviour; the important thing is that
    // the lexer terminated cleanly.
    assert( !ln.tokens.empty() );
    AssertTokenText( ln, 0, "x", "TestUnbalancedDollarParenRecoveryAtEOL" );
    // Whatever the second token text is, it must start with "$(" and
    // be flagged isSymbolic.
    assert( ln.tokens.size() >= 2 );
    assert( ln.tokens[1].text.size() >= 2 );
    assert( ln.tokens[1].text[0] == '$' && ln.tokens[1].text[1] == '(' );
    AssertTokenSymbolic( ln, 1, true, "TestUnbalancedDollarParenRecoveryAtEOL" );
}

// ----------------------------------------------------------------------
// Coverage added in response to Phase 0 adversarial review.

void TestTabIndentedLine()
{
    // P2-C: tabs in indentation are whitespace.
    std::cout << "TestTabIndentedLine..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "\t\tposition 1 2 3" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 4, "TestTabIndentedLine" );
    AssertTokenText( ln, 0, "position", "TestTabIndentedLine" );
    // `position` starts at column 2 (after two tabs).
    AssertTokenSpan( ln, 0, 2, 10, "TestTabIndentedLine" );
}

void TestHashMidTokenIsNotCommentMarker()
{
    // P1-C: `position 0 0 0# important` (no space before `#`) — the
    // existing AsciiCommandParser::TokenizeString splits only on
    // whitespace, so the `#` becomes part of the 4th token.  Our lexer
    // must match (or Phase 6.1's raw ↔ descriptor alignment breaks).
    std::cout << "TestHashMidTokenIsNotCommentMarker..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "position 0 0 0# comment" );
    const RawLine& ln = cap.AllLines()[0];
    // The line should produce: position, 0, 0, 0#, comment — five tokens.
    AssertTokenCount( ln, 5, "TestHashMidTokenIsNotCommentMarker" );
    AssertTokenText( ln, 3, "0#", "TestHashMidTokenIsNotCommentMarker" );
    AssertTokenText( ln, 4, "comment", "TestHashMidTokenIsNotCommentMarker" );
    // No comment span (no whitespace-anchored `#`).
    assert( !ln.comment.present );
}

void TestHashAtTokenBoundaryIsCommentMarker()
{
    // Confirm the regular case still works after the P1-C fix: a `#`
    // preceded by whitespace IS treated as comment-start.
    std::cout << "TestHashAtTokenBoundaryIsCommentMarker..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "position 0 0 0 # comment" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 4, "TestHashAtTokenBoundaryIsCommentMarker" );
    assert( ln.comment.present );
    // `#` is at index 15.
    assert( ln.comment.byteBegin == 15 );
}

void TestDollarInsideQuotedStringNotSymbolic()
{
    // P3-B: a `$` inside a quoted literal is data, not a macro
    // reference.  The token should NOT be flagged isSymbolic.
    std::cout << "TestDollarInsideQuotedStringNotSymbolic..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "label \"price: $5\"" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 2, "TestDollarInsideQuotedStringNotSymbolic" );
    AssertTokenText( ln, 1, "\"price: $5\"", "TestDollarInsideQuotedStringNotSymbolic" );
    AssertTokenSymbolic( ln, 1, false, "TestDollarInsideQuotedStringNotSymbolic" );
}

void TestDoubleHash()
{
    // P2-C: `## comment` — two `#` characters in a row, the first one
    // at the token boundary triggers comment recognition, the second
    // is just part of the comment text.
    std::cout << "TestDoubleHash..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "## double hash comment" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 0, "TestDoubleHash" );
    assert( ln.comment.present );
    assert( ln.comment.byteBegin == 0 );
}

void TestUnbalancedQuoteRunsToEOL()
{
    // P2-D: an unbalanced quote consumes until end-of-line.  The lexer
    // should not crash; the real parser will fail elsewhere on the
    // malformed string.
    std::cout << "TestUnbalancedQuoteRunsToEOL..." << std::endl;
    RawTokenCapture cap;
    cap.BeginScene();
    cap.RecordLine( 0, "name \"never closes" );
    const RawLine& ln = cap.AllLines()[0];
    AssertTokenCount( ln, 2, "TestUnbalancedQuoteRunsToEOL" );
    AssertTokenText( ln, 0, "name", "TestUnbalancedQuoteRunsToEOL" );
    AssertTokenText( ln, 1, "\"never closes", "TestUnbalancedQuoteRunsToEOL" );
}

}  // anonymous namespace

int main()
{
    std::cout << "===== RawTokenCaptureTest =====" << std::endl;
    TestPlainWhitespaceTokenization();
    TestDollarVarIsSymbolic();
    TestBalancedDollarParenIsOneToken();
    TestNestedDollarParen();
    TestQuotedString();
    TestTrailingComment();
    TestCommentOnlyLine();
    TestEmptyLine();
    TestWhitespaceOnlyLine();
    TestCRLFLineEndingTrailingCR();
    TestChunkHeaderLineWithBraceOnSameLine();
    TestSequenceOfLines();
    TestBeginSceneClearsState();
    TestUnbalancedDollarParenRecoveryAtEOL();
    TestTabIndentedLine();
    TestHashMidTokenIsNotCommentMarker();
    TestHashAtTokenBoundaryIsCommentMarker();
    TestDollarInsideQuotedStringNotSymbolic();
    TestDoubleHash();
    TestUnbalancedQuoteRunsToEOL();
    std::cout << "All RawTokenCapture tests PASSED" << std::endl;
    return 0;
}
