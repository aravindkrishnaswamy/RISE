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
#include <vector>
#include <cstdint>
#include <cstring>

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

		//! Perturbs `vec` by polar angle `down`, drawing azimuth uniformly on
		//! the sub-arc satisfying `Dot(result, clipN) > 0` (DL-68 / DL-111):
		//!   halfArc = PI                            if cot(down)cot(phi) >= 1
		//!           = acos(-cot(down)cot(phi))      otherwise
		//! where `phi` is the angle between `vec` and `clipN`. Caller scales
		//! unclipped azimuth PDF `1/(2*PI)` to `1/(2*halfArc)` using `outHalfArc`.
		//! Mirrors `TranslucentSPFDetail::SampleClippedPhong` (keep arc math in sync).
		//!
		//! Preconditions: (1) `Dot(vec, clipN) >= 0`; (2) `clipN` unit (non-unit
		//! is normalized with warning; degenerate fails); (3) valid arc non-empty
		//! (`down` not placing the entire cone outside the half-space). On failure
		//! returns `vec` unperturbed with `outHalfArc = 0`.
		/// \return Perturbed vector satisfying the clip (or `vec` on failure)
		extern Vector3 PerturbClipped(
					const Vector3& vec,									///< [in] Vector to perturb (must satisfy Dot(vec,clipN) >= 0)
					const Scalar down,									///< [in] Perturbation amount in theta
					const Vector3& clipN,								///< [in] Half-space normal the result must satisfy
					const Scalar u,										///< [in] One canonical random number
					Scalar* outHalfArc = 0								///< [out] Optional: half-width of the valid azimuth arc
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

		//! Per-axis tangents dP/du (depends only on v) and dP/dv (depends only
		//! on u) in RISE's BilinearPatch corner layout: pts[0]=(0,0),
		//! pts[1]=(0,1), pts[2]=(1,0), pts[3]=(1,1).
		extern Vector3 BilinearPatchTangentU(
					const BilinearPatch& patch,							///< [in] The bilinear patch
					const Scalar v										///< [in] Evaluation parameter v
					);
		extern Vector3 BilinearPatchTangentV(
					const BilinearPatch& patch,							///< [in] The bilinear patch
					const Scalar u										///< [in] Evaluation parameter u
					);

		//! Constant mixed second partial d2P/dudv in RISE's BilinearPatch corner
		//! layout (d2P/du2 = d2P/dv2 = 0 identically; feeds
		//! `SurfaceCurvature::ShapeOperatorFromSecondDerivatives`).
		extern Vector3 BilinearPatchSecondDerivUV(
					const BilinearPatch& patch							///< [in] The bilinear patch
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

		//! Deterministic SplitMix64 uniform stream seeded from a canonical
		//! Point3's bits (DL-448), for pure-function rejection samplers.
		//! Nested rejection samplers (e.g. scaled ellipsoid) MUST pass distinct
		//! salts so inner and outer levels draw independent accept variates.
		struct PrandStream
		{
			std::uint64_t state;
			PrandStream( const Point3& prand, const std::uint64_t salt )
			{
				static_assert( sizeof( Scalar ) == sizeof( std::uint64_t ), "PrandStream seeds from 64-bit Scalar bit patterns" );
				std::uint64_t b[3];
				std::memcpy( &b[0], &prand.x, sizeof( std::uint64_t ) );
				std::memcpy( &b[1], &prand.y, sizeof( std::uint64_t ) );
				std::memcpy( &b[2], &prand.z, sizeof( std::uint64_t ) );
				state = b[0] ^ ( b[1] * 0x9E3779B97F4A7C15ULL ) ^ ( b[2] * 0xC2B2AE3D27D4EB4FULL ) ^ ( salt * 0xD6E8FEB86659FD93ULL );
				Next();	// mix the seed once before the first variate
			}
			Scalar Next()
			{
				std::uint64_t z = ( state += 0x9E3779B97F4A7C15ULL );
				z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
				z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBULL;
				z ^= ( z >> 31 );
				return Scalar( z >> 11 ) * Scalar( 1.0 / 9007199254740992.0 );
			}
		};

		//! Area-uniform bilinear patch sampling via shared midpoint quadrature:
		//! use AccumulateBilinearAreaCDF and BilinearCellToUV together so
		//! reported patch area and sampling density match exactly.

		//! Appends nCells*nCells running cumulative midpoint-quadrature areas
		//! onto `cdfOut` starting from `runningTotal`, returning this patch's area.
		extern Scalar AccumulateBilinearAreaCDF(
					const Point3& c00, const Point3& c10,
					const Point3& c11, const Point3& c01,
					const unsigned int nCells,							///< [in] Grid resolution per axis
					const Scalar runningTotal,							///< [in] Cumulative area before this patch
					std::vector<Scalar>& cdfOut							///< [in,out] Appended with nCells*nCells cumulative entries
					);

		//! Maps a cell index produced by searching a table from
		//! AccumulateBilinearAreaCDF, plus two uniform variates, to (u, v).
		extern void BilinearCellToUV(
					const unsigned int cellIndex,						///< [in] Index WITHIN a patch's block, in [0, nCells*nCells)
					const unsigned int nCells,
					const Scalar rU,									///< [in] Uniform variate for the cell's u extent
					const Scalar rV,									///< [in] Uniform variate for the cell's v extent
					Scalar& outU, Scalar& outV
					);

		//! Recovers (u, v) in [0, 1]^2 such that BilinearForward(..., u, v) ≈ P
		//! (Heckbert 1989 2x2 quadratic reduction along the best-conditioned axis
		//! pair). Returns true iff a root in [-eps, 1+eps]^2 achieves surface
		//! residual <= tolerance * max(extent, 1); picks minimum-residual branch.
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

		//! DL-20: second partial derivatives of a bicubic Bezier patch, for
		//! the shape operator (see SurfaceCurvature::ShapeOperatorFromSecondDerivatives).
		//! Same Bernstein-basis convention as EvaluateBezierPatchAt / the
		//! tangent functions above -- these are the analogous second
		//! derivatives of the SAME sum, obtained by differentiating the
		//! Bernstein basis twice (d2Pduu), once each way (d2Pduv), or twice
		//! in v (d2Pdvv).
		extern Vector3 BezierPatchSecondDerivUU(
					const BezierPatch& patch,							///< [in] The bezier patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);
		extern Vector3 BezierPatchSecondDerivUV(
					const BezierPatch& patch,							///< [in] The bezier patch
					const Scalar u,										///< [in] Evaluation parameter u
					const Scalar v										///< [in] Evaluation parameter v
					);
		extern Vector3 BezierPatchSecondDerivVV(
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


		//! Planar open-sheet certificate (DL-382): true iff every corner lies
		//! within `max(1e-9, 1e-6 * bbox diagonal)` of one plane and every
		//! non-degenerate triangle's winding normal points to the same side.
		//! Refuses closed oriented surfaces (even arbitrarily thin boxes, since
		//! total flux vanishes) and multi-plane slabs. `corner(t, k)` returns
		//! triangle `t`'s k-th vertex.
		template< class CornerFn >
		inline bool IsPlanarConsistentlyWoundSheet( const size_t nTris, CornerFn corner )
		{
			if( nTris == 0 ) {
				return false;
			}
			BoundingBox bb( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
			for( size_t t = 0; t < nTris; ++t ) {
				for( int k = 0; k < 3; ++k ) {
					bb.Include( corner( t, k ) );
				}
			}
			const Vector3 ext = Vector3Ops::mkVector3( bb.ur, bb.ll );
			const Scalar diag = Vector3Ops::Magnitude( ext );
			if( !( diag > 0 ) || !( diag < RISE_INFINITY ) ) {
				return false;
			}
			const Scalar eps = diag * 1e-6 > 1e-9 ? diag * 1e-6 : 1e-9;
			const Scalar degenerate = diag * diag * 1e-12;

			// Reference plane: the largest triangle's.
			size_t best = nTris;
			Scalar bestMag = 0;
			for( size_t t = 0; t < nTris; ++t ) {
				const Point3& a = corner( t, 0 );
				const Vector3 c = Vector3Ops::Cross(
					Vector3Ops::mkVector3( corner( t, 1 ), a ),
					Vector3Ops::mkVector3( corner( t, 2 ), a ) );
				const Scalar m = Vector3Ops::Magnitude( c );
				if( m > bestMag ) {
					bestMag = m;
					best = t;
				}
			}
			if( best == nTris || !( bestMag > degenerate ) ) {
				return false;
			}
			const Point3 p0 = corner( best, 0 );
			const Vector3 n = Vector3Ops::Normalize( Vector3Ops::Cross(
				Vector3Ops::mkVector3( corner( best, 1 ), p0 ),
				Vector3Ops::mkVector3( corner( best, 2 ), p0 ) ) );

			for( size_t t = 0; t < nTris; ++t ) {
				for( int k = 0; k < 3; ++k ) {
					const Scalar d = Vector3Ops::Dot( Vector3Ops::mkVector3( corner( t, k ), p0 ), n );
					if( !( d <= eps && d >= -eps ) ) {
						return false;
					}
				}
				const Point3& a = corner( t, 0 );
				const Vector3 c = Vector3Ops::Cross(
					Vector3Ops::mkVector3( corner( t, 1 ), a ),
					Vector3Ops::mkVector3( corner( t, 2 ), a ) );
				const Scalar m = Vector3Ops::Magnitude( c );
				if( m > degenerate && !( Vector3Ops::Dot( c, n ) > 0 ) ) {
					return false;
				}
			}
			return true;
		}

		//! Welds `points` to shared IDs per physical corner (DL-143): visits
		//! points in order and joins the nearest earlier representative within
		//! `eps = max(1e-9, 1e-6 * bbox diagonal, minEps)` (ties: lowest ID),
		//! else creates one. `outTransitive` is true iff every point is within
		//! `eps/2` of its rep and reps are > `1.5 eps` apart (unambiguous weld;
		//! distinct classes >= `eps/2` apart). Returns false on non-finite input.
		extern bool WeldVertexPositions(
			const std::vector<Point3>& points,
			std::vector<unsigned int>& outWeldedId,
			Scalar* outEps = nullptr,
			Scalar minEps = Scalar( 0 ),
			bool* outTransitive = nullptr
			);

		//! Result of IsEmbeddedOpenOrientedSurface.
		struct OpenSurfaceCertificate
		{
			bool	certified = false;	//!< every test passed
			bool	topologicalRefusal = false;	//!< refused at Step 2/2b/3 (depends only on post-weld vertex partition)
			bool	budgetRefusal = false;	//!< refused because Step 4's insertion or pair budget was exceeded (no geometric witness)
			std::size_t	refusalTriA = std::size_t( -1 );	//!< Step 1 (A == B) or Step 4 (A != B) failure witness
			std::size_t	refusalTriB = std::size_t( -1 );
			//! Signed volume of the triangles about their bounding-box
			//! centre (sum of (a-c).((b-c)x(d-c))/6).  For a closed
			//! outward-wound solid it is the enclosed volume; negative on a
			//! certified surface hints at an inside-out cracked solid
			//! (diagnostic only -- see docs/DEBT_LEDGER.md
			//! section 2.1).  Filled only when `certified`.
			Scalar	signedVolumeAboutCentre = 0;
			//! DL-623: certified lower bound on the Step-4 Euclidean
			//! separation across all triangle pairs at the certified
			//! positions (> eps when `certified`).
			Scalar	separationLowerBound = 0;
		};

		extern bool CheckOpenSurfaceStep4Pair(
			const Point3* Pi,
			const unsigned int* Ii,
			const Point3* Pj,
			const unsigned int* Ij,
			Scalar epsSq
			);

		//! Rigorous lower bound on lambda_min of symmetric 3x3 `G` (upper
		//! triangle read) via Cholesky of `G - (t + c)I` with backward-error
		//! margin `c = 8*eps*tr(G)` (Higham Thm 10.3; Rump 2006). Returns 0 if
		//! no positive bound is proved (not a PSD claim) or on non-finite input.
		extern Scalar SymmetricMinEigenvalueLowerBound3( const Scalar G[3][3] );

		//! General open-surface certificate for triangle meshes (DL-382; proof
		//! in docs/DEBT_LEDGER.md §2). Conservative; certified iff post-weld:
		//!  1. distinct welded corners per triangle and min altitude > eps;
		//!  2. manifold edges (1 or 2 uses, oppositely oriented) and single component;
		//!  3. free-face peeling removes every triangle (H_2 = 0);
		//!  4. triangles meet only in shared welded faces with margin `eps`.
		//! Authored normal agreement with winding is checked by the caller.
		extern OpenSurfaceCertificate IsEmbeddedOpenOrientedSurface(
			const std::vector<Point3>& corners,
			std::vector<unsigned int>* outWeldedId = nullptr,
			Scalar minEps = Scalar( 0 ),
			const std::vector<unsigned int>* preWeldedId = nullptr,
			Scalar preWeldEps = Scalar( 0 ),
			bool preWeldTopologyVerified = false
			);

		//! Analytic watertight / open-sheet certification for a patch set
		//! (DL-616). At most one of `outCertifiedWatertight` (connected,
		//! outward-wound embedded closed 2-manifold) or `outProvablyOpenSheet`
		//! (connected embedded 2-manifold with boundary) is set, proved from
		//! exact bitwise-reversed seams (no tolerance welding; at most one
		//! collapsed pole side per patch) plus sub-patch grid injectivity,
		//! corner plane separation, and recursive box disjointness within the
		//! work/sub-patch budget. `outNegativeOpenSignedVolume` flags negative
		//! signed volume on an open sheet (inward-winding warning).
		struct PatchSetCertDiagnostics
		{
			std::size_t		workBudget = 0;	//!< [in] 0 = the default budget
			const char*		reason = "";	//!< [out]
			unsigned int	grid = 0;		//!< [out] 4 or 8 (0: refused before subdividing)
			std::size_t		workUsed = 0;	//!< [out]
		};

		extern void CertifyBilinearPatchSet(
			const std::vector<BilinearPatch>& patches,
			bool& outCertifiedWatertight,
			bool& outProvablyOpenSheet,
			bool& outNegativeOpenSignedVolume,
			PatchSetCertDiagnostics* diag = nullptr
			);

		//! Topological openness of a patch set from exact seam identity and
		//! vertex/pole-fan links alone (no embedding proof; DL-617/624):
		//! +1 free boundary, 0 closed manifold, -1 undecidable.
		extern int BezierPatchSetOpenness( const std::vector<BezierPatch>& patches );
		extern int BilinearPatchSetOpenness( const std::vector<BilinearPatch>& patches );

		//! Conservative topological openness of a welded triangle mesh (no
		//! embedding proof; DL-621): +1 free boundary with free-face peeling
		//! removing all triangles AND substantial boundary cap area (>= 2% of
		//! mesh area); 0 manifold closed mesh excluding flat zero-volume
		//! pillows (coplanar-opposed fold with `2|V|/A <= weldEps`); -1
		//! undecidable (keeps composites on solid interval algebra).
		extern int TriangleMeshOpenness( const std::vector<Point3>& corners );

		//! DL-616: a process-wide counter bumped whenever any patch set's
		//! certificate may have changed (a patch added, a set re-prepared),
		//! so a composite's cached certificate can tell it is stale.
		extern unsigned int PatchCertificateEpoch();
		extern void BumpPatchCertificateEpoch();

		//! DL-616: as `CertifyBilinearPatchSet`, for bicubic Bezier patches
		//! (a bilinear patch is certified as its exact degree elevation).
		extern void CertifyBezierPatchSet(
			const std::vector<BezierPatch>& patches,
			bool& outCertifiedWatertight,
			bool& outProvablyOpenSheet,
			bool& outNegativeOpenSignedVolume,
			PatchSetCertDiagnostics* diag = nullptr
			);
	}
}


#endif

