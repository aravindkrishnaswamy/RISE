//////////////////////////////////////////////////////////////////////
//
//  CstSlicePrototype.h - Shared scaffolding for the agentic-redesign
//  implementation slices (docs/agentic-redesign, D10/D18).
//
//  This is the prototype Concrete Syntax Tree (CST) and its codec/derive
//  helpers, factored out of the first slice so the incremental-derive and
//  reference slices reuse ONE implementation rather than copy it. It is NOT
//  yet a productionized src/Library/Cst module (deliberate -- validate the
//  design before churning the five build projects). The mechanisms are
//  faithful (immutable green nodes, path-copy structural sharing, relative-
//  width red cursor, descriptor-driven binding, NodeId lineage vs name-path
//  addressing, real apply-layer reuse). Honest deferrals are marked.
//
//  Each test that includes this header is a standalone executable; the
//  functions are `inline` so an unused one never trips -Wunused-function.
//
//////////////////////////////////////////////////////////////////////
#ifndef RISE_TESTS_CST_SLICE_PROTOTYPE_H
#define RISE_TESTS_CST_SLICE_PROTOTYPE_H

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

//////////////////////////////////////////////////////////////////////
// Tiny harness (RISE tests are standalone executables; exit code is the
// verdict, and run_all_tests.sh reads it).
//////////////////////////////////////////////////////////////////////
inline int g_pass = 0;
inline int g_fail = 0;
inline void Check( bool cond, const char* what )
{
	if( cond ) { ++g_pass; }
	else       { ++g_fail; std::printf( "  FAIL: %s\n", what ); }
}
inline int CheckSummary()
{
	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}

//////////////////////////////////////////////////////////////////////
// The green CST: immutable, structurally shared nodes (D2/D11/D16).
//
//  - A node is either a LEAF (Tok = a meaningful token; Trivia = whitespace /
//    comment / blank bytes) carrying its EXACT bytes, or an INTERNAL node
//    (Document / Chunk / Param) carrying only ordered children.
//  - Losslessness invariant: every input byte lands in exactly one leaf,
//    leaves in document order. So SerializeCst == identity.
//  - Width is RELATIVE (D2): leaf = byte count, internal = sum of children.
//    No absolute offset is stored; the red cursor computes positions (D16).
//////////////////////////////////////////////////////////////////////
namespace cst {

using namespace RISE;

enum class NK { Document, Chunk, Param, Tok, Trivia };

struct Green {
	NK                                         kind;
	std::string                                text;   // leaves only
	std::string                                role;   // Chunk: keyword; Param: param name; Tok: kw/lbrace/rbrace/pname/pvalue/tok
	std::vector<std::shared_ptr<const Green>>  kids;   // internal only
};
using GP = std::shared_ptr<const Green>;
using IdMap = std::map<const Green*,int>;

inline GP leaf( NK k, std::string t, std::string role = "" )
{
	auto g = std::make_shared<Green>();
	g->kind = k; g->text = std::move(t); g->role = std::move(role);
	return g;
}
inline GP node( NK k, std::vector<GP> kids, std::string role = "" )
{
	auto g = std::make_shared<Green>();
	g->kind = k; g->kids = std::move(kids); g->role = std::move(role);
	return g;
}

// Relative width (D2/D16): leaf = byte count, internal = sum of children.
inline size_t Width( const GP& g )
{
	if( g->kids.empty() ) return g->text.size();
	size_t w = 0;
	for( const auto& k : g->kids ) w += Width( k );
	return w;
}

// SerializeCst: concatenate leaf bytes in document order.
inline void SerInto( const GP& g, std::string& out )
{
	if( g->kids.empty() ) out += g->text;
	else for( const auto& k : g->kids ) SerInto( k, out );
}
inline std::string SerializeCst( const GP& g ) { std::string s; SerInto( g, s ); return s; }

//////////////////////////////////////////////////////////////////////
// Lexer: split bytes into a flat token stream whose texts concatenate back
// to the input EXACTLY. Trivia runs absorb whitespace + `#`-to-EOL comments;
// words stop at whitespace, `#`, and braces; braces are single-char punct.
//////////////////////////////////////////////////////////////////////
struct Raw { bool trivia; std::string text; };

inline bool IsWs( char c ) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline std::vector<Raw> Tokenize( const std::string& in )
{
	std::vector<Raw> out;
	size_t i = 0, n = in.size();
	while( i < n ) {
		char c = in[i];
		if( IsWs(c) || c == '#' ) {                 // trivia run (whitespace + comments)
			size_t s = i;
			while( i < n && (IsWs(in[i]) || in[i] == '#') ) {
				if( in[i] == '#' ) { while( i < n && in[i] != '\n' ) ++i; }
				else ++i;
			}
			out.push_back( { true, in.substr(s, i-s) } );
		} else if( c == '{' || c == '}' ) {
			out.push_back( { false, std::string(1,c) } );
			++i;
		} else {
			size_t s = i;
			while( i < n ) { char d = in[i]; if( IsWs(d) || d == '#' || d == '{' || d == '}' ) break; ++i; }
			out.push_back( { false, in.substr(s, i-s) } );
		}
	}
	return out;
}

//////////////////////////////////////////////////////////////////////
// ParseChunk: one chunk (keyword '{' body '}') from t[i]; advances i past the
// closing brace. Brace-depth counter -> nested '{...}' captured losslessly,
// never truncates. Flat "pname pvalue" at body-depth 1 binds to Param nodes
// (with NodeIds); everything else is generic (still lossless). Bounds-guarded.
//////////////////////////////////////////////////////////////////////
inline GP ParseChunk( const std::vector<Raw>& t, size_t& i, IdMap& ids, int& nextId )
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
			std::string pname = tx;
			std::vector<GP> pk;
			pk.push_back( leaf(NK::Tok, t[i++].text, "pname") );
			while( i < t.size() && t[i].trivia ) pk.push_back( leaf(NK::Trivia, t[i++].text) );
			if( i < t.size() && !t[i].trivia && t[i].text != "}" && t[i].text != "{" ) {
				GP pval = leaf( NK::Tok, t[i++].text, "pvalue" );
				pk.push_back( pval );
				GP param = node( NK::Param, pk, pname );
				ids[ pval.get() ]  = nextId++;
				ids[ param.get() ] = nextId++;
				ck.push_back( param );
			} else {
				for( const auto& x : pk ) ck.push_back( x );   // value-less line: flatten (lossless)
			}
		} else {
			ck.push_back( leaf(NK::Tok, t[i++].text, "tok") );  // nested-block content: generic (lossless)
		}
	}
	GP chunk = node( NK::Chunk, ck, keyword );
	ids[ chunk.get() ] = nextId++;   // the Chunk node has its own NodeId (D26 lineage)
	return chunk;
}

