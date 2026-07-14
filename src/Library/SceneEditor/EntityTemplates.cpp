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
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Utilities/Color/Color.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

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
			&LambertianMaterialTemplate(), &LambertianLuminaireMaterialTemplate(), &DielectricMaterialTemplate(),
			&GGXMaterialTemplate(), &PerfectRefractorMaterialTemplate()
		};
		static const std::vector<const EntityTemplateDef*> kPainter = {
			&UniformColorPainterTemplate(), &ScalarPainterTemplate(), &SpectralPainterTemplate(),
			&PngPainterTemplate(), &Perlin2DPainterTemplate(), &CheckerPainterTemplate()
		};
		static const std::vector<const EntityTemplateDef*> kMedium = {
			&HomogeneousMediumTemplate(), &PainterHeterogeneousMediumTemplate()
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
}   // anonymous namespace

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

std::string EntityTemplates::EnsureDefaultTextureFile()
{
	const char* tmpEnv = std::getenv( "TMPDIR" );
	if( !tmpEnv || !tmpEnv[0] ) tmpEnv = std::getenv( "TEMP" );
	if( !tmpEnv || !tmpEnv[0] ) tmpEnv = std::getenv( "TMP" );
	std::string dir = ( tmpEnv && tmpEnv[0] ) ? tmpEnv : "/tmp";
	if( dir.back() != '/' && dir.back() != '\\' ) dir += '/';
	const std::string path = dir + "rise_entity_template_default_texture.png";

	// Idempotent: reuse an existing file from a prior call in this
	// (or an earlier) process rather than rewriting every time.
	struct stat st;
	if( ::stat( path.c_str(), &st ) == 0 && st.st_size > 0 )
		return path;

	IRasterImage* img = nullptr;
	if( !RISE_API_CreateRISEColorRasterImage( &img, 4, 4, RISEColor( RISEPel( 0.6, 0.6, 0.6 ), 1.0 ) ) || !img )
		return std::string();

	IWriteBuffer* buf = nullptr;
	if( !RISE_API_CreateDiskFileWriteBuffer( &buf, path.c_str() ) || !buf )
	{
		img->release();
		return std::string();
	}

	IRasterImageWriter* writer = nullptr;
	const bool madeWriter = RISE_API_CreatePNGWriter( &writer, *buf, 8, eColorSpace_Rec709RGB_Linear ) && writer;
	if( madeWriter )
	{
		img->DumpImage( writer );   // void -- verify success via stat() below
		writer->release();
	}
	buf->release();
	img->release();

	if( !madeWriter || ::stat( path.c_str(), &st ) != 0 || st.st_size <= 0 )
		return std::string();
	return path;
}

}   // namespace RISE
