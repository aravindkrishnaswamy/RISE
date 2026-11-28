//////////////////////////////////////////////////////////////////////
//
//  EntityTemplates.cpp - See EntityTemplates.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "EntityTemplates.h"

#include "../RISE_API.h"
#include "ChunkDescriptorRegistry.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Color/Color.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <algorithm>
#include <array>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif

namespace RISE
{

namespace
{
	using Category = SceneEditController::Category;

	// -------------------------------------------------------------
	// Lights
	// -------------------------------------------------------------
	const EntityTemplateDef& OmniLightTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Light;
			t.label = "Omni Light";
			t.baseName = "omni";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"omni_light\n"
				"{\n"
				"name @NAME@\n"
				"power 3.0\n"
				"color 1.0 1.0 1.0\n"
				"position 0.0 3.0 0.0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& DirectionalLightTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Light;
			t.label = "Directional Light";
			t.baseName = "directional";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			// direction is FROM-surface-TO-light (SCENE_CONVENTIONS.md) --
			// +Z is a safe default for the common +Z-looking-at-origin camera setup.
			t.chunkTexts.push_back(
				"directional_light\n"
				"{\n"
				"name @NAME@\n"
				"power 3.0\n"
				"color 1.0 1.0 1.0\n"
				"direction 0 0 1\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& SpotLightTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Light;
			t.label = "Spot Light";
			t.baseName = "spot";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"spot_light\n"
				"{\n"
				"name @NAME@\n"
				"position 0 5 0\n"
				"target 0 0 0\n"
				"color 1.0 1.0 1.0\n"
				"power 50.0\n"
				"inner 22\n"
				"outer 45\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& HosekWilkieSkylightTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Light;
			t.label = "Hosek-Wilkie Sky";
			t.baseName = "";                 // this chunk has no `name` param -- not name-addressable
			t.hasNamedIdentity = false;
			// create_sun (default TRUE) atomically creates a matching
			// directional_light named "__hw_sun__" -- that is the
			// entity CategoryEntityName(Light) will show after this
			// template lands, so it is what Instantiate reports.
			t.fixedResultName = "__hw_sun__";
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"hosek_wilkie_skylight\n"
				"{\n"
				"solar_elevation 45.0\n"
				"solar_azimuth 0.0\n"
				"turbidity 3.0\n"
				"sky_intensity_scale 1.0\n"
				"sun_intensity_scale 3.14\n"
				"create_sun TRUE\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	// -------------------------------------------------------------
	// Objects (geometry + standard_object sequence)
	// -------------------------------------------------------------
	const EntityTemplateDef& SphereObjectTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Object;
			t.label = "Sphere";
			t.baseName = "sphere";
			t.hasNamedIdentity = true;
			t.needsMaterial = true;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"sphere_geometry\n"
				"{\n"
				"name @NAME@_geo\n"
				"radius 1.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"standard_object\n"
				"{\n"
				"name @NAME@\n"
				"geometry @NAME@_geo\n"
				"material @MATERIAL@\n"
				"position 0 0 0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& BoxObjectTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Object;
			t.label = "Box";
			t.baseName = "box";
			t.hasNamedIdentity = true;
			t.needsMaterial = true;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"box_geometry\n"
				"{\n"
				"name @NAME@_geo\n"
				"width 1.0\n"
				"height 1.0\n"
				"depth 1.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"standard_object\n"
				"{\n"
				"name @NAME@\n"
				"geometry @NAME@_geo\n"
				"material @MATERIAL@\n"
				"position 0 0 0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& CylinderObjectTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Object;
			t.label = "Cylinder";
			t.baseName = "cylinder";
			t.hasNamedIdentity = true;
			t.needsMaterial = true;
			t.needsTexture = false;
			// capped defaults TRUE (closed solid) -- fine for a
			// generic Add-Entity default.
			t.chunkTexts.push_back(
				"cylinder_geometry\n"
				"{\n"
				"name @NAME@_geo\n"
				"axis y\n"
				"radius 0.5\n"
				"height 1.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"standard_object\n"
				"{\n"
				"name @NAME@\n"
				"geometry @NAME@_geo\n"
				"material @MATERIAL@\n"
				"position 0 0 0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& InfinitePlaneObjectTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Object;
			t.label = "Infinite Plane";
			t.baseName = "plane";
			t.hasNamedIdentity = true;
			t.needsMaterial = true;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"infiniteplane_geometry\n"
				"{\n"
				"name @NAME@_geo\n"
				"xtile 1.0\n"
				"ytile 1.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"standard_object\n"
				"{\n"
				"name @NAME@\n"
				"geometry @NAME@_geo\n"
				"material @MATERIAL@\n"
				"position 0 0 0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	// -------------------------------------------------------------
	// Materials (each bundles the small painter(s) its slots need)
	// -------------------------------------------------------------
	const EntityTemplateDef& NullBoundaryMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "Null Medium Boundary";
			t.baseName = "medium_boundary";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"null_boundary_material\n"
				"{\n"
				"name @NAME@\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& LambertianMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "Lambertian";
			t.baseName = "lambertian";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_reflectance\n"
				"color 0.7 0.7 0.7\n"
				"}\n" );
			t.chunkTexts.push_back(
				"lambertian_material\n"
				"{\n"
				"name @NAME@\n"
				"reflectance @NAME@_reflectance\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& LambertianLuminaireMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "Lambertian Luminaire";
			t.baseName = "luminaire";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_exitance\n"
				"color 1.0 1.0 1.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"lambertian_luminaire_material\n"
				"{\n"
				"name @NAME@\n"
				"exitance @NAME@_exitance\n"
				"scale 10.0\n"
				"material none\n"
				"}\n" );
			return t;
		}();
		return d;
	}

    const EntityTemplateDef& RoughDielectricMaterialTemplate()
    {
        static const EntityTemplateDef d = [] {
            EntityTemplateDef t;
            t.category=Category::Material; t.label="Rough Dielectric (Glass)";
            t.baseName="rough_glass"; t.hasNamedIdentity=true;
            t.needsMaterial=false; t.needsTexture=false;
            t.chunkTexts.push_back("rough_dielectric_material\n{\nname @NAME@\nior 1.5\nalphax 0.2\nalphay 0.2\n}\n");
            return t;
        }();
        return d;
    }

