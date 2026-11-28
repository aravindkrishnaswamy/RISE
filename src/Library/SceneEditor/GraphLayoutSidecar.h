//////////////////////////////////////////////////////////////////////
//
//  GraphLayoutSidecar.h - Layout-position persistence for the Painter/
//    Material node graph (docs/gui/NODE_GRAPH_CANVAS.md §1.3).
//
//    Format: `<scenePath>.risegraph.json`, versioned JSON keyed by node
//    name (GraphNodeHandle is per-publish-generation and cannot survive
//    save/reload):
//
//        { "version": 1,
//          "nodes": { "<name>": { "x": 120.0, "y": 40.0 }, ... } }
//
//    Unknown top-level or per-node keys are ignored on read for forward
//    compatibility. Kept out of `.RISEscene` so layout stays CST-invisible.
//    Reuses `RISE::Agent::JsonValue`/`JsonParse`/`JsonSerialize`.
//    Writes atomically via sibling temp file + rename with relaxed fsync
//    (layout is disposable; a crash degrades to auto-layout on next open).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_GRAPHLAYOUTSIDECAR_
#define RISE_GRAPHLAYOUTSIDECAR_

#include "GraphLayout.h"

#include <set>
#include <string>

namespace RISE
{
	//! Pure/static except for the three filesystem entry points
	//! (ReadSidecar / WriteSidecar / MigrateSidecarOnSaveAs).
	class GraphLayoutSidecar
	{
	public:
		//! Returns `<scenePath>.risegraph.json`, or `""` when `scenePath` is empty (unsaved scene).
		static std::string SidecarPathForScene( const std::string& scenePath );

		//! Parse sidecar JSON into `out`. Returns true on a valid top-level object
		//! (missing `nodes` key is valid and yields an empty map); returns false
		//! (`out` empty, no log) on invalid JSON or non-object root.
		//! Non-finite or non-numeric `x`/`y` are replaced with 0.0; finite values
		//! exceeding `kAbsurdCoordinateBound` are clamped (sign-preserving); each
		//! sanitization increments `*outSanitizedCount` if non-null. Non-object node
		//! entries or entries missing both `x` and `y` are skipped individually.
		static bool ParsePositions( const std::string& text, GraphLayout::Positions& out,
		                            unsigned int* outSanitizedCount = nullptr );

		//! Deterministic compact JSON inverse of ParsePositions (`std::map` lexical
		//! key order) so equal maps produce byte-identical output.
		static std::string SerializePositions( const GraphLayout::Positions& positions );

		//! Read sidecar for `scenePath`. Absent -> empty map (no log); malformed ->
		//! empty map + one `eLog_Warning`; valid -> positions (+ one `eLog_Warning`
		//! if any coordinates were sanitized). Never fatal.
		static GraphLayout::Positions ReadSidecar( const std::string& scenePath );

		//! Prune entries not in `liveNodeNames` and atomically write `<scenePath>.risegraph.json`.
		//! No-ops (returns true, touches no file) when `scenePath` is empty, when
		//! serialized bytes match the on-disk file, or when the file does not exist
		//! and the pruned map is empty. Removes temp file on failure; returns false
		//! with `outError` set on I/O failure.
		static bool WriteSidecar( const std::string& scenePath, const GraphLayout::Positions& positions,
		                          const std::set<std::string>& liveNodeNames, std::string& outError );

		//! In-memory rename hook: moves `positions[oldName]` to `positions[newName]`
		//! and returns true. Returns false (leaving `positions` unchanged) if `oldName`
		//! is absent or `newName` already exists. Call once after a successful `Cst::DocRename`.
		static bool MigrateName( GraphLayout::Positions& positions,
		                         const std::string& oldName, const std::string& newName );

		//! Best-effort copy of `<oldScenePath>.risegraph.json` to `<newScenePath>.risegraph.json`
		//! after Save-As (old sidecar is always left in place). No-ops (returns true)
		//! if either path is empty, paths are equal, old sidecar is absent, or target
		//! sidecar already exists (never overwrites). Copy failure returns false with
		//! `outError` (non-fatal to the scene save).
		static bool MigrateSidecarOnSaveAs( const std::string& oldScenePath, const std::string& newScenePath,
		                                     std::string& outError );

		//! Coordinate magnitude clamp bound for corrupted/hand-edited sidecars (see ParsePositions).
		static constexpr double kAbsurdCoordinateBound = 1.0e7;
	};
}

#endif
