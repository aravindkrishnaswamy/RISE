//////////////////////////////////////////////////////////////////////
//
//  CstFirstSliceTest.cpp - First slice of the agentic redesign.
//
//  Proves the `sphere_geometry` vertical end-to-end (docs/agentic-redesign,
//  §D10/§D18, and 10-scene-language-and-cst.md §6.2): the smallest non-trivial
//  chunk (2 params, no refs, no repeatables, no expressions) carried through
//
//      bytes  --ParseToCst-->  CST  --SerializeCst-->  bytes        (Gate G1)
//      CST    --derive-------> Job::AddSphereGeometry (REAL apply layer)
//      edit   --path-copy----> new CST (structural sharing)
//      change --re-derive----> drop+add the changed chunk (apply-layer reuse)
//
//  This is the FALSIFIABLE VERTICAL the design called for: prove the lossless
//  round-trip tar-pit (G1) and the apply-layer-reuse seam on the smallest
//  faithful build, reusing the real engine apply layer, BEFORE committing to
//  the phased migration.
//
//  HONEST SCOPE (what this slice does and does NOT prove -- after adversarial
//  review):
//    * G1 lossless round-trip: PROVEN, including MULTI-CHUNK documents in the
//      real corpus shape (keyword and `{` on their own lines, tab-aligned
//      columns, comments between chunks) and the tar-pit cases (tabs, CRLF,
//      inline `#` comments, blank lines, no-trailing-newline). NOT proven on
//      full arbitrary scenes (header line, FOR/DEFINE/expr/`>` commands) --
//      those need the general grammar (a later phase).
//    * Apply-layer reuse: PROVEN -- the real Job::AddSphereGeometry builds the
//      derived scene; the radius is read back from the engine.
//    * Re-derive is a FULL re-derive of the changed chunk (drop+add). This is
//      the REBUILD path. TRUE incremental derivation -- node-granular CST diff,
//      memoization by derivation key, traced-input invalidation, the APPLY
//      fast-path (§2.4/§2.5, D4/D20) -- is the NEXT slice, NOT exercised here.
//    * Structural sharing + NodeId lineage are proven across a STRUCTURED edit
//      only. Identity across a free-form RE-PARSE (the hard D15 case) is the
//      next slice. The NodeId side-map here is a std::map keyed by green
//      pointer -- a slice placeholder; production identity is occurrence-keyed
//      in the Version's persistent identityRoot (D26) and would NOT key on a
//      pointer (shared green nodes collide -- matters at RepeatGroup, slice 3).
//    * The red cursor is O(depth x branching) over a vector; the O(log N)
//      rope (D16) is a later slice.
//
//  SCOPE NOTE (deliberate): the CST lives in THIS test file, not yet a
//  productionized src/Library/Cst module -- so the design can be validated
//  without premature churn across the five build projects.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <chrono>

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

//////////////////////////////////////////////////////////////////////
// Tiny harness (RISE tests are standalone executables; exit code is the
// verdict, and run_all_tests.sh reads it).
//////////////////////////////////////////////////////////////////////
static int g_pass = 0;
static int g_fail = 0;
static void Check( bool cond, const char* what )
{
	if( cond ) { ++g_pass; }
	else       { ++g_fail; std::printf( "  FAIL: %s\n", what ); }
}

