//////////////////////////////////////////////////////////////////////
//
//  CstDeriveGoldenTest.cpp - the CST-ONLY drift-catching safety net for the
//  Model-B P5 CST cutover (Slice 6c-1).
//
//  WHY THIS EXISTS
//  ---------------
//  Today CstCorpusEquivalenceTest is the ORACLE that proves CST-derive matches
//  the LEGACY parser: for every corpus scene it asserts
//      DumpJob(legacy-parse) == DumpJob(CST-derive).
//  Slice 6c DELETES the legacy parser, so that cross-check disappears.  BEFORE
//  legacy is deleted, this test captures the CURRENT CST-derived state of the
//  corpus as a committed GOLDEN reference (tests/data/cst_derive_golden.txt) and
//  provides a CST-ONLY regression that detects future CST-derive drift WITHOUT
//  needing the legacy parser.  Because the equivalence oracle currently passes,
//  today's CST-derive == legacy -- so today's CST-derive IS the validated golden.
//
//  The two tests are complementary and BOTH run now:
//    - CstCorpusEquivalenceTest : legacy-vs-CST  (validates the golden == legacy)
//    - CstDeriveGoldenTest       : CST-vs-golden  (catches CST-derive drift)
//  6c deletes the former; this one survives as the safety net.
//
//  GOLDEN FORMAT (tests/data/cst_derive_golden.txt)
//  ------------------------------------------------
//  One line per covered scene, sorted by repo-relative path:
//      <repo-relative-path>  <sha256-of-canonical-DumpJob>  <dump-bytes>  <dump-lines>
//  The digest is the primary drift signal; the byte/line counts are diagnostic
//  breadcrumbs (a size change localizes WHAT drifted before you re-derive).  The
//  CST-derive pipeline is IDENTICAL to the equivalence oracle's CST side:
//      Migrate(text) -> ParseToCst -> DeriveToJob -> DumpJob
//  so the captured golden is exactly the legacy-validated CST state.
//
//  COVERAGE
//  --------
//  Only TRACKED, non-Internal (scenes/Internal is gitignored) *.RISEscene files
//  that CST-derive CLEANLY are captured.  A scene is SKIPPED when:
//    (a) its derive produces the empty-job dump (a negative test the CST rejects,
//        or a scene that derives to nothing), OR
//    (b) its derive emits ANY diagnostic (a chunk failed to apply -- typically a
//        media-missing mesh/texture, e.g. the sponza_new* scenes reference an
//        absolute Windows glTF path and pr.RISEscene a missing chair.rawmesh).
//  Case (b) matters for TRUST + PORTABILITY: legacy hard-fails those same scenes
//  (no legacy dump was ever produced -> the CST derive was NEVER validated by the
//  oracle), and a partial media-dependent derive would bake a machine-specific
//  state into the golden.  So the golden covers exactly the scenes that derive
//  cleanly AND were legacy-validated -- mirroring the oracle's legacy-fail skip.
//  The generator reports covered vs skipped (empty / diag) with reasons.
//
//  REGENERATE (after an INTENTIONAL derive change -- reviewer must eyeball the diff)
//  --------------------------------------------------------------------------------
//      export RISE_MEDIA_PATH="$(pwd)/"                 # media root = repo root
//      make -C build/make/rise ../../../bin/tests/CstDeriveGoldenTest
//      ./bin/tests/CstDeriveGoldenTest --generate        # rewrites tests/data/cst_derive_golden.txt
//      git diff tests/data/cst_derive_golden.txt         # REVIEW every changed digest before commit
//  Run from the REPO ROOT (cwd) so repo-root-relative includes/media resolve.
//
//  VERIFY (the CST-only regression, the default mode)
//  --------------------------------------------------
//      export RISE_MEDIA_PATH="$(pwd)/"
//      ./bin/tests/CstDeriveGoldenTest                   # re-derives each golden scene, asserts digest ==
//  A mismatch fails naming the SCENE PATH so a future CST-derive regression is
//  caught + localized.  VERIFY ALSO asserts COVERAGE COMPLETENESS against the live
//  tracked corpus (mirroring what the legacy oracle did by enumerating the corpus):
//    - UNCOVERED: a corpus scene that CST-derives CLEANLY but is missing from the
//      golden fails (a new mis-deriving scene can't slip in silently).  The known
//      legacy-fails (media-missing / negative / non-native) derive empty or emit a
//      diagnostic, so they auto-skip -- no hardcoded skip list.
//    - STALE: a manifest entry for a scene no longer in the corpus fails (the
//      manifest can't rot).
//  So adding / removing a scene, or a scene's derive-status flipping, all force a
//  golden regenerate.
//
//  SUITE BEHAVIOUR (run_all_tests.sh runs this bare, with NO RISE_MEDIA_PATH)
//  -------------------------------------------------------------------------
//  Like CstCorpusEquivalenceTest, this is a MANUAL / opt-in gate: loading the
//  corpus needs the media root.  Run BARE (no args, no RISE_MEDIA_PATH) it SKIPS
//  with a clear message and returns success -- it does NOT hard-fail the plain
//  suite.  It ACTUALLY runs + verifies when the media root is present (either
//  RISE_MEDIA_PATH is set, or a probe derive of the first golden scene produces a
//  non-empty job).  See the run_all_tests.sh report for the skip/verify split.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"          // DumpJob (the canonical equivalence metric)
#include "../src/Library/Cst/Cst.h"        // ParseToCst / DeriveToJob
#include "../tools/CstMigrator.h"          // Migrate / ReadFile (the oracle's CST-side pipeline)

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

