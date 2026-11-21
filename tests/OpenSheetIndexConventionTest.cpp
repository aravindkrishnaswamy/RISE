//////////////////////////////////////////////////////////////////////
//
//  OpenSheetIndexConventionTest.cpp - DL-345 / DL-339 (b): the index
//    convention of an OPEN transmissive sheet.
//
//  An open sheet (a surface that provably encloses no volume:
//  `RayIntersectionGeometric::bProvablyNoInterior`) is an interface
//  whose FRONT side (the side its true geometric normal points to) is
//  the surrounding medium and whose BACK side is the sheet's material.
//  Every crossing refracts BY ITS FACE -- front -> back enters, back ->
//  front exits -- and a radiance walk pays (eta_from / eta_to)^2 of the
//  refraction it actually performed.  Before DL-345 the transmissive
//  materials decided "entering" from the IOR stack alone, so the eye and
//  light walks bent at DIFFERENT sheets of a two-sheet slab (and at a
//  single sheet reached from behind), and PT / BDPT / VCM / SMS read
//  different images of one scene (up to 3x).
//
//  Rows (each config: n salted renders, mean +/- sd, single worker):
//    single  a lone ior-1.5 open sheet between an area emitter and a
//            receiver the camera sees DIRECTLY (DL-339 (b)'s fixture):
//            PT, PT+SMS, BDPT, VCM must agree.
//    slab    a 0.2-thick ior-1.5 slab built as two open sheets (flat
//            clipped planes, and displaced_geometry over them at
//            disp_scale 0 -- the shipped SMS scenes' construction)
//            against the SAME slab as one closed box, with the camera
//            below it (receiver seen directly) and above it (receiver
//            seen through the slab): each integrator open / closed ~ 1.
//    mesh    DL-382: the same sheets as planar `indexedmesh_geometry`
//            quads (certified open by `IsProvablyOpenSheet`), both
//            windings: mesh == clipped plane, every integrator agrees,
//            two-mesh slab == closed box.  Red (certification off):
//            7 passed / 9 failed (PT 0.44 of the plane; slab 0.59-0.63).
//            A slab authored as ONE two-sheet mesh is recorded, not gated.
//
//  argv[1]: seed base (default 1000).  OPEN_SHEET_N (env): repeats per
//  config (default 4).  OPEN_SHEET_MESH_ONLY (env): run only the mesh rows.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
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
#include "TestTempDir.h"

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
static char g_optPath[1024];

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