//////////////////////////////////////////////////////////////////////
// The green CST: immutable, structurally shared nodes (D2/D11/D16).
//
//  - A node is either a LEAF (Tok = a meaningful token; Trivia = whitespace /
//    comment / blank bytes) carrying its EXACT bytes, or an INTERNAL node
//    (Document / Chunk / Param) carrying only ordered children.
//  - Losslessness invariant (the whole G1 game): every input byte lands in
//    exactly one leaf, leaves in document order. So SerializeCst == identity.
//  - Width is RELATIVE (D2): a leaf's width is its byte count; an internal
//    node's width is the sum of its children. No absolute offset is stored
//    anywhere -- the "red cursor" computes positions on demand (D16).
//////////////////////////////////////////////////////////////////////
namespace cst {

enum class NK { Document, Chunk, Param, Tok, Trivia };

struct Green {
	NK                                         kind;
	std::string                                text;   // leaves only
	std::string                                role;   // Chunk: keyword; Param: param name; Tok: kw/lbrace/rbrace/pname/pvalue/tok
	std::vector<std::shared_ptr<const Green>>  kids;   // internal only
};
using GP = std::shared_ptr<const Green>;

static GP leaf( NK k, std::string t, std::string role = "" )
{
	auto g = std::make_shared<Green>();
	g->kind = k; g->text = std::move(t); g->role = std::move(role);
	return g;
}
static GP node( NK k, std::vector<GP> kids, std::string role = "" )
{
	auto g = std::make_shared<Green>();
	g->kind = k; g->kids = std::move(kids); g->role = std::move(role);
	return g;
}

// Relative width (D2/D16): leaf = byte count, internal = sum of children.
static size_t Width( const GP& g )
{
	if( g->kids.empty() ) return g->text.size();
	size_t w = 0;
	for( const auto& k : g->kids ) w += Width( k );
	return w;
}

// SerializeCst (INV-4): concatenate leaf bytes in document order.
static void SerInto( const GP& g, std::string& out )
{
	if( g->kids.empty() ) out += g->text;
	else for( const auto& k : g->kids ) SerInto( k, out );
}
static std::string SerializeCst( const GP& g ) { std::string s; SerInto( g, s ); return s; }

//////////////////////////////////////////////////////////////////////
// Lexer (the RawTokenCapture role, F1 §3): split bytes into a flat token
// stream whose texts concatenate back to the input EXACTLY. Trivia runs
// absorb whitespace + `#`-to-end-of-line comments; words stop at whitespace,
// `#`, and braces; braces are single-char punct.
//////////////////////////////////////////////////////////////////////
struct Raw { bool trivia; std::string text; };

static bool IsWs( char c ) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static std::vector<Raw> Tokenize( const std::string& in )
{
	std::vector<Raw> out;
	size_t i = 0, n = in.size();
	while( i < n ) {
		char c = in[i];
		if( IsWs(c) || c == '#' ) {                 // trivia run (whitespace + comments)
			size_t s = i;
			while( i < n && (IsWs(in[i]) || in[i] == '#') ) {
				if( in[i] == '#' ) { while( i < n && in[i] != '\n' ) ++i; }  // comment body, stop AT newline
				else ++i;
			}
			out.push_back( { true, in.substr(s, i-s) } );
		} else if( c == '{' || c == '}' ) {          // punctuation
			out.push_back( { false, std::string(1,c) } );
			++i;
		} else {                                     // word (keyword / name / value)
			size_t s = i;
			while( i < n ) { char d = in[i]; if( IsWs(d) || d == '#' || d == '{' || d == '}' ) break; ++i; }
			out.push_back( { false, in.substr(s, i-s) } );
		}
	}
	return out;
}

//////////////////////////////////////////////////////////////////////
// ParseChunk: parse ONE chunk (keyword '{' body '}') starting at t[i] (which
// points at the keyword); advances i past the closing brace. The body is
// parsed with a BRACE-DEPTH counter, so nested '{...}' (sub-blocks in other
// chunk types) are captured losslessly and never truncate the chunk. Flat
// "pname pvalue" lines at body-depth 1 bind to Param nodes (with NodeIds);
// everything else is captured as generic tokens (still lossless). Every
// indexed access is bounds-guarded -- a truncated/odd file stops cleanly
// instead of reading past the token stream (no UB).
//////////////////////////////////////////////////////////////////////
static GP ParseChunk( const std::vector<Raw>& t, size_t& i,
                      std::map<const Green*,int>& ids, int& nextId )
{
	std::vector<GP> ck;
	std::string keyword = t[i].text;
	ck.push_back( leaf(NK::Tok, t[i++].text, "kw") );
	while( i < t.size() && t[i].trivia ) ck.push_back( leaf(NK::Trivia, t[i++].text) );
	if( i < t.size() && !t[i].trivia && t[i].text == "{" ) ck.push_back( leaf(NK::Tok, t[i++].text, "lbrace") );

	int depth = 1;
	while( i < t.size() && depth > 0 ) {
		if( t[i].trivia ) { ck.push_back( leaf(NK::Trivia, t[i++].text) ); continue; }
		const std::string& tx = t[i].text;
		if( tx == "}" ) { --depth; ck.push_back( leaf(NK::Tok, t[i++].text, depth == 0 ? "rbrace" : "tok") ); continue; }
		if( tx == "{" ) { ++depth; ck.push_back( leaf(NK::Tok, t[i++].text, "tok") ); continue; }
		if( depth == 1 ) {
			// flat param: pname trivia* pvalue
			std::string pname = tx;
			std::vector<GP> pk;
			pk.push_back( leaf(NK::Tok, t[i++].text, "pname") );
			while( i < t.size() && t[i].trivia ) pk.push_back( leaf(NK::Trivia, t[i++].text) );
			if( i < t.size() && !t[i].trivia && t[i].text != "}" && t[i].text != "{" ) {
				GP pval = leaf( NK::Tok, t[i++].text, "pvalue" );
				pk.push_back( pval );
				GP param = node( NK::Param, pk, pname );
				ids[ pval.get() ]  = nextId++;      // value-token identity
				ids[ param.get() ] = nextId++;      // param-occurrence identity
				ck.push_back( param );
			} else {
				for( const auto& x : pk ) ck.push_back( x );   // value-less line: flatten (lossless)
			}
		} else {
			ck.push_back( leaf(NK::Tok, t[i++].text, "tok") );  // nested-block content: generic (lossless)
		}
	}
	return node( NK::Chunk, ck, keyword );
}

//////////////////////////////////////////////////////////////////////
// ParseToCst: a Document is leading trivia + a sequence of chunks (each with
// its own surrounding trivia), to EOF. MULTI-CHUNK by construction -- the
// lossless invariant has to hold for the real corpus, which is never one
// chunk. Bound to the descriptor for typed binding at derive time.
//////////////////////////////////////////////////////////////////////
static GP ParseToCst( const std::vector<Raw>& t, const ChunkDescriptor& desc,
                      std::map<const Green*,int>& ids, int& nextId )
{
	(void)desc;   // descriptor kind-lookup happens at derive/edit time
	size_t i = 0;
	std::vector<GP> docKids;
	while( i < t.size() ) {
		if( t[i].trivia ) { docKids.push_back( leaf(NK::Trivia, t[i++].text) ); continue; }
		docKids.push_back( ParseChunk( t, i, ids, nextId ) );
	}
	return node( NK::Document, docKids, "document" );
}

//////////////////////////////////////////////////////////////////////
// Red cursor (D16): compute a leaf's ABSOLUTE byte offset on demand from the
// relative widths -- the byte<->node map the UI/agent uses for "click at
// offset" / "what's the span of geometry/s.radius". (O(depth x branching)
// over a vector; the O(log N) rope is a later slice.)
//////////////////////////////////////////////////////////////////////
static bool PathTo( const GP& g, const Green* target, std::vector<size_t>& path )
{
	if( g.get() == target ) return true;
	for( size_t i = 0; i < g->kids.size(); ++i ) {
		path.push_back( i );
		if( PathTo( g->kids[i], target, path ) ) return true;
		path.pop_back();
	}
	return false;
}
static bool AbsOffsetOf( const GP& root, const Green* target, size_t& off )
{
	std::vector<size_t> path;
	if( !PathTo( root, target, path ) ) return false;
	off = 0;
	GP cur = root;
	for( size_t idx : path ) {
		for( size_t j = 0; j < idx; ++j ) off += Width( cur->kids[j] );
		cur = cur->kids[idx];
	}
	return true;
}

//////////////////////////////////////////////////////////////////////
// Name-path addressing (D9/D15/D44): find a chunk by keyword + its `name`
// value ("geometry/s" -> the sphere_geometry chunk whose name is s), then a
// param's value token within it.
//////////////////////////////////////////////////////////////////////
static bool ParamValueTextIn( const Green* chunk, const std::string& paramRole, std::string& out )
{
	if( !chunk ) return false;
	for( const auto& p : chunk->kids )
		if( p->kind == NK::Param && p->role == paramRole )
			for( const auto& v : p->kids )
				if( v->kind == NK::Tok && v->role == "pvalue" ) { out = v->text; return true; }
	return false;
}
static const Green* FindChunk( const GP& doc, const std::string& keyword, const std::string& name = "" )
{
	for( const auto& c : doc->kids ) {
		if( c->kind != NK::Chunk || c->role != keyword ) continue;
		if( name.empty() ) return c.get();
		std::string nm;
		if( ParamValueTextIn(c.get(), "name", nm) && nm == name ) return c.get();
	}
	return nullptr;
}
static const Green* FindValueTok( const Green* chunk, const std::string& paramRole )
{
	if( !chunk ) return nullptr;
	for( const auto& p : chunk->kids )
		if( p->kind == NK::Param && p->role == paramRole )
			for( const auto& v : p->kids )
				if( v->kind == NK::Tok && v->role == "pvalue" ) return v.get();
	return nullptr;
}

//////////////////////////////////////////////////////////////////////
// Edit = path-copy (D11). Replace one Param's value token in the named chunk;
// rebuild only the ancestor chain (Param -> Chunk -> Document); SHARE every
// untouched sibling by pointer. The new value token inherits the old one's
// NodeId (lineage, D26/D44). O(depth) structural sharing on the smallest tree.
//////////////////////////////////////////////////////////////////////
static GP WithReplacedKid( const GP& parent, size_t idx, GP newKid )
{
	std::vector<GP> k = parent->kids;
	k[idx] = std::move(newKid);
	return node( parent->kind, std::move(k), parent->role );
}

static GP SetParamValue( const GP& doc, const std::string& keyword, const std::string& name,
                         const std::string& paramRole, const std::string& newText,
                         std::map<const Green*,int>& ids )
{
	for( size_t ci = 0; ci < doc->kids.size(); ++ci ) {
		GP chunk = doc->kids[ci];
		if( chunk->kind != NK::Chunk || chunk->role != keyword ) continue;
		std::string nm;
		if( !name.empty() && !(ParamValueTextIn(chunk.get(), "name", nm) && nm == name) ) continue;
		for( size_t pi = 0; pi < chunk->kids.size(); ++pi ) {
			GP param = chunk->kids[pi];
			if( param->kind != NK::Param || param->role != paramRole ) continue;
			for( size_t vi = 0; vi < param->kids.size(); ++vi ) {
				GP val = param->kids[vi];
				if( val->kind != NK::Tok || val->role != "pvalue" ) continue;
				GP newVal = leaf( NK::Tok, newText, "pvalue" );
				auto it = ids.find( val.get() );
				if( it != ids.end() ) ids[ newVal.get() ] = it->second;   // preserve lineage
				GP newParam = WithReplacedKid( param, vi, newVal );
				auto pit = ids.find( param.get() );
				if( pit != ids.end() ) ids[ newParam.get() ] = pit->second;
				GP newChunk = WithReplacedKid( chunk, pi, newParam );
				return WithReplacedKid( doc, ci, newChunk );
			}
		}
	}
	return doc;
}

} // namespace cst

