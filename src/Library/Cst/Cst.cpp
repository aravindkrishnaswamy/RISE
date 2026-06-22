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
	// First NodeId for name (0 if none); the O(log N) COUNTED lookup behind DocFindByName.
	NodeId NameFind( const NameMapRef& s, const std::string& name, int& visits )
	{
		const NameNode* cur = s.get();
		while( cur ) {
			++visits;
			if( name < cur->name ) cur = cur->left.get();
			else if( cur->name < name ) cur = cur->right.get();
			else return cur->ids.empty() ? 0 : cur->ids.front();
		}
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
	// item 4: fresh NodeIds 1..N in lockstep (the identity side-map) + name index.
	std::vector<NodeId> ids;
	for( int k = 0; k < (int)items.size(); ++k ) {
		NodeId id = (NodeId)( k + 1 );
		ids.push_back( id );
		std::string np = ChunkNamePath( items[k] );
		if( !np.empty() ) d.byName = NameInsert( d.byName, np, id );
	}
	d.idseq  = IdBuild( ids, 0, (int)ids.size() );
	d.nextId = (NodeId)items.size() + 1;
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
	// identity (item 4): the NodeId at `index` PERSISTS (idseq unchanged); only
	// re-key byName if the chunk's name changed.
	const std::string oldName = ChunkNamePath( SeqItemAt( doc.items, index ) );
	const std::string newName = ChunkNamePath( newItem );
	int v = 0;
	Document d = doc;                                              // carry idseq / byName / nextId
	d.items = SeqReplace( doc.items, index, std::move(newItem), v );
	if( visits ) *visits = v;
	if( oldName != newName ) {
		int iv = 0; NodeId id = IdAt( doc.idseq, index, iv );   // the persisting id
		if( !oldName.empty() ) d.byName = NameErase( d.byName, oldName, id );
		if( !newName.empty() ) d.byName = NameInsert( d.byName, newName, id );
	}
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
	int v = 0;
	Document d = doc;
	d.items  = SeqInsertAt( doc.items, index, std::move(newItem), v );   // O(log N) WBT
	d.idseq  = IdInsertAt( doc.idseq, index, id );                       // O(log N) lockstep splice
	if( !np.empty() ) d.byName = NameInsert( d.byName, np, id );
	d.nextId = doc.nextId + 1;
	if( visits ) *visits = v;
	return d;
}