static double MeanLuminance( const CapturingRasterizerOutput& cap )
{
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
	char path[1024];
	std::snprintf( path, sizeof(path), "%s/open_sheet_%d.RISEscene", RISE::TestSupport::ScratchBaseDir().c_str(), static_cast<int>( ::getpid() ) );
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
		if( pJob->LoadAsciiSceneViaCst( path ) )
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
	std::remove( path );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Rasterizers (oidn off, box filter).
//////////////////////////////////////////////////////////////////////
static const char* kOutputChunk =
	"file_rasterizeroutput\n{\n\tpattern rendered/open_sheet_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

enum Integrator { kPT, kPTSMS, kBDPT, kVCM, kNumIntegrators, kPTTransparentShadows };
static const char* kIntegratorName[kNumIntegrators] = { "PT", "PT+SMS", "BDPT", "VCM" };

static std::string Rasterizer( Integrator which, unsigned int sppOverride = 0 )
{
	std::ostringstream ss;
	switch( which ) {
	case kPT:
	case kPTSMS:
		ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << ( sppOverride ? sppOverride : 64u )
		   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n"
		   << "\tsms_enabled " << ( which == kPTSMS ? "TRUE" : "FALSE" ) << "\n}\n\n";
		break;
	case kPTTransparentShadows:
		ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << ( sppOverride ? sppOverride : 32u )
		   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\ttransparent_shadows TRUE\n}\n\n";
		break;
	case kBDPT:
		ss << "bdpt_pel_rasterizer\n{\n\tsamples " << ( sppOverride ? sppOverride : 32u )
		   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
		break;
	default:
		ss << "vcm_pel_rasterizer\n{\n\tsamples " << ( sppOverride ? sppOverride : 32u )
		   << "\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
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
enum Glass { kSingleSheet, kOpenSlab, kDisplacedSlab, kClosedSlab, kOpenStacked, kClosedStacked,
	kSingleSheetDown, kMeshSingle, kMeshSingleDown, kMeshSlab,
	kMeshSingleOneSided, kCurvedOpenSlab, kCurvedClosedSlab };
enum Camera { kBelow, kAbove };

static std::string PlaneChunk( const char* name, double y, bool up, bool doubleSided = true )
{
	std::ostringstream ss;
	ss << "clippedplane_geometry\n{\n\tname " << name << "\n";
	if( up ) {
		ss << "\tpta -4 " << y << " 4\n\tptb 4 " << y << " 4\n\tptc 4 " << y << " -4\n\tptd -4 " << y << " -4\n";
	} else {
		ss << "\tpta -4 " << y << " -4\n\tptb 4 " << y << " -4\n\tptc 4 " << y << " 4\n\tptd -4 " << y << " 4\n";
	}
	ss << "\tdoublesided " << ( doubleSided ? "TRUE" : "FALSE" ) << "\n}\n\n";
	return ss.str();
}

//! DL-382: the same 8 x 8 sheet as an inline indexed mesh (two
//! triangles, same corner order as PlaneChunk, so the same facing).
static std::string MeshChunk( const char* name, double y, bool up, bool doubleSided = true )
{
	std::ostringstream ss;
	ss << "indexedmesh_geometry\n{\n\tname " << name << "\n";
	if( up ) {
		ss << "\tvertex -4 " << y << " 4\n\tvertex 4 " << y << " 4\n\tvertex 4 " << y << " -4\n\tvertex -4 " << y << " -4\n";
	} else {
		ss << "\tvertex -4 " << y << " -4\n\tvertex 4 " << y << " -4\n\tvertex 4 " << y << " 4\n\tvertex -4 " << y << " 4\n";
	}
	ss << "\ttriangle 0 1 2\n\ttriangle 0 2 3\n";
	ss << "\tdouble_sided " << ( doubleSided ? "TRUE" : "FALSE" ) << "\n}\n\n";
	return ss.str();
}

//! Curved open indexed-mesh glass slab helpers (DL-382, parabolic K = 0.01).
static const double kCurvedK = 0.01;
static const int kCurvedGrid = 8;
struct CurvedP3 { double x, y, z; };
static CurvedP3 CurvedGP( int i, int j, double y0, double k )
{
	const double x = -4.0 + 8.0 * i / kCurvedGrid;
	const double z = -4.0 + 8.0 * j / kCurvedGrid;
	return CurvedP3{ x, y0 + k * ( x * x + z * z ), z };
}
static void CurvedTri( std::ostringstream& v, std::ostringstream& t, int& nv,
	CurvedP3 a, CurvedP3 b, CurvedP3 c, double ox, double oy, double oz )
{
	const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
	const double wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
	const double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
	if( nx * ox + ny * oy + nz * oz < 0.0 ) std::swap( b, c );
	for( CurvedP3 p : { a, b, c } ) v << "\tvertex " << p.x << " " << p.y << " " << p.z << "\n";
	t << "\ttriangle " << nv << " " << nv + 1 << " " << nv + 2 << "\n";
	nv += 3;
}
static std::string CurvedSheetChunk( const char* name, double y0, bool up, double k )
{
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n\tname " << name << "\n";
	for( int j = 0; j <= kCurvedGrid; ++j ) {
		for( int i = 0; i <= kCurvedGrid; ++i ) {
			const CurvedP3 p = CurvedGP( i, j, y0, k );
			s << "\tvertex " << p.x << " " << p.y << " " << p.z << "\n";
		}
	}
	auto id = []( int i, int j ) { return j * ( kCurvedGrid + 1 ) + i; };
	for( int j = 0; j < kCurvedGrid; ++j ) {
		for( int i = 0; i < kCurvedGrid; ++i ) {
			const int a = id( i, j + 1 ), b = id( i + 1, j + 1 ), c = id( i + 1, j ), d = id( i, j );
			if( up ) {
				s << "\ttriangle " << a << " " << b << " " << c << "\n\ttriangle " << a << " " << c << " " << d << "\n";
			} else {
				s << "\ttriangle " << a << " " << c << " " << b << "\n\ttriangle " << a << " " << d << " " << c << "\n";
			}
		}
	}
	s << "\tdouble_sided TRUE\n}\n\n";
	return s.str();
}
static std::string CurvedClosedSlabChunk( const char* name, double yTop, double yBot, double k )
{
	std::ostringstream v, t;
	int nv = 0;
	for( int j = 0; j < kCurvedGrid; ++j ) {
		for( int i = 0; i < kCurvedGrid; ++i ) {
			CurvedP3 a = CurvedGP( i, j, yTop, k ), b = CurvedGP( i + 1, j, yTop, k );
			CurvedP3 c = CurvedGP( i + 1, j + 1, yTop, k ), d = CurvedGP( i, j + 1, yTop, k );
			CurvedTri( v, t, nv, a, b, c, 0, 1, 0 );
			CurvedTri( v, t, nv, a, c, d, 0, 1, 0 );
			CurvedP3 A = CurvedGP( i, j, yBot, k ), B = CurvedGP( i + 1, j, yBot, k );
			CurvedP3 C = CurvedGP( i + 1, j + 1, yBot, k ), D = CurvedGP( i, j + 1, yBot, k );
			CurvedTri( v, t, nv, A, B, C, 0, -1, 0 );
			CurvedTri( v, t, nv, A, C, D, 0, -1, 0 );
		}
	}
	for( int e = 0; e < kCurvedGrid; ++e ) {
		struct Side { int i0, j0, i1, j1; double ox, oz; };
		const Side sides[4] = {
			{ e, 0, e + 1, 0, 0, -1 },
			{ e, kCurvedGrid, e + 1, kCurvedGrid, 0, 1 },
			{ 0, e, 0, e + 1, -1, 0 },
			{ kCurvedGrid, e, kCurvedGrid, e + 1, 1, 0 }
		};
		for( const Side& sd : sides ) {
			CurvedP3 a = CurvedGP( sd.i0, sd.j0, yTop, k ), b = CurvedGP( sd.i1, sd.j1, yTop, k );
			CurvedP3 c = CurvedGP( sd.i1, sd.j1, yBot, k ), d = CurvedGP( sd.i0, sd.j0, yBot, k );
			CurvedTri( v, t, nv, a, b, c, sd.ox, 0, sd.oz );
			CurvedTri( v, t, nv, a, c, d, sd.ox, 0, sd.oz );
		}
	}
	return std::string( "indexedmesh_geometry\n{\n\tname " ) + name + "\n" + v.str() + t.str() + "\tdouble_sided TRUE\n}\n\n";
}

static std::string Scene( Glass glass, Camera cam, bool omni = false )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 12\n\theight 12\n}\n\n";
	if( cam == kBelow ) {
		ss << "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n";
	} else {
		ss << "pinhole_camera\n{\n\tlocation 0 3 1.0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 8.0\n}\n\n";
	}
	ss <<
		"uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 4.0\n\tmaterial none\n}\n\n"
		<< ( omni
			? "omni_light\n{\n\tname lgt\n\tposition 0 4 0\n\tcolor 1 1 1\n\tpower 16\n}\n\n"
			: "clippedplane_geometry\n{\n\tname geo_emit\n\tpta -2 4 -2\n\tptb 2 4 -2\n\tptc 2 4 2\n\tptd -2 4 2\n\tdoublesided FALSE\n}\n\n"
			  "standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n" ) <<
		"uniformcolor_painter\n{\n\tname pnt_glass\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"perfectrefractor_material\n{\n\tname mat_glass\n\trefractance pnt_glass\n\tior 1.5\n}\n\n";

	switch( glass ) {
	case kSingleSheet:
		ss << PlaneChunk( "geo_sheet", 2.0, true )
		   << "standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_glass\n}\n\n";
		break;
	case kSingleSheetDown:
		ss << PlaneChunk( "geo_sheet", 2.0, false )
		   << "standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_glass\n}\n\n";
		break;
	case kMeshSingleOneSided:
		ss << MeshChunk( "geo_sheet", 2.0, true, false )
		   << "standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_glass\n}\n\n";
		break;
	case kMeshSingle:
	case kMeshSingleDown:
		ss << MeshChunk( "geo_sheet", 2.0, glass == kMeshSingle )
		   << "standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_glass\n}\n\n";
		break;
	case kMeshSlab:
		ss << MeshChunk( "geo_top", 2.1, true ) << MeshChunk( "geo_bot", 1.9, false )
		   << "standard_object\n{\n\tname obj_top\n\tgeometry geo_top\n\tmaterial mat_glass\n}\n\n"
		      "standard_object\n{\n\tname obj_bot\n\tgeometry geo_bot\n\tmaterial mat_glass\n}\n\n";
		break;
	case kCurvedOpenSlab:
		ss << CurvedSheetChunk( "geo_top", 2.1, true, kCurvedK ) << CurvedSheetChunk( "geo_bot", 1.9, false, kCurvedK )
		   << "standard_object\n{\n\tname obj_top\n\tgeometry geo_top\n\tmaterial mat_glass\n}\n\n"
		      "standard_object\n{\n\tname obj_bot\n\tgeometry geo_bot\n\tmaterial mat_glass\n}\n\n";
		break;
	case kCurvedClosedSlab:
		ss << CurvedClosedSlabChunk( "geo_box", 2.1, 1.9, kCurvedK )
		   << "standard_object\n{\n\tname obj_box\n\tgeometry geo_box\n\tmaterial mat_glass\n}\n\n";
		break;
	case kOpenSlab:
		ss << PlaneChunk( "geo_top", 2.1, true ) << PlaneChunk( "geo_bot", 1.9, false )
		   << "standard_object\n{\n\tname obj_top\n\tgeometry geo_top\n\tmaterial mat_glass\n}\n\n"
		      "standard_object\n{\n\tname obj_bot\n\tgeometry geo_bot\n\tmaterial mat_glass\n}\n\n";
		break;
	case kDisplacedSlab:
		// The shipped sms_k1_refract / sms_k2_flatslab construction: a
		// unit plane in object space (facing +z), displaced (here by 0),
		// oriented by the standard_object.
		ss << "uniformcolor_painter\n{\n\tname pnt_zero\n\tcolor 0 0 0\n}\n\n"
		      "clippedplane_geometry\n{\n\tname geo_base\n\tpta -4 -4 0\n\tptb 4 -4 0\n\tptc 4 4 0\n\tptd -4 4 0\n}\n\n"
		      "displaced_geometry\n{\n\tname geo_disp\n\tbase_geometry geo_base\n\tdetail 8\n\tdisplacement pnt_zero\n\tdisp_scale 0.0\n\tbsp TRUE\n}\n\n"
		      "standard_object\n{\n\tname obj_top\n\tgeometry geo_disp\n\torientation -90 0 0\n\tposition 0 2.1 0\n\tmaterial mat_glass\n}\n\n"
		      "standard_object\n{\n\tname obj_bot\n\tgeometry geo_disp\n\torientation 90 0 0\n\tposition 0 1.9 0\n\tmaterial mat_glass\n}\n\n";
		break;
	case kOpenStacked:
		// Two open-sheet slabs with an AIR gap between them (DL-345's T7:
		// the gap used to read as glass).  Camera above sees through both.
		ss << PlaneChunk( "geo_t1", 2.5, true ) << PlaneChunk( "geo_b1", 2.3, false )
		   << PlaneChunk( "geo_t2", 1.7, true ) << PlaneChunk( "geo_b2", 1.5, false );
		for( const char* g : { "geo_t1", "geo_b1", "geo_t2", "geo_b2" } ) {
			ss << "standard_object\n{\n\tname obj_" << g << "\n\tgeometry " << g << "\n\tmaterial mat_glass\n}\n\n";
		}
		break;
	case kClosedStacked:
		ss << "box_geometry\n{\n\tname geo_box\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
		      "standard_object\n{\n\tname obj_box1\n\tgeometry geo_box\n\tposition 0 2.4 0\n\tmaterial mat_glass\n}\n\n"
		      "standard_object\n{\n\tname obj_box2\n\tgeometry geo_box\n\tposition 0 1.6 0\n\tmaterial mat_glass\n}\n\n";
		break;
	default:
		ss << "box_geometry\n{\n\tname geo_box\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
		      "standard_object\n{\n\tname obj_box\n\tgeometry geo_box\n\tposition 0 2 0\n\tmaterial mat_glass\n}\n\n";
		break;
	}
	return ss.str();
}

