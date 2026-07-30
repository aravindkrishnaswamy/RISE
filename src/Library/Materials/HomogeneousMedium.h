//////////////////////////////////////////////////////////////////////
//
//  HomogeneousMedium.h - Homogeneous participating medium
//
//  Implements a spatially uniform medium with constant absorption,
//  scattering, and emission coefficients.  Uses analytic exponential
//  sampling for free-flight distances and Beer-Lambert law for
//  transmittance evaluation.
//
//  SAMPLING ALGORITHM (exponential free-flight):
//    Given sigma_t (extinction), the distance to the next scatter
//    event is sampled as:
//      t = -ln(1 - xi) / sigma_t_max
//    where sigma_t_max = max(sigma_t.r, sigma_t.g, sigma_t.b).
//
//    If t < maxDist, a scatter event occurs.  The sampling PDF is:
//      p(t) = sigma_t_max * exp(-sigma_t_max * t)
//    and the transmittance along [0, t) is:
//      T(t) = exp(-sigma_t * t)   (per-channel Beer-Lambert)
//
//  SPECTRAL MODE:
//    In spectral (NM) mode, GetCoefficientsNM evaluates genuine
//    per-wavelength sigma_a(lambda)/sigma_s(lambda) from the optional
//    m_sigma_*_spectral IFunction1D curves when either is bound (G1,
//    vitreous enamel); with neither bound it falls back to the
//    luminance of the RGB triples (byte-identical to pre-G1).  The
//    distance sampler and transmittance both read sigma_t(lambda)
//    through GetCoefficientsNM so the two paths cannot drift.
//
//  Aligned with Blender/Cycles homogeneous volume integration in
//  volume_integrate_homogeneous()
//  (intern/cycles/kernel/integrator/shade_volume.h).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HOMOGENEOUS_MEDIUM_
#define HOMOGENEOUS_MEDIUM_

#include "../Interfaces/IMedium.h"
#include "../Interfaces/IFunction1D.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/Color/ColorMath.h"

namespace RISE
{
	/// \brief Homogeneous (spatially uniform) participating medium
	///
	/// Stores constant sigma_a, sigma_s, and optional emission.
	/// The phase function is shared via reference counting.
	class HomogeneousMedium :
		public virtual IMedium,
		public virtual Implementation::Reference
	{
	protected:
		RISEPel m_sigma_a;					///< Absorption coefficient [1/m]
		RISEPel m_sigma_s;					///< Scattering coefficient [1/m]
		RISEPel m_sigma_t;					///< Extinction = sigma_a + sigma_s
		RISEPel m_emission;					///< Volumetric emission (usually zero)
		Scalar m_sigma_t_max;				///< Max channel of sigma_t (for sampling)

		const IPhaseFunction* m_pPhase;		///< Phase function (ref-counted)

		// Optional per-wavelength coefficient curves (G1, vitreous
		// enamel).  When either is non-null, the NM (spectral) path
		// evaluates genuine sigma_a(lambda)/sigma_s(lambda) from these
		// curves; when BOTH are null the NM path uses the luminance
		// fallback (byte-identical to pre-G1 behaviour).  A null curve
		// with the sibling non-null contributes ZERO for that
		// coefficient in spectral mode (a pure-absorber colorant leaves
		// the scattering curve null).  The RGB path
		// (GetCoefficients/EvalTransmittance/SampleDistance) is
		// unaffected and continues to use the RGB triples above for the
		// interactive/RGB preview.  Both are addref'd in the ctor and
		// released in the dtor.
		const IFunction1D* m_sigma_a_spectral = nullptr;	///< sigma_a(lambda) [1/m], nullable
		const IFunction1D* m_sigma_s_spectral = nullptr;	///< sigma_s(lambda) [1/m], nullable

		virtual ~HomogeneousMedium();

	public:
		/// Construct a homogeneous medium with given coefficients.
		/// The phase function is addref'd and will be released on destruction.
		HomogeneousMedium(
			const RISEPel& sigma_a,			///< [in] Absorption coefficient
			const RISEPel& sigma_s,			///< [in] Scattering coefficient
			const IPhaseFunction& phase		///< [in] Phase function for scattering
			);

		/// Construct with emission
		HomogeneousMedium(
			const RISEPel& sigma_a,			///< [in] Absorption coefficient
			const RISEPel& sigma_s,			///< [in] Scattering coefficient
			const RISEPel& emission,		///< [in] Volumetric emission
			const IPhaseFunction& phase		///< [in] Phase function for scattering
			);

		/// Construct with optional per-wavelength coefficient curves for
		/// the spectral (NM) path.  \a sigma_a_spectral / \a sigma_s_spectral
		/// may be null (=> that coefficient is zero in spectral mode; if
		/// BOTH are null the NM path falls back to the luminance of the RGB
		/// triples).  The RGB triples still drive the RGB rendering/preview
		/// path.  Non-null curves are addref'd here and released on
		/// destruction.
		HomogeneousMedium(
			const RISEPel& sigma_a,				///< [in] Absorption coefficient (RGB preview)
			const RISEPel& sigma_s,				///< [in] Scattering coefficient (RGB preview)
			const RISEPel& emission,			///< [in] Volumetric emission
			const IFunction1D* sigma_a_spectral,///< [in] sigma_a(lambda) curve, or null
			const IFunction1D* sigma_s_spectral,///< [in] sigma_s(lambda) curve, or null
			const IPhaseFunction& phase			///< [in] Phase function for scattering
			);

		MediumCoefficients GetCoefficients(
			const Point3& pt
			) const override;

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt,
			const Scalar nm
			) const override;

