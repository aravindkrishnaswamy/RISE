//////////////////////////////////////////////////////////////////////
//
//  OwnershipClosure.h - The ownership-closure rule + REFUSE path
//    (doc-88 Phase 3 S19; docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S19,
//    implementing docs/gui/MATERIAL_EDITOR.md sect. 3.7a).
//
//    Answers ONE question, ahead of any mutation: "may this topology
//    edit be committed by rewriting only chunks this edit OWNS -- or
//    would it re-emit a chunk that something OUTSIDE the edited
//    subgraph also depends on?"  The three-step rule of sect. 3.7a,
//    verbatim:
//
//      1. COMPUTE THE OWNERSHIP CLOSURE.  Walk the chunks reachable
//         from the edited chunk and keep exactly the set that is
//         SOLELY OWNED by this graph -- reachable from it, not shared
//         with any other material / object / light, not
//         expression-generated.
//      2. REWRITE ONLY THE CLOSURE.  Every byte outside it stays
//         byte-identical.
//      3. REFUSE WHEN THE CLOSURE IS AMBIGUOUS -- shared (>= 2
//         owners), un-addressable, or expression-driven -- with a
//         diagnostic naming (a) the shared chunk(s), (b) the
//         out-of-closure referrers, and (c) the Duplicate-node escape
//         hatch (S20), rather than guessing a block to clobber.
//
//    WHAT "OWNER" MEANS HERE, AND WHY IT IS NOT "REFERRER COUNT".
//    sect. 3.7a's acceptance criterion (MATERIAL_EDITOR.md:367) speaks of
//    "a painter with >= 2 referrers", but a literal referrer count is
//    the WRONG test in two directions and would make the rule both
//    over- and under-strict:
//
//      * OVER-strict: one material that binds the same painter into
//        TWO of its own slots (`blend_painter.colora` and `.colorb`
//        both naming `P`), or two blend painters under ONE material
//        that both name `P`, gives `P` two referrer EDGES / two
//        referrer CHUNKS -- yet `P` is still solely owned by that one
//        material's graph, and rewriting it can surprise nobody.
//      * UNDER-strict: it says nothing about a material that TWO
//        objects bind, which is the ordinary link semantics
//        (MATERIAL_EDITOR.md sect. 3.8: "one node with two out-edges ...
//        edit once, both consumers change") the canvas is SUPPOSED to
//        have -- refusing there would refuse essentially every edit.
//
//    So ownership is computed against ROOTS, exactly as sect. 3.7a
//    words it ("not shared with any other MATERIAL / OBJECT / LIGHT"):
//
//      Roots are the chunks that are not interior graph nodes -- every
//      category EXCEPT Painter and Function.  A root OWNS ITSELF.
//      An interior node's owners are the UNION of its referrers'
//      owners; an interior node with no referrer at all owns itself
//      (an orphan painter the user is mid-way through wiring up).
//
//    `Owners(c).size() >= 2` is then precisely "shared between two
//    independent graphs", and `== 1` is precisely sect. 3.7a's
//    "solely owned, single-source, unshared".  See `OwnersOf`.
//
//    WHAT THIS DOES NOT DO.  It is a POLICY layer over
//    `SceneReferenceGraph` (S11), not a second reference resolver: every
//    edge it reasons about comes from `SceneReferenceGraph::Edges`,
//    which delegates resolution entirely to `Cst::BuildReferenceGraph`.
//    It does not check whether the proposed binding is LEGAL (that is
//    `ConnectionLegality`, S17, which the caller must run FIRST -- see
//    `SceneEditController::RewireConnection`), it does not mutate
//    anything, and it does not DELETE the chunks an edit orphans: the
//    `nowUnreferenced` report exists so the canvas can BADGE them, and
//    the reference-safe delete policy that acts on that badge is S20's
//    (ENTITY_CREATION.md sect. 5).
//
//    WHY ITS OWN FILE PAIR RATHER THAN MORE OF ReferenceGraph.h.
//    `SceneReferenceGraph` is documented as "the shared reference-graph
//    QUERY surface" -- a pure, policy-free question-answerer with two
//    consumers.  Ownership closure is EDIT POLICY: which edits are
//    safe, what the refusal says, which escape hatch it names.  S17 set
//    exactly this precedent (`ConnectionLegality` is also a pure
//    function of a `Cst::Document`, also layered over the same query
//    substrate, also its own pair).  Keeping them separate also keeps
//    ReferenceGraph.h's single-scan cost model undiluted.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_OWNERSHIPCLOSURE_
#define RISE_OWNERSHIPCLOSURE_

