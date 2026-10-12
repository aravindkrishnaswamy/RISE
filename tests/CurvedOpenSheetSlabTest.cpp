// Reviewer fixture (DL-382): curved open indexed-mesh glass slabs, PT/BDPT/VCM parity.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
#include <filesystem>
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
#include "../src/Library/Utilities/SobolSampler.h"

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

static unsigned int g_seedBase = 1000;
static unsigned int g_renderIndex = 0;
static std::string g_optPath;
static std::string TempFile(const std::string& leaf)
{
    std::error_code error;
    const auto directory=std::filesystem::temp_directory_path(error);
    return error?std::string():(directory/leaf).string();
}

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	CapturingRasterizerOutput() {}
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		const unsigned int w = img.GetWidth(), h = img.GetHeight();
		pixels.resize( w * h );
		for( unsigned int y = 0; y < h; y++ ) {
			for( unsigned int x = 0; x < w; x++ ) {
				pixels[y * w + x] = img.GetPEL( x, y );
			}
		}
	}
};

static unsigned long long g_hash=0;
static double MeanLuminance( const CapturingRasterizerOutput& cap )
{
	{ unsigned long long h=1469598103934665603ULL; for( const RISEColor& c : cap.pixels ) { const unsigned char* b=(const unsigned char*)&c; for(size_t k=0;k<sizeof(RISEColor);++k){ h^=b[k]; h*=1099511628211ULL; } } g_hash=h; }
	if( cap.pixels.empty() ) return -1.0;
	double sum = 0;
	for( const RISEColor& c : cap.pixels ) {
		const double r = c.base.r * c.a, g = c.base.g * c.a, b = c.base.b * c.a;
		if( !std::isfinite( r ) || !std::isfinite( g ) || !std::isfinite( b ) ) return -1.0;
		sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
	}
	return sum / double( cap.pixels.size() );
}

