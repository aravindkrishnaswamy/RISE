//////////////////////////////////////////////////////////////////////
//
//  EntityTemplates.h - Static "Add Entity" template registry for the
//    interactive editor.  A template is a data-driven recipe: one or
//    more complete v7 chunk texts (root chunks first) that, once the
//    placeholder tokens below are substituted, insert cleanly via
//    SceneEditController::ApplyAgentInsertChunk.
//
//    Every template's chunk text is grammar-valid v7, cross-checked
//    against the live ChunkDescriptorRegistry / real scenes/ examples
//    at authoring time -- every parameter written here exists on the
//    corresponding chunk's Describe() (see docs/... none yet; the
//    cross-check happened during EntityTemplatesTest.cpp authoring).
//
//    Placeholder tokens substituted by
//    SceneEditController::InstantiateEntityTemplate before each
//    chunk in the sequence is inserted:
//      @NAME@      -> the deduped instance name chosen for this Add
//                     (also used to derive @NAME@_geo / @NAME@_rd /
//                     etc. sub-chunk names baked directly into the
//                     template text below).
//      @MATERIAL@  -> (Object templates only) the name of an existing
//                     material, or a freshly-bundled default Lambertian
//                     if the scene has none yet.
//      @TEXTURE@   -> (the png_painter template only) an absolute path
//                     to a runtime-generated placeholder texture file
//                     (a scene-authored relative path would depend on
//                     RISE_MEDIA_PATH being configured, which a
//                     template must not assume).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_ENTITYTEMPLATES_
#define RISE_ENTITYTEMPLATES_

#include "SceneEditController.h"
#include "../Parsers/ChunkDescriptor.h"
#include <string>
#include <vector>

namespace RISE
{
	//! One instantiable "Add Entity" template.
	struct EntityTemplateDef
	{
		SceneEditController::Category category         = SceneEditController::Category::None;
		std::string                   label;             //!< display label, e.g. "Sphere"
		std::string                   baseName;           //!< dedup base name, e.g. "sphere"
		bool                          hasNamedIdentity = true;  //!< false only for hosek_wilkie_skylight (no `name` param on that chunk)
		std::string                   fixedResultName;     //!< when !hasNamedIdentity: the entity name Instantiate should report (the light the chunk atomically creates)
		bool                          needsMaterial    = false; //!< true for Object templates -- @MATERIAL@ must be resolved before substitution
		bool                          needsTexture     = false; //!< true only for the png_painter template -- @TEXTURE@ must be resolved before substitution
		std::vector<std::string>      chunkTexts;          //!< root chunk texts, in insertion order (dependencies first)
	};

	//! Static table of entity templates, grouped by
	//! SceneEditController::Category.  Data-driven per the corpus
	//! frequency set in the design brief -- see EntityTemplates.cpp
	//! for the full list.
	class EntityTemplates
	{
	public:
		//! Number of templates registered for `cat` (0 for categories
		//! with no Add-Entity templates, e.g. Camera/Rasterizer/Film).
		static unsigned int Count( SceneEditController::Category cat );

		//! Template at `idx` within `cat`, or null if out of range.
		//! Pointer is valid for the lifetime of the program (backed by
		//! a function-static table).
		static const EntityTemplateDef* At( SceneEditController::Category cat, unsigned int idx );

		//! A minimal, self-contained `uniformcolor_painter` chunk text
		//! (mid-gray) bundled as a dependency when a template needs a
		//! Painter reference and none suitable exists yet.  `name` is
		//! the chunk's own name -- caller is responsible for dedup.
		static std::string DefaultPainterChunkText( const std::string& name );

		//! A minimal, self-contained `lambertian_material` chunk text
		//! bound to `painterName`'s reflectance.  Bundled as the
		//! Object templates' fallback material when the scene has none
		//! yet.  `name` is the chunk's own name -- caller is
		//! responsible for dedup.
		static std::string DefaultLambertianChunkText( const std::string& name, const std::string& painterName );

