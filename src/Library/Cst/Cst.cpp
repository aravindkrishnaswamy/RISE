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
//  normalised exactly as the legacy parser normalised it -- so, while the
//  legacy parser still existed, the CST and legacy paths built an identical
//  Job for the canonical scenes the CST was fed.  The legacy parser was
//  deleted in the Model-B P5 retirement; see Cst.h's header and DeriveToJob's
//  doc for the retired-oracle record and today's single-path failure boundary.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <atomic>
#include "Cst.h"
#include "../Interfaces/IJob.h"
#include "../Interfaces/IJobPriv.h"      // GetObjects() (manager access for the slice-3 stable-object apply)
#include "../Interfaces/IObjectManager.h" // IObjectPriv getBoundingBox / GetMaterial, spatial-structure generation
#include "../Managers/GenericManager.h"  // D35 record-during-derive sinks (g_cstProduction/ResolutionSink)
#include "../Objects/CSGObject.h"         // workstream #3c: detect a CSG operand-reference change (GetOperandA/B) to refuse it
#include "../Parsers/ChunkParserRegistry.h"   // CreateAllChunkParsers (the LIVE registry)
#include "../Parsers/IAsciiChunkParser.h"     // IAsciiChunkParser, DispatchChunkParameters
#include "../Painters/ExpressionEval.h"      // ExpressionProgram (expr(...) derive-time eval, #5 slice 2)

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace RISE;
using namespace RISE::Cst;

namespace
{
	//! Split a string into whitespace-separated tokens (" \t\r\n"), matching the
	//! legacy tokenizer Finalize re-parses composite values with. Shared by the
	//! reference tracer (tuple-Reference tokens) and the within-chunk value editor.
	std::vector<std::string> SplitWs( const std::string& s )
	{
		std::vector<std::string> toks; const size_t n = s.size(); size_t i = 0;
		while( i < n ) {
			while( i < n && ( s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n' ) ) ++i;
			const size_t st = i;
			while( i < n && !( s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n' ) ) ++i;
			if( i > st ) toks.push_back( s.substr( st, i - st ) );
		}
		return toks;
	}

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

	//! Does this top-level item INSTANCE another node -- i.e. is it a `standard_object`
	//! carrying a live `source`?  This is EXACTLY DeriveToJob PASS-2's `isSrc` trigger
	//! (same role, same empty/`none` sentinels), and PASS-2 calls this function rather
	//! than spelling the test a second time, so `Document::sourceInstanceCount` -- and
	//! therefore DeriveToJobIncremental's document-wide refusal -- cannot come to a
	//! different answer than the expansion it exists to gate.
	//!
	//! ROLE-SCOPED ON PURPOSE.  `channel_painter` also declares a `source` (a Reference
	//! to a painter, nothing to do with instancing), so a role-blind "any chunk with a
	//! `source`" predicate would make every scene that uses one pay a full re-derive on
	//! every gizmo drag for a feature it does not use.
	bool ChunkIsSourceInstance( const Node* c )
	{
		if( !c || c->kind != NodeKind::Chunk || c->role != "standard_object" ) return false;
		std::string src;
		return ParamValue( c, "source", src ) && !src.empty() && src != "none";
	}

	//----------------------------------------------------------------------
	// Item 3 -- persistent balanced sequence of top-level items (the D16 rope).
	//----------------------------------------------------------------------

	//! COST-GATE COUNTERS.  Diagnostic only -- nothing branches on them; the
	//! gate tests read them single-threaded and assert a bound.  ATOMIC
	//! (relaxed) because the parse/edit entry points are NOT single-threaded:
	//! the agent surface validates a candidate (ParseToCst) on one thread
	//! while another commits an edit (DocReplaceItem) -- ThreadSanitizer
	//! flags the plain ++ as a data race there.  Relaxed is the right
	//! ordering: these count events, they order nothing.
	//!
	//! Bumped once per per-ITEM stat walk (MkSeqFresh). A correct path-copy
	//! edit walks exactly one item; a hidden re-scan of the unchanged spine
	//! would bump it. Read by the cost gate via Cst::DebugItemStatWalks().
	std::atomic<unsigned long> g_itemStatWalks( 0 );

	//! Cost-gate instrumentation for DocReparse: old-item touches during matching.
	//! The 4-pass hashed matcher touches each old item O(1) times -> grows O(M+N);
	//! a regression to a nested-loop matcher would make it O(M*N).
	std::atomic<unsigned long> g_reparseOldVisits( 0 );

	//! Cost-gate instrumentation for the insert label-reflow: labels rewritten by
	//! ReflowWindow. A WINDOWED reflow writes O(window) << N; a regression to a
	//! global reflow would write N per gap-exhausting insert.
	std::atomic<unsigned long> g_reflowLabelWrites( 0 );

	//! Cost-gate instrumentation for param matching: old-param touches in
	//! MatchParamSlots. Hashed buckets touch each old param O(1) -> O(P); a
	//! regression to the nested-loop matcher would make it O(P^2).
	std::atomic<unsigned long> g_paramMatchVisits( 0 );

	//! #4b cost gate: ComputeChunkRefs evaluations.  A from-scratch / rebuild does N (one per
	//! chunk); an incremental reference/cp edit does exactly 1.  Read by Cst::DebugChunkRefsComputed().
	std::atomic<unsigned long> g_chunkRefsComputed( 0 );

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
		g_itemStatWalks.fetch_add( 1, std::memory_order_relaxed );   // one per-item stat walk (cost-gate instrumentation)
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
	//! Byte offset where in-order index `index` STARTS (the exact reverse of
	//! SeqAtOffset: index -> offset, instead of offset -> index). Iterative
	//! descent mirroring SeqItemAt, accumulating the left-subtree/left-
	//! sibling byte widths from the cached aggregates -- O(log N), no item
	//! re-walk. `++visits` per node descended, same convention as
	//! SeqAtOffset/IdRankByLabel. Returns (size_t)-1 if `index` is out of
	//! range (caller is expected to have range-checked already; this is the
	//! defensive fallback).
	size_t SeqOffsetAt( const SeqRef& s, int index, int& visits )
	{
		const SeqNode* cur = s.get();
		size_t base = 0;
		while( cur ) {
			++visits;
			const int li = SeqCount( cur->left );
			if( index <  li ) cur = cur->left.get();
			else if( index == li ) return base + SeqBytes( cur->left );
			else { base += SeqBytes( cur->left ) + cur->itemBytes; index -= li + 1; cur = cur->right.get(); }
		}
		return (size_t)-1;
	}

	//----------------------------------------------------------------------
	// Item 4 -- identity side-map + name-path index (both persistent, separate
	// from the green/seq node, weight-balanced like the item sequence).
	//----------------------------------------------------------------------

	// ("keyword/name" addressing-key construction lives in the EXPORTED
	// RISE::Cst::ChunkNamePath below -- moved out of this anonymous
	// namespace for Model-B F5 S2 so Job's insert_chunk duplicate check
	// reuses the one canonical construction.)

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
		for( int i = 0; i < O; ++i ) { std::string b; Serialize( oldSlots[i].node, b ); oldByFull[ oldSlots[i].role + "\x1f" + b ].push_back( i ); g_paramMatchVisits.fetch_add( 1, std::memory_order_relaxed ); }
		for( int j = 0; j < M; ++j ) { std::string b; Serialize( newSlots[j].node, b ); newByFull[ newSlots[j].role + "\x1f" + b ].push_back( j ); }
		// pass 1: full-content groups with EQUAL count -> carry in document order
		for( auto& kv : newByFull ) {
			auto it = oldByFull.find( kv.first );
			if( it == oldByFull.end() || it->second.size() != kv.second.size() ) continue;   // changed / ambiguous group -> defer
			for( size_t k = 0; k < kv.second.size(); ++k ) { int oi = it->second[k]; oldUsed[oi] = true; newIds[ kv.second[k] ] = oldSlots[oi].id; }
		}
		// pass 2: unique role among the remainder (a unique-role value edit keeps its id)
		std::unordered_map<std::string,int> oldRem, newRem, oldRemIdx;
		for( int i = 0; i < O; ++i ) if( !oldUsed[i] ) { oldRem[oldSlots[i].role]++; oldRemIdx[oldSlots[i].role] = i; g_paramMatchVisits.fetch_add( 1, std::memory_order_relaxed ); }
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
					g_reflowLabelWrites.fetch_add( 1, std::memory_order_relaxed );
				}
				return d;
			}
			radius *= 2;
		}
	}
}

namespace RISE { namespace Cst {

//! "keyword/name" addressing key for a named chunk; "" otherwise.  EXPORTED
//! (Cst.h) since Model-B F5 S2 -- Job::ApplyCstInsertChunk builds the same
//! key for its duplicate-(kind,name) check; every internal name-index site
//! below also routes through this one construction.
std::string ChunkNamePath( const NodeRef& item )
{
	if( !item || item->kind != NodeKind::Chunk ) return std::string();
	std::string nm;
	if( !ParamValue( item.get(), "name", nm ) || nm.empty() ) return std::string();
	return item->role + "/" + nm;
}

// True iff `doc` is native v7-form -- loadable by the CST path WITHOUT mis-deriving.  CST-load (DeriveToJob)
// SILENTLY SKIPS every top-level `>` directive (v7 has no `>` command layer), so the ONLY `>` lines safe to
// accept are ones whose effect is RENDER-NEUTRAL when dropped.  DumpJob is blind to render state, so
// "render-neutral" -- NOT "DumpJob-MATCH" -- is the bar.  ACCEPTED top-level content:
//   * the `RISE ASCII SCENE <n>` header (required; version NUMBER not checked -- the CST is version-agnostic,
//     so both the post-cutover `7` and the transitional `6` load; a true skew is caught downstream by
//     DeriveToJob's descriptor validation)
//   * any chunk
//   * `> echo ...`            -- logging only
//   * `> set accelerator ...` -- an image-identical TLAS choice (vs the default BVH4)
// REJECTED (each would mis-derive / mis-render if its `>`/construct were silently skipped):
//   * `FOR`/`ENDFOR`          -- a loop: body derives once, not N times
//   * `> run`/`> load`        -- an include: the included chunks are dropped
//   * `> modify ...`          -- RENDER-AFFECTING engine mutations (material swaps, scale, ...).  Being
//     deprecated; its watch-hero "light configurations" use case moves to a first-class CST feature.
//   * `> set <other>` (e.g. light_rr_threshold) -- RENDER-AFFECTING; the migrator must convert it to a chunk
//     (Slice 2 easy-convert) before such a scene is CST-loadable.
// (`> set global_medium` IS already converted to a chunk -> never a `>` line here; `$()/@` refs inside a chunk
// VALUE are caught by DeriveToJob's numeric validation; a folded-away DEFINE never reaches here.  SeqToVec is
// the anon-namespace flattener above; it is TU-visible here.)
bool IsNativeV7Document( const Document& doc )
{
	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );

