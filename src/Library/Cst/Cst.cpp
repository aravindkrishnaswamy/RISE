//////////////////////////////////////////////////////////////////////
//
//  Cst.cpp - Concrete Syntax Tree kernel (agentic redesign).
//
//  Promoted from the validated prototype (tests/CstSlicePrototype.h, slices
//  1/1.5/2/3) into the real library. See Cst.h for the full landed scope. Item 3
//  puts the Document's top-level item list on a persistent balanced sequence
//  with cached byte-width / newline aggregates, so locating an edit target by
//  byte offset (or index) is O(log N) and COUNTED (not an O(N) scan). Item 4
//  adds NodeId lineage + the identity side-maps. Item 5 routes DeriveToJob
//  through the LIVE chunk-parser registry (CreateAllChunkParsers): every chunk
//  type is validated via the shared DispatchChunkParameters and applied via the
//  shared IAsciiChunkParser::Finalize -- with each param line whitespace-
//  normalised exactly as the legacy parser normalises it -- so the CST and
//  legacy paths build an identical Job for the canonical scenes the CST is fed
//  (see DeriveToJob for the exact equivalence scope + failure boundary).
//
//////////////////////////////////////////////////////////////////////

#include "Cst.h"
#include "../Interfaces/IJob.h"
#include "../Parsers/ChunkParserRegistry.h"   // CreateAllChunkParsers (the LIVE registry)
#include "../Parsers/IAsciiChunkParser.h"     // IAsciiChunkParser, DispatchChunkParameters

