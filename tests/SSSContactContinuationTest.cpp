// DL-408: an SSS exit against a COINCIDENT neighbour (contact continuation).
//
// Every row compares a touching scene against its twin with a 2e-6 gap
// (wider than the 1e-6 outward offset an SSS exit gives its continuation,
// optically nothing), with PAIRED salts (ValueSalt, common random numbers),
// n = 8 salted renders per side.  Under the white environment with
// conservative materials the gapped twin is the oracle (the diffusion
// profile is not energy conserving on a finite cube, so its absolute value
// is not 1); rows whose every material conserves energy are also gated
// against the absolute furnace value 1.
//
// Pre-fix (master ba133dd38) the diffusion rows read 0.90 of their twins,
// the reversed random-walk / Lambertian pair 0.9968; the fix is the contact
// launch (docs/DL408_SSS_CONTACT_CONTINUATION.md).  Default: gate (exit 1
// on any failing row); `--measure` prints only.  `--only <substr>` filters.
#define main SSSExteriorSuiteMain
#include "SSSExteriorIndexInvarianceTest.cpp"
#undef main
#include <cstdlib>
#include <functional>

namespace
{
	enum class Rast { PT, BDPT, PTSpectral, VCM, HWSS, MLT };

	const char* RastName( Rast r )
	{
		switch( r ) {
			case Rast::PT: return "PT";
			case Rast::BDPT: return "BDPT";
			case Rast::PTSpectral: return "PT-spectral";
			case Rast::VCM: return "VCM";
			case Rast::HWSS: return "PT-spectral hwss";
			case Rast::MLT: return "MLT";
		}
		return "?";
	}

	//! Replaces the scene's rasterizer chunk with `r`'s.
	//! `clamp` > 0 adds a direct/indirect contribution clamp: the diffusion
	//! profile on a finite cube is heavy-tailed (one salt of the PT
	//! diffusion|diffusion pair read 2.4 against a mean of 1.0), and the
	//! clamp is identical in both twins, so the contact/gap ratio stays the
	//! oracle while its noise drops.
	bool SetRasterizer( std::string& scene, Rast r, unsigned int samples, double clamp = 0 )
	{
		const char* names[] = { "pathtracing_pel_rasterizer\n{", "pathtracing_spectral_rasterizer\n{" };
		std::size_t begin = std::string::npos;
		for( const char* n : names ) {
			begin = scene.find( n );
			if( begin != std::string::npos ) break;
		}
		if( begin == std::string::npos ) return false;
		const std::size_t end = scene.find( "}\n\n", begin );
		if( end == std::string::npos ) return false;
		std::ostringstream c;
		const char* env = "\tradiance_map white\n\tradiance_scale 1\n\tradiance_background TRUE\n";
		switch( r ) {
			case Rast::PT:
				c << "pathtracing_pel_rasterizer\n{\n\tsamples " << samples
				  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\tpathguiding FALSE\n\tadaptive_max_samples 0\n" << env;
				break;
			case Rast::PTSpectral:
				c << "pathtracing_spectral_rasterizer\n{\n\tsamples " << samples
				  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n" << env;
				break;
			case Rast::BDPT:
				c << "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 64\n\tmax_light_depth 64\n\tsamples " << samples
				  << "\n\tpixel_filter box\n\toidn_denoise FALSE\n" << env;
				break;
			case Rast::VCM:
				c << "vcm_pel_rasterizer\n{\n\tmax_eye_depth 64\n\tmax_light_depth 64\n\tsamples " << samples
				  << "\n\tpixel_filter box\n\toidn_denoise FALSE\n" << env;
				break;
			case Rast::HWSS:
				c << "pathtracing_spectral_rasterizer\n{\n\tsamples " << samples
				  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\thwss TRUE\n" << env;
				break;
			case Rast::MLT:
				c << "mlt_rasterizer\n{\n\tmax_eye_depth 64\n\tmax_light_depth 64\n\tbootstrap_samples 20000\n\tchains 64\n\tmutations_per_pixel "
				  << samples << "\n\toidn_denoise FALSE\n";
				clamp = 0;	// MLT takes no stability clamps
				break;
		}
		if( clamp > 0 ) {
			c << "\tdirect_clamp " << clamp << "\n\tindirect_clamp " << clamp << "\n";
		}
		scene.replace( begin, end - begin, c.str() );
		return true;
	}

