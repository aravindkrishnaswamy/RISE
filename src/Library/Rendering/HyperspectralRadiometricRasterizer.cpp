//////////////////////////////////////////////////////////////////////
//
//  HyperspectralRadiometricRasterizer.cpp - Raw radiometric fixed-
//    wavelength hyperspectral rasterizer (P6b).
//
//  Strict-FP TU: compiled without -ffast-math to preserve exact
//  IEEE-754 binary64 radiometric accounting.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#if defined(__FAST_MATH__)
#error "HyperspectralRadiometricRasterizer.cpp must be compiled without -ffast-math"
#endif

#include "pch.h"
#include "HyperspectralRadiometricRasterizer.h"
#include "RadiometricCheckpoint.h"
#include "SpectralSourceDomain.h"
#include "RayCaster.h"
#include "../RasterImages/RadiometricEXRWriter.h"
#include "../Shaders/PathTracingIntegrator.h"
#include "../Managers/ObjectManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/IFilm.h"
#include "../Interfaces/IRadianceMap.h"
#include "../Lights/PointLight.h"
#include "../Lights/SpotLight.h"
#include "../Lights/DirectionalLight.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/ThreadPool.h"
#include "../Utilities/RISECBOR64.h"
#include "../Utilities/DiskFileWriteBuffer.h"
#include "../Utilities/Log/Log.h"
#include "../Agent/Json.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

using namespace RISE;
using namespace RISE::Implementation;