static std::string Assemble( Integrator which, Glass glass, Camera cam, bool omni = false, unsigned int sppOverride = 0 )
{
	return std::string( "RISE ASCII SCENE 7\n" )
		+ "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ Rasterizer( which, sppOverride ) + Scene( glass, cam, omni );
}

struct Stat { double mean, sd; };

static Stat Measure( Integrator which, Glass glass, Camera cam, unsigned int n, bool omni = false, unsigned int sppOverride = 0 )
{
	std::vector<double> v;
	for( unsigned int i = 0; i < n; i++ ) {
		v.push_back( Render( Assemble( which, glass, cam, omni, sppOverride ) ) );
	}
	double m = 0;
	for( double x : v ) m += x;
	m /= double( n );
	double s = 0;
	for( double x : v ) s += ( x - m ) * ( x - m );
	s = n > 1 ? std::sqrt( s / double( n - 1 ) ) : 0.0;
	return Stat{ m, s };
}

//! a / b and its sd from the two means' sd (independent).
static void RatioRow( const char* label, const Stat& a, const Stat& b, double tol )
{
	const double r = a.mean / b.mean;
	const double sdr = r * std::sqrt( ( a.sd / a.mean ) * ( a.sd / a.mean ) + ( b.sd / b.mean ) * ( b.sd / b.mean ) );
	char buf[320];
	std::snprintf( buf, sizeof(buf), "%s: %.5f / %.5f = %.4f +/- %.4f (band %.3f)", label, a.mean, b.mean, r, sdr, tol );
	std::cout << "  " << buf << std::endl;
	Check( a.mean > 0 && b.mean > 0 && std::fabs( r - 1.0 ) <= tol, buf );
}

