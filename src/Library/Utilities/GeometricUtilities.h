//////////////////////////////////////////////////////////////////////
//
//  GeometricUtilities.h - Defines a few useful geometric utilities
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 8, 2001
//  Tabs: 4
//  Comments:  Influence by ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GEOMETRIC_UTILITIES_
#define GEOMETRIC_UTILITIES_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Plane.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/BoundingBox.h"
#include "../Polygon.h"

namespace RISE
{
	namespace GeometricUtilities
	{
		//! Maps a point to a disk
		/// \return The point on the disk
		extern Point2	PointOnDisk(
					Scalar R,											///< [in] Radius of the disk	
					const Point2& uv									///< [in] Point in a canonical space (box)
					);

		//! Creates a vector with a density proportional to cos(theta)  (lambertian)
		/// \return Vector with cosine distribution
		extern Vector3 CreateDiffuseVector(
					const OrthonormalBasis3D& uvw,						///< [in] Orthonormal Basis to generate point about
					const Point2& p										///< [in] Two canonical random numbers
					);

		//! Perturbs the given vector
		/// \return Perturbed vector
		extern Vector3 Perturb( 
					const Vector3& vec,									///< [in] Vector to perturb
					const Scalar down,									///< [in] Perturbation amount in theta
					const Scalar around									///< [in] Perturbation amount in phi
					);

		//! Generates a random point on a sphere
		/// \return Point on sphere
		extern Point3 PointOnSphere( 
					const Point3& ptCenter,								///< [in] Center of the sphere
					const Scalar radius,								///< [in] Radius of the sphere
					const Point2& coord									///< [in] Two canonical random numbers
					);

		//! Generates a random point on a sphere
		/// \return Point on sphere
		extern Point3 PointOnEllipsoid( 
					const Point3& ptCenter,								///< [in] Center of the ellipsoid
					const Vector3& r,									///< [in] Radii of the ellipsoid
					const Point2& coord									///< [in] Two canonical random numbers
					);

		//! Creates a point based on the given spherical angles and an ONB
		/// \return Vector representing the spherical co-ordinates 
		extern Point3 CreatePoint3FromSphericalONB( 
					const OrthonormalBasis3D& onb,						///< [in] Orthonormal basis to serve as orientation
					const Scalar phi,									///< [in] Spherical co-ordinate phi
					const Scalar theta									///< [in] Spherical co-ordinate theta
					);

		//! Creates a point from a given theta and phi using a canonical ONB
		/// \return Vector representing the spherical co-ordinates in a canonical space
		extern Point3 CreatePoint3FromSpherical( 
					const Scalar phi,									///< [in] Spherical co-ordinate phi
					const Scalar theta									///< [in] Spherical co-ordinate theta
					);

		//! Computes the angles theta and phi on the sphere centered around the origin, corresponding to
		//! the given Point3
		/// \return TRUE if successful, FALSE otherwise
		extern bool GetSphericalFromPoint3( 
					const Point3& pt,									///< [in] Point to convert
					Scalar& phi,										///< [out] phi in spherical co-ordinates
					Scalar& theta										///< [out] theta in spherical co-oridnates
					);

		//! Computes the angles thetha and phi on the sphere cantered around the origin, correspinding to 
		//! the given ONB and the given Point3
		/// \return TRUE if successful, FALSE otherwise
		extern bool GetSphericalFromPoint3ONB(
					const Point3& pt,									///< [in] Point to convert
					const OrthonormalBasis3D& onb,						///< [in] Orthonormal basis to serve as orientation
					Scalar& phi,										///< [out] phi in spherical co-ordinates
					Scalar& theta										///< [out] theta in spherical co-oridnates
					);

		//! Creates a vector with a density proportional to cos^n (theta) distributed around a vector
		//! Don't know this works, I smell bogosity
		/// \return A vector with a density proportional to cos^n (theta) distributed around a vector
		extern Vector3 CreatePhongVector( 
					const Vector3& v,									///< [in] Vector to generate around
					const Point2& p,									///< [in] Two canonical random numbers
					const Scalar n										///< [in] Phong exponent
					);

