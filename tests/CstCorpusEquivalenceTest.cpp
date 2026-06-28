//////////////////////////////////////////////////////////////////////
//
//  CstCorpusEquivalenceTest.cpp - the v6->v7 migrator's GATE + offline migrator.
//  For every scene under a root dir (argv[1]), parse the ORIGINAL via the LEGACY
//  AsciiSceneParser and DERIVE the MIGRATED text via the CST, then compare
//  DumpJob.  A MATCH means the migrated scene derives identically to the v6
//  original (the migration is faithful); a MISMATCH/LEGACY-FAIL is categorized
//  by the residual v6-only construct in the MIGRATED text (include / for /
//  define / expr / set / other) -- the migrator's remaining work-list.
//
//  Migrate() is the offline transform (text -> text), grown one slice at a time:
//    slice 1: FlattenIncludes -- recursively inline `> run` / `> load`,
//             COMMENT-AWARE (a directive inside a /* */ block is NOT inlined,
//             per D37 -- a line-grep rewriter would fold watch_dial's
//             commented-out `> modify` and break equivalence).
//  The live corpus is NOT modified (offline dev, per D8); the actual file
//  rewrite + front-line pivot is a separate atomic cutover later.
//
//  Both paths run from the REPO ROOT (cwd) so repo-root-relative `> run`/media
//  resolve identically.  Read-only.  Per-scene status is flushed so a crash
//  names the offending scene.  SUITE-SAFE: run bare it SKIPS (loads media for
//  the whole corpus); pass a scene root to run it manually.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"
#include "../tools/CstMigrator.h"   // shared v6->v7 migrator (Migrate / ReadFile)

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <unistd.h>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

// Categorize a residual mismatch by the FIRST v6-only construct present in the MIGRATED text.
static std::string Reason( const std::string& t )
{
	if( t.find("> run") != std::string::npos || t.find("> load") != std::string::npos ) return "include";
	{
		size_t i = 0;                                          // FOR loop (line-start, allowing leading whitespace)
		while( i < t.size() ) {
			size_t e = t.find('\n', i); if( e == std::string::npos ) e = t.size();
			size_t b = i; while( b < e && (t[b]==' '||t[b]=='\t') ) ++b;
			if( e - b >= 4 && t.compare(b,4,"FOR ")==0 ) return "for";
			i = e + 1;
		}
	}
	if( t.find("DEFINE ") != std::string::npos ) return "define";
	if( t.find("$(") != std::string::npos ) return "expr";
	if( t.find("> set") != std::string::npos ) return "set";
	if( t.find("> modify") != std::string::npos || t.find("> echo") != std::string::npos ) return "command";
	return "other";
}

// KNOWN-ACCEPTED divergences (user-ratified 2026-06-25): a mismatch here is EXPECTED, not a regression,
// so the gate does not fail on it (and CI fails only on an UNEXPECTED mismatch).
static std::string KnownAccepted( const std::string& path )
{
	if( path.find( "SubsurfaceScattering/sss.RISEscene" ) != std::string::npos ||
	    path.find( "SubsurfaceScattering/sss_multiple_lights.RISEscene" ) != std::string::npos )
		return "legacy s_painterColors include-isolation quirk -- the CST energy-conserves the translucent material (more physically correct than legacy); accepted";
	return "";
}

// A render-AFFECTING `>` directive survives migration but is SILENTLY dropped by CST-load (DeriveToJob skips
// `>` lines), so a scene carrying one mis-renders if CST-loaded.  These are everything EXCEPT the proven
// render-neutral `> echo` / `> set accelerator` (`> run`/`> load`/`> set global_medium` are flattened/converted
// away by the migrator -> never present here).  An INDEPENDENT oracle mirroring IsNativeV7Document's accept set
// (the gate must not merely re-run the function under test).
static bool HasRenderAffectingDirective( const std::string& migrated )
{
	// Strip /* */ block + # line comments first: a COMMENTED-OUT `>` line (e.g. watch_dial's disabled
	// night-mode `/* > modify ... */` block) is NOT a live directive.  ParseToCst -- hence IsNativeV7Document --
	// already ignores comments (Trivia); this independent text oracle must match, else it false-flags.
	std::string s; s.reserve( migrated.size() );
	for( size_t i = 0; i < migrated.size(); ) {
		if( migrated[i] == '/' && i + 1 < migrated.size() && migrated[i+1] == '*' ) {   // block comment
			i += 2;
			while( i + 1 < migrated.size() && !( migrated[i] == '*' && migrated[i+1] == '/' ) ) ++i;
			i = ( i + 2 <= migrated.size() ) ? i + 2 : migrated.size();
			continue;
		}
		if( migrated[i] == '#' ) { while( i < migrated.size() && migrated[i] != '\n' ) ++i; continue; }   // line comment
		s += migrated[i++];
	}
	std::istringstream is( s );
	std::string ln;
	while( std::getline( is, ln ) ) {
		const size_t p = ln.find_first_not_of( " \t" );
		if( p == std::string::npos || ln[p] != '>' ) continue;
		std::istringstream ts( ln.substr( p + 1 ) );
		std::string a, b; ts >> a >> b;
		if( a == "echo" ) continue;
		if( a == "set" && b == "accelerator" ) continue;
		return true;   // a LIVE > modify / > set <other> => render-affecting
	}
	return false;
}

