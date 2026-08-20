//////////////////////////////////////////////////////////////////////
//
//  PainterIntrospection.h - The painter half of the properties-panel
//    introspection surface (doc 88 S4; MATERIAL_EDITOR.md §2.1's
//    first TO-BUILD item).
//
//    WHY THIS EXISTS ON TOP OF CstIntrospection.  A painter chunk's
//    single-occurrence parameters already round-trip through the
//    GENERIC descriptor+CST surface (CstIntrospection::Inspect with
//    roleKindSuffix "painter") -- that is deliberately not duplicated
//    here; `Inspect` below CALLS it and is the descriptor's consumer,
//    not a second source of truth.  What the generic surface cannot
//    answer, and what this module adds, is everything that needs to
//    know it is looking at a PAINTER:
//
//      1. WHICH PIPE.  A painter name can be registered in the colour
//         manager (IPainter) or the physical-scalar manager
//         (IScalarPainter).  Today NO kind registers in both: the mask
//         below is a MASK (not an enum) for future-proofing, but the
//         only chunk that ever calls IScalarPainterManager::AddItem is
//         `scalar_painter` itself (ChunkParserRegistry.cpp ~1542), and
//         it registers there ONLY.  `blend_painter` / `ramp_painter`
//         have a real dual-registration, but it is NOT this one --
//         RegisterPainterDual (Job.cpp) registers such a painter into
//         IPainterManager (colour) AND IFunction2DManager (so it can
//         also be bound as a UV-domain function elsewhere), never into
//         IScalarPainterManager.  The CST chunk keyword does not say
//         which pipe a name is in -- `scalar_painter` is the only
//         keyword that names its pipe -- so the answer comes from the
//         LIVE managers.  This is the one place a live-object hook is
//         genuinely required (CLAUDE.md's IScalarPainter refactor: the
//         two pipes are NOT interchangeable, and the panel must not
//         imply they are).
//
//      2. REPEATABLE PARAMETERS.  CstIntrospection SKIPS every
//         `repeatable` descriptor param, because the edit route it
//         hands rows to addresses occurrence 0 only.  For most element
//         families that loses nothing; for painters it loses the
//         CONTENT: a `ramp_painter`'s `stop` lines ARE the ramp, and an
//         `expression_painter`'s `param` / `def` lines ARE the
//         authored knobs.  Those rows are surfaced here, ONE ROW PER
//         OCCURRENCE, named "<param>[<index>]" -- and, since S4b,
//         individually EDITABLE; see the editability contract below.
//
//      3. PARAM-SPEC UI METADATA.  An `expression_painter` /
//         `scalar_painter { expression ... }` `param` line may carry
//         `min` / `max` / `step` / `label` (ExpressionParamSpec.h).
//         The compiler ignores it; it exists for this panel.  The
//         painter object parsed it once at construction and keeps it
//         (ExpressionPainter::GetParamSpecs), so it is merged onto the
//         matching `param[i]` row rather than re-parsed here -- since
//         S4b as FIRST-CLASS row fields (CameraProperty::hasRange /
//         rangeMin / rangeMax / rangeStep) that both shells carry, not
//         only as description prose.  That is doc 88's Tier-1 payoff:
//         the human scrubs the knobs the author (often an LLM) named.
//
//  EDITABILITY CONTRACT (doc 88 S4 + S4b).  Every SINGLE-OCCURRENCE
//  painter parameter -- `expr`, `seed`, `time`, `interpolation`,
//  `channel`, `input`, `color`, `octaves`, `persistence`, `scale`, ...
//  -- is FULLY EDITABLE and round-trips through the one CST edit
//  pathway (SceneEditController::SetPropertyForCategory(
//  Category::Painter) -> ApplyAgentParamEditInner_ ->
//  Job::ApplyCstParamEditChecked), with prior-value capture,
//  inverse-patch undo/redo, dirty marking and a re-render kick, exactly
//  like a material-slot edit.
//
//  REPEATABLE-OCCURRENCE rows are EDITABLE TOO as of S4b.  S4 shipped
//  them read-only because the whole capture/write/undo chain was pinned
//  at occurrence 0 by documented invariant; S4b threads `occ` through
//  it end to end -- SceneEditController::SetPropertyForCategory parses
//  the `<role>[<index>]` row name (Painter category ONLY) and validates
//  `0 <= i < ParamOccurrenceCount`, ApplyAgentParamEditInner_ and
//  CaptureAgentPriorParamValue_ take an `occ`, SceneEdit carries it
//  (`cstParamOcc`), and both mutation arms route it to
//  Job::ApplyCstParamEditChecked / ApplyCstParamRemoveChecked.  A write
//  to `stop[2]` edits the third `stop` line and nothing else; Undo
//  restores that same line, whole (multi-token values included).
//
//  What an occurrence row does NOT get is a blind Undo.  The layout of
//  a repeatable param can move between the edit and its Undo (an agent
//  `insert`/`remove_chunk`, another occurrence edit), so occurrence N
//  at undo time need not be the line the edit wrote.  SceneEditor's
//  drift guard (OccurrenceEditStillAddressable_) therefore re-checks
//  BOTH that an occurrence N still exists AND that it still holds the
//  value this edit last wrote, and refuses honestly rather than
//  clobbering a neighbour.  See SceneEdit::cstParamOccAddressed.
//
//  BARE repeatable role names (`stop`, `param` with no index) stay
//  REFUSED: they carry no occurrence, so honouring one would silently
//  mean "occurrence 0" -- exactly the half-support S4 refused.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_PAINTERINTROSPECTION_
#define RISE_PAINTERINTROSPECTION_

