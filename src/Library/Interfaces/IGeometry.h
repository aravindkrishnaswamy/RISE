//////////////////////////////////////////////////////////////////////
//
//  IGeometry.h - Geometry interface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IGEOMETRY_
#define IGEOMETRY_

#include "IReference.h"
#include "IKeyframable.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/BoundingBox.h"
#include "../Polygon.h"

namespace RISE
{
	//! Surface derivative data for specular manifold sampling.
	//! Contains position and normal partial derivatives at a surface point,
	//! needed for the Newton solver's constraint Jacobian.
	struct SurfaceDerivatives
	{
		Vector3 dpdu;		///< Position partial derivative w.r.t. first surface parameter
		Vector3 dpdv;		///< Position partial derivative w.r.t. second surface parameter
		Vector3 dndu;		///< Normal partial derivative w.r.t. first surface parameter
		Vector3 dndv;		///< Normal partial derivative w.r.t. second surface parameter
		Point2  uv;			///< Surface parameters at this point
		bool    valid;		///< True if derivatives were successfully computed

		SurfaceDerivatives() :
		dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
		dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
		uv( Point2(0,0) ), valid( false )
		{
		}
	};

	//! Geometry represents the basic geometry of a scene object
	//! It needs only to provide basic geometric intersection details
	class IGeometry :
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		IGeometry(){};
		virtual ~IGeometry(){};

	public:
		//! Tessellates this geometry into an indexed triangle mesh.
		//!
		//! Consumers (e.g. DisplacedGeometry, future GPU mesh export) use this to obtain a
		//! triangle-mesh representation of any geometry that supports it.  The four output
		//! vectors are appended to; callers typically pass freshly-constructed empty vectors.
		//!
		//! Detail semantics are per-geometry and documented on each override:
		//!  - Parametric primitives (sphere, torus, cylinder, box, disk, clipped plane):
		//!    `detail` is the number of segments per natural parameter axis.
		//!  - Mesh geometries (TriangleMesh, TriangleMeshGeometryIndexed): `detail` is ignored;
		//!    existing triangles are emitted unchanged.
		//!  - InfinitePlaneGeometry: returns false (cannot tessellate infinite extent).
		//!
		//! Default implementation returns false (unsupported).  Callers that need an error
		//! message should log it themselves using whatever user-facing name they have for
		//! the geometry — the base default deliberately stays silent to keep the interface
		//! header free of logging dependencies.
		/// \return TRUE if tessellation produced a mesh, FALSE if the geometry cannot be tessellated.
		virtual bool TessellateToMesh(
			IndexTriangleListType& tris,		///< [out] Indexed triangles (appended)
			VerticesListType&      vertices,	///< [out] Vertex positions (appended)
			NormalsListType&       normals,		///< [out] Per-vertex normals (appended)
			TexCoordsListType&     coords,		///< [out] Per-vertex (u,v) texture coords (appended)
			const unsigned int     detail		///< [in] Tessellation detail level (see per-geometry docs)
			) const
		{
			return false;
		}

		//! This the most important function
		//! It asks the geometric object to intersect itself
		//! and return intersection details
		//
		//! If a sub class doesn't override this method, then
		//! it will be called here and we'll handle it
		virtual void IntersectRay( 
			RayIntersectionGeometric& ri,				///< [in/out] Receives the geometric intersection information
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces,					///< [in] Should we process the intersection if the element is back facing?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! This function is here to help the shadow checks
		//! It asks the geometry object if the given ray
		//! will intersect the object, we don't care about
		//! where or the normal or any of that junk
		//!
		//! Similar to IntersectRay, if the sub class doesn't
		//! override this method, then it will be called here
		//! and we'll handle it
		/// \return TRUE if there is an intersection, FALSE otherwise
		virtual bool IntersectRay_IntersectionOnly(
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Generates a sphere that envelopes all the geometry
		virtual void GenerateBoundingSphere(
			Point3& ptCenter,							///< [out] Center of the bounding sphere
			Scalar& radius								///< [out] Radius of the bounding sphere
			) const = 0;

		//! Generates a box that envelopes all the geometry
		virtual BoundingBox GenerateBoundingBox(
			) const = 0;

		//! Should bounding spheres or boxes be tested against before doing a full out intersection test?
		/// \return TRUE if bounding boxes/sphere should be checked, FALSE otherwise
		virtual bool DoPreHitTest() const = 0; 

		//! Returns a uniformly random point on the surface.  Needed to sample luminary geometry surfaces
		//! This function guarantees that for the same prand, the same data is returned
		virtual void UniformRandomPoint(
			Point3* point,								///< [out] Point on the surface
			Vector3* normal,							///< [out] Normal at the point on the surface
			Point2* coord,								///< [out] Texture co-ordinate at the point on the surface
			const Point3& prand						///< [in] Variables used in point generation
			) const = 0;

		//! Gets the area of the geometry.  Needed for luminaries.
		/// \return Area of the geometry in Meters Squared
		virtual Scalar GetArea( ) const = 0;

		//! Computes surface derivatives at a point on the geometry.
		//! Used by specular manifold sampling for the Newton solver's
		//! constraint Jacobian.  All inputs and outputs are in object space.
		//!
		//! Default implementation returns invalid (valid=false).
		//! Geometry subclasses should override with analytical derivatives.
		/// \return SurfaceDerivatives struct with dpdu, dpdv, dndu, dndv
		virtual SurfaceDerivatives ComputeSurfaceDerivatives(
			const Point3& objSpacePoint,		///< [in] Point on the surface in object space
			const Vector3& objSpaceNormal		///< [in] Normal at the point in object space
			) const
		{
			return SurfaceDerivatives();
		}

		//! Smoothing-aware analytical surface query keyed on parametric (u, v).
		//!
		//! Returns position, unit normal, and tangent / normal partial
		//! derivatives of the underlying parameterised surface.  The
		//! `smoothing` parameter ∈ [0, 1] interpolates between the actual
		//! high-frequency-detailed surface (s = 0) and a Lipschitz-smooth
		//! reference surface (s = 1).  For tessellated analytical primitives
		//! (sphere, ellipsoid, ...) smoothing is a no-op — there is no
		//! high-frequency detail to attenuate.  For composite surfaces like
		//! `displaced_geometry`, smoothing scales the displacement amplitude:
		//! at s = 1 the surface collapses to its smooth base.
		//!
		//! Used by the SMS two-stage Newton solver: stage 1 walks on s = 1
		//! to find a seed in a C1-smooth landscape, stage 2 refines on
		//! s = 0.  See `docs/SMS_TWO_STAGE_SOLVER.md`.
		//!
		//! Default implementation returns false.  Pure triangle meshes
		//! loaded from .obj/.glTF have no smooth analytical surface.
		//!
		//! \return TRUE if analytical derivatives were produced; FALSE otherwise.
		virtual bool ComputeAnalyticalDerivatives(
			const Point2& uv,
			Scalar        smoothing,			///< [in] 0 = full detail, 1 = smooth base
			Point3&       outPosition,			///< [out] Object-space surface position
			Vector3&      outNormal,			///< [out] Object-space unit normal
			Vector3&      outDpdu,				///< [out] dP/du
			Vector3&      outDpdv,				///< [out] dP/dv
			Vector3&      outDndu,				///< [out] dN/du
			Vector3&      outDndv				///< [out] dN/dv
			) const
		{
			(void)uv; (void)smoothing;
			(void)outPosition; (void)outNormal;
			(void)outDpdu; (void)outDpdv;
			(void)outDndu; (void)outDndv;
			return false;
		}
	};
}


#endif
