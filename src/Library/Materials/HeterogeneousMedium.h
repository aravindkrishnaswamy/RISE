//////////////////////////////////////////////////////////////////////
//
//  HeterogeneousMedium.h - Heterogeneous (spatially varying)
//    participating medium using delta tracking
//
//  Implements a spatially varying medium where density is driven by
//  a volume dataset (IVolumeAccessor).  The absorption and scattering
//  coefficients at any point are:
//    sigma_a(x) = density(x) * max_sigma_a
//    sigma_s(x) = density(x) * max_sigma_s
//  where density(x) is looked up from the volume (range [0, 1]).
//
//  DISTANCE SAMPLING: Delta tracking (Woodcock tracking)
//    Uses a majorant sigma_t_max = max(max_sigma_a + max_sigma_s)
//    (the maximum possible extinction when density=1).  At each
//    step along the ray, a null collision occurs if the local
//    sigma_t is less than the majorant.
//
//    Reference: Woodcock et al. 1965, "Techniques used in the GEM
//    code for Monte Carlo neutronics calculations".
//    Aligned with Blender/Cycles delta tracking in
//    volume_sample_distance() (shade_volume.h).
//
//  TRANSMITTANCE: Ratio tracking
//    Unbiased transmittance estimate using the same majorant.
//    At each step, the running transmittance is multiplied by
//    (1 - sigma_t_local / sigma_t_majorant).
//
//    Reference: Novak et al. 2014, "Residual Ratio Tracking for
//    Estimating Attenuation in Participating Media".
//
//  COORDINATE MAPPING:
//    The volume exists in world space within an axis-aligned
//    bounding box (AABB).  World points are mapped to volume
//    coordinates via:
//      vol_x = ((world_x - min_x) / extent_x - 0.5) * width
//    which maps the AABB to the centered coordinate system used
//    by RISE's Volume<T> / VolumeAccessor classes.  Points outside
//    the AABB have zero density (vacuum).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HETEROGENEOUS_MEDIUM_
#define HETEROGENEOUS_MEDIUM_

#include "../Interfaces/IMedium.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IVolumeAccessor.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/MajorantGrid.h"