// ParseToCst: leading trivia + a sequence of chunks (with surrounding trivia),
// to EOF. MULTI-CHUNK by construction.
inline GP ParseToCst( const std::vector<Raw>& t, IdMap& ids, int& nextId )
{
	size_t i = 0;
	std::vector<GP> docKids;
	while( i < t.size() ) {
		if( t[i].trivia ) { docKids.push_back( leaf(NK::Trivia, t[i++].text) ); continue; }
		docKids.push_back( ParseChunk( t, i, ids, nextId ) );
	}
	return node( NK::Document, docKids, "document" );
}
inline GP ParseStr( const std::string& src, IdMap& ids, int& nextId ) { return ParseToCst( Tokenize(src), ids, nextId ); }

//////////////////////////////////////////////////////////////////////
// Red cursor (D16): compute a leaf's ABSOLUTE byte offset on demand from the
// relative widths. O(depth x branching) over a vector; the O(log N) rope is
// a later slice.
//////////////////////////////////////////////////////////////////////
inline bool PathTo( const GP& g, const Green* target, std::vector<size_t>& path )
{
	if( g.get() == target ) return true;
	for( size_t i = 0; i < g->kids.size(); ++i ) {
		path.push_back( i );
		if( PathTo( g->kids[i], target, path ) ) return true;
		path.pop_back();
	}
	return false;
}
inline bool AbsOffsetOf( const GP& root, const Green* target, size_t& off )
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
// Name-path addressing (D9/D15/D44): chunk by keyword + `name` value, then a
// param's value token within it.
//////////////////////////////////////////////////////////////////////
inline bool ParamValueTextIn( const Green* chunk, const std::string& paramRole, std::string& out )
{
	if( !chunk ) return false;
	for( const auto& p : chunk->kids )
		if( p->kind == NK::Param && p->role == paramRole )
			for( const auto& v : p->kids )
				if( v->kind == NK::Tok && v->role == "pvalue" ) { out = v->text; return true; }
	return false;
}
inline const Green* FindChunk( const GP& doc, const std::string& keyword, const std::string& name = "" )
{
	for( const auto& c : doc->kids ) {
		if( c->kind != NK::Chunk || c->role != keyword ) continue;
		if( name.empty() ) return c.get();
		std::string nm;
		if( ParamValueTextIn(c.get(), "name", nm) && nm == name ) return c.get();
	}
	return nullptr;
}
inline const Green* FindValueTok( const Green* chunk, const std::string& paramRole )
{
	if( !chunk ) return nullptr;
	for( const auto& p : chunk->kids )
		if( p->kind == NK::Param && p->role == paramRole )
			for( const auto& v : p->kids )
				if( v->kind == NK::Tok && v->role == "pvalue" ) return v.get();
	return nullptr;
}

