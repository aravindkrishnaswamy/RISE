//////////////////////////////////////////////////////////////////////
//
//  CausticReachHarnessTest.cpp - Empirical evaluation of candidate
//    transport-reach signals for DL-167.
//
//    Evaluates:
//      - current mean ratio: vcm.mean / pt.mean
//      - current robust mean ratio: vcm.robustMean (p99) / pt.mean
//      - current median ratio: vcm.median / pt.median
//      - per-pixel ratio p95 (regularized)
//      - per-pixel ratio p90 (regularized)
//      - pixel fraction with VCM > 2 * PT (and VCM > 0.01 * mean)
//      - energy fraction in pixels with VCM > 2 * PT
//      - pixel fraction with VCM > 1.5 * PT
//
//    Across all 18 UNIFIED_INTEGRATOR_BASELINES scenes + crystal_garden.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <iomanip>

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Rendering/AutoRasterizer.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

struct SceneEntry
{
	const char* name;
	const char* path;
	const char* truthWinner; // "VCM", "PT", or "BDPT"
};

static const SceneEntry kScenes[] = {
	{ "diamond_teapot",   "scenes/FeatureBased/Combined/diamond_teapot_pour.RISEscene", "VCM" },
	{ "pool_caustics",     "scenes/FeatureBased/Caustics/pool_caustics.RISEscene",       "VCM" },
	{ "torus_chain",       "scenes/FeatureBased/BDPT/bdpt_torus_chain_atrium.RISEscene", "VCM" },
	{ "spectral_caustic",  "scenes/Tests/Spectral/spectral_dispersive_caustic.RISEscene","VCM" },
	{ "glass_pavilion",    "scenes/FeatureBased/Combined/glass_pavilion.RISEscene",      "PT" },
	{ "jewel_vault",       "scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene",    "PT" },
	{ "crystal_garden",    "scenes/FeatureBased/BDPT/bdpt_crystal_garden.RISEscene",     "PT" },
	{ "cloister",          "scenes/FeatureBased/BDPT/bdpt_cloister.RISEscene",           "PT" },
	{ "ggx_showcase",      "scenes/FeatureBased/Materials/ggx_showcase.RISEscene",       "PT" },
	{ "gi_spheres",        "scenes/FeatureBased/Combined/gi_spheres.RISEscene",          "BDPT" },
	{ "alchemists",        "scenes/FeatureBased/PathTracing/pt_alchemists_sanctum.RISEscene", "BDPT" },
	{ "sculptors_studio",  "scenes/FeatureBased/Combined/sculptors_studio.RISEscene",    "PT" },
	{ "showroom",          "scenes/FeatureBased/Combined/showroom.RISEscene",            "PT" },
	{ "homogeneous_fog",   "scenes/Tests/Volumes/pt_homogeneous_fog.RISEscene",          "BDPT" },
	{ "env_fog",           "scenes/Tests/Volumes/pt_env_through_fog.RISEscene",          "PT" },
	{ "prism_dispersion",  "scenes/Tests/Spectral/hwss_prism_dispersion_pt.RISEscene",   "PT" },
	{ "env_only",          "scenes/Tests/UnifiedLighting/envmap_nee_test_pt.RISEscene",   "BDPT" },
	{ "env_mesh",          "scenes/Tests/Lights/env_plus_mesh_emitter_pt.RISEscene",     "PT" },
	{ "corridor_100lights","scenes/Tests/LightBVH/corridor_100lights_bvh.RISEscene",     "PT" },
};