#include "ReferenceGraph.h"
#include "../Cst/Cst.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace RISE
{
	// ---- shared REFUSAL diagnostic strings ------------------------------
	//
	// `printf`-style format strings, the SOLE definition of this module's
	// refusals.  Exported for the SAME reason ChunkDescriptor.h exports the
	// parser's own diagnostic formats (see its "shared parser diagnostic
	// strings" block, and S17's finding that two hand-kept "verbatim"
	// copies had ALREADY drifted apart): the S19 test asserts the
	// refusal wording by FORMATTING THESE SYMBOLS, so a test proving the
	// diagnostic names the shared chunk / the referrers / the escape hatch
	// cannot degrade into a test that merely proves the .cpp agrees with
	// itself.
	//
	// (c) -- the Duplicate-node escape hatch sect. 3.7a mandates -- is a
	// SEPARATE suffix appended to both refusals that have one, so the
	// hatch's wording lives in exactly one place even though two
	// classifications name it.  S20 ships the action; S19 ships the text
	// that points at it.

	//! Shared-target refusal.  %s = the shared chunk's name, %s = the
	//! comma-joined owner names, %s = the comma-joined out-of-closure
	//! referrer names.
	inline constexpr const char* const kClosureSharedFmt =
		"this topology edit would rewrite `%s`, which is shared between %s -- referenced from %s.  "
		"Rewriting it here would silently change the other graph(s) too.";

	//! The escape hatch, appended to a refusal that has one.
	inline constexpr const char* const kClosureDuplicateHatch =
		"  Duplicate the node to fork an owned copy, then repeat the edit.";

	//! Expression-driven refusal.  %s = the chunk's name, %s = its keyword.
	inline constexpr const char* const kClosureExpressionFmt =
		"this topology edit would rewrite `%s`, a `%s` chunk whose value is driven by an authored "
		"expression body (`expr` / `def`) the editor cannot rewrite as a unit -- edit the source instead.";

	//! Ambiguous-name refusal.  %s = the name, %d = the match count.
	inline constexpr const char* const kClosureAmbiguousFmt =
		"this topology edit names `%s`, which %d chunks in this category share -- the editor refuses "
		"to guess which one it addresses rather than rewrite the wrong chunk.";

	//! Unresolvable-target refusal.  %s = the name.
	inline constexpr const char* const kClosureUnresolvedFmt =
		"this topology edit names `%s`, which resolves to no chunk in this document.";

	//! What kind of topology edit a closure is being computed for.  Both
	//! kinds ask the SAME ownership question of the SAME target chunk;
	//! they differ only in what the `nowUnreferenced` report means (a
	//! Rewire that re-points the slot at `newRefChunk`, vs a Detach that
	//! drops the binding entirely).
	//!
	//! NOTE ON DETACH'S COMMIT HALF: `Detach` is computable here (so a
	//! canvas can pre-flight "would detaching this orphan anything?"),
	//! but S19 ships NO commit path for it --
	//! `SceneEditController::RewireConnection` requires a real
	//! `newRefChunk`.  The REAL blocker (corrected -- an earlier draft of
	//! this note claimed a reference slot has no valid empty literal,
	//! which is false: `none` parses and commits into most reference
	//! slots fine when written directly -- probes landed it in 5 of 6
	//! tried slots) is that `ConnectionLegality::CheckConnection` (S17,
	//! the gate `RewireConnection` runs BEFORE ownership) is addressed
	//! entirely by `Cst::NodeId` on both sides -- see its own header.
	//! The `none` sentinel is a RUNTIME default, not a Document chunk,
	//! so it has no `NodeId`, and there is therefore no way to run the
	//! candidate side of the legality check for a detach: per-slot
	//! legality of writing `none` into an arbitrary target param can't
	//! be verified without one.  A NodeId-free legality path for the
	//! `none` candidate (or an equivalent descriptor-only check) is
	//! S20's scope, alongside the reference-safe delete/unbind policy
	//! it already owns.
	enum class TopologyEditKind
	{
		Rewire = 0,
		Detach = 1
	};

	//! One topology edit, addressed the way `SceneReferenceGraph` addresses
	//! everything: by stable `Cst::NodeId`.
	struct TopologyEditIntent
	{
		TopologyEditKind kind        = TopologyEditKind::Rewire;
		Cst::NodeId      targetChunk = 0;   //!< the chunk whose reference param would be rewritten
		std::string      paramName;          //!< the reference param's role on that chunk
		int              occurrence  = 0;    //!< 0-based occurrence among same-role siblings
		Cst::NodeId      newRefChunk = 0;    //!< Rewire only; ignored (and expected 0) for Detach
	};

	//! Why a closure computation refused, machine-readable so a bridge /
	//! canvas can branch (e.g. offer Duplicate-node only for `Shared`)
	//! without parsing the diagnostic prose.
	enum class ClosureClassification
	{
		//! Solely owned: the edit rewrites `closure` and nothing else.
		Clean = 0,
		//! `targetChunk` names no chunk (or no chunk with a registered
		//! descriptor) in this document.
		UnresolvedTarget,
		//! Reserved for a NAME-addressed caller (`SceneEditController::
		//! RewireConnection`) whose (category, name) resolved to more than
		//! one chunk -- `Compute` itself is NodeId-addressed and cannot
		//! produce it.  See `RewireConnection`'s own resolution step.
		AmbiguousTargetName,
		//! The target is an expression-bodied chunk the editor cannot
		//! rewrite as a unit (sect. 3.7a's residual `let`/expression case).
		ExpressionDrivenTarget,
		//! The target is an interior graph node owned by >= 2 roots.
		SharedTarget
	};

	//! One chunk in the computed closure.
	struct ClosureChunk
	{
		Cst::NodeId   id       = 0;
		String        name;                              //!< "" for an unnamed chunk
		String        keyword;
		ChunkCategory category = ChunkCategory::Painter;
	};

	//! The full answer: the closure, the classification, and the three
	//! things sect. 3.7a requires a refusal to name.
	struct OwnershipClosureResult
	{
		ClosureClassification classification = ClosureClassification::UnresolvedTarget;

		//! True iff the edit may proceed.  A refusal MUST leave the
		//! document byte-identical -- this type carries no mutation of
		//! its own, so that is the CALLER's obligation, pinned by
		//! RewireConnectionTest's byte-identity assertions.
		bool Clean() const { return classification == ClosureClassification::Clean; }

		//! sect. 3.7a step 1/2: the solely-owned, single-source, unshared set
		//! this edit is permitted to re-emit -- the target chunk plus every
		//! chunk transitively reachable from it whose owner set is
		//! identical to the target's.  Document order.  Empty on a refusal.
		//!
		//! S19's rewire re-emits only ONE reference token on the TARGET, so
		//! it consumes `closure` only as a report; the set is computed in
		//! full because sect. 3.7a defines the fallback in terms of it and
		//! because a later slice re-emitting a whole subgraph must not
		//! re-derive the rule.
		std::vector<ClosureChunk> closure;

		//! (a) The chunk(s) this edit would rewrite that it does not solely
		//! own.  Exactly one entry (the target) for `SharedTarget`; empty
		//! otherwise.
		std::vector<String> sharedChunks;

		//! (b) The referrers that live OUTSIDE the closure -- the chunks
		//! that would be silently affected.  Populated for `SharedTarget`.
		std::vector<String> outOfClosureReferrers;

		//! The ROOT chunks that own the target (see the header's Owners
		//! definition).  Size 1 on a `Clean` verdict, >= 2 on `SharedTarget`.
		//! An unnamed root contributes its keyword in angle brackets so the
		//! diagnostic never reads "shared between  and ".
		//!
		//! NOT populated on EVERY resolved target (P3, S19 review round 1 --
		//! corrects an earlier "always populated when the target resolved"
		//! claim here): `ExpressionDrivenTarget` refuses BEFORE the owner
		//! walk runs (`Compute` checks "is this chunk rewritable as a unit
		//! at all?" ahead of "and does this edit own it?"), so a target that
		//! resolves fine but is expression-bodied leaves this EMPTY too --
		//! only `Clean` and `SharedTarget` ever populate it.
		std::vector<String> owners;

		//! Chunks whose referrer count drops to ZERO if this edit commits --
		//! for the canvas to badge as newly-orphaned.  S19 does NOT delete
		//! them (that is S20's reference-safe delete policy); a rewire
		//! removes exactly ONE reference edge, so this holds at most one
		//! entry: the old referent, and only when the edited slot was its
		//! LAST referrer.  Deliberately NOT cascaded down the old subtree:
		//! since nothing is deleted, an orphaned parent chunk still sits in
		//! the document still referencing its children, so those children
		//! are NOT unreferenced.  APPLIED GUARD: this is a prediction
		//! computed BEFORE the mutating commit -- a caller (like
		//! `SceneEditController::RewireConnection`) that reports this
		//! alongside a real commit outcome MUST clear/ignore it when that
		//! later commit did not actually apply (mid-transaction, a stale
		//! baseVersion conflict, ...), or it leaks a "would-be" orphan that
		//! never happened (S19 review round 1 P2-1).
		std::vector<String> nowUnreferenced;

		//! Human-readable refusal, formatted from the `kClosure*Fmt`
		//! symbols above.  Empty iff `Clean()`.
		std::string diagnostic;
	};

	class OwnershipClosure
	{
	public:
		//! The chunk index + reverse/forward adjacency `OwnersOf` and
		//! `Compute` both walk, hoisted into its own type (S19 review round
		//! 1 P2-2) so a BATCH caller -- badging every node's owner-count on
		//! a canvas refresh, i.e. calling `OwnersOf` once PER CHUNK in the
		//! document -- can build it ONCE and hand the SAME `Adjacency` to
		//! every call, instead of each call independently re-indexing the
		//! chunk list and rebuilding the reverse-edge map.  Without this, a
		//! per-node batch is O(nodes * (N log N + E)); with it, one O(N log
		//! N + E) build followed by O(nodes) O(1)-amortized walks -- the
		//! same "build the shared scan once" discipline `SceneReferenceGraph`
		//! documents for `AllChunks`/`EdgesAndDangling`, one layer up.
		//!
		//! Treat the fields as an implementation detail; construct it only
		//! via `BuildAdjacencyFor` and pass it straight through.  (Defined
		//! here, not forward-declared, because it is returned by value.)
		struct Adjacency
		{
			//! NodeId -> that chunk's `SceneReferenceGraph::DocumentChunk`
			//! (the same `IndexById` result `OwnersWalk` needs).
			std::map<Cst::NodeId, const SceneReferenceGraph::DocumentChunk*> byId;
			//! target -> the set of chunk ids that reference it.
			std::map<Cst::NodeId, std::set<Cst::NodeId> > rev;
			//! referrer -> the set of chunk ids it references.  `OwnersOf`
			//! alone never needs this; `Compute`'s closure walk does.
			std::map<Cst::NodeId, std::set<Cst::NodeId> > fwd;
		};

		//! Build `Adjacency` once for `doc`.  `prescanned` / `snapshot`
		//! follow the SAME contract as `Compute`'s (below): when non-null,
		//! MUST be `SceneReferenceGraph::AllChunks(doc)` /
		//! `SceneReferenceGraph::EdgesAndDangling(doc, prescanned)` for this
		//! exact `doc`; omitted, this computes them itself.  A caller doing
		//! a per-node batch calls this ONCE, then passes the result to every
		//! `OwnersOf`/`Compute` call via their own `adjacency` parameter.
		static Adjacency BuildAdjacencyFor(
			const Cst::Document& doc,
			const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned = nullptr,
			const SceneReferenceGraph::Snapshot* snapshot = nullptr );

		//! sect. 3.7a's rule, end to end.  A pure function of `doc` -- no
		//! Job, no manager, no controller lock -- so it is directly
		//! unit-testable against an in-test parsed scene string, and safe
		//! to call from a canvas hover/pre-flight path.
		//!
		//! `prescanned` / `snapshot`, when non-null, MUST be
		//! `SceneReferenceGraph::AllChunks(doc)` and
		//! `SceneReferenceGraph::EdgesAndDangling(doc, prescanned)` for this
		//! exact `doc` -- pass them when the caller already computed them
		//! (the canvas refresh does) so this does not re-walk the document.
		//! Omit both and it computes them itself, in the same ONE-scan
		//! discipline ReferenceGraph.h documents.
		//!
		//! `adjacency`, when non-null, MUST be `BuildAdjacencyFor(doc, ...)`'s
		//! result for this exact `doc` (S19 review round 1 P2-2) -- pass it
		//! for a per-node batch (see `Adjacency`'s own doc) and this skips
		//! its own index/reverse/forward build entirely; `snapshot` is then
		//! unused (`adjacency` already subsumes it).  `prescanned` is NOT
		//! subsumed -- it is still consulted for the closure's DOCUMENT-ORDER
		//! listing, which `Adjacency`'s NodeId-keyed maps do not preserve --
		//! so a batch caller that also wants Compute cheap should pass its
		//! own `prescanned` alongside `adjacency` (both are just pointers to
		//! what it already built); omitting it costs one `AllChunks` re-scan,
		//! same as today. A single one-off call (the ordinary
		//! `SceneEditController::RewireConnection` case) can omit `adjacency`
		//! entirely; the signature stays a THIN WRAPPER that builds one
		//! locally, same as today.
		static OwnershipClosureResult Compute(
			const Cst::Document& doc,
			const TopologyEditIntent& intent,
			const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned = nullptr,
			const SceneReferenceGraph::Snapshot* snapshot = nullptr,
			const Adjacency* adjacency = nullptr );

		//! The ROOT chunks `chunk` is owned by, per the header's definition
		//! (a non-Painter/Function chunk owns itself; an interior node
		//! inherits the union of its referrers' owners; a referrer-less
		//! interior node owns itself).  Sorted by NodeId, duplicate-free.
		//! Empty iff `chunk` does not resolve.
		//!
		//! Exposed (not merely used internally) because the canvas's
		//! shared-node fan-out badge -- MATERIAL_EDITOR.md sect. 3.8's
		//! "surface the share visually so an edit-once doesn't silently
		//! change a node the user thought was private" -- wants exactly
		//! this number, and must not re-derive the rule from edge counts
		//! (which, per the header, gives the wrong answer twice over).
		//!
		//! CYCLE-SAFE: a hostile document whose reference graph contains a
		//! cycle (the parser cannot produce one post-derive, but a test or
		//! a hand-edited file can hand us one) terminates via a visited
		//! set, never recurses, and reports the cycle's members as their
		//! own owners rather than hanging.
		//!
		//! `adjacency`, when non-null, MUST be `BuildAdjacencyFor(doc, ...)`'s
		//! result for this exact `doc` (S19 review round 1 P2-2) -- pass it
		//! when calling this PER NODE over the same document (a badge-every-
		//! node canvas pass) so the O(N log N + E) index/reverse build runs
		//! once for the whole batch rather than once per call; `prescanned`/
		//! `snapshot` are then ignored (the prebuilt `Adjacency` already
		//! subsumes them).  Omit it for a one-off call -- the signature stays
		//! a thin wrapper that builds its own, same as today.
		static std::vector<Cst::NodeId> OwnersOf(
			const Cst::Document& doc,
			Cst::NodeId chunk,
			const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned = nullptr,
			const SceneReferenceGraph::Snapshot* snapshot = nullptr,
			const Adjacency* adjacency = nullptr );

		//! True iff `category` is an INTERIOR graph node kind (Painter or
		//! Function) rather than a root.  The single definition the rest of
		//! this module -- and any future consumer of the same rule -- reads.
		static bool IsInteriorCategory( ChunkCategory category );
	};
}

#endif
