//////////////////////////////////////////////////////////////////////
//
//  SourceHygieneTest.cpp - mechanical guardrail against the recurring
//    "false-green test" disease.
//
//  RISE used to build with bare -ffast-math (implying -ffinite-math-only);
//  under it the compiler may assume no NaN/Inf and FOLD a NaN-sentinel
//  comparison to a constant.  A test that returns std::nan("") as a "not
//  found" sentinel and then asserts `abs(x - K) < eps` therefore silently
//  PASSES even when the lookup failed -- a false-green that hid a real
//  bug THREE times during the snapshot/transaction work (see
//  docs/skills/red-proof-and-test-integrity.md).
//
//  As of 2026-07-29 every macOS configuration also passes
//  -fno-finite-math-only, so NaN/Inf comparisons evaluate correctly again
//  and that specific folding no longer occurs (see
//  docs/INTEGRATOR_BUGFIX_FINDINGS.md §"SUPERSEDED 2026-07-29").  This
//  guardrail is deliberately KEPT anyway: it is one build-setting edit
//  away from mattering again (a -Ofast anywhere re-implies fast-math), it
//  still holds for compilers/platforms outside our four build systems,
//  and a NaN used as control flow is fragile in a test regardless of
//  whether the comparison happens to fold today.
//
//  This test scans every other tests/*.cpp for foldable not-found
//  sentinels and FAILS the suite if any is found, so the disease can
//  never reach a second file again.  A genuinely-intentional NaN/Inf use
//  (e.g. a test that verifies the renderer's own NaN handling) opts out
//  with a `// HYGIENE-OK: <reason>` comment on the same line.
//
//  It has since become the repo's home for SOURCE-GREP invariants
//  generally -- claims that live in text and cannot be checked by
//  running anything: cross-platform GUI preset parity, the IJob vtable
//  append-only manifest, the chat-render session routing, and (fix round
//  17) the two agent-facing ENUMERATION registries, which had produced a
//  stale count or a stale list in four consecutive review rounds -- and
//  which round 20 then had to repair, because the enforcement itself
//  false-positived on ordinary comments and was blind to the model-facing
//  string literals it most needed to see.  The rule for anything added
//  here: derive the expected value from the CODE, never restate it in
//  this file -- a test carrying its own copy of the list just moves the
//  drift.  Corollary the hard way: the GROUND-TRUTH parse must read CODE
//  ONLY (StripCommentsPreservingLayout), and the PROSE scan must read
//  comments AND literals (ExtractProseBlocks); getting those two backwards
//  is what made round 17's mechanism defective.
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstring>   // std::strlen -- read_schema batch-cap parity scan
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "../src/Library/Agent/Json.h"

namespace fs = std::filesystem;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

// Forbidden NaN/Inf-sentinel constructs.  Under -ffinite-math-only a NaN/Inf
// VALUE is undefined-ish and its comparisons may fold; macOS no longer sets
// that flag (see the header), but a NaN must never be used as a control-flow
// sentinel in a test regardless -- one -Ofast anywhere re-arms the fold.
static const char* kForbidden[] = {
	"std::nan(",
	"quiet_NaN(",
	"signaling_NaN(",
	"::infinity(",
};

// Locate the tests/ directory regardless of the binary's working dir
// (run_all_tests.sh runs from the repo root; ad-hoc runs may differ).
static fs::path FindTestsDir()
{
	const char* candidates[] = { "tests", "../tests", "../../tests", "../../../tests" };
	for( const char* c : candidates ) {
		fs::path p( c );
		if( fs::exists( p / "SourceHygieneTest.cpp" ) ) { return p; }
	}
	return fs::path();
}

// ---- Shared source-text helpers for the agent enumeration guards ----
// (fix round 20).  The round-17 commit (d9487763, whose own in-code markers
// said "18"; the markers now follow the commit titles, which are the only
// immutable anchor) shipped two GROUND-TRUTH parsers that read raw
// bytes, and a prose scanner that saw only `//` lines.  A reviewer red-proved
// three defects in that mechanism, all of which live here now:
//
//   * The ground-truth parsers ingested COMMENT text.  An ordinary comment
//     containing a quoted word inside IsProposeSafeVerb's body was parsed as
//     if it were a verb name (24 spurious findings); a commented-out
//     `outReason = "..."` line was parsed as a live reason value (8 spurious
//     findings).  Both told the author to change the CORRECT text.  Ground
//     truth must be CODE: StripCommentsPreservingLayout below.
//   * The prose scanner missed `/* */` blocks and, worse, MODEL-FACING string
//     literals -- the surface this whole family of findings is about.  A
//     fully-anchored WRONG claim in either was invisible.  ExtractProseBlocks
//     below covers `//` runs, block comments, and runs of adjacent string
//     literals alike.

// Replace every comment byte with a space, preserving the file's length,
// newlines, and string/char literals.  Offsets into the result therefore
// still index the original source.
static std::string StripCommentsPreservingLayout( const std::string& src )
{
	std::string out = src;
	const size_t n = src.size();
	size_t i = 0;
	while( i < n ) {
		// Comments FIRST: an apostrophe inside prose ("don't") must never be
		// mistaken for the start of a character literal.
		if( src[i] == '/' && i + 1 < n && src[i + 1] == '/' ) {
			while( i < n && src[i] != '\n' ) { out[i] = ' '; ++i; }
			continue;
		}
		if( src[i] == '/' && i + 1 < n && src[i + 1] == '*' ) {
			out[i] = ' '; out[i + 1] = ' ';
			i += 2;
			while( i < n ) {
				if( src[i] == '*' && i + 1 < n && src[i + 1] == '/' ) {
					out[i] = ' '; out[i + 1] = ' '; i += 2; break;
				}
				if( src[i] != '\n' ) { out[i] = ' '; }
				++i;
			}
			continue;
		}
		if( src[i] == '"' || src[i] == '\'' ) {
			const char quote = src[i];
			++i;
			while( i < n ) {
				if( src[i] == '\\' ) { i += 2; continue; }
				if( src[i] == quote || src[i] == '\n' ) { ++i; break; }
				++i;
			}
			continue;
		}
		++i;
	}
	return out;
}

// A run of contiguous prose: consecutive `//` lines, one `/* */` block, or a
// run of adjacent string literals (which C++ concatenates, and which is how
// every multi-line model-facing tool description in this repo is written).
struct ProseBlock
{
	std::string text;
	std::vector<std::pair<size_t,int>> starts;   // (offset in text, source line)
	const char* kind = "comment";

	int LineAt( size_t off ) const
	{
		int best = starts.empty() ? 0 : starts.front().second;
		for( const auto& s : starts ) { if( s.first <= off ) { best = s.second; } else { break; } }
		return best;
	}
	void Append( const std::string& seg, int lineNo )
	{
		if( !text.empty() ) { text += " "; }
		starts.push_back( std::make_pair( text.size(), lineNo ) );
		text += seg;
	}
};

// Decode the escapes a model-facing literal actually uses.  `\"` matters most:
// these descriptions quote verb names and JSON keys.
static std::string DecodeLiteral( const std::string& raw )
{
	std::string s;
	for( size_t i = 0; i < raw.size(); ++i ) {
		if( raw[i] != '\\' || i + 1 >= raw.size() ) { s += raw[i]; continue; }
		const char e = raw[++i];
		if( e == 'n' || e == 't' || e == 'r' ) { s += ' '; }
		else { s += e; }
	}
	return s;
}

static std::vector<ProseBlock> ExtractProseBlocks( const std::string& src )
{
	std::vector<ProseBlock> out;
	const size_t n = src.size();

	ProseBlock lineRun;  int lineRunLast = -1;
	ProseBlock strRun;   size_t strRunEnd = std::string::npos;
	lineRun.kind = "comment";
	strRun.kind  = "string";

	auto flushLines = [&]() {
		if( !lineRun.text.empty() ) { out.push_back( lineRun ); }
		lineRun = ProseBlock(); lineRun.kind = "comment"; lineRunLast = -1;
	};
	auto flushStrings = [&]() {
		if( !strRun.text.empty() ) { out.push_back( strRun ); }
		strRun = ProseBlock(); strRun.kind = "string"; strRunEnd = std::string::npos;
	};

	int line = 1;
	size_t i = 0;
	while( i < n ) {
		if( src[i] == '\n' ) { ++line; ++i; continue; }
		if( src[i] == '/' && i + 1 < n && src[i + 1] == '/' ) {
			flushStrings();
			size_t b = i + 2;
			if( b < n && src[b] == '!' ) { ++b; }
			size_t e = b;
			while( e < n && src[e] != '\n' ) { ++e; }
			if( lineRunLast >= 0 && line != lineRunLast + 1 && line != lineRunLast ) { flushLines(); }
			lineRun.Append( src.substr( b, e - b ), line );
			lineRunLast = line;
			i = e;
			continue;
		}
		if( src[i] == '/' && i + 1 < n && src[i + 1] == '*' ) {
			flushLines(); flushStrings();
			ProseBlock blk; blk.kind = "comment";
			size_t p = i + 2;
			const int startLine = line;
			size_t segBeg = p;
			int segLine = startLine;
			while( p < n ) {
				if( src[p] == '*' && p + 1 < n && src[p + 1] == '/' ) { break; }
				if( src[p] == '\n' ) {
					blk.Append( src.substr( segBeg, p - segBeg ), segLine );
					++line; segBeg = p + 1; segLine = line;
				}
				++p;
			}
			if( p > segBeg ) { blk.Append( src.substr( segBeg, p - segBeg ), segLine ); }
			if( !blk.text.empty() ) { out.push_back( blk ); }
			i = ( p < n ) ? p + 2 : n;
			continue;
		}
		if( src[i] == '\'' ) {   // character literal -- never prose
			++i;
			while( i < n ) {
				if( src[i] == '\\' ) { i += 2; continue; }
				if( src[i] == '\'' || src[i] == '\n' ) { break; }
				++i;
			}
			if( i < n && src[i] == '\n' ) { continue; }
			if( i < n ) { ++i; }
			continue;
		}
		if( src[i] == '"' ) {
			// Adjacent literals separated only by whitespace concatenate; a
			// literal reached over anything else starts a NEW run.
			if( strRunEnd != std::string::npos ) {
				bool onlySpace = true;
				for( size_t k = strRunEnd; k < i; ++k ) {
					if( !isspace( (unsigned char)src[k] ) ) { onlySpace = false; break; }
				}
				if( !onlySpace ) { flushStrings(); }
			}
			const int startLine = line;
			size_t p = i + 1;
			std::string raw;
			while( p < n ) {
				if( src[p] == '\\' && p + 1 < n ) { raw += src[p]; raw += src[p + 1]; p += 2; continue; }
				if( src[p] == '"' || src[p] == '\n' ) { break; }
				raw += src[p];
				++p;
			}
			strRun.Append( DecodeLiteral( raw ), startLine );
			if( p < n && src[p] == '\n' ) { i = p; continue; }
			strRunEnd = ( p < n ) ? p + 1 : n;
			i = strRunEnd;
			continue;
		}
		++i;
	}
	flushLines();
	flushStrings();
	return out;
}

//----------------------------------------------------------------------
// FIX 2's double-apply gate, as a TRUTH TABLE.
//
// READ THE SCOPE HONESTLY.  The function below is a TRANSLITERATION of
// the Qt driver's editRefusalIsWhollyUnapplied
// (build/VS2022/RISE-GUI/ChatPanel.cpp) onto this repo's JSON reader --
// a COPY, not the shipping predicate.  This binary links no Qt, so the
// real gate cannot be called from here and no assertion in this file can
// observe an edit to it; the only thing tying the copy to the code that
// ships is the substring pinning in main()'s FIX-2 section, where every
// condition below appears as a literal lifted from the driver's source.
// What this DOES buy: the truth table the section's prose describes is
// executable, so "which response shapes may be retried" is checked
// rather than asserted in a comment -- including the partially-applied
// batch, the shape whose mis-handling double-applies.
//
// Type-check parity is deliberate: QJsonValue::isDouble() -> isNumber(),
// isBool() -> isBool(), and toBool()/toString() on a mismatched type
// yield false / "" in both readers.
//----------------------------------------------------------------------
static bool TransliteratedEditRefusalIsWhollyUnapplied( const std::string& responseLine )
{
	using RISE::Agent::JsonValue;
	JsonValue doc;
	std::string err;
	if( !RISE::Agent::JsonParse( responseLine, doc, err ) ) return false;
	if( !doc.isObject() ) return false;
	if( doc.has( "error" ) ) return false;
	const JsonValue* resultV = doc.find( "result" );
	if( !resultV || !resultV->isObject() ) return false;
	const JsonValue& result = *resultV;

	// BATCH FIRST -- the driver's ordering.
	const JsonValue* results = result.find( "results" );
	if( results && results->isArray() ) {
		if( results->size() == 0 ) return false;
		const JsonValue* applied = result.find( "applied" );
		if( !applied || !applied->isNumber() ) return false;
		if( applied->asNumber() != 0.0 ) return false;
		for( std::size_t i = 0; i < results->size(); ++i ) {
			const JsonValue& e = results->at( i );
			if( !e.isObject() ) return false;
			const JsonValue* ea = e.find( "applied" );
			if( !ea || !ea->isBool() || ea->asBool() ) return false;
			const JsonValue* er = e.find( "retriable" );
			if( !er || !er->isBool() || !er->asBool() ) return false;
			if( e.get( "status" ).asString() != "rejected" ) return false;
		}
		return true;
	}

	const JsonValue* applied = result.find( "applied" );
	if( !applied || !applied->isBool() || applied->asBool() ) return false;
	const JsonValue* retriable = result.find( "retriable" );
	if( !retriable || !retriable->isBool() || !retriable->asBool() ) return false;
	if( result.get( "status" ).asString() != "rejected" ) return false;
	return true;
}

static void EditRefusalGateBehaviour()
{
	auto envelope = []( const std::string& body ) {
		return std::string( "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":" ) + body + "}";
	};

	struct Case { const char* what; std::string line; bool expect; };
	const std::vector<Case> cases = {
		// --- singular (propose_patch / insert_chunk / remove_chunk) ---
		{ "singular rejected+retriable -> RETRY",
		  envelope( "{\"applied\":false,\"retriable\":true,\"status\":\"rejected\"}" ), true },
		{ "singular applied -> never retry",
		  envelope( "{\"applied\":true,\"retriable\":false,\"status\":\"applied\"}" ), false },
		{ "singular rejected but PERMANENT -> never retry",
		  envelope( "{\"applied\":false,\"retriable\":false,\"status\":\"rejected\"}" ), false },
		{ "singular \"diagnosed\" (applied:false but the head MOVED) -> never retry",
		  envelope( "{\"applied\":false,\"retriable\":true,\"status\":\"diagnosed\"}" ), false },
		{ "singular with a NUMERIC applied (batch field in a singular shape) -> never retry",
		  envelope( "{\"applied\":0,\"retriable\":true,\"status\":\"rejected\"}" ), false },

		// --- batch (propose_patches / insert_chunks) ---
		{ "batch wholly refused -> RETRY",
		  envelope( "{\"applied\":0,\"total\":2,\"results\":["
		            "{\"applied\":false,\"retriable\":true,\"status\":\"rejected\"},"
		            "{\"applied\":false,\"retriable\":true,\"status\":\"rejected\"}]}" ), true },
		{ "MONEY: batch PARTIALLY applied -> never retry (re-issuing double-applies element 0)",
		  envelope( "{\"applied\":1,\"total\":2,\"results\":["
		            "{\"applied\":true,\"retriable\":false,\"status\":\"applied\"},"
		            "{\"applied\":false,\"retriable\":true,\"status\":\"rejected\"}]}" ), false },
		{ "batch whose count says 0 but an ELEMENT applied -> never retry",
		  envelope( "{\"applied\":0,\"total\":2,\"results\":["
		            "{\"applied\":true,\"retriable\":false,\"status\":\"applied\"},"
		            "{\"applied\":false,\"retriable\":true,\"status\":\"rejected\"}]}" ), false },
		{ "batch containing a \"staged\" element -> never retry",
		  envelope( "{\"applied\":0,\"total\":1,\"results\":["
		            "{\"applied\":false,\"retriable\":false,\"status\":\"staged\"}]}" ), false },
		{ "batch containing a \"diagnosed\" element (head moved) -> never retry",
		  envelope( "{\"applied\":0,\"total\":1,\"results\":["
		            "{\"applied\":false,\"retriable\":true,\"status\":\"diagnosed\"}]}" ), false },
		{ "batch with a PERMANENT rejection among the refusals -> never retry",
		  envelope( "{\"applied\":0,\"total\":2,\"results\":["
		            "{\"applied\":false,\"retriable\":true,\"status\":\"rejected\"},"
		            "{\"applied\":false,\"retriable\":false,\"status\":\"rejected\"}]}" ), false },
		{ "batch with an EMPTY results array -> never retry",
		  envelope( "{\"applied\":0,\"total\":0,\"results\":[]}" ), false },
		{ "batch whose applied is a BOOL (a number reads as a bool on both toolchains) -> never retry",
		  envelope( "{\"applied\":false,\"total\":1,\"results\":["
		            "{\"applied\":false,\"retriable\":true,\"status\":\"rejected\"}]}" ), false },
		{ "batch with a non-object element -> never retry",
		  envelope( "{\"applied\":0,\"total\":1,\"results\":[\"rejected\"]}" ), false },

		// --- malformed / non-result envelopes ---
		{ "not JSON at all -> never retry", "this is not json", false },
		{ "JSON but not an object -> never retry", "[1,2,3]", false },
		{ "JSON-RPC error envelope -> never retry",
		  "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32603,\"message\":\"boom\"}}", false },
		{ "no result member -> never retry", "{\"jsonrpc\":\"2.0\",\"id\":7}", false },
		{ "result is not an object -> never retry", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":42}", false },
		{ "result missing every gate field -> never retry", envelope( "{}" ), false },
	};

	for( const Case& c : cases ) {
		Check( TransliteratedEditRefusalIsWhollyUnapplied( c.line ) == c.expect,
		       std::string( "edit-retry gate truth table (transliterated copy): " ) + c.what );
	}
}