	// Group consecutive top-level WORD tokens into "lines" (a newline-bearing Trivia, or a Chunk, ends one),
	// then classify each line by its FIRST token -- so a `> echo FOR done` is judged a directive (allowed),
	// not a FOR loop.
	bool sawHeader = false;
	std::vector<std::string> line;
	auto classify = [&]( const std::vector<std::string>& ln ) -> bool {   // true = native-v7 OK; false = reject
		if( ln.empty() ) return true;
		if( ln[0] == "RISE" ) {   // the version-header line -- `7` post-cutover, `6` still accepted (back-compat)
			sawHeader = ( ln.size() >= 4 && ln[1] == "ASCII" && ln[2] == "SCENE" &&
				!ln[3].empty() && ln[3].find_first_not_of( "0123456789" ) == std::string::npos );
			return sawHeader;
		}
		if( ln[0] == ">" ) {      // a directive line: accept ONLY proven render-NEUTRAL directives (see the header)
			if( ln.size() >= 2 && ln[1] == "echo" ) return true;                            // logging only, no scene effect
			if( ln.size() >= 3 && ln[1] == "set" && ln[2] == "accelerator" ) return true;   // image-identical TLAS choice
			return false;         // render-AFFECTING (> modify / > set <other>) or an include (> run / > load) => reject
		}
		return false;             // FOR/ENDFOR/DEFINE/UNDEF / a bare stray at line start => NOT native v7
	};
	for( const NodeRef& c : items ) {
		if( !c ) continue;
		if( c->kind == NodeKind::Token ) { line.push_back( c->text ); continue; }
		const bool endsLine = ( c->kind == NodeKind::Chunk ) ||
			( c->kind == NodeKind::Trivia && c->text.find( '\n' ) != std::string::npos );
		if( endsLine ) { if( !classify( line ) ) return false; line.clear(); }
	}
	if( !classify( line ) ) return false;   // trailing line (no terminating newline)
	return sawHeader;
}

Document ParseToCst( const std::string& bytes )
{
	std::vector<RawTok> t = Tokenize( bytes );
	size_t i = 0;
	std::vector<NodeRef> items;
	while( i < t.size() ) {
		if( t[i].trivia ) { items.push_back( Leaf(NodeKind::Trivia, t[i++].text, "") ); continue; }
		// A chunk is `keyword {` (brace may be on the next line, or -- structurally -- the same
		// line; this recognition step stays permissive so the tree is always lossless). A bare word
		// not followed by `{` -- e.g. each token of the `RISE ASCII SCENE 7` header -- is preserved
		// losslessly as a stray Token, NOT swallowed as a never-closed chunk. (A dedicated
		// version-header node is a later item.) Whether the brace actually sat on its own line, as
		// the authoring convention requires, is a DERIVE-time (PASS-1) check -- see
		// ChunkBraceViolations / ResolveChunkParams below -- not a parse-time one.
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
		if( ChunkIsSourceInstance( items[k].get() ) ) ++d.sourceInstanceCount;   // the O(1) incremental-refuse signal
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

// Model-B shared-undo U2: item-level counterpart of SerializeCst -- serializes exactly ONE node (chunk or
// trivia) to its verbatim bytes.  Delegates to the SAME anonymous-namespace Serialize() SerializeCst/SeqSerialize
// use, so a caller composing several nodes back-to-back (e.g. the chunk + its tidied-away trailing separator)
// reproduces exactly the substring SerializeCst would have shown at that position.
std::string SerializeNode( const NodeRef& n )
{
	std::string s;
	if( n ) Serialize( n, s );
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

//! PASS-1 for ONE chunk, shared by the full (DeriveToJob) and incremental
//! (DeriveToJobIncremental) derive so they normalise params IDENTICALLY (no
//! divergence): find the parser for the chunk's keyword; flag any value-less
//! parameter; build the ParamsList in document order, ONE line per param
//! occurrence, each line's name + value TOKENS joined with single spaces --
//! exactly the normalisation the legacy parser applies (TokenizeString on
//! " \t\r" runs + make_string_from_tokens(" ")) before DispatchChunkParameters.
//! Returns the parser (caller constructs the bag + dispatches + Finalizes) or
//! null + a diagnostic on an unknown chunk type. (Value-less-param diagnostics
//! are pushed even when the parser resolves, matching the legacy reject.)
//! #5 slice 2 -- the expr(...) value sublanguage (Facet 1 owns PARSING+the canonical derive-time
//! evaluation; the editor's incremental traced-input invalidation is deferred Facet-2/D4 work).
//! If `value` is exactly `expr( <arithmetic> )`, evaluate it via the shared ExpressionProgram and
//! return the numeric literal in `outLit` (so the descriptor parser sees a number -- exactly what
//! the v6->v7 migrator would bake).  A non-expr value returns false + is passed through VERBATIM
//! (so non-expr scenes are byte-identical, the CstDeriveDifferentialTest gate).  A malformed /
//! non-finite expr pushes a diagnostic + returns false -> the value stays `expr(...)`, which the
//! descriptor parser then rejects -> the caller's refuse-all applies NOTHING (no silent bad value).
//! #5 slice 3 -- document-level `let` constants (§2.6, the DEFINE replacement).  A `let { NAME
//! value ... }` chunk declares scene-global named constants used in expr(...): `let { POWER 2.3 }` +
//! `power expr( POWER )`.  `let` is NOT an engine entity -- DeriveToJob collects its bindings here and
//! does NOT apply the chunk.  A binding is one numeric literal OR an expr(...) that may reference
//! EARLIER lets (lexical document-order scope, no UNDEF; a forward/cyclic reference fails to resolve
//! -> diagnosed -> refuse).  Bindings are stored as EVALUATED NUMERIC values (NAME -> double),
//! evaluated once here in document order; a consumer's expr then simply AddParams them.
typedef std::vector<std::pair<std::string,double> > LetBindings;

//! Is `value` exactly `expr( <balanced> )`?  If so set `body` to the inside.  The opening '(' (index
//! 4) must balance to the FINAL ')', so `expr(a)+expr(b)` / `expr(a) x` are NOT a single expr value
//! (they fall through to the descriptor parser's rejection).
static bool IsExprValue( const std::string& value, std::string& body )
{
	if( value.size() < 6 || value.compare( 0, 5, "expr(" ) != 0 || value.back() != ')' ) return false;
	int depth = 0; size_t lastClose = std::string::npos;
	for( size_t k = 4; k < value.size(); ++k ) {
		if( value[k] == '(' ) ++depth;
		else if( value[k] == ')' ) { if( --depth == 0 ) { lastClose = k; break; } }
	}
	if( lastClose != value.size() - 1 ) return false;
	body = value.substr( 5, value.size() - 6 );   // between `expr(` and the final `)`
	return true;
}

//! Compile + evaluate one expr BODY over the numeric `lets` + the reserved built-ins PI/E (added
//! AFTER the lets, so a let cannot shadow them).  Returns the finite result, or false with `err`
//! set (a compile error, or a non-finite result -- rejected by a compiler-opaque scan of the %.17g
//! string -- this derive path formats r to %.17g anyway, so the byte scan is the free check here;
//! ExpressionProgram::IsFinite is now volatile-hardened and is the equivalent hot-path guard).  u/v are the evaluator's query coordinates --
//! bound to an instancing chunk's i/j-derived u,v (87 step 3c); a let / scene expr passes u=v=0 (a constant).
//!
//! THE NON-FINITE GUARD DOES NOT COVER DIVISION OR MODULO BY ZERO, and reading it as "an expr
//! is protected against a bad divisor" is wrong.  `ExpressionEval.h`'s `kDiv` / `kMod` return
//! ZERO for a zero divisor rather than producing an inf/nan -- deliberate, and the painters
//! depend on it (a procedural texture must not blow up on one pixel) -- so nothing non-finite
//! ever reaches this scan.  `expr(1/(i-1))` over `count_u 2` therefore derives x = -1 and then
//! x = 0, with NO diagnostic, and `count_u expr(4/0)` derives a silently EMPTY array.  Do not
//! "fix" it in the evaluator; it is documented as an authoring hazard in SCENE_CONVENTIONS
//! (§ per-instance expressions).
static bool EvalExprBody( const std::string& body, const LetBindings& lets, double u, double v, double& outVal, std::string& err )
{
	RISE::Implementation::ExpressionProgram prog = RISE::Implementation::ExpressionProgram::Invalid();
	RISE::Implementation::ExpressionProgram::Builder b;
	// P2-A (review round 1, S1 texture-expressions VM): this Builder does NOT
	// call EnableContextVars, so it stays at the default OFF -- `time`, `P`,
	// `Po`, `N`, `fw` are ordinary "unknown variable" compile errors here,
	// exactly as before the S1 VM extension added those five names at all.
	// u/v are unaffected (never gated) and mean the query coordinates below.
	// Do NOT flip this on: this surface (document-level `expr(...)` / `let`)
	// has no shading context to supply P/N/etc. from, and enabling it would
	// silently start accepting names that mean nothing here.
	for( const std::pair<std::string,double>& L : lets ) b.AddParam( L.first, (Scalar)L.second );
	b.AddParam( "PI", (Scalar)3.14159265358979323846 );
	b.AddParam( "E",  (Scalar)2.71828182845904523536 );
	if( !b.Finalize( body, prog ) || !prog.IsValid() ) { err = "failed to compile: " + prog.Error(); return false; }
	// P2-A: the S1 VM added a vec3 type -- this surface only ever meant ONE
	// scalar (the whole point of `expr(...)` is "compute a number"), so a
	// vec3-typed final expression (reachable via `expr(vec3(...))` even with
	// context vars off) must be a hard error too, not a silent .x derive.
	if( prog.ResultType() != RISE::Implementation::ExpressionProgram::kScalar ) {
		err = "expr(...) must evaluate to a scalar, not vec3";
		return false;
	}
	const Scalar r = prog.Eval( (Scalar)u, (Scalar)v );   // u/v = the query coordinates (instance vars for a counted `source`; 0 for a constant expr)
	char buf[64];
	// %.17g round-trips the double EXACTLY (C-locale '.' decimal, shared with the sscanf("%lf")/strtod
	// parse-back path).  A finite %.17g is only [0-9.eE+-]; an 'n'(an)/'i'(nf) char marks nan/inf.
	std::snprintf( buf, sizeof(buf), "%.17g", (double)r );
	for( const char* q = buf; *q; ++q )
		if( *q == 'n' || *q == 'N' || *q == 'i' || *q == 'I' ) { err = std::string( "evaluated to a non-finite value (" ) + buf + ")"; return false; }
	outVal = (double)r;
	return true;
}

//! Identifiers the expression grammar OWNS, so a `let` may not bind them: the query coordinates
//! u/v + the future instance vars i/j (pre-registered Builder slots -- a let named u/v would be
//! silently clobbered by Eval), and the math constants pi/e/tau (ExpressionProgram lexer constants)
//! + PI/E (the reserved uppercase built-ins).  Rejecting them keeps every math constant + coordinate
//! un-shadowable and avoids the u/v clobber.
//!
//! P2-A NOTE (review round 1, S1 texture-expressions VM): P, Po, N, fw, time
//! are deliberately NOT in this list, even though the VM added them as
//! context-var names.  EvalExprBody's Builder keeps EnableContextVars OFF on
//! this surface (see its call site), so those five names are not otherwise
//! bound to anything here -- there is nothing for a `let` to shadow, so
//! `let { time 1.5 }` is a perfectly ordinary user param like any other, and
//! a later `expr(time*2+1)` resolves it via the normal m_index lookup
//! (Builder::ParseAtom checks m_index before context vars regardless of this
//! flag).  Without that let, `time` stays a hard "unknown variable" compile
//! error, matching the pre-vec3-VM behaviour this surface is pinned to.  Do
//! NOT add P/Po/N/fw/time here -- that would only make them un-let-able for
//! no benefit, since they mean nothing on this surface either way.
static bool IsReservedExprName( const std::string& n )
{
	return n == "u" || n == "v" || n == "i" || n == "j" ||
	       n == "pi" || n == "e" || n == "tau" || n == "PI" || n == "E";
}

//! Split a value into whitespace-separated components, treating a BALANCED (...) span as part of one
//! component, so `expr( i + 1 ) 0 0` -> 3 components (not 7).  Used for per-component instance eval.
static std::vector<std::string> SplitComponents( const std::string& value )
{
	std::vector<std::string> out;
	size_t i = 0, n = value.size();
	while( i < n ) {
		while( i < n && ( value[i] == ' ' || value[i] == '\t' ) ) ++i;
		if( i >= n ) break;
		const size_t start = i; int depth = 0;
		while( i < n ) {
			const char ch = value[i];
			if( ch == '(' ) ++depth;
			else if( ch == ')' ) { if( depth > 0 ) --depth; }
			else if( ( ch == ' ' || ch == '\t' ) && depth == 0 ) break;
			++i;
		}
		out.push_back( value.substr( start, i - start ) );
	}
	return out;
}

//! Evaluate a multi-component instancing-chunk param VALUE for ONE instance (i,j indices +
//! u,v in [0,1] in scope) -- each component is an `expr(...)` (evaluated) or a literal (kept verbatim);
//! the components are re-joined single-spaced.  i/j are passed as numeric bindings; u/v via the
//! evaluator's query coordinates (they are pre-registered slots, so they CANNOT be AddParam'd).
static bool EvalInstanceValue( const std::string& value, const LetBindings& lets,
                               int iVal, int jVal, double u, double v, std::string& out, std::string& err )
{
	LetBindings inst = lets;
	inst.push_back( std::make_pair( std::string( "i" ), (double)iVal ) );
	inst.push_back( std::make_pair( std::string( "j" ), (double)jVal ) );
	out.clear();
	for( const std::string& comp : SplitComponents( value ) ) {
		std::string body;
		if( IsExprValue( comp, body ) ) {
			double r;
			if( !EvalExprBody( body, inst, u, v, r, err ) ) return false;
			char buf[64]; std::snprintf( buf, sizeof(buf), "%.17g", r );
			if( !out.empty() ) out += ' '; out += buf;
		} else {
			if( !out.empty() ) out += ' '; out += comp;
		}
	}
	return true;
}

//! 87 step 3c: the PER-INSTANCE variable bindings one repetition of an instancing chunk
//! evaluates its own parameters under -- `i`/`j` indices and `u`/`v` in [0,1].  Passed as a
//! POINTER everywhere below, and a null pointer means "not a repetition": the single-instance
//! `source` form and every ordinary chunk keep the whole-value `expr(...)` handling they had.
struct InstanceVars
{
	int    i;
	int    j;
	double u;
	double v;
};

//! Validate ONE count (`count_u` / `count_v`) for a repetition generator.
//!
//! The diagnostic PREFIX is parameterised (`who`) so one implementation can serve every
//! caller; 87 step 3d left `source` counts as the only one.
//!
//! THE ARITHMETIC ENCODES TWO PRIOR P1 FIXES AND MUST NOT BE "CLEANED UP":
//!   * the `(long long)` round-trip rejects a FRACTIONAL count (P1-B) -- silently rounding
//!     via `(int)(d+0.5)` would change the generator's cardinality (`count_u 1.5` -> 2);
//!   * `errno == ERANGE` rejects an out-of-range literal AT THE SOURCE.  Overflow
//!     (`1e999` -> HUGE_VAL) must not be left to an `inf > 1e6` compare, which was
//!     unreliable under bare `-ffast-math`; UNDERFLOW (`1e-999`) is caught by NOTHING
//!     ELSE HERE -- strtod returns a finite 0, which passes both the range test and the
//!     integrality test, so deleting the ERANGE term silently turns it into `count 0`.
//! The nan/inf char scan catches an explicit "nan"/"inf" literal (strtod sets no errno for
//! those); an expr-valued count already passed EvalExprBody's finite guard.
static bool EvalInstanceCount( const std::string& raw, const LetBindings& lets,
                               const std::string& who, const char* which,
                               std::vector<std::string>& diags, int& out )
{
	out = 0;
	std::string cs, e;
	if( !EvalInstanceValue( raw, lets, 0, 0, 0.0, 0.0, cs, e ) ) { diags.push_back( who + ": " + which + " " + e ); return false; }
	errno = 0;
	char* end = nullptr; const double d = std::strtod( cs.c_str(), &end );
	bool bad = cs.empty() || end != cs.c_str() + cs.size() || errno == ERANGE;
	for( size_t ci = 0; ci < cs.size(); ++ci ) { const char ch = cs[ci]; if( ch == 'n' || ch == 'N' || ch == 'i' || ch == 'I' ) bad = true; }
	if( !bad ) { if( d < 0.0 || d > 1.0e6 ) bad = true; else if( (double)(long long)d != d ) bad = true; }
	if( bad ) { diags.push_back( who + ": " + which + " must be a non-negative integer <= 1e6 (got '" + cs + "')" ); return false; }
	out = (int)d;
	return true;
}

static LetBindings CollectLetBindings( const std::vector<NodeRef>& items, std::vector<std::string>& diags )
{
	LetBindings out;
	for( const NodeRef& c : items ) {
		if( c->kind != NodeKind::Chunk || c->role != "let" ) continue;
		for( const NodeRef& kid : c->kids )                       // a name with no value on its line
			if( kid->kind == NodeKind::Token && kid->role == "pname" )
				diags.push_back( "let." + kid->text + ": value-less binding (needs one numeric literal or expr(...))" );
		for( const NodeRef& kid : c->kids ) {
			if( kid->kind != NodeKind::Param ) continue;
			std::string name, raw; bool first = true;
			for( const NodeRef& tk : kid->kids )
				if( tk->kind == NodeKind::Token ) {
					if( first ) { name = tk->text; first = false; }
					else { if( !raw.empty() ) raw += ' '; raw += tk->text; }
				}
			if( IsReservedExprName( name ) ) {
				diags.push_back( "let." + name + ": reserved expression identifier (u/v/i/j coordinate or pi/e/tau/PI/E constant) -- cannot be a let name" );
				continue;
			}
			std::string body; double val = 0;
			if( IsExprValue( raw, body ) ) {                  // expr-valued let: eval over the EARLIER lets only
				std::string err;
				if( !EvalExprBody( body, out, 0, 0, val, err ) ) {   // a forward/cyclic ref fails here (not yet in `out`)
					diags.push_back( "let." + name + ": expr binding " + err );
					continue;
				}
			} else {                                          // literal: exactly ONE finite numeric token
				char* end = nullptr;
				val = std::strtod( raw.c_str(), &end );
				if( raw.empty() || end != raw.c_str() + raw.size() ) {
					diags.push_back( "let." + name + ": a binding must be one numeric literal or expr(...) (got '" + raw + "')" );
					continue;
				}
			}
			out.push_back( std::make_pair( name, val ) );
		}
	}
	return out;
}

bool EvaluateObjectRepeatCounts( const Document& doc, const NodeRef& chunk, int& outCountU, int& outCountV )
{
	outCountU = 1;
	outCountV = 1;
	if( !chunk ) return false;
	std::string countU_raw, countV_raw;
	const bool hasCountU = ParamValue( chunk.get(), "count_u", countU_raw ) && !countU_raw.empty();
	if( !hasCountU ) return false;   // not a repeat chunk -- see this function's own .h header comment
	const bool hasCountV = ParamValue( chunk.get(), "count_v", countV_raw ) && !countV_raw.empty();

	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );
	std::vector<std::string> diags;   // discarded -- see this function's own .h header comment
	const LetBindings lets = CollectLetBindings( items, diags );

	int cu = 1, cv = 1;
	bool ok = EvalInstanceCount( countU_raw, lets, "count_u", "count_u", diags, cu );
	if( hasCountV ) ok = EvalInstanceCount( countV_raw, lets, "count_v", "count_v", diags, cv ) && ok;
	if( !ok ) return false;
	outCountU = cu;
	outCountV = cv;
	return true;
}

static bool TryEvalExprValue( const std::string& value, std::string& outLit,
                              std::vector<std::string>& diags, const std::string& kw, const std::string& pname,
                              const LetBindings& lets )
{
	std::string body;
	if( !IsExprValue( value, body ) ) return false;   // not a single expr( ... ) value -> passed through verbatim
	double result; std::string err;
	if( !EvalExprBody( body, lets, 0, 0, result, err ) ) {        // compile error / non-finite -> refuse (refuse-all upstream).
		// Caught at the eval boundary so EVERY slot kind is guarded, incl. String / inline-scalar-painter
		// slots that have no later numeric check (the slice-2 non-finite escape).
		diags.push_back( kw + "." + pname + ": expr(...) " + err );
		return false;
	}
	char buf[64];
	std::snprintf( buf, sizeof(buf), "%.17g", result );
	outLit = buf;
	return true;
}

//! A chunk's parameters as ORDERED (pname, value) pairs, with `expr(...)` values
//! evaluated -- the normalisation ResolveChunkParams applies, exposed as PAIRS so
//! 87 step 3's `source` expansion can MERGE two chunks' parameters BY NAME.
//!
//! Reads the CST TOKENS directly, and everything downstream of it must keep doing
//! so: `ResolveChunkParams` evaluates `expr(...)` with i=j=u=v=0 BEFORE any
//! ParseStateBag exists, so a bag-driven expansion would silently freeze every
//! per-instance expression at instance zero.  3a has no per-instance exprs, but
//! 3c does, and it inherits this code path.
//!
//! `iv` (87 step 3c) is the per-instance context: NON-NULL for a repetition of an
//! instancing chunk, in which case EVERY value is evaluated PER COMPONENT through
//! `EvalInstanceValue` (so `position expr(i*2) 0 0` is three components, one of them an
//! expr) with i/j/u/v in scope.  NULL keeps the
//! whole-value-only handling every other chunk has always had, and is byte-identical to
//! it for a value with no `expr(` in it: `EvalInstanceValue` re-joins the components
//! single-spaced, which is what the token loop below already produced.
//!
//! Returns false ONLY when `iv` is non-null and a per-instance eval failed (the caller
//! must then stop -- a partial expansion is worse than none).  A whole-value expr failure under `iv == 0` keeps the legacy shape:
//! diagnosed, value left verbatim, refuse-all upstream.
static bool ChunkParamPairs( const NodeRef& c, const LetBindings& lets,
                             std::vector<std::string>& diags,
                             std::vector<std::pair<std::string,std::string> >& out,
                             const InstanceVars* iv = nullptr )
{
	const std::string& kw = c->role;
	for( const auto& kid : c->kids )
		if( kid->kind == NodeKind::Param ) {
			std::string pname, value;
			bool first = true;
			for( const auto& tk : kid->kids )
				if( tk->kind == NodeKind::Token ) {            // pname + pvalue tokens; skip trivia
					if( first ) { pname = tk->text; first = false; }
					else { if( !value.empty() ) value += ' '; value += tk->text; }
				}
			if( iv ) {
				std::string ev, e;
				if( !EvalInstanceValue( value, lets, iv->i, iv->j, iv->u, iv->v, ev, e ) ) {
					char where[64]; std::snprintf( where, sizeof(where), "[%d,%d]", iv->i, iv->j );
					diags.push_back( kw + "." + pname + " " + where + ": " + e );
					return false;
				}
				out.push_back( std::make_pair( pname, ev ) );
				continue;
			}
			// #5 slice 2: an expr(...) value -> a numeric literal (Facet-1 derive-time eval); a non-expr
			// value is passed through verbatim, so non-expr scenes stay byte-identical to legacy.
			std::string lit;
			if( TryEvalExprValue( value, lit, diags, kw, pname, lets ) ) value = lit;
			out.push_back( std::make_pair( pname, value ) );
		}
	return true;
}

//! Does sibling `c->kids[idx]` (a brace Token) share its physical line with the DIRECT sibling
//! found by walking `c->kids` outward in `dir` (+1 forward, -1 backward)?  Walks past any number
//! of intervening Trivia siblings (in practice at most one -- Tokenize always merges a run of
//! whitespace/comments into a single Trivia token, so two Trivia siblings never sit back to back)
//! stopping the FIRST TIME either: (a) a Trivia sibling's text contains a newline -- the brace's
//! line ends there with nothing else on it in this direction, not a violation; or (b) a non-Trivia
//! sibling is reached with no newline yet seen -- that sibling is content sharing the brace's line,
//! a violation.  Reaching past the end of `c->kids` in `dir` (the brace is the first/last kid, or
//! only Trivia remains) is NOT itself a violation -- for the forward direction off the closing `}`
//! this is actually the norm (see NextDocContentSharesLine below for what that direction needs
//! instead, since `}` is always `c`'s last kid and can have no forward sibling within the chunk).
static bool SharesLineWithSibling( const NodeRef& c, std::size_t idx, int dir )
{
	for( std::size_t k = idx; ; ) {
		if( dir > 0 ) { if( k + 1 >= c->kids.size() ) return false; ++k; }
		else          { if( k == 0 ) return false; --k; }
		const NodeRef& sib = c->kids[k];
		if( sib->kind == NodeKind::Trivia ) { if( sib->text.find( '\n' ) != std::string::npos ) return false; continue; }
		return true;
	}
}

//! Does content immediately FOLLOW top-level item `itemIndex` (chunk `c`, whose closing `}` is
//! that item's last kid) on the SAME physical line as that `}` -- i.e. is there a next chunk /
//! stray token glued onto this one's close brace with no newline between?  `}` is always the last
//! kid of its own Chunk node (ParseChunk stops the instant depth reaches 0), so nothing in `c`'s
//! own kids can answer this; it requires peeking at the DOCUMENT's next top-level item(s).  Cheap:
//! SeqItemAt is O(log N) and a Trivia run is always maximal (Tokenize never emits two adjacent
//! Trivia items), so this loop is bounded at two iterations in practice, never a document scan.
static bool NextDocContentSharesLine( const Document& doc, std::size_t itemIndex )
{
	const int n = DocItemCount( doc );
	for( int k = (int)itemIndex + 1; k < n; ++k ) {
		const NodeRef item = SeqItemAt( doc.items, k );
		if( !item ) return false;
		if( item->kind == NodeKind::Trivia ) {
			if( item->text.find( '\n' ) != std::string::npos ) return false;
			continue;   // whitespace/comment run with no newline: keep looking at what follows it
		}
		return true;   // a chunk (or stray token) starts right after `}` before any newline
	}
	return false;   // `}` is the last thing in the document
}

//! Does chunk `c` violate "each of `{` and `}` must be on its own line" -- the documented
//! authoring convention (CLAUDE.md / docs/SCENE_CONVENTIONS.md / Parsers/README.md)?  ParseChunk
//! itself stays permissive about this (a brace-sharing / one-line chunk still parses losslessly
//! into a tree -- see ParseToCst's "Lossless" contract, and WithParamValueOrInsert's defensive
//! handling further down, which predates this check); this function backs a DERIVE-time (PASS-1)
//! validation, the same layer "unknown chunk type" / "value-less parameter" are diagnosed at, so a
//! violation refuses the WHOLE derive (see DeriveToJob's two-tier boundary comment in Cst.h)
//! instead of quietly deriving a chunk whose params silently merged into one another. THE ACTUAL
//! BUG THIS CLOSES: `standard_object { name x geometry g material m }` written on one line has no
//! newline anywhere in its body, so the per-param same-line value-collection loop in ParseChunk
//! (above) swallows `geometry g material m` as MORE pvalue tokens of `name` -- the chunk keeps its
//! keyword but silently loses every param after the first, and Finalize used to emit an object
//! with no geometry/material instead of erroring (the "vanished wall objects, zero diagnostics"
//! report).
//!
//! `openSameLine` fires when `{` shares its line with EITHER side: the keyword before it, or
//! whatever follows it (a param, `}` on an empty one-line chunk, or nested content) -- this second
//! half is task_7f42984d's closed gap: `kw\n{ name x geometry g material m\n}` used to report ZERO
//! violations (kw/`{` are on different lines, and `}` is alone on its own line) while ParseChunk's
//! same-line value loop still swallowed every param after `name` exactly as in the fully-glued
//! case, re-opening the silent-loss hole this check exists to close.  `closeSameLine` fires when
//! `}` shares its line with EITHER side: whatever precedes it (a param value, or `{` itself, on an
//! empty chunk) -- covered locally via `c->kids` -- or whatever follows it at the DOCUMENT level (a
//! sibling chunk glued on with no intervening newline) -- covered via NextDocContentSharesLine,
//! which needs `doc`/`itemIndex` (both optional; omitting them only disables that half of the
//! close-brace check, matching every existing call site that always has them available).  A brace
//! comment (`{   # note`) is NOT a violation: `#`-to-EOL is folded into ONE Trivia token by
//! Tokenize, and that token's text still contains the terminating `\n`, so SharesLineWithSibling's
//! newline scan sees it and stops -- verified in tests/CstDeriveContractsTest.cpp.
static void ChunkBraceViolations( const Document* doc, std::size_t itemIndex, const NodeRef& c,
	bool& openSameLine, bool& closeSameLine )
{
	openSameLine = false;
	closeSameLine = false;
	if( !c ) return;
	std::size_t lbraceIdx = (std::size_t)-1, rbraceIdx = (std::size_t)-1;
	for( std::size_t k = 0; k < c->kids.size(); ++k ) {
		if( c->kids[k]->kind != NodeKind::Token ) continue;
		if( c->kids[k]->role == "lbrace" && lbraceIdx == (std::size_t)-1 ) lbraceIdx = k;
		else if( c->kids[k]->role == "rbrace" ) rbraceIdx = k;   // depth-0 close: unique, always the last kid
	}
	if( lbraceIdx != (std::size_t)-1 )
		openSameLine = SharesLineWithSibling( c, lbraceIdx, -1 ) || SharesLineWithSibling( c, lbraceIdx, +1 );
	if( rbraceIdx != (std::size_t)-1 ) {
		closeSameLine = SharesLineWithSibling( c, rbraceIdx, -1 );
		if( !closeSameLine && doc != nullptr && itemIndex != (std::size_t)-1 )
			closeSameLine = NextDocContentSharesLine( *doc, itemIndex );
	}
}

//! 1-based line number of chunk `c` (top-level item `itemIndex` in `doc`)'s opening `{`
//! (isClose=false) or closing `}` (isClose=true) -- feeds the ChunkBraceViolations diagnostic
//! ONLY. Reserializes the whole document to count newlines up to the brace's byte offset: an O(N)
//! cost that is acceptable here because this is reached EXCLUSIVELY on the rare violation path (a
//! normal, well-formed derive never calls it) -- never on a per-chunk or per-derive hot path.
//! `intra` sums the serialized width of every kid BEFORE the target brace -- for isClose that's
//! everything up to (not including) `rbrace`; for !isClose it's everything up to (not including)
//! `lbrace`, i.e. just the keyword (+ its trailing trivia). Getting the !isClose case wrong reports
//! the KEYWORD's line instead of `{`'s -- invisible whenever they share a line (the common
//! kw-glued-to-`{` violation), but wrong the moment `{` is on its own line and STILL violates (e.g.
//! content glued to `{`'s own line, one line down from `kw`) -- exactly the shape
//! [open-brace-content] in tests/CstDeriveContractsTest.cpp exists to catch.
static int LineOfChunkBrace( const Document& doc, std::size_t itemIndex, const NodeRef& c, bool isClose )
{
	const size_t chunkOff = DocByteOffsetOfItem( doc, (int)itemIndex );
	if( chunkOff == (size_t)-1 ) return -1;
	const char* targetRole = isClose ? "rbrace" : "lbrace";
	size_t intra = 0;
	for( const auto& k : c->kids ) {
		if( k->kind == NodeKind::Token && k->role == targetRole ) break;
		std::string s; Serialize( k, s ); intra += s.size();
	}
	const std::string full = SerializeCst( doc );
	size_t off = chunkOff + intra;
	if( off > full.size() ) off = full.size();
	return 1 + (int)std::count( full.begin(), full.begin() + off, '\n' );
}

//! A chunk keyword a PAST RELEASE of RISE accepted and that has since been REMOVED.
//!
//! WHY A TABLE AND NOT THE GENERIC MESSAGE.  Deleting a chunk parser makes its keyword
//! unknown, and the generic "unknown chunk type 'X'" is a dead end for the one author who
//! most needs help: the person holding a scene file that USED to load.  It names neither
//! the replacement nor the migrator that rewrites the chunk, so the author's next move is a
//! web search or a grep of the source tree.  This is the chunk-level analogue of
//! `bezierpatch_geometry`'s `kRetired` PARAMETER table (ChunkParserRegistry.cpp), which
//! exists for exactly the same reason one level down.
//!
//! One entry per removal.  `advice` is appended to "chunk type 'X' has been removed -- "
//! and must name (a) what to use instead and (b) how to convert an existing scene.
struct RetiredChunkKeyword { const char* keyword; const char* advice; };

static const RetiredChunkKeyword kRetiredChunks[] = {
	{ "bumpmap_modifier",
	  "removed 2026-09-06 (docs/RELIEF_MODIFIER_DESIGN.md 7.5).  Use `relief_modifier`, "
	  "whose height field is any `scalar_painter` and which needs no texcoords in its "
	  "default `surface` domain.  To convert this scene LOSSLESSLY -- the amplitude fold "
	  "and the sign flip are not obvious by hand -- run "
	  "`python3 tools/migrate_scenes_relief.py <this file>`" }
};

//! The directed advice for a REMOVED chunk keyword, or null when `kw` names no chunk RISE
//! ever had (in which case the caller's generic "unknown chunk type" is the honest message).
//! Declared in Cst.h (RISE::Cst) -- not static -- so SchemaGen's describe_chunk/read_schema
//! and AgentSession's insert_chunk near-miss analyser can surface the same directed message
//! (see the header doc).
const char* RetiredChunkAdvice( const std::string& kw )
{
	for( std::size_t i = 0; i < sizeof(kRetiredChunks)/sizeof(kRetiredChunks[0]); ++i ) {
		if( kw == kRetiredChunks[i].keyword ) return kRetiredChunks[i].advice;
	}
	return 0;
}

static const IAsciiChunkParser* ResolveChunkParams(
	const NodeRef& c,
	const std::map<std::string, const IAsciiChunkParser*>& registry,
	IAsciiChunkParser::ParamsList& plist,
	std::vector<std::string>& diags,
	const LetBindings& lets,
	const InstanceVars* iv = nullptr,
	const Document* doc = nullptr,
	std::size_t itemIndex = (std::size_t)-1 )
{
	const std::string& kw = c->role;
	std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( kw );
	if( it == registry.end() ) {
		// A keyword RISE USED to accept gets a directed message naming its replacement and
		// the migrator; anything else gets the honest generic one.  See kRetiredChunks.
		const char* const retired = RetiredChunkAdvice( kw );
		diags.push_back( retired
			? ( "chunk type '" + kw + "' has been removed -- " + retired )
			: ( "unknown chunk type '" + kw + "'" ) );
		return nullptr;
	}
	// Hard-reject BEFORE any param is read: on a violation, the params extracted below cannot be
	// trusted anyway (see ChunkBraceViolations' header for the swallow mechanism), and we must not
	// go on to silently apply a chunk missing every param after the one that absorbed its siblings.
	bool openSameLine = false, closeSameLine = false;
	ChunkBraceViolations( doc, itemIndex, c, openSameLine, closeSameLine );
	if( openSameLine || closeSameLine ) {
		const bool haveLoc = doc != nullptr && itemIndex != (std::size_t)-1;
		if( openSameLine ) {
			const int line = haveLoc ? LineOfChunkBrace( *doc, itemIndex, c, false ) : -1;
			diags.push_back( kw + ( line > 0 ? " (line " + std::to_string( line ) + ")" : "" ) + ": chunk braces must be on their own lines" );
		}
		if( closeSameLine ) {
			const int line = haveLoc ? LineOfChunkBrace( *doc, itemIndex, c, true ) : -1;
			diags.push_back( kw + ( line > 0 ? " (line " + std::to_string( line ) + ")" : "" ) + ": chunk braces must be on their own lines" );
		}
		return nullptr;
	}
	for( const auto& kid : c->kids )
		if( kid->kind == NodeKind::Token && kid->role == "pname" )
			diags.push_back( kw + ": value-less parameter '" + kid->text + "'" );
	std::vector<std::pair<std::string,std::string> > pairs;
	(void)ChunkParamPairs( c, lets, diags, pairs, iv );   // a failed instance eval is already diagnosed -> refuse-all upstream
	for( std::size_t i = 0; i < pairs.size(); ++i ) {
		std::string line = pairs[i].first;
		if( !pairs[i].second.empty() ) { line += ' '; line += pairs[i].second; }
		plist.push_back( String( line.c_str() ) );
	}
	return it->second;
}

//! Map a chunk's category to its IJob typed removal (the drop half of an
//! incremental re-derive). The drop is a COMPLETE undo of the chunk's Finalize
//! only when that Finalize registered the item in exactly this one manager; these
//! five categories are verified single-manager (Geometry->geometry,
//! Material->material, Object->object, Light->light, Modifier->modifier).
//! Returns false for any OTHER category -- the incremental path then refuses +
//! the caller falls back to a full DeriveToJob. Two categories are deliberately
//! NOT here: PAINTER (the category spans painter sub-types with DIFFERING
//! reversibility -- the colour painters dual-register the SAME object in BOTH the
//! painter manager AND the function-2D manager, and RemovePainter now reverses
//! that pair identity-gated; but a `scalar_painter` (also ChunkCategory::Painter)
//! lives in the SEPARATE scalar-painter manager that RemovePainter does not touch,
//! so a typed drop of that sub-type is still an incomplete undo), and CAMERA
//! (RemoveCamera has auto-promotion semantics a blind drop+re-add would disturb).
//! Re-deriving painter needs the per-sub-type rollback that is Facet-2 work; until
//! then the caller full-re-derives those (D51: never a silent corruption).
static bool DropChunkByCategory( IJob& pJob, ChunkCategory cat, const char* name )
{
	switch( cat ) {
		case ChunkCategory::Material: return pJob.RemoveMaterial( name );
		case ChunkCategory::Geometry: return pJob.RemoveGeometry( name );
		case ChunkCategory::Object:   return pJob.RemoveObject  ( name );
		case ChunkCategory::Light:    return pJob.RemoveLight   ( name );
		case ChunkCategory::Modifier: return pJob.RemoveModifier( name );
		default: return false;   // Painter (mixed reversibility -- scalar_painter), Camera, Function, ...
	}
}

//! 87 step 3b: the DOCUMENT-WIDE cap on SYNTHESIZED OBJECT ENTRIES -- every object an
//! expansion creates that no chunk of its own name declares, summed over every
//! `source` chunk in the document.
//!
//! ENTRIES, not instances, and that is the whole point of the number.  A chunk's
//! own `count_u * count_v <= 10,000,000` bounds how many times IT repeats, which was
//! an adequate proxy while every repetition was exactly one flat object.  A subtree
//! instance produces `count x subtree size` entries, so the per-generator cap stops
//! bounding what actually reaches the TLAS, the luminary list, `parentByName` and
//! every per-frame walk.  Same magnitude as the per-generator cap it subsumes, so no
//! scene that derives today can hit it.
static const long long kMaxSynthesizedEntries = 10000000LL;

//! 87 step 3a: the parameters that say WHERE a node is and WHAT IT IS CALLED,
//! as opposed to what it IS.  These are NEVER inherited through `source`.
//!
//! This is the choice that makes `position` on an instancing chunk mean "where
//! the copy goes" instead of "where the copy goes relative to wherever the
//! original happened to be": the source's local transform is DROPPED, not
//! composed.  `name` and `parent` are the instance's own for the same reason --
//! an instance is a node in its own right, placeable anywhere in the tree --
//! and `source` itself is consumed by the expansion.
//!
//! doc 89 slice C adds `mirror`.  A mirror is part of a node's own LOCAL TRANSFORM --
//! it composes innermost in `P * O * Stretch * Scale * Mirror` -- so it belongs with
//! `position` / `orientation` / `scale`, which are dropped for exactly the reason given
//! above.  Inheriting it instead would make `standard_object { name right_wing  source
//! left_wing  mirror x }` DOUBLE-mirror any further copy of `right_wing`, and would make
//! an un-mirrored instance of a mirrored source silently come out reflected.  It also
//! keeps the subtree right without a second rule: descendants are built from their OWN
//! chunks and composed through the mirrored parent's world matrix, so they reflect
//! exactly once -- a child that inherited the root's `mirror` would reflect twice.
//!
//! 87 step 3c adds `count_u` / `count_v` to the list: a count says how many times THIS
//! chunk repeats, so inheriting one would make a copy of a repetition repeat again.
//! Belt-and-braces rather than the live guard -- `ChunkCarriesCounts` refuses a counted
//! chunk as a `source` or as a subtree member outright -- but the two must not disagree.
static bool IsInstanceOwnParam( const std::string& pname )
{
	return pname == "name"        || pname == "parent"      || pname == "source"
	    || pname == "position"    || pname == "orientation" || pname == "quaternion"
	    || pname == "matrix"      || pname == "scale"      || pname == "mirror"
	    || pname == "count_u"     || pname == "count_v";
}

//! 87 step 3c: does this chunk carry a repetition count?  PRESENCE selects the array form,
//! never the VALUE -- `count_u 1` is `I[0,0]`, not `I`.  Keying on the value would make the
//! entry NAMES depend on a number that may be an `expr(...)` of a `let`, so an author's
//! `parent I[0,0]` would silently dangle when a constant changed from 2 to 1.
static bool ChunkCarriesCounts( const Node* c )
{
	std::string v;
	return ( ParamValue( c, "count_u", v ) && !v.empty() ) || ( ParamValue( c, "count_v", v ) && !v.empty() );
}

//! 87 step 3c: the ENTRY name of ONE repetition's root -- `I` when the chunk carries no
//! counts, `I[i,j]` when it does.  `<name>[i,j]` is the spelling the retired
//! `instance_array` generator used, kept deliberately so an objectmap legend, a saved
//! isolate name or a `parent` line written against one of its grids means the same thing
//! now that 87 step 3d has replaced it with a counted `source`.
//!
//! BUILT WITH `std::string`, NOT A FIXED BUFFER, AND THAT IS LOAD-BEARING.  A chunk name
//! is unbounded, and the collision scan's whole argument rests on this function being
//! INJECTIVE in (i,j) ("distinct (i,j) give distinct bases by construction" -- see the
//! per-repetition scan).  A truncating `snprintf` into `char[256]` breaks exactly that: a
//! 253-character name makes `[0,0]` and `[0,1]` both truncate to `<name>[0`, the scan sees
//! two names it believes distinct, and the SECOND repetition then fails at AddItem with a
//! message blaming an apply failure -- with the first already applied.  Longer still and
//! the `[i,j]` suffix vanishes entirely, collapsing every repetition onto one name.
static std::string InstanceBaseName( const std::string& instName, bool counted, int i, int j )
{
	if( !counted ) return instName;
	return instName + "[" + std::to_string( i ) + "," + std::to_string( j ) + "]";
}

//! Is `role` a chunk type whose Finalize registers a scene-graph OBJECT that can
//! carry a `parent`?  Four parsers call `IJob::SetObjectParent`: `standard_object`,
//! `csg_object`, and the two area-light sugars `rect_light` / `shape_light`, each of
//! which synthesizes one object (plus three helper entities named after it).
//!
//! 87 step 3b walks a source's SUBTREE, so this set decides what a subtree can
//! CONTAIN -- and a `rect_light` parented to an assembly is the motivating case for
//! parenting a light in the first place (see SCENE_CONVENTIONS).  3a scanned only
//! `standard_object` / `csg_object`, which is why a source whose only child was a
//! `rect_light` slipped past its has-children refusal and instanced root-only,
//! silently dropping the lamp.
//!
//! ⚠ FOUR PARSERS, BUT A FIFTH ROUTE.  `gltf_import` also produces objects --
//! `Job::ImportGLTFScene` calls `AddObjectMatrix` per mesh primitive
//! (GLTFSceneImporter.cpp) and registers names built from the glTF hierarchy,
//! `<name_prefix>.obj.n<node>.p<prim>`.  Those names appear in NO `name` param
//! anywhere in the document, so NOTHING role-based can see them: this predicate,
//! `BuildObjectChunkIndex`, and every derive-time guard reading them are blind to
//! that whole slice of the live object keyspace.  A guard that assumes "the
//! document's `name` params ARE the object keyspace" is therefore incomplete by
//! construction, not by oversight -- which is why the GUI's outliner keeps its own
//! live-keyspace defence (SceneEditController::BuildObjectTreeSeedsLocked_ PASS 1b)
//! instead of relying on a derive-time refusal.
static bool RoleDeclaresGraphObject( const std::string& role )
{
	return role == "standard_object" || role == "csg_object"
	    || role == "rect_light"      || role == "shape_light";
}

//! The entry name a nameless chunk of `role` will REGISTER, or empty if the role
//! requires a name and refuses without one.
//!
//! This exists because the document keyspace and the LIVE MANAGER keyspace used to
//! disagree, and the disagreement was silent.  `BuildObjectChunkIndex` indexed only
//! chunks that SPELL a `name`, while `StandardObjectAsciiChunkParser::Finalize`
//! (ChunkParserRegistry.cpp, `bag.GetString( "name", "noname" )`) and its
//! `csg_object` twin DEFAULT the live entry to `noname`.  So a nameless chunk
//! registered a real object the collision scan could not see -- and an instancing
//! chunk explicitly named `noname` then coexisted with it, undiagnosed, two
//! authored things claiming one name in the LIVE object keyspace.
//!
//! WHAT THAT ACTUALLY BROKE was the GUI's OUTLINER FOLD, not name-addressed
//! editing.  `DocFindByNameAnyRole` skips every chunk with an empty
//! `ChunkNamePath`, so a nameless chunk was never document-addressable in the
//! first place and never competed in that lookup.  The fold is where the live
//! keyspace IS the lookup: `BuildObjectTreeSeedsLocked_` folds `noname[0,0]` /
//! `noname[1,0]` into the live entry called `noname`, which here is the unrelated
//! nameless object -- so the whole counted array AND the chunk the author wrote
//! got no outliner row at all.  (Proven by disabling that pass's `unfold` set: the
//! tree loses both.)  87 step 4a defended it CONTROLLER-LOCAL; this closes it at
//! the source instead, for the chunks a document can see.
//!
//! Of the four object-creating roles only these TWO default; `rect_light` and
//! `shape_light` read `bag.GetString( "name", std::string() )` and REFUSE when it is
//! empty, so they can never produce an entry the author did not name.  Do not add
//! them here without checking that again.
//!
//! ⚠ This does NOT make the document keyspace complete -- `gltf_import` registers
//! objects under names no `name` param spells (see RoleDeclaresGraphObject above),
//! which is why PASS 1b stays load-bearing.
//!
//! ⚠ The literal is duplicated from the parser by necessity (no header is shared
//! between the CST and the parser registry).  `CstSourceInstanceTest` pins the
//! agreement BEHAVIOURALLY ("nameless: the premise") -- it derives a nameless chunk
//! and asserts the live manager holds exactly this name -- so a change to either
//! side reddens a test rather than silently re-opening the divergence.
static std::string RoleDefaultedEntryName( const std::string& role )
{
	if( role == "standard_object" || role == "csg_object" ) return "noname";
	return std::string();
}

//! An O(N) index of the document's OBJECT-CREATING chunks, built ONCE per derive
//! and shared by every `source` expansion in it (87 step 3a/3b).
struct ObjectChunkIndex
{
	//! name -> the item indices declaring it, restricted to the two roles a `source`
	//! may NAME (`standard_object` / `csg_object`).  MORE THAN ONE is an authored
	//! duplicate, which the collision scan reports naming both.  `override_object`
	//! is deliberately absent: it names an EXISTING object BY DESIGN, so counting
	//! it would report every override as a collision.
	std::map<std::string, std::vector<std::size_t> > byName;
	//! name -> declaring item indices over EVERY object-producing role
	//! (RoleDeclaresGraphObject).  This is the ENTRY-NAME keyspace, so it is what
	//! 3b's collision scan tests a synthesized name against: a `rect_light` named
	//! `I.C` collides with a clone called `I.C` just as surely as a
	//! `standard_object` would, and `byName` above cannot see it.
	std::map<std::string, std::vector<std::size_t> > entryByName;
	//! parent-name -> the ITEM INDICES of the chunks naming it as their `parent`,
	//! i.e. the nodes that HAVE children, over every object-producing role.  This
	//! is the edge set 3b's subtree walk follows.  Indices, not a bare flag,
	//! because `I source S parent S` makes the INSTANCING chunk its own source's
	//! only child -- an author's self-inflicted `parent` link, not the multi-node
	//! subtree 3b expands, and the two need different diagnostics.
	std::map<std::string, std::vector<std::size_t> > childrenOf;
	//! Every name a `csg_object` chunk binds as `obja` / `objb` -- the CSG OPERANDS.
	//! A DOCUMENT scan, deliberately, so the answer does not depend on WHERE the
	//! composite sits relative to the chunk asking: the live `IsWorldVisible()`
	//! state only becomes false once the `csg_object`'s Finalize has run, which
	//! would make the operand refusal fire or not fire purely on declaration order.
	std::set<std::string> csgOperands;
	//! Every name an `override_object` layer targets, and the item index of the
	//! first such layer.  A subtree member carrying one cannot be faithfully
	//! CLONED: the clone is built by re-`Finalize`ing the member's own chunk, and
	//! `override_object` is applied to the LIVE object afterwards by name -- so the
	//! clone would silently be the un-overridden pose.  Overlaying the override's
	//! params onto the merge is not equivalent either (a base `matrix` plus an
	//! override `position` COMPOSES on the live object and would be swallowed by
	//! `standard_object`'s matrix-wins precedence in a merged param list), so 3b
	//! refuses instead of approximating.
	std::map<std::string, std::size_t> overriddenNames;
};

static void BuildObjectChunkIndex( const std::vector<NodeRef>& items, ObjectChunkIndex& out )
{
	for( std::size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( !c || c->kind != NodeKind::Chunk ) continue;
		if( c->role == "override_object" ) {
			std::string nm;
			if( ParamValue( c.get(), "name", nm ) && !nm.empty() ) out.overriddenNames.insert( std::make_pair( nm, i ) );
			continue;
		}
		if( !RoleDeclaresGraphObject( c->role ) ) continue;
		std::string nm;
		const bool spelled = ( ParamValue( c.get(), "name", nm ) && !nm.empty() );
		if( !spelled ) {
			// NAMELESS.  It still REGISTERS an object for the two roles that default
			// the name, so it must be indexed under the name it will actually take --
			// otherwise the collision scan is blind to it and an instancing chunk
			// spelling that same name coexists with it undiagnosed.  Costs nothing on
			// the corpus: 0 of 404 tracked scenes have a nameless object chunk.
			nm = RoleDefaultedEntryName( c->role );
		}
		if( !nm.empty() ) {
			// `entryByName` is the COLLISION keyspace -- "what live entry name does
			// this chunk claim?" -- and a defaulted name claims one exactly as a
			// spelled one does, so it belongs here.
			out.entryByName[ nm ].push_back( i );
			// `byName` is the ADDRESSABILITY keyspace -- "which chunk does this name
			// RESOLVE to?" -- and it is read by `source` resolution
			// (ExpandSourceInstance, SourceChainOf) and by both ClonePlanBuilder
			// nested-source lookups.  A DEFAULTED name must NOT enter it.  Feeding
			// it in would make `source noname` resolve to a chunk the editor cannot
			// address (`DocFindByNameAnyRole` skips an empty `ChunkNamePath`), and
			// would break with a misleading "no object of that name" the moment the
			// author gave that chunk the explicit name it never had.  Measured: it
			// turned `source noname` from REFUSED into legal.  So: SPELLED only.
			if( spelled && ( c->role == "standard_object" || c->role == "csg_object" ) )
				out.byName[ nm ].push_back( i );
		}
		std::string pr;
		if( ParamValue( c.get(), "parent", pr ) && !pr.empty() && pr != "none" ) out.childrenOf[ pr ].push_back( i );
		if( c->role == "csg_object" ) {
			std::string op;
			if( ParamValue( c.get(), "obja", op ) && !op.empty() && op != "none" ) out.csgOperands.insert( op );
			op.clear();
			if( ParamValue( c.get(), "objb", op ) && !op.empty() && op != "none" ) out.csgOperands.insert( op );
		}
	}
}

//! 1-based position of item `idx` among the document's CHUNK items; trivia
//! (comments, blank lines) do not count.  Diagnostics quote THIS, never the raw
//! item index -- an author counts chunks in the file they wrote, and a file with
//! three comments in it puts the 6th chunk at item 16.
static unsigned int ChunkOrdinal( const std::vector<NodeRef>& items, std::size_t idx )
{
	unsigned int n = 0;
	for( std::size_t i = 0; i < items.size() && i <= idx; ++i )
		if( items[i] && items[i]->kind == NodeKind::Chunk ) ++n;
	return n;
}

//! 87 step 3c: EVERY WAY A CHUNK CAN NAME A COUNTED CHUNK BY ITS BARE NAME, refused
//! here with the real cause spelled out.
//!
//! A chunk carrying `count_u` / `count_v` produces `I[0,0]`, `I[1,0]`, ... and NEVER an
//! entry called `I`.  `source I` already has its own dedicated refusal (see
//! ExpandSourceInstance and ClonePlanBuilder::SourceSubtree); the two OTHER references
//! an author can write did not, and each landed on a pre-existing diagnostic that
//! enumerates causes NONE of which is the real one:
//!
//!   `parent I`          -> "A `parent` must be a DECLARED-EARLIER object; must not be this
//!                          object; must not already be one of its descendants; and must not
//!                          be a CSG operand" -- four causes, all false.  `I` IS declared
//!                          earlier, is not this object, is nobody's descendant and is not an
//!                          operand; it simply is not an entry name.
//!   `override_object I` -> "target `I` not found in scene.  Possible causes: (a) the chunk
//!                          appears BEFORE the chunk that creates the target (b) ... deleted
//!                          (c) ... typo" -- again none of them.
//!
//! Both diagnostics are structurally unable to say better: they are emitted from a parser
//! `Finalize`, which sees the live manager and not the DOCUMENT, so it cannot know that the
//! name belongs to a counted chunk.  The document scan can, and it also names the working
//! spelling -- `parent I[0,0]` is the intended idiom, not an error at all.
//!
//! Scanned over EVERY chunk role, not just the graph-object ones: any role that declares a
//! `parent` reaches the identical dead end, and a `source` chunk's `parent` is the same line
//! on the same role.
//!
//! `byName` is the right keyspace: only `standard_object` may carry counts (the parser
//! refuses counts without a `source`), and `byName` holds exactly the `standard_object` /
//! `csg_object` declarations.  `front()` because a duplicate name is separately reported.
static bool RefuseBareReferencesToCountedChunks( const std::vector<NodeRef>& items,
                                                const ObjectChunkIndex& index,
                                                std::vector<std::string>& diags )
{
	const std::size_t before = diags.size();
	for( std::size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( !c || c->kind != NodeKind::Chunk ) continue;
		// The name this chunk points AT, and the parameter it points with.
		std::string ref, param;
		if( c->role == "override_object" ) {
			if( !ParamValue( c.get(), "name", ref ) || ref.empty() ) continue;
			param = "name";
		} else {
			if( !ParamValue( c.get(), "parent", ref ) || ref.empty() || ref == "none" ) continue;
			param = "parent";
		}
		const std::map<std::string, std::vector<std::size_t> >::const_iterator ni = index.byName.find( ref );
		if( ni == index.byName.end() || ni->second.empty() ) continue;   // not a declared object at all -> the existing diagnostics are correct
		if( !ChunkCarriesCounts( items[ ni->second.front() ].get() ) ) continue;
		const std::string first = InstanceBaseName( ref, /*counted*/true, 0, 0 );
		diags.push_back( c->role + " (chunk " + std::to_string( ChunkOrdinal( items, i ) ) + "): `" + param + " " + ref
			+ "` names a chunk carrying `count_u` / `count_v`, so `" + ref + "` is a REPETITION -- it produces one "
			  "entry per (i,j) (`" + first + "`, `" + InstanceBaseName( ref, true, 1, 0 ) + "`, ...) and NO entry "
			  "called `" + ref + "` at all.  Name the repetition you mean: `" + param + " " + first + "`." );
	}
	return diags.size() == before;
}

//! The `source` chain of chunk `idx`, NEAREST SOURCE FIRST: the chunk it instances,
//! the chunk THAT one instances, and so on.  Empty when the chunk carries no
//! `source`.  Terminates by the declare-earlier rule (every link points strictly
//! backwards in the document, so a cycle is impossible by construction); the depth
//! cap is a belt against a future apply path that admits a link this walk never saw.
//!
//! Returns false on a link that does not resolve (no such chunk, or a forward
//! reference) and on a chain deeper than the cap -- the CALLER turns that into its
//! own diagnostic, because the two entry points want different wording.
static bool SourceChainOf( const std::vector<NodeRef>& items, const ObjectChunkIndex& index,
                           std::size_t idx, std::vector<std::size_t>& chain )
{
	chain.clear();
	std::string src;
	if( !ParamValue( items[idx].get(), "source", src ) || src.empty() || src == "none" ) return true;
	std::size_t cursor = idx;
	for( unsigned int guard = 0; guard < 256u; ++guard ) {
		const std::map<std::string, std::vector<std::size_t> >::const_iterator si = index.byName.find( src );
		if( si == index.byName.end() || si->second.empty() || si->second.front() >= cursor ) return false;
		cursor = si->second.front();
		chain.push_back( cursor );
		src.clear();
		if( !ParamValue( items[cursor].get(), "source", src ) || src.empty() || src == "none" ) return true;
	}
	return false;   // deeper than the cap
}

//! Merge a chunk's OWN parameters with everything it inherits through `source`, and
//! answer the chunk type the result must be built through.
//!
//! ONE function for the 3a collapse root and every 3b subtree clone, which is what
//! makes an instance NESTED inside an instanced subtree inherit by exactly the rule
//! the top-level instancing chunk does.  Root-first, so a nearer chunk's binding
//! overrides a farther one's and the chunk's OWN parameters override everything.
//! Insertion order is kept so the synthesized parameter list is deterministic.
//!
//! `IsInstanceOwnParam` is applied to the INHERITED chunks only: the source's name,
//! parent and local transform are DROPPED, never composed (see its own header).  The
//! chunk's own params come through whole except `source` / `count_u` / `count_v`, which
//! the expansion consumes.
//!
//! `iv` (87 step 3c) applies PER-INSTANCE EXPRESSIONS -- and it applies to THIS CHUNK'S
//! OWN PARAMS ONLY, never to the inherited chain.  That is the same scope the retired
//! `instance_array` generator had: `i`/`j`/`u`/`v` vary the parameters of the chunk
//! that carries the counts, and a DESCENDANT's parameters are not per-instance
//! variable.  (Nor could they be by accident: a subtree member is an ordinary
//! `standard_object`, and PASS-1 rejects `position expr(i) 0 0` on one because a
//! multi-component value with an embedded expr is not a whole-value expr.)
//!
//! Returns false when a per-instance eval failed; the caller must stop.
static bool MergeChunkParams(
	const std::vector<NodeRef>& items,
	const LetBindings& lets,
	std::size_t chunkIdx,
	const std::vector<std::size_t>& chain,
	std::vector<std::string>& order,
	std::map<std::string, std::string>& merged,
	std::string& targetRole,
	std::vector<std::string>& diags,
	const InstanceVars* iv = nullptr )
{
	order.clear();
	merged.clear();
	auto put = [&order, &merged]( const std::string& k, const std::string& v ) {
		if( merged.find( k ) == merged.end() ) order.push_back( k );
		merged[ k ] = v;
	};
	for( std::size_t ci = chain.size(); ci-- > 0; ) {
		std::vector<std::pair<std::string,std::string> > sp;
		(void)ChunkParamPairs( items[ chain[ci] ], lets, diags, sp );   // no `iv`: the inherited chain is not per-instance
		for( std::size_t k = 0; k < sp.size(); ++k )
			if( !IsInstanceOwnParam( sp[k].first ) ) put( sp[k].first, sp[k].second );
	}
	std::vector<std::pair<std::string,std::string> > own;
	if( !ChunkParamPairs( items[ chunkIdx ], lets, diags, own, iv ) ) return false;
	for( std::size_t k = 0; k < own.size(); ++k )
		if( own[k].first != "source" && own[k].first != "count_u" && own[k].first != "count_v" )
			put( own[k].first, own[k].second );

	// The clone is built through the SOURCE's own chunk type -- so a csg_object
	// source yields a csg_object, `obja` / `objb` / `operation` come along
	// UNCHANGED, and the two composites SHARE their operands.
	//
	// SHARING IS CORRECT HERE AND IT IS NOT OBVIOUS: an operand's matrix is
	// CSG-LOCAL.  `CSGObject::IntersectRay` transforms the world ray into the
	// COMPOSITE's own frame before handing it to either operand, so the operand's
	// own matrix is read relative to whichever composite is asking -- two
	// composites at different world poses both get the right shape out of one
	// shared operand.  Deep-cloning the operands would cost N copies and would
	// also have to invent names for them.
	//
	// SHARING IS NOT FREE OF CONSEQUENCES, and an earlier draft of this comment
	// claimed "no behavioural difference", which was wrong.  Being an operand is
	// recorded ON the operand, and 3b makes that an N-way relation: `CSGObject`
	// used to express it with `SetWorldVisible(false)`, a plain bool, which its
	// destructor and its re-assign path unconditionally set back to TRUE -- so
	// removing ONE of the N composites (reachable live through the console's
	// `remove object`) resurrected an operand that the other N-1 were still
	// consuming.  It then rendered as a standalone shape, and became parentable,
	// `ObjectManager::SetObjectParent` identifying an operand as "hidden and has
	// geometry".  Consumption is therefore a COUNT (IObjectPriv::AddConsumer),
	// orthogonal to visibility, and that is what makes the sharing below safe.
	// (An operand can never itself BE a subtree member: `Job::AddCSGObject`
	// refuses an operand that has a parent or children, and
	// `ObjectManager::SetObjectParent` refuses it from the other side -- so the
	// transitive-`parent` definition of "subtree" excludes operands
	// self-consistently, and nothing in the walk below can reach one.)
	targetRole = items[ chain.empty() ? chunkIdx : chain.back() ]->role;
	return true;
}

//! Build + apply ONE synthesized node from a merged parameter set: force its `name`
//! (and, for a 3b clone, its `parent`), validate every param against the TARGET
//! chunk's descriptor, then run that chunk type's own `Finalize`.
//!
//! `parentOverride` is null for the 3a collapse root (whose `parent` is the
//! instancing chunk's own, already in `merged`) and non-null for a 3b clone, whose
//! parent must be the CLONE of its source-side parent rather than the original.
static bool ApplySynthesizedNode(
	const std::map<std::string, const IAsciiChunkParser*>& registry,
	IJob& pJob,
	std::vector<std::string>& order,
	std::map<std::string, std::string>& merged,
	const std::string& targetRole,
	const std::string& entryName,
	const std::string* parentOverride,
	const std::string& who,
	const std::string& srcName,
	std::vector<std::string>& diags )
{
	const std::map<std::string, const IAsciiChunkParser*>::const_iterator ti = registry.find( targetRole );
	if( ti == registry.end() || !ti->second ) {
		diags.push_back( who + ": `source " + srcName + "` resolves to a `" + targetRole + "` chunk, which the parser registry does not know" );
		return false;
	}
	const IAsciiChunkParser* targetParser = ti->second;

	if( parentOverride ) {
		if( merged.find( "parent" ) == merged.end() ) order.push_back( "parent" );
		merged[ "parent" ] = *parentOverride;
	}

	// Every merged parameter must be one the TARGET's descriptor accepts.  The one
	// way this fires in practice: a csg_object source plus a `matrix` / `quaternion`
	// / `scale` on the instancing chunk, which a csg_object chunk has no way to
	// express.  Name the offenders instead of letting DispatchChunkParameters emit
	// an undeclared-parameter error with no mention of `source`.
	{
		std::set<std::string> accepted;
		const ChunkDescriptor& td = targetParser->Describe();
		for( std::size_t k = 0; k < td.parameters.size(); ++k ) accepted.insert( td.parameters[k].name );
		std::string rejected;
		for( std::size_t k = 0; k < order.size(); ++k )
			if( !accepted.count( order[k] ) ) { if( !rejected.empty() ) rejected += ", "; rejected += "`" + order[k] + "`"; }
		if( !rejected.empty() ) {
			diags.push_back( who + ": `source " + srcName + "` resolves to a `" + targetRole + "` node, and an "
				"instance is built through the SOURCE's own chunk type -- so it accepts exactly that chunk's "
				"parameters, which do not include " + rejected + "." );
			return false;
		}
	}

	IAsciiChunkParser::ParamsList plist;
	plist.push_back( String( ( std::string( "name " ) + entryName ).c_str() ) );
	for( std::size_t k = 0; k < order.size(); ++k ) {
		if( order[k] == "name" ) continue;   // already emitted first
		std::string line = order[k];
		if( !merged[ order[k] ].empty() ) { line += ' '; line += merged[ order[k] ]; }
		plist.push_back( String( line.c_str() ) );
	}

	ParseStateBag bag( &targetParser->Describe() );
	if( !DispatchChunkParameters( targetParser->Describe(), bag, plist ) ) {
		diags.push_back( who + ": the synthesized `" + targetRole + "` `" + entryName + "` has invalid params (see log)" );
		return false;
	}
	if( !targetParser->Finalize( bag, pJob ) ) {
		diags.push_back( who + ": `source " + srcName + "` expanded, but applying the synthesized `" + targetRole
			+ "` `" + entryName + "` failed (see log)" );
		return false;
	}
	return true;
}

//! 87 step 3b -- the PLAN for one `source` expansion's subtree: one item per node in
//! the source's subtree, PRE-ORDER, so a parent is always applied before its child
//! (which is what `ObjectManager::SetObjectParent`'s declare-before-use guard needs)
//! and siblings come out in the order they have in the tree being copied (which is what
//! step 2's per-parent registration-serial sort reads as child display order).
//!
//! SIBLING ORDER IS THE ORIGINAL'S ORDER, not "document order" flatly.  For a node
//! whose siblings are all authored, the two are the same thing.  For a node that is
//! itself an INSTANCE they are not: the entries ITS expansion made were registered at
//! its own document position, while anything written `parent <it>` must be declared
//! BELOW it -- so the synthesized siblings precede the authored ones, and the walk
//! descends the `source` chain BEFORE walking document children to match.
//!
//! BUILT BEFORE ANYTHING IS APPLIED.  The collision scan has to be able to name
//! EVERY entry the expansion would create while none of them exists yet -- both
//! against the live manager and against the document -- so a colliding scene refuses
//! with nothing half-applied rather than failing on the fourth of nine clones.
//!
//! THE PLAN IS NAMED RELATIVE TO THE INSTANCE ROOT, and 87 step 3c is why.  A plan item
//! records the copied node's SOURCE-SIDE entry name and its PARENT's source-side entry
//! name; the actual entry names are `<base>.<srcEntryName>` for a base that is `I` in the
//! single-instance form and `I[i,j]` for one repetition of a counted one.  Building the
//! plan once and composing names per repetition is what keeps a count from re-walking the
//! document N times -- and it is why nothing in the walk below knows the instance name.
struct SubtreeClonePlanItem
{
	std::size_t chunkIdx;      //!< the source-subtree chunk this clone is a copy of
	std::string srcEntryName;  //!< that node's ENTRY name in the source tree (itself qualified, when it is a nested instance's clone)
	std::string parentRel;     //!< the source-side entry name of the node this one is parented to; EMPTY means the instance root
};

//! The recursive walk that fills a SubtreeClonePlan.
//!
//! NAMING -- ONE LEVEL OF QUALIFICATION, and the recursion is what makes that
//! literally true.  A clone is `I` + "." + the copied node's ENTRY name, never a
//! path: `I.C` for a direct child, `I.D` (not `I.C.D`) for its grandchild, because
//! `D`'s entry name in the source tree is just `D`.  The qualification advances by
//! ONE step only when the walk crosses a `source` boundary, because THAT is where
//! the entry name it is copying is itself qualified: instancing `S`, which contains
//! an instance `I1` whose own expansion produced `I1.A`, as `I2` gives `I2.I1` and
//! `I2.I1.A` -- one level added to each, on top of a name that already had one.
//!
//! `.` and not `/`: `ChunkNamePath` keys the document index as `role + "/" + name`,
//! so a `/` in an entry name would collide with that separator.
//!
//! THE QUALIFICATION IS CARRIED AS COMPONENTS, not as one pre-joined string, and that
//! is what makes `ChildKeysOf` below able to enumerate the document keys under which
//! a SYNTHESIZED entry's children can be declared.  See its header.
struct ClonePlanBuilder
{
	const std::vector<NodeRef>&  items;
	const ObjectChunkIndex&      index;
	const std::string&           who;
	std::size_t                  instIndex;     //!< the instancing chunk's own item index -- every subtree member must precede it
	IObjectManager*              objMgr;
	std::vector<std::string>&    diags;
	std::vector<SubtreeClonePlanItem>& plan;
	std::set<std::size_t>        path;          //!< chunk indices on the CURRENT clone path -- a revisit is a recursive definition

	//! Clone the subtree of the ENTRY produced by chunk `srcChunkIdx`, whose clone's
	//! RELATIVE name is `parentRel` ("" at the instance root).  `ctxParts` qualifies the
	//! entry names of `srcChunkIdx`'s own document children (empty at the top of a source tree).
	bool SourceSubtree( std::size_t srcChunkIdx, const std::string& parentRel, const std::vector<std::string>& ctxParts, int depth );
	//! Clone chunk `chunkIdx` (and everything under it) as a child of the clone whose
	//! relative name is `parentRel`.
	bool ClonedEntry( std::size_t chunkIdx, const std::vector<std::string>& ctxParts, const std::string& parentRel, int depth );

	//! `ctxParts[from..]` joined with dots and a trailing dot, or "" when empty.
	static std::string JoinFrom( const std::vector<std::string>& ctxParts, std::size_t from )
	{
		std::string s;
		for( std::size_t i = from; i < ctxParts.size(); ++i ) { s += ctxParts[i]; s += '.'; }
		return s;
	}

	//! EVERY `parent` key under which a document node can declare itself a child of the
	//! entry `JoinFrom(ctxParts,0) + ownName`, paired with the ctxParts PREFIX LENGTH
	//! that qualifies whatever is found there.
	//!
	//! THE WALK IS OVER THE DOCUMENT AND THE LIVE TREE IS NOT THE SAME SHAPE, which is
	//! the whole reason this function exists.  Cloning the entry `I1.B` (`ownName` = `B`,
	//! `ctxParts` = {`I1`}), the live children of `I1.B` come from TWO different document
	//! keys and both are real:
	//!   * `parent B`   -- a document child of the ORIGINAL `B`.  `I1`'s own expansion
	//!                     copied it as `I1.Y`, so this copy must be `I2.I1.Y`: the find
	//!                     keeps the WHOLE prefix `I1.`.
	//!   * `parent I1.B`-- a document node parented onto the SYNTHESIZED entry directly.
	//!                     Its entry name is its own (`X`), so this copy is `I2.X`: the
	//!                     find consumes the prefix `I1.` and keeps NOTHING.
	//! Every intermediate split is a real key too once the nesting is deeper than one
	//! (`I2.I1.B` also answers to `I1.B` with `I2.` kept, and to `B` with `I2.I1.` kept).
	//! A FIRST-MATCH FALLBACK CHAIN IS NOT A SUBSTITUTE, and this is the sharp edge:
	//! the two keys above can be non-empty AT THE SAME TIME, so "try the entry name,
	//! else the chunk name" answers with one branch and silently DROPS the other.
	//! The `override_object` lookup below is a genuine first-match search because it
	//! only needs to know WHETHER any layer exists; the child and generator walks need
	//! the UNION, and get it.
	void ChildKeysOf( const std::vector<std::string>& ctxParts, const std::string& ownName,
	                  std::vector<std::pair<std::string, std::size_t> >& outKeys ) const
	{
		outKeys.clear();
		for( std::size_t keep = 0; keep <= ctxParts.size(); ++keep )
			outKeys.push_back( std::make_pair( JoinFrom( ctxParts, keep ) + ownName, keep ) );
	}

	bool DepthOk( int depth )
	{
		if( depth <= 64 ) return true;
		diags.push_back( who + ": the source subtree nests deeper than 64 levels of instancing; refusing to expand it" );
		return false;
	}
};

bool ClonePlanBuilder::SourceSubtree( std::size_t srcChunkIdx, const std::string& parentRel,
                                     const std::vector<std::string>& ctxParts, int depth )
{
	if( !DepthOk( depth ) ) return false;
	std::string srcOwnName;
	ParamValue( items[srcChunkIdx].get(), "name", srcOwnName );
	// 87 step 3c: a chunk that carries counts produces N ENTRIES, not one node, so there is
	// no single thing for a `source` to be a copy OF -- taking the first of them is the
	// silent partial copy every other refusal in this walk exists to prevent.  Reached for
	// the instancing chunk's own source and for every chunk further down its `source` chain.
	if( ChunkCarriesCounts( items[srcChunkIdx].get() ) ) {
		diags.push_back( who + ": `" + srcOwnName + "` carries `count_u` / `count_v`, so it is a REPETITION -- it "
			"produces one entry per (i,j), not a single node, and a copy of it would silently be a copy of just "
			"one.  Instance the node the counts repeat, or write the counts on THIS chunk instead." );
		return false;
	}
	// DEFENSIVE, and deliberately kept -- and, since the derive-continuation change
	// below (bug-fix wave, 2026-08-28), no longer provably unreachable, so read this
	// note as history rather than a live unreachability proof.  Reaching it needs a
	// `source` hop back onto a chunk already on the clone path, i.e. a chunk `K` that
	// instances an ancestor of itself in the document tree.  `K`'s OWN expansion, which
	// runs EARLIER in PASS-2 (a `source` must name something declared above), walks
	// that same ancestor down to `K` and refuses there first -- `ClonedEntry`'s revisit
	// guard or its declare-before-use one -- so THIS guard was provably unreachable
	// back when DeriveToJob's outer loop stopped at the first Finalize/expansion
	// failure (K's own refusal would have ended the whole derive before any LATER
	// pending chunk -- "the instance that would trip THIS one" -- was ever attempted).
	// DeriveToJob's PASS-2 loop now keeps going after a failed chunk instead of
	// stopping (every later chunk gets its own chance + its own diagnostic), which is
	// exactly the "future diagnostic pass that collected every refusal instead" this
	// comment used to anticipate -- so a LATER pending chunk can now be REACHED (its
	// own expansion attempted) after `K` refuses, where before it never was.  That does
	// NOT by itself mean THIS specific guard is reached, though: `K`'s own refusal
	// leaves `K` producing no object, and the ordinary "does the name I'm sourcing from
	// resolve to an object" probes elsewhere (`ExpandSourceInstance`'s own precondition;
	// `ClonedEntry`'s own "member produced no object" check) refuse the later chunk
	// FIRST, on a plainer diagnostic, before its walk ever gets far enough to attempt
	// re-visiting `K`'s own subtree.  Reaching THIS guard for real additionally needs
	// the failed name to still resolve to SOME object despite `K`'s own failure -- e.g.
	// an EARLIER chunk already registered an object under that same name, so `K`'s own
	// attempt to add one never reached the manager at all (Job's add helpers diagnose-
	// and-refuse a duplicate name outright; a later `GetItem` lookup on that name finds
	// the EARLIER chunk's object, not `K`'s).  So: no longer provably UNreachable (the
	// premise that proved it so is gone), but the mechanism by which it becomes
	// reachable is narrower than "any later pending chunk" -- do not read this note as
	// a full reachability proof either way.  THIS revisit guard (the `path.insert`
	// check just below) is itself the genuinely-pinned one: CstSourceInstanceTest's
	// "a recursion through a TRANSITIVE descendant is caught by the walk's revisit
	// guard" fixture is a positive fire ("this scene is what proves that guard is not
	// dead code"), with "revisit-diamond" / "revisit-sibling" as its matching non-
	// false-positive controls (~2533-2650).  `ClonedEntry`'s "member produced no
	// object" guard (its `objMgr->GetItem` probe, a few hundred lines down) has only a
	// NEGATIVE assertion on record -- the count-collision fixture (~3305) checks that
	// ITS message does NOT appear on a scene it does not apply to -- so do not cite
	// that guard as pinned until a positive fixture exists for it.
	if( !path.insert( srcChunkIdx ).second ) {
		diags.push_back( who + ": expanding this instance would copy `" + srcOwnName + "` into its own subtree -- a "
			"recursive definition with no fixed point.  The usual cause is a `parent` line that puts a node inside "
			"the very subtree it instances." );
		return false;
	}
	// THE SOURCE CHAIN FIRST, THEN THE DOCUMENT CHILDREN -- and the order is not
	// arbitrary.  If this chunk is itself an INSTANCE, the entries ITS expansion made
	// were registered at ITS OWN document position, whereas anything written `parent
	// <it>` must be declared BELOW it (declare-before-use).  So in the ORIGINAL tree
	// the synthesized siblings always precede the authored ones, and a walk that
	// emitted document children first would hand the copy the reverse of the order the
	// thing it copies has.  Sibling order is what step 2's per-parent registration-serial
	// sort shows as child display order, so "the copy looks like the original" includes
	// this.
	//
	// The qualification advances by one across the `source` boundary, because THAT is
	// where the entry names being copied are themselves qualified.
	std::string nested;
	if( ParamValue( items[srcChunkIdx].get(), "source", nested ) && !nested.empty() && nested != "none" ) {
		const std::map<std::string, std::vector<std::size_t> >::const_iterator ni = index.byName.find( nested );
		if( ni != index.byName.end() && !ni->second.empty() && ni->second.front() < srcChunkIdx ) {
			std::vector<std::string> deeper = ctxParts;
			deeper.push_back( srcOwnName );
			if( !SourceSubtree( ni->second.front(), parentRel, deeper, depth + 1 ) ) return false;
		}
	}
	// The source entry's own document children.
	//
	// ONE key here, not `ChildKeysOf`'s union, and the asymmetry with `ClonedEntry` is
	// the point: this chunk's own name is NEVER an entry under a qualification.  It is
	// either the top of the chain (whose entry name IS `srcOwnName`, so `ctxParts` is
	// empty and the union would degenerate to this one key anyway) or a chunk reached
	// ACROSS a `source` boundary, whose identity is subsumed into the clone's own name and which
	// therefore mints no entry of its own for anything to be parented to.  Every entry
	// that does get minted is minted by `ClonedEntry`, which does take the union.
	const std::map<std::string, std::vector<std::size_t> >::const_iterator kids = index.childrenOf.find( srcOwnName );
	if( kids != index.childrenOf.end() ) {
		for( std::size_t k = 0; k < kids->second.size(); ++k )
			if( !ClonedEntry( kids->second[k], ctxParts, parentRel, depth + 1 ) ) return false;
	}
	// `path` IS A PATH, NOT A VISITED-SET, and this line is the whole difference.  Deleting
	// it turns the revisit guard above from "this chunk is on the path from the instancing
	// node to here" -- a real cycle -- into "this chunk was reached at some point during
	// this expansion", which refuses ordinary authoring: TWO SIBLING INSTANCES OF ONE
	// SOURCE inside one instanced subtree (`P source L`, `Q source L`, both `parent A`,
	// then `I source A`) hop onto `L` twice in the same expansion along two DISJOINT
	// paths, and the second hop would be reported as a recursive definition while the
	// whole instance subtree was dropped.  Pinned by CstSourceInstanceTest's
	// "revisit-sibling" fixture; `ClonedEntry`'s matching erase is pinned by
	// "revisit-diamond".
	path.erase( srcChunkIdx );
	return true;
}

bool ClonePlanBuilder::ClonedEntry( std::size_t chunkIdx, const std::vector<std::string>& ctxParts,
                                   const std::string& parentRel, int depth )
{
	if( !DepthOk( depth ) ) return false;
	std::string ownName;
	if( !ParamValue( items[chunkIdx].get(), "name", ownName ) || ownName.empty() ) {
		diags.push_back( who + ": a `" + items[chunkIdx]->role + "` in the source subtree has no `name`, so the copy "
			"has nothing to be called" );
		return false;
	}
	// The revisit guard runs BEFORE the declare-before-use one below, and the order is
	// load-bearing: the INSTANCING chunk is seeded onto the path, and it is also
	// trivially "not declared before itself", so a scene that puts a node inside the
	// subtree it instances would otherwise be told to move its instance further down
	// the file -- advice that cannot fix a recursive definition.
	if( !path.insert( chunkIdx ).second ) {
		diags.push_back( who + ": expanding this instance would copy `" + ownName + "` into its own subtree -- a "
			"recursive definition with no fixed point.  The usual cause is a `parent` line that puts a node inside "
			"the very subtree it instances." );
		return false;
	}
	// DECLARE-BEFORE-USE applies to the WHOLE subtree, not only to its root.  The
	// expansion runs at the instancing chunk's own document position, so a subtree
	// member declared LATER has not produced an object yet and cannot be copied --
	// and instancing "the part of the subtree that happens to precede me" is exactly
	// the silent partial copy 3a refused to make.  Its own message, because
	// "declared earlier but produced no object" would be a lie about the cause.
	if( chunkIdx >= instIndex ) {
		diags.push_back( who + ": the source subtree member `" + ownName + "` (chunk #"
			+ std::to_string( ChunkOrdinal( items, chunkIdx ) ) + ") is declared AFTER this instancing chunk, so it "
			"does not exist yet when the copy is made.  A whole subtree must be declared before anything instances "
			"it -- move the instance below the last member of the subtree." );
		return false;
	}
	// 87 step 3c: a MEMBER that carries counts is the same silent-partial-copy shape
	// `SourceSubtree` refuses for a counted SOURCE, from the other side: the member is N
	// entries in the original (`M[0,0]`, `M[1,0]`, ...) and the copy would carry one node
	// called `I.M`.  Refused rather than approximated; counts belong on the chunk doing
	// the instancing, which for a subtree is its ROOT.
	if( ChunkCarriesCounts( items[chunkIdx].get() ) ) {
		diags.push_back( who + ": the source subtree member `" + ownName + "` (chunk #"
			+ std::to_string( ChunkOrdinal( items, chunkIdx ) ) + ") carries `count_u` / `count_v`, so in the "
			"original it is one entry per (i,j) rather than a single node -- the copy would silently hold just "
			"one of them.  Counts belong on the chunk that instances the subtree, not on a member of it." );
		return false;
	}
	const std::string srcEntryName = JoinFrom( ctxParts, 0 ) + ownName;

	// THE KEY SET this entry answers to -- its own qualified entry name, its bare chunk
	// name, and every intermediate.  Read three times below (override layers, children,
	// generators), because all three ask the same question: "what in the document is
	// attached to the thing I am copying?"
	std::vector<std::pair<std::string, std::size_t> > keys;
	ChildKeysOf( ctxParts, ownName, keys );

	// An `override_object` layer is applied to the LIVE object by NAME, after its
	// base chunk -- so it never reaches a clone built by re-Finalizing that base
	// chunk.  Every spelling is refused: a layer on the ENTRY (which is what makes
	// the source node's pose what it is), a layer on the underlying CHUNK when
	// the entry is itself a nested clone (whose pose the layer likewise decided for
	// the original but not for this copy), and -- once the nesting is two deep -- the
	// intermediate names in between, which are entries in their own right.
	//
	// ONLY THE FIRST KEY (the fully-qualified entry name) WAS PROVABLY REACHABLE under
	// the OLD break-on-first-failure PASS-2 (see the derive-continuation note further
	// down at DeriveToJob's apply loop, bug-fix wave 2026-08-28); the rest were
	// defensive.  An override on a member's BARE chunk name is always caught by the
	// FIRST expansion that copies that member -- the one whose `ctxParts` is empty, so
	// its first key already IS the bare name -- and that expansion is necessarily
	// earlier in the document than any nested one.  Under the old break-on-first-
	// failure PASS-2, a refusal anywhere in or before that first expansion ended the
	// WHOLE derive, closing off one (not necessarily the only) route by which a
	// qualified copy of the same member could be planned before this lookup is reached
	// with keys[0] already spoken for.  PASS-2 now keeps going past a failed chunk, so
	// that particular closure no longer holds -- meaning "only keys[0] is reachable" is
	// no longer a proof, not that every OTHER key is now demonstrated reachable by some
	// specific route (this note does not attempt that derivation, and the guard three
	// lines below this block is written not to assume it: it names `ov->first`,
	// whichever key actually matched).  The union is kept whole (never trimmed to
	// `keys[0]`) because the CHILD walk below genuinely needs every key regardless.
	{
		std::map<std::string, std::size_t>::const_iterator ov = index.overriddenNames.end();
		for( std::size_t ki = 0; ki < keys.size() && ov == index.overriddenNames.end(); ++ki )
			ov = index.overriddenNames.find( keys[ki].first );
		if( ov != index.overriddenNames.end() ) {
			char pos[64];
			std::snprintf( pos, sizeof(pos), "chunk #%u", ChunkOrdinal( items, ov->second ) );
			// NAME THE ENTRY THE LAYER IS ACTUALLY ON -- `ov->first`, the key that
			// matched -- not `srcEntryName`, which is only `keys[0]`.  The two differ
			// whenever a non-zero key matches, and the message would then assert the
			// layer is on the fully-qualified entry when it is on a shorter name, i.e.
			// send the author to the wrong line to fix it.  A non-zero key match was
			// PROVABLY unreachable back when PASS-2 broke on the first failure
			// elsewhere in the document (see the block above); that proof no longer
			// holds (bug-fix wave, 2026-08-28), and no replacement proof of reachability
			// is claimed either -- `ov->first` is simply the correct expression
			// regardless of which key matched, so it costs nothing to have it be right.
			diags.push_back( who + ": the source subtree member `" + ov->first + "` has an `override_object` layer ("
				+ pos + ").  An override is applied to the LIVE object by name, after its base chunk, so a copy built "
				"from that base chunk would silently carry the UN-overridden pose.  Fold the override into the base "
				"chunk, or instance a subtree without one." );
			return false;
		}
	}

	// Resolve the SOURCE-SIDE entry through the manager.  It verifies the subtree member
	// actually produced an object -- a member whose own chunk failed would otherwise be
	// cloned from params nothing validated.
	//
	// IT DOES NOT CLOSE THE SUBTREE-MEMBER EDIT HOP, and an earlier version of this comment
	// claimed it did.  Two independent reasons, either one sufficient: (a) the derive's
	// resolution sink is armed ONLY when DeriveToJob is passed a non-null `outRecorded`, and
	// every production call site passes nullptr (the sole non-null caller in tree is
	// tests/CstRecordDeriveTest.cpp) -- so `g_cstResolutionSink` is null here and NOTHING is
	// recorded; and (b) even when it is armed, the recorded graph is not what closure
	// consumers read (Cst.h says so verbatim -- they still read `BuildReferenceGraph`, the
	// static descriptor-Reference graph, until the consumer-switch lands), and no descriptor
	// Reference points at a subtree member.  `DocEditClosure( d, <member> )` therefore does
	// NOT contain the instancing chunk.  The hop is closed instead by
	// DeriveToJobIncremental's DOCUMENT-WIDE `source` refusal -- see it for the cost and for
	// what would let it go.
	if( objMgr && !objMgr->GetItem( srcEntryName.c_str() ) ) {
		diags.push_back( who + ": the source subtree member `" + srcEntryName + "` is declared earlier but did not "
			"produce an object (its own chunk failed, or it was dropped by a scene variant)" );
		return false;
	}

	SubtreeClonePlanItem it;
	it.chunkIdx     = chunkIdx;
	it.srcEntryName = srcEntryName;
	it.parentRel    = parentRel;
	plan.push_back( it );

	// IF THIS MEMBER IS ITSELF AN INSTANCE, the entries its own expansion made are part
	// of what we are copying too, one qualification level deeper -- and they come FIRST,
	// for the reason SourceSubtree's own ordering comment gives: in the original tree
	// they were registered at this chunk's position, while anything written `parent
	// <this>` has to be declared below it.  Emitting them first is what makes the copy's
	// sibling order the same as the original's.
	std::string nested;
	if( ParamValue( items[chunkIdx].get(), "source", nested ) && !nested.empty() && nested != "none" ) {
		const std::map<std::string, std::vector<std::size_t> >::const_iterator ni = index.byName.find( nested );
		if( ni != index.byName.end() && !ni->second.empty() && ni->second.front() < chunkIdx ) {
			std::vector<std::string> deeper = ctxParts;
			deeper.push_back( ownName );
			if( !SourceSubtree( ni->second.front(), srcEntryName, deeper, depth + 1 ) ) return false;
		}
	}

	// THIS ENTRY'S CHILDREN, over every document key that can name it -- see
	// `ChildKeysOf`.  A node written `parent B` and a node written `parent I1.B` are
	// BOTH children of the live entry `I1.B`, and the copy has to carry both; they
	// differ only in how much of the qualification their own entry names already
	// carry, which is what the paired prefix length records.
	//
	// SORTED BY CHUNK INDEX across the whole union, so siblings stay in DOCUMENT order
	// no matter which key each was found under -- that order is the registration-serial
	// order step 2's per-parent sort reads as child display order.
	{
		std::vector<std::pair<std::size_t, std::size_t> > kidsFound;   // (chunk index, prefix length to keep)
		for( std::size_t ki = 0; ki < keys.size(); ++ki ) {
			const std::map<std::string, std::vector<std::size_t> >::const_iterator kids = index.childrenOf.find( keys[ki].first );
			if( kids == index.childrenOf.end() ) continue;
			for( std::size_t k = 0; k < kids->second.size(); ++k )
				kidsFound.push_back( std::make_pair( kids->second[k], keys[ki].second ) );
		}
		std::sort( kidsFound.begin(), kidsFound.end() );
		for( std::size_t k = 0; k < kidsFound.size(); ++k ) {
			const std::vector<std::string> kidCtx( ctxParts.begin(), ctxParts.begin() + kidsFound[k].second );
			if( !ClonedEntry( kidsFound[k].first, kidCtx, srcEntryName, depth + 1 ) ) return false;
		}
	}
	// THE POP that matches the revisit guard's push, and the guard is only a cycle test
	// because of it -- see the twin at `SourceSubtree`'s tail.  The shape this one admits
	// is a DIAMOND: `B parent A`, `D parent B`, `C parent A source B`, then `I source A`.
	// Expanding `I` visits `B` and `D` under `I.B`, pops both, and then reaches them AGAIN
	// through `I.C` (whose `source B` re-enters the same two chunks by a disjoint path) to
	// produce `I.C.D`.  Without the pop the second visit reads as recursion and the whole
	// `I` subtree vanishes behind a false refusal.  Pinned by CstSourceInstanceTest's
	// "revisit-diamond" fixture.
	path.erase( chunkIdx );
	return true;
}

//! 87 step 3a/3b -- expand a `standard_object` carrying `source` into the instancing
//! chunk's own entry plus one clone per node in the source's SUBTREE.
//!
//! The instancing node IS the clone of the source ROOT: `I` takes S's bindings and
//! its OWN local transform (S's is dropped), so `source <leaf>` collapses to exactly
//! one object under the chunk's own name and costs ZERO parent links.  Each
//! transitive `parent`-descendant `X` of `S` becomes one further entry `I.X`,
//! parented to the clone of `X`'s parent (`I` for S's direct children).
//!
//! WHERE THIS RUNS, AND WHY THAT IS THE DESIGN.  Not as a trailing post-pass,
//! and not inside the parser's Finalize, but at the
//! instancing chunk's OWN POSITION in DeriveToJob's PASS-2 loop, inside the
//! window where the D35 reference sinks are armed.  Three properties follow from
//! that placement and from nowhere else:
//!   (a) declare-before-use still holds for `parent` ON the instancing node: the
//!       object exists exactly when its document position says it does, so a
//!       later chunk can parent to it and an earlier one cannot -- which is the
//!       precondition ObjectManager::SetObjectParent's cycle guard relies on;
//!   (b) a later `timeline` can name it, for the same reason;
//!   (c) the geometry / material / modifier / ... this expansion resolves are
//!       recorded as DEPENDENTS of this chunk, so editing one of them puts the
//!       instance in the incremental edit closure.  The `GetItem( source )` probe
//!       below is load-bearing for the same reason and not only for its refusal:
//!       resolving the source OBJECT through the manager records the source's
//!       chunk as a producer this chunk consumes, so editing the SOURCE also
//!       re-derives the instance.
//!
//! `source` COPIES; IT DOES NOT MOVE OR HIDE ANYTHING.  The source subtree keeps
//! rendering exactly as it did.  RISE has one hide-on-reference mechanism --
//! CSGObject::AssignObjects, which SetWorldVisible(false)s its two operands --
//! and that is OWNERSHIP: the composite CONSUMES the operand, which has no
//! independent existence afterwards.  `source` consumes nothing, so it hides
//! nothing, and the two must not be reasoned about as the same mechanism.
//!
//! READS RAW CST TOKENS, NEVER A ParseStateBag.  ResolveChunkParams evaluates
//! `expr(...)` with i=j=u=v=0 before any bag is built, so a bag-driven expansion
//! would freeze every per-instance expression at instance zero.  3a authors no
//! per-instance exprs -- but 3c does, and it inherits this code path.
//!
//! `entryBudget` is the DOCUMENT-WIDE remaining synthesized-entry allowance, and since
//! 87 step 3d this function is its ONLY writer.  Entries, not instances, are what the cap
//! has to count: a subtree instance produces `count x subtreeSize` of them, and it is the
//! entry count that reaches the TLAS, the luminary list and every per-frame walk.
static bool ExpandSourceInstance(
	const std::vector<NodeRef>& items,
	std::size_t instIndex,
	const ObjectChunkIndex& index,
	const LetBindings& lets,
	const std::map<std::string, const IAsciiChunkParser*>& registry,
	IJob& pJob,
	long long& entryBudget,
	std::vector<std::string>& diags )
{
	const NodeRef& inst = items[ instIndex ];

	std::string instName, srcName, instGeometry;
	ParamValue( inst.get(), "name",     instName );
	ParamValue( inst.get(), "source",   srcName );
	ParamValue( inst.get(), "geometry", instGeometry );

	const std::string who = "standard_object `" + instName + "`";

	if( instName.empty() ) {
		diags.push_back( "standard_object: a chunk carrying `source` needs a `name` -- the instance IS a node, and every node is addressable" );
		return false;
	}
	// Mutually exclusive forms, counted and refused BEFORE any mutation, the same
	// shape sweep_geometry uses for profile_point / profile_circle / profile_rect.
	// ZERO forms is legal here (that is the container); TWO never is.
	//
	// `geometry none` COUNTS AS A FORM once `source` is present, and this asymmetry
	// is the whole point.  On its own, `geometry none` is the container spelling --
	// zero forms, no leaf shape, legal.  Alongside `source` it is not inert: the
	// merge below drops only `source` from the instancing chunk's own params, so a
	// `geometry none` written there OVERRIDES the geometry inherited from the
	// source and the "copy" derives as an empty, invisible container -- with no
	// diagnostic at all, because the count said one form.  Writing a REAL geometry
	// onto the same chunk is refused; the two spellings must reach the same answer,
	// and the answer that keeps `geometry none` meaningful is "that is a second
	// form", not "the merge quietly ignores it".
	//
	// `srcForm` is always TRUE at today's only entry: PASS-2 only marks a chunk
	// `isSourceInstance` when `source` names something real.  It stays written as a
	// COUNT anyway so a future entry path (3b/3c) cannot silently re-open the hole.
	const bool srcForm = ( !srcName.empty() && srcName != "none" );
	const int formCount = ( ( !instGeometry.empty() && ( instGeometry != "none" || srcForm ) ) ? 1 : 0 )
	                    + ( srcForm ? 1 : 0 );
	if( formCount > 1 ) {
		diags.push_back( who + ": `geometry` and `source` are mutually exclusive -- a node is EITHER a leaf "
			"shape (`geometry`) OR an instance of another node (`source`), never both.  Drop one."
			+ ( instGeometry == "none"
			      ? std::string( "  `geometry none` is not inert here: it is merged onto the copy and would "
			                     "override the geometry inherited from `" + srcName + "`, leaving an empty "
			                     "container rather than an instance.  Delete the `geometry` line." )
			      : std::string() ) );
		return false;
	}
	if( srcName == instName ) {
		diags.push_back( who + ": `source " + srcName + "` names the chunk ITSELF.  An instance is a copy of "
			"ANOTHER node; a node cannot be a copy of itself." );
		return false;
	}

	// DECLARED EARLIER.  This is also the RECURSION GUARD: `source` links point
	// strictly backwards in the document, so a chain of them is acyclic by
	// construction and a mutual pair (`A source B`, `B source A`) is impossible --
	// one of the two would have to reference forward.
	const std::map<std::string, std::vector<std::size_t> >::const_iterator si = index.byName.find( srcName );
	if( si == index.byName.end() || si->second.empty() || si->second.front() >= instIndex ) {
		diags.push_back( who + ": `source " + srcName + "` must name an object DECLARED EARLIER in the file"
			+ ( ( si == index.byName.end() || si->second.empty() )
			      ? std::string( " -- no `standard_object` / `csg_object` of that name exists" )
			      : std::string( " -- it is declared LATER (a forward reference; that ordering rule is also what "
			                     "makes a `source` cycle impossible)" ) ) );
		return false;
	}

	// 87 step 3c: THE SOURCE ITSELF CARRIES COUNTS.  Checked HERE, before the manager
	// probe below, because a counted chunk produces `A[i,j]` and NO entry called `A` --
	// so the probe would refuse it with "declared earlier but did not produce an object
	// (its own chunk failed)", which names a cause that did not happen and sends the
	// author looking for a broken chunk.  The walk has the same check for every chunk
	// further down the `source` chain and for every subtree member.
	if( ChunkCarriesCounts( items[ si->second.front() ].get() ) ) {
		diags.push_back( who + ": `source " + srcName + "` names a chunk carrying `count_u` / `count_v`, so it is a "
			"REPETITION -- it produces one entry per (i,j) (`" + srcName + "[0,0]`, ...) rather than a single node, "
			"and a copy of it would silently be a copy of just one.  Instance the node the counts repeat, or write "
			"the counts on THIS chunk instead." );
		return false;
	}

	IJobPriv* priv = dynamic_cast<IJobPriv*>( &pJob );
	IObjectManager* objMgr = priv ? priv->GetObjects() : 0;
	if( !objMgr ) {
		diags.push_back( who + ": `source` needs the object manager to resolve `" + srcName + "`, and this Job has none" );
		return false;
	}
	// Resolves the source through the manager -- (c) above: the recorded dependency
	// edge that puts this instance in the closure of an edit to the SOURCE.
	IObjectPriv* srcObj = objMgr->GetItem( srcName.c_str() );
	if( !srcObj ) {
		diags.push_back( who + ": `source " + srcName + "` is declared earlier but did not produce an object "
			"(its own chunk failed, or it was dropped by a scene variant)" );
		return false;
	}
	// A CSG OPERAND is CONSUMED by its composite: CSGObject::AssignObjects takes
	// ownership and SetWorldVisible(false)s it, so it has no existence as a
	// standalone shape -- it is a term in someone else's boolean expression, not a
	// node.  The same coherent rule ObjectManager::SetObjectParent applies when it
	// refuses to let an operand take (or be) a parent.
	//
	// A DOCUMENT scan, not the live `IsWorldVisible()` state.  Visibility only goes
	// false once the `csg_object`'s own Finalize has run, so a live-state test would
	// refuse when the composite is declared BEFORE the instancing chunk and let the
	// identical scene through when it is declared after -- an order-dependent rule
	// where every other refusal in this function is a document scan.
	//
	// (The reason this refusal ORIGINALLY gave -- "its matrix is CSG-local, so a
	// copy placed by a world `position` would not land where the number says" -- is
	// not true under the collapse semantics: IsInstanceOwnParam drops the source's
	// matrix entirely, so nothing CSG-local is ever carried into the copy.)
	if( index.csgOperands.count( srcName ) ) {
		diags.push_back( who + ": `source " + srcName + "` is a CSG OPERAND -- a `csg_object` in this file names it "
			"as `obja` / `objb`, which CONSUMES it: the composite owns it and hides it, so it is a term in a "
			"boolean expression rather than a shape that stands on its own.  Instance the `csg_object` itself "
			"(that IS a node), or instance the geometry directly with a `geometry` binding." );
		return false;
	}
	// `I source S parent S` -- the copy would be a CHILD of the very node it is a
	// copy of, so `S`'s subtree would contain a copy of `S`.  That is a recursive
	// definition with no fixed point, and it is refused HERE, before the subtree walk,
	// because it is worth its own diagnosis: the walk's generic revisit guard would
	// name the recursion but not the one `parent` line that created it.
	//
	// The test is "the instancing chunk is itself among the source's children", not
	// "every child of S instances S": under 3b a source with a GENUINE other child is
	// expanded rather than refused, so the round-1 "all children are self-sourced"
	// shape no longer separates anything -- what matters is only whether THIS chunk
	// is inside the subtree THIS chunk is copying.
	{
		const std::map<std::string, std::vector<std::size_t> >::const_iterator kids = index.childrenOf.find( srcName );
		if( kids != index.childrenOf.end() ) {
			for( std::size_t k = 0; k < kids->second.size(); ++k ) {
				if( kids->second[k] != instIndex ) continue;
				diags.push_back( who + ": `source " + srcName + "` and `parent " + srcName + "` on the SAME chunk -- "
					"the copy would be a CHILD of the very node it is a copy of, so `" + srcName + "`'s subtree "
					"would contain a copy of `" + srcName + "`.  That is a recursive definition with no fixed point: "
					"the subtree expansion would have to copy this chunk into its own output, forever.  Parent the "
					"instance to something other than its source." );
				return false;
			}
		}
	}

	// 87 step 3c -- THE REPETITION COUNTS.  `count_u U [count_v V]` repeats this whole
	// instance (root + subtree) U x V times, naming each repetition's root `I[i,j]` and
	// each of its clones `I[i,j].X`.
	//
	// PRESENCE selects the array form, never the VALUE.  `count_u 1` derives `I[0,0]`,
	// not `I` -- because the value may be an `expr(...)` of a `let`, and a naming scheme
	// that flipped when a constant went from 2 to 1 would silently dangle every `parent
	// I[0,0]` in the file.  It also keeps the entry names byte-compatible with the
	// `instance_array` generator this replaced (87 step 3d), which named its
	// single-instance case `g[0,0]` for the same reason.
	//
	// A count of ZERO is legal and produces NO entries, exactly as the shared validator
	// has always allowed.
	std::string countU_raw, countV_raw;
	const bool hasCountU = ParamValue( inst.get(), "count_u", countU_raw ) && !countU_raw.empty();
	const bool hasCountV = ParamValue( inst.get(), "count_v", countV_raw ) && !countV_raw.empty();
	const bool counted   = hasCountU || hasCountV;
	if( hasCountV && !hasCountU ) {
		diags.push_back( who + ": `count_v` without `count_u`.  `count_u` is the first axis of the repetition and "
			"`count_v` the optional second, so a `count_v` alone describes no grid -- write `count_u` too (use "
			"`count_u 1` for a single column)." );
		return false;
	}
	int countU = 1, countV = 1;
	if( counted ) {
		// The shared validator (`instance_array`'s own before 87 step 3d retired it, moved
		// out verbatim).  Both counts are evaluated even when the first fails, so two bad
		// counts report twice.
		bool countOk = EvalInstanceCount( countU_raw, lets, who, "count_u", diags, countU );
		if( hasCountV && !EvalInstanceCount( countV_raw, lets, who, "count_v", diags, countV ) ) countOk = false;
		if( !countOk ) return false;
	}

	// Walk the `source` chain back to the ROOT chunk -- the one that declares what
	// the thing actually IS.  `I source S` where `S source T` is legal and means
	// "another copy of T, with S's overrides on top of it".
	std::vector<std::size_t> chain;   // nearest source first
	if( !SourceChainOf( items, index, instIndex, chain ) || chain.empty() ) {
		diags.push_back( who + ": `source` chain from `" + srcName + "` does not resolve (a broken link, or deeper than 256 links)" );
		return false;
	}

	// doc 89 slice C: WARN when the SOURCE ROOT carries a `mirror` and this instancing
	// chunk does not.  `mirror` is instance-own (see IsInstanceOwnParam), so the copy
	// is built WITHOUT the source's reflection -- which makes a plain `source` clone of
	// a mirrored node come out as that node's MIRROR IMAGE, the exact opposite of what
	// "copy" suggests.  The mechanics are correct and deliberate (they are `position` /
	// `scale`'s, and pinned by ObjectMirrorTest [D4]); what is not obvious is which of
	// the two things an author gets, so say it, and say the one-token fix.
	//
	// A WARNING, NOT A DIAGNOSTIC.  A `diags` entry fails the derive -- every caller
	// treats a non-empty bag as a refusal -- and both readings are legitimate scenes:
	// `source W` on a mirrored `W` is how you author the OTHER half of a bilateral pair
	// from a half that was itself mirrored into place.
	//
	// Keyed on `chain.front()`, the chunk `source` actually NAMES: that is the node the
	// author is looking at, and it is the node whose own `mirror` param decides how it
	// renders (a mirror further up the chain is dropped for that chunk by the same rule).
	{
		std::string ownMirror, srcMirror;
		ParamValue( inst.get(), "mirror", ownMirror );
		ParamValue( items[ chain.front() ].get(), "mirror", srcMirror );
		const bool ownHas = ( !ownMirror.empty() && ownMirror != "none" );
		const bool srcHas = ( !srcMirror.empty() && srcMirror != "none" );
		if( srcHas && !ownHas ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"%s: `source %s` copies a node that carries `mirror %s`, but this chunk carries none.  "
				"`mirror` is INSTANCE-OWN -- dropped with `position` / `orientation` / `scale` -- so the clone "
				"will be the MIRROR IMAGE of what you see, not a copy of it.  Add `mirror %s` here to reproduce "
				"the source exactly.",
				who.c_str(), srcName.c_str(), srcMirror.c_str(), srcMirror.c_str() );
		}
	}

	// THE SUBTREE PLAN, built before anything is applied, and NAMED RELATIVE to the
	// instance root -- see ClonePlanBuilder.  Built ONCE even for a counted expansion:
	// the plan depends on the DOCUMENT, not on (i,j), so a 100x100 grid walks the
	// document once and composes 10 000 sets of names out of the one plan.
	std::vector<SubtreeClonePlanItem> plan;
	{
		ClonePlanBuilder b = { items, index, who, instIndex, objMgr, diags, plan, std::set<std::size_t>() };
		b.path.insert( instIndex );   // this chunk is on the path from the start -- see the recursive-definition guard
		if( !b.SourceSubtree( chain.front(), std::string(), std::vector<std::string>(), 1 ) ) return false;
	}

	// THE DOCUMENT-WIDE ENTRY CAP.  `perInstance` counts the root plus one per subtree
	// node; the total is `count_u * count_v * perInstance`, which is what 87 step 3c
	// requires the cap to count -- NOT the instance count.  The budget is shared with
	// every other expansion in this document, because what has to be bounded is the
	// number of entries reaching the TLAS, not the number of times any one generator
	// repeats.  No overflow: EvalInstanceCount clamps each count to 1e6, so the product
	// is at most 1e12, and `perInstance` is bounded by the document's chunk count.
	//
	// The per-count 1e6 clamp came from `instance_array` and is kept deliberately (it comes
	// with the shared validator): without it a `count_u 1e12` on a subtree whose plan is
	// EMPTY-but-for-the-root would be refused only by this budget line, and the refusal
	// would be the only thing between the author and a 1e12-iteration loop.  There is no
	// separate `count_u * count_v <= 1e7` product cap here because the budget subsumes
	// it exactly: `perInstance >= 1`, so `total >= count_u * count_v`.
	const long long perInstance = (long long)plan.size() + 1;
	// AN INVARIANT THAT IS CHECKED, NOT ASSERTED, AND THE DIFFERENCE IS A HANGING SUITE.
	// `perInstance` is the root plus one per subtree node, so it is >= 1 by construction --
	// but it is the ONLY factor keeping `total` off zero for a LEAF source, whose plan is
	// empty.  Drop the `+ 1` and `total` is 0 for every leaf: the cap below passes, and the
	// per-repetition collision scan and apply loop underneath then run count_u * count_v
	// times with nothing bounding them.  The leaf cap fixture (1e6 x 100) turns into a
	// 1e8-iteration walk and CstSourceInstanceTest never terminates -- which reads in CI as
	// infrastructure flake rather than as the red test it is.  A bare `assert` would not
	// help: MSVC Release carries /DNDEBUG and run_all_tests.ps1 defaults to Release, so it
	// compiles to nothing on the very configuration the hang would be hardest to diagnose
	// on (see commit 64d73157).  Refuse instead, on every configuration.
	if( perInstance < 1 ) {
		diags.push_back( who + ": internal error -- a repetition computed " + std::to_string( perInstance )
			+ " entries, which is impossible (it is the instance root plus one per subtree node, so it is at "
			  "least 1).  Refusing rather than expanding a loop with nothing bounding it." );
		return false;
	}
	const long long total       = (long long)countU * (long long)countV * perInstance;
	if( total > entryBudget ) {
		char cap[128];
		std::snprintf( cap, sizeof(cap), "%lld more (of %lld document-wide)", entryBudget, (long long)kMaxSynthesizedEntries );
		diags.push_back( who + ": expanding `source " + srcName + "` would synthesize "
			+ std::to_string( total ) + " objects, and this document has room for only "
			+ cap + ".  A subtree instance costs one entry per subtree NODE, so the total is count x subtree size." );
		return false;
	}

	// COLLISION, both kinds, over the WHOLE entry-name set this expansion would
	// create -- every repetition's root plus every clone under it.
	//
	// (1) MANAGER-level.  AddItem already rejects a duplicate and Job::AddObject
	//     honours its bool, but that diagnostic names neither the instancing chunk
	//     nor which synthesized entry clashed.  Pre-check for the chunk-localized
	//     message.
	// (2) DOCUMENT-level, which (1) structurally CANNOT see.  An authored chunk
	//     whose name equals a synthesized one makes DocFindByNameAnyRole resolve a
	//     picked instance to the WRONG chunk -- so the editor writes an edit into a
	//     chunk that has nothing to do with what the author clicked.  The manager
	//     pre-check misses it whenever that authored chunk is declared AFTER the
	//     instancing one: it does not exist yet.  Scan the document, and name BOTH
	//     items so the author can see which two to reconcile.  Tested over the ENTRY
	//     keyspace (`entryByName`), not the source-resolvable one, because a
	//     `rect_light` named `I.C` claims the entry name `I.C` just as a
	//     `standard_object` would.
	//
	// RUN IN FULL BEFORE ANYTHING IS APPLIED, counted case included: a COLLIDING scene
	// must refuse with nothing half-applied rather than fail on the fourth of nine
	// clones of the seventh of a hundred repetitions.
	//
	// THAT GUARANTEE IS THE COLLISION SCAN'S, AND IT IS NOT DOCUMENT-WIDE -- do not read
	// it as "this expansion is atomic".  PER-INSTANCE PARAMETER EVALUATION HAPPENS INSIDE
	// THE APPLY LOOP BELOW, and PASS-1 validated only (i,j) = (0,0), so a value that is
	// finite at instance zero and NOT at instance two (`position expr(sqrt(1-i)) 0 0`,
	// count_u 4) applies `I[0,0]` and `I[1,0]` and THEN refuses.  Deliberately not
	// pre-validated over every (i,j): this per-repetition apply loop (below) stops at
	// ITS OWN first refusal -- a member's Finalize failure, or any later repetition's,
	// already leaves the Job partially applied within THIS chunk's expansion.  This is
	// a property of THIS loop and is unrelated to whether DeriveToJob's OUTER PASS-2
	// loop stops or keeps going past a DIFFERENT chunk's failure (it keeps going, since
	// the derive-continuation change, bug-fix wave 2026-08-28 -- see the note at
	// DeriveToJob's own apply loop) -- an extra count_u*count_v evaluation pass here
	// would buy an atomicity the surrounding machinery does not have.  What makes a
	// partially-applied expansion harmless is NOT "no caller ever sees it" -- some do,
	// by design (see the CANONICAL enumeration in DeriveToJob's doc comment in Cst.h,
	// "CANONICAL STATEMENT") -- it is that EVERY caller that CAN surface a derive
	// failure DOES: it gates on `diags`, discards the throwaway/staging Job outright,
	// or separately diagnoses it as an Error afterward, and every chunk that failed
	// (including a partially-applied `source` expansion) is diagnosed exactly as such.
	// The one caller that reads NEITHER (`AgentSession::FindUnacknowledgedNullGeometryEmitters_`
	// -- its local `diags` out-param is populated and never inspected) is still harmless,
	// but for a DIFFERENT reason: it derives into its OWN throwaway `Job` and releases it
	// before returning, so only its finding list -- never `diags`, never the Job -- crosses
	// back out (see the CANONICAL enumeration for the full account).
	{
		// The chunk name is ambiguous whether or not counts are present: provenance maps
		// every entry back to THIS name, so two chunks holding it send a picked instance
		// to whichever the lookup finds first.  Under counts the entry names are
		// `I[i,j]`, so the name itself is never an entry -- hence "the name", not "the
		// entry name", in the counted spelling.
		const std::map<std::string, std::vector<std::size_t> >::const_iterator ci = index.entryByName.find( instName );
		if( ci != index.entryByName.end() && ci->second.size() > 1 ) {
			// Say WHICH TWO in terms the author can find: role + position among the
			// file's CHUNKS.  The raw CST item index counts trivia, so a file with
			// three comments in it would report the 6th and 7th chunks as "item 16
			// and item 18" -- a pair of numbers nobody can count to.
			const std::size_t a = ci->second[0], b = ci->second[1];
			char pos[192];
			std::snprintf( pos, sizeof(pos), "chunk #%u, a `%s`, and chunk #%u, a `%s`",
				ChunkOrdinal( items, a ), items[a] ? items[a]->role.c_str() : "?",
				ChunkOrdinal( items, b ), items[b] ? items[b]->role.c_str() : "?" );
			// A chunk in the pair may be NAMELESS and still be here: the two roles
			// that default their entry name register `noname` without spelling it,
			// and the index now records that (or the scan would be blind to them).
			// "Rename one" is the wrong instruction for such a chunk -- there is no
			// name on it to change -- so say which fix applies.
			//
			// SCANNED OVER `{a, b}` ONLY, and that restriction is the whole point:
			// the message NAMES those two and no others, so asking the question of
			// any further declarer would attach "one of them spells no `name`" to a
			// pair where it is true of NEITHER, while the chunk it is true of goes
			// unnamed.  The predicate has to have the same domain as the sentence.
			const std::size_t pair[2] = { a, b };
			bool anyNameless = false;
			for( std::size_t k = 0; k < 2 && !anyNameless; ++k ) {
				const std::size_t idx = pair[k];
				std::string spelled;
				if( !items[idx] ) continue;
				if( !( ParamValue( items[idx].get(), "name", spelled ) && !spelled.empty() ) ) anyNameless = true;
			}
			diags.push_back( who + ": " + ( counted ? std::string( "the name `" ) : std::string( "the entry name `" ) )
				+ instName + "` is declared by MORE THAN ONE object chunk ("
				+ pos + ").  A picked instance would resolve back to whichever the name lookup finds first, so an "
				"edit could land in the wrong chunk.  "
				+ ( anyNameless
				    ? std::string( "One of them spells no `name` at all and takes `" ) + instName
				      + "` by default -- give it an explicit, different name."
				    : std::string( "Rename one." ) ) );
			return false;
		}
		// Per REPETITION, because the base name is what differs between them.  A `planned`
		// set spanning every repetition is not needed and would cost one string per
		// synthesized entry: distinct (i,j) give distinct bases by construction, so two
		// repetitions can never resolve to one entry name.
		for( int j = 0; j < countV; ++j )
			for( int i = 0; i < countU; ++i ) {
				const std::string base = InstanceBaseName( instName, counted, i, j );
				std::set<std::string> planned;
				planned.insert( base );
				// The UNCOUNTED root's name IS the instancing chunk's name, so the document
				// test for it is "declared more than once" (done above); a COUNTED root's
				// name is synthesized like a clone's, so it takes the same "declared at
				// all" test the clones do.  The manager test applies to both.
				if( objMgr->GetItem( base.c_str() ) ) {
					diags.push_back( who + ": the entry `" + base + "` this instance would create already exists as an object" );
					return false;
				}
				if( counted ) {
					const std::map<std::string, std::vector<std::size_t> >::const_iterator bi = index.entryByName.find( base );
					if( bi != index.entryByName.end() && !bi->second.empty() ) {
						char pos[128];
						std::snprintf( pos, sizeof(pos), "chunk #%u, a `%s`",
							ChunkOrdinal( items, bi->second[0] ),
							items[ bi->second[0] ] ? items[ bi->second[0] ]->role.c_str() : "?" );
						diags.push_back( who + ": this instance would synthesize the entry `" + base
							+ "`, but the document ALREADY declares an object of that name (" + pos
							+ ").  A pick on the synthesized entry would resolve back to that chunk, so an edit could "
							  "land in something unrelated.  Rename one." );
						return false;
					}
				}
				for( std::size_t k = 0; k < plan.size(); ++k ) {
					const std::string nm = base + "." + plan[k].srcEntryName;
					// A BELT, not a live case: two subtree nodes can only resolve to one entry
					// name if an authored chunk is named exactly like some earlier expansion's
					// synthesized entry -- which the document scan below refuses at THAT
					// expansion, before this one runs.  Kept because it is the one check whose
					// absence would let a copy silently REPLACE another copy rather than fail.
					if( !planned.insert( nm ).second ) {
						diags.push_back( who + ": expanding `source " + srcName + "` would synthesize the entry name `" + nm
							+ "` TWICE.  Two nodes in the subtree resolve to the same entry name, so one copy would silently "
							"replace the other." );
						return false;
					}
					if( objMgr->GetItem( nm.c_str() ) ) {
						diags.push_back( who + ": the entry `" + nm + "` this instance would synthesize for the subtree member `"
							+ plan[k].srcEntryName + "` already exists as an object" );
						return false;
					}
					const std::map<std::string, std::vector<std::size_t> >::const_iterator di = index.entryByName.find( nm );
					if( di != index.entryByName.end() && !di->second.empty() ) {
						const std::size_t a = di->second[0];
						char pos[128];
						std::snprintf( pos, sizeof(pos), "chunk #%u, a `%s`",
							ChunkOrdinal( items, a ), items[a] ? items[a]->role.c_str() : "?" );
						diags.push_back( who + ": this instance would synthesize the entry `" + nm + "` for the subtree member `"
							+ plan[k].srcEntryName + "`, but the document ALREADY declares an object of that name (" + pos
							+ ").  A pick on the synthesized entry would resolve back to that chunk, so an edit could land in "
							"something unrelated.  Rename one." );
						return false;
					}
				}
			}
	}

	// THE MEMBERS' MERGED PARAMS, computed ONCE for the whole expansion.  They cannot
	// vary with (i,j): per-instance variables scope to the chunk that CARRIES the counts,
	// which is the instancing chunk, never a descendant (see MergeChunkParams).  Hoisting
	// them out of the repetition loop is what keeps a counted subtree's derive cost
	// proportional to `count + subtreeSize` rather than to `count * subtreeSize` in
	// document parsing -- and it means a member's own diagnostic is emitted ONCE, not
	// once per repetition.
	struct MemberBuild
	{
		std::vector<std::string>            order;
		std::map<std::string, std::string>  merged;
		std::string                         targetRole;
		std::string                         kidSrc;
	};
	std::vector<MemberBuild> members( plan.size() );
	for( std::size_t k = 0; k < plan.size(); ++k ) {
		std::vector<std::size_t> kidChain;
		if( !SourceChainOf( items, index, plan[k].chunkIdx, kidChain ) ) {
			diags.push_back( who + ": the subtree member `" + plan[k].srcEntryName + "` has a `source` chain that does "
				"not resolve (a broken link, or deeper than 256 links)" );
			return false;
		}
		if( !MergeChunkParams( items, lets, plan[k].chunkIdx, kidChain, members[k].order, members[k].merged,
		                       members[k].targetRole, diags ) )
			return false;
		ParamValue( items[ plan[k].chunkIdx ].get(), "source", members[k].kidSrc );
	}

	// APPLY, one repetition at a time.  Within a repetition the root comes first -- its
	// clones are parented to it, and ObjectManager::SetObjectParent requires the parent
	// to exist already.
	for( int j = 0; j < countV; ++j )
		for( int i = 0; i < countU; ++i ) {
			// The instance variables, EXACTLY as `instance_array` defined them before 87
			// step 3d retired it: u and v are the index normalized into [0,1], and a count
			// of 1 gives 0 rather than a 0/0 NaN.
			const double u = ( countU > 1 ) ? (double)i / (double)( countU - 1 ) : 0.0;
			const double v = ( countV > 1 ) ? (double)j / (double)( countV - 1 ) : 0.0;
			const InstanceVars iv = { i, j, u, v };
			const std::string base = InstanceBaseName( instName, counted, i, j );
			{
				std::vector<std::string> order;
				std::map<std::string, std::string> merged;
				std::string targetRole;
				// `&iv` UNCONDITIONALLY, counted or not, and that is not an oversight.
				// PASS-1 validates every instancing chunk at instance zero (see there), so
				// a chunk with a per-component expr and NO counts is admitted by PASS-1;
				// evaluating it here with the whole-value-only rule instead would hand
				// `ApplySynthesizedNode` the raw `expr(...)` text and refuse at the
				// descriptor -- PASS-1 and PASS-2 disagreeing about the same chunk, which
				// is the divergence class this arc keeps closing.  A single instance IS
				// instance zero.  For a value with no `expr(` in it the two paths are
				// byte-identical (EvalInstanceValue re-joins the components single-spaced,
				// which is what the token loop already produced), so every existing 3a/3b
				// scene derives exactly as it did.
				// A FAILURE HERE CAN LEAVE EARLIER REPETITIONS APPLIED (see the collision
				// scan's note above): only (i,j) = (0,0) was validated in PASS-1, so an
				// expression that goes non-finite at a later index is diagnosed at THAT
				// index, with everything before it already in the Job.  Contained by the
				// caller, not by this loop.
				if( !MergeChunkParams( items, lets, instIndex, chain, order, merged, targetRole, diags, &iv ) )
					return false;
				if( !ApplySynthesizedNode( registry, pJob, order, merged, targetRole, base,
				                           /*parentOverride*/ 0, who, srcName, diags ) )
					return false;
			}
			// PROVENANCE.  Recorded even in the collapse case, where the entry name and the
			// instancing chunk name are the same string: consumers ask ONE question ("where
			// did this entry come from?") and get one answer, instead of each re-deriving
			// the relationship from the spelling of the name.  Under counts the row is
			// `I[i,j] -> (I, S)`: the chunk to edit and the node it copies.
			objMgr->SetObjectProvenance( base.c_str(), instName.c_str(), srcName.c_str() );
			--entryBudget;

			// Then the subtree, pre-order, each clone built by re-`Finalize`ing the node's OWN
			// chunk with `name` and `parent` remapped.  Re-Finalizing rather than deep-copying
			// the live object is what makes a chunk whose Finalize synthesizes MORE THAN ONE
			// entity come out right for free: a `rect_light` names its painter / material /
			// geometry `<name>__pnt` / `__mat` / `__geo`, so remapping the one `name` renames
			// all four, where an object-level clone walk would collide on all three helpers.
			for( std::size_t k = 0; k < plan.size(); ++k ) {
				const std::string cloneName = base + "." + plan[k].srcEntryName;
				const std::string parentClone = plan[k].parentRel.empty() ? base : ( base + "." + plan[k].parentRel );
				// A COPY per repetition, because ApplySynthesizedNode WRITES the `parent`
				// override into the map it is handed and this one is shared across
				// repetitions.  DEFENSIVE, not load-bearing, and measured as such: aliasing
				// `members[k]` directly leaves the whole suite green, because the write is
				// unconditional (every repetition overwrites the previous one's value) and
				// `order` only ever gains `parent` once.  The copy is kept because that
				// green rests on ApplySynthesizedNode's write being unconditional -- a
				// property of a function three call sites away, which nothing here states
				// or checks -- and the failure mode if it ever stops holding is every
				// repetition after the first being parented into the FIRST one.
				std::vector<std::string>           order  = members[k].order;
				std::map<std::string, std::string> merged = members[k].merged;
				if( !ApplySynthesizedNode( registry, pJob, order, merged, members[k].targetRole, cloneName,
				                           &parentClone, who, members[k].kidSrc.empty() ? srcName : members[k].kidSrc, diags ) )
					return false;
				// `I.X -> (I, X)`: the node this entry is a COPY OF, which is what makes the
				// entry traceable to something an author can edit.  X is the SOURCE-SIDE
				// ENTRY name, which for a nested instance's clone is itself qualified -- so a
				// consumer that follows the chain lands on a real live entry at every hop.
				// Under counts every repetition's clone names the SAME source node: `I[i,j].X
				// -> (I, X)`.
				objMgr->SetObjectProvenance( cloneName.c_str(), instName.c_str(), plan[k].srcEntryName.c_str() );
				--entryBudget;
			}
		}
	return true;
}

int DeriveToJob( const Document& doc, IJob& pJob, std::vector<std::string>* diagnostics, ReferenceGraph* outRecorded, const char* activeVariantOverride )
{
	std::vector<std::string> local;
	std::vector<std::string>& diags = diagnostics ? *diagnostics : local;

	// D35 (review): the recorder only APPENDS to `outRecorded->dependents`, so RESET the whole
	// graph at entry -- before any early return -- or a reused/replaced ReferenceGraph (or a
	// caller-supplied BuildReferenceGraph result) would mix stale dependents/edges/stamp with
	// this derive's, and a validation-failure early return would leave the caller's old graph
	// untouched (silently stale). After this, `outRecorded` reflects THIS derive only -- empty
	// on a PASS-1 refuse-all (nothing was ever applied), but a PASS-2 failure can still record a
	// graph covering most of the document, since every chunk that DID apply still records its
	// productions/resolutions (PASS-2 keeps going past a failure -- see DeriveToJob's doc comment
	// in Cst.h).
	if( outRecorded ) *outRecorded = ReferenceGraph();

	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );

	// Reset the chunk parsers' cross-chunk parse state FIRST, exactly as the
	// legacy ParseAndLoadScene did at the start of every parse (loader deleted
	// in Slice 6c; this derive is now the registry's sole caller). Some Finalize()s
	// read/write file-scope caches within one scene (notably the
	// uniformcolor_painter colour cache that translucent_material's energy-
	// conservation check reads); without this, deriving scene A then scene B
	// would leak A's state into B (the redesign runs DeriveToJob repeatedly on
	// every edit), giving B a Job a fresh parse of B would not.
	ClearChunkParserState();

	// Reset the Job's scene-variant records too (ClearChunkParserState above clears PARSER state, not Job state):
	// a re-derive on a reused Job must not inherit the prior derive's variant declarations / active selection.
	pJob.ClearSceneVariants();

	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();

	// #5 slice 3: collect document-level `let` constants up front (a malformed binding diags -> the
	// refuse-all below applies nothing).  `let` chunks are CST-level, NOT engine entities -> skipped.
	const LetBindings lets = CollectLetBindings( items, diags );

	// PASS 1 -- validate EVERY chunk through the live descriptor registry, the
	// SAME validation the legacy parser runs (DispatchChunkParameters: rejects a
	// no-space line, an undeclared parameter name, or a non-finite/non-numeric
	// numeric value -- see its doc in IAsciiChunkParser.h). Collect each
	// chunk's populated bag; if ANY chunk fails, apply NOTHING (refuse-all).
	// `itemIndex` + `isSourceInstance`: 87 step 3a expands a `source` chunk at its OWN
	// position in PASS-2 (see ExpandSourceInstance), which needs the CST node, not the bag.
	struct Pending { const IAsciiChunkParser* parser; ParseStateBag bag; std::string keyword; NodeId nodeId; std::size_t itemIndex; bool isSourceInstance; };
	std::vector<Pending> pending;
	pending.reserve( items.size() );
	for( size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( c->kind != NodeKind::Chunk ) continue;   // header strays / trivia: not derivable chunks
		if( c->role == "let" ) continue;   // #5 slice 3: CST-level, not a 1:1 engine chunk
		// 87 step 3a: read `source` off the CST TOKEN, not the bag -- the expansion is
		// deliberately bag-free (ExpandSourceInstance's header says why), and so is its trigger.
		// Read BEFORE the params are resolved, because 87 step 3c makes the resolution itself
		// depend on the answer.  Through `ChunkIsSourceInstance`, which is ALSO what
		// `Document::sourceInstanceCount` counts -- so the incremental derive's document-wide
		// refusal and this expansion trigger cannot drift apart.
		const bool isSrc = ChunkIsSourceInstance( c.get() );
		// Resolve the parser + normalise the params (shared with the incremental
		// derive, so both paths normalise identically -- see ResolveChunkParams).
		//
		// 87 step 3c: AN INSTANCING CHUNK IS VALIDATED AT INSTANCE ZERO.  Its params may
		// hold PER-COMPONENT `expr(...)` (`position expr(i*2) 0 0`), which the ordinary
		// whole-value handling passes through verbatim -- and `position expr(i*2) 0 0` is
		// not a finite numeric triple, so PASS-1 would refuse the scene before the
		// expansion ever ran.  Evaluating at (i,j,u,v) = 0 gives the descriptor a real
		// value to check the arity and kind of, which is validation this chunk would
		// otherwise lose entirely; the values the SCENE gets are computed per repetition
		// in ExpandSourceInstance, from the raw tokens.
		// The bag built here is never applied for such a chunk -- PASS-2 expands instead
		// of calling its Finalize.
		const InstanceVars instZero = { 0, 0, 0.0, 0.0 };
		IAsciiChunkParser::ParamsList plist;
		const IAsciiChunkParser* parser = ResolveChunkParams( c, registry, plist, diags, lets, isSrc ? &instZero : nullptr, &doc, i );
		if( !parser ) continue;                       // unknown chunk type (diagnostic already pushed)
		ParseStateBag bag( &parser->Describe() );
		if( !DispatchChunkParameters( parser->Describe(), bag, plist ) ) {
			diags.push_back( c->role + ": invalid parameter(s) (see log)" );
			continue;
		}
		pending.push_back( Pending{ parser, std::move(bag), c->role, DocNodeIdAt( doc, (int)i ), i, isSrc } );   // nodeId: D35 recording attribution
	}
	if( !diags.empty() ) return 0;   // refuse-all: a malformed scene applies NOTHING

	// ----- scene_variant bake-at-derive (doc 63 §12): pre-scan the active variant + its overridden material
	// names so PASS-2 registers the ACTIVE definition per material name (objects then bind to it by name -- no
	// post-derive re-pointing of a built scene).  CST-native; the legacy reader renders the base.
	std::string svActiveName, svActiveCamera;
	if( activeVariantOverride ) {                       // GUI re-derive FORCES a variant ("none"/"" => base), bypassing the active_scene_variant chunk
		svActiveName = activeVariantOverride;
	} else {
		for( const Pending& p : pending )
			if( p.keyword == "active_scene_variant" ) svActiveName = p.bag.GetString( "name", "" );
	}
	if( svActiveName == "none" ) svActiveName.clear();
	std::set<std::string> svOverriddenNames;
	std::map<std::string, size_t> svActiveOverride;   // active-variant override name -> its pending index (applied at the base's slot)
	if( !svActiveName.empty() ) {
		std::set<std::string> svBaseMaterialNames;
		bool svActiveDeclared = false;
		for( const Pending& p : pending ) {
			if( p.keyword == "scene_variant" && p.bag.GetString( "name", "" ) == svActiveName ) {
				svActiveCamera = p.bag.GetString( "active_camera", "" );
				svActiveDeclared = true;
			}
			if( p.parser->Describe().category != ChunkCategory::Material ) continue;
			std::string mv = p.bag.GetString( "variant", "" );
			if( mv == "none" ) mv.clear();   // `variant none` = the no-variant sentinel -> an ordinary base, not a tag
			const std::string mn = p.bag.GetString( "name", "noname" );
			if( mv == svActiveName ) {
				svOverriddenNames.insert( mn );
				if( !svActiveOverride.insert( std::make_pair( mn, (size_t)( &p - pending.data() ) ) ).second )   // two overrides of one name -> ambiguous
					diags.push_back( "scene_variant `" + svActiveName + "`: duplicate override of material `" + mn + "`" );
				svActiveDeclared = true;
			}
			else if( mv.empty() && !svBaseMaterialNames.insert( mn ).second )
				// Two untagged base materials of the same name.  Belt-and-suspenders: the bake redirects an overridden
				// base's slot to the single override (it never skips an untagged base), so a dup base would ALSO trip
				// AddItem's dup-name hard-error in PASS-2 -- but this pre-scan diagnostic is clearer + fires before apply.
				diags.push_back( "scene_variant: duplicate base material name `" + mn + "`" );
		}
		// An active_scene_variant naming a variant neither declared (a scene_variant chunk) nor used (a variant-
		// tagged material) is a selector typo -- the symmetric twin of a dangling override.  Refuse, don't silently
		// fall back to the base.
		if( !svActiveDeclared )
			diags.push_back( "active_scene_variant `" + svActiveName + "` names no declared scene_variant (typo?)" );
		// A variant override whose name has NO base material is a dangling override (typically a typo of the base
		// name): it would silently register a phantom material while the intended base stays unchanged -- exactly
		// the silent mis-render this feature exists to prevent (doc 63 §3.2).  Refuse-all so the author fixes it.
		for( const std::string& on : svOverriddenNames )
			if( !svBaseMaterialNames.count( on ) )
				diags.push_back( "scene_variant `" + svActiveName + "`: override of `" + on + "` has no base material of that name (dangling override -- typo?)" );
		if( !diags.empty() ) return 0;
	}

	// PASS 2 -- apply via the SAME Finalize the (now-deleted) legacy parser
	// used to call, so the CST path builds the identical Job a validation-
	// clean CANONICAL registry scene always built (see DeriveToJob's doc for
	// the exact scope).  A Finalize failure is an APPLY-TIME error that
	// PASS-1 validation cannot detect (e.g. a reference to a not-yet/never-
	// defined chunk).
	//
	// CONTINUE past a Finalize failure rather than stopping at the first one
	// (bug-fix wave, 2026-08-28): every EARLIER design here matched the
	// legacy parser's abort-on-first-failure -- one bad chunk's diagnostic
	// was the only one a caller ever saw, and every chunk after it in the
	// document silently vanished with NO diagnostic of its own, even a chunk
	// with no relation to the failure.  That made a single mistake (e.g. a
	// scalar-pipe parameter whose default resolved through the wrong
	// painter manager -- see ISCALARPAINTER_REFACTOR.md) look like it had
	// deleted the rest of the scene, when the actual, fixable cause was one
	// line.  Now every pending chunk is still attempted: a failure is
	// diagnosed BY NAME and the loop moves on, so one run surfaces every
	// independent problem instead of forcing fix-one/rebuild/find-the-next.
	// A chunk that legitimately references the failed one's would-be entity
	// still fails too (its OWN unresolved-reference diagnostic) -- that is
	// expected, not a bug: the failed chunk's entity genuinely never existed
	// to reference.  Chunks that applied stay applied, exactly as before;
	// full apply-atomicity (rollback of every applied chunk) is later
	// Facet-2 work.  The OVERALL derive still fails whenever `diags` is
	// non-empty -- every caller that reads `diags` treats that as "the derive
	// failed" (one caller derives but never reads its own `diags` at all; see
	// the CANONICAL enumeration for why that is still harmless), so "keep
	// going" buys more diagnostics per run, never a scene `diags` calls clean
	// when it is not.  Which callers DISCARD a diagnosed Job outright vs.
	// which still consult a possibly-partial one is NOT uniform -- see the
	// CANONICAL enumeration in this function's own doc comment in Cst.h
	// (search "CANONICAL STATEMENT"); do not restate or re-derive that list
	// here, it has drifted out of sync with copies before.
	// D35 record-during-derive (slice 1, §8): when recording, capture each chunk's PRODUCED
	// + RESOLVED entities from the engine's actual manager AddItem/GetItem (the chokepoint
	// hooks in GenericManager.h), and build (producer -> consumer) reverse-adjacency from the
	// pointer identity -- no heuristic, so the recorded graph cannot drift from the engine.
	// `productionMap` (entity pointer -> producing chunk) persists across chunks: a consumer's
	// resolution hit maps to the producer recorded when that producer's chunk was applied.
	std::map<const void*, NodeId> productionMap;
	// RAII backstop: clear the thread-local sinks on ANY exit (a Finalize that threw would
	// otherwise leave them dangling at the loop-local vectors). RISE Finalize is C-style/no-
	// throw, but this keeps the invariant unconditional.
	struct SinkGuard { ~SinkGuard() { g_cstProductionSink = nullptr; g_cstResolutionSink = nullptr; g_cstFinalizeDiagSink = nullptr; } } sinkGuard;
	(void)sinkGuard;

	// 87 step 3a: ONE O(N) pass over the document's object-creating chunks, shared by
	// every `source` expansion below (name -> declaring items, and the set of names
	// that have children).  Built here rather than per-expansion so N sources cost
	// O(N), not O(N^2).
	ObjectChunkIndex objIndex;
	BuildObjectChunkIndex( items, objIndex );

	// 87 step 3c: refuse `parent I` / `override_object I` where `I` carries counts, BEFORE
	// anything is applied.  A document scan, and it has to be -- the parsers that own those
	// two diagnostics run against the live manager and cannot see that the missing name
	// belongs to a counted chunk, so each of them enumerates causes none of which is the
	// real one.  Refuse-all on the PASS-1 model: this is a document defect, not an apply
	// failure, and half a scene is no better here than it is there.
	if( !RefuseBareReferencesToCountedChunks( items, objIndex, diags ) ) return 0;

	// 87 step 3b: ONE document-wide synthesized-entry allowance, spent by every
	// `source` expansion below -- and, since 87 step 3d deleted the `instance_array`
	// generator, by nothing else.  See kMaxSynthesizedEntries for why it is in entries.
	// DISCLOSED RESIDUAL (bug-fix wave, 2026-08-28): PASS-2 now keeps applying past a
	// failed chunk (see the header comment above), so a `source` expansion that spends
	// some of this budget before itself failing partway through leaves LESS budget for
	// a later, wholly unrelated `source` chunk -- which could then be refused with a
	// "this document has room for only N more" diagnostic that is honest about the
	// number but misleading about the cause.  Not a safety issue (the budget only ever
	// shrinks, and the headroom is kMaxSynthesizedEntries, ~1e7) and not fixed here:
	// "refund exactly what a failed expansion spent" needs bookkeeping this loop does
	// not have (a partial expansion's own per-repetition apply can itself succeed
	// partway before failing -- see ExpandSourceInstance -- so "spent" and "still live
	// in the Job" are not the same set to refund from).  Flagging for whoever next
	// touches source-instance budget accounting.
	long long entryBudget = kMaxSynthesizedEntries;

	int count = 0;
	int failedCount = 0;   // chunks whose Finalize/expansion failed -- see the header comment above
	for( Pending& p : pending ) {
		// scene_variant bake (doc 63): apply the ACTIVE definition per material name.  An active override is applied at
		// its overridden BASE's slot (so objects bind it by name regardless of the override chunk's file position -- the
		// override may sit after the objects, as the watch night block does); the override's own slot + every inactive
		// override are then dropped, and so is the now-superseded base.
		const Pending* applyP = &p;
		if( p.parser->Describe().category == ChunkCategory::Material ) {
			std::string svv = p.bag.GetString( "variant", "" );
			if( svv == "none" ) svv.clear();   // `variant none` sentinel -> base, not a tag (mirrors svActiveName=="none")
			if( !svv.empty() ) continue;   // variant-tagged: the active override is applied at its base's slot (below); inactive ones dropped
			std::map<std::string, size_t>::const_iterator ov = svActiveOverride.find( p.bag.GetString( "name", "noname" ) );
			if( ov != svActiveOverride.end() ) applyP = &pending[ ov->second ];   // overridden base -> apply the active override HERE
		}
		std::vector<const void*> produced, resolved;
		if( outRecorded ) { g_cstProductionSink = &produced; g_cstResolutionSink = &resolved; }
		// See GenericManager.h: a Finalize that fails may set *g_cstFinalizeDiagSink to a
		// specific reason instead of the generic "apply failed" message below.
		std::string finalizeDiag;
		g_cstFinalizeDiagSink = &finalizeDiag;
		// 87 step 3a: a `source` chunk EXPANDS here -- at its own document position,
		// inside this armed-sink window -- instead of going through its own Finalize.
		// ExpandSourceInstance's header records why the position and the window are
		// both load-bearing.  It pushes its own, specific diagnostic on failure, so
		// the generic "apply failed" line below is suppressed for it.
		bool expandDiagnosed = false;
		bool ok;
		if( applyP->isSourceInstance ) {
			ok = ExpandSourceInstance( items, applyP->itemIndex, objIndex, lets, registry, pJob, entryBudget, diags );
			expandDiagnosed = !ok;
		} else {
			ok = applyP->parser->Finalize( applyP->bag, pJob );
		}
		g_cstFinalizeDiagSink = nullptr;
		if( outRecorded ) {
			g_cstProductionSink = nullptr; g_cstResolutionSink = nullptr;   // no GetItem between here and the next set
			for( const void* e : produced ) productionMap[ e ] = applyP->nodeId;  // this chunk's productions (incl. intra-chunk, so a self-ref self-skips)
			for( const void* e : resolved ) {
				std::map<const void*, NodeId>::const_iterator it = productionMap.find( e );
				if( it != productionMap.end() && it->second != applyP->nodeId )   // skip self-reference (a chunk resolving its own product)
					outRecorded->dependents[ it->second ].insert( applyP->nodeId );   // editing the producer re-derives this consumer
			}
		}
		if( ok ) { ++count; continue; }
		if( !expandDiagnosed )
			diags.push_back( finalizeDiag.empty()
				? applyP->keyword + ": apply failed (e.g. unresolved reference); see log"
				: applyP->keyword + ": " + finalizeDiag );
		++failedCount;
		// CONTINUE, not break (see the header comment above): a later chunk
		// unrelated to this failure still deserves the chance to apply --
		// and to be diagnosed BY NAME if it too fails, rather than vanishing
		// silently behind this one's diagnostic.
	}
	// Summary LOG LINE (not a `diags` entry) when anything failed above:
	// `diags` is already non-empty (each failure pushed its own named
	// diagnostic), which is what every caller actually keys "did the derive
	// fail" on -- this line exists only so a log skimmed for just the last
	// line still shows the derive failed and how many chunks were involved,
	// not just the LAST diagnostic pushed.  Deliberately logged directly
	// rather than pushed into `diags`: `diags` is a STRUCTURED per-chunk
	// feed some callers consume mechanically (e.g. `AgentSession::ValidateText`
	// maps every entry to a localized `AgentDiagnostic` with a specific
	// chunk keyword and byte offset) -- an aggregate count entry has no
	// chunk to name and no offset to localize, so it would land as a
	// spurious, unlocalized, generic-code diagnostic alongside the real
	// per-chunk ones instead of the log-only footnote it is meant to be.
	// Every caller that logs `diags` already does so in its own loop, so
	// this is visible in the SAME log without duplicating that machinery
	// inside DeriveToJob for every other diagnostic.  For which callers
	// discard a diagnosed Job outright vs. which still consult a possibly-
	// partial one (relevant to whether "not considered successful" below is
	// the whole story for a given caller), see the CANONICAL enumeration in
	// this function's own doc comment in Cst.h ("CANONICAL STATEMENT") --
	// not restated here, it has drifted out of sync with copies before.
	if( failedCount > 0 )
		// Denominator is `count + failedCount` (chunks actually ATTEMPTED),
		// not `pending.size()`: an inactive scene_variant-tagged material
		// `continue`s above before either counter increments, so it was
		// never attempted at all and must not inflate the total.
		GlobalLog()->PrintEx( eLog_Error,
			"DeriveToJob:: %u of %u chunk(s) failed to apply (see the diagnostic(s) above, by keyword); "
			"this derive is not considered successful",
			(unsigned int)failedCount, (unsigned int)( count + failedCount ) );
	// scene_variant: apply the active variant's camera (its material overrides were baked above).
	// `none` is the universal no-reference sentinel (cf. `material none`) -> no camera override, NOT a
	// missing-camera error; exclude it from the diagnostic below.
	if( diags.empty() && !svActiveName.empty() && !svActiveCamera.empty() && svActiveCamera != "none" )
		if( !pJob.SetActiveCamera( svActiveCamera.c_str() ) )   // Reference not existence-checked in PASS-1 -> diag here
			diags.push_back( "scene_variant `" + svActiveName + "`: active_camera `" + svActiveCamera + "` is not a declared camera" );
	// The GUI re-derive forces a variant via activeVariantOverride (which bypasses the active_scene_variant
	// chunk's Finalize); reconcile the Job's active-variant record with the forced decision ("" => base).
	if( diags.empty() && activeVariantOverride )
		pJob.SetActiveSceneVariant( svActiveName.c_str() );
	// 87 structural flatten: derive RECORDS parent links and composes nothing;
	// this is where the authored tree is walked once, parent before child, to
	// bake `world = parent.world * local` into the flat render list.  It runs
	// after PASS-2 and after instance-array expansion so the whole object graph
	// exists, and it is a no-op for a scene with no parent links.
	pJob.ComposeObjectHierarchy();
	return count;
}

//! Exact equality of two world bounding boxes (finite extents).  A non-spatial
//! re-point re-runs the SAME transform on the SAME geometry -> bit-identical bbox;
//! a geometry-extent or transform edit changes it.  Exact == is the right gate (a
//! tolerance would let a genuine sub-epsilon move skip the TLAS rebuild); finite-only,
//! so -ffast-math's no-NaN/Inf assumption does not affect it.
static bool LooksNumeric( const std::string& s );   // defined below (shared with BuildReferenceGraph)

//! Does an entity of (cat, name) exist in the ALREADY-derived Job's managers?  Used by
//! the incremental apply's whole-plan preflight (review P1.7 atomicity): every drop target
//! AND every reference must exist BEFORE any mutation, so a rename / stale / dangling
//! closure refuses with NOTHING changed instead of aborting mid-apply.  A Painter is
//! checked in BOTH manager sub-namespaces (colour + scalar) -- the resolver's
//! (category,name) key cannot tell them apart (review P1.4); for an EXISTENCE preflight,
//! "resolves to SOME painter" is the right question.  Categories without a manager-backed
//! check return true (not refused -- the re-Finalize still resolves them; the residual is
//! a rare dangling reference in an unchecked category, which falls back to caller-reset).
static bool EntityExists( IJobPriv& priv, ChunkCategory cat, const std::string& name )
{
	const char* n = name.c_str();
	switch( cat ) {
		case ChunkCategory::Geometry: return priv.GetGeometries() != 0 && priv.GetGeometries()->GetItem( n ) != 0;
		case ChunkCategory::Material: return priv.GetMaterials()  != 0 && priv.GetMaterials()->GetItem( n )  != 0;
		case ChunkCategory::Object:   return priv.GetObjects()    != 0 && priv.GetObjects()->GetItem( n )    != 0;
		case ChunkCategory::Light:    return priv.GetLights()     != 0 && priv.GetLights()->GetItem( n )     != 0;
		case ChunkCategory::Modifier: return priv.GetModifiers()  != 0 && priv.GetModifiers()->GetItem( n )  != 0;
		case ChunkCategory::Shader:   return priv.GetShaders()    != 0 && priv.GetShaders()->GetItem( n )    != 0;
		case ChunkCategory::ShaderOp: return priv.GetShaderOps()  != 0 && priv.GetShaderOps()->GetItem( n )  != 0;
		case ChunkCategory::Painter:  return ( priv.GetPainters()       != 0 && priv.GetPainters()->GetItem( n )       != 0 )
		                                  || ( priv.GetScalarPainters() != 0 && priv.GetScalarPainters()->GetItem( n ) != 0 );
		case ChunkCategory::Function: return ( priv.GetFunction1Ds() != 0 && priv.GetFunction1Ds()->GetItem( n ) != 0 )
		                                  || ( priv.GetFunction2Ds() != 0 && priv.GetFunction2Ds()->GetItem( n ) != 0 );   // incl. colour painters dual-registered as Function2D
		case ChunkCategory::Medium:   return priv.GetMedium( n ) != 0;
		case ChunkCategory::HairGuides: {
			// The `hair_geometry.guides` slot.  Manager-less like Medium, but
			// with no by-name getter -- so probe the enumeration hook instead
			// (a guide table is tens of entries; this runs once per closure
			// reference in a preflight, never per sample).  Answering
			// `default: return true` here would be a REGRESSION: before the
			// category split this slot declared `{Geometry}` and so was
			// checked against the geometry manager (which never holds a guide
			// set) -- i.e. always refused, always falling back to a full
			// derive.  An exact probe is the honest version of that: a real
			// guide set now passes the preflight, and a stale/misspelt one
			// still refuses BEFORE anything is mutated.
			struct Probe : public IEnumCallback<const char*> {
				const char* want; bool found;
				Probe( const char* w ) : want( w ), found( false ) {}
				bool operator()( const char* const& nm ) override {
					if( nm && strcmp( nm, want ) == 0 ) { found = true; return false; }
					return true;
				}
			} probe( n );
			priv.EnumerateHairGuideNames( probe );
			return probe.found;
		}
		default: return true;
	}
}

static bool BBoxEqual( const BoundingBox& a, const BoundingBox& b )
{
	return a.ll.x == b.ll.x && a.ll.y == b.ll.y && a.ll.z == b.ll.z
	    && a.ur.x == b.ur.x && a.ur.y == b.ur.y && a.ur.z == b.ur.z;
}

int DeriveToJobIncremental( const Document& doc, IJob& pJob, const std::vector<NodeId>& chunkIds, std::vector<std::string>* diagnostics )
{
	std::vector<std::string> local;
	std::vector<std::string>& diags = diagnostics ? *diagnostics : local;
	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();

	// Animated scenes are refused wholesale (review P1.8/timeline): a `timeline`
	// caches a raw POINTER to its animated element (AddKeyframeToAnimation ->
	// GetItem(element)), and references that element + its owning `animation` as
	// ValueKind::String, not Reference -- so those edges are INVISIBLE to the static
	// reference graph the closure is built from.  Recreating a timeline-referenced
	// element would dangle the cached keyframe pointer, and the closure would not
	// include the timeline to re-resolve it.  Until the resolver tracks timeline
	// references (a later slice -- promote them to Reference with an element_type-keyed
	// category so the closure includes the timeline), the whole incremental falls back
	// to a full derive whenever the Job has ANY animation.  This is an O(1) query (the
	// Animator's declared-animation count; a `timeline` keyframe declares the implicit
	// "(default)" animation, so it is counted) -- it does NOT scan the document,
	// preserving the O(closure . log N) cost for non-animated scenes (review: the
	// earlier O(N) doc-scan made the incremental O(N), failing the ~flat-in-N gate).
	if( pJob.GetAnimationCount() > 0 ) {
		diags.push_back( "incremental: the Job has an animation/timeline whose String element references the static graph cannot trace; fall back to a full derive" );
		return 0;
	}

	// scene_variant is baked whole-document (doc 63): an incremental edit cannot re-run the bake, so a Job with
	// any declared/active scene variant falls back to a full re-derive (which re-bakes).  O(1) engine query.
	if( pJob.HasSceneVariants() ) {
		diags.push_back( "incremental: the Job has scene variant(s) whose override bake is whole-document; fall back to a full derive" );
		return 0;
	}

	// Slice 3 (stable-object apply) needs the object manager: it re-points existing
	// objects in place and reads each object's bbox to gate the TLAS rebuild.  Downcast
	// IJob -> IJobPriv exactly as the parser does (AsciiSceneParser.cpp ~470); an IJob
	// that is not an IJobPriv cannot be derived into at all -> full derive.
	IJobPriv* priv = dynamic_cast<IJobPriv*>( &pJob );
	IObjectManager* objMgr = priv ? priv->GetObjects() : 0;
	if( !objMgr ) {
		diags.push_back( "incremental: Job does not expose its object manager (IJobPriv); fall back to a full derive" );
		return 0;
	}

	// override_object guard (review P1.3): an override_object modifies an existing
	// object's transform in place and references its target by a ValueKind::String
	// `name`, INVISIBLE to the static reference graph -- so the closure of editing the
	// target would not include the override, and a re-point would erase its effective
	// transform; a rename of the target would also not rewrite the override's String.
	// Refuse whenever the Job carries any override (O(1), like the animation guard) until
	// the resolver traces String object references.
	if( pJob.GetObjectOverrideCount() > 0 ) {
		diags.push_back( "incremental: the Job has override_object(s) whose String target reference the static graph cannot trace; fall back to a full derive" );
		return 0;
	}

	// `source` guard: A DOCUMENT HOLDING ANY INSTANCING CHUNK TAKES THE FULL RE-DERIVE, WHOLESALE.
	//
	// THE HOP THIS CLOSES is an edit to a SUBTREE MEMBER of an instanced source.  The reference
	// graph's `parent` edge runs child -> parent, so editing `B` (where `B parent A`, and some `I
	// source A` copies A's subtree) reaches B's own chunk and STOPS: nothing REFERENCES B, so `I`
	// never enters the edit closure, the incremental apply re-points the live `B` alone, and every
	// `I.B` clone keeps the pre-edit binding.  The divergence is silent, it is live-only (the saved
	// bytes are correct, so it self-heals on reload), and it is exactly what §4 of
	// docs/agentic-redesign/87-recursive-scene-graph.md predicted.
	//
	// A DOCUMENT-WIDE GATE, and deliberately not a narrower one.  The per-chunk refusals below
	// cover the chunks the closure DOES reach (the instancing chunk itself; an entry still holding
	// a provenance row).  They cannot cover this case, because the whole defect is that the
	// closure never contains an instancing chunk at all -- a gate that only inspects the closure's
	// members is structurally unable to see the member-edit hop.  Nor is the count of `source`
	// chunks a proxy for "an edit is dangerous": we would have to know the edited chunk is inside
	// SOME source's subtree, and answering that per edit is the transitive-membership query the
	// static graph does not have.
	//
	// WHAT IT COSTS: every discrete edit in a scene that uses `source` even once -- a panel value
	// change, a gizmo drag commit, an agent param edit -- pays a full DeriveToJob (two, on the
	// agent path, which dry-runs first) instead of an O(closure . log N) re-point, and the object
	// manager is REPLACED so every caller must rebind (rc 2/3 rather than 1).  That is the honest
	// cost 87 §4 named when it concluded "the refusal survives step 3", and 87 step 3d was wrong
	// to delete it: the deleted form keyed on `instanceArrayCount`, which is 0 in a `source`-only
	// document, so the guard it removed never covered this case either -- the hole is older than
	// 3d -- but the four places 3d recorded the hop as CLOSED are what stopped it being found.
	//
	// WHAT WOULD LET IT GO -- both, not either:
	//   (1) the closure consumer-switch (Cst.h `RecordedGraph`): closure consumers still read
	//       BuildReferenceGraph, the STATIC descriptor-Reference graph, so the derive's recorded
	//       resolutions (which DO include the manager lookup ClonePlanBuilder performs on each
	//       subtree member) reach no consumer -- and the recording sink is opt-in via
	//       DeriveToJob's `outRecorded`, which every production call site passes nullptr for;
	//   (2) a TRANSITIVE structural edge from a `source` to every `parent`-descendant of its
	//       source, which MaintainedReferenceGraph cannot maintain incrementally today (its
	//       SetParamValue re-runs ComputeChunkRefs for the EDITED chunk only, while a `parent`
	//       edit anywhere changes subtree membership for every ancestor `source`).
	// O(1): a Document-level count maintained at parse / replace / insert / erase, NOT a per-edit
	// O(N) doc scan (which would make the incremental O(N) and fail the ~flat-in-N cost gate,
	// exactly as the animation guard above warns).
	if( doc.sourceInstanceCount > 0 ) {
		diags.push_back( "incremental: the document contains a `source` instancing chunk, and an edit to a "
			"member of an instanced SUBTREE is not traced to the instance (the `parent` edge runs child -> "
			"parent, so nothing references the member); fall back to a full derive" );
		return 0;
	}

	// Re-apply ONLY the given closure (DocEditClosure) into an ALREADY-derived Job
	// after an edit: recreate the non-object entities (drop + re-Finalize), but
	// re-point the closure's OBJECTS in place (slice 3) -- so the work is
	// O(closure . log N), not the O(N . log N) of a full DeriveToJob, AND object
	// addresses survive (the TLAS holds raw object pointers; recreating them was the
	// P1.1 UAF + the reason a non-spatial edit could not skip the TLAS, P1.2).
	// Deliberately does NOT ClearChunkParserState: the chunks OUTSIDE the closure are
	// unchanged and keep their applied state + the parsers' file-scope caches as a fresh
	// full parse would leave them.  (Cross-chunk caches are keyed by name and overwritten
	// on a chunk's re-Finalize, and the closure is applied entities-first then objects (by doc index) so a producer
	// re-applies before its consumer reads it.)
	//
	// Same refuse-all PASS-1 contract as DeriveToJob: validate the WHOLE closure
	// before touching the Job -- every chunk must be named AND of a type whose
	// re-derivation is a clean single-manager create-and-undo (DropChunkByCategory)
	// for entities, or an in-place re-point for standard_object/csg_object.  On any
	// validation failure nothing is dropped or applied.  PASS-2 below, unlike
	// DeriveToJob's (which now keeps going past a failed chunk -- bug-fix wave
	// 2026-08-28), DELIBERATELY KEEPS abort-on-first-failure + full rollback: this
	// function mutates a LIVE, already-derived Job in place rather than building a
	// throwaway/staging one, so "keep going and diagnose everything" is not an option
	// here -- a failure must restore the Job to its exact pre-edit state (see the
	// rollback machinery below), which only composes with stopping immediately.
	struct Pending { const IAsciiChunkParser* parser; ParseStateBag bag; NodeRef node; int index; ChunkCategory cat; std::string name; };
	std::vector<Pending> pending;
	pending.reserve( chunkIds.size() );
	for( NodeId id : chunkIds ) {
		NodeRef node;
		const int idx = DocIndexOfNodeId( doc, id, &node, nullptr );
		if( idx < 0 || !node || node->kind != NodeKind::Chunk ) { diags.push_back( "incremental: id is not a chunk in this document" ); return 0; }
		if( node->role == "let" ) { diags.push_back( "incremental: a let-binding edit requires a full derive (its consumers are not yet traced)" ); return 0; }   // #5 slice 3
		IAsciiChunkParser::ParamsList plist;
		// #5 slice 3: NO lets collected on the O(closure) path (collecting them is O(N)); a closure expr
		// referencing a let then refuses + the caller full-derives (which has the lets).  Plain / PI/E exprs work.
		const IAsciiChunkParser* parser = ResolveChunkParams( node, registry, plist, diags, LetBindings(), nullptr, &doc, (std::size_t)idx );
		if( !parser ) return 0;                                  // unknown chunk type (diagnostic pushed)
		const ChunkCategory cat = parser->Describe().category;
		std::string name; ParamValue( node.get(), "name", name );
		// Every closure chunk must be named (an entity drop / an object re-point both key
		// by name), else a blind re-Finalize would duplicate it.  Refuse -> full derive.
		if( name.empty() )                       { diags.push_back( node->role + ": incremental needs a name to drop+re-apply" ); return 0; }
		switch( cat ) {                                          // categories the apply handles
			case ChunkCategory::Material: case ChunkCategory::Geometry:
			case ChunkCategory::Object:  case ChunkCategory::Light: case ChunkCategory::Modifier: break;
			default: diags.push_back( node->role + ": incremental cannot fully drop this category (e.g. a scalar_painter has no colour-painter-manager entry for RemovePainter to drop); fall back to a full derive" ); return 0;
		}
		// Reversibility is PER-PARSER, not per-category (review P1.3/P1.5): refuse the
		// chunk types whose re-derivation is NOT a clean single-manager create-and-undo
		// (or, for objects, a clean in-place re-point), so nothing is half-applied (D51).
		if( cat == ChunkCategory::Material && pJob.IsMaterialComposed( name.c_str() ) ) {
			diags.push_back( node->role + " '" + name + "': composed material (creates helper painters); a typed RemoveMaterial is an incomplete undo -- fall back to a full derive" );
			return 0;
		}
		if( node->role == "translucent_material" ) {            // re-Finalize reads the ambient painter-colour cache
			diags.push_back( node->role + " '" + name + "': re-Finalize reads ambient thread-local parser state (the painter-colour cache); not yet incrementally re-derivable -- fall back to a full derive" );
			return 0;
		}
		{	// 87 step 3a: a `standard_object` carrying `source` is not applied by its own
			// Finalize at all -- Cst::DeriveToJob EXPANDS it at PASS-2, merging the source
			// chunk's bindings in.  Re-Finalizing it here would refuse (the parser's own
			// unexpanded-`source` backstop) or, worse under a future relaxation, build a
			// bare CONTAINER where the author asked for a copy.  Refuse -> full derive,
			// which re-expands from the document.  PER-CHUNK, matching the three refusals
			// below.
			//
			// UNREACHABLE IN PRACTICE TODAY, and kept anyway: the DOCUMENT-WIDE `source`
			// refusal above already returned 0 for any document holding an instancing
			// chunk, so no closure containing one gets this far.  This stays as the
			// per-chunk statement of WHY such a chunk cannot be re-Finalized, so that
			// retiring the document-wide gate (which needs the closure consumer-switch
			// plus a transitive source -> descendant edge) does not silently re-open it.
			// An earlier version of this comment said the document-wide guard "is not
			// needed here, because the expansion resolves the source object THROUGH the
			// manager and so records the source's chunk as a traced dependency"; that was
			// wrong twice over -- the recording sink is not armed in production, and the
			// recorded graph is not the closure consumer (see ExpandSourceInstance's
			// manager-resolution comment).
			//
			// CLEARING the slot needs the same fallback, for a DIFFERENT reason: the
			// chunk then derives as a plain container, which the in-place re-point
			// handles fine -- but the entry still carries the PROVENANCE row the
			// earlier expansion wrote, and provenance is retired only by RemoveItem /
			// Shutdown, neither of which an in-place re-point calls.  An incremental
			// commit would leave `GetObjectProvenance` still answering "(I, S)" for an
			// object that is no longer an instance of anything.
			//
			// That second gate keys on the LIVE PROVENANCE ROW, not on the `source`
			// TEXT, and both halves of that matter.  Keying on the text's PRESENCE
			// closes `source none` but NOT `source` DELETED -- and deleting the line
			// is the spelling production actually reaches (SceneEditor's agent-Undo of
			// an INSERTED `source` routes through ApplyCstParamRemoveChecked), which
			// would leave exactly the stale row this gate exists to retire.  Keying on
			// the text also OVER-applies in the other direction: `standard_object {
			// name C  source none  position 5 0 0 }` is a plain container that never
			// had provenance, and it would pay a ClearAll + full derive + manager
			// rebind on every gizmo drag commit FOREVER for a retirement that has
			// nothing to retire.  The row itself answers both questions exactly.
			std::string srcRef;
			const bool carriesSource = ( node->role == "standard_object"
			                          && ParamValue( node.get(), "source", srcRef )
			                          && !srcRef.empty() && srcRef != "none" );
			const bool hasProvenanceRow = ( cat == ChunkCategory::Object
			                             && objMgr->GetObjectProvenance( name.c_str(), 0, 0 ) );
			if( carriesSource || hasProvenanceRow ) {
				const std::string why = carriesSource
					? ( "carries `source " + srcRef + "` -- an INSTANCE, expanded by the full derive at PASS-2 "
					    "rather than by this chunk's own Finalize" )
					: std::string( "the live entry still carries the PROVENANCE row of an earlier `source` "
					               "expansion, and only a full re-derive retires it" );
				diags.push_back( node->role + " '" + name + "': " + why + "; fall back to a full derive" );
				return 0;
			}
		}
		if( node->role == "gltf_import" ) {                      // a single chunk whose Finalize spawns many entries
			diags.push_back( node->role + ": a bulk importer -- one chunk creates many objects/materials/painters/lights, which a typed RemoveGeometry cannot undo; fall back to a full derive" );
			return 0;
		}
		if( node->role == "rect_light" || node->role == "shape_light" ) {  // parse-time sugar: one chunk, four entities
			// Same class as gltf_import, one size down: rect_light's Finalize expands into a
			// painter + a luminaire material + a clippedplane geometry + a standard_object, and
			// shape_light's into a painter + a luminaire material + a sphere / ellipsoid / box /
			// cylinder geometry + a standard_object (ChunkParserRegistry.cpp,
			// RectLightAsciiChunkParser / ShapeLightAsciiChunkParser).  Their ChunkCategory is
			// Light -- which is right for every OTHER consumer (the editor files them under
			// Lights, the agent surface treats them as lights) -- but there is no ILight to drop,
			// so DropChunkByCategory's RemoveLight would fail while three real entities stayed
			// behind.  Refuse -> full derive, which rebuilds all four from the document (D51:
			// never a silent corruption).
			diags.push_back( node->role + " '" + name + "': one chunk expands into a painter, a "
				"luminaire material, a geometry and an object; a typed RemoveLight cannot undo that -- "
				"fall back to a full derive" );
			return 0;
		}
		if( cat == ChunkCategory::Object && node->role != "standard_object" && node->role != "csg_object" ) {
			// standard_object + csg_object are re-pointed in place (AddObject / AddCSGObject repoint).
			// A csg_object re-points op (SetOperation) + slots + its operands via AssignObjects with
			// the SAME operand pointers (operands are re-pointed in place, address-stable, so the
			// parent CSG's pointers stay valid).  An operand-REFERENCE change (obja/objb -> a different
			// object) is refused later in the snapshot loop (re-binding would un-hide a possibly-shared
			// dropped operand).  Any OTHER object-spawning chunk is unknown -- refuse -> full derive (D51).
			diags.push_back( node->role + ": only standard_object/csg_object are re-pointed in place incrementally (other object chunks need a full derive); fall back" );
			return 0;
		}
		ParseStateBag bag( &parser->Describe() );
		if( !DispatchChunkParameters( parser->Describe(), bag, plist ) ) { diags.push_back( node->role + ": invalid parameter(s) (see log)" ); return 0; }
		pending.push_back( Pending{ parser, std::move(bag), node, idx, cat, name } );
	}
	if( !diags.empty() ) return 0;
	// Apply ENTITIES first, then OBJECTS, each by doc index (DocEditClosure returns an unspecified
	// DFS order): respects producer-before-consumer AND re-points objects LAST (entity-only rollback).
	// (Among objects, doc index is a valid topological order: an object produces for another only
	// via a csg_object's operand refs, and RISE's scene language is definitions-before-use, so an
	// operand is always declared -- lower doc index -- before its parent CSG.  No object-rollback
	// is needed: the slot-precise preflight + AddCSGObject's resolve-first make a CSG re-point
	// unable to fail, so objects-last + entity-only rollback stays sufficient.)
	std::sort( pending.begin(), pending.end(), []( const Pending& a, const Pending& b ){ const bool ao = ( a.cat == ChunkCategory::Object ), bo = ( b.cat == ChunkCategory::Object ); if( ao != bo ) return !ao; return a.index < b.index; } );

	// ENTITY-ONLY ROLLBACK is sound because objects are Finalized LAST: the sort above groups
	// every non-object entity before every object (then doc index within each group) -- a stricter
	// topological order than pure doc index that still respects producer-before-consumer (entities
	// produce, objects consume).  So a re-Finalize failure is ALWAYS at an entity, BEFORE any object
	// is re-pointed; restoring the captured entities (Part A) fully restores the Job.  This SUPERSEDES
	// the earlier refusal of an "object before a later non-object entity" closure -- the entities-first
	// sort makes that interleaving safe to apply rather than refusing it (review #1 / workstream #3).

	// WHOLE-PLAN PREFLIGHT (review P1.7 -- atomicity): validate that EVERY drop target
	// exists AND EVERY reference resolves (SLOT-PRECISELY -- radiance_map colour-only, below)
	// BEFORE any mutation, so a rename / stale closure / dangling reference refuses with
	// NOTHING changed.  O(closure . log N): closure chunks x their references x a manager
	// lookup -- no O(N) doc scan.  The preflight catches the foreseeable failures up front;
	// any residual re-Finalize failure it cannot see (a non-reference invalid that
	// DispatchChunkParameters accepted; a numeric in a pure-painter slot) is caught at the
	// Finalize loop and ROLLED BACK to the pre-edit entities (Part A, review #1) -- so the
	// apply is atomic on EITHER path: nothing is left partially mutated.
	for( const Pending& p : pending ) {
		if( !EntityExists( *priv, p.cat, p.name ) ) {
			diags.push_back( p.node->role + " '" + p.name + "': not present in the derived Job (a rename or stale closure); refusing -- nothing mutated (review P1.7)" );
			return 0;
		}
		for( const ParameterDescriptor& pd : p.parser->Describe().parameters ) {
			if( pd.kind != ValueKind::Reference || pd.referenceCategories.empty() ) continue;   // tuple refs do not occur in the incremental's allowed categories
			std::string val;
			if( !ParamValue( p.node.get(), pd.name.c_str(), val ) ) continue;     // param absent
			// An OBJECT reference slot (geometry/material/modifier/shader/radiance_map/
			// interior_medium) is ALWAYS a named reference, never an inline numeric -- so a
			// numeric there is invalid AND dangerous: interior_medium is applied by a SEPARATE
			// SetObjectInteriorMedium call AFTER AddObject has already re-pointed (and possibly
			// MOVED) the object, so a numeric would slip past the literal-skip below, half-mutate
			// the object, then fail -- leaving a stale object + un-invalidated TLAS the
			// entity-only rollback cannot undo (review #1, 2nd pass).  Refuse it here, BEFORE any
			// mutation, so "return 0 == nothing changed" stays true.
			if( p.cat == ChunkCategory::Object && LooksNumeric( val ) ) {
				diags.push_back( p.node->role + " '" + p.name + "'." + pd.name + " -> '" + val + "': numeric in an object reference slot is invalid (would half-apply via the post-AddObject SetObjectInteriorMedium step); refusing -- nothing mutated (review #1)" );
				return 0;
			}
			if( val.empty() || val == "none" || LooksNumeric( val ) ) continue;   // explicit-none / numeric literal -> not a reference
			bool resolves = false;
			// SLOT-PRECISE resolution for the radiance_map {Painter} slot: AddObject binds it
			// via the COLOUR painter manager ONLY, so a scalar-only painter there does NOT
			// resolve.  Check colour-only rather than EntityExists(Painter) (which accepts
			// EITHER manager) -- this is what makes an OBJECT re-point unable to fail mid-apply
			// (Part A atomicity: objects are re-pointed AFTER the entities are recreated; an
			// object failure would strand already-re-pointed prior objects, which the
			// entity-only rollback below cannot restore -- so objects must never fail;
			// see the 87 caveat at the apply loop about `parent`).
			if( pd.name == "radiance_map" )
				resolves = ( priv->GetPainters() != 0 && priv->GetPainters()->GetItem( val.c_str() ) != 0 );
			else
				for( ChunkCategory rc : pd.referenceCategories ) if( EntityExists( *priv, rc, val ) ) { resolves = true; break; }
			if( !resolves ) {
				diags.push_back( p.node->role + " '" + p.name + "'." + pd.name + " -> '" + val + "': unresolved reference; refusing -- nothing mutated (review P1.7)" );
				return 0;
			}
		}
	}

	// Snapshot each closure OBJECT's pre-apply state: it MUST already exist (objects are
	// re-pointed in place, never created here -- a missing object means a rename or stale
	// closure, which aborts BEFORE any mutation).  The bbox snapshot lets the closure-
	// gated invariant pass tell a spatial edit (bbox changes) from a non-spatial one
	// (bbox identical), which is what makes "non-spatial edits skip the TLAS" valid.
	struct ObjState { IObjectPriv* obj; BoundingBox bbox; bool wasEmissive; bool clearMat, clearMod, clearShader, clearRad, clearMedium; };
	std::vector<ObjState> objStates;
	for( const Pending& p : pending ) {
		if( p.cat != ChunkCategory::Object ) continue;
		IObjectPriv* obj = objMgr->GetItem( p.name.c_str() );
		if( !obj ) {
			diags.push_back( p.node->role + " '" + p.name + "': object not in the derived Job (a rename or stale closure); aborting -- a full reset+re-derive is required" );
			return 0;   // nothing mutated yet
		}
		// CSG OPERAND-REFERENCE CHANGE (review #3c): a csg_object re-point re-binds operands via
		// AssignObjects, which UN-HIDES the dropped operand.  That matches a full derive for a
		// DEDICATED operand (it becomes a standalone visible object), but DIVERGES for a SHARED
		// operand (still an operand of another CSG -- a full derive keeps it hidden; the un-hide
		// would wrongly show it).  Sharing is common in canonical CSG scenes and detecting it is an
		// O(N) scan; instead refuse ANY operand-reference change (obja/objb resolves to a DIFFERENT
		// object than the live CSG holds) -> full derive.  Operand-INTERNAL edits keep the same
		// operand pointer (re-pointed in place, address-stable) and op/slot edits don't touch
		// operands, so those stay incremental.
		if( p.node->role == "csg_object" ) {
			if( RISE::Implementation::CSGObject* csg = dynamic_cast<RISE::Implementation::CSGObject*>( obj ) ) {
				const IObjectPriv* newA = objMgr->GetItem( p.bag.GetString( "obja", "none" ).c_str() );
				const IObjectPriv* newB = objMgr->GetItem( p.bag.GetString( "objb", "none" ).c_str() );
				if( newA != csg->GetOperandA() || newB != csg->GetOperandB() ) {
					diags.push_back( p.node->role + " '" + p.name + "': a CSG operand-reference change (obja/objb re-pointed to a different object) is not applied incrementally -- re-binding would un-hide a possibly-shared dropped operand; fall back to a full derive" );
					return 0;
				}
			}
		}
		// OPTIONAL-SLOT REMOVAL (workstream #3): an in-place re-point re-binds the slots the
		// chunk specifies but cannot CLEAR one it omits -- AssignX has no null-sentinel, the
		// parser passes 0 for a "none" material/modifier/shader/radiance, and skips
		// interior_medium when "none" -- so a removed slot would keep its stale binding.
		// Detect each removal (existing getter set AND chunk value "none") and CLEAR it
		// post-Finalize (below), matching a full derive of the edited chunk (a fresh object
		// has every optional slot unset).  A slot unchanged or CHANGED stays present (re-bound);
		// a slot ADDED was absent before (getter null) -> not a removal.
		const bool clearMat    = ( obj->GetMaterial()       && p.bag.GetString( "material",        "none" ) == "none" );
		const bool clearMod    = ( obj->GetModifier()       && p.bag.GetString( "modifier",        "none" ) == "none" );
		const bool clearShader = ( obj->GetShader()         && p.bag.GetString( "shader",          "none" ) == "none" );
		const bool clearRad    = ( obj->GetRadianceMap()    && p.bag.GetString( "radiance_map",    "none" ) == "none" );
		const bool clearMedium = ( obj->GetInteriorMedium() && p.bag.GetString( "interior_medium", "none" ) == "none" );
		// Capture whether this object's PRE-edit material emits (incl. a removal: clearMat with
		// a pre-edit emissive material drops an area-light emitter).  A switch emissive->non-
		// emissive (or a removal) means the post-edit-only check below would miss the bump and
		// leave a reused RayCaster's LightSampler listing a now-dark luminary.  Snapshot here,
		// while `obj` still holds the pre-edit material (mirrors SceneEditor's wasEmissive idiom).
		const IMaterial* preMat = obj->GetMaterial();
		objStates.push_back( ObjState{ obj, obj->getBoundingBox(), ( preMat && preMat->GetEmitter() ) != 0, clearMat, clearMod, clearShader, clearRad, clearMedium } );
	}

	// PART A (true in-place atomicity, review #1): capture each non-object closure entity
	// (addref'd) BEFORE the drop, so a mid-apply re-Finalize failure the preflight cannot
	// foresee (a numeric in a pure-painter slot; any non-reference DispatchChunkParameters
	// accepted) ROLLS BACK to the pre-edit Job rather than leaving a partial mutation.  Only
	// Material/Geometry/Light/Modifier reach the drop loop (the validation switch above) and
	// each is a single-manager entity, so capture/restore is a clean per-category GetItem /
	// RemoveItem+AddItem.  Objects are NOT captured: the slot-precise preflight above (incl.
	// radiance_map colour-only) guarantees every object re-point resolves, and the
	// entities-first sort above guarantees every object is re-pointed only AFTER every
	// entity is recreated -- so a failure can only occur at an entity, BEFORE any object is
	// touched, and restoring the entities fully restores the Job.
	struct EntCap { ChunkCategory cat; std::string name; IReference* old; };   // old addref'd (or null)
	std::vector<EntCap> entCaps;
	entCaps.reserve( pending.size() );
	for( const Pending& p : pending ) {
		if( p.cat == ChunkCategory::Object ) continue;
		IReference* old = 0;
		switch( p.cat ) {
			case ChunkCategory::Material: old = priv->GetMaterials()->GetItem( p.name.c_str() );  break;
			case ChunkCategory::Geometry: old = priv->GetGeometries()->GetItem( p.name.c_str() ); break;
			case ChunkCategory::Light:    old = priv->GetLights()->GetItem( p.name.c_str() );     break;
			case ChunkCategory::Modifier: old = priv->GetModifiers()->GetItem( p.name.c_str() );  break;
			default: break;
		}
		if( old ) old->addref();   // survive the drop; released on success or after the rollback restore
		entCaps.push_back( EntCap{ p.cat, p.name, old } );
	}
	// Restore the captured pre-edit entities after a mid-apply failure: remove any entity a
	// partial re-Finalize re-created (or left half-built), then re-add the captured original
	// under the same name.  Objects that were never re-pointed still hold their refs on these
	// originals, so the Job is byte-for-behaviour back to its pre-edit state.
	auto rollbackEntities = [&]() {
		for( const EntCap& c : entCaps ) {
			const char* nm = c.name.c_str();
			// Remove any entity the partial re-Finalize re-created (GUARD the remove so a
			// not-yet-recreated entity does not log a spurious "not found"), then restore the
			// captured original.  AddItem rejects a name collision, so the remove must precede.
			switch( c.cat ) {
				case ChunkCategory::Material: { auto* mgr = priv->GetMaterials();  if( mgr->GetItem( nm ) ) mgr->RemoveItem( nm ); if( c.old ) mgr->AddItem( dynamic_cast<IMaterial*>( c.old ), nm );  break; }
				case ChunkCategory::Geometry: { auto* mgr = priv->GetGeometries(); if( mgr->GetItem( nm ) ) mgr->RemoveItem( nm ); if( c.old ) mgr->AddItem( dynamic_cast<IGeometry*>( c.old ), nm ); break; }
				case ChunkCategory::Light:    { auto* mgr = priv->GetLights();     if( mgr->GetItem( nm ) ) mgr->RemoveItem( nm ); if( c.old ) mgr->AddItem( dynamic_cast<ILightPriv*>( c.old ), nm );    break; }
				case ChunkCategory::Modifier: { auto* mgr = priv->GetModifiers();  if( mgr->GetItem( nm ) ) mgr->RemoveItem( nm ); if( c.old ) mgr->AddItem( dynamic_cast<IRayIntersectionModifier*>( c.old ), nm ); break; }
				default: break;
			}
		}
	};
	auto releaseCaps = [&]() { for( const EntCap& c : entCaps ) if( c.old ) c.old->release(); };

	// Drop the NON-OBJECT entities; objects are NEVER dropped (re-pointed in place below)
	// so their addresses -- which the TLAS stores raw -- survive the edit.
	for( const Pending& p : pending ) {
		if( p.cat == ChunkCategory::Object ) continue;          // re-pointed, not dropped (slice 3)
		if( DropChunkByCategory( pJob, p.cat, p.name.c_str() ) ) continue;
		// A drop that finds nothing means the closure name does not match the derived Job
		// (a rename was applied; incremental is value-edit only).  Do NOT silently re-add
		// (that would leave BOTH entities -- review P1.4): ROLL BACK the entities already
		// dropped and abort.  (The preflight's EntityExists makes this unreachable -- every
		// drop target was confirmed present -- but the rollback keeps the contract honest.)
		rollbackEntities();
		releaseCaps();
		diags.push_back( p.node->role + " '" + p.name + "': drop found no such entity in the derived Job (a rename or stale closure?); rolled back -- nothing mutated" );
		return 0;
	}

	int count = 0;
	bool failed = false;
	{
		// Re-point mode: an object chunk's re-Finalize re-points its existing (stable)
		// object in place instead of creating a new one.  RAII so the flag is cleared on
		// every exit (even an early break), never leaking into a later full derive.
		struct RepointGuard { IJob& j; RepointGuard( IJob& j_ ) : j( j_ ) { j.SetIncrementalRepointMode( true ); } ~RepointGuard() { j.SetIncrementalRepointMode( false ); } } guard( pJob );
		for( Pending& p : pending ) {
			if( p.parser->Finalize( p.bag, pJob ) ) { ++count; continue; }
			diags.push_back( p.node->role + ": incremental apply failed (e.g. unresolved reference); see log" );
			failed = true;
			break;
		}
	}
	// PART A atomicity (review #1): on ANY re-Finalize failure, ROLL BACK to the pre-edit
	// entities and report nothing-applied.  Because the failure is always at an entity (the
	// slot-precise preflight keeps objects from failing, and objects are recreated last), no
	// object has been re-pointed yet -- so restoring the entities fully restores the Job.
	if( failed ) {
		rollbackEntities();
		releaseCaps();
		// 87 step 2: compose before bailing.  PART A's rollback is ENTITY-only,
		// and an object chunk's Finalize now has a second mutating step after
		// AddObject -- SetObjectParent, called UNCONDITIONALLY, including with
		// no parent, i.e. it DETACHES.  So a closure whose first object chunk
		// drops its `parent` line and whose second chunk then fails would leave
		// a detached ex-child holding a live stale parent world, which nothing
		// downstream repairs any more: the per-frame walk is link-sized and can
		// no longer see a node that has left the link map.  Before step 2 the
		// next PrepareForRendering self-healed it.
		//
		// Argued unreachable (the slot-precise preflight keeps objects from
		// failing, and no `parent` value can be edited through this path today)
		// -- but this file already carries two other unreachability arguments
		// propping up the same entity-only rollback, and this is the cheapest
		// possible way not to add a third.
		pJob.ComposeObjectHierarchy();
		return 0;
	}
	releaseCaps();   // success: the new entities are live; drop the capture refs on the originals

	// OPTIONAL-SLOT REMOVAL (workstream #3): every entity + object re-point succeeded, so clear
	// the slots the edit removed (recorded in the snapshot above) -- the in-place re-point could
	// not (no AssignX null-sentinel).  This is the missing clear path; it matches a full derive of
	// the edited chunk (a fresh object has every optional slot unset).  Done BEFORE the invariant
	// pass so its post-edit EMITTER check sees the cleared state: a removed emissive material ->
	// post-clear GetMaterial() is null, but the pre-edit wasEmissive snapshot fires the light-
	// topology bump (else a reused LightSampler keeps a now-dark luminary).  (Slot removals don't
	// change the bbox -- it is geometry-derived -- so they stay non-spatial.)  Safe without
	// rollback: objects never fail (slot-precise preflight) and clears cannot.
	// 87 CAVEAT: an object chunk's Finalize now has a SECOND mutating step after
	// AddObject/AddCSGObject -- SetObjectParent -- whose three semantic refusals
	// (self-parent, cycle, CSG operand) the preflight does not model; it only
	// checks that the parent NAME resolves.
	//
	// No divergence is reachable today, but NOT because a dry run catches it:
	// Job::DeriveEditedCstDocument_ dry-runs ONLY when requireFullDerivability
	// is set, and Job::ApplyCstParamEdit -- the GUI panel / gizmo route -- passes
	// FALSE, so its D2 fallback happens AFTER this incremental path has already
	// mutated the live Job.  The actual reason is narrower: NOTHING CAN EDIT A
	// `parent` VALUE THROUGH THIS PATH.  The panel's `parent` row is read-only
	// (ObjectIntrospection's IsRuntimeEditable omits it), and the one writer --
	// the agent -- goes through the GATED ApplyCstParamEditChecked.
	//
	// So it becomes reachable the moment a `parent` value can be changed on the
	// UNGATED route: a `set_parent` agent verb wired to ApplyCstParamEdit, or
	// the tree UI's reparent (87 step 4).  Extend the preflight then -- do not
	// assume a dry run is standing behind you.
	//
	// This is not the only such argument any more.  Job::AddObject /
	// AddObjectMatrix's incremental re-point carries a second one (it refuses a
	// name held by a CSGObject, unreachable because a chunk cannot change
	// keyword).  BOTH prop up PART A's entity-only rollback, whose whole premise
	// is that a re-Finalize failure lands at an ENTITY, before any object has
	// been re-pointed -- a partial object re-point is precisely what it cannot
	// undo.  If either argument stops holding, that rollback has to cover
	// objects too; a new refusal inside an object's Finalize is not a local
	// change.
	for( const ObjState& s : objStates ) {
		if( s.clearMat )    s.obj->ClearMaterial();
		if( s.clearMod )    s.obj->ClearModifier();
		if( s.clearShader ) s.obj->ClearShader();
		if( s.clearRad )    s.obj->ClearRadianceMap();
		if( s.clearMedium ) s.obj->ClearInteriorMedium();
	}

	// Closure-gated invariant pass (slice 3) -- NOT a verbatim RunObjectInvariantChain
	// (which invalidates the TLAS UNCONDITIONALLY, reproducing P1.2).  Invalidate the
	// top-level acceleration iff a re-pointed object's WORLD BBOX actually changed (a
	// geometry-extent or transform edit -- the genuinely spatial case); a non-spatial
	// edit (material/painter value) leaves every object's bbox identical, so the TLAS is
	// preserved (P1.2 dissolved).  Bump the light-topology generation iff the emitter set
	// may have changed: a re-pointed object whose material emits (its luminary footprint
	// may have moved or its emission changed), or any Light chunk recreated in the closure.
	// 87: re-bake the hierarchy BEFORE the bbox comparison.  A re-applied object
	// re-runs ClearAllTransforms + the component setters + finalize, which
	// rebuilds its LOCAL transform from scratch -- correct, but composed against
	// whatever parent world it happened to be holding.  Walking the tree here
	// also pushes the change down to DESCENDANTS the closure never touched,
	// which is why 87 needs no group-style incremental refusal: composition is
	// simply not part of the closure's job.  Those descendants are absent from
	// `objStates`, so their moved bounding boxes cannot be caught by the loop
	// below -- the walk's own "did any world matrix change" answer is what
	// makes the spatial gate sound.
	const bool composeMoved = pJob.ComposeObjectHierarchy();
	bool spatial = composeMoved;
	// The same argument that made `spatial` need the compose's answer applies to
	// the EMITTER gate, and for the same reason: the objects the compose moved
	// are DESCENDANTS, which are absent from `objStates`, so the loop below
	// cannot see that one of them emits.  Dragging a container that happens to
	// parent a lamp would otherwise re-render the lamp's geometry at its new
	// place while the light sampler kept sampling the old one.  Deliberately
	// CONSERVATIVE -- we do not know whether any moved descendant is emissive,
	// and the cost of being wrong in this direction is one sampler rebuild,
	// while the cost of being wrong in the other is a silent geometry/light
	// desync for the rest of the session.
	bool emitter = composeMoved;
	for( const ObjState& s : objStates ) {
		if( !BBoxEqual( s.bbox, s.obj->getBoundingBox() ) ) spatial = true;
		const IMaterial* m = s.obj->GetMaterial();
		// Bump if EITHER the pre-edit OR the post-edit material emits: post-edit catches
		// add/move/emission-change of an emitter; pre-edit catches an emissive->non-emissive
		// material switch that REMOVES one (the post-edit material no longer emits).
		if( s.wasEmissive || ( m && m->GetEmitter() ) ) emitter = true;
	}
	for( const Pending& p : pending ) if( p.cat == ChunkCategory::Light ) emitter = true;
	if( spatial ) pJob.InvalidateSpatialStructure();
	if( emitter ) pJob.BumpLightTopologyGeneration();
	return count;
}

//! The (category, name) the engine pre-registers in EVERY Job before any chunk is
//! applied (Job::InitializeContainers): the `none` material + painter, and the
//! `Default*` shader ops. A reference to one of these RESOLVES (to the default) --
//! it is NOT a dangling reference -- but yields no CST-chunk edge. CstResolverTest's
//! [namespace] check derives an empty scene and asserts EVERY one of these
//! (material/painter `none` + all 11 shader ops) is PRESENT in the Job -- so a
//! Job.cpp default RENAMED or REMOVED relative to this list fails the test. (Not
//! auto-caught: a Job-side ADDITION, or dropping an entry from THIS list alone --
//! the latter surfaces only when a scene references the now-unseeded default, as a
//! dangling diagnostic.)
static const std::vector<std::pair<ChunkCategory,std::string> >& RuntimeDefaultDefs()
{
	static const std::vector<std::pair<ChunkCategory,std::string> > d = {
		{ ChunkCategory::Material, "none" },
		{ ChunkCategory::Painter,  "none" },
		{ ChunkCategory::ShaderOp, "DefaultReflection" },
		{ ChunkCategory::ShaderOp, "DefaultRefraction" },
		{ ChunkCategory::ShaderOp, "DefaultEmission" },
		{ ChunkCategory::ShaderOp, "DefaultDirectLighting" },
		{ ChunkCategory::ShaderOp, "DefaultCausticPelPhotonMap" },
		{ ChunkCategory::ShaderOp, "DefaultCausticSpectralPhotonMap" },
		{ ChunkCategory::ShaderOp, "DefaultGlobalPelPhotonMap" },
		{ ChunkCategory::ShaderOp, "DefaultGlobalSpectralPhotonMap" },
		{ ChunkCategory::ShaderOp, "DefaultTranslucentPelPhotonMap" },
		{ ChunkCategory::ShaderOp, "DefaultShadowPhotonMap" },
		{ ChunkCategory::ShaderOp, "DefaultPathTracing" },
	};
	return d;
}

//! Is `s` ENTIRELY numeric tokens -- a single scalar (`0.5`) OR a whitespace tuple
//! of scalars (`1 2 3`, the inline `r g b` some scalar slots accept)? The caller
//! uses this to decide that a non-resolving reference VALUE is a LITERAL, not a
//! dangling reference: a dangling reference is a non-resolving NAME, and a number is
//! never a name. (A purely-numeric value in a PURE-reference slot -- e.g.
//! `reflectance 0.5` -- is therefore not flagged here as "dangling"; it is a
//! TYPE MISMATCH, which the full DeriveToJob refuses at apply time -- the static
//! reference-graph pass does not double-report it. This is the precise formulation
//! that replaced a fragile per-slot ref-or-literal flag: no slot allowlist to keep
//! complete, and inline `r g b` literals are handled too.)
static bool LooksNumeric( const std::string& s )
{
	bool any = false;
	for( const std::string& tok : SplitWs( s ) ) {
		if( tok.empty() ) continue;
		char* e = 0; std::strtod( tok.c_str(), &e );
		if( !( e && *e == '\0' ) ) return false;       // a non-numeric token -> this is a name, not a literal
		any = true;
	}
	return any;                                        // true iff at least one token and every token is numeric
}

// Synthetic `defs` sub-namespace keys for the Function namespace (review #3, 2nd pass):
// ChunkCategory::Function lumps Function1D + Function2D producers, but the engine resolves
// `function1d` via GetFunction1Ds and `function2d`/`heightfield_function` via GetFunction2Ds
// (AsciiSceneParser.cpp ~1386/1397).  Seeding producers + resolving consumers through these
// DIMENSION-PRECISE keys (well above any ChunkCategory enum value, so no collision in the
// (int,name) `defs` map) makes a same-named 1D/2D pair resolve to the RIGHT one in BOTH
// DocEditClosure and DocRename -- not the (Function,name) first-wins edge.  The coarse
// (Function,name) seed is KEPT too, for the conflation diagnostic + the DocRename guard
// (a name with >1 Function producer, incl. a dual-registered colour painter).  Workstream #2 dropped ior/film_ior's Function category, so no CONSUMER reaches the coarse key.
static const int kFunc1DSubCat = 100001;
static const int kFunc2DSubCat = 100002;

//! The dimension-precise Function sub-namespace a reference PARAM resolves into, or 0 for a
//! coarse {Function} consumer.  `pd` is the param's OWN ParameterDescriptor -- the caller (the
//! single one, in ComputeChunkRefs) has already resolved it against the chunk's ChunkDescriptor
//! and dereferenced it, so in practice it is never null; the `pd &&` guard on the pipe test below
//! is defensive only, and a hypothetical null caller would get just the name-keyed cases.
static int FunctionSubNamespace( const std::string& paramName, const ParameterDescriptor* pd )
{
	// Function1D consumers: NAME-keyed, because none of these declare a `ParameterPipe` (they
	// predate S17's pipe audit and are out of this workstream's scope -- Function1D has no
	// dual-registration ambiguity to motivate widening past a name list the way Function2D does
	// below).  Each is resolved by the engine through pFunc1DManager, a DIMENSION-SPECIFIC
	// manager, so the resolver must match (the descriptor's coarse {Function}/{Painter,Function}
	// is spurious for these; resolving coarsely first-wins to a same-named 2D function was the
	// misbind): function1d + the directvolumerendering RGBA transfer_* channels (Job.cpp ~6248);
	// homogeneous_medium's sigma(lambda) curves (Job::AddHomogeneousMediumSpectral).
	if( paramName == "function1d" ) return kFunc1DSubCat;
	if( paramName == "transfer_red" || paramName == "transfer_green" ||
	    paramName == "transfer_blue" || paramName == "transfer_alpha" ) return kFunc1DSubCat;
	if( paramName == "absorption_spectral" || paramName == "scattering_spectral" ) return kFunc1DSubCat;
	// Function2D consumers with no declared pipe yet (transfer_spectral) still need the name
	// check; `function2d` / `heightfield_function` are KEPT here too even though both now also
	// satisfy the pipe-based rule below (harmless redundancy -- both routes agree) so this list
	// stays the complete, self-contained record of every NAME-keyed Function2D case.
	if( paramName == "function2d" || paramName == "heightfield_function" ||
	    paramName == "transfer_spectral" ) return kFunc2DSubCat;
	// EVERY OTHER Function2D-piped parameter (ChunkParserRegistry.cpp's
	// `p.semantics.pipe = ParameterPipe::Function2D`, audited against the real Job.cpp resolve
	// code -- see ChunkDescriptor.h's ParameterPipe doc comment) resolves through this SAME
	// dimension-precise sub-namespace, keyed on the DECLARATION rather than a hand-maintained
	// name list.  Until 2026-09-06 this was instead a literal `paramName == "displacement" ||
	// "child_a" || "child_b"` list: those three params were declared {Painter} (or, for
	// heightfield_function, coarse {Function}) with no `pipe` at all, so resolving them coarsely
	// via (Painter,name) MISSED a plf2d target (plf2d is not in the painter managers) -- a
	// stale-closure bug (review #3, 3rd-pass exhaustive table) patched with a name list rather
	// than fixing the underlying under-declaration.  The proper fix landed the same day: those
	// parameters (displaced_geometry.displacement, composite_function2d_painter.child_a/.child_b,
	// sdf_geometry.heightfield_function, scalar_painter.function2d, function2d_painter.function2d)
	// now all carry `ParameterPipe::Function2D` + `referenceCategories = {Painter, Function}`, so
	// a BRAND-NEW Function2D-piped parameter is covered here with NO Cst.cpp edit -- proven by
	// CstResolverTest's registry-wide [func2d-registry-invariant] case, which walks the LIVE
	// registry (no hardcoded param list) rather than asserting against these five by name.
	if( pd && pd->semantics.pipe == ParameterPipe::Function2D ) return kFunc2DSubCat;
	return 0;
}

//! Runtime-default target sentinel (file scope: shared by BuildReferenceNamespace + ComputeChunkRefs):
//! a (category,name) that resolves to an engine runtime default, NOT a CST chunk.
static const NodeId kRuntimeDefaultTarget = -1;

//! PASS A (factored out for #4b): build the (category,name) -> producer-NodeId namespace `defs`
//! (runtime defaults first, then CST producers first-wins; the dimension-precise Function 1D/2D
//! sub-namespace seeds; the colour-painter <-> Function2D dual-register seeds) AND emit the
//! order-insensitive painter same-name ALIAS mutual dependents into `aliasDeps`.  Producers do not
//! change on a reference-VALUE edit, so the maintained graph holds + REUSES this across such edits.
static std::map<std::pair<int,std::string>, NodeId> BuildReferenceNamespace(
	const Document& doc, const std::vector<NodeRef>& items,
	const std::map<std::string, const IAsciiChunkParser*>& registry,
	std::map<NodeId, std::set<NodeId> >& aliasDeps, std::vector<std::string>& diags )
{
	std::map<std::pair<int,std::string>, NodeId> defs;
	for( const auto& rd : RuntimeDefaultDefs() )
		defs[ std::pair<int,std::string>( (int)rd.first, rd.second ) ] = kRuntimeDefaultTarget;
	std::map<std::string, std::vector<std::pair<bool,NodeId> > > painterNs;   // painter name -> ALL same-name painters (isScalar, NodeId): detect + alias the cross-manager conflation (P1.4)
	std::unordered_set<std::string> painterAliasDiagnosed;   // emit the cross-manager conflation diagnostic ONCE per name
	std::map<std::string, bool> funcChunkNames;  // names produced by a Function-category chunk: detect the 1D/2D conflation (#3)
	for( size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( c->kind != NodeKind::Chunk ) continue;
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
		if( it == registry.end() ) continue;                       // unknown chunk: not a target
		std::string name;
		if( !ParamValue( c.get(), "name", name ) || name.empty() ) continue;   // unnamed: not referenceable
		const ChunkCategory cat = it->second->Describe().category;
		// Painter namespace-conflation diagnostic + CONSERVATIVE ALIAS (review P1.4): scalar
		// and colour painters share ChunkCategory::Painter but live in SEPARATE managers; the
		// defs key cannot tell them apart, so an edge to a name present in BOTH resolves to
		// only one (first-wins) and may disagree with the derive (which picks by the referring
		// slot's painter sub-type).  Unlike the Function 1D/2D axis (disambiguated by param
		// name), the colour/scalar fact lives only in each Finalize's manager choice, not the
		// descriptor -- so precise per-slot resolution is deferred.  Flag it, and ALIAS the two
		// painters (below) so closure stays a correct SUPERSET; DocRename refuses the rename.
		if( cat == ChunkCategory::Painter ) {
			const bool isScalar = ( c->role == "scalar_painter" );
			const NodeId thisId = DocNodeIdAt( doc, (int)i );
			// CONSERVATIVE closure alias (review P1.4): scalar + colour painters share ChunkCategory::
			// Painter but live in SEPARATE managers; the (Painter,name) edge first-wins to ONE, so a
			// consumer the engine binds to the OTHER (by its slot's colour/scalar sub-type) would be
			// MISSED from that painter's closure.  Make them MUTUAL dependents so editing EITHER
			// re-derives BOTH -- a SUPERSET closure (never misses; may over-include).  ORDER-INSENSITIVE
			// (review P1, 2nd): link THIS painter to EVERY previously-seen same-name painter of the
			// OPPOSITE kind, so the alias set is (all colour-q) x (all scalar-q) -- a function of the
			// chunk SET, NOT its declaration order.  (A first-seen-only alias was order-sensitive for
			// >=3 same-named mixed painters: a reorder changed the dependents while the COMMUTATIVE
			// stamp stayed put -> 'same stamp, different graph'.  All-cross-kind depends only on the
			// chunk set, which the per-chunk stamp already reflects.)  DocRename still REFUSES the rename.
			// COST: O(K) per painter -> O(K^2) for K same-name MIXED-kind painters (the alias edge count
			// K_scalar x K_colour is the true dependent set, not waste); degenerate only -- real scenes
			// have distinct painter names, so this stays within BuildReferenceGraph's O(N log N) bound.
			for( const std::pair<bool,NodeId>& prev : painterNs[ name ] ) {
				if( prev.first != isScalar && prev.second != thisId ) {
					if( painterAliasDiagnosed.insert( name ).second )
						diags.push_back( "painter '" + name + "': defined in BOTH the colour and scalar painter managers; the (category,name) graph cannot disambiguate them, so the edge is imprecise (review P1.4) -- aliased for a CONSERVATIVE (superset) closure" );
					aliasDeps[ thisId ].insert( prev.second );
					aliasDeps[ prev.second ].insert( thisId );
				}
			}
			painterNs[ name ].push_back( std::make_pair( isScalar, thisId ) );
		}
		const std::pair<int,std::string> key( (int)cat, name );
		// Function-namespace CONFLATION diagnostic (review #3): Function1D and Function2D
		// producers share ChunkCategory::Function but live in SEPARATE managers
		// (GetFunction1Ds/GetFunction2Ds), and the derive does TYPED lookups.  The
		// dimension-precise consumers -- function1d (1D), function2d/heightfield_function (2D),
		// transfer_* (1D) -- now resolve through the 1D/2D sub-namespace keys seeded just below,
		// so a same-named 1D+2D pair binds the RIGHT one.  No consumer reaches the coarse path now (#2 dropped ior/film_ior's Function); what stays ambiguous is
		// the RENAME rewrite for a conflated name -- flag
		// it so the rewrite is refused; DocRename refuses the rename outright.
		if( cat == ChunkCategory::Function ) {
			if( defs.find( key ) != defs.end() || funcChunkNames.count( name ) )
				diags.push_back( "function '" + name + "': another Function (1D/2D) producer or a dual-registered painter shares this name; reference edges to it are imprecise in the COARSE {Function} namespace -- function1d/function2d/heightfield_function/transfer_* consumers resolve dimension-precisely (review #3, 2nd pass), but the COARSE rename rewrite cannot disambiguate them, so DocRename refuses renaming such a name (review #3)" );
			funcChunkNames[ name ] = true;
			// DIMENSION-PRECISE seed (review #3, 2nd pass): a piecewise_linear_function is a
			// Function1D (Job::AddPiecewiseLinearFunction -> GetFunction1Ds); a
			// piecewise_linear_function2d is a Function2D.  Seed the sub-namespace key so a
			// `function1d`/`function2d` consumer binds the RIGHT one even when a same-named 1D+2D
			// pair exists -- the coarse (Function,name) key below cannot (first-wins).
			if( c->role == "piecewise_linear_function" ) {
				const std::pair<int,std::string> sk( kFunc1DSubCat, name );
				if( defs.find( sk ) == defs.end() ) defs[sk] = DocNodeIdAt( doc, (int)i );
			} else if( c->role == "piecewise_linear_function2d" ) {
				const std::pair<int,std::string> sk( kFunc2DSubCat, name );
				if( defs.find( sk ) == defs.end() ) defs[sk] = DocNodeIdAt( doc, (int)i );
			}
			// A piecewise_linear_function ALSO dual-registers into the COLOUR painter manager
			// (Job::AddPiecewiseLinearFunction -> Function1DSpectralPainter), so it is
			// referenceable from a colour slot (e.g. lambertian_material.reflectance <plf1d>).
			// Seed (Painter, name) too -- the REVERSE of the colour-painter -> Function2D
			// dual-register below -- so that reference RESOLVES (matching the derive's colour
			// pPntManager->GetItem) instead of being a false dangling + a missed closure/rename
			// edge (review #3a).  It is the one Job::Add* for a FUNCTION-category chunk that
			// ALSO registers into the colour-painter manager (the reverse of the ~40 colour
			// painters that register Painter->Function2D, seeded below), so only this 1D
			// function needs it; piecewise_linear_function2d does not.  A same-name
			// colour painter makes (Painter,name) ambiguous (flag); DocRename's #3 guard already
			// refuses such a rename (its funcProducers count includes colour painters).
			if( c->role == "piecewise_linear_function" ) {
				const std::pair<int,std::string> pkey( (int)ChunkCategory::Painter, name );
				if( defs.find( pkey ) != defs.end() )
					diags.push_back( "painter '" + name + "': a piecewise_linear_function and a colour painter share this name; the {Painter} reference edge is imprecise (review #3a)" );
				else
					defs[pkey] = DocNodeIdAt( doc, (int)i );
			}
		}
		if( defs.find( key ) == defs.end() ) defs[key] = DocNodeIdAt( doc, (int)i );   // first-wins; defaults already seeded
		// A COLOUR painter dual-registers in the Function-2D manager (Job::Add*Painter), so
		// it is also referenceable as a {Function} reference (e.g. scalar_painter.function2d,
		// function2d_painter.function2d).  Seed (Function, name) too, so such a reference
		// RESOLVES to the painter (matching the derive's pFunc2DManager->GetItem) instead of
		// being a false dangling + a missed closure/rename edge (review P1.4 sibling).  The
		// scalar painter manager has no such dual-register, so scalar_painter is excluded.
		if( cat == ChunkCategory::Painter && c->role != "scalar_painter" ) {
			if( funcChunkNames.count( name ) )   // a real Function chunk also produces this name -> ambiguous (review #3)
				diags.push_back( "function '" + name + "': a Function chunk and a dual-registered painter share this name; the {Function} reference edge is imprecise (review #3)" );
			const std::pair<int,std::string> fkey( (int)ChunkCategory::Function, name );
			if( defs.find( fkey ) == defs.end() ) defs[fkey] = DocNodeIdAt( doc, (int)i );
			// A colour painter dual-registers as a Function2D, so seed the 2D sub-namespace too
			// (review #3, 2nd pass) -- so a `function2d` consumer binds it dimension-precisely.
			const std::pair<int,std::string> sk( kFunc2DSubCat, name );
			if( defs.find( sk ) == defs.end() ) defs[sk] = DocNodeIdAt( doc, (int)i );
		}
	}

	return defs;
}

//! One chunk's reference edges, reverse-dependent TARGETS, and per-chunk stamp `cs`, resolved
//! against the already-built namespace `defs` (PASS B's per-chunk body, factored out for #4b so the
//! maintained graph can re-run it for a SINGLE chunk on a reference edit -- O(this chunk's refs . log N)).
//! `deps` lists each resolved target T (chunkId != T); the caller adds chunkId to dependents[T].
struct ChunkRefs { std::vector<ReferenceUse> edges; std::vector<NodeId> deps; unsigned long long cs; };
static ChunkRefs ComputeChunkRefs( const Document& doc,
	const std::map<std::pair<int,std::string>, NodeId>& defs, const ChunkDescriptor& desc,
	const NodeRef& c, NodeId chunkId, std::vector<std::string>& diags,
	std::vector<UnresolvedReference>* unresolved = nullptr )
{
	g_chunkRefsComputed.fetch_add( 1, std::memory_order_relaxed );   // #4b cost gate (1 per incremental edit; N per full build)
	ChunkRefs out;
	unsigned long long cs;   // this chunk's accumulator (the body's first line seeds the FNV-1a basis)
	auto mix = [&cs]( const std::string& s ) {
		for( unsigned char ch : s ) { cs ^= ch; cs *= 1099511628211ULL; }
		cs ^= (unsigned char)'|'; cs *= 1099511628211ULL;
	};
	cs = 1469598103934665603ULL;   // #4: reset the per-chunk stamp accumulator (FNV-1a basis)
	mix( c->role );
	mix( std::to_string( (long long)chunkId ) );
	{ std::string nm; if( ParamValue( c.get(), "name", nm ) ) mix( nm ); }
	std::map<std::string,int> occ;   // per-role occurrence index, for DocParamId
	for( const auto& kid : c->kids ) {
		if( kid->kind != NodeKind::Param ) continue;
		const std::string role = kid->role;
		const int thisOcc = occ[role]++;
		const ParameterDescriptor* pd = 0;
		for( const auto& p : desc.parameters ) if( p.name == role ) { pd = &p; break; }
		if( !pd ) continue;
		// piecewise_linear_function2d.cp embeds a Function1D NAME as the 2nd whitespace token of
		// each repeatable `cp` row ("<x> <function1d_name>"; consumed by AddPiecewiseLinearFunction2D
		// via GetFunction1Ds).  The descriptor types `cp` as an opaque String, so the generic
		// reference pass below would SKIP it -- trace it explicitly here (against the dimension-
		// precise 1D sub-namespace) so DocEditClosure includes this Function2D consumer when its
		// Function1D dependency is edited (review #2, 2nd pass).  Rename still REFUSES a doc with
		// this chunk (the name is a String token, not a rewritable Reference -- the cp guard);
		// fold the name into the stamp so a maintained graph notices an edit to it.
		if( c->role == "piecewise_linear_function2d" && role == "cp" ) {
			const std::vector<std::string> toks = SplitWs( ParamNodeValue( kid.get() ) );
			if( toks.size() >= 2 ) {
				const std::string& fname = toks[1];
				mix( fname );
				if( !fname.empty() && fname != "none" && !LooksNumeric( fname ) ) {
					std::map<std::pair<int,std::string>, NodeId>::const_iterator d = defs.find( std::pair<int,std::string>( kFunc1DSubCat, fname ) );
					if( d != defs.end() && d->second != kRuntimeDefaultTarget && d->second != 0 ) {
						const NodeId srcParam = DocParamId( doc, chunkId, role, thisOcc );
						mix( std::to_string( (long long)srcParam ) );
						mix( std::to_string( (long long)d->second ) );   // #4a-fix (review P1): fold the RESOLVED target (cp)
						out.edges.push_back( ReferenceUse{ srcParam, d->second } );
						if( chunkId != d->second ) out.deps.push_back( d->second );
					}
				}
			}
			continue;   // handled this cp row; do not fall through to the generic skip
		}
		std::vector<std::string> refVals;
		if( pd->kind == ValueKind::Reference ) {
			refVals.push_back( ParamNodeValue( kid.get() ) );
		} else if( !pd->tupleKinds.empty() ) {
			const std::vector<std::string> toks = SplitWs( ParamNodeValue( kid.get() ) );
			for( size_t k = 0; k < pd->tupleKinds.size() && k < toks.size(); ++k )
				if( pd->tupleKinds[k] == ValueKind::Reference ) refVals.push_back( toks[k] );
		} else {
			continue;
		}
		for( const std::string& val : refVals ) {
			mix( val );                                        // stamp: every reference value
			if( val.empty() || val == "none" ) continue;       // explicit-none / empty: not an edge
			NodeId target = 0;
			// DIMENSION-PRECISE resolution for the function consumers FunctionSubNamespace maps
			// (function1d/transfer_* -> 1D; function2d/heightfield_function/transfer_spectral -> 2D)
			// (review #3, 2nd pass): resolve through the 1D/2D sub-namespace key so a same-named
			// Function1D+Function2D pair binds the RIGHT one (matching the engine's GetFunction1Ds vs
			// GetFunction2Ds), NOT the (Function,name) first-wins edge.  No fallback to the coarse key:
			// a function1d naming only a Function2D is genuinely dangling (the engine would not find it
			// either).  The else branch iterates a param's declared referenceCategories (params NOT in
			// FunctionSubNamespace).  ior/film_ior were the coarse {Painter,Function} slots here until
			// workstream #2 dropped their phantom Function category (ResolveOrDiagnoseScalar resolves a
			// scalar-then-colour painter, then numeric -- NEVER a Function manager), so they are now
			// {Painter}; their residual painter colour-vs-scalar ambiguity is handled by the alias above.
			const int fsub = FunctionSubNamespace( role, pd );
			if( fsub != 0 ) {
				std::map<std::pair<int,std::string>, NodeId>::const_iterator d = defs.find( std::pair<int,std::string>( fsub, val ) );
				if( d != defs.end() ) target = d->second;
			} else {
				for( ChunkCategory rc : pd->referenceCategories ) {
					std::map<std::pair<int,std::string>, NodeId>::const_iterator d = defs.find( std::pair<int,std::string>( (int)rc, val ) );
					if( d != defs.end() ) { target = d->second; break; }
				}
			}
			if( target == kRuntimeDefaultTarget ) continue;    // resolves to a runtime default: not an edge, not dangling
			if( target == 0 ) {                                // unresolved
				// A non-resolving value is a DANGLING reference only if it is a
				// NAME (not entirely numeric tokens). An entirely-numeric value is
				// a LITERAL (a scalar `0.5` or an inline `r g b`) -- not a dangling
				// reference. (In a pure-reference slot a numeric is a TYPE mismatch,
				// which DeriveToJob refuses at apply time; the static pass does not
				// double-report it. See LooksNumeric.)
				if( !LooksNumeric( val ) ) {
					diags.push_back( c->role + "." + role + " -> '" + val + "': unresolved reference" );
					// Structured sibling of the string above (same site, same exclusions): lets a
					// caller (e.g. AgentSession's post-insert warning) attribute the dangling
					// reference to its chunk/param/value without re-parsing prose.
					if( unresolved )
						unresolved->push_back( UnresolvedReference{ chunkId, c->role, role, val } );
				}
				continue;
			}
			// sourceValueNodeId is the param NodeId (a tuple's ref tokens share it;
			// value-atom sub-identity is the deferred refinement).
			const NodeId srcParam = DocParamId( doc, chunkId, role, thisOcc );
			// Fold the SOURCE-PARAM NodeId into the stamp (review #4, the param-level
			// twin of P1.5's chunk-NodeId fold): edges are KEYED by the source param's
			// NodeId, so removing a reference param and reinserting an identical one (a
			// NEW param NodeId, same value) changes the graph's edge -- the stamp must
			// move, else a reused graph carries a dead source NodeId.  A value edit
			// preserves the param NodeId, so this stays stable across non-structural edits.
			mix( std::to_string( (long long)srcParam ) );
			mix( std::to_string( (long long)target ) );   // #4a-fix (review P1): fold the RESOLVED target -- a first-wins producer reorder changes this edge but not the per-chunk-value sum
			out.edges.push_back( ReferenceUse{ srcParam, target } );
			// Reverse adjacency, computed in this SAME pass (slice 5): the referenced
			// chunk -> the chunk that references it, so DocEditClosure( id, graph ) is a
			// pure O(closure . log N) BFS over a reused graph.  (Self-reference excluded,
			// matching the from-scratch DocEditClosure; duplicate dependents are harmless
			// -- the BFS dedups via its seen-set.)
			if( chunkId != target ) out.deps.push_back( target );
		}
	}
	out.cs = cs;
	return out;
}

ReferenceGraph BuildReferenceGraph( const Document& doc, std::vector<std::string>* diagnostics,
	std::vector<UnresolvedReference>* unresolved )
{
	std::vector<std::string> local;
	std::vector<std::string>& diags = diagnostics ? *diagnostics : local;
	ReferenceGraph graph;
	// Commutative per-chunk stamp (#4): each chunk folds its reference-relevant content into a
	// FRESH per-chunk FNV-1a accumulator `cs` (reset per chunk in PASS B); the graph stamp is the
	// SUM of those per-chunk stamps.  Order-independent + per-chunk, so the maintained graph can
	// update it INCREMENTALLY on a single-chunk edit (subtract the chunk's old cs, add its new) in
	// O(chunk) rather than an O(N) re-fold -- preserving "same stamp => same graph" (each cs folds
	// the chunk's distinct NodeId, so distinct chunks don't sum-cancel).
	// Each cs ALSO folds the RESOLVED TARGET NodeId of every edge (review P1): the per-chunk SUM is
	// order-independent, but namespace resolution is FIRST-WINS (order-sensitive) on duplicate
	// definitions -- so without the target fold a producer reorder would change an edge while leaving
	// the sum unchanged.  Folding the resolved target makes the stamp reflect the resolved GRAPH,
	// restoring "same stamp => same graph".  (Incremental-safe: the target is recomputed in the
	// per-chunk resolution that the maintained graph re-runs on an edit -- #4b.)
	unsigned long long stamp = 0;                          // sum of per-chunk stamps (0 == empty graph)

	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();
	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );

