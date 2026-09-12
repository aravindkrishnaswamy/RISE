// DL-04 complete-event eta convention consistency pin.
// Coarse bounds reject an unmatched eta square; they do not certify exact
// SSS energy conservation. DL48/49/52/53 and finite walk caps remain separate.
//
// Exact guards: the loaded camera samples the same slab top in every model,
// the requested modern material/medium is bound, and every captured RGB and
// alpha value is finite. Nonzero image means are only smoke guards.
// Measurements: serial repeated renders, whole-film RGB means, their
// repeat standard deviation and descriptive stdev/sqrt(K), and channelwise ratios.
// PT reuses its fixed pixel Sobol scramble: these repeats are NOT independent
// QMC replicates and their dispersion is NOT an integration-error estimate.
// No invalid render may enter an aggregate or be replaced with zero.
//
// A full BSSRDF event can cancel entry 1/eta^2 against exit eta^2. An outer
// camera ratio alone cannot prove the BSSRDF normalization. Absolute air
// furnace measurements and later mutation discriminators must establish it.
// The unit conservative furnace supplies the reference. A deliberate 20%
// convention-only allowance retains separately recorded normalization, MIS
// and sampling errors while rejecting eta^2 and inverse-eta^2 at 1.5 or 2.
// K=4 is an initial sanity measurement; K=1 has no variance estimate.
//
// Run from the repository root with RISE_MEDIA_PATH="$PWD/". Defaults:
// 16x16, 256 spp, 4 trials, ior 1.5, sigma_a=0, sigma_s=2, g=0.
// --probe labels a diagnostic run; --samples N (perfect square), --trials K,
// --seed N, --ior N, --outer-fresnel, --slab-radius R are optional.
// The default curved ellipsoid is (R,R,10) centered at z=-10, R=40.
// --flat selects the diagnostic box for the DL-52 planar probe-origin hole;
// --curved explicitly restores curved geometry (last shape flag wins).
// --volume-cap N / --rw-cap N / --path-cap N default to 1024 / 8192 / 4096.
// --air-only implies --probe and runs all three models' air baseline only.
// --helper-only implies --probe, loads only the diffusion air scene, and
// samples the actual BSSRDF helper without any rasterization. Its trials use
// distinct local RNG seeds. --helper-attempts N defaults to 256 and also
// controls the coverage diagnostic attached to ordinary diffusion renders.
// An actual SampleEntryPoint probe records valid and near-top samples; curved
// diffusion requires near-top coverage so it cannot pass on surface Fresnel
// alone. This coverage guard does not establish energy normalization.
// Normal and --probe invocations currently execute the same selected
// matrix; --probe reports measurements without convention bounds.

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
#include "../src/Library/Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/IndependentSampler.h"

