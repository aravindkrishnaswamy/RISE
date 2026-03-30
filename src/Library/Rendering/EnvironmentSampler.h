//////////////////////////////////////////////////////////////////////
//
//  EnvironmentSampler.h - Importance sampling for HDR environment
//  maps using a 2D marginal/conditional CDF built from luminance.
//
//  Supports the angular (light-probe) mapping used by RadianceMap:
//    s = 0.5 + vx * r,  t = 0.5 - vy * r
//    where r = acos(-vz) / (2*PI * sqrt(vx^2 + vy^2))
//
//  The Jacobian of this mapping is |d(s,t)/d(theta,phi)| =
//  theta / (4*PI^2), so the solid-angle PDF is:
//    pdf_omega = pdf_st * theta / (4*PI^2 * sin(theta))
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ENVIRONMENT_SAMPLER_
#define ENVIRONMENT_SAMPLER_

#include "../Interfaces/IRadianceMap.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Reference.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class EnvironmentSampler : public virtual Reference
		{
		protected:
			virtual ~EnvironmentSampler();

			const IPainter&			painter;		///< HDR texture painter
			Scalar					dScale;			///< Radiance scale factor
			Matrix4					mxtransform;	///< World-to-map transform
			Matrix4					mxInvTransform;	///< Map-to-world transform (inverse)

			unsigned int			width;			///< CDF resolution (columns)
			unsigned int			height;			///< CDF resolution (rows)

			// Marginal/conditional CDF for importance sampling.
			// conditionalCDF[v] has (width+1) entries; conditionalCDF[v][width] = row total.
			// marginalCDF has (height+1) entries; marginalCDF[height] = grand total.
			std::vector< std::vector<Scalar> >	conditionalCDF;
			std::vector<Scalar>					marginalCDF;

			/// Total integrated luminance (for normalization)
			Scalar					totalLuminance;

			/// Converts a (theta, phi) to texture coordinates (s, t)
			/// using the light-probe mapping.
			static void DirectionToTexCoord(
				const Vector3& dir,
				Scalar& s,
				Scalar& t
				);

			/// Converts texture coordinates (s, t) back to a direction vector.
			static Vector3 TexCoordToDirection(
				const Scalar s,
				const Scalar t
				);

			/// Binary search for the CDF: finds the bin index such that
			/// cdf[index] <= xi < cdf[index+1], returns the fractional
			/// offset within the bin.
			static unsigned int SampleCDF(
				const std::vector<Scalar>& cdf,
				const unsigned int n,
				const Scalar xi,
				Scalar& fractional
				);

		public:
			EnvironmentSampler(
				const IPainter& painter,
				const Scalar scale,
				const Matrix4& transform,
				const unsigned int resolution
				);

			/// Build the 2D importance map from the HDR texture.
			/// Must be called once before Sample() or Pdf().
			void Build();

			/// Sample a direction proportional to luminance.
			void Sample(
				const Scalar u1,			///< [in] Uniform random [0,1)
				const Scalar u2,			///< [in] Uniform random [0,1)
				Vector3& direction,			///< [out] Sampled world-space direction
				Scalar& pdf					///< [out] Solid-angle PDF
				) const;

			/// Evaluate the solid-angle PDF for a given world-space direction.
			Scalar Pdf(
				const Vector3& direction	///< [in] World-space direction
				) const;

			/// \return True if the importance map has been built and is valid.
			bool IsValid() const { return totalLuminance > 0; }
		};
	}
}

#endif