	// PASS A -- the (category,name) namespace + painter-alias dependents (factored: #4b).
	std::map<std::pair<int,std::string>, NodeId> defs = BuildReferenceNamespace( doc, items, registry, graph.dependents, diags );

	// PASS B -- resolve each chunk's references against that namespace; the graph is the union of
	// the per-chunk edges/dependents and the COMMUTATIVE SUM of the per-chunk stamps (#4).
	for( size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( c->kind != NodeKind::Chunk ) continue;
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
		if( it == registry.end() ) continue;
		const NodeId chunkId = DocNodeIdAt( doc, (int)i );
		ChunkRefs cr = ComputeChunkRefs( doc, defs, it->second->Describe(), c, chunkId, diags, unresolved );
		graph.edges.insert( graph.edges.end(), cr.edges.begin(), cr.edges.end() );
		for( NodeId t : cr.deps ) graph.dependents[ t ].insert( chunkId );
		stamp += cr.cs;
	}
	graph.stamp = stamp;
	return graph;
}

std::vector<ReferenceUse> TraceReferences( const Document& doc, std::vector<std::string>* diagnostics )
{
	return BuildReferenceGraph( doc, diagnostics ).edges;   // slice 1: thin wrapper over the stamped resolver
}

//! Rebuild `chunk` with its (occ-th) param named `role` set to `newValue`
//! (re-tokenised into pvalue tokens, single-spaced), KEEPING the pname + its
//! leading trivia and SHARING every other child by pointer (structural sharing).
//! Returns the original chunk unchanged if that param is not present.
static NodeRef WithParamValue( const NodeRef& chunk, const std::string& role, int occ, const std::string& newValue )
{
	if( !chunk ) return chunk;
	std::vector<NodeRef> kids;
	kids.reserve( chunk->kids.size() );
	int seen = -1;
	bool done = false;
	for( const auto& k : chunk->kids ) {
		if( !done && k->kind == NodeKind::Param && k->role == role && ++seen == occ ) {
			std::vector<NodeRef> pk;
			for( const auto& pkid : k->kids ) {
				if( pkid->kind == NodeKind::Token && pkid->role == "pvalue" ) break;   // keep pname + leading trivia only
				pk.push_back( pkid );                                                   // shared
			}
			const std::vector<std::string> toks = SplitWs( newValue );
			for( size_t t = 0; t < toks.size(); ++t ) {
				if( t ) pk.push_back( Leaf( NodeKind::Trivia, " ", "" ) );
				pk.push_back( Leaf( NodeKind::Token, toks[t], "pvalue" ) );
			}
			kids.push_back( Internal( NodeKind::Param, std::move(pk), role ) );
			done = true;
			continue;
		}
		kids.push_back( k );   // shared
	}
	if( !done ) return chunk;
	return Internal( NodeKind::Chunk, std::move(kids), chunk->role );
}

