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

// Explicit render seeding: library renders are NOT
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
#include <iomanip>
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Shaders/VCMIntegrator.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/PathValueOps.h"

// A Lambertian plane under an on-axis point light at distance D has
// L=rho*I*cos(theta)/(pi*D^2) at the center. Every point on this
// bounded plane differs from the head-on value by <0.21% through 45deg
// at D=1000, safely below the 2% gate without an integrator reference.
// This oracle is independent of every renderer and BSDF helper.
static std::string FlatScene(const char* integrator, double degrees, int spp)
{
    std::ostringstream s;
    s << std::setprecision(17);
    s << "RISE ASCII SCENE 7\nstandard_shader\n{\n name global\n shaderop DefaultDirectLighting\n}\n";
    s << integrator << "\n{\n samples " << spp << "\n oidn_denoise FALSE\n pixel_filter box\n}\n";
    s << "film\n{\n width 32\n height 32\n}\n"
         "pinhole_camera\n{\n location 0 0 -4\n lookat 0 0 0\n up 0 1 0\n fov 30\n}\n"
         "scalar_painter\n{\n name slope\n expression P.x\n}\n"
         "relief_modifier\n{\n name relief\n height slope\n domain surface\n scale " << std::tan(degrees*PI/180) << "\n}\n"
         "uniformcolor_painter\n{\n name albedo\n color 0.8 0.8 0.8\n colorspace Rec709RGB_Linear\n}\n"
         "lambertian_material\n{\n name matte\n reflectance albedo\n}\n"
         "clippedplane_geometry\n{\n name plane\n pta -2 -2 0\n ptb -2 2 0\n ptc 2 2 0\n ptd 2 -2 0\n}\n"
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


static void Sphere(int spp=256)
{
    const std::string body=R"SCENE(RISE ASCII SCENE 7
standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

film
{
	width			200
	height			200
}

pinhole_camera
{
	location		0 0 6
	lookat			0 0 0
	up			0 1 0
	fov			40.0
}

# ---- the relief field: a smooth low-frequency uv bump, DL-157's tilt knob ----
expression_function2d
{
	name	bump_field
	param	nu 8.0
	param	nv 6.0
	expr	0.5 + 0.25*sin(tau*nu*u)*sin(tau*nv*v)
}

scalar_painter
{
	name	bump_height
	function2d	bump_field
}

relief_modifier
{
	name	bumps
	height	bump_height
	domain	uv
	step	0.004
	scale	SCALE
}

uniformcolor_painter
{
	name			p_ref
	color			0.5 0.3 0.2
}

uniformcolor_painter
{
	name			p_tau
	color			0.4 0.6 0.3
}

uniformcolor_painter
{
	name			p_emit
	color			1.0 1.0 1.0
}

lambertian_material
{
	name			mat_translucent
	reflectance		p_ref
}

lambertian_luminaire_material
{
	name			mat_light
	exitance		p_emit
	scale			40.0
	material		none
}

sphere_geometry
{
	name			sph
	radius			1.2
}

# A ceiling quad at y = 3 whose winding puts its normal at -Y (down at
# the sphere).  It is outside the 40-degree frame (half-height 2.18 at
# z = 0), so no emitter pixel enters the measured region directly.
clippedplane_geometry
{
	name			quad
	pta			-1.2 3.0 -1.2
	ptb			 1.2 3.0 -1.2
	ptc			 1.2 3.0  1.2
	ptd			-1.2 3.0  1.2
}

standard_object
{
	name			o_sphere
	geometry		sph
	material		mat_translucent
	modifier		bumps
	position		0 0 0
}

standard_object
{
	name			o_light
	geometry		quad
	material		mat_light
	position		0 0 0
}
)SCENE";
    for(double scale:{0.,-.2,.2}) {
        double refs[3]={};
        const char* modes[]={"pathtracing_pel_rasterizer", "bdpt_pel_rasterizer", "vcm_pel_rasterizer"};
        for(int m=0;m<3;++m) {
            std::string scene=body;
            scene.replace(scene.find("SCALE"),5,std::to_string(scale));
            scene+=std::string(modes[m])+"\n{\n samples "+std::to_string(spp)+"\n oidn_denoise FALSE\n pixel_filter box\n}\n";
            double sum=0,sum2=0;
            for(int r=0;r<3;++r) {
                auto* cap=RenderScene(scene,modes[m]);
                Check(cap!=nullptr,"sphere scene renders");
                if(!cap) continue;
                double mean=0;
                for(unsigned y=60;y<140;++y) for(unsigned x=60;x<140;++x) {
                    const auto& c=cap->pixels[y*cap->width+x];
                    mean+=(c.base.r+c.base.g+c.base.b)*c.a/(3*6400);
                }
                std::printf("SPHERE_RAW mode=%s scale=%.2f repeat=%d mean=%.9f\n",modes[m],scale,r,mean);
                sum+=mean;sum2+=mean*mean;
                safe_release(cap);
            }
            refs[m]=sum/3;
            std::printf("SPHERE mode=%s scale=%.2f spp=%d n=3 mean=%.9f sd=%.12g\n",modes[m],scale,spp,refs[m],std::sqrt(std::max(0.,(sum2-sum*sum/3)/2)));
            if(m) Check(refs[0]>0 && refs[m]>0 && std::fabs(refs[m]/refs[0]-1)<.08,"sphere PT/bidirectional within 8%");
        }
    }
}


