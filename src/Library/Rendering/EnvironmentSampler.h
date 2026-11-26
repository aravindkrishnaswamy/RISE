//////////////////////////////////////////////////////////////////////
//
//  EnvironmentSampler.h - Environment importance proposals.
//
//  Legacy RGB weights are sampled at equal-solid-angle (z, phi) cell
//  centres, querying the source painter through its angular-probe UV map.
//  Each cell spans 4*pi/(width*height) steradians. Sampling is uniform
//  inside the chosen cell; queried density is its PMF divided by that area.
//  The spectral-domain baseline remains uniform in WORLD solid angle.
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
#include "SpectralSourceDomain.h"

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

			std::shared_ptr<const SpectralSourceDomain> spectralDomain;
			const IRadianceMap* spectralMap = nullptr;
			SpectralSourceDomain::Mass spectralPower;

			unsigned int			width;			///< Equal-area azimuth cells
			unsigned int			height;			///< Equal-area z cells

			// Marginal/conditional CDF for importance sampling.
            // Normalized row/column CDFs. A zero-mass row is never sampled.
            // Solid-angle density uses their cell-probability differences.
			std::vector< std::vector<Scalar> >	conditionalCDF;
			std::vector<Scalar>					marginalCDF;

            /// Midpoint angular integral of max-channel source radiance.
			Scalar					totalLuminance;

            static Vector3 EqualAreaToDirection(Scalar u, Scalar v);
            static bool DirectionToEqualArea(const Vector3& direction, Scalar& u, Scalar& v);

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

            // Correctness baseline: uniform WORLD-SPHERE proposal. Estimate
            // root-selection power through the actual NM map (scale/orientation
            // and any current time adapter included). No spectral CDF claim.
            EnvironmentSampler(const IRadianceMap& map,
                std::shared_ptr<const SpectralSourceDomain> domain);
            const SpectralSourceDomain::Mass& SpectralPower() const { return spectralPower; }

			/// Build equal-solid-angle RGB weights (or spectral root-power estimate).
			/// Must be called once before Sample() or Pdf().
			void Build();

			/// Select an importance-weighted cell, then sample it uniformly in solid angle.
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
			bool IsValid() const { return spectralDomain ? spectralMap != nullptr : totalLuminance > 0; }

            /// Approximate integral over solid angle of the nonnegative
            /// max-channel RGB source proxy, including scale (not CIE luminance).
            /// A constant source C gives exactly 4*pi*C at every resolution.
            /// LightSampler multiplies by scene-disc area for root-class power.
            /// Zero for black/unbuilt legacy maps and for spectral-domain mode;
            /// spectral root selection reads SpectralPower() instead.
			Scalar TotalLuminance() const { return totalLuminance; }
		};
	}
}

#endif