// SELF-TEST (runs with NO scene root, so the unit suite exercises it without the media corpus): the
// migrator macro surface must match legacy's -- define = DEFINE|define (legacy's leading-'!' define is a
// non-functional parse-fail, NOT honored); undef = UNDEF|undef|'~' -- plus `> set global_medium` -> chunk.
// The bang case is checked against the ACTUAL legacy parser below (asserts legacy rejects it), so the
// oracle is migrator-vs-legacy, not migrator-vs-migrator.
static int RunSelfTest()
{
	int fails = 0;
	auto CHECK = [&]( bool c, const char* m ){ if(!c){ ++fails; std::printf("  SELFTEST FAIL: %s\n", m); } };

	const std::string base = "sphere_geometry\n{\nname s\nradius @CY\n}\n";
	const std::string up = Migrate( "DEFINE CY 5\n" + base );
	const std::string lo = Migrate( "define CY 5\n" + base );
	CHECK( up == lo, "lowercase 'define' == 'DEFINE'" );
	CHECK( up.find( "@CY" ) == std::string::npos, "@CY substituted (macro took effect)" );
	CHECK( up.find( "DEFINE" ) == std::string::npos && up.find( "define" ) == std::string::npos, "define directive consumed" );
	// Legacy's leading-'!' define parse-FAILS, so the migrator must NOT honor '! CY 5' as a define:
	// CY never gets defined, so @CY stays unsubstituted.
	const std::string bg = Migrate( "! CY 5\n" + base );
	CHECK( bg.find( "@CY" ) != std::string::npos, "'! CY 5' NOT honored as a define; @CY left as-is" );

	const std::string ub = "sphere_geometry\n{\nname s\nradius 1\n}\n";
	const std::string u1 = Migrate( "DEFINE Q 1\nUNDEF Q\n" + ub );
	const std::string u2 = Migrate( "DEFINE Q 1\nundef Q\n" + ub );
	const std::string u3 = Migrate( "DEFINE Q 1\n~ Q\n"     + ub );
	CHECK( u1 == u2, "lowercase 'undef' == 'UNDEF'" );
	CHECK( u1 == u3, "tilde '~ Q' == 'UNDEF'" );
	CHECK( u1.find( "UNDEF" ) == std::string::npos && u1.find( "undef" ) == std::string::npos, "undef directive consumed" );

	// Oracle vs the ACTUAL legacy parser: `! CY 5` is a legacy PARSE-FAIL, confirming the non-handling above
	// is faithful (a legacy quirk, not a dropped feature).
	{
		const char* bp = "/tmp/cst_selftest_bang.RISEscene";
		{ std::ofstream bf( bp ); bf << "RISE ASCII SCENE 6\n! CY 5\n"; }
		Job* jb = new Job(); ISceneParser* pp = 0; bool okb = true;
		if( RISE_API_CreateAsciiSceneParser( &pp, bp ) && pp ) { okb = pp->ParseAndLoadScene( *jb ); pp->release(); }
		jb->release(); std::remove( bp );
		CHECK( !okb, "legacy REJECTS '! CY 5' (leading-'!' define non-functional) -- migrator matches" );
	}

	const std::string gm = Migrate( "> set global_medium fog\n" );
	CHECK( gm.find( "global_medium\n{\nmedium fog\n}" ) != std::string::npos, "'> set global_medium' -> chunk" );

	// hal() FOLDS via the per-top-level-scene Halton: Migrate() Reset()s g_migratorHalton at its start
	// (mirroring the legacy parser's per-top-level `mh` reset), so the first sample of any dimension is the
	// radical inverse of 0 = 0 and $(hal(0)+1.0) folds to exactly 1.0.
	const std::string hf = Migrate( "sphere_geometry\n{\nname s\nradius $(hal(0)+1.0)\n}\n" );
	CHECK( hf.find( "hal(" ) == std::string::npos, "hal() is folded (no $()/hal remains)" );
	CHECK( hf.find( "1.00000000000000000" ) != std::string::npos, "hal(0) first sample 0 -> $(hal(0)+1.0) folds to 1.0" );

	// P1 (order-independence): the folded hal() literals must NOT depend on what was migrated earlier in the
	// same process (corpus batch order).  Migrate scene A fresh, then again AFTER a different hal scene B that
	// advances the sequence -- the two A migrations must be byte-identical.  (Pre-fix, with a never-reset
	// global Halton, the second A folds hal() at a higher index and DIFFERS.)  Probe DIMENSION 1: dim 0's
	// bit-reversed Halton is mod1(integer)=0 for small indices, so it cannot witness an index shift.
	const std::string halA = "sphere_geometry\n{\nname a\nradius $(hal(1)+1.0)\n}\n";
	const std::string halB = "a $(hal(1))\nb $(hal(1))\nc $(hal(1))\nd $(hal(1))\n";
	const std::string a1 = Migrate( halA );
	(void) Migrate( halB );
	const std::string a2 = Migrate( halA );
	CHECK( a1 == a2, "Migrate(hal scene) is order-independent (Halton Reset() per top-level scene)" );

	// P1 (SCENE-6 dual-readable): inlining a `> run X.RISEscript` must STRIP the script's `RISE ASCII
	// SCRIPT N` version header -- else a stray header line is spliced into the scene body, which the legacy
	// v6 reader rejects as an unknown chunk.  Write a temp script (with a header), migrate a scene that runs
	// it, and assert (a) no stray version header survives and (b) the migrated text LEGACY-parses.
	{
		const char* sp = "/tmp/cstmig_selftest_script.RISEscript";
		const char* sc = "/tmp/cstmig_selftest_scene.RISEscene";
		{ std::ofstream f( sp ); f << "RISE ASCII SCRIPT 3\nsphere_geometry\n{\nname sst\nradius 1.0\n}\n"; }
		const std::string mig = Migrate( std::string( "RISE ASCII SCENE 6\n> run " ) + sp + "\n" );
		CHECK( mig.find( "RISE ASCII SCRIPT" ) == std::string::npos, "inlined script version header is stripped (no stray header)" );
		CHECK( mig.find( "sphere_geometry" ) != std::string::npos, "inlined script body survives the header strip" );
		{ std::ofstream f( sc ); f << mig; }
		Job* js = new Job(); ISceneParser* ps = 0; bool oks = false;
		if( RISE_API_CreateAsciiSceneParser( &ps, sc ) && ps ) { oks = ps->ParseAndLoadScene( *js ); ps->release(); }
		js->release();
		CHECK( oks, "legacy parser ACCEPTS the migrated `> run script` output (SCENE-6 dual-readable)" );
		std::remove( sp ); std::remove( sc );
	}

	std::printf( "CstCorpusEquivalenceTest SELF-TEST: %s (%d failure[s])\n", fails ? "FAIL" : "PASS", fails );
	return fails ? 1 : 0;
}

