//////////////////////////////////////////////////////////////////////
//
//  AgentDiagnostic.h - structured validation diagnostics produced by
//    AgentSession::Validate (docs/agentic-redesign/50-agentic-surface.md §2.5).
//
//    Each diagnostic carries severity, a stable machine-matchable code, a
//    human-readable message, and a best-effort byte span (offset/length)
//    into the validated `.RISEscene` text (0/0 when non-localizable).
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
		//! One structured validation diagnostic (byte span into validated text;
		//! 0/0 when not localizable).
		struct AgentDiagnostic
		{
			enum class Severity { Error, Warning, Info };

			Severity    severity = Severity::Error;
			std::string code;      //!< stable machine-matchable code (AgentDiagnosticCode)
			std::string message;   //!< human-readable diagnostic message
			std::size_t offset = 0;   //!< byte offset in validated text (0 if unlocalized)
			std::size_t length = 0;   //!< byte length in validated text (0 if unlocalized)
		};

		//! Stable diagnostic code strings matching JSON-RPC `code` fields 1:1.
		namespace AgentDiagnosticCode
		{
			//! CST parse failure (e.g. unbalanced braces).
			static const char* const PARSE_ERROR       = "PARSE_ERROR";
			//! Unregistered chunk keyword.
			static const char* const UNKNOWN_CHUNK     = "UNKNOWN_CHUNK";
			//! Parameter not declared on its chunk's descriptor.
			static const char* const UNKNOWN_PARAMETER = "UNKNOWN_PARAMETER";
			//! Ill-typed or missing parameter value.
			static const char* const INVALID_VALUE     = "INVALID_VALUE";
			//! Derive-time failure carried verbatim from DeriveToJob.
			static const char* const DERIVE_ERROR      = "DERIVE_ERROR";
			//! Candidate contains no scene chunks (empty/whitespace/comments only).
			static const char* const EMPTY_DOCUMENT    = "EMPTY_DOCUMENT";

			// ---- Design-note advisories (Severity::Info, self-disarming,
			//      computed via ComputeDesignNoteConditionsFromDoc_ in AgentSession.cpp) ----

			//! >= 3 standard_objects with no scalar_painter chunk (docs/agentic-redesign/73-creative-richness-design.md §9).
			static const char* const DESIGN_SCALAR_PIPE_UNUSED    = "DESIGN_SCALAR_PIPE_UNUSED";
			//! >= 4 standard_objects with no sdf_geometry / sweep_geometry / displaced_geometry.
			static const char* const DESIGN_NO_ADVANCED_GEOMETRY  = "DESIGN_NO_ADVANCED_GEOMETRY";
			//! >= 5 hand-authored standard_objects sharing geometry and all non-transform
			//! bindings while no `source` / `count_u` / `count_v` instancing is used.
			static const char* const DESIGN_HAND_REPEATED_COPIES  = "DESIGN_HAND_REPEATED_COPIES";
			//! >= 3 materials expose flat numeric microsurface slots with none spatially
			//! varying; names `vary_material` (shares predicate with AgentSession::VaryMaterial).
			static const char* const DESIGN_CONSTANT_MICROSURFACE = "DESIGN_CONSTANT_MICROSURFACE";
			//! `expression_painter` / `scalar_painter{expression}` with >= kParamErosionLiteralGate
			//! distinct numeric literals and <= kParamErosionMaxParams `param` declarations.
			static const char* const DESIGN_PARAM_METADATA_EROSION = "DESIGN_PARAM_METADATA_EROSION";
			//! Unreferenced Painter/Function chunk (document-wide SceneReferenceGraph scan;
			//! flags chain heads iteratively; excludes Materials).
			static const char* const DESIGN_ORPHANED_PAINTERS = "DESIGN_ORPHANED_PAINTERS";
			//! Unreferenced Material chunk (suppressed while build protocol is in Pieces phase).
			static const char* const DESIGN_UNBOUND_MATERIAL = "DESIGN_UNBOUND_MATERIAL";
			//! >= 3 standard_objects where every color-pipe material slot resolves to a
			//! constant painter (uniformcolor/blackbody/spectral).
			static const char* const DESIGN_FLAT_ALBEDO = "DESIGN_FLAT_ALBEDO";
			//! Name heuristic: object/material/painter contains a transmissive keyword
			//! (glass, crystal, water, etc.) but material kind is opaque/reflection-only
			//! (excludes emitters and file/wrapper kinds).
			static const char* const DESIGN_MATERIAL_KIND_MISMATCH = "DESIGN_MATERIAL_KIND_MISMATCH";
			//! `sdf_geometry` `smin` joint whose blend radius k exceeds ~1/3 of the joined
			//! part's characteristic dimension (excludes `skeleton_geometry`).
			static const char* const DESIGN_SDF_BLEND_SCALE = "DESIGN_SDF_BLEND_SCALE";
			//! Bound literal low-roughness (<0.3), high-metallic (>=0.7) PBR material
			//! coexisting with a strongly saturated uniformcolor_painter `radiance_map`.
			static const char* const DESIGN_ENV_REFLECTION = "DESIGN_ENV_REFLECTION";
			//! >= 3 materials paint flat constant color on normal-bearing geometry; names
			//! `add_wear` (shares predicate with AgentSession::AddWear; docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md).
			static const char* const DESIGN_UNWORN_MATERIALS = "DESIGN_UNWORN_MATERIALS";
			//! Positional light (omni/spot/rect/shape) lies inside the derived world bbox of
			//! an opaque-material object (excluding camera-enclosing rooms and the light's
			//! own fixture). Requires a derived scene; silent on stateless scans without one.
			static const char* const DESIGN_ENCLOSED_LIGHT_OPAQUE_SHELL = "DESIGN_ENCLOSED_LIGHT_OPAQUE_SHELL";
			//! Positional light with non-trivial authored power/exitance whose valid cached
			//! `light_scene` solo share is < 2% (suppressed if M already names the light).
			//! Requires session solo cache; mutually exclusive with DESIGN_DIM_LIGHT_REMEASURE.
			static const char* const DESIGN_DIM_HERO_LIGHT = "DESIGN_DIM_HERO_LIGHT";
			//! Light previously flagged as a qualified dim finding whose chunk text has since
			//! changed (same kind, not enclosed by M), nudging a `light_scene` re-measure.
			//! Requires session solo cache; mutually exclusive with DESIGN_DIM_HERO_LIGHT.
			static const char* const DESIGN_DIM_LIGHT_REMEASURE = "DESIGN_DIM_LIGHT_REMEASURE";
			//! Rain/wet vocabulary in scene names or comments while qualifying materials
			//! remain dry; names `add_wetness` (shares predicate with AgentSession::AddWetness;
			//! docs/WETNESS_COAT_DESIGN.md).
			static const char* const DESIGN_DRY_RAIN_SCENE = "DESIGN_DRY_RAIN_SCENE";
			//! Spatially-varying non-emission, non-wetness-prelude color-pipe binding on a
			//! renderable standard_object/csg_object (resolved through source chains, wrapper
			//! materials, and enclosing CSG overrides) with no effective modifier (excludes
			//! hair and luminaires; docs/RELIEF_MODIFIER_DESIGN.md §9).
			static const char* const DESIGN_FLAT_RELIEF = "DESIGN_FLAT_RELIEF";

			// ---- Functional warnings (Severity::Warning) ----

			//! Emissive material bound to a null-geometry object (e.g. `csg_object`), skipped
			//! by NEE at render time. Suppressed by `allow_non_sampling_emitter TRUE`; paired
			//! with a creation gate in InsertChunk/ProposePatch via CollectNullGeometryEmitters_.
			static const char* const LUMINAIRE_NULL_GEOMETRY = "LUMINAIRE_NULL_GEOMETRY";
			//! Non-identity `csg_object` transform composing over already-transformed operands
			//! (docs/SCENE_CONVENTIONS.md §5.5). Suppressed by `allow_transformed_operands TRUE`.
			//! Warning-only (intentionally no creation gate since rebased halves are common).
			static const char* const CSG_OPERAND_REBASE = "CSG_OPERAND_REBASE";
		}
	}
}

#endif
