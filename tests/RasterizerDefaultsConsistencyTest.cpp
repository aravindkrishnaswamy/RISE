//////////////////////////////////////////////////////////////////////
//
//  RasterizerDefaultsConsistencyTest.cpp - Asserts the descriptor's
//    `defaultValueHint` for each rasterizer parameter agrees with
//    the matching `*Defaults` struct field formatted by `to_hint()`.
//
//    The chunk descriptors drive the GUI panel's "Parser default: …"
//    column.  The parser's `Finalize` `bag.GetX(name, defaults.field)`
//    fallbacks read the same `*Defaults` struct field directly.
//    `Job::InstantiateRasterizerWithDefaults` builds rasterizers from
//    the same struct.  This test catches drift the moment one of the
//    three surfaces moves out of sync — e.g. someone adds a parameter
//    to a parser's `Describe()` but hardcodes the `defaultValueHint`
//    string instead of calling `to_hint(d.field)`.
//
//    Coverage: every parameter whose descriptor matches a `*Defaults`
//    field is asserted.  Parameters not represented by a `*Defaults`
//    field (per-config-struct knobs surfaced through `Add*Params`
//    helpers, or "Legacy — ignored" parameters with empty hints) are
//    skipped — those have their own consistency rooted in the config
//    struct's in-class initializer, which `Add*Params` reads directly.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 5, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"   // Scalar typedef used by configs
#include "../src/Library/Parsers/ChunkDescriptor.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/Utilities/RasterizerDefaults.h"
#include "../src/Library/Utilities/SMSConfig.h"
#include "../src/Library/Utilities/PixelFilterConfig.h"
#include "../src/Library/Utilities/SpectralConfig.h"
#include "../src/Library/Utilities/StabilityConfig.h"
#include "../src/Library/Utilities/AdaptiveSamplingConfig.h"
#include "../src/Library/Utilities/RadianceMapConfig.h"
#include "../src/Library/Utilities/ProgressiveConfig.h"
#include "../src/Library/Utilities/PathGuidingField.h"

using namespace RISE;

static int passCount = 0;
static int failCount = 0;

static const std::string* HintFor( const ChunkDescriptor* desc, const char* paramName )
{
	if( !desc ) return nullptr;
	for( const ParameterDescriptor& p : desc->parameters ) {
		if( p.name == paramName ) return &p.defaultValueHint;
	}
	return nullptr;
}