int main()
{
	std::cout << "=== SourceHygieneTest ===" << std::endl;

	const fs::path testsDir = FindTestsDir();
	Check( !testsDir.empty(), "tests/ directory located" );
	if( testsDir.empty() ) {
		std::cout << "  (could not find tests/ from cwd; skipping scan)" << std::endl;
		std::cout << std::endl << passCount << " passed, " << failCount << " failed." << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	std::vector<std::string> offenders;
	int scanned = 0;

	for( const auto& entry : fs::directory_iterator( testsDir ) ) {
		if( !entry.is_regular_file() ) { continue; }
		const fs::path& f = entry.path();
		if( f.extension() != ".cpp" ) { continue; }
		if( f.filename() == "SourceHygieneTest.cpp" ) { continue; }   // don't scan ourselves
		++scanned;

		std::ifstream in( f );
		std::string line;
		int lineNo = 0;
		while( std::getline( in, line ) ) {
			++lineNo;
			if( line.find( "HYGIENE-OK" ) != std::string::npos ) { continue; }
			for( const char* tok : kForbidden ) {
				const size_t tokPos = line.find( tok );
				if( tokPos == std::string::npos ) { continue; }
				// Skip comments: a NaN/Inf mentioned in a // comment is fine.
				const size_t commentPos = line.find( "//" );
				if( commentPos != std::string::npos && commentPos < tokPos ) { continue; }
				// The DISEASE is a NaN/Inf RETURNED as a not-found sentinel
				// (e.g. `if( !l ) return std::nan("");`).  A NaN/Inf used as a
				// test INPUT (constructed and passed into the code under test)
				// is legitimate, so only flag a same-line `return ... <tok>`.
				const size_t retPos = line.find( "return" );
				if( retPos == std::string::npos || retPos > tokPos ) { continue; }
				offenders.push_back(
					f.filename().string() + ":" + std::to_string( lineNo )
					+ "  ->  return " + tok );
			}
		}
	}

	Check( scanned > 0, "scanned at least one test file" );

	for( const std::string& o : offenders ) {
		std::cout << "  FORBIDDEN foldable NaN/Inf sentinel: " << o << std::endl;
	}
	Check( offenders.empty(),
	       "no -ffast-math-foldable NaN/Inf sentinels in tests/ (use a finite "
	       "poison or an explicit existence Check; see docs/skills/"
	       "red-proof-and-test-integrity.md)" );

	// ---- Start-screen starter-template sync (docs/gui/START_SCREEN.md §5.1)
	// The canonical scenes/Templates/empty_starter.RISEscene is copied into
	// each GUI build's resources (Mac: build/XCode/rise/RISE-GUI/Resources/).
	// The asset headers CLAIM a test keeps the copies identical -- this is
	// that test.  A one-sided edit would otherwise silently desync what the
	// create-with-agent path actually loads from what the repo documents.
	{
		const fs::path repoRoot = testsDir.parent_path();
		const fs::path canonical =
			repoRoot / "scenes" / "Templates" / "empty_starter.RISEscene";
		const fs::path macCopy = repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "Resources" / "empty_starter.RISEscene";
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string canonicalBytes = slurp( canonical );
		Check( !canonicalBytes.empty(),
		       "canonical starter template exists (scenes/Templates/empty_starter.RISEscene)" );
		const std::string macBytes = slurp( macCopy );
		Check( !macBytes.empty(),
		       "Mac bundle copy of the starter template exists (RISE-GUI/Resources)" );
		Check( canonicalBytes == macBytes,
		       "starter-template copies are byte-identical (edit the canonical, re-copy to Resources)" );
	}

	// ---- Agent skills are bound to the INSTALL, not to the open scene ----
	// THE BUG THIS PINS.  AgentSession::SkillsRoot() resolves
	// $RISE_SKILLS_PATH -> $RISE_MEDIA_PATH + "skills/agent/" ->
	// "./skills/agent/".  Both GUIs re-point RISE_MEDIA_PATH on EVERY scene
	// load (walking up from the scene to the nearest global.options), so
	// before this fix the agent's skills were a property of whichever scene
	// was open: build one from scratch, or open one outside a RISE project
	// tree, and all seven skills vanished -- silently, because an empty
	// index also omits the entire skills section of the system prompt.
	// Each shell now sets tier 1 ONCE at startup from its OWN location.
	// This is a source-wiring guard (neither GUI compiles in this suite);
	// the resolution ladder itself is covered portably by AgentSkillsTest.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string macApp = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "RISEApp.swift" );
		const std::string macChat = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ChatViewModel.swift" );
		const std::string macProj = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "rise.xcodeproj" / "project.pbxproj" );
		const std::string winMain = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "main.cpp" );
		const std::string winChat = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.cpp" );

		// macOS: resolved from the app's own location, installed before
		// anything can load a scene, and NEVER derived from the media path.
		const size_t macInstallDecl = macApp.find( "enum SkillsRootBootstrap" );
		const size_t macInstallCall = macApp.find( "SkillsRootBootstrap.install()" );
		Check( macInstallDecl != std::string::npos
		       && macInstallCall != std::string::npos
		       && macInstallDecl < macInstallCall
		       && macApp.find( "setenv(\"RISE_SKILLS_PATH\", root, 1)" ) != std::string::npos
		       && macApp.find( "Bundle.main.resourceURL" ) != std::string::npos
		       && macApp.find( "Bundle.main.bundleURL" ) != std::string::npos,
		       "macOS resolves RISE_SKILLS_PATH from the app itself (bundle resource, "
		       "then a walk-up) and installs it at launch" );
		// The bundle-resource tier only exists if the skills are actually
		// COPIED into the .app -- a folder reference, so there is no second
		// copy of the markdown to drift out of sync.
		Check( macProj.find( "lastKnownFileType = folder; name = skills; path = ../../../skills;" )
		       != std::string::npos
		       && macProj.find( "/* skills in Resources */" ) != std::string::npos,
		       "the Xcode RISE-GUI target copies the repo's skills/ into the app bundle" );

		// Windows: the same anchor, from applicationDirPath (no bundle).
		// UNVERIFIED -- the Qt GUI does not compile in this environment.
		Check( winMain.find( "void installSkillsRoot()" ) != std::string::npos
		       && winMain.find( "installSkillsRoot();" ) != std::string::npos
		       && winMain.find( "RISE_SKILLS_PATH" ) != std::string::npos
		       && winMain.find( "QCoreApplication::applicationDirPath()" ) != std::string::npos,
		       "Windows resolves RISE_SKILLS_PATH from the executable and installs it at launch" );

		// AND AN EMPTY INDEX IS LOUD TO THE USER on both shells -- a
		// skill-less agent is a degraded product, not a neutral state.
		Check( macChat.find( "if indexText.isEmpty {" ) != std::string::npos
		       && macChat.find( "No scene-authoring skills are loaded" ) != std::string::npos
		       && macChat.find( "skillIndexNote(fromRpcResponse:" ) != std::string::npos,
		       "macOS surfaces an empty skill index to the user as a transcript notice" );
		Check( winChat.find( "m_skillIndexEmpty = index.isEmpty();" ) != std::string::npos
		       && winChat.find( "auto* noSkills = new QLabel(text);" ) != std::string::npos
		       && winChat.find( "m_transcriptLayout->addWidget(noSkills);" ) != std::string::npos
		       && winChat.find( "No scene-authoring skills are loaded" ) != std::string::npos,
		       "Windows surfaces an empty skill index to the user in the transcript" );
	}

	// ---- N-up shell preset parity (docs/gui/RENDER_MODES.md §7.2) ----
	// The preset is intentionally shell-owned, but it must not drift between
	// macOS and Windows: a typo is accepted only as a failed setter and then
	// retried forever, while a one-sided valid edit silently gives the two
	// desktop apps different first-reveal behavior.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string mac = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "MultiPaneViewport.swift" );
		const std::string win = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ViewportWidget.cpp" );
		const std::string design = slurp( repoRoot / "docs" / "gui" / "RENDER_MODES.md" );
		const std::string ledger = slurp( repoRoot / "docs" / "gui" / "OPEN_ITEMS.md" );
		Check( mac.find( "[\"preview\", \"wireframe\", \"normals\", \"depth\"]" )
		       != std::string::npos,
		       "macOS N-up preset is exactly Preview/Wireframe/Normals/Depth" );
		Check( win.find( "{ \"preview\", \"wireframe\", \"normals\", \"depth\" }" )
		       != std::string::npos,
		       "Windows N-up preset is exactly Preview/Wireframe/Normals/Depth" );
		Check( design.find( "pane 1 = `wireframe`, pane 2 = `normals`, pane 3 =\n`depth`" )
		       != std::string::npos,
		       "RENDER_MODES documents the desktop preset exactly" );
		Check( ledger.find( "pane 1 = `wireframe`, pane 2 = `normals`, and (in Quad) pane 3\n  as `depth`" )
		       != std::string::npos,
		       "OPEN_ITEMS records the shipped desktop preset exactly" );
	}

	// ---- Post-load timeline reveal parity ----
	// The timeline used to be constructed/hidden from a load-time-only flag.
	// Keep both desktop shells wired to their existing live-scene polls so an
	// agent insertion of the first keyframe reveals it without a scene reload.
	// This is deliberately a source-wiring guard: the portable runtime test in
	// SceneEditorAnimationFramesTest covers the mutex-safe live snapshot, while
	// the platform GUI builds type-check the code reached by these markers.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string macModel = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "RenderViewModel.swift" );
		const std::string macChat = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ChatViewModel.swift" );
		const std::string macContent = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ContentView.swift" );
		const std::string macViewport = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ViewportView.swift" );
		const std::string macTimeline = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "TimelineSlider.swift" );
		const std::string macBridge = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "Bridge" / "RISEViewportBridge.mm" );
		const std::string win = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "MainWindow.cpp" );
		const std::string winHeader = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "MainWindow.h" );
		const std::string winChat = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.cpp" );
		const std::string winChatHeader = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.h" );
		const std::string winBridge = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ViewportBridge.cpp" );
		const std::string winTimeline = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ViewportTimeline.cpp" );
		const size_t winRangeBegin = winTimeline.find( "void ViewportTimeline::setRange" );
		const size_t winRangeEnd = winTimeline.find(
			"void ViewportTimeline::setAnimationFrameCount", winRangeBegin );
		const std::string winSetRange =
			( winRangeBegin != std::string::npos && winRangeEnd != std::string::npos )
			? winTimeline.substr( winRangeBegin, winRangeEnd - winRangeBegin )
			: std::string();
		Check( macModel.find( "RunLoop.main.add(timer, forMode: .common)" )
		       != std::string::npos
		       && macModel.find( "self.pollRefinementState(vb)" ) != std::string::npos
		       && macModel.find( "let liveAnimationPresence = vb.animationPresence" )
		       != std::string::npos
		       && macModel.find( "hasAnimation = liveHasAnimation" ) != std::string::npos
		       && macContent.find( "timelineVisible: viewModel.hasAnimation" )
		          != std::string::npos
		       && macContent.find( "timelineRange: viewModel.animationTimeStart...viewModel.animationTimeEnd" )
		          != std::string::npos
		       && macViewport.find( "range: timelineRange" ) != std::string::npos
		       && macBridge.find( "RISE_API_SceneEditController_GetHasAnimation(_controller, &hasAnimation)" )
		          != std::string::npos,
		       "post-load timeline reveal remains wired into the macOS live-scene poll" );
		Check( win.find( "connect(m_cstSyncTimer, &QTimer::timeout, this, &MainWindow::onCstSyncTick);" )
		       != std::string::npos
		       && win.find( "if (m_cstSyncTimer) m_cstSyncTimer->start();" )
		          != std::string::npos
		       && win.find( "const int animationPresence = m_viewportBridge->animationPresence();" )
		       != std::string::npos
		       && win.find( "m_viewportTimeline->setVisible(liveHasAnimation);" )
		          != std::string::npos
		       && winBridge.find( "RISE_API_SceneEditController_GetHasAnimation(m_controller, &hasAnimation)" )
		          != std::string::npos,
		       "post-load timeline reveal remains wired into the Windows live-scene poll" );
		Check( macModel.find( "if optionsChanged { stopPreviewPlay() }" )
		       != std::string::npos
		       && macModel.find( "reconcileSceneTime(to: clampedTime, using: vb)" )
		          != std::string::npos
		       && macModel.find( "if !optionsChanged || !manualTimelineScrubActive" )
		          != std::string::npos
		       && macViewport.find( "consumePreappliedSceneTime(newValue)" )
		          != std::string::npos
		       && macViewport.find( "endManualTimelineScrub(using: bridge)" )
		          != std::string::npos
		       && macTimeline.find( "onScrubMove(newTime)" ) != std::string::npos
		       && macTimeline.find( "onJump(t)" ) != std::string::npos,
		       "macOS live range changes stop stale playback and reconcile scene time" );
		const size_t macChatRenderFunction = macChat.find(
			"private func executeRenderToolCallAsync" );
		const size_t macChatRenderFinalize = macChat.find(
			"chatRenderWillSubmit()", macChatRenderFunction );
		const size_t macChatRenderSubmit = macChat.find(
			"vb.agentHandleToolCall(asyncLine, autonomy: pinnedAutonomy)",
			macChatRenderFinalize );
		Check( macChatRenderFunction != std::string::npos
		       && macChatRenderFinalize != std::string::npos
		       && macChatRenderSubmit != std::string::npos
		       && macChatRenderFinalize < macChatRenderSubmit
		       && macModel.find( "chat.chatRenderWillSubmit = { [weak self] in" )
		          != std::string::npos
		       && macModel.find( "self.stopPreviewPlay()" ) != std::string::npos
		       && macViewport.find( "viewModel.stopPreviewPlay()" ) != std::string::npos,
		       "macOS chat render stops timeline playback before controller submission" );
		Check( winSetRange.find( "stopPlayback();" ) != std::string::npos
		       && winSetRange.find( "if (m_scrubbing)" ) != std::string::npos
		       && winSetRange.find( "m_hasPendingRange = true;" ) != std::string::npos
		       && winTimeline.find( "applyPendingRange();" ) != std::string::npos
		       && winSetRange.find( "const double clampedTime = std::clamp(canonicalTime, m_minT, m_maxT);" )
		       != std::string::npos
		       && winSetRange.find( "jumpToTime(clampedTime);" ) != std::string::npos,
		       "Windows live range changes stop stale playback and reconcile scene time" );
		Check( win.find( "finalizeOpenTimelineInteraction();" ) != std::string::npos
		       && winTimeline.find( "void ViewportTimeline::finalizeOpenTimelineInteraction()" )
		          != std::string::npos
		       && winTimeline.find( "m_slider->setSliderDown(false);" ) != std::string::npos
		       && winTimeline.find( "emit scrubEnd();" ) != std::string::npos
		       && winTimeline.find( "m_hasPendingRange = false;" ) != std::string::npos,
		       "Windows timeline removal finalizes an open manual scrub" );
		const size_t chatRenderFinalize = winChat.find( "emit chatRenderWillSubmit();" );
		const size_t chatRenderSubmit = winChat.find(
			"m_bridge->agentHandleToolCall(asyncLine, pinnedAutonomy)",
			chatRenderFinalize );
		Check( chatRenderFinalize != std::string::npos
		       && chatRenderSubmit != std::string::npos
		       && chatRenderFinalize < chatRenderSubmit
		       && winChatHeader.find( "void chatRenderWillSubmit();" ) != std::string::npos
		       && win.find( "&ChatPanel::chatRenderWillSubmit" ) != std::string::npos
		       && win.find( "m_viewportTimeline->finalizeOpenTimelineInteraction();" )
		          != std::string::npos,
		       "Windows chat render finalizes timeline before controller submission" );
		Check( winChat.find(
			"if (m_stopRequested || !m_bridge || !m_sceneEditableExternal)" )
		       == std::string::npos
		       && winChat.find( "if (!m_renderCancellationDraining" )
		          != std::string::npos
		       && winChat.find( "&& (m_stopRequested || !m_sceneEditableExternal)" )
		          != std::string::npos,
		       "Windows chat render polls through its own occupancy gate" );
		const size_t winCancelRenderBegin = winChat.find(
			"void ChatPanel::cancelOutstandingRender()" );
		const size_t winCancelRenderEnd = winChat.find(
			"void ChatPanel::drainPendingToolCallsAsCancelled", winCancelRenderBegin );
		const std::string winCancelRender =
			( winCancelRenderBegin != std::string::npos
			  && winCancelRenderEnd != std::string::npos )
			? winChat.substr( winCancelRenderBegin,
			                  winCancelRenderEnd - winCancelRenderBegin )
			: std::string();
		Check( winCancelRender.find( "m_renderCancellationDraining = true;" )
		       != std::string::npos
		       && winCancelRender.find( "m_renderPollTimer->start();" )
		          != std::string::npos
		       && winCancelRender.find( "setOutstandingRenderJobId(0);" )
		          != std::string::npos
		       && winCancelRender.find( "if (!m_bridge)" )
		          < winCancelRender.find( "setOutstandingRenderJobId(0);" ),
		       "Windows chat render retains occupancy through cancellation drain" );

		// ---- Chat render session routing (fix round 2, P1-B) ----
		// WHY THIS IS GUARDED IN SOURCE.  `agentHandleLine` is the
		// ADMINISTRATIVE session; `agentHandleToolCall` is the
		// autonomy-selected session every OTHER chat tool call uses.  Both GUI
		// drivers used to submit and poll the chat `render` through
		// `agentHandleLine`, so -- back when the last-render PNG cache was
		// PER-SESSION -- the agent's render populated one session's cache and
		// its follow-up `read_image` read another's: byteLength 0, or the
		// stale objectmap left by query_object_at.  A model then re-rendered
		// for 3-9 turns trying to get pixels back, and in one observed run
		// judged a beauty render from a flat segmentation image.
		//
		// THE CACHE IS NO LONGER THE REASON (2026-07): the three in-app
		// sessions share ONE AgentImageCache, so the pixels line up whichever
		// of them rendered.  What still requires this routing is the state
		// deliberately NOT shared -- render_status / render_wait answer out of
		// the running session's own async-result record, so a poll on a
		// sibling returns completed:true with no `result` payload.  Same
		// markers, live rationale.
		//
		// The library-level AgentRenderAsyncTest "(img-cache)" and
		// "(shared-img-cache)" cases characterize the isolated default and
		// the opt-in sharing; both would still pass
		// if someone reverted this GUI routing entirely.  These markers are
		// what actually pin the routing.  Bodies are extracted by
		// find(begin_symbol)/find(next_symbol) so a marker cannot be
		// satisfied by an unrelated call elsewhere in the file.
		auto bodyBetween = []( const std::string& src, const char* begin,
		                       const char* next ) -> std::string {
			const size_t b = src.find( begin );
			if( b == std::string::npos ) return std::string();
			const size_t e = src.find( next, b );
			if( e == std::string::npos ) return std::string();
			return src.substr( b, e - b );
		};
		// Every render-lifecycle body must use the PINNED overload and must
		// contain NO `agentHandleLine` call at all.
		const std::string macRenderSubmitBody = bodyBetween( macChat,
			"private func executeRenderToolCallAsync", "private static func injectAsyncTrue" );
		Check( !macRenderSubmitBody.empty()
		       && macRenderSubmitBody.find( "vb.agentHandleToolCall(asyncLine, autonomy:" )
		          != std::string::npos
		       && macRenderSubmitBody.find( "vb.agentHandleToolCall(waitLine, autonomy:" )
		          != std::string::npos
		       && macRenderSubmitBody.find( "agentHandleLine" ) == std::string::npos,
		       "macOS chat render submit+poll run on the AUTONOMY-SELECTED session (agentHandleToolCall, pinned), "
		       "never the administrative agentHandleLine -- render_wait's `result` payload is PER-SESSION, so "
		       "splitting them again makes the poll complete with no result to report" );
		const std::string macRenderDrainBody = bodyBetween( macChat,
			"private func waitForChatRenderDrain(", "private func finishChatRenderOccupancy" );
		Check( !macRenderDrainBody.empty()
		       && macRenderDrainBody.find( "vb.agentHandleToolCall(waitLine, autonomy:" )
		          != std::string::npos
		       && macRenderDrainBody.find( "agentHandleLine" ) == std::string::npos,
		       "macOS chat render DRAIN poll stays on the pinned tool-call session (same per-session render_wait result reason)" );
		const std::string macRenderCancelBody = bodyBetween( macChat,
			"private func cancelAnyOutstandingChatRender()", "private func beginChatRenderDrain" );
		Check( !macRenderCancelBody.empty()
		       && macRenderCancelBody.find( "vb.agentHandleToolCall(line, autonomy:" )
		          != std::string::npos
		       && macRenderCancelBody.find( "agentHandleLine" ) == std::string::npos,
		       "macOS chat render CANCEL stays on the pinned tool-call session (same per-session render_wait result reason)" );
		const std::string winRenderSubmitBody = bodyBetween( winChat,
			"void ChatPanel::startAsyncRenderToolCall", "void ChatPanel::pollOutstandingRender()" );
		Check( !winRenderSubmitBody.empty()
		       && winRenderSubmitBody.find( "m_bridge->agentHandleToolCall(asyncLine, pinnedAutonomy)" )
		          != std::string::npos
		       && winRenderSubmitBody.find( "agentHandleLine" ) == std::string::npos,
		       "Windows chat render SUBMIT runs on the autonomy-selected session (agentHandleToolCall, pinned), "
		       "never the administrative agentHandleLine -- render_wait's `result` payload is PER-SESSION" );
		// Ends at foldInlineImageIntoRenderResult, which now sits between the
		// poll and the cancel -- so this span still means "the poll body".
		const std::string winRenderPollBody = bodyBetween( winChat,
			"void ChatPanel::pollOutstandingRender()",
			"QJsonObject ChatPanel::foldInlineImageIntoRenderResult" );
		Check( !winRenderPollBody.empty()
		       && winRenderPollBody.find( "m_bridge->agentHandleToolCall(" ) != std::string::npos
		       && winRenderPollBody.find( "agentHandleLine" ) == std::string::npos,
		       "Windows chat render POLL stays on the pinned tool-call session (same per-session render_wait result reason)" );
		Check( !winCancelRender.empty()
		       && winCancelRender.find( "m_bridge->agentHandleToolCall(" ) != std::string::npos
		       && winCancelRender.find( "agentHandleLine" ) == std::string::npos,
		       "Windows chat render CANCEL stays on the pinned tool-call session (same per-session render_wait result reason)" );
		// INVERSE regression: the ORDINARY (non-render) tool dispatch site
		// must keep using agentHandleToolCall too -- "fix" the routing by
		// moving everything to agentHandleLine and the split reappears from
		// the other direction.  A no-agentHandleLine assertion is NOT made
		// here: processNextToolCall / driveTurn carry the historical
		// explanation of the old split in a comment, deliberately.
		// ---- Shared image cache: WHO GETS THE HANDLE (2026-07) ----
		// The three IN-APP sessions share one AgentImageCache so a `render` on
		// any of them is readable by `read_image` on the others (this is what
		// closed the autonomy-flip residual).  The hosted loopback (MCP)
		// server's External session MUST NOT be in that group: a remote client
		// holding the bearer token would otherwise be able to read pixels the
		// user's own in-app render produced.  That isolation is no longer a
		// consequence of where the fields live -- it is purely a question of
		// which construction sites are passed the handle, which is exactly the
		// kind of property a source guard is for.
		//
		// Bodies extracted by symbol so a marker cannot be satisfied from an
		// unrelated part of the file.  Comment-stripped, so the prose above
		// each site cannot stand in for the argument itself.
		{
			auto countOccurrences = []( const std::string& hay, const char* needle ) -> int {
				int n = 0;
				const std::string pat( needle );
				for( size_t at = hay.find( pat ); at != std::string::npos;
				     at = hay.find( pat, at + pat.size() ) ) ++n;
				return n;
			};
			const std::string macBridgeCode = StripCommentsPreservingLayout( macBridge );
			const std::string winBridgeCode = StripCommentsPreservingLayout( winBridge );

			const std::string macInit = bodyBetween( macBridgeCode,
				"MakeSharedImageCache()", "- (NSString *)agentHandleLine:" );
			// The body STARTS at the MakeSharedImageCache() call, so the
			// anchor itself covers the declaration and the count below is the
			// three WrapJob call sites that consume it.
			Check( !macInit.empty()
			       && countOccurrences( macInit, "inAppImageCache" ) >= 3,
			       "macOS bridge: all THREE in-app sessions are constructed with the SAME "
			       "shared AgentImageCache handle" );

			const std::string macHosted = bodyBetween( macBridgeCode,
				"startAgentHostedServerWithLabel:", "- (void)stopAgentHostedServer" );
			Check( !macHosted.empty()
			       && macHosted.find( "WrapJob(pJob, RISE::Agent::AgentAuthority::External)" )
			          != std::string::npos
			       && macHosted.find( "inAppImageCache" ) == std::string::npos,
			       "macOS bridge: the HOSTED External session is constructed with NO image-cache "
			       "handle -- a remote MCP client must not be able to read in-app render pixels" );

			Check( winBridgeCode.find( "MakeSharedImageCache()" ) != std::string::npos
			       && countOccurrences( winBridgeCode, "inAppImageCache" ) >= 4,
			       "Windows bridge: all THREE in-app sessions share ONE AgentImageCache handle" );
		}

		// ---- read_schema batch-cap parity ---------------------------------
		// GROUND TRUTH IS THE CODE.  `read_schema`'s batch cap is one constant
		// (`kMaxBatch` in AgentRpc.cpp) restated in FIVE places a reader or a
		// model relies on: the wire doc (AgentRpc.h), the chat tool prose AND
		// its `keywords` parameter description (AgentChatCodecs.cpp), the MCP
		// parameter description (AgentMcpAdapter.cpp), and the system prompt
		// (AgentChatLoop.cpp).  Hand-maintaining five copies of a number is
		// exactly the drift the verb-set parity block above exists to stop, and
		// a stale copy here is a lie told to the model on every turn -- it would
		// spend a round trip discovering the real limit.
		//
		// So: parse the cap out of the code, then require every "at most <N>"
		// that sits near the word `keywords` in those files to BE that number.
		// The anchor is deliberately narrow (a specific phrase, in a window
		// around the parameter name) so unrelated prose with numbers in it is
		// invisible -- and the corollary, stated here because it is the
		// maintenance rule, is that a NEW restatement must use the "at most
		// <N>" phrasing near `keywords`, CONTIGUOUS within one string literal,
		// to buy coverage.  (A cap split across a literal boundary -- `"at most
		// " "24 ..."` -- reads fine to a model but is invisible to this scan;
		// one of the five was written that way and had to be reflowed.)
		{
			const fs::path agentDir = repoRoot / "src" / "Library" / "Agent";
			const std::string rpcCppSrc = slurp( agentDir / "AgentRpc.cpp" );
			const std::string capAnchor = "const std::size_t kMaxBatch = ";
			const size_t capAt = rpcCppSrc.find( capAnchor );
			std::string capDigits;
			if( capAt != std::string::npos ) {
				size_t d = capAt + capAnchor.size();
				while( d < rpcCppSrc.size() && isdigit( (unsigned char)rpcCppSrc[d] ) )
					capDigits += rpcCppSrc[d++];
			}
			Check( !capDigits.empty(),
			       "read_schema cap parity: parsed kMaxBatch out of AgentRpc.cpp (got \"" +
			       capDigits + "\")" );

			static const char* const kCapSurfaces[] = {
				"AgentRpc.h", "AgentChatCodecs.cpp", "AgentMcpAdapter.cpp", "AgentChatLoop.cpp" };
			const char* const kPhrase = "at most ";
			std::vector<std::string> capProblems;
			int capMentions = 0;
			for( const char* fname : kCapSurfaces ) {
				// Case-folded scan: a doc comment legitimately opens the
				// sentence with "At most 24 ...".  tolower does not touch the
				// digits, so offsets and the captured number are unaffected.
				std::string src = slurp( agentDir / fname );
				for( char& ch : src ) ch = (char)tolower( (unsigned char)ch );
				int inThisFile = 0;
				for( size_t p = src.find( kPhrase ); p != std::string::npos;
				     p = src.find( kPhrase, p + 1 ) ) {
					size_t d = p + std::strlen( kPhrase );
					std::string digits;
					while( d < src.size() && isdigit( (unsigned char)src[d] ) ) digits += src[d++];
					if( digits.empty() ) continue;
					// Only the mentions that are ABOUT this parameter.
					const size_t lo = p > 400 ? p - 400 : 0;
					const size_t hi = std::min( src.size(), p + 400 );
					if( src.substr( lo, hi - lo ).find( "keywords" ) == std::string::npos ) continue;
					++capMentions;
					++inThisFile;
					if( digits != capDigits ) {
						capProblems.push_back( std::string( fname ) + ": says \"at most " + digits +
							"\" near `keywords` but AgentRpc.cpp's kMaxBatch is " + capDigits );
					}
				}
				if( inThisFile == 0 ) {
					capProblems.push_back( std::string( fname ) +
						": states no \"at most <N>\" cap near `keywords` -- a surface that "
						"describes the batch form must state the limit, or the reader/model "
						"has to discover it by being refused" );
				}
			}
			for( const std::string& p : capProblems )
				std::cout << "  READ_SCHEMA CAP DRIFT: " << p << std::endl;
			Check( capProblems.empty() && capMentions >= 5,
			       "read_schema's batch cap is stated identically in every surface that "
			       "describes it, and equals AgentRpc.cpp's kMaxBatch (mentions checked: " +
			       std::to_string( capMentions ) + ")" );
		}

		// ---- R1c: agent rasterizer allowlist, surface parity --------------
		// GROUND TRUTH IS THE CODE.  The policy lives in three arrays in
		// AgentSession.cpp (kAgentAllowedRasterizers_ /
		// kAgentUngatedUtilityRasterizers_ / kAgentBlockedRasterizers_) and is
		// RESTATED, in prose, on the two hand-authored tool-description
		// surfaces a model actually reads: the shared chat tool defs
		// (AgentChatCodecs.cpp `kToolDefs`) and the MCP tools/list text
		// (AgentMcpAdapter.cpp).  Those two are written independently -- there
		// is no shared string -- so they drift silently, and a model told the
		// wrong allowlist burns a round trip discovering the real one, which
		// is precisely the failure the read_schema cap block above exists to
		// stop.  Neither surface is compiled against the arrays, so nothing
		// but this scan pins them.
		//
		// The rule: EVERY keyword in the allowed set and EVERY keyword in the
		// known-blocked set must appear verbatim in BOTH files.  Adding a
		// fifth allowed rasterizer, or blocking a newly added kind, therefore
		// cannot land without updating both surfaces.  (The two ungated
		// utility rasterizers are checked as a pair rather than individually
		// for the same reason -- an agent that is never told they are exempt
		// will avoid pixelpel_rasterizer, which alpha-mask scenes require.)
		{
			const fs::path agentDir = repoRoot / "src" / "Library" / "Agent";
			const std::string sessionSrc = slurp( agentDir / "AgentSession.cpp" );

			// Pull the quoted keywords out of one of the policy arrays.
			auto arrayKeywords = [&]( const char* arrayName ) {
				std::vector<std::string> out;
				const std::string anchorDecl = std::string( "kAgent" ) + arrayName + "Rasterizers_[] = {";
				const size_t at = sessionSrc.find( anchorDecl );
				if( at == std::string::npos ) return out;
				const size_t end = sessionSrc.find( "};", at );
				if( end == std::string::npos ) return out;
				const std::string body = sessionSrc.substr( at + anchorDecl.size(),
				                                            end - ( at + anchorDecl.size() ) );
				for( size_t q = body.find( '"' ); q != std::string::npos; q = body.find( '"', q + 1 ) ) {
					const size_t q2 = body.find( '"', q + 1 );
					if( q2 == std::string::npos ) break;
					out.push_back( body.substr( q + 1, q2 - q - 1 ) );
					q = q2;
				}
				return out;
			};

			const std::vector<std::string> allowedKw = arrayKeywords( "Allowed" );
			const std::vector<std::string> ungatedKw = arrayKeywords( "UngatedUtility" );
			const std::vector<std::string> blockedKw = arrayKeywords( "Blocked" );
			Check( allowedKw.size() == 4 && ungatedKw.size() == 2 && blockedKw.size() == 6,
			       "R1c allowlist parity: parsed the THREE policy arrays out of AgentSession.cpp "
			       "(allowed=" + std::to_string( allowedKw.size() ) +
			       ", ungated=" + std::to_string( ungatedKw.size() ) +
			       ", blocked=" + std::to_string( blockedKw.size() ) + ")" );

			static const char* const kPolicySurfaces[] = { "AgentChatCodecs.cpp", "AgentMcpAdapter.cpp" };
			std::vector<std::string> policyProblems;
			for( const char* fname : kPolicySurfaces ) {
				const std::string src = slurp( agentDir / fname );
				for( const std::string& kw : allowedKw )
					if( src.find( kw ) == std::string::npos )
						policyProblems.push_back( std::string( fname ) + ": never names the ALLOWED "
							"rasterizer `" + kw + "` -- a model reading this surface cannot know it "
							"may select it" );
				for( const std::string& kw : blockedKw )
					if( src.find( kw ) == std::string::npos )
						policyProblems.push_back( std::string( fname ) + ": never names the BLOCKED "
							"rasterizer `" + kw + "` -- a model reading this surface will try it and "
							"be refused" );
				bool anyUngated = false;
				for( const std::string& kw : ungatedKw ) if( src.find( kw ) != std::string::npos ) anyUngated = true;
				if( !anyUngated )
					policyProblems.push_back( std::string( fname ) + ": never mentions the DELIBERATELY "
						"UNGATED utility rasterizers -- a model will avoid pixelpel_rasterizer, which "
						"alpha-mask scenes require" );
			}
			for( const std::string& p : policyProblems )
				std::cout << "  R1C ALLOWLIST DRIFT: " << p << std::endl;
			Check( policyProblems.empty(),
			       "R1c: both hand-authored tool-description surfaces state the SAME rasterizer "
			       "allowlist AgentSession.cpp enforces" );
		}

		// ---- G2: part-plan gate, surface parity --------------------------
		// GROUND TRUTH IS THE CODE.  The closed `construction` enum lives in
		// AgentSession.cpp's kPartPlanConstructionValues, and three facts about
		// the gate are RESTATED, in prose, on the two hand-authored surfaces a
		// model actually reads: the shared chat tool defs (AgentChatCodecs.cpp
		// `kToolDefs` -- the text sent with EVERY API call) and the MCP
		// tools/list description (AgentMcpAdapter.cpp).  They are written
		// independently, share no string, and are compiled against nothing, so
		// only this scan pins them.
		//
		// The three facts are load-bearing, not decoration.  (1) The ENUM: a
		// value outside it is a -32602, so a surface that omits a value costs
		// the model a round trip discovering it -- and a surface that omits the
		// enum entirely invites free text into the one required field.  (2)
		// NON-BINDING: if a model believes the declaration constrains its later
		// authoring, it will either under-declare defensively or refuse to
		// deviate -- both of which corrupt the measurement this gate exists to
		// produce.  (3) CAPPED AT 3 REFUSALS (2026-08-10 refuse-until-filed
		// redesign, superseding the original once-per-session design): the
		// refusal asserts the cap, so both surfaces must too, or the model is
		// reading two different contracts.
		//
		// This is the G1 review's finding generalized: a caveat present on the
		// MCP schema and absent from the chat codec is absent from the text
		// that ships with every API call.
		{
			const fs::path agentDir = repoRoot / "src" / "Library" / "Agent";
			const std::string sessionSrc = slurp( agentDir / "AgentSession.cpp" );

			std::vector<std::string> enumValues;
			{
				const std::string anchorDecl = "kPartPlanConstructionValues[6] =";
				const size_t at = sessionSrc.find( anchorDecl );
				const size_t end = at == std::string::npos ? std::string::npos
				                                           : sessionSrc.find( "};", at );
				if( end != std::string::npos ) {
					const std::string body = sessionSrc.substr( at, end - at );
					for( size_t q = body.find( '"' ); q != std::string::npos; q = body.find( '"', q + 1 ) ) {
						const size_t q2 = body.find( '"', q + 1 );
						if( q2 == std::string::npos ) break;
						enumValues.push_back( body.substr( q + 1, q2 - q - 1 ) );
						q = q2;
					}
				}
			}
			Check( enumValues.size() == 6,
			       "G2 parity: parsed the closed construction enum out of AgentSession.cpp (got "
			       + std::to_string( enumValues.size() ) + ")" );

			static const char* const kPlanSurfaces[] = { "AgentChatCodecs.cpp", "AgentMcpAdapter.cpp" };
			std::vector<std::string> planProblems;
			for( const char* fname : kPlanSurfaces ) {
				const std::string src = slurp( agentDir / fname );
				if( src.find( "file_part_plan" ) == std::string::npos ) {
					planProblems.push_back( std::string( fname ) + ": never mentions file_part_plan at all" );
					continue;
				}
				// A surface satisfies the ENUM half either MECHANICALLY (it
				// builds the list from kPartPlanConstructionValues, so it
				// cannot drift) or by naming every value verbatim.  The MCP
				// adapter takes the first route and the chat codec -- whose
				// schemas are raw string literals, so it cannot -- takes the
				// second, with each value spelled inside an ESCAPED JSON
				// string (\"primitive\") in the C++ source.
				const bool derivesEnum = src.find( "kPartPlanConstructionValues" ) != std::string::npos;
				if( !derivesEnum ) {
					for( const std::string& v : enumValues )
						if( src.find( "\\\"" + v + "\\\"" ) == std::string::npos )
							planProblems.push_back( std::string( fname ) + ": never names the construction "
								"value `" + v + "` (and does not derive the list from "
								"kPartPlanConstructionValues) -- a model reading this surface will not know "
								"it is legal, and any other value is a -32602" );
				}
				if( src.find( "NOT binding" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that the part plan "
						"is NOT binding -- a model that believes otherwise will under-declare or refuse "
						"to deviate, corrupting the measurement" );
				if( src.find( "up to 3 refusals" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that the gate refuses "
						"up to 3 times before it stops intercepting -- the refusal itself asserts the cap "
						"(2026-08-10 refuse-until-filed redesign), so this surface would be a second, "
						"different contract" );
				// G3a (2026-08-10): three MORE facts, load-bearing for the same
				// reason the G2 three are.  (4) `outline` is REQUIRED with no
				// opt-out -- a surface that presents it as optional turns the
				// whole slice off for whichever provider reads that surface,
				// silently and per-provider.  (5) The 3-POINT MINIMUM: fewer is
				// a -32602, so a surface that omits the floor costs a round
				// trip discovering it.  (6) `view` EXISTS and says what it
				// means -- it is optional, so a surface that never mentions it
				// leaves a model unable to declare a side or top sketch at all.
				if( src.find( "`outline` is REQUIRED per part" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that `outline` is "
						"REQUIRED per part -- the required-ness IS the G3a mechanism (design decision 2: "
						"strictly required, no opt-out value), so a surface that presents it as optional "
						"disables the slice for every model reading that surface" );
				if( src.find( "at least 3" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state the outline's "
						"3-point minimum -- fewer points is a -32602, so omitting the floor costs a "
						"round trip to discover" );
				if( src.find( "Which axis-aligned direction this outline is drawn from." ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not describe the optional "
						"`view` field -- a model reading this surface cannot declare a side or top "
						"sketch, and every outline is then read as a front elevation" );
				// G3b (2026-08-10): SIX MORE facts (NINE, with the three the
				// fix round adds at the end of this block), on the CONSUMING side of
				// the same artifact -- `render {isolate, target}`.  Same test,
				// same list, because the sketch and the comparison are one
				// mechanism: a surface that describes the filing but not the
				// consultation leaves the target filed and never used, which
				// is a named stop-rule condition in the design doc (sec 11).
				// Each entry below is a fact a model CANNOT recover by
				// experiment without burning a call, or one whose absence
				// makes it misread what it is looking at.
				if( src.find( "REQUIRES `isolate`" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that `target` "
						"REQUIRES `isolate` -- the pairing is a -32602, so a surface that omits it "
						"costs a round trip on the very first comparison" );
				if( src.find( "looking along -Z" ) == std::string::npos ||
				    src.find( "looking along -X" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state the AXIS-ALIGNED "
						"vantages front/side/top resolve to -- a silhouette is uninterpretable "
						"without knowing which direction produced it" );
				if( src.find( "measures SHAPE ONLY, not position and not size" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that the IoU is "
						"bbox-normalized and measures SHAPE only -- a model that reads it as a "
						"placement score will move objects to chase a number that cannot move" );
				if( src.find( "REPLACES the rendered frame" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that the "
						"comparison composite REPLACES the rendered frame as the call's image -- a "
						"model expecting its beauty frame back would misread the strip it gets" );
				if( src.find( "sketch-only" ) == std::string::npos ||
				    src.find( "silhouette-only" ) == std::string::npos ||
				    src.find( "RED" ) == std::string::npos || src.find( "CYAN" ) == std::string::npos ||
				    src.find( "WHITE" ) == std::string::npos || src.find( "BLACK" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not give the overlay tile's "
						"colour legend (sketch-only RED / silhouette-only CYAN / overlap WHITE / "
						"neither BLACK) -- the third tile is unreadable without it, and the composite "
						"is the whole point of the call" );
				if( src.find( "not scores: nothing is gated on them" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that nothing is "
						"gated on the comparison numbers -- a surface that leaves that open invites "
						"the model to optimize the metric, which is exactly the Goodhart failure the "
						"design forbids (sec 5.4/sec 8)" );
				// G3b fix-round (2026-08-10) FIX 2: THREE MORE, and they exist
				// because the drift ALREADY HAPPENED -- these three caveats
				// shipped in the MCP text and were missing from the chat
				// codec, the SECOND slice running in which the codec surface
				// lost caveats the MCP text carried (G1's bboxCoverage
				// upper-bound disclosure was the first).  Pinned on BOTH
				// surfaces so this particular drift cannot recur for these
				// facts.  Each is a MISREAD, not merely a gap:
				//   (7) the two area fractions are OF THE SHARED 256x256
				//       CANVAS while the neighbouring `isolate.bboxCoverage`
				//       is OF THE FRAME -- a model that conflates them reads
				//       a shape statistic as a framing statistic and reframes
				//       to chase it.
				//   (8)/(9) `silhouetteAspect` and `thinnestAxisRatio` are
				//       CONDITIONAL keys.  A surface that lists them as if
				//       always present teaches a model to expect a key that
				//       is legitimately absent, which reads as a broken
				//       result rather than as the documented "not measurable
				//       here" it actually is.
				if( src.find( "filled fraction OF THAT SHARED CANVAS" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that "
						"sketchAreaFraction/silhouetteAreaFraction are fractions of the SHARED 256x256 "
						"CANVAS rather than of the frame -- the neighbouring isolate.bboxCoverage IS a "
						"frame fraction, so an unqualified 'area fraction' here is actively misleading" );
				if( src.find( "OMITTED when no pixel of the object landed in the frame" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that "
						"`silhouetteAspect` is OMITTED when no pixel of the object landed in the frame "
						"-- it is a conditional key, and a surface that presents it as always-present "
						"makes a legitimate omission look like a broken result" );
				if( src.find( "is OMITTED when the bounding box is unusable" ) == std::string::npos )
					planProblems.push_back( std::string( fname ) + ": does not state that "
						"`thinnestAxisRatio` is OMITTED when the bounding box is unusable -- same "
						"conditional-key contract, same misread if it is presented as always-present" );
			}
			// G3a: the parts cap is a real dispatcher rejection, and the chat
			// codec cannot derive it (its schemas are raw string literals), so
			// it spells the number.  Pin the literal against the header's
			// kPartPlanMaxParts the same way the enum above is pinned against
			// kPartPlanConstructionValues -- a bump on one side that silently
			// left the other behind would advertise a limit the dispatcher
			// does not enforce, or hide one it does.
			{
				const std::string headerSrc = slurp( agentDir / "AgentSession.h" );
				const std::string anchor = "kPartPlanMaxParts = ";
				const size_t at = headerSrc.find( anchor );
				std::string maxParts;
				if( at != std::string::npos ) {
					size_t q = at + anchor.size();
					while( q < headerSrc.size() && std::isdigit( static_cast<unsigned char>( headerSrc[q] ) ) )
						maxParts += headerSrc[q++];
				}
				Check( !maxParts.empty(),
				       "G3a parity: parsed kPartPlanMaxParts out of AgentSession.h" );
				if( !maxParts.empty() ) {
					const std::string codecSrc = slurp( agentDir / "AgentChatCodecs.cpp" );
					if( codecSrc.find( "\\\"maxItems\\\":" + maxParts ) == std::string::npos )
						planProblems.push_back( "AgentChatCodecs.cpp: its file_part_plan `parts` schema "
							"does not declare maxItems:" + maxParts + " -- the dispatcher rejects a longer "
							"list with -32602, so this surface would advertise a contract it does not have" );
					// The MCP adapter DERIVES the number from the constant, so
					// it cannot drift -- assert that it still does, rather
					// than grepping for a literal it deliberately lacks.
					const std::string mcpSrc = slurp( agentDir / "AgentMcpAdapter.cpp" );
					if( mcpSrc.find( "kPartPlanMaxParts" ) == std::string::npos )
						planProblems.push_back( "AgentMcpAdapter.cpp: no longer derives its `parts` maxItems "
							"from kPartPlanMaxParts" );
				}
			}
			// ---- Arc 77 Phase 2 (2026-08-11): the IMAGINE-SCENE surface pins.
			//
			// Same test, same problem list, same reason as the G2/G3a/G3b pins
			// above: `imagine_scene` and the render-time `sceneTarget` block it
			// switches on are ONE mechanism described by TWO hand-authored
			// surfaces, and a caveat that lands on only one of them silently
			// changes the mechanism for whichever transport reads the other.
			// The codec text is CANONICAL and the MCP text mirrors it (the
			// recorded drift-class fix); these pins are what make "mirrors"
			// checkable.
			//
			// MATCHED ON THE JOINED SOURCE, not the raw file.  Both surfaces
			// are C++ string literals wrapped across lines, and the two wrap
			// at different columns -- so a raw find() would pin the WRAPPING
			// rather than the sentence, and a harmless reflow would fail the
			// test while a real deletion could slip through a re-wrap.
			// Collapsing `" <ws> "` (adjacent-literal concatenation, the only
			// thing between the halves of a wrapped sentence) makes the pin
			// about the TEXT.
			{
				auto joinLiterals = []( const std::string& src ) {
					std::string out;
					out.reserve( src.size() );
					for( std::size_t i = 0; i < src.size(); ) {
						if( src[i] == '"' ) {
							std::size_t j = i + 1;
							while( j < src.size() &&
							       ( src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r' ) )
								++j;
							if( j > i + 1 && j < src.size() && src[j] == '"' ) { i = j + 1; continue; }
						}
						out += src[i++];
					}
					return out;
				};

				struct Pin { const char* text; const char* why; };
				// Each entry is a fact a model CANNOT recover by experiment
				// without burning a provider round trip, or one whose absence
				// makes it misread what it is looking at.
				static const Pin kImaginePins[] = {
					{ "one image from exactly that text",
					  "does not state that the provider generates the image from the model's OWN "
					  "description verbatim -- the whole mechanism is that the model authors the "
					  "target, and a surface that leaves that vague invites a one-word prompt" },
					{ "`description` is REQUIRED",
					  "does not state that `description` is REQUIRED -- an empty/absent one is a "
					  "-32602, so omitting the requirement costs a round trip to discover" },
					{ "every full-frame production render",
					  "does not state WHERE the comparison lands -- the whole design decision is "
					  "that the target rides the surface models actually visit, and a model that "
					  "does not know renders carry it will never look for it" },
					{ "not an isolate render",
					  "does not state the exclusions (draft / mode: / isolate) -- those renders "
					  "carry NO comparison, and a model expecting one there reads its absence as "
					  "a broken result" },
					{ "in place of the rendered frame",
					  "does not state that the [target | render] strip REPLACES the frame as the "
					  "call's image -- a model expecting its beauty frame back would misread the "
					  "strip it gets" },
					{ "each axis the smaller of the two, so neither is ever enlarged",
					  "does not state the shared-canvas convention -- an RMSE whose resampling "
					  "rule is unstated is a number a model cannot interpret across two renders "
					  "of different sizes" },
					{ "nothing is gated on them",
					  "does not state that nothing is gated on the comparison numbers -- leaving "
					  "that open invites optimizing the metric, the Goodhart failure the design "
					  "forbids outright (sec 5.4/sec 8)" },
					{ "Calling it again replaces this session's scene target",
					  "does not state the re-imagine REPLACE semantics -- a model that believes "
					  "targets accumulate cannot reason about which one a render measured against" },
					{ "does not generate images",
					  "does not state the capability-conditional outcome -- on a provider without "
					  "image generation the call honestly refuses, and a surface that promises an "
					  "image unconditionally turns that honest answer into an apparent failure" }
				};

				for( const char* fname : kPlanSurfaces ) {
					const std::string joined = joinLiterals( slurp( agentDir / fname ) );
					if( joined.find( "imagine_scene" ) == std::string::npos ) {
						planProblems.push_back( std::string( fname ) +
							": never mentions imagine_scene at all" );
						continue;
					}
					for( std::size_t k = 0; k < sizeof( kImaginePins ) / sizeof( kImaginePins[0] ); ++k ) {
						if( joined.find( kImaginePins[k].text ) == std::string::npos )
							planProblems.push_back( std::string( fname ) + ": " + kImaginePins[k].why );
					}
				}
			}

			for( const std::string& p : planProblems )
				std::cout << "  G2 PART-PLAN SURFACE DRIFT: " << p << std::endl;
			Check( planProblems.empty(),
			       "G2/G3a/G3b: both hand-authored tool-description surfaces state the SAME construction "
			       "enum, the SAME non-binding / capped-refusal contract, the SAME required-outline "
			       "/ optional-view schema, and the SAME render{target} comparison contract "
			       "(isolate pairing, named vantages, shape-only IoU, composite-replaces-frame, "
			       "overlay legend, nothing-is-gated, canvas-not-frame area fractions, and both "
			       "omitted-field conditions) -- and, Arc 77 Phase 2, the SAME imagine_scene "
			       "contract (description-required, generated-from-that-text, where the sceneTarget "
			       "comparison lands and where it does NOT, strip-replaces-frame, the shared-canvas "
			       "convention, nothing-is-gated, replace-on-re-imagine, and the "
			       "capability-conditional refusal)" );

			// G3b (2026-08-10): THE FILL-FRACTION AND FIT PINS.
			//
			// The comparison's whole validity rests on one invariant: the
			// sketch mask and the rendered silhouette are normalized by the
			// SAME transform.  If the two diverge the IoU stops being a
			// shape match and becomes a comparison of two framings -- a
			// plausible number, systematically wrong, with nothing failing.
			// TWO independent pins, because the invariant has two halves:
			//
			//  (1) NUMERIC -- AgentSession.h's kPartSketchFillFraction and
			//      AgentSession.cpp's kIsolateFrameFill are the same
			//      literal.  These are deliberately separate constants (one
			//      frames a 3D camera, one fits a 2D polygon) that MUST hold
			//      the same value; G3a's comments on both say "change one,
			//      change both", and this is what makes that enforceable.
			//  (2) STRUCTURAL -- both mask producers go through the one
			//      SketchFitTransform_ helper.  A shared constant alone is
			//      not enough: the centering, the letterbox and the
			//      aspect-preserving max() all have to agree too, and a
			//      re-inlined formula on one side would pass pin (1) while
			//      silently breaking the metric.
			{
				const fs::path agentDir = repoRoot / "src" / "Library" / "Agent";
				const std::string headerSrc  = slurp( agentDir / "AgentSession.h" );
				const std::string sessionSrc = slurp( agentDir / "AgentSession.cpp" );

				auto literalAfter = []( const std::string& src, const std::string& anchor ) {
					const size_t at = src.find( anchor );
					if( at == std::string::npos ) return std::string();
					size_t q = at + anchor.size();
					std::string v;
					while( q < src.size() &&
					       ( std::isdigit( static_cast<unsigned char>( src[q] ) ) || src[q] == '.' ) )
						v += src[q++];
					return v;
				};
				const std::string sketchFill  = literalAfter( headerSrc,  "kPartSketchFillFraction = " );
				const std::string isolateFill = literalAfter( sessionSrc, "kIsolateFrameFill = " );
				Check( !sketchFill.empty() && !isolateFill.empty(),
				       "G3b parity: parsed both fill-fraction literals" );
				Check( sketchFill == isolateFill,
				       "G3b: kPartSketchFillFraction (" + sketchFill + ") and kIsolateFrameFill (" +
				       isolateFill + ") MUST be the same literal -- the sketch mask and the isolate "
				       "render's silhouette are compared by IoU, and a divergence here biases every "
				       "comparison silently" );

				// Both call sites of the shared fit, plus the definition:
				// three occurrences minimum (RasterizePartOutline_,
				// NormalizeSilhouetteToCanvas_, and the function itself).
				std::size_t fitUses = 0;
				for( size_t at = sessionSrc.find( "SketchFitTransform_" );
				     at != std::string::npos;
				     at = sessionSrc.find( "SketchFitTransform_", at + 1 ) )
					++fitUses;
				Check( fitUses >= 3,
				       "G3b: BOTH mask producers still call the shared SketchFitTransform_ (found " +
				       std::to_string( fitUses ) + " references; the definition plus the sketch "
				       "rasterizer plus the silhouette normalizer is 3).  A re-inlined fit on either "
				       "side would keep the fill fraction equal and still break the IoU." );
			}
		}

		// ---- MCP tools/call image branch: no re-grown private verb list ---
		// AgentMcpAdapter.cpp's tools/call handler used to keep its OWN
		// hardcoded list of image-capable verbs, separate from
		// AgentChatCodecs.cpp's IsImageResult -- and it drifted: it omitted
		// a plain `render{imageMaxEdge}`, so that PNG reached MCP clients
		// only as base64 text buried inside the serialized JSON, not as a
		// real image content block, until the 2026-08-11 unification onto
		// the shared ChatToolResultCarriesImage predicate.  Pin BOTH halves
		// so the private list cannot silently re-grow: the adapter must
		// still route through the shared predicate, and it must not carry
		// a hand-rolled verb check that could drift from it again.
		{
			const fs::path agentDir = repoRoot / "src" / "Library" / "Agent";
			const std::string mcpSrc = slurp( agentDir / "AgentMcpAdapter.cpp" );
			// Match the CALL, not the bare name: the name also appears in
			// this file's own comments and in the adapter's, so a find() on
			// the name alone would report a false PASS for a revision that
			// reverted the routing but kept a comment mentioning it.
			Check( mcpSrc.find( "ChatToolResultCarriesImage( " ) != std::string::npos,
			       "MCP image-branch parity: AgentMcpAdapter.cpp's tools/call handler still CALLS "
			       "the shared ChatToolResultCarriesImage predicate (not merely names it in a comment)" );
			// The adapter is a pass-through: it translates envelopes and does
			// not branch on WHICH verb it is forwarding.  So no comparison of
			// toolName against any image-capable verb name has a legitimate
			// reason to exist here, in any lexical form -- and each one is a
			// private list starting to re-grow.  `render` is deliberately NOT
			// banned: unlike the other four it has non-image reasons to be
			// named (the async detour), so banning it would fire on unrelated
			// work.  A re-grown list must still name at least one of these
			// four to be a list at all.
			// Arc 77 Phase 2 (2026-08-11): `imagine_scene` joins the ban list
			// -- it is image-bearing (IsImageResult lists it) and the adapter
			// must reach it through the shared predicate, exactly like the
			// other four.
			static const char* const kImageVerbLiterals[] = {
				"read_image", "read_viewport", "compare_to_reference", "file_part_plan",
				"imagine_scene" };
			for( std::size_t v = 0; v < sizeof( kImageVerbLiterals ) / sizeof( kImageVerbLiterals[0] ); ++v ) {
				const std::string verb = kImageVerbLiterals[v];
				Check( mcpSrc.find( "toolName == \"" + verb + "\"" ) == std::string::npos &&
				       mcpSrc.find( "toolName != \"" + verb + "\"" ) == std::string::npos,
				       "MCP image-branch parity: AgentMcpAdapter.cpp has not re-grown a hardcoded "
				       "`toolName == \"" + verb + "\"` verb check -- that private list is exactly "
				       "what drifted (it omitted plain render) between G3a and the 2026-08-11 "
				       "unification onto the shared predicate" );
			}
		}

		// ---- render{imageMaxEdge} through the drivers' async detour -------
		// WHY THIS IS GUARDED IN SOURCE.  `render{imageMaxEdge:N}` returns the
		// PNG inline so an ordinary look costs ONE turn.  The RPC refuses that
		// parameter alongside `async` -- right for a raw async caller, since
		// the submit returns before pixels exist -- but BOTH GUI drivers
		// transparently inject `{"async":true}` into EVERY render, and the
		// model is never taught `async`.  So the pair collided on a call the
		// model IS taught to make and the one-call form was unreachable from
		// the only transport it was built for: in the recorded 2026-07-29 GUI
		// trajectory the model called render{imageMaxEdge:192,samples:32}, got
		// -32602, and reverted to render + read_image for all five later looks.
		//
		// The fix is DRIVER-SIDE (strip the parameter before injecting async,
		// re-apply its effect after completion), and driver source is not
		// compiled by any test -- so these markers are what pin it.  The
		// library-level sequence proof lives in AgentObjectMapTest's
		// "drivers' async detour" case; it would still pass if either driver
		// dropped the staging entirely.
		//
		// Six properties per platform: (1) the line SUBMITTED is the STAGED
		// one, not the model's raw line -- otherwise the collision is back;
		// (2) the fold runs on the JOB-PINNED session, never the
		// administrative agentHandleLine -- NOT because a sibling could not
		// see the pixels (the three in-app sessions share one AgentImageCache,
		// asserted below) but so the fold runs under the same session and
		// posture as the render it belongs to, like every other call in that
		// job's lifecycle; (3) the fold is guarded on the render having
		// SUCCEEDED, or a failed render would ship the previous frame's pixels
		// as its own; (4) objectmap is NOT staged, so the RPC still refuses it
		// rather than the driver silently downscaling an identity map; (5) the
		// fold uses the MODEL'S staged bound, not a hardcoded one -- nothing
		// else would catch `maxEdge: 192` baked in, since the library-level
		// sequence test takes the bound as a parameter; (6) the completion
		// path actually CALLS the fold -- an orphaned helper compiles clean on
		// both platforms (an unused `private static func` draws no Swift
		// diagnostic), so only a marker catches its removal.
		{
			const std::string macStageBody = bodyBetween( macChat,
				"private static func stageInlineImageMaxEdge",
				"private static func foldingInlineImage" );
			const std::string macFoldBody = bodyBetween( macChat,
				"private static func foldingInlineImage",
				"private static func makeSyntheticResponseLine" );
			Check( macRenderSubmitBody.find(
			           "Self.injectAsyncTrue(intoJsonRpcLine: staged.line)" ) != std::string::npos
			       && macRenderSubmitBody.find( "Self.stageInlineImageMaxEdge(fromJsonRpcLine: submitLine)" )
			          != std::string::npos,
			       "macOS chat render submits the STAGED line (imageMaxEdge removed) to the async "
			       "wire form -- submitting the model's raw line reinstates the -32602 collision" );
			Check( !macStageBody.empty()
			       && macStageBody.find( "objectmap" ) != std::string::npos,
			       "macOS staging leaves imageMaxEdge in place for mode:\"objectmap\", so the RPC "
			       "still refuses it (a downscale breaks the identity-colour legend match)" );
			Check( !macStageBody.empty()
			       && macStageBody.find( "2147483647" ) != std::string::npos,
			       "macOS staging mirrors ParseClampedUInt's ACCEPTED WINDOW -- staging a value "
			       "the RPC would reject (not clamp) yields a render with no image and no error" );
			Check( !macFoldBody.empty()
			       && macFoldBody.find( "vb.agentHandleToolCall(readLine, autonomy:" ) != std::string::npos
			       && macFoldBody.find( "agentHandleLine" ) == std::string::npos,
			       "macOS inline-image fetch runs on the JOB-PINNED tool-call session, like every "
			       "other call in that job's lifecycle -- never the administrative agentHandleLine" );
			Check( !macFoldBody.empty()
			       && macFoldBody.find( "(result[\"ok\"] as? Bool) == true" ) != std::string::npos,
			       "macOS inline-image fold is guarded on ok==true -- a failed or cancelled render "
			       "must attach nothing, not the PREVIOUS frame still in the cache" );
			Check( !macFoldBody.empty()
			       && macFoldBody.find( "\\(maxEdge)}}" ) != std::string::npos,
			       "macOS inline-image fetch uses the MODEL'S staged bound, not a hardcoded one" );
			Check( macRenderSubmitBody.find( "Self.foldingInlineImage(" ) != std::string::npos
			       && macRenderSubmitBody.find( "staged.inlineImageMaxEdge" ) != std::string::npos,
			       "macOS completion path actually CALLS the fold with the staged bound -- an "
			       "orphaned helper draws no Swift diagnostic, so only this catches its removal" );

			const std::string winStageBody = bodyBetween( winChat,
				"QString stripInlineImageMaxEdge", "QString makeSyntheticResponseLine" );
			const std::string winFoldBody = bodyBetween( winChat,
				"QJsonObject ChatPanel::foldInlineImageIntoRenderResult",
				"void ChatPanel::cancelOutstandingRender()" );
			Check( winRenderSubmitBody.find( "injectAsyncTrue(stagedLine)" ) != std::string::npos
			       && winRenderSubmitBody.find( "stripInlineImageMaxEdge(" ) != std::string::npos,
			       "Windows chat render submits the STAGED line (imageMaxEdge removed) to the async "
			       "wire form -- submitting the model's raw line reinstates the -32602 collision" );
			Check( !winStageBody.empty()
			       && winStageBody.find( "objectmap" ) != std::string::npos,
			       "Windows staging leaves imageMaxEdge in place for mode:\"objectmap\", so the RPC "
			       "still refuses it" );
			Check( !winStageBody.empty()
			       && winStageBody.find( "2147483647" ) != std::string::npos,
			       "Windows staging mirrors ParseClampedUInt's ACCEPTED WINDOW (same reason)" );
			Check( !winFoldBody.empty()
			       && winFoldBody.find( "m_bridge->agentHandleToolCall(" ) != std::string::npos
			       && winFoldBody.find( "m_outstandingRenderAutonomy" ) != std::string::npos
			       && winFoldBody.find( "agentHandleLine" ) == std::string::npos,
			       "Windows inline-image fetch runs on the JOB-PINNED tool-call session, like every "
			       "other call in that job's lifecycle" );
			Check( !winFoldBody.empty()
			       && winFoldBody.find( "{ \"maxEdge\", maxEdge }" ) != std::string::npos
			       && winFoldBody.find( "m_outstandingRenderInlineImageMaxEdge" ) != std::string::npos,
			       "Windows inline-image fetch uses the MODEL'S staged bound, not a hardcoded one" );
			Check( !winFoldBody.empty()
			       && winFoldBody.find( "renderResult.value(\"ok\").toBool(false)" ) != std::string::npos,
			       "Windows inline-image fold is guarded on ok==true -- a failed or cancelled render "
			       "must attach nothing" );
			Check( !winChat.empty()
			       && winChat.find( "foldInlineImageIntoRenderResult(waitResult.value(\"result\").toObject())" )
			          != std::string::npos,
			       "Windows completion path folds the inline image into the downgraded result" );

			// The RPC-level refusal is NOT weakened by any of the above: it is
			// still right for a genuine async caller (`rise --agent-stdio`, an
			// MCP client), which has no driver to re-apply the effect.
			const std::string rpcSrc = slurp( repoRoot / "src" / "Library" / "Agent" / "AgentRpc.cpp" );
			Check( rpcSrc.find( "'imageMaxEdge' is not supported with 'async'" ) != std::string::npos,
			       "the RPC still refuses imageMaxEdge+async for RAW async callers -- the drivers "
			       "work around it by staging, they do not weaken it" );
		}

		const std::string winOrdinaryToolBody = bodyBetween( winChat,
			"void ChatPanel::processNextToolCall()", "void ChatPanel::startAsyncRenderToolCall" );
		Check( !winOrdinaryToolBody.empty()
		       && winOrdinaryToolBody.find( "m_bridge->agentHandleToolCall(toQString(line))" )
		          != std::string::npos,
		       "Windows ORDINARY tool dispatch still uses agentHandleToolCall -- render and read_image must share "
		       "ONE session, in both directions" );
		// macOS: attempt 1 is dispatched INLINE at driveTurn's routing site
		// (`let firstResponse = ...`), and the FIX-2 retry helper re-issues
		// the SAME line on the same selector.  Both are asserted so neither
		// half can be moved onto agentHandleLine.
		const std::string macEditRetryBody = bodyBetween( macChat,
			"private func retryWhollyRefusedEditToolCall",
			"private func executeRenderToolCallAsync" );
		Check( macChat.find( "let firstResponse = vb.agentHandleToolCall(line)" ) != std::string::npos
		       && !macEditRetryBody.empty()
		       && macEditRetryBody.find( "responseLine = vb.agentHandleToolCall(line)" )
		          != std::string::npos
		       && macEditRetryBody.find( "agentHandleLine" ) == std::string::npos,
		       "macOS ORDINARY tool dispatch still uses agentHandleToolCall -- render and read_image must share "
		       "ONE session, in both directions" );
		const size_t winDestructor = win.find( "MainWindow::~MainWindow()" );
		const size_t winDestructorEnd = win.find(
			"// ============================================================", winDestructor );
		const std::string winDestructorBody =
			( winDestructor != std::string::npos && winDestructorEnd != std::string::npos )
			? win.substr( winDestructor, winDestructorEnd - winDestructor )
			: std::string();
		Check( winDestructor != std::string::npos
		       && win.find( "MainWindow::~MainWindow()", winDestructor + 1 ) == std::string::npos
		       && winHeader.find( "~MainWindow() override;" ) != std::string::npos
		       && winHeader.find( "~MainWindow() override;",
		                          winHeader.find( "~MainWindow() override;" ) + 1 )
		          == std::string::npos
		       && winDestructorBody.find( "m_engine->cancelAndJoinInFlightWork();" )
		          != std::string::npos
		       && winDestructorBody.find( "teardownViewport();" ) != std::string::npos,
		       "Windows MainWindow has one ordered shutdown path" );
	}

	// ---- IJob vtable append-only manifest (round-4 review, 2026-07-22) ----
	// IJob is a public abstract interface: its virtual DECLARATION ORDER is
	// the vtable ABI.  The append-only convention lived only in tail comments
	// and was violated (a new virtual landed mid-vtable next to its semantic
	// sibling, shifting every later slot).  This makes the convention
	// MECHANICAL: extract the ordered virtual names from IJob.h and compare
	// against tests/IJobVtableManifest.txt.  A legal tail append = one new
	// line at the END of the manifest, same commit.  A mid-insert / reorder /
	// removal mismatches at some index and fails the suite.
	{
		const fs::path repoRoot = testsDir.parent_path();
		const fs::path header = repoRoot / "src" / "Library" / "Interfaces" / "IJob.h";
		const fs::path manifestPath = testsDir / "IJobVtableManifest.txt";

		// Extract the ordered virtual-method names from `class IJob`'s body.
		// Brace-count CODE only (strip comments first -- doc text contains
		// braces); skip the destructor (a `~` before the name).  Round 20:
		// this uses the shared stripper, so a `/* */` block carrying a brace
		// or a `virtual` cannot desynchronise the walk either.
		std::vector<std::string> extracted;
		{
			std::ifstream raw( header );
			std::istringstream in( StripCommentsPreservingLayout(
				std::string( std::istreambuf_iterator<char>( raw ),
				             std::istreambuf_iterator<char>() ) ) );
			std::string line;
			bool inClass = false, started = false;
			int depth = 0;
			while( std::getline( in, line ) ) {
				const std::string& code = line;
				if( !inClass ) {
					const size_t k = code.find( "class IJob" );
					if( k != std::string::npos
					 && ( code.size() <= k + 10 || !isalnum( (unsigned char)code[k + 10] ) )
					 && code.find( ';' ) == std::string::npos ) {
						inClass = true;
						for( char c : code ) { if( c == '{' ) ++depth; else if( c == '}' ) --depth; }
						started = depth > 0;
					}
					continue;
				}
				for( char c : code ) { if( c == '{' ) ++depth; else if( c == '}' ) --depth; }
				if( !started && depth > 0 ) started = true;
				if( started && depth <= 0 ) break;
				// A declaration line: first token after stripping tabs is `virtual`.
				size_t b = code.find_first_not_of( " \t" );
				if( b == std::string::npos ) continue;
				if( code.compare( b, 8, "virtual " ) != 0 ) continue;
				const size_t paren = code.find( '(', b );
				if( paren == std::string::npos ) continue;
				if( code.rfind( '~', paren ) != std::string::npos
				 && code.rfind( '~', paren ) > b ) continue;   // destructor
				size_t e = paren;
				while( e > b && ( code[e-1] == ' ' || code[e-1] == '\t' ) ) --e;
				size_t s = e;
				while( s > b && ( isalnum( (unsigned char)code[s-1] ) || code[s-1] == '_' ) ) --s;
				if( s < e ) extracted.push_back( code.substr( s, e - s ) );
			}
		}
		Check( extracted.size() > 200,
		       "IJob.h parsed: extracted the virtual-method order (sanity: >200 methods)" );

		std::vector<std::string> manifest;
		{
			std::ifstream in( manifestPath );
			std::string line;
			while( std::getline( in, line ) ) {
				if( line.empty() || line[0] == '#' ) continue;
				manifest.push_back( line );
			}
		}
		Check( !manifest.empty(), "tests/IJobVtableManifest.txt loaded" );

		size_t firstDiff = 0;
		const size_t common = std::min( extracted.size(), manifest.size() );
		while( firstDiff < common && extracted[firstDiff] == manifest[firstDiff] ) ++firstDiff;
		if( firstDiff < common ) {
			std::cout << "  IJob VTABLE ORDER MISMATCH at slot " << firstDiff
			          << ": header has `" << extracted[firstDiff]
			          << "`, manifest has `" << manifest[firstDiff] << "`" << std::endl
			          << "  A new IJob virtual must be APPENDED at the class tail (append-only"
			          << " vtable ABI); a rename/removal is an ABI break -- see"
			          << " abi-preserving-api-evolution." << std::endl;
		}
		Check( firstDiff == common,
		       "IJob virtual order matches the manifest prefix (no mid-vtable insert/reorder)" );
		if( extracted.size() < manifest.size() ) {
			std::cout << "  IJob.h is MISSING manifest tail entries (removal = ABI break):"
			          << " first missing `" << manifest[extracted.size()] << "`" << std::endl;
		} else if( extracted.size() > manifest.size() ) {
			std::cout << "  NEW IJob tail virtual(s) not yet in the manifest -- append `"
			          << extracted[manifest.size()]
			          << "` (and any after it) to tests/IJobVtableManifest.txt in this commit."
			          << std::endl;
		}
		Check( extracted.size() == manifest.size(),
		       "IJob virtual count matches the manifest (tail appends update the manifest consciously)" );
	}

	// ---- Agent verb-set enumeration parity (fix rounds 17, 20) ---------
	// WHY THIS EXISTS.  Nine review rounds on fix/gui-agent-chattiness; the
	// executable code has been clean since round 2 and EVERY finding for the
	// last five rounds was a doc/claim mismatch -- four of them COUNT or
	// ENUMERATION drift in the agent-facing comment family.  Round 13
	// declared this exact sweep done, fixed two instances, and missed the
	// one attached to IsReadSafeVerb itself; round 17 found that one plus
	// six siblings ("the 3 mutating verbs" where there are five, a
	// blind-edit mutation set missing its batch forms).  Hand-fixing the
	// cited number each round is what makes it recur, so the class is
	// mechanized here instead.
	//
	// GROUND TRUTH IS THE CODE, NOT A LIST IN THIS FILE.  The verb sets are
	// parsed out of AgentRpc.cpp's IsReadSafeVerb / IsProposeSafeVerb
	// bodies -- the two functions the dispatcher's autonomy choke point
	// actually calls.  A hardcoded copy here would just relocate the drift.
	//
	// WHAT IS ENFORCED (deliberately narrow, so unrelated prose edits do not
	// trip it -- an empirical sweep of src/Library/Agent showed a blanket
	// "any list of 3+ verb names" rule false-positives on the many LEGITIMATE
	// subsets, e.g. "the 3 single-item verbs", "read_document/read_schema/
	// read_skill/validate"):
	//   (A) A CLAIM of the form "<N> [<prefix>-]mutating verb(s)/tool(s)":
	//       N must equal the size of IsProposeSafeVerb's set, and when the
	//       claim is immediately followed by a parenthesised list of verb
	//       names, that list must BE that set.
	//   (B) A list introduced by "read-safe allowlist (" or by
	//       "IsReadSafeVerb -- " must be exactly IsReadSafeVerb's set.
	// Anything not carrying one of those anchors is left alone: an author
	// naming a subset in prose is not making a set claim.  The corollary --
	// stated here because it is the maintenance rule -- is that a NEW remote
	// restatement should use one of these anchor phrasings so it is covered.
	//
	// WHERE IT LOOKS (round 20 widened this; round 17's scanner saw `//`
	// lines ONLY; FIX 5b, 2026-08-09, widened it again to the GUI bridge
	// sources -- see the DISCLOSED RESIDUAL note and the scan below).  Every
	// prose run in src/Library/Agent/*.{cpp,h} (part (A) ALSO runs, narrowed,
	// over build/XCode/rise/RISE-GUI/**/*.{swift,h,mm} and
	// build/VS2022/RISE-GUI/**/*.{h,cpp}):
	//   * runs of consecutive `//` lines,
	//   * `/* */` blocks, and
	//   * runs of adjacent string literals -- the MODEL-FACING tool
	//     descriptions, which is where the finding that started this family
	//     actually lived and which round 17 could not see at all.
	// A finding in a literal is tagged "(MODEL-FACING literal)" so the author
	// knows the text a model reads is what is wrong.
	//
	// DISCLOSED RESIDUAL, precisely.  (1) UN-ANCHORED prose: a bare
	// "propose_patch/insert_chunk/remove_chunk" with no counted-mutating or
	// read-safe-allowlist anchor is invisible by design (the anchors are what
	// keep the many legitimate subsets from false-positiving).  Rephrase such
	// a restatement into an anchor form to buy coverage.  (2) Files outside
	// src/Library/Agent -- the GUI bridges' own host-loop/bridge sources
	// (build/XCode/rise/RISE-GUI/**/*.{swift,h,mm}, build/VS2022/RISE-GUI/**/*.{h,cpp})
	// are ALSO scanned since FIX 5b (2026-08-09 R1 fix round), but for part (A)'s
	// counted-mutating-verb claim ONLY -- not part (B)'s read-safe-allowlist
	// enumeration.  This is CODE-ENFORCED (FIX C, round-3 fix, 2026-08-09):
	// part (B) is gated on `isAgentFile` inside the scan loop below, not just
	// documented here -- an earlier draft of this loop ran BOTH parts over
	// the full agentFiles+guiFiles union with no per-file branch, and did not
	// false-positive only because existing GUI prose happened to be followed
	// by capitalized identifiers that parseRun's lowercase-only isVerbChar
	// rejects, which is luck, not enforcement.  docs and skills restate these
	// sets too and remain UNSCANNED here; only the reason-code registry below
	// scans repo-wide.  (3) Raw string literals (`R"(...)"`) are not decoded;
	// none exist in this tree.
	{
		const fs::path repoRoot = testsDir.parent_path();
		const fs::path agentDir = repoRoot / "src" / "Library" / "Agent";
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string rpcCpp = slurp( agentDir / "AgentRpc.cpp" );
		Check( !rpcCpp.empty(), "verb-parity: AgentRpc.cpp read" );
		// GROUND TRUTH IS CODE, NOT COMMENT TEXT (fix round 20).  These
		// predicates are heavily commented, and a comment inside the body
		// that merely QUOTES a word used to be parsed as a verb name -- a
		// false positive that told the author to "fix" correct comments.
		const std::string rpcCode = StripCommentsPreservingLayout( rpcCpp );

		// The ordered string literals inside a `bool <fn>( ... )` body.
		auto verbsIn = [&]( const char* fn ) -> std::vector<std::string> {
			std::vector<std::string> v;
			const std::string sig = std::string( "bool " ) + fn + "( const std::string& method )";
			const size_t s = rpcCode.find( sig );
			if( s == std::string::npos ) { return v; }
			const size_t open = rpcCode.find( '{', s );
			if( open == std::string::npos ) { return v; }
			int depth = 0; size_t end = open;
			for( size_t i = open; i < rpcCode.size(); ++i ) {
				if( rpcCode[i] == '{' ) { ++depth; }
				else if( rpcCode[i] == '}' ) { if( --depth == 0 ) { end = i; break; } }
			}
			for( size_t i = open; i < end; ++i ) {
				if( rpcCode[i] != '"' ) { continue; }
				const size_t q = rpcCode.find( '"', i + 1 );
				if( q == std::string::npos ) { break; }
				v.push_back( rpcCode.substr( i + 1, q - i - 1 ) );
				i = q;
			}
			return v;
		};
		const std::vector<std::string> readSafe = verbsIn( "IsReadSafeVerb" );
		const std::vector<std::string> proposeSafe = verbsIn( "IsProposeSafeVerb" );
		Check( readSafe.size() >= 5,
		       "verb-parity: parsed IsReadSafeVerb's body (sanity: >=5 verbs; got "
		       + std::to_string( readSafe.size() ) + ")" );
		Check( proposeSafe.size() >= 3,
		       "verb-parity: parsed IsProposeSafeVerb's body (sanity: >=3 verbs; got "
		       + std::to_string( proposeSafe.size() ) + ")" );

		std::vector<std::string> readSorted = readSafe, proposeSorted = proposeSafe;
		std::sort( readSorted.begin(), readSorted.end() );
		std::sort( proposeSorted.begin(), proposeSorted.end() );
		{
			std::vector<std::string> both;
			std::set_intersection( readSorted.begin(), readSorted.end(),
			                       proposeSorted.begin(), proposeSorted.end(),
			                       std::back_inserter( both ) );
			Check( both.empty(),
			       "verb-parity: the read-safe and mutating sets are DISJOINT (a verb in "
			       "both would make the Read-posture refusal meaningless)" );
		}
		std::vector<std::string> known = readSorted;
		known.insert( known.end(), proposeSorted.begin(), proposeSorted.end() );
		std::sort( known.begin(), known.end() );
		auto isKnownVerb = [&]( const std::string& t ) {
			return std::binary_search( known.begin(), known.end(), t );
		};
		auto join = []( const std::vector<std::string>& v ) {
			std::string s;
			for( size_t i = 0; i < v.size(); ++i ) { if( i ) s += "/"; s += v[i]; }
			return s;
		};

		auto lower = []( std::string s ) {
			for( char& ch : s ) { ch = (char)tolower( (unsigned char)ch ); }
			return s;
		};
		auto isVerbChar = []( char ch ) {
			return islower( (unsigned char)ch ) || isdigit( (unsigned char)ch ) || ch == '_';
		};
		// Parse the maximal `/`- or `,`-separated run of KNOWN verb names
		// starting at `i` (skipping leading space and one `(`).  Empty when
		// the text there is not a verb list at all -- e.g. "(IsProposeSafeVerb)"
		// or "(the 3 single-item verbs ...", both of which must be left alone.
		auto parseRun = [&]( const std::string& b, size_t i, bool requireParen ) {
			std::vector<std::string> run;
			while( i < b.size() && isspace( (unsigned char)b[i] ) ) { ++i; }
			if( i < b.size() && b[i] == '(' ) { ++i; }
			else if( requireParen ) { return run; }
			for( ;; ) {
				while( i < b.size() && isspace( (unsigned char)b[i] ) ) { ++i; }
				size_t e = i;
				while( e < b.size() && isVerbChar( b[e] ) ) { ++e; }
				const std::string tok = b.substr( i, e - i );
				if( tok.empty() || !isKnownVerb( tok ) ) { break; }
				run.push_back( tok );
				i = e;
				while( i < b.size() && isspace( (unsigned char)b[i] ) ) { ++i; }
				if( i < b.size() && ( b[i] == '/' || b[i] == ',' ) ) { ++i; continue; }
				break;
			}
			return run;
		};
		auto setOf = []( std::vector<std::string> v ) {
			std::sort( v.begin(), v.end() );
			v.erase( std::unique( v.begin(), v.end() ), v.end() );
			return v;
		};

		std::vector<std::string> verbProblems;
		int countedMutatingClaims = 0, mutatingRunsChecked = 0, readSafeRunsChecked = 0;
		int literalClaimsChecked = 0;
		std::vector<fs::path> agentFiles;
		for( const auto& e : fs::directory_iterator( agentDir ) ) {
			if( !e.is_regular_file() ) { continue; }
			const std::string ext = e.path().extension().string();
			if( ext == ".cpp" || ext == ".h" ) { agentFiles.push_back( e.path() ); }
		}
		std::sort( agentFiles.begin(), agentFiles.end() );
		Check( agentFiles.size() >= 5,
		       "verb-parity: found the src/Library/Agent sources to scan (got "
		       + std::to_string( agentFiles.size() ) + ")" );

		// FIX 5b (P2, 2026-08-09 R1 fix round): the DISCLOSED RESIDUAL above ("Files outside
		// src/Library/Agent -- the GUI bridges ... restate these sets too") is exactly the gap that let
		// FIX 4's four stale "5 mutating verbs" GUI sites (RISEViewportBridge.h x2, ViewportBridge.h x2,
		// ChatPanel.cpp) survive undetected -- extend the SAME anchored counted-claim scan (part (A) above)
		// to the GUI host-loop/bridge sources that restate this count in prose:
		// build/XCode/rise/RISE-GUI/**/*.{swift,h,mm} and build/VS2022/RISE-GUI/**/*.{h,cpp}.  Deliberately
		// NARROWER than the Agent-dir scan: only part (A)'s counted-mutating-verb claim is checked on these
		// files, not part (B)'s read-safe-allowlist enumeration -- every GUI site found in this arc restates
		// ONLY the mutating count/list, never the read-safe set, so wiring (B) here would be untested dead
		// code with no real coverage to show for it; a narrower, provably-live check beats a broader,
		// unverified one.  (ExtractProseBlocks's generic `//` / `/* */` / string-literal grammar covers
		// Objective-C++ (.mm) and Swift line/block comments the same way it covers C++; Swift's `///` doc
		// marker is not specially recognised, but that only folds one extra leading `/` into the prose text,
		// which the substring/anchor search below does not care about.)
		std::vector<fs::path> guiFiles;
		{
			std::error_code ec;
			struct GuiRoot { fs::path dir; std::vector<std::string> exts; };
			const GuiRoot roots[] = {
				{ repoRoot / "build" / "XCode" / "rise" / "RISE-GUI", { ".swift", ".h", ".mm" } },
				{ repoRoot / "build" / "VS2022" / "RISE-GUI",         { ".h", ".cpp" } },
			};
			for( const GuiRoot& root : roots ) {
				fs::recursive_directory_iterator it( root.dir, ec ), end;
				if( ec ) { continue; }
				for( ; it != end; it.increment( ec ) ) {
					if( ec ) { break; }
					if( !it->is_regular_file() ) { continue; }
					const std::string ext = it->path().extension().string();
					if( std::find( root.exts.begin(), root.exts.end(), ext ) != root.exts.end() )
						guiFiles.push_back( it->path() );
				}
			}
		}
		std::sort( guiFiles.begin(), guiFiles.end() );
		// Liveness for the GUI extension itself: if the roots above ever get renamed/moved, this silently
		// degenerating to zero files would silently disable the exact coverage FIX 5b exists to add.
		Check( guiFiles.size() >= 4,
		       "verb-parity: found the GUI bridge/host-loop sources to scan (got "
		       + std::to_string( guiFiles.size() ) + ")" );

		std::vector<fs::path> scanFiles = agentFiles;
		scanFiles.insert( scanFiles.end(), guiFiles.begin(), guiFiles.end() );

		// FIX C (P2, round-3 fix, 2026-08-09): part (B) below (the read-safe-
		// allowlist enumeration check) runs ONLY against agentFiles, matching
		// the DISCLOSED RESIDUAL note above.  A linear membership test against
		// the (small, single-digit-to-low-double-digit) agentFiles list is
		// cheap enough not to warrant a set/map here.
		auto isAgentFile = [&]( const fs::path& f ) {
			return std::find( agentFiles.begin(), agentFiles.end(), f ) != agentFiles.end();
		};

		for( const fs::path& f : scanFiles ) {
			const bool fileIsAgentFile = isAgentFile( f );
			const std::string name = f.filename().string();
			for( const ProseBlock& blk : ExtractProseBlocks( slurp( f ) ) ) {
				const std::string& b = blk.text;
				const std::string lb = lower( b );
				const std::string kind = blk.kind;
				auto where = [&]( size_t off ) {
					return name + ":" + std::to_string( blk.LineAt( off ) )
					       + ( kind == std::string( "string" ) ? " (MODEL-FACING literal)" : "" );
				};

				// (A) "<N> [<prefix>-]mutating verb(s)/tool(s)"
				for( size_t p = lb.find( "mutating" ); p != std::string::npos;
				     p = lb.find( "mutating", p + 1 ) ) {
					if( p > 0 && ( isalpha( (unsigned char)lb[p-1] ) || lb[p-1] == '_' ) ) { continue; }
					// right side: whitespace then verb/tool
					size_t r = p + 8;
					while( r < lb.size() && isspace( (unsigned char)lb[r] ) ) { ++r; }
					const bool isVerbWord = lb.compare( r, 4, "verb" ) == 0;
					const bool isToolWord = lb.compare( r, 4, "tool" ) == 0;
					if( !isVerbWord && !isToolWord ) { continue; }
					size_t afterWord = r + 4;
					if( afterWord < lb.size() && lb[afterWord] == 's' ) { ++afterWord; }
					// left side: optional hyphenated prefix word, then a count.
					size_t l = p;
					while( l > 0 && isspace( (unsigned char)lb[l-1] ) ) { --l; }
					if( l > 0 && lb[l-1] == '-' ) {
						--l;
						while( l > 0 && isalpha( (unsigned char)lb[l-1] ) ) { --l; }
						while( l > 0 && isspace( (unsigned char)lb[l-1] ) ) { --l; }
					}
					size_t numEnd = l, numBeg = l;
					while( numBeg > 0 && isalnum( (unsigned char)lb[numBeg-1] ) ) { --numBeg; }
					const std::string numTok = lb.substr( numBeg, numEnd - numBeg );
					static const char* kWords[] = { "zero","one","two","three","four","five",
					                                "six","seven","eight","nine","ten" };
					long stated = -1;
					if( !numTok.empty() && isdigit( (unsigned char)numTok[0] )
					    && numTok.find_first_not_of( "0123456789" ) == std::string::npos ) {
						stated = std::stol( numTok );
					} else {
						for( long w = 0; w < 11; ++w ) { if( numTok == kWords[w] ) { stated = w; break; } }
					}
					if( stated < 0 ) { continue; }   // not a counted claim
					++countedMutatingClaims;
					if( kind == std::string( "string" ) ) { ++literalClaimsChecked; }
					if( (size_t)stated != proposeSafe.size() ) {
						verbProblems.push_back(
							where( numBeg ) + ": claims \"" + numTok + " mutating "
							+ ( isVerbWord ? "verb" : "tool" ) + "s\" but IsProposeSafeVerb has "
							+ std::to_string( proposeSafe.size() ) + " (" + join( proposeSorted ) + ")" );
					}
					const std::vector<std::string> run = parseRun( b, afterWord, true );
					if( run.empty() ) { continue; }   // "(IsProposeSafeVerb)", "(the 3 ...)"
					++mutatingRunsChecked;
					if( setOf( run ) != proposeSorted ) {
						verbProblems.push_back(
							where( afterWord ) + ": the mutating-verb list \"" + join( run )
							+ "\" is not IsProposeSafeVerb's set (" + join( proposeSorted ) + ")" );
					}
				}

				// (B) "read-safe allowlist (" / "IsReadSafeVerb -- ".  FIX C
				// (round-3 fix, 2026-08-09): agentFiles ONLY -- see the
				// DISCLOSED RESIDUAL note and the `fileIsAgentFile` gate set
				// up above the outer loop.
				if( fileIsAgentFile ) {
					const char* anchors[] = { "read-safe allowlist", "isreadsafeverb --" };
					for( int a = 0; a < 2; ++a ) {
						const std::string anchor = anchors[a];
						for( size_t p = lb.find( anchor ); p != std::string::npos;
						     p = lb.find( anchor, p + 1 ) ) {
							const std::vector<std::string> run =
								parseRun( b, p + anchor.size(), false );
							if( run.empty() ) { continue; }
							++readSafeRunsChecked;
							if( setOf( run ) != readSorted ) {
								verbProblems.push_back(
									where( p ) + ": the read-safe allowlist \"" + join( run )
									+ "\" is not IsReadSafeVerb's set (" + join( readSorted ) + ")" );
							}
						}
					}
				}
			}
		}

		for( const std::string& p : verbProblems ) {
			std::cout << "  AGENT VERB-SET DRIFT: " << p << std::endl;
		}
		if( !verbProblems.empty() ) {
			std::cout << "  Fix the TEXT at each line above -- the comment, or, where it is "
			          << "tagged (MODEL-FACING literal), the tool description a model reads. "
			          << "src/Library/Agent/AgentRpc.cpp's IsReadSafeVerb / IsProposeSafeVerb "
			          << "bodies are the source of truth; if the CODE is what changed, the text "
			          << "still has to follow.  Prefer deleting a restated count over "
			          << "re-deriving it." << std::endl;
		}
		Check( verbProblems.empty(),
		       "agent verb-set enumerations in src/Library/Agent match IsReadSafeVerb / "
		       "IsProposeSafeVerb" );
		// Liveness: the guard must actually be finding anchored claims.  Not a
		// fixed expected count (that would be the very defect being fixed) --
		// just proof that rewording every anchor away cannot silently disable
		// the check.
		Check( countedMutatingClaims > 0 && mutatingRunsChecked > 0 && readSafeRunsChecked > 0
		       && literalClaimsChecked > 0,
		       "agent verb-set guard is LIVE (counted-mutating claims: "
		       + std::to_string( countedMutatingClaims ) + ", mutating lists: "
		       + std::to_string( mutatingRunsChecked ) + ", read-safe lists: "
		       + std::to_string( readSafeRunsChecked ) + ", MODEL-FACING literal claims: "
		       + std::to_string( literalClaimsChecked )
		       + ") -- a zero here means the anchor phrasings were reworded away; a zero on "
		       "the LAST one means no model-facing description carries a checkable claim, "
		       "which is where this family's findings keep landing" );
	}

	// ---- The enumeration guards' own parser, red-proved (fix round 20) --
	// The round-17 mechanism was defective in ways only a test of the TEST
	// can pin: it ingested comment text as ground truth and saw neither
	// `/* */` blocks nor string literals.  Each case below is one of the
	// reviewer's red-proofs, frozen so the mechanism cannot regress to it.
	{
		const std::string sample =
			"bool Pick( const std::string& m )\n"
			"{\n"
			"\t// a comment that says \"ghost\" must not become a value\n"
			"\treturn m == \"alpha\" || m == \"beta\";\n"
			"}\n"
			"//! run line one\n"
			"//! run line two\n"
			"int x = 1;   // detached\n"
			"/* block comment mentions five mutating verbs */\n"
			"const char* d = \"first literal \"\n"
			"                \"second literal, escaped \\\"quoted\\\" word\";\n"
			"// value = \"dead\";\n";

		const std::string code = StripCommentsPreservingLayout( sample );
		Check( code.size() == sample.size(),
		       "self-test: comment stripping preserves offsets" );
		Check( code.find( "ghost" ) == std::string::npos
		       && code.find( "dead" ) == std::string::npos
		       && code.find( "block comment" ) == std::string::npos,
		       "self-test: a quoted word inside a `//` comment, a commented-out assignment, and a "
		       "`/* */` block are ALL invisible to a ground-truth parser" );
		Check( code.find( "\"alpha\"" ) != std::string::npos
		       && code.find( "\"beta\"" ) != std::string::npos,
		       "self-test: real string literals survive comment stripping" );

		const std::vector<ProseBlock> blocks = ExtractProseBlocks( sample );
		bool sawRun = false, sawBlockComment = false, sawJoinedLiteral = false, sawGhost = false;
		for( const ProseBlock& b : blocks ) {
			if( b.text.find( "run line one" ) != std::string::npos
			    && b.text.find( "run line two" ) != std::string::npos ) { sawRun = true; }
			if( b.text.find( "five mutating verbs" ) != std::string::npos ) { sawBlockComment = true; }
			if( b.text.find( "first literal" ) != std::string::npos
			    && b.text.find( "second literal" ) != std::string::npos
			    && b.text.find( "\"quoted\"" ) != std::string::npos ) { sawJoinedLiteral = true; }
			if( b.text.find( "ghost" ) != std::string::npos ) { sawGhost = true; }
		}
		Check( sawRun, "self-test: consecutive `//` lines join into ONE prose run" );
		Check( sawBlockComment, "self-test: a `/* */` block is scanned for claims" );
		Check( sawJoinedLiteral,
		       "self-test: adjacent MODEL-FACING string literals join, with `\\\"` decoded" );
		Check( sawGhost, "self-test: comment prose is still scanned as PROSE (only ground truth excludes it)" );
	}

	// ---- GUI edit-refusal retry gate (FIX 2) --------------------------
	// WHY THIS IS GUARDED IN SOURCE.  Both GUI chat drivers now RETRY an
	// edit tool call that came back a retriable refusal, instead of
	// spending a full LLM round-trip per retry (the measured failure: an
	// agent refused five times with "editor transaction or gesture in
	// progress", burning ~13 of a 23-turn session).  The retry is SAFE only
	// because of one condition: the batch verbs (insert_chunks /
	// propose_patches) are SEQUENTIAL and BEST-EFFORT -- AgentRpc.cpp
	// documents that a rejected element does not stop the batch -- so a
	// batch can be PARTIALLY applied, and re-issuing one whose result
	// merely says `retriable` would DOUBLE-APPLY whatever already landed.
	//
	// Dropping the `applied == 0` half of the gate would leave a retry that
	// still LOOKS correct, still passes every functional test (the happy
	// path is a full refusal), and silently duplicates chunks in exactly
	// the scene-building sessions that batch the most, so it is pinned
	// here, on both platforms.
	//
	// WHAT THESE PINS DO AND DO NOT PROVE.  They are SUBSTRING matches
	// against the two drivers' source: they prove each condition's TEXT is
	// still present in the named function's body.  They do NOT execute
	// either gate, so they cannot see a change that leaves every pinned
	// string in place and still admits a partially-applied batch (append
	// `&& false` to each Qt guard, or `||` in a re-admitting clause on the
	// Swift side, and this whole section stays green -- verified by doing
	// it).  The truth table the pins are describing is asserted
	// BEHAVIOURALLY, on a transliterated copy, by
	// EditRefusalGateBehaviour() (defined above main, called at the end of
	// this section) -- read that pair together: the copy is what runs, and
	// these string pins are the only thing tying it to the code that ships.
	//
	// ROUND 2 (P1-1) -- THE GUARD MUST GUARD ITS OWN WIRING.  As first
	// written this section pinned the helpers, the gate, the bound, the
	// backoff table, the teardown answer and the verb set -- but NOT the
	// two DISPATCH SITES that call any of it.  Deleting one line on each
	// platform (macOS `responseLine = await retryWhollyRefusedEditToolCall
	// (...)` -> `responseLine = firstResponse`; Windows the
	// `if (isRetriableEditVerb(...) && editRefusalIsWhollyUnapplied(...))`
	// block in processNextToolCall) disconnected the ENTIRE fix while
	// leaving every Check in this section green.  Section (0) below closes
	// that: the wiring is now pinned inside the two dispatchers' extracted
	// BODIES, so the fix cannot be silently unplugged.
	//
	// Ground truth for the VERB SET is AgentRpc.cpp's IsProposeSafeVerb --
	// the mutating verbs are exactly the ones whose results carry the
	// `applied`+`retriable` pair the gate reads.  This file names no verb
	// of its own, and (round 20) parses that set, and both drivers' copies
	// of it, with COMMENTS STRIPPED: a comment containing a quoted word
	// anywhere in any of the three bodies used to be read as a verb name.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		auto bodyBetween = []( const std::string& src, const char* begin,
		                       const char* next ) -> std::string {
			const size_t b = src.find( begin );
			if( b == std::string::npos ) return std::string();
			const size_t e = src.find( next, b );
			if( e == std::string::npos ) return std::string();
			return src.substr( b, e - b );
		};
		const std::string rpcCpp = StripCommentsPreservingLayout(
			slurp( repoRoot / "src" / "Library" / "Agent" / "AgentRpc.cpp" ) );
		const std::string macChat = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ChatViewModel.swift" );
		const std::string winChat = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.cpp" );
		const std::string winChatHeader = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.h" );

		// The mutating verb set, parsed out of IsProposeSafeVerb's body.
		std::vector<std::string> mutatingVerbs;
		{
			const size_t s = rpcCpp.find( "bool IsProposeSafeVerb( const std::string& method )" );
			const size_t open = ( s == std::string::npos ) ? std::string::npos : rpcCpp.find( '{', s );
			if( open != std::string::npos ) {
				int depth = 0; size_t end = open;
				for( size_t i = open; i < rpcCpp.size(); ++i ) {
					if( rpcCpp[i] == '{' ) { ++depth; }
					else if( rpcCpp[i] == '}' ) { if( --depth == 0 ) { end = i; break; } }
				}
				for( size_t i = open; i < end; ++i ) {
					if( rpcCpp[i] != '"' ) { continue; }
					const size_t q = rpcCpp.find( '"', i + 1 );
					if( q == std::string::npos ) { break; }
					mutatingVerbs.push_back( rpcCpp.substr( i + 1, q - i - 1 ) );
					i = q;
				}
			}
		}
		Check( mutatingVerbs.size() >= 3,
		       "edit-retry: parsed IsProposeSafeVerb's verb set (sanity: >=3; got "
		       + std::to_string( mutatingVerbs.size() ) + ")" );

		// Comment-stripped copies, for the assertions that must not be
		// satisfiable (or defeated) by prose: the numeric bound/backoff
		// comparison below, and the Windows never-block negative list.
		const std::string macChatCode = StripCommentsPreservingLayout( macChat );
		const std::string winChatCode = StripCommentsPreservingLayout( winChat );

		// (0) THE DISPATCH SITES -- see the P1-1 note in this section's
		//     header.  Everything else here guards machinery that a
		//     one-line edit at these two call sites can orphan wholesale.
		//     Both are asserted inside the extracted body of the
		//     dispatcher that owns them, so an occurrence in a comment or
		//     in some other function cannot satisfy them.
		const std::string macDriveTurn = bodyBetween( macChatCode,
			"private func driveTurn() async {", "private static func parseAskUserArgs" );
		Check( !macDriveTurn.empty()
		       && macDriveTurn.find( "let firstResponse = vb.agentHandleToolCall(line)" )
		          != std::string::npos
		       && macDriveTurn.find( "responseLine = await retryWhollyRefusedEditToolCall(" )
		          != std::string::npos,
		       "macOS driveTurn STILL ROUTES the ordinary tool dispatch through "
		       "retryWhollyRefusedEditToolCall -- replacing that await with `responseLine = "
		       "firstResponse` disconnects FIX 2 entirely while every other Check here stays green" );
		const std::string winProcessNext = bodyBetween( winChatCode,
			"void ChatPanel::processNextToolCall()", "void ChatPanel::startAsyncRenderToolCall" );
		Check( !winProcessNext.empty()
		       && winProcessNext.find(
		              "isRetriableEditVerb(call.name) && editRefusalIsWhollyUnapplied(response)" )
		          != std::string::npos
		       // NB: matched WITHOUT the `/*attemptsSoFar=*/1` inline comment
		       // -- winChatCode has had every comment blanked out.
		       && winProcessNext.find( "scheduleEditToolCallRetry(call, line," )
		          != std::string::npos
		       && winProcessNext.find( "baselineHead);" ) != std::string::npos,
		       "Windows processNextToolCall STILL PARKS a wholly-refused edit into "
		       "scheduleEditToolCallRetry -- deleting that block disconnects FIX 2 entirely while "
		       "every other Check here stays green" );

		// (1) The retry gate itself, on BOTH platforms.  Bodies are
		//     extracted by symbol so a marker cannot be satisfied by an
		//     unrelated line elsewhere in the file.
		const std::string macGate = bodyBetween( macChat,
			"private static func editRefusalIsWhollyUnapplied",
			"private static func refusalHeadVersionKey" );
		Check( !macGate.empty()
		       // BATCH: `applied` is a COUNT and must be exactly zero, the
		       // element list must be non-empty, and EVERY element must be
		       // an unapplied retriable rejection.
		       && macGate.find( "appliedCount.intValue == 0" ) != std::string::npos
		       && macGate.find( "!results.isEmpty" ) != std::string::npos
		       && macGate.find( "elementApplied == false" ) != std::string::npos
		       && macGate.find( "elementRetriable" ) != std::string::npos
		       // EVERY element, not just the first: rewriting the loop to
		       // `results.first` would satisfy each marker above on its own.
		       // The trailing `{` is load-bearing -- without it the marker is
		       // a PREFIX of `for element in results.prefix(1) {` and the
		       // weakening passes (proved by red/green, round 2).
		       && macGate.find( "for element in results {" ) != std::string::npos
		       // status == "rejected", per element AND singular (round 2, C1):
		       // `applied` is CLEAN-APPLY-ONLY, so a "diagnosed" element
		       // reports applied:false with the head ALREADY moved.
		       && macGate.find( "elementStatus == \"rejected\"" ) != std::string::npos
		       && macGate.find( "status == \"rejected\"" ) != std::string::npos
		       // A JSON bool ALSO bridges to NSNumber -- the count position
		       // must reject one, matching Qt's isDouble().
		       && macGate.find( "CFGetTypeID(appliedCount) != CFBooleanGetTypeID()" )
		          != std::string::npos
		       // SINGULAR: applied==false AND retriable==true.
		       && macGate.find( "applied == false" ) != std::string::npos
		       && macGate.find( "let retriable = result[\"retriable\"] as? Bool, retriable" )
		          != std::string::npos,
		       "macOS edit-retry gate's body still CONTAINS every NOTHING-APPLIED condition (batch: "
		       "applied count == 0 AND EVERY element unapplied+retriable+rejected; singular: "
		       "applied==false AND retriable AND status==rejected) -- text presence only, not a "
		       "behavioural check that the gate refuses a partially-applied batch" );

		const std::string winGate = bodyBetween( winChat,
			"bool editRefusalIsWhollyUnapplied(const QString& responseLine)",
			"QString refusalHeadVersionKey(" );
		Check( !winGate.empty()
		       && winGate.find( "result.value(\"applied\").isDouble()" ) != std::string::npos
		       && winGate.find( "result.value(\"applied\").toDouble() != 0.0" ) != std::string::npos
		       && winGate.find( "results.isEmpty()" ) != std::string::npos
		       && winGate.find( "e.value(\"applied\").toBool()" ) != std::string::npos
		       && winGate.find( "!e.value(\"retriable\").toBool()" ) != std::string::npos
		       // EVERY element, not just the first (see the macOS twin).
		       && winGate.find( "for (const QJsonValue& element : results)" ) != std::string::npos
		       // status == "rejected", per element AND singular (round 2, C1).
		       && winGate.find( "e.value(\"status\").toString() != QStringLiteral(\"rejected\")" )
		          != std::string::npos
		       && winGate.find( "result.value(\"status\").toString() != QStringLiteral(\"rejected\")" )
		          != std::string::npos
		       && winGate.find( "!result.value(\"applied\").isBool() || result.value(\"applied\").toBool()" )
		          != std::string::npos
		       && winGate.find( "!result.value(\"retriable\").toBool()" ) != std::string::npos,
		       "Windows edit-retry gate's body still CONTAINS every NOTHING-APPLIED condition (batch: "
		       "applied count == 0 AND EVERY element unapplied+retriable+rejected; singular: "
		       "applied==false AND retriable AND status==rejected) -- text presence only, not a "
		       "behavioural check that the gate refuses a partially-applied batch" );

		// (2) The gate is BATCH-FIRST on both platforms.  This is DEFENCE IN
		//     DEPTH, not load-bearing, and the Check says so on purpose: a
		//     batch envelope carries no TOP-LEVEL `retriable` (AgentRpc.cpp
		//     emits only {applied,total,results}), so the singular arm would
		//     reject it on the missing key whichever order they ran in, and a
		//     PARTIALLY applied batch has applied >= 1, which reads as `true`,
		//     not `false`.  What is real is the bridging hazard the ordering
		//     means nobody has to reason about: a JSON number reads as a bool
		//     on both toolchains (NSNumber bridging / QJsonValue::toBool).
		Check( macGate.find( "result[\"results\"] as? [[String: Any]]" )
		         < macGate.find( "let applied = result[\"applied\"] as? Bool" )
		       && winGate.find( "result.value(\"results\").isArray()" )
		            < winGate.find( "!result.value(\"applied\").isBool()" ),
		       "edit-retry gate tests the BATCH shape BEFORE the singular shape on both platforms "
		       "(defence in depth against a JSON number reading as a bool)" );

		// (3) The retry is BOUNDED and YIELDS between attempts -- never a
		//     blocking wait on the thread that owns the editor transaction
		//     we are waiting on (that would deadlock the gesture).
		//     End-anchors are SYMBOLS, never prose: a doc sentence in an
		//     unrelated function is not a stable boundary -- rewording it
		//     silently empties the extracted body and every marker below
		//     goes vacuously green.
		const std::string macRetry = bodyBetween( macChat,
			"private func retryWhollyRefusedEditToolCall",
			"private func executeRenderToolCallAsync" );
		Check( !macRetry.empty()
		       && macRetry.find( "attempts < Self.editRetryMaxAttempts" ) != std::string::npos
		       && macRetry.find( "await Task.sleep" ) != std::string::npos
		       // Post-await re-verification: Stop / cancelled Task / scene
		       // close / production render starting.
		       && macRetry.find( "Task.isCancelled || stopRequested" ) != std::string::npos
		       && macRetry.find( "guard viewportBridge != nil" ) != std::string::npos
		       && macRetry.find( "guard sceneEditable()" ) != std::string::npos
		       // HEAD-VERSION GUARD (round 2, C2): a re-issue is admitted only
		       // while the refusal keeps reporting the head attempt 1 saw.
		       && macRetry.find( "let baselineHead = Self.refusalHeadVersionKey(firstResponse)" )
		          != std::string::npos
		       && macRetry.find( "Self.refusalHeadVersionKey(responseLine) == baselineHead" )
		          != std::string::npos
		       // The stamp fires ONLY on a real retry -- otherwise every
		       // single-attempt edit result would be re-serialized and
		       // annotated for nothing.
		       && macRetry.find( "guard attempts > 1 else { return responseLine }" )
		          != std::string::npos,
		       "macOS edit retry is bounded, yields the MainActor between attempts, re-verifies "
		       "Stop/scene-close/production-render at every suspension point, abandons on a MOVED head, "
		       "and stamps the result only when a retry actually happened" );

		const std::string winRetry = bodyBetween( winChat,
			"void ChatPanel::scheduleEditToolCallRetry",
			"void ChatPanel::deliverEditToolCallResult" );
		// Negatives are evaluated on the COMMENT-STRIPPED body: the prose
		// around this function legitimately talks about sleeping and event
		// loops, and a negative assertion that a comment can trip (or that a
		// reworded comment can silently satisfy) guards nothing.
		const std::string winRetryCode = StripCommentsPreservingLayout( winRetry );
		Check( !winRetry.empty()
		       && winRetry.find( "QTimer::singleShot" ) != std::string::npos
		       && winRetry.find( "attempts < kEditRetryMaxAttempts" ) != std::string::npos
		       && winRetry.find( "m_editRetryToken != token" ) != std::string::npos
		       && winRetry.find( "m_stopRequested || !m_sceneEditable || !m_bridge" )
		          != std::string::npos
		       // HEAD-VERSION GUARD (round 2, C2) -- the Qt half of the same
		       // rule; the baseline itself is captured at the dispatch site
		       // pinned in (0) above.
		       && winRetry.find( "refusalHeadVersionKey(response) == baselineHead" )
		          != std::string::npos
		       // A blocking wait here would guarantee the gesture can never
		       // complete -- the retry MUST return to the event loop.  The
		       // list is every blocking idiom reachable from a Qt GUI slot,
		       // not just the two the first draft happened to name.
		       && winRetryCode.find( "QThread::" ) == std::string::npos
		       && winRetryCode.find( "processEvents" ) == std::string::npos
		       && winRetryCode.find( "std::this_thread::sleep_for" ) == std::string::npos
		       && winRetryCode.find( "QEventLoop" ) == std::string::npos
		       && winRetryCode.find( "::Sleep(" ) == std::string::npos
		       && winRetryCode.find( "QCoreApplication::exec" ) == std::string::npos
		       && winRetryCode.find( "QApplication::exec" ) == std::string::npos,
		       "Windows edit retry is bounded, returns to the Qt event loop between attempts (never a "
		       "blocking sleep/nested event loop on the thread that owns the editor transaction), "
		       "re-verifies Stop/scene-close/production-render at every tick, and abandons on a MOVED head" );

		// (3b) The driver stamp fires ONLY when a retry actually happened, on
		//      BOTH platforms -- the macOS half is pinned in macRetry above,
		//      the Windows half lives in deliverEditToolCallResult.
		const std::string winDeliver = bodyBetween( winChat,
			"void ChatPanel::deliverEditToolCallResult", "void ChatPanel::cancelActiveTurn" );
		Check( !winDeliver.empty()
		       && winDeliver.find( "(attempts > 1)" ) != std::string::npos
		       && winDeliver.find( "stampDriverRetry(" ) != std::string::npos,
		       "Windows edit-retry result is stamped only when attempts > 1 (the single-attempt path "
		       "stays byte-for-byte unchanged)" );

		// (3c) The two drivers agree on the ATTEMPT BOUND and the BACKOFF
		//      TABLE, derived from each driver's own declaration.  Without
		//      this, dropping Swift to 3 attempts (or re-tuning one backoff
		//      table) while Qt stays at 5 is a silent cross-platform
		//      behaviour split that every other Check here tolerates.
		auto numbersAfterAssignment = []( const std::string& src, const char* marker ) -> std::string {
			const size_t b = src.find( marker );
			if( b == std::string::npos ) return std::string();
			const size_t eq = src.find( '=', b );
			if( eq == std::string::npos ) return std::string();
			std::string out;
			bool inNumber = false;
			for( size_t i = eq + 1; i < src.size(); ++i ) {
				const char c = src[i];
				if( c == '\n' || c == ';' ) break;
				if( isdigit( (unsigned char)c ) ) { out += c; inNumber = true; }
				else if( inNumber ) { out += ','; inNumber = false; }
			}
			while( !out.empty() && out[out.size() - 1] == ',' ) out.erase( out.size() - 1 );
			return out;
		};
		const std::string macAttempts = numbersAfterAssignment(
			macChatCode, "private static let editRetryMaxAttempts" );
		const std::string winAttempts = numbersAfterAssignment(
			winChatCode, "const int kEditRetryMaxAttempts" );
		const std::string macBackoff = numbersAfterAssignment(
			macChatCode, "private static let editRetryBackoffMs" );
		const std::string winBackoff = numbersAfterAssignment(
			winChatCode, "const int kEditRetryBackoffMs" );
		Check( !macAttempts.empty() && macAttempts == winAttempts,
		       "edit-retry ATTEMPT BOUND matches across the two drivers (macOS \"" + macAttempts
		       + "\" vs Windows \"" + winAttempts + "\")" );
		Check( !macBackoff.empty() && macBackoff == winBackoff,
		       "edit-retry BACKOFF TABLE matches across the two drivers (macOS \"" + macBackoff
		       + "\" vs Windows \"" + winBackoff + "\")" );

		// (4) The parked retry is answered when the turn is torn down.  Its
		//     call was already popped from m_pendingToolCalls, so the drain
		//     cannot see it -- cancelActiveTurn must, or the loop replays an
		//     unanswered tool call forever.
		//     (End-anchor is the NEXT SYMBOL, not a comment banner -- see the
		//     note in (3).)
		const std::string winCancelTurn = bodyBetween( winChat,
			"void ChatPanel::cancelActiveTurn", "void ChatPanel::refreshProposals()" );
		const std::string winRequestStop = bodyBetween( winChat,
			"void ChatPanel::requestStop()", "void ChatPanel::resetConversation()" );
		Check( !winCancelTurn.empty()
		       && winCancelTurn.find( "++m_editRetryToken;" ) != std::string::npos
		       && winCancelTurn.find( "if (m_editRetryPending)" ) != std::string::npos
		       && winCancelTurn.find( "m_loop->AddToolResult(m_editRetryCall" ) != std::string::npos
		       && !winRequestStop.empty()
		       && winRequestStop.find( "m_editRetryPending" ) != std::string::npos
		       && winRequestStop.find( "cancelActiveTurn" ) != std::string::npos
		       && winChatHeader.find( "void scheduleEditToolCallRetry(" ) != std::string::npos,
		       "Windows cancelActiveTurn abandons a parked edit retry and answers its call (it is "
		       "already popped from m_pendingToolCalls, so the drain cannot), and requestStop routes "
		       "a parked retry through it" );

		// (5) Both drivers gate on the SAME verb set IsProposeSafeVerb
		//     defines -- the only verbs whose results carry applied+retriable.
		std::vector<std::string> macMissing, winMissing;
		// Comments stripped INSIDE the extracted body only: the bodyBetween
		// markers are code symbols, but the body itself is commented, and a
		// quoted word in one of those comments would be counted as a verb.
		const std::string macVerbSet = StripCommentsPreservingLayout( bodyBetween( macChat,
			"private static let retriableEditVerbs", "private static let editRetryMaxAttempts" ) );
		const std::string winVerbSet = StripCommentsPreservingLayout( bodyBetween( winChat,
			"bool isRetriableEditVerb(const std::string& name)", "int editRetryBackoffMs(" ) );
		for( const std::string& v : mutatingVerbs ) {
			if( macVerbSet.find( "\"" + v + "\"" ) == std::string::npos ) macMissing.push_back( v );
			if( winVerbSet.find( "\"" + v + "\"" ) == std::string::npos ) winMissing.push_back( v );
		}
		// Count-equality too, so an EXTRA (non-mutating) verb cannot be
		// smuggled into either driver's set.
		auto quotedCount = []( const std::string& body ) {
			size_t n = 0;
			for( size_t p = body.find( '"' ); p != std::string::npos; ) {
				const size_t q = body.find( '"', p + 1 );
				if( q == std::string::npos ) break;
				++n;
				p = body.find( '"', q + 1 );
			}
			return n;
		};
		Check( !macVerbSet.empty() && macMissing.empty()
		       && quotedCount( macVerbSet ) == mutatingVerbs.size(),
		       "macOS edit-retry verb set is exactly IsProposeSafeVerb's set" );
		Check( !winVerbSet.empty() && winMissing.empty()
		       && quotedCount( winVerbSet ) == mutatingVerbs.size(),
		       "Windows edit-retry verb set is exactly IsProposeSafeVerb's set" );

		// (6) The truth table itself, executed -- on the transliterated copy
		//     described above the pins.  Scope caveat there, not repeated.
		EditRefusalGateBehaviour();
	}

	// ---- E4 Part 2: macOS degenerate-turn retry-once TEXTUAL pin ----
	// Swift has NO test binary here (no swiftc/xcodebuild-driven unit test
	// runs in this suite -- only xcodebuild's own compile gate does), so
	// this is DISCLOSED as textual, not behavioural, exactly like every
	// other macGate-style pin in the block above: it guards against
	// SILENT DELETION OR DRIFT of the retry-once structure (the per-round
	// `attempt==1` gate, the reset on a successful round, the
	// attempt/retryOf trajectory stamp), not against a logic bug in it --
	// AgentChatLoopTest.cpp's T44 (TestDegenerateTurnRetryParity) is what
	// actually EXERCISES the equivalent host-loop policy, at the
	// AgentChatLoop API level both Swift and the eval runner build on.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		auto bodyBetween = []( const std::string& src, const char* begin,
		                       const char* next ) -> std::string {
			const size_t b = src.find( begin );
			if( b == std::string::npos ) return std::string();
			const size_t e = src.find( next, b );
			if( e == std::string::npos ) return std::string();
			return src.substr( b, e - b );
		};
		const std::string macChat = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ChatViewModel.swift" );
		Check( !macChat.empty(), "degenerate-retry pin: read ChatViewModel.swift" );
		const std::string macChatCode = StripCommentsPreservingLayout( macChat );

		// Symbol-anchored, like every bodyBetween extraction in this file:
		// a marker inside driveTurn()'s own body cannot be satisfied by an
		// unrelated occurrence elsewhere (e.g. the FIX-2 edit-retry gate's
		// OWN `attempts`/`attempt` locals, which live in a DIFFERENT
		// function and are excluded by these very boundaries).
		const std::string macDriveTurn = bodyBetween( macChatCode,
			"private func driveTurn() async {", "private static func parseAskUserArgs" );
		Check( !macDriveTurn.empty()
		       // The two per-round locals, declared OUTSIDE the `while true`
		       // loop (so a retry's own `continue` does not reset them --
		       // only a genuinely NEW round does).
		       && macDriveTurn.find( "var httpAttempt = 1" ) != std::string::npos
		       && macDriveTurn.find( "var degenerateTurnRetried = false" ) != std::string::npos
		       // The retry-once GATE: exactly the eval runner's `attempt==1`
		       // check, ported -- retryDegenerateTurn alone is NOT enough,
		       // the per-round flag must also still be false.
		       && macDriveTurn.find( "if step.retryDegenerateTurn && !degenerateTurnRetried {" )
		          != std::string::npos
		       && macDriveTurn.find( "degenerateTurnRetried = true" ) != std::string::npos
		       && macDriveTurn.find( "httpAttempt += 1" ) != std::string::npos
		       // RESET on a round that actually succeeded (.toolCalls) -- a
		       // LATER round's own degenerate response must get its OWN
		       // retry allowance, not inherit an already-spent one.  A bare
		       // `.find("httpAttempt = 1")` would be VACUOUS here (it also
		       // matches inside the "var httpAttempt = 1" DECLARATION above
		       // -- caught in review), so the anchor is the whole
		       // `case .toolCalls:` reset, contiguous and in order.
		       && macDriveTurn.find(
		              "case .toolCalls:\n                consecutiveHttp400s = 0\n"
		              "                httpAttempt = 1\n                degenerateTurnRetried = false" )
		          != std::string::npos
		       // The trajectory STAMP: the retry rides as an honest sibling
		       // `llm` record (attempt/retryOf), the SAME convention T33's
		       // TestMultimodalRetry pins for retryWithoutImages.
		       && macDriveTurn.find( "if httpAttempt > 1 {" ) != std::string::npos
		       && macDriveTurn.find( "attempt: httpAttempt, retryOf: httpAttempt - 1" )
		          != std::string::npos,
		       "macOS driveTurn's degenerate-turn retry-once structure is still present: the two "
		       "per-round locals, the attempt==1-equivalent gate before re-issuing, the reset on a "
		       "successful round, and the attempt/retryOf trajectory stamp -- TEXTUAL PRESENCE ONLY "
		       "(Swift has no unit-test binary here; this guards against silent deletion/drift, not "
		       "against a logic bug in the retry policy itself)" );
	}

	// ---- Context-compaction budget wiring parity (FIX 3) ----
	// AgentChatLoop::CompactTranscript is fully implemented but INERT until a
	// host calls SetContextBudget -- and for the whole life of the feature
	// NEITHER GUI did, so span compaction never once ran in production.  The
	// C++ side of that is unit-tested (AgentChatLoopTest T36), but "is it
	// plugged in" is a source-wiring fact in two files no portable test can
	// reach (an ObjC++ bridge and a Qt panel), which is exactly what this
	// file exists for.  Unplugging either call must turn the suite red --
	// otherwise the fix could silently regress to the state it was fixing.
	//
	// Both drivers must pass the SHARED constants rather than literals, so
	// the two cannot drift apart or from the header's documented rationale.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string macBridge = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "Bridge" / "RISEAgentChatBridge.mm" );
		const std::string winChat = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.cpp" );
		const std::string loopHeader = slurp( repoRoot / "src" / "Library" / "Agent"
			/ "AgentChatLoop.h" );
		Check( !macBridge.empty() && !winChat.empty() && !loopHeader.empty(),
		       "context-budget wiring: read both GUI chat drivers and AgentChatLoop.h" );

		// The constants must EXIST in the header the drivers cite.  This is
		// NOT what stops a bare-literal driver -- the two driver checks below
		// already require the constant IDENTIFIERS by name, so a driver
		// passing literals fails those.  What this one catches is the
		// constants being RENAMED or DELETED from the header while the
		// drivers still name them (which would fail to compile) or while a
		// careless edit updates all three together (which would silently
		// drop the shared source of truth).
		// Comment-stripped like every other check in this block: without it,
		// deleting the constants while a doc comment still spelled them out
		// in full would leave this green.
		const std::string loopHeaderCode = StripCommentsPreservingLayout( loopHeader );
		Check( loopHeaderCode.find( "kDefaultContextBudgetHighTokens" ) != std::string::npos
		       && loopHeaderCode.find( "kDefaultContextBudgetLowTokens" ) != std::string::npos,
		       "context-budget wiring: AgentChatLoop.h declares the shared default budget" );

		// StripCommentsPreservingLayout so a COMMENT mentioning the call (the
		// cross-reference each driver carries to its twin) cannot stand in
		// for the call itself.
		const std::string macCode = StripCommentsPreservingLayout( macBridge );
		const std::string winCode = StripCommentsPreservingLayout( winChat );
		Check( macCode.find( "SetContextBudget" ) != std::string::npos
		       && macCode.find( "kDefaultContextBudgetHighTokens" ) != std::string::npos
		       && macCode.find( "kDefaultContextBudgetLowTokens" ) != std::string::npos,
		       "macOS chat bridge installs the shared context budget (span compaction is LIVE)" );
		Check( winCode.find( "SetContextBudget" ) != std::string::npos
		       && winCode.find( "kDefaultContextBudgetHighTokens" ) != std::string::npos
		       && winCode.find( "kDefaultContextBudgetLowTokens" ) != std::string::npos,
		       "Windows chat panel installs the shared context budget (span compaction is LIVE)" );

		// ...AND BOTH drivers must TELL the user when compaction drops turns
		// from the model's memory.  The two have OPPOSITE symptoms, and both
		// are dishonest without a notice:
		//   * Windows renders straight out of the loop's transcript
		//     (m_loop->TranscriptAt), so the rows visibly VANISH -- which
		//     reads as data loss.
		//   * macOS keeps its own append-only display array, so nothing
		//     changes on screen -- the panel keeps showing turns the model
		//     can no longer see, and the resulting amnesia is
		//     indistinguishable from a model defect.  That is arguably the
		//     WORSE case, and an earlier revision of this guard wrongly
		//     concluded the mirror made a notice unnecessary.
		// Asserted on comment-stripped source, so a comment mentioning the
		// counter cannot stand in for reading it.
		Check( winCode.find( "CompactedEntryCount" ) != std::string::npos,
		       "Windows chat panel surfaces CompactedEntryCount() (compaction is not a "
		       "silent history wipe -- it renders the loop transcript directly)" );
		// COMMENT-STRIPPED, like the two driver checks above -- Swift's `//`
		// and `/* */` are the same shapes StripCommentsPreservingLayout
		// handles, and string literals are preserved.  Without this a
		// COMMENT mentioning compactedEntryCount satisfies the notice check
		// while the call is unplugged; red-proved by removing the call and
		// watching this stay green until the strip was added.
		const std::string macVmRaw = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ChatViewModel.swift" );
		Check( !macVmRaw.empty(),
		       "context-budget wiring: read the macOS ChatViewModel" );
		const std::string macVm = StripCommentsPreservingLayout( macVmRaw );
		// NOTE the earlier form of this Check OR-ed against
		// macBridge.find("transcript"), which is unconditionally true (the
		// bridge declares transcript ACCESSORS) -- so it short-circuited and
		// never read the Swift file at all.  Assert the two facts directly.
		Check( macVm.find( "private(set) var transcript: [Entry]" ) != std::string::npos,
		       "macOS driver keeps its own append-only display transcript (which is WHY "
		       "a dropped wire span is invisible there, and why it needs a notice)" );
		Check( macVm.find( "compactedEntryCount" ) != std::string::npos,
		       "macOS chat driver surfaces compactedEntryCount (its display keeps showing "
		       "turns the model can no longer see -- silence there is the worse lie)" );
		// MENTIONING the counter is not USING it.  The previous form of this
		// check passed with the notice's own condition rewritten to
		// `if false && droppedNow > lastReportedCompactedEntryCount` -- the
		// identifier is still there, the notice is dead.  Pin the CONDITION
		// itself: the delta comparison must be the whole guard.  (This is a
		// source-shape guard, not an execution test -- the Swift driver
		// cannot be exercised from the C++ suite; the same limitation every
		// check in this block lives with.)
		// Pin the WHOLE statement, brace included, so neither operand order
		// of a short-circuit survives: `if false && droppedNow > ...` fails
		// the prefix, and `if droppedNow > ... && false {` fails the brace.
		Check( macVm.find( "if droppedNow > lastReportedCompactedEntryCount {" ) != std::string::npos,
		       "macOS compaction notice is guarded by the DELTA comparison and NOTHING "
		       "else -- no `&& false` / feature flag on either side of it" );
		// ...and the guarded body must actually APPEND the notice.  Without
		// this, deleting the transcript.append while keeping the condition and
		// the wording leaves every other check green and the notice dead.
		// Anchored as a contiguous span so the append cannot merely exist
		// elsewhere in the file.
		{
			const std::string::size_type cond =
				macVm.find( "if droppedNow > lastReportedCompactedEntryCount {" );
			const std::string::size_type appended = ( cond == std::string::npos )
				? std::string::npos
				: macVm.find( "transcript.append(Entry(", cond );
			const std::string::size_type worded = ( cond == std::string::npos )
				? std::string::npos
				: macVm.find( "earlier transcript row(s)", cond );
			Check( appended != std::string::npos && worded != std::string::npos &&
			       appended < worded && ( worded - cond ) < 1200,
			       "the macOS compaction notice is APPENDED to the transcript inside that "
			       "guard (condition -> transcript.append -> the row(s) wording, contiguous)" );
		}
		// ...and it must report the right UNIT.  CompactedEntryCount counts
		// erased transcript ENTRIES; one compacted span is a user message
		// plus every round it provoked, so calling them "turns" overstates
		// the loss.  Windows already says "transcript row(s)"; both must.
		Check( macVm.find( "earlier transcript row(s)" ) != std::string::npos &&
		       macVm.find( "earlier turn(s)" ) == std::string::npos,
		       "macOS compaction notice reports ENTRIES (\"transcript row(s)\"), matching "
		       "what CompactedEntryCount counts and what Windows says" );
		Check( winCode.find( "earlier transcript row(s)" ) != std::string::npos,
		       "Windows compaction notice reports the same unit" );

		// Role::DriverNote (E4, 2026-08): NO CURRENT PRODUCER.  The one
		// message it existed for -- the blind-edit NUDGE -- was replaced by
		// the SEQUENCING GATE (a tool-call REFUSAL, delivered as an
		// ordinary ToolResults entry every driver already renders through
		// its normal tool-result path; see AgentChatLoop.h's SEQUENCING
		// GATE block).  Windows renders the wire transcript directly and
		// its Role switch is exhaustive with no `default` (a compile-time
		// -Wswitch guard against a silently-unhandled future enumerator --
		// see ChatPanel.cpp's comment on that switch), so it MUST still
		// carry a `case Role::DriverNote:` arm even with zero producers
		// today; check that it still does, so a future removal of the
		// case (leaving the enumerator behind, or vice versa) is caught
		// here rather than at the next Windows build.  macOS renders its
		// OWN display list rather than the wire transcript and had a
		// polling mirror (driverNoteCount/lastDriverNoteText) for exactly
		// this note -- retired alongside the nudge, since a SEQUENCING
		// GATE refusal needs no such mirror (it is a tool result, and the
		// per-call tool-result row is already generic) -- so there is
		// nothing left to check on the macOS side.
		Check( winCode.find( "Role::DriverNote" ) != std::string::npos,
		       "Windows chat panel still handles Role::DriverNote (an exhaustive switch over "
		       "a still-live enumerator, even with zero producers today)" );
	}

	// ---- read_viewport reason-code surface registry (fix rounds 17, 20) --
	// AgentSession.h's read_viewport authority block carries a hand-written
	// "every surface that enumerates the reason codes" list, and used to
	// carry hand-written counts of it ("EIGHT files, TEN places").  Both the
	// list and the counts went stale -- the counts twice.  The counts are now
	// deleted (a list is its own count) and the LIST is checked here, THREE
	// WAYS:
	//   * every registered surface must still enumerate every reason value
	//     (catches a surface falling behind when a reason is added),
	//   * every ENUMERATION PASSAGE inside a registered surface must be
	//     complete, not just the file as a whole (round 20: scrubbing a
	//     reason out of two of observe-modes.md's four registered passages
	//     left the file-granular check GREEN, because the token survived
	//     elsewhere in the same file -- and miscounting those very passages
	//     is what started this family), and
	//   * no unregistered file may enumerate them all (catches a NEW surface
	//     being created without registration -- the failure the block exists
	//     to prevent, and the half a human reviewer never catches).
	// Ground truth is AgentSession.cpp's `outReason = "..."` assignments --
	// the code that actually produces the wire values, read with COMMENTS
	// STRIPPED (round 20: a commented-out assignment used to be parsed as a
	// live value, and every surface was then told to add a dead reason).
	// This test names no reason value of its own, so it cannot itself go
	// stale.
	//
	// SCAN SCOPE, stated exactly (round 20; round 17 scanned only
	// {src,skills,docs,tests,build} and the authority block nonetheless
	// claimed unregistered surfaces were caught "by construction" -- probe
	// files at the repo root, under scenes/, and as docs/*.txt or *.json all
	// enumerated every reason and left the suite green).  The walk now starts
	// at the REPO ROOT and covers every file with a text extension in
	// kTextExts, pruning only build outputs and vendored/binary trees
	// (.git, extlib, bin, rendered, agent worktrees, _out, DerivedData,
	// node_modules, and nested `build` dirs -- the top-level build/ tree,
	// which holds the .swift/.mm GUI surfaces, is SCANNED).  Residual, and
	// it is the honest one: a file with an extension outside kTextExts, or
	// inside a pruned tree, is invisible.  Widen kTextExts rather than
	// restate the claim if a new surface kind appears.
	//
	// The registry-path extractor uses THE SAME kTextExts and no root
	// prefix list, so anything the scanner can FIND can also be REGISTERED
	// (round 20: `build/...` surfaces were detected as unregistered but
	// could not be registered -- the extractor's roots were
	// {src,skills,docs,tests}, so the printed remedy prescribed an action
	// that left the suite red with no hint why).
	{
		// ABSOLUTE, because the walk starts AT the repo root: testsDir is
		// relative ("tests"), whose parent_path() is the empty path, and a
		// recursive_directory_iterator over "" yields nothing -- which would
		// make the unregistered-surface half silently vacuous.
		const fs::path repoRoot = fs::weakly_canonical( fs::absolute( testsDir ) ).parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		// ONE list, used by both the scanner and the registry-path extractor.
		static const char* kTextExts[] = {
			".cpp", ".h", ".hpp", ".c", ".mm", ".m", ".swift", ".cs", ".java", ".kt",
			".md", ".txt", ".json", ".py", ".sh", ".ps1", ".yml", ".yaml", ".RISEscene",
		};
		auto isTextExt = []( const std::string& ext ) {
			for( const char* e : kTextExts ) { if( ext == e ) { return true; } }
			return false;
		};

		const fs::path definer = repoRoot / "src" / "Library" / "Agent" / "AgentSession.cpp";
		const std::string definerSrc = StripCommentsPreservingLayout( slurp( definer ) );

		std::vector<std::string> reasons;
		{
			const std::string key = "outReason = \"";
			for( size_t p = definerSrc.find( key ); p != std::string::npos;
			     p = definerSrc.find( key, p + 1 ) ) {
				const size_t b = p + key.size();
				const size_t e = definerSrc.find( '"', b );
				if( e == std::string::npos ) { break; }
				reasons.push_back( definerSrc.substr( b, e - b ) );
			}
			std::sort( reasons.begin(), reasons.end() );
			reasons.erase( std::unique( reasons.begin(), reasons.end() ), reasons.end() );
		}
		Check( reasons.size() >= 3,
		       "reason-registry: parsed AgentSession.cpp's read_viewport reason values "
		       "(sanity: >=3; got " + std::to_string( reasons.size() ) + ")" );

		// WHOLE-TOKEN containment.  Not `"<name>"`: the model-facing surfaces
		// spell the values inside C++ string literals, where the closing
		// delimiter is an ESCAPED quote (`..._progress\"`), and the markdown
		// ones use backticks or bare prose.  Requiring a non-identifier
		// character on both sides matches all of those while still refusing a
		// longer identifier that merely CONTAINS a reason name.
		auto containsToken = []( const std::string& body, const std::string& tok ) {
			for( size_t p = body.find( tok ); p != std::string::npos; p = body.find( tok, p + 1 ) ) {
				const char before = ( p == 0 ) ? ' ' : body[p - 1];
				const size_t after = p + tok.size();
				const char nxt = ( after >= body.size() ) ? ' ' : body[after];
				auto ident = []( char ch ) {
					return isalnum( (unsigned char)ch ) || ch == '_';
				};
				if( !ident( before ) && !ident( nxt ) ) { return true; }
			}
			return false;
		};

		// The registry: repo-relative paths inside the delimited block.
		// `readerFacing` drops the block's own TESTS group -- the block says
		// in so many words that those "pin the wire values ...; they are not
		// themselves an enumeration a reader acts on", and a test file walks
		// the values one ARM at a time rather than restating the set.
		std::vector<std::string> registry, readerFacing;
		{
			const std::string sessionH = slurp( repoRoot / "src" / "Library" / "Agent" / "AgentSession.h" );
			const size_t b = sessionH.find( "[read_viewport-reason-surfaces]" );
			const size_t e = sessionH.find( "[/read_viewport-reason-surfaces]" );
			Check( b != std::string::npos && e != std::string::npos && b < e,
			       "reason-registry: AgentSession.h carries the delimited "
			       "[read_viewport-reason-surfaces] block (do not remove the markers -- "
			       "they are what makes the surface list checkable)" );
			if( b != std::string::npos && e != std::string::npos && b < e ) {
				// NO ROOT PREFIX LIST (round 20).  Any path-shaped token whose
				// extension is one the scanner can find is a registry entry, so
				// the two halves cannot disagree about what is registerable.
				// A repo-root file (AGENTS.md) qualifies; a bare filename
				// mentioned in the surrounding prose does not.
				const std::string block = sessionH.substr( b, e - b );
				const size_t testsHeading = block.find( "TESTS (" );
				size_t p = 0;
				while( p < block.size() ) {
					if( !( isalnum( (unsigned char)block[p] ) || block[p] == '_'
					       || block[p] == '.' || block[p] == '/' ) ) { ++p; continue; }
					const size_t at = p;
					size_t q = p;
					while( q < block.size()
					       && ( isalnum( (unsigned char)block[q] ) || block[q] == '/'
					            || block[q] == '.' || block[q] == '_' || block[q] == '-' ) ) { ++q; }
					std::string path = block.substr( p, q - p );
					p = q;
					while( !path.empty() && path.back() == '.' ) { path.pop_back(); }
					const size_t dot = path.rfind( '.' );
					if( dot == std::string::npos ) { continue; }
					if( !isTextExt( path.substr( dot ) ) ) { continue; }
					if( path.find( '/' ) == std::string::npos
					    && !fs::exists( repoRoot / path ) ) { continue; }   // bare prose filename
					registry.push_back( path );
					if( testsHeading == std::string::npos || at < testsHeading ) {
						readerFacing.push_back( path );
					}
				}
				std::sort( registry.begin(), registry.end() );
				registry.erase( std::unique( registry.begin(), registry.end() ), registry.end() );
				std::sort( readerFacing.begin(), readerFacing.end() );
				readerFacing.erase( std::unique( readerFacing.begin(), readerFacing.end() ),
				                    readerFacing.end() );
			}
		}
		Check( registry.size() >= 3 && readerFacing.size() >= 3
		       && readerFacing.size() < registry.size(),
		       "reason-registry: extracted the registered surface paths (sanity: >=3, with the "
		       "block's TESTS group split off; got " + std::to_string( registry.size() )
		       + " total / " + std::to_string( readerFacing.size() ) + " reader-facing)" );

		// Which files actually enumerate EVERY reason value?  Whole-repo walk
		// (see SCAN SCOPE above) so a surface cannot dodge registration by
		// living somewhere the round-17 root list happened not to name.
		std::vector<std::string> enumerating;
		size_t filesScanned = 0;
		{
			std::error_code ec;
			fs::recursive_directory_iterator it( repoRoot, ec ), end;
			for( ; it != end; it.increment( ec ) ) {
				if( ec ) { break; }
				if( it->is_directory() ) {
					const std::string d = it->path().filename().string();
					const std::string rel = fs::relative( it->path(), repoRoot ).generic_string();
					// Build outputs, vendored sources, binaries, and the agent
					// worktree pool -- none of which is a maintained surface.
					// Note `build` is pruned only when NESTED: the top-level
					// build/ tree holds the GUI's .swift/.mm surfaces.
					if( d == ".git" || d == "_out" || d == "DerivedData" || d == "node_modules"
					    || d == "worktrees" || rel == "extlib" || rel == "bin" || rel == "rendered"
					    || ( d == "build" && rel != "build" ) ) { it.disable_recursion_pending(); }
					continue;
				}
				if( !it->is_regular_file() ) { continue; }
				if( !isTextExt( it->path().extension().string() ) ) { continue; }
				++filesScanned;
				const std::string body = slurp( it->path() );
				bool all = true;
				for( const std::string& r : reasons ) {
					if( !containsToken( body, r ) ) { all = false; break; }
				}
				if( !all ) { continue; }
				enumerating.push_back( fs::relative( it->path(), repoRoot ).generic_string() );
			}
			std::sort( enumerating.begin(), enumerating.end() );
		}
		Check( !enumerating.empty(), "reason-registry: found the enumerating surfaces to check" );
		// A collapsed walk (a bad prune, a moved repo root) would silently
		// make the unregistered-surface half vacuous.
		Check( filesScanned > 1000,
		       "reason-registry: the repo-wide scan is LIVE (text files scanned: "
		       + std::to_string( filesScanned ) + ")" );

		// (i) every registered surface still enumerates every reason.
		std::vector<std::string> missing;
		for( const std::string& r : registry ) {
			if( !fs::exists( repoRoot / r ) ) { missing.push_back( r + " (registered path does not exist)" ); continue; }
			if( !std::binary_search( enumerating.begin(), enumerating.end(), r ) ) {
				const std::string body = slurp( repoRoot / r );
				std::string absent;
				for( const std::string& v : reasons ) {
					if( !containsToken( body, v ) ) {
						if( !absent.empty() ) absent += ", ";
						absent += v;
					}
				}
				missing.push_back( r + " (missing: " + absent + ")" );
			}
		}
		for( const std::string& m : missing ) {
			std::cout << "  READ_VIEWPORT REASON SURFACE INCOMPLETE: " << m << std::endl;
		}
		if( !missing.empty() ) {
			std::cout << "  A surface registered in AgentSession.h's "
			          << "[read_viewport-reason-surfaces] block must enumerate EVERY reason "
			          << "value AgentSession.cpp can emit.  Add the missing value(s) there "
			          << "(model-facing surfaces must also state retriability and the action)."
			          << std::endl;
		}
		Check( missing.empty(),
		       "every registered read_viewport reason surface enumerates all reason values" );

		// (i-b) PASSAGE granularity, not just file granularity (round 20).
		// A registered file typically restates the set in SEVERAL passages
		// (observe-modes.md registers four); the file-level check above is
		// satisfied by any ONE of them, so a reason scrubbed from the other
		// three stayed green.
		//
		// A "passage" is a BLANK-LINE-DELIMITED block -- the natural unit in
		// both of the shapes these surfaces use: a markdown table or paragraph,
		// and a run of `//!` doc lines (which never contains a truly blank
		// source line, so one doc block is one passage).  Proximity clustering
		// was tried first and is NOT sufficient: observe-modes.md's
		// "Hard warnings" table and the prose paragraph under it sit ~600
		// characters apart, so any workable character window merged the two
		// registered passages into one and re-hid exactly the defect this
		// check exists for.
		//
		// A block counts as an ENUMERATION (rather than an incidental mention
		// or a deliberate subset) only when it already names ALL BUT ONE of the
		// values -- deliberately conservative, because several surfaces
		// legitimately name the four same-gate reasons as a subset.  Such a
		// block must then be COMPLETE.
		{
			auto ident = []( char ch ) { return isalnum( (unsigned char)ch ) || ch == '_'; };
			std::vector<std::string> partial;
			for( const std::string& r : readerFacing ) {
				if( !fs::exists( repoRoot / r ) ) { continue; }
				const std::string body = slurp( repoRoot / r );
				// Per-offset block index: bumped by every blank (or whitespace-
				// only) line.
				std::vector<size_t> blockAt( body.size() + 1, 0 );
				{
					size_t blk = 0, pos = 0;
					while( pos <= body.size() ) {
						const size_t nl = body.find( '\n', pos );
						const size_t stop = ( nl == std::string::npos ) ? body.size() : nl;
						bool blank = true;
						for( size_t k = pos; k < stop; ++k ) {
							if( !isspace( (unsigned char)body[k] ) ) { blank = false; break; }
						}
						if( blank ) { ++blk; }
						for( size_t k = pos; k <= stop && k < blockAt.size(); ++k ) { blockAt[k] = blk; }
						if( nl == std::string::npos ) { break; }
						pos = nl + 1;
					}
				}
				std::vector<std::pair<size_t,size_t>> hits;   // (offset, reason index)
				for( size_t k = 0; k < reasons.size(); ++k ) {
					const std::string& tok = reasons[k];
					for( size_t p = body.find( tok ); p != std::string::npos; p = body.find( tok, p + 1 ) ) {
						const char before = ( p == 0 ) ? ' ' : body[p - 1];
						const size_t after = p + tok.size();
						const char nxt = ( after >= body.size() ) ? ' ' : body[after];
						if( !ident( before ) && !ident( nxt ) ) { hits.push_back( std::make_pair( p, k ) ); }
					}
				}
				std::sort( hits.begin(), hits.end() );
				size_t i = 0;
				while( i < hits.size() ) {
					size_t j = i;
					while( j + 1 < hits.size()
					       && blockAt[hits[j + 1].first] == blockAt[hits[i].first] ) { ++j; }
					std::vector<bool> seen( reasons.size(), false );
					for( size_t k = i; k <= j; ++k ) { seen[hits[k].second] = true; }
					size_t distinct = 0;
					for( bool s : seen ) { if( s ) { ++distinct; } }
					if( distinct + 1 == reasons.size() ) {
						std::string absent;
						for( size_t k = 0; k < reasons.size(); ++k ) {
							if( !seen[k] ) { absent = reasons[k]; }
						}
						const size_t line =
							1 + (size_t)std::count( body.begin(), body.begin() + (long)hits[i].first, '\n' );
						partial.push_back( r + ":" + std::to_string( line )
						                   + " (this passage names " + std::to_string( distinct )
						                   + " of " + std::to_string( reasons.size() )
						                   + " reasons; missing: " + absent + ")" );
					}
					i = j + 1;
				}
			}
			for( const std::string& p : partial ) {
				std::cout << "  READ_VIEWPORT REASON PASSAGE INCOMPLETE: " << p << std::endl;
			}
			if( !partial.empty() ) {
				std::cout << "  A registered surface restates the reason set in more than one "
				          << "passage, and EVERY passage that is enumerating the set has to be "
				          << "complete -- a model reads whichever one it lands on.  Add the "
				          << "missing value at that line, or, if that passage is deliberately a "
				          << "SUBSET, say so by naming fewer of them (a passage naming all but "
				          << "one reads as an incomplete enumeration, not as a subset)."
				          << std::endl;
			}
			Check( partial.empty(),
			       "every enumeration PASSAGE inside a registered read_viewport reason surface is "
			       "complete (not just the file as a whole)" );
		}

		// (i-c) A stated COUNT of the reason set must equal it (round 20).
		// Five reader-facing surfaces still restate "SEVEN reasons" in prose;
		// the registry above deliberately carries no count, but these do, and
		// an unguarded count in this family has gone stale in four separate
		// rounds.
		//
		// The blanket rule -- "any number before the word reason(s)" -- was
		// TRIED and REJECTED here: these same files legitimately count
		// SUBSETS, e.g. observe-modes.md's "the two no-viewport reasons
		// (no_controller, no_frame_yet)", "the ONE reason where a render call
		// does not hit that same gate", "the OTHER FOUR refusal reasons", and
		// 50-agentic-surface.md's unrelated "for two reasons (D42)".  A guard
		// that fires on those would be the round-17 defect again, in a new
		// place.  So only TOTALITY phrasings are checked -- and the corollary,
		// which is the maintenance rule: state a whole-set count in one of
		// these forms, or it buys no coverage.
		{
			static const char* kNumWords[] = { "zero","one","two","three","four","five",
			                                   "six","seven","eight","nine","ten" };
			auto lowerOf = []( std::string s ) {
				for( char& ch : s ) { ch = (char)tolower( (unsigned char)ch ); }
				return s;
			};
			std::vector<std::string> countProblems;
			for( const std::string& r : readerFacing ) {
				if( !fs::exists( repoRoot / r ) ) { continue; }
				const std::string body = slurp( repoRoot / r );
				const std::string lb = lowerOf( body );
				for( long w = 0; w <= 10; ++w ) {
					const std::string num = kNumWords[w];
					const std::string digit = std::to_string( w );
					for( int form = 0; form < 2; ++form ) {
						const std::string tok = ( form == 0 ) ? num : digit;
						for( size_t p = lb.find( tok ); p != std::string::npos; p = lb.find( tok, p + 1 ) ) {
							const char before = ( p == 0 ) ? ' ' : lb[p - 1];
							const size_t after = p + tok.size();
							if( isalnum( (unsigned char)before ) || before == '_' ) { continue; }
							if( after < lb.size() && ( isalnum( (unsigned char)lb[after] ) ) ) { continue; }
							// Which totality phrasing, if any, is this?
							const std::string tail = lb.substr( after, 24 );
							const size_t backFrom = ( p >= 16 ) ? p - 16 : 0;
							const std::string head = lb.substr( backFrom, p - backFrom );
							const bool hyphenCompound = tail.compare( 0, 7, "-reason" ) == 0;
							const bool thereAre =
								( head.find( "there are " ) != std::string::npos
								  || head.find( "there were " ) != std::string::npos )
								&& ( tail.compare( 0, 7, " reason" ) == 0 );
							const bool oneOf = head.find( "one of " ) != std::string::npos
							                   && tail.compare( 0, 7, " reason" ) == 0;
							bool totalWord = false;
							if( tail.compare( 0, 8, " reasons" ) == 0 ) {
								const std::string rest = tail.substr( 8, 12 );
								totalWord = rest.compare( 0, 6, " total" ) == 0
								            || rest.compare( 0, 9, " in total" ) == 0;
							}
							if( !hyphenCompound && !thereAre && !oneOf && !totalWord ) { continue; }
							if( (size_t)w == reasons.size() ) { continue; }
							const size_t line =
								1 + (size_t)std::count( body.begin(), body.begin() + (long)p, '\n' );
							countProblems.push_back(
								r + ":" + std::to_string( line ) + ": states \"" + tok
								+ "\" reasons, but AgentSession.cpp emits "
								+ std::to_string( reasons.size() ) );
						}
					}
				}
			}
			for( const std::string& c : countProblems ) {
				std::cout << "  READ_VIEWPORT REASON COUNT STALE: " << c << std::endl;
			}
			if( !countProblems.empty() ) {
				std::cout << "  Prefer DELETING the count -- the enumeration next to it is its "
				          << "own count, and every restated one in this family has gone stale."
				          << std::endl;
			}
			Check( countProblems.empty(),
			       "no reader-facing surface states a whole-set reason COUNT that disagrees with "
			       "AgentSession.cpp" );
		}

		// (ii) nothing else enumerates them without being registered.
		std::vector<std::string> unregistered;
		const std::string definerRel = fs::relative( definer, repoRoot ).generic_string();
		for( const std::string& e : enumerating ) {
			if( e == definerRel ) { continue; }   // the file that DEFINES the values
			if( std::find( registry.begin(), registry.end(), e ) == registry.end() ) {
				unregistered.push_back( e );
			}
		}
		for( const std::string& u : unregistered ) {
			std::cout << "  UNREGISTERED READ_VIEWPORT REASON SURFACE: " << u << std::endl;
		}
		if( !unregistered.empty() ) {
			std::cout << "  This file enumerates every read_viewport reason value but is not "
			          << "listed in AgentSession.h's [read_viewport-reason-surfaces] block.  "
			          << "Register it (with its repo-relative path) so the next reason added "
			          << "cannot silently leave it behind -- that is exactly what the block "
			          << "exists to prevent." << std::endl;
		}
		Check( unregistered.empty(),
		       "no unregistered file enumerates the read_viewport reason values" );
	}

	std::cout << std::endl
	          << "(scanned " << scanned << " test files) "
	          << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