Document DocSetParamValue( const Document& doc, NodeId chunkId, const std::string& role, int occ, const std::string& newValue, int* visits )
{
	if( visits ) *visits = 0;
	NodeRef chunk;
	const int index = DocIndexOfNodeId( doc, chunkId, &chunk, visits );
	if( index < 0 || !chunk || chunk->kind != NodeKind::Chunk ) return doc;   // not a top-level chunk: no-op
	NodeRef edited = WithParamValue( chunk, role, occ, newValue );
	if( edited.get() == chunk.get() ) return doc;                             // param absent: no-op
	return DocReplaceItem( doc, index, edited, visits );                      // chunk NodeId PRESERVED (D44)
}

// Like WithParamValue but INSERTS `role value` as a new param line before the closing brace when the param
// is ABSENT (a defaulted slot the scene text omits).  Returns the chunk UNCHANGED only if !chunk.
static NodeRef WithParamValueOrInsert( const NodeRef& chunk, const std::string& role, int occ,
                                       const std::string& newValue, bool* inserted )
{
	if( inserted ) *inserted = false;
	if( !chunk ) return chunk;
	NodeRef edited = WithParamValue( chunk, role, occ, newValue );            // existing param: replace in place
	if( edited.get() != chunk.get() ) return edited;

	// Param ABSENT.  Insert only makes sense for the FIRST occurrence with a non-empty value; refuse
	// otherwise (the sole caller passes occ 0 + a validated non-empty value, but a future caller must not
	// silently get a value-less or mis-indexed insert).
	const std::vector<std::string> toks = SplitWs( newValue );
	if( occ != 0 || toks.empty() ) return chunk;

	// Build a `role value` Param.
	std::vector<NodeRef> pk;
	pk.push_back( Leaf( NodeKind::Token, role, "pname" ) );
	for( size_t t = 0; t < toks.size(); ++t ) {
		pk.push_back( Leaf( NodeKind::Trivia, " ", "" ) );
		pk.push_back( Leaf( NodeKind::Token, toks[t], "pvalue" ) );
	}
	NodeRef param = Internal( NodeKind::Param, std::move( pk ), role );

	// Splice it in before the closing brace.  The "braces on their own lines" rule is an AUTHORING convention
	// the CST TOKENIZER/parser does not enforce structurally -- ParseChunk stays lossless and still builds a
	// tree for a brace-sharing / one-line chunk (see ChunkBraceViolations' header) -- DERIVE now hard-rejects
	// such a chunk (ResolveChunkParams, PASS-1), so by the time an already-loaded document reaches this editor
	// function no live chunk carries the violation.  This splice logic is kept defensive regardless (a
	// programmatically-built or mid-edit node shape this function has never actually been proven immune to),
	// so we still must NOT rely on the pre-brace trivia ending the previous line.  Emit a LEADING newline
	// whenever that trivia lacks one,
	// else the relexer would glue the new tokens onto the previous param's value list (the parser's same-line
	// value loop) and save+reload would derive a DIFFERENT scene than the in-memory edit -- a Slice-4 round-trip
	// corruption (the material's slot reference would absorb `<role> <value>` and become unresolvable).
	std::vector<NodeRef> kids; kids.reserve( chunk->kids.size() + 3 );
	bool placed = false;
	for( const auto& k : chunk->kids ) {
		if( !placed && k->kind == NodeKind::Token && k->role == "rbrace" ) {
			if( kids.empty() || kids.back()->text.find( '\n' ) == std::string::npos )
				kids.push_back( Leaf( NodeKind::Trivia, "\n", "" ) );
			kids.push_back( param );
			kids.push_back( Leaf( NodeKind::Trivia, "\n", "" ) );
			placed = true;
		}
		kids.push_back( k );
	}
	if( !placed ) {                                                          // no rbrace (defensive): append
		kids.push_back( Leaf( NodeKind::Trivia, "\n", "" ) );
		kids.push_back( param );
	}
	if( inserted ) *inserted = true;
	return Internal( NodeKind::Chunk, std::move( kids ), chunk->role );
}

