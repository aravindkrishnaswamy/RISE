//////////////////////////////////////////////////////////////////////
//
//  Cst.cpp - Concrete Syntax Tree kernel (agentic redesign).
//
//  Promoted from the validated prototype (tests/CstSlicePrototype.h, slices
//  1/1.5/2/3) into the real library. See Cst.h for scope.
//
//////////////////////////////////////////////////////////////////////

#include "Cst.h"
#include "../Interfaces/IJob.h"

#include <cstdlib>

using namespace RISE;
using namespace RISE::Cst;

namespace
{
	NodeRef Leaf( NodeKind k, std::string text, std::string role )
	{
		auto n = std::make_shared<Node>();
		n->kind = k; n->text = std::move(text); n->role = std::move(role);
		return n;
	}
	NodeRef Internal( NodeKind k, std::vector<NodeRef> kids, std::string role )
	{
		auto n = std::make_shared<Node>();
		n->kind = k; n->kids = std::move(kids); n->role = std::move(role);
		return n;
	}

	//----------------------------------------------------------------------
	// Lexer: split bytes into a flat token stream whose texts concatenate back
	// to the input EXACTLY. Trivia runs absorb whitespace + `#`-to-EOL comments;
	// words stop at whitespace, `#`, and braces; braces are single-char punct.
	//----------------------------------------------------------------------
	struct RawTok { bool trivia; std::string text; };

	bool IsWs( char c ) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

	std::vector<RawTok> Tokenize( const std::string& in )
	{
		std::vector<RawTok> out;
		size_t i = 0, n = in.size();
		while( i < n ) {
			char c = in[i];
			if( IsWs(c) || c == '#' ) {
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

	//----------------------------------------------------------------------
	// Parse one chunk (keyword '{' body '}') from t[i]; advances i past the
	// closing brace. Brace-depth counter -> nested '{...}' captured losslessly,
	// never truncates. Flat "pname pvalue" lines at body-depth 1 bind to Param
	// nodes; everything else is generic (still lossless). Bounds-guarded.
	//----------------------------------------------------------------------
	NodeRef ParseChunk( const std::vector<RawTok>& t, size_t& i )
	{
		std::vector<NodeRef> ck;
		std::string keyword = t[i].text;
		ck.push_back( Leaf(NodeKind::Token, t[i++].text, "kw") );
		while( i < t.size() && t[i].trivia ) ck.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") );
		if( i < t.size() && !t[i].trivia && t[i].text == "{" ) ck.push_back( Leaf(NodeKind::Token, t[i++].text, "lbrace") );

		int depth = 1;
		while( i < t.size() && depth > 0 ) {
			if( t[i].trivia ) { ck.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") ); continue; }
			const std::string& tx = t[i].text;
			if( tx == "}" ) { --depth; ck.push_back( Leaf(NodeKind::Token, t[i++].text, depth == 0 ? "rbrace" : "tok") ); continue; }
			if( tx == "{" ) { ++depth; ck.push_back( Leaf(NodeKind::Token, t[i++].text, "tok") ); continue; }
			if( depth == 1 ) {
				std::string pname = tx;
				std::vector<NodeRef> pk;
				pk.push_back( Leaf(NodeKind::Token, t[i++].text, "pname") );
				while( i < t.size() && t[i].trivia ) pk.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") );
				if( i < t.size() && !t[i].trivia && t[i].text != "}" && t[i].text != "{" ) {
					pk.push_back( Leaf(NodeKind::Token, t[i++].text, "pvalue") );
					ck.push_back( Internal(NodeKind::Param, std::move(pk), pname) );
				} else {
					for( auto& x : pk ) ck.push_back( x );   // value-less line: flatten (lossless)
				}
			} else {
				ck.push_back( Leaf(NodeKind::Token, t[i++].text, "tok") );
			}
		}
		return Internal( NodeKind::Chunk, std::move(ck), keyword );
	}

	void Serialize( const NodeRef& g, std::string& out )
	{
		if( g->kids.empty() ) out += g->text;
		else for( const auto& k : g->kids ) Serialize( k, out );
	}

	//! First pvalue text of a param role within a chunk (item-2 derive helper).
	bool ParamValue( const Node* chunk, const std::string& role, std::string& out )
	{
		if( !chunk ) return false;
		for( const auto& p : chunk->kids )
			if( p->kind == NodeKind::Param && p->role == role )
				for( const auto& v : p->kids )
					if( v->kind == NodeKind::Token && v->role == "pvalue" ) { out = v->text; return true; }
		return false;
	}
}

namespace RISE { namespace Cst {

Document ParseToCst( const std::string& bytes )
{
	std::vector<RawTok> t = Tokenize( bytes );
	size_t i = 0;
	std::vector<NodeRef> docKids;
	while( i < t.size() ) {
		if( t[i].trivia ) { docKids.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") ); continue; }
		// A chunk is `keyword {` (brace may be on the next line). A bare word
		// not followed by `{` -- e.g. each token of the `RISE ASCII SCENE 6`
		// header line -- is preserved losslessly as a stray Token, NOT swallowed
		// as a never-closed chunk. (A dedicated version-header node is a later
		// transfer-gate item; this keeps round-trip + chunk structure correct.)
		size_t j = i + 1;
		while( j < t.size() && t[j].trivia ) ++j;
		if( j < t.size() && !t[j].trivia && t[j].text == "{" ) docKids.push_back( ParseChunk( t, i ) );
		else docKids.push_back( Leaf(NodeKind::Token, t[i++].text, "stray") );
	}
	Document d;
	d.root = Internal( NodeKind::Document, std::move(docKids), "document" );
	return d;
}

std::string SerializeCst( const Document& doc )
{
	std::string s;
	if( doc.root ) Serialize( doc.root, s );
	return s;
}

int DeriveToJob( const Document& doc, IJob& pJob )
{
	if( !doc.root ) return 0;
	int count = 0;
	for( const auto& c : doc.root->kids ) {
		if( c->kind != NodeKind::Chunk || c->role != "sphere_geometry" ) continue;
		std::string name = "noname", radStr = "1.0";
		ParamValue( c.get(), "name",   name );
		ParamValue( c.get(), "radius", radStr );
		if( pJob.AddSphereGeometry( name.c_str(), std::atof( radStr.c_str() ) ) ) ++count;
	}
	return count;
}

} }
