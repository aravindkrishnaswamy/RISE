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
			//! Fix 2 (2026-08-24), THE BLEND-SCALE LAW mechanized: db9a88a9
			//! added one sentence to sdf_geometry's `part` descriptor --
			//! "k IS A WORLD-UNIT RADIUS... a k comparable to the smallest
			//! part in the join DISSOLVES that part into its neighbour...
			//! wants k at about a third of that small part's radius or
			//! less" -- and models don't do the arithmetic.  Fires on ANY
			//! `smin` joint (union/subtract/intersect are NOT this law's
			//! concern) whose blend radius k exceeds ~1/3
			//! (kBlendScaleFraction, the SAME fraction the prose states) of
			//! the joined part's own characteristic dimension
			//! (SDFPartCharacteristicDim_, AgentSession.cpp -- radius for
			//! sphere/capsule/superellipsoid, min(half-extents) for box/
			//! roundbox, min(radius,half-height) for cylinder, tube radius
			//! for torus, min(base,tip radius) for roundcone; scaled by the
			//! part's own precomputed minScale).  Scoped to literal
			//! `sdf_geometry` chunks -- `skeleton_geometry` is excluded
			//! because it is a DIFFERENT chunk role at this scan's level
			//! (it only expands into an sdf_geometry at derive time) and its
			//! blend self-scales by the joint radius by construction
			//! (db9a88a9's own stated contrast). Severity::Info, self-
			//! disarming, same bounded-list formatting as its siblings, no
			//! volume gate (fires on the first offending joint).
			static const char* const DESIGN_SDF_BLEND_SCALE = "DESIGN_SDF_BLEND_SCALE";
			//! GPT slice item 4 (2026-08-24), THE ENV-REFLECTION ADVISORY:
			//! a low-roughness (< 0.3) metallic (>= 0.7 literal `metallic`)
			//! pbr_metallic_roughness_material, bound to at least one
			//! object, coexisting with a strongly saturated (HSV
			//! saturation >= kEnvReflectionSaturationGate, 0.5) env dome
			//! bound as the rasterizer's `radiance_map` -- the "blue-blob
			//! lamp" failure: dark painted metal mirrored a saturated dusk
			//! dome into unrecognizability.  Deliberately narrow scope
			//! (never a false positive from a case it cannot read): only a
			//! FLAT `uniformcolor_painter` dome (an image/gradient/
			//! procedural dome has no static "the colour" this scan can
			//! read without deriving the scene) and only pbr_metallic_
			//! roughness_material's `metallic`/`roughness` (the one kind
			//! with an unambiguous 0..1 conductor fraction) with BOTH
			//! bound to LITERAL numbers (a painter-bound, spatially
			//! varying value is not a fact this static scan can read).
			//! Severity::Info, self-disarming (a saturated dome CAN be the
			//! deliberate look, unlike condition J's geometric-scale
			//! fact), same bounded-list formatting as its siblings.
			static const char* const DESIGN_ENV_REFLECTION = "DESIGN_ENV_REFLECTION";
			//! docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md sec 11 / sec 13 Phase 4
			//! (2026-08-30), condition L: at least kWearCandidateGate (3)
			//! materials paint ONE flat, readable constant colour onto
			//! geometry with a real normal field -- the form is there and
			//! nothing reads it.  Its message NAMES `add_wear`, the C-VERB
			//! escalation the 2026-08-29 census earned: descriptor text
			//! reached 6/6 trajectories and the worked patina example every
			//! gpt run, for 1/6 adoption and `P.z` position proxies in its
			//! place -- the same advice-fails-a-typing-prior profile that
			//! preceded `vary_material`.  Fires off the SAME predicate
			//! AgentSession::AddWear uses to pick what to rewrite (see
			//! WearMaterial_'s four clauses), so the note can never advertise
			//! a call the verb then declines.  Severity::Info; the clause
			//! carries its own targeted anti-churn escape, so no generic
			//! self-disarm suffix is appended (condition C/D's rule).
			static const char* const DESIGN_UNWORN_MATERIALS = "DESIGN_UNWORN_MATERIALS";
			//! Condition M (2026-08-30): the "emissive-on-opaque-shell"
			//! translucency fake -- the single most-repeated agent failure on
			//! record (4 graded trajectories: asked for a glowing thin-walled
			//! lantern, the agent encloses the light in an opaque shell and
			//! paints an emissive gradient on the OUTSIDE; one run's interior
			//! candle measured at literally ZERO luminance contribution, since
			//! a solid, non-transmissive shell blocks every ray the interior
			//! light casts).
			//!
			//! Fires on >= 1 instance (unlike condition L's gate of three --
			//! one enclosed light already IS the failure) of: a POSITIONAL
			//! light (`omni_light` / `spot_light` -- the light manager's own
			//! `ILight::position()`; `rect_light` / `shape_light` -- the
			//! emissive object each derives to, at its world-box centre,
			//! PARENTED ONES INCLUDED.  `ambient_light`, `directional_light`
			//! and `hosek_wilkie_skylight` carry no world point and are never
			//! candidates) whose world position falls inside the world-space
			//! bounding box of an OBJECT bound to an OPAQUE material.
			//! "Opaque" reuses
			//! OpaqueReflectionOnlyMaterialKinds_ VERBATIM -- the exact
			//! classification condition I already owns (no transmission
			//! marker AND not a genuine emitter) -- so a `translucent_material`
			//! / dielectric-class shell never fires, and a true luminaire
			//! "flame" fixture standing in for the light itself is excluded
			//! from being flagged as the blocking shell for the same reason a
			//! luminaire is excluded from condition I's opaque set.
			//!
			//! The bbox comes from the LIVE DERIVED SCENE
			//! (`IObject::getBoundingBox()` on the object manager's realized
			//! entries), NOT from Document text.  The shipped first cut read it
			//! statically and had to disqualify any object carrying `parent`,
			//! which made it structurally inert on harness-authored scenes --
			//! `build_element` parents every piece of a multi-piece element --
			//! and it could only size six analytic geometry kinds.  Reading the
			//! derived scene composes the whole parent chain, covers EVERY
			//! geometry kind (lathe / mesh / CSG included) and gets rotations
			//! exact.  The MATERIAL classification stays document-side (the
			//! scene holds an IMaterial, not the authored keyword) and joins to
			//! the scene by name; an object whose material this scan cannot
			//! name -- a machine-minted instancing copy, a synthesized light
			//! fixture -- is never a candidate, which can only make this MISS
			//! an enclosure, never invent one.  A light's OWN derived fixture
			//! object is likewise never selected as its shell.  Objects with a
			//! non-finite or degenerate box (an infinite ground plane, a
			//! transform-only container node, a flat panel) are dropped: they
			//! have no interior.  A candidate whose bbox ALSO contains the scene's camera is
			//! skipped (a room/skybox shell, not a one-light lantern).  When
			//! more than one opaque candidate contains the light, the
			//! SMALLEST-volume one is named (the actual shell, not an
			//! enclosing room). When the winning shell's own material ALSO
			//! binds a spatially-varying `emissive` painter, a second sentence
			//! calls out that the painted glow cannot respond to the light it
			//! is impersonating.
			//!
			//! Self-disarming with NO state and NO generic suffix, condition
			//! J's reason: switching the shell to a transmissive material kind
			//! or moving the light out makes the predicate itself false --
			//! this is a physics fact, not a styling judgement, so there is no
			//! "deliberately simple" reading of a light that cannot escape its
			//! shell. Severity::Info, matching every other condition in this
			//! family (a self-disarming design ADVISORY, not a hard render
			//! refusal). HEDGED as a bounding-box containment test, not a
			//! watertight-geometry one -- an open or concave shell can
			//! register as "enclosing" too.
			//!
			//! REQUIRES A DERIVED SCENE.  A caller computing the design-note
			//! conditions WITHOUT one (the four verb call sites, which read
			//! conditions C/D/L only) gets this condition SILENT, never a
			//! crash and never a guess.  All three carriers that surface it do
			//! have one: `ValidateText` reuses the throwaway job its derive
			//! step already built, the render-result note reads the live
			//! session job, and the stateless `ComputeDesignNote(text)` wrapper
			//! derives its own throwaway -- so the note and the diagnostic can
			//! never disagree about whether this fired on the same bytes.
			static const char* const DESIGN_ENCLOSED_LIGHT_OPAQUE_SHELL = "DESIGN_ENCLOSED_LIGHT_OPAQUE_SHELL";
			//! Condition N (2026-08-30): the DIM HERO LIGHT -- a light authored
			//! BRIGHT that this scene's own `light_scene` solo audit MEASURED
			//! contributing almost nothing.
			//!
			//! MEASURED MOTIVATION (a live GUI trajectory, 2026-08-29).  The
			//! agent ran `light_scene`; the pass's own per-light solo audit
			//! reported the lantern's interior candle at 1.3 mean luma against
			//! a 67.9 all-lights frame -- about 1% of the scene's measured
			//! light total -- with an authored exitance of 5000-6000, plus a
			//! duplicate light at 0%.  The agent read straight past both
			//! numbers and its closing summary told the user the lantern was
			//! "illuminated".  The audit had MEASURED THE TRUTH; nothing
			//! carried it forward.  That is the whole of this condition: the
			//! measurement rides the note carriers an agent actually reads,
			//! because one-time result text was demonstrably ignored (the
			//! mechanism law -- facts must BLOCK or ARRIVE).
			//!
			//! FIRES on >= 1 light for which ALL THREE hold:
			//!   (i)   its most recent `light_scene` solo measurement puts it
			//!         below kDimLightShareGate (2%) of the scene's measured
			//!         light total (the SUM of every soloed light's mean luma
			//!         -- `AgentLightContribution::shareOfSoloedTotal`, not a
			//!         fraction of the all-lights frame, which light transport
			//!         does not make additive);
			//!   (ii)  its AUTHORED intensity is non-trivial -- `power` >= 1.0
			//!         for omni/spot (the descriptor's own default, so anything
			//!         at or above it is a real authored intent), `exitance` >=
			//!         10.0 for rect/shape (the bottom of the range those
			//!         descriptors document, "tens for a soft interior fill
			//!         panel") -- AND its `color` is not black.  This is what
			//!         exempts a deliberately-faint fill someone authored at
			//!         `power 0.1` while catching "exitance 5000 measuring 1%";
			//!   (iii) the cached measurement is still VALID: the light's own
			//!         document chunk text is byte-identical to what it was
			//!         when the measurement was taken.  A retuned light drops
			//!         its entry silently -- a stale measurement never speaks.
			//!
			//! SCOPE is condition M's OWN positional set (`omni_light`,
			//! `spot_light`, `rect_light`, `shape_light`) and nothing else: a
			//! `directional_light` / `ambient_light` / sky dome measuring low is
			//! ordinarily a deliberate ambient wash, and those kinds carry no
			//! notion of "enclosed" for the M-suppression below to key off.
			//!
			//! SUPPRESSED for any light condition M already names in the SAME
			//! computation.  M explains WHY that light is dark (an opaque shell
			//! blocks it); N without M says only "it is dark, find out why".
			//! Noting one light twice in one note is noise, so M wins.
			//!
			//! HONEST LIMITATION, stated in the clause itself: the validity key
			//! is the LIGHT'S OWN chunk text.  Edits ELSEWHERE -- moving the
			//! shell that blocks it, retuning the other lights, changing the
			//! camera -- change what the light really contributes WITHOUT
			//! invalidating the cache, so the clause says the figure is from
			//! the last `light_scene` run and asks for a re-run to re-measure.
			//!
			//! REQUIRES SESSION STATE (the cache `light_scene` writes), which is
			//! what makes it the first condition in this family that the
			//! STATELESS text-only carriers cannot compute: `ValidateText(text)`
			//! and `ComputeDesignNote(text)` called with no cache are simply
			//! SILENT on it, exactly as condition M is silent without a derived
			//! scene.  The carriers that DO have a session pass their cache
			//! through (`AgentSession::Validate`, and the render-result note in
			//! `RenderCore_`), and both render the SAME shared clause.
			//! Severity::Info, this family's convention.
			static const char* const DESIGN_DIM_HERO_LIGHT = "DESIGN_DIM_HERO_LIGHT";
			//! Condition O (2026-08-31): the RE-MEASURE NUDGE that closes the
			//! handoff void between conditions M and N -- N's own
			//! cache-invalidation event (the light's chunk text changes) is
			//! EXACTLY what happens when an agent acts on condition M's advice
			//! and "fixes" an enclosed light by editing the light itself.
			//! Editing erases N's own evidence: the stale entry stops matching
			//! and N goes silent, without the fix's effect ever having been
			//! re-measured.
			//!
			//! MEASURED MOTIVATION (altar p4 forensic, 2026-08-31): condition M
			//! fired live on an enclosed dim light in both runs.  Both agents
			//! fixed the enclosure by EDITING THE LIGHT (moving it, enlarging
			//! it, retuning it) -- which is the light's OWN chunk changing,
			//! i.e. N's invalidation case -- and then never re-ran
			//! `light_scene`.  The fix's effectiveness was unverifiable and
			//! both closing summaries claimed an unmeasured "glow".  Acting on
			//! M's advice erases N's evidence; O exists to close that handoff
			//! void rather than leave it a dead end.
			//!
			//! FIRES on >= 1 light for which ALL THREE hold:
			//!   (1) The CACHED measurement was itself a QUALIFIED DIM FINDING
			//!       when it was recorded -- condition N's own qualification
			//!       (share < kDimLightShareGate, and authored-non-trivial
			//!       with a non-black colour), evaluated against the RECORDED
			//!       SNAPSHOT rather than the live document
			//!       (IsQualifiedDimFinding_ in AgentSession.cpp, built on the
			//!       SAME primitive N's own (ii) check calls -- no duplicated
			//!       thresholds).  A cache entry that was never a real N
			//!       finding to begin with (a healthy share, or a light
			//!       authored faint on purpose) has nothing to nudge a
			//!       re-measure over.
			//!   (2) The light's CURRENT chunk exists, with the SAME authored
			//!       kind, but its text is DIFFERENT from the recorded
			//!       `chunkText` -- the edit happened.  This is N's OWN
			//!       invalidation test read the other way: N requires
			//!       byte-identical text to speak from the cache; O requires
			//!       it to differ.  A DELETED light, or one whose name was
			//!       re-authored as a different light kind, is silent -- there
			//!       is no live chunk to nudge a re-measure on.
			//!   (3) Condition M does NOT currently name this light.  If M
			//!       still fires, the enclosure is not fixed yet and nudging a
			//!       re-measure is premature -- the same "speak together or
			//!       not at all" rule M/N already share, including the
			//!       bootstrap case: when M cannot be computed at all (no
			//!       derived scene, so M is structurally silent), O stays
			//!       silent too, exactly as N does.
			//!
			//! MUTUALLY EXCLUSIVE WITH N, BY CONSTRUCTION: N requires the
			//! cached `chunkText` to be BYTE-IDENTICAL to the light's current
			//! chunk; O requires it to be DIFFERENT.  The same light can never
			//! satisfy both predicates in the same computation -- one reads
			//! "still valid and still dim", the other reads "no longer valid
			//! because it was just edited".
			//!
			//! THE SELF-DISARMING LOOP this condition closes: M fires (light
			//! enclosed) -> an agent edits the light itself (the attempted
			//! fix) -> the edit invalidates N's cache entry, so N goes silent
			//! -> O picks up exactly that silence and nudges a re-run -> when
			//! `light_scene` actually re-runs it REPLACES the cache entry with
			//! a fresh one keyed to the NEW chunk text -> if the fix worked,
			//! the fresh measurement is healthy and BOTH N and O fall silent;
			//! if it did not, N fires again on the fresh (now validly-keyed)
			//! low share and O is silent (there is no longer anything stale to
			//! nudge).  Either branch, re-running `light_scene` on THIS light
			//! is what ENDS this condition -- as of the 2026-08-31 retention
			//! fix in `RecordLightSoloMeasurements` (AgentSession.cpp), an
			//! audit that solos other lights and never touches this one can no
			//! longer end it by accident: the stale-by-edit entry now survives
			//! any audit that does not re-measure this specific light, keyed
			//! on the chunk merely EXISTING with the same kind rather than on
			//! byte-identical text (byte identity is still checked, just at
			//! read time by N and O themselves, not at retention time).  The
			//! one other way out is deleting the light (or re-authoring its
			//! name as a different light kind): the entry then has no live
			//! chunk to key against and is dropped, which is not "measuring
			//! again" but is the same "nothing left to nudge" case (2) above
			//! already treats as silent -- an author who removes the light
			//! has, in the sense this condition cares about, also resolved
			//! it.
			//!
			//! REQUIRES SESSION STATE, exactly like N: the stateless text-only
			//! carriers (`ValidateText`, `ComputeDesignNote` called with no
			//! cache) are silent on it, for the identical reason -- a
			//! condition whose input the caller does not have is absent, never
			//! guessed.  Severity::Info, this family's convention; both
			//! carriers render the SAME shared clause
			//! (FormatDimLightRemeasureClause_ in AgentSession.cpp), so they
			//! can never disagree about what fired.
			static const char* const DESIGN_DIM_LIGHT_REMEASURE = "DESIGN_DIM_LIGHT_REMEASURE";
			//! docs/WETNESS_COAT_DESIGN.md sec 6.4/13 (2026-08-31), condition P:
			//! the DRY RAIN SCENE -- the scene's OWN language (its name, an
			//! object/element name, or a comment anywhere in the document text)
			//! uses rain/wet/storm/damp/puddle vocabulary, while every material
			//! this scan can rewrite for wetness (`WetnessMaterial_`'s
			//! qualifying scan, shared one-predicate-two-consumers with
			//! `AgentSession::AddWetness`, exactly as condition L shares
			//! `WearMaterial_` with `AddWear`) still reads bone-dry.
			//!
			//! FIRES on >= 1 qualifying-but-unwet material once the rain
			//! vocabulary test passes -- no volume gate, matching conditions
			//! M/N's "one already is the failure" convention: a scene whose own
			//! name or a comment says "rainstorm" and whose stone floor is a
			//! flat, dry `lambertian_material` is already the described failure,
			//! not a matter of degree.
			//!
			//! THE VOCABULARY TEST IS A HEURISTIC, STATED AS SUCH: a
			//! case-insensitive, whole-word scan of the SERIALIZED document text
			//! (chunk `name`s and any `#`-comment trivia the CST preserves) for
			//! { rain, raining, rainy, rainstorm, downpour, drizzle, monsoon,
			//! storm, wet, damp, puddle }.  This can both under- and over-fire
			//! (a `name` like `raincoat_display_case` is not a rain SCENE; a
			//! scene actually about rain that never spells the word anywhere is
			//! invisible to it) -- acceptable for an Info-severity, self-
			//! disarming advisory whose only claim is "consider `add_wetness`",
			//! never a hard refusal.
			//!
			//! The note NAMES `add_wetness`, the verb this condition is paired
			//! with, so the advisory and the escalation are the same act (the
			//! `add_wear`/condition L precedent).  Severity::Info; the clause
			//! carries the same generic self-disarm suffix every sibling in this
			//! family appends ("if a deliberately dry look was wanted, ignore").
			static const char* const DESIGN_DRY_RAIN_SCENE = "DESIGN_DRY_RAIN_SCENE";
			//! docs/RELIEF_MODIFIER_DESIGN.md sec 9 (2026-09-06), condition Q:
			//! the DECAL-ON-PLASTIC detector -- an object whose material paints
			//! a spatially-varying colour but whose shading normal never
			//! responds to it.  Doc 88 sec 2/7's adoption laws (C-ADV/C-VERB)
			//! are why this ships advisory-only: the record on this exact shape
			//! (`vary_material`, `add_wear`) is that advice alone moves
			//! adoption approximately zero, so this note is not expected to
			//! either -- it exists to make the census (docs/
			//! RELIEF_MODIFIER_DESIGN.md sec 9's "Census" bullet) measurable,
			//! and its text is the one place the relief idiom is taught at the
			//! moment the agent is looking at the material.  Per C-VERB, if
			//! the census shows advice + recipe do not move adoption, a
			//! `relief_amplitude` argument on `add_wear` is the named
			//! escalation (see AgentSession::AddWear's own hook-point note,
			//! AgentSession.cpp) -- it is not built until then.
			//!
			//! FIRES on >= 1 (no volume gate -- condition M/N/O/I's
			//! convention, not condition D/H/L/P's "systemic flatness" one:
			//! ONE decal-on-plastic object already IS the described failure)
			//! `standard_object` or `csg_object` -- EXCLUDING a pure CONTAINER
			//! (a `standard_object` with neither `geometry` nor `source`: a
			//! transform node, invisible to the renderer, with no surface for
			//! a modifier to act on regardless of what its otherwise-inert
			//! material binds; `csg_object` has no `geometry`/`source` field
			//! at all -- its shape is always its two operands -- so this
			//! exclusion never applies to it) -- for which ALL of:
			//!   (i)   its EFFECTIVE material names at least one colour-pipe
			//!         slot (`ColorMaterialSlotsByKind_`, condition H's own
			//!         registry-derived table -- never a hand list) that
			//!         resolves to a SPATIALLY-VARYING painter kind
			//!         (`ClassifyColorBinding_`, the SAME Constant/Varying/
			//!         Opaque classifier condition H and `add_wear`/
			//!         `add_wetness` already share). An Opaque (unreadable)
			//!         slot does NOT qualify -- that proves only that the slot
			//!         is not a plain flat constant, never that it is
			//!         genuinely textured.
			//!
			//!         FIX ROUND 1 (2026-09-06) narrowed "varying" three ways,
			//!         each because the first cut advised relief where it did
			//!         not belong:
			//!          * a PASS-THROUGH painter (`blend_painter`,
			//!            `ramp_painter`, `mapping_painter`,
			//!            `channel_painter`) is only as varying as its inputs
			//!            -- `ClassifyColorBinding_` now recurses, exactly as
			//!            the scalar twin `ClassifyMicrosurfaceBinding_`
			//!            already walked its base/multiply chains, so an
			//!            all-uniform blend is CONSTANT.  Pattern painters
			//!            (checker / perlin / voronoi / ...) stay Varying by
			//!            construction: two uniform inputs still give a
			//!            chequerboard;
			//!          * an EMISSION slot (`emissive` / `exitance` /
			//!            `emission`, per `ColorSlotIsEmissionRole_`) is never
			//!            a candidate -- relief cannot sell a GLOW.  A varying
			//!            `emissive` over flat rd/rs on `ggx_material` or
			//!            `pbr_metallic_roughness_material` is a complete look
			//!            on its own.  SLOT-scoped, not material-scoped: the
			//!            same material with a varying `rd` still fires;
			//!          * a slot `add_wetness` rebound to its own WETNESS-
			//!            PRELUDE expression (condition H's own
			//!            `WetnessBodyReadsPreludeDefs_` marker: `dryness` AND
			//!            `film_amount`) is a wet film, not authored texture.
			//!            Advising relief there is physically backwards -- a
			//!            film conforms to the relief already present rather
			//!            than adding new micro-geometry (see
			//!            `AgentSession::AddWetness`'s own hook-point note).
			//!         Conversely the search is RECURSIVE THROUGH WRAPPER
			//!         MATERIALS: `coated_material` / `fabric_material` /
			//!         `composite_material` have no varying colour slot of
			//!         their own while the surface under them is fully
			//!         textured, so every `ParameterPipe::Material` slot
			//!         (`MaterialWrapperSlotsByKind_`, registry-derived) is
			//!         followed into the wrapped base, with the (iv)
			//!         material-kind exclusions applied at every level.
			//!
			//!         "EFFECTIVE material", not "the object chunk's own
			//!         `material` literal": FIX ROUND 2 (2026-09-06) taught
			//!         this clause the SAME two engine rules fix round 1
			//!         taught clause (ii), because both rules govern the PAIR
			//!         `pMaterial`/`pModifier` rather than the modifier alone
			//!         -- `AdoptCsgSurfaceBindings` copies both,
			//!         `IsInstanceOwnParam` excludes both, and a composite
			//!         applies its own through the matched pair
			//!         `if( pMaterial ) ri.pMaterial = pMaterial;  if(
			//!         pModifier ) ri.pModifier = pModifier;` at the bottom of
			//!         `CSGObject::IntersectRay`.  So:
			//!          * the material is resolved through the `source` chain
			//!            (own `material` wins, else the inherited one; own
			//!            `material none` clears it), so a bare `copy { source
			//!            orig }` inheriting a varying material with no
			//!            modifier IS a candidate.  The literal-param lookup
			//!            this replaced dropped every such copy -- an
			//!            UNDER-report, and one that also left the clause's
			//!            "and N more objects" tally short;
			//!          * an object under an ENCLOSING composite that spells a
			//!            `material` is NOT a candidate on its own material:
			//!            the composite's binding replaces it on every hit, so
			//!            the operand advertises a texture that is never
			//!            shaded.  The COMPOSITE is the candidate, on the
			//!            material IT spells, which the ordinary per-object
			//!            walk already reaches.  Nested: any enclosing
			//!            composite spelling one silences the operand, because
			//!            the outermost spelled material is what survives the
			//!            inside-out adoption.  Unlike `modifier none` on a
			//!            `source` copy, `material none` on a COMPOSITE
			//!            overrides nothing -- the parser passes 0, so the
			//!            guarded assignment never fires;
			//!   (ii)  the rendered surface carries NO EFFECTIVE MODIFIER.
			//!         Not "the object chunk spells no `modifier`" -- FIX
			//!         ROUND 1 (2026-09-06) replaced that literal-param test
			//!         with `ObjectHasEffectiveModifier_`, which resolves the
			//!         question the ENGINE answers.  A modifier is effective
			//!         by any of four routes:
			//!          (a) the object's OWN `modifier` (absent, empty or
			//!              "none" means unbound);
			//!          (b) one INHERITED down a `source` chain.  Cst.cpp's
			//!              `MergeChunkParams` folds the whole chain into the
			//!              derived instance and `modifier` is NOT in
			//!              `IsInstanceOwnParam`, so an instancing copy
			//!              renders WITH its source's modifier.  The copy's
			//!              own params merge LAST, so a copy spelling
			//!              `modifier none` genuinely CLEARS the inherited one
			//!              and IS a candidate -- "none" stops the walk, it is
			//!              not "absent";
			//!          (c) EVERY OPERAND of a `csg_object` carrying one
			//!              (recursively, through nested csg).
			//!              `CSGObject::IntersectRay` reports the OPERAND's
			//!              modifier on each hit (`AdoptCsgSurfaceBindings`),
			//!              so a composite that binds nothing itself still has
			//!              a fully relief-bearing surface when both operands
			//!              do.  A composite with no modifier and only SOME
			//!              operands bearing relief FIRES, naming the
			//!              composite -- that partly-flat surface is exactly
			//!              the described failure.  A composite whose operands
			//!              cannot be resolved is NOT bound: this scan never
			//!              claims relief it cannot see;
			//!          (d) an ENCLOSING composite (transitively) binding one
			//!              over this object.  The composite's own binding
			//!              takes final precedence over the operand's on every
			//!              hit it reports, so the operand is never flat.
			//!         Every walk here is bounded, but not by the same rule
			//!         (FIX ROUND 2): the `source` walks stop at
			//!         `kSourceChainHopBound_ = 256`, the SAME guard value
			//!         `Cst.cpp`'s `SourceChainOf` uses.  The walk actually
			//!         REACHES 255 hops (hop 0 resolves the object's own
			//!         literal; hops 1..255 walk its `source` ancestors), and
			//!         that is itself a belt that never binds: the engine's
			//!         real instancing-expansion cap is
			//!         `ClonePlanBuilder::DepthOk` (`Cst.cpp`), which refuses
			//!         to EXPAND a `source` chain past 64 levels, well inside
			//!         this scan's 255-hop reach -- so no chain the ENGINE
			//!         will expand can outrun the scan.  The first cut's
			//!         bound of 8 reached only 7 hops and justified itself by
			//!         a `source` cycle the declare-earlier rule makes
			//!         impossible, which made a LEGAL 8-deep chain with the
			//!         modifier at its root fire falsely WHEN the far link
			//!         also re-spelled its own material -- without that, the
			//!         object has no material at all, and clause (i) drops
			//!         it rather than firing.  The csg walks are
			//!         bounded on NESTING DEPTH instead, deliberately modest
			//!         because the enclosure edges can fan out; a nest deeper
			//!         than that resolves to "no modifier" / "no enclosing
			//!         material", which costs at most one advisory.
			//!         Which modifier KIND is bound is never asked: a
			//!         `modifier_stack` name counts exactly as a single
			//!         modifier chunk does, and a bumpmap/normal-map/glint
			//!         modifier silences this exactly as a relief one would,
			//!         since the claim is narrowly "the shading normal is
			//!         inert", not "the wrong modifier kind was chosen".
			//!
			//!         WHY THE ENCLOSING-COMPOSITE RULES ARE SAFE -- in (ii)
			//!         above and in (i)'s material twin alike: A CSG OPERAND
			//!         NEVER RENDERS STANDALONE, so silencing one can never
			//!         silence a surface that reaches the frame on its own.
			//!         `CSGObject::AssignObjects` marks both operands consumed
			//!         (`Object::AddConsumer`; `IsWorldVisible()` is
			//!         `bIsWorldVisible && nConsumedBy == 0`), which takes
			//!         them out of every world-visible enumeration, and
			//!         `Job::AddCSGObject` refuses an operand that is
			//!         `parent`ed elsewhere ("Parent the csg_object instead").
			//!         The only surface an operand contributes to is its
			//!         composite's;
			//!   (iii) the object's geometry is not `hair_geometry` (a
			//!         strand's own tangent-frame shading has no purchase for
			//!         this kind of relief); and
			//!   (iv)  the bound material's kind is not `hair_material`, is
			//!         not a luminaire (`DescriptorIsEmissiveMaterial_`, the
			//!         registry "carries `exitance`" rule conditions I/M
			//!         already share -- a painted glow is sold by emission,
			//!         and micro-relief on a light source is not this note's
			//!         business), and the object itself does not classify as
			//!         a light-object (`ChunkIsLightObject_`, the arc-80
			//!         rect_light/shape_light fixture classifier -- redundant
			//!         with the luminaire check in every case audited, kept
			//!         for the same reason `TargetIsFormBearing_` keeps it: a
			//!         synthesized light fixture is never this note's
			//!         business even if some future luminaire kind's
			//!         descriptor stops carrying `exitance`).
			//!
			//! The message NAMES the object, its material, the varying slot,
			//! and the painter bound there, and states the two-chunk fix
			//! (`scalar_painter { painter <field> channel R }` bridging the
			//! SAME field into a `relief_modifier`, attached via `modifier`) --
			//! read_skill {"name":"procedural-textures"}'s relief section is
			//! where the worked recipe lives.  NO HERO-MATERIAL PICK: unlike
			//! conditions D/H/L/P (which choose ONE material a bare verb call
			//! would rewrite), Phase 4 ships no verb, so there is nothing a
			//! hero pick would target -- every qualifying OBJECT is a finding,
			//! the first named in full and the rest counted, the same bounded
			//! multi-finding convention conditions M/N/O already use for a
			//! per-instance (rather than per-material) finding.
			//! Severity::Info, self-disarming (condition H's own kSelfDisarm
			//! suffix -- the claim IS "flat/simple styling", condition A's
			//! topic), same shared ComputeDesignNoteConditionsFromDoc_ scan as
			//! every sibling in this family.
			static const char* const DESIGN_FLAT_RELIEF = "DESIGN_FLAT_RELIEF";
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
			//! Advisory sibling of LUMINAIRE_NULL_GEOMETRY -- same one-flag
			//! acknowledgment idiom on the same chunk kind, but Warning-tier
			//! ONLY (see the "NO CREATION GATE" note below for why the second
			//! tier does not apply here).  Mirrors `Job::AddCSGObject`'s
			//! parse-time advisory (src/Library/Job.cpp) and
			//! docs/SCENE_CONVENTIONS.md sec 5.5: a `csg_object`'s own
			//! `position`/`orientation` is composed on top of each operand's
			//! transform, and an operand's transform is interpreted in that
			//! `csg_object`'s LOCAL frame, not the world's -- so a csg_object
			//! that is ITSELF transformed RE-BASES any operand that is already
			//! transformed.  The composite lands at (csg_object's transform)
			//! composed with (operand's transform), not at the operand's
			//! authored world coordinates; a large enough re-base (or one that
			//! rotates the composite out of the camera's frustum) can render
			//! shifted, empty, or entirely all-black with no visual clue why.
			//!
			//! The acknowledgment: `allow_transformed_operands TRUE` on the
			//! csg_object chunk suppresses this diagnostic (and, at runtime,
			//! `Job::AddCSGObject`'s matching log advisory).
			//!   * UNACKNOWLEDGED (the flag absent/FALSE): this Warning fires
			//!     whenever the csg_object's own transform is non-identity AND
			//!     at least one operand is already transformed.
			//!   * ACKNOWLEDGED (the flag TRUE): this Warning is SUPPRESSED
			//!     entirely for that csg_object -- a permanent Warning on a
			//!     deliberate, disclosed choice is the nag-loop anti-pattern.
			//! AgentSession::ValidateText's (b2) audit is the SOLE consumer;
			//! the classification helper lives in AgentSession.cpp
			//! (CollectRebasedOperandCsgs_) alongside, but NOT sharing, the
			//! LUMINAIRE_NULL_GEOMETRY predicate (CollectNullGeometryEmitters_)
			//! -- the two diagnostics flag different chunk defects and key off
			//! different tests (emitter-with-no-geometry vs
			//! dual-transformed-halves), so a shared helper would just be an
			//! `if` fork wearing one name.
			//!
			//! NO CREATION GATE, deliberately -- unlike LUMINAIRE_NULL_GEOMETRY,
			//! which pairs its Warning with a refusal in
			//! AgentSession::InsertChunk / AgentSession::ProposePatch that
			//! blocks CREATING an unacknowledged null-geometry emitter.  That
			//! gate's own governing comment (AgentSession.cpp, the R1c
			//! rasterizer-allowlist block) states the stop rule any new escape
			//! flag is held to: one is only worth adding when the construct it
			//! acknowledges "has a rare but LEGITIMATE authoring intent... the
			//! agent can honestly disclose" -- otherwise "an escape param would
			//! be exactly the habituation surface... a flag the model learns
			//! to set reflexively, turning a refusal into a two-call
			//! formality."  Operand rebase fails that test in the OPPOSITE
			//! direction from a rare-and-risky construct: it is the DOMINANT
			//! in-corpus csg_object idiom, not a rare exception -- 9 of the 14
			//! csg_object scenes in the corpus (`grep -rl '^csg_object$'
			//! scenes/`) use exactly this
			//! two-halves-positioned-locally-then-rebased-as-a-unit pattern
			//! (the "lens" idiom -- see
			//! scenes/FeatureBased/Combined/crystal_lens.RISEscene and
			//! docs/SCENE_CONVENTIONS.md sec 5.5), and every one of them
			//! already carries `allow_transformed_operands TRUE` as of the
			//! commit that introduced the runtime advisory (27aeeae4).  A
			//! creation gate here would refuse the common, correct case far
			//! more often than the rare mistake it exists to catch --
			//! training exactly the reflexive-acknowledgment habituation the
			//! stop rule warns against, for a construct that is ordinary
			//! rather than risky.  Warning tier, surfaced once post-derive, is
			//! therefore the whole mechanism: it tells the author (or the
			//! agent reading diagnostics) what happened and how to
			//! acknowledge or fix it, without refusing legitimate authoring.
			static const char* const CSG_OPERAND_REBASE = "CSG_OPERAND_REBASE";
		}
	}
}

#endif
