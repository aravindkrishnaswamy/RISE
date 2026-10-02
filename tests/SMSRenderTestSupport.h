#pragma once
// Shared, undenoised, salted capture support for the cheapbatch SMS regressions.
#include <cstdio>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

inline unsigned int g_seedBase = 3400u;
inline unsigned int g_renderIndex = 0;
inline int passCount = 0;
inline int failCount = 0;

inline void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Capture
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	std::vector<RISEColor> rawPixels;
	unsigned int delayMs = 0;
	std::atomic<bool> delayed{false};
	CapturingRasterizerOutput() {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {
        if( delayMs && !delayed.exchange(true) ) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    virtual void OutputPreDenoisedImage( const IRasterImage& img, const Rect* r, const unsigned int frame ) override {
        OutputImage(img,r,frame);
        rawPixels=pixels;
    }
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		pixels.resize( img.GetWidth() * img.GetHeight() );
		for( unsigned int y = 0; y < img.GetHeight(); y++ ) {
			for( unsigned int x = 0; x < img.GetWidth(); x++ ) {
				pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
			}
		}
	}
};

struct RenderResult
{
	std::vector<RISEColor> pixels;
	double mean;				//!< grey mean, composited over black
	unsigned long long rawHash;
	unsigned long long hash;	//!< FNV-1a of the final captured pixel doubles
	unsigned int appliedDelayMs;
	bool ok;
};

//! FNV-1a over the captured buffer -- the bit-identity A/B for the
//! single-sided control rows.
inline unsigned long long HashPixels( const std::vector<RISEColor>& pixels )
{
	unsigned long long h = 1469598103934665603ULL;
	for( const RISEColor& c : pixels ) {
		const double v[4] = { c.base.r, c.base.g, c.base.b, c.a };
		const unsigned char* b = reinterpret_cast<const unsigned char*>( v );
		for( std::size_t k = 0; k < sizeof( v ); k++ ) {
			h ^= b[k];
			h *= 1099511628211ULL;
		}
	}
	return h;
}

inline constexpr uint32_t kSaltTag = 0xCBu;

// Configure before the first cached GlobalOptions read. Paired inputs
// differ only in the contract under test, not worker RNG start order.
inline void ConfigureTestWorker()
{
    static const bool configured=[]() {
        if(!std::getenv("RISE_OPTIONS_FILE")) {
            static char optionsPath[512];
            std::snprintf(optionsPath,sizeof(optionsPath),"/tmp/cheapbatch_options_%d.txt",int(::getpid()));
            std::ofstream options(optionsPath);
            options << "force_number_of_threads 1\n";
            options.close();
#ifdef _WIN32
            _putenv_s("RISE_OPTIONS_FILE",optionsPath);
#else
            setenv("RISE_OPTIONS_FILE",optionsPath,1);
#endif
            std::atexit([](){ std::remove(optionsPath); });
        }
        return true;
    }();
    (void)configured;
}

inline RenderResult Render( const std::string& sceneText, const char* tag, unsigned int delayMs = 0 )
{
	ConfigureTestWorker();
	RenderResult r{ {}, 0, 0, 0, 0, false };
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/cheapbatch_%s_%d.RISEscene", tag, static_cast<int>( ::getpid() ) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return r;
		ofs << sceneText;
	}
	const unsigned int seed=g_seedBase+g_renderIndex;
	std::srand(seed);
	GlobalRNG()=RandomNumberGenerator(seed);
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { std::remove( path ); return r; }
	if( pJob->LoadAsciiSceneViaCst( path ) ) {
		pJob->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
		pCap->delayMs = delayMs;
		pJob->GetRasterizer()->AddRasterizerOutput( pCap );
		const uint32_t salt = SobolSequence::HashCombine( g_seedBase + g_renderIndex, kSaltTag );
		SobolSamplerTestHooks::ValueSalt().store( salt );
		std::srand(seed);
		GlobalRNG()=RandomNumberGenerator(seed);
		++g_renderIndex;
		const bool bRendered = pJob->Rasterize();
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		if( bRendered && !pCap->pixels.empty() ) {
			double sum = 0;
			bool finite = true;
			for( const RISEColor& c : pCap->pixels ) {
				const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
				if( !std::isfinite( v ) ) { finite = false; break; }
				sum += v;
			}
			if( finite ) {
				r.mean = sum / double( pCap->pixels.size() );
				r.hash = HashPixels( pCap->pixels );
				r.rawHash = HashPixels( pCap->rawPixels );
				r.pixels = pCap->pixels;
				r.appliedDelayMs = pCap->delayed.load() ? delayMs : 0;
				r.ok = true;
			}
		}
		safe_release( pCap );
	}
	safe_release( pJob );
	std::remove( path );
	return r;
}


struct Stats { double mean, sd; };
inline Stats Summarize( const std::vector<double>& values )
{
    double mean=0, ss=0;
    for( double v : values ) mean += v;
    mean /= values.size();
    for( double v : values ) ss += (v-mean)*(v-mean);
    return { mean, values.size()>1 ? std::sqrt(ss/(values.size()-1)) : 0.0 };
}

inline std::string ReadScene( const char* path )
{
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
inline void ReplaceNamedChunk( std::string& scene, const std::string& type,
    const std::string& name, const std::string& replacement )
{
    const auto namePos=scene.find(name);
    if( namePos==std::string::npos ) { Check(false,"named chunk found"); return; }
    const auto begin=scene.rfind(type,namePos);
    const auto end=scene.find("\n}",namePos);
    if(begin==std::string::npos || end==std::string::npos) { Check(false,"named chunk bounds"); return; }
    scene.replace(begin,end+2-begin,replacement);
}
inline void ReplaceFirstChunk( std::string& scene, const std::string& type, const std::string& replacement )
{
    const auto begin=scene.find(type+"\n{");
    const auto end=scene.find("\n}",begin);
    if(begin==std::string::npos || end==std::string::npos) { Check(false,"chunk bounds"); return; }
    scene.replace(begin,end+2-begin,replacement);
}
