//////////////////////////////////////////////////////////////////////
//
//  HyperspectralRadiometricRasterizer.h - Dedicated raw radiometric
//    fixed-wavelength hyperspectral rasterizer (P6b).
//
//    Sweeps an explicit list (or uniform grid) of physical wavelengths
//    in nanometres using PathTracingIntegrator::IntegrateRayNM,
//    accumulates raw L_lambda [W m^-2 sr^-1 nm^-1] into RadiometricFilm
//    (no RGB conversion, no tone-mapping, no OIDN, no clamping),
//    supports RadiometricCheckpoint save/resume, writes lossless
//    single-channel FLOAT L_lambda EXR files via WriteRadiometricEXR,
//    and emits an atomic JSON sweep manifest.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_HYPERSPECTRAL_RADIOMETRIC_RASTERIZER_H
#define RISE_HYPERSPECTRAL_RADIOMETRIC_RASTERIZER_H

#include "Rasterizer.h"
#include "RadiometricFilm.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/SMSConfig.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	class IRayCaster;

	//! Configuration for `hyperspectral_radiometric_rasterizer`.
	struct HyperspectralRadiometricConfig
	{
		static constexpr std::uint32_t kCurrentVersion = 2u;

		std::uint32_t structVersion = kCurrentVersion;
		std::uint32_t structSize    = sizeof( HyperspectralRadiometricConfig );

		// Explicit strictly increasing wavelengths in nm.  When empty,
		// generated from [lambdaBegin, lambdaEnd] at lambdaStep intervals.
		std::vector<double> wavelengthsNm;
		double lambdaBegin = 400.0;
		double lambdaEnd   = 700.0;
		double lambdaStep  = 10.0;

		std::uint64_t samplesPerPixel = 16;
		std::uint64_t seed            = 1;

		// Required output file prefix (writes `<prefix>_<nm>nm.exr` and
		// `<prefix>_manifest.json`).
		std::string outputPrefix;

		// Optional checkpoint prefix (reads/writes `<checkpointPrefix>_<nm>nm.rcp`).
		std::string checkpointPrefix;

		// Strict scientific mode (default false) refuses non-physical /
		// RGB-uplifted delta lights and non-spectral environment maps.
		bool allowRGBUpliftSources = false;

		// Bias-control guards: preflight REFUSES any non-zero clamp,
		// glossy filtering, OIDN denoising, tone mapping, or negative-lobe
		// pixel filter.
		bool oidnDenoise = false;
		bool toneMap     = false;
		std::string pixelFilterName = "box";

		SMSConfig            smsConfig;
		StabilityConfig      stabilityConfig;
		Implementation::RadiometricLimits limits;

		// Tail-appended v2 fields:
		// Output observable mode:
		//   "radiance_density" (default): per-wavelength L_lambda [W m^-2 sr^-1 nm^-1]
		//   "band_integrated":  integral L_lambda dlambda [W m^-2 sr^-1]
		//   "band_average":     (1 / delta_lambda) integral L_lambda dlambda [W m^-2 sr^-1 nm^-1]
		//   "sensor_energy":    integral L_lambda * s(lambda) dlambda [W m^-2 sr^-1]
		//   "sensor_photon":    integral L_lambda * (lambda / (h*c)) * s(lambda) dlambda [photons s^-1 m^-2 sr^-1]
		std::string outputObservable = "radiance_density";

		// Tabulated sensor spectral sensitivity (wavelength_nm, weight >= 0),
		// required when outputObservable is "sensor_energy" or "sensor_photon".
		std::vector<std::pair<double, double>> sensorResponse;
	};

	namespace Implementation
	{
		class PathTracingIntegrator;

		class HyperspectralRadiometricRasterizer : public virtual Rasterizer
		{
		protected:
			IRayCaster*                    pCaster;
			PathTracingIntegrator*         pIntegrator;
			HyperspectralRadiometricConfig config;

			mutable std::string                       lastError;
			mutable bool                              lastCompleted;
			mutable std::vector<RadiometricSnapshot>  lastSnapshots;
			mutable std::string                       lastManifestPath;
			mutable std::vector<double>               lastDerivedObservable;
			mutable std::string                       lastDerivedObservablePath;

			virtual ~HyperspectralRadiometricRasterizer();

			bool ResolveWavelengths(
				std::vector<double>& outWavelengths,
				std::string& outError
				) const;

		public:
			HyperspectralRadiometricRasterizer(
				IRayCaster* pCaster_,
				const HyperspectralRadiometricConfig& config_
				);

			//! Preflight validation of config, bias controls, HyLIoS band
			//! coverage, and physical spectral light sources.
			bool ValidateSceneAndConfig(
				const IScene& pScene,
				std::vector<double>& outWavelengths,
				std::string& outError
				) const;

			void AttachToScene( const IScene* pScene ) override;
			void DetachFromScene( const IScene* pScene ) override;

			unsigned int PredictTimeToRasterizeScene(
				const IScene& pScene,
				const ISampling2D& pSampling,
				unsigned int* pActualTime
				) const override;

			void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const override;

			void RasterizeSceneAnimation(
				const IScene& pScene,
				const Scalar time_start,
				const Scalar time_end,
				const unsigned int num_frames,
				const bool do_fields,
				const bool invert_fields,
				const Rect* pRect,
				const unsigned int* specificFrame,
				IRasterizeSequence* pRasterSequence
				) const override;

			bool LastRenderCompleted() const override { return lastCompleted; }
			const std::string& LastError() const noexcept { return lastError; }
			const std::vector<RadiometricSnapshot>& LastSnapshots() const noexcept { return lastSnapshots; }
			const std::string& LastManifestPath() const noexcept { return lastManifestPath; }
			const std::vector<double>& LastDerivedObservable() const noexcept { return lastDerivedObservable; }
			const std::string& LastDerivedObservablePath() const noexcept { return lastDerivedObservablePath; }
			const HyperspectralRadiometricConfig& GetConfig() const noexcept { return config; }
		};
	}
}

#endif // RISE_HYPERSPECTRAL_RADIOMETRIC_RASTERIZER_H
