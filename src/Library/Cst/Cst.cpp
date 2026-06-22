//////////////////////////////////////////////////////////////////////
//
//  Cst.cpp - Concrete Syntax Tree kernel (agentic redesign).
//
//  Promoted from the validated prototype (tests/CstSlicePrototype.h, slices
//  1/1.5/2/3) into the real library. See Cst.h for scope. Item 3 puts the
//  Document's top-level item list on a persistent balanced sequence with
//  cached byte-width / newline aggregates, so locating an edit target by byte
//  offset (or index) is O(log N) and COUNTED (not an O(N) scan).
//
//////////////////////////////////////////////////////////////////////

#include "Cst.h"
#include "../Interfaces/IJob.h"

#include <cstdlib>
#include <unordered_map>

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
	// to the input EXACTLY. Trivia runs absorb whitespace + `#`-to-EOL comments
	// + `/* ... */` block comments; words stop at whitespace, `#`, braces, and
	// `/*`; braces are single-char punct.
	//----------------------------------------------------------------------
	struct RawTok { bool trivia; std::string text; };

	bool IsWs( char c ) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

	std::vector<RawTok> Tokenize( const std::string& in )
	{
		std::vector<RawTok> out;
		size_t i = 0, n = in.size();
		auto isBlockStart = [&]( size_t k ) { return k + 1 < n && in[k] == '/' && in[k+1] == '*'; };
		while( i < n ) {
			char c = in[i];
			if( IsWs(c) || c == '#' || isBlockStart(i) ) {
				size_t s = i;
				for( ;; ) {
					if( i >= n ) break;
					if( IsWs(in[i]) ) { ++i; continue; }
					if( in[i] == '#' ) { while( i < n && in[i] != '\n' ) ++i; continue; }
					if( isBlockStart(i) ) {
						i += 2;
						while( i + 1 < n && !(in[i] == '*' && in[i+1] == '/') ) ++i;
						i = (i + 1 < n) ? i + 2 : n;   // skip the closing */ (or to EOF if unterminated)
						continue;
					}
					break;
				}
				out.push_back( { true, in.substr(s, i-s) } );
			} else if( c == '{' || c == '}' ) {
				out.push_back( { false, std::string(1,c) } );
				++i;
			} else {
				size_t s = i;
				while( i < n ) { char d = in[i]; if( IsWs(d) || d == '#' || d == '{' || d == '}' || isBlockStart(i) ) break; ++i; }
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
				bool sawNewline = false;
				while( i < t.size() && t[i].trivia ) {
					if( t[i].text.find('\n') != std::string::npos ) sawNewline = true;
					pk.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") );
				}
				// A param's value is on the SAME line as its name. If the next
				// token is on a later line (or a brace), this is a value-less
				// line -- flatten it; do NOT swallow the next line's token as the
				// value (which the legacy parser would never do).
				if( !sawNewline && i < t.size() && !t[i].trivia && t[i].text != "}" && t[i].text != "{" ) {
					pk.push_back( Leaf(NodeKind::Token, t[i++].text, "pvalue") );
					ck.push_back( Internal(NodeKind::Param, std::move(pk), pname) );
				} else {
					for( auto& x : pk ) ck.push_back( x );   // value-less line: flatten (lossless)
				}
			} else {
				ck.push_back( Leaf(NodeKind::Token, t[i++].text, "tok") );  // nested-block content: generic (lossless)
			}
		}
		return Internal( NodeKind::Chunk, std::move(ck), keyword );
	}

	void Serialize( const NodeRef& g, std::string& out )
	{
		if( g->kids.empty() ) out += g->text;
		else for( const auto& k : g->kids ) Serialize( k, out );
	}

	//! Value of a param role within a chunk (item-2 derive helper). On a
	//! repeated param, LAST occurrence wins -- matching the legacy parser's
	//! ParseStateBag overwrite semantics.
	bool ParamValue( const Node* chunk, const std::string& role, std::string& out )
	{
		if( !chunk ) return false;
		bool found = false;
		for( const auto& p : chunk->kids )
			if( p->kind == NodeKind::Param && p->role == role )
				for( const auto& v : p->kids )
					if( v->kind == NodeKind::Token && v->role == "pvalue" ) { out = v->text; found = true; }
		return found;
	}

	//! Validate one sphere_geometry chunk; append any failures to `diags`. The
	//! item-2 SAFE BOUNDARY (item 5 generalizes via the descriptor registry):
	//! unknown parameter, value-less parameter line (a flattened bare pname), or
	//! a radius that is not a fully-consumed finite number.
	void ValidateSphereChunk( const Node* chunk, std::vector<std::string>& diags )
	{
		for( const auto& k : chunk->kids ) {
			if( k->kind == NodeKind::Param ) {
				if( k->role != "name" && k->role != "radius" )
					diags.push_back( "sphere_geometry: unknown parameter '" + k->role + "'" );
			} else if( k->kind == NodeKind::Token && k->role == "pname" ) {
				diags.push_back( "sphere_geometry: value-less parameter '" + k->text + "'" );
			}
		}
		std::string rs;
		if( ParamValue( chunk, "radius", rs ) ) {
			const char* p = rs.c_str();
			char* end = 0;
			std::strtod( p, &end );
			bool consumed = ( end != 0 && end != p && *end == '\0' );
			std::string low = rs;
			for( size_t i = 0; i < low.size(); ++i ) if( low[i] >= 'A' && low[i] <= 'Z' ) low[i] = char( low[i] - 'A' + 'a' );
			bool special = low.find( "inf" ) != std::string::npos || low.find( "nan" ) != std::string::npos;
			if( !consumed || special )
				diags.push_back( "sphere_geometry: radius '" + rs + "' is not a finite number" );
		}
	}

	//----------------------------------------------------------------------
	// Item 3 -- persistent balanced sequence of top-level items (the D16 rope).
	//----------------------------------------------------------------------

	//! Diagnostic counter (single-threaded parse/edit context): bumped once per
	//! per-ITEM stat walk (MkSeqFresh). A correct path-copy edit walks exactly one
	//! item; a hidden re-scan of the unchanged spine would bump it. Read by the
	//! cost gate via Cst::DebugItemStatWalks().
	unsigned long g_itemStatWalks = 0;

	//! Cost-gate instrumentation for DocReparse: old-item touches during matching.
	//! The 4-pass hashed matcher touches each old item O(1) times -> grows O(M+N);
	//! a regression to a nested-loop matcher would make it O(M*N).
	unsigned long g_reparseOldVisits = 0;

	//! An item's own serialized byte width + newline count (computed once; the
	//! immutable item never changes, so it is cached in the SeqNode).
	void NodeStats( const NodeRef& n, size_t& bytes, int& newlines )
	{
		if( n->kids.empty() ) {
			bytes += n->text.size();
			for( char c : n->text ) if( c == '\n' ) ++newlines;
		} else for( const auto& k : n->kids ) NodeStats( k, bytes, newlines );
	}

	int    SeqCount( const SeqRef& s ) { return s ? s->count : 0; }
	size_t SeqBytes( const SeqRef& s ) { return s ? s->bytes : 0; }
	int    SeqNl   ( const SeqRef& s ) { return s ? s->newlines : 0; }

	//! Build a SeqNode from children + an item whose own stats are already known.
	SeqRef MkSeq( SeqRef l, NodeRef item, size_t itemBytes, int itemNewlines, SeqRef r )
	{
		auto s = std::make_shared<SeqNode>();
		s->left = std::move(l); s->right = std::move(r); s->item = std::move(item);
		s->itemBytes = itemBytes; s->itemNewlines = itemNewlines;
		s->count    = 1            + SeqCount(s->left) + SeqCount(s->right);
		s->bytes    = itemBytes    + SeqBytes(s->left) + SeqBytes(s->right);
		s->newlines = itemNewlines + SeqNl   (s->left) + SeqNl   (s->right);
		return s;
	}
	//! Build a SeqNode, computing the item's own stats once (for a fresh/changed item).
	SeqRef MkSeqFresh( SeqRef l, NodeRef item, SeqRef r )
	{
		++g_itemStatWalks;   // one per-item stat walk (cost-gate instrumentation)
		size_t b = 0; int nl = 0; NodeStats( item, b, nl );
		return MkSeq( std::move(l), std::move(item), b, nl, std::move(r) );
	}
	//! Perfectly-balanced build from an ordered vector (height = ceil(log2 N)).
	SeqRef SeqBuild( const std::vector<NodeRef>& v, int lo, int hi )
	{
		if( lo >= hi ) return SeqRef();
		int mid = (lo + hi) / 2;
		return MkSeqFresh( SeqBuild(v, lo, mid), v[mid], SeqBuild(v, mid+1, hi) );
	}
	void SeqToVec( const SeqRef& s, std::vector<NodeRef>& out )
	{
		if( !s ) return;
		SeqToVec( s->left, out ); out.push_back( s->item ); SeqToVec( s->right, out );
	}
	void SeqSerialize( const SeqRef& s, std::string& out )
	{
		if( !s ) return;
		SeqSerialize( s->left, out ); Serialize( s->item, out ); SeqSerialize( s->right, out );
	}
	//! Locate the item spanning byte `off` within subtree `s` (whose subtree
	//! starts at global byte `base` / index `idxBase`). Returns the global index
	//! or -1; sets outItem/outStart; ++visits per node descended (O(log N)).
	int SeqAtOffset( const SeqRef& s, size_t off, size_t base, int idxBase,
	                 NodeRef& outItem, size_t& outStart, int& visits )
	{
		if( !s ) return -1;
		++visits;
		size_t lb = SeqBytes( s->left );
		int    li = SeqCount( s->left );
		if( off < base + lb ) return SeqAtOffset( s->left, off, base, idxBase, outItem, outStart, visits );
		if( off < base + lb + s->itemBytes ) { outItem = s->item; outStart = base + lb; return idxBase + li; }
		return SeqAtOffset( s->right, off, base + lb + s->itemBytes, idxBase + li + 1, outItem, outStart, visits );
	}
	//! Path-copy replace of in-order index `index`: O(log N) new nodes, the rest
	//! shared; the unchanged spine reuses its cached itemBytes (no item re-walk).
	SeqRef SeqReplace( const SeqRef& s, int index, NodeRef newItem, int& visits )
	{
		++visits;
		int li = SeqCount( s->left );
		if( index <  li ) return MkSeq( SeqReplace(s->left, index, std::move(newItem), visits), s->item, s->itemBytes, s->itemNewlines, s->right );
		if( index == li ) return MkSeqFresh( s->left, std::move(newItem), s->right );
		return MkSeq( s->left, s->item, s->itemBytes, s->itemNewlines, SeqReplace(s->right, index - li - 1, std::move(newItem), visits) );
	}

	//----------------------------------------------------------------------
	// Weight-balanced (BB[alpha], Adams delta=3 / gamma=2) persistent insert +
	// erase -- O(log N): path-copy down the spine + O(1) balance rotations, the
	// untouched subtrees shared by pointer, rotations reusing cached itemBytes
	// (no item re-walk). This is D16's O(log N) insert/remove (NOT a flatten +
	// full rebuild). The size invariant keeps height O(log N) across any mix of
	// inserts/erases, so subsequent lookups/edits stay O(log N) too.
	//----------------------------------------------------------------------
	const int WBT_DELTA = 3, WBT_GAMMA = 2;

	//! Rebuild node (l, item, r) rebalancing a single size violation (the standard
	//! single/double rotation). Aggregates flow through MkSeq; no NodeStats.
	SeqRef Balance( SeqRef l, NodeRef item, size_t ib, int in_, SeqRef r )
	{
		const int ln = SeqCount(l), rn = SeqCount(r);
		if( ln + rn > 1 ) {
			if( rn > WBT_DELTA * ln ) {                                   // right too heavy
				if( SeqCount(r->left) < WBT_GAMMA * SeqCount(r->right) )  // single left
					return MkSeq( MkSeq(l, item, ib, in_, r->left), r->item, r->itemBytes, r->itemNewlines, r->right );
				const SeqRef& rl = r->left;                               // double left
				return MkSeq( MkSeq(l, item, ib, in_, rl->left), rl->item, rl->itemBytes, rl->itemNewlines,
				              MkSeq(rl->right, r->item, r->itemBytes, r->itemNewlines, r->right) );
			}
			if( ln > WBT_DELTA * rn ) {                                   // left too heavy
				if( SeqCount(l->right) < WBT_GAMMA * SeqCount(l->left) )  // single right
					return MkSeq( l->left, l->item, l->itemBytes, l->itemNewlines, MkSeq(l->right, item, ib, in_, r) );
				const SeqRef& lr = l->right;                              // double right
				return MkSeq( MkSeq(l->left, l->item, l->itemBytes, l->itemNewlines, lr->left), lr->item, lr->itemBytes, lr->itemNewlines,
				              MkSeq(lr->right, item, ib, in_, r) );
			}
		}
		return MkSeq( l, item, ib, in_, r );
	}

	SeqRef SeqInsertAt( const SeqRef& s, int index, NodeRef item, int& visits )
	{
		++visits;
		if( !s ) return MkSeqFresh( SeqRef(), std::move(item), SeqRef() );   // 1 item walk (counted), leaf
		const int li = SeqCount( s->left );
		if( index <= li ) return Balance( SeqInsertAt(s->left, index, std::move(item), visits), s->item, s->itemBytes, s->itemNewlines, s->right );
		return Balance( s->left, s->item, s->itemBytes, s->itemNewlines, SeqInsertAt(s->right, index - li - 1, std::move(item), visits) );
	}

	//! Remove + return the leftmost item's stats; rebuild the rest (balanced).
	SeqRef SeqEraseMin( const SeqRef& s, NodeRef& oItem, size_t& oB, int& oN, int& visits )
	{
		++visits;
		if( !s->left ) { oItem = s->item; oB = s->itemBytes; oN = s->itemNewlines; return s->right; }
		return Balance( SeqEraseMin(s->left, oItem, oB, oN, visits), s->item, s->itemBytes, s->itemNewlines, s->right );
	}
	SeqRef SeqEraseAt( const SeqRef& s, int index, int& visits )
	{
		++visits;
		if( !s ) return s;
		const int li = SeqCount( s->left );
		if( index <  li ) return Balance( SeqEraseAt(s->left, index, visits), s->item, s->itemBytes, s->itemNewlines, s->right );
		if( index >  li ) return Balance( s->left, s->item, s->itemBytes, s->itemNewlines, SeqEraseAt(s->right, index - li - 1, visits) );
		if( !s->left )  return s->right;     // index == li: drop this node (merge children)
		if( !s->right ) return s->left;
		NodeRef si; size_t sb; int sn;       // replace with the right subtree's leftmost (successor)
		SeqRef nr = SeqEraseMin( s->right, si, sb, sn, visits );
		return Balance( s->left, si, sb, sn, nr );
	}

	//! Item NodeRef at in-order index (O(log N), iterative).
	NodeRef SeqItemAt( const SeqRef& s, int index )
	{
		const SeqNode* cur = s.get();
		while( cur ) {
			const int li = SeqCount( cur->left );
			if( index <  li ) cur = cur->left.get();
			else if( index == li ) return cur->item;
			else { index -= li + 1; cur = cur->right.get(); }
		}
		return NodeRef();
	}

	//----------------------------------------------------------------------
	// Item 4 -- identity side-map + name-path index (both persistent, separate
	// from the green/seq node, weight-balanced like the item sequence).
	//----------------------------------------------------------------------

	//! "keyword/name" addressing key for a named chunk; "" otherwise.
	std::string ChunkNamePath( const NodeRef& item )
	{
		if( !item || item->kind != NodeKind::Chunk ) return std::string();
		std::string nm;
		if( !ParamValue( item.get(), "name", nm ) || nm.empty() ) return std::string();
		return item->role + "/" + nm;
	}

	// ---- IdSeq: positional persistent WBT of NodeId (occurrence -> NodeId) ----
	int IdSize( const IdSeqRef& s ) { return s ? s->count : 0; }
	IdSeqRef IdMk( IdSeqRef l, NodeId id, IdSeqRef r )
	{
		auto n = std::make_shared<IdNode>();
		n->left = std::move(l); n->right = std::move(r); n->id = id;
		n->count = 1 + IdSize(n->left) + IdSize(n->right);
		return n;
	}
	IdSeqRef IdBalance( IdSeqRef l, NodeId id, IdSeqRef r )
	{
		const int ln = IdSize(l), rn = IdSize(r);
		if( ln + rn > 1 ) {
			if( rn > WBT_DELTA * ln ) {
				if( IdSize(r->left) < WBT_GAMMA * IdSize(r->right) ) return IdMk( IdMk(l, id, r->left), r->id, r->right );
				const IdSeqRef& rl = r->left;
				return IdMk( IdMk(l, id, rl->left), rl->id, IdMk(rl->right, r->id, r->right) );
			}
			if( ln > WBT_DELTA * rn ) {
				if( IdSize(l->right) < WBT_GAMMA * IdSize(l->left) ) return IdMk( l->left, l->id, IdMk(l->right, id, r) );
				const IdSeqRef& lr = l->right;
				return IdMk( IdMk(l->left, l->id, lr->left), lr->id, IdMk(lr->right, id, r) );
			}
		}
		return IdMk( l, id, r );
	}
	IdSeqRef IdBuild( const std::vector<NodeId>& v, int lo, int hi )
	{
		if( lo >= hi ) return IdSeqRef();
		const int mid = (lo + hi) / 2;
		return IdMk( IdBuild(v, lo, mid), v[mid], IdBuild(v, mid+1, hi) );
	}
	IdSeqRef IdInsertAt( const IdSeqRef& s, int index, NodeId id )
	{
		if( !s ) return IdMk( IdSeqRef(), id, IdSeqRef() );
		const int li = IdSize(s->left);
		if( index <= li ) return IdBalance( IdInsertAt(s->left, index, id), s->id, s->right );
		return IdBalance( s->left, s->id, IdInsertAt(s->right, index - li - 1, id) );
	}
	IdSeqRef IdEraseMin( const IdSeqRef& s, NodeId& out )
	{
		if( !s->left ) { out = s->id; return s->right; }
		return IdBalance( IdEraseMin(s->left, out), s->id, s->right );
	}
	IdSeqRef IdEraseAt( const IdSeqRef& s, int index )
	{
		if( !s ) return s;
		const int li = IdSize(s->left);
		if( index <  li ) return IdBalance( IdEraseAt(s->left, index), s->id, s->right );
		if( index >  li ) return IdBalance( s->left, s->id, IdEraseAt(s->right, index - li - 1) );
		if( !s->left )  return s->right;
		if( !s->right ) return s->left;
		NodeId sid; IdSeqRef nr = IdEraseMin( s->right, sid );
		return IdBalance( s->left, sid, nr );
	}
	NodeId IdAt( const IdSeqRef& s, int index, int& visits )
	{
		const IdNode* cur = s.get();
		while( cur ) {
			++visits;
			const int li = IdSize(cur->left);
			if( index <  li ) cur = cur->left.get();
			else if( index == li ) return cur->id;
			else { index -= li + 1; cur = cur->right.get(); }
		}
		return 0;
	}
	void IdToVec( const IdSeqRef& s, std::vector<NodeId>& out )
	{
		if( !s ) return;
		IdToVec( s->left, out ); out.push_back( s->id ); IdToVec( s->right, out );
	}

	// ---- NameMap: key-ordered persistent WBT (name-path -> list of NodeIds) ----
	// The value is a LIST so duplicate name-paths (a degenerate but representable
	// scene) don't corrupt the index: erase/rename of one occurrence removes only
	// that id, survivors stay findable. NameFind returns the first occurrence.
	int NameSize( const NameMapRef& s ) { return s ? s->count : 0; }
	NameMapRef NameMk( NameMapRef l, std::string name, std::vector<NodeId> ids, NameMapRef r )
	{
		auto n = std::make_shared<NameNode>();
		n->left = std::move(l); n->right = std::move(r); n->name = std::move(name); n->ids = std::move(ids);
		n->count = 1 + NameSize(n->left) + NameSize(n->right);
		return n;
	}
	NameMapRef NameBalance( NameMapRef l, std::string name, std::vector<NodeId> ids, NameMapRef r )
	{
		const int ln = NameSize(l), rn = NameSize(r);
		if( ln + rn > 1 ) {
			if( rn > WBT_DELTA * ln ) {
				if( NameSize(r->left) < WBT_GAMMA * NameSize(r->right) ) return NameMk( NameMk(l, std::move(name), std::move(ids), r->left), r->name, r->ids, r->right );
				const NameMapRef& rl = r->left;
				return NameMk( NameMk(l, std::move(name), std::move(ids), rl->left), rl->name, rl->ids, NameMk(rl->right, r->name, r->ids, r->right) );
			}
			if( ln > WBT_DELTA * rn ) {
				if( NameSize(l->right) < WBT_GAMMA * NameSize(l->left) ) return NameMk( l->left, l->name, l->ids, NameMk(l->right, std::move(name), std::move(ids), r) );
				const NameMapRef& lr = l->right;
				return NameMk( NameMk(l->left, l->name, l->ids, lr->left), lr->name, lr->ids, NameMk(lr->right, std::move(name), std::move(ids), r) );
			}
		}
		return NameMk( l, std::move(name), std::move(ids), r );
	}
	// Append `id` to name's id-list (creating the entry if absent).
	NameMapRef NameInsert( const NameMapRef& s, const std::string& name, NodeId id )
	{
		if( !s ) return NameMk( NameMapRef(), name, std::vector<NodeId>{ id }, NameMapRef() );
		if( name < s->name ) return NameBalance( NameInsert(s->left, name, id), s->name, s->ids, s->right );
		if( s->name < name ) return NameBalance( s->left, s->name, s->ids, NameInsert(s->right, name, id) );
		std::vector<NodeId> merged = s->ids; merged.push_back( id );   // equal key -> append (duplicate name)
		return NameMk( s->left, s->name, std::move(merged), s->right );
	}
	// First NodeId for name + its occurrence count (the O(log N) COUNTED lookup
	// behind DocFindByName; count lets the caller refuse an ambiguous duplicate).
	NodeId NameFind( const NameMapRef& s, const std::string& name, int& visits, int& count )
	{
		const NameNode* cur = s.get();
		while( cur ) {
			++visits;
			if( name < cur->name ) cur = cur->left.get();
			else if( cur->name < name ) cur = cur->right.get();
			else { count = (int)cur->ids.size(); return cur->ids.empty() ? 0 : cur->ids.front(); }
		}
		count = 0;
		return 0;
	}
	NameMapRef NameEraseMin( const NameMapRef& s, std::string& kOut, std::vector<NodeId>& vOut )
	{
		if( !s->left ) { kOut = s->name; vOut = s->ids; return s->right; }
		return NameBalance( NameEraseMin(s->left, kOut, vOut), s->name, s->ids, s->right );
	}
	// Remove ONE occurrence of `id` from name's id-list; drop the node only when
	// the list becomes empty (so duplicate-name survivors stay findable). A no-op
	// if `id` is not present under `name`.
	NameMapRef NameErase( const NameMapRef& s, const std::string& name, NodeId id )
	{
		if( !s ) return s;
		if( name < s->name ) return NameBalance( NameErase(s->left, name, id), s->name, s->ids, s->right );
		if( s->name < name ) return NameBalance( s->left, s->name, s->ids, NameErase(s->right, name, id) );
		std::vector<NodeId> rest; rest.reserve( s->ids.size() );   // equal key: drop the first matching id
		bool removed = false;
		for( NodeId e : s->ids ) { if( !removed && e == id ) { removed = true; continue; } rest.push_back( e ); }
		if( !rest.empty() ) return NameMk( s->left, s->name, std::move(rest), s->right );   // value update, key stays
		if( !s->left )  return s->right;                                                    // last occurrence -> drop node
		if( !s->right ) return s->left;
		std::string k; std::vector<NodeId> v; NameMapRef nr = NameEraseMin( s->right, k, v );
		return NameBalance( s->left, std::move(k), std::move(v), nr );
	}

	// ---- IdMap: key-ordered persistent WBT (NodeId -> current green node) ----
	// The reverse index: a durable NodeId resolves to the node it now labels in
	// O(log N), without scanning the id side-map.
	int IdMapSize( const IdMapRef& s ) { return s ? s->count : 0; }
	IdMapRef IdMapMk( IdMapRef l, NodeId key, NodeRef val, IdMapRef r )
	{
		auto n = std::make_shared<IdMapNode>();
		n->left = std::move(l); n->right = std::move(r); n->key = key; n->val = std::move(val);
		n->count = 1 + IdMapSize(n->left) + IdMapSize(n->right);
		return n;
	}
	IdMapRef IdMapBalance( IdMapRef l, NodeId key, NodeRef val, IdMapRef r )
	{
		const int ln = IdMapSize(l), rn = IdMapSize(r);
		if( ln + rn > 1 ) {
			if( rn > WBT_DELTA * ln ) {
				if( IdMapSize(r->left) < WBT_GAMMA * IdMapSize(r->right) ) return IdMapMk( IdMapMk(l, key, std::move(val), r->left), r->key, r->val, r->right );
				const IdMapRef& rl = r->left;
				return IdMapMk( IdMapMk(l, key, std::move(val), rl->left), rl->key, rl->val, IdMapMk(rl->right, r->key, r->val, r->right) );
			}
			if( ln > WBT_DELTA * rn ) {
				if( IdMapSize(l->right) < WBT_GAMMA * IdMapSize(l->left) ) return IdMapMk( l->left, l->key, l->val, IdMapMk(l->right, key, std::move(val), r) );
				const IdMapRef& lr = l->right;
				return IdMapMk( IdMapMk(l->left, l->key, l->val, lr->left), lr->key, lr->val, IdMapMk(lr->right, key, std::move(val), r) );
			}
		}
		return IdMapMk( l, key, std::move(val), r );
	}
	IdMapRef IdMapSet( const IdMapRef& s, NodeId key, NodeRef val )
	{
		if( !s ) return IdMapMk( IdMapRef(), key, std::move(val), IdMapRef() );
		if( key < s->key ) return IdMapBalance( IdMapSet(s->left, key, std::move(val)), s->key, s->val, s->right );
		if( s->key < key ) return IdMapBalance( s->left, s->key, s->val, IdMapSet(s->right, key, std::move(val)) );
		return IdMapMk( s->left, s->key, std::move(val), s->right );   // overwrite
	}
	NodeRef IdMapGet( const IdMapRef& s, NodeId key, int& visits )
	{
		const IdMapNode* cur = s.get();
		while( cur ) {
			++visits;
			if( key < cur->key ) cur = cur->left.get();
			else if( cur->key < key ) cur = cur->right.get();
			else return cur->val;
		}
		return NodeRef();
	}
	IdMapRef IdMapEraseMin( const IdMapRef& s, NodeId& kOut, NodeRef& vOut )
	{
		if( !s->left ) { kOut = s->key; vOut = s->val; return s->right; }
		return IdMapBalance( IdMapEraseMin(s->left, kOut, vOut), s->key, s->val, s->right );
	}
	IdMapRef IdMapErase( const IdMapRef& s, NodeId key )
	{
		if( !s ) return s;
		if( key < s->key ) return IdMapBalance( IdMapErase(s->left, key), s->key, s->val, s->right );
		if( s->key < key ) return IdMapBalance( s->left, s->key, s->val, IdMapErase(s->right, key) );
		if( !s->left )  return s->right;
		if( !s->right ) return s->left;
		NodeId k; NodeRef v; IdMapRef nr = IdMapEraseMin( s->right, k, v );
		return IdMapBalance( s->left, k, std::move(v), nr );
	}

	// ---- ParamMap: key-ordered persistent WBT ("<chunkId>\x1f<role>" -> NodeId) ----
	// Per-parameter-occurrence identity (D26/D36). Keyed by owning chunk's id +
	// the param role, so a value edit (chunk id + role unchanged) keeps the id.
	int ParamSize( const ParamMapRef& s ) { return s ? s->count : 0; }
	ParamMapRef ParamMk( ParamMapRef l, std::string key, NodeId id, ParamMapRef r )
	{
		auto n = std::make_shared<ParamMapNode>();
		n->left = std::move(l); n->right = std::move(r); n->key = std::move(key); n->id = id;
		n->count = 1 + ParamSize(n->left) + ParamSize(n->right);
		return n;
	}
	ParamMapRef ParamBalance( ParamMapRef l, std::string key, NodeId id, ParamMapRef r )
	{
		const int ln = ParamSize(l), rn = ParamSize(r);
		if( ln + rn > 1 ) {
			if( rn > WBT_DELTA * ln ) {
				if( ParamSize(r->left) < WBT_GAMMA * ParamSize(r->right) ) return ParamMk( ParamMk(l, std::move(key), id, r->left), r->key, r->id, r->right );
				const ParamMapRef& rl = r->left;
				return ParamMk( ParamMk(l, std::move(key), id, rl->left), rl->key, rl->id, ParamMk(rl->right, r->key, r->id, r->right) );
			}
			if( ln > WBT_DELTA * rn ) {
				if( ParamSize(l->right) < WBT_GAMMA * ParamSize(l->left) ) return ParamMk( l->left, l->key, l->id, ParamMk(l->right, std::move(key), id, r) );
				const ParamMapRef& lr = l->right;
				return ParamMk( ParamMk(l->left, l->key, l->id, lr->left), lr->key, lr->id, ParamMk(lr->right, std::move(key), id, r) );
			}
		}
		return ParamMk( l, std::move(key), id, r );
	}
	ParamMapRef ParamSet( const ParamMapRef& s, const std::string& key, NodeId id )
	{
		if( !s ) return ParamMk( ParamMapRef(), key, id, ParamMapRef() );
		if( key < s->key ) return ParamBalance( ParamSet(s->left, key, id), s->key, s->id, s->right );
		if( s->key < key ) return ParamBalance( s->left, s->key, s->id, ParamSet(s->right, key, id) );
		return ParamMk( s->left, s->key, id, s->right );   // overwrite
	}
	NodeId ParamGet( const ParamMapRef& s, const std::string& key )
	{
		const ParamMapNode* cur = s.get();
		while( cur ) {
			if( key < cur->key ) cur = cur->left.get();
			else if( cur->key < key ) cur = cur->right.get();
			else return cur->id;
		}
		return 0;
	}
	ParamMapRef ParamEraseMin( const ParamMapRef& s, std::string& kOut, NodeId& vOut )
	{
		if( !s->left ) { kOut = s->key; vOut = s->id; return s->right; }
		return ParamBalance( ParamEraseMin(s->left, kOut, vOut), s->key, s->id, s->right );
	}
	ParamMapRef ParamErase( const ParamMapRef& s, const std::string& key )
	{
		if( !s ) return s;
		if( key < s->key ) return ParamBalance( ParamErase(s->left, key), s->key, s->id, s->right );
		if( s->key < key ) return ParamBalance( s->left, s->key, s->id, ParamErase(s->right, key) );
		if( !s->left )  return s->right;
		if( !s->right ) return s->left;
		std::string k; NodeId v; ParamMapRef nr = ParamEraseMin( s->right, k, v );
		return ParamBalance( s->left, std::move(k), v, nr );
	}
	std::string ParamKey( NodeId chunkId, const std::string& role ) { return std::to_string( (long)chunkId ) + "\x1f" + role; }

	//! The (role, node) Param children of a chunk (skips keyword/braces/trivia).
	std::vector<std::pair<std::string, NodeRef>> ChunkParams( const NodeRef& chunk )
	{
		std::vector<std::pair<std::string, NodeRef>> out;
		if( chunk && chunk->kind == NodeKind::Chunk )
			for( const auto& k : chunk->kids )
				if( k->kind == NodeKind::Param ) out.push_back( { k->role, k } );
		return out;
	}
	//! Mint FRESH param ids for `chunk`'s params (parse / insert / new-chunk path).
	void AddChunkParams( ParamMapRef& pids, IdMapRef& byId, NodeId chunkId, const NodeRef& chunk, NodeId& nextId )
	{
		for( auto& rp : ChunkParams(chunk) ) {
			NodeId pid = nextId++;
			pids = ParamSet( pids, ParamKey(chunkId, rp.first), pid );
			byId = IdMapSet( byId, pid, rp.second );
		}
	}
	//! Drop `oldChunk`'s param ids from both indices (erase path).
	void DropChunkParams( ParamMapRef& pids, IdMapRef& byId, NodeId chunkId, const NodeRef& oldChunk )
	{
		for( auto& rp : ChunkParams(oldChunk) ) {
			std::string key = ParamKey( chunkId, rp.first );
			NodeId pid = ParamGet( pids, key );
			if( pid ) { byId = IdMapErase( byId, pid ); pids = ParamErase( pids, key ); }
		}
	}
	//! Re-point chunk `chunkId`'s param ids at the NEW chunk's param nodes, REUSING
	//! the id where the role persists (a value edit keeps the param's id), minting
	//! for added roles and dropping vanished roles.
	void ReindexChunkParams( ParamMapRef& pids, IdMapRef& byId, NodeId chunkId, const NodeRef& oldChunk, const NodeRef& newChunk, NodeId& nextId )
	{
		auto np = ChunkParams( newChunk );
		for( auto& rp : ChunkParams(oldChunk) ) {            // drop roles that vanished
			bool kept = false;
			for( auto& q : np ) if( q.first == rp.first ) { kept = true; break; }
			if( !kept ) { std::string key = ParamKey(chunkId, rp.first); NodeId pid = ParamGet(pids, key); if( pid ) { byId = IdMapErase(byId, pid); pids = ParamErase(pids, key); } }
		}
		for( auto& rp : np ) {                               // reuse or mint, repoint to the new node
			std::string key = ParamKey( chunkId, rp.first );
			NodeId pid = ParamGet( pids, key );
			if( !pid ) { pid = nextId++; pids = ParamSet( pids, key, pid ); }
			byId = IdMapSet( byId, pid, rp.second );
		}
	}
}

