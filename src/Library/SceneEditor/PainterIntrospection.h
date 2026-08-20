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
//         OCCURRENCE, named "<param>[<i>]" -- and READ-ONLY, see the
//         editability contract below.
//
//      3. PARAM-SPEC UI METADATA.  An `expression_painter` /
//         `scalar_painter { expression ... }` `param` line may carry
//         `min` / `max` / `step` / `label` (ExpressionParamSpec.h).
//         The compiler ignores it; it exists for this panel.  The
//         painter object parsed it once at construction and keeps it
//         (ExpressionPainter::GetParamSpecs), so it is merged onto the
//         matching `param[i]` row rather than re-parsed here.
//
//  EDITABILITY CONTRACT (scoped deferral, doc 88 S4).  Every
//  SINGLE-OCCURRENCE painter parameter -- `expr`, `seed`, `time`,
//  `interpolation`, `channel`, `input`, `color`, `octaves`,
//  `persistence`, `scale`, ... -- is FULLY EDITABLE and round-trips
//  through the one CST edit pathway (SceneEditController::
//  SetPropertyForCategory(Category::Painter) ->
//  ApplyAgentParamEditInner_ -> Job::ApplyCstParamEditChecked), with
//  prior-value capture, inverse-patch undo/redo, dirty marking and a
//  re-render kick, exactly like a material-slot edit.
//
//  REPEATABLE-OCCURRENCE parameters are surfaced READ-ONLY
//  (`editable = false`).  They are NOT editable because the entire
//  agent/undo chain is fixed at occurrence 0 BY DOCUMENTED INVARIANT
//  -- SceneEditController::ApplyAgentParamEditInner_ passes `occ = 0`,
//  CaptureAgentPriorParamValue_'s AgentReadFirstParamValue reads the
//  FIRST occurrence precisely so capture and write address the same
//  line, SceneEdit carries no occurrence field, and
//  SceneEditor::ApplyRevertMutation's SetAgentCstParam arm passes 0 to
//  Job::ApplyCstParamRemoveChecked.  Making occurrence N editable means
//  threading `occ` through all five, i.e. changing the shared-undo op's
//  capture/restore pairing -- a change that earns its own review round,
//  not a side effect of this slice.  Offering occurrence 0 alone would
//  be exactly the half-support that produces a panel where the first
//  ramp stop edits and the rest silently do not.  So the rows are
//  VISIBLE (they were invisible before this module) and honestly
//  marked non-editable, with the reason in each row's description.
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
		//!   4. every REPEATABLE descriptor parameter, one READ-ONLY row
		//!      per occurrence, named "<param>[<i>]" (see the header's
		//!      editability contract), with ExpressionParamSpec
		//!      min/max/step/label folded into the description for the
		//!      expression painters' `param` lines.
		//!
		//! Returns an empty vector when `doc` is null, the name does not
		//! resolve to a painter-kind chunk, or the keyword has no
		//! registered descriptor -- same contract as the generic surface
		//! it wraps.
		static std::vector<CameraProperty> Inspect(
			const RISE::Cst::Document* doc,
			IJobPriv& job,
			const String& painterName );

		//! True iff `rowName` is one of this module's synthetic
		//! occurrence-addressed row names ("stop[1]", "param[0]", ...).
		//! The edit route consults it as a belt-and-braces refusal: such
		//! a name is not a CST param role, so routing it would ask
		//! DocSetOrAddParamValue to INSERT a line the descriptor does not
		//! declare.  (The full-derivability dry-run inside
		//! Job::ApplyCstParamEditChecked already rejects that, and both
		//! shells honour `editable = false` and never offer the edit --
		//! this is the third layer, so a future loosening of either
		//! cannot turn a read-only row into a chunk-corrupting write.)
		static bool IsOccurrenceRowName( const String& rowName );
	};
}

#endif
