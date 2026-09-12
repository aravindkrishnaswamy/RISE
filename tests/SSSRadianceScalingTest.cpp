// DL-04 initial measurement fixture. NOT an eta-normalization oracle yet.
//
// Exact guards: the loaded camera samples the same slab top in every model,
// the requested modern material/medium is bound, and every captured RGB and
// alpha value is finite. Nonzero image means are only smoke guards.
// Measurements: independent serial renders, whole-film RGB means, their
// sample standard deviation and standard error, and channelwise ratios.
// No invalid render may enter an aggregate or be replaced with zero.
//
// A full BSSRDF event can cancel entry 1/eta^2 against exit eta^2. An outer
// camera ratio alone cannot prove the BSSRDF normalization. Absolute air
// furnace measurements and later mutation discriminators must establish it.
// No statistical acceptance band has been guessed from unmeasured output.
// K=4 is an initial sanity measurement; K=1 has no variance estimate.
//
// Run from the repository root with RISE_MEDIA_PATH="$PWD/". Defaults:
// 16x16, 1024 spp, 4 trials, ior 1.5, sigma_a=0, sigma_s=2, g=0.
// --probe labels a diagnostic run; --samples N (perfect square), --trials K,
// --seed N, --ior N, --outer-fresnel are optional. Both invocations currently
// execute the same provisional three-model matrix. No physics closure claim.

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;
namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace {
using RGB = std::array<double, 3>;
constexpr unsigned int kFilmSize = 16;
enum class Model { ExplicitDielectricVolume, DiffusionSSS, RandomWalkSSS };
enum class Topology { Air, WaterInside, WaterOutside };
const char* ModelName( Model m )
{
	switch( m ) {
	case Model::ExplicitDielectricVolume: return "explicit_dielectric_volume";
	case Model::DiffusionSSS: return "diffusion_sss";
	case Model::RandomWalkSSS: return "randomwalk_sss";
	}
	return "unknown_model";
}
const char* TopologyName( Topology t )
{
	switch( t ) {
	case Topology::Air: return "air_no_enclosure";
	case Topology::WaterInside: return "water_camera_inside";
	case Topology::WaterOutside: return "water_camera_outside";
	}
	return "unknown_topology";
}
struct Config {
	unsigned int samples = 1024, trials = 4, seedBase = 1000;
	double surfaceIOR = 1.5;
	bool probe = false, outerFresnel = false;
};
unsigned int passCount = 0, failCount = 0, renderIndex = 0;
bool Check( bool condition, const std::string& label )
{
	if( condition ) ++passCount;
	else { ++failCount; std::cerr << "FAIL: " << label << std::endl; }
	return condition;
}
bool Near( double value, double expected )
{
	return std::isfinite( value ) && std::fabs( value - expected ) <= 1e-9;
}
bool ParseUInt( const char* text, unsigned int& value )
{
	if( !text || !*text || *text == '-' ) return false;
	errno = 0;
	char* end = nullptr;
	const unsigned long n = std::strtoul( text, &end, 10 );
	if( errno || *end || n == 0 || n > std::numeric_limits<unsigned int>::max() ) return false;
	value = static_cast<unsigned int>( n );
	return true;
}
bool ParseArgs( int argc, char** argv, Config& cfg )
{
	for( int i = 1; i < argc; ++i ) {
		const std::string arg( argv[i] );
		if( arg == "--probe" ) cfg.probe = true;
		else if( arg == "--outer-fresnel" ) cfg.outerFresnel = true;
		else if( arg == "--samples" || arg == "--trials" || arg == "--seed" ) {
			if( ++i == argc ) return false;
			unsigned int& value = arg == "--samples" ? cfg.samples : (arg == "--trials" ? cfg.trials : cfg.seedBase);
			if( !ParseUInt( argv[i], value ) ) return false;
		} else if( arg == "--ior" ) {
			if( ++i == argc ) return false;
			errno = 0;
			char* end = nullptr;
			cfg.surfaceIOR = std::strtod( argv[i], &end );
			if( errno || end == argv[i] || *end || !std::isfinite( cfg.surfaceIOR ) || cfg.surfaceIOR <= 0 ) return false;
		} else return false;
	}
	const auto side = static_cast<unsigned int>( std::sqrt( static_cast<double>( cfg.samples ) ) );
	return side * side == cfg.samples;
}

std::string BuildScene( Model model, Topology topology, const Config& cfg, bool lightingControl )
{
	std::ostringstream s;
	s << std::setprecision( 17 );
	s << "RISE ASCII SCENE 7\n"
		"film\n{\n width 16\n height 16\n}\n"
		"orthographic_camera\n{\n location 0 0 " << (topology == Topology::WaterOutside ? 120 : 4) <<
		"\n lookat 0 0 0\n up 0 1 0\n viewport_scale 1\n}\n"
		"uniformcolor_painter\n{\n name white\n color 1 1 1\n}\n";
	if( lightingControl ) {
		s << "lambertian_material\n{\n name slab_material\n reflectance white\n}\n";
	} else if( model == Model::ExplicitDielectricVolume ) {
		s << "dielectric_material\n{\n name slab_material\n ior " << cfg.surfaceIOR <<
			"\n tau 1\n scattering 1000000\n}\n"
			"homogeneous_medium\n{\n name slab_medium\n absorption 0 0 0\n scattering 2 2 2\n phase isotropic\n}\n";
	} else {
		s << (model == Model::DiffusionSSS ? "subsurfacescattering_material" : "randomwalk_sss_material") <<
			"\n{\n name slab_material\n ior " << cfg.surfaceIOR <<
			"\n absorption 0\n scattering 2\n g 0\n roughness 0\n";
		if( model == Model::RandomWalkSSS ) s << " max_bounces 512\n";
		s << "}\n";
	}
	// Shared geometry: x/y +/-40, z [-20,0]. The camera is never inside
	// the SSS solid (ordinary SSS absorbs a primary back-face hit).
	if( lightingControl ) s << "sphere_geometry\n{\n name slab_geometry\n radius 1\n}\n";
	else s << "box_geometry\n{\n name slab_geometry\n width 80\n height 80\n depth 20\n}\n";
	s << "standard_object\n{\n name slab\n geometry slab_geometry\n material slab_material\n position 0 0 " << (lightingControl ? 0 : -10) << "\n";
	if( model == Model::ExplicitDielectricVolume && !lightingControl ) s << " interior_medium slab_medium\n";
	s << "}\n";
	if( topology != Topology::Air ) {
		if( cfg.outerFresnel ) {
			s << "dielectric_material\n{\n name water_material\n ior 1.33\n tau 1\n scattering 1000000\n}\n";
		} else {
			// Ideal nonreflecting IOR enclosure. Explicit white refractance
			// matters: the perfect-refractor default painter is black.
			s << "perfectrefractor_material\n{\n name water_material\n ior 1.33\n refractance white\n}\n";
		}
		s << "box_geometry\n{\n name water_geometry\n width 200\n height 200\n depth 200\n}\n"
			"standard_object\n{\n name water\n geometry water_geometry\n material water_material\n}\n";
	}
	// The radiance-map painter must precede the rasterizer. Pure PT has
	// no scene max_depth knob; its existing C++ setter is used below.
	s << "standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"
		"pathtracing_pel_rasterizer\n{\n samples " << cfg.samples <<
		"\n oidn_denoise FALSE\n pixel_filter box\n adaptive_max_samples 0\n"
		" pathguiding FALSE\n direct_clamp 0\n indirect_clamp 0\n"
		" rr_min_depth 8\n rr_threshold 0.05\n max_volume_bounce 256\n"
		" max_translucent_bounce 256\n max_transmission_bounce 256\n"
		" radiance_map white\n radiance_scale 1\n radiance_background TRUE\n}\n"
		"file_rasterizeroutput\n{\n pattern rendered/sss_radiance_scaling_unused\n"
		" type EXR\n bpp 32\n color_space Rec709RGB_Linear\n}\n";
	return s.str();
}

// Guards inspect the loaded production objects and camera, not just strings.
bool CheckGeometry( IJobPriv& job, Model model, Topology topology, const std::string& label, bool lightingControl )
{
	const ICamera* camera = job.GetScene() ? job.GetScene()->GetCamera() : nullptr;
	const IObjectManager* objects = job.GetObjects();
	if( !Check( camera && objects, label + ": loaded camera and object manager" ) ) return false;
	const IObject* slab = objects->GetItem( "slab" );
	const IObject* water = objects->GetItem( "water" );
	if( !Check( slab && slab->GetMaterial(), label + ": slab and material exist" ) ) return false;
	const IMaterial* material = slab->GetMaterial();
	const bool explicitVolume = model == Model::ExplicitDielectricVolume && !lightingControl;
	if( !Check( (slab->GetInteriorMedium() != nullptr) == explicitVolume &&
		(material->GetDiffusionProfile() != nullptr) == (model == Model::DiffusionSSS && !lightingControl) &&
		(material->GetRandomWalkSSSParams() != nullptr) == (model == Model::RandomWalkSSS && !lightingControl),
		label + ": requested modern transport model is bound" ) ) return false;
	if( !Check( (water != nullptr) == (topology != Topology::Air), label + ": enclosure presence" ) ) return false;
	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	// Include film corners so the full pixel footprints fit, not only centers.
	for( unsigned int y = 0; y <= kFilmSize; ++y ) {
		for( unsigned int x = 0; x <= kFilmSize; ++x ) {
			Ray ray;
			if( !Check( camera->GenerateRay( rc, ray, Point2( x, y ) ), label + ": camera ray exists" ) ) return false;
			const double expectedX = double(x) / kFilmSize - 0.5;
			const double expectedY = double(y) / kFilmSize - 0.5;
			if( !Check( Near( ray.origin.x, expectedX ) && Near( ray.origin.y, expectedY ) &&
				Near( ray.origin.z, topology == Topology::WaterOutside ? 120 : 4 ) &&
				Near( ray.dir.x, 0 ) && Near( ray.dir.y, 0 ) && Near( ray.dir.z, -1 ),
				label + ": matched orthographic footprint and normal incidence" ) ) return false;
			const RasterizerState rast = { x, y };
			RayIntersection hit( ray, rast );
			slab->IntersectRay( hit, 1000, true, false, false );
			const double expectedZ = lightingControl ? std::sqrt(1 - expectedX * expectedX - expectedY * expectedY) : 0;
			if( !Check( hit.geometric.bHit && Near( hit.geometric.ptIntersection.x, expectedX ) &&
				Near( hit.geometric.ptIntersection.y, expectedY ) && Near( hit.geometric.ptIntersection.z, expectedZ ) &&
				Near( hit.geometric.vNormal.z, lightingControl ? expectedZ : 1 ),
				label + ": exact front-face geometry hit" ) ) return false;
			if( water ) {
				RayIntersection enclosureHit( ray, rast );
				water->IntersectRay( enclosureHit, 1000, true, true, false );
				if( !Check( enclosureHit.geometric.bHit && Near( enclosureHit.geometric.ptIntersection.z,
					topology == Topology::WaterOutside ? 100 : -100 ), label + ": correct side of enclosure" ) ) return false;
			}
		}
	}
	return true;
}

class Capture : public virtual IRasterizerOutput, public virtual Reference {
public:
	unsigned int width = 0, height = 0;
	std::vector<RISEColor> pixels;
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage& image, const Rect*, const unsigned int ) override
	{
		width = image.GetWidth(); height = image.GetHeight();
		pixels.resize( size_t(width) * height );
		for( unsigned int y = 0; y < height; ++y )
			for( unsigned int x = 0; x < width; ++x ) pixels[size_t(y) * width + x] = image.GetPEL( x, y );
	}
protected:
	~Capture() override = default;
};

