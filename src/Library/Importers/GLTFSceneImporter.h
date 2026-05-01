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
//  Lifecycle (since 2026-05-01): the constructor runs cgltf_parse_file
//  + cgltf_load_buffers + cgltf_validate ONCE for the file.  The
//  destructor runs cgltf_free.  ImportScene (bulk) and ImportPrimitive
//  (single primitive — the path used by `gltf_geometry` chunks via
//  Job::AddGLTFTriangleMeshGeometry) BOTH consume that single parse.
//  Pre-2026-05-01 the bulk path called Job::AddGLTFTriangleMeshGeometry
//  per primitive, which constructed a fresh TriangleMeshLoaderGLTF that
//  re-parsed the .gltf and re-read the .bin from disk for every primitive
//  — quadratic-or-worse on heavyweight assets like NewSponza (a 1.5 GB
//  .bin reread ~200 times).  TriangleMeshLoaderGLTF and the matching
//  RISE_API_CreateGLTFTriangleMeshLoader were retired in the same
//  cleanup; this importer is the sole glTF entry point.
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
#include <set>
#include <string>

namespace RISE
{
	// Forward declarations — these flow through the importer's public
	// surface but pulling them in via header would drag the geometry
	// interface chain into every translation unit that just needs to
	// kick off an import.
	class ITriangleMeshGeometryIndexed;

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
			bool lowmemTextures;					///< Defer texture color-space conversion to per-sample access.  Trades ~25% per-sample render cost for a ~4x peak texture-memory reduction and a 5-10x faster scene load.  Default false (final-render workflow); flip to true for iteration on heavy-PBR scenes (NewSponza class).  See SCENE_CONVENTIONS.md / the sponza_new.RISEscene file header for the trade-off.
			double lightsIntensityOverride;			///< When > 0, replaces the intensity of any imported KHR_lights_punctual entry whose authored intensity is exactly 0.  Default 0 (no override).  Many assets (NewSponza is the canonical case) carry their light fixtures as positional metadata with intensity=0 by author convention — "lighting is up to the renderer".  Setting this override > 0 wakes up those dormant fixtures uniformly, without touching lights the author did set non-zero intensities for.

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
			  importNormalMaps( true ),
			  lowmemTextures( false ),
			  lightsIntensityOverride( 0.0 )
			{}
		};

		//! GLTFSceneImporter does NOT inherit from Reference; it's a one-shot
		//! local stack object used inside Job::ImportGLTFScene and
		//! Job::AddGLTFTriangleMeshGeometry.  All persistent state ends up in
		//! the IJob's managers (geometries, painters, materials, modifiers,
		//! lights, cameras, objects).
		//!
		//! The cgltf parse runs in the constructor and lives as long as the
		//! importer.  Callers MUST check IsValid() before invoking ImportScene
		//! or ImportPrimitive — when the parse, buffer load, or validate step
		//! fails, IsValid() returns false and the import methods are no-ops
		//! that return false.
		//!
		//! Single-call contract: call ImportScene AT MOST ONCE per importer
		//! instance.  ImportPrimitive may be called any number of times so
		//! long as each call uses a unique geomName.  The reason: the bulk
		//! Walker memoizes per-(meshIdx,primIdx) registration in a function-
		//! local set so multi-node instancing of a shared mesh registers the
		//! geometry once and binds N objects (NewSponza columns, Khronos
		//! OrientationTest, instanced foliage etc.).  That set is fresh per
		//! ImportScene call; a second ImportScene on the same importer would
		//! see an empty set, attempt to re-register every geomName, hit
		//! pGeomManager's duplicate-name reject, and silently drop every
		//! object on the second pass.  Today every call site constructs a
		//! fresh importer per import so this is latent; if you ever need to
		//! re-walk a parsed file, construct a new GLTFSceneImporter.
		class GLTFSceneImporter
		{
		public:
			//! Resolve the path through GlobalMediaPathLocator, then run the
			//! full cgltf_parse_file → cgltf_load_buffers → cgltf_validate
			//! sequence.  Stores the parsed cgltf_data internally; freed in
			//! the destructor.  On failure logs the cause; IsValid() then
			//! returns false and other methods do nothing.
			explicit GLTFSceneImporter( const char* glbPath );
			~GLTFSceneImporter();

			//! True iff cgltf parsing + buffer load + validation all succeeded.
			bool IsValid() const;

			//! Bulk import: walks the requested (or default) scene tree and
			//! emits per-primitive geometries, per-material PBR materials +
			//! optional normal_map_modifier, per-image painters, lights, and
			//! every camera-bearing node (one named camera per node, with
			//! the first DFS-encountered camera designated active when no
			//! pre-existing user camera was set).  Note: this method is
			//! NOT all-or-nothing.  Materials are created up front before
			//! the scene walk; if the file parses + validates but has a
			//! malformed scene reference, the materials and their texture
			//! painters will still be in the manager.  Callers that need
			//! atomic import should check the return value AND call
			//! `Job::Reset` on failure to clean up.
			//!
			//! Concurrency: ImportScene is a structural mutation — like
			//! the upstream multi-camera CL's AddCamera contract, callers
			//! MUST NOT run ImportScene concurrently with rendering.  A
			//! scene-editor-driven re-import should follow the cancel-
			//! and-park pattern (cancel any in-flight pass, cv-wait until
			//! rendering goes false, then call ImportScene).  The per-
			//! camera snapshot/restore around the walk assumes a serial
			//! caller — an interleaving SetActiveCamera between snapshot
			//! and restore would silently revert.
			bool ImportScene( IJob& job, const GLTFImportOptions& opts );

			//! Single-primitive import: build geometry for `meshIdx.primIdx`
			//! and register it with `job` under `geomName`.  This is the
			//! entry point used by `Job::AddGLTFTriangleMeshGeometry` (the
			//! `gltf_geometry` chunk parser's target) — a chunk that imports
			//! one named primitive without the full scene walk.
			bool ImportPrimitive(
				IJob& job,
				const char* geomName,
				unsigned int meshIdx,
				unsigned int primIdx,
				bool doubleSided,
				bool faceNormals,
				bool flipV );

			//! Lower-level building block: fill the caller-owned geometry
			//! with the named primitive's vertex / index data.  Does NOT
			//! register the geometry with any manager and does NOT take
			//! ownership of `pGeom`.  Public so unit tests (and future
			//! consumers) can exercise the primitive-extraction logic in
			//! isolation; the importer's internal Walker and ImportPrimitive
			//! both delegate here.
			//!
			//! Returns false (and logs) on out-of-range indices,
			//! unsupported topology, Draco/meshopt compression, missing
			//! POSITION attribute, or accessor-count mismatches.
			bool BuildGeometryFromPrimitive(
				ITriangleMeshGeometryIndexed* pGeom,
				unsigned int meshIdx,
				unsigned int primIdx,
				bool flipV );

		private:
			std::string		szFilename;				///< Resolved on-disk path to the .glb / .gltf
			void*			cgltfData;				///< Opaque cgltf_data*; NULL when parse / load / validate failed.
													///  Ownership: this object owns the parse and frees it in the
													///  destructor.  Kept opaque to avoid leaking cgltf into IJob
													///  consumers.

			// PreDecodeTextures (called from ImportScene before the materials
			// loop) walks every material's textureslots, collects unique
			// (image, role) tuples, and submits the whole batch to
			// Job::AddTexturePaintersBatch — which decodes in parallel via
			// the global ThreadPool, then registers the painters serially.
			// On NewSponza-class assets (137 PNGs) this collapses tens of
			// seconds of single-threaded libpng decode to single-digit
			// seconds across the worker pool.  After PreDecodeTextures
			// returns, every texture painter the materials need is already
			// in pPntManager — and CreateTexturePainter's fast path returns
			// the painter name immediately on lookup hit.  See
			// `mRegisteredTextures` below.
			void PreDecodeTextures( IJob& job, const GLTFImportOptions& opts );

			// Painter names that PreDecodeTextures successfully registered.
			// CreateTexturePainter checks this first; on hit it skips its
			// own (now-redundant) decode + AddItem and just returns the
			// name.  On miss (a request that PreDecodeTextures didn't see
			// or that failed to decode), CreateTexturePainter falls through
			// to its per-call decode path, preserving the pre-2026-05-01
			// behaviour as a safety net.
			std::set<std::string>	mRegisteredTextures;
		};
	}
}

#endif
