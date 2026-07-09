//////////////////////////////////////////////////////////////////////
//  MigrateScenesV6toV7.cpp -- the canonical v6->v7 scene migrator tool.
//  Wraps the shared Migrate() (tools/CstMigrator.h) -- the SAME transform the equivalence gate
//  (CstCorpusEquivalenceTest) verifies -- so the tool output is byte-identical to the gated migrator
//  BY CONSTRUCTION.  Phase A of docs/agentic-redesign/61-v6v7-parser-cutover-execution-plan.md.
//
//  Usage:  MigrateScenesV6toV7 <scene.RISEscene> [more ...]
//    Prints each input's migrated text to stdout.  Constructs are folded toward v7 AND the scene-version
//    header is bumped `RISE ASCII SCENE 6` -> `... 7` (Phase C).  A fresh conversion therefore emits a
//    post-cutover `7`-headered scene; the reader still accepts `6` for back-compat.
//
//  Exit: 0 = all inputs migrated; 1 = an input was empty or unreadable (named on stderr); 2 = no args.
//////////////////////////////////////////////////////////////////////
#include <cstdio>
#include <string>
#include "CstMigrator.h"

int main( int argc, char** argv )
{
	if( argc < 2 ) {
		std::fprintf( stderr, "usage: MigrateScenesV6toV7 <scene.RISEscene> [more ...]\n"
		                      "  prints each input's migrated text to stdout (folded; header bumped to SCENE 7)\n" );
		return 2;
	}
	int failures = 0;
	for( int i = 1; i < argc; ++i ) {
		const std::string src = ReadFile( argv[i] );   // "" on missing / unreadable / directory / empty
		if( src.empty() ) { std::fprintf( stderr, "MigrateScenesV6toV7: '%s' is empty or unreadable (skipped)\n", argv[i] ); ++failures; continue; }
		const std::string v7 = BumpSceneHeader( Migrate( src ) );   // fold body, then stamp the SCENE 7 header (Phase C)
		std::fputs( v7.c_str(), stdout );
	}
	return failures ? 1 : 0;
}