bool ComputeMean( const Capture& capture, RGB& mean, const std::string& label )
{
	if( !Check( capture.width == kFilmSize && capture.height == kFilmSize &&
		capture.pixels.size() == size_t(kFilmSize) * kFilmSize, label + ": complete 16x16 capture" ) ) return false;
	RGB sum{};
	for( size_t pixel = 0; pixel < capture.pixels.size(); ++pixel ) {
		const RISEColor& color = capture.pixels[pixel];
		const RGB raw = { color.base.r, color.base.g, color.base.b };
		if( !Check( std::isfinite( color.a ) && color.a >= 0 && color.a <= 1,
			label + ": finite coverage alpha at pixel " + std::to_string(pixel) ) ) return false;
		for( size_t channel = 0; channel < 3; ++channel ) {
			const double value = raw[channel] * color.a;
			if( !Check( std::isfinite( raw[channel] ) && std::isfinite( value ),
				label + ": finite raw/composited channel " + std::to_string(channel) + " pixel " + std::to_string(pixel) ) ) return false;
			sum[channel] += value;
		}
	}
	for( size_t channel = 0; channel < 3; ++channel ) {
		mean[channel] = sum[channel] / capture.pixels.size();
		if( !Check( std::isfinite( mean[channel] ) && mean[channel] > 1e-8,
			label + ": finite nonzero image mean channel " + std::to_string(channel) ) ) return false;
	}
	return true;
}