using namespace RISE;
using namespace RISE::Implementation;
namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace {
using RGBChannels = std::array<double, 3>;
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
	unsigned int samples = 256, trials = 4, seedBase = 1000;
	unsigned int volumeCap = 1024, rwCap = 8192, pathCap = 4096;
	unsigned int helperAttempts = 256;
	double surfaceIOR = 1.5, slabRadius = 40;
	bool probe = false, outerFresnel = false, curved = true, airOnly = false;
	bool helperOnly = false;
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
		else if( arg == "--curved" ) cfg.curved = true;
		else if( arg == "--flat" ) cfg.curved = false;
		else if( arg == "--air-only" ) { cfg.airOnly = true; cfg.probe = true; }
		else if( arg == "--helper-only" ) { cfg.helperOnly = true; cfg.probe = true; }
		else if( arg == "--samples" || arg == "--trials" || arg == "--seed" ||
			arg == "--volume-cap" || arg == "--rw-cap" || arg == "--path-cap" || arg == "--helper-attempts" ) {
			if( ++i == argc ) return false;
			unsigned int value = 0;
			if( !ParseUInt( argv[i], value ) ) return false;
			if( arg == "--samples" ) cfg.samples = value;
			else if( arg == "--trials" ) cfg.trials = value;
			else if( arg == "--seed" ) cfg.seedBase = value;
			else if( arg == "--volume-cap" ) cfg.volumeCap = value;
			else if( arg == "--rw-cap" ) cfg.rwCap = value;
			else if( arg == "--path-cap" ) cfg.pathCap = value;
			else cfg.helperAttempts = value;
		} else if( arg == "--ior" || arg == "--slab-radius" ) {
			if( ++i == argc ) return false;
			errno = 0;
			char* end = nullptr;
			double& value = arg == "--ior" ? cfg.surfaceIOR : cfg.slabRadius;
			value = std::strtod( argv[i], &end );
			if( errno || end == argv[i] || *end || !std::isfinite(value) || value <= 0 ) return false;
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
		if( model == Model::RandomWalkSSS ) s << " max_bounces " << cfg.rwCap << "\n";
		s << "}\n";
	}
	// Shared geometry: x/y +/-40, z [-20,0]. The camera is never inside
	// the SSS solid (ordinary SSS absorbs a primary back-face hit).
	if( lightingControl ) s << "sphere_geometry\n{\n name slab_geometry\n radius 1\n}\n";
	else if( cfg.curved ) s << "ellipsoid_geometry\n{\n name slab_geometry\n radii " << cfg.slabRadius << ' ' << cfg.slabRadius << " 10\n}\n";
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
		" rr_min_depth 8\n rr_threshold 0.05\n max_volume_bounce " << cfg.volumeCap << "\n"
		" max_translucent_bounce 256\n max_transmission_bounce 256\n"
		" radiance_map white\n radiance_scale 1\n radiance_background TRUE\n}\n"
		"file_rasterizeroutput\n{\n pattern rendered/sss_radiance_scaling_unused\n"
		" type EXR\n bpp 32\n color_space Rec709RGB_Linear\n}\n";
	return s.str();
}

