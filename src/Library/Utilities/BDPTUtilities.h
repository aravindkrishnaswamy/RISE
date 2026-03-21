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

namespace RISE
{
	namespace BDPTUtilities
	{
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
	}
}

#endif