struct OwnedInput {
	std::filesystem::path path;
	~OwnedInput() { std::error_code ec; std::filesystem::remove( path, ec ); }
};
bool Render( Model model, Topology topology, const Config& cfg, RGB& mean, const std::string& label, bool lightingControl = false )
{
	std::error_code ec;
	std::filesystem::create_directories( "rendered/sss_radiance_scaling", ec );
	if( !Check( !ec, label + ": create relative input directory" ) ) return false;
	OwnedInput input{ std::filesystem::path("rendered/sss_radiance_scaling") /
		("scene_" + std::to_string(::getpid()) + "_" + std::to_string(renderIndex) + ".RISEscene") };
	std::ofstream stream( input.path );
	stream << BuildScene( model, topology, cfg, lightingControl );
	stream.close();
	if( !Check( !stream.fail(), label + ": write complete scene input" ) ) return false;
	IJobPriv* job = nullptr;
	if( !Check( RISE_CreateJobPriv( &job ) && job, label + ": create job" ) ) { safe_release(job); return false; }
	if( !Check( job->LoadAsciiSceneViaCst( input.path.string().c_str() ), label + ": parse native v7 scene" ) ||
		!CheckGeometry( *job, model, topology, label, lightingControl ) ) { safe_release(job); return false; }
	PathTracingPelRasterizer* pt = dynamic_cast<PathTracingPelRasterizer*>( job->GetRasterizer() );
	if( !Check( pt != nullptr, label + ": pure RGB PT rasterizer" ) ) { safe_release(job); return false; }
	pt->SetMaxPathDepth( 1024 );
	// Remove the relative linear-EXR declaration before capture: no output
	// file or encoded/denoised image participates in the measurement.
	job->RemoveRasterizerOutputs();
	Capture* capture = new Capture();
	GlobalLog()->PrintNew( capture, __FILE__, __LINE__, "SSS test capture output" );
	pt->AddRasterizerOutput( capture );
	std::srand( cfg.seedBase + renderIndex++ );
	const bool rendered = job->Rasterize();
	const bool valid = Check( rendered, label + ": render completed" ) && ComputeMean( *capture, mean, label );
	safe_release( capture );
	safe_release( job );
	return valid;
}

