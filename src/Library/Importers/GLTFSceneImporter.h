//////////////////////////////////////////////////////////////////////
//
//  GLTFSceneImporter.h - Bulk glTF 2.0 scene importer.
//  Walks a .glb / .gltf file's scene tree and populates an IJob
//  with one geometry per primitive, one PBR material per glTF
//  material, painters per texture (.glb embedded images go straight
//  through Job::AddInMemoryPNG/JPEGTexturePainter -- no disk
//  round-trip), one light per KHR_lights_punctual, and one camera
//  (first only -- RISE has a single active camera).
//
//  See docs/GLTF_IMPORT.md §7 (phased plan), §13 (delivered vs
//  deferred), and §15 (Phase 4 status) for the full picture.  Phase 4
//  added: KHR_materials_emissive_strength, KHR_materials_unlit, per-
//  pixel alpha (straight, via IPainter::GetAlpha), alphaMode = BLEND
//  via transparency_shaderop, the standalone Charlie/Neubelt sheen
//  BRDF, and the scalar subset of KHR_materials_transmission + volume
//  + ior.  Out-of-scope features (animation, skinning, morph targets,
//  KHR_materials_clearcoat/sheen as a layer over PBR,
//  transmission_texture, other KHR_materials_* extensions beyond the
//  core PBR shape and the scalar transmission triplet) emit a one-time
//  warning per file or per material.  Phase 3 retired the lossy Euler-
//  decomposition transform path: node-world matrices now flow through
//  Job::AddObjectMatrix verbatim.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GLTF_SCENE_IMPORTER_
#define GLTF_SCENE_IMPORTER_

#include "../Interfaces/IJob.h"
#include <climits>
#include <string>

namespace RISE
{
	namespace Implementation
	{
		struct GLTFImportOptions
		{
			std::string namePrefix;					///< Prefix for all created object names (avoids manager-level collisions)
			unsigned int sceneIndex;				///< glTF scene index, or `kSceneIndexDefault` to import the file's default scene
			bool importMeshes;						///< Create per-primitive standard_objects
			bool importMaterials;					///< Create one pbr_metallic_roughness_material per glTF material; else use a default
			bool importLights;						///< Create lights from KHR_lights_punctual
			bool importCameras;						///< Create cameras (first only; subsequent ones warn)
			bool importNormalMaps;					///< Attach normal_map_modifier when a material has normalTexture

			//! Sentinel for "use the file's default scene" (i.e., the scene
			//! the glTF JSON's top-level `"scene"` field points at, falling
			//! back to scenes[0] when no default is set).  Distinct from
			//! `sceneIndex = 0` which now means "explicitly select the first
			//! scene in the array" -- the two semantics diverge for files
			//! whose default is not the first.
			static const unsigned int kSceneIndexDefault = UINT_MAX;

			GLTFImportOptions() :
			  namePrefix( "gltf" ),
			  sceneIndex( kSceneIndexDefault ),
			  importMeshes( true ),
			  importMaterials( true ),
			  importLights( true ),
			  importCameras( true ),
			  importNormalMaps( true )
			{}
		};

		//! GLTFSceneImporter does NOT inherit from Reference; it's a one-shot
		//! local stack object used inside Job::ImportGLTFScene.  All persistent
		//! state ends up in the IJob's managers (geometries, painters,
		//! materials, modifiers, lights, cameras, objects).
		class GLTFSceneImporter
		{
		public:
			GLTFSceneImporter( const char* glbPath );
			~GLTFSceneImporter();

			//! Walk the scene tree and populate `job`.  Returns false on parse
			//! / validation error.  Note: this method is NOT all-or-nothing.
			//! Materials are created up front (loop in Import) before the scene
			//! walk; if the file parses + validates but has a malformed scene
			//! reference, the materials and their texture painters will still
			//! be in the painter / material managers.  Callers that need
			//! atomic import should check the return value AND call
			//! `Job::Reset` (or equivalent) on failure to clean up.
			bool Import( IJob& job, const GLTFImportOptions& opts );

		private:
			std::string		szFilename;				///< Resolved on-disk path to the .glb / .gltf

			// Internal helpers (defined in the .cpp).  Forward-declared cgltf
			// types are referenced through opaque void* in the header to
			// avoid leaking cgltf into IJob consumers.
		};
	}
}

#endif