		//! Creates a vector with a density proportional to cos^n (theta)
		/// \return A vector with a density proportional to cos^n (theta)
		extern Vector3 CreatePhongVector(
					const OrthonormalBasis3D& onb,						///< [in] Orthonormal basis to serve as orientation
					const Point2& p,									///< [in] Two canonical random numbers
					const Scalar n										///< [in] Phong exponent
					);

		//! Creates a vector with a density proportional to cos^n (theta/2)
		/// \return A vector with a density proportional to cos^n (theta)
		extern Vector3 CreateHalfPhongVector(
					const OrthonormalBasis3D& onb,						///< [in] Orthonormal basis to serve as orientation
					const Point2& p,									///< [in] Two canonical random numbers
					const Scalar n										///< [in] Phong exponent
					);

		//! Creates a vector with a wacky density (see the Ashikhmin / Shirley paper)
		//! BIG FAT WARNING!  Don't use this unless you really really know what you are doing
		/// \return A vector with the ashikmin / shirley Anisotropic phong density
		extern Vector3 CreateAshikminShirleyAnisotropicPhongHalfVector( 
					const OrthonormalBasis3D& onb,						///< [in] Orthonormal basis to serve as orientation
					const Point2& p,									///< [in] Two canonical random numbers
					const Scalar Nu,									///< [in] Phong exponent 1
					const Scalar Nv										///< [in] Phong exponent 2
					);

		//! Computes the area of a patch on a sphere
		/// \return The area of the spherical patch
		extern Scalar SphericalPatchArea( 
					const Scalar& theta_begin,							///< [in] Where the patch begins in theta
					const Scalar& theta_end,							///< [in] Where the patch ends in theta
					const Scalar& phi_begin,							///< [in] Where the patch begins in phi
					const Scalar& phi_end,								///< [in] Where the patch ends in phi
					const Scalar& radius								///< [in] Radius of the sphere
					);

		//! Determines if a point is inside a sphere
		/// \return TRUE if the point lies inside the sphere. FALSE otherwise
		extern bool IsPointInsideSphere( 
					const Point3& point,								///< [in] Point to be tested
					const Scalar radius,								///< [in] Radius of the sphere
					const Point3& center								///< [in] Center of the sphere
					);

		//! Determines if a point is inside a box
		/// \return TRUE if the point lies inside the box, FALSE otherwise
		extern bool IsPointInsideBox( 
					const Point3& point,								///< [in] Point to be tested
					const Point3& ptLowerLeft,							///< [in] Lower left hand point of the box
					const Point3& ptUpperRight							///< [in] Upper right hand point of the box
					);

		//! Computes texture co-ordinates for a sphere
		extern void SphereTextureCoord( 
					const Vector3& vUp,									///< [in] The up vector, these vectors determine how the sphere is wrapped
					const Vector3& vForward,							///< [in] The forward vector
					const Point3& ptPoint,								///< [in] Point to generate co-ordinate for
					Point2& uv											///< [out] The generated co-ordinate
					);

		//! Computes texture co-ordinates for a sphere
		extern void SphereTextureCoord( 
					const Vector3& vUp,									///< [in] The up vector, these vectors determine how the sphere is wrapped
					const Vector3& vForward,							///< [in] The forward vector
					const Vector3& vNormal,								///< [in] The normal to generate the texture co-ordinate from
					Point2& uv											///< [out] The generated co-ordinate
					);

		//! Computes texture co-ordinates for a torus.
		//! The (u, v) returned matches TorusGeometry::TessellateToMesh's
		//! parameterisation: u = ring-angle/2π, v = tube-angle/2π.
		//! vUp / vForward / vNormal are unused (kept for ABI compat) — the
		//! parameterisation is derived from the position alone.
		extern void TorusTextureCoord(
					const Vector3& vUp,									///< [in] (unused — see body comment)
					const Vector3& vForward, 							///< [in] (unused)
					const Point3& ptPoint, 								///< [in] Point on the torus surface (object space)
					const Vector3& vNormal,								///< [in] (unused)
					Point2& uv,											///< [out] The generated co-ordinate
					const Scalar dMajorRadius,							///< [in] Ring radius R
					const Scalar dMinorRadius							///< [in] Tube radius r
					);