void PrintRGB( const RGB& rgb )
{
	std::cout << '(' << rgb[0] << ',' << rgb[1] << ',' << rgb[2] << ')';
}
bool Aggregate( const std::vector<RGB>& values, RGB& mean, const std::string& label )
{
	RGB m2{};
	mean = RGB{};
	size_t n = 0;
	for( const RGB& sample : values ) {
		++n;
		for( size_t channel = 0; channel < 3; ++channel ) {
			const double delta = sample[channel] - mean[channel];
			mean[channel] += delta / n;
			m2[channel] += delta * (sample[channel] - mean[channel]);
		}
	}
	RGB stdev{}, stderrMean{};
	for( size_t channel = 0; channel < 3; ++channel ) {
		if( !Check( n > 0 && std::isfinite(mean[channel]) && mean[channel] > 1e-8 &&
			std::isfinite(m2[channel]) && m2[channel] >= 0, label + ": valid aggregate" ) ) return false;
		if( n > 1 ) { stdev[channel] = std::sqrt(m2[channel] / (n - 1)); stderrMean[channel] = stdev[channel] / std::sqrt(double(n)); }
	}
	std::cout << "AGGREGATE " << label << " K=" << n << " mean=";
	PrintRGB(mean);
	if( n > 1 ) { std::cout << " stdev="; PrintRGB(stdev); std::cout << " stderr="; PrintRGB(stderrMean); }
	else std::cout << " stdev=unavailable stderr=unavailable (K=1)";
	std::cout << std::endl;
	return true;
}
bool Ratio( const RGB& numerator, const RGB& denominator, RGB& result, const std::string& label )
{
	for( size_t channel = 0; channel < 3; ++channel ) {
		if( !Check( std::isfinite(numerator[channel]) && std::isfinite(denominator[channel]) &&
			denominator[channel] > 1e-8, label + ": finite positive denominator channel " + std::to_string(channel) ) ) return false;
		result[channel] = numerator[channel] / denominator[channel];
		if( !Check( std::isfinite(result[channel]) && result[channel] > 0, label + ": finite positive ratio" ) ) return false;
	}
	std::cout << "RATIO " << label << " = "; PrintRGB(result); std::cout << std::endl;
	return true;
}
} // namespace

