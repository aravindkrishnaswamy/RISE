//////////////////////////////////////////////////////////////////////
//
//  RawTokenCapture.h - Phase 0 of the round-trip-save pipeline.
//
//  Capture raw, pre-macro-substitution tokens from each scene-file
//  line.  Used by `SourceSpanIndex` (Phase 6.1) to record per-parameter
//  byte ranges so the save engine can decide between in-place line
//  rewrite (Mode A) and managed override block emission (Mode B).
//
//  Lexer rules (docs/ROUND_TRIP_SAVE_PLAN.md §6.2):
//    1. Split at whitespace boundaries by default.
//    2. "..." quoted strings are ONE token; whitespace inside the
//       quotes is preserved.
//    3. $(...) balanced expressions are ONE token — paren depth is
//       tracked, internal whitespace and nested $(...) are preserved
//       as part of the single token.  Required so that
//       `position 0 $(i * 1.5) 0` yields four tokens matching the
//       descriptor-driven Vec3 parser's value-slot count.
//    4. A trailing `# comment...` is excluded from token capture
//       but its byte range is recorded for Mode A splice preservation.
//    5. A token containing `$` (either `$var` short-form or `$(...)`)
//       sets the token's isSymbolic flag.
//
//  RawTokenCapture does NOT perform macro substitution; that runs
//  separately on the post-strip token array used by chunk parsers.
//
//  Scope: Phase 0 captures the raw data only.  Chunk boundary
//  indexing, per-parameter span construction, and FOR-revisit
//  detection are Phase 6.1's job — they live on top of `AllLines()`.
//
//////////////////////////////////////////////////////////////////////

#ifndef RawTokenCapture_
#define RawTokenCapture_

#include <cstddef>
#include <string>
#include <vector>

namespace RISE
{
    namespace Implementation
    {
        /// One raw-source token's location and symbolic-ness.
        struct RawToken
        {
            std::string text;        // raw bytes as authored (may contain $var, $(expr))
            std::size_t byteBegin;   // file offset of the first byte of the token
            std::size_t byteEnd;     // one past last byte of the token
            bool        isSymbolic;  // true if `text` contains '$' (macro or expression)
        };

        /// Byte range of a trailing `# comment` on a line, if any.
        /// Recorded so Mode A's splice can preserve the comment when
        /// rewriting the value portion of a parameter line.
        struct RawCommentSpan
        {
            std::size_t byteBegin;   // file offset of the '#' character
            std::size_t byteEnd;     // one past last byte (excludes EOL)
            bool        present;     // true iff the line had a `#`-comment
        };

        /// Captured tokens + spans for a single source-file line.
        struct RawLine
        {
            std::size_t              lineBeginOffset;   // file offset of the first byte of the line
            std::size_t              lineEndOffset;     // one past last content byte (excludes EOL)
            std::vector<RawToken>    tokens;
            RawCommentSpan           comment;
        };

        /// Captures raw tokens from each line of a scene file.
        /// Owned by `AsciiSceneParser` for the duration of one
        /// `ParseAndLoadScene` call; data is consumed by Phase 6.1's
        /// `SourceSpanIndex` builder, which tracks its own chunk
        /// boundary indices into `AllLines()`.
        ///
        /// FOR-loop note: the parser seeks back to a loop body via
        /// `in.seekg(...)` and re-reads the same lines for each
        /// iteration.  Each re-read produces a fresh `RawLine` entry
        /// with the SAME byte offsets but appended after prior
        /// entries.  Phase 6.1 dedupes by `(byteBegin, byteEnd)` and
        /// sets `chunkRevisited = true` on the entry that survives.
        class RawTokenCapture
        {
        public:
            RawTokenCapture();
            ~RawTokenCapture();

            /// Reset for a fresh parse.  Equivalent to Clear().
            void BeginScene();

            /// Drop all recorded state.
            void Clear();

            /// Tokenize and stash `lineBytes` (a null-terminated string
            /// returned by `std::ifstream::getline`).  `lineBeginOffset`
            /// is the byte offset of the first character of `lineBytes`
            /// in the source file.
            void RecordLine(std::size_t lineBeginOffset, const char* lineBytes);

            /// All recorded lines, in the order RecordLine was called.
            const std::vector<RawLine>& AllLines() const;

        private:
            void TokenizeOneLine(RawLine& out, std::size_t baseOffset, const char* line);

            std::vector<RawLine>  mAllLines;
        };
    }
}

#endif
