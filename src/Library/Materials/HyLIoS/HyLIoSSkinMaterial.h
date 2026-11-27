//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSkinMaterial.h - HyLIoS hyperspectral skin material (P5 local
//    baked response + shared visible guide + capability metadata).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HYLIOS_SKIN_MATERIAL_H_
#define HYLIOS_SKIN_MATERIAL_H_

#include "../../Interfaces/IMaterial.h"
#include "../../Interfaces/IBSDF.h"
#include "../../Interfaces/ISPF.h"
#include "../../Utilities/Reference.h"
#include "HyLIoSLocalResponse.h"
#include "HyLIoSLocalResponseAsset.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RISE
{
	//! Versioned construction configuration for `hylios_skin_material`.
	//! Callers must initialize via default member initializers (or set
	//! `structVersion = kCurrentVersion` and `structSize = sizeof(HyLIoSSkinMaterialConfig)`).
	struct HyLIoSSkinMaterialConfig
	{
		static constexpr std::uint32_t kCurrentVersion = 1u;

		std::uint32_t structVersion = kCurrentVersion;
		std::uint32_t structSize    = sizeof( HyLIoSSkinMaterialConfig );

		// Explicit model, data, transport, and boundary revisions.
		const char* modelRevision       = "hylios_local_bake_analog_projected_v2";
		const char* dataRevision        = "hylios_cleanroom_composition_v1";
		const char* transportMode       = "baked_local";   ///< "baked_local" (P5); "nonlocal"/"diagnostic" (P7)
		const char* boundaryMode        = "smooth";        ///< "smooth" | "hylios2015"
		const char* boundaryRevision    = "smooth_planar_dielectric_v1";
		const char* placementPolicy     = "null_collision_placement_v1";
		const char* postScatterPolicy   = "discard_after_scatter_v1";
		const char* preset              = "S3";            ///< "S1" | "S2" | "S3" | "S4" | "custom"
		bool        explicitComplexPackV1 = false;

		// Resolved physical parameters (SI units).
		double      exteriorIOR         = 1.0;
		double      physicalScale       = 1.0;             ///< scene units per metre
		double      fiberRadiusMetres   = 1e-7;
		double      fiberFraction       = 0.22;
		double      fiberRelativeIOR    = 1.5 / 1.33;

		// Authored / resolved file paths (UTF-8).
		const char* responseAssetPath   = nullptr;         ///< Required for baked_local
		const char* specimenSourcePath  = nullptr;         ///< Resolved specimen JSON for independent semantic identity

		// Optional explicit wavelength grid (nm).  When null/empty and
		// specimenSourcePath is supplied, the specimen's own wavelength grid is used.
		const double* wavelengthsNm     = nullptr;
		std::size_t   wavelengthCount   = 0;

		// Configurable safety budgets (workplan §11 defaults: 256 MiB file / 1 GiB resident).
		std::uint64_t maximumFileBytes      = std::uint64_t{256} * 1024 * 1024;
		std::uint64_t maximumResidentBytes  = std::uint64_t{1024} * 1024 * 1024;
		std::uint64_t aggregateResidentCap  = std::uint64_t{2048} * 1024 * 1024;

		// Optional standalone-asset codec path for offline/analytic unit-test fixtures.
		// When `useStandaloneAssetCodec` is true, `explicitSemanticIdentity` must point
		// to an independently constructed `RISE::HyLIoS::LocalAssetSemanticIdentity`.
		const void* explicitSemanticIdentity = nullptr;
		bool        useStandaloneAssetCodec  = false;

		// P7 nonlocal spatial skin transport parameters (used when transportMode == "nonlocal").
		double        boundaryRoughness       = 0.25;              ///< GGX alpha for rise_reciprocal_boundary_v1 (0 = smooth delta)
		const char*   spatialDomain           = "planar_polygon";  ///< "planar_polygon" | "concentric_sphere" | "concentric_cylinder" | "intrinsic_layer"
		double        domainHalfWidthMetres   = 0.05;              ///< Planar polygon half-width / cylinder half-length (m)
		double        domainRadiusMetres      = 0.10;              ///< Concentric sphere/cylinder outer radius (m)
		std::uint64_t maxSubsurfaceSteps      = 256;               ///< Bounded iterative subsurface step budget
	};

	namespace Implementation
	{
		//! Shared D65-weighted, white-normalized visible hemispherical reflectance
		//! guide projection (workplan §11).  Returns false when the supplied nodes
		//! have no overlap with the visible CIE [380, 780] nm domain.
		bool ComputeHyLIoSVisibleGuideRGB(
			const std::vector<double>& wavelengthsNm,
			const std::vector<double>& hemisphericalReflectance,
			RISEPel& outGuideRGB
			);

		//! Local baked-response BSDF for `HyLIoSSkinMaterial`.
		class HyLIoSLocalBSDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			std::shared_ptr<const RISE::HyLIoS::HyLIoSLocalResponse> pResponse;
			RISEPel guideRGB;
			bool    hasVisibleGuide;

			virtual ~HyLIoSLocalBSDF();

		public:
			HyLIoSLocalBSDF(
				std::shared_ptr<const RISE::HyLIoS::HyLIoSLocalResponse> response,
				const RISEPel& guideRGB_,
				const bool hasVisibleGuide_
				);

			RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const override;
			Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const override;
			RISEPel albedo( const RayIntersectionGeometric& ri ) const override;
		};

		//! Local baked-response SPF backed by `HyLIoSSupportedCameraProposal`.
		class HyLIoSLocalSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			std::shared_ptr<const RISE::HyLIoS::HyLIoSSupportedCameraProposal> pProposal;
			RISEPel guideRGB;
			bool    hasVisibleGuide;

			virtual ~HyLIoSLocalSPF();

		public:
			HyLIoSLocalSPF(
				std::shared_ptr<const RISE::HyLIoS::HyLIoSSupportedCameraProposal> proposal,
				const RISEPel& guideRGB_,
				const bool hasVisibleGuide_
				);

			void Scatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const override;

			void ScatterNM(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const override;

			Scalar Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const override;

			Scalar PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const override;
		};

		//! Immutable provenance & metadata record retained by `HyLIoSSkinMaterial`.
		struct HyLIoSSkinMaterialSummary
		{
			std::string modelRevision;
			std::string dataRevision;
			std::string transportMode;
			std::string boundaryMode;
			std::string boundaryRevision;
			std::string placementPolicy;
			std::string postScatterPolicy;
			std::string preset;
			std::string responseAssetPath;
			std::string specimenSourcePath;
			std::string contentSHA256;
			std::string semanticSHA256;
			std::string productionSHA256;
			double      exteriorIOR = 1.0;
			double      physicalScale = 1.0;
			double      coveredFirstNm = 0.0;
			double      coveredLastNm = 0.0;
			std::uint64_t residentBytesBound = 0;
			bool        hasVisibleGuide = false;
		};

		//! Result of one iterative nonlocal spatial skin transport step (P7).
		struct HyLIoSNonlocalSample
		{
			bool   escaped = false;
			bool   isExternalReflection = false;
			bool   isDelta = false;
			bool   domainBoundaryLoss = false;
			Point3 exitPos;
			Vector3 exitNormal;
			Vector3 exitDir;
			Scalar throughput = Scalar( 0 );
			Scalar pdf = Scalar( 0 );
			double lateralDisplacementMetres = 0.0;
		};

		struct HyLIoSNonlocalRuntime;

		//! HyLIoS hyperspectral skin material (P5 local baked mode + P7 nonlocal spatial mode).
		class HyLIoSSkinMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			HyLIoSLocalBSDF* pBSDF;
			HyLIoSLocalSPF*  pSPF;
			std::shared_ptr<const void> pCacheToken;
			std::shared_ptr<const RISE::HyLIoS::HyLIoSLocalResponseAsset> pAsset;
			std::shared_ptr<const RISE::HyLIoS::HyLIoSSupportedCameraProposal> pProposal;
			std::shared_ptr<HyLIoSNonlocalRuntime> pNonlocal;
			RISEPel guideRGB;
			HyLIoSSkinMaterialSummary summary;

			virtual ~HyLIoSSkinMaterial();

			HyLIoSSkinMaterial(
				std::shared_ptr<const void> cacheToken,
				std::shared_ptr<const RISE::HyLIoS::HyLIoSLocalResponseAsset> asset,
				std::shared_ptr<const RISE::HyLIoS::HyLIoSSupportedCameraProposal> proposal,
				std::shared_ptr<HyLIoSNonlocalRuntime> nonlocal,
				const RISEPel& guideRGB_,
				const HyLIoSSkinMaterialSummary& summary_
				);

		public:
			//! Validates `config`, independently resolves semantic identity, loads
			//! (or shares from the immutable render cache) the validated response
			//! asset (baked_local) or prepares the nonlocal spatial runtime (nonlocal),
			//! and constructs the material.  Returns nullptr and writes a diagnostic
			//! into `outError` on any validation, coverage, budget, or I/O failure.
			static HyLIoSSkinMaterial* Create(
				const HyLIoSSkinMaterialConfig& config,
				std::string* outError = nullptr
				);

			IBSDF* GetBSDF() const override { return pBSDF; }
			ISPF*  GetSPF() const override  { return pSPF; }
			IEmitter* GetEmitter() const override { return 0; }

			bool IsHyperspectralSkin() const override { return true; }
			bool IsNonlocalSkin() const noexcept { return pNonlocal != nullptr; }
			bool EvaluateVisibleGuideAlbedo( const RayIntersectionGeometric& geom, RISEPel& outAlbedo ) const override;
			bool CoversWavelengthRange( const Scalar nmBegin, const Scalar nmEnd ) const override;

			//! Executes one bounded iterative nonlocal spatial skin step at `nm`
			//! (no recursive CastRay).  Returns true iff a continuation ray escaped.
			bool SampleNonlocalNM(
				const RayIntersectionGeometric& ri,
				const Scalar nm,
				ISampler& sampler,
				HyLIoSNonlocalSample& outSample
				) const;

			//! Number of paths terminated at explicit absorbing lateral side walls
			//! (tracked separately from bulk absorption).
			std::uint64_t DomainBoundaryLossCount() const noexcept;

			const HyLIoSSkinMaterialSummary& GetSummary() const noexcept { return summary; }
			const RISEPel& GetVisibleGuideRGB() const noexcept { return guideRGB; }
			const RISE::HyLIoS::HyLIoSLocalResponseAsset& GetAsset() const noexcept { return *pAsset; }
			const RISE::HyLIoS::HyLIoSSupportedCameraProposal& GetProposal() const noexcept { return *pProposal; }

			//! Diagnostic helper for tests verifying asset cache sharing.
			static std::size_t ActiveCachedAssetCount();
		};
	}
}

#endif // HYLIOS_SKIN_MATERIAL_H_