	//! Replaces the material chunk named `name` by `chunk`.
	bool ReplaceMaterial( std::string& scene, const std::string& header, const std::string& chunk )
	{
		const auto begin = scene.find( header );
		if( begin == std::string::npos ) return false;
		const auto closing = scene.find( "}\n", begin );
		if( closing == std::string::npos ) return false;
		scene.replace( begin, closing + 2 - begin, chunk );
		return true;
	}

	const std::string kLambertRight = "lambertian_material\n{\n name block_right\n reflectance white\n}\n";
	const std::string kDiffusionRight = "subsurfacescattering_material\n{\n name block_right\n ior 1.5\n absorption 0\n scattering 20\n g 0\n roughness 0\n}\n";
	const std::string kDiffusionLeft = "subsurfacescattering_material\n{\n name block\n ior 1.5\n absorption 0\n scattering 20\n g 0\n roughness 0\n}\n";

	//! The F2 pair: left `block`, right `block_right`; `kind` 0 random walk
	//! both, 1 random walk | Lambertian, 2 random walk | diffusion, 3
	//! diffusion | diffusion.  `reverse` swaps the two materials.
	bool BuildPair( int kind, bool reverse, Scalar gap, Rast r, unsigned int samples, std::string& scene )
	{
		scene = BuildTouchingPairScene( Model::RandomWalk, 2, gap, samples );
		if( kind == 1 && !ReplaceMaterial( scene, "randomwalk_sss_material\n{\n\tname block_right", kLambertRight ) ) return false;
		if( kind >= 2 && !ReplaceMaterial( scene, "randomwalk_sss_material\n{\n\tname block_right", kDiffusionRight ) ) return false;
		if( kind == 3 && !ReplaceMaterial( scene, "randomwalk_sss_material\n{\n\tname block\n", kDiffusionLeft ) ) return false;
		if( reverse ) {
			const auto left = scene.find( "\tmaterial block\n" );
			if( left == std::string::npos ) return false;
			scene.replace( left, 16, "\tmaterial swap\n" );
			const auto right = scene.find( "\tmaterial block_right\n" );
			if( right == std::string::npos ) return false;
			scene.replace( right, 22, "\tmaterial block\n" );
			const auto swapped = scene.find( "\tmaterial swap\n" );
			if( swapped == std::string::npos ) return false;
			scene.replace( swapped, 15, "\tmaterial block_right\n" );
		}
		return SetRasterizer( scene, r, samples, kind >= 2 ? 4.0 : 0.0 );
	}

	//! A unit cube resting on a white Lambertian table slab (3 x 0.2 x 3,
	//! top face coincident with the cube's bottom when `gap` == 0), seen
	//! orthographically from the front.  `diffusion` picks the cube's
	//! material (else a conservative random walk).
	std::string BuildTable( bool diffusion, Scalar gap, Rast r, unsigned int samples )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\n";
		s << "film\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "orthographic_camera\n{\n\tlocation 0 -0.2 4\n\tlookat 0 -0.2 0\n\tup 0 1 0\n\tviewport_scale 1.6 1.6\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		if( diffusion ) {
			s << kDiffusionLeft << "\n";
		} else {
			s << "randomwalk_sss_material\n{\n\tname block\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
		}
		s << "lambertian_material\n{\n\tname table\n\treflectance white\n}\n\n";
		s << "box_geometry\n{\n\tname block_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		s << "box_geometry\n{\n\tname table_geo\n\twidth 3\n\theight 0.2\n\tdepth 3\n}\n\n";
		s << "standard_object\n{\n\tname block\n\tgeometry block_geo\n\tmaterial block\n\tposition 0 0 0\n}\n\n";
		s << "standard_object\n{\n\tname table\n\tgeometry table_geo\n\tmaterial table\n\tposition 0 " << -0.6 - gap << " 0\n}\n\n";
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n";
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl408_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		std::string out = s.str();
		SetRasterizer( out, r, samples, diffusion ? 4.0 : 0.0 );
		return out;
	}