//////////////////////////////////////////////////////////////////////
// Derivation (Facet 2): read a bound sphere_geometry Chunk and call the REAL
// apply layer Job::AddSphereGeometry. Re-derive of a changed chunk drops the
// prior derived object (the manager rejects duplicate names) and adds the new
// one -- this is the FULL-rebuild path; true incremental memoized derivation
// is the next slice. Reads the radius back through GenerateBoundingSphere to
// prove the derived SCENE actually changed.
//////////////////////////////////////////////////////////////////////
static bool DeriveSphereChunk( const cst::Green* chunk, const ChunkDescriptor& desc, Job* job, double& outRadius )
{
	if( !chunk ) return false;
	std::string nameStr = "noname", radStr = "1.0";
	cst::ParamValueTextIn( chunk, "name",   nameStr );
	cst::ParamValueTextIn( chunk, "radius", radStr );

	// Descriptor-driven typing: confirm the radius slot is a Double before atof.
	ValueKind radKind = ValueKind::String;
	for( const auto& p : desc.parameters ) if( p.name == "radius" ) radKind = p.kind;
	if( radKind != ValueKind::Double ) return false;
	double radius = std::atof( radStr.c_str() );

	if( job->GetGeometries()->GetItem( nameStr.c_str() ) )        // drop the prior derived object
		job->GetGeometries()->RemoveItem( nameStr.c_str() );      //   for this chunk, if one exists
	bool ok = job->AddSphereGeometry( nameStr.c_str(), radius );  // REAL engine apply layer (unchanged)

	IGeometry* g = job->GetGeometries()->GetItem( nameStr.c_str() );
	if( g ) { Point3 ctr; Scalar r = 0; g->GenerateBoundingSphere( ctr, r ); outRadius = r; }
	return ok;
}

