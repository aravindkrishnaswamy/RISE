//////////////////////////////////////////////////////////////////////
//
//  EmitterAverageExitanceTest.cpp - DL-431 regression: the AVERAGE
//    radiant exitance of a luminary -- the light-selection importance
//    weight (LightSampler) and the photon power / photon budget
//    (PhotonTracer, SpectralPhotonTracer, SMSPhotonMap) -- must be
//    estimated over the luminary's OWN surface points, not over a
//    default-constructed record (P = Po = (0,0,0)).
//
//    THE BUG: LambertianEmitter / PhongEmitter / CompositeEmitter
//    ::RefreshAverages sampled a 10x10 UV grid at construction, where an
//    emitter does not know its geometry, so emission keyed on world /
//    object position was weighted by its value AT THE ORIGIN.  A field that
//    vanishes there got zero importance and was never sampled; one that
//    does not vanish got the wrong multi-light importance and a wrong
//    (biased) photon power.
//
//    ROWS
//      1. Render: a 2x2 emitter BEHIND the camera facing a diffuse wall
//         (the DL-44 / DL-298 rig), exitance M(P) = 1.5 (P.x^2 + P.y^2)
//         (zero at the origin) vs (a) the same field keyed on the
//         emitter's UV (control, read correctly by every path) and (b) a
//         deterministic closed form.  PT / BDPT / VCM RGB, PT / BDPT
//         spectral.
//      2. Photons: the real PhotonTracer / SpectralPhotonTracer emission
//         loops with an ideal deposit sink on a unit sphere whose exitance
//         is 1.5 (P.x^2 + P.y^2): total emitted flux must be the closed
//         form 4 pi, and the flux-weighted <z^2> must be 1/5 (the field's
//         own spatial distribution; a uniform power would read 1/3).  A
//         second sphere displaced to x = -10 with the field 1.5 ((P.x+10)^2
//         + P.y^2) makes the origin value (150) nonzero but wrong.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

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
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/PhotonMapping/GlobalPelPhotonTracer.h"
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonTracer.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Managers/CameraManager.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Scene.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width, height;
	CapturingRasterizerOutput() : width( 0 ), height( 0 ) {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int ) override
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

static double MeanLuminance( const CapturingRasterizerOutput& cap )
{
	if( cap.pixels.empty() ) return -1.0;
	double sum = 0;
	for( const RISEColor& c : cap.pixels ) {
		sum += 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
	}
	return sum / double( cap.pixels.size() );
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	const char* tmpBase = std::getenv( "TMPDIR" );
	if( !tmpBase || !*tmpBase ) tmpBase = "/tmp";
	std::snprintf( path, sizeof(path), "%s/emitter_average_exitance_%s_%d.RISEscene",
		tmpBase, tag, static_cast<int>( ::getpid() ) );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	return std::string( path );
}

static double RenderMean( const char* scenePath, unsigned salt )
{
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return -1.0;
	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return -1.0;
	}
	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );
	std::srand( 431000u + salt );
	SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 0x431u, salt ) );
	const bool bRendered = pJob->Rasterize();
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	const double mean = bRendered ? MeanLuminance( *pCap ) : -1.0;
	safe_release( pCap );
	safe_release( pJob );
	return mean;
}

struct Stat { double mean, sd; int n; };

//! Salted repeats (unsalted repeats of a Sobol' render reuse identical points).
static Stat SaltedMean( const std::string& scene, const char* tag, int n )
{
	const std::string path = WriteSceneToTempFile( scene, tag );
	Stat s = { -1.0, 0.0, 0 };
	if( path.empty() ) return s;
	double sum = 0, sumSq = 0;
	for( int i = 0; i < n; i++ ) {
		const double m = RenderMean( path.c_str(), unsigned( i ) + 1u );
		if( m < 0 ) { std::remove( path.c_str() ); return s; }
		sum += m;  sumSq += m * m;
	}
	std::remove( path.c_str() );
	s.n = n;
	s.mean = sum / n;
	s.sd = n > 1 ? std::sqrt( std::max( 0.0, ( sumSq - n * s.mean * s.mean ) / ( n - 1 ) ) ) : 0.0;
	return s;
}


//////////////////////////////////////////////////////////////////////
// Row 1: render.
//////////////////////////////////////////////////////////////////////
static const char* kGeometry =
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n";

static const char* kEmitterObject =
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -1.0 1.0 6.0\n\tptb 1.0 1.0 6.0\n\tptc 1.0 -1.0 6.0\n\tptd -1.0 -1.0 6.0\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// M = 1.5 r^2 (area mean 1.0 over the 2x2 quad, ZERO at the origin).
static const char* kLambertP =
	"expression_painter\n{\n\tname pnt_ex\n\texpr 1.5*(P.x*P.x+P.y*P.y)\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_ex\n\tscale 10.0\n\tmaterial none\n}\n\n";
static const char* kLambertUV =
	"expression_painter\n{\n\tname pnt_ex\n\texpr 1.5*((2.0*u-1.0)*(2.0*u-1.0)+(2.0*v-1.0)*(2.0*v-1.0))\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_ex\n\tscale 10.0\n\tmaterial none\n}\n\n";