	//! An indexed-mesh unit cube (the DL-370 review's generator): `ds`
	//! double-sided, `inward` reversed winding, `dup` one duplicated
	//! triangle (DL-143 cannot certify it).
	std::string MeshCube( const std::string& name, bool ds, bool inward, bool dup )
	{
		std::ostringstream s;
		s << "indexedmesh_geometry\n{\n\tname " << name << "\n";
		for( int i = 0; i < 8; ++i ) {
			s << "\tvertex " << ( ( i & 1 ) ? 0.5 : -0.5 ) << " " << ( ( i & 2 ) ? 0.5 : -0.5 ) << " " << ( ( i & 4 ) ? 0.5 : -0.5 ) << "\n";
		}
		const int tri[12][3] = { {0,4,6},{0,6,2},{1,3,7},{1,7,5},{0,1,5},{0,5,4},{2,6,7},{2,7,3},{0,2,3},{0,3,1},{4,5,7},{4,7,6} };
		for( int t = 0; t < 12 + ( dup ? 1 : 0 ); ++t ) {
			const int* v = tri[t % 12];
			if( inward ) s << "\ttriangle " << v[0] << " " << v[2] << " " << v[1] << "\n";
			else s << "\ttriangle " << v[0] << " " << v[1] << " " << v[2] << "\n";
		}
		s << "\tdouble_sided " << ( ds ? "TRUE" : "FALSE" ) << "\n\tface_normals TRUE\n}\n\n";
		return s.str();
	}