static void Check( bool cond, const std::string& name )
{
	if( cond ) {
		++passCount;
	} else {
		++failCount;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

static void CheckHint( const ChunkDescriptor* desc, const char* keyword,
	const char* param, const std::string& expected )
{
	const std::string* got = HintFor( desc, param );
	if( !got ) {
		++failCount;
		std::cout << "  FAIL: " << keyword << ":" << param
			<< " — descriptor has no such parameter" << std::endl;
		return;
	}
	if( *got != expected ) {
		++failCount;
		std::cout << "  FAIL: " << keyword << ":" << param
			<< " — descriptor hint \"" << *got << "\" != to_hint(field) \"" << expected << "\""
			<< std::endl;
		return;
	}
	++passCount;
}

//
// to_hint formatting unit tests — the formatter is the bridge between
// every *Defaults field and the descriptor strings, so a regression
// here cascades across every parser.
//
static void TestToHintFormatting()
{
	std::cout << "to_hint() formatting" << std::endl;
	Check( to_hint( true )                          == "TRUE",      "to_hint(true)" );
	Check( to_hint( false )                         == "FALSE",     "to_hint(false)" );
	Check( to_hint( static_cast<unsigned int>(0) )  == "0",         "to_hint(0u)" );
	Check( to_hint( static_cast<unsigned int>(8) )  == "8",         "to_hint(8u)" );
	Check( to_hint( static_cast<unsigned int>(100000) ) == "100000","to_hint(100000u)" );
	Check( to_hint( UINT_MAX )                      == "unlimited", "to_hint(UINT_MAX)" );
	Check( to_hint( 0.0 )                           == "0",         "to_hint(0.0)" );
	Check( to_hint( 1.0 )                           == "1.0",       "to_hint(1.0)" );
	Check( to_hint( 0.5 )                           == "0.5",       "to_hint(0.5)" );
	Check( to_hint( 1e-5 )                          == "1e-05",     "to_hint(1e-5)" );
	Check( to_hint( OidnQuality::Auto )             == "auto",      "to_hint(OidnQuality::Auto)" );
	Check( to_hint( OidnDevice::Auto )              == "auto",      "to_hint(OidnDevice::Auto)" );
	Check( to_hint( OidnPrefilter::Fast )           == "fast",      "to_hint(OidnPrefilter::Fast)" );
	Check( to_hint( std::string("global") )         == "global",    "to_hint(\"global\")" );
}

//
// Per-rasterizer descriptor hints.  For each rasterizer chunk we
// confirm every parameter whose canonical default lives in a
// *Defaults struct (the per-rasterizer scalars + base shared block).
// Per-config-struct params surfaced through Add*Params helpers (SMS /
// pixel filter / pathguiding / etc.) are validated by the per-config
// section below — the helper reads the config struct's in-class
// initializer directly so there's only one drift surface to check.
//
template<class D>
static void CheckBase( const ChunkDescriptor* desc, const char* keyword, const D& d )
{
	CheckHint( desc, keyword, "defaultshader",   to_hint( d.defaultShader ) );
	CheckHint( desc, keyword, "samples",         to_hint( d.numPixelSamples ) );
	CheckHint( desc, keyword, "show_luminaires", to_hint( d.showLuminaires ) );
	CheckHint( desc, keyword, "oidn_denoise",    to_hint( d.oidnDenoise ) );
	CheckHint( desc, keyword, "oidn_quality",    to_hint( d.oidnQuality ) );
	CheckHint( desc, keyword, "oidn_device",     to_hint( d.oidnDevice ) );
	CheckHint( desc, keyword, "oidn_prefilter",  to_hint( d.oidnPrefilter ) );
}

static void TestPixelPel()
{
	std::cout << "pixelpel_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "pixelpel_rasterizer" ) );
	Check( desc != nullptr, "pixelpel_rasterizer descriptor present" );
	if( !desc ) return;
	PixelPelDefaults d;
	CheckBase( desc, "pixelpel_rasterizer", d );
	CheckHint( desc, "pixelpel_rasterizer", "max_recursion",          to_hint( d.maxRecursion ) );
	CheckHint( desc, "pixelpel_rasterizer", "lum_samples",            to_hint( d.numLumSamples ) );
	CheckHint( desc, "pixelpel_rasterizer", "luminary_sampler",       to_hint( d.luminarySampler ) );
	CheckHint( desc, "pixelpel_rasterizer", "luminary_sampler_param", to_hint( d.luminarySamplerParam ) );
}

static void TestPixelIntegratingSpectral()
{
	std::cout << "pixelintegratingspectral_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "pixelintegratingspectral_rasterizer" ) );
	Check( desc != nullptr, "pixelintegratingspectral_rasterizer descriptor present" );
	if( !desc ) return;
	PixelIntegratingSpectralDefaults d;
	CheckBase( desc, "pixelintegratingspectral_rasterizer", d );
	CheckHint( desc, "pixelintegratingspectral_rasterizer", "max_recursion",          to_hint( d.maxRecursion ) );
	CheckHint( desc, "pixelintegratingspectral_rasterizer", "lum_samples",            to_hint( d.numLumSamples ) );
	CheckHint( desc, "pixelintegratingspectral_rasterizer", "luminary_sampler",       to_hint( d.luminarySampler ) );
	CheckHint( desc, "pixelintegratingspectral_rasterizer", "luminary_sampler_param", to_hint( d.luminarySamplerParam ) );
}

static void TestPathTracingPel()
{
	std::cout << "pathtracing_pel_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "pathtracing_pel_rasterizer" ) );
	Check( desc != nullptr, "pathtracing_pel_rasterizer descriptor present" );
	if( !desc ) return;
	PathTracingPelDefaults d;
	CheckBase( desc, "pathtracing_pel_rasterizer", d );
}

static void TestPathTracingSpectral()
{
	std::cout << "pathtracing_spectral_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "pathtracing_spectral_rasterizer" ) );
	Check( desc != nullptr, "pathtracing_spectral_rasterizer descriptor present" );
	if( !desc ) return;
	PathTracingSpectralDefaults d;
	CheckBase( desc, "pathtracing_spectral_rasterizer", d );
}

static void TestBDPTPel()
{
	std::cout << "bdpt_pel_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_pel_rasterizer" ) );
	Check( desc != nullptr, "bdpt_pel_rasterizer descriptor present" );
	if( !desc ) return;
	BDPTPelDefaults d;
	CheckBase( desc, "bdpt_pel_rasterizer", d );
	CheckHint( desc, "bdpt_pel_rasterizer", "max_eye_depth",   to_hint( d.maxEyeDepth ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "max_light_depth", to_hint( d.maxLightDepth ) );
}

static void TestBDPTSpectral()
{
	std::cout << "bdpt_spectral_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_spectral_rasterizer" ) );
	Check( desc != nullptr, "bdpt_spectral_rasterizer descriptor present" );
	if( !desc ) return;
	BDPTSpectralDefaults d;
	CheckBase( desc, "bdpt_spectral_rasterizer", d );
	CheckHint( desc, "bdpt_spectral_rasterizer", "max_eye_depth",   to_hint( d.maxEyeDepth ) );
	CheckHint( desc, "bdpt_spectral_rasterizer", "max_light_depth", to_hint( d.maxLightDepth ) );
}