	const EntityTemplateDef& DielectricMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "Dielectric (Glass)";
			t.baseName = "glass";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			// tau / ior / scattering are Reference-typed slots that also
			// accept an inline literal (ResolveScalarPainterArg) -- no
			// bundled painter needed, matching scenes/pr.RISEscene's
			// `glass` material.
			t.chunkTexts.push_back(
				"dielectric_material\n"
				"{\n"
				"name @NAME@\n"
				"tau 1\n"
				"ior 1.5\n"
				"scattering 10000\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& GGXMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "GGX (Metal/Glossy)";
			t.baseName = "ggx";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			// fresnel_mode schlick_f0 treats rs as F0 directly and
			// ignores ior/extinction -- avoids needing a physically
			// plausible complex IOR pair for a generic default.
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_rd\n"
				"color 0.8 0.8 0.8\n"
				"}\n" );
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_rs\n"
				"color 0.9 0.9 0.9\n"
				"}\n" );
			t.chunkTexts.push_back(
				"ggx_material\n"
				"{\n"
				"name @NAME@\n"
				"rd @NAME@_rd\n"
				"rs @NAME@_rs\n"
				"alphax 0.15\n"
				"alphay 0.15\n"
				"fresnel_mode schlick_f0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& PerfectRefractorMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "Perfect Refractor";
			t.baseName = "refractor";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_refractance\n"
				"color 1.0 1.0 1.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"perfectrefractor_material\n"
				"{\n"
				"name @NAME@\n"
				"refractance @NAME@_refractance\n"
				"ior 1.5\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& DiffuseTransmissionMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "Diffuse Transmission";
			t.baseName = "diffuse_trans";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_reflectance\n"
				"color 0.4 0.4 0.4\n"
				"}\n" );
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_transmittance\n"
				"color 0.4 0.4 0.4\n"
				"}\n" );
			t.chunkTexts.push_back(
				"diffuse_transmission_material\n"
				"{\n"
				"name @NAME@\n"
				"reflectance @NAME@_reflectance\n"
				"transmittance @NAME@_transmittance\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& MixMaterialTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Material;
			t.label = "Mix Material";
			t.baseName = "mix";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_a_color\n"
				"color 0.7 0.7 0.7\n"
				"}\n" );
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_b_color\n"
				"color 0.2 0.2 0.2\n"
				"}\n" );
			t.chunkTexts.push_back(
				"lambertian_material\n"
				"{\n"
				"name @NAME@_a\n"
				"reflectance @NAME@_a_color\n"
				"}\n" );
			t.chunkTexts.push_back(
				"lambertian_material\n"
				"{\n"
				"name @NAME@_b\n"
				"reflectance @NAME@_b_color\n"
				"}\n" );
			t.chunkTexts.push_back(
				"mix_material\n"
				"{\n"
				"name @NAME@\n"
				"child @NAME@_a weight 0.5\n"
				"child @NAME@_b weight 0.5\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	// -------------------------------------------------------------
	// Painters
	// -------------------------------------------------------------
	const EntityTemplateDef& UniformColorPainterTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Painter;
			t.label = "Uniform Color";
			t.baseName = "color";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@\n"
				"color 0.7 0.7 0.7\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& ScalarPainterTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Painter;
			t.label = "Scalar Value";
			t.baseName = "scalar";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"scalar_painter\n"
				"{\n"
				"name @NAME@\n"
				"value 0.5\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& SpectralPainterTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Painter;
			t.label = "Spectral Curve";
			t.baseName = "spectral";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			// Inline `cp` sample points -- no file dependency, so this
			// template instantiates cleanly with no RISE_MEDIA_PATH.
			t.chunkTexts.push_back(
				"spectral_painter\n"
				"{\n"
				"name @NAME@\n"
				"nmbegin 400\n"
				"nmend 700\n"
				"cp 400 0.5\n"
				"cp 550 0.8\n"
				"cp 700 0.5\n"
				"scale 1.0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& PngPainterTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Painter;
			t.label = "Image (PNG)";
			t.baseName = "image";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = true;    // @TEXTURE@ resolved at Instantiate time
			t.chunkTexts.push_back(
				"png_painter\n"
				"{\n"
				"name @NAME@\n"
				"file @TEXTURE@\n"
				"color_space Rec709RGB_Linear\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& Perlin2DPainterTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Painter;
			t.label = "Perlin Noise";
			t.baseName = "perlin";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_a\n"
				"color 0.0 0.0 0.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_b\n"
				"color 1.0 1.0 1.0\n"
				"}\n" );
			t.chunkTexts.push_back(
				"perlin2d_painter\n"
				"{\n"
				"name @NAME@\n"
				"colora @NAME@_a\n"
				"colorb @NAME@_b\n"
				"persistence 0.5\n"
				"octaves 4\n"
				"scale 4.0 4.0\n"
				"shift 0 0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& CheckerPainterTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Painter;
			t.label = "Checker";
			t.baseName = "checker";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_a\n"
				"color 0.1 0.1 0.1\n"
				"}\n" );
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_b\n"
				"color 0.9 0.9 0.9\n"
				"}\n" );
			t.chunkTexts.push_back(
				"checker_painter\n"
				"{\n"
				"name @NAME@\n"
				"colora @NAME@_a\n"
				"colorb @NAME@_b\n"
				"size 1.0\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	// -------------------------------------------------------------
	// Media
	// -------------------------------------------------------------
	const EntityTemplateDef& HomogeneousMediumTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Medium;
			t.label = "Homogeneous Fog";
			t.baseName = "fog";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"homogeneous_medium\n"
				"{\n"
				"name @NAME@\n"
				"absorption 0.01 0.01 0.01\n"
				"scattering 0.05 0.05 0.05\n"
				"phase isotropic\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& PainterHeterogeneousMediumTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Medium;
			t.label = "Painter-Driven Volume";
			t.baseName = "volume";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"uniformcolor_painter\n"
				"{\n"
				"name @NAME@_density\n"
				"color 0.3 0.3 0.3\n"
				"}\n" );
			t.chunkTexts.push_back(
				"painter_heterogeneous_medium\n"
				"{\n"
				"name @NAME@\n"
				"absorption 0.01 0.01 0.01\n"
				"scattering 0.2 0.2 0.2\n"
				"phase isotropic\n"
				"density_painter @NAME@_density\n"
				"resolution 32\n"
				"color_to_scalar luminance\n"
				"bbox_min -1 -1 -1\n"
				"bbox_max 1 1 1\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	const EntityTemplateDef& MultichannelHeterogeneousMediumTemplate()
	{
		static const EntityTemplateDef d = []{
			EntityTemplateDef t;
			t.category = Category::Medium;
			t.label = "Fire/Smoke Multichannel Volume";
			t.baseName = "fire_volume";
			t.hasNamedIdentity = true;
			t.needsMaterial = false;
			t.needsTexture = false;
			t.chunkTexts.push_back(
				"scalar_painter\n"
				"{\n"
				"name @NAME@_carbon\n"
				"value 0.02\n"
				"}\n" );
			t.chunkTexts.push_back(
				"scalar_painter\n"
				"{\n"
				"name @NAME@_temperature\n"
				"value 1200\n"
				"}\n" );
			t.chunkTexts.push_back(
				"scalar_painter\n"
				"{\n"
				"name @NAME@_condensed\n"
				"value 0.01\n"
				"}\n" );
			t.chunkTexts.push_back(
				"multichannel_heterogeneous_medium\n"
				"{\n"
				"name @NAME@\n"
				"channel_carbon painter @NAME@_carbon\n"
				"channel_temperature painter @NAME@_temperature\n"
				"channel_condensed painter @NAME@_condensed\n"
				"chem_model none\n"
				"bake_resolution 32 32 64\n"
				"bbox_min -0.02 0 -0.02\n"
				"bbox_max 0.02 0.08 0.02\n"
				"optical_record fire_optics_v1\n"
				"}\n" );
			return t;
		}();
		return d;
	}

	// -------------------------------------------------------------
	// Per-category template lists
	// -------------------------------------------------------------
	const std::vector<const EntityTemplateDef*>& TemplatesFor( Category cat )
	{
		static const std::vector<const EntityTemplateDef*> kLight = {
			&OmniLightTemplate(), &DirectionalLightTemplate(), &SpotLightTemplate(), &HosekWilkieSkylightTemplate()
		};
		static const std::vector<const EntityTemplateDef*> kObject = {
			&SphereObjectTemplate(), &BoxObjectTemplate(), &CylinderObjectTemplate(), &InfinitePlaneObjectTemplate()
		};
		static const std::vector<const EntityTemplateDef*> kMaterial = {
			&NullBoundaryMaterialTemplate(), &LambertianMaterialTemplate(), &LambertianLuminaireMaterialTemplate(), &DielectricMaterialTemplate(), &RoughDielectricMaterialTemplate(),
			&GGXMaterialTemplate(), &PerfectRefractorMaterialTemplate(), &DiffuseTransmissionMaterialTemplate(), &MixMaterialTemplate()
		};
		static const std::vector<const EntityTemplateDef*> kPainter = {
			&UniformColorPainterTemplate(), &ScalarPainterTemplate(), &SpectralPainterTemplate(),
			&PngPainterTemplate(), &Perlin2DPainterTemplate(), &CheckerPainterTemplate()
		};
		static const std::vector<const EntityTemplateDef*> kMedium = {
			&HomogeneousMediumTemplate(), &PainterHeterogeneousMediumTemplate(),
			&MultichannelHeterogeneousMediumTemplate()
		};
		static const std::vector<const EntityTemplateDef*> kEmpty;

		switch( cat )
		{
		case Category::Light:    return kLight;
		case Category::Object:   return kObject;
		case Category::Material: return kMaterial;
		case Category::Painter:  return kPainter;
		case Category::Medium:   return kMedium;
		default:                 return kEmpty;
		}
	}
	// ================================================================
	// S18: keyword-driven default node bodies.  See EntityTemplates.h's
	// "S18" block for WHY a seed table exists alongside the descriptor.
	// ================================================================

	//! One descriptor-invisible line a minimal-valid body must carry.
	//! Repeat the same `param` for a repeatable minimum (ramp's two
	//! `stop` lines).  A seed is SKIPPED entirely when the caller
	//! supplied ANY arg for that param -- the caller's authoring wins,
	//! including the repeatable case (a caller that supplies one `stop`
	//! owns the whole stop list and gets its own derive failure, rather
	//! than a silently half-seeded ramp).
	struct NodeSeed
	{
		const char* keyword;
		const char* param;
		const char* value;
	};

	// Every entry below was chosen to be the LEAST OPINIONATED body that
	// still derives, and each is covered by the S18 "every painter and
	// material keyword creates" sweep in tests/EntityTemplatesTest.cpp --
	// if a future chunk grows a minimal-validity rule its descriptor
	// cannot express, that sweep fails and points here.
	const NodeSeed kNodeSeeds[] = {
		// scalar_painter: thirteen mutually-exclusive forms, none marked
		// `required`.  Form 1 (UniformScalarPainter) is the only one with
		// no dependency on another chunk or a file on disk.
		{ "scalar_painter",        "value", "0.5" },

		// ramp_painter: Finalize hard-fails below two `stop` lines.  A
		// black->white ramp is the identity-shaped starting point a user
		// then edits, and it is colour-space independent.
		{ "ramp_painter",          "stop",  "0.0 0.0 0.0 0.0" },
		{ "ramp_painter",          "stop",  "1.0 1.0 1.0 1.0" },

		// spectral_painter: round-1 P2-a.  Finalize hard-fails when both
		// `wavelengths` and `amplitudes` end up empty (ChunkParserRegistry.cpp,
		// the "no samples (empty / all-comment file, no inline cp)" error) --
		// but `cp` (repeatable) is a perfectly usable static default, exactly
		// the ramp_painter repeatable-minimum shape above, and needs no
		// RISE_MEDIA_PATH-relative file.  A flat mid-grey curve (same
		// amplitude at both ends) is the least-opinionated starting shape;
		// this used to be listed in kNodeExtraRequirements as a `file`
		// requirement, which made the keyword UNCREATABLE with no caller
		// input even though `file` was never actually required.
		{ "spectral_painter",      "cp",    "400 0.5" },
		{ "spectral_painter",      "cp",    "700 0.5" },

		// The two expression chunks: `expr` is `required` but is a STRING
		// (no reference to any other chunk), so a static default exists
		// and the node is creatable with no caller input.  Mid-gray (0.5),
		// independently chosen -- NOT the same literal
		// EntityTemplates::DefaultPainterChunkText picks (0.7); the two
		// have no requirement to match.
		{ "expression_painter",    "expr",  "vec3(0.5, 0.5, 0.5)" },
		{ "expression_function2d", "expr",  "0.5" },

		// voronoi2d/3d: Job::AddVoronoi{2,3}DPainter hard-fails below TWO
		// generators, and `gen` is `repeatable`, never `required` -- the
		// repeatable-minimum shape again.  The painter token is left at
		// the `none` sentinel, exactly as blend_painter's colora/colorb
		// and every other optional painter slot default to; the user
		// re-wires the cells on the canvas.
		{ "voronoi2d_painter",     "gen",   "0.25 0.25 none" },
		{ "voronoi2d_painter",     "gen",   "0.75 0.75 none" },
		{ "voronoi3d_painter",     "gen",   "0.25 0.25 0.25 none" },
		{ "voronoi3d_painter",     "gen",   "0.75 0.75 0.75 none" },

		// SCALAR-PIPE slots.  Historically (pre-fix) these four Finalize()s
		// defaulted straight to the `none` painter name -- `none` resolves
		// only in the COLOUR manager, so ResolveOrDiagnoseScalar rejected it
		// ("bound to `IPainter` chunk `none`; this slot now requires a
		// scalar_painter" -- docs/ISCALARPAINTER_REFACTOR.md), and a BARE
		// chunk (this param omitted) hard-failed to parse at all.  Fixed:
		// each Finalize's own default is now the numeric literal `0.0`,
		// reproducing the pre-refactor "none" IPainter default (black) bit-
		// for-bit, so a bare chunk parses and applies cleanly without this
		// table's help.  The seeds below are kept anyway, NOT as a
		// parse-failure workaround any more, but because `0.0` is a
		// visually UNDERWHELMING default for three of these four (no coat /
		// no extinction) -- an agent-created node is more useful for canvas
		// exploration with a value that actually shows the effect.  (`g 0.0`
		// on generic_human_tissue_material is the odd one out: isotropic
		// scattering is NOT inert -- with `sca` at its own numeric default
		// it is an active, visible phase function -- kept seeded anyway
		// purely for symmetry with the physically-recommended isotropic
		// default, not because 0.0 needed rescuing here the way it does for
		// tau/ext.  The other scalar slots on these same chunks already
		// default to a numeric literal in their own Finalize and need
		// nothing here.)
		// DL-401: `polished_material` used to be seeded here (`tau 1.0`, a
		// visible coat).  It is DEPRECATED now (DL-323 follow-through), so
		// the create path no longer dresses it up; the modern replacement,
		// `coated_material` over a `lambertian_material`, needs NO seed --
		// its required `base` is a Material reference the create verb
		// already satisfies with a Lambertian stand-in (S18g), and its
		// descriptor defaults are a full-coverage clear coat that shows.
		// A bare `polished_material` still derives (`tau` defaults to the
		// literal 0.0, i.e. no coat reflection).
		{ "dielectric_material",   "tau",        "1.0" },
		{ "translucent_material",  "ext",        "1.0" },
		{ "generic_human_tissue_material", "g",  "0.0" },

		// hair_material: a ONE-OF FORM SELECTION the descriptor cannot
		// express, same shape as scalar_painter's thirteen forms above --
		// `color` / `sigma_a` / `eumelanin` / `pheomelanin` all default to
		// the `none` sentinel (tier not bound), and Job::AddHairMaterial
		// requires EXACTLY ONE of the three tiers (color / sigma_a /
		// melanin) to end up bound; none of the four params is marked
		// `required`, so NodeRequirements() sees nothing missing and a
		// bare create would derive-fail with the tier-exclusivity
		// diagnostic.  Seed the Tier-1 melanin path (the physically-based,
		// recommended default per the descriptor's own text) with
		// `eumelanin 1.3` -- ~= brown-black hair, the standard Chiang
		// et al. 2016 mid-brown default -- rather than a synthetic `color`
		// or `sigma_a`, so an agent-inserted hair material renders
		// sensibly out of the box.
		{ "hair_material",         "eumelanin", "1.3" },
		// mix_material: Finalize requires at least one `child` entry.
		{ "mix_material",          "child",     "none weight 1.0" },
	};

	//! Requirements the descriptor's `required` flag does not carry, so
	//! the caller (the S21 canvas) is told about them BEFORE the create
	//! rather than after a failed derive.  Two families:
	//!
	//! FILES.  A keyword whose minimal body needs a file on disk that no
	//! static default can supply.
	//!
	//! Two reasons this is a keyword table and not "every
	//! ValueKind::Filename parameter".  (1) Several Filename params in
	//! scope are genuinely OPTIONAL -- `scalar_painter`'s `file` is one
	//! of thirteen mutually-exclusive forms (we seed a different one), and
	//! `voronoi2d_painter` / `voronoi3d_painter` take an optional
	//! generator list -- so promoting them would make creatable nodes
	//! uncreatable.  (2) The failure mode without this is not a tidy
	//! refusal: the image readers are handed the literal string "none",
	//! and OpenEXR's reader THROWS (Iex::InputExc, "Unable to open
	//! 'none' for read") out through the derive, which is an abort, not
	//! a diagnostic.  Catching it here -- before any insert runs -- is
	//! what keeps "a refused create leaves the Document byte-identical"
	//! true for these keywords rather than "a refused create takes the
	//! process down".  The S21 canvas supplies the path from its file
	//! picker, exactly as the Add-Entity png template already does.
	//!
	//! REFERENCES.  A slot whose Finalize hard-fails on the `none`
	//! sentinel because the value must be a LIVE object of a specific
	//! runtime kind (an IFunction2D), which no seed can conjure -- the
	//! same "must name another chunk in THIS scene" situation as the
	//! descriptor-`required` references, just undeclared.  Marked
	//! `isReference` so the canvas resolves it from the drag context.
	struct NodeExtraRequirement
	{
		const char* keyword;
		const char* param;
		bool        isReference;
		const char* description;
	};
	const NodeExtraRequirement kNodeExtraRequirements[] = {
		{ "png_painter",         "file",     false, "Path to a PNG image file" },
		{ "jpg_painter",         "file",     false, "Path to a JPEG image file" },
		{ "hdr_painter",         "file",     false, "Path to a Radiance HDR image file" },
		{ "exr_painter",         "file",     false, "Path to an OpenEXR image file" },
		{ "tiff_painter",        "file",     false, "Path to a TIFF image file" },
		{ "datadriven_material", "filename", false, "Path to a measured-BRDF data file" },
		// HyLIoS P5: the default `baked_local` transport reads a baked local
		// response asset and Finalize refuses an empty path (the other modes
		// need a `specimen_source` file instead, so no mode is file-free).
		{ "hylios_skin_material", "response_asset", false, "Path to a baked HyLIoS local response asset (.hylios) for the default baked_local transport" },

		// Job::AddCompositeFunction2DPainter (child_a/child_b) and
		// Job::AddFunction2DColorPainter (function2d) both look the name up
		// in pFunc2DManager and reject `none` / an unregistered name.
		// round-1 P3: that is NOT the narrow "must implement IFunction2D"
		// set the old comment here claimed.  Job.cpp's `RegisterPainterDual`
		// dual-indexes EVERY successfully-added colour painter into BOTH
		// pPntManager AND pFunc2DManager, with exactly two documented
		// exceptions (`expression_painter`, single-registered; and
		// `scalar_painter`, a wholly separate IScalarPainterManager that
		// never touches either) -- see ConnectionLegality::IsFunction2DCapable
		// for the authoritative list.  So the real requirement these three
		// undeclared slots share with the descriptor-`required` reference
		// slots above is simply "must name an already-created, non-none
		// chunk in THIS scene" -- not a narrower painter-kind restriction.
		// ConnectionLegality (S17) is what the canvas should filter the
		// candidate list with (it already encodes the real rule, not this
		// table's prose).
		{ "composite_function2d_painter", "child_a",    true, "First operand: any colour painter in this scene" },
		{ "composite_function2d_painter", "child_b",    true, "Second operand: any colour painter in this scene" },
		{ "function2d_painter",           "function2d", true, "The named painter to wrap as a greyscale colour" },

	};

	// True iff `args` mentions `param` at all.
	bool ArgsMention( const std::vector<EntityTemplates::ChunkNodeArg>& args, const std::string& param )
	{
		for( const EntityTemplates::ChunkNodeArg& a : args )
			if( a.param == param ) return true;
		return false;
	}

	// A value is emittable as a chunk parameter line iff it stays on one
	// line and cannot terminate the chunk body.  Rejecting `}` as well as
	// the newlines is what makes the caller's argument strictly a VALUE:
	// without it a canvas arg could splice arbitrary chunks into the
	// document through a verb whose whole contract is "one node".
	bool ValueIsSingleLine( const std::string& v )
	{
		return v.find_first_of( "\n\r}{" ) == std::string::npos;
	}

	// The descriptor parameter named `pname` on `desc`, or null.
	const ParameterDescriptor* FindParam( const ChunkDescriptor& desc, const std::string& pname )
	{
		for( const ParameterDescriptor& p : desc.parameters )
			if( p.name == pname ) return &p;
		return nullptr;
	}

}   // anonymous namespace

std::vector<EntityTemplates::ChunkNodeRequirement>
EntityTemplates::NodeRequirements( const std::string& keyword )
{
	std::vector<ChunkNodeRequirement> out;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( keyword.c_str() ) );
	if( !desc ) return out;
	if( desc->category != ChunkCategory::Painter && desc->category != ChunkCategory::Material )
		return out;

	for( const ParameterDescriptor& p : desc->parameters )
	{
		if( !p.required ) continue;
		if( p.name == "name" ) continue;   // this verb picks the name itself

		// A required param the SEED TABLE covers is not the caller's
		// problem -- it has a static default (expression_painter.expr).
		bool seeded = false;
		for( const NodeSeed& s : kNodeSeeds )
			if( keyword == s.keyword && p.name == s.param ) { seeded = true; break; }
		if( seeded ) continue;

		ChunkNodeRequirement r;
		r.param               = p.name;
		r.description         = p.description;
		r.isReference = ( p.kind == ValueKind::Reference );
		if( r.isReference ) r.referenceCategories = p.referenceCategories;
		out.push_back( r );
	}

	// The requirements the descriptor cannot flag (see the table's own
	// doc).  Appended, never duplicated: skip a param the descriptor
	// loop above already reported.
	for( const NodeExtraRequirement& f : kNodeExtraRequirements )
	{
		if( keyword != f.keyword ) continue;
		bool already = false;
		for( const ChunkNodeRequirement& r : out ) if( r.param == f.param ) { already = true; break; }
		if( already ) continue;
		// Only advertise a param the chunk actually declares -- a typo in
		// this table must not manufacture an unsatisfiable requirement.
		const ParameterDescriptor* pd = FindParam( *desc, f.param );
		if( !pd ) continue;
		ChunkNodeRequirement r;
		r.param       = f.param;
		r.description = f.description;
		r.isReference = f.isReference;
		// Carry the descriptor's own legal-category set for a reference
		// slot, so the canvas filters candidates from ONE source of
		// truth rather than from this table's prose.
		if( f.isReference ) r.referenceCategories = pd->referenceCategories;
		out.push_back( r );
	}
	return out;
}