// P5 Slice 3 expansion (object transform): remove EVERY occurrence of param `role` from a chunk (and one
// whitespace-only Trivia immediately following each, so no blank line is left).  Idempotent: returns the chunk
// unchanged when the param is absent.  Used to strip the now-dead position/orientation/quaternion/scale params
// when an object transform is committed as the authoritative `matrix` param (a coexisting component param would
// be masked AND would emit the parser's `matrix overrides ...` warning).  Brace-safe: it only DROPS nodes, and
// params are token-separated by Trivia, so the surviving neighbours never glue.
static NodeRef WithParamRemoved( const NodeRef& chunk, const std::string& role )
{
	if( !chunk ) return chunk;
	bool found = false;
	for( const auto& k : chunk->kids ) if( k->kind == NodeKind::Param && k->role == role ) { found = true; break; }
	if( !found ) return chunk;
	std::vector<NodeRef> kids; kids.reserve( chunk->kids.size() );
	bool justRemoved = false;
	for( const auto& k : chunk->kids ) {
		if( k->kind == NodeKind::Param && k->role == role ) { justRemoved = true; continue; }
		if( justRemoved && k->kind == NodeKind::Trivia && k->text.find_first_not_of( " \t\r\n" ) == std::string::npos ) {
			justRemoved = false; continue;
		}
		justRemoved = false;
		kids.push_back( k );
	}
	return Internal( NodeKind::Chunk, std::move( kids ), chunk->role );
}