static void TestVCMPel()
{
	std::cout << "vcm_pel_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "vcm_pel_rasterizer" ) );
	Check( desc != nullptr, "vcm_pel_rasterizer descriptor present" );
	if( !desc ) return;
	VCMPelDefaults d;
	CheckBase( desc, "vcm_pel_rasterizer", d );
	CheckHint( desc, "vcm_pel_rasterizer", "max_eye_depth",   to_hint( d.maxEyeDepth ) );
	CheckHint( desc, "vcm_pel_rasterizer", "max_light_depth", to_hint( d.maxLightDepth ) );
	CheckHint( desc, "vcm_pel_rasterizer", "merge_radius",    to_hint( d.mergeRadius ) );
	CheckHint( desc, "vcm_pel_rasterizer", "vc_enabled",      to_hint( d.enableVC ) );
	CheckHint( desc, "vcm_pel_rasterizer", "vm_enabled",      to_hint( d.enableVM ) );
}

static void TestVCMSpectral()
{
	std::cout << "vcm_spectral_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "vcm_spectral_rasterizer" ) );
	Check( desc != nullptr, "vcm_spectral_rasterizer descriptor present" );
	if( !desc ) return;
	VCMSpectralDefaults d;
	CheckBase( desc, "vcm_spectral_rasterizer", d );
	CheckHint( desc, "vcm_spectral_rasterizer", "max_eye_depth",   to_hint( d.maxEyeDepth ) );
	CheckHint( desc, "vcm_spectral_rasterizer", "max_light_depth", to_hint( d.maxLightDepth ) );
	CheckHint( desc, "vcm_spectral_rasterizer", "merge_radius",    to_hint( d.mergeRadius ) );
	CheckHint( desc, "vcm_spectral_rasterizer", "vc_enabled",      to_hint( d.enableVC ) );
	CheckHint( desc, "vcm_spectral_rasterizer", "vm_enabled",      to_hint( d.enableVM ) );
}