bool EntityTemplates::BuildNodeChunkText( const std::string& keyword,
                                          const std::string& name,
                                          const std::vector<ChunkNodeArg>& args,
                                          std::string& outText,
                                          std::string& outDiag )
{
	outDiag.clear();

	const ChunkDescriptor* desc = DescriptorForKeyword( String( keyword.c_str() ) );
	if( !desc )
	{
		outDiag = "unknown chunk keyword `" + keyword + "`";
		return false;
	}
	if( desc->category != ChunkCategory::Painter && desc->category != ChunkCategory::Material )
	{
		outDiag = "`" + keyword + "` is not a painter or material chunk -- "
		          "this verb creates node-graph nodes only";
		return false;
	}
	if( name.empty() || !ValueIsSingleLine( name ) )
	{
		outDiag = "the generated node name is empty or not a single-line identifier";
		return false;
	}
	if( !FindParam( *desc, "name" ) )
	{
		// Every painter/material chunk in the registry declares `name`;
		// a future one that does not could not be addressed, wired, or
		// removed by name, so refuse rather than emit an unnamed node.
		outDiag = "`" + keyword + "` declares no `name` parameter -- it cannot be a named graph node";
		return false;
	}

	// Validate every caller arg against the descriptor BEFORE emitting a
	// byte: an undeclared parameter name would be refused two layers down
	// by the parser's own kUndeclaredParameterFmt diagnostic, but only
	// after the whole insert pipeline had run.  Catching it here keeps the
	// refusal cheap and lets the message name the offending param.
	for( const ChunkNodeArg& a : args )
	{
		if( a.param.empty() || !ValueIsSingleLine( a.param ) )
		{
			outDiag = "an argument carries an empty or malformed parameter name";
			return false;
		}
		if( a.param == "name" )
		{
			// The name is this verb's to pick (it is what makes the
			// result collision-safe); accepting a caller `name` would
			// silently defeat the dedup and hand back a name the caller
			// did not get told about.
			outDiag = "`name` is chosen by this verb -- pass the desired base name instead";
			return false;
		}
		if( !FindParam( *desc, a.param ) )
		{
			outDiag = "`" + keyword + "` declares no parameter named `" + a.param + "`";
			return false;
		}
		if( a.value.empty() )
		{
			// round-1 P3: refuse directly, naming the offending arg, rather
			// than silently falling through to the emit loop below -- which
			// used to write just the param with no value at all (`text +=
			// a.param` with the `!a.value.empty()` guard skipping the
			// value), a line the parser then rejects two layers down with a
			// diagnostic that never names WHICH argument was empty.  Same
			// byte-identical-on-refusal contract as every other check in
			// this loop: caught before a single byte is composed.
			outDiag = "the value for `" + a.param + "` is empty -- omit the argument instead of passing an empty value";
			return false;
		}
		if( !ValueIsSingleLine( a.value ) )
		{
			outDiag = "the value for `" + a.param + "` must be a single line with no braces";
			return false;
		}
	}

	// Required params with no seed and no caller arg -- the honest refusal
	// (a ramp_painter with no `input` cannot be created out of nothing).
	const std::vector<ChunkNodeRequirement> needs = NodeRequirements( keyword );
	for( const ChunkNodeRequirement& n : needs )
	{
		if( ArgsMention( args, n.param ) ) continue;
		outDiag = "`" + keyword + "` requires `" + n.param + "`"
		        + ( n.description.empty() ? std::string() : ( " (" + n.description + ")" ) )
		        + " -- supply it as a creation argument";
		return false;
	}

	// Emit.  Braces on their own lines (the documented v7 authoring
	// convention, and the shape ApplyCstInsertChunk's grammar checks
	// expect), name first, then the caller's args in the order given,
	// then the seeds the caller did not override.
	std::string text;
	text += keyword;
	text += "\n{\nname ";
	text += name;
	text += "\n";
	for( const ChunkNodeArg& a : args )
	{
		text += a.param;
		if( !a.value.empty() ) { text += " "; text += a.value; }
		text += "\n";
	}
	for( const NodeSeed& s : kNodeSeeds )
	{
		if( keyword != s.keyword ) continue;
		if( ArgsMention( args, s.param ) ) continue;
		// Defensive: a seed naming a parameter the chunk does not declare
		// would make EVERY create of that keyword fail the parse with an
		// "undeclared parameter" diagnostic pointing at a line the user
		// never wrote.  Skip it; the keyword sweep in
		// tests/EntityTemplatesTest.cpp is what actually catches the typo
		// (the derive then fails for the seed's original reason).
		if( !FindParam( *desc, s.param ) ) continue;
		text += s.param;
		text += " ";
		text += s.value;
		text += "\n";
	}
	text += "}\n";

	outText.swap( text );
	return true;
}