//! One render, Sobol'-salted by (seed base, render index) so repeats are
//! independent randomized-QMC draws, libc rand() pinned likewise.
static double Render( const std::string& sceneText )
{
    const std::string path=TempFile("curved_open_sheet_"+std::to_string(::getpid())+".RISEscene");
    if(path.empty()) return -1;
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return -1.0;
		ofs << sceneText;
	}
	std::srand( g_seedBase + g_renderIndex );
	SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 0x0345u + g_seedBase, g_renderIndex ) );
	g_renderIndex++;

	double result = -1.0;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob )
	{
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) )
		{
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			if( pJob->Rasterize() ) {
				result = MeanLuminance( *pCap );
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	std::remove( path.c_str() );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Rasterizers (oidn off, box filter).
//////////////////////////////////////////////////////////////////////
static const char* kOutputChunk =
	"file_rasterizeroutput\n{\n\tpattern rendered/open_sheet_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

enum Integrator { kPT, kPTSMS, kBDPT, kVCM, kNumIntegrators, kPTTransparentShadows };
static const char* kIntegratorName[kNumIntegrators] = { "PT", "PT+SMS", "BDPT", "VCM" };

static std::string Rasterizer( Integrator which )
{
	std::ostringstream ss;
	switch( which ) {
	case kPT:
	case kPTSMS:
		ss << "pathtracing_pel_rasterizer\n{\n\tsamples 256\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n"
		   << "\tsms_enabled " << ( which == kPTSMS ? "TRUE" : "FALSE" ) << "\n}\n\n";
		break;
	case kPTTransparentShadows:
		ss << "pathtracing_pel_rasterizer\n{\n\tsamples 64\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\ttransparent_shadows TRUE\n}\n\n";
		break;
	case kBDPT:
		ss << "bdpt_pel_rasterizer\n{\n\tsamples 128\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
		break;
	default:
		ss << "vcm_pel_rasterizer\n{\n\tsamples 128\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
		   << "\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
		break;
	}
	ss << kOutputChunk;
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// Scenes (y up).  Receiver: 2 x 2 Lambertian (rho 0.5) at y 0.  Emitter:
// 4 x 4 one-sided luminaire at y 4 facing DOWN.  Glass: ior 1.5 perfect
// refractor, 8 x 8.  A clipped plane wound (-x,+z) -> (+x,+z) -> (+x,-z)
// faces UP (+y); the reverse winding faces DOWN.
//////////////////////////////////////////////////////////////////////
static const double K = 0.01;
static const int NG = 8;
struct P3 { double x,y,z; };
static P3 GP( int i, int j, double y0, double k ) { double x=-4+8.0*i/NG, z=-4+8.0*j/NG; return P3{ x, y0 + k*(x*x+z*z), z }; }
static void Tri( std::ostringstream& v, std::ostringstream& t, int& nv, P3 a, P3 b, P3 c, double ox, double oy, double oz )
{
	// orient so that (b-a)x(c-a) . o > 0
	double ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z, wx=c.x-a.x,wy=c.y-a.y,wz=c.z-a.z;
	double nx=uy*wz-uz*wy, ny=uz*wx-ux*wz, nz=ux*wy-uy*wx;
	if( nx*ox+ny*oy+nz*oz < 0 ) std::swap( b, c );
	for( P3 p : {a,b,c} ) v << "\tvertex " << p.x << " " << p.y << " " << p.z << "\n";
	t << "\ttriangle " << nv << " " << nv+1 << " " << nv+2 << "\n"; nv += 3;
}
// Welded grid (shared vertices) for a single sheet.
static std::string Sheet( const char* name, double y0, bool up, double k )
{
	std::ostringstream s; s << "indexedmesh_geometry\n{\n\tname " << name << "\n";
	for( int j=0;j<=NG;++j ) for( int i=0;i<=NG;++i ) { P3 p=GP(i,j,y0,k); s << "\tvertex " << p.x << " " << p.y << " " << p.z << "\n"; }
	auto id=[&](int i,int j){ return j*(NG+1)+i; };
	for( int j=0;j<NG;++j ) for( int i=0;i<NG;++i ) {
		int a=id(i,j+1), b=id(i+1,j+1), c=id(i+1,j), d=id(i,j);
		if( up ) { s << "\ttriangle " << a << " " << b << " " << c << "\n\ttriangle " << a << " " << c << " " << d << "\n"; }
		else { s << "\ttriangle " << a << " " << c << " " << b << "\n\ttriangle " << a << " " << d << " " << c << "\n"; }
	}
	s << "\tdouble_sided TRUE\n}\n\n"; return s.str();
}
// Closed curved slab, per-corner (welded by DL-143), outward wound.
static std::string ClosedSlab( const char* name, double yTop, double yBot, double k )
{
	std::ostringstream v, t; int nv=0;
	for( int j=0;j<NG;++j ) for( int i=0;i<NG;++i ) {
		P3 a=GP(i,j,yTop,k), b=GP(i+1,j,yTop,k), c=GP(i+1,j+1,yTop,k), d=GP(i,j+1,yTop,k);
		Tri(v,t,nv,a,b,c,0,1,0); Tri(v,t,nv,a,c,d,0,1,0);
		P3 A=GP(i,j,yBot,k), B=GP(i+1,j,yBot,k), C=GP(i+1,j+1,yBot,k), D=GP(i,j+1,yBot,k);
		Tri(v,t,nv,A,B,C,0,-1,0); Tri(v,t,nv,A,C,D,0,-1,0);
	}
	for( int e=0;e<NG;++e ) {
		struct { int i0,j0,i1,j1; double ox,oz; } sides[4] = { {e,0,e+1,0,0,-1}, {e,NG,e+1,NG,0,1}, {0,e,0,e+1,-1,0}, {NG,e,NG,e+1,1,0} };
		for( auto& sd : sides ) {
			P3 a=GP(sd.i0,sd.j0,yTop,k), b=GP(sd.i1,sd.j1,yTop,k), c=GP(sd.i1,sd.j1,yBot,k), d=GP(sd.i0,sd.j0,yBot,k);
			Tri(v,t,nv,a,b,c,sd.ox,0,sd.oz); Tri(v,t,nv,a,c,d,sd.ox,0,sd.oz);
		}
	}
	return std::string("indexedmesh_geometry\n{\n\tname ")+name+"\n"+v.str()+t.str()+"\tdouble_sided TRUE\n}\n\n";
}
enum Glass { kCurvedSingle, kCurvedSingleDown, kCurvedOpenSlab, kCurvedClosedSlab };
enum Camera { kBelow, kAbove };
static std::string Scene( Glass glass, Camera cam )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n";
	if( cam == kBelow ) ss << "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n";
	else ss << "pinhole_camera\n{\n\tlocation 0 3 1.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 8.0\n}\n\n";
	ss << "uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 4.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_emit\n\tpta -2 4 -2\n\tptb 2 4 -2\n\tptc 2 4 2\n\tptd -2 4 2\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_glass\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"perfectrefractor_material\n{\n\tname mat_glass\n\trefractance pnt_glass\n\tior 1.5\n}\n\n";
	switch( glass ) {
	case kCurvedSingle: case kCurvedSingleDown:
		ss << Sheet( "geo_sheet", 2.0, glass == kCurvedSingle, K ) << "standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_glass\n}\n\n"; break;
	case kCurvedOpenSlab:
		ss << Sheet( "geo_top", 2.1, true, K ) << Sheet( "geo_bot", 1.9, false, K )
		   << "standard_object\n{\n\tname obj_top\n\tgeometry geo_top\n\tmaterial mat_glass\n}\n\n"
		      "standard_object\n{\n\tname obj_bot\n\tgeometry geo_bot\n\tmaterial mat_glass\n}\n\n"; break;
	default:
		ss << ClosedSlab( "geo_box", 2.1, 1.9, K ) << "standard_object\n{\n\tname obj_box\n\tgeometry geo_box\n\tmaterial mat_glass\n}\n\n"; break;
	}
	return ss.str();
}
static std::string Assemble( Integrator which, Glass glass, Camera cam )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" + Rasterizer( which ) + Scene( glass, cam );
}
struct Stat { double mean, sd; };
static Stat Measure( Integrator which, Glass glass, Camera cam, unsigned int n )
{
	std::vector<double> v; for( unsigned int i=0;i<n;i++ ) v.push_back( Render( Assemble( which, glass, cam ) ) );
	double m=0; for( double x : v ) m+=x; m/=double(n);
	double s=0; for( double x : v ) s+=(x-m)*(x-m); s = n>1 ? std::sqrt( s/double(n-1) ) : 0.0;
	return Stat{ m, s/std::sqrt(double(n)) };	// sd of the mean
}
static void RatioRow( const std::string& label, const Stat& a, const Stat& b )
{
	const double r=a.mean/b.mean; const double sdr=r*std::sqrt((a.sd/a.mean)*(a.sd/a.mean)+(b.sd/b.mean)*(b.sd/b.mean));
	std::printf( "  %s: %.5f / %.5f = %.4f +/- %.4f\n", label.c_str(), a.mean, b.mean, r, sdr );
    Check(a.mean>0 && b.mean>0 && std::isfinite(r) && std::fabs(r-1)<0.05, label);
}
int main( int argc, char** argv )
{
    g_optPath=TempFile("curved_options_"+std::to_string(::getpid())+".txt");
    if(g_optPath.empty()) return 1;
	{ std::ofstream opt( g_optPath ); opt << "force_number_of_threads 1\n"; }
	#ifdef _WIN32
    _putenv_s("RISE_OPTIONS_FILE", g_optPath.c_str());
#else
    setenv( "RISE_OPTIONS_FILE", g_optPath.c_str(), 1 );
#endif
	unsigned int n = 8; if( const char* s = std::getenv( "ZN" ) ) n = (unsigned)std::atoi( s );
	if( argc > 1 ) g_seedBase = (unsigned)std::atoi( argv[1] );
	if( const char* zs = std::getenv( "ZSCENE" ) ) {
		std::ifstream in( zs ); std::stringstream b; b << in.rdbuf(); std::vector<double> v;
		for( unsigned i=0;i<n;i++ ) v.push_back( Render( b.str() ) );
		double m=0; for(double x:v) m+=x; m/=n; double sd=0; for(double x:v) sd+=(x-m)*(x-m); sd=std::sqrt(sd/(n-1))/std::sqrt(double(n));
		std::printf( "%s mean %.10g +/- %.6f hash %016llx\n", zs, m, sd, g_hash ); return 0; }
	const char* camName[2] = { "below", "above" };
	if( std::getenv( "ZDUMP" ) ) { std::cout << Assemble( kPT, kCurvedClosedSlab, kBelow ) << Assemble( kPT, kCurvedOpenSlab, kBelow ); return 0; }
	for( int w=0; w<2; ++w ) {
		Glass g = w==0 ? kCurvedSingle : kCurvedSingleDown;
		Stat s[kNumIntegrators]; for( int i=0;i<kNumIntegrators;i++ ) s[i]=Measure( Integrator(i), g, kBelow, n );
		for( int i=0;i<kNumIntegrators;i++ ) RatioRow( std::string("curved single ")+(w?"down":"up")+" "+kIntegratorName[i]+" / PT", s[i], s[kPT] );
	}
	for( int c=0;c<2;c++ ) {
		Stat closed[kNumIntegrators], open[kNumIntegrators];
		for( int i=0;i<kNumIntegrators;i++ ) { closed[i]=Measure( Integrator(i), kCurvedClosedSlab, Camera(c), n ); open[i]=Measure( Integrator(i), kCurvedOpenSlab, Camera(c), n ); }
		for( int i=0;i<kNumIntegrators;i++ ) RatioRow( std::string("curved open slab / closed ")+kIntegratorName[i]+" cam "+camName[c], open[i], closed[i] );
		for( int i=1;i<kNumIntegrators;i++ ) RatioRow( std::string("curved open slab ")+kIntegratorName[i]+" / PT cam "+camName[c], open[i], open[kPT] );
		for( int i=1;i<kNumIntegrators;i++ ) RatioRow( std::string("curved closed slab ")+kIntegratorName[i]+" / PT cam "+camName[c], closed[i], closed[kPT] );
	}
	std::remove( g_optPath.c_str() );
	std::cout << passCount << " passed, " << failCount << " failed\n";
    return failCount ? 1 : 0;
}
