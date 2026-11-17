//////////////////////////////////////////////////////////////////////
//
//  MaterialPreview.h - Headless 3D shaderball preview engine
//    (MATERIAL_EDITOR.md §4): renders a deterministic, spectral-honest
//    RGBA8 thumbnail of a named IMaterial on an analytical 3D sphere
//    lit by a multi-light studio rig over a 3D checker backdrop.
//
//  WHY AN ANALYTICAL SHADERBALL EVALUATOR.
//  Material thumbnails are fetched on-demand for the C1 Material
//  inspector header (56x56) and for every Material node on the C2
//  NodeGraphCanvas (40x40), under SceneEditController's non-blocking
//  try_lock + mRenderOwnsScene guard (matching PainterPreview).
//  Standing up a full Scene + RayCaster + GlobalThreadPool render
//  inside that lock would contend with the interactive viewport
//  thread pool and hold mMutex for tens or hundreds of milliseconds
//  per material node.  Instead, this module ray-traces an analytical
//  3D shaderball sphere with continuous spherical UVs and seam-free
//  latitude/longitude tangents (so anisotropic GGX/Ward/Ashikhmin
//  materials show their true highlight orientation and 2D/3D
//  procedural painters wrap cleanly over the sphere) and evaluates:
//    - IBSDF::valueStatefulNM across a deterministic 12-wavelength
//      spectral quadrature [390, 710] nm projected through the CIE
//      CMFs to D65-normalized Rec.709 RGB (so thin-film interference,
//      measured metal n(lambda)/k(lambda) conductors, and spectral
//      painters render with their true spectral response),
//    - IMaterial::GetSpecularInfoNM for delta materials
//      (DielectricMaterial, PerfectReflectorMaterial,
//      PerfectRefractorMaterial), tracing two-interface sphere
//      refraction with per-wavelength Snell's law and Beer-Lambert
//      interior transmittance against the 3D checker backdrop (so
//      Sellmeier glasses visibly disperse the refracted backdrop),
//    - IBSDF::albedo for hemisphere/environment reflections on smooth
//      conductors and coats,
//    - IMaterial::GetRandomWalkSSSParams / GetDiffusionProfile for
//      subsurface wrap scattering, and
//    - IEmitter::emittedRadiance for luminaire materials.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_MATERIALPREVIEW_
#define RISE_MATERIALPREVIEW_

#include "../Interfaces/IJobPriv.h"
#include "../Utilities/RString.h"
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	class MaterialPreview
	{
	public:
		static const unsigned int kMaxDim = 2048;
		static const unsigned int kMaxSampleBudget = 96u * 96u;

		enum class Status { Ok, Refused };

		struct Result
		{
			Status      status = Status::Refused;
			std::string refusalReason;

			unsigned int width  = 0;
			unsigned int height = 0;
			//! Row-major top-to-bottom RGBA8, size == width * height * 4 iff status == Ok.
			std::vector<unsigned char> rgba;
		};

		//! Renders a 3D shaderball preview of the material named
		//! `materialName` in `job.GetMaterials()`. Refuses when `w` or `h`
		//! is 0 or > kMaxDim, or when `materialName` is empty, `"none"`, or
		//! unresolved. Requests with `w * h > kMaxSampleBudget` evaluate on
		//! a decimated grid and nearest-upscale to `w * h`.
		static Result RenderMaterialPreview(
			IJobPriv& job,
			const String& materialName,
			unsigned int w = 64,
			unsigned int h = 64 );

		//! Direction D3 (SPECTRAL_DIFFERENTIATORS.md §4): renders a
		//! horizontal angle-sweep iridescence strip (0 deg normal incidence
		//! at x=0 to 89 deg grazing incidence at x=w-1) for a GGXMaterial
		//! in `fresnel_mode thinfilm` using `ThinFilm::ReflectanceConductorRGBSpectral`.
		//! Optionally reports the sampled `film_thickness` (nm) and
		//! `film_ior` (at 550 nm).
		static Result RenderThinFilmAngleStrip(
			IJobPriv& job,
			const String& materialName,
			unsigned int w = 220,
			unsigned int h = 20,
			double* outThicknessNm = nullptr,
			double* outFilmIor = nullptr );

		//! Evaluates the exact spectral thin-film reflectance of a
		//! `ggx_material` in `fresnel_mode thinfilm` at incidence angle
		//! `angleDeg` ([0, 89]), writing gamma-2.2 RGBA8 into `outRGBA[4]`
		//! and optionally reporting `outThicknessNm` and `outFilmIor`.
		static bool EvaluateThinFilmAtAngle(
			IJobPriv& job,
			const String& materialName,
			double angleDeg,
			unsigned char outRGBA[4],
			double* outThicknessNm = nullptr,
			double* outFilmIor = nullptr );

		//! Direction D1 (SPECTRAL_DIFFERENTIATORS.md §2): renders a
		//! Planckian blackbody chromaticity strip sweeping temperature from
		//! `minK` to `maxK` (default 1000 K to 12000 K).
		static Result RenderBlackbodyKelvinStrip(
			unsigned int w = 220,
			unsigned int h = 20,
			double minK = 1000.0,
			double maxK = 12000.0 );

		//! Evaluates the normalized Planckian chromaticity RGBA8 swatch and
		//! Wien's displacement peak wavelength (nm) for `temperatureK`.
		static bool EvaluateBlackbodyKelvin(
			double temperatureK,
			double* outPeakNm,
			unsigned char outRGBA[4] );

		//! Evaluates the Fraunhofer d-line refractive index n_d (587.56 nm)
		//! and Abbe number V_d = (n_d - 1) / (n_F - n_C) from a 6-number
		//! Sellmeier string `B1 B2 B3 C1 C2 C3` (wavelength in um).
		static bool EvaluateSellmeierMetrics(
			const String& coeffsSixTuple,
			double* outNd,
			double* outAbbeVd );

		//! Looks up a curated Schott / optical glass Sellmeier preset
		//! ("N-BK7", "SF11", "Diamond", "FusedSilica").
		static bool LookupSellmeierGlassPreset(
			const String& presetId,
			std::string& outCoeffs,
			double* outNd = nullptr,
			double* outAbbeVd = nullptr );

		//! Direction D2 (SPECTRAL_DIFFERENTIATORS.md §3): looks up a
		//! curated CC0 measured metal preset ("Au", "Ag", "Cu", "Al", "Cr",
		//! "Ti"), returning the repository-relative `.n` and `.k` file
		//! paths, fallback 4-term polynomial fits over [380, 780] nm when
		//! the media files are unavailable on disk, and the normal-incidence
		//! spectral conductor Fresnel RGBA8 swatch.
		static bool LookupMeasuredMetalPreset(
			const String& metalId,
			std::string& outFileN,
			std::string& outFileK,
			std::string& outPolyN,
			std::string& outPolyK,
			unsigned char outSwatchRGBA[4] );
	};
}

#endif