//////////////////////////////////////////////////////////////////////
// Self-contained SHA-256 (test-only; avoids a src/Library dependency + a
// 5-build-project edit).  Standard FIPS-180-4 implementation over a byte string.
//////////////////////////////////////////////////////////////////////
namespace {

struct Sha256 {
	uint32_t h[8];
	uint64_t totalLen;
	uint8_t  buf[64];
	size_t   bufLen;

	static uint32_t Rotr( uint32_t x, uint32_t n ) { return ( x >> n ) | ( x << ( 32 - n ) ); }

	Sha256() { Reset(); }
	void Reset() {
		h[0]=0x6a09e667u; h[1]=0xbb67ae85u; h[2]=0x3c6ef372u; h[3]=0xa54ff53au;
		h[4]=0x510e527fu; h[5]=0x9b05688cu; h[6]=0x1f83d9abu; h[7]=0x5be0cd19u;
		totalLen = 0; bufLen = 0;
	}

	void Block( const uint8_t* p ) {
		static const uint32_t k[64] = {
			0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
			0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
			0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
			0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
			0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
			0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
			0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
			0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
		uint32_t w[64];
		for( int i = 0; i < 16; ++i )
			w[i] = ( (uint32_t)p[i*4] << 24 ) | ( (uint32_t)p[i*4+1] << 16 ) | ( (uint32_t)p[i*4+2] << 8 ) | (uint32_t)p[i*4+3];
		for( int i = 16; i < 64; ++i ) {
			uint32_t s0 = Rotr(w[i-15],7) ^ Rotr(w[i-15],18) ^ ( w[i-15] >> 3 );
			uint32_t s1 = Rotr(w[i-2],17) ^ Rotr(w[i-2],19)  ^ ( w[i-2] >> 10 );
			w[i] = w[i-16] + s0 + w[i-7] + s1;
		}
		uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
		for( int i = 0; i < 64; ++i ) {
			uint32_t S1 = Rotr(e,6) ^ Rotr(e,11) ^ Rotr(e,25);
			uint32_t ch = ( e & f ) ^ ( ~e & g );
			uint32_t t1 = hh + S1 + ch + k[i] + w[i];
			uint32_t S0 = Rotr(a,2) ^ Rotr(a,13) ^ Rotr(a,22);
			uint32_t maj = ( a & b ) ^ ( a & c ) ^ ( b & c );
			uint32_t t2 = S0 + maj;
			hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
		}
		h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
	}

	void Update( const void* data, size_t len ) {
		const uint8_t* p = (const uint8_t*)data;
		totalLen += len;
		while( len ) {
			size_t take = 64 - bufLen; if( take > len ) take = len;
			std::memcpy( buf + bufLen, p, take );
			bufLen += take; p += take; len -= take;
			if( bufLen == 64 ) { Block( buf ); bufLen = 0; }
		}
	}

	std::string HexDigest() {
		uint64_t bitLen = totalLen * 8;
		uint8_t pad = 0x80; Update( &pad, 1 );
		uint8_t zero = 0x00;
		while( bufLen != 56 ) Update( &zero, 1 );
		uint8_t lenBytes[8];
		for( int i = 0; i < 8; ++i ) lenBytes[i] = (uint8_t)( bitLen >> ( 56 - i*8 ) );
		Update( lenBytes, 8 );
		char out[65];
		for( int i = 0; i < 8; ++i )
			std::snprintf( out + i*8, 9, "%08x", h[i] );
		return std::string( out, 64 );
	}
};

std::string Sha256Hex( const std::string& s ) {
	Sha256 sh; sh.Update( s.data(), s.size() ); return sh.HexDigest();
}

//////////////////////////////////////////////////////////////////////
// Corpus enumeration -- TRACKED, non-Internal (scenes/Internal is gitignored)
// *.RISEscene, sorted by path.  Uses `git ls-files` so the golden covers exactly
// the committed corpus (the equivalence oracle's raw `find` would also pick up
// the gitignored Internal scenes + any untracked scratch scene).
//
// FIX 2 (deterministic order): the paths are sorted IN-PROCESS with std::sort
// (byte / std::string ordering), NOT by a shell `sort` inside popen -- a shell
// `sort` is LC_COLLATE-dependent, so a differently-collated machine reorders the
// generated manifest lines (a spurious git diff that reads like drift; digests
// are unaffected).  std::string's operator< is a locale-independent byte compare,
// which matches `LC_ALL=C sort` -- the order the committed golden was written in.
//////////////////////////////////////////////////////////////////////
std::vector<std::string> EnumerateCorpus() {
	std::vector<std::string> paths;
	FILE* pp = popen( "git ls-files 'scenes/*.RISEscene' | grep -v '^scenes/Internal/'", "r" );
	if( !pp ) return paths;
	char line[8192];
	while( std::fgets( line, sizeof(line), pp ) ) {
		std::string s( line );
		while( !s.empty() && ( s.back()=='\n' || s.back()=='\r' ) ) s.pop_back();
		if( !s.empty() ) paths.push_back( s );
	}
	pclose( pp );
	std::sort( paths.begin(), paths.end() );   // locale-independent byte order (== LC_ALL=C)
	return paths;
}

// The CST-derive pipeline -- IDENTICAL to the equivalence oracle's CST side, so
// the golden captured here is exactly the legacy-validated CST state.
// Returns the canonical DumpJob string; `outDiagCount` (optional) receives the
// number of DERIVE diagnostics (a non-zero count means a chunk failed to apply --
// typically a media-missing mesh/texture the derive could not load).
std::string DeriveDump( const std::string& path, size_t* outDiagCount = nullptr ) {
	const std::string text = ReadFile( path );
	Job* jC = new Job();
	std::vector<std::string> diags;
	Document d = ParseToCst( Migrate( text ) );
	DeriveToJob( d, *jC, &diags );
	std::string dump = DumpJob( *jC );
	jC->release();
	if( outDiagCount ) *outDiagCount = diags.size();
	return dump;
}

// The DumpJob of an EMPTY job (all managers empty, no film/lights/media) is a
// stable fingerprint of "this scene derived to nothing" -- used to skip scenes
// the CST could not derive (media-missing mesh/texture, or a non-derived
// construct).  Computed once from a fresh empty Job so it tracks any future
// DumpJob format change automatically.
const std::string& EmptyDump() {
	static const std::string e = []{ Job* j = new Job(); std::string s = DumpJob( *j ); j->release(); return s; }();
	return e;
}

const char* kGoldenPath = "tests/data/cst_derive_golden.txt";

struct GoldenEntry { std::string path, digest; long bytes, lines; };

std::vector<GoldenEntry> ReadGolden() {
	std::vector<GoldenEntry> out;
	std::ifstream f( kGoldenPath );
	if( !f ) return out;
	std::string line;
	while( std::getline( f, line ) ) {
		if( line.empty() || line[0] == '#' ) continue;
		std::istringstream is( line );
		GoldenEntry e; e.bytes = 0; e.lines = 0;
		if( is >> e.path >> e.digest >> e.bytes >> e.lines ) out.push_back( e );
	}
	return out;
}

long CountLines( const std::string& s ) {
	long n = 0; for( char c : s ) if( c == '\n' ) ++n; return n;
}

//////////////////////////////////////////////////////////////////////
// GENERATE mode: re-derive the whole corpus, capture the golden manifest.
//////////////////////////////////////////////////////////////////////
int Generate() {
	// Media root defaults to the repo-root cwd (matches the oracle's setenv) so
	// relative includes/media resolve during the derive.
	char cwd[4096];
	if( getcwd( cwd, sizeof(cwd) ) ) setenv( "RISE_MEDIA_PATH", ( std::string(cwd) + "/" ).c_str(), 0 );

	std::vector<std::string> paths = EnumerateCorpus();
	if( paths.empty() ) { std::printf( "GENERATE: no corpus scenes found (run from repo root; `git ls-files` must see scenes/)\n" ); return 2; }

	std::vector<GoldenEntry> entries;
	int covered = 0, skippedEmpty = 0, skippedDiag = 0;
	const std::string& empty = EmptyDump();
	for( const std::string& path : paths ) {
		size_t diagCount = 0;
		const std::string dump = DeriveDump( path, &diagCount );
		if( diagCount > 0 ) { ++skippedDiag; std::printf( "  SKIP (derive diagnostic -- media-missing / chunk-apply-fail) %s\n", path.c_str() ); continue; }
		if( dump == empty ) { ++skippedEmpty; std::printf( "  SKIP (empty derive -- negative test or non-derived) %s\n", path.c_str() ); continue; }
		GoldenEntry e; e.path = path; e.digest = Sha256Hex( dump ); e.bytes = (long)dump.size(); e.lines = CountLines( dump );
		entries.push_back( e ); ++covered;
	}

	std::ofstream o( kGoldenPath, std::ios::binary | std::ios::trunc );
	if( !o ) { std::printf( "GENERATE: cannot open %s for write (is tests/data/ present?)\n", kGoldenPath ); return 2; }
	o << "# CST-derive golden manifest -- Model-B P5 Slice 6c-1 (pre-legacy-delete safety net).\n";
	o << "# Regenerate:  export RISE_MEDIA_PATH=\"$(pwd)/\" && ./bin/tests/CstDeriveGoldenTest --generate\n";
	o << "# Then REVIEW git diff before commit -- an unexpected digest change is a CST-derive regression.\n";
	o << "# Columns:  <repo-relative-path>  <sha256(DumpJob)>  <dump-bytes>  <dump-lines>\n";
	for( const GoldenEntry& e : entries )
		o << e.path << "  " << e.digest << "  " << e.bytes << "  " << e.lines << "\n";
	o.flush();

	std::printf( "\n=== GENERATE: %d covered, %d skipped-diag (media-missing), %d skipped-empty (of %zu tracked non-Internal scenes) -> %s ===\n",
		covered, skippedDiag, skippedEmpty, paths.size(), kGoldenPath );
	return 0;
}

//////////////////////////////////////////////////////////////////////
// VERIFY mode (default): re-derive each golden scene, assert digest == golden.
//////////////////////////////////////////////////////////////////////
int Verify() {
	std::vector<GoldenEntry> golden = ReadGolden();
	if( golden.empty() ) {
		std::printf( "CstDeriveGoldenTest: SKIP -- no golden manifest at %s (run from repo root; --generate to create it).\n", kGoldenPath );
		return 0;   // suite-safe: a missing manifest is not a suite failure
	}

	// Media gate: loading the corpus needs the media root.  Mirror
	// CstCorpusEquivalenceTest's degrade-to-skip so the plain suite (no
	// RISE_MEDIA_PATH) does not hard-fail.  We probe the FIRST golden scene: if
	// it derives to the empty job (media unreachable), SKIP the whole test.  If
	// RISE_MEDIA_PATH is already set, default it to the cwd so relative media
	// resolves; the probe still guards the case where even that is wrong.
	char cwd[4096];
	if( getcwd( cwd, sizeof(cwd) ) ) setenv( "RISE_MEDIA_PATH", ( std::string(cwd) + "/" ).c_str(), 0 );

	{
		const std::string probe = DeriveDump( golden.front().path );
		if( probe == EmptyDump() ) {
			std::printf( "CstDeriveGoldenTest: SKIP -- corpus/media unavailable (probe scene '%s' derived empty).\n"
				"  This is a MANUAL gate: run from repo root with the media corpus present:\n"
				"    export RISE_MEDIA_PATH=\"$(pwd)/\" && ./bin/tests/CstDeriveGoldenTest\n",
				golden.front().path.c_str() );
			return 0;   // suite-safe skip; it MUST actually verify when media IS present
		}
	}

	// Index the manifest by path for the completeness cross-check (FIX 1) and to
	// keep the existing per-scene DRIFT check.
	std::set<std::string> goldenPaths;
	for( const GoldenEntry& e : golden ) goldenPaths.insert( e.path );

	int ok = 0, fail = 0;
	for( const GoldenEntry& e : golden ) {
		const std::string dump = DeriveDump( e.path );
		const std::string digest = Sha256Hex( dump );
		if( digest == e.digest ) { ++ok; }
		else {
			++fail;
			std::printf( "DRIFT %s\n    golden digest=%s (%ld bytes, %ld lines)\n    derived digest=%s (%zu bytes, %ld lines)\n",
				e.path.c_str(), e.digest.c_str(), e.bytes, e.lines, digest.c_str(), dump.size(), CountLines( dump ) );
		}
		std::fflush( stdout );
	}

	//////////////////////////////////////////////////////////////////////
	// FIX 1 -- COVERAGE COMPLETENESS.  ReadGolden() only enumerates the MANIFEST,
	// so a scene ADDED to the tracked corpus after this commit -- even one that
	// CST-derives WRONG -- escapes the digest loop (it is not in the golden, stays
	// green).  Once the legacy oracle (which enumerated the LIVE corpus) is deleted,
	// nothing catches such a new mis-deriving scene.  Close it by ALSO enumerating
	// the live tracked corpus (the SAME way the generator does) and asserting:
	//   (a) UNCOVERED: every scene that CST-derives CLEANLY (non-empty, no derive
	//       diagnostic -- i.e. would be captured by --generate) MUST be present in
	//       the golden.  The 7 known legacy-fails (media-missing / negative /
	//       non-native) derive empty or emit a diagnostic and are skipped here --
	//       exactly as they are excluded from the golden -- with NO hardcoded skip
	//       list; the derive status auto-classifies them.
	//   (b) STALE: every manifest entry must correspond to a scene STILL tracked in
	//       the corpus, so a removed scene forces a golden update (the manifest
	//       can't rot).
	// Net: adding a scene, removing a scene, or a scene's derive-status flipping all
	// force a golden regenerate.
	//////////////////////////////////////////////////////////////////////
	int uncovered = 0, stale = 0;
	std::vector<std::string> corpus = EnumerateCorpus();
	std::set<std::string> corpusSet( corpus.begin(), corpus.end() );

	// (a) UNCOVERED -- a cleanly-deriving corpus scene missing from the manifest.
	for( const std::string& path : corpus ) {
		if( goldenPaths.count( path ) ) continue;   // already covered (digest-checked above)
		size_t diagCount = 0;
		const std::string dump = DeriveDump( path, &diagCount );
		if( diagCount > 0 || dump == EmptyDump() ) continue;   // legacy-fail: excluded from golden, correctly skipped
		++uncovered; ++fail;
		std::printf( "UNCOVERED %s -- a scene that CST-derives is not in the golden; regenerate after verifying its derive is correct\n", path.c_str() );
		std::fflush( stdout );
	}

	// (b) STALE -- a manifest entry for a scene no longer in the corpus.
	for( const std::string& path : goldenPaths ) {
		if( corpusSet.count( path ) ) continue;
		++stale; ++fail;
		std::printf( "STALE %s -- golden entry for a scene no longer in the corpus\n", path.c_str() );
		std::fflush( stdout );
	}

	std::printf( "\n=== CstDeriveGoldenTest: %d MATCH, %d DRIFT (of %zu golden scenes); coverage: %zu corpus scenes, %d UNCOVERED, %d STALE ===\n",
		ok, fail - uncovered - stale, golden.size(), corpus.size(), uncovered, stale );
	if( fail ) std::printf( "  A DRIFT/UNCOVERED/STALE is a CST-derive or coverage regression.  If INTENTIONAL, review + regenerate: ./bin/tests/CstDeriveGoldenTest --generate\n" );
	return fail ? 1 : 0;
}

} // namespace

int main( int argc, char** argv ) {
	if( argc > 1 && std::strcmp( argv[1], "--generate" ) == 0 ) return Generate();
	return Verify();
}
