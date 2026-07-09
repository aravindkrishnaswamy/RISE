//////////////////////////////////////////////////////////////////////
//
//  CylinderGeometry.h - Definition of a cylinder
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CYLINDER_GEOMETRY_
#define CYLINDER_GEOMETRY_

#include "Geometry.h"

namespace RISE
{
	namespace Implementation
	{
		class CylinderGeometry : public Geometry
		{
		protected:
			const int		m_chAxis;
			Scalar			m_dRadius;
			Scalar			m_dOVRadius;
			Scalar			m_dHeight;
			Scalar			m_dAxisMin;
			Scalar			m_dAxisMax;
			const bool		m_bCapped;		///< TRUE: closed solid (two end-cap disks); FALSE: open tube (side only)

			virtual ~CylinderGeometry( );

			// Surface-region codes returned by IntersectCappedSolid / consumed by
			// FillSurfaceNormalUV.  Only meaningful when m_bCapped is TRUE.
			enum CylSurface { SURF_SIDE = 0, SURF_CAPMIN = 1, SURF_CAPMAX = 2 };

			//! Closed-solid ray intersection (side wall + two cap disks).  Returns the
			//! nearest boundary crossing ahead of the ray (tNear/surfNear) and, when the
			//! ray enters from outside, the exit crossing (tFar/surfFar).  When the ray
			//! origin is inside the solid there is a single crossing: tFar is set to 0 and
			//! surfFar mirrors surfNear (matching the open-tube "started inside" convention).
			bool IntersectCappedSolid( const Ray& ray, Scalar& tNear, int& surfNear, Scalar& tFar, int& surfFar ) const;

			//! Fills the outward normal (and, if coord != 0, the UV) for a point known to lie
			//! on surface region `surf` of the capped cylinder.
			void FillSurfaceNormalUV( const Point3& pt, int surf, Vector3& normal, Point2* coord ) const;

		public:
			CylinderGeometry( const int chAxis, const Scalar dRadius, const Scalar dHeight, const bool capped );

			// Tessellates the cylinder side to (detail+1) x (detail+1) vertices (duplicating the
			// u=0/u=1 seam).  When m_bCapped, appends a triangle fan for each end cap so the
			// tessellation is a closed solid matching the analytic intersection.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea( ) const;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
