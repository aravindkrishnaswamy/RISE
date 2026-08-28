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
		// scalar_painter: twelve mutually-exclusive forms, none marked
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
		{ "polished_material",     "tau",        "1.0" },
		{ "dielectric_material",   "tau",        "1.0" },
		{ "translucent_material",  "ext",        "1.0" },
		{ "generic_human_tissue_material", "g",  "0.0" },

		// hair_material: a ONE-OF FORM SELECTION the descriptor cannot
		// express, same shape as scalar_painter's twelve forms above --
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
	//! of twelve mutually-exclusive forms (we seed a different one), and
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

		// voronoi2d/3d `border`: its Finalize maps the `none` sentinel to
		// a NULL pointer (`border=="none" ? 0 : ...`), and
		// Job::AddVoronoi{2,3}DPainter then fails the null border lookup.
		// So unlike every other optional painter slot, this one has no
		// usable "unbound" value at all -- it is a required reference the
		// descriptor never marked.
		{ "voronoi2d_painter",            "border",     true, "Border colour painter (the `none` sentinel is not accepted here)" },
		{ "voronoi3d_painter",            "border",     true, "Border colour painter (the `none` sentinel is not accepted here)" },
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
