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
#include <functional>

namespace
{
	enum class Rast { PT, BDPT, PTSpectral, VCM };

	const char* RastName( Rast r )
	{
		switch( r ) {
			case Rast::PT: return "PT";
			case Rast::BDPT: return "BDPT";
			case Rast::PTSpectral: return "PT-spectral";
			case Rast::VCM: return "VCM";
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

	struct Row
	{
		std::string label;
		std::function<bool( Scalar gap, std::string& scene )> build;
		bool absolute;		// every material conserves energy: also gate both sides against 1
	};
}

int main( int argc, char** argv )
{
	bool gate = true;
	std::string only;
	for( int i = 1; i < argc; ++i ) {
		const std::string a = argv[i];
		if( a == "--measure" ) gate = false;
		else if( a == "--gate" ) gate = true;
		else if( a == "--only" && i + 1 < argc ) only = argv[++i];
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

	bool failed = false;
	unsigned int salt = 49000;
	for( const Row& row : rows ) {
		if( !only.empty() && row.label.find( only ) == std::string::npos ) continue;
		std::vector<double> contact, gapped;
		bool valid = true;
		for( unsigned int t = 0; t < kSalts; ++t, ++salt ) {
			for( bool separated : { false, true } ) {
				std::string scene;
				if( !row.build( separated ? 2e-6 : 0.0, scene ) ) { valid = false; break; }
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
		const double band = r_max( 4 * d.sd / std::sqrt( double( kSalts ) ), 0.002 * b.mean );
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