Document DocRemoveParam( const Document& doc, NodeId chunkId, const std::string& role, int* visits )
{
	if( visits ) *visits = 0;
	NodeRef chunk;
	const int index = DocIndexOfNodeId( doc, chunkId, &chunk, visits );
	if( index < 0 || !chunk || chunk->kind != NodeKind::Chunk ) return doc;
	NodeRef edited = WithParamRemoved( chunk, role );
	if( edited.get() == chunk.get() ) return doc;
	return DocReplaceItem( doc, index, edited, visits );
}

// Shared-undo U1 P1-2 fix (round 1): like WithParamRemoved but targets ONLY the `occ`-th occurrence of `role`
// (0 = first, matching WithParamValue's `++seen == occ` counting convention) -- every OTHER occurrence (and its
// own trivia) is left byte-exact, shared by pointer.  Same brace-safe tidy as WithParamRemoved: the one
// whitespace-only Trivia kid immediately following the removed Param is also dropped so no blank line survives.
// Returns the original chunk unchanged if the `occ`-th occurrence is absent.
static NodeRef WithParamRemovedOcc( const NodeRef& chunk, const std::string& role, int occ )
{
	if( !chunk || occ < 0 ) return chunk;
	int seen = -1;
	bool found = false;
	for( const auto& k : chunk->kids ) if( k->kind == NodeKind::Param && k->role == role && ++seen == occ ) { found = true; break; }
	if( !found ) return chunk;
	std::vector<NodeRef> kids; kids.reserve( chunk->kids.size() );
	seen = -1;
	bool justRemoved = false;
	for( const auto& k : chunk->kids ) {
		if( !justRemoved && k->kind == NodeKind::Param && k->role == role && ++seen == occ ) { justRemoved = true; continue; }
		if( justRemoved && k->kind == NodeKind::Trivia && k->text.find_first_not_of( " \t\r\n" ) == std::string::npos ) {
			justRemoved = false; continue;
		}
		justRemoved = false;
		kids.push_back( k );
	}
	return Internal( NodeKind::Chunk, std::move( kids ), chunk->role );
}