unsigned int EntityTemplates::Count( Category cat )
{
	return static_cast<unsigned int>( TemplatesFor( cat ).size() );
}

const EntityTemplateDef* EntityTemplates::At( Category cat, unsigned int idx )
{
	const std::vector<const EntityTemplateDef*>& v = TemplatesFor( cat );
	if( idx >= v.size() ) return nullptr;
	return v[idx];
}

std::string EntityTemplates::DefaultPainterChunkText( const std::string& name )
{
	std::string s;
	s += "uniformcolor_painter\n{\nname " + name + "\ncolor 0.7 0.7 0.7\n}\n";
	return s;
}

std::string EntityTemplates::DefaultLambertianChunkText( const std::string& name, const std::string& painterName )
{
	std::string s;
	s += "lambertian_material\n{\nname " + name + "\nreflectance " + painterName + "\n}\n";
	return s;
}

namespace
{
const std::vector<unsigned char>& DefaultTextureBytes()
{
	// Encode the known thumbnail once. Cache validation compares only this
	// bounded payload, so partial or unrelated files are never reused.
	static const std::vector<unsigned char> encoded = [] {
		const auto release = []( const IReference* reference ) { reference->release(); };
		IRasterImage* rawImage = nullptr;
		RISE_API_CreateRISEColorRasterImage( &rawImage, 4, 4, RISEColor( RISEPel( 0.6, 0.6, 0.6 ), 1.0 ) );
		std::unique_ptr<IRasterImage, decltype(release)> image( rawImage, release );
		IMemoryBuffer* rawBuffer = nullptr;
		RISE_API_CreateMemoryBuffer( &rawBuffer );
		std::unique_ptr<IMemoryBuffer, decltype(release)> buffer( rawBuffer, release );
		if( !image || !buffer ) throw std::runtime_error("Could not create default PNG storage");
		IRasterImageWriter* rawWriter = nullptr;
		RISE_API_CreatePNGWriter( &rawWriter, *buffer, 8, eColorSpace_Rec709RGB_Linear );
		std::unique_ptr<IRasterImageWriter, decltype(release)> writer( rawWriter, release );
		if( !writer ) throw std::runtime_error("Could not create default PNG writer");
		image->DumpImage( writer.get() );
		// PNGWriter reports encoding failures by exception. Also verify the
		// known payload ends with IEND before accepting it for cache repair.
		constexpr std::array<unsigned char, 12> endChunk = {
			0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82
		};
		const unsigned int size = buffer->getCurPos();
		const auto* bytes = reinterpret_cast<const unsigned char*>( buffer->Pointer() );
		if( size < endChunk.size() ||
			!std::equal( endChunk.begin(), endChunk.end(), bytes + size - endChunk.size() ) )
			throw std::runtime_error("Default PNG encoding did not complete");
		return std::vector<unsigned char>( bytes, bytes + size );
	}();
	return encoded;
}

#ifndef _WIN32
class TextureDescriptor
{
public:
	explicit TextureDescriptor( int descriptor ) : fd( descriptor ) {}
	~TextureDescriptor() { if( fd >= 0 ) close( fd ); }
	TextureDescriptor( const TextureDescriptor& ) = delete;
	TextureDescriptor& operator=( const TextureDescriptor& ) = delete;
	int fd;
};

bool HasSafeTextureACL( int fd )
{
#ifdef __APPLE__
	// macOS extended ALLOW entries can bypass mode bits and can propagate
	// onto newly created children. Refuse them rather than editing any ACL.
	// On an existing opened inode, ENOENT means there is no extended ACL.
	acl_t acl = acl_get_fd_np( fd, ACL_TYPE_EXTENDED );
	if( !acl ) return errno == ENOENT;
	bool safe = acl_valid( acl ) == 0;
	if( safe ) {
		acl_entry_t entry;
		int next = ACL_FIRST_ENTRY;
		for( ;; ) {
			if( acl_get_entry( acl, next, &entry ) != 0 ) {
				// Darwin reports end-of-ACL as -1/EINVAL, including an
				// empty ACL. The ACL itself was validated above.
				safe = errno == EINVAL;
				break;
			}
			acl_tag_t tag;
			if( acl_get_tag_type( entry, &tag ) != 0 || tag != ACL_EXTENDED_DENY ) {
				safe = false;
				break;
			}
			next = ACL_NEXT_ENTRY;
		}
	}
	acl_free( acl );
	return safe;
#else
	return fd >= 0;
#endif
}

bool IsOwnedTextureFile( int fd )
{
	struct stat info{};
	return fd >= 0 && fstat( fd, &info ) == 0 && S_ISREG( info.st_mode ) &&
		info.st_uid == geteuid() && info.st_nlink == 1 && (info.st_mode & 0022) == 0 && HasSafeTextureACL( fd );
}

bool IsTrustedTextureDirectory( int fd )
{
	struct stat info{};
	return fd >= 0 && fstat( fd, &info ) == 0 && S_ISDIR( info.st_mode ) &&
		(info.st_uid == geteuid() || info.st_uid == 0) &&
		((info.st_mode & 0022) == 0 || (info.st_mode & S_ISVTX) != 0) && HasSafeTextureACL( fd );
}

int OpenTrustedTextureRoot( const std::string& canonicalParent )
{
	TextureDescriptor current( open( "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC ) );
	if( !IsTrustedTextureDirectory( current.fd ) ) return -1;
	for( const auto& component : std::filesystem::path( canonicalParent ).relative_path() ) {
		if( component.empty() ) continue;
		const int next = openat( current.fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
		if( current.fd >= 0 ) close( current.fd );
		current.fd = next;
		if( !IsTrustedTextureDirectory( current.fd ) ) return -1;
	}
	const int result = current.fd;
	current.fd = -1;
	return result;
}

bool TextureMatches( int fd, const std::vector<unsigned char>& encoded )
{
	std::array<unsigned char, 256> actual{};
	std::size_t offset = 0;
	while( offset < encoded.size() ) {
		const std::size_t remaining = encoded.size() - offset;
		const std::size_t amount = remaining < actual.size() ? remaining : actual.size();
		const ssize_t count = read( fd, actual.data(), amount );
		if( count < 0 && errno == EINTR ) continue;
		if( count <= 0 || !std::equal( actual.begin(), actual.begin() + count, encoded.begin() + offset ) ) return false;
		offset += static_cast<std::size_t>( count );
	}
	ssize_t count;
	do { count = read( fd, actual.data(), 1 ); } while( count < 0 && errno == EINTR );
	return count == 0;
}

bool EnsurePrivateTexture( const std::string& parent, std::string& path,
	const std::vector<unsigned char>& encoded )
{
	// Anchor every operation to opened directories. An lstat(path) followed
	// by an ordinary pathname write would still follow a substituted link.
	const TextureDescriptor parentDir( OpenTrustedTextureRoot( parent ) );
	if( parentDir.fd < 0 ) return false;
	const std::string cacheName = "rise_entity_templates_" + std::to_string( geteuid() );
	if( mkdirat( parentDir.fd, cacheName.c_str(), 0700 ) != 0 && errno != EEXIST ) return false;
	const TextureDescriptor cacheDir( openat( parentDir.fd, cacheName.c_str(),
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC ) );
	struct stat info{};
	if( cacheDir.fd < 0 || fstat( cacheDir.fd, &info ) != 0 || !S_ISDIR( info.st_mode ) ||
		info.st_uid != geteuid() || (info.st_mode & 0077) != 0 || !HasSafeTextureACL( cacheDir.fd ) ) return false;
	constexpr const char* filename = "rise_entity_template_default_texture.png";
	path = parent + cacheName + "/" + filename;
	{
		const TextureDescriptor input( openat( cacheDir.fd, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC ) );
		if( input.fd >= 0 ) {
			if( !IsOwnedTextureFile( input.fd ) ) return false;
			if( TextureMatches( input.fd, encoded ) ) return true;
		} else if( errno != ENOENT ) return false;
	}
	// Open without truncating, then validate that same descriptor before
	// changing any bytes. Refuse symlinks, hardlinks, and foreign-owned files.
	const TextureDescriptor output( openat( cacheDir.fd, filename,
		O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600 ) );
	if( !IsOwnedTextureFile( output.fd ) ) return false;
	int locked;
	do { locked = flock( output.fd, LOCK_EX ); } while( locked != 0 && errno == EINTR );
	if( locked != 0 || !IsOwnedTextureFile( output.fd ) ) return false;
	// Another process may have repaired this same inode after our first
	// read. Recheck under the writer lock so a stale validator never
	// truncates a PNG that a successful caller is about to consume.
	if( TextureMatches( output.fd, encoded ) ) return true;
	if( ftruncate( output.fd, 0 ) != 0 || lseek( output.fd, 0, SEEK_SET ) != 0 ) return false;
	std::size_t offset = 0;
	while( offset < encoded.size() ) {
		const ssize_t count = write( output.fd, encoded.data() + offset, encoded.size() - offset );
		if( count < 0 && errno == EINTR ) continue;
		if( count <= 0 ) return false;
		offset += static_cast<std::size_t>( count );
	}
	return fsync( output.fd ) == 0;
}
#else
class TextureHandle
{
public:
	explicit TextureHandle( HANDLE value ) : handle( value ) {}
	~TextureHandle() { if( handle && handle != INVALID_HANDLE_VALUE ) CloseHandle( handle ); }
	TextureHandle( const TextureHandle& ) = delete;
	TextureHandle& operator=( const TextureHandle& ) = delete;
	const HANDLE handle;
};

bool IsOwnedTextureFile( HANDLE file )
{
	BY_HANDLE_FILE_INFORMATION info{};
	if( file == INVALID_HANDLE_VALUE || GetFileType( file ) != FILE_TYPE_DISK ||
		!GetFileInformationByHandle( file, &info ) || info.nNumberOfLinks != 1 ||
		(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ) return false;
	HANDLE rawToken = nullptr;
	if( !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &rawToken ) ) return false;
	const TextureHandle token( rawToken );
	DWORD size = 0;
	GetTokenInformation( token.handle, TokenUser, nullptr, 0, &size );
	if( size == 0 ) return false;
	std::vector<unsigned char> tokenBytes( size );
	if( !GetTokenInformation( token.handle, TokenUser, tokenBytes.data(), size, &size ) ) return false;
	PSID owner = nullptr;
	PSECURITY_DESCRIPTOR security = nullptr;
	if( GetSecurityInfo( file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
		&owner, nullptr, nullptr, nullptr, &security ) != ERROR_SUCCESS ) return false;
	const bool owned = owner && EqualSid( owner, reinterpret_cast<TOKEN_USER*>( tokenBytes.data() )->User.Sid );
	LocalFree( security );
	return owned;
}

bool TextureMatches( HANDLE file, const std::vector<unsigned char>& encoded )
{
	std::array<unsigned char, 256> actual{};
	std::size_t offset = 0;
	while( offset < encoded.size() ) {
		const std::size_t remaining = encoded.size() - offset;
		const DWORD amount = static_cast<DWORD>( remaining < actual.size() ? remaining : actual.size() );
		DWORD count = 0;
		if( !ReadFile( file, actual.data(), amount, &count, nullptr ) || count == 0 ||
			!std::equal( actual.begin(), actual.begin() + count, encoded.begin() + offset ) ) return false;
		offset += count;
	}
	DWORD count = 0;
	return ReadFile( file, actual.data(), 1, &count, nullptr ) && count == 0;
}

bool EnsurePrivateTexture( const std::string& parent, std::string& path,
	const std::vector<unsigned char>& encoded )
{
	// Windows checks the opened leaf's reparse flag and token owner before
	// reading/truncating that handle; no pathname check/write window exists.
	path = parent + "rise_entity_template_default_texture.png";
	const auto nativePath = std::filesystem::path( path ).wstring();
	{
		const TextureHandle input( CreateFileW( nativePath.c_str(), GENERIC_READ | READ_CONTROL,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr ) );
		if( input.handle != INVALID_HANDLE_VALUE ) {
			if( !IsOwnedTextureFile( input.handle ) ) return false;
			if( TextureMatches( input.handle, encoded ) ) return true;
		} else if( GetLastError() != ERROR_FILE_NOT_FOUND ) return false;
	}
	const TextureHandle output( CreateFileW( nativePath.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
		0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr ) );
	if( !IsOwnedTextureFile( output.handle ) ) return false;
	if( TextureMatches( output.handle, encoded ) ) return true;
	LARGE_INTEGER start{};
	if( !SetFilePointerEx( output.handle, start, nullptr, FILE_BEGIN ) || !SetEndOfFile( output.handle ) ) return false;
	std::size_t offset = 0;
	while( offset < encoded.size() ) {
		DWORD count = 0;
		if( !WriteFile( output.handle, encoded.data() + offset,
			static_cast<DWORD>( encoded.size() - offset ), &count, nullptr ) || count == 0 ) return false;
		offset += count;
	}
	return FlushFileBuffers( output.handle ) != 0;
}
#endif
}

std::string EntityTemplates::EnsureDefaultTextureFile()
{
	const char* tmpEnv = std::getenv( "TMPDIR" );
	if( !tmpEnv || !tmpEnv[0] ) tmpEnv = std::getenv( "TEMP" );
	if( !tmpEnv || !tmpEnv[0] ) tmpEnv = std::getenv( "TMP" );
	std::error_code pathError;
	const auto absoluteRoot = std::filesystem::absolute( (tmpEnv && tmpEnv[0]) ? tmpEnv : "/tmp", pathError );
	if( pathError ) return std::string();
	const auto canonicalRoot = std::filesystem::canonical( absoluteRoot, pathError );
	if( pathError ) return std::string();
	std::string dir = canonicalRoot.generic_string();
	if( dir.empty() ) return std::string();
	if( dir.back() != '/' ) dir += '/';

	const std::vector<unsigned char>* encodedPointer = nullptr;
	try {
		// A throwing static initializer retries after a transient encoding failure.
		encodedPointer = &DefaultTextureBytes();
	} catch( const std::exception& failure ) {
		GlobalLog()->PrintEx(eLog_Warning, "EntityTemplates: default PNG encoding failed: %s", failure.what());
		return std::string();
	}
	std::string path;
	return EnsurePrivateTexture( dir, path, *encodedPointer ) ? path : std::string();

}

}   // namespace RISE
