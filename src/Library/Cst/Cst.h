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
//  derive `sphere_geometry` into a Job through the real apply layer. The
//  persistent-sequence Document (O(log N) edit/diff), live descriptor binding,
//  the traced derivation graph, and edit/identity are subsequent transfer-gate
//  items; the green-node core here is shaped to grow into them.
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

		//! A parsed document (the CST root).
		struct Document
		{
			NodeRef root;
		};

		//! bytes -> CST. Lossless: every input byte lands in exactly one leaf,
		//! leaves in document order. Multi-chunk, brace-nested, bounds-safe.
		Document ParseToCst( const std::string& bytes );

		//! CST -> bytes. Byte-identical to the parsed input for an unedited tree
		//! (the INV-4 round-trip invariant).
		std::string SerializeCst( const Document& doc );

		//! Derive the document's `sphere_geometry` chunks into pJob through the
		//! real apply layer (IJob::AddSphereGeometry). Returns the number of
		//! geometries derived. Item-2 scope is sphere_geometry only; other chunk
		//! types are subsequent transfer-gate items.
		int DeriveToJob( const Document& doc, IJob& pJob );
	}
}

#endif
