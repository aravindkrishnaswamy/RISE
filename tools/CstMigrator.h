#ifndef RISE_CST_MIGRATOR_H
#define RISE_CST_MIGRATOR_H
// Shared v6->v7 scene migrator (text -> text): flatten includes (D7) + the legacy macro / $() / FOR / hal
// surface, folded to the v7 declarative form -- BODY ONLY: the scene-version header line stays
// `RISE ASCII SCENE 6` until Phase C bumps it to 7.  Extracted from CstCorpusEquivalenceTest so the equivalence
// GATE and the migration TOOL (tools/MigrateScenesV6toV7) share ONE migrator -- byte-identical by
// construction.  Phase A of docs/agentic-redesign/61-v6v7-parser-cutover-execution-plan.md.
//
// Header-local `static` functions + state: each translation unit that includes this (the gate, the tool --
// separate executables) gets its own copy + its own g_migratorHalton.  That is correct: each PROCESS
// accumulates the Halton sequence in its own scene order, mirroring the legacy parser's file-static `mh`.
// INVARIANT: include this into at most ONE translation unit per executable -- two includers linked into the
// same binary would each get an independent g_migratorHalton, silently diverging their hal() folds.
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "../src/Library/Parsers/MathExpressionEvaluator.h"
#include "../src/Library/Sampling/HaltonPoints.h"

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
// is byte-identical to what the legacy parser produced).  hal() folds via the process-global
// g_migratorHalton below.

// Process-global Halton mirroring the legacy parser's file-static `mh` (AsciiSceneParser.cpp): it
// accumulates across scenes in corpus order and is NEVER reset, so the migrator's per-dimension halton
// state tracks legacy's EXACTLY across every scene that uses hal() (diamond_teapot_pour sorts first and
// gets the index-0 samples; painters picks up where diamond left off) -- the only way a stateful seq
// can be folded deterministically to match legacy.
static RISE::MultiHalton g_migratorHalton;
static int FoldFirstFunction( std::string& token )
{
	std::string str = token; std::string processed;
	std::string::size_type x = str.find_first_of( "scth" );
	if( x == std::string::npos ) return 0;
	if( x > 0 ) { processed = str.substr( 0, x ); str = str.substr( x ); }
	const std::string::size_type px = str.find_first_of( "(" );  if( px == std::string::npos ) return 2;
	const std::string::size_type py = str.find_first_of( ")" );  if( py == std::string::npos ) return 2;
	const std::string szexpr = str.substr( px, py - px + 1 );
	RISE::MathExpressionEvaluator::Expression expr( szexpr.c_str() );
	if( expr.error() ) return 2;
	double val = 0;
	switch( str[0] ) {
		case 's': if( str[1]=='i' && str[2]=='n' ) val = std::sin( (double)expr.eval() );
		          else if( str[1]=='q' && str[2]=='r' && str[3]=='t' ) val = std::sqrt( (double)expr.eval() );
		          else return 2; break;
		case 'c': if( str[1]=='o' && str[2]=='s' ) val = std::cos( (double)expr.eval() ); else return 2; break;
		case 't': if( str[1]=='a' && str[2]=='n' ) val = std::tan( (double)expr.eval() ); else return 2; break;
		case 'h':
			if( str[1]=='a' && str[2]=='l' ) {
				const int d = int( expr.eval() );
				if( d < 0 || d >= RISE::QMC_NUM_PRIMES ) return 2;
				val = g_migratorHalton.next_halton( d );
			} else return 2;
			break;
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
	RISE::MathExpressionEvaluator::Expression expr( token.c_str() + 1 );   // skip '$', eval "(...)"
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
		// Legacy macro surface, VERIFIED against the legacy parser: define = DEFINE | define.  Legacy's define
		// branch (AsciiSceneParser.cpp ~10798) ALSO fires on a leading '!', but then substitutes over
		// token[0]="!" -> empty-macro lookup -> hard parse-fail, so `! NAME VAL` is NON-FUNCTIONAL in legacy;
		// the migrator deliberately does NOT honor it.  Undef = UNDEF | undef | leading '~' (the undef branch
		// ~10823 does NOT substitute token[0], so '~ NAME' genuinely works).
		const bool isDefine = !toks.empty() && ( toks[0] == "DEFINE" || toks[0] == "define" );
		const bool isUndef  = !toks.empty() && !toks[0].empty() && ( toks[0] == "UNDEF" || toks[0] == "undef" || toks[0][0] == '~' );
		// FAITHFULNESS NOTE: the migrator is faithful on WELL-FORMED macros (the corpus).  On MALFORMED ones
		// (redefine, undef-of-undefined, missing value/name, non-[A-Z_] names) it is intentionally LENIENT
		// where legacy HARD-FAILS; the cutover's front-line parser must restore legacy's strict validation.
		if( toks.size() >= 3 && isDefine ) { std::string dv = toks[2]; SubstituteMacrosInPlace( dv, macros ); FoldExprsInPlace( dv ); macros[toks[1]] = atof( dv.c_str() ); ++i; continue; }
		if( toks.size() >= 2 && isUndef )  { macros.erase(toks[1]); ++i; continue; }
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
			out += "global_medium\n{\nmedium " + toks[3] + "\n}\n"; ++i; continue;   // v6 `> set global_medium X` -> v7 chunk (at the `> set` position -- after the medium def, so SetGlobalMedium's definitions-before-use order holds).
			// (Only `> set global_medium` is converted here.  `> set accelerator` (~136 scenes) is render-neutral
			// -- a TLAS perf choice, image-identical to the default BVH4 -- so it passes through harmlessly.  But
			// `> set light_rr_threshold` (1 scene) is NOT render-neutral: the CST path drops the `>` line,
			// disabling that scene's Russian-roulette (DumpJob is blind to it, so the gate still says MATCH).
			// Both are deferred cutover work; Reason() buckets them "set".)
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

#endif // RISE_CST_MIGRATOR_H