// Guards inspect the loaded production objects and camera, not just strings.
bool CheckGeometry( IJobPriv& job, Model model, Topology topology, const Config& cfg, const std::string& label, bool lightingControl )
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
				Near( ray.Dir().x, 0 ) && Near( ray.Dir().y, 0 ) && Near( ray.Dir().z, -1 ),
				label + ": matched orthographic footprint and parallel -Z rays" ) ) return false;
			const RasterizerState rast = { x, y };
			RayIntersection hit( ray, rast );
			slab->IntersectRay( hit, 1000, true, false, false );
			double expectedZ = 0;
			Vector3 expectedNormal( 0, 0, 1 );
			if( lightingControl ) {
				expectedZ = std::sqrt(1 - expectedX * expectedX - expectedY * expectedY);
				expectedNormal = Vector3(expectedX, expectedY, expectedZ);
			} else if( cfg.curved ) {
				const double scaledX = expectedX / cfg.slabRadius;
				const double scaledY = expectedY / cfg.slabRadius;
				const double radicand = 1 - scaledX * scaledX - scaledY * scaledY;
				if( !Check( std::isfinite(radicand) && radicand > 0,
					label + ": curved slab covers entire film footprint" ) ) return false;
				expectedZ = -10 + 10 * std::sqrt(radicand);
				// Gradient of x^2/R^2 + y^2/R^2 + (z+10)^2/100 = 1.
				expectedNormal = Vector3Ops::Normalize(Vector3(
					scaledX / cfg.slabRadius, scaledY / cfg.slabRadius, (expectedZ + 10) / 100));
			}
			if( !Check( hit.geometric.bHit && Near( hit.geometric.ptIntersection.x, expectedX ) &&
				Near( hit.geometric.ptIntersection.y, expectedY ) && Near( hit.geometric.ptIntersection.z, expectedZ ) &&
				Near( hit.geometric.vNormal.x, expectedNormal.x ) && Near( hit.geometric.vNormal.y, expectedNormal.y ) &&
				Near( hit.geometric.vNormal.z, expectedNormal.z ),
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

// This invokes the real helper on the loaded central camera hit. A profile
// pointer proves binding only; it does not prove that BSSRDF continuation
// reaches the illuminated near surface (the flat-box failure in DL-52).
bool DiagnoseBSSRDFCoverage( IJobPriv& job, const Config& cfg, const std::string& label, bool conventionGuard = false )
{
	const IObject* slab = job.GetObjects()->GetItem("slab");
	const ICamera* camera = job.GetScene()->GetCamera();
	RandomNumberGenerator rng( cfg.seedBase + renderIndex );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	IndependentSampler sampler( rng );
	Ray ray;
	if( !Check(camera->GenerateRay(rc, ray, Point2(kFilmSize / 2, kFilmSize / 2)),
		label + ": central camera ray for BSSRDF coverage") ) return false;
	const RasterizerState rast = { kFilmSize / 2, kFilmSize / 2 };
	RayIntersection hit( ray, rast );
	slab->IntersectRay(hit, 1000, true, false, false);
	if( !Check(hit.geometric.bHit && Near(hit.geometric.ptIntersection.z, 0),
		label + ": central primary slab hit for BSSRDF coverage") ) return false;
	const unsigned int attempts = cfg.helperAttempts;
	unsigned int valid = 0, nearTop = 0, zOnlyNearTop = 0;
	RGBChannels spatialSum{}, fullSum{};
	for( unsigned int attempt = 0; attempt < attempts; ++attempt ) {
		const BSSRDFSampling::SampleResult result = BSSRDFSampling::SampleEntryPoint(
			hit.geometric, slab, slab->GetMaterial(), sampler, 0);
		// Rejected helper samples contribute zero to the unconditional
		// estimator. This is a physical sampling outcome, not replacement of
		// an invalid/nonfinite image. Every successful sample is checked.
		if( !result.valid ) continue;
		if( !Check(std::isfinite(result.entryPoint.x) && std::isfinite(result.entryPoint.y) &&
			std::isfinite(result.entryPoint.z) && std::isfinite(result.pdfSurface) && result.pdfSurface > 0 &&
			std::isfinite(result.entryGeomNormal.x) && std::isfinite(result.entryGeomNormal.y) && std::isfinite(result.entryGeomNormal.z) &&
			std::isfinite(result.weight.r) && std::isfinite(result.weight.g) && std::isfinite(result.weight.b) &&
			std::isfinite(result.weightSpatial.r) && std::isfinite(result.weightSpatial.g) && std::isfinite(result.weightSpatial.b),
			label + ": successful BSSRDF helper sample has finite point, weight and positive PDF") ) return false;
		const RGBChannels spatial = { result.weightSpatial.r, result.weightSpatial.g, result.weightSpatial.b };
		const RGBChannels full = { result.weight.r, result.weight.g, result.weight.b };
		for( size_t channel = 0; channel < 3; ++channel ) {
			spatialSum[channel] += spatial[channel];
			fullSum[channel] += full[channel];
		}
		++valid;
		if( result.entryPoint.z > -1 ) {
			++zOnlyNearTop;
			const Vector3 displacement = Vector3Ops::mkVector3(result.entryPoint, hit.geometric.ptIntersection);
			const double distanceSquared = Vector3Ops::Dot(displacement, displacement);
			if( !Check(std::isfinite(distanceSquared), label + ": finite helper displacement") ) return false;
			if( distanceSquared < 25 && result.entryGeomNormal.z > 0.9 ) ++nearTop;
		}
	}
	std::cout << "BSSRDF_COVERAGE " << label << " attempts=" << attempts << " valid=" << valid <<
		" nearTop=" << nearTop << " (entryPoint.z>-1, distance_from_central_hit<5, entryGeomNormal.z>0.9)" <<
		" zOnlyNearTop=" << zOnlyNearTop << " (legacy z-only diagnostic; can include far sidewalls)" << std::endl;
	if( cfg.helperOnly ) {
		const ISubSurfaceDiffusionProfile* profile = slab->GetMaterial()->GetDiffusionProfile();
		if( !Check(profile != nullptr, label + ": diffusion profile for measured exit transmission") ) return false;
		const double ftExit = profile->FresnelTransmission(1, hit.geometric);
		if( !Check(std::isfinite(ftExit) && ftExit > 0, label + ": finite positive measured FtExit denominator") ) return false;
		RGBChannels spatialMean{}, fullMean{};
		for( size_t channel = 0; channel < 3; ++channel ) {
			spatialMean[channel] = spatialSum[channel] / attempts;
			fullMean[channel] = fullSum[channel] / attempts;
			if( !Check(std::isfinite(spatialMean[channel]) && std::isfinite(fullMean[channel]),
				label + ": finite unconditional helper mean") ) return false;
		}
		const double meanJ = spatialMean[0] / ftExit;
		if( !Check(std::isfinite(meanJ), label + ": finite measured mean J") ) return false;
		std::cout << "HELPER_MEAN " << label << " seed=" << cfg.seedBase + renderIndex <<
			" denominator_all_attempts=" << attempts <<
			" spatial_RGB=(" << spatialMean[0] << ',' << spatialMean[1] << ',' << spatialMean[2] << ')' <<
			" full_RGB=(" << fullMean[0] << ',' << fullMean[1] << ',' << fullMean[2] << ')' <<
			" FtExit=" << ftExit << " mean_J=spatial_red/FtExit=" << meanJ << std::endl;
		if( conventionGuard ) {
			Check(meanJ > 0.9 && meanJ < 1.1, label + ": independent spatial integral within 10% of unit conservative profile");
			for( size_t channel = 0; channel < 3; ++channel ) {
				const double furnace = 1 - ftExit + fullMean[channel];
				Check(furnace > 0.8 && furnace < 1.2, label + ": independent full event rejects unmatched eta square (20% convention bound)");
			}
		}
	}
	if( cfg.curved ) return Check(nearTop > 0, label + ": curved BSSRDF helper reaches near top");
	// Known flat probe-origin failure remains visible; do not turn a binding
	// check or the finite reflected-surface signal into an SSS-activity claim.
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

bool ComputeMean( const Capture& capture, RGBChannels& mean, const std::string& label )
{
	if( !Check( capture.width == kFilmSize && capture.height == kFilmSize &&
		capture.pixels.size() == size_t(kFilmSize) * kFilmSize, label + ": complete 16x16 capture" ) ) return false;
	RGBChannels sum{};
	for( size_t pixel = 0; pixel < capture.pixels.size(); ++pixel ) {
		const RISEColor& color = capture.pixels[pixel];
		const RGBChannels raw = { color.base.r, color.base.g, color.base.b };
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
bool RunHelperOnly( const Config& cfg, bool conventionGuard = false )
{
	std::error_code ec;
	std::filesystem::create_directories("rendered/sss_radiance_scaling", ec);
	if( !Check(!ec, "helper-only: create relative input directory") ) return false;
	OwnedInput input{ std::filesystem::path("rendered/sss_radiance_scaling") /
		("helper_" + std::to_string(::getpid()) + ".RISEscene") };
	std::ofstream stream(input.path);
	stream << BuildScene(Model::DiffusionSSS, Topology::Air, cfg, false);
	stream.close();
	if( !Check(!stream.fail(), "helper-only: write complete scene input") ) return false;
	IJobPriv* job = nullptr;
	if( !Check(RISE_CreateJobPriv(&job) && job, "helper-only: create job") ) { safe_release(job); return false; }
	if( !Check(job->LoadAsciiSceneViaCst(input.path.string().c_str()), "helper-only: parse native v7 scene") ||
		!CheckGeometry(*job, Model::DiffusionSSS, Topology::Air, cfg, "helper-only/diffusion_air", false) ) {
		safe_release(job); return false;
	}
	job->RemoveRasterizerOutputs();
	for( unsigned int trial = 0; trial < cfg.trials; ++trial ) {
		if( !DiagnoseBSSRDFCoverage(*job, cfg, "helper-only/diffusion_air/trial=" + std::to_string(trial), conventionGuard) ) {
			safe_release(job); return false;
		}
		++renderIndex; // Next trial constructs a fresh, distinctly seeded local RNG.
	}
	safe_release(job);
	return true;
}
bool Render( Model model, Topology topology, const Config& cfg, RGBChannels& mean, const std::string& label, bool lightingControl = false )
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
		!CheckGeometry( *job, model, topology, cfg, label, lightingControl ) ) { safe_release(job); return false; }
	if( model == Model::DiffusionSSS && !lightingControl && !DiagnoseBSSRDFCoverage(*job, cfg, label) ) {
		safe_release(job); return false;
	}
	PathTracingPelRasterizer* pt = dynamic_cast<PathTracingPelRasterizer*>( job->GetRasterizer() );
	if( !Check( pt != nullptr, label + ": pure RGB PT rasterizer" ) ) { safe_release(job); return false; }
	pt->SetMaxPathDepth( cfg.pathCap );
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

void PrintRGB( const RGBChannels& rgb )
{
	std::cout << '(' << rgb[0] << ',' << rgb[1] << ',' << rgb[2] << ')';
}
bool Aggregate( const std::vector<RGBChannels>& values, RGBChannels& mean, const std::string& label )
{
	RGBChannels m2{};
	mean = RGBChannels{};
	size_t n = 0;
	for( const RGBChannels& sample : values ) {
		++n;
		for( size_t channel = 0; channel < 3; ++channel ) {
			const double delta = sample[channel] - mean[channel];
			mean[channel] += delta / n;
			m2[channel] += delta * (sample[channel] - mean[channel]);
		}
	}
	RGBChannels stdev{}, stderrMean{};
	for( size_t channel = 0; channel < 3; ++channel ) {
		if( !Check( n > 0 && std::isfinite(mean[channel]) && mean[channel] > 1e-8 &&
			std::isfinite(m2[channel]) && m2[channel] >= 0, label + ": valid aggregate" ) ) return false;
		if( n > 1 ) { stdev[channel] = std::sqrt(m2[channel] / (n - 1)); stderrMean[channel] = stdev[channel] / std::sqrt(double(n)); }
	}
	std::cout << "AGGREGATE " << label << " K=" << n << " mean=";
	PrintRGB(mean);
	if( n > 1 ) { std::cout << " repeat_stdev="; PrintRGB(stdev); std::cout << " descriptive_stdev_over_sqrtK="; PrintRGB(stderrMean); }
	else std::cout << " repeat_stdev=unavailable descriptive_stdev_over_sqrtK=unavailable (K=1)";
	std::cout << std::endl;
	return true;
}
bool Ratio( const RGBChannels& numerator, const RGBChannels& denominator, RGBChannels& result, const std::string& label )
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
		std::cerr << "Usage: SSSRadianceScalingTest [--probe] [--samples square_N] [--trials K] [--seed N] [--ior N] [--outer-fresnel] [--curved|--flat] [--slab-radius R] [--volume-cap N] [--rw-cap N] [--path-cap N] [--air-only] [--helper-only] [--helper-attempts N]\n";
		return 2;
	}
	if( !cfg.probe && (!cfg.curved || cfg.slabRadius != 40 || cfg.outerFresnel ||
		(cfg.surfaceIOR != 1.5 && cfg.surfaceIOR != 2.0) || cfg.samples < 256 || cfg.trials < 4) ) {
		std::cerr << "Convention bounds require curved R=40, ideal enclosure, IOR 1.5 or 2, at least 256 spp and 4 trials; use --probe for other diagnostics.\n";
		return 2;
	}
	std::cout << std::setprecision(10) << "SSSRadianceScalingTest: " << (cfg.probe ? "PROBE (no convention bounds)" : "CONVENTION CONSISTENCY PIN") <<
		"\nsamples=" << cfg.samples << " trials=" << cfg.trials <<
		" seed_base=" << cfg.seedBase << " surface_ior=" << cfg.surfaceIOR <<
		" sigma_a=0 sigma_s=2 g=0 film=16x16 max_path_depth=" << cfg.pathCap <<
		" volume_cap=" << cfg.volumeCap << " rw_cap=" << cfg.rwCap << " helper_attempts=" << cfg.helperAttempts <<
		"\nmatrix=" << (cfg.helperOnly ? "helper_only (diffusion air; no rasterization)" :
			(cfg.airOnly ? "air_only (probe; no observer ratios)" : "all_three_topologies")) << "\nshape=" <<
		(cfg.curved ? "ellipsoid" : "flat_box") << " slab_radius=" << cfg.slabRadius <<
		(cfg.curved ? " radii=(R,R,10), center_z=-10\nouter=" : " (unused for flat box: dimensions=80x80x20, center_z=-10)\nouter=") <<
		(cfg.outerFresnel ? "Fresnel dielectric water (additive reflected environment affects outside camera)" :
		"ideal nonreflecting IOR enclosure, n=1.33") <<
		"\n" << std::flush;
	if( cfg.helperOnly ) {
		std::cout << "Helper trials use separately seeded local IndependentSampler streams; no pixel Sobol samples or rasterization.\n";
		if( !RunHelperOnly(cfg) ) return 1;
		std::cout << "Helper guards passed: " << passCount << " failed: " << failCount <<
			". Unconditional helper measurements complete; no energy acceptance band.\n";
		return failCount == 0 ? 0 : 1;
	}
	std::cout << "Libc seeds vary per render; fixed pixel Sobol scrambles repeat. Dispersion is descriptive only, not QMC uncertainty; worker scheduling prevents bitwise reproducibility.\n";
	if( !cfg.probe ) {
		Config independent = cfg;
		independent.helperOnly = true;
		independent.helperAttempts = 100000;
		if( !RunHelperOnly(independent, true) || failCount != 0 ) return 1;
	}
	// Cheap air-furnace lighting/capture control before the expensive matrix.
	Config controlConfig = cfg;
	controlConfig.samples = 4;
	RGBChannels controlMean{};
	if( !Render(Model::ExplicitDielectricVolume, Topology::Air, controlConfig, controlMean, "white_lambertian_sphere_control", true) ) return 1;
	std::cout << "LIGHTING_CONTROL air white Lambertian sphere samples=4 RGB_mean=";
	PrintRGB(controlMean); std::cout << " (uniform-furnace reference=1; diagnostic)" << std::endl;
	const Model models[] = { Model::ExplicitDielectricVolume, Model::DiffusionSSS, Model::RandomWalkSSS };
	const Topology topologies[] = { Topology::Air, Topology::WaterInside, Topology::WaterOutside };
	const size_t topologyCount = cfg.airOnly ? 1 : 3;
	std::array<RGBChannels, 3> observerRatios{};
	for( size_t m = 0; m < 3; ++m ) {
		std::array<RGBChannels, 3> means{};
		for( size_t t = 0; t < topologyCount; ++t ) {
			const std::string label = std::string(ModelName(models[m])) + "/" + TopologyName(topologies[t]);
			std::vector<RGBChannels> values;
			for( unsigned int trial = 0; trial < cfg.trials; ++trial ) {
				RGBChannels mean{};
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
			if( !cfg.probe && t == 0 ) {
				for( size_t channel = 0; channel < 3; ++channel ) {
					const double allowance = m == 0 ? 0.03 : 0.20;
					Check(std::fabs(means[t][channel] - 1) < allowance,
						label + ": absolute conservative furnace rejects unmatched eta square (coarse convention bound)");
				}
			}
		}
		if( cfg.airOnly ) continue;
		if( !Ratio(means[1], means[2], observerRatios[m], std::string(ModelName(models[m])) + " water_inside/water_outside") ) return 1;
		RGBChannels immersionRatio{};
		if( !Ratio(means[1], means[0], immersionRatio, std::string(ModelName(models[m])) + " water_inside/air_furnace") ) return 1;
	}
	for( size_t m = 1; !cfg.airOnly && m < 3; ++m ) {
		RGBChannels ratioOfRatios{};
		if( !Ratio(observerRatios[m], observerRatios[0], ratioOfRatios,
			std::string(ModelName(models[m])) + " observer_ratio/explicit_observer_ratio") ) return 1;
		if( !cfg.probe ) for( double ratio : ratioOfRatios )
			Check(std::fabs(ratio - 1) < 0.10, "observer ratio-of-ratios within 10% wiring bound; not the eta discriminator");
	}
	std::cout << "Guards passed: " << passCount << " failed: " << failCount <<
		(cfg.probe ? ". Probe complete; no convention bounds applied.\n" : ". Complete-event convention checks complete; exact energy conservation is not asserted.\n");
	return failCount == 0 ? 0 : 1;
}