namespace RISE
{
	/// \brief Heterogeneous participating medium driven by a volume dataset
	///
	/// Density is looked up from an IVolumeAccessor and scaled by
	/// user-specified maximum coefficients.  Uses delta tracking for
	/// distance sampling and ratio tracking for transmittance.
	class HeterogeneousMedium :
		public virtual IMedium,
		public virtual Implementation::Reference
	{
	protected:
		RISEPel m_max_sigma_a;				///< Max absorption coefficient [1/m]
		RISEPel m_max_sigma_s;				///< Max scattering coefficient [1/m]
		RISEPel m_max_sigma_t;				///< Max extinction = max_sigma_a + max_sigma_s
		RISEPel m_emission;					///< Volumetric emission (constant, not density-modulated)
		Scalar m_sigma_t_majorant;			///< Max channel of max_sigma_t (for delta tracking)

		const IPhaseFunction* m_pPhase;		///< Phase function (ref-counted)
		IVolumeAccessor* m_pAccessor;		///< Density field accessor (ref-counted)
		MajorantGrid* m_pMajorantGrid;		///< Per-cell majorant grid for DDA tracking

		/// World-space bounding box
		Point3 m_bboxMin;
		Point3 m_bboxMax;
		Vector3 m_bboxExtent;				///< bbox_max - bbox_min

		/// Volume dimensions (for coordinate mapping)
		unsigned int m_volWidth;
		unsigned int m_volHeight;
		unsigned int m_volDepth;

		/// Compute ray-AABB intersection (entry/exit distances).
		/// Returns false if the ray misses the box entirely.
		bool IntersectBBox(
			const Ray& ray,						///< [in] Ray to test
			Scalar& tEntry,						///< [out] Entry distance (may be 0 if origin inside)
			Scalar& tExit						///< [out] Exit distance
			) const;

		/// Deterministic optical depth via accessor-knot-aligned DDA +
		/// Gauss-Legendre quadrature.
		///
		/// Computes integral_0^targetDist sigma_t_eff * density(s) ds
		/// by Amanatides-Woo DDA traversal that splits the ray at
		/// accessor knot planes (integer coordinates in the centered
		/// accessor coordinate system).  This ensures each GL panel
		/// stays within one interpolation stencil region regardless
		/// of volume dimension parity.  5-point Gauss-Legendre
		/// quadrature is exact for polynomials up to degree 9,
		/// covering all supported accessor types: nearest-neighbor
		/// (degree 0), trilinear (degree 3 along ray), and
		/// Catmull-Rom tricubic (degree 9 along ray).
		///
		/// This is used by EvalDistancePdf to provide a deterministic
		/// technique density for MIS, avoiding the stochastic ratio
		/// tracking path through EvalTransmittance.
		/// The chromatic fire overload evaluates sigma_t(nm) at the same
		/// nodes.  Phase A's phi-aware root splitting / 7-point upgrade is a
		/// deliberately later increment.
		Scalar EvalDeterministicOpticalDepth(
			const Ray& ray,
			const Scalar targetDist,
			const Scalar sigma_t_eff
			) const;

		Scalar EvalDeterministicOpticalDepthNM(
			const Ray& ray,
			const Scalar targetDist,
			const Scalar nm
			) const;

		Scalar EvalDeterministicOpticalDepthImpl(
			const Ray& ray,
			const Scalar targetDist,
			const Scalar sigma_t_eff,
			const bool spectral,
			const Scalar nm
			) const;

		/// Scalar majorant used by the NM tracker.  Fire overrides this with
		/// the locked conservative max over the visible wavelength interval.
		virtual Scalar SpectralTrackingMajorant( const Scalar nm ) const;

		/// Construct the tracking substrate without an accessor.  Used by
		/// derived baked media that must create their channel accessors after
		/// base construction, then install a derived extinction accessor.
		HeterogeneousMedium(
			const Scalar trackingSigmaT,
			const IPhaseFunction& phase,
			const unsigned int volWidth,
			const unsigned int volHeight,
			const unsigned int volDepth,
			const Point3& bboxMin,
			const Point3& bboxMax
			);

		/// Install the accessor used by the shared delta/ratio-tracking code
		/// and build its immutable per-cell majorant grid.
		void InitializeTrackingAccessor( IVolumeAccessor& accessor );

		/// Variant for derived media whose tracking field is nonlinear in
		/// multiple channels.  The separate majorant accessor supplies a
		/// conservative phi-sup field while `accessor` remains the exact
		/// local extinction queried by tracking and quadrature.
		void InitializeTrackingAccessor(
			IVolumeAccessor& accessor,
			IVolumeAccessor& majorantAccessor
			);

		virtual ~HeterogeneousMedium();

	public:
		/// Look up density [0,1] at a world-space point.
		/// Returns 0 for points outside the bounding box.
		/// Public to allow DDA visitors to query density.
		Scalar LookupDensity(
			const Point3& worldPt				///< [in] World-space point
			) const;

		/// Construct a heterogeneous medium.
		/// The phase function and accessor are addref'd.
		HeterogeneousMedium(
			const RISEPel& max_sigma_a,			///< [in] Max absorption coefficient
			const RISEPel& max_sigma_s,			///< [in] Max scattering coefficient
			const IPhaseFunction& phase,		///< [in] Phase function for scattering
			IVolumeAccessor& accessor,			///< [in] Density field accessor (bound to volume)
			const unsigned int volWidth,		///< [in] Volume width in voxels
			const unsigned int volHeight,		///< [in] Volume height in voxels
			const unsigned int volDepth,		///< [in] Volume depth in voxels
			const Point3& bboxMin,				///< [in] World-space AABB minimum corner
			const Point3& bboxMax				///< [in] World-space AABB maximum corner
			);

		/// Construct with emission
		HeterogeneousMedium(
			const RISEPel& max_sigma_a,			///< [in] Max absorption coefficient
			const RISEPel& max_sigma_s,			///< [in] Max scattering coefficient
			const RISEPel& emission,			///< [in] Volumetric emission
			const IPhaseFunction& phase,		///< [in] Phase function for scattering
			IVolumeAccessor& accessor,			///< [in] Density field accessor (bound to volume)
			const unsigned int volWidth,		///< [in] Volume width in voxels
			const unsigned int volHeight,		///< [in] Volume height in voxels
			const unsigned int volDepth,		///< [in] Volume depth in voxels
			const Point3& bboxMin,				///< [in] World-space AABB minimum corner
			const Point3& bboxMax				///< [in] World-space AABB maximum corner
			);

		MediumCoefficients GetCoefficients(
			const Point3& pt
			) const override;

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt,
			const Scalar nm
			) const override;

		const IPhaseFunction* GetPhaseFunction() const override;

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

