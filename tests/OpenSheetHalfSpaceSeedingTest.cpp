//////////////////////////////////////////////////////////////////////
//
//  OpenSheetHalfSpaceSeedingTest.cpp - DL-618 / DL-619 review fixes.
//
//  DL-618: a certified OPEN refractive sheet's medium (its back side,
//  DL-345) holds a seed point iff the ray from the point along the
//  sheet's OWN normal reaches the sheet's back face with nothing but
//  other media in between (IORStackSeeding.h, the convention above
//  `ClassifyMembershipHit`).  The first-pass rule seeded the sheet when
//  ANY of six axis probes met its back face ANYWHERE along an unbounded
//  line, walking through opaque geometry: a camera in a closed opaque
//  room below a glass pane was seeded in glass, the result depended on
//  which axis happened to see the sheet (not rotation invariant), and a
//  point beside a tilted pane was seeded.
//
//  Part A (structural, SeedFromPoint / OpenSheetEnclosesCrossing /
//  SMSDomainReplay::Capture on CST-loaded scenes):
//    A1  opaque room under a pane: NOT seeded (pane-only control: seeded)
//    A2  tilted pane: a point inside its normal prism is seeded, a point
//        beside it whose +Y probe still meets its back face is not
//    A3  rotating pane + points together does not change the answer
//    A4  nested lone sheets of two materials: both, outer first
//    A5  sibling sheets (a wedge): one entry for one material, none
//        (declined) for two materials
//    A6  a closed glass sphere between point and pane is transparent;
//        a point inside it seeds [pane, sphere]
//    A7  probe budget: 1 walk with no sheet (the pre-DL-618 cost), +1
//        per open sheet in the scene
//    A8  OpenSheetEnclosesCrossing: a CLOSED object sharing the sheet's
//        material neither vetoes nor approves; an opaque room around the
//        crossing makes the sheet's medium not continue
//    A9  SMSDomainReplay::Capture declines a stale CLOSED key no axis
//        ray reaches, keeps an open-sheet key off the axes
//
//  Part B (renders, single worker, Sobol'-salted repeats):
//    B1  camera under a lone glass pane, a glass sphere below it: PT,
//        BDPT and VCM agree, and PT matches the same scene with the pane
//        replaced by a closed glass box (the half-space it models)
//    B2  opaque room under a pane: the image equals the room without the
//        pane, per integrator
//    B3  DL-619 fixture (closed 2.2 box sliced by a down-facing 1.5
//        sheet): PT+SMS == PT
//
//  argv[1]: seed base (default 2000).  HALF_SPACE_N (env): repeats
//  (default 4).  HALF_SPACE_UNIT_ONLY (env): Part A only.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

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

static unsigned int g_seedBase = 2000;
static unsigned int g_renderIndex = 0;
static unsigned int g_sceneIndex = 0;
static char g_optPath[256];

static std::string TempPath( const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/half_space_%s_%d_%u.RISEscene", tag, static_cast<int>( ::getpid() ), g_sceneIndex++ );
	return path;
}

//////////////////////////////////////////////////////////////////////
// Scene pieces (y up).
//////////////////////////////////////////////////////////////////////
static std::string HeaderBody()
{
	return
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"lambertian_material\n{\n\tname matte\n\treflectance grey\n}\n\n"
		"lambertian_material\n{\n\tname soot\n\treflectance black\n}\n\n"
		"perfectrefractor_material\n{\n\tname glass15\n\trefractance white\n\tior 1.5\n}\n\n"
		"perfectrefractor_material\n{\n\tname glass18\n\trefractance white\n\tior 1.8\n}\n\n"
		"perfectrefractor_material\n{\n\tname glass133\n\trefractance white\n\tior 1.33\n}\n\n"
		"perfectrefractor_material\n{\n\tname glass20\n\trefractance white\n\tior 2.0\n}\n\n";
}

static std::string Header()
{
	return "RISE ASCII SCENE 7\n" + HeaderBody();
}

//! A clipped plane with corners a, b, c, d (normal (b-a) x (c-a)).
static std::string SheetQuad( const char* name, const double a[3], const double b[3], const double c[3], const double d[3] )
{
	std::ostringstream ss;
	ss << "clippedplane_geometry\n{\n\tname " << name << "\n"
	   << "\tpta " << a[0] << " " << a[1] << " " << a[2] << "\n"
	   << "\tptb " << b[0] << " " << b[1] << " " << b[2] << "\n"
	   << "\tptc " << c[0] << " " << c[1] << " " << c[2] << "\n"
	   << "\tptd " << d[0] << " " << d[1] << " " << d[2] << "\n"
	   << "\tdoublesided TRUE\n}\n\n";
	return ss.str();
}

//! Horizontal half-extent `h` sheet at height `y`; `up` = normal +y.
static std::string HSheet( const char* name, double y, double h, bool up )
{
	const double a[3] = { -h, y, up ? h : -h }, b[3] = { h, y, up ? h : -h };
	const double c[3] = { h, y, up ? -h : h }, d[3] = { -h, y, up ? -h : h };
	return SheetQuad( name, a, b, c, d );
}

//! Point (x, y) rotated by `deg` about z.
static void RotZ( double deg, double& x, double& y )
{
	const double t = deg * 3.14159265358979323846 / 180.0;
	const double nx = x * std::cos( t ) - y * std::sin( t );
	const double ny = x * std::sin( t ) + y * std::cos( t );
	x = nx; y = ny;
}