// Derive a specific named chunk.
static bool DeriveSphere( const cst::GP& doc, const std::string& name, const ChunkDescriptor& desc, Job* job, double& outRadius )
{
	return DeriveSphereChunk( cst::FindChunk(doc, desc.keyword, name), desc, job, outRadius );
}

// Derive ALL sphere_geometry chunks in the document; returns how many succeeded.
static int DeriveAllSpheres( const cst::GP& doc, const ChunkDescriptor& desc, Job* job )
{
	int n = 0;
	for( const auto& c : doc->kids )
		if( c->kind == cst::NK::Chunk && c->role == desc.keyword ) {
			double r; if( DeriveSphereChunk(c.get(), desc, job, r) ) ++n;
		}
	return n;
}

//////////////////////////////////////////////////////////////////////
// The sphere_geometry descriptor (mirrors the real SphereGeometryAsciiChunk-
// Parser::Describe(), AsciiSceneParser.cpp). In production the CST binds to
// the live descriptor registry; constructing it here keeps the slice from
// needing that plumbing (a later slice) while still proving descriptor-driven
// binding against the real ChunkDescriptor/ParameterDescriptor TYPES.
//////////////////////////////////////////////////////////////////////
static ChunkDescriptor SphereDescriptor()
{
	ChunkDescriptor cd;
	cd.keyword = "sphere_geometry";
	cd.category = ChunkCategory::Geometry;
	cd.description = "Implicit sphere geometry.";
	{ ParameterDescriptor p; p.name = "name";   p.kind = ValueKind::String; cd.parameters.push_back(p); }
	{ ParameterDescriptor p; p.name = "radius"; p.kind = ValueKind::Double; cd.parameters.push_back(p); }
	return cd;
}

