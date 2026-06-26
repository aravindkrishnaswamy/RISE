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
#include "../src/Library/Parsers/MathExpressionEvaluator.h"

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

static std::string ReadFile( const std::string& p )
{
	std::ifstream f( p.c_str(), std::ios::binary );
	std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string Trim( const std::string& s )
{
	size_t b = 0, e = s.size();
	while( b < e && (s[b]==' '||s[b]=='\t'||s[b]=='\r') ) ++b;
	while( e > b && (s[e-1]==' '||s[e-1]=='\t'||s[e-1]=='\r') ) --e;
	return s.substr( b, e - b );
}

// Strip /* */ block-comment spans from one line, carrying the open/closed state
// across lines via `inComment`.  Returns the line's NON-comment text (so a
// directive hidden inside a comment is invisible to the include scan).
static std::string StripBlockComments( const std::string& line, bool& inComment )
{
	std::string out; size_t i = 0;
	while( i < line.size() ) {
		if( inComment ) {
			size_t e = line.find( "*/", i );
			if( e == std::string::npos ) { i = line.size(); }
			else { inComment = false; i = e + 2; }
		} else {
			size_t b = line.find( "/*", i );
			if( b == std::string::npos ) { out += line.substr( i ); i = line.size(); }
			else { out += line.substr( i, b - i ); inComment = true; i = b + 2; }
		}
	}
	return out;
}

// Recursively inline `> run` / `> load` includes (comment-aware), depth-capped
// against cycles.  An include path is repo-root-relative (read from cwd).
static std::string FlattenIncludes( const std::string& text, int depth )
{
	if( depth > 24 ) return text;                              // cycle / runaway guard
	std::string out; bool inComment = false; size_t i = 0;
	while( true ) {
		size_t e = text.find( '\n', i ); const bool last = ( e == std::string::npos ); if( last ) e = text.size();
		const std::string line = text.substr( i, e - i );
		const bool wasInComment = inComment;
		const std::string code = Trim( StripBlockComments( line, inComment ) );   // non-comment text; updates inComment

		std::string incPath;
		// scene context uses `> run`/`> load`; SCRIPT context (an inlined .RISEscript) uses BARE run/load.
		if( !wasInComment && ( (!code.empty() && code[0]=='>') || code.compare(0,4,"run ")==0 || code.compare(0,5,"load ")==0 ) ) {
			std::istringstream iss( code ); std::string tk; std::vector<std::string> toks;
			while( iss >> tk ) toks.push_back( tk );
			if( toks.size() >= 3 && toks[0] == ">" && ( toks[1] == "run" || toks[1] == "load" ) ) incPath = toks[2];       // > run/load X
			else if( toks.size() >= 2 && ( toks[0] == "run" || toks[0] == "load" ) ) incPath = toks[1];                    // bare run/load X (script)
		}
		if( !incPath.empty() ) {
			std::ifstream f( incPath.c_str(), std::ios::binary );
			if( f ) { std::stringstream ss; ss << f.rdbuf(); out += FlattenIncludes( ss.str(), depth + 1 ); if( !out.empty() && out.back() != '\n' ) out += '\n'; }
			else { out += line; if( !last ) out += '\n'; }     // missing include: keep the directive (legacy fails too)
		} else {
			out += line; if( !last ) out += '\n';
		}
		if( last ) break;
		i = e + 1;
	}
	return out;
}

// Substitute @NAME / !NAME macro refs in-place (exactly substitute_macro): a name is [A-Z_]+; @ -> %.12f
// of the value, ! -> %.4d of (int)value; an unknown macro is left as-is.
static void SubstituteMacrosInPlace( std::string& s, const std::map<std::string,double>& macros )
{
	size_t i = 0;
	while( i < s.size() ) {
		if( s[i] == '@' || s[i] == '!' ) {
			const char mc = s[i];
			size_t j = i + 1;
			while( j < s.size() && ( ( s[j] >= 'A' && s[j] <= 'Z' ) || s[j] == '_' ) ) ++j;
			const std::string name = s.substr( i + 1, j - ( i + 1 ) );
			std::map<std::string,double>::const_iterator it = macros.find( name );
			if( !name.empty() && it != macros.end() ) {
				char buf[64];
				if( mc == '@' ) std::snprintf( buf, sizeof(buf), "%.12f", it->second );
				else            std::snprintf( buf, sizeof(buf), "%.4d", (int)it->second );
				s = s.substr( 0, i ) + buf + s.substr( j );
				i += std::strlen( buf );
			} else { i = j; }
		} else ++i;
	}
}

// --- $() fold: a faithful port of the legacy evaluate_functions/evaluate_expression (same
// MathExpressionEvaluator + the same per-function %.17f intermediate rounding, so the folded literal
// is byte-identical to what the legacy parser produced).  hal() is DEFERRED (a stateful halton seq).
static int FoldFirstFunction( std::string& token )
{
	std::string str = token; std::string processed;
	std::string::size_type x = str.find_first_of( "scth" );
	if( x == std::string::npos ) return 0;
	if( x > 0 ) { processed = str.substr( 0, x ); str = str.substr( x ); }
	const std::string::size_type px = str.find_first_of( "(" );  if( px == std::string::npos ) return 2;
	const std::string::size_type py = str.find_first_of( ")" );  if( py == std::string::npos ) return 2;
	const std::string szexpr = str.substr( px, py - px + 1 );
	MathExpressionEvaluator::Expression expr( szexpr.c_str() );
	if( expr.error() ) return 2;
	double val = 0;
	switch( str[0] ) {
		case 's': if( str[1]=='i' && str[2]=='n' ) val = std::sin( (double)expr.eval() );
		          else if( str[1]=='q' && str[2]=='r' && str[3]=='t' ) val = std::sqrt( (double)expr.eval() );
		          else return 2; break;
		case 'c': if( str[1]=='o' && str[2]=='s' ) val = std::cos( (double)expr.eval() ); else return 2; break;
		case 't': if( str[1]=='a' && str[2]=='n' ) val = std::tan( (double)expr.eval() ); else return 2; break;
		case 'h': return 2;   // hal(): deferred (stateful halton sequence)
		default:  return 2;
	}
	char buf[512]; std::snprintf( buf, sizeof(buf), "%.17f", val );
	processed.append( buf ); processed.append( str.substr( py + 1 ) );
	token = processed; return 1;
}
static bool FoldFunctions( std::string& token )
{
	for(;;) { const int c = FoldFirstFunction( token ); if( c == 0 ) return true; if( c == 2 ) return false; }
}
static bool FoldExpr( std::string& token )   // "$(...)" -> %.17f of its value
{
	if( token.size() <= 4 || token[0] != '$' || token[1] != '(' || token[token.size()-1] != ')' ) return false;
	if( !FoldFunctions( token ) ) return false;
	MathExpressionEvaluator::Expression expr( token.c_str() + 1 );   // skip '$', eval "(...)"
	if( expr.error() ) return false;
	char buf[512]; std::snprintf( buf, sizeof(buf), "%.17f", (double)expr.eval() );
	token = buf; return true;
}
// Fold every $(...) token in a line in place (balanced-paren match; an unfoldable one, e.g. hal, is left).
static void FoldExprsInPlace( std::string& s )
{
	size_t i = 0;
	while( ( i = s.find( "$(", i ) ) != std::string::npos ) {
		int depth = 0; size_t j = i + 1;
		for( ; j < s.size(); ++j ) { if( s[j]=='(' ) ++depth; else if( s[j]==')' ) { if( --depth == 0 ) { ++j; break; } } }
		std::string tok = s.substr( i, j - i );
		if( FoldExpr( tok ) ) { s = s.substr( 0, i ) + tok + s.substr( j ); i += tok.size(); } else i = j;
	}
}

// Recursive line processor (the migrator's slice 2+3): DEFINE/UNDEF (removed) + FOR unroll (do-while,
// var as a macro, nested) + @/! substitution + $() fold.  A faithful port of the legacy preprocessor;
// document-order macro scope, comment-aware directive/FOR detection.
static void ProcessLines( const std::vector<std::string>& lines, size_t lo, size_t hi,
                          std::map<std::string,double>& macros, std::string& out, bool& inComment )
{
	for( size_t i = lo; i < hi; ) {
		const std::string& line = lines[i];
		const bool wasIn = inComment;
		std::string code = StripBlockComments( line, inComment );
		const size_t hh = code.find( '#' ); if( hh != std::string::npos ) code = code.substr( 0, hh );
		const std::string tt = Trim( code );
		std::vector<std::string> toks;
		if( !wasIn && !tt.empty() ) { std::istringstream iss(tt); std::string tk; while( iss>>tk ) toks.push_back(tk); }
		if( toks.size() >= 3 && toks[0] == "DEFINE" ) { std::string dv = toks[2]; SubstituteMacrosInPlace( dv, macros ); FoldExprsInPlace( dv ); macros[toks[1]] = atof( dv.c_str() ); ++i; continue; }
		if( toks.size() >= 2 && toks[0] == "UNDEF" )  { macros.erase(toks[1]); ++i; continue; }
		if( toks.size() >= 5 && toks[0] == "FOR" ) {
			std::string fl = tt; SubstituteMacrosInPlace( fl, macros ); FoldExprsInPlace( fl );
			std::vector<std::string> ft; { std::istringstream iss(fl); std::string tk; while( iss>>tk ) ft.push_back(tk); }
			const std::string var = ft.size()>1 ? ft[1] : std::string();
			const double start = ft.size()>2 ? atof(ft[2].c_str()) : 0.0;
			const double endv  = ft.size()>3 ? atof(ft[3].c_str()) : 0.0;
			const double inc   = ft.size()>4 ? atof(ft[4].c_str()) : 1.0;
			size_t bodyStart = i + 1, j = bodyStart; int nest = 1; bool sc = inComment;
			for( ; j < hi; ++j ) {
				std::string c2 = StripBlockComments( lines[j], sc ); size_t h2 = c2.find('#'); if( h2!=std::string::npos ) c2 = c2.substr(0,h2);
				std::string t2 = Trim( c2 );
				if( t2.compare(0,3,"FOR")==0 && ( t2.size()==3 || t2[3]==' ' || t2[3]=='\t' ) ) ++nest;
				else if( t2 == "ENDFOR" ) { if( --nest == 0 ) break; }
			}
			const size_t endforIdx = j;
			const bool snap = inComment;
			double v = start;
			do { macros[var] = v; bool ic = snap; ProcessLines( lines, bodyStart, endforIdx, macros, out, ic ); v += inc; } while( v <= endv );
			macros.erase( var );
			{ bool ic = snap; for( size_t k = bodyStart; k < endforIdx; ++k ) StripBlockComments( lines[k], ic ); inComment = ic; }
			i = endforIdx + 1; continue;
		}
		if( toks.size() >= 4 && toks[0] == ">" && toks[1] == "set" && toks[2] == "global_medium" ) {
			out += "global_medium\n{\nmedium " + toks[3] + "\n}\n"; ++i; continue;   // v6 `> set global_medium X` -> v7 chunk (at the `> set` position -- after the medium def, so SetGlobalMedium's definitions-before-use order holds)
		}
		std::string ln = line; SubstituteMacrosInPlace( ln, macros ); FoldExprsInPlace( ln );
		out += ln; out += '\n'; ++i;
	}
}
static std::string Preprocess( const std::string& text )
{
	std::vector<std::string> lines;
	{ size_t i=0; for(;;) { size_t e=text.find('\n',i); const bool last=(e==std::string::npos); if(last)e=text.size(); lines.push_back(text.substr(i,e-i)); if(last)break; i=e+1; } }
	std::map<std::string,double> macros; macros["PI"]=M_PI; macros["E"]=M_E; std::string out; bool inComment=false;
	ProcessLines( lines, 0, lines.size(), macros, out, inComment );
	return out;
}

// The offline v6->v7 migrator transform (text -> text).  Grows per slice.
static std::string Migrate( const std::string& text )
{
	return Preprocess( FlattenIncludes( text, 0 ) );         // flatten includes; then DEFINE/UNDEF/FOR/@/!/$()
}

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

int main( int argc, char** argv )
{
	if( argc < 2 ) { std::printf("CstCorpusEquivalenceTest: MANUAL gate -- pass a scene root (e.g. \"scenes\") to run the\n  corpus legacy-vs-CST(migrated) comparison.  Skipped (suite-safe: it loads media for the whole corpus).\n"); return 0; }
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

	int match = 0, mismatch = 0, legacyFail = 0;
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
		else if( dumpL == dumpC ) { ++match; }
		else { ++mismatch; const std::string r = Reason(migrated); ++mmReason[r]; std::printf("MISMATCH[%-7s] %s\n", r.c_str(), path.c_str()); }
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