static const char* kPT =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
static const char* kBDPT =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
static const char* kVCM =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 256\n\toidn_denoise FALSE\n"
	"\tpixel_filter box\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n}\n";
static const char* kPTSpectral =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
	"pathtracing_spectral_rasterizer\n{\n\tsamples 512\n\toidn_denoise FALSE\n\tpixel_filter box\n\tnmbegin 380\n\tnmend 720\n"
	"\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss false\n\tmax_diffuse_bounce 3\n}\n";
static const char* kBDPTSpectral =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 512\n\tnmbegin 380\n\tnmend 720\n"
	"\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss false\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";

static std::string Build( const char* emitter, const char* rasterizer )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += kGeometry;
	s += emitter;
	s += kEmitterObject;
	s += "\n";
	s += rasterizer;
	return s;
}

// Mean over the frame's pixel centres of the wall radiance
//   L(p) = (0.8/pi) * integral L_e cos_p cos_e / d^2 dA_e,  L_e = 10 * 1.5 r^2 / pi.
static double ClosedForm()
{
	const int W = 24, G = 160;
	const double kScale = 10.0, rho = 0.8, PI_ = 3.14159265358979323846;
	const double half = 3.5 * std::tan( 15.0 * PI_ / 180.0 );
	double acc = 0;
	for( int j = 0; j < W; j++ ) for( int i = 0; i < W; i++ ) {
		const double px = half * ( 2.0 * ( i + 0.5 ) / W - 1.0 );
		const double py = half * ( 2.0 * ( j + 0.5 ) / W - 1.0 );
		double E = 0;
		for( int b = 0; b < G; b++ ) for( int a = 0; a < G; a++ ) {
			const double ex = -1.0 + 2.0 * ( a + 0.5 ) / G;
			const double ey = -1.0 + 2.0 * ( b + 0.5 ) / G;
			const double dx = px - ex, dy = py - ey, dz = 6.0;
			const double d2 = dx * dx + dy * dy + dz * dz;
			const double cs = dz / std::sqrt( d2 );
			const double Le = kScale * 1.5 * ( ex * ex + ey * ey ) / PI_;
			E += Le * cs * cs / d2 * ( 4.0 / ( G * G ) );
		}
		acc += rho / PI_ * E;
	}
	return acc / ( W * W );
}

struct Row { const char* name; const char* rast; };

