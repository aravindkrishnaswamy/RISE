//////////////////////////////////////////////////////////////////////
//
//  ConnectionLegality.h - The connection-legality validator (doc-88
//    Phase 3 S17; docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S17;
//    docs/gui/MATERIAL_EDITOR.md sect. 6.4 "the one genuinely new
//    shared-C++ piece"; docs/GUI_ROADMAP.md:361 ParameterSemantics).
//
//    Answers ONE question: "may THIS chunk's name be bound to THAT
//    param?" -- the check the node-graph canvas (S21) runs before
//    committing a drag-drop wire, so a canvas-rejected drop reads
//    identically to what a hand-edited scene file would fail on at
//    derive time (MATERIAL_EDITOR.md:145's requirement).
//
//    SOURCE-OF-TRUTH DISCIPLINE.  This is a validator, not a second
//    parser: every verdict is derived from data the REAL pipeline
//    already carries --
//      - `ChunkDescriptor::parameters` (does the target param exist,
//        is it Reference-kind, which `referenceCategories` does it
//        declare) -- the same descriptor `DispatchChunkParameters`
//        validates a hand-authored scene line against
//        (ChunkParserRegistry.cpp);
//      - `ParameterDescriptor::semantics` (S17's new field) -- which
//        MANAGER/pipe the value actually resolves against, audited
//        per parameter from the real `Job::Add*` resolution code (see
//        ChunkParserRegistry.cpp's `p.semantics.pipe = ...`
//        assignments and their audit-comment trail);
//      - the candidate chunk's own keyword/category (from the
//        `Cst::Document`, via `SceneReferenceGraph`).
//
//    Where the real parser has ONE well-defined, shared diagnostic for
//    a failure class -- `ResolveOrDiagnoseScalar`'s three-way
//    Scalar-pipe message (Job.cpp), or `DispatchChunkParameters`'s
//    "not declared in `%s` descriptor" message
//    (ChunkParserRegistry.cpp) -- this validator consumes the SAME
//    shared `inline constexpr` symbol the real parser formats from
//    (ChunkDescriptor.h's "shared parser diagnostic strings" section:
//    `kUndeclaredParameterFmt` / `kScalarBoundToIPainterFmt` / etc.),
//    not a copy of the text. Where the real parser has NO
//    shared message (many Color-pipe slots just `return false` with no
//    `GlobalLog` call at all -- e.g. `Job::AddLambertianMaterial`),
//    this validator still produces a clear, honestly-labelled
//    diagnostic for the CANVAS's benefit rather than staying silent --
//    it is not claimed to be a verbatim parser quote in that case, and
//    the .cpp says so at each such site.
//
//    WHAT THIS DOES NOT DO.  It does not evaluate an
//    `expression`/`expression_painter` program's declared TYPE (scalar
//    vs vec3), so a `requireSingle` Scalar-pipe slot fed a `multiply`-
//    or `expression`-form `scalar_painter` whose value happens to be
//    per-channel is NOT caught here -- only the statically-visible
//    `values` form is (VALIDATION_ARCHITECTURE.md sect. 5.3: "the
//    schema/descriptor is a first-pass filter; the parser remains the
//    authority"). It does not model VALUE-level constraints that are
//    independent of candidate identity (`scalar_painter { texture ...
//    channel A }` is refused at the value layer, not because of WHICH
//    chunk is bound -- see `ParameterSemantics::note` on that
//    parameter for the full special case).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_CONNECTIONLEGALITY_
#define RISE_CONNECTIONLEGALITY_

#include "../Cst/Cst.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"
#include <string>

namespace RISE
{
	//! The result of one `CheckConnection` call.  `diagnostic` is empty
	//! iff `legal` is true.
	struct ConnectionVerdict
	{
		bool        legal = false;
		std::string diagnostic;
	};