int main( int argc, char** argv )
{
	Config cfg;
	if( !ParseArgs( argc, argv, cfg ) ) {
		std::cerr << "Usage: SSSRadianceScalingTest [--probe] [--samples square_N] [--trials K] [--seed N] [--ior N] [--outer-fresnel]\n";
		return 2;
	}
	std::cout << std::setprecision(10) << "SSSRadianceScalingTest: PROVISIONAL " << (cfg.probe ? "PROBE" : "MEASUREMENT") <<
		"; no eta-normalization assertion\nsamples=" << cfg.samples << " trials=" << cfg.trials <<
		" seed_base=" << cfg.seedBase << " surface_ior=" << cfg.surfaceIOR <<
		" sigma_a=0 sigma_s=2 g=0 film=16x16 max_path_depth=1024 volume_cap=256 rw_cap=512\nouter=" <<
		(cfg.outerFresnel ? "Fresnel dielectric water (additive reflected environment affects outside camera)" :
		"ideal nonreflecting IOR enclosure, n=1.33") <<
		"\nSeeds vary per render; worker scheduling still prevents bitwise reproducibility.\n" << std::flush;
	// Cheap air-furnace lighting/capture control before the expensive matrix.
	Config controlConfig = cfg;
	controlConfig.samples = 4;
	RGB controlMean{};
	if( !Render(Model::ExplicitDielectricVolume, Topology::Air, controlConfig, controlMean, "white_lambertian_sphere_control", true) ) return 1;
	std::cout << "LIGHTING_CONTROL air white Lambertian sphere samples=4 RGB_mean=";
	PrintRGB(controlMean); std::cout << " (uniform-furnace reference=1; diagnostic)" << std::endl;
	const Model models[] = { Model::ExplicitDielectricVolume, Model::DiffusionSSS, Model::RandomWalkSSS };
	const Topology topologies[] = { Topology::Air, Topology::WaterInside, Topology::WaterOutside };
	std::array<RGB, 3> observerRatios{};
	for( size_t m = 0; m < 3; ++m ) {
		std::array<RGB, 3> means{};
		for( size_t t = 0; t < 3; ++t ) {
			const std::string label = std::string(ModelName(models[m])) + "/" + TopologyName(topologies[t]);
			std::vector<RGB> values;
			for( unsigned int trial = 0; trial < cfg.trials; ++trial ) {
				RGB mean{};
				const std::string trialLabel = label + "/trial=" + std::to_string(trial);
				std::cout << "RENDER " << trialLabel << " seed=" << cfg.seedBase + renderIndex << std::endl;
				if( !Render(models[m], topologies[t], cfg, mean, trialLabel) ) {
					std::cerr << "INCOMPLETE: invalid render; no aggregate or ratios for this matrix.\n";
					return 1;
				}
				std::cout << "TRIAL " << trialLabel << " RGB_mean="; PrintRGB(mean); std::cout << std::endl;
				values.push_back(mean);
			}
			if( !Aggregate(values, means[t], label) ) return 1;
		}
		if( !Ratio(means[1], means[2], observerRatios[m], std::string(ModelName(models[m])) + " water_inside/water_outside") ) return 1;
		RGB immersionRatio{};
		if( !Ratio(means[1], means[0], immersionRatio, std::string(ModelName(models[m])) + " water_inside/air_furnace") ) return 1;
	}
	for( size_t m = 1; m < 3; ++m ) {
		RGB ratioOfRatios{};
		if( !Ratio(observerRatios[m], observerRatios[0], ratioOfRatios,
			std::string(ModelName(models[m])) + " observer_ratio/explicit_observer_ratio") ) return 1;
	}
	std::cout << "Guards passed: " << passCount << " failed: " << failCount <<
		". Provisional measurements complete; no physics acceptance band or eta closure claimed.\n";
	return failCount == 0 ? 0 : 1;
}
