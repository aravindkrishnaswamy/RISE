// DL-224: independent direct-light closed form, with a real relief modifier.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
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
#include "../src/Library/Utilities/Color/Color_Template.h"

// Explicit render seeding (rise-render-seeding.md): renders are NOT
// wall-clock seeded, and this file compares raw pixel statistics
// across three separate renders, so each must srand() explicitly.
static unsigned int g_renderSeed = 2240001u;

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- same shape as EnvLightBalanceTest /
// BDPTStrategyBalanceTest.
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
	{
		width = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/dl224_relief_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//! Renders one scene, returning the captured buffer.  Empty (width==0)
//! on any load/render failure.
static CapturingRasterizerOutput* RenderScene( const std::string& sceneText, const char* tag )
{
	const std::string path = WriteSceneToTempFile( sceneText, tag );
	if( path.empty() ) {
		return nullptr;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return nullptr;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		safe_release( pJob );
		return nullptr;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pCap->addref();
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( g_renderSeed++ );
	const bool bRendered = pJob->Rasterize();

	safe_release( pJob );

	if( !bRendered || pCap->width == 0 ) {
		safe_release( pCap );
		return nullptr;
	}
	return pCap;
}


#include <sstream>

// A Lambertian plane under an on-axis point light at distance D has
// L=rho*I*cos(theta)/(pi*D^2). The symmetric image removes the odd
// off-axis x term exactly; D=1000 makes inverse-square variation <4e-6.
// This oracle is independent of every renderer and BSDF helper.
static std::string FlatScene(const char* integrator, double degrees, int spp)
{
    std::ostringstream s;
    s << "RISE ASCII SCENE 7\nstandard_shader\n{\n name global\n shaderop DefaultDirectLighting\n}\n";
    s << integrator << "\n{\n samples " << spp << "\n oidn_denoise FALSE\n pixel_filter box\n}\n";
    s << "film\n{\n width 32\n height 32\n}\n"
         "pinhole_camera\n{\n location 0 0 -4\n lookat 0 0 0\n up 0 1 0\n fov 30\n}\n"
         "scalar_painter\n{\n name slope\n expression P.x\n}\n"
         "relief_modifier\n{\n name relief\n height slope\n domain surface\n scale " << std::tan(degrees*PI/180) << "\n}\n"
         "lambertian_material\n{\n name matte\n reflectance 0.8 0.8 0.8\n}\n"
         "clippedplane_geometry\n{\n name plane\n pointa -2 -2 0\n pointb -2 2 0\n pointc 2 2 0\n pointd 2 -2 0\n}\n"
         "standard_object\n{\n name target\n geometry plane\n material matte\n modifier relief\n}\n"
         "omni_light\n{\n name key\n power 1000000\n color 1 1 1\n colorspace Rec709RGB_Linear\n position 0 0 -1000\n}\n";
    return s.str();
}

static double Mean(const CapturingRasterizerOutput& cap)
{
    double sum=0;
    for(const auto& c:cap.pixels) {
        const double v=(c.base.r+c.base.g+c.base.b)*c.a/3;
        if(!std::isfinite(v)) return -1;
        sum+=v;
    }
    return cap.pixels.empty() ? -1 : sum/cap.pixels.size();
}

int main()
{
    const char* modes[]={"pixelpel_rasterizer", "pathtracing_pel_rasterizer", "bdpt_pel_rasterizer", "vcm_pel_rasterizer"};
    for(double tilt:{0.,10.,20.,30.,45.}) {
        const double expected=.8*std::cos(tilt*PI/180)/PI;
        for(const char* mode:modes) {
            std::vector<double> means;
            for(int r=0;r<3;++r) {
                auto* cap=RenderScene(FlatScene(mode,tilt,64),mode);
                Check(cap!=nullptr,"flat scene loads and renders");
                if(!cap) continue;
                means.push_back(Mean(*cap));
                safe_release(cap);
            }
            double mean=0,sd=0;
            for(double x:means) mean+=x;
            if(!means.empty()) mean/=means.size();
            for(double x:means) sd+=(x-mean)*(x-mean);
            sd=means.size()>1?std::sqrt(sd/(means.size()-1)):0;
            std::printf("FLAT mode=%s tilt=%.0f spp=64 n=%zu mean=%.9f sd=%.9f expected=%.9f relative=%+.6f\n",mode,tilt,means.size(),mean,sd,expected,mean/expected-1);
            Check(means.size()==3 && std::fabs(mean/expected-1)<.02,"flat tilt radiance within 2% of independent closed form");
        }
    }
    std::printf("ReliefBidirectionalConsistencyTest: %d passed, %d failed\n",passCount,failCount);
    return failCount?1:0;
}