#include "../Cst/Cst.h"
#include "../Interfaces/IJobPriv.h"
#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty (panel-row struct)
#include <string>
#include <vector>

namespace RISE
{
	class PainterIntrospection
	{
	public:
		//! Which manager(s) a painter name is registered in.  Kept as a
		//! MASK (not an enum discriminator) for future-proofing, but no
		//! kind registers in BOTH today: `blend_painter` / `ramp_painter`
		//! dual-register into IPainterManager (colour) and
		//! IFunction2DManager (see RegisterPainterDual, Job.cpp), never
		//! into IScalarPainterManager -- so PipeColour|PipeScalar is
		//! currently unreachable except via an unrelated same-name
		//! collision between the two managers.
		enum PipeMask : unsigned int
		{
			PipeNone   = 0u,
			PipeColour = 1u,   //!< IPainterManager -- the colour + JH-uplift pipe
			PipeScalar = 2u    //!< IScalarPainterManager -- the physical-scalar pipe (never uplifted)
		};

		//! The pipe mask for `painterName`, read from the LIVE managers.
		//! PipeNone when the name resolves in neither (a chunk that
		//! failed to derive, or a name that is not a painter at all).
		static unsigned int PipesFor( IJobPriv& job, const String& painterName );

		//! One panel row per surfaceable parameter of the painter chunk
		//! named `painterName`, in this order:
		//!
		//!   1. the generic surface's leading read-only "type" row
		//!      (the chunk keyword),
		//!   2. a read-only "pipe" row naming the manager(s) the painter
		//!      is registered in (see PipesFor),
		//!   3. every SINGLE-OCCURRENCE descriptor parameter, editable,
		//!      value read from the retained Document (or the
		//!      descriptor's default hint when the scene omits it) --
		//!      straight from CstIntrospection::Inspect,
		//!   4. every REPEATABLE descriptor parameter, one EDITABLE row
		//!      per occurrence, named "<param>[<index>]" (see the header's
		//!      editability contract), with ExpressionParamSpec
		//!      min/max/step carried as the row's range FIELDS -- and
		//!      min/max/step/label ALSO folded into the description --
		//!      for the expression painters' `param` lines.
		//!
		//! Returns an empty vector when `doc` is null, the name does not
		//! resolve to a painter-kind chunk, or the keyword has no
		//! registered descriptor -- same contract as the generic surface
		//! it wraps.
		static std::vector<CameraProperty> Inspect(
			const RISE::Cst::Document* doc,
			IJobPriv& job,
			const String& painterName );

		//! True iff `rowName` has this module's synthetic occurrence-row
		//! SHAPE ("stop[1]", "param[0]", ...).  A real CST param role
		//! never contains a bracket, so the bracket alone discriminates.
		//! Says nothing about whether the name is well-formed or in
		//! range -- ParseOccurrenceRowName answers that.
		static bool IsOccurrenceRowName( const String& rowName );

		//! Split a synthetic occurrence-row name into its role and
		//! index: "stop[2]" -> ("stop", 2).  Returns false -- leaving
		//! the outputs untouched -- for anything that is not EXACTLY
		//! `<non-empty role>[<decimal digits>]`: no bracket, an empty or
		//! non-numeric or negative index, trailing text after `]`, a
		//! nested bracket, or an index that overflows.  The caller must
		//! still bound the index against the chunk's actual occurrence
		//! count (Cst::ParamOccurrenceCount) and confirm the role is
		//! `repeatable` on the chunk's descriptor -- this function only
		//! parses the NAME, it cannot know the document.
		//!
		//! Deliberately strict: an occurrence index that parses loosely
		//! ("stop[2junk]" -> 2) would route an edit to a line the user
		//! never named.  Refuse instead.
		static bool ParseOccurrenceRowName( const String& rowName,
		                                    std::string& outRole,
		                                    int& outOccurrence );
	};
}

#endif