		//! ---- S18 (docs/gui/NODE_GRAPH_CANVAS.md sect. 6): KEYWORD-driven
		//! default bodies for the node-graph canvas's "create node" verb.
		//!
		//! The static table above is a fixed PICKER of hand-authored
		//! recipes ("Sphere", "Omni Light").  The canvas needs something
		//! different: given ANY painter/material KEYWORD the user dragged
		//! out of the search palette, produce the MINIMAL body that
		//! derives cleanly with zero further input.  Sourced from the live
		//! ChunkDescriptorRegistry (so a new chunk keyword is creatable
		//! the day its Describe() lands) plus the small seed table below
		//! for the handful of shapes a ChunkDescriptor cannot express.
		//!
		//! WHY A SEED TABLE IS UNAVOIDABLE.  `ParameterDescriptor::required`
		//! is METADATA ONLY -- the parameter dispatcher does not enforce it
		//! (see ChunkParserRegistry.cpp's "REQUIRED PARAMETERS" comment) --
		//! and, worse, several chunks carry minimal-validity rules the
		//! descriptor has no vocabulary for at all:
		//!   * a REPEATABLE MINIMUM (`ramp_painter` needs >= 2 `stop`
		//!     lines; `stop` is merely `repeatable`, never `required`);
		//!   * a ONE-OF FORM SELECTION (`scalar_painter` has thirteen
		//!     mutually-exclusive forms, none of them `required`, and a
		//!     body with no form at all is not a usable painter).
		//! Those live in the seed table.  Everything else is derived.
		struct ChunkNodeArg
		{
			std::string param;
			std::string value;
		};

		//! One parameter the CALLER must supply for `keyword` -- a
		//! `required` parameter the descriptor gives no usable default
		//! for and the seed table deliberately does not invent one for.
		//! Today these are exactly the required REFERENCE parameters
		//! (`ramp_painter.input`, `mapping_painter.source`, ...): a
		//! reference names another chunk in THIS scene, so no static
		//! default can exist -- the S21 canvas supplies it from the drag
		//! context (the wire the user dropped onto the new node).
		struct ChunkNodeRequirement
		{
			std::string                param;
			std::string                description;      //!< the descriptor's own text, verbatim
			//! True iff the param is ValueKind::Reference (or, for the
			//! kNodeExtraRequirements table, flagged `isReference` there).
			//! THE authoritative reference/literal flag -- round-1 P3:
			//! `referenceCategories` below must NOT be used to infer this,
			//! because a reference param with an unrestricted (empty)
			//! category list -- legal; ConnectionLegality's CategoryAllowed
			//! treats empty as "any category" -- would otherwise read back
			//! as "not a reference" even though it is one.
			bool                       isReference = false;
			std::vector<ChunkCategory> referenceCategories;   //!< legal categories when isReference; empty = unrestricted
		};

		//! The caller-supplied-argument contract for `keyword` (empty for
		//! a keyword that needs nothing, and for an unknown keyword).
		//! Pure descriptor read -- no scene state, no locking.
		static std::vector<ChunkNodeRequirement> NodeRequirements( const std::string& keyword );

		//! Build the minimal derivable chunk text for `keyword` named
		//! `name`, with `args` supplying (at least) every
		//! NodeRequirements() param.  Emits ONLY the lines that are
		//! load-bearing -- `name`, every caller arg, and the seeds -- and
		//! deliberately omits every optional parameter so the chunk's own
		//! Finalize() defaults apply (a `defaultValueHint` is a GUI HINT,
		//! not always a parser-acceptable literal: "unlimited", "none",
		//! "noname").
		//!
		//! Returns false WITHOUT touching `outText` when: the keyword is
		//! unknown; its category is not Painter or Material (this verb is
		//! the canvas's node creator, not a general chunk inserter -- use
		//! the agent surface's insert_chunk for anything else); an arg
		//! names a parameter the descriptor does not declare; a required
		//! arg is missing; or an arg value carries a newline or a `}`
		//! (which would break out of the chunk body).  `outDiag` always
		//! receives the reason.
		static bool BuildNodeChunkText( const std::string& keyword,
		                                const std::string& name,
		                                const std::vector<ChunkNodeArg>& args,
		                                std::string& outText,
		                                std::string& outDiag );

		//! Ensure a small placeholder PNG texture file exists on disk
		//! for the png_painter template's `file` parameter (a real
		//! file has to exist for AddPNGTexturePainter to succeed -- a
		//! scene-authored relative path would depend on RISE_MEDIA_PATH
		//! being configured, which a general-purpose template must not
		//! assume).  Idempotent within a process: writes once, reuses
		//! the file on repeat calls (checks for existence first).
		//! Returns an absolute path, or an empty string on write
		//! failure (disk full, unwritable temp dir, etc.).
		static std::string EnsureDefaultTextureFile();
	};
}

#endif