#include <cstdlib>
#include <map>
#include <unordered_map>
#include <unordered_set>

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
					pk.push_back( Leaf(NodeKind::Token, t[i++].text, "pvalue") );   // first value token
					// A param's value can be MULTIPLE same-line tokens (e.g.
					// `color 1 0 0`). Capture each additional same-line token (with
					// its inter-token trivia) as another pvalue, until a newline or a
					// brace ends the line. The trailing newline stays for the outer
					// loop (chunk-level trivia between this param and the next).
					for( ;; ) {
						size_t k = i; bool nl = false;
						while( k < t.size() && t[k].trivia ) { if( t[k].text.find('\n') != std::string::npos ) nl = true; ++k; }
						if( nl || k >= t.size() || t[k].text == "}" || t[k].text == "{" ) break;
						while( i < k ) pk.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") );   // inter-value trivia
						pk.push_back( Leaf(NodeKind::Token, t[i++].text, "pvalue") );             // next value token
					}
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

	//! Value of ONE param node: all its pvalue tokens (+ their inter-value
	//! trivia) joined, i.e. everything from the first pvalue to the end of the
	//! Param -- so a multi-token value like `1 0 0` reads back as "1 0 0",
	//! matching the legacy ParamsList line's `pvalue` (split on the first space).
	std::string ParamNodeValue( const Node* p )
	{
		std::string v; bool inVal = false;
		if( p )
			for( const auto& k : p->kids ) {
				if( !inVal && k->kind == NodeKind::Token && k->role == "pvalue" ) inVal = true;
				if( inVal ) v += k->text;
			}
		return v;
	}

	//! Value of a param ROLE within a chunk (derive helper). On a repeated
	//! param, LAST occurrence wins -- matching the legacy parser's ParseStateBag
	//! overwrite semantics. (To preserve every occurrence in order -- needed for
	//! repeatable params -- iterate the Param nodes and read ParamNodeValue per
	//! node instead, as DeriveToJob does when building the ParamsList.)
	bool ParamValue( const Node* chunk, const std::string& role, std::string& out )
	{
		if( !chunk ) return false;
		bool found = false;
		for( const auto& p : chunk->kids )
			if( p->kind == NodeKind::Param && p->role == role ) { out = ParamNodeValue( p.get() ); found = true; }
		return found;
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

	//! Cost-gate instrumentation for the insert label-reflow: labels rewritten by
	//! ReflowWindow. A WINDOWED reflow writes O(window) << N; a regression to a
	//! global reflow would write N per gap-exhausting insert.
	unsigned long g_reflowLabelWrites = 0;

	//! Cost-gate instrumentation for param matching: old-param touches in
	//! MatchParamSlots. Hashed buckets touch each old param O(1) -> O(P); a
	//! regression to the nested-loop matcher would make it O(P^2).
	unsigned long g_paramMatchVisits = 0;

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

	// ---- IdSeq: positional persistent WBT of (NodeId, order-label) ----
	// Position-ordered (in-order = document order), so order-LABELS ascend in-order
	// too. The label is a stable per-item key (unlike position, which shifts), so a
	// durable NodeId resolves to its current position in O(log N): byId gives the
	// label, IdRankByLabel ranks it here. Labels are midpoints on insert, reflowed
	// on exhaustion / reparse.
	const std::int64_t LABEL_GAP = (std::int64_t)1 << 32;
	int IdSize( const IdSeqRef& s ) { return s ? s->count : 0; }
	IdSeqRef IdMk( IdSeqRef l, NodeId id, std::int64_t label, IdSeqRef r )
	{
		auto n = std::make_shared<IdNode>();
		n->left = std::move(l); n->right = std::move(r); n->id = id; n->label = label;
		n->count = 1 + IdSize(n->left) + IdSize(n->right);
		return n;
	}
	IdSeqRef IdBalance( IdSeqRef l, NodeId id, std::int64_t label, IdSeqRef r )
	{
		const int ln = IdSize(l), rn = IdSize(r);
		if( ln + rn > 1 ) {
			if( rn > WBT_DELTA * ln ) {
				if( IdSize(r->left) < WBT_GAMMA * IdSize(r->right) ) return IdMk( IdMk(l, id, label, r->left), r->id, r->label, r->right );
				const IdSeqRef& rl = r->left;
				return IdMk( IdMk(l, id, label, rl->left), rl->id, rl->label, IdMk(rl->right, r->id, r->label, r->right) );
			}
			if( ln > WBT_DELTA * rn ) {
				if( IdSize(l->right) < WBT_GAMMA * IdSize(l->left) ) return IdMk( l->left, l->id, l->label, IdMk(l->right, id, label, r) );
				const IdSeqRef& lr = l->right;
				return IdMk( IdMk(l->left, l->id, l->label, lr->left), lr->id, lr->label, IdMk(lr->right, id, label, r) );
			}
		}
		return IdMk( l, id, label, r );
	}
	IdSeqRef IdBuild( const std::vector<NodeId>& v, const std::vector<std::int64_t>& labels, int lo, int hi )
	{
		if( lo >= hi ) return IdSeqRef();
		const int mid = (lo + hi) / 2;
		return IdMk( IdBuild(v, labels, lo, mid), v[mid], labels[mid], IdBuild(v, labels, mid+1, hi) );
	}
	IdSeqRef IdInsertAt( const IdSeqRef& s, int index, NodeId id, std::int64_t label )
	{
		if( !s ) return IdMk( IdSeqRef(), id, label, IdSeqRef() );
		const int li = IdSize(s->left);
		if( index <= li ) return IdBalance( IdInsertAt(s->left, index, id, label), s->id, s->label, s->right );
		return IdBalance( s->left, s->id, s->label, IdInsertAt(s->right, index - li - 1, id, label) );
	}
	IdSeqRef IdEraseMin( const IdSeqRef& s, NodeId& idOut, std::int64_t& labelOut )
	{
		if( !s->left ) { idOut = s->id; labelOut = s->label; return s->right; }
		return IdBalance( IdEraseMin(s->left, idOut, labelOut), s->id, s->label, s->right );
	}
	IdSeqRef IdEraseAt( const IdSeqRef& s, int index )
	{
		if( !s ) return s;
		const int li = IdSize(s->left);
		if( index <  li ) return IdBalance( IdEraseAt(s->left, index), s->id, s->label, s->right );
		if( index >  li ) return IdBalance( s->left, s->id, s->label, IdEraseAt(s->right, index - li - 1) );
		if( !s->left )  return s->right;
		if( !s->right ) return s->left;
		NodeId sid; std::int64_t slabel; IdSeqRef nr = IdEraseMin( s->right, sid, slabel );
		return IdBalance( s->left, sid, slabel, nr );
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
	//! The order-label at in-order index (for computing an insert midpoint).
	std::int64_t IdLabelAt( const IdSeqRef& s, int index )
	{
		const IdNode* cur = s.get();
		while( cur ) {
			const int li = IdSize(cur->left);
			if( index <  li ) cur = cur->left.get();
			else if( index == li ) return cur->label;
			else { index -= li + 1; cur = cur->right.get(); }
		}
		return 0;
	}
	//! Position (in-order rank) of `label`, or -1 if absent. O(log N) -- labels
	//! ascend in-order, so this is a counted BST descent (the durable id -> position
	//! step that makes edit-by-NodeId O(log N)).
	int IdRankByLabel( const IdSeqRef& s, std::int64_t label, int& visits )
	{
		const IdNode* cur = s.get();
		int rank = 0;
		while( cur ) {
			++visits;
			if( label < cur->label ) cur = cur->left.get();
			else if( cur->label < label ) { rank += IdSize(cur->left) + 1; cur = cur->right.get(); }
			else return rank + IdSize(cur->left);
		}
		return -1;
	}
	void IdToVec( const IdSeqRef& s, std::vector<NodeId>& out )
	{
		if( !s ) return;
		IdToVec( s->left, out ); out.push_back( s->id ); IdToVec( s->right, out );
	}
	//! Set the order-label at in-order index (structure unchanged; path-copy O(log N)).
	IdSeqRef IdSetLabelAt( const IdSeqRef& s, int index, std::int64_t label )
	{
		if( !s ) return s;
		const int li = IdSize(s->left);
		if( index <  li ) return IdMk( IdSetLabelAt(s->left, index, label), s->id, s->label, s->right );
		if( index == li ) return IdMk( s->left, s->id, label, s->right );
		return IdMk( s->left, s->id, s->label, IdSetLabelAt(s->right, index - li - 1, label) );
	}

	// ---- NameMap: key-ordered persistent WBT (name-path -> list of NodeIds) ----
	// The value is a LIST so duplicate name-paths (a degenerate but representable
	// scene) don't corrupt the index: erase/rename of one occurrence removes only
	// that id, survivors stay findable. NameFind returns the first occurrence + the
	// count; DocFindByName uses the count to REFUSE an ambiguous (!=1) name.
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

	// ---- IdMap: key-ordered persistent WBT (NodeId -> {current green node, label}) ----
	// The reverse index: a durable NodeId resolves to the node it now labels in
	// O(log N). It also stores the item's order-label (0 for a param id, which has
	// no rope position) so NodeId -> position is O(log N) (label, then IdRankByLabel).
	int IdMapSize( const IdMapRef& s ) { return s ? s->count : 0; }
	IdMapRef IdMapMk( IdMapRef l, NodeId key, NodeRef val, std::int64_t label, IdMapRef r )
	{
		auto n = std::make_shared<IdMapNode>();
		n->left = std::move(l); n->right = std::move(r); n->key = key; n->val = std::move(val); n->label = label;
		n->count = 1 + IdMapSize(n->left) + IdMapSize(n->right);
		return n;
	}
	IdMapRef IdMapBalance( IdMapRef l, NodeId key, NodeRef val, std::int64_t label, IdMapRef r )
	{
		const int ln = IdMapSize(l), rn = IdMapSize(r);
		if( ln + rn > 1 ) {
			if( rn > WBT_DELTA * ln ) {
				if( IdMapSize(r->left) < WBT_GAMMA * IdMapSize(r->right) ) return IdMapMk( IdMapMk(l, key, std::move(val), label, r->left), r->key, r->val, r->label, r->right );
				const IdMapRef& rl = r->left;
				return IdMapMk( IdMapMk(l, key, std::move(val), label, rl->left), rl->key, rl->val, rl->label, IdMapMk(rl->right, r->key, r->val, r->label, r->right) );
			}
			if( ln > WBT_DELTA * rn ) {
				if( IdMapSize(l->right) < WBT_GAMMA * IdMapSize(l->left) ) return IdMapMk( l->left, l->key, l->val, l->label, IdMapMk(l->right, key, std::move(val), label, r) );
				const IdMapRef& lr = l->right;
				return IdMapMk( IdMapMk(l->left, l->key, l->val, l->label, lr->left), lr->key, lr->val, lr->label, IdMapMk(lr->right, key, std::move(val), label, r) );
			}
		}
		return IdMapMk( l, key, std::move(val), label, r );
	}
	IdMapRef IdMapSet( const IdMapRef& s, NodeId key, NodeRef val, std::int64_t label )   // set node + label
	{
		if( !s ) return IdMapMk( IdMapRef(), key, std::move(val), label, IdMapRef() );
		if( key < s->key ) return IdMapBalance( IdMapSet(s->left, key, std::move(val), label), s->key, s->val, s->label, s->right );
		if( s->key < key ) return IdMapBalance( s->left, s->key, s->val, s->label, IdMapSet(s->right, key, std::move(val), label) );
		return IdMapMk( s->left, s->key, std::move(val), label, s->right );
	}
	IdMapRef IdMapRepoint( const IdMapRef& s, NodeId key, NodeRef val )           // overwrite node, KEEP label
	{
		if( !s ) return s;
		if( key < s->key ) return IdMapBalance( IdMapRepoint(s->left, key, std::move(val)), s->key, s->val, s->label, s->right );
		if( s->key < key ) return IdMapBalance( s->left, s->key, s->val, s->label, IdMapRepoint(s->right, key, std::move(val)) );
		return IdMapMk( s->left, s->key, std::move(val), s->label, s->right );
	}
	IdMapRef IdMapSetLabel( const IdMapRef& s, NodeId key, std::int64_t label )           // overwrite label, KEEP node (reflow)
	{
		if( !s ) return s;
		if( key < s->key ) return IdMapBalance( IdMapSetLabel(s->left, key, label), s->key, s->val, s->label, s->right );
		if( s->key < key ) return IdMapBalance( s->left, s->key, s->val, s->label, IdMapSetLabel(s->right, key, label) );
		return IdMapMk( s->left, s->key, s->val, label, s->right );
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
	std::int64_t IdMapGetLabel( const IdMapRef& s, NodeId key, int& visits )
	{
		const IdMapNode* cur = s.get();
		while( cur ) {
			++visits;
			if( key < cur->key ) cur = cur->left.get();
			else if( cur->key < key ) cur = cur->right.get();
			else return cur->label;
		}
		return 0;
	}
	IdMapRef IdMapEraseMin( const IdMapRef& s, NodeId& kOut, NodeRef& vOut, std::int64_t& lOut )
	{
		if( !s->left ) { kOut = s->key; vOut = s->val; lOut = s->label; return s->right; }
		return IdMapBalance( IdMapEraseMin(s->left, kOut, vOut, lOut), s->key, s->val, s->label, s->right );
	}
	IdMapRef IdMapErase( const IdMapRef& s, NodeId key )
	{
		if( !s ) return s;
		if( key < s->key ) return IdMapBalance( IdMapErase(s->left, key), s->key, s->val, s->label, s->right );
		if( s->key < key ) return IdMapBalance( s->left, s->key, s->val, s->label, IdMapErase(s->right, key) );
		if( !s->left )  return s->right;
		if( !s->right ) return s->left;
		NodeId k; NodeRef v; std::int64_t lb; IdMapRef nr = IdMapEraseMin( s->right, k, v, lb );
		return IdMapBalance( s->left, k, std::move(v), lb, nr );
	}

	// ---- ParamMap: key-ordered persistent WBT ("<chunkId>\x1f<role>\x1f<occ>" -> NodeId) ----
	// Per-parameter-occurrence identity (D26/D36). Keyed by owning chunk's id +
	// the param role + occurrence index, so REPEATED same-role params each get a
	// distinct id; on edit, params are matched by CONTENT (MatchParamSlots), so a
	// value edit keeps the id and a sibling insert/remove never shifts ids.
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
	void ParamCollectIds( const ParamMapRef& s, std::vector<NodeId>& out )
	{
		if( !s ) return;
		ParamCollectIds( s->left, out ); out.push_back( s->id ); ParamCollectIds( s->right, out );
	}
	//! Param-occurrence key: (owning chunk id, role, occurrence index among same-
	//! role siblings) -- so REPEATED params (part / cp / value / time / shaderop)
	//! each get a distinct identity rather than overwriting by role.
	std::string ParamKey( NodeId chunkId, const std::string& role, int occ ) { return std::to_string( (std::int64_t)chunkId ) + "\x1f" + role + "\x1f" + std::to_string( occ ); }

	//! A chunk's Param children as (role, occurrence-index, node) slots (skips
	//! keyword/braces/trivia). Within-VALUE atoms are NOT given identity here --
	//! value-atom occurrences are RepeatGroup-era, like repeated-param VALUE nodes.
	struct ParamSlot { std::string role; int occ; NodeRef node; };
	std::vector<ParamSlot> ChunkParams( const NodeRef& chunk )
	{
		std::vector<ParamSlot> out;
		std::unordered_map<std::string, int> seen;
		if( chunk && chunk->kind == NodeKind::Chunk )
			for( const auto& k : chunk->kids )
				if( k->kind == NodeKind::Param ) { int occ = seen[k->role]++; out.push_back( { k->role, occ, k } ); }
		return out;
	}
	//! Mint FRESH param ids for `chunk`'s param occurrences (parse / insert path).
	void AddChunkParams( ParamMapRef& pids, IdMapRef& byId, NodeId chunkId, const NodeRef& chunk, NodeId& nextId )
	{
		for( auto& rp : ChunkParams(chunk) ) {
			NodeId pid = nextId++;
			pids = ParamSet( pids, ParamKey(chunkId, rp.role, rp.occ), pid );
			byId = IdMapSet( byId, pid, rp.node, 0 );
		}
	}
	//! Drop `oldChunk`'s param ids from both indices (erase path); push them to
	//! `inv` (if non-null) -- their durable bindings just died.
	void DropChunkParams( ParamMapRef& pids, IdMapRef& byId, NodeId chunkId, const NodeRef& oldChunk, std::vector<NodeId>* inv )
	{
		for( auto& rp : ChunkParams(oldChunk) ) {
			std::string key = ParamKey( chunkId, rp.role, rp.occ );
			NodeId pid = ParamGet( pids, key );
			if( pid ) { byId = IdMapErase( byId, pid ); pids = ParamErase( pids, key ); if( inv ) inv->push_back( pid ); }
		}
	}
	//! Match a chunk's NEW param occurrences to its OLD ones by CONTENT, not by
	//! occurrence index -- so a repeated param (part/cp/value/time/shaderop) keeps
	//! its id when an earlier sibling is inserted/removed, instead of having ids
	//! shift onto unrelated values. O(P) via hashed buckets (each param serialized
	//! ONCE), mirroring the top-level reparse matcher:
	//!   1. full-content GROUPS with EQUAL multiset count -> carry in order (an
	//!      unchanged group keeps ids; a count-changed group of BYTE-IDENTICAL
	//!      repeats is genuinely AMBIGUOUS and is NOT consumed here -- it falls to
	//!      pass 3 / invalidate, never a per-occurrence guess);
	//!   2. unique role among the remainder -> a unique-role value edit keeps its id;
	//!   3. the rest: mint fresh / invalidate (D9/D15: invalidate-don't-remap).
	struct OldParamSlot { std::string role; NodeRef node; NodeId id; };
	void MatchParamSlots( const std::vector<OldParamSlot>& oldSlots, const std::vector<ParamSlot>& newSlots,
	                      NodeId& nextId, std::vector<NodeId>& newIds, std::vector<NodeId>& invalidatedIds )
	{
		const int O = (int)oldSlots.size(), M = (int)newSlots.size();
		newIds.assign( M, 0 );
		std::vector<bool> oldUsed( O, false );
		// content key = role + bytes, serialized ONCE per slot (no nested re-serialize)
		std::unordered_map<std::string, std::vector<int>> oldByFull, newByFull;
		for( int i = 0; i < O; ++i ) { std::string b; Serialize( oldSlots[i].node, b ); oldByFull[ oldSlots[i].role + "\x1f" + b ].push_back( i ); ++g_paramMatchVisits; }
		for( int j = 0; j < M; ++j ) { std::string b; Serialize( newSlots[j].node, b ); newByFull[ newSlots[j].role + "\x1f" + b ].push_back( j ); }
		// pass 1: full-content groups with EQUAL count -> carry in document order
		for( auto& kv : newByFull ) {
			auto it = oldByFull.find( kv.first );
			if( it == oldByFull.end() || it->second.size() != kv.second.size() ) continue;   // changed / ambiguous group -> defer
			for( size_t k = 0; k < kv.second.size(); ++k ) { int oi = it->second[k]; oldUsed[oi] = true; newIds[ kv.second[k] ] = oldSlots[oi].id; }
		}
		// pass 2: unique role among the remainder (a unique-role value edit keeps its id)
		std::unordered_map<std::string,int> oldRem, newRem, oldRemIdx;
		for( int i = 0; i < O; ++i ) if( !oldUsed[i] ) { oldRem[oldSlots[i].role]++; oldRemIdx[oldSlots[i].role] = i; ++g_paramMatchVisits; }
		for( int j = 0; j < M; ++j ) if( newIds[j] == 0 ) newRem[newSlots[j].role]++;
		for( int j = 0; j < M; ++j ) if( newIds[j] == 0 ) {
			const std::string& r = newSlots[j].role;
			if( newRem[r] == 1 && oldRem.count(r) == 1 && oldRem[r] == 1 ) { int i = oldRemIdx[r]; oldUsed[i] = true; newIds[j] = oldSlots[i].id; }
		}
		for( int j = 0; j < M; ++j ) if( newIds[j] == 0 ) newIds[j] = nextId++;            // pass 3: mint
		for( int i = 0; i < O; ++i ) if( !oldUsed[i] ) invalidatedIds.push_back( oldSlots[i].id );   // + invalidate
	}
	//! Re-point chunk `chunkId`'s param ids at the NEW chunk's param nodes by
	//! content (see MatchParamSlots); occurrence indices are recomputed, never used
	//! as identity. `inv` (if non-null) receives invalidated param ids.
	void ReindexChunkParams( ParamMapRef& pids, IdMapRef& byId, NodeId chunkId, const NodeRef& oldChunk, const NodeRef& newChunk, NodeId& nextId, std::vector<NodeId>* inv )
	{
		auto oldP = ChunkParams( oldChunk );
		std::vector<OldParamSlot> oldSlots;
		for( auto& rp : oldP ) oldSlots.push_back( { rp.role, rp.node, ParamGet( pids, ParamKey(chunkId, rp.role, rp.occ) ) } );
		auto newSlots = ChunkParams( newChunk );
		std::vector<NodeId> newIds, invd;
		MatchParamSlots( oldSlots, newSlots, nextId, newIds, invd );
		for( auto& rp : oldP ) pids = ParamErase( pids, ParamKey(chunkId, rp.role, rp.occ) );   // occurrences may have shifted
		for( NodeId id : invd ) byId = IdMapErase( byId, id );
		for( int j = 0; j < (int)newSlots.size(); ++j ) {
			pids = ParamSet( pids, ParamKey(chunkId, newSlots[j].role, newSlots[j].occ), newIds[j] );
			byId = IdMapSet( byId, newIds[j], newSlots[j].node, 0 );
		}
		if( inv ) for( NodeId id : invd ) inv->push_back( id );
	}
	//! Make room for an insert at position `p` by reflowing a WINDOW of order-labels
	//! around it -- the smallest enclosing run [a,b] whose label-span has spare room
	//! -- not always the whole document. NodeIds are unchanged (durable); only the
	//! position-order labels of the window move, leaving a gap at `p`.
	//! COST (honest worst case): this is a fixed-density (1/4) + radius-doubling
	//! window, which is TINY in the common (sparse) case -- measured window 2 -- so
	//! it improves the COMMON case markedly over a global reflow. But it does NOT
	//! achieve list-labeling's amortized-O(log N) relabels (that needs LEVEL-SCALED
	//! density thresholds, not a fixed one): an adversarial DENSE pattern (repeated
	//! inserts packing a prefix) can grow the window to Theta(N), making that insert
	//! Theta(N log N) worst-case (~Theta(log^3 N) amortized on the dense pile). So
	//! the reflow is NOT an asymptotic improvement over global -- only a common-case
	//! one -- and is the disclosed v1 fallback (D23 sanctions an O(N) v1 identity
	//! cost). Bender's two-level / level-scaled order-maintenance (window -> O(1)
	//! amortized, restoring O(log N) inserts) is the documented refinement, not yet
	//! landed. The [reflow] gate drives the dense adversary and asserts correctness.
	Document ReflowWindow( const Document& doc, int p )
	{
		const int N = IdSize( doc.idseq );
		int radius = 1;
		for( ;; ) {
			int a = p - radius; if( a < 0 ) a = 0;
			int b = p - 1 + radius; if( b > N - 1 ) b = N - 1;
			const int count = b - a + 1;
			const std::int64_t lower = ( a > 0 ) ? IdLabelAt( doc.idseq, a - 1 ) : 0;
			std::int64_t upper;
			if( b < N - 1 ) {
				upper = IdLabelAt( doc.idseq, b + 1 );
			} else {
				// synthetic tail upper; SATURATE to avoid int64 overflow (only
				// reachable at ~1e9 items -- tens of GB of tree -- but keeps the
				// arithmetic UB-free; redistribution below still leaves step >= 2).
				const std::int64_t CEIL = (std::int64_t)1 << 62;
				const std::int64_t need = (std::int64_t)( count + 2 );
				upper = ( lower < CEIL && need < ( CEIL - lower ) / LABEL_GAP ) ? lower + need * LABEL_GAP : CEIL;
			}
			const std::int64_t avail = upper - lower;
			const bool whole = ( a == 0 && b == N - 1 );
			if( avail >= (std::int64_t)4 * ( count + 1 ) || whole ) {
				std::int64_t step = avail / ( count + 1 );
				if( step < 2 ) step = 2;               // whole-doc fallback (unreachable with a 2^62 space)
				Document d = doc;
				for( int k = 0; k < count; ++k ) {
					const int idx = a + k;
					int dummy = 0; NodeId id = IdAt( doc.idseq, idx, dummy );   // positions stable under label-only edits
					const std::int64_t lab = lower + (std::int64_t)( k + 1 ) * step;
					d.idseq = IdSetLabelAt( d.idseq, idx, lab );
					d.byId  = IdMapSetLabel( d.byId, id, lab );
					++g_reflowLabelWrites;
				}
				return d;
			}
			radius *= 2;
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
	std::vector<NodeId> ids; std::vector<std::int64_t> labels;
	for( int k = 0; k < (int)items.size(); ++k ) {
		NodeId id = (NodeId)( k + 1 );
		std::int64_t label = (std::int64_t)( k + 1 ) * LABEL_GAP;       // evenly-spaced order labels
		ids.push_back( id ); labels.push_back( label );
		d.byId = IdMapSet( d.byId, id, items[k], label );
		std::string np = ChunkNamePath( items[k] );
		if( !np.empty() ) d.byName = NameInsert( d.byName, np, id );
	}
	d.idseq  = IdBuild( ids, labels, 0, (int)ids.size() );
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

//! The LIVE chunk-parser registry (item 5), shared by DeriveToJob and
//! TraceReferences: one instance of every chunk parser the grammar supports,
//! kept alive for the process (so each parser's descriptor + Finalize stay
//! valid), keyed by the registry's dispatch keyword (which carries aliases).
//! Built once; the parse/derive context is single-threaded and the function-
//! static init is thread-safe.
static const std::map<std::string, const IAsciiChunkParser*>& DescriptorRegistry()
{
	static const std::vector<ChunkParserEntry> entries = CreateAllChunkParsers();
	static const std::map<std::string, const IAsciiChunkParser*> reg = [&]{
		std::map<std::string, const IAsciiChunkParser*> m;
		for( const auto& e : entries ) m[e.keyword] = e.parser.get();
		return m;
	}();
	return reg;
}

int DeriveToJob( const Document& doc, IJob& pJob, std::vector<std::string>* diagnostics )
{
	std::vector<std::string> local;
	std::vector<std::string>& diags = diagnostics ? *diagnostics : local;
	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );

	// Reset the chunk parsers' cross-chunk parse state FIRST, exactly as the
	// legacy ParseAndLoadScene does at the start of every parse. Some Finalize()s
	// read/write file-scope caches within one scene (notably the
	// uniformcolor_painter colour cache that translucent_material's energy-
	// conservation check reads); without this, deriving scene A then scene B
	// would leak A's state into B (the redesign runs DeriveToJob repeatedly on
	// every edit), giving B a Job a fresh parse of B would not.
	ClearChunkParserState();

	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();

	// PASS 1 -- validate EVERY chunk through the live descriptor registry, the
	// SAME validation the legacy parser runs (DispatchChunkParameters: rejects a
	// no-space line, an undeclared parameter name, or a non-finite/non-numeric
	// numeric value -- see its doc in IAsciiChunkParser.h). Collect each
	// chunk's populated bag; if ANY chunk fails, apply NOTHING (refuse-all).
	struct Pending { const IAsciiChunkParser* parser; ParseStateBag bag; std::string keyword; };
	std::vector<Pending> pending;
	pending.reserve( items.size() );
	for( const auto& c : items ) {
		if( c->kind != NodeKind::Chunk ) continue;   // header strays / trivia: not derivable chunks
		const std::string& kw = c->role;
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( kw );
		if( it == registry.end() ) { diags.push_back( "unknown chunk type '" + kw + "'" ); continue; }
		const IAsciiChunkParser* parser = it->second;
		// A value-less parameter line (a flattened bare pname) is malformed; its
		// tokens never reach the ParamsList below, so flag them explicitly. (The
		// legacy DispatchChunkParameters likewise rejects such a line.)
		for( const auto& kid : c->kids )
			if( kid->kind == NodeKind::Token && kid->role == "pname" )
				diags.push_back( kw + ": value-less parameter '" + kid->text + "'" );
		// Build the ParamsList in document order, ONE line per param occurrence
		// (so repeatable params survive), NORMALISED exactly as the legacy parser
		// normalises a chunk-body line: it runs each line through
		// AsciiCommandParser::TokenizeString (splits on runs of " \t\r", dropping
		// all interior whitespace) and rejoins with make_string_from_tokens(" ")
		// before feeding DispatchChunkParameters. So we join this param's name +
		// value TOKENS with single spaces, dropping the original inter-token
		// trivia -- collapsing tabs / multi-space / column alignment identically.
		// (Feeding the verbatim source bytes instead would drift on string-valued
		// params: e.g. `geometry  ball` would keep a leading space in the value
		// and silently fail the reference lookup.)
		IAsciiChunkParser::ParamsList plist;
		for( const auto& kid : c->kids )
			if( kid->kind == NodeKind::Param ) {
				std::string line;
				for( const auto& tk : kid->kids )
					if( tk->kind == NodeKind::Token ) {          // pname + pvalue tokens; skip trivia
						if( !line.empty() ) line += ' ';
						line += tk->text;
					}
				plist.push_back( String( line.c_str() ) );
			}
		ParseStateBag bag( &parser->Describe() );
		if( !DispatchChunkParameters( parser->Describe(), bag, plist ) ) {
			diags.push_back( kw + ": invalid parameter(s) (see log)" );
			continue;
		}
		pending.push_back( Pending{ parser, std::move(bag), kw } );
	}
	if( !diags.empty() ) return 0;   // refuse-all: a malformed scene applies NOTHING

	// PASS 2 -- apply via the SAME Finalize the legacy parser calls, so the CST
	// path and the legacy path build an identical Job for a validation-clean
	// CANONICAL registry scene (see DeriveToJob's doc for the exact scope).
	// A Finalize failure is an APPLY-TIME error that PASS-1
	// validation cannot detect (e.g. a reference to a not-yet/never-defined
	// chunk): match the legacy parser's abort-on-first-failure -- surface a
	// diagnostic and STOP (do not silently swallow it, and do not keep applying
	// later chunks, which would diverge from legacy). Chunks before the failure
	// stay applied, exactly as the legacy parser leaves them; full apply-atomicity
	// (rollback of the prior chunks) is later Facet-2 work.
	int count = 0;
	for( Pending& p : pending ) {
		if( p.parser->Finalize( p.bag, pJob ) ) { ++count; continue; }
		diags.push_back( p.keyword + ": apply failed (e.g. unresolved reference); see log" );
		break;
	}
	return count;
}

std::vector<ReferenceUse> TraceReferences( const Document& doc, std::vector<std::string>* diagnostics )
{
	std::vector<std::string> local;
	std::vector<std::string>& diags = diagnostics ? *diagnostics : local;
	std::vector<ReferenceUse> uses;

	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();
	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );

	// PASS A -- index every chunk's definition by (category, name) -> NodeId: the
	// descriptor-derived category namespace the named managers key on (§2.5), so a
	// reference resolves to the chunk of that name in the referenced CATEGORY,
	// regardless of which keyword defined it (sphere_geometry / box_geometry both
	// answer to category Geometry). On a duplicate (category,name) keep the FIRST,
	// matching the manager's first-wins AddItem (the derive layer rejects dups).
	std::map<std::pair<int,std::string>, NodeId> defs;
	for( size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( c->kind != NodeKind::Chunk ) continue;
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
		if( it == registry.end() ) continue;                       // unknown chunk: not a target
		std::string name;
		if( !ParamValue( c.get(), "name", name ) || name.empty() ) continue;   // unnamed: not referenceable
		const std::pair<int,std::string> key( (int)it->second->Describe().category, name );
		if( defs.find( key ) == defs.end() ) defs[key] = DocNodeIdAt( doc, (int)i );
	}

	// PASS B -- for every chunk, resolve each EXPLICIT reference: a param actually
	// present whose descriptor kind is Reference, with a non-"none" value. Record
	// a ReferenceUse{ referring-param NodeId, target chunk NodeId } per resolved
	// reference; an unresolvable one is a dangling reference (diagnostic), never a
	// silent edge.
	for( size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( c->kind != NodeKind::Chunk ) continue;
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
		if( it == registry.end() ) continue;
		const ChunkDescriptor& desc = it->second->Describe();
		const NodeId chunkId = DocNodeIdAt( doc, (int)i );
		std::map<std::string,int> occ;   // per-role occurrence index, for DocParamId
		for( const auto& kid : c->kids ) {
			if( kid->kind != NodeKind::Param ) continue;
			const std::string role = kid->role;
			const int thisOcc = occ[role]++;
			const ParameterDescriptor* pd = 0;
			for( const auto& p : desc.parameters ) if( p.name == role ) { pd = &p; break; }
			if( !pd || pd->kind != ValueKind::Reference ) continue;
			const std::string val = ParamNodeValue( kid.get() );
			if( val.empty() || val == "none" ) continue;           // explicit-none / empty: not an edge
			NodeId target = 0;
			for( ChunkCategory rc : pd->referenceCategories ) {
				std::map<std::pair<int,std::string>, NodeId>::const_iterator d = defs.find( std::pair<int,std::string>( (int)rc, val ) );
				if( d != defs.end() ) { target = d->second; break; }
			}
			if( target == 0 ) {
				diags.push_back( c->role + "." + role + " -> '" + val + "': unresolved reference" );
				continue;
			}
			uses.push_back( ReferenceUse{ DocParamId( doc, chunkId, role, thisOcc ), target } );
		}
	}
	return uses;
}

int    DocItemCount  ( const Document& doc ) { return SeqCount( doc.items ); }
size_t DocByteWidth  ( const Document& doc ) { return SeqBytes( doc.items ); }
int    DocNewlineCount( const Document& doc ) { return SeqNl( doc.items ); }
unsigned long DebugItemStatWalks() { return g_itemStatWalks; }
unsigned long DebugReparseOldVisits() { return g_reparseOldVisits; }
unsigned long DebugReflowLabelWrites() { return g_reflowLabelWrites; }
unsigned long DebugParamMatchVisits() { return g_paramMatchVisits; }

int DocItemAtByteOffset( const Document& doc, size_t offset, NodeRef* outItem, size_t* outStart, int* visits )
{
	NodeRef item; size_t start = 0; int v = 0;
	int idx = SeqAtOffset( doc.items, offset, 0, 0, item, start, v );
	if( visits ) *visits = v;
	if( idx >= 0 ) { if( outItem ) *outItem = item; if( outStart ) *outStart = start; }
	return idx;
}

Document DocReplaceItem( const Document& doc, int index, NodeRef newItem, int* visits, std::vector<NodeId>* invalidated )
{
	if( visits ) *visits = 0;
	if( invalidated ) invalidated->clear();
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
	d.byId  = IdMapRepoint( d.byId, id, newItem );               // reverse index -> the new node (label unchanged)
	d.items = SeqReplace( doc.items, index, std::move(newItem), v );
	if( visits ) *visits = v;
	if( oldName != newName ) {
		if( !oldName.empty() ) d.byName = NameErase( d.byName, oldName, id );
		if( !newName.empty() ) d.byName = NameInsert( d.byName, newName, id );
	}
	ReindexChunkParams( d.paramIds, d.byId, id, oldChunk, newChunk, d.nextId, invalidated );
	return d;
}

Document DocInsertItem( const Document& doc, int index, NodeRef newItem, int* visits )
{
	if( visits ) *visits = 0;
	if( !newItem ) return doc;                                     // non-null contract: refuse
	int n = SeqCount( doc.items );
	if( index < 0 ) index = 0;
	if( index > n ) index = n;
	// order label for the new item = midpoint of its neighbours; reflow a WINDOW of
	// labels (not the whole doc) if the gap is exhausted / would overflow on append.
	Document src = doc;
	bool reflow = false;
	std::int64_t before = ( index > 0 ) ? IdLabelAt( src.idseq, index - 1 ) : 0;
	std::int64_t after;
	if( index < n ) { after = IdLabelAt( src.idseq, index ); if( after - before < 2 ) reflow = true; }
	else            { if( before > ( (std::int64_t)1 << 62 ) ) reflow = true; after = before + 2 * LABEL_GAP; }
	if( reflow ) {
		src = ReflowWindow( doc, index );               // windowed, not a global O(N) reflow
		before = ( index > 0 ) ? IdLabelAt( src.idseq, index - 1 ) : 0;
		after  = ( index < n ) ? IdLabelAt( src.idseq, index )     : before + 2 * LABEL_GAP;
	}
	const std::int64_t label = before + ( after - before ) / 2;
	const NodeId id = src.nextId;                                  // fresh identity
	const std::string np = ChunkNamePath( newItem );
	const NodeRef newChunk = newItem;                             // handle before the move
	int v = 0;
	Document d = src;
	d.byId   = IdMapSet( d.byId, id, newItem, label );                  // reverse index (node + label)
	d.items  = SeqInsertAt( src.items, index, std::move(newItem), v );   // O(log N) WBT
	d.idseq  = IdInsertAt( src.idseq, index, id, label );               // O(log N) lockstep splice
	if( !np.empty() ) d.byName = NameInsert( d.byName, np, id );
	d.nextId = src.nextId + 1;
	AddChunkParams( d.paramIds, d.byId, id, newChunk, d.nextId );        // param occurrence ids
	if( visits ) *visits = v;
	return d;
}

Document DocEraseItem( const Document& doc, int index, int* visits, std::vector<NodeId>* invalidated )
{
	if( visits ) *visits = 0;
	if( invalidated ) invalidated->clear();
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
	DropChunkParams( d.paramIds, d.byId, eid, oldChunk, invalidated );   // erased param ids -> invalidated
	if( invalidated ) invalidated->push_back( eid );              // ... plus the chunk's own id
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

int DocIndexOfNodeId( const Document& doc, NodeId id, NodeRef* outItem, int* visits )
{
	int gv = 0; const std::int64_t label = IdMapGetLabel( doc.byId, id, gv );   // O(log N) id -> order-label
	int rv = 0; const int idx = ( label == 0 ) ? -1 : IdRankByLabel( doc.idseq, label, rv );   // O(log N) label -> position (0 = not a top-level item)
	if( visits ) *visits = gv + rv;
	if( outItem ) *outItem = ( idx >= 0 ) ? SeqItemAt( doc.items, idx ) : NodeRef();
	return idx;
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
	// stable param NodeId (keyed by the param's occurrence index among same-role
	// siblings, so repeated params resolve to distinct ids).
	const size_t want = offset - start;
	size_t acc = 0;
	std::unordered_map<std::string, int> seen;
	for( const auto& k : item->kids ) {
		size_t kb = 0; int kn = 0; NodeStats( k, kb, kn );
		if( want < acc + kb ) {
			if( k->kind != NodeKind::Param ) return NodeRef();
			if( outParamId ) *outParamId = ParamGet( doc.paramIds, ParamKey(chunkId, k->role, seen[k->role]) );
			return k;
		}
		if( k->kind == NodeKind::Param ) seen[k->role]++;   // count same-role params BEFORE the target
		acc += kb;
	}
	return NodeRef();
}

NodeId DocParamId( const Document& doc, NodeId chunkId, const std::string& role, int occ )
{
	return ParamGet( doc.paramIds, ParamKey(chunkId, role, occ) );
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

	// PASS 1 -- FULL content. For CHUNK groups, carry ONLY when the multiset is
	// UNCHANGED (old count == new count): a count-changed group of byte-identical
	// chunks is genuinely ambiguous, so greedily pairing them would SWAP identities
	// (the surviving twin of an edited pair must be invalidated, not re-bound).
	// For TRIVIA/STRAY groups there is no id-swap hazard: a ref can only rebind
	// WITHIN a byte-identical group (the fullOf key), so the bytes -- hence the
	// meaning, even for a comment -- are preserved. Carry GREEDILY in document
	// order even when the count changed -- otherwise a pure append would spuriously
	// invalidate every existing "\n" separator id.
	{
		std::unordered_map<std::string, std::vector<int>> oldF, newF;
		for( int i = 0; i < O; ++i ) { oldF[ fullOf(oldItems[i]) ].push_back( i ); ++g_reparseOldVisits; }
		for( int j = 0; j < M; ++j )   newF[ fullOf(newItems[j]) ].push_back( j );
		for( auto& kv : newF ) {
			auto oit = oldF.find( kv.first );
			if( oit == oldF.end() ) continue;
			const bool isChunk = ( newItems[ kv.second[0] ]->kind == NodeKind::Chunk );
			if( isChunk && oit->second.size() != kv.second.size() ) continue;   // ambiguous chunk group -> defer
			const size_t pairs = oit->second.size() < kv.second.size() ? oit->second.size() : kv.second.size();
			for( size_t k = 0; k < pairs; ++k ) { int oi = oit->second[k]; oldUsed[oi] = true; carried[ kv.second[k] ] = oldIds[oi]; }
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
	std::vector<std::int64_t> labels; labels.reserve( M );
	for( int j = 0; j < M; ++j ) labels.push_back( (std::int64_t)( j + 1 ) * LABEL_GAP );   // fresh evenly-spaced labels by new position
	d.idseq    = IdBuild( carried, labels, 0, M );
	d.byName   = NameMapRef();
	d.byId     = IdMapRef();
	d.paramIds = ParamMapRef();
	std::vector<NodeId> oldParamIds; ParamCollectIds( oldDoc.paramIds, oldParamIds );   // for P1-C invalidation
	std::unordered_set<NodeId> reusedParam;
	std::unordered_map<NodeId, NodeRef> oldById;                                        // old chunk id -> old node
	for( int i = 0; i < O; ++i ) oldById[ oldIds[i] ] = oldItems[i];
	NodeId pnext = next;                               // mint fresh ids (chunk + param) from here
	for( int j = 0; j < M; ++j ) {
		d.byId = IdMapSet( d.byId, carried[j], newItems[j], labels[j] );
		std::string np = ChunkNamePath( newItems[j] );
		if( !np.empty() ) d.byName = NameInsert( d.byName, np, carried[j] );
		// params: content-match the carried chunk's NEW params to its OLD params (by
		// content, NOT occurrence -> a repeated param keeps lineage across a
		// sibling insert/remove); a fresh chunk (no old) mints all fresh.
		std::vector<OldParamSlot> oldSlots;
		auto oit = oldById.find( carried[j] );
		if( oit != oldById.end() )
			for( auto& rp : ChunkParams( oit->second ) )
				oldSlots.push_back( { rp.role, rp.node, ParamGet( oldDoc.paramIds, ParamKey(carried[j], rp.role, rp.occ) ) } );
		auto newSlots = ChunkParams( newItems[j] );
		std::vector<NodeId> pIds, pInvd;
		MatchParamSlots( oldSlots, newSlots, pnext, pIds, pInvd );
		for( int k = 0; k < (int)newSlots.size(); ++k ) {
			if( pIds[k] < next ) reusedParam.insert( pIds[k] );   // an old (carried) param id (fresh ids are >= next)
			d.paramIds = ParamSet( d.paramIds, ParamKey(carried[j], newSlots[k].role, newSlots[k].occ), pIds[k] );
			d.byId = IdMapSet( d.byId, pIds[k], newSlots[k].node, 0 );
		}
	}
	// P1-C: any old param id NOT carried is INVALIDATED -- a removed chunk, a
	// dropped param, or an ambiguous repeated-param value edit -- so widget /
	// diagnostic / ReferenceUse bindings learn their durable ref died.
	if( invalidated ) for( NodeId pid : oldParamIds ) if( !reusedParam.count(pid) ) invalidated->push_back( pid );
	NodeId mx = pnext;                                 // nextId strictly above every live id
	for( NodeId id : carried ) if( id + 1 > mx ) mx = id + 1;
	d.nextId = mx;
	return d;
}

} }