//////////////////////////////////////////////////////////////////////
// Edit = path-copy (D11): replace one Param's value token in the named chunk;
// rebuild only the ancestor chain; SHARE every untouched sibling by pointer.
// New value token inherits the old NodeId (lineage, D26/D44).
//////////////////////////////////////////////////////////////////////
inline GP WithReplacedKid( const GP& parent, size_t idx, GP newKid )
{
	std::vector<GP> k = parent->kids;
	k[idx] = std::move(newKid);
	return node( parent->kind, std::move(k), parent->role );
}
inline GP SetParamValue( const GP& doc, const std::string& keyword, const std::string& name,
                         const std::string& paramRole, const std::string& newText, IdMap& ids )
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
				if( it != ids.end() ) ids[ newVal.get() ] = it->second;
				GP newParam = WithReplacedKid( param, vi, newVal );
				auto pit = ids.find( param.get() );
				if( pit != ids.end() ) ids[ newParam.get() ] = pit->second;
				GP newChunk = WithReplacedKid( chunk, pi, newParam );
				auto cit = ids.find( chunk.get() );
				if( cit != ids.end() ) ids[ newChunk.get() ] = cit->second;   // carry chunk identity across the edit
				return WithReplacedKid( doc, ci, newChunk );
			}
		}
	}
	return doc;
}

} // namespace cst

//////////////////////////////////////////////////////////////////////
// The sphere_geometry descriptor (mirrors the real SphereGeometryAsciiChunk-
// Parser::Describe()). In production the CST binds to the live registry;
// constructing it here proves descriptor-driven binding against the real
// ChunkDescriptor/ParameterDescriptor TYPES without that plumbing.
//////////////////////////////////////////////////////////////////////
inline RISE::ChunkDescriptor SphereDescriptor()
{
	RISE::ChunkDescriptor cd;
	cd.keyword = "sphere_geometry";
	cd.category = RISE::ChunkCategory::Geometry;
	cd.description = "Implicit sphere geometry.";
	{ RISE::ParameterDescriptor p; p.name = "name";   p.kind = RISE::ValueKind::String; cd.parameters.push_back(p); }
	{ RISE::ParameterDescriptor p; p.name = "radius"; p.kind = RISE::ValueKind::Double; cd.parameters.push_back(p); }
	return cd;
}

//////////////////////////////////////////////////////////////////////
// Derivation (Facet 2): read a bound sphere_geometry Chunk and call the REAL
// apply layer Job::AddSphereGeometry. Re-derive of a changed chunk drops the
// prior derived object and adds the new one (the REBUILD path). Reads the
// radius back through GenerateBoundingSphere to prove the scene changed.
//////////////////////////////////////////////////////////////////////
inline bool DeriveSphereChunk( const cst::Green* chunk, const RISE::ChunkDescriptor& desc, RISE::Job* job, double& outRadius )
{
	using namespace RISE;
	if( !chunk ) return false;
	std::string nameStr = "noname", radStr = "1.0";
	cst::ParamValueTextIn( chunk, "name",   nameStr );
	cst::ParamValueTextIn( chunk, "radius", radStr );

	ValueKind radKind = ValueKind::String;
	for( const auto& p : desc.parameters ) if( p.name == "radius" ) radKind = p.kind;
	if( radKind != ValueKind::Double ) return false;
	double radius = std::atof( radStr.c_str() );

	if( job->GetGeometries()->GetItem( nameStr.c_str() ) )
		job->GetGeometries()->RemoveItem( nameStr.c_str() );
	bool ok = job->AddSphereGeometry( nameStr.c_str(), radius );

	IGeometry* g = job->GetGeometries()->GetItem( nameStr.c_str() );
	if( g ) { Point3 ctr; Scalar r = 0; g->GenerateBoundingSphere( ctr, r ); outRadius = r; }
	return ok;
}
inline bool DeriveSphere( const cst::GP& doc, const std::string& name, const RISE::ChunkDescriptor& desc, RISE::Job* job, double& outRadius )
{
	return DeriveSphereChunk( cst::FindChunk(doc, desc.keyword, name), desc, job, outRadius );
}
inline int DeriveAllSpheres( const cst::GP& doc, const RISE::ChunkDescriptor& desc, RISE::Job* job )
{
	int n = 0;
	for( const auto& c : doc->kids )
		if( c->kind == cst::NK::Chunk && c->role == desc.keyword ) {
			double r; if( DeriveSphereChunk(c.get(), desc, job, r) ) ++n;
		}
	return n;
}

template <typename F>
inline double TimeMicros( F&& f )
{
	auto t0 = std::chrono::steady_clock::now();
	f();
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::micro>( t1 - t0 ).count();
}

#endif // RISE_TESTS_CST_SLICE_PROTOTYPE_H
