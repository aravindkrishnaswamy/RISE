//////////////////////////////////////////////////////////////////////
//
//  BDPTUtilities.h - Geometric and PDF measure conversion utilities
//  for BDPT.
//
//  BDPT requires frequent conversions between solid angle and area
//  PDF measures.  The relationship is (Veach thesis eq. 8.8):
//    pdfArea = pdfSolidAngle * |cos(theta)| / dist^2
//  where theta is the angle at the receiving vertex and dist is the
//  distance between vertices.
//
//  The geometric term G(x<->y) is the product of cosines at both
//  endpoints divided by the squared distance.  It appears in every
//  BDPT connection evaluation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_UTILITIES_
#define BDPT_UTILITIES_

#include "Math3D/Math3D.h"
#include "../Shaders/BDPTVertex.h"

namespace RISE
{
	namespace BDPTUtilities
	{
		//! Convert a solid-angle PDF at `from` to the canonical
		//! measure stored at `to`.
		//!
		//! For ordinary surface / medium / camera destinations this
		//! is the standard SA -> area Jacobian (cos / dist^2 for
		//! surface, sigma_t / dist^2 for medium, 1/dist^2 for
		//! camera).  For infinite-area / environment-light
		//! destinations the conversion is the IDENTITY — the env
		//! vertex's pdfFwd / pdfRev are stored in solid-angle
		//! measure (sr^-1) directly, matching PBRT-v4 §15.5.2
		//! `ConvertDensity`.  This is what closes the disc-area
		//! MIS gap on env-IBL paths (see IMPROVEMENTS.md #12 and
		//! BDPTVertex.h's env-vertex semantics block).
		/// \return PDF in the destination's canonical measure.
		inline Scalar ConvertDensity(
			const Scalar pdfSolidAngle,				///< [in] PDF in SA measure at `from` [1/sr]
			const BDPTVertex& from,					///< [in] Source vertex (where pdfSolidAngle is parameterised)
			const BDPTVertex& to					///< [in] Destination vertex (whose measure we convert TO)
			)
		{
			if( to.IsInfiniteLight() ) {
				// Env vertex stores SA-measure pdfs directly — skip
				// the area-Jacobian to match PBRT-v4's first-class
				// infinite-light vertex treatment.
				return pdfSolidAngle;
			}
			const Vector3 d = Vector3Ops::mkVector3( to.position, from.position );
			const Scalar distSq = Vector3Ops::SquaredModulus( d );
			if( distSq < 1e-20 ) {
				return 0;
			}
			if( to.type == BDPTVertex::MEDIUM ) {
				// Medium destination: sigma_t replaces |cos|.
				return pdfSolidAngle * to.sigma_t_scalar / distSq;
			}
			if( to.type == BDPTVertex::CAMERA ) {
				// Camera destination: implicit cos=1 (camera-pinhole
				// directional reparameterisation handled upstream).
				return pdfSolidAngle / distSq;
			}
			const Scalar invDist = Scalar( 1 ) / sqrt( distSq );
			const Vector3 dHat = d * invDist;
			const Scalar absCos = fabs( Vector3Ops::Dot( to.geomNormal, dHat ) );
			return pdfSolidAngle * absCos / distSq;
		}

		//! Computes the geometric term G(x <-> y) between two surface points.
		//! G = |cos(theta_x)| * |cos(theta_y)| / ||x - y||^2
		//! where theta_x is the angle between normal at x and the direction toward y,
		//! and theta_y is the angle between normal at y and the direction toward x.
		/// \return The geometric term value
		inline Scalar GeometricTerm(
			const Point3& p1,						///< [in] Position of first vertex
			const Vector3& n1,						///< [in] Normal at first vertex
			const Point3& p2,						///< [in] Position of second vertex
			const Vector3& n2						///< [in] Normal at second vertex
			)
		{
			Vector3 d = Vector3Ops::mkVector3( p2, p1 );
			const Scalar distSq = Vector3Ops::SquaredModulus( d );

			if( distSq < 1e-20 ) {
				return 0;
			}

			const Scalar invDist = 1.0 / sqrt( distSq );
			d = d * invDist;

			const Scalar cosTheta1 = fabs( Vector3Ops::Dot( n1, d ) );
			const Scalar cosTheta2 = fabs( Vector3Ops::Dot( n2, -d ) );

			return (cosTheta1 * cosTheta2) / distSq;
		}

