//////////////////////////////////////////////////////////////////////
//
//  AgentDiagnostic.h - one structured validation diagnostic (Facet 5,
//    the agentic surface -- slice 0a).
//
//    A single, localized problem produced by AgentSession::Validate.
//    This is the slice-0 seed of the richer `ValidationReport` the
//    design (docs/agentic-redesign/50-agentic-surface.md §2.5)
//    ultimately returns: severity + a stable machine-matchable `code`
//    + a human message + a best-effort byte span (offset/length) into
//    the validated text.  Node-path anchoring, line/column, and
//    suggestion/candidates ranking are LATER refinements -- this slice
//    lands the read/validate keystone with byte-offset localization only.
//
//    `offset`/`length` are byte offsets into the text that was validated
//    (the candidate `.RISEscene` string), and are 0/0 when the diagnostic
//    cannot be localized to a span (see AgentSession::Validate for the
//    honest limits of slice-0 localization -- the underlying DeriveToJob
//    refuse-all messages are coarse, so localization is a best-effort
//    re-derivation of WHICH token is at fault).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTDIAGNOSTIC_
#define RISE_AGENT_AGENTDIAGNOSTIC_

#include <cstddef>
#include <string>

namespace RISE
{
	namespace Agent
	{
		//! One structured validation diagnostic.  See the file header for
		//! the offset/length contract (byte span into the validated text;
		//! 0/0 when not localizable).
		struct AgentDiagnostic
		{
			enum class Severity { Error, Warning, Info };

			Severity    severity = Severity::Error;
			std::string code;      //!< a stable machine-matchable code (see AgentDiagnosticCode)
			std::string message;   //!< the human-readable message (verbatim from the derive layer when coarse)
			std::size_t offset = 0;   //!< byte offset of the offending span into the validated text (0 when not localizable)
			std::size_t length = 0;   //!< byte length of the offending span (0 when not localizable)
		};

