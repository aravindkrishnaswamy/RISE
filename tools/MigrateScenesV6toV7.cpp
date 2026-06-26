//////////////////////////////////////////////////////////////////////
//  MigrateScenesV6toV7.cpp -- the canonical v6->v7 scene migrator tool.
//  Wraps the shared Migrate() (tools/CstMigrator.h) -- the SAME transform the equivalence gate
//  (CstCorpusEquivalenceTest) verifies -- so the tool output is byte-identical to the gated migrator
//  BY CONSTRUCTION.  Phase A of docs/agentic-redesign/61-v6v7-parser-cutover-execution-plan.md.
//
//  Usage:  MigrateScenesV6toV7 <scene.RISEscene> [more ...]
//    Prints each input's migrated v7 text to stdout.  Corpus file-rewriting (in-place / out-dir) is
//    Phase C; this Phase-A tool is the migrator made first-class + runnable.
//////////////////////////////////////////////////////////////////////
#include <cstdio>
#include <string>
#include "CstMigrator.h"

int main( int argc, char** argv )
{
	if( argc < 2 ) {
		std::fprintf( stderr, "usage: MigrateScenesV6toV7 <scene.RISEscene> [more ...]\n"
		                      "  prints each input's migrated v7 text to stdout\n" );
		return 2;
	}
	for( int i = 1; i < argc; ++i ) {
		const std::string v7 = Migrate( ReadFile( argv[i] ) );
		std::fputs( v7.c_str(), stdout );
	}
	return 0;
}
