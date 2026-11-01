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
	};
}

#endif