		//! Stable diagnostic codes -- the slice-0 subset of the design's
		//! §2.5 code set.  String constants (not an enum) so they match the
		//! JSON-RPC `code` field 1:1 when slice 0c adds the transport, and so
		//! a caller can compare without a lookup table.
		namespace AgentDiagnosticCode
		{
			//! The candidate text could not be parsed into a CST at all
			//! (malformed structure -- e.g. an unbalanced brace).
			static const char* const PARSE_ERROR       = "PARSE_ERROR";
			//! A chunk keyword is not a registered scene chunk type.
			static const char* const UNKNOWN_CHUNK     = "UNKNOWN_CHUNK";
			//! A parameter name is not declared on its chunk's descriptor.
			static const char* const UNKNOWN_PARAMETER = "UNKNOWN_PARAMETER";
			//! A parameter value is ill-typed for its declared kind (a
			//! non-numeric / non-finite value in a numeric slot, a value-less
			//! parameter line, etc.).
			static const char* const INVALID_VALUE     = "INVALID_VALUE";
			//! A derive-time (apply) failure not reducible to one of the
			//! above -- carried verbatim from DeriveToJob's diagnostics.
			static const char* const DERIVE_ERROR      = "DERIVE_ERROR";
			//! The text holds NO scene chunk at all -- empty, whitespace, or
			//! comments only.  DeriveToJob has nothing to complain about in
			//! such a text, so without this an empty candidate came back with
			//! an EMPTY diagnostics array: a CLEAN verdict on a non-document,
			//! which reads to a model as "the scene I just wrote is fine".
			static const char* const EMPTY_DOCUMENT    = "EMPTY_DOCUMENT";
			//! Creative-richness P2.b (docs/agentic-redesign/73-creative-
			//! richness-design.md sec 9): the scene has enough
			//! standard_object chunks (>=3) to plausibly want a
			//! spatially-varying physical-scalar parameter, but no
			//! scalar_painter chunk exists anywhere in the document, so
			//! every physical-scalar slot that DOES exist is necessarily a
			//! flat constant.  Severity::Info -- ADVISORY, not a
			//! correctness problem; the message self-disarms for a
			//! deliberately flat/simple scene.  Fire condition is computed
			//! ONCE, shared with the render-result "DESIGN NOTE" (see
			//! AgentSession.cpp's ComputeDesignNoteConditionsFromDoc_).
			static const char* const DESIGN_SCALAR_PIPE_UNUSED    = "DESIGN_SCALAR_PIPE_UNUSED";
			//! Creative-richness P2.b: the scene has enough standard_object
			//! chunks (>=4) to plausibly want an advanced geometry form,
			//! but no sdf_geometry / sweep_geometry / displaced_geometry
			//! chunk exists anywhere in the document.  Severity::Info --
			//! same advisory/self-disarming convention as
			//! DESIGN_SCALAR_PIPE_UNUSED, and the same shared fire
			//! condition.
			static const char* const DESIGN_NO_ADVANCED_GEOMETRY  = "DESIGN_NO_ADVANCED_GEOMETRY";
			//! 88 (2026-08-19), the same advisory family: five or more
			//! `standard_object` chunks are separate hand-authored copies of
			//! ONE geometry that share every non-transform binding and differ
			//! only in where they sit -- the repetition `source` + `count_u`
			//! expresses in a single chunk.  SILENT whenever the document
			//! carries a `source` / `count_u` / `count_v` anywhere (the author
			//! has already reached for the idiom, so pricing it again is
			//! noise), and geometry-less container nodes -- including the
			//! harness's own `<prefix>element_root` -- never enter a group at
			//! all.  Severity::Info, same self-disarming convention as its two
			//! siblings, and the same shared
			//! ComputeDesignNoteConditionsFromDoc_ scan.  NOTE it is a
			//! BACKSTOP: it rides render results and `validate`, both of which
			//! run AFTER the objects exist, so it prompts a correction rather
			//! than preventing the fan-out -- getting it right the first time
			//! is the skill prose's job (object-modeling-recipes,
			//! "Assemblies are subtrees; repeats are one chunk").
			static const char* const DESIGN_HAND_REPEATED_COPIES  = "DESIGN_HAND_REPEATED_COPIES";
			//! 88 S5 (2026-08-20), the same advisory family: three or more
			//! materials expose a microsurface (roughness / alphax / facets /
			//! ...) whose every slot is a bare numeric constant, and NO
			//! microsurface slot anywhere in the document is bound to anything
			//! that varies across a surface.  Distinct from
			//! DESIGN_SCALAR_PIPE_UNUSED, which asks only whether a
			//! `scalar_painter` chunk EXISTS: a scene can carry one bound to an
			//! IOR and still have every roughness flat, and only this condition
			//! sees that.  Its message NAMES `vary_material` -- the measured
			//! record (0/24 lifetime adoptions of hand-authored spatially-varying
			//! roughness, against advice delivered up to 30x/session) is that
			//! advice alone does not move this deficit, so the clause states a
			//! CALL rather than a rewrite.  Severity::Info; the clause carries its
			//! own self-disarm sentence.  Same shared
			//! ComputeDesignNoteConditionsFromDoc_ scan as its three siblings, and
			//! the SAME predicate AgentSession::VaryMaterial uses to pick what to
			//! rewrite -- so the note can never advertise a call that then edits a
			//! different material.
			static const char* const DESIGN_CONSTANT_MICROSURFACE = "DESIGN_CONSTANT_MICROSURFACE";
			//! "Adoption polish" (2026-08-21), the same advisory family:
			//! motivated by a Gemini trajectory analysis showing revision
			//! passes (v2+) repeatedly DROP min/max/step/label `param`
			//! metadata in favour of hardcoded literals in the body/defs --
			//! the capability exists (a prompt that demands params gets
			//! 51/51 compliance) but iteration pressure erodes it (a floor
			//! run kept params on only 6 of 29 final expression chunks).
			//! Fires per `expression_painter` / `scalar_painter{expression}`
			//! chunk whose body (its `def` lines plus its final `expr` /
			//! `expression` line) contains at least `kParamErosionLiteralGate`
			//! DISTINCT numeric literals while the chunk declares at most
			//! `kParamErosionMaxParams` `param` lines -- see
			//! ComputeDesignNoteConditionsFromDoc_'s condition E for the
			//! calibration evidence (the whole in-tree scene corpus has ZERO
			//! genuine chunks in that gap; every real expression-family chunk
			//! either declares real params or has too few literals to be a
			//! candidate). Severity::Info, teaches rather than scores --
			//! names the chunk and suggests promoting the literals to
			//! `param <name> <value> min .. max .. step .. label ".."`.  Same
			//! self-disarming convention as its siblings; the bounded-list
			//! formatting (up to 3 named chunks + "and N more") is shared
			//! with DESIGN_ORPHANED_PAINTERS below so neither can flood a big
			//! document.
			static const char* const DESIGN_PARAM_METADATA_EROSION = "DESIGN_PARAM_METADATA_EROSION";
			//! "Adoption polish" (2026-08-21), the same advisory family:
			//! motivated by the same trajectory analysis -- 14 superseded
			//! expression chunks left dead in one floor-run document.  The
			//! node-graph canvas already badges an unreferenced Painter/
			//! Function node for a human (NodeGraphCanvas.swift's
			//! `isOrphaned`); this gives the agent the same signal.  Fires
			//! when the document contains a Painter- or Function-category
			//! chunk with a name that NO reference anywhere in the document
			//! resolves to -- computed from `SceneReferenceGraph::EdgesAndDangling`'s
			//! FULL, document-wide edge list (every referrer category, not
			//! only Painter/Material), so a painter bound only as a
			//! rasterizer's `radiance_map` (the environment dome) or another
			//! non-Painter/Material referrer still counts as referenced --
			//! deliberately BROADER than the canvas's own `isOrphaned`, whose
			//! `PainterMaterialGraph` only seeds edges from a Painter/
			//! Material (or promoted Function) referrer.  Materials are
			//! excluded from candidacy the same way the canvas excludes them
			//! (a Material is this graph's natural root -- nothing
			//! references IT). Severity::Info, self-disarming, same
			//! bounded-list formatting as DESIGN_PARAM_METADATA_EROSION.
			//!
			//! TRANSITIVE-DEAD-CHAIN SCOPE (review-round P3-d): only the
			//! CHAIN HEAD is flagged per scan pass.  If A references B and
			//! nothing references A (A is the orphan), B still has A as a
			//! referrer and does NOT fire even though the whole A->B chain
			//! is unreachable from anything live -- the referrer-count check
			//! only sees "does at least one edge point here", not "is that
			//! edge itself reachable from something live".  This SELF-
			//! CORRECTS iteratively rather than needing a reachability walk:
			//! removing A (the flagged head, via `remove_chunk`) drops its
			//! outgoing edge to B, so the NEXT scan pass flags B as the new
			//! head.  A chain of length N surfaces one link at a time across
			//! N passes, not all at once -- acceptable for an advisory that
			//! is re-computed on every render/validate call anyway.
			static const char* const DESIGN_ORPHANED_PAINTERS = "DESIGN_ORPHANED_PAINTERS";
			//! Doc 91 (2026-08-23), the same advisory family, condition G:
			//! a Material-category chunk with a name that NO reference
			//! anywhere in the document resolves to -- DESIGN_ORPHANED_PAINTERS
			//! above is the direct precedent, both the scan (the SAME
			//! SceneReferenceGraph::EdgesAndDangling pass, just scoped to
			//! ChunkCategory::Material instead of excluded from it) and the
			//! message shape.  A Material is a graph ROOT for the
			//! Painter/Function orphan question (nothing references a
			//! material FROM a painter), which is why DESIGN_ORPHANED_PAINTERS
			//! excludes it -- but that is exactly backwards for this
			//! condition, which asks the opposite question about the same
			//! root category: does any OBJECT bind it?  Generic across any
			//! scene shape (still life, furniture, creature, landscape) --
			//! nothing here is keyed to a subject.  Severity::Info,
			//! self-disarming ("bind it or remove_chunk it"), same
			//! bounded-list formatting as its two siblings above.  SUPPRESSED
			//! while a build-protocol element is actively under construction
			//! (`AgentSession::BuildProtocolActive() && BuildPhase() ==
			//! AgentBuildPhase::Pieces`, the session's own PUBLIC accessors --
			//! threaded in by each caller as `ValidateText`'s `inPiecesPhase`
			//! argument, since ValidateText itself is a stateless static):
			//! a material authored before the
			//! object that will bind it is normal mid-build, not a mistake,
			//! and the whole point of the Pieces phase is that a scene is
			//! transiently incomplete while one element is under
			//! construction -- see AgentSession.cpp's
			//! ComputeDesignNoteConditionsFromDoc_ condition G for the exact
			//! gate.
			static const char* const DESIGN_UNBOUND_MATERIAL = "DESIGN_UNBOUND_MATERIAL";
			//! Doc 91 (2026-08-23), the same advisory family, condition H:
			//! the COLOUR-pipe twin of DESIGN_SCALAR_PIPE_UNUSED above.  The
			//! scene has enough standard_object chunks (>=3, the SAME
			//! threshold condition A uses) to plausibly want a
			//! spatially-varying colour, but every colour-carrying material
			//! slot that exists (base_color / reflectance / emission / ...,
			//! enumerated from the registered Material-category descriptors'
			//! `ParameterSemantics.pipe == ParameterPipe::Color` Reference
			//! params -- never hand-listed, so a material kind added later is
			//! covered without editing this condition) resolves to a flat
			//! uniformcolor_painter (or an equivalent constant-by-construction
			//! kind, blackbody_painter/spectral_painter).  MUST NOT fire when
			//! ANY colour-pipe material slot anywhere in the document binds to
			//! a spatially-varying painter (a perlin/expression/checker/
			//! ramp/... painter into base_color or reflectance) -- one such
			//! binding proves the author has already reached the affordance.
			//! Severity::Info, self-disarming, same
			//! ComputeDesignNoteConditionsFromDoc_ scan as its siblings.
			static const char* const DESIGN_FLAT_ALBEDO = "DESIGN_FLAT_ALBEDO";
			//! Materials-realism item 4 (2026-08-24): a BRIEFED-VS-BOUND
			//! mismatch -- an object name, its bound material's name, or a
			//! colour painter bound INTO that material (reflectance/
			//! base_color/emission/...) contains a transmissive-surface word
			//! (glass, crystal, translucent, membrane, jelly, ice, liquid,
			//! water, gel -- case-insensitive substring) while the material's
			//! KIND is opaque/reflection-only -- no transmission or
			//! scattering-class param, derived from the registered Material
			//! descriptors (OpaqueReflectionOnlyMaterialKinds_, AgentSession.cpp)
			//! rather than a hand-listed kind set.  Motivated by three
			//! trajectories that all summoned glass/liquid-briefed geometry
			//! and got an opaque pbr_metallic_roughness_material.  A
			//! HEURISTIC on NAMES -- the message says so ("named like") -- so
			//! it deliberately does NOT fire on the inverse case (a painter
			//! named e.g. `glass_green` bound through a genuinely transmissive
			//! dielectric_material/translucent_material/etc. -- that IS the
			//! correct authoring, and the material KIND, not the name, is
			//! what this condition classifies).  Two calibrated exclusions
			//! from the opaque-kind set (see OpaqueReflectionOnlyMaterialKinds_'s
			//! own doc for the corpus evidence): EMITTERS (a glass/crystal/
			//! jelly LUMINAIRE sold by emission, not transmission -- 68% of
			//! the in-tree corpus's raw name hits) and two wrapper/file-
			//! driven kinds whose transmission behaviour isn't determinable
			//! from the descriptor alone. Severity::Info, self-disarming,
			//! same bounded-list formatting and same
			//! ComputeDesignNoteConditionsFromDoc_ scan as its siblings.
			static const char* const DESIGN_MATERIAL_KIND_MISMATCH = "DESIGN_MATERIAL_KIND_MISMATCH";
			//! Crash-fix sibling (see LuminaryManager::AddToLuminaryList,
			//! src/Library/Rendering/LuminaryManager.cpp): an emissive material
			//! is bound to an object with no directly-owned geometry (e.g. a
			//! csg_object -- its shape comes from its two operand objects, not
			//! a single geometry chunk).  Such an object is silently skipped
			//! by NEE at render time (it still glows when hit directly).
			//! Severity::Warning -- a real functional gap, not a style
			//! suggestion, but the scene still renders (the render-side
			//! null-geometry candidate is refused, not dereferenced).
			//!
			//! Post-arc enforcement E1 (docs/agentic-redesign/75-expressive-
			//! surface-arc.md sec 7 / 76-...-log.md sec 3's mechanism law --
			//! blocking facts act, a Warning gets skimmed) made this TWO-TIER:
			//! the csg_object chunk descriptor gained an optional
			//! `allow_non_sampling_emitter` Bool param an author sets TRUE to
			//! acknowledge the gap is intentional (glow-only-on-direct-view).
			//!   * UNACKNOWLEDGED (the flag absent/FALSE -- every scene-file
			//!     load of a pre-existing document, and any construct this
			//!     diagnostic's Warning still applies to unchanged): this
			//!     Warning fires exactly as before.
			//!   * ACKNOWLEDGED (the flag TRUE): this Warning is SUPPRESSED
			//!     entirely for that object -- a permanent Warning on a
			//!     deliberate, disclosed choice is the nag-loop anti-pattern,
			//!     and it would fail eval's `diagnostics: clean` on a scene
			//!     that honestly acknowledged the gap.
			//! AgentSession::ValidateText's (b2) audit and
			//! AgentSession::InsertChunk / AgentSession::ProposePatch's
			//! CREATION GATE (which REFUSES an insert/patch that would CREATE an
			//! unacknowledged binding, rather than only warning about one that
			//! already landed) share ONE classification helper
			//! (CollectNullGeometryEmitters_ in AgentSession.cpp) so the
			//! Warning and the gate can never drift apart on what counts.
			static const char* const LUMINAIRE_NULL_GEOMETRY = "LUMINAIRE_NULL_GEOMETRY";
		}
	}
}

#endif
