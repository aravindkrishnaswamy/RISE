// DL-408: an SSS exit against a COINCIDENT neighbour (contact continuation).
//
// Contact rows compare a touching scene against its twin with a 2e-6 gap
// (wider than the 1e-6 outward offset an SSS exit gives its continuation,
// optically nothing), with PAIRED salts (ValueSalt, common random numbers),
// n = 8 per side (unclamped diffusion VCM uses 16). The perspective rows
// are unclamped; outside-light occlusion rows keep geometry fixed and turn
// off the fully occluded exterior light as an independent reference.
// Under the white environment with
// conservative materials the gapped twin is the oracle (the diffusion
// profile is not energy conserving on a finite cube, so its absolute value
// is not 1); rows whose every material conserves energy are also gated
// against the absolute furnace value 1.
//
// Pre-fix (master ba133dd38) the diffusion rows read 0.90 of their twins,
// the reversed random-walk / Lambertian pair 0.9968; the fix is the contact
// launch (docs/DEBT_LEDGER.md).  Default: gate (exit 1
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
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 16\n\theight 8\n}\n\n";
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
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 16\n\theight 16\n}\n\n";
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
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 16\n\theight 16\n}\n\n";
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

	//! Review round 4 (P2): a RW cube on the floor of a closed box room lit
	//! by a small panel under the ceiling AND by a bright single-sided panel
	//! OUTSIDE, under the floor, facing up (the review's fixture).  Nothing
	//! from outside may reach the inside: a light subpath from the outer
	//! panel meets the floor's outer face exactly where the cube's bottom
	//! face coincides with it, and must take the floor.  No environment.
	std::string BuildRoomOutside( Scalar gap, Rast r, unsigned int samples, bool outsideOn = true )
	{
		std::ostringstream s;
		s << std::setprecision( 17 );
		s << "RISE ASCII SCENE 7\nfilm\n{\n\twidth 20\n\theight 15\n}\n\n";
		s << "pinhole_camera\n{\n\tlocation 0 0.8 2.5\n\tlookat 0 -0.6 0\n\tup 0 1 0\n\tfov 55\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname white\n\tcolor 0.8 0.8 0.8\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname lt\n\tcolor 1 1 1\n}\n\n";
		s << "randomwalk_sss_material\n{\n\tname block\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
		s << "lambertian_material\n{\n\tname wall\n\treflectance white\n}\n\n";
		s << "lambertian_luminaire_material\n{\n\tname lum\n\texitance lt\n\tscale 40\n\tmaterial none\n}\n\n";
		s << "lambertian_luminaire_material\n{\n\tname lumout\n\texitance lt\n\tscale 20\n\tmaterial none\n}\n\n";
		s << "box_geometry\n{\n\tname block_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		s << "standard_object\n{\n\tname blk\n\tgeometry block_geo\n\tmaterial block\n\tposition 0 " << -0.5 + gap << " 0\n}\n\n";
		s << "box_geometry\n{\n\tname room_geo\n\twidth 6\n\theight 4\n\tdepth 6\n}\n\n";
		s << "standard_object\n{\n\tname room\n\tgeometry room_geo\n\tmaterial wall\n\tposition 0 1 0\n}\n\n";
		s << "clippedplane_geometry\n{\n\tname cl\n\tpta -0.5 0 -0.5\n\tptb 0.5 0 -0.5\n\tptc 0.5 0 0.5\n\tptd -0.5 0 0.5\n\tdoublesided FALSE\n}\n\n";
		s << "standard_object\n{\n\tname ceil\n\tgeometry cl\n\tmaterial lum\n\tposition 0 2.9 0\n}\n\n";
		if( outsideOn ) {
			s << "clippedplane_geometry\n{\n\tname ol\n\tpta -2 0 2\n\tptb 2 0 2\n\tptc 2 0 -2\n\tptd -2 0 -2\n\tdoublesided FALSE\n}\n\n";
			s << "standard_object\n{\n\tname outl\n\tgeometry ol\n\tmaterial lumout\n\tposition 0 -1.5 0\n}\n\n";
		}
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n";
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl408_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		std::string out = s.str();
		SetRasterizer( out, r, samples, 0.0 );
		const std::string env = "\tradiance_map white\n\tradiance_scale 1\n\tradiance_background TRUE\n";
		const std::size_t e = out.find( env );
		if( e != std::string::npos ) {
			out.erase( e, env.size() );
		}
		// The review's depth caps: at 12 the tie's leak showed on
		// `8df8b6dff` (BDPT 1.024); at 64 it hid (1.001).
		for( const char* key : { "\tmax_eye_depth 64\n", "\tmax_light_depth 64\n" } ) {
			const std::size_t k = out.find( key );
			if( k != std::string::npos ) {
				std::string rep( key );
				rep.replace( rep.find( "64" ), 2, "12" );
				out.replace( k, std::string( key ).size(), rep );
			}
		}
		return out;
	}

	//! Supplied round-4 perspective fixture (gen4.py), unclamped.
	std::string BuildReview4Pad( bool diffusion, Scalar gap, Rast r )
	{
		std::string out = R"REVIEW(RISE ASCII SCENE 7
film
{
	width 20
	height 15
}

pinhole_camera
{
	location 0 0.3 3.0
	lookat 0 -1.2 0
	up 0 1 0
	fov 55
}

uniformcolor_painter
{
	name white
	color 0.8 0.8 0.8
}

uniformcolor_painter
{
	name lt
	color 1 1 1
}

randomwalk_sss_material
{
	name block
	ior 1.5
	absorption 0
	scattering 2
	g 0
	roughness 0
	max_bounces 8192
}

lambertian_material
{
	name wall
	reflectance white
}

lambertian_luminaire_material
{
	name lum
	exitance lt
	scale 1
	material none
}

box_geometry
{
	name block_geo
	width 1
	height 1
	depth 1
}

standard_object
{
	name blk
	geometry block_geo
	material block
	position 0 -0.5 0
}

clippedplane_geometry
{
	name pad
	pta -0.5 0 0.5
	ptb 0.5 0 0.5
	ptc 0.5 0 -0.5
	ptd -0.5 0 -0.5
}

standard_object
{
	name padobj
	geometry pad
	material lum
	position 0 -1 0
}

clippedplane_geometry
{
	name fl
	pta -4 0 4
	ptb 4 0 4
	ptc 4 0 -4
	ptd -4 0 -4
}

standard_object
{
	name floor
	geometry fl
	material wall
	position 0 -1.4 0
}

standard_shader
{
	name global
	shaderop DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples 256
	pixel_filter box
	oidn_denoise FALSE
	pathguiding FALSE
	adaptive_max_samples 0
}

file_rasterizeroutput
{
	pattern rendered/unused
	type EXR
	bpp 32
	color_space Rec709RGB_Linear
}
)REVIEW";
		if( diffusion ) ReplaceMaterial( out, "randomwalk_sss_material\n{\n\tname block", kDiffusionLeft );
		const std::string position = "\tposition 0 -0.5 0\n";
		std::ostringstream moved; moved << std::setprecision(17) << "\tposition 0 " << -0.5 + gap << " 0\n";
		out.replace( out.find(position), position.size(), moved.str() );
		SetRasterizer( out, r, r == Rast::PT ? 32 : 16, 0 );
		const std::string env = "\tradiance_map white\n\tradiance_scale 1\n\tradiance_background TRUE\n";
		out.erase( out.find(env), env.size() );
		for( const char* key : { "\tmax_eye_depth 64\n", "\tmax_light_depth 64\n" } ) {
			const auto at = out.find(key); if( at == std::string::npos ) continue;
			std::string replacement(key); replacement.replace(replacement.find("64"),2,"30");
			out.replace(at,std::string(key).size(),replacement);
		}
		return out;
	}

	bool TestOutsideArrivalOwnership()
	{
		bool ok=true;
		for( bool diffusion : {false,true} ) for( bool wideTraversal : {false,true} ) {
			std::string scene=BuildReview4Pad(diffusion,0,Rast::PT);
			const std::string sheet="standard_object\n{\n\tname padobj\n\tgeometry pad\n\tmaterial lum\n\tposition 0 -1 0\n}\n\n";
			scene.erase(scene.find(sheet),sheet.size());
			// Replace the open pad by an adjacent solid neighbour. Arrival
			// is outside SSS but inside this neighbour; its exit is first.
			scene += "\ndielectric_material\n{\n name neighbour_material\n ior 1.5\n tau 1\n scattering 1000000\n}\nbox_geometry\n{\n name neighbour_geo\n width 1\n height 0.4\n depth 1\n}\nstandard_object\n{\n name solid_neighbour\n geometry neighbour_geo\n material neighbour_material\n position 0 -1.2 0\n}\n";
			if(wideTraversal) for(unsigned i=0;i<16;++i) {
				std::ostringstream far;far<<"\nstandard_object\n{\n name far"<<i<<"\n geometry block_geo\n material wall\n position "<<100+10*i<<" 0 0\n}\n";scene+=far.str();
			}
			const std::string path=WriteScene(scene,"dl408_ownership");
			IJobPriv* job=nullptr;
			if(!RISE_CreateJobPriv(&job) || !job || !job->SetPrimaryAcceleration(wideTraversal,false,4,32) || !job->LoadAsciiSceneViaCst(path.c_str())) {safe_release(job);std::remove(path.c_str());return false;}
			std::remove(path.c_str());
			IObjectManager* objects=job->GetObjects();objects->PrepareForRendering();
			const IObject* sss=objects->GetItem("blk");
			const IObject* neighbour=objects->GetItem("solid_neighbour");
			if(!sss || !neighbour) {safe_release(job);return false;}
			// The only coincident neighbour is the solid.
			// The sheet is omitted before loading; no prepared tree is mutated.
			unsigned wrong=0,managerWrong=0,exitWrong=0;
			for(unsigned i=0;i<128;++i) {
				Ray ray(Point3(-.4+.8*double(i%16)/15,-1.2,-.4+.8*double(i/16)/7),Vector3(0,1,0));
				RayIntersection ri(ray,nullRasterizerState);sss->IntersectRay(ri,RISE_INFINITY,true,true,false);
				BSSRDFSampling::ResolveCoincidentEntry(*objects,ri,true,true);
				if(!ri.geometric.bHit || ri.pObject!=neighbour) ++wrong;
				RayIntersection primary(ray,nullRasterizerState);objects->IntersectRay(primary,true,true,false);
				if(!primary.geometric.bHit || primary.pObject!=neighbour) ++managerWrong;
				RayIntersection withExit(ray,nullRasterizerState),expectedExit(ray,nullRasterizerState);
				objects->IntersectRay(withExit,true,true,true);neighbour->IntersectRay(expectedExit,RISE_INFINITY,true,true,true);
				if(withExit.pObject!=neighbour || withExit.hasBoundaryRange2!=expectedExit.hasBoundaryRange2 || !RelClose(withExit.geometric.ptExit.y,expectedExit.geometric.ptExit.y,1e-12) ||
					!RelClose(withExit.geometric.vNormal2.y,expectedExit.geometric.vNormal2.y,1e-12)) ++exitWrong;
			}
			std::cout << "DL408 ownership " << (diffusion?"diffusion":"RW") << (wideTraversal?" BVH":" linear") << " outside-SSS/inside-neighbour helper-wrong=" <<wrong<<"/128 manager-wrong="<<managerWrong<<"/128 exit-wrong="<<exitWrong<<"/128"<<std::endl;
			if(wrong || managerWrong || exitWrong) ok=false;
			safe_release(job);
		}
		return ok;
	}

	bool TestNonTouchIdentity()
	{
		unsigned index=0;
		for(unsigned fixture=0;fixture<8;++fixture) for(unsigned repeat=0;repeat<4;++repeat) {
			const Rast r=fixture%3==0?Rast::PT:fixture%3==1?Rast::BDPT:Rast::VCM;
			const std::string scene=fixture<6?BuildReview4Pad(fixture>=3,1e-3,r):
				BuildRoomOutside(1e-3,fixture==6?Rast::BDPT:Rast::VCM,64);
			const std::string path=WriteScene(scene,"dl408_identity");
			IJobPriv* job=nullptr;
			if(!RISE_CreateJobPriv(&job) || !job || !job->LoadAsciiSceneViaCst(path.c_str())) {safe_release(job);std::remove(path.c_str());return false;}
			std::remove(path.c_str());job->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* cap=new CapturingRasterizerOutput();job->GetRasterizer()->AddRasterizerOutput(cap);
			const unsigned seed=841000+fixture*31+repeat;
			SobolSamplerTestHooks::ValueSalt().store(SobolSequence::HashCombine(seed,0x315u));std::srand(seed);
			const bool rendered=job->Rasterize();SobolSamplerTestHooks::ValueSalt().store(0u);
			bool finite=true;
			unsigned long long hash=1469598103934665603ull;
			for(const auto& pixel:cap->pixels) {
				const double channels[]={pixel.base.r,pixel.base.g,pixel.base.b,pixel.a};
				for(double channel:channels) if(!std::isfinite(channel)) finite=false;
				const auto* bytes=reinterpret_cast<const unsigned char*>(channels);
				for(std::size_t k=0;k<sizeof(channels);++k) {hash^=bytes[k];hash*=1099511628211ull;}
			}
			const bool valid=rendered && finite && !cap->pixels.empty();safe_release(cap);safe_release(job);
			if(!valid)return false;
			std::cout<<"DL408 IDENTITY "<<index++<<" seed="<<seed<<" hash="<<std::hex<<hash<<std::dec<<std::endl;
		}
		return true;
	}
	//! A nearby sheet is not a tie, even after a large scene translation.
	bool TestEntryDistance()
	{
		bool ok=true;
		for(double x : {0.,10000.}) {
			for(double gap : {0.,2e-6}) for(bool grazing : {false,true}) {
				std::string scene=BuildReview4Pad(false,0,Rast::PT);
				for(auto item : {std::pair<const char*,double>{"-0.5",-.5},{"-1",-gap},{"-1.4",-1.4}}) {
					const std::string old="\tposition 0 "+std::string(item.first)+" 0\n";
					std::ostringstream moved; moved<<std::setprecision(17)<<"\tposition "<<x<<" "<<item.second<<" 0\n";
					scene.replace(scene.find(old),old.size(),moved.str());
				}
				const auto path=WriteScene(scene,"noncontact"); IJobPriv* job=nullptr;
				if(!RISE_CreateJobPriv(&job)||!job||!job->LoadAsciiSceneViaCst(path.c_str())) {
					safe_release(job); std::remove(path.c_str()); return false;
				}
				std::remove(path.c_str()); auto* objects=job->GetObjects(); objects->PrepareForRendering();
				const auto* sss=objects->GetItem("blk"); const auto* pad=objects->GetItem("padobj");
				if(!sss||!pad) {safe_release(job);return false;}
				Ray ray(Point3(x,grazing?1e-7:.2,0),grazing?Vector3(1,-1e-4,0):Vector3(0,-1,0)); RayIntersection result(ray,nullRasterizerState);
				objects->IntersectRay(result,true,true,false);
				std::cout<<"DL408 distance x="<<x<<" gap="<<gap<<" grazing="<<grazing<<" SSS-first="<<(result.pObject==sss)
					<<" pad-first="<<(result.pObject==pad)<<" range="<<std::setprecision(17)<<result.geometric.range<<std::endl;
				ok=ok && result.pObject==(gap>0?sss:pad); safe_release(job);
			}
		}
		return ok;
	}

	struct Row
	{
		std::string label;
		std::function<bool( Scalar gap, std::string& scene )> build;
		bool absolute;		// every material conserves energy: also gate both sides against 1
		double floor = 0.002;	// relative floor of the paired band
		bool outsideOffOracle = false;
		unsigned trials = 4;
	};
}