// Inspect a LIVE generated light path, independently pricing one Lambertian
// bounce by differential power on the geometric receiving area. The large
// sphere only catches continuations so the post-bounce beta is observable.
static void LightWalk()
{
    std::string scene=FlatScene("bdpt_pel_rasterizer",30,1);
    const std::string far="position 0 0 -1000";
    scene.replace(scene.find(far),far.size(),"position 0 0 -1");
    scene+="sphere_geometry\n{\n name catcher\n radius 10\n}\n"
           "standard_object\n{\n name catch_object\n geometry catcher\n material matte\n}\n";
    const std::string path=WriteSceneToTempFile(scene,"lightwalk");
    IJobPriv* job=nullptr;
    Check(RISE_CreateJobPriv(&job) && job && job->LoadAsciiSceneViaCst(path.c_str()),"light-walk scene loads");
    if(!job) return;
    StandardShader* shader=new StandardShader(std::vector<IShaderOp*>());
    RayCaster* caster=new RayCaster(false,8,*shader,false);
    caster->AttachScene(job->GetScene());
    StabilityConfig cfg;
    cfg.rrMinDepth=20;
    BDPTIntegrator* bdpt=new BDPTIntegrator(4,4,cfg);
    bdpt->SetLightSampler(caster->GetLightSampler());
    RandomNumberGenerator rng(224001);
    IndependentSampler sampler(rng);
    unsigned count=0;
    double worst=0;
    for(unsigned i=0;i<2048;++i) {
        std::vector<BDPTVertex> verts;
        std::vector<uint32_t> starts;
        bdpt->GenerateLightSubpath(*job->GetScene(),*caster,sampler,verts,starts,rng);
        if(verts.size()<3 || std::fabs(verts[1].position.z)>1e-6 || std::fabs(verts[1].normal.x)<.1) continue;
        const auto& v=verts[1];
        const Vector3 wi=Vector3Ops::Normalize(Vector3Ops::mkVector3(verts[0].position,v.position));
        const Vector3 wo=Vector3Ops::Normalize(Vector3Ops::mkVector3(verts[2].position,v.position));
        // With Ng=-Z and Ns=(-sin30,0,-cos30), projected input flux
        // changes by (sin30*wi.x+cos30*wi.z)/wi.z. The sampled output
        // has cosine density about Ns, whereas received power projects
        // onto Ng. No production correction helper enters this oracle.
        const double expected=.8*std::fabs((.5*wi.x+std::sqrt(.75)*wi.z)*wo.z /
                        (wi.z*(.5*wo.x+std::sqrt(.75)*wo.z)));
        const double got=verts[2].throughput.r/v.throughput.r;
        // Preserve every accepted sample, including the large grazing
        // adjoint factors. This is evidence, never an outlier filter.
        std::printf("LIGHT_RAW path=%u wi=(%.17g,%.17g,%.17g) wo=(%.17g,%.17g,%.17g) expected=%.17g got=%.17g\n",
            i,wi.x,wi.y,wi.z,wo.x,wo.y,wo.z,expected,got);
        worst=std::max(worst,std::fabs(got/expected-1));
        ++count;
    }
    std::printf("LIGHT_WALK count=%u worst_relative=%.9g\n",count,worst);
    Check(count>100,"live light walk visits tilted surface and next receiver");
    Check(worst<1e-5,"importance throughput equals independent projected-power oracle");
    safe_release(bdpt);safe_release(caster);safe_release(shader);safe_release(job);
}