Document DocRemoveParamOcc( const Document& doc, NodeId chunkId, const std::string& role, int occ, int* visits )
{
	if( visits ) *visits = 0;
	NodeRef chunk;
	const int index = DocIndexOfNodeId( doc, chunkId, &chunk, visits );
	if( index < 0 || !chunk || chunk->kind != NodeKind::Chunk ) return doc;
	NodeRef edited = WithParamRemovedOcc( chunk, role, occ );
	if( edited.get() == chunk.get() ) return doc;
	return DocReplaceItem( doc, index, edited, visits );
}

Document DocSetOrAddParamValue( const Document& doc, NodeId chunkId, const std::string& role, int occ, const std::string& newValue, bool* inserted, int* visits )
{
	if( visits ) *visits = 0;
	if( inserted ) *inserted = false;
	NodeRef chunk;
	const int index = DocIndexOfNodeId( doc, chunkId, &chunk, visits );
	if( index < 0 || !chunk || chunk->kind != NodeKind::Chunk ) return doc;   // not a top-level chunk: no-op
	NodeRef edited = WithParamValueOrInsert( chunk, role, occ, newValue, inserted );
	if( edited.get() == chunk.get() ) return doc;                             // nothing changed
	return DocReplaceItem( doc, index, edited, visits );                      // chunk NodeId PRESERVED (D44)
}

//! Rewrite a TUPLE value's reference tokens: at each tupleKinds position that is a
//! Reference whose token == oldName, substitute newName; rejoin single-spaced. (So a
//! rename rewrites a tuple referrer -- advanced_shader's `shaderop <ref> <min> <max>`,
//! voronoi's `gen <x> <y> <ref>` -- not just plain Reference params.)
static std::string RewriteTupleRef( const std::string& val, const std::vector<ValueKind>& tk, const std::string& oldName, const std::string& newName )
{
	std::vector<std::string> toks = SplitWs( val );
	for( size_t k = 0; k < tk.size() && k < toks.size(); ++k )
		if( tk[k] == ValueKind::Reference && toks[k] == oldName ) toks[k] = newName;
	std::string out;
	for( size_t i = 0; i < toks.size(); ++i ) { if( i ) out += ' '; out += toks[i]; }
	return out;
}

Document DocRename( const Document& doc, NodeId chunkId, const std::string& newName, std::vector<std::string>* diagnostics )
{
	std::vector<std::string> local;
	std::vector<std::string>& diags = diagnostics ? *diagnostics : local;

	NodeRef target = DocResolveNodeId( doc, chunkId );
	if( !target || target->kind != NodeKind::Chunk ) { diags.push_back( "rename: target is not a chunk" ); return doc; }
	std::string oldName;
	if( !ParamValue( target.get(), "name", oldName ) ) { diags.push_back( "rename: target chunk has no name parameter (nothing to rename)" ); return doc; }

	// Name VALIDATION (P1.7): an empty new name would emit a value-less parameter;
	// refuse it. A no-op rename (newName == oldName) returns the document unchanged.
	if( newName.empty() ) { diags.push_back( "rename: empty new name refused" ); return doc; }
	if( newName == oldName ) return doc;

	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();
	std::map<std::string, const IAsciiChunkParser*>::const_iterator tit = registry.find( target->role );
	const int targetCat = ( tit == registry.end() ) ? -1 : (int)tit->second->Describe().category;

	// COLLISION vs the RUNTIME-DEFAULT namespace (P1.7): renaming to a `none` /
	// `Default*` of the target's category would collide with the engine's pre-
	// registered default (the manager registered it first and rejects the dup),
	// silently re-pointing referrers to the default. Refuse.
	for( const std::pair<ChunkCategory,std::string>& rd : RuntimeDefaultDefs() )
		if( (int)rd.first == targetCat && rd.second == newName ) {
			diags.push_back( "rename: '" + newName + "' is a reserved runtime default of this category; refused" );
			return doc;
		}

	std::vector<NodeRef> items;
	SeqToVec( doc.items, items );

	// ANIMATION guard (P1.8 / timeline): a `timeline` references its element + owning
	// animation as ValueKind::String, invisible to the static reference graph -- a
	// rename could leave such a reference dangling. Until the resolver traces timeline
	// references (slice 5), refuse a rename when the document has ANY Animation chunk
	// (a one-shot rename can afford the O(N) scan).
	for( const NodeRef& c : items ) {
		if( c->kind != NodeKind::Chunk ) continue;
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
		if( it != registry.end() && it->second->Describe().category == ChunkCategory::Animation ) {
			diags.push_back( "rename: document has an animation/timeline whose String references the static graph cannot rewrite; refused" );
			return doc;
		}
	}

	// override_object guard (review P1.3): an override_object references its TARGET object
	// by a ValueKind::String `name`, invisible to the static reference graph -- renaming
	// the target would not rewrite that String, leaving it dangling. Refuse when the
	// document has any override_object (a one-shot rename can afford the O(N) scan).
	for( const NodeRef& c : items ) {
		if( c->kind == NodeKind::Chunk && c->role == "override_object" ) {
			diags.push_back( "rename: document has an override_object whose String target reference the static graph cannot rewrite; refused" );
			return doc;
		}
	}

	// PAINTER namespace-conflation guard (review P1.4): scalar and colour painters share
	// ChunkCategory::Painter but live in SEPARATE managers, and the reference graph keys
	// edges by (category, name) -- so if the target painter's name ALSO names a painter
	// in the OTHER sub-namespace (scalar_painter vs a colour painter), the traced edges to
	// that name are ambiguous and the rename cannot reliably rewrite the referrers.
	// Refuse. (Precise per-manager resolution is a deferred refinement -- 21-*.md S5.)
	if( targetCat == (int)ChunkCategory::Painter ) {
		const bool targetIsScalar = ( target->role == "scalar_painter" );
		for( const NodeRef& c : items ) {
			if( c->kind != NodeKind::Chunk ) continue;
			std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
			if( it == registry.end() || it->second->Describe().category != ChunkCategory::Painter ) continue;
			if( ( c->role == "scalar_painter" ) == targetIsScalar ) continue;   // same sub-namespace -> the normal collision check handles it
			std::string nm;
			if( ParamValue( c.get(), "name", nm ) && nm == oldName ) {
				diags.push_back( "rename: painter '" + oldName + "' exists in BOTH the colour and scalar painter managers; the (category,name) reference graph cannot disambiguate them -- refused (review P1.4)" );
				return doc;
			}
		}
	}

	// piecewise_linear_function2d guard (review #2): its `cp` entries embed a Function1D
	// NAME extracted at runtime.  BuildReferenceGraph now TRACES those refs for closure
	// (review #2, 2nd pass), but the descriptor declares `cp` as an opaque String -- a
	// ValueKind::String token, NOT a rewritable Reference param -- so the rename rewrite
	// loop cannot substitute the name and would leave it dangling. Refuse when the document
	// has any such chunk (one-shot rename can afford the O(N) scan). Sibling of the
	// override / timeline String-reference guards.
	for( const NodeRef& c : items ) {
		if( c->kind == NodeKind::Chunk && c->role == "piecewise_linear_function2d" ) {
			diags.push_back( "rename: document has a piecewise_linear_function2d whose `cp` entries embed Function1D names traced for closure but held as String tokens the rename cannot rewrite; refused (review #2)" );
			return doc;
		}
	}

	// FUNCTION namespace-conflation guard (review #3): Function1D and Function2D producers
	// share ChunkCategory::Function but live in SEPARATE managers (typed lookups), and a
	// colour painter dual-registers as Function2D -- so a name with >1 Function-namespace
	// producer is ambiguous.  CLOSURE resolves the function1d/function2d consumers
	// dimension-precisely (review #3, 2nd pass), but the rename REWRITE path is coarse: it
	// cannot rewrite a value shared across the 1D/2D managers without mis-targeting a
	// 1D/2D (or dual-registered colour-painter) referrer, so renaming such a chunk/painter is refused.
	if( targetCat == (int)ChunkCategory::Function ||
	    ( targetCat == (int)ChunkCategory::Painter && target->role != "scalar_painter" ) ) {
		int funcProducers = 0;
		for( const NodeRef& c : items ) {
			if( c->kind != NodeKind::Chunk ) continue;
			std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
			if( it == registry.end() ) continue;
			const ChunkCategory cc = it->second->Describe().category;
			const bool producesFunction = ( cc == ChunkCategory::Function ) ||
			                              ( cc == ChunkCategory::Painter && c->role != "scalar_painter" );  // colour painter dual-registers as Function2D
			if( !producesFunction ) continue;
			std::string nm;
			if( ParamValue( c.get(), "name", nm ) && nm == oldName ) ++funcProducers;
		}
		if( funcProducers > 1 ) {
			diags.push_back( "rename: '" + oldName + "' has multiple Function-namespace producers (Function1D/Function2D chunk and/or dual-registered painter); the (Function,name) graph cannot disambiguate them -- refused (review #3)" );
			return doc;
		}
	}

	// One walk: detect a same-category CST name COLLISION + map each param NodeId ->
	// (owning chunk, role, occ, tupleKinds*) so a referrer edge becomes a rewrite.
	struct Loc { NodeId chunk; std::string role; int occ; const std::vector<ValueKind>* tk; };
	std::map<NodeId, Loc> paramLoc;
	bool collision = false;
	for( size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( c->kind != NodeKind::Chunk ) continue;
		const NodeId cid = DocNodeIdAt( doc, (int)i );
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
		const ChunkDescriptor* desc = ( it == registry.end() ) ? 0 : &it->second->Describe();
		if( cid != chunkId && desc && (int)desc->category == targetCat ) {
			std::string nm;
			if( ParamValue( c.get(), "name", nm ) && nm == newName ) collision = true;
		}
		std::map<std::string,int> occ;
		for( const auto& kid : c->kids ) {
			if( kid->kind != NodeKind::Param ) continue;
			const std::string role = kid->role;
			const int thisOcc = occ[role]++;
			const std::vector<ValueKind>* tk = 0;
			if( desc ) for( const ParameterDescriptor& p : desc->parameters ) if( p.name == role ) { if( !p.tupleKinds.empty() ) tk = &p.tupleKinds; break; }
			paramLoc[ DocParamId( doc, cid, role, thisOcc ) ] = Loc{ cid, role, thisOcc, tk };
		}
	}

	if( collision ) {
		diags.push_back( "rename: '" + newName + "' already names another chunk of the same category; refused (would create an ambiguous name)" );
		return doc;
	}

	// Rewrite EVERY referrer (D14: rewrite-all-or-refuse, never a partial rename).
	// Every edge in the static graph is rewritable -- a plain Reference param via its
	// whole value, a TUPLE param by substituting the reference token(s).  By the time we
	// reach this loop, every chunk->chunk reference in the v6/v7 grammar is EITHER a
	// rewritable graph edge OR has been REFUSED above, so no referrer is left dangling:
	//   * plain Reference params + Reference tuple-tokens -> rewritten here.
	//   * BOTH cross-category dual-registers are seeded as graph edges in PASS A so they
	//     ARE rewritten: a colour painter referenceable as {Function} (-> Function2D), and
	//     a piecewise_linear_function referenceable as a colour {Painter} (review #3a).
	//   * a timeline's ValueKind::String element/animation ref -> refused (animation guard).
	//   * an override_object's String target -> refused (override guard, P1.3).
	//   * a piecewise_linear_function2d's `cp`-embedded Function1D names: TRACED for closure
	//     (review #2, 2nd pass) but a ValueKind::String token, not a rewritable Reference, so the
	//     rename still refuses (the cp guard, review #2).
	//   * a name shared across a sub-namespace the (category,name) graph cannot disambiguate
	//     (colour/scalar painter, Function1D/2D, plf1d/colour-painter) -> refused (the
	//     conflation guards, P1.4 / #3 / #3a).
	// (The redesign's name-path expr(...) value sublanguage -- the only thing that could add
	// an untraceable chunk reference -- is not in the grammar; expression_function2d's
	// def/expr name no chunks, only u/v/params, so they create no rewritable edge.)
	std::vector<ReferenceUse> uses = TraceReferences( doc );
	Document result = DocSetParamValue( doc, chunkId, "name", 0, newName );   // the rename itself (NodeId preserved)
	for( const ReferenceUse& u : uses ) {
		if( u.targetNodeId != chunkId ) continue;
		std::map<NodeId, Loc>::const_iterator l = paramLoc.find( u.sourceValueNodeId );
		if( l == paramLoc.end() ) continue;
		if( l->second.tk ) {
			NodeRef pnode = DocResolveNodeId( doc, u.sourceValueNodeId );
			const std::string cur = pnode ? ParamNodeValue( pnode.get() ) : std::string();
			result = DocSetParamValue( result, l->second.chunk, l->second.role, l->second.occ, RewriteTupleRef( cur, *l->second.tk, oldName, newName ) );
		} else {
			result = DocSetParamValue( result, l->second.chunk, l->second.role, l->second.occ, newName );
		}
	}
	return result;
}

