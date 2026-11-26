//////////////////////////////////////////////////////////////////////
//
//  TorusGeometry.h - Definition of a torus
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 12, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TORUS_GEOMETRY_
#define TORUS_GEOMETRY_

#include "Geometry.h"

#include <cmath>			// std::fabs / std::sqrt (SelfHitRootFloor)

namespace RISE
{
	namespace Implementation
	{
		class TorusGeometry : public Geometry
		{
		protected:
			Scalar			m_dMajorRadius;
			Scalar			m_dMinorRadius;
			Scalar			m_p0;
			Scalar			m_p1;
			Scalar			m_sqrP0;
			Scalar			m_sqrP1;

			virtual ~TorusGeometry( );

		public:
			TorusGeometry( const Scalar dMajorRadius, const Scalar dMinorRadius );

			// Tessellates the torus to a triangle mesh with (detail+1) x (detail+1) vertices.
			// Grid spans azimuth (u, around Y axis) x tube angle (v, around the tube cross-section).
			// Both seams (u=0/u=1 and v=0/v=1) are duplicated for clean displacement.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override; 
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest( ) const override { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea( ) const override;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			//! IGeometry::DistanceToSurface -- EXACT outside, using the engine's
			//! own Y-axis torus form; 0 inside the tube
			//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2).
			bool DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const override;

			//! IGeometry::SignedDistanceLower -- EXACT on BOTH sides: the
			//! torus field `length(length(p.xz) - R, p.y) - r` is the true
			//! signed distance to the tube everywhere.
			bool SignedDistanceLower( const Point3& ptObject, const Scalar maxDistObject,
				Scalar& outSigned, bool& outExact ) const override;

			//! IGeometry::SelfHitRootFloor -- the torus's real gate is NOT the
			//! quartic solver's `s[i] > NEARZERO` root test (which the generic
			//! default already over-states).  It is the QUARTIC DEFLATION test in
			//! RayTorusIntersection: when the constant coefficient
			//!   C[4] = u^2 - q*t = F(O),   F(P) = (|P|^2 + R^2 - r^2)^2 - 4R^2(Px^2 + Pz^2)
			//! lands within a RELATIVE band of the quartic's own scale,
			//!   |C[4]| <= quartScale * 1e-10,   quartScale = |u^2| + |q*t|,
			//! the routine concludes "origin on surface", DROPS the near root and
			//! solves the deflated cubic.  A probe that stands off by less than the
			//! width of that band is inside it: its genuine near root is discarded
			//! and the FAR wall is reported instead -- which, for a torus CSG
			//! operand, is exactly the exit-face-probe miss
			//! `CSGObject::AdoptCsgExitFacePayloadViaProbe` exists to avoid.  The
			//! band is ORDERS wider than the generic default the torus used to
			//! inherit: BoxGeometryTest's bisection measures the real gate at
			//! 1.20e-9 (R = 4, r = 1, normal incidence, unit scale) and 1.20e-6 at
			//! 1000x, against a default of 9.0e-12 / 9.0e-9 -- a 133x
			//! UNDER-statement that scales with the shape, i.e. a silent,
			//! size-dependent P1.  (The adversarial review that found it reported
			//! 3.33e-9 / 3.33e-6 from its own probe geometry; same defect, a
			//! different point on the tube.)
			//!
			//! Width of the band along `localDir`, to first order.  With
			//!   u = |P|^2 + R^2 - r^2,   grad F = 4u*P - 8R^2 (Px, 0, Pz)
			//! (the tube circles the local Y axis -- RayTorusIntersection's
			//! `t = origin.x^2 + origin.z^2` fixes that), F(P + d*dir) ~=
			//! F(P) + d*(grad F . dir), and F(P) = 0 for an on-surface P.  So the
			//! deflation keeps firing while
			//!   d * |grad F . dir|  <=  1e-10 * quartScale
			//! and the smallest standoff that clears it is
			//!   1e-10 * quartScale / |grad F . dir|,
			//! doubled here for headroom (the same 2x every other override and the
			//! probe itself carry).  quartScale is computed EXACTLY as the routine
			//! computes it (u^2 + 4R^2(Px^2+Pz^2), i.e. q = 4R^2 with the solver's
			//! g == 1 for a unit direction), so the two cannot drift.
			//!
			//! A GRAZING query (`localDir` nearly tangent, |grad F . dir| -> 0)
			//! would send this to infinity, so the directional derivative is
			//! clamped at 5 % of |grad F| -- the same 1/20 grazing floor the probe
			//! applies to its own rate.  Steeper-than-that exits miss the probe and
			//! take its graceful entry-payload fallback, the marginal outcome the
			//! probe already accepts.
			//!
			//! Seeded with the generic `NEARZERO * (1 + |o|_1 + R + r)` so a
			//! degenerate torus (r or R ~ 0, where grad F vanishes with quartScale
			//! and the ratio is 0/0) still reports a positive, scale-relative
			//! floor rather than zero.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override
			{
				(void)localNormal;

				const Scalar R = m_p0;
				const Scalar r = m_p1;
				const Scalar generic = NEARZERO * ( Scalar(1) +
					std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z ) +
					std::fabs( R ) + std::fabs( r ) );

				// The solver's own quantities, verbatim (see RayTorusIntersection).
				const Scalar sqrLen  = localOrigin.x*localOrigin.x + localOrigin.y*localOrigin.y + localOrigin.z*localOrigin.z;
				const Scalar radial2 = localOrigin.x*localOrigin.x + localOrigin.z*localOrigin.z;
				const Scalar u          = sqrLen + m_sqrP0 - m_sqrP1;
				const Scalar quartScale = std::fabs( u*u ) + std::fabs( Scalar(4) * m_sqrP0 * radial2 );

				const Scalar gx = Scalar(4)*u*localOrigin.x - Scalar(8)*m_sqrP0*localOrigin.x;
				const Scalar gy = Scalar(4)*u*localOrigin.y;
				const Scalar gz = Scalar(4)*u*localOrigin.z - Scalar(8)*m_sqrP0*localOrigin.z;
				const Scalar gradMag = std::sqrt( gx*gx + gy*gy + gz*gz );

				Scalar dirDeriv = std::fabs( gx*localDir.x + gy*localDir.y + gz*localDir.z );
				const Scalar grazeClamp = Scalar(0.05) * gradMag;
				if( dirDeriv < grazeClamp ) {
					dirDeriv = grazeClamp;
				}
				if( !( dirDeriv > Scalar(0) ) ) {
					return generic;			// degenerate torus: no usable gradient
				}

				const Scalar deflationBand = Scalar(2) * Scalar(1e-10) * quartScale / dirDeriv;
				return ( deflationBand > generic ) ? deflationBand : generic;
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData( ) override;
		};
	}
}

#endif