Document DocEraseItem( const Document& doc, int index, int* visits )
{
	if( visits ) *visits = 0;
	if( index < 0 || index >= SeqCount(doc.items) ) return doc;    // out of range: no-op
	const std::string np = ChunkNamePath( SeqItemAt( doc.items, index ) );
	int iv = 0; const NodeId eid = IdAt( doc.idseq, index, iv );   // the erased item's id
	int v = 0;
	Document d = doc;
	d.items  = SeqEraseAt( doc.items, index, v );                  // O(log N) WBT
	d.idseq  = IdEraseAt( doc.idseq, index );                      // O(log N) lockstep splice
	if( !np.empty() ) d.byName = NameErase( d.byName, np, eid );
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

NodeId DocFindByName( const Document& doc, const std::string& namePath, int* visits )
{
	int v = 0;
	NodeId id = NameFind( doc.byName, namePath, v );
	if( visits ) *visits = v;
	return id;
}

int DocIndexOfNodeId( const Document& doc, NodeId id, NodeRef* outItem )
{
	std::vector<NodeId> ids; IdToVec( doc.idseq, ids );
	for( int i = 0; i < (int)ids.size(); ++i )
		if( ids[i] == id ) { if( outItem ) *outItem = SeqItemAt( doc.items, i ); return i; }
	if( outItem ) *outItem = NodeRef();
	return -1;
}

NodeRef DocParamAtByteOffset( const Document& doc, size_t offset, NodeRef* outChunk, int* visits )
{
	int v = 0;
	NodeRef item; size_t start = 0;
	int idx = SeqAtOffset( doc.items, offset, 0, 0, item, start, v );
	if( visits ) *visits = v;
	if( outChunk ) *outChunk = NodeRef();
	if( idx < 0 || !item || item->kind != NodeKind::Chunk ) return NodeRef();
	if( outChunk ) *outChunk = item;
	// within-chunk: walk the chunk's kids (a handful) accumulating byte widths to
	// find the kid spanning (offset - start); return it iff it is a Param.
	const size_t want = offset - start;
	size_t acc = 0;
	for( const auto& k : item->kids ) {
		size_t kb = 0; int kn = 0; NodeStats( k, kb, kn );
		if( want < acc + kb ) return ( k->kind == NodeKind::Param ) ? k : NodeRef();
		acc += kb;
	}
	return NodeRef();
}

Document DocReparse( const Document& oldDoc, const std::string& newText, std::vector<NodeId>* invalidated )
{
	Document fresh = ParseToCst( newText );   // fresh ids 1..M
	std::vector<NodeRef> oldItems; SeqToVec( oldDoc.items, oldItems );
	std::vector<NodeId>  oldIds;   IdToVec ( oldDoc.idseq, oldIds );
	std::vector<NodeRef> newItems; SeqToVec( fresh.items, newItems );
	const int O = (int)oldItems.size();
	const int M = (int)newItems.size();

	// Two keys per item, both O(1) to compare via hash maps (overall O(M+N)):
	//   fullOf -- the item's exact bytes (identity-stable across REORDER: an
	//             unchanged chunk/trivia carries its id wherever it moved).
	//   keyOf  -- a chunk's (keyword, name); a trivia/stray's bytes (identity-
	//             stable across a VALUE EDIT of a NAMED chunk: bytes change but
	//             the name key does not).
	auto fullOf = []( const NodeRef& it ) -> std::string { std::string s; Serialize( it, s ); return s; };
	auto keyOf  = []( const NodeRef& it ) -> std::string {
		if( it->kind == NodeKind::Chunk ) { std::string nm; ParamValue(it.get(),"name",nm); return "C:" + it->role + "/" + nm; }
		std::string s; Serialize( it, s ); return "T:" + s;
	};

	std::vector<NodeId> carried( M, 0 );
	std::vector<bool>   oldUsed( O, false );

	// PASS 1 -- match by FULL content (greedy, in order within a bucket). Identical
	// items carry their id across any reordering; position is irrelevant.
	{
		std::unordered_map<std::string, std::vector<int>> bucket;   // full -> old indices, in order
		for( int i = 0; i < O; ++i ) bucket[ fullOf(oldItems[i]) ].push_back( i );
		std::unordered_map<std::string, size_t> cursor;
		for( int j = 0; j < M; ++j ) {
			const std::string f = fullOf( newItems[j] );
			auto it = bucket.find( f );
			if( it == bucket.end() ) continue;
			size_t& c = cursor[f];
			if( c < it->second.size() ) { int oi = it->second[c++]; oldUsed[oi] = true; carried[j] = oldIds[oi]; }
		}
	}

	// PASS 2 -- match the remainder by content-KEY, but ONLY when the key is
	// UNIQUE among the still-unmatched on BOTH sides (a 1<->1 pairing). This carries
	// a NAMED chunk's value edit ("the chunk named s, edited") without ever
	// position-remapping a key shared by several rows. Genuinely-ambiguous groups
	// (a key with >1 unmatched old or new -- e.g. several edited unnamed chunks of
	// the same type) are left for pass 3: invalidate, don't guess (D9/D15).
	{
		std::unordered_map<std::string, std::vector<int>> oldRem, newRem;
		for( int i = 0; i < O; ++i ) if( !oldUsed[i] )       oldRem[ keyOf(oldItems[i]) ].push_back( i );
		for( int j = 0; j < M; ++j ) if( carried[j] == 0 )   newRem[ keyOf(newItems[j]) ].push_back( j );
		for( auto& kv : newRem ) {
			if( kv.second.size() != 1 ) continue;
			auto oit = oldRem.find( kv.first );
			if( oit == oldRem.end() || oit->second.size() != 1 ) continue;
			int j = kv.second[0], i = oit->second[0];
			oldUsed[i] = true; carried[j] = oldIds[i];
		}
	}

	// PASS 3 -- still-unmatched new items get FRESH ids; still-unmatched old ids
	// are INVALIDATED (a rename, a deletion, or a genuinely-ambiguous row).
	NodeId next = oldDoc.nextId;
	for( int j = 0; j < M; ++j ) if( carried[j] == 0 ) carried[j] = next++;
	if( invalidated ) { invalidated->clear(); for( int i = 0; i < O; ++i ) if( !oldUsed[i] ) invalidated->push_back( oldIds[i] ); }

	Document d = fresh;
	d.idseq  = IdBuild( carried, 0, M );
	d.byName = NameMapRef();
	for( int j = 0; j < M; ++j ) { std::string np = ChunkNamePath( newItems[j] ); if( !np.empty() ) d.byName = NameInsert( d.byName, np, carried[j] ); }
	NodeId mx = next;                                  // nextId strictly above every live id
	for( NodeId id : carried ) if( id + 1 > mx ) mx = id + 1;
	d.nextId = mx;
	return d;
}

} }