static void RunRender()
{
	std::cout << "Row 1: render, exitance M(P) = 1.5 (P.x^2 + P.y^2) (zero at the origin)\n";
	const double truth = ClosedForm();
	std::printf( "  closed form (RGB wall mean) = %.6f\n", truth );
	static const Row rows[] = {
		{ "PT RGB",        kPT },
		{ "BDPT RGB",      kBDPT },
		{ "VCM RGB",       kVCM },
		{ "PT spectral",   kPTSpectral },
		{ "BDPT spectral", kBDPTSpectral },
	};
	for( const Row& r : rows ) {
		const bool rgb = std::string( r.name ).find( "RGB" ) != std::string::npos;
		const Stat p = SaltedMean( Build( kLambertP,  r.rast ), "p",  4 );
		const Stat c = SaltedMean( Build( kLambertUV, r.rast ), "uv", 4 );
		const double ratio = c.mean > 1e-9 ? p.mean / c.mean : -1.0;
		std::printf( "  %-14s P-keyed %.6f (sd %.6f)  UV control %.6f (sd %.6f)  P/UV = %.4f",
			r.name, p.mean, p.sd, c.mean, c.sd, ratio );
		if( rgb ) std::printf( "  P/closed = %.4f  UV/closed = %.4f", p.mean / truth, c.mean / truth );
		std::printf( "\n" );
		const std::string label = std::string( "render / " ) + r.name;
		Check( c.mean > 1e-6, label + ": UV control is lit" );
		Check( p.mean > 1e-6, label + ": P-keyed emitter is lit (not weighted by its origin value 0)" );
		if( c.mean > 1e-6 ) {
			Check( ratio > 0.97 && ratio < 1.03, label + ": P-keyed agrees with the UV-keyed control" );
		}
		if( rgb ) {
			Check( std::fabs( p.mean / truth - 1.0 ) < 0.05, label + ": P-keyed matches the closed form" );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Row 2: photons.
//////////////////////////////////////////////////////////////////////
template<class Base> class RGBTracer : public PhotonTracer<Base> {
	void TraceSinglePhoton( const Ray& r, const RISEPel& p, Base&, const IORStack& ) const override
	{
		++calls;  flux += p.r;  fluxZ2 += p.r * r.origin.z * r.origin.z;
	}
	void SetSpecificPhotonMapForScene( Base* ) const override {}
public:
	mutable double flux = 0, fluxZ2 = 0;  mutable unsigned calls = 0;
	RGBTracer() : PhotonTracer<Base>( false, 1, 1, true ) {}
};
template<class Base> class NMTracer : public SpectralPhotonTracer<Base> {
	void TraceSinglePhoton( const Ray& r, Scalar p, Scalar, Base&, const IORStack& ) const override
	{
		++calls;  flux += p;  fluxZ2 += p * r.origin.z * r.origin.z;
	}
	void SetSpecificPhotonMapForScene( Base* ) const override {}
public:
	mutable double flux = 0, fluxZ2 = 0;  mutable unsigned calls = 0;
	NMTracer() : SpectralPhotonTracer<Base>( 550, 552, 1, 1, 1, true ) {}
};

struct SphereFixture
{
	Scene* scene = new Scene;  ObjectManager* objects = new ObjectManager( false, false, 4, 8 );  LightManager* lights = new LightManager;
	IPainter* painter;  LambertianMaterial* base;  LambertianLuminaireMaterial* lum;  SphereGeometry* geo;
	static IPainter* MakeExpr( const char* text )
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;  b.EnableContextVars( true );
		if( !b.Finalize( text, prog ) ) { std::cout << "  expression failed to compile\n"; return nullptr; }
		std::vector<ParamSpec> params;
		return new ExpressionPainter( prog, params, 0.0, eSpectrumKind_Unbounded );
	}
	SphereFixture( const char* expr, double x, double scale )
	{
		scene->SetObjectManager( objects );  scene->SetLightManager( lights );
		auto* cameras = new CameraManager;  scene->SetCameraManager( cameras );  cameras->release();
		auto* camera = new PinholeCamera( Point3( 0, 0, 30 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), .5, 1, 1, 1, 1, 0, 0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );
		scene->AddCamera( "camera", camera );  scene->SetActiveCamera( "camera" );  camera->release();
		painter = MakeExpr( expr );
		auto* white = new UniformColorPainter( RISEPel( 1 ) );
		base = new LambertianMaterial( *white );
		lum = new LambertianLuminaireMaterial( *painter, scale, *base );
		white->release();
		geo = new SphereGeometry( 1 );
		auto* o = new Object( geo );  o->AssignMaterial( *lum );  o->SetPosition( Point3( x, 0, 0 ) );  o->FinalizeTransformations();
		objects->AddItem( o, "lum" );  o->release();
	}
	~SphereFixture() { geo->release(); lum->release(); base->release(); painter->release(); scene->release(); objects->release(); lights->release(); }
};

// The tracers' power is flux / numPhotons only after the photon map's own
// 1/N normalisation, which this ideal sink bypasses: divide by N here.
static const double kPhotons = 100003.0;

template<class Tracer> static void PhotonRow( const char* label, const char* expr, double x, double controlScale, bool nm )
{
	// Control: the SAME photon loop on a constant exitance of 1 (the field's area mean), to
	// absorb the spectral uplift / illuminant normalisation of the NM path.
	double controlFlux = 0;
	{
		SphereFixture f( "1.0", x, controlScale );
		auto* t = new Tracer;  t->AttachScene( f.scene );
		Check( t->TracePhotons( 100003, 0, false, nullptr ), std::string( label ) + ": control shoot succeeds" );
		controlFlux = t->flux / kPhotons;  t->release();
	}
	SphereFixture f( expr, x, controlScale );
	auto* t = new Tracer;  t->AttachScene( f.scene );
	Check( t->TracePhotons( 100003, 0, false, nullptr ), std::string( label ) + ": shoot succeeds" );
	const double flux = t->flux / kPhotons, zz = t->flux > 0 ? t->fluxZ2 / t->flux : -1.0;
	t->release();
	const double truth = nm ? controlFlux : 4.0 * 3.14159265358979323846;
	std::printf( "  %-34s flux %.6f  expected %.6f (ratio %.4f)  flux-weighted <z^2> = %.4f (field 0.2000, uniform 0.3333)\n",
		label, flux, truth, truth > 0 ? flux / truth : -1.0, zz );
	Check( std::fabs( flux / truth - 1.0 ) < 0.03, std::string( label ) + ": total emitted flux matches the closed form" );
	Check( std::fabs( zz - 0.2 ) < 0.02, std::string( label ) + ": flux follows the field's own spatial distribution" );
}

int main()
{
	std::cout << "=== EmitterAverageExitanceTest (DL-431) ===\n";
	RunRender();
	std::cout << "Row 2: photons (ideal deposit sink), unit sphere, exitance 1.5 (P.x^2 + P.y^2)\n";
	PhotonRow<RGBTracer<GlobalPelPhotonMap>>( "RGB, vanishing at the origin", "1.5*(P.x*P.x+P.y*P.y)", 0, 1, false );
	PhotonRow<RGBTracer<GlobalPelPhotonMap>>( "RGB, origin value 150 (wrong)", "1.5*((P.x+10.0)*(P.x+10.0)+P.y*P.y)", -10, 1, false );
	PhotonRow<NMTracer<GlobalSpectralPhotonMap>>( "NM, vanishing at the origin", "1.5*(P.x*P.x+P.y*P.y)", 0, 1, true );
	std::cout << "\nPassed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
