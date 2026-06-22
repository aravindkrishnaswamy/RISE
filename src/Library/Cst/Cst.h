//////////////////////////////////////////////////////////////////////
//
//  Cst.h - Concrete Syntax Tree kernel (agentic redesign).
//
//  The in-tree kernel for the document-canonical architecture
//  (docs/agentic-redesign): the scene document is a lossless, immutable CST;
//  text is its serialization and the rendered scene is a separate derivation.
//  This is transfer-gate item 2 (docs/agentic-redesign/IMPLEMENTATION_SLICES.md)
//  -- the foundation validated by the four `tests/Cst*SliceTest` prototypes,
//  now promoted into the real library and gated by the render-equivalence
//  harness (DumpJob(cstJob) == DumpJob(legacyJob)).
//
//  Item-2 scope: bytes <-> CST (lossless, multi-chunk, brace-nested) +
//  derive `sphere_geometry` into a Job through the real apply layer. This kernel
//  is the SEAM that subsequent transfer-gate items extend; it does NOT itself
//  have them: the child container is a std::vector, so the persistent rope and
//  its byte-width/newline aggregates (the O(log N) edit/diff) ARE the item-3
//  work; live descriptor binding is item 5; the traced derivation graph and
//  edit/identity are later. (No O(log N) and no NodeId are claimed here.)
//
//  Derive domain: the CST is the v7 runtime format -- macro-free and
//  expression-free. v6 `$( )` / DEFINE / FOR are the one-shot v6->v7 MIGRATOR's
//  job (D8), never the CST runtime's; descriptor-driven validation of params
//  (rejecting unknown/ill-typed values, which the legacy parser does) is item 5.
//  So the equivalence gate is exact for macro-free, descriptor-valid scenes.
//
//////////////////////////////////////////////////////////////////////

#ifndef CST_H
#define CST_H

#include <string>
#include <vector>
#include <memory>

namespace RISE
{
	class IJob;

	namespace Cst
	{
		//! Green-node kinds. Leaves (Token / Trivia) carry exact bytes; internal
		//! nodes (Document / Chunk / Param) carry ordered children only.
		enum class NodeKind { Document, Chunk, Param, Token, Trivia };

		//! An immutable, structurally-shared green node. Width is RELATIVE
		//! (a leaf's byte count, an internal node's child sum) -- never a stored
		//! absolute offset; positions are computed on demand.
		struct Node
		{
			NodeKind                                    kind;
			std::string                                 text;   //!< leaves only (Token / Trivia)
			std::string                                 role;   //!< Chunk: keyword; Param: param name; Token: kw / lbrace / rbrace / pname / pvalue / tok
			std::vector< std::shared_ptr<const Node> >  kids;   //!< internal nodes only
		};
		typedef std::shared_ptr<const Node> NodeRef;

		//! A persistent, structurally-shared balanced sequence of the document's
		//! TOP-LEVEL items (chunks + inter-chunk trivia) -- the D16 rope. Each
		//! node caches its subtree's aggregates: element count, serialized byte
		//! width, and newline count. Those aggregates are what make locating an
		//! edit target by byte offset (or index) O(log N) -- the find is COUNTED,
		//! never an O(N) side scan (the slice-3 gap the item-2 review flagged).
		//! An edit path-copies only the root->leaf spine (O(log N) new nodes);
		//! everything else is shared by pointer.
		struct SeqNode
		{
			std::shared_ptr<const SeqNode> left, right;
			NodeRef item;          //!< the element (a Chunk or a trivia / stray leaf)
			int     count;         //!< subtree element count
			size_t  bytes;         //!< subtree serialized byte width
			int     newlines;      //!< subtree newline count
			size_t  itemBytes;     //!< THIS item's own byte width (cached once; the
			                       //!<   immutable item's stats never change, so a
			                       //!<   path-copy edit reuses them -> rebuild is O(log N)
			int     itemNewlines;  //!<   regardless of item size, not O(log N * item))
		};
		typedef std::shared_ptr<const SeqNode> SeqRef;

		//! A parsed document: a persistent sequence of its top-level items.
		struct Document
		{
			SeqRef items;
		};

		//! bytes -> CST. Lossless: every input byte lands in exactly one leaf,
		//! leaves in document order. Multi-chunk, brace-nested, bounds-safe.
		Document ParseToCst( const std::string& bytes );

		//! CST -> bytes. Byte-identical to the parsed input for an unedited tree
		//! (the INV-4 round-trip invariant).
		std::string SerializeCst( const Document& doc );

		//! Derive the document's `sphere_geometry` chunks into pJob through the
		//! real apply layer (IJob::AddSphereGeometry). Returns the number of
		//! geometries applied.
		//!
		//! SAFE BOUNDARY (item-2 review): every sphere_geometry chunk is VALIDATED
		//! first -- unknown parameter, value-less parameter line, non-finite /
		//! non-numeric radius. If ANY chunk fails, NOTHING is applied (refuse-all)
		//! and the failures are reported in `diagnostics` (when non-null). A
		//! malformed scene is refused, never silently half-derived (no atof()->0
		//! sphere, no ignored unknown parameter). Item-5 generalizes this to the
		//! descriptor registry; here the validation is sphere-specific. Item-2
		//! scope is sphere_geometry only; other chunk types are subsequent items.
		int DeriveToJob( const Document& doc, IJob& pJob, std::vector<std::string>* diagnostics = nullptr );

		//==============================================================
		// Item 3 -- persistent Document: lookups + edit where finding the edit
		// target is COUNTED in the cost (O(log N) via the cached aggregates, not
		// an O(N) scan). These are the load-bearing operations the item-2 review
		// required not be shortcut.
		//==============================================================

		//! Number of top-level items.
		int DocItemCount( const Document& doc );

		//! Total serialized byte width of the document (== SerializeCst(doc).size()),
		//! read from the root aggregate in O(1).
		size_t DocByteWidth( const Document& doc );

		//! Locate the top-level item spanning byte `offset` (the byte->node map a
		//! UI/agent uses for "what's at this cursor position"). Returns the item's
		//! index (or -1 if out of range); `outItem`/`outStart` receive the item and
		//! its start offset, and `*visits` (if non-null) receives the number of
		//! sequence nodes descended -- O(log N), using the cached byte-width
		//! aggregates. THE find is counted here, not assumed.
		int DocItemAtByteOffset( const Document& doc, size_t offset,
		                         NodeRef* outItem, size_t* outStart, int* visits );

		//! Replace top-level item `index` with `newItem` (path-copy: O(log N) new
		//! sequence nodes, the rest shared by pointer; aggregates recomputed along
		//! the spine). `*visits` (if non-null) receives the rebuilt-node count.
		Document DocReplaceItem( const Document& doc, int index, NodeRef newItem, int* visits );

		//! Insert / erase a top-level item. Functional + aggregate-correct, but
		//! balance-via-rebuild (O(N)) -- a persistent self-rebalancing sequence
		//! (RRB / weight-balanced) is a refinement; the O(log N) claims are for the
		//! value-edit (DocReplaceItem) + lookup hot paths. (Structural edits are
		//! D24's O(N log N) regime.)
		Document DocInsertItem( const Document& doc, int index, NodeRef newItem );
		Document DocEraseItem ( const Document& doc, int index );
	}
}

#endif