//! An up-facing horizontal sheet of half-extent `h` at height `y`,
//! rotated by `deg` about the z axis through the origin.
static std::string RotatedSheet( const char* name, double y, double h, double deg )
{
	double p[4][3] = { { -h, y, h }, { h, y, h }, { h, y, -h }, { -h, y, -h } };
	for( auto& q : p ) RotZ( deg, q[0], q[1] );
	return SheetQuad( name, p[0], p[1], p[2], p[3] );
}

static std::string Obj( const char* name, const char* geo, const char* mat, const char* position = 0 )
{
	std::string s = std::string( "standard_object\n{\n\tname " ) + name + "\n\tgeometry " + geo + "\n";
	if( position ) s += std::string( "\tposition " ) + position + "\n";
	return s + "\tmaterial " + mat + "\n}\n\n";
}

struct Loaded
{
	IJobPriv* job;
	bool ok;
	std::string path;
	explicit Loaded( const std::string& text ) : job( 0 ), ok( false )
	{
		path = TempPath( "unit" );
		{
			std::ofstream ofs( path.c_str() );
			ofs << text;
		}
		ok = RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( path.c_str() );
		if( ok ) job->GetScene()->GetObjects()->PrepareForRendering();
	}
	~Loaded() { safe_release( job ); std::remove( path.c_str() ); }
	const IScene& Scene() const { return *job->GetScene(); }
	const IObject* O( const char* name ) const { return job->GetScene()->GetObjects()->GetItem( name ); }
	IORStack Seed( const Point3& p ) const
	{
		IORStack s( 1.0 );
		IORStackSeeding::SeedFromPoint( s, p, Scene() );
		return s;
	}
};

static bool Close( double a, double b ) { return std::fabs( a - b ) < 1e-12; }