static std::string ReadFileToString( const char* path )
{
	std::ifstream in( path, std::ios::in | std::ios::binary );
	if( !in ) return std::string();
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

static std::vector<std::string> SplitLines( const std::string& str )
{
	std::vector<std::string> out;
	std::string::size_type start = 0;
	while( start < str.size() ) {
		std::string::size_type nl = str.find( '\n', start );
		if( nl == std::string::npos ) {
			out.push_back( str.substr( start ) );
			break;
		}
		std::string line = str.substr( start, nl - start );
		if( !line.empty() && line.back() == '\r' ) {
			line.pop_back();
		}
		out.push_back( line );
		start = nl + 1;
	}
	return out;
}

static bool IsRasterizerHeader( const std::string& t )
{
	if( t.size() < 11 ) return false;
	if( t.find( ' ' ) != std::string::npos || t.find( '\t' ) != std::string::npos ) return false;
	const std::string suffix = "_rasterizer";
	return t.size() >= suffix.size() &&
	       t.compare( t.size() - suffix.size(), suffix.size(), suffix ) == 0;
}

static bool FindChunk( const std::vector<std::string>& lines, bool (*pred)( const std::string& ), size_t& rs, size_t& re )
{
	for( size_t i = 0; i < lines.size(); ++i ) {
		std::string t = lines[i];
		while( !t.empty() && ( t.front() == ' ' || t.front() == '\t' ) ) t.erase( t.begin() );
		while( !t.empty() && ( t.back() == ' ' || t.back() == '\t' ) ) t.pop_back();
		if( !pred( t ) ) continue;

		size_t braceStart = i + 1;
		while( braceStart < lines.size() ) {
			std::string bt = lines[braceStart];
			while( !bt.empty() && ( bt.front() == ' ' || bt.front() == '\t' ) ) bt.erase( bt.begin() );
			if( bt == "{" ) break;
			++braceStart;
		}
		if( braceStart >= lines.size() ) return false;

		int depth = 1;
		size_t k = braceStart + 1;
		while( k < lines.size() && depth > 0 ) {
			for( char c : lines[k] ) {
				if( c == '{' ) depth++;
				else if( c == '}' ) depth--;
			}
			if( depth == 0 ) break;
			++k;
		}
		if( depth == 0 ) {
			rs = i;
			re = k;
			return true;
		}
	}
	return false;
}

static std::string MakeAutoProbeScene( const char* corpusPath, unsigned int samples, unsigned int dim )
{
	const std::string text = ReadFileToString( corpusPath );
	if( text.empty() ) return std::string();
	std::vector<std::string> lines = SplitLines( text );

	size_t rs, re;
	if( !FindChunk( lines, IsRasterizerHeader, rs, re ) ) return std::string();

	char autoChunk[256];
	std::snprintf( autoChunk, sizeof(autoChunk),
		"auto_rasterizer\n{\n\tintegrator auto\n\tprobe true\n\tsamples %u\n\tpixel_filter box\n\toidn_denoise false\n}",
		samples );

	std::vector<std::string> out;
	for( size_t i = 0; i < lines.size(); ++i ) {
		if( i == rs ) { out.push_back( autoChunk ); i = re; continue; }
		out.push_back( lines[i] );
	}

	size_t fs, fe;
	auto isFilm = []( const std::string& t ) { return t == "film"; };
	if( FindChunk( out, isFilm, fs, fe ) ) {
		char filmChunk[128];
		std::snprintf( filmChunk, sizeof(filmChunk),
			"film\n{\n\twidth %u\n\theight %u\n}", dim, dim );
		std::vector<std::string> out2;
		for( size_t i = 0; i < out.size(); ++i ) {
			if( i == fs ) { out2.push_back( filmChunk ); i = fe; continue; }
			out2.push_back( out[i] );
		}
		out.swap( out2 );
	}

	std::string joined;
	for( size_t i = 0; i < out.size(); ++i ) { joined += out[i]; joined.push_back( '\n' ); }
	return joined;
}

struct StatsSummary
{
	double mean;
	double sd;
};

static StatsSummary CalcStats( const std::vector<double>& vals )
{
	StatsSummary s{ 0.0, 0.0 };
	if( vals.empty() ) return s;
	double sum = 0.0;
	for( double v : vals ) sum += v;
	s.mean = sum / double( vals.size() );
	if( vals.size() > 1 ) {
		double sumSq = 0.0;
		for( double v : vals ) sumSq += ( v - s.mean ) * ( v - s.mean );
		s.sd = std::sqrt( sumSq / double( vals.size() - 1 ) );
	}
	return s;
}

int main( int argc, char* argv[] )
{
	const unsigned int nTrials = 3;
	std::cout << "================================================================================" << std::endl;
	std::cout << "DL-167 Reach Signal Measurement Harness (Trials=" << nTrials << ", Probe scale=4, spp=4)" << std::endl;
	std::cout << "================================================================================" << std::endl;

	struct SceneStats
	{
		std::string name;
		std::string truth;
		bool entersCaustic;
		StatsSummary meanRatio;
		StatsSummary robustMeanRatio;
		StatsSummary medRatio;
		StatsSummary mergeShare;
		StatsSummary p95Ratio;
		StatsSummary p90Ratio;
		StatsSummary fracGt2;
		StatsSummary fracGt1_5;
		StatsSummary energyFracGt2;
	};

	std::vector<SceneStats> allResults;

	for( const auto& entry : kScenes ) {
		std::cout << "Running scene: " << entry.name << " (" << entry.truthWinner << ")..." << std::flush;
		const std::string sceneText = MakeAutoProbeScene( entry.path, 4, 128 );
		if( sceneText.empty() ) {
			std::cout << " FAILED TO LOAD" << std::endl;
			continue;
		}

		char tmpPath[256];
		std::snprintf( tmpPath, sizeof(tmpPath), "/tmp/caustic_harness_%s.RISEscene", entry.name );
		{
			std::ofstream ofs( tmpPath );
			ofs << sceneText;
		}

		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob || !pJob->LoadAsciiSceneViaCst( tmpPath ) ) {
			std::remove( tmpPath );
			if( pJob ) safe_release( pJob );
			std::cout << " FAILED TO PARSE" << std::endl;
			continue;
		}
		std::remove( tmpPath );

		AutoRasterizer* pAuto = dynamic_cast<AutoRasterizer*>( pJob->GetRasterizer() );
		const IScene* scene = pJob->GetScene();

		if( !pAuto || !scene ) {
			safe_release( pJob );
			std::cout << " NOT AUTO/SCENE" << std::endl;
			continue;
		}

		// Read actual probe config
		AutoRasterizer::ProbeConfig cfg = pAuto->ForTest_ReadProbeConfig();
		cfg.scale = 4;
		cfg.spp = 4;

		std::vector<double> vMeanR, vRobMeanR, vMedR, vMergeShare, vP95, vP90, vFracGt2, vFracGt1_5, vEFracGt2;

		for( unsigned int t = 0; t < nTrials; ++t ) {
			std::srand( 12345 + t * 997 );
			AutoRasterizer::ProbeResult ptRes = pAuto->ForTest_ProbeCandidate( scene, AutoIntegratorChoice::PT, cfg, false );
			AutoRasterizer::ProbeResult vcmRes = pAuto->ForTest_ProbeCandidate( scene, AutoIntegratorChoice::VCM, cfg, false );

			if( !ptRes.valid || !vcmRes.valid ) continue;

			AutoRasterizer::CausticReachSignals sig = AutoRasterizer::ComputeCausticReachSignals( ptRes, vcmRes );
			if( !sig.valid ) continue;

			vMeanR.push_back( sig.meanRatio );
			vRobMeanR.push_back( sig.robustMeanRatio );
			vMedR.push_back( sig.medRatio );
			vMergeShare.push_back( sig.vcmMergeShare );
			vP95.push_back( sig.p95Ratio );
			vP90.push_back( sig.p90Ratio );
			vFracGt2.push_back( sig.fracGt2 );
			vFracGt1_5.push_back( sig.fracGt1_5 );
			vEFracGt2.push_back( sig.energyFracGt2 );
		}

		safe_release( pJob );

		SceneStats ss;
		ss.name = entry.name;
		ss.truth = entry.truthWinner;
		ss.entersCaustic = !vMeanR.empty();
		ss.meanRatio = CalcStats( vMeanR );
		ss.robustMeanRatio = CalcStats( vRobMeanR );
		ss.medRatio = CalcStats( vMedR );
		ss.mergeShare = CalcStats( vMergeShare );
		ss.p95Ratio = CalcStats( vP95 );
		ss.p90Ratio = CalcStats( vP90 );
		ss.fracGt2 = CalcStats( vFracGt2 );
		ss.fracGt1_5 = CalcStats( vFracGt1_5 );
		ss.energyFracGt2 = CalcStats( vEFracGt2 );

		allResults.push_back( ss );
		std::cout << " done (trials=" << vMeanR.size()
		          << " mergeShare=" << ss.mergeShare.mean << "+-" << ss.mergeShare.sd << ")" << std::endl;
	}

	std::cout << std::endl;
	std::cout << "======================================================================================================================================================" << std::endl;
	std::cout << std::setw(20) << "Scene"
	          << std::setw(8)  << "Truth"
	          << std::setw(15) << "meanRatio"
	          << std::setw(15) << "robMeanRatio"
	          << std::setw(15) << "medRatio"
	          << std::setw(16) << "mergeShare"
	          << std::setw(15) << "p95Ratio"
	          << std::setw(15) << "p90Ratio"
	          << std::setw(15) << "fracGt2"
	          << std::setw(15) << "energyFracGt2"
	          << std::endl;
	std::cout << "======================================================================================================================================================" << std::endl;

	for( size_t i = 0; i < allResults.size(); ++i ) {
		const auto& r = allResults[i];
		char bMean[32], bRobMean[32], bMed[32], bMerge[32], bP95[32], bP90[32], bFrac2[32], bEFrac[32];
		std::snprintf( bMean, sizeof(bMean), "%.2f+-%.2f", r.meanRatio.mean, r.meanRatio.sd );
		std::snprintf( bRobMean, sizeof(bRobMean), "%.2f+-%.2f", r.robustMeanRatio.mean, r.robustMeanRatio.sd );
		std::snprintf( bMed, sizeof(bMed), "%.2f+-%.2f", r.medRatio.mean, r.medRatio.sd );
		std::snprintf( bMerge, sizeof(bMerge), "%.4f+-%.4f", r.mergeShare.mean, r.mergeShare.sd );
		std::snprintf( bP95, sizeof(bP95), "%.2f+-%.2f", r.p95Ratio.mean, r.p95Ratio.sd );
		std::snprintf( bP90, sizeof(bP90), "%.2f+-%.2f", r.p90Ratio.mean, r.p90Ratio.sd );
		std::snprintf( bFrac2, sizeof(bFrac2), "%.3f+-%.3f", r.fracGt2.mean, r.fracGt2.sd );
		std::snprintf( bEFrac, sizeof(bEFrac), "%.3f+-%.3f", r.energyFracGt2.mean, r.energyFracGt2.sd );

		std::cout << std::setw(20) << r.name
		          << std::setw(8)  << r.truth
		          << std::setw(15) << bMean
		          << std::setw(15) << bRobMean
		          << std::setw(15) << bMed
		          << std::setw(16) << bMerge
		          << std::setw(15) << bP95
		          << std::setw(15) << bP90
		          << std::setw(15) << bFrac2
		          << std::setw(15) << bEFrac
		          << std::endl;
	}
	std::cout << "======================================================================================================================================================" << std::endl;

	return 0;
}