template <typename F>
static double TimeMicros( F&& f )
{
	auto t0 = std::chrono::steady_clock::now();
	f();
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::micro>( t1 - t0 ).count();
}

int main()
{
	using namespace cst;
	std::printf( "CstFirstSliceTest -- sphere_geometry vertical (D10/D18 first slice)\n" );

	const ChunkDescriptor desc = SphereDescriptor();

	//----------------------------------------------------------------------
	// GATE G1 -- lossless round-trip (the non-negotiable correctness gate).
	// Fixtures cover the F1 §4 tar-pit cases AND the real corpus shape:
	// MULTI-CHUNK, keyword and `{` on their own lines, tab-aligned columns,
	// comments between chunks, blank lines, CRLF, no-trailing-newline.
	//----------------------------------------------------------------------
	std::printf( "[G1] lossless parse->serialize identity\n" );
	const std::vector<std::string> fixtures = {
		// canonical: tabs, inline comment, blank line, trailing newline
		"sphere_geometry {\n\tname s\n\tradius 0.6  # main ball\n\n}\n",
		// leading comment + no trailing newline
		"# a scene\nsphere_geometry {\n\tname ball\n\tradius 2\n}",
		// CRLF line endings + extra interior spacing
		"sphere_geometry {\r\n\tname   s\r\n\tradius    0.6\r\n}\r\n",
		// trailing whitespace on lines
		"sphere_geometry {\n\tname s \n\tradius 1.0\t\n}\n",
		// MULTI-CHUNK in the real corpus shape (keyword and `{` on own lines,
		// tab-aligned columns, a comment between chunks) -- mirrors the byte
		// shape of scenes/Tests/Geometry/csg.RISEscene's sphere chunks.
		"sphere_geometry\n{\n\tname\t\t\tspheregeomA\n\tradius\t\t\t0.165\n}\n\n"
		"# second ball\nsphere_geometry\n{\n\tname\t\t\tspheregeomB\n\tradius\t\t\t0.25\n}\n",
		// nested braces inside a (non-sphere) chunk must NOT truncate
		"some_chunk {\n\touter {\n\t\tinner 1\n\t}\n\tname z\n}\nsphere_geometry {\n\tname s\n\tradius 3\n}\n",
	};
	for( size_t fi = 0; fi < fixtures.size(); ++fi ) {
		std::map<const Green*,int> fids; int fn = 1;
		GP fdoc = ParseToCst( Tokenize(fixtures[fi]), desc, fids, fn );
		std::string out = SerializeCst( fdoc );
		bool identical = (out == fixtures[fi]);
		char msg[80]; std::snprintf( msg, sizeof(msg), "fixture %zu round-trips byte-identical", fi );
		Check( identical, msg );
		if( !identical ) std::printf( "    in (%zuB) =[%s]\n    out(%zuB)=[%s]\n",
		                              fixtures[fi].size(), fixtures[fi].c_str(), out.size(), out.c_str() );
	}

	// G1 on a real on-disk scene file's sphere chunks, if present (golden corpus).
	{
		const char* path = "scenes/Tests/Geometry/csg.RISEscene";
		FILE* f = std::fopen( path, "rb" );
		if( f ) {
			std::string bytes; char buf[4096]; size_t r;
			while( (r = std::fread(buf, 1, sizeof(buf), f)) > 0 ) bytes.append( buf, r );
			std::fclose( f );
			// Extract just the sphere_geometry chunks (this slice's grammar) and
			// round-trip them as a multi-chunk document.
			std::string spheres;
			size_t p = 0;
			while( (p = bytes.find("sphere_geometry", p)) != std::string::npos ) {
				size_t open = bytes.find('{', p);
				size_t close = (open == std::string::npos) ? std::string::npos : bytes.find('}', open);
				if( close == std::string::npos ) break;
				spheres += bytes.substr( p, close - p + 1 );
				spheres += "\n";
				p = close + 1;
			}
			if( !spheres.empty() ) {
				std::map<const Green*,int> gids; int gn = 1;
				GP gdoc = ParseToCst( Tokenize(spheres), desc, gids, gn );
				Check( SerializeCst(gdoc) == spheres, "real csg.RISEscene sphere chunks round-trip byte-identical" );
			} else {
				std::printf( "  (note: no sphere_geometry chunks found in %s; skipping golden round-trip)\n", path );
			}
		} else {
			std::printf( "  (note: %s not found from cwd; skipping golden round-trip -- run from repo root)\n", path );
		}
	}

	//----------------------------------------------------------------------
	// Build the canonical fixture once for the remaining checks.
	//----------------------------------------------------------------------
	const std::string src = fixtures[0];
	std::map<const Green*,int> ids; int nid = 1;
	GP doc = ParseToCst( Tokenize(src), desc, ids, nid );

	//----------------------------------------------------------------------
	// Descriptor-driven binding + name-path addressing.
	//----------------------------------------------------------------------
	std::printf( "[bind] descriptor-driven structure + name-path addressing\n" );
	const Green* chunk = FindChunk( doc, "sphere_geometry", "s" );
	Check( chunk != nullptr, "Chunk node addressed by geometry/s (keyword + name)" );
	std::string nameVal, radVal;
	Check( ParamValueTextIn(chunk, "name",   nameVal) && nameVal == "s",   "geometry/s.name == s via name-path" );
	Check( ParamValueTextIn(chunk, "radius", radVal)  && radVal  == "0.6", "geometry/s.radius == 0.6 via name-path" );

	//----------------------------------------------------------------------
	// Red cursor (D16): the computed offset must LAND on the value's bytes.
	// Oracle is "the computed offset, sliced from src, equals the value text"
	// -- an independent check, not a substring search that could be ambiguous.
	//----------------------------------------------------------------------
	std::printf( "[cursor] relative-width red cursor computes absolute offsets\n" );
	const Green* radTok = FindValueTok( chunk, "radius" );
	size_t computed = 0; bool found = AbsOffsetOf( doc, radTok, computed );
	Check( found && radTok && computed + radTok->text.size() <= src.size()
	             && src.compare( computed, radTok->text.size(), radTok->text ) == 0,
	       "red cursor offset lands exactly on the radius value bytes" );

	//----------------------------------------------------------------------
	// Edit = path-copy with structural sharing (D11) + NodeId lineage (D26).
	//----------------------------------------------------------------------
	std::printf( "[edit] path-copy: structural sharing + preserved NodeId\n" );
	int radNodeIdBefore = ids.count(radTok) ? ids[radTok] : -1;
	const Green* nameParamBefore = nullptr;
	const Green* commentTriviaBefore = nullptr;
	for( const auto& k : chunk->kids ) {
		if( k->kind == NK::Param && k->role == "name" ) nameParamBefore = k.get();
		if( k->kind == NK::Trivia && k->text.find("# main ball") != std::string::npos ) commentTriviaBefore = k.get();
	}
	GP doc2 = SetParamValue( doc, "sphere_geometry", "s", "radius", "0.8", ids );
	Check( doc2.get() != doc.get(),       "edit produced a NEW document root (immutability)" );
	Check( SerializeCst(doc) == src,      "original document still serializes to the original bytes (persistence)" );

	const Green* chunk2 = FindChunk( doc2, "sphere_geometry", "s" );
	const Green* nameParamAfter = nullptr;
	const Green* commentTriviaAfter = nullptr;
	for( const auto& k : chunk2->kids ) {
		if( k->kind == NK::Param && k->role == "name" ) nameParamAfter = k.get();
		if( k->kind == NK::Trivia && k->text.find("# main ball") != std::string::npos ) commentTriviaAfter = k.get();
	}
	Check( nameParamBefore && nameParamBefore == nameParamAfter,             "untouched 'name' Param is pointer-SHARED (structural sharing)" );
	Check( commentTriviaBefore && commentTriviaBefore == commentTriviaAfter, "untouched comment trivia is pointer-SHARED" );

	const Green* radTok2 = FindValueTok( chunk2, "radius" );
	Check( radTok2 && ids.count(radTok2) && ids[radTok2] == radNodeIdBefore, "radius value keeps its NodeId across the edit (lineage)" );

	//----------------------------------------------------------------------
	// Cross-chunk structural sharing on a MULTI-CHUNK document: editing one
	// chunk must share the OTHER chunk's whole subtree by pointer.
	//----------------------------------------------------------------------
	std::printf( "[edit] multi-chunk: editing one chunk shares the other chunk's subtree\n" );
	{
		std::map<const Green*,int> mids; int mn = 1;
		GP mdoc = ParseToCst( Tokenize(fixtures[4]), desc, mids, mn );
		const Green* chunkB_before = FindChunk( mdoc, "sphere_geometry", "spheregeomB" );
		GP mdoc2 = SetParamValue( mdoc, "sphere_geometry", "spheregeomA", "radius", "0.999", mids );
		const Green* chunkB_after = FindChunk( mdoc2, "sphere_geometry", "spheregeomB" );
		Check( chunkB_before && chunkB_before == chunkB_after, "untouched sibling chunk is pointer-SHARED after editing its neighbor" );
		Check( SerializeCst(mdoc2).find("0.999") != std::string::npos
		    && SerializeCst(mdoc2).find("0.25")  != std::string::npos, "edit changed chunk A's radius, preserved chunk B's" );
	}

	//----------------------------------------------------------------------
	// Round-trip after edit: only the value changed; comment + tabs preserved.
	//----------------------------------------------------------------------
	std::printf( "[edit] round-trip preserves comment + formatting, changes only the value\n" );
	const std::string expectAfter = "sphere_geometry {\n\tname s\n\tradius 0.8  # main ball\n\n}\n";
	Check( SerializeCst(doc2) == expectAfter, "edited document serializes with only 0.6->0.8 changed" );

	//----------------------------------------------------------------------
	// Derive via the REAL apply layer + re-derive of the changed chunk.
	//----------------------------------------------------------------------
	std::printf( "[derive] apply-layer reuse (Job::AddSphereGeometry); re-derive = drop+add (full, not yet incremental)\n" );
	Job* job = new Job();   // ctor calls InitializeContainers() -> geometry manager ready
	if( !job ) {
		Check( false, "Job created" );
		std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
		return 1;
	}

	double derived = 0;
	Check( DeriveSphere(doc,  "s", desc, job, derived) && derived == 0.6, "derive v1 -> engine sphere radius 0.6 (read back)" );
	Check( DeriveSphere(doc2, "s", desc, job, derived) && derived == 0.8, "re-derive changed chunk -> engine sphere radius 0.8 (read back)" );
	Check( job->GetGeometries()->getItemCount() == 1, "exactly one geometry after re-derive (replaced, not duplicated)" );

	// Multi-chunk derive: a 2-sphere document derives 2 distinct geometries.
	{
		std::map<const Green*,int> mids; int mn = 1;
		GP mdoc = ParseToCst( Tokenize(fixtures[4]), desc, mids, mn );
		Job* job2 = new Job();
		Check( DeriveAllSpheres(mdoc, desc, job2) == 2,            "multi-chunk: both sphere chunks derive" );
		Check( job2->GetGeometries()->getItemCount() == 2,        "multi-chunk: two distinct geometries in the scene" );
		double rA = 0, rB = 0;
		IGeometry* gA = job2->GetGeometries()->GetItem("spheregeomA");
		IGeometry* gB = job2->GetGeometries()->GetItem("spheregeomB");
		if( gA ) { Point3 c; Scalar r=0; gA->GenerateBoundingSphere(c,r); rA = r; }
		if( gB ) { Point3 c; Scalar r=0; gB->GenerateBoundingSphere(c,r); rB = r; }
		Check( rA == 0.165 && rB == 0.25, "multi-chunk: each derived radius matches its chunk" );
		job2->release();
	}

	//----------------------------------------------------------------------
	// Latency mechanism (toy scale). The G2 gate proper (<50ms incremental
	// DERIVE on a 155-mesh Sponza-scale scene) needs a large scene AND true
	// incremental derivation (memo/diff/APPLY-path) -- both later slices. Here
	// we only show the CST path-copy edit is cheaper than a full re-parse.
	//----------------------------------------------------------------------
	std::printf( "[perf] CST op latency (toy scale; the <50ms-Sponza G2 gate + true incremental derive are later slices)\n" );
	double tParse = TimeMicros( [&]{ std::map<const Green*,int> z; int n=1; volatile auto d = ParseToCst(Tokenize(src), desc, z, n); (void)d; } );
	double tSer   = TimeMicros( [&]{ volatile auto s = SerializeCst(doc); (void)s; } );
	double tEdit  = TimeMicros( [&]{ std::map<const Green*,int> z=ids; volatile auto d = SetParamValue(doc,"sphere_geometry","s","radius","0.7",z); (void)d; } );
	std::printf( "      parse=%.1fus  serialize=%.1fus  path-copy-edit=%.2fus\n", tParse, tSer, tEdit );
	Check( tEdit < tParse, "incremental path-copy edit cheaper than a full re-parse" );

	job->release();

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