int main( int argc, char** argv )
{
	bool gate = true, identityOnly=false, ownershipOnly=false;
	std::string only;
	double gapTwin = 2e-6;	// --gap: the separated twin's gap (measurement aid)
	for( int i = 1; i < argc; ++i ) {
		const std::string a = argv[i];
		if( a == "--identity-only" ) identityOnly=true;
		else if( a == "--ownership-only" ) ownershipOnly=true;
		else if( a == "--measure" ) gate = false;
		else if( a == "--gate" ) gate = true;
		else if( a == "--only" && i + 1 < argc ) only = argv[++i];
		else if( a == "--gap" && i + 1 < argc ) gapTwin = std::atof( argv[++i] );
	}

	std::vector<Row> rows;
	for( Rast r : { Rast::PT, Rast::BDPT, Rast::VCM } ) {
		const unsigned int spp = r == Rast::PT ? 32 : 16;
		rows.push_back( { std::string( "pair RW|diffusion " ) + RastName( r ),
			[=]( Scalar gap, std::string& sc ) { return BuildPair( 2, false, gap, r, spp, sc ); }, false } );
	}
	rows.push_back( { "pair RW|diffusion PT-spectral hwss",
		[]( Scalar gap, std::string& sc ) { sc = BuildVariantPair( "box", false, gap, Rast::HWSS, 32 ); return true; }, false } );
	rows.push_back( { "pair RW|diffusion right meshds PT",
		[]( Scalar gap, std::string& sc ) { sc = BuildVariantPair( "meshds", false, gap, Rast::PT, 32 ); return true; }, false } );
	rows.push_back( { "RW block on an open-sheet floor PT",
		[]( Scalar gap, std::string& sc ) { sc = BuildSheetFloor( false, gap, Rast::PT, 32 ); return true; }, false } );
	rows.push_back( { "RW block on a closed mesh room floor BDPT",
		[]( Scalar gap, std::string& sc ) { sc = BuildRoom( true, gap, Rast::BDPT, 16 ); return true; }, false } );
	rows.push_back( { "review4 original diffusion pad-floor VCM",
		[]( Scalar gap, std::string& sc ) { sc = BuildReview4Pad( true, gap, Rast::VCM ); return true; }, false, 0, false, 4u } );
	rows.push_back( { "review4 original outside-light occlusion BDPT",
		[]( Scalar side, std::string& sc ) { sc = BuildRoomOutside( 0, Rast::BDPT, 32, side == 0 ); return true; }, false, 0, true, 4u } );

	if(identityOnly) return TestNonTouchIdentity()?0:1;
	const bool neighbourOK=TestOutsideArrivalOwnership();
	const bool distanceOK=TestEntryDistance();
	const bool ownershipOK=neighbourOK && distanceOK;
	if(ownershipOnly) return ownershipOK?0:1;
	bool failed = !ownershipOK;
	unsigned int salt = 49000;
	for( const Row& row : rows ) {
		const unsigned kSalts=row.trials;
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
		// 4 SE of the PAIRED differences for legacy rows, 3 SE for original
		// review fixtures -- a heavy-tailed salt shows up in both twins and
		// cancels -- with a 0.2 % floor on legacy rows and a 1e-4 (100 ppm)
		// relative floor on review fixtures so 2e-6 gap geometry / rounding
		// residuals do not trigger false reds (still 1000x tighter than the
		// 10 % DL-408 contact drop).
		std::vector<double> diff;
		for( unsigned int i = 0; i < kSalts; ++i ) diff.push_back( contact[i] - gapped[i] );
		std::vector<double> ratios;
		for(unsigned i=0;i<kSalts;++i) ratios.push_back(contact[i]/gapped[i]);
		const Stats ratioStats=Summarize(ratios);
		const Stats d = Summarize( diff );
		const double relFloor = r_max( row.floor, 1e-4 );
		const double band = r_max( (row.label.find("review4 original")==0?3:4) * d.sd / std::sqrt( double( kSalts ) ), relFloor * std::fabs( b.mean ) );
		const bool ratioOK = std::fabs( d.mean ) <= band;
		const bool absOK = !row.absolute ||
			( std::fabs( a.mean - 1 ) <= r_max( 4 * seA, 0.002 ) && std::fabs( b.mean - 1 ) <= r_max( 4 * seB, 0.002 ) );
		const bool ok = ratioOK && absOK;
		std::cout << std::setprecision( 6 ) << "DL408 " << ( ok ? "ok   " : "FAIL " ) << row.label
			<< ": contact " << a.mean << " +/- " << a.sd << (row.outsideOffOracle?"  outside-off ":"  gap ") << b.mean << " +/- " << b.sd
			<< "  ratio " << a.mean / b.mean << "  ratio-sd " << ratioStats.sd << "  diff " << d.mean << "  (n=" << kSalts << ", paired band " << band << ")"
			<< ( row.absolute ? ( absOK ? "  furnace ok" : "  furnace FAIL" ) : "" ) << std::endl;
		if( !ok ) failed = true;
	}
	return ( gate && failed ) ? 1 : 0;
}