namespace {

std::string ToLowerASCII( const std::string& s )
{
	std::string out;
	out.reserve( s.size() );
	for( unsigned char c : s ) {
		out.push_back( static_cast<char>( std::tolower( c ) ) );
	}
	return out;
}

std::uint64_t SplitMix64( std::uint64_t x ) noexcept
{
	x += 0x9e3779b97f4a7c15ULL;
	x = ( x ^ ( x >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
	x = ( x ^ ( x >> 27 ) ) * 0x94d049bb133111ebULL;
	return x ^ ( x >> 31 );
}

std::string FormatWavelengthTag( const double nm )
{
	char buf[64];
	const double rounded = std::round( nm );
	if( std::fabs( nm - rounded ) < 1e-6 ) {
		std::snprintf( buf, sizeof( buf ), "%.0fnm", rounded );
	} else {
		std::snprintf( buf, sizeof( buf ), "%.2fnm", nm );
	}
	return std::string( buf );
}

bool WriteTextFileAtomic( const std::string& path, const std::string& content, std::string& error )
{
	try {
		const std::filesystem::path target( path );
		if( target.has_parent_path() && !target.parent_path().empty() ) {
			std::filesystem::create_directories( target.parent_path() );
		}
		const std::filesystem::path tmp = target.string() + ".tmp";
		{
			std::ofstream ofs( tmp, std::ios::binary | std::ios::trunc );
			if( !ofs.good() ) {
				error = "failed to open temporary manifest file: " + tmp.string();
				return false;
			}
			ofs.write( content.data(), static_cast<std::streamsize>( content.size() ) );
			ofs.flush();
			if( !ofs.good() ) {
				error = "failed to write temporary manifest file: " + tmp.string();
				return false;
			}
		}
		std::error_code ec;
		std::filesystem::rename( tmp, target, ec );
		if( ec ) {
			std::filesystem::remove( tmp, ec );
			error = "failed to atomically rename manifest file to " + path;
			return false;
		}
		return true;
	} catch( const std::exception& ex ) {
		error = ex.what();
		return false;
	}
}

bool IsAllowedPixelFilter( const std::string& filterName )
{
	const std::string lower = ToLowerASCII( filterName );
	return lower.empty() || lower == "box" || lower == "triangle" || lower == "gaussian";
}

bool BuildProvenanceCBOR(
	const double nm,
	const std::uint32_t width,
	const std::uint32_t height,
	const std::uint64_t spp,
	const std::uint64_t seed,
	const std::vector<double>& sweepWls,
	const bool allowRGBUplift,
	RISECBOR64::Bytes& outBytes,
	std::string& outError
	)
{
	using V = RISECBOR64::Value;
	const V doc = V::MapValue( {
		{ "allow_rgb_uplift", V::Bool( allowRGBUplift ) },
		{ "height",           V::Unsigned( height ) },
		{ "rasterizer",       V::String( "hyperspectral_radiometric_v1" ) },
		{ "samples_per_pixel",V::Unsigned( spp ) },
		{ "seed",             V::Unsigned( seed ) },
		{ "sweep_first_nm",   V::Float( sweepWls.front() ) },
		{ "sweep_last_nm",    V::Float( sweepWls.back() ) },
		{ "sweep_nodes",      V::Unsigned( sweepWls.size() ) },
		{ "wavelength_nm",    V::Float( nm ) },
		{ "width",            V::Unsigned( width ) }
	} );
	return RISECBOR64::Encode( doc, outBytes, &outError );
}

} // namespace

HyperspectralRadiometricRasterizer::HyperspectralRadiometricRasterizer(
	IRayCaster* pCaster_,
	const HyperspectralRadiometricConfig& config_
	) :
  pCaster( pCaster_ ),
  pIntegrator( 0 ),
  config( config_ ),
  lastCompleted( false )
{
	if( pCaster ) {
		pCaster->addref();
	}
	pIntegrator = new PathTracingIntegrator( ManifoldSolverConfig(), config.stabilityConfig );
	GlobalLog()->PrintNew( pIntegrator, __FILE__, __LINE__, "HyperspectralRadiometricRasterizer::pIntegrator" );
}

HyperspectralRadiometricRasterizer::~HyperspectralRadiometricRasterizer()
{
	safe_release( pIntegrator );
	safe_release( pCaster );
}

bool HyperspectralRadiometricRasterizer::ResolveWavelengths(
	std::vector<double>& outWavelengths,
	std::string& outError
	) const
{
	outWavelengths.clear();
	if( !config.wavelengthsNm.empty() ) {
		double prev = 0.0;
		for( double nm : config.wavelengthsNm ) {
			if( !std::isfinite( nm ) || nm <= prev ) {
				outError = "hyperspectral_radiometric_rasterizer: wavelengths must be finite, positive, and strictly increasing";
				return false;
			}
			prev = nm;
			outWavelengths.push_back( nm );
		}
		return true;
	}

	if( !std::isfinite( config.lambdaBegin ) || !std::isfinite( config.lambdaEnd ) ||
		!std::isfinite( config.lambdaStep ) || config.lambdaBegin <= 0.0 ||
		config.lambdaEnd < config.lambdaBegin || config.lambdaStep <= 0.0 )
	{
		outError = "hyperspectral_radiometric_rasterizer: invalid wavelength sweep bounds or step";
		return false;
	}

	const double span = config.lambdaEnd - config.lambdaBegin;
	const std::size_t maxSteps = 4096;
	const std::size_t nSteps = static_cast<std::size_t>( std::floor( span / config.lambdaStep + 1e-9 ) ) + 1;
	if( nSteps == 0 || nSteps > maxSteps ) {
		outError = "hyperspectral_radiometric_rasterizer: wavelength sweep node count out of range (1..4096)";
		return false;
	}
	outWavelengths.reserve( nSteps );
	for( std::size_t i = 0; i < nSteps; ++i ) {
		const double nm = config.lambdaBegin + static_cast<double>( i ) * config.lambdaStep;
		if( nm <= config.lambdaEnd + 1e-9 ) {
			outWavelengths.push_back( nm );
		}
	}
	return !outWavelengths.empty();
}

bool HyperspectralRadiometricRasterizer::ValidateSceneAndConfig(
	const IScene& pScene,
	std::vector<double>& outWavelengths,
	std::string& outError
	) const
{
	if( config.structVersion != HyperspectralRadiometricConfig::kCurrentVersion ||
		config.structSize != sizeof( HyperspectralRadiometricConfig ) )
	{
		outError = "hyperspectral_radiometric_rasterizer: unsupported config struct version/size";
		return false;
	}

	if( !pCaster ) {
		outError = "hyperspectral_radiometric_rasterizer: null ray caster";
		return false;
	}

	if( !ResolveWavelengths( outWavelengths, outError ) ) {
		return false;
	}

	if( config.samplesPerPixel == 0 ) {
		outError = "hyperspectral_radiometric_rasterizer: samples_per_pixel must be >= 1";
		return false;
	}

	if( config.outputPrefix.empty() ) {
		outError = "hyperspectral_radiometric_rasterizer: output_prefix is required";
		return false;
	}

	// Mechanical preflight refusal of bias controls.
	if( config.stabilityConfig.directClamp > Scalar( 0 ) ||
		config.stabilityConfig.indirectClamp > Scalar( 0 ) )
	{
		outError = "hyperspectral_radiometric_rasterizer: clamping (direct_clamp / indirect_clamp > 0) is refused in raw radiometric mode";
		return false;
	}
	if( config.stabilityConfig.filterGlossy > Scalar( 0 ) ) {
		outError = "hyperspectral_radiometric_rasterizer: filter_glossy > 0 is refused in raw radiometric mode";
		return false;
	}
	if( config.oidnDenoise ) {
		outError = "hyperspectral_radiometric_rasterizer: OIDN denoising is refused in raw radiometric mode";
		return false;
	}
	if( config.toneMap ) {
		outError = "hyperspectral_radiometric_rasterizer: tone mapping is refused in raw radiometric mode";
		return false;
	}
	if( !IsAllowedPixelFilter( config.pixelFilterName ) ) {
		outError = "hyperspectral_radiometric_rasterizer: negative-lobe or unsupported pixel filter '" +
			config.pixelFilterName + "' is refused in raw radiometric mode (allowed: box, triangle, gaussian)";
		return false;
	}

	const ICamera* pCamera = pScene.GetCamera();
	const IFilm* pFilm = pScene.GetFilm();
	if( !pCamera || !pFilm || pFilm->GetWidth() == 0 || pFilm->GetHeight() == 0 ) {
		outError = "hyperspectral_radiometric_rasterizer: scene requires a valid camera and non-empty film";
		return false;
	}

	// HyLIoS wavelength band coverage preflight.
	if( const ObjectManager* pObjMgr = dynamic_cast<const ObjectManager*>( pScene.GetObjects() ) ) {
		std::string offender;
		if( !pObjMgr->SceneCoversHyLIoSWavelengths( outWavelengths.front(), outWavelengths.back(), &offender ) ) {
			char buf[256];
			std::snprintf( buf, sizeof( buf ),
				"hyperspectral_radiometric_rasterizer: HyLIoS material on object '%s' does not cover sweep band [%.2f, %.2f] nm",
				offender.c_str(), outWavelengths.front(), outWavelengths.back() );
			outError = buf;
			return false;
		}
		if( pObjMgr->SceneHasSmoothNonlocalHyLIoS( &offender ) &&
			pScene.GetLights() && !pScene.GetLights()->getLights().empty() )
		{
			outError = "hyperspectral_radiometric_rasterizer: smooth (boundary_roughness == 0) nonlocal HyLIoS material on object '" +
				offender + "' cannot perform boundary-event NEE against delta lights";
			return false;
		}
	}

	// Physical spectral source preflight (unless explicitly overridden).
	if( !config.allowRGBUpliftSources ) {
		if( const ILightManager* pLightMgr = pScene.GetLights() ) {
			const ILightManager::LightsList& lights = pLightMgr->getLights();
			for( const ILightPriv* pLight : lights ) {
				if( !pLight ) continue;
				bool hasPhysical = false;
				if( const PointLight* pl = dynamic_cast<const PointLight*>( pLight ) ) {
					hasPhysical = ( pl->GetPhysicalSpectrum() != nullptr );
				} else if( const SpotLight* sl = dynamic_cast<const SpotLight*>( pLight ) ) {
					hasPhysical = ( sl->GetPhysicalSpectrum() != nullptr );
				} else if( const DirectionalLight* dl = dynamic_cast<const DirectionalLight*>( pLight ) ) {
					hasPhysical = ( dl->GetPhysicalSpectrum() != nullptr );
				}
				if( !hasPhysical ) {
					outError = "hyperspectral_radiometric_rasterizer: non-physical / RGB-uplifted delta light refused "
						"(use physical_point_omni_light, physical_point_spot_light, physical_directional_light, or set allow_rgb_uplift_sources TRUE)";
					return false;
				}
			}
		}
	}

	return true;
}

void HyperspectralRadiometricRasterizer::AttachToScene( const IScene* pScene )
{
	if( pCaster ) {
		pCaster->AttachScene( pScene );
	}
}

void HyperspectralRadiometricRasterizer::DetachFromScene( const IScene* )
{
	if( pCaster ) {
		pCaster->AttachScene( 0 );
	}
}

unsigned int HyperspectralRadiometricRasterizer::PredictTimeToRasterizeScene(
	const IScene&,
	const ISampling2D&,
	unsigned int* pActualTime
	) const
{
	if( pActualTime ) {
		*pActualTime = 0;
	}
	return 0;
}

void HyperspectralRadiometricRasterizer::RasterizeScene(
	const IScene& pScene,
	const Rect*,
	IRasterizeSequence*
	) const
{
	lastCompleted = false;
	lastError.clear();
	lastSnapshots.clear();
	lastManifestPath.clear();

	std::vector<double> wavelengths;
	if( !ValidateSceneAndConfig( pScene, wavelengths, lastError ) ) {
		GlobalLog()->PrintEx( eLog_Error, "%s", lastError.c_str() );
		return;
	}

	// Build immutable SpectralSourceDomain from the sweep wavelengths and
	// attach it to RayCaster BEFORE AttachScene so LightSampler / EnvironmentSampler
	// build their proposal tables over the exact sweep domain.
	try {
		std::vector<SpectralSourceDomain::Node> nodes;
		nodes.reserve( wavelengths.size() );
		for( double nm : wavelengths ) {
			nodes.push_back( SpectralSourceDomain::Node{ nm, 1.0 } );
		}
		std::shared_ptr<const SpectralSourceDomain> domain = SpectralSourceDomain::Create( nodes );
		if( RayCaster* pConcreteCaster = dynamic_cast<RayCaster*>( pCaster ) ) {
			pConcreteCaster->SetSpectralSourceDomain( domain );
		}
	} catch( const std::exception& ex ) {
		lastError = std::string( "hyperspectral_radiometric_rasterizer: failed to create SpectralSourceDomain: " ) + ex.what();
		GlobalLog()->PrintEx( eLog_Error, "%s", lastError.c_str() );
		return;
	}

	pScene.GetObjects()->PrepareForRendering();
	pCaster->AttachScene( &pScene );

	const ICamera* pCamera = pScene.GetCamera();
	const IFilm* pFilm = pScene.GetFilm();
	const std::uint32_t width  = static_cast<std::uint32_t>( pFilm->GetWidth() );
	const std::uint32_t height = static_cast<std::uint32_t>( pFilm->GetHeight() );
	const double pixelAR       = static_cast<double>( pFilm->GetPixelAR() );
	const IRadianceMap* pEnv   = pScene.GetGlobalRadianceMap();

	static const char* kSeedAlgorithm = "sobol_owen_pixel_index_v1";

	struct ManifestEntry
	{
		double nm;
		std::string exrPath;
		std::string status;
		std::uint64_t targetSamples;
	};
	std::vector<ManifestEntry> manifestEntries;
	manifestEntries.reserve( wavelengths.size() );

	bool cancelled = false;

	for( std::size_t wlIdx = 0; wlIdx < wavelengths.size(); ++wlIdx ) {
		const double nm = wavelengths[wlIdx];
		const std::string wlTag = FormatWavelengthTag( nm );
		const std::string exrPath = config.outputPrefix + "_" + wlTag + ".exr";
		const std::string ckptPath = config.checkpointPrefix.empty()
			? std::string()
			: ( config.checkpointPrefix + "_" + wlTag + ".rcp" );

		RISECBOR64::Bytes provBytes;
		if( !BuildProvenanceCBOR( nm, width, height, config.samplesPerPixel, config.seed,
				wavelengths, config.allowRGBUpliftSources, provBytes, lastError ) )
		{
			GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: provenance encode failed: %s", lastError.c_str() );
			return;
		}

		RadiometricIdentity ident;
		ident.width            = width;
		ident.height           = height;
		ident.wavelengthNM     = nm;
		ident.pixelAspectRatio = pixelAR;
		ident.targetSamples    = config.samplesPerPixel;
		ident.seed             = config.seed;
		ident.seedAlgorithm    = kSeedAlgorithm;
		ident.provenance       = std::move( provBytes );

		std::unique_ptr<RadiometricFilm> film;
		if( !ckptPath.empty() && std::filesystem::exists( ckptPath ) ) {
			RadiometricSnapshot loaded;
			if( !LoadRadiometricCheckpoint( ckptPath, ident, loaded, lastError, config.limits ) ) {
				GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: checkpoint load failed (%s): %s",
					ckptPath.c_str(), lastError.c_str() );
				return;
			}
			film = RadiometricFilm::Restore( loaded, lastError, config.limits );
			if( !film ) {
				GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: film restore failed: %s", lastError.c_str() );
				return;
			}
			if( loaded.status == RadiometricStatus::Cancelled ) {
				if( !film->Resume( ident, lastError ) ) {
					GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: film resume failed: %s", lastError.c_str() );
					return;
				}
			}
		} else {
			film = RadiometricFilm::Create( ident, lastError, config.limits );
			if( !film ) {
				GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: film create failed: %s", lastError.c_str() );
				return;
			}
		}

		// Snapshot initial per-pixel nextSampleIndex for resumption.
		const RadiometricSnapshot startSnap = film->Snapshot();
		std::atomic<bool> failed( false );
		std::atomic<bool> cancelFlag( false );
		std::mutex errMutex;
		std::string workerError;

		const std::uint64_t totalPixels = static_cast<std::uint64_t>( width ) * static_cast<std::uint64_t>( height );

		auto renderPixel = [&]( const std::uint64_t pIdx ) {
			if( failed.load( std::memory_order_relaxed ) || cancelFlag.load( std::memory_order_relaxed ) ) {
				return;
			}
			const std::uint32_t y = static_cast<std::uint32_t>( pIdx / width );
			const std::uint32_t x = static_cast<std::uint32_t>( pIdx % width );
			const std::uint64_t startSample = startSnap.pixels[pIdx].nextSampleIndex;
			if( startSample >= config.samplesPerPixel ) {
				return;
			}

			const std::uint64_t key = SplitMix64(
				config.seed ^
				( static_cast<std::uint64_t>( wlIdx + 1 ) * 0x9e3779b97f4a7c15ULL ) ^
				( static_cast<std::uint64_t>( y ) << 20 ) ^
				static_cast<std::uint64_t>( x ) );
			const std::uint32_t pixelSeed = static_cast<std::uint32_t>( key ^ ( key >> 32 ) );

			for( std::uint64_t s = startSample; s < config.samplesPerPixel; ++s ) {
				if( failed.load( std::memory_order_relaxed ) || cancelFlag.load( std::memory_order_relaxed ) ) {
					return;
				}

				SobolSampler sampler( static_cast<unsigned int>( s ), pixelSeed );
				RandomNumberGenerator rng( pixelSeed ^ static_cast<unsigned int>( s * 2654435761u ) );
				RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

				// Box-filter pixel sample: uniform jitter in [x, x+1) x [y, y+1).
				const Point2 jitter = sampler.Get2D();
				const Point2 ptScreen(
					static_cast<Scalar>( x ) + jitter.x,
					static_cast<Scalar>( height - 1 - y ) + jitter.y );

				Ray ray;
				double sampleL = 0.0;
				if( pCamera->GenerateRay( rc, ray, ptScreen ) ) {
					RasterizerState rast = { x, y };
					const Scalar L = pIntegrator->IntegrateRayNM(
						rc, rast, ray, static_cast<Scalar>( nm ), pScene, *pCaster, sampler, pEnv );
					sampleL = static_cast<double>( L );
				}

				std::string commitErr;
				if( !film->Commit( x, y, s, sampleL, commitErr ) ) {
					failed.store( true, std::memory_order_relaxed );
					std::lock_guard<std::mutex> lk( errMutex );
					if( workerError.empty() ) {
						workerError = commitErr;
					}
					return;
				}
			}
		};

		// Process row-by-row so progress / cancel can be checked cleanly between rows.
		for( std::uint32_t y = 0; y < height; ++y ) {
			if( pProgressFunc && !pProgressFunc->Progress(
					static_cast<double>( wlIdx * height + y ),
					static_cast<double>( wavelengths.size() * height ) ) )
			{
				cancelFlag.store( true, std::memory_order_relaxed );
				cancelled = true;
				break;
			}
			const std::uint64_t rowBase = static_cast<std::uint64_t>( y ) * width;
			GlobalThreadPool().ParallelFor( static_cast<unsigned int>( width ), [&]( const unsigned int x ) {
				renderPixel( rowBase + x );
			} );
			if( failed.load( std::memory_order_relaxed ) ) {
				break;
			}
		}

		if( failed.load( std::memory_order_relaxed ) ) {
			lastError = workerError.empty() ? "radiometric pixel commit failed" : workerError;
			GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: %s", lastError.c_str() );
			return;
		}

		if( cancelled ) {
			film->Cancel();
			const RadiometricSnapshot snap = film->Snapshot();
			if( !ckptPath.empty() ) {
				std::string ckptErr;
				if( !SaveRadiometricCheckpoint( ckptPath, snap, ckptErr, config.limits ) ) {
					GlobalLog()->PrintEx( eLog_Warning, "HyperspectralRadiometricRasterizer:: failed to save cancel checkpoint: %s", ckptErr.c_str() );
				}
			}
			lastSnapshots.push_back( snap );
			return;
		}

		const RadiometricSnapshot snap = film->Snapshot();
		if( snap.status != RadiometricStatus::Complete ) {
			lastError = "hyperspectral_radiometric_rasterizer: wavelength sweep did not reach Complete state";
			GlobalLog()->PrintEx( eLog_Error, "%s", lastError.c_str() );
			return;
		}

		if( !ckptPath.empty() ) {
			if( !SaveRadiometricCheckpoint( ckptPath, snap, lastError, config.limits ) ) {
				GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: checkpoint save failed: %s", lastError.c_str() );
				return;
			}
		}

		if( !WriteRadiometricEXR( exrPath, snap, lastError, config.limits ) ) {
			GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: EXR write failed (%s): %s",
				exrPath.c_str(), lastError.c_str() );
			return;
		}

		manifestEntries.push_back( ManifestEntry{ nm, exrPath, "complete", config.samplesPerPixel } );
		lastSnapshots.push_back( snap );
		(void)totalPixels;
	}