int main( int argc, char** argv )
{
	if( argc < 2 ) { std::printf("CstCorpusEquivalenceTest: MANUAL gate -- pass a scene root (e.g. \"scenes\") to run the\n  corpus legacy-vs-CST(migrated) comparison.  Running SELF-TEST only (suite-safe; the corpus gate needs the media root).\n"); return RunSelfTest(); }
	const std::string rootArg = argv[1];
	char cwd[4096]; if( !getcwd( cwd, sizeof(cwd) ) ) { std::printf("getcwd failed\n"); return 2; }
	setenv( "RISE_MEDIA_PATH", (std::string(cwd) + "/").c_str(), 1 );
	// single-scene debug: dump legacy(original) + CST(migrated) to /tmp and report the diags.
	if( rootArg.size() > 10 && rootArg.substr( rootArg.size()-10 ) == ".RISEscene" ) {
		const std::string text = ReadFile( rootArg );
		Job* jL = new Job(); ISceneParser* p = 0; bool okL = false;
		if( RISE_API_CreateAsciiSceneParser( &p, rootArg.c_str() ) && p ) { okL = p->ParseAndLoadScene( *jL ); p->release(); }
		Job* jC = new Job(); std::vector<std::string> diags; Document d = ParseToCst( Migrate( text ) ); DeriveToJob( d, *jC, &diags );
		{ std::ofstream f( "/tmp/cst_L.txt" ); f << ( okL ? DumpJob( *jL ) : std::string("(legacy-fail)") ); }
		{ std::ofstream f( "/tmp/cst_C.txt" ); f << DumpJob( *jC ); }
		std::printf( "wrote /tmp/cst_L.txt (legacy) + /tmp/cst_C.txt (CST-migrated); okL=%d diags=%zu\n", (int)okL, diags.size() );
		for( size_t i = 0; i < diags.size() && i < 12; ++i ) std::printf( "  diag: %s\n", diags[i].c_str() );
		jL->release(); jC->release(); return 0;
	}

	std::printf( "CstCorpusEquivalenceTest -- legacy(original) vs CST(migrated) DumpJob over %s/\n", rootArg.c_str() );

	std::vector<std::string> paths;
	{
		const std::string cmd = "find " + rootArg + " -name '*.RISEscene' | sort";
		FILE* pp = popen( cmd.c_str(), "r" );
		if( !pp ) { std::printf("popen(find) failed\n"); return 2; }
		char line[8192];
		while( std::fgets( line, sizeof(line), pp ) ) {
			std::string s( line );
			while( !s.empty() && (s.back()=='\n'||s.back()=='\r') ) s.pop_back();
			if( !s.empty() ) paths.push_back( s );
		}
		pclose( pp );
	}

	int match = 0, mismatch = 0, legacyFail = 0, accepted = 0, falseReject = 0, falseAccept = 0, deferredDirective = 0;
	std::map<std::string,int> mmReason, lfReason;
	for( size_t k = 0; k < paths.size(); ++k ) {
		const std::string& path = paths[k];
		const std::string text = ReadFile( path );
		const std::string migrated = Migrate( text );

		// legacy: parse the actual ORIGINAL file from the repo root (relative includes/media resolve here)
		Job* jL = new Job();
		ISceneParser* p = 0; bool okL = false;
		if( RISE_API_CreateAsciiSceneParser( &p, path.c_str() ) && p ) { okL = p->ParseAndLoadScene( *jL ); p->release(); }
		const std::string dumpL = okL ? DumpJob( *jL ) : std::string("(legacy-fail)");

		// CST: derive the MIGRATED text
		Job* jC = new Job();
		std::vector<std::string> diags;
		Document d = ParseToCst( migrated );
		DeriveToJob( d, *jC, &diags );
		const std::string dumpC = DumpJob( *jC );

		if( !okL ) { ++legacyFail; ++lfReason[Reason(migrated)]; std::printf("LEGACY-FAIL[%-7s] %s\n", Reason(migrated).c_str(), path.c_str()); }
		else if( dumpL == dumpC ) {
			++match;
			// A MATCH (DumpJob-identical) scene SHOULD be CST-loadable -- UNLESS it carries a render-affecting `>`
			// directive (DumpJob is blind to those; CST-load drops them).  Bidirectional: render-affecting => MUST be
			// refused (else FALSE-ACCEPT = a silent mis-render); otherwise => MUST be accepted (else FALSE-REJECT).
			const bool affecting = HasRenderAffectingDirective( migrated );
			const bool native    = IsNativeV7Document( d );
			if( affecting && native ) { ++falseAccept;
				std::printf("FALSE-ACCEPT (render-affecting > directive but IsNativeV7Document=true -> CST-load would silently drop it) %s\n", path.c_str()); }
			else if( !affecting && !native ) { ++falseReject;
				std::printf("FALSE-REJECT (no render-affecting directive but IsNativeV7Document=false -> would refuse a CST-loadable scene) %s\n", path.c_str()); }
			else if( affecting ) { ++deferredDirective;
				std::printf("DEFERRED-DIRECTIVE (render-affecting > directive; correctly refused, pending Slice-2 migrator conversion) %s\n", path.c_str()); }
		}
		else {
			const std::string acc = KnownAccepted( path );
			if( !acc.empty() ) { ++accepted; std::printf("ACCEPTED            %s -- %s\n", path.c_str(), acc.c_str()); }
			else { ++mismatch; const std::string r = Reason(migrated); ++mmReason[r]; std::printf("MISMATCH[%-7s] %s\n", r.c_str(), path.c_str()); }
		}
		std::fflush( stdout );
		jL->release(); jC->release();
	}

	std::printf( "\n=== %zu scenes: %d MATCH, %d ACCEPTED, %d MISMATCH(unexpected), %d LEGACY-FAIL, %d FALSE-REJECT, %d FALSE-ACCEPT, %d DEFERRED-DIRECTIVE ===\n", paths.size(), match, accepted, mismatch, legacyFail, falseReject, falseAccept, deferredDirective );
	for( std::map<std::string,int>::const_iterator it = mmReason.begin(); it != mmReason.end(); ++it )
		std::printf( "  MISMATCH[%-7s] = %d\n", it->first.c_str(), it->second );
	for( std::map<std::string,int>::const_iterator it = lfReason.begin(); it != lfReason.end(); ++it )
		std::printf( "  LEGACY-FAIL[%-7s] = %d\n", it->first.c_str(), it->second );
	return ( mismatch > 0 || falseReject > 0 || falseAccept > 0 ) ? 1 : 0;   // CI: fail on an UNEXPECTED mismatch, a false-reject, OR a false-accept (silent mis-render)
}