		//! Generates a point on a cylinder given two numbers in [0,1]
		extern void PointOnCylinder( 
					const Point2& can,									///< [in] Two canonical random numbers
					const int chAxis,									///< [in] Which axis does the cylinder lie on (x|y|z)
					const Scalar dRadius,								///< [in] Radius of the sphere
					const Scalar dAxisMin,								///< [in] Min value of the cylinder on the axis it lies on.  Min+Max equals the Cylinder's height
					const Scalar dAxisMax,								///< [in] Max value of the cylinder on the axis it lies on
					Point3& point										///< [out] The generated Point
					);

		//! Computes texture co-ordinates for a cylinder given a point on the cylinder
		extern void CylinderTextureCoord( 
					const Point3 point,									///< [in] Point to generate co-ordinate for
					const int chAxis,									///< [in] Which axis does the cylinder lie on (x|y|z)
					const Scalar dOVRadius,								///< [in] Inverse of the cylinder's radius
					const Scalar dAxisMin, 								///< [in] Min value of the cylinder on the axis it lies on.  Min+Max equals the Cylinder's height
					const Scalar dAxisMax,								///< [in] Max value of the cylinder on the axis it lies on
					Point2& coord										///< [out] The generated co-ordinate
					);

		//! Computes the normal of the cylinder at the given point
		extern void CylinderNormal(
					const Point3 point,									///< [in] Point to compute normal for
					const int chAxis,									///< [in] Which axis does the cylinder lie on (x|y|z)
					Vector3& normal										///< [out] The generated normal
					);

		//! Generates a point in a triangle
		extern void PointOnTriangle(
					Vertex* point,										///< [out] Resultant point on triangle
					Normal* normal,										///< [out] Resultant normal on triangle
					TexCoord* coord,									///< [out] Resultant texture coord on triangle
					const Triangle& t,									///< [in] The incoming triangle
					const Scalar a_,									///< [in] Distance in one direction
					const Scalar b_										///< [in] Distance in other direction
					);

		//! Generates a point on a triangle where each element of
		//! of the triangle is a pointer to the value
		extern void PointOnTriangle( 
					Vertex* point,										///< [out] Resultant point on triangle
					Normal* normal,										///< [out] Resultant normal on triangle
					TexCoord* coord,									///< [out] Resultant texture coord on triangle
					const PointerTriangle& t,							///< [in] The incoming pointer triangle
					const Scalar a_,									///< [in] Distance in one direction
					const Scalar b_										///< [in] Distance in other direction
					);

		//! Generates the bounding box of the given bezier patch
		extern BoundingBox BezierPatchBoundingBox(
					const BezierPatch& patch							///< [in] The bezier patch
					);

		//! Generates the bounding box of the given bilinear patch
		extern BoundingBox BilinearPatchBoundingBox(
					const BilinearPatch& patch							///< [in] The bilinear patch
					);