// Both the camera and light lie below the perturbed shading normal, but
// above the physical plane. LambertianSPF samples the flipped shading
// hemisphere on this legitimate grazing hit; NEE must use that same frame.
static void GrazingView()
{
    for(const char* mode:{"pixelpel_rasterizer","pathtracing_pel_rasterizer","bdpt_pel_rasterizer","vcm_pel_rasterizer"}) {
        std::string scene=FlatScene(mode,45,64);
        auto replace=[&](const std::string& from,const std::string& to) { scene.replace(scene.find(from),from.size(),to); };
        replace("location 0 0 -4","location 4 0 -1");
        replace("fov 30","fov 3");
        replace("position 0 0 -1000","position 866.025403784 0 -500");
        auto* cap=RenderScene(scene,mode);
        Check(cap!=nullptr,"grazing scene renders");
        if(!cap) continue;
        const double mean=Mean(*cap);
        const double expected=.8*std::sin(PI/12)/PI;
        std::printf("GRAZING mode=%s mean=%.12g expected=%.12g relative=%+.6f\n",mode,mean,expected,mean/expected-1);
        Check(std::fabs(mean/expected-1)<.02,"grazing flipped shading frame follows cosine law");
        safe_release(cap);
        replace("position 866.025403784 0 -500","position 0 0 -1000");
        cap=RenderScene(scene,mode);
        Check(cap!=nullptr,"opposite shading hemisphere scene renders");
        if(cap) {
            Check(Mean(*cap)<1e-12,"opposite shading hemisphere remains dark");
            safe_release(cap);
        }
    }
}


static void ModifierSiblings()
{
    for(const char* kind:{"normalmap","displacement"}) for(const char* mode:{"pathtracing_pel_rasterizer","bdpt_pel_rasterizer","vcm_pel_rasterizer"}) {
        std::string scene=FlatScene(mode,0,64);
        const std::string binding="modifier relief";
        if(std::string(kind)=="normalmap") {
            scene.insert(scene.find("standard_object\n"),"uniformcolor_painter\n{\n name normal_field\n color 0.8535533905932737 0.5 0.8535533905932737\n colorspace Rec709RGB_Linear\n}\n"
                   "normal_map_modifier\n{\n name normal_mod\n normal_map normal_field\n}\n");
            scene.replace(scene.find(binding),binding.size(),"modifier normal_mod");
        } else {
            scene.insert(scene.find("standard_object\n"),"displaced_geometry\n{\n name displaced\n base_geometry plane\n detail 8\n height slope\n disp_scale 1\n}\n");
            scene.erase(scene.find(binding),binding.size());
            const std::string geomBinding="\n geometry plane\n";
            scene.replace(scene.find(geomBinding),geomBinding.size(),"\n geometry displaced\n");
        }
        auto* cap=RenderScene(scene,kind);
        Check(cap!=nullptr,"sibling modifier scene renders");
        if(!cap) continue;
        const double mean=Mean(*cap),expected=.8/(PI*std::sqrt(2.));
        std::printf("SIBLING kind=%s mode=%s mean=%.12g expected=%.12g relative=%+.6f\n",kind,mode,mean,expected,mean/expected-1);
        Check(std::fabs(mean/expected-1)<.02,"normal-map/displacement follows same independent cosine law");
        safe_release(cap);
    }
}


static void Spectral()
{
    for(int spp:{64,128}) for(const char* mode:{"pathtracing_spectral_rasterizer","bdpt_spectral_rasterizer","vcm_spectral_rasterizer"}) for(bool hwss:{false,true}) {
        std::string scene=FlatScene(mode,30,spp);
        const size_t at=scene.find("\n{\n",scene.find(mode));
        scene.insert(at+3,std::string(" num_wavelengths 160\n hwss ")+(hwss?"TRUE":"FALSE")+"\n");
        double sum=0,sum2=0;
        for(int r=0;r<3;++r) {
            auto* cap=RenderScene(scene,mode);
            Check(cap!=nullptr,"matched-grid spectral scene renders");
            if(!cap) continue;
            const double mean=Mean(*cap);sum+=mean;sum2+=mean*mean;
            std::printf("SPECTRAL_RAW mode=%s hwss=%d spp=%d repeat=%d mean=%.12g\n",mode,hwss,spp,r,mean);
            safe_release(cap);
        }
        const double mean=sum/3,expected=.8*std::sqrt(.75)/PI;
        std::printf("SPECTRAL mode=%s hwss=%d nw=160 spp=%d n=3 mean=%.12g sd=%.12g expected=%.12g\n",mode,hwss,spp,mean,std::sqrt(std::max(0.,(sum2-sum*sum/3)/2)),expected);
        Check(std::fabs(mean/expected-1)<.02,"spectral hero/bundle follows independent cosine law");
    }
}