std::vector<NodeId> DocEditClosure( NodeId changedChunkId, const ReferenceGraph& graph )
{
	// Pure reverse-BFS over the PRE-BUILT reverse adjacency (slice 5): the re-derive
	// closure is the changed chunk + everything that transitively references it (D25).
	// No document re-trace -- O(closure . log N) over a maintained / cached graph.  The
	// returned ORDER is unspecified (callers needing document order sort by index).
	std::vector<NodeId> closure;
	std::unordered_set<long long> seen;
	std::vector<NodeId> stack; stack.push_back( changedChunkId );
	while( !stack.empty() ) {
		const NodeId n = stack.back(); stack.pop_back();
		if( !seen.insert( (long long)n ).second ) continue;
		closure.push_back( n );
		std::map<NodeId, std::set<NodeId> >::const_iterator d = graph.dependents.find( n );
		if( d != graph.dependents.end() ) for( NodeId r : d->second ) if( !seen.count( (long long)r ) ) stack.push_back( r );
	}
	return closure;
}

std::vector<NodeId> DocEditClosure( const Document& doc, NodeId changedChunkId )
{
	// From-scratch: trace the whole graph (O(N log N)), then the same BFS.  A caller
	// that holds a maintained graph should call the (id, graph) overload directly to
	// skip the re-trace (CstEditCostTest measures both).
	return DocEditClosure( changedChunkId, BuildReferenceGraph( doc ) );
}

//! Would editing `paramRole` of chunk `chunkId` change the reference graph?  The chunk's
//! `name` re-resolves every edge TO it; a Reference (plain or tuple) param re-targets an
//! edge FROM it; any other (non-reference) value leaves the edges + NodeId-keyed
//! dependents untouched.  Decided in O(log N) -- resolve the chunk via the NodeId index
//! (O(log N)) + scan its descriptor (O(params)); the basis for MaintainedReferenceGraph's
//! reuse-vs-update decision (#4b: non-affecting -> reuse; reference/cp -> INCREMENTAL;
//! name / alias-involved-painter -> rebuild) WITHOUT recomputing the (O(N)) stamp.
static bool IsGraphAffectingParam( const Document& doc, NodeId chunkId, const std::string& paramRole )
{
	if( paramRole == "name" ) return true;
	NodeRef c = DocResolveNodeId( doc, chunkId );
	if( !c || c->kind != NodeKind::Chunk ) return true;   // unknown -> conservatively rebuild
	// piecewise_linear_function2d.cp embeds a TRACED Function1D reference (review #2, 2nd pass),
	// but it is a ValueKind::String param -- the descriptor scan below would call it graph-neutral
	// and the maintained graph would reuse a stale graph on a cp edit.  Treat it as affecting.
	if( c->role == "piecewise_linear_function2d" && paramRole == "cp" ) return true;
	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();
	std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
	if( it == registry.end() ) return true;
	for( const ParameterDescriptor& pd : it->second->Describe().parameters )
		if( pd.name == paramRole )
			return pd.kind == ValueKind::Reference || !pd.tupleKinds.empty();
	return true;   // param not in the descriptor -> conservatively rebuild
}

void MaintainedReferenceGraph::RebuildAll()
{
	// Full O(N) build: the namespace m_defs + the per-chunk caches (edges/cs) + the held graph.  Used
	// by the ctor and by name/alias edits (which can change the namespace or the painter alias set).
	m_graph = ReferenceGraph();
	m_chunkEdges.clear();
	m_chunkCs.clear();
	m_aliasInvolved.clear();
	std::vector<NodeRef> items;
	SeqToVec( m_doc.items, items );
	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();
	std::vector<std::string> diags;
	m_defs = BuildReferenceNamespace( m_doc, items, registry, m_graph.dependents, diags );
	// PASS A wrote the painter same-name ALIAS mutual dependents into m_graph.dependents; their keys
	// are exactly the alias-involved painters (the alias is mutual).  Capture them BEFORE PASS B adds
	// reference-edge dependents, so a reference edit on such a painter falls back to a rebuild (its
	// dependents entry is shared with the alias and can't be incrementally distinguished).
	for( std::map<NodeId, std::set<NodeId> >::const_iterator it = m_graph.dependents.begin(); it != m_graph.dependents.end(); ++it )
		m_aliasInvolved.insert( it->first );
	unsigned long long stamp = 0;
	for( size_t i = 0; i < items.size(); ++i ) {
		const NodeRef& c = items[i];
		if( c->kind != NodeKind::Chunk ) continue;
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = registry.find( c->role );
		if( it == registry.end() ) continue;
		const NodeId chunkId = DocNodeIdAt( m_doc, (int)i );
		ChunkRefs cr = ComputeChunkRefs( m_doc, m_defs, it->second->Describe(), c, chunkId, diags );
		m_graph.edges.insert( m_graph.edges.end(), cr.edges.begin(), cr.edges.end() );
		for( NodeId t : cr.deps ) m_graph.dependents[ t ].insert( chunkId );
		m_chunkEdges[ chunkId ] = cr.edges;
		m_chunkCs[ chunkId ] = cr.cs;
		stamp += cr.cs;
	}
	m_graph.stamp = stamp;
	m_edgesDirty = false;
}

MaintainedReferenceGraph::MaintainedReferenceGraph( const Document& doc )
	: m_doc( doc ), m_lastRebuilt( true ), m_edgesDirty( false )
{
	RebuildAll();
}

const ReferenceGraph& MaintainedReferenceGraph::Graph() const
{
	// Lazily reflatten the edges view from the per-chunk source of truth (an incremental edit updates
	// m_chunkEdges + dependents + stamp but DEFERS the flat edges rebuild).  Emitted in DOC order to
	// match a from-scratch BuildReferenceGraph.  dependents + stamp are already current (no work here).
	if( m_edgesDirty ) {
		m_graph.edges.clear();
		std::vector<NodeRef> items;
		SeqToVec( m_doc.items, items );
		for( size_t i = 0; i < items.size(); ++i ) {
			const NodeRef& c = items[i];
			if( c->kind != NodeKind::Chunk ) continue;
			const NodeId chunkId = DocNodeIdAt( m_doc, (int)i );
			std::map<NodeId, std::vector<ReferenceUse> >::const_iterator it = m_chunkEdges.find( chunkId );
			if( it != m_chunkEdges.end() )
				m_graph.edges.insert( m_graph.edges.end(), it->second.begin(), it->second.end() );
		}
		m_edgesDirty = false;
	}
	return m_graph;
}

void MaintainedReferenceGraph::SetParamValue( NodeId chunkId, const std::string& paramRole, int occurrence, const std::string& value )
{
	// Decide the edit class from the EDIT (O(log N)), NOT by recomputing the stamp (O(N)).
	const bool affecting = IsGraphAffectingParam( m_doc, chunkId, paramRole );
	if( !affecting ) {                                  // non-reference value edit: graph unchanged -> reuse
		m_doc = DocSetParamValue( m_doc, chunkId, paramRole, occurrence, value );
		m_lastRebuilt = false;
		return;
	}
	// A NAME edit changes the namespace; a reference edit on an ALIAS-involved painter could touch a
	// dependents entry shared with the painter same-name alias (the merged set can't tell them apart);
	// an unknown chunk (no cached cs) is conservative.  All -> full O(N) rebuild.
	if( paramRole == "name" || m_aliasInvolved.count( chunkId ) ||
	    m_chunkCs.find( chunkId ) == m_chunkCs.end() ) {
		m_doc = DocSetParamValue( m_doc, chunkId, paramRole, occurrence, value );
		RebuildAll();
		m_lastRebuilt = true;
		return;
	}
	// INCREMENTAL (#4b): a reference/cp edit on a non-alias chunk re-resolves ONLY this chunk against
	// the held namespace m_defs (producers are unchanged by a value edit) -> O(this chunk's refs . log N),
	// atop the O(log N + this chunk's params) DocSetParamValue every edit pays.
	const std::vector<ReferenceUse> oldEdges = m_chunkEdges[ chunkId ];
	const unsigned long long oldCs = m_chunkCs[ chunkId ];
	std::set<NodeId> oldDeps;
	for( const ReferenceUse& e : oldEdges ) if( e.targetNodeId != chunkId ) oldDeps.insert( e.targetNodeId );

	m_doc = DocSetParamValue( m_doc, chunkId, paramRole, occurrence, value );

	NodeRef c = DocResolveNodeId( m_doc, chunkId );
	const std::map<std::string, const IAsciiChunkParser*>& registry = DescriptorRegistry();
	std::map<std::string, const IAsciiChunkParser*>::const_iterator it =
		( c && c->kind == NodeKind::Chunk ) ? registry.find( c->role ) : registry.end();
	if( !c || c->kind != NodeKind::Chunk || it == registry.end() ) {   // defensive: can't resolve -> rebuild
		RebuildAll(); m_lastRebuilt = true; return;
	}
	std::vector<std::string> diags;
	ChunkRefs cr = ComputeChunkRefs( m_doc, m_defs, it->second->Describe(), c, chunkId, diags );
	std::set<NodeId> newDeps;
	for( const ReferenceUse& e : cr.edges ) if( e.targetNodeId != chunkId ) newDeps.insert( e.targetNodeId );

	// Reverse adjacency: drop this chunk from targets it no longer references (cleaning emptied entries
	// to match a from-scratch build), add it to the new ones.  Safe to erase: this chunk is NOT
	// alias-involved (guarded above), so it never sits in dependents[T] via the alias.
	for( NodeId t : oldDeps ) if( !newDeps.count( t ) ) {
		std::map<NodeId, std::set<NodeId> >::iterator d = m_graph.dependents.find( t );
		if( d != m_graph.dependents.end() ) { d->second.erase( chunkId ); if( d->second.empty() ) m_graph.dependents.erase( d ); }
	}
	for( NodeId t : newDeps ) m_graph.dependents[ t ].insert( chunkId );

	m_graph.stamp += cr.cs - oldCs;                  // commutative: swap this chunk's per-chunk stamp (modular, exact)
	m_chunkEdges[ chunkId ] = cr.edges;
	m_chunkCs[ chunkId ] = cr.cs;
	m_edgesDirty = true;                             // the flat edges view is now stale (Graph() reflattens lazily)
	m_lastRebuilt = false;
}

int    DocItemCount  ( const Document& doc ) { return SeqCount( doc.items ); }
size_t DocByteWidth  ( const Document& doc ) { return SeqBytes( doc.items ); }
int    DocNewlineCount( const Document& doc ) { return SeqNl( doc.items ); }
unsigned long DebugItemStatWalks() { return g_itemStatWalks.load( std::memory_order_relaxed ); }
unsigned long DebugReparseOldVisits() { return g_reparseOldVisits.load( std::memory_order_relaxed ); }
unsigned long DebugReflowLabelWrites() { return g_reflowLabelWrites.load( std::memory_order_relaxed ); }
unsigned long DebugParamMatchVisits() { return g_paramMatchVisits.load( std::memory_order_relaxed ); }
unsigned long DebugChunkRefsComputed() { return g_chunkRefsComputed.load( std::memory_order_relaxed ); }

int DocItemAtByteOffset( const Document& doc, size_t offset, NodeRef* outItem, size_t* outStart, int* visits )
{
	NodeRef item; size_t start = 0; int v = 0;
	int idx = SeqAtOffset( doc.items, offset, 0, 0, item, start, v );
	if( visits ) *visits = v;
	if( idx >= 0 ) { if( outItem ) *outItem = item; if( outStart ) *outStart = start; }
	return idx;
}

//! The exact reverse of DocItemAtByteOffset: byte offset where top-level
//! item `index` STARTS in SerializeCst(doc). Descends the SAME cached
//! byte-width aggregate spine (SeqOffsetAt, mirroring SeqAtOffset's
//! convention) -- O(log N), no serialization walk. Out-of-range `index`
//! (negative or >= DocItemCount) is refused, returning the sentinel
//! (size_t)-1 (never a valid offset, since offset 0 is only valid for
//! index 0 -- any other index's true offset is > 0).
size_t DocByteOffsetOfItem( const Document& doc, int index )
{
	if( index < 0 || index >= SeqCount( doc.items ) ) return (size_t)-1;
	int visits = 0;
	return SeqOffsetAt( doc.items, index, visits );
}

bool DocByteRangeOfParam( const Document& doc, NodeId chunkId, const std::string& role,
                          int occ, size_t* outOffset, size_t* outLength )
{
	// Resolve the chunk to its top-level index (for its absolute byte offset) and
	// its green node (to walk kids).  The param must live in a top-level chunk.
	NodeRef chunk;
	const int idx = DocIndexOfNodeId( doc, chunkId, &chunk );
	if( idx < 0 || !chunk ) return false;
	const size_t chunkOff = DocByteOffsetOfItem( doc, idx );
	if( chunkOff == (size_t)-1 ) return false;

	// Serialize is an in-order concatenation of a node's kids (leaf = text), so a
	// kid's byte offset within the chunk is the summed serialized width of every
	// preceding kid.  Walk the chunk's kids, counting `role` Param occurrences.
	size_t intra = 0;
	int    seen  = 0;
	for( const NodeRef& kid : chunk->kids )
	{
		if( !kid ) continue;   // defensive (kernel never populates null kids)
		std::string kidBytes;
		Serialize( kid, kidBytes );
		if( kid->kind == NodeKind::Param && kid->role == role )
		{
			if( seen == occ )
			{
				if( outOffset ) *outOffset = chunkOff + intra;
				if( outLength ) *outLength = kidBytes.size();
				return true;
			}
			++seen;
		}
		intra += kidBytes.size();
	}
	return false;   // no `occ`-th `role` param in this chunk
}

// The PUBLIC last-wins param read (see the header doc for why last, and for why it
// is NOT interchangeable with an occurrence-0 read).  A thin export of the file-
// local ParamValue/ParamNodeValue pair the derive itself reads through -- deliberately
// delegating rather than re-walking the kids, so a reader outside this TU cannot drift
// from the semantics the parse actually has.  Before the export there were three
// hand-rolled copies of this kid-walk outside this TU and they did NOT agree: two
// returned the FIRST occurrence, one the LAST.  One of the first-occurrence readers
// was the properties panel, which is how the panel came to display a value the
// renderer had never used.  (The occurrence-addressed question -- "which line does an
// occ=N edit write" -- is a DIFFERENT one, answered by ParamValueAtOccurrence below;
// it too delegates to ParamNodeValue so the two cannot disagree about the token join.)
std::string ParamValueAsParsed( const NodeRef& chunk, const std::string& role, bool* outPresent )
{
	std::string v;
	const bool found = ParamValue( chunk.get(), role, v );
	if( outPresent ) *outPresent = found;
	return found ? v : std::string();
}

// The occurrence-addressed read (see the header doc).  Counts Params by `role` from the
// FRONT, exactly as WithParamValue / WithParamRemovedOcc count them for a write, and
// delegates the token join to the SAME file-local ParamNodeValue the derive reads
// through -- so an occurrence's captured prior value is byte-identical to what the
// parse would have seen for that line, multi-token values included.  Doc-88 S4b made
// this an export: before it, THREE call sites outside this TU hand-rolled the walk
// (PainterIntrospection's read-only occurrence rows, SceneEditController's
// capture-before-agent-edit, and -- newly needed -- SceneEditor's undo/redo drift
// guard), and a capture/write pair that disagreed on the join is precisely the class
// of bug an occurrence-addressed undo cannot survive.  Round-1 P3-c correction: the
// count was actually FOUR -- Job.cpp's S2ChunkParamValue (occurrence-0 read of a
// chunk's `name`/`variant` tag for agent chunk-CRUD) was the identical walk under a
// different name and was missed at S4b export time.  It now delegates here too.
std::string ParamValueAtOccurrence( const NodeRef& chunk, const std::string& role, int occ, bool* outPresent )
{
	if( outPresent ) *outPresent = false;
	if( !chunk || occ < 0 ) return std::string();
	int seen = 0;
	for( const auto& p : chunk->kids ) {
		if( !p || p->kind != NodeKind::Param || p->role != role ) continue;
		if( seen++ != occ ) continue;
		if( outPresent ) *outPresent = true;
		return ParamNodeValue( p.get() );
	}
	return std::string();
}

int ParamOccurrenceCount( const NodeRef& chunk, const std::string& role )
{
	if( !chunk ) return 0;
	int n = 0;
	for( const NodeRef& k : chunk->kids )
		if( k && k->kind == NodeKind::Param && k->role == role ) ++n;
	return n;
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
	// EVERY param mutation funnels through here (DocSetParamValue / DocSetOrAddParamValue /
	// DocRemoveParam / DocRemoveParamOcc / DocRename all rebuild the chunk and land on this
	// function), so this ONE delta covers adding, editing, clearing and DELETING a `source`
	// line as well as a whole-item replace that flips the role.
	d.sourceInstanceCount += ( ChunkIsSourceInstance( newChunk.get() ) ? 1 : 0 )
	                       - ( ChunkIsSourceInstance( oldChunk.get() ) ? 1 : 0 );
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
	if( ChunkIsSourceInstance( newChunk.get() ) ) ++d.sourceInstanceCount;
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
	if( ChunkIsSourceInstance( oldChunk.get() ) && d.sourceInstanceCount > 0 ) --d.sourceInstanceCount;
	d.items  = SeqEraseAt( doc.items, index, v );                  // O(log N) WBT
	d.idseq  = IdEraseAt( doc.idseq, index );                      // O(log N) lockstep splice
	d.byId   = IdMapErase( d.byId, eid );                          // reverse index drops the id
	if( !np.empty() ) d.byName = NameErase( d.byName, np, eid );
	DropChunkParams( d.paramIds, d.byId, eid, oldChunk, invalidated );   // erased param ids -> invalidated
	if( invalidated ) invalidated->push_back( eid );              // ... plus the chunk's own id
	if( visits ) *visits = v;
	return d;
}

Document DocRemoveItem( const Document& doc, int index, int* visits )
{
	// Exact inverse of DocInsertItem: drop the item + its idseq label + its byId/byName
	// (and its param ids) consistently, leaving every sibling's NodeId/label intact.
	// DocEraseItem already performs exactly this symmetric remove; delegate to it (the
	// dropped param ids are this caller's don't-care, so pass invalidated=nullptr).
	return DocEraseItem( doc, index, visits, nullptr );
}

//! True iff the top-level item at `index` in `doc` serializes to bytes whose LAST
//! byte is a newline (`\n`) -- the glue-safety predicate. Serializes exactly ONE
//! item (O(item bytes)); an out-of-range/absent/empty item is NOT newline-terminated.
static bool ItemEndsInNewline( const Document& doc, int index )
{
	if( index < 0 || index >= SeqCount( doc.items ) ) return false;
	const NodeRef it = SeqItemAt( doc.items, index );
	if( !it ) return false;
	std::string s;
	Serialize( it, s );
	return !s.empty() && s.back() == '\n';
}

//! True iff the item is a PURE whitespace/newline Trivia leaf (a separator we may
//! collapse). Anything with a non-whitespace byte, or any non-Trivia node, is NOT.
static bool IsPureWhitespaceTrivia( const NodeRef& it )
{
	return it && it->kind == NodeKind::Trivia
	    && it->text.find_first_not_of( " \t\r\n" ) == std::string::npos;
}

//==============================================================
// GENERAL trivia-preserving chunk erase (Model-B P5): safely remove ANY top-level
// chunk (file-authored OR clone-inserted), keeping the Document well-formed and
// minimal. This is the SAFE, chunk-agnostic counterpart to the CLONE-UNDO-ONLY
// removal Job::ApplyCstRemoveCameraChunk performs (which drops the item at idx-1
// UNCONDITIONALLY -- correct only for the synthetic [leadSep][chunk][trailSep]
// triple a clone-insert always produces, a LANDMINE on a file-authored chunk
// whose idx-1 is the PREVIOUS chunk's real trailing newline).
//==============================================================
Document DocEraseChunkTidy( const Document& doc, int index, int* visits )
{
	if( visits ) *visits = 0;
	if( index < 0 || index >= SeqCount( doc.items ) ) return doc;   // out of range: no-op

	// (1) Remove the chunk itself.  The remaining tidy is a PURELY structural, glue-safe
	// collapse of ONE adjacent separator -- never enough to make the result ill-formed.
	int v0 = 0;
	Document d = DocRemoveItem( doc, index, &v0 );

	// (2) The item now AT `index` is the removed chunk's OLD TRAILING separator (the
	// inter-chunk trivia that followed it -- the probe showed the separator is a single
	// pure-newline `"\n\n"` Trivia leaf, distinct from the chunk, whose `}` is NOT
	// newline-terminated on its own).  Collapse that one separator so removing a chunk
	// does not accumulate a blank line -- but ONLY when it cannot cause `}<keyword>`
	// glue.  Evaluate the REAL glue condition (precision-fix style: the actual bytes at
	// the gap, not a "clone-insert always did X" assumption):
	//
	//   * `index == 0`  -- the item now at 0 is LEADING trivia (the chunk was the very
	//     first top-level item, so its trailing separator is now the document head).
	//     Dropping leading blank lines is harmless at document start: there is NO
	//     preceding chunk to glue onto.  Drop it if pure-whitespace.  (In a real
	//     RISE scene the `RISE ASCII SCENE` header strays precede every chunk, so a
	//     chunk is rarely index 0; the branch is the principled document-start case.)
	//
	//   * index > 0  -- there IS a preceding item (index-1).  Dropping the separator
	//     glues the neighbours' bytes together; that is glue-SAFE iff the preceding
	//     item's serialized bytes already end in `\n` (so the previous chunk's `}` stays
	//     line-terminated and the NEXT item begins on a fresh line).  A blank-line
	//     separator `"\n\n"` sits between two chunks each ending in `}` (NOT newline-
	//     terminated), but the item BEFORE the removed chunk is itself a separator that
	//     DOES end in `\n` (the probe: chunk, `"\n\n"`, chunk, `"\n\n"`, chunk...), so a
	//     MIDDLE removal collapses the trailing `"\n\n"` and leaves exactly the ONE
	//     leading `"\n\n"` that was already there -- neighbours keep a single separator,
	//     no glue.  If the preceding bytes do NOT end in `\n` (e.g. a hand-authored file
	//     with a chunk glued directly after a `}` on the same line, or trivia we cannot
	//     prove safe), we DO NOT drop the separator -- correctness (no glue) beats
	//     minimality (one extra blank line).
	//
	// LAST-chunk (index == old last): after the chunk drop there is NO item at `index`
	// (SeqCount dropped by 1), so the range guard below skips the collapse -- the
	// document's own final trailing newline stays, and no glue is possible.
	const int nAfter = SeqCount( d.items );
	if( index < nAfter ) {
		const NodeRef sep = SeqItemAt( d.items, index );
		if( IsPureWhitespaceTrivia( sep ) ) {
			const bool glueSafe = ( index == 0 ) || ItemEndsInNewline( d, index - 1 );
			if( glueSafe ) {
				int v1 = 0;
				d = DocRemoveItem( d, index, &v1 );
				v0 += v1;
			}
		}
	}
	if( visits ) *visits = v0;
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

// The five params whose precedence the object-chunk parsers implement (matrix > quaternion >
// per-field).  A name test, deliberately: these are the params, not a category.
static bool RoleIsObjectTransformParam_( const std::string& role )
{
	return role == "position" || role == "orientation" || role == "scale"
	    || role == "matrix"   || role == "quaternion";
}

NodeId DocTransformOwnerId( const Document& doc, NodeId chunkId, const std::string& role, bool* outIsObjectTransform )
{
	if( outIsObjectTransform ) *outIsObjectTransform = false;
	if( chunkId == 0 || !RoleIsObjectTransformParam_( role ) ) return chunkId;
	const NodeRef chunk = DocResolveNodeId( doc, chunkId );
	if( !chunk ) return chunkId;
	// Descriptor-driven, like RoleMatchesKindConstraint's registry arm: ChunkCategory::Object is
	// exactly standard_object / csg_object / override_object -- the chunks whose parsers rank these
	// five params.  A keyword suffix test would not do: `override_object` and `standard_object`
	// share one, while a future object keyword need not.
	const std::map<std::string, const IAsciiChunkParser*>& reg = DescriptorRegistry();
	std::map<std::string, const IAsciiChunkParser*>::const_iterator it = reg.find( chunk->role );
	if( it == reg.end() || it->second->Describe().category != ChunkCategory::Object ) return chunkId;
	if( outIsObjectTransform ) *outIsObjectTransform = true;
	if( chunk->role == "override_object" ) return chunkId;   // addressed directly -- do not redirect
	const std::string namePath = ChunkNamePath( chunk );
	const size_t slash = namePath.rfind( '/' );
	const std::string bareName = ( slash == std::string::npos ) ? std::string() : namePath.substr( slash + 1 );
	if( bareName.empty() ) return chunkId;                   // unnamed -- an override targets BY NAME
	const std::string overridePath = std::string( "override_object/" ) + bareName;
	// LAST wins: overrides are applied in document order, so the final one decides the pose.
	for( int index = DocItemCount( doc ) - 1; index >= 0; --index ) {
		const NodeId candidateId = DocNodeIdAt( doc, index );
		const NodeRef candidate = DocResolveNodeId( doc, candidateId );
		if( candidate && candidate->role == "override_object" && ChunkNamePath( candidate ) == overridePath )
			return candidateId;
	}
	return chunkId;
}

bool RoleMatchesKindConstraint( const std::string& role, const std::string& roleKindSuffix )
{
	if( roleKindSuffix.empty() ) return true;                      // no constraint
	const std::string under = "_" + roleKindSuffix;
	bool kindMatch = ( role == roleKindSuffix ) ||
		( role.size() > under.size() && role.compare( role.size() - under.size(), under.size(), under ) == 0 );
	// Round-6 regression fix (SourceTraceTest): the kind constraint must
	// honour the REGISTRY CLASSIFIER, not just the keyword suffix -- an
	// expression_function2d is a registry Painter (and function chunks
	// dual-register into the Painter UI union) despite not ending in
	// `_painter`.  The suffix-only narrowing introduced by the kind-
	// constraint fix silently un-addressed those chunks (reveal-in-file /
	// jump broke).  Suffix stays the fast path; the registry category is
	// the authority when the suffix disagrees.
	if( !kindMatch ) {
		// INVARIANT (round-7 review): this registry arm can over-match helper chunks whose
		// category equals an entity kind but which are NOT that entity (camera_defaults /
		// scene_options -> Camera, gltf_import -> Geometry, hosek_wilkie_skylight -> Light).
		// That is unreachable today because none of those keywords declares a `name` param,
		// so they never enter bare-name matching.  If any of them ever gains a `name`
		// parameter, add an exclusion here first.
		const std::map<std::string, const IAsciiChunkParser*>& reg = DescriptorRegistry();
		std::map<std::string, const IAsciiChunkParser*>::const_iterator it = reg.find( role );
		if( it != reg.end() ) {
			const ChunkCategory cc = it->second->Describe().category;
			if(      roleKindSuffix == "painter"  ) kindMatch = ( cc == ChunkCategory::Painter || cc == ChunkCategory::Function );
			else if( roleKindSuffix == "material" ) kindMatch = ( cc == ChunkCategory::Material );
			else if( roleKindSuffix == "geometry" ) kindMatch = ( cc == ChunkCategory::Geometry );
			else if( roleKindSuffix == "light"    ) kindMatch = ( cc == ChunkCategory::Light );
			else if( roleKindSuffix == "medium"   ) kindMatch = ( cc == ChunkCategory::Medium );
			else if( roleKindSuffix == "camera"   ) kindMatch = ( cc == ChunkCategory::Camera );
		}
	}
	return kindMatch;
}

// Camera-kind predicate for the UNNAMED-chunk scans below: keyword suffix ONLY
// (role == "camera" or ends in "_camera"), deliberately NOT the registry
// classifier -- ChunkCategory::Camera also covers non-camera helper chunks
// (camera_defaults), which must never count as an addressable camera.
static bool RoleIsCameraChunk_( const std::string& role )
{
	static const std::string under = "_camera";
	return ( role == "camera" ) ||
		( role.size() > under.size() && role.compare( role.size() - under.size(), under.size(), under ) == 0 );
}

bool DocCameraUniqueFallbackPermitted( const Document& doc, const std::string& bareName, const std::string& activeCameraName )
{
	if( bareName.empty() ) return true;   // (Camera, "") active-camera / kind-addressed-singleton convention
	if( !activeCameraName.empty() && bareName == activeCameraName ) return true;   // live registry name of the active camera
	// Exactly one camera-kind chunk TOTAL (named or unnamed): with one camera any
	// camera-kind address can only mean that camera; the fallback branch itself
	// still requires the one camera to be UNNAMED before resolving by position.
	std::vector<NodeRef> items; SeqToVec( doc.items, items );
	int cameraChunks = 0;
	for( size_t i = 0; i < items.size(); ++i ) {
		if( !items[i] || items[i]->kind != NodeKind::Chunk ) continue;
		if( RoleIsCameraChunk_( items[i]->role ) ) ++cameraChunks;
	}
	return cameraChunks == 1;
}

NodeId DocFindByNameAnyRole( const Document& doc, const std::string& bareName, int* occurrences, const std::string& roleKindSuffix, bool uniqueFallback )
{
	std::vector<NodeRef> items; SeqToVec( doc.items, items );
	std::vector<std::string> matches;                              // "keyword/name" of every bare-name hit
	for( size_t i = 0; i < items.size(); ++i ) {
		const std::string np = ChunkNamePath( items[i] );          // "keyword/name" or ""
		if( np.empty() ) continue;
		const size_t slash = np.rfind( '/' );
		if( slash != std::string::npos && np.compare( slash + 1, std::string::npos, bareName ) == 0 ) matches.push_back( np );
	}
	if( occurrences ) *occurrences = (int)matches.size();
	std::string matchPath;
	if( !matches.empty() && !roleKindSuffix.empty() ) {
		// A supplied kind is a constraint, not merely a collision hint. Keep
		// only chunks whose keyword names that kind even when the bare name has
		// exactly one match; otherwise ("material", "x") could silently target
		// a uniquely named `sphere_geometry x`.  RoleMatchesKindConstraint is
		// the ONE narrowing predicate (suffix fast path + registry-classifier
		// authority) -- shared with the defensive re-verification sites.
		int narrowed = 0;
		for( size_t m = 0; m < matches.size(); ++m ) {
			const size_t slash = matches[m].rfind( '/' );
			const std::string role = ( slash == std::string::npos ) ? matches[m] : matches[m].substr( 0, slash );
			if( RoleMatchesKindConstraint( role, roleKindSuffix ) ) { ++narrowed; matchPath = matches[m]; }
		}
		// Round-7 (P3): report the POST-narrowing count so a caller's diagnostic
		// distinguishes "no chunk of that kind" (0 -> not-found) from a genuine
		// same-kind ambiguity (>1) -- the raw bare-name count made remove_chunk
		// claim "2 chunks named x match" when neither was the requested kind.
		if( occurrences ) *occurrences = narrowed;
		if( narrowed != 1 ) return 0;                              // ambiguous or wrong kind -> refuse
	} else if( matches.size() == 1 ) {
		matchPath = matches[0];                                   // no kind constraint: unique name is enough
	} else if( matches.size() > 1 ) {
		return 0;                                                  // ambiguous with no kind constraint
	} else if( uniqueFallback && matches.empty() && !roleKindSuffix.empty() ) {
		// Unnamed-entity fallback (e.g. the sole, UNNAMED camera the editor addresses as the active camera):
		// no chunk carries this bare name, but if exactly ONE UNNAMED top-level
		// chunk is of the requested kind, use it by position. Named chunks are
		// excluded deliberately: empty-target singleton addressing must never
		// broaden into "whichever sole material/object happens to exist."
		const std::string under = "_" + roleKindSuffix;
		int kindCount = 0, kindIndex = -1;
		for( size_t i = 0; i < items.size(); ++i ) {
			if( items[i]->kind != NodeKind::Chunk ) continue;
			if( !ChunkNamePath( items[i] ).empty() ) continue;       // fallback is for truly unnamed chunks only
			const std::string& role = items[i]->role;
			const bool kindMatch = ( role == roleKindSuffix ) ||
				( role.size() > under.size() && role.compare( role.size() - under.size(), under.size(), under ) == 0 );
			if( kindMatch ) { ++kindCount; kindIndex = (int)i; }
		}
		return ( kindCount == 1 ) ? DocNodeIdAt( doc, kindIndex ) : 0;
	} else {
		return 0;                                                  // absent, or ambiguous with no kind hint -> refuse
	}
	int v = 0, c = 0; return NameFind( doc.byName, matchPath, v, c );
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

RepeatGroupView DocRepeatGroup( const Document& doc, NodeId chunkId, const std::string& role )
{
	// D3 read-through view: the repeated Param children in DOCUMENT order (the order Finalize's
	// GetRepeatable consumes), each paired with its stable per-occurrence NodeId.  A snapshot, not a
	// stored node -- document order stays canonical.
	RepeatGroupView v;
	v.chunkId = chunkId;
	v.role = role;
	NodeRef c = DocResolveNodeId( doc, chunkId );
	if( !c || c->kind != NodeKind::Chunk ) return v;
	int occ = 0;
	for( const NodeRef& kid : c->kids ) {
		if( kid->kind != NodeKind::Param || kid->role != role ) continue;
		v.occurrences.push_back( kid );
		v.occurrenceIds.push_back( DocParamId( doc, chunkId, role, occ ) );
		++occ;
	}
	return v;
}

int DocRepeatCount( const Document& doc, NodeId chunkId, const std::string& role )
{
	NodeRef c = DocResolveNodeId( doc, chunkId );
	if( !c || c->kind != NodeKind::Chunk ) return 0;
	int n = 0;
	for( const NodeRef& kid : c->kids ) if( kid->kind == NodeKind::Param && kid->role == role ) ++n;
	return n;
}

NodeId DocRepeatElementId( const Document& doc, NodeId chunkId, const std::string& role, int index, NodeRef* outParam )
{
	if( outParam ) *outParam = NodeRef();
	if( index < 0 ) return 0;
	NodeRef c = DocResolveNodeId( doc, chunkId );
	if( !c || c->kind != NodeKind::Chunk ) return 0;
	int occ = 0;
	for( const NodeRef& kid : c->kids ) {
		if( kid->kind != NodeKind::Param || kid->role != role ) continue;
		if( occ == index ) { if( outParam ) *outParam = kid; return DocParamId( doc, chunkId, role, occ ); }
		++occ;
	}
	return 0;
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
		for( int i = 0; i < O; ++i ) { oldF[ fullOf(oldItems[i]) ].push_back( i ); g_reparseOldVisits.fetch_add( 1, std::memory_order_relaxed ); }
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
		for( int i = 0; i < O; ++i ) if( !oldUsed[i] && oldItems[i]->kind == NodeKind::Chunk ) { oldR[ byKeyword ? oldItems[i]->role : keyOf(oldItems[i]) ].push_back( i ); g_reparseOldVisits.fetch_add( 1, std::memory_order_relaxed ); }
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

	Document d = fresh;   // ... incl. `sourceInstanceCount`, which ParseToCst just counted over the NEW text
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
