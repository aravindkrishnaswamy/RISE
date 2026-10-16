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
//    B5  DL-622 review: an unpushed exit off a sheet's normal prism is
//        priced by the crossing, not the walk's history: PT == BDPT
//    B6  DL-622 Gap 1: a camera under a rock under a pane is seeded via
//        the free-space detour (matches the closed-box reference)
//    B7  DL-622: HWSS companion lanes re-indexed from a hero stack
//        holding an open sheet match independent wavelengths
//    B8  DL-622 review: no open sheet -- HWSS companion lanes seeded as
//        before DL-622 match independent wavelengths
//
//  HALF_SPACE_ONLY_NEW (env): Part B rows B5..B8 only.
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
#include "../src/Library/RISE_API.h"
#include "../src/Library/Rendering/RayCaster.h"

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

//! The index of stack entry @a i (0 = environment) through public pop/top
//! only, so the file also builds against DL-622's parent for red-proofs.
static Scalar IorAt( const IORStack& st, std::size_t i )
{
	IORStack c( st );
	while( c.Depth() > i + 1 ) {
		c.SetCurrentObject( c.topObject() );
		c.pop();
	}
	return c.top();
}

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
				// A light root passes its luminary (BDPTIntegrator, photon
				// tracers); the geometric guess alone is recorded below.
				IORStack low( 1.0 );
				IORStackSeeding::SeedFromPoint( low, hit.geometric.ptIntersection, lamp.Scene(), 0, true, lamp.O( "lamp" ) );
				std::printf( "  [recorded] A12 without the luminary (geometric guess): depth %zu\n",
					lamp.Seed( hit.geometric.ptIntersection ).Depth() );
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

	// A14 (round 2, P3-a; updated by DL-622 (3)): dynamic open-sheet vector
	// orders arbitrary numbers of nested sheets without an 8-sheet cap.
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
			const IORStack st9 = l9.Seed( Point3( 0, 0, 0 ) );
			bool order9 = ( st9.Depth() == 10 );
			for( int k = 1; order9 && k <= 9; k++ ) {
				char o[16];
				std::snprintf( o, sizeof(o), "s%d", k );
				order9 = ( st9.ObjectAt( std::size_t( 10 - k ) ) == l9.O( o ) );
			}
			Check( order9, "A14: nine nested sheets are ordered s9..s1 and seeded (DL-622 (3))" );
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

	// A18 (round 3, P3-1): a point on the inside of a standing opaque ring
	// (a torus, axis along z), at the bottom of its hole.  The normal ray up
	// crosses the hole and ENTERS the same ring at the top before reaching a
	// pond sheet above: the ring blocks.  Only the ray's first exit from the
	// seed's own body is skipped (A12).
	{
		Loaded ring( Header() + HSheet( "g_pond", 4, 5, true ) + Obj( "pond", "g_pond", "glass133" )
			+ "torus_geometry\n{\n\tname g_ring\n\tmajorradius 1.0\n\tminorratio 0.3\n}\n\n"
			+ "standard_object\n{\n\tname ring\n\tgeometry g_ring\n\tposition 0 1.5 0\n\torientation 90 0 0\n\tmaterial matte\n}\n\n" );
		Check( ring.ok, "A18 scene loads" );
		if( ring.ok ) {
			RayIntersection hit( Ray( Point3( 0.02, 1.5, 0.01 ), Vector3( 0, -1, 0 ) ), nullRasterizerState );
			ring.O( "ring" )->IntersectRay( hit, RISE_INFINITY, true, true, false );
			Check( hit.geometric.bHit && std::fabs( hit.geometric.ptIntersection.y - 0.8 ) < 0.05,
				"A18: probe meets the ring's inner surface at the bottom of its hole" );
			if( hit.geometric.bHit ) {
				Check( ring.Seed( hit.geometric.ptIntersection ).Depth() == 1,
					"A18: the seed's own object blocks once the normal ray re-enters it" );
			}
		}
	}

	// A19 (round 3 review, P2-a): a TESSELLATED sphere luminaire (a mesh,
	// whose intersector may report the point's own facet at t ~ 0) under a
	// sea sheet.  A near-origin self-hit must not consume the own-body
	// exemption that the later exit through the sphere's top needs.
	{
		Loaded mesh( Header() + HSheet( "g_sea", 4, 6, true ) + Obj( "sea", "g_sea", "glass133" )
			+ "lambertian_luminaire_material\n{\n\tname m_lamp\n\texitance white\n\tscale 1\n\tmaterial matte\n}\n\n"
			+ "uniformcolor_painter\n{\n\tname pnt_zero\n\tcolor 0 0 0\n}\n\n"
			+ "sphere_geometry\n{\n\tname g_ball\n\tradius 0.5\n}\n\n"
			+ "displaced_geometry\n{\n\tname g_mesh\n\tbase_geometry g_ball\n\tdetail 24\n\tdisplacement pnt_zero\n\tdisp_scale 0.0\n}\n\n"
			+ "sdf_geometry\n{\n\tname g_sdf\n\tpart sphere union 0 0 0 0 0 0 0 1 1 1 0.5 0 0 0\n\tsampling_detail 64\n}\n\n"
			+ Obj( "lamp", "g_mesh", "m_lamp", "0 1 0" ) + Obj( "sdflamp", "g_sdf", "m_lamp", "3 1 0" ) );
		Check( mesh.ok, "A19 scene loads" );
		if( mesh.ok ) {
			int seeded = 0, total = 0;
			for( int k = 0; k < 12; k++ ) {
				const bool sdf = k >= 6;
				const double ox = sdf ? 3.0 : 0.0;
				RayIntersection hit( Ray( Point3( ox + 0.05 * ( k % 6 ) - 0.12, -2, 0.03 * ( k % 6 ) - 0.07 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
				mesh.O( sdf ? "sdflamp" : "lamp" )->IntersectRay( hit, RISE_INFINITY, true, true, false );
				if( !hit.geometric.bHit ) continue;
				total++;
				IORStack st( 1.0 );
				IORStackSeeding::SeedFromPoint( st, hit.geometric.ptIntersection, mesh.Scene(), 0, true, mesh.O( sdf ? "sdflamp" : "lamp" ) );
				if( st.Depth() == 2 && Close( st.top(), 1.33 ) ) seeded++;
			}
			char label[160];
			std::snprintf( label, sizeof(label), "A19: %d of %d points on the lower halves of a tessellated and an SDF luminaire are seeded", seeded, total );
			Check( total > 0 && seeded == total, label );
		}
	}

	// A20 (round 3 review, P2-b): keyframed clipped-plane corners move a
	// sheet without moving its transform, outside any time-indexed pass.
	// PosedOpenSheet must re-describe it.
	{
		Loaded kf( Header() + HSheet( "g_pane", 2, 1, true ) + Obj( "pane", "g_pane", "glass15" ) );
		Check( kf.ok, "A20 scene loads" );
		if( kf.ok ) {
			IORStackSeeding::OpenSheetCatalogEntry before;
			const bool found = IORStackSeeding::FindOpenSheet( kf.Scene().GetObjects(), kf.O( "pane" ), before );
			IKeyframable* geom = dynamic_cast<IKeyframable*>( kf.job->GetGeometries()->GetItem( "g_pane" ) );
			Check( found && geom, "A20: pane catalogued and keyframable" );
			if( found && geom ) {
				// Tilt the pane 40 deg about z by moving its corners.
				double p[4][3] = { { -1, 0, 1 }, { 1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 } };
				const char* names[4] = { "pta", "ptb", "ptc", "ptd" };
				for( int c = 0; c < 4; c++ ) {
					RotZ( -40, p[c][0], p[c][1] );
					p[c][1] += 2;
					char value[96];
					std::snprintf( value, sizeof(value), "%g %g %g", p[c][0], p[c][1], p[c][2] );
					IKeyframeParameter* kp = geom->KeyframeFromParameters( names[c], value );
					if( kp ) { geom->SetIntermediateValue( *kp ); kp->release(); }
				}
				geom->RegenerateData();
				IORStackSeeding::OpenSheetCatalogEntry after;
				Check( IORStackSeeding::PosedOpenSheet( before, after ), "A20: re-posed sheet present" );
				char label[200];
				std::snprintf( label, sizeof(label), "A20: re-posed normal (%.3f, %.3f, %.3f), expected (0.643, 0.766, 0)",
					after.normal.x, after.normal.y, after.normal.z );
				Check( std::fabs( after.normal.x - 0.6428 ) < 0.01 && std::fabs( after.normal.y - 0.7660 ) < 0.01, label );
			}
		}
	}

	// A21 (round 3 review, P3-1): an emitter quad lying 1e-3 above
	// the floor of a closed opaque room, a pond sheet above the room.  The
	// room's ceiling is the normal ray's first opaque event, an exit; the
	// room's floor passes within the back probe's reach but not through the
	// seed point, so the room is not the seed's own body and blocks.
	{
		Loaded flush( Header() + HSheet( "g_pond", 6, 6, true ) + Obj( "pond", "g_pond", "glass133" )
			+ "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 3\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte", "0 1.5 0" )
			+ "lambertian_luminaire_material\n{\n\tname m_lamp\n\texitance white\n\tscale 1\n\tmaterial matte\n}\n\n"
			+ HSheet( "g_lamp", 0.001, 0.5, true ) + Obj( "lamp", "g_lamp", "m_lamp" ) );
		Check( flush.ok, "A21 scene loads" );
		if( flush.ok ) {
			RayIntersection hit( Ray( Point3( 0.1, 1, 0.05 ), Vector3( 0, -1, 0 ) ), nullRasterizerState );
			flush.O( "lamp" )->IntersectRay( hit, RISE_INFINITY, true, true, false );
			Check( hit.geometric.bHit, "A21: probe meets the flush emitter" );
			if( hit.geometric.bHit ) {
				Check( flush.Seed( hit.geometric.ptIntersection ).Depth() == 1,
					"A21: a flush emitter inside a closed room is not seeded into the pond above the room" );
			}
		}
	}

	// A22 (round 3 final, P3-1; updated by DL-622 (4)): an emitter quad 5e-5
	// above the floor of a closed opaque room, a pond above the room.  With
	// the geometric own-body guess removed (DL-622 (4)), both the explicit
	// pSeedObject call and the no-seed-object call reject the room's ceiling,
	// while an SDF luminaire's lower half under the pond is still seeded when
	// pSeedObject is passed.
	{
		Loaded flush( Header() + HSheet( "g_pond", 6, 6, true ) + Obj( "pond", "g_pond", "glass133" )
			+ "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 3\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte", "0 1.5 0" )
			+ "lambertian_luminaire_material\n{\n\tname m_lamp\n\texitance white\n\tscale 1\n\tmaterial matte\n}\n\n"
			+ HSheet( "g_lamp", 0.00005, 0.5, true ) + Obj( "lamp", "g_lamp", "m_lamp" )
			+ "sdf_geometry\n{\n\tname g_sdf\n\tpart sphere union 0 0 0 0 0 0 0 1 1 1 0.5 0 0 0\n\tsampling_detail 64\n}\n\n"
			+ Obj( "sdflamp", "g_sdf", "m_lamp", "3.5 1 0" ) );
		Check( flush.ok, "A22 scene loads" );
		if( flush.ok ) {
			RayIntersection hit( Ray( Point3( 0.1, 1, 0.05 ), Vector3( 0, -1, 0 ) ), nullRasterizerState );
			flush.O( "lamp" )->IntersectRay( hit, RISE_INFINITY, true, true, false );
			Check( hit.geometric.bHit, "A22: probe meets the flush emitter" );
			if( hit.geometric.bHit ) {
				IORStack withObject( 1.0 );
				IORStackSeeding::SeedFromPoint( withObject, hit.geometric.ptIntersection, flush.Scene(), 0, true, flush.O( "lamp" ) );
				Check( withObject.Depth() == 1,
					"A22: with the luminary passed, an emitter 5e-5 above a closed room's floor is not seeded into the pond" );
				const IORStack noObject = flush.Seed( hit.geometric.ptIntersection );
				Check( noObject.Depth() == 1,
					"A22 (DL-622 (4)): without pSeedObject, the geometric own-body guess is gone and the point is also not seeded" );
			}
			RayIntersection sdfHit( Ray( Point3( 3.48, -2, -0.07 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
			flush.O( "sdflamp" )->IntersectRay( sdfHit, RISE_INFINITY, true, true, false );
			if( sdfHit.geometric.bHit ) {
				IORStack sdfSeed( 1.0 );
				IORStackSeeding::SeedFromPoint( sdfSeed, sdfHit.geometric.ptIntersection, flush.Scene(), 0, true, flush.O( "sdflamp" ) );
				Check( sdfSeed.Depth() == 2 && Close( sdfSeed.top(), 1.33 ),
					"A22: an SDF luminaire's lower half under the pond is seeded with the luminary passed" );
			}
		}
	}

	// A23 (round 3 review, P2-1): a camera inside a closed room built from
	// SEPARATE outward-wound clipped-plane quads, a pond above.  A quad is an
	// open surface: an exit through it never makes the seed "on its body",
	// so the ceiling blocks.
	{
		std::string room = Header() + HSheet( "g_pond", 5, 6, true ) + Obj( "pond", "g_pond", "glass133" );
		// Six quads of the box x,z in [-2,2], y in [0,3], normals outward.
		const double faces[6][4][3] = {
			{ { -2, 3, 2 }, { 2, 3, 2 }, { 2, 3, -2 }, { -2, 3, -2 } },		// ceiling, +y
			{ { -2, 0, -2 }, { 2, 0, -2 }, { 2, 0, 2 }, { -2, 0, 2 } },		// floor, -y
			{ { 2, 0, -2 }, { 2, 3, -2 }, { 2, 3, 2 }, { 2, 0, 2 } },		// +x
			{ { -2, 0, 2 }, { -2, 3, 2 }, { -2, 3, -2 }, { -2, 0, -2 } },	// -x
			{ { -2, 0, 2 }, { 2, 0, 2 }, { 2, 3, 2 }, { -2, 3, 2 } },		// +z
			{ { 2, 0, -2 }, { -2, 0, -2 }, { -2, 3, -2 }, { 2, 3, -2 } }		// -z
		};
		for( int f = 0; f < 6; f++ ) {
			char g[16], o[16];
			std::snprintf( g, sizeof(g), "g_wall%d", f );
			std::snprintf( o, sizeof(o), "wall%d", f );
			room += SheetQuad( g, faces[f][0], faces[f][1], faces[f][2], faces[f][3] ) + Obj( o, g, "matte" );
		}
		// A24 (round 3 review, P1): the same room in a scene that also holds
		// an infinite plane (unbounded bounds must not inflate the self-hit
		// tolerance into one that skips the ceiling).
		{
			Loaded withPlane( room + "infiniteplane_geometry\n{\n\tname g_inf\n\txtile 1\n\tytile 1\n}\n\n"
				+ Obj( "inf", "g_inf", "matte", "0 -10 -50" ) );
			Check( withPlane.ok, "A24 scene loads" );
			if( withPlane.ok ) {
				Check( withPlane.Seed( Point3( 0.1, 1, 0.05 ) ).Depth() == 1,
					"A24: with an infinite plane in the scene, the camera in the quad room is still not seeded" );
			}
		}
		Loaded quads( room );
		Check( quads.ok, "A23 scene loads" );
		if( quads.ok ) {
			RayIntersection up( Ray( Point3( 0.1, 1, 0.05 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
			quads.O( "wall0" )->IntersectRay( up, RISE_INFINITY, true, true, false );
			Check( up.geometric.bHit && up.geometric.TrueGeomFacing( Vector3( 0, 1, 0 ) ) > 0,
				"A23 fixture: the ceiling quad's true normal points up (outward)" );
			Check( quads.Seed( Point3( 0.1, 1, 0.05 ) ).Depth() == 1,
				"A23: a camera inside a room of outward-wound quads is not seeded into the pond above" );
		}
	}

	// A25 (round 3 review, P2): a camera 0.05 below the ceiling of a closed
	// opaque room in a scene ~7000 units across, a pond above the room.  The
	// self-hit tolerance must not grow with the scene's size into a length
	// that skips the ceiling.
	{
		Loaded big( Header() + HSheet( "g_pond", 6, 8, true ) + Obj( "pond", "g_pond", "glass133" )
			+ "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 3\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte", "0 1.5 0" )
			+ "sphere_geometry\n{\n\tname g_far\n\tradius 1\n}\n\n" + Obj( "far", "g_far", "matte", "7000 0 0" ) );
		Check( big.ok, "A25 scene loads" );
		if( big.ok ) {
			Check( big.Seed( Point3( 0.1, 2.95, 0.05 ) ).Depth() == 1,
				"A25: a camera 0.05 below a closed room's ceiling in a 7000-unit scene is not seeded into the pond" );
		}
	}

	// A26 (round 3 review): the infinite-bounds filter on the scene size.
	// A small scene FAR from the origin (x = 1e6) holding an infinite plane:
	// the scene diagonal must be the bounded objects' (finite), so the
	// self-hit cap (1e-5 x diagonal ~ 2e-4) binds instead of 1e-7 x 1e6 =
	// 0.1, which would step over a closed room's ceiling 0.05 above the
	// camera and seed it into the pond above the room.
	{
		Loaded farScene( Header() + HSheet( "g_pond", 6, 8, true ) + Obj( "pond", "g_pond", "glass133", "1000000 0 0" )
			+ "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 3\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte", "1000000 1.5 0" )
			+ "infiniteplane_geometry\n{\n\tname g_inf\n\txtile 1\n\tytile 1\n}\n\n" + Obj( "inf", "g_inf", "matte", "1000000 -10 -50" ) );
		Check( farScene.ok, "A26 scene loads" );
		if( farScene.ok ) {
			const Scalar diag = IORStackSeeding::SceneLengthScale( farScene.Scene().GetObjects() );
			char label[200];
			std::snprintf( label, sizeof(label), "A26: scene diagonal %g is the bounded objects' (finite, < 100)", diag );
			Check( std::isfinite( diag ) && diag > 1 && diag < 100, label );
			Check( farScene.Seed( Point3( 1000000.1, 2.95, 0.05 ) ).Depth() == 1,
				"A26: a camera 0.05 below the ceiling of a room at x = 1e6 (infinite plane in the scene) is not seeded" );
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

	// A27 (DL-622 (1)): a point in free water under a submerged opaque rock
	// (closed box) or under a submerged open opaque canopy (clipped plane)
	// inside an open sea sheet's half-space is seeded via HasFreeSpaceDetourToSheet;
	// and a walk that entered `sea` and moved laterally outside `sea`'s
	// normal prism keeps `sea` on `ResolveUnpushedExit` when crossing a
	// different-material sheet `pane`.
	{
		Loaded sub( Header()
			+ HSheet( "g_sea", 5, 6, true ) + Obj( "sea", "g_sea", "glass133" )
			+ "box_geometry\n{\n\tname g_rock\n\twidth 2\n\theight 1\n\tdepth 2\n}\n\n" + Obj( "rock", "g_rock", "matte", "0 2.5 0" )
			+ HSheet( "g_canopy", 2.5, 1.0, true ) + Obj( "canopy", "g_canopy", "matte", "3.5 0 0" )
			+ HSheet( "g_pane", 1, 1, true ) + Obj( "pane", "g_pane", "glass15", "12 0 0" ) );
		Check( sub.ok, "A27 scene loads" );
		if( sub.ok ) {
			const IORStack underRock = sub.Seed( Point3( 0, 0, 0 ) );
			Check( underRock.Depth() == 2 && underRock.topObject() == sub.O( "sea" ) && Close( underRock.top(), 1.33 ),
				"A27: a point under a submerged closed opaque rock is seeded into the sea via lateral detour" );
			const IORStack inRock = sub.Seed( Point3( 0, 2.5, 0 ) );
			Check( inRock.Depth() == 1,
				"A27 control: a point strictly inside the submerged opaque rock is NOT seeded" );
			const IORStack underCanopy = sub.Seed( Point3( 3.5, 0, 0 ) );
			Check( underCanopy.Depth() == 2 && underCanopy.topObject() == sub.O( "sea" ) && Close( underCanopy.top(), 1.33 ),
				"A27: a point under a submerged open opaque canopy is seeded into the sea via lateral detour" );

			// DL-622 review: an unpushed exit is resolved by the CROSSING, not
			// by the walk's history.  Two walks that entered the sea at
			// different points (above the rock; above the canopy) cross an
			// unrelated pane's back face at x = 12, outside the sea's normal
			// prism: both leave the sea there, exactly as a seed just past
			// the crossing is not in it (the eye walk, a light walk and a
			// seed must price one point with one index).
			const Point3 pAfter( 12, 1.001, 0 );
			const IORStack seedAfter = sub.Seed( pAfter );
			Check( seedAfter.Depth() == 1, "A27 fixture: a seed just above the off-prism pane is not in the sea" );
			const IORStack histories[2] = { underRock, underCanopy };
			for( int h = 0; h < 2; h++ ) {
				IORStack walkStack = histories[h];
				walkStack.SetMediumProbeScene( &sub.Scene() );
				walkStack.SetCurrentObject( sub.O( "pane" ) );
				RayIntersection exitHit( Ray( Point3( 12, 0, 0 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
				sub.O( "pane" )->IntersectRay( exitHit, RISE_INFINITY, true, true, false );
				Check( exitHit.geometric.bHit, "A27: probe hits off-prism underwater pane" );
				if( exitHit.geometric.bHit ) {
					const IORStackSeeding::OpenSheetCrossing cross =
						IORStackSeeding::ResolveOpenSheetCrossing( exitHit.geometric, 1.5, walkStack );
					Check( !cross.bEntering && walkStack.Depth() == seedAfter.Depth() && Close( cross.etaTo, seedAfter.top() ),
						h == 0 ? "A27: an unpushed exit off the sea's prism agrees with a seed past it (walk entered above the rock)"
							: "A27: an unpushed exit off the sea's prism agrees with a seed past it (walk entered above the canopy)" );
				}
			}
		}
		// Nested narrower inner sheet (y=1.5, half-width 0.5) below two stacked
		// submerged opaque rocks (y=2.5 width 2, y=3.8 width 3.2) under sea (y=5):
		Loaded subNested( Header()
			+ HSheet( "g_sea", 5, 6, true ) + Obj( "sea", "g_sea", "glass133" )
			+ HSheet( "g_inner", 1.5, 0.5, true ) + Obj( "inner", "g_inner", "glass15" )
			+ "box_geometry\n{\n\tname g_rock1\n\twidth 2\n\theight 0.6\n\tdepth 2\n}\n\n" + Obj( "rock1", "g_rock1", "matte", "0 2.5 0" )
			+ "box_geometry\n{\n\tname g_rock2\n\twidth 3.2\n\theight 0.6\n\tdepth 3.2\n}\n\n" + Obj( "rock2", "g_rock2", "matte", "0 3.8 0" ) );
		Check( subNested.ok, "A27 nested-detour scene loads" );
		if( subNested.ok ) {
			const IORStack stNested = subNested.Seed( Point3( 0, 0, 0 ) );
			Check( stNested.Depth() == 3 &&
				stNested.ObjectAt( 1 ) == subNested.O( "sea" ) && Close( IorAt( stNested, 1 ), 1.33 ) &&
				stNested.ObjectAt( 2 ) == subNested.O( "inner" ) && Close( IorAt( stNested, 2 ), 1.5 ),
				"A27: detour around stacked obstacles preserves pre-obstacle inner sheet in passedSheets ordering" );
		}
	}

	// A28 (DL-622 (2)): folded and overhanging open sheets:
	//  (a) an L-folded open mesh (horizontal roof y=3, x in [-3,0] with +y
	//      normal; vertical wall x=0, y in [0,3] with +x normal) seeds points
	//      under the roof including (-2.8, 2.5, 0), which lies outside the
	//      global least-squares normal prism;
	//  (b) a 3-pleat S-folded open mesh (y=1 +y, y=2 -y, y=3 +y) seeds a point
	//      below all three pleats (2 back, 1 front -> signed parity +1 > 0),
	//      while a 2-pleat C-folded air tube (y=1 -y, y=2 +y) does NOT seed
	//      a point below both pleats (1 front, 1 back -> signed parity 0).
	{
		const std::string lFold =
			"indexedmesh_geometry\n{\n\tname g_lfold\n\tdouble_sided TRUE\n\tface_normals TRUE\n"
			"\tvertex -3 3  3\n\tvertex  0 3  3\n\tvertex  0 3 -3\n\tvertex -3 3 -3\n"
			"\tvertex  0 0  3\n\tvertex  0 0 -3\n"
			"\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n\tuv 0 0\n\tuv 1 1\n"
			"\ttriangle 0 1 2\n\ttriangle 0 2 3\n"
			"\ttriangle 4 5 2\n\ttriangle 4 2 1\n"
			"}\n\n";
		const std::string sFoldAccordion =
			"indexedmesh_geometry\n{\n\tname g_sacc\n\tdouble_sided TRUE\n\tface_normals TRUE\n"
			"\tvertex -3 1  3\n\tvertex  3 1  3\n\tvertex  3 1 -3\n\tvertex -3 1 -3\n"
			"\tvertex -3 2  3\n\tvertex -3 2 -3\n"
			"\tvertex  3 3  3\n\tvertex  3 3 -3\n"
			"\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n"
			"\ttriangle 0 1 2\n\ttriangle 0 2 3\n"	// bottom pleat (-3..3, y=1), normal +y
			"\ttriangle 1 4 5\n\ttriangle 1 5 2\n"	// middle pleat (3->-3, y=1..2), normal (-1/sqrt(37), -6/sqrt(37), 0) -> -y
			"\ttriangle 4 6 7\n\ttriangle 4 7 5\n"	// top pleat (-3->3, y=2..3), normal (-1/sqrt(37), +6/sqrt(37), 0) -> +y
			"}\n\n";
		const std::string cFoldTube =
			"indexedmesh_geometry\n{\n\tname g_ctube\n\tdouble_sided TRUE\n\tface_normals TRUE\n"
			"\tvertex -3 1  3\n\tvertex  3 1  3\n\tvertex  3 1 -3\n\tvertex -3 1 -3\n"
			"\tvertex -3 2  3\n\tvertex -3 2 -3\n"
			"\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n\tuv 0 0\n\tuv 1 1\n"
			"\ttriangle 0 3 2\n\ttriangle 0 2 1\n"	// bottom pleat (-3..3, y=1), normal -y (front hit from below!)
			"\ttriangle 1 2 5\n\ttriangle 1 5 4\n"	// top pleat (3->-3, y=1..2), normal +y (back hit from below!)
			"}\n\n";
		Loaded lf( Header() + lFold + Obj( "lfold", "g_lfold", "glass15" ) );
		Loaded sf( Header() + sFoldAccordion + Obj( "sacc", "g_sacc", "glass15" ) );
		Loaded cf( Header() + cFoldTube + Obj( "ctube", "g_ctube", "glass15" ) );
		Check( lf.ok && sf.ok && cf.ok, "A28 scenes load" );
		if( lf.ok && sf.ok && cf.ok ) {
			const IORStack stCorner = lf.Seed( Point3( -2.8, 2.5, 0 ) );
			Check( stCorner.Depth() == 2 && Close( stCorner.top(), 1.5 ),
				"A28a: L-folded overhanging sheet seeds a point under its horizontal lip outside the global normal prism" );
			const IORStack stInner = lf.Seed( Point3( -1.5, 1.5, 0 ) );
			Check( stInner.Depth() == 2 && Close( stInner.top(), 1.5 ),
				"A28a: L-folded sheet seeds an interior point in the corner of both facets" );
			const IORStack stOut = lf.Seed( Point3( 1.0, 2.5, 0 ) );
			Check( stOut.Depth() == 1,
				"A28a control: L-folded sheet declines a point outside both facets' normal prisms" );
			const IORStack stS = sf.Seed( Point3( 0, 0, 0 ) );
			Check( stS.Depth() == 2 && Close( stS.top(), 1.5 ),
				"A28b: 3-pleat S-folded sheet (2 back, 1 front along normal) seeds a point below all pleats" );
			const IORStack stC = cf.Seed( Point3( 0, 0, 0 ) );
			Check( stC.Depth() == 1,
				"A28b control: 2-pleat C-folded air tube (1 front, 1 back along normal) does NOT seed a point below it" );
		}
	}

	// A29 (DL-622 (1)/(2)): SeedCompanionLane re-indexes a walk-carried
	// dispersive stack in 0 scene ray walks, preserving membership under an
	// opaque rock and outside an open sheet's normal prism.
	{
		const std::string dispHeader = Header()
			+ "scalar_painter\n{\n\tname pnt_disp\n\tvalues 1.4 1.5 1.8\n}\n\n"
			+ "perfectrefractor_material\n{\n\tname glass_disp\n\trefractance white\n\tior pnt_disp\n}\n\n";
		Loaded disp( dispHeader + HSheet( "g_pane", 2, 1, true ) + Obj( "pane", "g_pane", "glass_disp" ) );
		Check( disp.ok, "A29 scene loads" );
		if( disp.ok ) {
			IORStack hero( 1.0 );
			IORStackSeeding::SeedFromPoint( hero, Point3( 0, 0, 0 ), disp.Scene(), 550 );
			Check( hero.Depth() == 2, "A29: hero seeded at (0,0,0)" );
			// Walk moves laterally to (10, 0, 0), far outside the pane's [-1, 1] prism:
			const Point3 farOrigin( 10, 0, 0 ), farMid( 12, 0, 0 );
			IORStackSeeding::DebugResetProbeTraceCount();
			IORStack lane450( 1.0 ), lane650( 1.0 );
			IORStackSeeding::SeedCompanionLane( lane450, hero, farOrigin, farMid, disp.Scene(), 450 );
			IORStackSeeding::SeedCompanionLane( lane650, hero, farOrigin, farMid, disp.Scene(), 650 );
			const std::size_t walks = IORStackSeeding::DebugGetProbeTraceCount();
			Check( walks == 0 && lane450.Depth() == 2 && lane650.Depth() == 2 && lane450.top() != lane650.top(),
				"A29: SeedCompanionLane re-indexes dispersive open-sheet membership outside the normal prism in 0 ray walks" );
		}
	}

	// A30 (DL-622 (3)): 12 nested open sheets at a point are all seeded in
	// exact outer-to-inner order without any 8-sheet truncation or decline.
	{
		std::string s12 = Header();
		const char* mats[3] = { "glass133", "glass15", "glass20" };
		const double iors[3] = { 1.33, 1.5, 2.0 };
		for( int k = 1; k <= 12; k++ ) {
			char g[16], o[16];
			std::snprintf( g, sizeof(g), "g_n%d", k );
			std::snprintf( o, sizeof(o), "n%d", k );
			s12 += HSheet( g, k, 6, true ) + Obj( o, g, mats[k % 3] );
		}
		Loaded l12( s12 );
		Check( l12.ok, "A30 scene loads" );
		if( l12.ok ) {
			const IORStack st = l12.Seed( Point3( 0, 0, 0 ) );
			bool orderOk = ( st.Depth() == 13 );
			// Outer sheet (y = 12, k = 12) is pushed first (index 1); innermost
			// sheet (y = 1, k = 1) is pushed last (index 12, top of stack).
			for( int k = 1; orderOk && k <= 12; k++ ) {
				char o[16];
				std::snprintf( o, sizeof(o), "n%d", k );
				const std::size_t slot = std::size_t( 13 - k );
				orderOk = ( st.ObjectAt( slot ) == l12.O( o ) ) && Close( IorAt( st, slot ), iors[k % 3] );
			}
			Check( orderOk, "A30: 12 nested open sheets seed all 12 entries in outer-to-inner order" );
		}
	}

	// A31 (DL-622 (6)): a hollow glass picture frame (an open mesh in
	// [-4,4]^2 \ [-3.6,3.6]^2 at y = 2, missed by all 15 Stage-1 centre and
	// quarter-point probes at 0 and +/-2) is catalogued by Stage 2 of
	// DescribeOpenSheet and seeds a point behind its border.
	{
		const std::string hollowFrame =
			"indexedmesh_geometry\n{\n\tname g_frame\n\tdouble_sided TRUE\n\tface_normals TRUE\n"
			"\tvertex -4.0 2  4.0\n\tvertex  4.0 2  4.0\n\tvertex  4.0 2 -4.0\n\tvertex -4.0 2 -4.0\n"
			"\tvertex -3.6 2  3.6\n\tvertex  3.6 2  3.6\n\tvertex  3.6 2 -3.6\n\tvertex -3.6 2 -3.6\n"
			"\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n\tuv 0 0\n\tuv 1 0\n\tuv 1 1\n\tuv 0 1\n"
			"\ttriangle 0 1 5\n\ttriangle 0 5 4\n"
			"\ttriangle 1 2 6\n\ttriangle 1 6 5\n"
			"\ttriangle 2 3 7\n\ttriangle 2 7 6\n"
			"\ttriangle 3 0 4\n\ttriangle 3 4 7\n"
			"}\n\n";
		Loaded frame( Header() + hollowFrame + Obj( "frame", "g_frame", "glass15" ) );
		Check( frame.ok, "A31 scene loads" );
		if( frame.ok ) {
			IORStackSeeding::OpenSheetCatalogEntry entry;
			const bool catOk = IORStackSeeding::FindOpenSheet( frame.Scene().GetObjects(), frame.O( "frame" ), entry );
			Check( catOk && std::fabs( entry.normal.y - 1.0 ) < 1e-5,
				"A31: hollow picture frame missed by all 15 Stage-1 rays is catalogued by Stage 2" );
			const IORStack behindBorder = frame.Seed( Point3( 3.8, 0, 0 ) );
			Check( behindBorder.Depth() == 2 && behindBorder.topObject() == frame.O( "frame" ) && Close( behindBorder.top(), 1.5 ),
				"A31: a point behind the hollow picture frame's border is seeded" );
			const IORStack inHole = frame.Seed( Point3( 0, 0, 0 ) );
			Check( inHole.Depth() == 1,
				"A31 control: a point below the empty centre hole of the picture frame is not seeded" );
		}
	}

	// A32 (DL-622 (5)): 50 stacked coplanar-normal glass panes (y = 1..50)
	// share one normal-group ray walk via ConfirmSharedNormalRay (2 total
	// ray walks instead of 51) and seed all 50 panes.
	{
		std::string s50 = Header();
		for( int k = 1; k <= 50; k++ ) {
			char g[16], o[16];
			std::snprintf( g, sizeof(g), "g_p%d", k );
			std::snprintf( o, sizeof(o), "p%d", k );
			s50 += HSheet( g, k, 5, true ) + Obj( o, g, "glass15" );
		}
		Loaded l50( s50 );
		Check( l50.ok, "A32 scene loads" );
		if( l50.ok ) {
			IORStackSeeding::DebugResetProbeTraceCount();
			const IORStack st = l50.Seed( Point3( 0, 0, 0 ) );
			const std::size_t walks = IORStackSeeding::DebugGetProbeTraceCount();
			char label[200];
			std::snprintf( label, sizeof(label), "A32: 50 stacked glass panes seeded (depth %zu, want 51) in %zu ray walks (want 2)",
				st.Depth(), walks );
			Check( st.Depth() == 51 && walks == 2, label );
		}
	}

	// A33 (DL-622 (7)): RefreshLegacySeedFallbackNM and ReindexStackForWavelength
	// re-index a dispersive starting stack entry whose object (`card_off` at
	// (5,5,5)) is off all 6 principal axes from `start = (0,0,0)` (both with
	// walk-carried EntryContext and with a bare synthetic stack), and
	// RefreshLegacySeedFallbackNM resolves an unpushed open-sheet exit along
	// the chain via ResolveUnpushedExit.
	{
		const std::string smsSceneText = Header()
			+ "homogeneous_medium\n{\n\tname vacuum_med\n\tabsorption 0 0 0\n\tscattering 0 0 0\n\tphase isotropic\n}\n\n"
			+ "global_medium\n{\n\tmedium vacuum_med\n}\n\n"
			+ "scalar_painter\n{\n\tname pnt_disp\n\tvalues 1.4 1.5 1.8\n}\n\n"
			+ "perfectrefractor_material\n{\n\tname glass_disp\n\trefractance white\n\tior pnt_disp\n}\n\n"
			+ HSheet( "g_card_off", 0, 0.5, true )
			+ "standard_object\n{\n\tname card_off\n\tgeometry g_card_off\n\tposition 5 5 5\n\tmaterial glass_disp\n}\n\n"
			+ HSheet( "g_slab_top", 2, 4, true ) + Obj( "slab_top", "g_slab_top", "glass_disp" );
		Loaded smsL( smsSceneText );
		Check( smsL.ok, "A33 scene loads" );
		if( smsL.ok ) {
			const IObject* cardOff = smsL.O( "card_off" );
			const IObject* slabTop = smsL.O( "slab_top" );
			Check( cardOff && slabTop, "A33 objects found" );
			if( cardOff && slabTop ) {
				// Evaluate expected dispersive index of glass_disp at 400nm and 700nm
				RayIntersection dummyHit( Ray( Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
				slabTop->IntersectRay( dummyHit, RISE_INFINITY, true, true, false );
				IORStack emptySt( 1.0 );
				const Scalar ior400 = cardOff->GetMaterial()->GetSpecularInfoNM( dummyHit.geometric, emptySt, 400 ).ior;
				const Scalar ior700 = cardOff->GetMaterial()->GetSpecularInfoNM( dummyHit.geometric, emptySt, 700 ).ior;

				std::vector<IShaderOp*> ops;
				IShader* shader = nullptr;
				RISE_API_CreateStandardShader( &shader, ops );
				RayCaster* caster = new RayCaster( false, 16, *shader, true );
				caster->AttachScene( &smsL.Scene() );
				ManifoldSolverConfig cfg;
				cfg.enabled = true;
				ManifoldSolver* solver = new ManifoldSolver( cfg );

				// Manually constructed stack (no EntryContext) holding `card_off` at hero IOR 1.5,
				// off all 6 principal axes from pos = (0,0,0):
				IORStack manualStart( 1.0 );
				manualStart.SetMediumProbeScene( &smsL.Scene() );
				manualStart.SetCurrentObject( cardOff );
				manualStart.pushOpenSheet( 1.5 );

				// Ray from (0,0,0) to light at (0,4,0) crosses `slab_top` (y=2, normal +y)
				// from behind -> unpushed open-sheet exit of the `card_off`+`slab_top` slab!
				std::vector<ManifoldVertex> chain400, chain700;
				solver->BuildSnellBaseSeed(
					Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), Point3( 0, 4, 0 ),
					smsL.Scene(), *caster, chain400, &manualStart, nullptr, 400 );
				solver->BuildSnellBaseSeed(
					Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), Point3( 0, 4, 0 ),
					smsL.Scene(), *caster, chain700, &manualStart, nullptr, 700 );

				Check( chain400.size() == 1 && chain700.size() == 1,
					"A33: BuildSnellBaseSeed builds 1-vertex chain across slab_top" );
				if( chain400.size() == 1 && chain700.size() == 1 ) {
					Check( Close( chain400[0].etaI, ior400 ) && Close( chain700[0].etaI, ior700 ) && ior400 != ior700,
						"A33: dispersive index refreshed at 400nm and 700nm" );
					Check( Close( chain400[0].etaT, 1.0 ) && Close( chain700[0].etaT, 1.0 ),
						"A33: RefreshLegacySeedFallbackNM pops sibling card_off on unpushed exit through slab_top (etaT == 1.0)" );
				}

				// From pos = (0, 3, 0) toward light = (0, 0, 0), the ray hits `slab_top` (y=2, normal +y)
				// on its FRONT face (entry), so chain[0].etaI is `card_off`'s refreshed index on `manualStart`!
				std::vector<ManifoldVertex> enter400, enter700;
				solver->BuildSnellBaseSeed(
					Point3( 0, 3, 0 ), Vector3( 0, -1, 0 ), Point3( 0, 0, 0 ),
					smsL.Scene(), *caster, enter400, &manualStart, nullptr, 400 );
				solver->BuildSnellBaseSeed(
					Point3( 0, 3, 0 ), Vector3( 0, -1, 0 ), Point3( 0, 0, 0 ),
					smsL.Scene(), *caster, enter700, &manualStart, nullptr, 700 );
				Check( enter400.size() == 1 && enter700.size() == 1 &&
					Close( enter400[0].etaI, ior400 ) && Close( enter700[0].etaI, ior700 ),
					"A33: RefreshLegacySeedFallbackNM re-indexes off-axis bare starting stack entry card_off (etaI == ior(nm), not hero 1.5)" );

				// Also verify walk-carried EntryContext on `ctxStart` (seeded under `card_off` at (5, 4, 5)):
				const IORStack ctxStart = smsL.Seed( Point3( 5, 4, 5 ) );
				std::vector<ManifoldVertex> ctxEnter400, ctxEnter700;
				solver->BuildSnellBaseSeed(
					Point3( 0, 3, 0 ), Vector3( 0, -1, 0 ), Point3( 0, 0, 0 ),
					smsL.Scene(), *caster, ctxEnter400, &ctxStart, nullptr, 400 );
				solver->BuildSnellBaseSeed(
					Point3( 0, 3, 0 ), Vector3( 0, -1, 0 ), Point3( 0, 0, 0 ),
					smsL.Scene(), *caster, ctxEnter700, &ctxStart, nullptr, 700 );
				Check( ctxEnter400.size() == 1 && ctxEnter700.size() == 1 &&
					Close( ctxEnter400[0].etaI, ior400 ) && Close( ctxEnter700[0].etaI, ior700 ),
					"A33: RefreshLegacySeedFallbackNM re-indexes walk-carried EntryContext for off-axis card_off at 400nm and 700nm" );

				solver->release();
				caster->release();
				shader->release();
			}
		}
	}

	// A34 (DL-622 review, P2): the free-space detour's lateral leg must be
	// FREE space.  A camera in a sealed habitat -- opaque roof slab, opaque
	// walls, one wall a closed GLASS box (a window) -- under a sea sheet:
	// its normal ray meets the roof (an entry face, so the detour runs), and
	// the only waypoint not behind an opaque wall lies beyond the window.
	// Crossing the window does not carry the sea's index into the habitat.
	// Control: the same habitat with no +x wall at all is open water, and
	// the detour seeds it.
	{
		const std::string boxes =
			"box_geometry\n{\n\tname g_roof\n\twidth 4.4\n\theight 0.2\n\tdepth 4.4\n}\n\n"
			"box_geometry\n{\n\tname g_wallx\n\twidth 0.2\n\theight 3.2\n\tdepth 4.4\n}\n\n"
			"box_geometry\n{\n\tname g_wallz\n\twidth 4.0\n\theight 3.2\n\tdepth 0.2\n}\n\n";
		const std::string shell = Header()
			+ HSheet( "g_sea", 6, 12, true ) + Obj( "sea", "g_sea", "glass133" ) + boxes
			+ Obj( "roof", "g_roof", "matte", "0 3.1 0" )
			+ Obj( "wallmx", "g_wallx", "matte", "-2.1 1.6 0" )
			+ Obj( "wallpz", "g_wallz", "matte", "0 1.6 2.1" )
			+ Obj( "wallmz", "g_wallz", "matte", "0 1.6 -2.1" );
		Loaded sealed( shell + Obj( "window", "g_wallx", "glass15", "2.1 1.6 0" ) );
		Loaded opened( shell );
		Check( sealed.ok && opened.ok, "A34 scenes load" );
		if( sealed.ok && opened.ok ) {
			const IORStack inSealed = sealed.Seed( Point3( 0, 1, 0 ) );
			Check( inSealed.Depth() == 1,
				"A34: a camera in a sealed habitat (glass window wall) under a sea is NOT seeded into the sea" );
			const IORStack inOpened = opened.Seed( Point3( 0, 1, 0 ) );
			Check( inOpened.Depth() == 2 && inOpened.topObject() == opened.O( "sea" ) && Close( inOpened.top(), 1.33 ),
				"A34 control: the same habitat with its +x side open is seeded into the sea by the detour" );
		}
	}

	// A35 (DL-622 review, P2): the detour certificate's cost is bounded.
	// 50 co-normal glass panes over a submerged rock: every pane's normal
	// ray from the seed meets the rock.  One certificate per pane walks the
	// pane column again and again (measured at the DL-622 commit: 2652 scene
	// rays per seed, every camera sample); the group certificate
	// (HasFreeSpaceDetourToSheets) walks it once and seeds all 50 panes, and
	// each pane's answer is the one its own certificate gives
	// (PointInOpenSheetMedium, which runs the single-sheet certificate).
	{
		auto stacked = []( int count, bool rock ) {
			std::string t = Header();
			for( int k = 1; k <= count; k++ ) {
				char g[16], o[16];
				std::snprintf( g, sizeof(g), "g_p%d", k );
				std::snprintf( o, sizeof(o), "p%d", k );
				t += HSheet( g, 2 + k, 5, true ) + Obj( o, g, "glass15" );
			}
			if( rock ) {
				t += "box_geometry\n{\n\tname g_rock\n\twidth 2\n\theight 0.5\n\tdepth 2\n}\n\n" + Obj( "rock", "g_rock", "matte", "0 1.5 0" );
			}
			return t;
		};
		Loaded deep( stacked( 50, true ) );
		Loaded shallow( stacked( 3, true ) );
		Check( deep.ok && shallow.ok, "A35 scenes load" );
		if( deep.ok && shallow.ok ) {
			deep.Seed( Point3( 0, 0, 0 ) );	// warm the catalog
			IORStackSeeding::Diagnostics::SceneRayCounter() = 0;
			const IORStack stDeep = deep.Seed( Point3( 0, 0, 0 ) );
			const std::size_t rays = IORStackSeeding::Diagnostics::SceneRayCounter();
			char label[240];
			std::snprintf( label, sizeof(label),
				"A35: 50 panes over a rock: %zu scene rays per seed (bound 300), depth %zu (want 51, p1 innermost)",
				rays, stDeep.Depth() );
			std::cout << "  [recorded] " << label << std::endl;
			Check( rays <= 300 && stDeep.Depth() == 51 && stDeep.topObject() == deep.O( "p1" ), label );
			bool agree = true;
			for( int k = 1; k <= 50; k += 7 ) {
				char o[16];
				std::snprintf( o, sizeof(o), "p%d", k );
				bool inSeed = false;
				for( std::size_t i = 1; i < stDeep.Depth(); i++ ) {
					if( stDeep.ObjectAt( i ) == deep.O( o ) ) inSeed = true;
				}
				agree = agree && ( inSeed == IORStackSeeding::PointInOpenSheetMedium(
					deep.O( o ), deep.Scene().GetObjects(), Point3( 0, 0, 0 ) ) );
			}
			Check( agree, "A35: each pane's group-certificate answer equals its own certificate's (PointInOpenSheetMedium)" );
			const IORStack stShallow = shallow.Seed( Point3( 0, 0, 0 ) );
			Check( stShallow.Depth() == 4 && stShallow.topObject() == shallow.O( "p1" ),
				"A35 control: 3 panes over the same rock are seeded through the detour (depth 4, p1 innermost)" );
		}
	}

	// A36 (DL-622 review, P2): a companion lane's re-index keeps a GRADED
	// enclosing medium at the value the walk tracked (DL-335's
	// RecordEnclosingAt), not the index at the point it was entered: the
	// field is wavelength-independent and the walk's value is the one its
	// next exit refracts into.  Fixture: a graded box (ior 1.2 + 0.05 z)
	// enclosing a dispersive open pane, the walk moved to z = 3.
	{
		Loaded graded( Header()
			+ "scalar_painter\n{\n\tname ior_ramp\n\texpression 1.2+0.05*P.z\n}\n\n"
			+ "perfectrefractor_material\n{\n\tname glass_graded\n\trefractance white\n\tior ior_ramp\n}\n\n"
			+ "scalar_painter\n{\n\tname pnt_disp\n\tvalues 1.4 1.5 1.8\n}\n\n"
			+ "perfectrefractor_material\n{\n\tname glass_disp\n\trefractance white\n\tior pnt_disp\n}\n\n"
			+ "box_geometry\n{\n\tname g_tank\n\twidth 12\n\theight 12\n\tdepth 12\n}\n\n" + Obj( "tank", "g_tank", "glass_graded" )
			+ HSheet( "g_pane", 2, 4, true ) + Obj( "pane", "g_pane", "glass_disp" ) );
		Check( graded.ok, "A36 scene loads" );
		if( graded.ok ) {
			IORStack hero( 1.0 );
			IORStackSeeding::SeedFromPoint( hero, Point3( 0, 0, 0 ), graded.Scene(), 550 );
			Check( hero.Depth() == 3 && hero.ObjectAt( 1 ) == graded.O( "tank" ) && hero.ObjectAt( 2 ) == graded.O( "pane" ),
				"A36 fixture: hero seeded [tank, pane]" );
			if( hero.Depth() == 3 ) {
				GradedIndexMedium::RecordEnclosingAt( hero, Point3( 0, 0, 3 ) );
				const Scalar tracked = IorAt( hero, 1 );
				Check( Close( tracked, 1.35 ), "A36 fixture: the walk's tracked tank index is n(z = 3) = 1.35" );
				IORStack lane( 1.0 );
				IORStackSeeding::SeedCompanionLane( lane, hero, Point3( 0, 0, 3 ), Point3( 0, 0.5, 3 ), graded.Scene(), 450, false );
				char label[200];
				std::snprintf( label, sizeof(label), "A36: companion lane keeps the graded tank at the tracked %.4f (got %.4f)",
					tracked, lane.Depth() == 3 ? IorAt( lane, 1 ) : -1.0 );
				Check( lane.Depth() == 3 && lane.ObjectAt( 1 ) == graded.O( "tank" ) && Close( IorAt( lane, 1 ), tracked ), label );
			}
		}
	}

	// A37 (DL-622 review, P2): a companion lane for a stack with NO open-
	// sheet entry keeps the pre-DL-622 rule (seed at the segment's point),
	// so HWSS output on closed-medium scenes is unchanged.  A stale hero
	// stack holding a glass box the walk is not in: the lane follows the
	// seed (air), not the hero's membership.
	{
		Loaded box( Header() + "box_geometry\n{\n\tname g_box\n\twidth 2\n\theight 2\n\tdepth 2\n}\n\n" + Obj( "box", "g_box", "glass15" ) );
		Check( box.ok, "A37 scene loads" );
		if( box.ok ) {
			IORStack hero( 1.0 );
			hero.SetMediumProbeScene( &box.Scene() );
			hero.SetCurrentObject( box.O( "box" ) );
			hero.push( 1.5 );
			IORStack lane( 1.0 );
			IORStackSeeding::SeedCompanionLane( lane, hero, Point3( 5, 0, 0 ), Point3( 6, 0, 0 ), box.Scene(), 450, false );
			Check( lane.Depth() == 1,
				"A37: a closed-medium hero stack's companion lane is re-seeded at the segment point (pre-DL-622 rule)" );
		}
	}

	// A38 (DL-622 review, P1): re-generating a clipped plane (every
	// keyframed pose under time-indexed motion blur) does not invalidate
	// every thread's open-sheet catalog; a re-posed sheet is re-described
	// per thread (PosedOpenSheet) at the pose it is used.
	{
		const Point3 pts[4] = { Point3( -1, 0, 1 ), Point3( 1, 0, 1 ), Point3( 1, 0, -1 ), Point3( -1, 0, -1 ) };
		IGeometry* pGeom = 0;
		const unsigned long long before = IORStackSeeding::OpenSheetCatalogEpoch().load();
		const bool made = RISE_API_CreateClippedPlaneGeometry( &pGeom, pts, true ) && pGeom;
		if( made ) {
			pGeom->RegenerateData();
			pGeom->RegenerateData();
		}
		const unsigned long long after = IORStackSeeding::OpenSheetCatalogEpoch().load();
		Check( made && before == after,
			"A38: ClippedPlaneGeometry::RegenerateData leaves the open-sheet catalog epoch unchanged" );
		safe_release( pGeom );
	}

	// A40 (DL-622 review round 2, P2-1): a sheet confirmed by a CLEAR normal
	// ray never depends on another sheet's detour.  An underwater camera
	// sees a tilted sea's back face along the sea's own normal; a stack of
	// co-normal panes above it is blocked by a small rock (a detour).  The
	// shared ray budget of the previous round ran out on the panes and
	// declined EVERY sheet, the sea included (the camera seeded in air).
	// And a seed and an exit resolution must agree sheet by sheet.
	{
		std::string t = Header();
		{
			const double h = 20;
			double p[4][3] = { { -h, 30, h }, { h, 30, h }, { h, 30, -h }, { -h, 30, -h } };
			for( auto& q : p ) RotZ( -30, q[0], q[1] );
			t += SheetQuad( "g_sea", p[0], p[1], p[2], p[3] ) + Obj( "sea", "g_sea", "glass133" );
		}
		for( int k = 1; k <= 50; k++ ) {
			char g[16], o[16];
			std::snprintf( g, sizeof(g), "g_p%d", k );
			std::snprintf( o, sizeof(o), "p%d", k );
			t += HSheet( g, 2 + 0.2 * k, 10, true ) + Obj( o, g, "glass15" );
		}
		t += "box_geometry\n{\n\tname g_rock\n\twidth 0.6\n\theight 0.4\n\tdepth 0.6\n}\n\n" + Obj( "rock", "g_rock", "matte", "0 1.2 0" );
		Loaded mix( t );
		Check( mix.ok, "A40 scene loads" );
		if( mix.ok ) {
			const IObjectManager* objs = mix.Scene().GetObjects();
			const IORStack st = mix.Seed( Point3( 0, 0, 0 ) );
			bool seaIn = false;
			for( std::size_t i = 1; i < st.Depth(); i++ ) {
				if( st.ObjectAt( i ) == mix.O( "sea" ) ) seaIn = true;
			}
			Check( IORStackSeeding::PointInOpenSheetMedium( mix.O( "sea" ), objs, Point3( 0, 0, 0 ) ),
				"A40 fixture: the sea's normal ray from the camera is clear (the sea holds the camera)" );
			Check( seaIn, "A40: the sea is seeded whatever the pane stack's detours cost" );
			bool agree = true;
			const char* names[4] = { "sea", "p1", "p25", "p50" };
			for( int k = 0; k < 4; k++ ) {
				bool inSeed = false;
				for( std::size_t i = 1; i < st.Depth(); i++ ) {
					if( st.ObjectAt( i ) == mix.O( names[k] ) ) inSeed = true;
				}
				agree = agree && ( inSeed == IORStackSeeding::PointInOpenSheetMedium( mix.O( names[k] ), objs, Point3( 0, 0, 0 ) ) );
			}
			Check( agree, "A40: seed and PointInOpenSheetMedium agree for the sea and the panes" );
		}
	}

	// A41 (DL-622 review round 2, P2-2): an unpushed exit is decided by the
	// seed convention at the point past the crossing -- not by walking along
	// the walk's direction through opaque surfaces until the sheet is met.
	// A walk in the sea's medium (seeded above an opaque roof slab that
	// spans the sea's whole prism) leaves a small pane under the roof
	// through its back face going up: the sea's back face IS straight ahead,
	// past the roof, but the sea's normal ray from there is blocked by the
	// roof, so a seed there is not in the sea, and the walk leaves it too.
	{
		Loaded roofed( Header()
			+ HSheet( "g_sea", 5, 6, true ) + Obj( "sea", "g_sea", "glass133" )
			+ "box_geometry\n{\n\tname g_roof\n\twidth 14\n\theight 0.2\n\tdepth 14\n}\n\n" + Obj( "roof", "g_roof", "matte", "0 3.1 0" )
			+ HSheet( "g_pane", 1, 1, true ) + Obj( "pane", "g_pane", "glass15" ) );
		Check( roofed.ok, "A41 scene loads" );
		if( roofed.ok ) {
			IORStack walk = roofed.Seed( Point3( 0, 4, 0 ) );
			Check( walk.Depth() == 2 && walk.topObject() == roofed.O( "sea" ), "A41 fixture: a seed above the roof is in the sea" );
			const IORStack seedAfter = roofed.Seed( Point3( 0, 1.001, 0 ) );
			walk.SetMediumProbeScene( &roofed.Scene() );
			walk.SetCurrentObject( roofed.O( "pane" ) );
			RayIntersection exitHit( Ray( Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
			roofed.O( "pane" )->IntersectRay( exitHit, RISE_INFINITY, true, true, false );
			if( exitHit.geometric.bHit ) {
				const IORStackSeeding::OpenSheetCrossing cross =
					IORStackSeeding::ResolveOpenSheetCrossing( exitHit.geometric, 1.5, walk );
				char label[200];
				std::snprintf( label, sizeof(label),
					"A41: exit under the roof agrees with a seed past it (walk depth %zu, seed depth %zu)",
					walk.Depth(), seedAfter.Depth() );
				Check( !cross.bEntering && walk.Depth() == seedAfter.Depth() && Close( cross.etaTo, seedAfter.top() ), label );
			} else {
				Check( false, "A41: probe hits the pane" );
			}
		}
	}

	// A42 (DL-622 review round 2, P3): entry contexts are shared copy-on-
	// write: a copy that pushes, pops or re-indexes never changes the
	// original's contexts, and popping an entry below a context re-indexes
	// it.
	{
		Loaded two( Header() + HSheet( "g_a", 2, 3, true ) + Obj( "a", "g_a", "glass15" )
			+ HSheet( "g_b", 4, 3, true ) + Obj( "b", "g_b", "glass133" )
			+ "box_geometry\n{\n\tname g_box\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n" + Obj( "box", "g_box", "glass20", "0 -5 0" ) );
		Check( two.ok, "A42 scene loads" );
		if( two.ok ) {
			const IORStack orig = two.Seed( Point3( 0, 0, 0 ) );
			const bool fixtureOk = orig.Depth() == 3 && orig.ContextAt( 1 ).bHasContext && orig.ContextAt( 2 ).bHasContext;
			Check( fixtureOk, "A42 fixture: [b, a] seeded with contexts" );
			if( fixtureOk ) {
				const Point3 aAt = orig.ContextAt( 2 ).ptIntersection;
				IORStack copy( orig );
				copy.SetCurrentObject( two.O( "box" ) );
				copy.push( 2.0 );
				IORStack::EntryContext moved = copy.ContextAt( 2 );
				moved.ptIntersection = Point3( 9, 9, 9 );
				copy.SetContextAt( 2, moved );
				copy.SetCurrentObject( two.O( "b" ) );
				copy.pop();		// erase b (index 1) below a's context
				Check( orig.Depth() == 3 && orig.ContextAt( 2 ).ptIntersection.x == aAt.x &&
					orig.ContextAt( 1 ).bHasContext && orig.FindEntryContext( two.O( "a" ) ) == &orig.ContextAt( 2 ),
					"A42: mutating a copy leaves the original's contexts untouched" );
				Check( copy.Depth() == 3 && copy.ObjectAt( 1 ) == two.O( "a" ) && copy.ContextAt( 1 ).ptIntersection.x == 9 &&
					!copy.ContextAt( 2 ).bHasContext && copy.FindEntryContext( two.O( "b" ) ) == 0,
					"A42: popping an entry below a context re-indexes the copy's contexts" );
			}
		}
	}

	// A43 (DL-622 review round 2, P3): RefreshLegacySeedFallbackNM, an entry
	// it cannot re-index (a key that is not a scene object) stands in its
	// query stack as its PARENT's index, so a DL-46 translucent entry above
	// it re-pushes the refreshed ancestor at the companion wavelength (the
	// first DL-622 landing pushed the hero value 1.7 there).
	{
		Loaded tl( Header()
			+ "scalar_painter\n{\n\tname pnt_disp\n\tvalues 1.4 1.5 1.8\n}\n\n"
			+ "perfectrefractor_material\n{\n\tname glass_disp\n\trefractance white\n\tior pnt_disp\n}\n\n"
			+ "translucent_material\n{\n\tname mat_trans\n\tref white\n\ttau white\n\text 0\n\tN 1.0\n\tscattering 0.3\n}\n\n"
			+ "box_geometry\n{\n\tname g_tank\n\twidth 12\n\theight 12\n\tdepth 12\n}\n\n" + Obj( "tank", "g_tank", "glass_disp" )
			+ "box_geometry\n{\n\tname g_card\n\twidth 3\n\theight 3\n\tdepth 3\n}\n\n" + Obj( "card", "g_card", "mat_trans" )
			+ HSheet( "g_slab_top", 2, 4, true ) + Obj( "slab_top", "g_slab_top", "glass15" ) );
		Check( tl.ok, "A43 scene loads" );
		if( tl.ok ) {
			RayIntersection probe( Ray( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) ), nullRasterizerState );
			tl.O( "tank" )->IntersectRay( probe, RISE_INFINITY, true, true, false );
			IORStack empty( 1.0 );
			const Scalar ior400 = tl.O( "tank" )->GetMaterial()->GetSpecularInfoNM( probe.geometric, empty, 400 ).ior;
			static int notASceneObject = 0;
			IORStack start( 1.0 );
			start.SetMediumProbeScene( &tl.Scene() );
			start.SetCurrentObject( tl.O( "tank" ) );
			start.push( 1.5 );
			start.SetCurrentObject( reinterpret_cast<const IObject*>( &notASceneObject ) );
			start.push( 1.7 );
			start.SetCurrentObject( tl.O( "card" ) );
			start.push( 1.7 );
			std::vector<IShaderOp*> ops;
			IShader* shader = nullptr;
			RISE_API_CreateStandardShader( &shader, ops );
			RayCaster* caster = new RayCaster( false, 16, *shader, true );
			caster->AttachScene( &tl.Scene() );
			ManifoldSolverConfig cfg;
			cfg.enabled = true;
			ManifoldSolver* solver = new ManifoldSolver( cfg );
			std::vector<ManifoldVertex> chain;
			solver->BuildSnellBaseSeed(
				Point3( 0, 3, 0 ), Vector3( 0, -1, 0 ), Point3( 0, 1.0, 0 ),
				tl.Scene(), *caster, chain, &start, nullptr, 400 );
			char label[220];
			std::snprintf( label, sizeof(label),
				"A43: translucent entry above an unresolvable key re-pushes the refreshed ancestor (etaI %.4f, want ior(400) %.4f)",
				chain.empty() ? -1.0 : chain[0].etaI, ior400 );
			Check( chain.size() >= 1 && Close( chain[0].etaI, ior400 ), label );
			solver->release();
			caster->release();
			shader->release();
		}
	}

	// A39 (DL-622 review, P3): a probe table never silently drops a met
	// object.  With open sheets in the scene the probe's step budget grows
	// past kMaxProbeEntries; a probe that meets 33 small glass spheres
	// before leaving the big glass tank around the seed must still see the
	// tank (34 distinct objects; 20 sheets far away raise the budget to 72).
	{
		std::string t = Header()
			+ "box_geometry\n{\n\tname g_tank\n\twidth 90\n\theight 90\n\tdepth 90\n}\n\n" + Obj( "tank", "g_tank", "glass15" )
			+ "sphere_geometry\n{\n\tname g_bead\n\tradius 0.2\n}\n\n";
		for( int k = 1; k <= 33; k++ ) {
			char o[16], pos[48];
			std::snprintf( o, sizeof(o), "bead%d", k );
			std::snprintf( pos, sizeof(pos), "0 0 %d", k );
			t += Obj( o, "g_bead", "glass20", pos );
		}
		for( int k = 1; k <= 20; k++ ) {
			char g[16], o[16], pos[48];
			std::snprintf( g, sizeof(g), "g_far%d", k );
			std::snprintf( o, sizeof(o), "far%d", k );
			std::snprintf( pos, sizeof(pos), "300 %d 0", 3 * k );
			t += HSheet( g, 0, 1, true ) + Obj( o, g, "glass133", pos );
		}
		Loaded beads( t );
		Check( beads.ok, "A39 scene loads" );
		if( beads.ok ) {
			const IORStack st = beads.Seed( Point3( 0, 0, 0 ) );
			Check( st.Depth() == 2 && st.topObject() == beads.O( "tank" ) && Close( st.top(), 1.5 ),
				"A39: a seed inside a tank behind 33 beads along +z is seeded into the tank (no silent table cap)" );
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

enum Integrator { kPT, kPTSMS, kBDPT, kVCM, kPTSpectral, kPTSpectralHWSS };
static const char* kName[] = { "PT", "PT+SMS", "BDPT", "VCM", "PT spectral", "PT spectral HWSS" };

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
	case kPTSpectral:
	case kPTSpectralHWSS:
		ss << "pathtracing_spectral_rasterizer\n{\n\tsamples 128\n\tspectral_samples 1\n\tnum_wavelengths 160\n"
		   << "\tnmbegin 380\n\tnmend 780\n\thwss " << ( which == kPTSpectralHWSS ? "TRUE" : "FALSE" ) << "\n"
		   << "\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
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

//! B4: a one-sided emitter quad lying 5e-5 above the floor of a closed
//! opaque room (facing up into it), a 2.0 sphere beside it, camera inside;
//! optionally an up-facing glass pane above the room.
static std::string FlushRoomScene( Integrator which, bool pane )
{
	std::string s = "RISE ASCII SCENE 7\n" + Rasterizer( which ) + HeaderBody();
	s += "pinhole_camera\n{\n\tlocation 0 1.5 1.5\n\tlookat -0.8 0.4 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n";
	s += "lambertian_luminaire_material\n{\n\tname m_emit\n\texitance white\n\tscale 4\n\tmaterial none\n}\n\n";
	{
		std::string q = HSheet( "g_emit", 0.00005, 0.5, true );
		q.replace( q.find( "doublesided TRUE" ), 16, "doublesided FALSE" );
		s += q + Obj( "emit", "g_emit", "m_emit", "-0.8 0 0" );
	}
	s += "box_geometry\n{\n\tname g_room\n\twidth 4\n\theight 3\n\tdepth 4\n}\n\n" + Obj( "room", "g_room", "matte", "0 1.5 0" );
	s += "sphere_geometry\n{\n\tname g_ball\n\tradius 0.6\n}\n\n" + Obj( "ball", "g_ball", "glass20", "-0.8 0.62 0" );
	if( pane ) {
		s += HSheet( "g_pane", 6, 8, true ) + Obj( "pane", "g_pane", "glass15" );
	}
	return s;
}

//! B5 (DL-622 review): an unpushed exit is resolved by the crossing, not
//! the walk's history.  The camera sits in a sea sheet's medium (inside
//! its normal prism, y 4, |x|,|z| <= 3); it views a receiver at x = 5,
//! outside the prism, lit only THROUGH a glass pane above it (y 2) by an
//! emitter in air (y 3.5).  PT's eye walk crosses the pane's back face
//! carrying the sea on its stack; a rule that kept the sea there because
//! the walk could still see where it entered priced that crossing
//! 1.5 -> 1.33 while BDPT's light walk (seeded at the emitter, not in the
//! sea) priced it 1.0 -> 1.5: PT and BDPT disagreed.
static std::string OffPrismScene( Integrator which )
{
	// The receiver is lit only by BSDF-sampled hits on the emitter through
	// the pane (PT) or by light tracing (BDPT): 16x the base sample count
	// keeps each render's sd near 1 %.
	std::string r = Rasterizer( which );
	const std::size_t at = r.find( "\tsamples " );
	if( at != std::string::npos ) {
		const std::size_t eol = r.find( '\n', at );
		const int spp = std::atoi( r.substr( at + 9, eol - at - 9 ).c_str() );
		r.replace( at, eol - at, "\tsamples " + std::to_string( 16 * spp ) );
	}
	std::string s = "RISE ASCII SCENE 7\n" + r + HeaderBody();
	s += "pinhole_camera\n{\n\tlocation 0 1 2.5\n\tlookat 5 0 0\n\tup 0 1 0\n\tfov 18.0\n}\n\n";
	s += "lambertian_luminaire_material\n{\n\tname m_emit\n\texitance white\n\tscale 6\n\tmaterial none\n}\n\n";
	{
		std::string q = HSheet( "g_emit", 3.5, 0.7, false );
		q.replace( q.find( "doublesided TRUE" ), 16, "doublesided FALSE" );
		s += q + Obj( "emit", "g_emit", "m_emit", "5 0 0" );
	}
	s += HSheet( "g_sea", 4, 3, true ) + Obj( "sea", "g_sea", "glass133" );
	s += HSheet( "g_pane", 2, 1, true ) + Obj( "pane", "g_pane", "glass15", "5 0 0" );
	s += HSheet( "g_recv", 0, 0.9, true ) + Obj( "recv", "g_recv", "matte", "5 0 0" );
	s += HSheet( "g_floor", -0.25, 20, true ) + Obj( "floor", "g_floor", "soot" );
	return s;
}

//! B6 (DL-622 Gap 1): B1's scene with a small opaque rock between the
//! camera and the pane, over the camera.  The camera is seeded into the
//! pane's medium through the free-space detour around the rock, so the
//! image matches the same scene with the pane replaced by a closed glass
//! box (which seeds the camera by parity); before DL-622 the rock blocked
//! the camera's normal ray and the camera was left in air.
static std::string RockPaneScene( Integrator which, bool closedBox )
{
	std::string s = PaneScene( which, closedBox );
	s += "box_geometry\n{\n\tname g_rock\n\twidth 0.6\n\theight 0.2\n\tdepth 0.6\n}\n\n" + Obj( "rock", "g_rock", "matte", "0 1.7 1.0" );
	return s;
}

//! B7 (DL-622 companion re-index): the camera above a constant-index water
//! sheet looks down at a DISPERSIVE glass ball under it; at the ball the
//! HWSS hero hands off to its companions with the sheet on its stack, and
//! each lane takes the hero's membership re-indexed at its wavelength.
//! HWSS must match independent per-wavelength paths (hwss FALSE).
static std::string DispersiveUnderSheetScene( Integrator which )
{
	std::string s = "RISE ASCII SCENE 7\n" + Rasterizer( which ) + HeaderBody();
	s += "scalar_painter\n{\n\tname pnt_disp\n\tvalues 1.4 1.5 1.8\n}\n\n";
	s += "perfectrefractor_material\n{\n\tname glass_disp\n\trefractance white\n\tior pnt_disp\n}\n\n";
	s += "pinhole_camera\n{\n\tlocation 0 2.6 1.2\n\tlookat 0 0.3 0\n\tup 0 1 0\n\tfov 35.0\n}\n\n";
	s += Emitter( 4, 2, 4 );
	s += HSheet( "g_sea", 2, 8, true ) + Obj( "sea", "g_sea", "glass133" );
	s += "sphere_geometry\n{\n\tname g_ball\n\tradius 0.35\n}\n\n" + Obj( "ball", "g_ball", "glass_disp", "0 0.6 0" );
	s += HSheet( "g_recv", 0, 1, true ) + Obj( "recv", "g_recv", "matte" );
	s += HSheet( "g_floor", -0.25, 8, true ) + Obj( "floor", "g_floor", "soot" );
	return s;
}

//! B8 (DL-622 review): NO open sheet.  A dispersive glass ball inside a
//! closed water box, camera outside: the HWSS hero enters the box and hands
//! off at the ball with a closed-medium stack, which SeedCompanionLane
//! seeds exactly as before DL-622 (the parent commit renders this row with
//! the same means).  HWSS must match independent wavelengths.
static std::string DispersiveInBoxScene( Integrator which )
{
	std::string s = "RISE ASCII SCENE 7\n" + Rasterizer( which ) + HeaderBody();
	s += "scalar_painter\n{\n\tname pnt_disp\n\tvalues 1.4 1.5 1.8\n}\n\n";
	s += "perfectrefractor_material\n{\n\tname glass_disp\n\trefractance white\n\tior pnt_disp\n}\n\n";
	s += "pinhole_camera\n{\n\tlocation 0 2.6 1.2\n\tlookat 0 0.3 0\n\tup 0 1 0\n\tfov 35.0\n}\n\n";
	s += Emitter( 4, 2, 4 );
	s += "box_geometry\n{\n\tname g_tank\n\twidth 4\n\theight 2.25\n\tdepth 4\n}\n\n" + Obj( "tank", "g_tank", "glass133", "0 0.875 0" );
	s += "sphere_geometry\n{\n\tname g_ball\n\tradius 0.35\n}\n\n" + Obj( "ball", "g_ball", "glass_disp", "0 0.6 0" );
	s += HSheet( "g_recv", 0, 1, true ) + Obj( "recv", "g_recv", "matte" );
	s += HSheet( "g_floor", -0.25, 8, true ) + Obj( "floor", "g_floor", "soot" );
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
	const bool onlyB4 = std::getenv( "HALF_SPACE_ONLY_B4" ) != nullptr;
	// HALF_SPACE_ONLY_NEW: only the DL-622 review rows (B5..B8).
	const bool onlyNew = std::getenv( "HALF_SPACE_ONLY_NEW" ) != nullptr;

	if( !onlyB4 && !onlyNew ) {
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

	}
	if( !onlyNew ) {
	std::cout << "B4: emitter 5e-5 above a closed room's floor, pane above the room" << std::endl;
	for( Integrator i : { kPT, kBDPT } ) {
		const Stat with = Measure( FlushRoomScene( i, true ), n );
		const Stat without = Measure( FlushRoomScene( i, false ), n );
		RatioRow( std::string( "B4 flush-emitter room with pane / without, " ) + kName[i], with, without, 0.02 );
	}
	}

	if( onlyB4 ) {
		return;
	}

	std::cout << "B5: unpushed exit off a sheet's prism, PT vs BDPT" << std::endl;
	RatioRow( "B5 off-prism pane exit, PT / BDPT", Measure( OffPrismScene( kPT ), n ), Measure( OffPrismScene( kBDPT ), n ), 0.03 );

	std::cout << "B6: camera under a rock under a pane" << std::endl;
	{
		const Stat pt = Measure( RockPaneScene( kPT, false ), n );
		RatioRow( "B6 rock+pane PT / rock+closed-box PT", pt, Measure( RockPaneScene( kPT, true ), n ), 0.03 );
		RatioRow( "B6 rock+pane BDPT / PT", Measure( RockPaneScene( kBDPT, false ), n ), pt, 0.03 );
	}

	std::cout << "B7: dispersive ball under a water sheet, HWSS vs independent wavelengths" << std::endl;
	RatioRow( "B7 PT spectral hwss TRUE / FALSE", Measure( DispersiveUnderSheetScene( kPTSpectralHWSS ), n ),
		Measure( DispersiveUnderSheetScene( kPTSpectral ), n ), 0.03 );

	std::cout << "B8: dispersive ball in a closed water box (no open sheet), HWSS vs independent wavelengths" << std::endl;
	RatioRow( "B8 PT spectral hwss TRUE / FALSE", Measure( DispersiveInBoxScene( kPTSpectralHWSS ), n ),
		Measure( DispersiveInBoxScene( kPTSpectral ), n ), 0.03 );

	if( onlyNew ) {
		return;
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