	//! A RW cube on the left and a diffusion body of variant `kind` on the
	//! right (unit-cube footprint, touching at x = 0 when `gap` == 0):
	//! "meshds", "meshss", "meshdsin" (inward), "meshdsdup" (uncertified),
	//! "rot" (box rotated 90 deg), "csg" (box intersect big sphere),
	//! "displaced" (box tessellated, zero displacement), "sdf" (SDF box).
	//! `container` wraps both in an index-1.0 glass sphere of radius 2.5.
	std::string BuildVariantPair( const std::string& kind, bool container, Scalar gap, Rast r, unsigned int samples )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 32\n\theight 16\n}\n\n";
		s << "orthographic_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 2 1\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n";
		s << "randomwalk_sss_material\n{\n\tname block\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
		s << kDiffusionRight << "\n";
		s << "box_geometry\n{\n\tname block_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		const Scalar xr = 0.5 + gap / 2;
		std::string geo = "block_geo", extraObj;
		if( kind.compare( 0, 4, "mesh" ) == 0 ) {
			s << MeshCube( "right_geo", kind.find( "ds" ) != std::string::npos, kind.find( "in" ) != std::string::npos,
				kind.find( "dup" ) != std::string::npos );
			geo = "right_geo";
		} else if( kind == "displaced" ) {
			s << "displaced_geometry\n{\n\tname right_geo\n\tbase_geometry block_geo\n\tdetail 4\n\tdisplacement black\n\tdisp_scale 0\n}\n\n";
			geo = "right_geo";
		} else if( kind == "sdf" ) {
			s << "sdf_geometry\n{\n\tname right_geo\n\tpart roundbox union 0.0 0 0 0 0 0 0 0.5 0.5 0.5 0 0 0 0.0\n}\n\n";
			geo = "right_geo";
		}
		s << "standard_object\n{\n\tname left\n\tgeometry block_geo\n\tmaterial block\n\tposition " << -0.5 - gap / 2 << " 0 0\n}\n\n";
		if( kind == "csg" ) {
			s << "sphere_geometry\n{\n\tname big_geo\n\tradius 5\n}\n\n";
			s << "standard_object\n{\n\tname ra\n\tgeometry block_geo\n\tmaterial block_right\n\tposition " << xr << " 0 0\n}\n\n";
			s << "standard_object\n{\n\tname rb\n\tgeometry big_geo\n\tmaterial block_right\n\tposition " << xr << " 0 0\n}\n\n";
			s << "csg_object\n{\n\tname right\n\tobja ra\n\tobjb rb\n\toperation intersection\n\tmaterial block_right\n\tallow_transformed_operands TRUE\n}\n\n";
		} else {
			s << "standard_object\n{\n\tname right\n\tgeometry " << geo << "\n\tmaterial block_right\n\tposition " << xr << " 0 0\n"
			  << ( kind == "rot" ? "\torientation 90 0 0\n" : "" ) << "}\n\n";
		}
		if( r == Rast::MLT ) {
			// MLT takes no environment map: light the pair from a panel above.
			s << "lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 2\n\tmaterial none\n}\n\n";
			s << "clippedplane_geometry\n{\n\tname lum_geo\n\tpta -2 0 2\n\tptb 2 0 2\n\tptc 2 0 -2\n\tptd -2 0 -2\n}\n\n";
			s << "standard_object\n{\n\tname panel\n\tgeometry lum_geo\n\tmaterial lum\n\tposition 0 2 0\n}\n\n";
		}
		if( container ) {
			s << "dielectric_material\n{\n\tname glass\n\ttau 1\n\tior 1.0\n\tscattering 1000000\n}\n\n";
			s << "sphere_geometry\n{\n\tname cont_geo\n\tradius 2.5\n}\n\n";
			s << "standard_object\n{\n\tname cont\n\tgeometry cont_geo\n\tmaterial glass\n\tposition 0 0 0\n}\n\n";
		}
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n";
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl408_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		std::string out = s.str();
		SetRasterizer( out, r, samples, 4.0 );
		return out;
	}

	//! A cube resting on an OPEN floor (a clippedplane sheet, no interior)
	//! with a black absorbing box below it: light the exit sends into the
	//! sheet's plane must be met by the sheet, or it is lost in the black
	//! box (a white furnace cannot see this).  `diffusion` picks the cube.
	std::string BuildSheetFloor( bool diffusion, Scalar gap, Rast r, unsigned int samples )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "orthographic_camera\n{\n\tlocation 0 -0.2 4\n\tlookat 0 -0.2 0\n\tup 0 1 0\n\tviewport_scale 1.6 1.6\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n";
		if( diffusion ) {
			s << kDiffusionLeft << "\n";
		} else {
			s << "randomwalk_sss_material\n{\n\tname block\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
		}
		s << "lambertian_material\n{\n\tname table\n\treflectance white\n}\n\n";
		s << "lambertian_material\n{\n\tname blackm\n\treflectance black\n}\n\n";
		s << "box_geometry\n{\n\tname block_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		s << "clippedplane_geometry\n{\n\tname floor_geo\n\tpta -1.5 0 1.5\n\tptb 1.5 0 1.5\n\tptc 1.5 0 -1.5\n\tptd -1.5 0 -1.5\n}\n\n";
		s << "box_geometry\n{\n\tname under_geo\n\twidth 3\n\theight 0.5\n\tdepth 3\n}\n\n";
		s << "standard_object\n{\n\tname block\n\tgeometry block_geo\n\tmaterial block\n\tposition 0 0 0\n}\n\n";
		s << "standard_object\n{\n\tname table\n\tgeometry floor_geo\n\tmaterial table\n\tposition 0 " << -0.5 - gap << " 0\n}\n\n";
		s << "standard_object\n{\n\tname under\n\tgeometry under_geo\n\tmaterial blackm\n\tposition 0 -0.9 0\n}\n\n";
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n";
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl408_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		std::string out = s.str();
		SetRasterizer( out, r, samples, diffusion ? 4.0 : 0.0 );
		return out;
	}

	//! Review round 2, P1: a random-walk cube resting on the INSIDE face of
	//! a closed room (how Blender exports a room): `mesh` picks a closed
	//! double-sided indexed mesh room over a box_geometry one.  Grey walls,
	//! a luminaire panel under the ceiling, the camera inside.  Light the
	//! exit sends into the floor's plane must be met by the floor; missing
	//! it, the continuation escapes below the room and is lost.
	std::string BuildRoom( bool mesh, Scalar gap, Rast r, unsigned int samples )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "orthographic_camera\n{\n\tlocation 0 0.25 1.9\n\tlookat 0 0.25 0\n\tup 0 1 0\n\tviewport_scale 1.4 1.4\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n}\n\n";
		s << "randomwalk_sss_material\n{\n\tname block\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
		s << "lambertian_material\n{\n\tname wall\n\treflectance grey\n}\n\n";
		s << "lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 4\n\tmaterial none\n}\n\n";
		s << "box_geometry\n{\n\tname block_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		std::string roomGeo = "room_geo", roomScale;
		if( mesh ) {
			s << MeshCube( "room_geo", true, false, false );
			roomScale = "\tscale 4 3 4\n";
		} else {
			s << "box_geometry\n{\n\tname room_geo\n\twidth 4\n\theight 3\n\tdepth 4\n}\n\n";
		}
		s << "clippedplane_geometry\n{\n\tname lum_geo\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n}\n\n";
		s << "standard_object\n{\n\tname block\n\tgeometry block_geo\n\tmaterial block\n\tposition 0 " << gap << " 0\n}\n\n";
		s << "standard_object\n{\n\tname room\n\tgeometry " << roomGeo << "\n\tmaterial wall\n\tposition 0 1 0\n" << roomScale << "}\n\n";
		s << "standard_object\n{\n\tname panel\n\tgeometry lum_geo\n\tmaterial lum\n\tposition 0 2.4 0\n}\n\n";
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n";
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl408_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		std::string out = s.str();
		SetRasterizer( out, r, samples, 0.0 );
		return out;
	}

	//! Review round 2: an INSET -- a white Lambertian slab nested inside a
	//! random-walk cube, flush with the cube's top face (lowered by 2e-6 in
	//! the gap twin).  The walk ignores the slab's interior, and an exit on
	//! the top face leaves BOTH bodies into the air: the slab is not a
	//! neighbour across the face, so the contact twin must match the gap
	//! twin.  A consistency pin for the nesting rule's refusal half.
	std::string BuildInset( Scalar gap, Rast r, unsigned int samples )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "orthographic_camera\n{\n\tlocation 0 2 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 1.8 1.8\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		s << "randomwalk_sss_material\n{\n\tname block\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
		s << "lambertian_material\n{\n\tname slab\n\treflectance white\n}\n\n";
		s << "box_geometry\n{\n\tname block_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		s << "box_geometry\n{\n\tname slab_geo\n\twidth 0.6\n\theight 0.2\n\tdepth 0.6\n}\n\n";
		s << "standard_object\n{\n\tname block\n\tgeometry block_geo\n\tmaterial block\n\tposition 0 0 0\n}\n\n";
		s << "standard_object\n{\n\tname slab\n\tgeometry slab_geo\n\tmaterial slab\n\tposition 0 " << 0.4 - gap << " 0\n}\n\n";
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n";
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl408_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		std::string out = s.str();
		SetRasterizer( out, r, samples, 0.0 );
		return out;
	}

	//! Review round 3: a cube (random walk, or `diffusion`) resting on a
	//! 1 x 1 EMISSIVE pad exactly under it, lit by nothing else (no
	//! environment): every photon reaches the cube through the contact.
	//! An eye path that reaches the pad by a contact launch carries its
	//! emission (BDPT / VCM / MLT used to read exactly 0 here).
	std::string BuildPad( bool diffusion, Scalar gap, Rast r, unsigned int samples )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "orthographic_camera\n{\n\tlocation 0 1.5 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 1.8 1.8\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		if( diffusion ) {
			s << kDiffusionLeft << "\n";
		} else {
			s << "randomwalk_sss_material\n{\n\tname block\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
		}
		s << "lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 4\n\tmaterial none\n}\n\n";
		s << "box_geometry\n{\n\tname block_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		s << "clippedplane_geometry\n{\n\tname pad_geo\n\tpta -0.5 0 0.5\n\tptb 0.5 0 0.5\n\tptc 0.5 0 -0.5\n\tptd -0.5 0 -0.5\n}\n\n";
		s << "standard_object\n{\n\tname block\n\tgeometry block_geo\n\tmaterial block\n\tposition 0 0 0\n}\n\n";
		s << "standard_object\n{\n\tname pad\n\tgeometry pad_geo\n\tmaterial lum\n\tposition 0 " << -0.5 - gap << " 0\n}\n\n";
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n";
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl408_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		std::string out = s.str();
		SetRasterizer( out, r, samples, diffusion ? 4.0 : 0.0 );
		const std::string env = "\tradiance_map white\n\tradiance_scale 1\n\tradiance_background TRUE\n";
		const std::size_t e = out.find( env );
		if( e != std::string::npos ) {
			out.erase( e, env.size() );	// lit by the pad alone
		}
		return out;
	}

	struct Row
	{
		std::string label;
		std::function<bool( Scalar gap, std::string& scene )> build;
		bool absolute;		// every material conserves energy: also gate both sides against 1
		double floor = 0.002;	// relative floor of the paired band
	};
}