static void TestMLT()
{
	std::cout << "mlt_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "mlt_rasterizer" ) );
	Check( desc != nullptr, "mlt_rasterizer descriptor present" );
	if( !desc ) return;
	MLTDefaults d;
	// MLT inlines its own base-rasterizer params (no AddBaseRasterizerParams),
	// but the values are taken from the same MLTDefaults struct.
	CheckHint( desc, "mlt_rasterizer", "defaultshader",       to_hint( d.defaultShader ) );
	CheckHint( desc, "mlt_rasterizer", "show_luminaires",     to_hint( d.showLuminaires ) );
	CheckHint( desc, "mlt_rasterizer", "oidn_denoise",        to_hint( d.oidnDenoise ) );
	CheckHint( desc, "mlt_rasterizer", "oidn_quality",        to_hint( d.oidnQuality ) );
	CheckHint( desc, "mlt_rasterizer", "oidn_device",         to_hint( d.oidnDevice ) );
	CheckHint( desc, "mlt_rasterizer", "oidn_prefilter",      to_hint( d.oidnPrefilter ) );
	CheckHint( desc, "mlt_rasterizer", "max_eye_depth",       to_hint( d.maxEyeDepth ) );
	CheckHint( desc, "mlt_rasterizer", "max_light_depth",     to_hint( d.maxLightDepth ) );
	CheckHint( desc, "mlt_rasterizer", "bootstrap_samples",   to_hint( d.nBootstrap ) );
	CheckHint( desc, "mlt_rasterizer", "chains",              to_hint( d.nChains ) );
	CheckHint( desc, "mlt_rasterizer", "mutations_per_pixel", to_hint( d.nMutationsPerPixel ) );
	CheckHint( desc, "mlt_rasterizer", "large_step_prob",     to_hint( d.largeStepProb ) );
}