		//! Converts a PDF from solid angle measure to area measure.
		//! pdfArea = pdfSolidAngle * |cos(theta)| / dist^2
		/// \return PDF in area measure
		inline Scalar SolidAngleToArea(
			const Scalar pdfSolidAngle,				///< [in] PDF in solid angle measure [1/sr]
			const Scalar absCosTheta,				///< [in] |cos(theta)| at the receiving vertex
			const Scalar distSquared				///< [in] Squared distance between the two vertices
			)
		{
			if( distSquared < 1e-20 ) {
				return 0;
			}
			return pdfSolidAngle * absCosTheta / distSquared;
		}

		//! Converts a PDF from area measure to solid angle measure.
		//! pdfSolidAngle = pdfArea * dist^2 / |cos(theta)|
		/// \return PDF in solid angle measure
		inline Scalar AreaToSolidAngle(
			const Scalar pdfArea,					///< [in] PDF in area measure
			const Scalar absCosTheta,				///< [in] |cos(theta)| at the receiving vertex
			const Scalar distSquared				///< [in] Squared distance between the two vertices
			)
		{
			if( absCosTheta < 1e-20 ) {
				return 0;
			}
			return pdfArea * distSquared / absCosTheta;
		}

		//////////////////////////////////////////////////////////////////
		// Medium vertex utilities
		//
		// For medium scatter vertices (no surface orientation), the
		// geometric coupling and PDF measure conversions differ from
		// surface vertices:
		//
		//   Geometric coupling term G(x <-> y):
		//     surface <-> surface:  |cos_x| * |cos_y| / dist^2
		//     surface <-> medium:   |cos_surface| / dist^2
		//     medium  <-> medium:   1 / dist^2
		//
		//   Medium vertices have no surface normal, so no cosine factor
		//   appears.  The 1/dist^2 term is the inverse-square law for
		//   point-to-point radiance transport in free space.
		//
		//   For the solid-angle-to-area PDF conversion at medium
		//   vertices, sigma_t replaces |cos(theta)| (Veach thesis
		//   Ch. 11; PBRT v4 Section 16.3):
		//     pdfArea = pdfSolidAngle * sigma_t / dist^2
		//////////////////////////////////////////////////////////////////

		//! Converts a solid angle PDF to area measure at a medium vertex.
		//! sigma_t at the scatter point replaces |cos(theta)| because the
		//! free-flight sampling probability density is proportional to
		//! sigma_t, while surface "acceptance" is proportional to the
		//! projected area (|cos|/dist^2).
		/// \return PDF in generalized area measure
		inline Scalar SolidAngleToAreaMedium(
			const Scalar pdfSolidAngle,				///< [in] PDF in solid angle measure [1/sr]
			const Scalar sigma_t,					///< [in] Extinction coefficient at scatter point
			const Scalar distSquared				///< [in] Squared distance between vertices
			)
		{
			if( distSquared < 1e-20 ) {
				return 0;
			}
			return pdfSolidAngle * sigma_t / distSquared;
		}

		//! Geometric term between a surface vertex and a medium vertex.
		//! G = |cos(theta_surface)| / ||p_surface - p_medium||^2
		//! Only the surface side contributes a cosine factor.
		/// \return The geometric term value
		inline Scalar GeometricTermSurfaceMedium(
			const Point3& pSurface,					///< [in] Position of surface vertex
			const Vector3& nSurface,				///< [in] Normal at surface vertex
			const Point3& pMedium					///< [in] Position of medium vertex
			)
		{
			Vector3 d = Vector3Ops::mkVector3( pMedium, pSurface );
			const Scalar distSq = Vector3Ops::SquaredModulus( d );

			if( distSq < 1e-20 ) {
				return 0;
			}

			const Scalar invDist = 1.0 / sqrt( distSq );
			d = d * invDist;

			const Scalar cosTheta = fabs( Vector3Ops::Dot( nSurface, d ) );

			return cosTheta / distSq;
		}

		//! Geometric term between two medium vertices.
		//! G = 1 / ||p1 - p2||^2
		//! Neither side has a surface orientation.
		/// \return The geometric term value
		inline Scalar GeometricTermMediumMedium(
			const Point3& p1,						///< [in] Position of first medium vertex
			const Point3& p2						///< [in] Position of second medium vertex
			)
		{
			const Scalar distSq = Vector3Ops::SquaredModulus(
				Vector3Ops::mkVector3( p2, p1 ) );

			if( distSq < 1e-20 ) {
				return 0;
			}

			return 1.0 / distSq;
		}
	}
}

#endif
