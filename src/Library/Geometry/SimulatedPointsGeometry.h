//////////////////////////////////////////////////////////////////////
//
//  SimulatedPointsGeometry.h - M7 prerequisites: a particle set read from
//    a simulation cache (points.v1), one frame per render frame, each point
//    rendered as a sphere of its radius.
//
//  INTERSECTION.  One BVH over the points (BVH<uint32_t>, Acceleration/
//  BVH.h), rebuilt in full at every install on the thread pool (the same
//  tree as a serial build).  The per-point test is SphereGeometry's own --
//  the ray moved into the sphere's frame (origin minus centre), the same
//  root solve, normal, spherical UV and closed-form derivatives -- so a
//  non-blurred point hits exactly like a sphere_geometry of that radius at
//  that centre.
//
//  DEFORMATION BLUR.  As SimulatedMeshGeometry: two keys (shutter open and
//  close centres from the frame velocities, or from neighbours matched by
//  id), element boxes enclosing both, and the centre interpolated linearly at
//  the sample's normalised shutter time (the calling thread's time-indexed
//  pose, else the scene animator's last evaluated time).
//
//  SHADING.  The hit reports the sphere's UV; with `color_attribute` the
//  point's attribute (1 or 3 components) is the hit's vertex colour
//  (vertex_color_painter).  Points are not emitters (CanBeAreaLight false).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMULATED_POINTS_GEOMETRY_
#define SIMULATED_POINTS_GEOMETRY_

#include "Geometry.h"
#include "../Acceleration/BVH.h"
#include "../Interfaces/IRenderPreparationController.h"
#include "../Simulation/SimulatedPointsSource.h"
#include "../TreeElementProcessor.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Animator;

		class SimulatedPointsGeometry :
			public virtual Geometry,
			public virtual TreeElementProcessor<std::uint32_t>,
			public ITimeVaryingAssetProvider
		{
		public:
			struct Options
			{
				Simulation::PointsSourceOptions source;
				bool motionBlur = true;
			};

			//! Binds the cache (fails closed, see PointsCacheSource::Open) and
			//! installs the bind frame.  Null on failure.
			static SimulatedPointsGeometry* Create( const std::string& cacheRoot, const Options& options, std::string& error );
			//! A producer with no cache yet: no points, every prepare a no-op.
			static SimulatedPointsGeometry* CreateUnbound( const Options& options );

			void SetSampleClock( const Animator* clock );

			// ITimeVaryingAssetProvider
			bool PrepareAssetForRender( const RenderTimeSupport& support, std::string& error, std::function<void()>* rollback ) override;
			bool AcquireAssetFreeze( std::string& error ) override;
			bool ReleaseAssetFreeze( std::string& error ) override;

			bool HasSource() const { return static_cast<bool>( m_source ); }
			const Simulation::PointsCacheSource& Source() const { return *m_source; }

			// Diagnostics / test hooks.
			std::size_t PointCount() const { return m_radius.size(); }
			bool BlurActive() const { return m_blur; }
			bool Hidden() const { return m_hidden; }
			std::int64_t InstalledFrame() const { return m_installedFrame; }
			unsigned long long InstallCount() const { return m_installs; }
			Scalar ShutterFraction() const;
			Point3 Centre( std::size_t i ) const { return m_centre[i]; }
			Scalar Radius( std::size_t i ) const { return m_radius[i]; }

			// IGeometry
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;
			bool DoPreHitTest() const override { return true; }
			bool CanBeAreaLight() const override { return false; }
			bool CanTessellate() const override { return false; }
			bool TessellateToMesh( IndexTriangleListType&, VerticesListType&, NormalsListType&, TexCoordsListType&, const unsigned int ) const override { return false; }
			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea() const override { return m_area; }
			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) override {}
			void RegenerateData() override {}

			// TreeElementProcessor<uint32_t>
			void RayElementIntersection( RayIntersectionGeometric& ri, const std::uint32_t elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			void RayElementIntersection( RayIntersection& ri, const std::uint32_t elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const std::uint32_t elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			BoundingBox GetElementBoundingBox( const std::uint32_t elem ) const override;
			bool ElementBoxIntersection( const std::uint32_t elem, const BoundingBox& bbox ) const override;
			char WhichSideofPlaneIsElement( const std::uint32_t elem, const Plane& plane ) const override;
			void SerializeElement( IWriteBuffer&, const std::uint32_t ) const override {}
			void DeserializeElement( IReadBuffer&, std::uint32_t& ret ) const override { ret = 0; }

		protected:
			SimulatedPointsGeometry( const Options& options, std::unique_ptr<Simulation::PointsCacheSource> source );
			virtual ~SimulatedPointsGeometry();

		private:
			struct State
			{
				std::vector<Point3>	centre, open, close;
				std::vector<Scalar>	radius;
				std::vector<float>	color;
				bool				blur = false, hidden = false;
				double				openTime = 0, closeTime = 0, nominal = 0;
				std::int64_t		frame = 0;
				std::string			identity;
			};
			bool PrepareFrame( const RenderTimeSupport& support, std::string& error, std::function<void()>* rollback );
			//! Swaps `next` in (it then holds the previous state) and rebuilds the BVH.
			void Install( State& next );
			Point3 CentreAt( std::uint32_t i, Scalar f ) const;

			Options										m_options;
			std::unique_ptr<Simulation::PointsCacheSource>	m_source;
			const Animator*								m_clock;
			BVH<std::uint32_t>*							m_bvh;
			std::vector<std::uint32_t>					m_elements;
			// The installed frame (written only between frames).
			std::vector<Point3>	m_centre, m_open, m_close;
			std::vector<Scalar>	m_radius;
			std::vector<float>	m_color;
			bool				m_blur, m_hidden;
			double				m_openTime, m_closeTime, m_nominal, m_invSpan;
			std::int64_t		m_installedFrame;
			std::string			m_identity;
			Scalar				m_area;
			unsigned long long	m_installs;
			std::int64_t		m_lastWarnedMissing;
			std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> m_preparedSnapshot;
			RenderTimeSupport	m_preparedSupport;
			mutable std::mutex	m_freezeMutex;
			unsigned int		m_leases;
			unsigned long long	m_generation, m_frozenGeneration;
		};
	}
}

#endif