		const IPhaseFunction* GetPhaseFunction() const override;
		const IPhaseFunction* MakeContinuationPhaseClosurePel(
			const Point3& pt ) const override
		{
			if( m_pPhase ) m_pPhase->addref();
			return m_pPhase;
		}
		const IPhaseFunction* MakeContinuationPhaseClosureNM(
			const Point3& pt, const Scalar nm ) const override
		{
			if( m_pPhase ) m_pPhase->addref();
			return m_pPhase;
		}

		Scalar SampleDistance(
			const Ray& ray,
			const Scalar maxDist,
			ISampler& sampler,
			bool& scattered
			) const override;

		Scalar SampleDistanceNM(
			const Ray& ray,
			const Scalar maxDist,
			const Scalar nm,
			ISampler& sampler,
			bool& scattered
			) const override;

		RISEPel EvalTransmittance(
			const Ray& ray,
			const Scalar dist
			) const override;

		Scalar EvalTransmittanceNM(
			const Ray& ray,
			const Scalar dist,
			const Scalar nm
			) const override;

		bool IsHomogeneous() const override;

		//! Read-back + rebind for the interactive editor.  Setters
		//! update the derived `m_sigma_t = sigma_a + sigma_s` and
		//! `m_sigma_t_max` in lockstep — these caches are read by the
		//! distance-sampling and transmittance evaluation paths and
		//! would drift if the dependent inputs changed without a
		//! refresh.  Caller is responsible for the cancel-and-park
		//! gate against the render thread, same as the material
		//! painter rebinders.
		inline const RISEPel& GetAbsorption() const { return m_sigma_a; }
		inline const RISEPel& GetScattering() const { return m_sigma_s; }
		inline const RISEPel& GetEmission()   const { return m_emission; }
		//! Spectral coefficient curves (null when the medium is RGB-only).
		inline const IFunction1D* GetAbsorptionSpectral() const { return m_sigma_a_spectral; }
		inline const IFunction1D* GetScatteringSpectral() const { return m_sigma_s_spectral; }
		void SetAbsorption( const RISEPel& v );
		void SetScattering( const RISEPel& v );
		void SetEmission( const RISEPel& v );
	};
}

#endif