		Scalar ClipDistanceToBounds(
			const Ray& ray,
			const Scalar dist
			) const override;

		DistanceSample SampleDistanceWithPdf(
			const Ray& ray,
			const Scalar maxDist,
			ISampler& sampler
			) const override;

		DistanceSample SampleDistanceWithPdfNM(
			const Ray& ray,
			const Scalar maxDist,
			const Scalar nm,
			ISampler& sampler
			) const override;

		Scalar EvalDistancePdf(
			const Ray& ray,
			const Scalar t,
			const bool scattered,
			const Scalar maxDist
			) const override;

		Scalar EvalDistancePdfNM(
			const Ray& ray,
			const Scalar t,
			const bool scattered,
			const Scalar maxDist,
			const Scalar nm
			) const override;

		Scalar EvalLogDistancePdfNM(
			const Ray& ray,
			const Scalar t,
			const bool scattered,
			const Scalar maxDist,
			const Scalar nm
			) const override;

		bool GetBoundingBox(
			Point3& bbMin,
			Point3& bbMax
			) const override;
	};

	/// Painter-baked Phase-A fire/smoke medium carrying carbon concentration
	/// [g/m^3] and temperature [K] on one shared trilinear lattice.
	///
	/// Pel remains a grey 633-nm preview placeholder, while the NM path uses
	/// the wavelength-dependent §4.1/§4.3 hot-soot and cool-smoke laws.
	class MultichannelHeterogeneousMedium :
		public HeterogeneousMedium
	{
	protected:
		IVolumeAccessor* m_pCarbonAccessor;
		IVolumeAccessor* m_pTemperatureAccessor;
		Scalar m_sceneUnitMeters;
		Scalar m_sootEm;
		Scalar m_sootDensity;
		Scalar m_sootAlbedoHot;
		Scalar m_sootGHot;
		Scalar m_smokeKmCarbon;
		Scalar m_smokeNCarbon;
		Scalar m_smokeAlbedoCarbon;
		Scalar m_smokeGCarbon;
		Scalar m_hotAbsorptionMass633;
		Scalar m_hotExtinctionMass633;
		Scalar m_coolExtinctionMass633;
		bool m_valid;

		virtual ~MultichannelHeterogeneousMedium();
		Scalar SpectralTrackingMajorant( const Scalar nm ) const override;

		Scalar LookupChannel(
			const IVolumeAccessor& accessor,
			const Point3& worldPt
			) const;

		static Scalar ComputeHotAbsorptionMass633(
			Scalar sootEm,
			Scalar sootDensity
			);

	public:
		MultichannelHeterogeneousMedium(
			const IScalarPainter& carbonPainter,
			const IScalarPainter& temperaturePainter,
			const unsigned int volWidth,
			const unsigned int volHeight,
			const unsigned int volDepth,
			const Point3& bboxMin,
			const Point3& bboxMax,
			const Scalar sceneUnitMeters,
			const Scalar sootEm,
			const Scalar sootDensity,
			const Scalar sootAlbedoHot,
			const Scalar sootGHot,
			const Scalar smokeKmCarbon,
			const Scalar smokeNCarbon,
			const Scalar smokeAlbedoCarbon,
			const Scalar smokeGCarbon,
			const IPhaseFunction& phase
			);

		bool IsValid() const { return m_valid; }

		Scalar LookupCarbon( const Point3& worldPt ) const;
		Scalar LookupTemperature( const Point3& worldPt ) const;
		Scalar HotOpticsFraction( const Point3& worldPt ) const;
		Scalar HotSootVolumeFraction( const Point3& worldPt ) const;
		Scalar TrackingMajorantAt( const Point3& worldPt ) const;
		Scalar TrackingMajorantAtNM(
			const Point3& worldPt,
			const Scalar nm
			) const;

		MediumCoefficients GetCoefficients( const Point3& pt ) const override;
		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt,
			const Scalar nm
			) const override;

		bool IsFireMedium() const override { return true; }
		/// Fire scattering is wavelength/local-mixture bound.  Returning no
		/// legacy phase makes an accidental GetPhaseFunction fallback fail
		/// closed instead of silently using the constructor's base-class phase.
		const IPhaseFunction* GetPhaseFunction() const override { return 0; }
		const IPhaseFunction* MakePhaseClosure(
			const Point3& pt,
			const Scalar nm
			) const override;
		Scalar GetThermalEmissionNM(
			const Point3& pt,
			const Scalar nm
			) const override;
	};
}

#endif