	// Emit atomic JSON sweep manifest.
	{
		using J = RISE::Agent::JsonValue;
		J root = J::MakeObject();
		root.set( "schema_version", J::MakeNumber( 1 ) );
		root.set( "rasterizer", J::MakeString( "hyperspectral_radiometric_v1" ) );
		root.set( "width", J::MakeNumber( static_cast<double>( width ) ) );
		root.set( "height", J::MakeNumber( static_cast<double>( height ) ) );
		root.set( "pixel_aspect_ratio", J::MakeNumber( pixelAR ) );
		root.set( "samples_per_pixel", J::MakeNumber( static_cast<double>( config.samplesPerPixel ) ) );
		root.set( "seed", J::MakeNumber( static_cast<double>( config.seed ) ) );
		root.set( "seed_algorithm", J::MakeString( kSeedAlgorithm ) );
		root.set( "units", J::MakeString( "W m^-2 sr^-1 nm^-1" ) );
		root.set( "allow_rgb_uplift_sources", J::MakeBool( config.allowRGBUpliftSources ) );

		J bands = J::MakeArray();
		for( const ManifestEntry& e : manifestEntries ) {
			J b = J::MakeObject();
			b.set( "wavelength_nm", J::MakeNumber( e.nm ) );
			b.set( "exr_path", J::MakeString( e.exrPath ) );
			b.set( "status", J::MakeString( e.status ) );
			b.set( "samples_per_pixel", J::MakeNumber( static_cast<double>( e.targetSamples ) ) );
			bands.push_back( b );
		}
		root.set( "bands", bands );

		const std::string manifestPath = config.outputPrefix + "_manifest.json";
		const std::string jsonText = RISE::Agent::JsonSerialize( root ) + "\n";
		if( !WriteTextFileAtomic( manifestPath, jsonText, lastError ) ) {
			GlobalLog()->PrintEx( eLog_Error, "HyperspectralRadiometricRasterizer:: manifest write failed: %s", lastError.c_str() );
			return;
		}
		lastManifestPath = manifestPath;
	}

	lastCompleted = true;
}

void HyperspectralRadiometricRasterizer::RasterizeSceneAnimation(
	const IScene&,
	const Scalar,
	const Scalar,
	const unsigned int,
	const bool,
	const bool,
	const Rect*,
	const unsigned int*,
	IRasterizeSequence*
	) const
{
	lastCompleted = false;
	lastError = "hyperspectral_radiometric_rasterizer: animation sweeps are not supported; render individual frames";
	GlobalLog()->PrintEx( eLog_Error, "%s", lastError.c_str() );
}
