//////////////////////////////////////////////////////////////////////
//
//  CstCorpusEquivalenceTest.cpp - the v6->v7 migrator's BASELINE GATE.
//  For every scene under a root dir (argv[1], default "scenes"), parse it
//  via the LEGACY AsciiSceneParser and DERIVE it via the CST, then compare
//  DumpJob.  A MATCH means the CST already derives that scene identically to
//  legacy today (needs no migration); a MISMATCH is categorized by the v6-only
//  construct that explains it (include / FOR / DEFINE / expr / command / other)
//  -- the migrator's true work-list.
//
//  Both paths run from the REPO ROOT (cwd): scenes reference repo-root-relative
//  `> run`/media paths, so legacy (given the full scene path) and the CST
//  (deriving the text, media resolved cwd-relative) resolve identically.
//  Read-only: no scene file is modified.  Per-scene status is flushed so a crash
//  names the offending scene.  NOTE: this is a scoping/gate harness -- it loads
//  media for the whole corpus, so it is run MANUALLY (not part of the fast
//  suite) until the migrator productionizes it.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static std::string ReadFile( const std::string& p )
{
	std::ifstream f( p.c_str(), std::ios::binary );
	std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Categorize a mismatch / legacy-fail by the FIRST v6-only construct present (priority order).
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

int main( int argc, char** argv )
{
	if( argc < 2 ) { std::printf("CstCorpusEquivalenceTest: MANUAL gate -- pass a scene root (e.g. \"scenes\") to run the\n  corpus legacy-vs-CST comparison.  Skipped (suite-safe: it loads media for the whole corpus).\n"); return 0; }
	const std::string rootArg = argv[1];
	char cwd[4096]; if( !getcwd( cwd, sizeof(cwd) ) ) { std::printf("getcwd failed\n"); return 2; }
	setenv( "RISE_MEDIA_PATH", (std::string(cwd) + "/").c_str(), 1 );

	std::printf( "CstCorpusEquivalenceTest -- legacy-vs-CST DumpJob over %s/\n", rootArg.c_str() );

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

	int match = 0, mismatch = 0, legacyFail = 0;
	std::map<std::string,int> mmReason, lfReason;
	for( size_t k = 0; k < paths.size(); ++k ) {
		const std::string& path = paths[k];
		const std::string text = ReadFile( path );

		// legacy: parse the actual file from the repo root (relative includes/media resolve here)
		Job* jL = new Job();
		ISceneParser* p = 0; bool okL = false;
		if( RISE_API_CreateAsciiSceneParser( &p, path.c_str() ) && p ) { okL = p->ParseAndLoadScene( *jL ); p->release(); }
		const std::string dumpL = okL ? DumpJob( *jL ) : std::string("(legacy-fail)");

		// CST: derive the text
		Job* jC = new Job();
		std::vector<std::string> diags;
		Document d = ParseToCst( text );
		DeriveToJob( d, *jC, &diags );
		const std::string dumpC = DumpJob( *jC );

		if( !okL ) { ++legacyFail; ++lfReason[Reason(text)]; std::printf("LEGACY-FAIL[%-7s] %s\n", Reason(text).c_str(), path.c_str()); }
		else if( dumpL == dumpC ) { ++match; }
		else { ++mismatch; const std::string r = Reason(text); ++mmReason[r]; std::printf("MISMATCH[%-7s] %s\n", r.c_str(), path.c_str()); }
		std::fflush( stdout );
		jL->release(); jC->release();
	}

	std::printf( "\n=== %zu scenes: %d MATCH, %d MISMATCH, %d LEGACY-FAIL ===\n", paths.size(), match, mismatch, legacyFail );
	for( std::map<std::string,int>::const_iterator it = mmReason.begin(); it != mmReason.end(); ++it )
		std::printf( "  MISMATCH[%-7s] = %d\n", it->first.c_str(), it->second );
	for( std::map<std::string,int>::const_iterator it = lfReason.begin(); it != lfReason.end(); ++it )
		std::printf( "  LEGACY-FAIL[%-7s] = %d\n", it->first.c_str(), it->second );
	return 0;
}