	class ConnectionLegality
	{
	public:
		//! The literal spec shape: both chunks already exist in `doc`,
		//! addressed by their stable `Cst::NodeId` (the same identity
		//! `SceneReferenceGraph` uses everywhere). `targetChunk` is the
		//! chunk that OWNS `paramName`; `candidateChunk` is the chunk
		//! whose NAME would be written into that param's value.
		//!
		//! Illegal (with a diagnostic) when: either id fails to resolve
		//! to a chunk with a registered descriptor; `paramName` is not a
		//! declared parameter on the target's descriptor; the declared
		//! parameter is not Reference-kind (nothing to bind); the
		//! candidate's category is not in the parameter's
		//! `referenceCategories` (subject to the Function2D dual-
		//! registration special case below); or the parameter's audited
		//! `semantics.pipe` rejects the candidate's own pipe (a
		//! `scalar_painter` into a Color-pipe slot, a colour painter
		//! into a `requireSingle`-agnostic Scalar-pipe slot, etc.).
		//!
		//! SPECIAL CASES (docs/gui/NODE_GRAPH_CANVAS.md S17 brief):
		//!   - Function2D pipe: legal candidates are every
		//!     `ChunkCategory::Function` chunk PLUS every
		//!     `ChunkCategory::Painter` chunk EXCEPT `expression_painter`
		//!     and `scalar_painter` (Job.cpp's `RegisterPainterDual` dual-
		//!     registers almost every colour painter into the
		//!     IFunction2DManager; `expression_painter` is deliberately
		//!     single-registered -- see its Job.cpp comment -- and
		//!     `scalar_painter` never touches that manager at all).
		//!   - `hair_geometry`'s `guides` param: BOTH directions are typed.
		//!     Forward, a `keywordAllowlist` of {"hair_guides"} narrows what
		//!     may bind there.  Reverse, `hair_guides` carries its OWN
		//!     `ChunkCategory::HairGuides` (it is data in a Job-side table,
		//!     never an IGeometry in the geometry manager), so a guide-set
		//!     name is not a candidate for an ordinary Geometry-typed port
		//!     such as `standard_object.geometry` -- that refusal comes from
		//!     the `CategoryAllowed` fallback, not the allowlist.
		//!   - `scalar_painter`'s own `texture` param: a `keywordAllowlist`
		//!     on its `ParameterSemantics` narrows Color-pipe legality to
		//!     the five raster-image painter chunks (a `checker_painter`
		//!     is Color-pipe but not raster-backed, and is rejected).
		//!   - PBRMetallicRoughness's `metallic` / `roughness` /
		//!     `specular_factor` / `specular_color` / `anisotropy_factor`,
		//!     and GGX's `tangent_rotation`: Color-pipe by construction
		//!     though semantically a scalar/angle -- see each
		//!     `ParameterSemantics::note`.
		static ConnectionVerdict CheckConnection(
			const Cst::Document& doc,
			Cst::NodeId           targetChunk,
			const String&         paramName,
			Cst::NodeId           candidateChunk );

		//! Convenience wrapper: resolves `(targetCategory,targetName)` and
		//! `(candidateCategory,candidateName)` to NodeIds via
		//! `SceneReferenceGraph::ResolveChunk` and delegates to the
		//! NodeId overload. Illegal (never crashes) when either name
		//! fails to resolve uniquely -- see `ResolveChunk`'s own
		//! ambiguous-vs-missing contract.
		static ConnectionVerdict CheckConnectionByName(
			const Cst::Document& doc,
			ChunkCategory         targetCategory,
			const String&         targetName,
			const String&         paramName,
			ChunkCategory         candidateCategory,
			const String&         candidateName );

		//! Pure-descriptor form: no `Cst::Document` at all, for a
		//! caller that already knows both keywords (e.g. a "drag a
		//! not-yet-created palette entry onto an existing node" flow,
		//! or this slice's own corpus test, which drives every
		//! descriptor x candidate-keyword combination directly).
		//! `candidateValuesFormHint` is optional: when the candidate is
		//! itself a `scalar_painter`, passing its authored form's
		//! `values`-ness (true iff the candidate chunk uses the
		//! per-channel `values` form) lets a `requireSingle` target slot
		//! be checked statically -- omit (default false) when unknown;
		//! see the .cpp's `requireSingle` handling for what is and is
		//! not caught this way.
		static ConnectionVerdict CheckConnectionByKeyword(
			const std::string& targetKeyword,
			const std::string& paramName,
			const std::string& candidateKeyword,
			ChunkCategory       candidateCategory,
			bool                candidateIsPerChannelValues = false );

		//! Forward-reachability cycle check over `doc`'s CURRENT edges
		//! (`SceneReferenceGraph::Edges`): true iff committing a NEW
		//! edge `from -> to` (i.e. writing `to`'s name into some
		//! Reference-kind param on `from`) would create a cycle -- which
		//! holds iff `to` can already reach `from` along existing edges,
		//! or `from == to` (a direct self-reference). Iterative BFS, no
		//! recursion (the S20 wire-commit gate calls this on every
		//! attempted wire, so it must not blow the stack on a
		//! pathological chain).
		static bool WouldCycle( const Cst::Document& doc, Cst::NodeId from, Cst::NodeId to );

		//! True iff a chunk of `candidateCategory`/`candidateKeyword`
		//! may be bound wherever `ParameterPipe::Function2D` is
		//! declared -- the dual-registration rule documented above.
		//! Exposed (not just used internally) so a caller building a
		//! canvas socket-highlight pass doesn't have to re-derive it.
		static bool IsFunction2DCapable( const std::string& candidateKeyword, ChunkCategory candidateCategory );

		//! True iff a chunk of `candidateCategory`/`candidateKeyword` may
		//! be bound wherever `ParameterPipe::Color` is declared. NOT just
		//! "category == Painter && keyword != scalar_painter" --
		//! `piecewise_linear_function` (category Function) ALSO
		//! dual-registers into the colour-painter manager as a
		//! `Function1DSpectralPainter` (Job.cpp's `AddPiecewiseLinearFunction`,
		//! unconditional), so it is a legal Color-pipe candidate too --
		//! confirmed against the real parser by the corpus sweep. Exposed
		//! for the same socket-highlight reason as `IsFunction2DCapable`.
		static bool IsColorCapable( const std::string& candidateKeyword, ChunkCategory candidateCategory );
	};
}

#endif