int main( int argc, char** argv )
{
	bool gate = true;
	std::string only;
	double gapTwin = 2e-6;	// --gap: the separated twin's gap (measurement aid)
	for( int i = 1; i < argc; ++i ) {
		const std::string a = argv[i];
		if( a == "--measure" ) gate = false;
		else if( a == "--gate" ) gate = true;
		else if( a == "--only" && i + 1 < argc ) only = argv[++i];
		else if( a == "--gap" && i + 1 < argc ) gapTwin = std::atof( argv[++i] );
	}
	const unsigned int kSalts = 8;
	std::vector<Row> rows;
	const char* kindName[] = { "RW|RW", "RW|Lambert", "RW|diffusion", "diffusion|diffusion" };
	for( int kind = 0; kind < 4; ++kind ) {
		for( bool reverse : { false, true } ) {
			if( kind == 3 && reverse ) continue;	// symmetric pair
			std::ostringstream l;
			l << "pair " << kindName[kind] << ( reverse ? " reversed" : "" ) << " PT";
			rows.push_back( { l.str(), [=]( Scalar gap, std::string& sc ) { return BuildPair( kind, reverse, gap, Rast::PT, 256, sc ); }, kind <= 1 } );
		}
	}
	for( Rast r : { Rast::BDPT, Rast::VCM } ) {
		for( int kind : { 1, 2, 3 } ) {
			std::ostringstream l;
			l << "pair " << kindName[kind] << " " << RastName( r );
			rows.push_back( { l.str(), [=]( Scalar gap, std::string& sc ) { return BuildPair( kind, false, gap, r, 64, sc ); }, kind <= 1 } );
		}
	}
	rows.push_back( { "pair RW|diffusion PT-spectral", []( Scalar gap, std::string& sc ) { return BuildPair( 2, false, gap, Rast::PTSpectral, 256, sc ); }, false } );
	for( bool diffusion : { false, true } ) {
		for( Rast r : { Rast::PT, Rast::BDPT } ) {
			std::ostringstream l;
			l << ( diffusion ? "diffusion" : "RW (wax)" ) << " block on Lambert table " << RastName( r );
			const unsigned int spp = r == Rast::PT ? 256 : 64;
			rows.push_back( { l.str(), [=]( Scalar gap, std::string& sc ) { sc = BuildTable( diffusion, gap, r, spp ); return true; }, !diffusion } );
		}
	}

	// DL-408 review P1: the pair inside an enclosure (an index-1.0 glass
	// sphere) -- every exit's point lies in the enclosure too.
	for( Rast r : { Rast::PT, Rast::BDPT } ) {
		std::ostringstream l;
		l << "pair RW|diffusion inside a glass enclosure " << RastName( r );
		const unsigned int spp = r == Rast::PT ? 256 : 64;
		rows.push_back( { l.str(), [=]( Scalar gap, std::string& sc ) { sc = BuildVariantPair( "box", true, gap, r, spp ); return true; }, false } );
	}
	// Review P2-1: an open-sheet floor with an absorber below.
	rows.push_back( { "RW block on an open-sheet floor PT", []( Scalar gap, std::string& sc ) { sc = BuildSheetFloor( false, gap, Rast::PT, 256 ); return true; }, false } );
	rows.push_back( { "RW block on an open-sheet floor BDPT", []( Scalar gap, std::string& sc ) { sc = BuildSheetFloor( false, gap, Rast::BDPT, 64 ); return true; }, false } );
	rows.push_back( { "diffusion block on an open-sheet floor PT", []( Scalar gap, std::string& sc ) { sc = BuildSheetFloor( true, gap, Rast::PT, 256 ); return true; }, false } );
	// Review P2-2: neighbour geometry variants (RW | diffusion, PT).
	for( const char* kind : { "meshds", "meshss", "meshdsin", "meshdsdup", "rot", "csg", "displaced", "sdf" } ) {
		const std::string k = kind;
		rows.push_back( { "pair RW|diffusion right " + k + " PT", [=]( Scalar gap, std::string& sc ) { sc = BuildVariantPair( k, false, gap, Rast::PT, 256 ); return true; }, false } );
	}
	// Review round 2, P1: a cube on the inside floor of a closed room.
	for( bool mesh : { false, true } ) {
		for( Rast r : { Rast::PT, Rast::BDPT } ) {
			std::ostringstream l;
			l << "RW block on a closed " << ( mesh ? "mesh" : "box" ) << " room floor " << RastName( r );
			const unsigned int spp = r == Rast::PT ? 256 : 64;
			rows.push_back( { l.str(), [=]( Scalar gap, std::string& sc ) { sc = BuildRoom( mesh, gap, r, spp ); return true; }, false } );
		}
	}
	rows.push_back( { "RW block with a flush Lambert inset PT", []( Scalar gap, std::string& sc ) { sc = BuildInset( gap, Rast::PT, 256 ); return true; }, false } );
	// Review round 3: a cube on an emissive pad.
	for( bool diffusion : { false, true } ) {
		for( Rast r : { Rast::PT, Rast::BDPT, Rast::VCM } ) {
			std::ostringstream l;
			l << ( diffusion ? "diffusion" : "RW" ) << " block on an emissive pad " << RastName( r );
			const unsigned int spp = r == Rast::PT ? 256 : 64;
			rows.push_back( { l.str(), [=]( Scalar gap, std::string& sc ) { sc = BuildPad( diffusion, gap, r, spp ); return true; }, false } );
		}
	}
	{
		Row mlt{ "RW block on an emissive pad MLT (sanity)", []( Scalar gap, std::string& sc ) { sc = BuildPad( false, gap, Rast::MLT, 64 ); return true; }, false };
		mlt.floor = 0.03;
		rows.push_back( mlt );
	}
	rows.push_back( { "pair RW|diffusion PT-spectral hwss", []( Scalar gap, std::string& sc ) { sc = BuildVariantPair( "box", false, gap, Rast::HWSS, 256 ); return true; }, false } );
	{
		Row mlt{ "pair RW|diffusion MLT (sanity)", []( Scalar gap, std::string& sc ) { sc = BuildVariantPair( "box", false, gap, Rast::MLT, 64 ); return true; }, false };
		mlt.floor = 0.03;
		rows.push_back( mlt );
	}

	bool failed = false;
	unsigned int salt = 49000;
	for( const Row& row : rows ) {
		if( !only.empty() && row.label.find( only ) == std::string::npos ) continue;
		std::vector<double> contact, gapped;
		bool valid = true;
		for( unsigned int t = 0; t < kSalts; ++t, ++salt ) {
			for( bool separated : { false, true } ) {
				std::string scene;
				if( !row.build( separated ? gapTwin : 0.0, scene ) ) { valid = false; break; }
				const std::string path = WriteScene( scene, "dl408" );
				const double mean = RenderFurnaceMean( path, salt );
				std::remove( path.c_str() );
				if( !( mean > 0 ) || !std::isfinite( mean ) ) { valid = false; break; }
				( separated ? gapped : contact ).push_back( mean );
			}
			if( !valid ) break;
		}
		if( !valid ) {
			std::cout << "DL408 FAIL " << row.label << ": a render failed" << std::endl;
			failed = true;
			continue;
		}
		const Stats a = Summarize( contact ), b = Summarize( gapped );
		const double seA = a.sd / std::sqrt( double( kSalts ) ), seB = b.sd / std::sqrt( double( kSalts ) );
		// The twins share each salt (common random numbers), so the band is
		// 4 SE of the PAIRED differences -- a heavy-tailed salt shows up in
		// both twins and cancels -- with a 0.2 % floor for the pairing the
		// thread schedule breaks.
		std::vector<double> diff;
		for( unsigned int i = 0; i < kSalts; ++i ) diff.push_back( contact[i] - gapped[i] );
		const Stats d = Summarize( diff );
		const double band = r_max( 4 * d.sd / std::sqrt( double( kSalts ) ), row.floor * b.mean );
		const bool ratioOK = std::fabs( d.mean ) <= band;
		const bool absOK = !row.absolute ||
			( std::fabs( a.mean - 1 ) <= r_max( 4 * seA, 0.002 ) && std::fabs( b.mean - 1 ) <= r_max( 4 * seB, 0.002 ) );
		const bool ok = ratioOK && absOK;
		std::cout << std::setprecision( 6 ) << "DL408 " << ( ok ? "ok   " : "FAIL " ) << row.label
			<< ": contact " << a.mean << " +/- " << a.sd << "  gap " << b.mean << " +/- " << b.sd
			<< "  ratio " << a.mean / b.mean << "  diff " << d.mean << "  (n=" << kSalts << ", paired band " << band << ")"
			<< ( row.absolute ? ( absOK ? "  furnace ok" : "  furnace FAIL" ) : "" ) << std::endl;
		if( !ok ) failed = true;
	}
	return ( gate && failed ) ? 1 : 0;
}