static void EndpointFactors()
{
    BDPTVertex v;
    v.type=BDPTVertex::SURFACE;
    v.geomNormal=Vector3(0,0,1);
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.vNormal=Vector3(0,0,-1);
    Check(ri.RayFacingShadingCosine(Vector3(0,0,1))==1,"back-facing Ns orients toward view");
    Check(ri.RayFacingShadingCosine(Vector3(0,0,-1))==-1,"opposite shading hemisphere retains negative support gate");
    ri.vNormal=Vector3(0,0,1);
    Check(ri.RayFacingShadingCosine(Vector3(0,0,1))==1,"untilted signed cosine is unchanged");
    for(double degrees:{0.,1.,10.,20.,30.,45.}) {
        double theta=degrees*PI/180;
        v.normal=Vector3(std::sin(theta),0,std::cos(theta));
        const double factor=PathVertexEval::RadianceShadingNormalFactor(v,Vector3(0,0,1));
        Check(std::fabs(factor-std::cos(theta))<1e-14,"endpoint factor follows cosine law, including small tilt");
    }
}

// These are fixed path-space densities, not BSDF evaluations. Changing Ns
// while keeping positions, Ng and all sampling densities fixed cannot change
// any MIS density ratio. Exercise both walks, delta transport and BSSRDF entry.
static void DensityMeasures()
{
    const auto norm=ComputeNormalization(100,100,0,true,false);
    for(bool light:{false,true}) for(int kind=0;kind<3;++kind) {
        std::vector<BDPTVertex> v(3);
        for(int i=0;i<3;++i) {
            v[i].type=i?BDPTVertex::SURFACE:(light?BDPTVertex::LIGHT:BDPTVertex::CAMERA);
            v[i].position=Point3(i==0?0:i==1?2:5,0,0);
            v[i].normal=v[i].geomNormal=Vector3(i?-1:1,0,0);
            v[i].cosAtGen=1;
            v[i].isConnectible=true;
            v[i].pdfFwd=i==0?(light?.25:1):i==1?.1:.2;
            v[i].pdfRev=i==0?.3:i==1?.5:0;
            v[i].emissionPdfW=i==0?(light?.125:2):0;
        }
        v[1].isDelta=kind==1;
        v[1].isBSSRDFEntry=kind==2;
        std::vector<VCMMisQuantities> base,tilted;
        std::vector<LightVertex> store;
        if(light) VCMIntegrator::ConvertLightSubpath(v,norm,store,&base);
        else VCMIntegrator::ConvertEyeSubpath(v,norm,base);
        v[1].normal=Vector3(-.5,std::sqrt(.75),0);
        if(light) VCMIntegrator::ConvertLightSubpath(v,norm,store,&tilted);
        else VCMIntegrator::ConvertEyeSubpath(v,norm,tilted);
        Check(base.size()==3 && tilted.size()==3,"density arrays complete");
        if(base.size()!=3 || tilted.size()!=3) continue;
        const double error=std::max(std::fabs(base[2].dVC-tilted[2].dVC),std::fabs(base[2].dVM-tilted[2].dVM));
        std::printf("DENSITY light=%d kind=%d base=(%.12g,%.12g,%.12g) tilted=(%.12g,%.12g,%.12g) error=%.12g\n",light,kind,base[2].dVCM,base[2].dVC,base[2].dVM,tilted[2].dVCM,tilted[2].dVC,tilted[2].dVM,error);
        Check(error<1e-10,"fixed geometric densities independent of shading frame");
    }
}

int main(int argc, char** argv)
{
    if(argc==1 || std::string(argv[1])=="light") LightWalk();
    EndpointFactors();
    DensityMeasures();
    if(argc==1 || std::string(argv[1])=="spectral") Spectral();
    if(argc==1 || std::string(argv[1])=="siblings") ModifierSiblings();
    if(argc==1 || std::string(argv[1])=="grazing") GrazingView();
    const char* modes[]={"pixelpel_rasterizer", "pathtracing_pel_rasterizer", "bdpt_pel_rasterizer", "vcm_pel_rasterizer"};
    if(argc==1 || std::string(argv[1])=="flat") for(double tilt:{0.,10.,20.,30.,45.}) {
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
            std::printf("FLAT mode=%s tilt=%.0f spp=64 n=%zu mean=%.9f sd=%.12g expected=%.9f relative=%+.6f\n",mode,tilt,means.size(),mean,sd,expected,mean/expected-1);
            Check(means.size()==3 && std::fabs(mean/expected-1)<.02,"flat tilt radiance within 2% of independent closed form");
        }
    }
    if(argc==1 || std::string(argv[1])=="sphere") Sphere();
    if(argc>1 && std::string(argv[1])=="sphere512") Sphere(512);
    std::printf("ReliefBidirectionalConsistencyTest: %d passed, %d failed\n",passCount,failCount);
    return failCount?1:0;
}
