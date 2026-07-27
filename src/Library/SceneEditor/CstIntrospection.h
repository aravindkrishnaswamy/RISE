//////////////////////////////////////////////////////////////////////
//
//  CstIntrospection.h - GENERIC CST-descriptor-driven introspection of
//    any named chunk for the interactive editor's properties panel
//    (GUI redesign, 2026-07-22).
//
//    Generalizes the earlier painter-only introspection pattern to EVERY
//    element family whose chunks are addressable by (name, role-kind
//    suffix): resolve the entity's chunk in the retained CST Document,
//    look up the matching ChunkDescriptor (the SAME descriptor the
//    parser validates against -- single source of truth), and emit one
//    typed CameraProperty row per declared parameter with the CURRENT
//    text value read from the chunk (or the descriptor's
//    defaultValueHint when the scene omitted it).  Every row is
//    editable -- edits route through SceneEditController::
//    ApplyAgentParamEdit (the generic, undoable, re-deriving CST
//    param-edit path), so no per-family setter surface is needed.
//
//    Two enrichments over the painter original:
//      1. Reference rows (ValueKind::Reference) carry the descriptor's
//         referenceCategories (jump-to-definition metadata) AND their
//         presets are the LIVE candidate names enumerated from the
//         referenced categories' managers -- so a reference edit is a
//         pick-from-what-exists, not a blind text field.
//      2. CandidateNamesForChunkCategory is public: the controller's
//         PropertyJumpTarget uses the same enumeration to resolve a
//         reference value to the category it actually names
//         (first-wins across the declared categories, mirroring
//         Cst.cpp's ComputeChunkRefs resolution order).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_CSTINTROSPECTION_
#define RISE_CSTINTROSPECTION_

#include "../Cst/Cst.h"
#include "../Interfaces/IJobPriv.h"
#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty (panel-row struct)
#include <vector>

namespace RISE
{
	class CstIntrospection
	{
	public:
		//! Returns one CameraProperty per parameter the entity's chunk
		//! descriptor declares (the `name` param itself is surfaced as a
		//! leading read-only "type" row instead -- renaming stays a
		//! dedicated affordance).  Repeatable parameters are skipped --
		//! the single-value ApplyAgentParamEdit occurrence-0 edit path
		//! isn't a good fit for a repeated param; editing those stays
		//! scene-text-only.  Returns an empty vector when `doc` is null,
		//! `entityName` doesn't resolve to a chunk whose role matches
		//! `roleKindSuffix` (either exactly or as a "*_<suffix>"
		//! keyword), or no ChunkDescriptor is registered for its keyword.
		//!
		//! `roleKindSuffix` is the same vocabulary DocFindByNameAnyRole /
		//! RoleKindSuffixForCategory use ("painter", "geometry",
		//! "material", ...).  `typeRowDescription` is the doc string on
		//! the leading read-only type row (e.g. "Geometry chunk
		//! keyword").
		static std::vector<CameraProperty> Inspect(
			const RISE::Cst::Document* doc,
			IJobPriv& job,
			const String& entityName,
			const char* roleKindSuffix,
			const char* typeRowDescription );

		//! Live candidate names for a reference target category --
		//! enumerated from the corresponding manager(s), sorted stably.
		//! Painter is the two-manager union (colour + scalar pipes, the
		//! ISCALARPAINTER_REFACTOR distinction); Function returns the
		//! same union (colour painters dual-register as functions --
		//! Cst.cpp PASS A -- and functions have no UI category of their
		//! own).  Categories with no cheap live enumeration (ShaderOp,
		//! PhotonMap, ...) return empty -- callers fall back to the
		//! descriptor's static presets.
		static std::vector<String> CandidateNamesForChunkCategory(
			IJobPriv& job, ChunkCategory cat );

		//! MERGE the generic descriptor+CST rows into an existing
		//! live-introspection row list (the Material panel's migration
		//! path -- docs/gui/, GUI redesign 2026-07-22).  For each
		//! descriptor param of the entity's chunk:
		//!   - a same-named existing row that is READ-ONLY becomes
		//!     editable with the CST-backed value (the edit route --
		//!     SetMaterialProperty -> RouteCstParamEdit_ -- writes the
		//!     CST, so the CST text is the honest editable
		//!     representation), and inherits the descriptor's
		//!     referenceCategories / presets where it had none;
		//!   - a descriptor param with NO same-named row is APPENDED
		//!     (fully editable, generic-shaped);
		//!   - existing rows the descriptor doesn't know (live-computed
		//!     extras, repeatable-param specials like thin-film ar_layer
		//!     surfaces) are left byte-untouched.
		//! No-op when the entity has no CST chunk of the given kind (a
		//! programmatic / legacy-loaded scene) -- the live rows stand.
		static void AugmentWithCstRows(
			std::vector<CameraProperty>& rows,
			const RISE::Cst::Document* doc,
			IJobPriv& job,
			const String& entityName,
			const char* roleKindSuffix );

		//! ANNOTATE-ONLY variant: for rows whose name matches a
		//! descriptor Reference param of the entity's chunk, fill in
		//! referenceCategories (jump-to-definition metadata) -- no
		//! value, editability, preset, or row-set changes.  Used by the
		//! categories whose row building stays live-introspection-driven
		//! (Object / Light / Camera / Medium) so their existing panels
		//! gain the jump affordance without any behavior change.
		static void AnnotateReferenceRows(
			std::vector<CameraProperty>& rows,
			const RISE::Cst::Document* doc,
			const String& entityName,
			const char* roleKindSuffix );
	};
}

#endif