int main( int argc, char** argv )
{
	// One worker: a multithreaded render seeds workers from libc rand() in
	// thread-start order and is not reproducible run to run.
	if( !std::getenv( "RISE_OPTIONS_FILE" ) ) {
		std::snprintf( g_optPath, sizeof(g_optPath), "%s/open_sheet_options_%d.txt", RISE::TestSupport::ScratchBaseDir().c_str(), static_cast<int>( ::getpid() ) );
		std::atexit( []() { std::remove( g_optPath ); } );
		std::ofstream opt( g_optPath );
		opt << "force_number_of_threads 1\n";
		opt.close();
#ifdef _WIN32
		_putenv_s( "RISE_OPTIONS_FILE", g_optPath );
#else
		setenv( "RISE_OPTIONS_FILE", g_optPath, 1 );
#endif
	}
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}
	unsigned int n = 2;
	if( const char* s = std::getenv( "OPEN_SHEET_N" ) ) {
		const long v = std::strtol( s, nullptr, 10 );
		if( v > 0 ) n = (unsigned int)v;
	}
	std::cout << "OpenSheetIndexConventionTest (DL-345 / DL-339 (b))   seed base = " << g_seedBase << "   n = " << n << std::endl;

	const char* camName[2] = { "camera below (direct)", "camera above (through slab)" };
	const bool meshOnly = std::getenv( "OPEN_SHEET_MESH_ONLY" ) != nullptr;
	Stat single[kNumIntegrators] = {};
	bool hasSingle[kNumIntegrators] = {};
	Stat closedSlab[2][kNumIntegrators] = {};
	bool hasClosedSlab[2][kNumIntegrators] = {};
	auto getSingle = [&]( int i ) -> const Stat& {
		if( !hasSingle[i] ) {
			single[i] = Measure( Integrator( i ), kSingleSheet, kBelow, n );
			hasSingle[i] = true;
		}
		return single[i];
	};
	auto getClosedSlab = [&]( int c, int i ) -> const Stat& {
		if( !hasClosedSlab[c][i] ) {
			closedSlab[c][i] = Measure( Integrator( i ), kClosedSlab, Camera( c ), n );
			hasClosedSlab[c][i] = true;
		}
		return closedSlab[c][i];
	};

	if( !meshOnly ) {
	// single: every integrator against PT.
	std::cout << "single open sheet, receiver seen directly (DL-339 (b))" << std::endl;
	for( int i = 0; i < kNumIntegrators; i++ ) {
		getSingle( i );
	}
	for( int i = 1; i < kNumIntegrators; i++ ) {
		std::string label = std::string( "single sheet " ) + kIntegratorName[i] + " / PT";
		RatioRow( label.c_str(), single[i], single[kPT], 0.06 );
	}

	// slab: open (flat, displaced) / closed, per integrator and camera.
	std::cout << "slab, " << camName[kBelow] << std::endl;
	for( int i = 0; i < kNumIntegrators; i++ ) {
		const Stat& closed = getClosedSlab( kBelow, i );
		const Stat open = Measure( Integrator( i ), kOpenSlab, kBelow, n );
		std::string l1 = std::string( "open slab / closed box " ) + kIntegratorName[i] + ", " + camName[kBelow];
		RatioRow( l1.c_str(), open, closed, 0.06 );
		if( i == kPT || i == kPTSMS ) {
			const Stat disp = Measure( Integrator( i ), kDisplacedSlab, kBelow, n );
			std::string l2 = std::string( "displaced open slab / closed box " ) + kIntegratorName[i] + ", " + camName[kBelow];
			RatioRow( l2.c_str(), disp, closed, 0.06 );
		}
	}
	std::cout << "slab, " << camName[kAbove] << std::endl;
	for( Integrator i : { kPT, kBDPT } ) {
		const Stat& closed = getClosedSlab( kAbove, i );
		const Stat open = Measure( i, kOpenSlab, kAbove, n );
		std::string l1 = std::string( "open slab / closed box " ) + kIntegratorName[i] + ", " + camName[kAbove];
		RatioRow( l1.c_str(), open, closed, 0.06 );
	}

	// stacked: two slabs with an air gap, camera above, through both.
	// VCM through four refracting interfaces on a 12x12 film has higher
	// per-render variance than PT, so give it 256 spp and n >= 4 (band 0.12
	// vs the 2x-3x pre-DL-345 defect).
	std::cout << "two stacked slabs, camera above (through both)" << std::endl;
	for( Integrator i : { kPT, kVCM } ) {
		const unsigned int nRow = ( i == kVCM && n < 4u ) ? 4u : n;
		const unsigned int sppRow = ( i == kVCM ) ? 256u : 0u;
		const double tol = ( i == kVCM ) ? 0.12 : 0.06;
		const Stat closed = Measure( i, kClosedStacked, kAbove, nRow, false, sppRow );
		const Stat open = Measure( i, kOpenStacked, kAbove, nRow, false, sppRow );
		std::string l = std::string( "stacked open slabs / closed boxes " ) + kIntegratorName[i];
		RatioRow( l.c_str(), open, closed, tol );
	}

	// tshadow: the opt-in transparent-shadow walk through an open slab.
	std::cout << "transparent shadows, omni light, camera below" << std::endl;
	{
		const Stat closed = Measure( kPTTransparentShadows, kClosedSlab, kBelow, n, true );
		const Stat open = Measure( kPTTransparentShadows, kOpenSlab, kBelow, n, true );
		const Stat disp = Measure( kPTTransparentShadows, kDisplacedSlab, kBelow, n, true );
		RatioRow( "transparent shadows open slab / closed box", open, closed, 0.02 );
		RatioRow( "transparent shadows displaced open slab / closed box", disp, closed, 0.02 );
	}
	}

	// DL-382: open sheets built as planar indexed meshes (up-wound across all
	// integrators, plus down-wound PT winding-flip parity).
	std::cout << "DL-382: open sheets built as indexed meshes" << std::endl;
	{
		Stat mesh[kNumIntegrators];
		for( int i = 0; i < kNumIntegrators; i++ ) {
			mesh[i] = Measure( Integrator( i ), kMeshSingle, kBelow, n );
		}
		RatioRow( "single mesh sheet (up-wound) PT / clipped plane PT", mesh[kPT], getSingle( kPT ), 0.06 );
		for( int i = 1; i < kNumIntegrators; i++ ) {
			std::string l = std::string( "single mesh sheet (up-wound) " ) + kIntegratorName[i] + " / PT";
			RatioRow( l.c_str(), mesh[i], mesh[kPT], 0.06 );
		}
		const Stat meshDown = Measure( kPT, kMeshSingleDown, kBelow, n );
		const Stat planeDown = Measure( kPT, kSingleSheetDown, kBelow, n );
		RatioRow( "single mesh sheet (down-wound) PT / clipped plane PT", meshDown, planeDown, 0.06 );
	}
	// Single-sided planar mesh vs cached double-sided clipped plane.
	std::cout << "single-sided planar mesh vs double-sided clipped plane" << std::endl;
	for( Integrator i : { kPT, kBDPT } ) {
		const Stat& plane = getSingle( i );
		const Stat mesh = Measure( i, kMeshSingleOneSided, kBelow, n );
		std::string l = std::string( "single-sided mesh sheet / double-sided clipped plane " ) + kIntegratorName[i];
		RatioRow( l.c_str(), mesh, plane, 0.06 );
	}
	for( int c = 0; c < 2; c++ ) {
		std::cout << "mesh slab, " << camName[c] << std::endl;
		for( Integrator i : { kPT, kBDPT } ) {
			const Stat& closed = getClosedSlab( c, i );
			const Stat open = Measure( i, kMeshSlab, Camera( c ), n );
			std::string l1 = std::string( "two-mesh open slab / closed box " ) + kIntegratorName[i] + ", " + camName[c];
			RatioRow( l1.c_str(), open, closed, 0.06 );
		}
	}
	// DL-382 curved (parabolic K=0.01) open indexed-mesh glass slab, PT vs BDPT.
	std::cout << "DL-382: curved open indexed-mesh slab, PT vs BDPT" << std::endl;
	{
		const Stat closedPT = Measure( kPT, kCurvedClosedSlab, kBelow, n );
		const Stat openPT = Measure( kPT, kCurvedOpenSlab, kBelow, n );
		const Stat closedBDPT = Measure( kBDPT, kCurvedClosedSlab, kBelow, n );
		const Stat openBDPT = Measure( kBDPT, kCurvedOpenSlab, kBelow, n );
		RatioRow( "curved open slab / closed slab PT", openPT, closedPT, 0.06 );
		RatioRow( "curved open slab / closed slab BDPT", openBDPT, closedBDPT, 0.06 );
		RatioRow( "curved open slab BDPT / PT", openBDPT, openPT, 0.06 );
	}

	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
