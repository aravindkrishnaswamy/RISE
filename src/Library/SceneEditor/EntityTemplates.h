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