static void TestMLTSpectral()
{
	std::cout << "mlt_spectral_rasterizer" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "mlt_spectral_rasterizer" ) );
	Check( desc != nullptr, "mlt_spectral_rasterizer descriptor present" );
	if( !desc ) return;
	MLTSpectralDefaults d;
	CheckHint( desc, "mlt_spectral_rasterizer", "defaultshader",       to_hint( d.defaultShader ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "show_luminaires",     to_hint( d.showLuminaires ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "oidn_denoise",        to_hint( d.oidnDenoise ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "oidn_quality",        to_hint( d.oidnQuality ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "oidn_device",         to_hint( d.oidnDevice ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "oidn_prefilter",      to_hint( d.oidnPrefilter ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "max_eye_depth",       to_hint( d.maxEyeDepth ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "max_light_depth",     to_hint( d.maxLightDepth ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "bootstrap_samples",   to_hint( d.nBootstrap ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "chains",              to_hint( d.nChains ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "mutations_per_pixel", to_hint( d.nMutationsPerPixel ) );
	CheckHint( desc, "mlt_spectral_rasterizer", "large_step_prob",     to_hint( d.largeStepProb ) );
}

//
// Per-config-struct hints.  The Add*Params helpers in the parser
// default-construct each config struct internally and pass each
// field through to_hint().  We exercise every field on at least one
// rasterizer chunk that uses the helper, so a hand-edited
// defaultValueHint that diverges from the struct gets flagged here.
//
static void TestStabilityConfigHints()
{
	std::cout << "StabilityConfig hints (via bdpt_pel_rasterizer)" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_pel_rasterizer" ) );
	if( !desc ) { ++failCount; return; }
	StabilityConfig d;
	CheckHint( desc, "bdpt_pel_rasterizer", "direct_clamp",            to_hint( d.directClamp ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "indirect_clamp",          to_hint( d.indirectClamp ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "rr_min_depth",            to_hint( d.rrMinDepth ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "rr_threshold",            to_hint( d.rrThreshold ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "max_diffuse_bounce",      to_hint( d.maxDiffuseBounce ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "max_glossy_bounce",       to_hint( d.maxGlossyBounce ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "max_transmission_bounce", to_hint( d.maxTransmissionBounce ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "max_translucent_bounce",  to_hint( d.maxTranslucentBounce ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "max_volume_bounce",       to_hint( d.maxVolumeBounce ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "light_bvh",               to_hint( d.useLightBVH ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "optimal_mis",             to_hint( d.optimalMIS ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "optimal_mis_training_iterations", to_hint( d.optimalMISTrainingIterations ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "optimal_mis_tile_size",   to_hint( d.optimalMISTileSize ) );
}

static void TestPixelFilterHints()
{
	std::cout << "PixelFilterConfig hints (via bdpt_pel_rasterizer)" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_pel_rasterizer" ) );
	if( !desc ) { ++failCount; return; }
	PixelFilterConfig d;
	CheckHint( desc, "bdpt_pel_rasterizer", "pixel_sampler",       to_hint( std::string( d.pixelSampler.c_str() ) ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "pixel_sampler_param", to_hint( d.pixelSamplerParam ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "pixel_filter",        to_hint( std::string( d.filter.c_str() ) ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "pixel_filter_width",  to_hint( d.width ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "pixel_filter_height", to_hint( d.height ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "pixel_filter_paramA", to_hint( d.paramA ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "pixel_filter_paramB", to_hint( d.paramB ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "blue_noise_sampler",  to_hint( d.blueNoiseSampler ) );
}

static void TestSMSConfigHints()
{
	// BDPT no longer participates in SMS (excised 2026-05-07).  Validate
	// SMS hints via the PT-pel rasterizer instead, which still wires SMS.
	std::cout << "SMSConfig hints (via pathtracing_pel_rasterizer)" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "pathtracing_pel_rasterizer" ) );
	if( !desc ) { ++failCount; return; }
	SMSConfig d;
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_enabled",          to_hint( d.enabled ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_max_iterations",   to_hint( d.maxIterations ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_threshold",        to_hint( d.threshold ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_max_chain_depth",  to_hint( d.maxChainDepth ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_biased",           to_hint( d.biased ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_bernoulli_trials", to_hint( d.bernoulliTrials ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_multi_trials",     to_hint( d.multiTrials ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_photon_count",     to_hint( d.photonCount ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_two_stage",        to_hint( d.twoStage ) );
	CheckHint( desc, "pathtracing_pel_rasterizer", "sms_target_bounces",   to_hint( d.targetBounces ) );
}

static void TestProgressiveHints()
{
	std::cout << "ProgressiveConfig hints (via bdpt_pel_rasterizer)" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_pel_rasterizer" ) );
	if( !desc ) { ++failCount; return; }
	ProgressiveConfig d;
	CheckHint( desc, "bdpt_pel_rasterizer", "progressive_rendering",       to_hint( d.enabled ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "progressive_samples_per_pass", to_hint( d.samplesPerPass ) );
}

static void TestAdaptiveHints()
{
	std::cout << "AdaptiveSamplingConfig hints (via bdpt_pel_rasterizer)" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_pel_rasterizer" ) );
	if( !desc ) { ++failCount; return; }
	AdaptiveSamplingConfig d;
	CheckHint( desc, "bdpt_pel_rasterizer", "adaptive_max_samples", to_hint( d.maxSamples ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "adaptive_threshold",   to_hint( d.threshold ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "show_adaptive_map",    to_hint( d.showMap ) );
}

static void TestRadianceMapHints()
{
	std::cout << "RadianceMapConfig hints (via bdpt_pel_rasterizer)" << std::endl;
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_pel_rasterizer" ) );
	if( !desc ) { ++failCount; return; }
	RadianceMapConfig d;
	CheckHint( desc, "bdpt_pel_rasterizer", "radiance_scale",      to_hint( d.scale ) );
	CheckHint( desc, "bdpt_pel_rasterizer", "radiance_background", to_hint( d.isBackground ) );
}

int main()
{
	std::cout << "RasterizerDefaultsConsistencyTest" << std::endl;
	std::cout << "=================================" << std::endl;

	TestToHintFormatting();
	TestPixelPel();
	TestPixelIntegratingSpectral();
	TestPathTracingPel();
	TestPathTracingSpectral();
	TestBDPTPel();
	TestBDPTSpectral();
	TestVCMPel();
	TestVCMSpectral();
	TestMLT();
	TestMLTSpectral();
	TestStabilityConfigHints();
	TestPixelFilterHints();
	TestSMSConfigHints();
	TestProgressiveHints();
	TestAdaptiveHints();
	TestRadianceMapHints();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