//////////////////////////////////////////////////////////////////////
// Part A
//////////////////////////////////////////////////////////////////////
static void PartA()
{
	std::cout << "Part A: sheet-normal membership convention" << std::endl;

	// A1: a closed opaque room (y -1 .. 3) below an up-facing glass pane
	// at y 10.  The camera inside the room cannot see the pane.
	{
		Loaded room( Header() + HSheet( "g_pane", 10, 3, true ) + Obj( "pane", "g_pane", "glass15" )
			+ "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 4\n\tdepth 4\n}\n\n"
			+ Obj( "room", "g_room", "matte", "0 1 0" ) );
		Loaded open( Header() + HSheet( "g_pane", 10, 3, true ) + Obj( "pane", "g_pane", "glass15" ) );
		Check( room.ok && open.ok, "A1 scenes load" );
		if( room.ok && open.ok ) {
			const IORStack inRoom = room.Seed( Point3( 0, 0, 0 ) );
			Check( inRoom.Depth() == 1 && Close( inRoom.top(), 1.0 ),
				"A1: a point in a closed opaque room below a glass pane is NOT seeded into the pane's medium" );
			const IORStack free = open.Seed( Point3( 0, 0, 0 ) );
			Check( free.Depth() == 2 && free.topObject() == open.O( "pane" ) && free.topIsOpenSheet() && Close( free.top(), 1.5 ),
				"A1 control: the same point with no room is seeded 1.5" );
		}
	}

	// A2: an up-facing pane (half-extent 1) at height 2, tilted -40 deg
	// about z (normal (sin 40, cos 40, 0)).  (-0.5, 0, 0) projects inside
	// it along its normal; (0.5, 0, 0) projects outside (in-plane
	// coordinate 1.67) although its +Y probe meets the pane's back face.
	{
		double cx = 0, cy = 2;
		std::string text = Header();
		{
			double p[4][3] = { { -1, 0, 1 }, { 1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 } };
			for( auto& q : p ) { RotZ( -40, q[0], q[1] ); q[0] += cx; q[1] += cy; }
			text += SheetQuad( "g_tilt", p[0], p[1], p[2], p[3] );
		}
		Loaded tilt( text + Obj( "tilt", "g_tilt", "glass15" ) );
		Check( tilt.ok, "A2 scene loads" );
		if( tilt.ok ) {
			const IORStack in = tilt.Seed( Point3( -0.5, 0, 0 ) );
			const IORStack out = tilt.Seed( Point3( 0.5, 0, 0 ) );
			Check( in.Depth() == 2 && Close( in.top(), 1.5 ), "A2: a point inside a tilted pane's normal prism is seeded" );
			Check( out.Depth() == 1, "A2: a point outside the prism is not seeded even though its +Y probe meets the back face" );
		}
	}

	// A3: rotation invariance.  The same pane (half-extent 3 at height 2)
	// + points, unrotated and rotated -40 deg about z together.  Point 3
	// lies outside the prism; rotated, its +Y probe meets the pane's back
	// face (the first-pass any-axis rule seeded it there and not
	// unrotated).
	{
		Loaded flat( Header() + RotatedSheet( "g_s", 2, 3, 0 ) + Obj( "s", "g_s", "glass15" ) );
		Loaded rot( Header() + RotatedSheet( "g_s", 2, 3, -40 ) + Obj( "s", "g_s", "glass15" ) );
		Check( flat.ok && rot.ok, "A3 scenes load" );
		if( flat.ok && rot.ok ) {
			const double pts[3][2] = { { 0.2, 0.0 }, { 2.5, 0.5 }, { 3.4, 0.0 } };
			const bool expectIn[3] = { true, true, false };
			for( int i = 0; i < 3; i++ ) {
				double x = pts[i][0], y = pts[i][1];
				const bool a = flat.Seed( Point3( x, y, 0.1 ) ).Depth() == 2;
				RotZ( -40, x, y );
				const bool b = rot.Seed( Point3( x, y, 0.1 ) ).Depth() == 2;
				char label[160];
				std::snprintf( label, sizeof(label), "A3: point %d seeded %d unrotated, %d rotated, expected %d",
					i, int( a ), int( b ), int( expectIn[i] ) );
				Check( a == expectIn[i] && b == expectIn[i], label );
			}
		}
		// A small pane (half-extent 1 at height 2, tilted 40 deg) holds
		// (0.2, 0) in its prism, but no axis probe from that point meets it
		// (each misses the pane by up to sqrt(2) x the distance to its
		// plane): per-seed axis DISCOVERY never proposed it and the answer
		// depended on the scene's orientation.  The catalog lists every
		// sheet, so it is seeded in both frames.
		Loaded smallFlat( Header() + RotatedSheet( "g_s", 2, 1, 0 ) + Obj( "s", "g_s", "glass15" ) );
		Loaded smallRot( Header() + RotatedSheet( "g_s", 2, 1, -40 ) + Obj( "s", "g_s", "glass15" ) );
		if( smallFlat.ok && smallRot.ok ) {
			double x = 0.2, y = 0.0;
			const bool a = smallFlat.Seed( Point3( x, y, 0.1 ) ).Depth() == 2;
			RotZ( -40, x, y );
			const bool b = smallRot.Seed( Point3( x, y, 0.1 ) ).Depth() == 2;
			Check( a && b, "A3: a small tilted pane off every probe axis is still seeded inside its prism" );
		}
	}

	// A4: nested lone sheets of two materials, both up-facing above the
	// point: the outer (higher) one first, the inner on top.
	{
		Loaded nest( Header() + HSheet( "g_hi", 4, 5, true ) + HSheet( "g_lo", 2, 5, true )
			+ Obj( "hi", "g_hi", "glass133" ) + Obj( "lo", "g_lo", "glass15" ) );
		Check( nest.ok, "A4 scene loads" );
		if( nest.ok ) {
			const IORStack s = nest.Seed( Point3( 0, 0, 0 ) );
			Check( s.Depth() == 3 && s.ObjectAt( 1 ) == nest.O( "hi" ) && s.topObject() == nest.O( "lo" ) && Close( s.top(), 1.5 ),
				"A4: nested lone sheets seed [outer 1.33, inner 1.5]" );
			const IORStack between = nest.Seed( Point3( 0, 3, 0 ) );
			Check( between.Depth() == 2 && between.topObject() == nest.O( "hi" ),
				"A4: a point between them is in the outer sheet only" );
		}
	}

	// A5: siblings -- an up-facing sheet above and a vertical sheet
	// facing +x beside the point; neither is on the other's normal ray.
	{
		const double a[3] = { 2, -3, -3 }, b[3] = { 2, 3, -3 }, c[3] = { 2, 3, 3 }, d[3] = { 2, -3, 3 };
		const std::string wall = SheetQuad( "g_wall", a, b, c, d );
		Loaded same( Header() + HSheet( "g_roof", 2, 3, true ) + wall
			+ Obj( "roof", "g_roof", "glass15" ) + Obj( "wall", "g_wall", "glass15" ) );
		Loaded diff( Header() + HSheet( "g_roof", 2, 3, true ) + wall
			+ Obj( "roof", "g_roof", "glass15" ) + Obj( "wall", "g_wall", "glass18" ) );
		Check( same.ok && diff.ok, "A5 scenes load" );
		if( same.ok && diff.ok ) {
			const IORStack s = same.Seed( Point3( 0, 0, 0 ) );
			Check( s.Depth() == 2 && Close( s.top(), 1.5 ), "A5: same-material sibling sheets seed ONE entry" );
			const IORStack t = diff.Seed( Point3( 0, 0, 0 ) );
			Check( t.Depth() == 1, "A5: different-material sibling sheets (walk-history-dependent interior) decline" );
		}
	}

	// A6: a closed glass sphere between the point and the pane is a medium
	// inside the pane's half-space, not a boundary of it.
	{
		Loaded sph( Header() + HSheet( "g_pane", 3, 3, true ) + Obj( "pane", "g_pane", "glass15" )
			+ "sphere_geometry\n{\n\tname g_ball\n\tradius 0.5\n}\n\n" + Obj( "ball", "g_ball", "glass20", "0 1 0" ) );
		Check( sph.ok, "A6 scene loads" );
		if( sph.ok ) {
			const IORStack below = sph.Seed( Point3( 0, 0, 0 ) );
			Check( below.Depth() == 2 && below.topObject() == sph.O( "pane" ),
				"A6: a glass sphere between the point and the pane does not block membership" );
			const IORStack inside = sph.Seed( Point3( 0.1, 1, 0.05 ) );
			Check( inside.Depth() == 3 && inside.ObjectAt( 1 ) == sph.O( "pane" ) && inside.topObject() == sph.O( "ball" ) && Close( inside.top(), 2.0 ),
				"A6: a point inside the sphere seeds [pane, sphere]" );
		}
	}

	// A7: probe budget (DL-618 review P2-1).
	{
		Loaded none( Header() + HSheet( "g_floor", -1, 3, true ) + Obj( "floor", "g_floor", "matte" ) );
		Loaded one( Header() + HSheet( "g_pane", 2, 3, true ) + Obj( "pane", "g_pane", "glass15" ) );
		Check( none.ok && one.ok, "A7 scenes load" );
		if( none.ok && one.ok ) {
			IORStackSeeding::DebugResetProbeTraceCount();
			none.Seed( Point3( 0, 0, 0 ) );
			const std::size_t c0 = IORStackSeeding::DebugGetProbeTraceCount();
			IORStackSeeding::DebugResetProbeTraceCount();
			one.Seed( Point3( 0, 0, 0 ) );
			const std::size_t c1 = IORStackSeeding::DebugGetProbeTraceCount();
			char label[160];
			std::snprintf( label, sizeof(label), "A7: probe walks: %zu with no sheet (want 1, the pre-DL-618 cost), %zu with one open sheet (want 2)", c0, c1 );
			Check( c0 == 1 && c1 == 2, label );
		}
	}

	// A8: OpenSheetEnclosesCrossing (DL-618 review P2-2).  Y = an
	// up-facing glass pane at y 2; the walk leaves a vertical sheet of
	// another material at the origin travelling +x.
	{
		const double a[3] = { 0, -1, -1 }, b[3] = { 0, -1, 1 }, c[3] = { 0, 1, 1 }, d[3] = { 0, 1, -1 };
		const std::string vert = SheetQuad( "g_v", a, b, c, d );
		Loaded sharing( Header() + HSheet( "g_pane", 2, 6, true ) + Obj( "pane", "g_pane", "glass15" ) + vert + Obj( "v", "g_v", "glass18" )
			+ "sphere_geometry\n{\n\tname g_ball\n\tradius 0.5\n}\n\n" + Obj( "ball", "g_ball", "glass15", "3 0 0" ) );
		Loaded roomed( Header() + HSheet( "g_pane", 8, 6, true ) + Obj( "pane", "g_pane", "glass15" ) + vert + Obj( "v", "g_v", "glass18" )
			+ "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 4\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte" ) );
		Check( sharing.ok && roomed.ok, "A8 scenes load" );
		if( sharing.ok && roomed.ok ) {
			Check( IORStackSeeding::OpenSheetEnclosesCrossing( sharing.O( "pane" ), sharing.O( "v" ), &sharing.Scene(),
					Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) ),
				"A8: a closed sphere of the pane's material ahead of the walk does not veto the pane's medium" );
			Check( !IORStackSeeding::OpenSheetEnclosesCrossing( roomed.O( "pane" ), roomed.O( "v" ), &roomed.Scene(),
					Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) ),
				"A8: inside a closed opaque room the pane's medium does not continue" );
		}
	}

	// A9: SMSDomainReplay::Capture's bounds-centre fallback (DL-618 review
	// P3-b) applies to open-sheet keys only.
	{
		Loaded cap( Header() + "sphere_geometry\n{\n\tname g_ball\n\tradius 0.5\n}\n\n" + Obj( "ball", "g_ball", "glass15", "5 5 5" )
			+ HSheet( "g_card", 0, 0.5, true ) + "standard_object\n{\n\tname card\n\tgeometry g_card\n\tposition 5 5 5\n\tmaterial glass15\n}\n\n" );
		Check( cap.ok, "A9 scene loads" );
		if( cap.ok ) {
			IORStack staleClosed( 1.0 );
			staleClosed.SetCurrentObject( cap.O( "ball" ) );
			staleClosed.push( 1.5 );
			SMSStartingMedia media;
			Check( !SMSDomainReplay::Capture( cap.Scene(), Point3( 0, 0, 0 ), staleClosed, media ),
				"A9: a CLOSED key no axis ray from the anchor reaches is stale membership and declines" );
			IORStack sheetKey( 1.0 );
			sheetKey.SetCurrentObject( cap.O( "card" ) );
			sheetKey.pushOpenSheet( 1.5 );
			SMSStartingMedia sheetMedia;
			Check( SMSDomainReplay::Capture( cap.Scene(), Point3( 0, 0, 0 ), sheetKey, sheetMedia ) && sheetMedia.enclosing.size() == 1,
				"A9: an OPEN-SHEET key off every axis is captured toward its bounds" );
		}
	}

	// A10 (round 2, P2-1): a displaced (Gerstner) sea surface.  Its
	// representative normal must be the surface's, not a flank's, and every
	// submerged point must be seeded.
	{
		const std::string sea =
			"uniformcolor_painter\n{\n\tname pnt_lo\n\tcolor 0 0 0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_hi\n\tcolor 1 1 1\n}\n\n"
			"gerstnerwave_painter\n{\n\tname pnt_waves\n\tcolora pnt_lo\n\tcolorb pnt_hi\n\tnum_waves 6\n\tmedian_wavelength 1.0\n"
			"\twavelength_range 2.0\n\tmedian_amplitude 0.3\n\tamplitude_power 0.8\n\twind_dir 1.0 0.35\n\tdirectional_spread 1.6\n"
			"\tdispersion_speed 1.0\n\tseed 9\n\ttime 0.0\n}\n\n"
			"clippedplane_geometry\n{\n\tname g_base\n\tpta -6 0 -6\n\tptb -6 0 6\n\tptc 6 0 6\n\tptd 6 0 -6\n}\n\n"
			"displaced_geometry\n{\n\tname g_sea\n\tbase_geometry g_base\n\tdetail 64\n\tdisplacement pnt_waves\n\tdisp_scale 0.6\n\tuv_seam_fold FALSE\n}\n\n";
		Loaded wavy( Header() + sea + Obj( "sea", "g_sea", "glass133", "0 3 0" ) );
		Check( wavy.ok, "A10 scene loads" );
		if( wavy.ok ) {
			IORStackSeeding::OpenSheetCatalogEntry entry;
			const bool found = IORStackSeeding::FindOpenSheet( wavy.Scene().GetObjects(), wavy.O( "sea" ), entry );
			char label[200];
			std::snprintf( label, sizeof(label), "A10: the displaced sea is catalogued with a near-vertical normal (%.4f, %.4f, %.4f)",
				found ? entry.normal.x : -2.0, found ? entry.normal.y : -2.0, found ? entry.normal.z : -2.0 );
			if( std::getenv( "HALF_SPACE_DEBUG" ) ) std::printf( "  debug: %s\n", label );
			Check( found && entry.normal.y > 0.95, label );
			{
				const BoundingBox b = wavy.O( "sea" )->getBoundingBox();
				const Point3 c = b.GetCenter();
				RayIntersection xr( Ray( Point3( c.x - 20, c.y, c.z ), Vector3( 1, 0, 0 ) ), nullRasterizerState );
				wavy.O( "sea" )->IntersectRay( xr, RISE_INFINITY, true, true, false );
				std::printf( "  [recorded] first X-ray hit normal of the sea (the round-1 catalog normal): (%.3f, %.3f, %.3f)\n",
					xr.geometric.bHit ? xr.geometric.UnflippedGeomNormal().x : 0.0,
					xr.geometric.bHit ? xr.geometric.UnflippedGeomNormal().y : 0.0,
					xr.geometric.bHit ? xr.geometric.UnflippedGeomNormal().z : 0.0 );
			}
			int seeded = 0, total = 0;
			// Includes points near the sheet's edges, where a normal tilted
			// toward a flank leaves the sheet before reaching it.
			for( int ix = -4; ix <= 4; ix += 2 ) {
				for( int iz = -4; iz <= 4; iz += 2 ) {
					total++;
					const IORStack st = wavy.Seed( Point3( 0.7 * ix, 1.0, 0.7 * iz ) );
					if( st.Depth() == 2 && Close( st.top(), 1.33 ) ) seeded++;
					else if( std::getenv( "HALF_SPACE_DEBUG" ) ) std::printf( "  debug: A10 miss at (%g, %g)\n", 0.7 * ix, 0.7 * iz );
				}
			}
			std::snprintf( label, sizeof(label), "A10: %d of %d submerged points under the displaced sea are seeded", seeded, total );
			Check( seeded == total, label );
		}
	}

	// A10b (round 2, P2-1): a curved open mesh -- a symmetric tent (ridge
	// along z at x = 0, height 1.5 over a half-width of 4).  An X ray toward
	// the centre meets a flank (normal tilted ~21 deg toward -x); the
	// sheet's large-scale normal is vertical.  Points under the far -x side,
	// 3 units down, leave the sheet along the flank normal.
	{
		const std::string tent =
			"indexedmesh_geometry\n{\n\tname g_tent\n"
			"\tvertex -4 0 4\n\tvertex 0 1.5 4\n\tvertex 4 0 4\n\tvertex -4 0 -4\n\tvertex 0 1.5 -4\n\tvertex 4 0 -4\n"
			"\ttriangle 0 1 4\n\ttriangle 0 4 3\n\ttriangle 1 2 5\n\ttriangle 1 5 4\n\tdouble_sided TRUE\n}\n\n";
		Loaded tentScene( Header() + tent + Obj( "tent", "g_tent", "glass15", "0 3 0" ) );
		Check( tentScene.ok, "A10b scene loads" );
		if( tentScene.ok ) {
			int seeded = 0, total = 0;
			for( int iz = -2; iz <= 2; iz++ ) {
				for( double x : { -3.6, -3.2, 3.2, 3.6 } ) {
					total++;
					if( tentScene.Seed( Point3( x, 0.0, 1.2 * iz ) ).Depth() == 2 ) seeded++;
				}
			}
			char label[160];
			std::snprintf( label, sizeof(label), "A10b: %d of %d points under a curved (tent) open mesh are seeded", seeded, total );
			Check( seeded == total, label );
		}
	}

	// A11 (round 2, P2-2): a narrow strip (width 0.2) tilted 45 deg; the
	// point lies 1 unit behind its centre along its normal, so no axis ray
	// from the point meets the strip.  The walk's exit resolution
	// (PointInOpenSheetMedium) must agree with SeedFromPoint.
	{
		double p[4][3] = { { -0.1, 0, 3 }, { 0.1, 0, 3 }, { 0.1, 0, -3 }, { -0.1, 0, -3 } };
		for( auto& q : p ) { RotZ( -45, q[0], q[1] ); q[1] += 2; }
		Loaded strip( Header() + SheetQuad( "g_strip", p[0], p[1], p[2], p[3] ) + Obj( "strip", "g_strip", "glass15" ) );
		Check( strip.ok, "A11 scene loads" );
		if( strip.ok ) {
			const double s45 = std::sqrt( 0.5 );
			const Point3 behind( -s45, 2 - s45, 0 );
			Check( strip.Seed( behind ).Depth() == 2, "A11: SeedFromPoint seeds a point behind a narrow tilted strip" );
			Check( IORStackSeeding::PointInOpenSheetMedium( strip.O( "strip" ), strip.Scene().GetObjects(), behind ),
				"A11: PointInOpenSheetMedium agrees with SeedFromPoint (catalog normal, not the nearest axis hit)" );
		}
	}

	// A12 (round 2, P2-3): an opaque sphere luminaire under a sea sheet.  A
	// BDPT light root lies ON the sphere; from its lower half the normal ray
	// meets the sphere's own upper half from inside, which must not block.
	{
		Loaded lamp( Header() + HSheet( "g_sea", 4, 6, true ) + Obj( "sea", "g_sea", "glass133" )
			+ "lambertian_luminaire_material\n{\n\tname m_lamp\n\texitance white\n\tscale 1\n\tmaterial matte\n}\n\n"
			+ "sphere_geometry\n{\n\tname g_lamp\n\tradius 0.5\n}\n\n" + Obj( "lamp", "g_lamp", "m_lamp", "0 1 0" )
			+ "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 2\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte", "6 1 0" ) );
		Check( lamp.ok, "A12 scene loads" );
		if( lamp.ok ) {
			RayIntersection hit( Ray( Point3( 0.1, -2, 0.05 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
			lamp.O( "lamp" )->IntersectRay( hit, RISE_INFINITY, true, true, false );
			Check( hit.geometric.bHit, "A12: probe meets the lamp's lower half" );
			if( hit.geometric.bHit ) {
				const IORStack low = lamp.Seed( hit.geometric.ptIntersection );
				Check( low.Depth() == 2 && Close( low.top(), 1.33 ),
					"A12: a point on the lower half of an opaque luminaire under the sea is seeded" );
			}
			const IORStack inRoom = lamp.Seed( Point3( 6, 1, 0 ) );
			Check( inRoom.Depth() == 1, "A12 control: a point strictly inside a closed opaque box is still blocked" );
		}
	}

	// A13 (round 2, P2-5): the walk is in the sea (Y, up-facing at 5) and
	// leaves an underwater pane of another material through its back face
	// below an opaque rock overhang.  The sea's back face lies beyond the
	// rock: the sea's medium continues.
	{
		Loaded reef( Header() + HSheet( "g_sea", 5, 6, true ) + Obj( "sea", "g_sea", "glass133" )
			+ HSheet( "g_pane", 1, 1, true ) + Obj( "pane", "g_pane", "glass15" )
			+ "box_geometry\n{\n\tname g_rock\n\twidth 2\n\theight 1\n\tdepth 2\n}\n\n" + Obj( "rock", "g_rock", "matte", "0 2.5 0" ) );
		Check( reef.ok, "A13 scene loads" );
		if( reef.ok ) {
			Check( IORStackSeeding::OpenSheetEnclosesCrossing( reef.O( "sea" ), reef.O( "pane" ), &reef.Scene(),
					Point3( 0.2, 1, 0.1 ), Vector3( 0, 1, 0 ) ),
				"A13: an opaque overhang between the crossing and the sea's back face does not pop the sea" );
		}
	}

	// A14 (round 2, P3-a): more nested sheets than can be ordered soundly
	// are declined, not truncated in catalog order.
	{
		std::string many = Header(), few = Header();
		for( int k = 1; k <= 9; k++ ) {
			char g[16], o[16];
			std::snprintf( g, sizeof(g), "g_s%d", k );
			std::snprintf( o, sizeof(o), "s%d", k );
			many += HSheet( g, k, 5, true ) + Obj( o, g, "glass15" );
			if( k <= 3 ) few += HSheet( g, k, 5, true ) + Obj( o, g, "glass15" );
		}
		Loaded l9( many ), l3( few );
		Check( l9.ok && l3.ok, "A14 scenes load" );
		if( l9.ok && l3.ok ) {
			Check( l3.Seed( Point3( 0, 0, 0 ) ).Depth() == 4, "A14 control: three nested sheets seed three entries" );
			Check( l9.Seed( Point3( 0, 0, 0 ) ).Depth() == 1, "A14: nine nested sheets (over the cap) are declined, not truncated" );
		}
	}

	// A16 (round 3): a material-only edit (opaque -> glass) does not move the
	// spatial generation; the next PrepareForRendering must rebuild the
	// open-sheet catalog.
	{
		Loaded edit( Header() + HSheet( "g_pane", 2, 3, true ) + Obj( "pane", "g_pane", "matte" ) );
		Check( edit.ok, "A16 scene loads" );
		if( edit.ok ) {
			Check( edit.Seed( Point3( 0, 0, 0 ) ).Depth() == 1, "A16 control: an opaque pane is not an open sheet" );
			IObjectPriv* pane = edit.job->GetObjects()->GetItem( "pane" );
			const IMaterial* glass = edit.job->GetMaterials()->GetItem( "glass15" );
			Check( pane && glass, "A16: pane and glass found" );
			if( pane && glass ) pane->AssignMaterial( *glass );   // returns false by contract (Object::AssignMaterial)
			edit.job->GetScene()->GetObjects()->PrepareForRendering();
			const IORStack after = edit.Seed( Point3( 0, 0, 0 ) );
			Check( after.Depth() == 2 && Close( after.top(), 1.5 ),
				"A16: after PrepareForRendering the swapped-in glass pane seeds the point under it" );
		}
	}

	// A17 (round 3, P3-c): an HWSS companion lane delegating the first
	// segment.  The camera (0.9, 0, 0) is inside a pane's normal prism
	// (pane half-extent 1 at height 2); the segment's midpoint toward a wall
	// at x 5 is outside it.  The lane must follow the hero's seed.
	{
		Loaded prism( Header() + HSheet( "g_pane", 2, 1, true ) + Obj( "pane", "g_pane", "glass15" ) );
		Check( prism.ok, "A17 scene loads" );
		if( prism.ok ) {
			const Point3 origin( 0.9, 0, 0 ), midpoint( 2.95, 0, 0 );
			const IORStack hero = prism.Seed( origin );
			IORStack lane( 1.0 );
			IORStackSeeding::SeedCompanionLane( lane, hero, origin, midpoint, prism.Scene(), 550 );
			const IORStack atMid = prism.Seed( midpoint );
			char label[200];
			std::snprintf( label, sizeof(label), "A17: hero depth %zu, companion lane depth %zu (midpoint seed alone: %zu)",
				hero.Depth(), lane.Depth(), atMid.Depth() );
			Check( hero.Depth() == 2 && lane.Depth() == 2 && lane.topObject() == hero.topObject() && atMid.Depth() == 1, label );
		}
	}

	// A15 (round 2, P2-4): a point on a sheet's FRONT side is rejected
	// without a scene walk.
	{
		Loaded above( Header() + HSheet( "g_pane", -1, 3, true ) + Obj( "pane", "g_pane", "glass15" ) );
		if( above.ok ) {
			IORStackSeeding::DebugResetProbeTraceCount();
			above.Seed( Point3( 0, 0, 0 ) );
			const std::size_t c = IORStackSeeding::DebugGetProbeTraceCount();
			char label[160];
			std::snprintf( label, sizeof(label), "A15: probe walks above a sheet: %zu (want 1, no normal-ray walk)", c );
			Check( c == 1, label );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Part B: renders
//////////////////////////////////////////////////////////////////////
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

//! One render, Sobol'-salted by (seed base, render index), libc rand()
//! pinned likewise.
static double Render( const std::string& sceneText )
{
	const std::string path = TempPath( "render" );
	{
		std::ofstream ofs( path.c_str() );
		if( !ofs.is_open() ) return -1.0;
		ofs << sceneText;
	}
	std::srand( g_seedBase + g_renderIndex );
	SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 0x0618u + g_seedBase, g_renderIndex ) );
	g_renderIndex++;

	double result = -1.0;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
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

enum Integrator { kPT, kPTSMS, kBDPT, kVCM };
static const char* kName[] = { "PT", "PT+SMS", "BDPT", "VCM" };

static std::string Rasterizer( Integrator which )
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	switch( which ) {
	case kPT:
	case kPTSMS:
		ss << "pathtracing_pel_rasterizer\n{\n\tsamples 256\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n"
		   << "\tsms_enabled " << ( which == kPTSMS ? "TRUE" : "FALSE" ) << "\n}\n\n";
		break;
	case kBDPT:
		ss << "bdpt_pel_rasterizer\n{\n\tsamples 128\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
		break;
	default:
		ss << "vcm_pel_rasterizer\n{\n\tsamples 128\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
		   << "\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
		break;
	}
	ss << "file_rasterizeroutput\n{\n\tpattern rendered/half_space_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n\n"
	   << "film\n{\n\twidth 16\n\theight 16\n}\n\n";
	return ss.str();
}

static std::string Emitter( double y, double h, double scale )
{
	std::ostringstream ss;
	ss << "lambertian_luminaire_material\n{\n\tname m_emit\n\texitance white\n\tscale " << scale << "\n\tmaterial none\n}\n\n";
	const double a[3] = { -h, y, -h }, b[3] = { h, y, -h }, c[3] = { h, y, h }, d[3] = { -h, y, h };
	std::string q = SheetQuad( "g_emit", a, b, c, d );
	q.replace( q.find( "doublesided TRUE" ), 16, "doublesided FALSE" );
	return ss.str() + q + Obj( "emit", "g_emit", "m_emit" );
}

//! B1: camera under a lone up-facing glass pane at y 2, a 2.0 sphere
//! below it, a grey receiver at y 0, a black floor at y -0.25 so a
//! downward escape cannot differ from the closed-box reference, which
//! replaces the pane by a 1.5 box spanning y -0.5 .. 2.
static std::string PaneScene( Integrator which, bool closedBox )
{
	std::string s = "RISE ASCII SCENE 7\n" + Rasterizer( which ) + HeaderBody();
	s += "pinhole_camera\n{\n\tlocation 0 1.3 1.0\n\tlookat 0 0.3 0\n\tup 0 1 0\n\tfov 35.0\n}\n\n";
	s += Emitter( 4, 2, 4 );
	s += HSheet( "g_recv", 0, 1, true ) + Obj( "recv", "g_recv", "matte" );
	s += HSheet( "g_floor", -0.25, 8, true ) + Obj( "floor", "g_floor", "soot" );
	s += "sphere_geometry\n{\n\tname g_ball\n\tradius 0.35\n}\n\n" + Obj( "ball", "g_ball", "glass20", "0 0.6 0" );
	if( closedBox ) {
		s += "box_geometry\n{\n\tname g_box\n\twidth 16\n\theight 2.5\n\tdepth 16\n}\n\n" + Obj( "box", "g_box", "glass15", "0 0.75 0" );
	} else {
		s += HSheet( "g_pane", 2, 8, true ) + Obj( "pane", "g_pane", "glass15" );
	}
	return s;
}

//! B2: a closed opaque room (y 0 .. 3) lit from inside, a 2.0 sphere on
//! its floor, camera inside; optionally an up-facing glass pane at y 6.
static std::string RoomScene( Integrator which, bool pane )
{
	std::string s = "RISE ASCII SCENE 7\n" + Rasterizer( which ) + HeaderBody();
	s += "pinhole_camera\n{\n\tlocation 0 1.5 1.5\n\tlookat 0 0.4 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n";
	s += Emitter( 2.9, 0.5, 8 );
	s += "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 3\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte", "0 1.5 0" );
	s += "sphere_geometry\n{\n\tname g_ball\n\tradius 0.4\n}\n\n" + Obj( "ball", "g_ball", "glass20", "0 0.4 0" );
	if( pane ) {
		s += HSheet( "g_pane", 6, 8, true ) + Obj( "pane", "g_pane", "glass15" );
	}
	return s;
}

struct Stat { double mean, sd; };

static Stat Measure( const std::string& scene, unsigned int n )
{
	std::vector<double> v;
	for( unsigned int i = 0; i < n; i++ ) v.push_back( Render( scene ) );
	double m = 0;
	for( double x : v ) m += x;
	m /= double( n );
	double s = 0;
	for( double x : v ) s += ( x - m ) * ( x - m );
	s = n > 1 ? std::sqrt( s / double( n - 1 ) ) : 0.0;
	return Stat{ m, s };
}

static void RatioRow( const std::string& label, const Stat& a, const Stat& b, double tol )
{
	const double r = a.mean / b.mean;
	const double sdr = r * std::sqrt( ( a.sd / a.mean ) * ( a.sd / a.mean ) + ( b.sd / b.mean ) * ( b.sd / b.mean ) );
	char buf[400];
	std::snprintf( buf, sizeof(buf), "%s: %.6f (sd %.6f) / %.6f (sd %.6f) = %.4f +/- %.4f (band %.3f)",
		label.c_str(), a.mean, a.sd, b.mean, b.sd, r, sdr, tol );
	std::cout << "  " << buf << std::endl;
	Check( a.mean > 0 && b.mean > 0 && std::fabs( r - 1.0 ) <= tol, buf );
}

static std::string ReadFile( const char* path )
{
	std::ifstream in( path );
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static void PartB( unsigned int n )
{
	std::cout << "Part B: renders (n = " << n << ")" << std::endl;

	std::cout << "B1: camera under a lone glass pane, glass sphere below it" << std::endl;
	const Stat pt = Measure( PaneScene( kPT, false ), n );
	const Stat ptBox = Measure( PaneScene( kPT, true ), n );
	RatioRow( "B1 pane PT / closed-box PT", pt, ptBox, 0.03 );
	for( Integrator i : { kBDPT, kVCM } ) {
		RatioRow( std::string( "B1 pane " ) + kName[i] + " / PT", Measure( PaneScene( i, false ), n ), pt, 0.03 );
	}

	std::cout << "B2: closed opaque room under a pane" << std::endl;
	for( Integrator i : { kPT, kBDPT, kVCM } ) {
		const Stat with = Measure( RoomScene( i, true ), n );
		const Stat without = Measure( RoomScene( i, false ), n );
		RatioRow( std::string( "B2 room with pane / without, " ) + kName[i], with, without, 0.02 );
	}

	std::cout << "B3: DL-619 fixture, PT+SMS / PT" << std::endl;
	const std::string ptText = ReadFile( "tests/fixtures/dl382/PT_closed_below_ldn.RISEscene" );
	const std::string smsText = ReadFile( "tests/fixtures/dl382/SMS_closed_below_ldn.RISEscene" );
	Check( !ptText.empty() && !smsText.empty(), "B3 fixtures readable (run from the repository root)" );
	if( !ptText.empty() && !smsText.empty() ) {
		RatioRow( "B3 closed 2.2 box + down-facing 1.5 sheet, PT+SMS / PT", Measure( smsText, n ), Measure( ptText, n ), 0.03 );
	}
}

int main( int argc, char** argv )
{
	// One worker: a multithreaded render seeds workers from libc rand() in
	// thread-start order and is not reproducible run to run.
	if( !std::getenv( "RISE_OPTIONS_FILE" ) ) {
		std::snprintf( g_optPath, sizeof(g_optPath), "/tmp/half_space_options_%d.txt", static_cast<int>( ::getpid() ) );
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
	unsigned int n = 4;
	if( const char* s = std::getenv( "HALF_SPACE_N" ) ) {
		const long v = std::strtol( s, nullptr, 10 );
		if( v > 0 ) n = (unsigned int)v;
	}
	std::cout << "OpenSheetHalfSpaceSeedingTest (DL-618 / DL-619)   seed base = " << g_seedBase << std::endl;
	PartA();
	if( !std::getenv( "HALF_SPACE_UNIT_ONLY" ) ) {
		PartB( n );
	}
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
