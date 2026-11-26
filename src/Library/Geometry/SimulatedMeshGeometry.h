//////////////////////////////////////////////////////////////////////
//
//  SimulatedMeshGeometry.h - Simulation M3b: a fixed-topology triangle
//    mesh whose vertices come from a simulation cache (mesh.v1), one frame
//    per render frame, with deformation motion blur.
//
//  WHY A SUBCLASS OF THE INDEXED MESH.  Every IGeometry service (BVH,
//  area sampling, signals, proximity, double-sided handling, the hit record
//  the shaders read) is the indexed mesh's own, so a non-blurred simulated
//  mesh IS an indexed mesh at the installed frame -- bit-equal hits.  What
//  differs is confined to the TreeElementProcessor callbacks this class
//  overrides (element box, float filter, element intersection) and to the
//  installer.  The static mesh's code and vtable are untouched: a plain
//  TriangleMeshGeometryIndexed never reaches any of this.
//
//  DEFORMATION BLUR.  Each frame installs two KEYS, the positions (and
//  normals) at shutter open and close, from the frame's velocities
//  (SimulatedMeshSource: P + V * dt, the nearest frame).  While keys are
//  installed, the BVH's element boxes enclose both keys (Refit), the float
//  filter is off (its precomputed edges are one pose), and every element
//  intersection interpolates the triangle's vertices and normals linearly at
//  the sample's normalised shutter time.  The sample time comes from the
//  transform blur's own mechanism: the calling thread's time-indexed pose
//  (TimeIndexed::tlsSampleTime, written by TimeIndexedThread::PoseAt) or,
//  outside a time-indexed pass, the time the shared scene was last posed at
//  (Animator::LastEvaluatedTime).  Area sampling, GetArea, the signal bakes
//  and the proximity queries stay at the nominal positions.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMULATED_MESH_GEOMETRY_
#define SIMULATED_MESH_GEOMETRY_

#include "TriangleMeshGeometryIndexed.h"
#include "../Interfaces/IRenderPreparationController.h"
#include "../Simulation/SimulatedMeshSource.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RISE
{
	class IScene;

	namespace Implementation
	{
		class Animator;

		//! True when some object of the scene renders a SimulatedMeshGeometry
		//! (such a scene renders through the prepared, per-frame-install path).
		bool SceneBindsSimulatedMesh( const IScene& scene );

		class SimulatedMeshGeometry :
			public TriangleMeshGeometryIndexed,
			public ITimeVaryingAssetProvider
		{
		public:
			struct Options
			{
				Simulation::MeshSourceOptions source;
				bool motionBlur = true;
				bool faceNormals = false;
				bool doubleSided = false;
			};

			//! Binds the cache (fails closed, see MeshCacheSource::Open) and
			//! builds the mesh at the bind frame.  Null on failure.
			static SimulatedMeshGeometry* Create( const std::string& cacheRoot,
				const Options& options, std::string& error );

			//! Simulation M4: a mesh with no frame source yet (its producer has
			//! no cache): no triangles, hidden, every prepare a no-op.  The
			//! first RebindSource (a live run's publication) builds the
			//! topology from that source's bind frame.
			static SimulatedMeshGeometry* CreateUnbound( const Options& options );

			//! The shared-scene pose clock (the scene's animator), held by a
			//! counted reference.  Set by the Job before each prepared render;
			//! may be null (then the nominal time is used outside a
			//! time-indexed pass).
			void SetSampleClock( const Animator* clock );

			// ITimeVaryingAssetProvider
			bool PrepareAssetForRender( const RenderTimeSupport& support,
				std::string& error, std::function<void()>* rollback ) override;
			bool AcquireAssetFreeze( std::string& error ) override;
			bool ReleaseAssetFreeze( std::string& error ) override;

			//! Simulation M3c: swaps the frame source (a live in-memory
			//! publication for the baked cache, and back).  The topology must
			//! be the bound mesh's; the next prepare installs from the new
			//! source.  Only between frames (the caller parks the render) and
			//! never under the freeze.  On success `source` holds the previous one.
			bool RebindSource( std::unique_ptr<Simulation::MeshCacheSource>& source, std::string& error );
			//! Simulation M4: drops the frame source (the mesh hides; its
			//! topology stays, so a later RebindSource of the same topology
			//! shows it again).  `previous` receives the source.  Same rules
			//! as RebindSource (between frames, never under the freeze).
			bool UnbindSource( std::unique_ptr<Simulation::MeshCacheSource>& previous, std::string& error );
			//! False for an unbound mesh (CreateUnbound, UnbindSource): Source()
			//! must not be called then.
			bool HasSource() const { return static_cast<bool>( m_source ); }
			//! The topology has been built (from a cache or a first live frame).
			bool TopologyBuilt() const { return m_built; }

			// Diagnostics / test hooks.
			bool BlurActive() const { return m_blur; }
			bool Hidden() const { return m_hidden; }
			std::int64_t InstalledFrame() const { return m_installedFrame; }
			const std::string& InstalledIdentity() const { return m_identity; }
			unsigned long long InstallCount() const { return m_installs; }
			const Simulation::MeshCacheSource& Source() const { return *m_source; }
			//! Normalised shutter time a ray on the calling thread would use now.
			Scalar ShutterFraction() const;

			// IGeometry: a hidden frame (missing-frame policy `hide`) has no hits.
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			// TreeElementProcessor: the deformation-blur hooks.
			void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			BoundingBox GetElementBoundingBox( const MYOBJ elem ) const override;
			bool GetFloatTriangleVertices( const MYOBJ elem, float v0[3], float v1[3], float v2[3] ) const override;

		protected:
			SimulatedMeshGeometry( const Options& options, std::unique_ptr<Simulation::MeshCacheSource> source );
			virtual ~SimulatedMeshGeometry();

		private:
			struct State
			{
				VerticesListType	positions;
				NormalsListType		normals;
				VerticesListType	keyOpen, keyClose;
				NormalsListType		normalOpen, normalClose;
				bool				blur = false;
				bool				hidden = false;
				double				open = 0, close = 0, nominal = 0;
				std::int64_t		frame = 0;
				std::string			identity;
			};
			bool PrepareFrame( const RenderTimeSupport& support, std::string& error,
				std::function<void()>* rollback );
			bool BuildTopology( const Simulation::MeshCacheSource& source );
			State Capture() const;
			bool Install( State& next, std::string& error );
			void ComputeNormals( const VerticesListType& p, NormalsListType& out ) const;

			Options										m_options;
			std::unique_ptr<Simulation::MeshCacheSource>	m_source;
			const Animator*								m_clock;
			// The installed frame.  Written only between frames (Install).
			VerticesListType	m_keyOpen, m_keyClose;
			NormalsListType		m_normalOpen, m_normalClose;
			bool				m_blur;
			bool				m_hidden;
			double				m_open, m_close, m_nominal, m_invSpan;
			std::int64_t		m_installedFrame;
			std::string			m_identity;
			unsigned long long	m_installs;
			std::int64_t		m_lastWarnedMissing;
			// M3c: the generation and support of the last successful prepare.
			std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> m_preparedSnapshot;
			RenderTimeSupport	m_preparedSupport;
			// Freeze bookkeeping.
			mutable std::mutex	m_freezeMutex;
			unsigned int		m_leases;
			unsigned long long	m_generation, m_frozenGeneration;
			// M4: whether BuildTopology ran, and the digest of the topology it built.
			bool				m_built;
			std::string			m_builtTopology;
		};
	}
}

#endif