		//! Evaluates a bilinear patch for the given u and v
		extern Point3 EvaluateBilinearPatchAt(
					const BilinearPatch& patch,							///< [in] The bilinear patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);

		//! Finds the normal of a bilinear patch at the given co-ordinates
		extern Vector3 BilinearPatchNormalAt(
					const BilinearPatch& patch,							///< [in] The bilinear patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);

		// =========================================================================
		// Convention-agnostic bilinear-surface utilities.
		//
		// These take 4 corners in the canonical (c00, c10, c11, c01) layout —
		// i.e. (u, v) = (0, 0), (1, 0), (1, 1), (0, 1).  Callers using a
		// different corner-to-(u, v) mapping (e.g. RISE's BilinearPatch struct
		// uses pts[0]→(0,0), pts[1]→(0,1), pts[2]→(1,0), pts[3]→(1,1)) must
		// reorder before calling.
		//
		// Surface:    pos(u, v) = c00·(1-u)(1-v) + c10·u(1-v) + c11·uv + c01·(1-u)v
		// Equivalent: pos(u, v) = A + B·u + C·v + D·u·v
		//   A = c00,  B = c10 - c00,  C = c01 - c00,
		//   D = c11 - c10 - c01 + c00   (the "saddle" term; zero for planar
		//                                 parallelograms)
		// =========================================================================

		//! Forward formula: pos(u, v) on the bilinear surface.
		extern Point3 BilinearForward(
					const Point3& c00,									///< [in] corner at (u=0, v=0)
					const Point3& c10,									///< [in] corner at (u=1, v=0)
					const Point3& c11,									///< [in] corner at (u=1, v=1)
					const Point3& c01,									///< [in] corner at (u=0, v=1)
					const Scalar u,
					const Scalar v
					);

		//! Partial derivative dpos/du at (u, v).
		extern Vector3 BilinearTangentU(
					const Point3& c00, const Point3& c10,
					const Point3& c11, const Point3& c01,
					const Scalar v
					);

		//! Partial derivative dpos/dv at (u, v).
		extern Vector3 BilinearTangentV(
					const Point3& c00, const Point3& c10,
					const Point3& c11, const Point3& c01,
					const Scalar u
					);

		//! Inverse: given a 3D point P claimed to lie on the bilinear surface,
		//! recover (u, v) such that BilinearForward(...,u,v) ≈ P.
		//!
		//! Returns true iff a valid (u, v) ∈ [-eps, 1+eps]² was found AND the
		//! surface residual at the recovered (u, v) is below `tolerance`.
		//! Uses the 2x2 quadratic-in-v reduction described in
		//! Heckbert "Tutorial on Texture Mapping" (1989) and used in
		//! RayBilinearPatchIntersection.cpp; picks the axis pair most
		//! orthogonal to the patch normal at the centre for conditioning.
		//!
		//! For PARALLELOGRAM quads (D = 0) the equation degenerates to linear;
		//! this routine handles that case without iteration.  For TWISTED quads
		//! with multiple branches in [0, 1]² the candidate with smallest
		//! surface residual is returned.
		extern bool BilinearInverse(
					const Point3& c00,									///< [in] corner at (u=0, v=0)
					const Point3& c10,									///< [in] corner at (u=1, v=0)
					const Point3& c11,									///< [in] corner at (u=1, v=1)
					const Point3& c01,									///< [in] corner at (u=0, v=1)
					const Point3& P,									///< [in] query point on surface
					Scalar& u,											///< [out] recovered u in [0, 1]
					Scalar& v,											///< [out] recovered v in [0, 1]
					const Scalar tolerance = 1e-5						///< [in] max surface residual / patch-extent
					);

		//! Evaluates a bicubic Bezier patch P(u,v) using the Bernstein basis.
		//! Convention: P(u,v) = sum_{i,j} B_i(u) * B_j(v) * patch.c[i].pts[j]
		//! where i indexes the curve (u direction) and j indexes the control
		//! point along the curve (v direction).
		extern Point3 EvaluateBezierPatchAt(
					const BezierPatch& patch,							///< [in] The bezier patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);

		//! Partial derivative dP/du of a bicubic Bezier patch.
		extern Vector3 BezierPatchTangentU(
					const BezierPatch& patch,							///< [in] The bezier patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);

		//! Partial derivative dP/dv of a bicubic Bezier patch.
		extern Vector3 BezierPatchTangentV(
					const BezierPatch& patch,							///< [in] The bezier patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);

		//! Surface normal = dP/du x dP/dv (not normalized).  Caller should
		//! normalize.  At degenerate points (coincident tangents) may return
		//! the zero vector.
		extern Vector3 BezierPatchNormalAt(
					const BezierPatch& patch,							///< [in] The bezier patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);

		//! Tells us on which side of the plane a triangle lies
		//! 0 - "left" side, ie. side opposite of normal
		//! 1 - "right" side, ie. side of the normal
		//! 2 - it straddles the plane or is on the plane
		extern char WhichSideOfPlane( 
					const Plane& p,										///< [in] The plane
					const PointerTriangle& t							///< [in] The triangle to check
					);

		//! Tells us on which side of the plane a triangle lies
		//! 0 - "left" side, ie. side opposite of normal
		//! 1 - "right" side, ie. side of the normal
		//! 2 - it straddles the plane or is on the plane
		extern char WhichSideOfPlane( 
					const Plane& p,										///< [in] The plane
					const Triangle& t									///< [in] The triangle to check
					);

		//! Tells us on which side of the plane a bounding box lies
		//! 0 - "left" side, ie. side opposite of normal
		//! 1 - "right" side, ie. side of the normal
		//! 2 - it straddles the plane or is on the plane
		extern char WhichSideOfPlane( 
					const Plane& p,										///< [in] The plane
					const BoundingBox& bb								///< [in] The bounding box to check
					);

	}
}


#endif