namespace RISE { namespace Cst {

Document ParseToCst( const std::string& bytes )
{
	std::vector<RawTok> t = Tokenize( bytes );
	size_t i = 0;
	std::vector<NodeRef> items;
	while( i < t.size() ) {
		if( t[i].trivia ) { items.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") ); continue; }
		// A chunk is `keyword {` (brace may be on the next line). A bare word not
		// followed by `{` -- e.g. each token of the `RISE ASCII SCENE 6` header --
		// is preserved losslessly as a stray Token, NOT swallowed as a never-closed
		// chunk. (A dedicated version-header node is a later item.)
		size_t j = i + 1;
		while( j < t.size() && t[j].trivia ) ++j;
		if( j < t.size() && !t[j].trivia && t[j].text == "{" ) items.push_back( ParseChunk( t, i ) );
		else items.push_back( Leaf(NodeKind::Token, t[i++].text, "stray") );
	}
	Document d;
	d.items = SeqBuild( items, 0, (int)items.size() );
	// item 4: fresh NodeIds 1..N in lockstep (the identity side-map) + name index
	// + the NodeId -> node reverse index.
	std::vector<NodeId> ids;
	for( int k = 0; k < (int)items.size(); ++k ) {
		NodeId id = (NodeId)( k + 1 );
		ids.push_back( id );
		d.byId = IdMapSet( d.byId, id, items[k] );
		std::string np = ChunkNamePath( items[k] );
		if( !np.empty() ) d.byName = NameInsert( d.byName, np, id );
	}
	d.idseq  = IdBuild( ids, 0, (int)ids.size() );
	d.nextId = (NodeId)items.size() + 1;
	for( int k = 0; k < (int)items.size(); ++k )
		AddChunkParams( d.paramIds, d.byId, ids[k], items[k], d.nextId );   // per-param occurrence ids
	return d;
}

std::string SerializeCst( const Document& doc )
{
	std::string s;
	SeqSerialize( doc.items, s );
	return s;
}

int DeriveToJob( const Document& doc, IJob& pJob, std::vector<std::string>* diagnostics )
{
	std::vector<std::string> local;
	std::vector<std::string>& diags = diagnostics ? *diagnostics : local;
	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );

	// PASS 1 -- validate every sphere_geometry chunk (the safe boundary).
	for( const auto& c : items )
		if( c->kind == NodeKind::Chunk && c->role == "sphere_geometry" )
			ValidateSphereChunk( c.get(), diags );
	if( !diags.empty() ) return 0;   // refuse-all: a malformed scene applies NOTHING

	// PASS 2 -- apply (all chunks validated).
	int count = 0;
	for( const auto& c : items ) {
		if( c->kind != NodeKind::Chunk || c->role != "sphere_geometry" ) continue;
		std::string name = "noname", radStr = "1.0";
		ParamValue( c.get(), "name",   name );
		ParamValue( c.get(), "radius", radStr );
		if( pJob.AddSphereGeometry( name.c_str(), std::atof( radStr.c_str() ) ) ) ++count;
	}
	return count;
}

int    DocItemCount  ( const Document& doc ) { return SeqCount( doc.items ); }
size_t DocByteWidth  ( const Document& doc ) { return SeqBytes( doc.items ); }
int    DocNewlineCount( const Document& doc ) { return SeqNl( doc.items ); }
unsigned long DebugItemStatWalks() { return g_itemStatWalks; }
unsigned long DebugReparseOldVisits() { return g_reparseOldVisits; }

int DocItemAtByteOffset( const Document& doc, size_t offset, NodeRef* outItem, size_t* outStart, int* visits )
{
	NodeRef item; size_t start = 0; int v = 0;
	int idx = SeqAtOffset( doc.items, offset, 0, 0, item, start, v );
	if( visits ) *visits = v;
	if( idx >= 0 ) { if( outItem ) *outItem = item; if( outStart ) *outStart = start; }
	return idx;
}

Document DocReplaceItem( const Document& doc, int index, NodeRef newItem, int* visits )
{
	if( visits ) *visits = 0;
	if( !newItem ) return doc;                                     // non-null contract: refuse
	if( index < 0 || index >= DocItemCount(doc) ) return doc;      // out of range: no-op
	// identity (item 4): the NodeId at `index` PERSISTS (idseq unchanged); point
	// the reverse index at the new node, re-key byName if the name changed, and
	// re-point the chunk's PARAM identities (kept by role across a value edit).
	const NodeRef     oldChunk = SeqItemAt( doc.items, index );
	const std::string oldName  = ChunkNamePath( oldChunk );
	const std::string newName  = ChunkNamePath( newItem );
	const NodeRef     newChunk = newItem;                         // handle before the move
	int iv = 0; const NodeId id = IdAt( doc.idseq, index, iv );    // the persisting id
	int v = 0;
	Document d = doc;                                              // carry idseq / byName / byId / paramIds / nextId
	d.byId  = IdMapSet( d.byId, id, newItem );                    // reverse index -> the new node
	d.items = SeqReplace( doc.items, index, std::move(newItem), v );
	if( visits ) *visits = v;
	if( oldName != newName ) {
		if( !oldName.empty() ) d.byName = NameErase( d.byName, oldName, id );
		if( !newName.empty() ) d.byName = NameInsert( d.byName, newName, id );
	}
	ReindexChunkParams( d.paramIds, d.byId, id, oldChunk, newChunk, d.nextId );
	return d;
}

Document DocInsertItem( const Document& doc, int index, NodeRef newItem, int* visits )
{
	if( visits ) *visits = 0;
	if( !newItem ) return doc;                                     // non-null contract: refuse
	int n = SeqCount( doc.items );
	if( index < 0 ) index = 0;
	if( index > n ) index = n;
	const NodeId id = doc.nextId;                                  // fresh identity
	const std::string np = ChunkNamePath( newItem );
	const NodeRef newChunk = newItem;                             // handle before the move
	int v = 0;
	Document d = doc;
	d.byId   = IdMapSet( d.byId, id, newItem );                         // reverse index
	d.items  = SeqInsertAt( doc.items, index, std::move(newItem), v );   // O(log N) WBT
	d.idseq  = IdInsertAt( doc.idseq, index, id );                       // O(log N) lockstep splice
	if( !np.empty() ) d.byName = NameInsert( d.byName, np, id );
	d.nextId = doc.nextId + 1;
	AddChunkParams( d.paramIds, d.byId, id, newChunk, d.nextId );        // param occurrence ids
	if( visits ) *visits = v;
	return d;
}

Document DocEraseItem( const Document& doc, int index, int* visits )
{
	if( visits ) *visits = 0;
	if( index < 0 || index >= SeqCount(doc.items) ) return doc;    // out of range: no-op
	const NodeRef oldChunk = SeqItemAt( doc.items, index );
	const std::string np = ChunkNamePath( oldChunk );
	int iv = 0; const NodeId eid = IdAt( doc.idseq, index, iv );   // the erased item's id
	int v = 0;
	Document d = doc;
	d.items  = SeqEraseAt( doc.items, index, v );                  // O(log N) WBT
	d.idseq  = IdEraseAt( doc.idseq, index );                      // O(log N) lockstep splice
	d.byId   = IdMapErase( d.byId, eid );                          // reverse index drops the id
	if( !np.empty() ) d.byName = NameErase( d.byName, np, eid );
	DropChunkParams( d.paramIds, d.byId, eid, oldChunk );          // param occurrence ids
	if( visits ) *visits = v;
	return d;
}

//---- item 4: identity + name-path lookups ----

NodeId DocNodeIdAt( const Document& doc, int index, int* visits )
{
	int v = 0;
	NodeId id = ( index < 0 || index >= IdSize(doc.idseq) ) ? 0 : IdAt( doc.idseq, index, v );
	if( visits ) *visits = v;
	return id;
}

NodeId DocFindByName( const Document& doc, const std::string& namePath, int* visits, int* occurrences )
{
	int v = 0, count = 0;
	NodeId id = NameFind( doc.byName, namePath, v, count );
	if( visits ) *visits = v;
	if( occurrences ) *occurrences = count;
	return ( count == 1 ) ? id : 0;   // unique-or-refuse: an ambiguous duplicate name resolves to 0
}

NodeRef DocResolveNodeId( const Document& doc, NodeId id, int* visits )
{
	int v = 0;
	NodeRef n = IdMapGet( doc.byId, id, v );
	if( visits ) *visits = v;
	return n;
}

int DocIndexOfNodeId( const Document& doc, NodeId id, NodeRef* outItem )
{
	std::vector<NodeId> ids; IdToVec( doc.idseq, ids );
	for( int i = 0; i < (int)ids.size(); ++i )
		if( ids[i] == id ) { if( outItem ) *outItem = SeqItemAt( doc.items, i ); return i; }
	if( outItem ) *outItem = NodeRef();
	return -1;
}

NodeRef DocParamAtByteOffset( const Document& doc, size_t offset, NodeRef* outChunk, int* visits,
                              NodeId* outParamId, NodeId* outChunkId )
{
	int v = 0;
	NodeRef item; size_t start = 0;
	int idx = SeqAtOffset( doc.items, offset, 0, 0, item, start, v );
	if( visits ) *visits = v;
	if( outChunk )   *outChunk = NodeRef();
	if( outParamId ) *outParamId = 0;
	if( outChunkId ) *outChunkId = 0;
	if( idx < 0 || !item || item->kind != NodeKind::Chunk ) return NodeRef();
	if( outChunk ) *outChunk = item;
	int civ = 0; const NodeId chunkId = IdAt( doc.idseq, idx, civ );   // the enclosing chunk's id
	if( outChunkId ) *outChunkId = chunkId;
	// within-chunk: walk the chunk's kids (a handful) accumulating byte widths to
	// find the kid spanning (offset - start); return it iff it is a Param, with its
	// stable param NodeId.
	const size_t want = offset - start;
	size_t acc = 0;
	for( const auto& k : item->kids ) {
		size_t kb = 0; int kn = 0; NodeStats( k, kb, kn );
		if( want < acc + kb ) {
			if( k->kind != NodeKind::Param ) return NodeRef();
			if( outParamId ) *outParamId = ParamGet( doc.paramIds, ParamKey(chunkId, k->role) );
			return k;
		}
		acc += kb;
	}
	return NodeRef();
}

NodeId DocParamId( const Document& doc, NodeId chunkId, const std::string& role )
{
	return ParamGet( doc.paramIds, ParamKey(chunkId, role) );
}

Document DocReparse( const Document& oldDoc, const std::string& newText, std::vector<NodeId>* invalidated )
{
	Document fresh = ParseToCst( newText );   // fresh ids 1..M
	std::vector<NodeRef> oldItems; SeqToVec( oldDoc.items, oldItems );
	std::vector<NodeId>  oldIds;   IdToVec ( oldDoc.idseq, oldIds );
	std::vector<NodeRef> newItems; SeqToVec( fresh.items, newItems );
	const int O = (int)oldItems.size();
	const int M = (int)newItems.size();

	// Keys (all O(1) via hash maps):
	//   fullOf -- exact bytes (an unchanged item carries its id across a REORDER).
	//   keyOf  -- a chunk's (keyword,name) (a NAMED value edit keeps the key).
	//   role   -- a chunk's keyword (a RENAME keeps the type -> lineage survives).
	auto fullOf = []( const NodeRef& it ) -> std::string { std::string s; Serialize( it, s ); return s; };
	auto keyOf  = []( const NodeRef& it ) -> std::string { std::string nm; ParamValue(it.get(),"name",nm); return it->role + "/" + nm; };

	std::vector<NodeId> carried( M, 0 );
	std::vector<bool>   oldUsed( O, false );

	// PASS 1 -- FULL content, but ONLY for groups whose multiset is UNCHANGED
	// (old count == new count). Carries unchanged items -- incl. byte-identical
	// duplicates and trivia -- across any reorder, position-by-position in doc
	// order. A group whose count CHANGED (a partial edit of duplicates) is NOT
	// consumed here: greedily pairing indistinguishable rows would swap identities
	// (the surviving twin of an edited pair must be invalidated, not re-bound).
	{
		std::unordered_map<std::string, std::vector<int>> oldF, newF;
		for( int i = 0; i < O; ++i ) { oldF[ fullOf(oldItems[i]) ].push_back( i ); ++g_reparseOldVisits; }
		for( int j = 0; j < M; ++j )   newF[ fullOf(newItems[j]) ].push_back( j );
		for( auto& kv : newF ) {
			auto oit = oldF.find( kv.first );
			if( oit == oldF.end() || oit->second.size() != kv.second.size() ) continue;   // changed group -> defer
			for( size_t k = 0; k < kv.second.size(); ++k ) { int oi = oit->second[k]; oldUsed[oi] = true; carried[ kv.second[k] ] = oldIds[oi]; }
		}
	}

	// PASS 2 (keyword,name) then PASS 3 (keyword) -- carry a CHUNK by a key UNIQUE
	// 1<->1 among the remainder. Pass 2 keeps a named chunk's value-edit id; pass 3
	// keeps a rename's id when the chunk is the unique one of its type (lineage
	// survives rename + reparse on a best-effort basis, D9/D44). An ambiguous group
	// (a key with >1 remaining) is never position-guessed -> it falls to pass 4.
	auto uniqueCarry = [&]( bool byKeyword ) {
		std::unordered_map<std::string, std::vector<int>> oldR, newR;
		for( int i = 0; i < O; ++i ) if( !oldUsed[i] && oldItems[i]->kind == NodeKind::Chunk ) { oldR[ byKeyword ? oldItems[i]->role : keyOf(oldItems[i]) ].push_back( i ); ++g_reparseOldVisits; }
		for( int j = 0; j < M; ++j ) if( carried[j] == 0 && newItems[j]->kind == NodeKind::Chunk ) newR[ byKeyword ? newItems[j]->role : keyOf(newItems[j]) ].push_back( j );
		for( auto& kv : newR ) {
			if( kv.second.size() != 1 ) continue;
			auto oit = oldR.find( kv.first );
			if( oit == oldR.end() || oit->second.size() != 1 ) continue;
			int j = kv.second[0], i = oit->second[0];
			oldUsed[i] = true; carried[j] = oldIds[i];
		}
	};
	uniqueCarry( false );   // pass 2: (keyword, name)
	uniqueCarry( true );    // pass 3: keyword only (rename lineage)

	// PASS 4 -- fresh ids for unmatched new; INVALIDATE unmatched old.
	NodeId next = oldDoc.nextId;
	for( int j = 0; j < M; ++j ) if( carried[j] == 0 ) carried[j] = next++;
	if( invalidated ) { invalidated->clear(); for( int i = 0; i < O; ++i ) if( !oldUsed[i] ) invalidated->push_back( oldIds[i] ); }

	Document d = fresh;
	d.idseq    = IdBuild( carried, 0, M );
	d.byName   = NameMapRef();
	d.byId     = IdMapRef();
	d.paramIds = ParamMapRef();
	NodeId pnext = next;                               // mint fresh ids (chunk + param) from here
	for( int j = 0; j < M; ++j ) {
		d.byId = IdMapSet( d.byId, carried[j], newItems[j] );
		std::string np = ChunkNamePath( newItems[j] );
		if( !np.empty() ) d.byName = NameInsert( d.byName, np, carried[j] );
		// params: carry by (carried chunk id, role) from oldDoc -- a carried chunk
		// keeps its param ids; a fresh chunk's params mint fresh.
		for( auto& rp : ChunkParams( newItems[j] ) ) {
			std::string key = ParamKey( carried[j], rp.first );
			NodeId pid = ParamGet( oldDoc.paramIds, key );
			if( !pid ) pid = pnext++;
			d.paramIds = ParamSet( d.paramIds, key, pid );
			d.byId = IdMapSet( d.byId, pid, rp.second );
		}
	}
	NodeId mx = pnext;                                 // nextId strictly above every live id
	for( NodeId id : carried ) if( id + 1 > mx ) mx = id + 1;
	d.nextId = mx;
	return d;
}

} }
