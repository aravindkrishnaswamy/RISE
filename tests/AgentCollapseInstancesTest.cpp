//////////////////////////////////////////////////////////////////////
//
//  AgentCollapseInstancesTest.cpp - 88 step 2 (2026-08-19):
//    collapse_to_instances, the VERB half of design-note condition C.
//
//  THE CORRECTNESS BAR IS "NO-OP ON THE RENDERED SCENE", and this file
//  measures exactly that, numerically.  Every positive case derives the
//  document BEFORE the collapse, records the world transform of EVERY
//  object in the live scene, runs the verb, re-derives, records them
//  again, and asserts the two MULTISETS match to floating-point
//  tolerance.  Multisets, not sequences: the copies are renamed to
//  `<chunk>[i,j]` entries by construction, so name-keyed comparison would
//  measure the renaming rather than the geometry -- while a multiset of
//  world matrices catches the two failure modes that actually matter,
//  an array that sits one step off and an array that dropped its last
//  copy.  A test that only checked "a chunk with count_u appeared" would
//  catch neither.
//
//  Cases:
//    A  LINE.  Six copies on one geometry at an even spacing collapse to
//       ONE instancing chunk (`count_u 5`), the world-transform multiset
//       is unchanged, the object COUNT is unchanged, and the emitted
//       expression is the short decimal (`2.6`, not 2.6000000000000005).
//    A2 The OFF-BY-ONE, isolated: the derived entry names are exactly
//       [0,0]..[4,0] -- five, not six and not four -- and the source is
//       still there under its own name.
//    B  GRID.  Two rows of three collapse to TWO chunks (row-zero
//       remainder + every later row), because `source` COPIES and the
//       kept copy already occupies cell (0,0).  Same multiset proof.
//    C  NEGATIVE / DEGENERATE steps: a run marching in -X with a constant
//       Y and Z still fits, and the constant components emit as bare
//       literals rather than `+ i*0`.
//    D  REFUSALS, each with the document BYTE-IDENTICAL afterwards:
//       an irregular arrangement, a run with a per-copy `orientation`, a
//       run carrying a `matrix`, a member named by another chunk (a
//       `parent` link), a run below the size floor, and a colliding
//       `name`.
//    E  The note names the verb (the coupling that makes the verb worth
//       more than the note alone) and still carries its anti-churn
//       escape, at BOTH carriers, byte-identically.
//    F  Wire surface: the verb dispatches through AgentRpcDispatcher and
//       is declared in the shared chat-codec tool table.
//
//  RED-PROOF (documented, not automated): breaking the fit -- e.g.
//  emitting `i` where the row chunk emits `(i+1)`, or fitting against
//  member 0 instead of member 1 -- makes case A's multiset assertion
//  FAIL, because every minted copy lands one step short and the topmost
//  original position has no partner.  See the arc log for the counts.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IEnumCallback.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/Json.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	return dir + name;
}

static Job* LoadScene( const std::string& text, const std::string& path )
{
	{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		std::remove( path.c_str() );
		return nullptr;
	}
	return pJob;
}

//----------------------------------------------------------------------
// The world-transform census -- the observable this whole file is built
// on.  ONE entry per object the derive registered, carrying the composed
// world matrix.  Read through IObjectPriv::GetFinalTransformMatrix, the
// same accessor SceneGraphParentTest's composition oracles use, so this
// measures what the renderer will actually see rather than what the
// document says.
//----------------------------------------------------------------------
struct ObjectPose
{
	std::string name;
	Scalar      m[16];
};

class NameCollector : public IEnumCallback<const char*>
{
public:
	std::vector<std::string> names;
	bool operator()( const char* const& n ) override { if( n ) names.push_back( n ); return true; }
};

static std::vector<ObjectPose> CensusPoses( Job& j )
{
	std::vector<ObjectPose> out;
	const IScene* s = j.GetScene();
	if( !s ) return out;
	const IObjectManager* om = s->GetObjects();
	if( !om ) return out;
	NameCollector c;
	om->EnumerateItemNames( c );
	for( const std::string& n : c.names ) {
		IObjectPriv* o = om->GetItem( n.c_str() );
		if( !o ) continue;
		ObjectPose p;
		p.name = n;
		const Matrix4 w = o->GetFinalTransformMatrix();
		const Scalar* src = &w._00;
		for( int i = 0; i < 16; ++i ) p.m[i] = src[i];
		out.push_back( p );
	}
	return out;
}

static bool PoseClose( const ObjectPose& a, const ObjectPose& b, double eps )
{
	for( int i = 0; i < 16; ++i ) {
		const double d = std::fabs( static_cast<double>( a.m[i] ) - static_cast<double>( b.m[i] ) );
		const double tol = eps * std::max( 1.0, std::fabs( static_cast<double>( b.m[i] ) ) );
		if( d > tol ) return false;
	}
	return true;
}

//! THE PROOF.  Greedy multiset match: every BEFORE pose must find an
//! unused AFTER pose within tolerance, and vice versa by construction
//! (equal sizes plus a total matching).  `worst` receives the largest
//! per-element deviation actually seen, so a pass reports HOW exact it
//! was rather than merely that it passed -- a bar the numbers have to
//! clear out loud, not silently.
static bool PoseMultisetsMatch( const std::vector<ObjectPose>& before,
                                const std::vector<ObjectPose>& after,
                                double eps, double& worst, std::string& why )
{
	worst = 0.0;
	if( before.size() != after.size() ) {
		why = "object COUNT changed: " + std::to_string( before.size() ) +
			" -> " + std::to_string( after.size() );
		return false;
	}
	std::vector<bool> used( after.size(), false );
	for( const ObjectPose& b : before ) {
		std::size_t at = after.size();
		for( std::size_t k = 0; k < after.size(); ++k ) {
			if( used[k] ) continue;
			if( PoseClose( b, after[k], eps ) ) { at = k; break; }
		}
		if( at == after.size() ) {
			why = "no surviving object carries `" + b.name + "`'s world transform (origin " +
				std::to_string( static_cast<double>( b.m[12] ) ) + " " +
				std::to_string( static_cast<double>( b.m[13] ) ) + " " +
				std::to_string( static_cast<double>( b.m[14] ) ) + ")";
			return false;
		}
		used[at] = true;
		for( int i = 0; i < 16; ++i )
			worst = std::max( worst, std::fabs( static_cast<double>( b.m[i] ) - static_cast<double>( after[at].m[i] ) ) );
	}
	return true;
}

//----------------------------------------------------------------------
// Fixtures.  Every one derives on its own -- shader, rasterizer, film,
// camera, painter, material, geometry -- so a failure is never "the
// scene did not load".
//----------------------------------------------------------------------
static std::string Preamble()
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
		"lambertian_material\n{\n\tname mat2\n\treflectance pnt\n}\n\n"
		"sphere_geometry\n{\n\tname bottle\n\tradius 0.2\n}\n\n";
}

static std::string Bottle( const std::string& name, const std::string& pos,
                           const std::string& extra = std::string() )
{
	return "standard_object\n{\n\tname " + name + "\n\tgeometry bottle\n\tmaterial mat\n"
	       "\tposition " + pos + "\n" + extra + "}\n\n";
}

//! Six bottles at x = 0.6 + 2.6*k.  The spacing is deliberately a value
//! whose double difference is NOT the decimal an author wrote
//! (3.2 - 0.6 == 2.6000000000000005), so the emitted step is only `2.6`
//! if the fit really does search over the decimal text.
static std::string SceneLine()
{
	std::string s = Preamble();
	const char* const xs[] = { "0.6", "3.2", "5.8", "8.4", "11", "13.6" };
	for( int k = 0; k < 6; ++k )
		s += Bottle( std::string( "b" ) + std::to_string( k ), std::string( xs[k] ) + " 1.5 -2" );
	return s;
}

//! A 3 x 3 grid in document order (i fastest): x in (0.6, 3.2, 5.8),
//! z in (-2, -4.9, -7.8).  THREE rows, not two, deliberately: with two
//! rows the later-rows chunk has countV-1 == 1 and emits no `count_v` at
//! all (the descriptor default), so a two-row fixture could not tell a
//! correct row progression from one that ignored `j`.
static std::string SceneGrid()
{
	std::string s = Preamble();
	const char* const xs[] = { "0.6", "3.2", "5.8" };
	const char* const zs[] = { "-2", "-4.9", "-7.8" };
	int k = 0;
	for( int j = 0; j < 3; ++j )
		for( int i = 0; i < 3; ++i, ++k )
			s += Bottle( std::string( "g" ) + std::to_string( k ),
			             std::string( xs[i] ) + " 1.5 " + zs[j] );
	return s;
}

//----------------------------------------------------------------------

static void TestLineCollapse()
{
	std::printf( "A: LINE -- six copies -> one `source` + `count_u 5`, world transforms unchanged\n" );
	const std::string tmp = TempPath( "collapse_line.RISEscene" );
	Job* pJob = LoadScene( SceneLine(), tmp );
	Check( pJob != nullptr, "A: line fixture derives" );
	if( !pJob ) return;

	const std::vector<ObjectPose> before = CensusPoses( *pJob );
	Check( before.size() == 6, "A: six objects before the collapse" );

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentCollapseResult r = sess->CollapseToInstances();
	Check( r.ok && r.applied, std::string( "A: the no-argument call APPLIED -- " ) + r.message );
	Check( r.status == "applied", "A: status is \"applied\"" );
	Check( r.sourceObject == "b0", "A: the FIRST copy in document order is the one kept as `source`" );
	Check( r.geometry == "bottle", "A: the result names the shared geometry" );
	Check( r.collapsedCount == 6, "A: it reports collapsing all SIX copies" );
	Check( r.countU == 6 && r.countV == 1, "A: fitted as a LINE of six (countV == 1), not a grid" );
	Check( r.instanceChunks.size() == 1, "A: a line takes exactly ONE instancing chunk" );
	Check( r.removedObjects.size() == 5, "A: five copies were replaced (the source stayed)" );

	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "source b0" ) != std::string::npos, "A: the new chunk sources `b0`" );
	Check( docAfter.find( "count_u 5" ) != std::string::npos,
	       "A: count_u is FIVE, not six -- `source` copies, so the still-visible source is the sixth" );
	Check( docAfter.find( "count_v" ) == std::string::npos, "A: a line emits no count_v" );
	// The MONEY assertion on the emitted text: the step is the decimal an
	// author would write, which is only possible if the fit searched the
	// rendered TEXT.  3.2 - 0.6 is 2.6000000000000005 in double.
	Check( docAfter.find( "(i+1)*2.6" ) != std::string::npos,
	       "A: the step emits as the SHORT decimal `2.6` (the raw double difference is "
	       "2.6000000000000005) -- the fit searches the text it will write" );
	Check( docAfter.find( "2.6000000000000005" ) == std::string::npos,
	       "A: ...and the unrounded difference never reaches the document" );
	Check( docAfter.find( "0.6 + (i+1)" ) != std::string::npos,
	       "A: the base is the SOURCE's own position and the index term is `(i+1)` -- the "
	       "off-by-one is written out, not hidden in a shifted base" );
	// The N-1 replaced copies are gone as chunks.
	for( int k = 1; k < 6; ++k ) {
		const std::string nm = "name b" + std::to_string( k ) + "\n";
		Check( docAfter.find( nm ) == std::string::npos,
		       "A: `b" + std::to_string( k ) + "` is no longer an authored chunk" );
	}
	Check( docAfter.find( "name b0\n" ) != std::string::npos, "A: `b0` survives as the source chunk" );
	Check( docBefore != docAfter, "A: the document really changed" );

	// ---- THE PROOF -------------------------------------------------------
	const std::vector<ObjectPose> after = CensusPoses( *pJob );
	double worst = 0.0;
	std::string why;
	const bool ok = PoseMultisetsMatch( before, after, 1e-9, worst, why );
	Check( ok, "A MONEY: the world-transform MULTISET is identical after the collapse -- " + why );
	std::printf( "    A: %d objects before / %d after, worst per-element deviation %.3g\n",
	             static_cast<int>( before.size() ), static_cast<int>( after.size() ), worst );

	// A2 -- the off-by-one in isolation, on the DERIVED entry names.
	{
		const IObjectManager* om = pJob->GetScene() ? pJob->GetScene()->GetObjects() : nullptr;
		Check( om && om->GetItem( "b0" ) != nullptr, "A2: the source renders under its own name" );
		int seen = 0;
		for( int i = 0; i < 8; ++i ) {
			const std::string nm = "b0_array[" + std::to_string( i ) + ",0]";
			if( om && om->GetItem( nm.c_str() ) ) ++seen;
		}
		Check( seen == 5,
		       "A2 MONEY: exactly FIVE minted entries [0,0]..[4,0] -- one fewer would have dropped "
		       "the last copy, one more would have doubled the source" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestGridCollapse()
{
	std::printf( "B: GRID -- 3x3 copies -> two chunks (row-zero remainder + later rows)\n" );
	const std::string tmp = TempPath( "collapse_grid.RISEscene" );
	Job* pJob = LoadScene( SceneGrid(), tmp );
	Check( pJob != nullptr, "B: grid fixture derives" );
	if( !pJob ) return;

	const std::vector<ObjectPose> before = CensusPoses( *pJob );
	Check( before.size() == 9, "B: nine objects before the collapse" );

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentCollapseResult r = sess->CollapseToInstances();
	Check( r.ok && r.applied, std::string( "B: applied -- " ) + r.message );
	Check( r.countU == 3 && r.countV == 3, "B: fitted as a 3 x 3 grid" );
	Check( r.instanceChunks.size() == 2,
	       "B: a grid takes TWO instancing chunks -- `source` COPIES, so the kept copy already "
	       "occupies cell (0,0) and a countU x countV grid plus a visible source would be one "
	       "object too many" );
	Check( r.collapsedCount == 9 && r.removedObjects.size() == 8, "B: eight copies replaced" );

	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "count_u 2" ) != std::string::npos,
	       "B: the row-zero remainder is count_u 2 (three wide, minus the kept cell)" );
	Check( docAfter.find( "count_u 3" ) != std::string::npos &&
	       docAfter.find( "count_v 2" ) != std::string::npos,
	       "B: the later rows are a full-width 3 x (3-1) block" );
	Check( docAfter.find( "(j+1)*" ) != std::string::npos,
	       "B: the second chunk's row index is `(j+1)` -- row zero is already accounted for" );

	const std::vector<ObjectPose> after = CensusPoses( *pJob );
	double worst = 0.0;
	std::string why;
	const bool ok = PoseMultisetsMatch( before, after, 1e-9, worst, why );
	Check( ok, "B MONEY: the world-transform MULTISET is identical after the grid collapse -- " + why );
	std::printf( "    B: %d objects before / %d after, worst per-element deviation %.3g\n",
	             static_cast<int>( before.size() ), static_cast<int>( after.size() ), worst );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestNegativeAndConstantComponents()
{
	std::printf( "C: negative step + constant components\n" );
	std::string body = Preamble();
	// Marching in -X, with a per-copy `scale` that is CONSTANT across the run
	// (so it must be restated on the instancing chunk -- `scale` is not
	// inherited through `source`) and a constant Y/Z.
	const char* const xs[] = { "-1", "-2.5", "-4", "-5.5", "-7" };
	for( int k = 0; k < 5; ++k )
		body += Bottle( std::string( "n" ) + std::to_string( k ),
		                std::string( xs[k] ) + " 0.75 0", "\tscale 1.5 1.5 1.5\n" );

	const std::string tmp = TempPath( "collapse_neg.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "C: negative-step fixture derives" );
	if( !pJob ) return;

	const std::vector<ObjectPose> before = CensusPoses( *pJob );
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentCollapseResult r = sess->CollapseToInstances();
	Check( r.ok && r.applied, std::string( "C: applied -- " ) + r.message );

	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "- (i+1)*1.5" ) != std::string::npos,
	       "C: a NEGATIVE step flips the joining sign rather than emitting `+ (i+1)*-1.5`" );
	Check( docAfter.find( "*0" ) == std::string::npos,
	       "C: a ZERO step drops its whole term rather than emitting `+ (i+1)*0`" );
	Check( docAfter.find( "position expr(" ) != std::string::npos &&
	       docAfter.find( ") 0.75 0\n" ) != std::string::npos,
	       "C: the constant Y and Z components emit as BARE LITERALS, not degenerate exprs" );
	Check( docAfter.find( "scale 1.5 1.5 1.5" ) != std::string::npos,
	       "C: the run's constant `scale` is RESTATED on the instancing chunk -- it is not "
	       "inherited through `source`, so omitting it would silently shrink every copy" );

	const std::vector<ObjectPose> after = CensusPoses( *pJob );
	double worst = 0.0;
	std::string why;
	Check( PoseMultisetsMatch( before, after, 1e-9, worst, why ),
	       "C MONEY: world-transform multiset identical (scale carried through) -- " + why );
	std::printf( "    C: worst per-element deviation %.3g\n", worst );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! Run `body` through the verb and assert it REFUSED with the document
//! byte-identical.  `needle` must appear in the refusal so the test pins
//! WHICH rule fired, not merely that something did -- a fixture rejected
//! by a different rule than the one under test is a green test guarding
//! nothing.
static void ExpectRefusal( const char* label, const std::string& body,
                           const std::string& needle, const std::string& target = std::string(),
                           const std::string& name = std::string() )
{
	const std::string tmp = TempPath( "collapse_refuse.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, std::string( "D/" ) + label + ": fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();
	const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

	const Agent::AgentSession::AgentCollapseResult r = sess->CollapseToInstances( target, name );
	Check( !r.applied, std::string( "D/" ) + label + ": REFUSED (nothing applied)" );
	Check( r.message.find( needle ) != std::string::npos,
	       std::string( "D/" ) + label + ": refused for the RIGHT reason (message contains \"" +
	       needle + "\"), got: " + r.message );
	Check( sess->ReadDocument() == docBefore,
	       std::string( "D/" ) + label + ": the document is BYTE-IDENTICAL after the refusal" );
	Check( sess->HeadVersion() == vBefore,
	       std::string( "D/" ) + label + ": the head version did not move" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestRefusals()
{
	std::printf( "D: refusals -- each specific, each non-mutating\n" );

	// (1) IRREGULAR: five copies whose spacing is not constant.  The middle
	//     one is nudged, so no line fits; five is prime, so no grid does
	//     either.  THE headline refusal -- an array that looked right and sat
	//     one step off is the failure this verb exists to avoid.
	{
		std::string body = Preamble();
		const char* const xs[] = { "0", "1", "2", "3.4", "4" };
		for( int k = 0; k < 5; ++k )
			body += Bottle( std::string( "i" ) + std::to_string( k ), std::string( xs[k] ) + " 1 0" );
		ExpectRefusal( "irregular", body, "not on a regular arrangement" );
	}

	// (2) PER-COPY ORIENTATION.  Fittable positions, but each copy is turned
	//     differently -- and one instancing chunk states `orientation` once
	//     for every copy it mints.
	{
		std::string body = Preamble();
		for( int k = 0; k < 5; ++k )
			body += Bottle( std::string( "o" ) + std::to_string( k ),
			                std::to_string( k ) + " 1 0",
			                "\torientation 0 " + std::to_string( k * 15 ) + " 0\n" );
		ExpectRefusal( "orientation", body, "differ in `orientation`" );
	}

	// (3) A `matrix` ANYWHERE in the run.  A matrix overrides
	//     position/orientation/quaternion/scale, so the differing positions
	//     are not what places these copies and a fitted expr would be
	//     decorative.
	{
		std::string body = Preamble();
		for( int k = 0; k < 5; ++k )
			body += Bottle( std::string( "m" ) + std::to_string( k ), std::to_string( k ) + " 1 0",
			                "\tmatrix 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n" );
		ExpectRefusal( "matrix", body, "carries a `matrix`" );
	}

	// (4) A MEMBER IS NAMED BY ANOTHER CHUNK.  Here the kept copy has a
	//     child: `source` clones a whole SUBTREE, so every minted copy would
	//     arrive with a clone of that child attached -- which is precisely NOT
	//     a no-op, and is invisible in the chunk text.
	{
		std::string body = Preamble();
		for( int k = 0; k < 5; ++k )
			body += Bottle( std::string( "p" ) + std::to_string( k ), std::to_string( k ) + " 1 0" );
		body += "standard_object\n{\n\tname tag\n\tparent p0\n\tgeometry bottle\n\tmaterial mat2\n"
		        "\tposition 0 0.5 0\n}\n\n";
		ExpectRefusal( "referenced", body, "other chunks NAME copies in this run" );
	}

	// (5) BELOW THE FLOOR.  Two copies is not a run worth collapsing.
	{
		std::string body = Preamble();
		body += Bottle( "t0", "0 1 0" );
		body += Bottle( "t1", "1 1 0" );
		ExpectRefusal( "too-small", body, "no run of hand-authored copies to collapse" );
	}

	// (6) An EXPLICIT `name` that is already taken.
	{
		std::string body = Preamble();
		for( int k = 0; k < 5; ++k )
			body += Bottle( std::string( "c" ) + std::to_string( k ), std::to_string( k ) + " 1 0" );
		ExpectRefusal( "name-collision", body, "already exists", std::string(), "mat2" );
	}

	// (7) An explicit `target` that is not a copy at all.
	{
		std::string body = Preamble();
		for( int k = 0; k < 5; ++k )
			body += Bottle( std::string( "q" ) + std::to_string( k ), std::to_string( k ) + " 1 0" );
		ExpectRefusal( "bad-target", body, "is not a geometry-bearing", "no_such_object" );
	}
}

static void TestNoteNamesTheVerb()
{
	std::printf( "E: the design note NAMES the verb, and keeps its escape\n" );
	std::string body = Preamble();
	for( int k = 0; k < 6; ++k )
		body += Bottle( std::string( "b" ) + std::to_string( k ), std::to_string( k ) + " 1 0" );

	const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
	const Agent::AgentDiagnostic* d = nullptr;
	for( const Agent::AgentDiagnostic& e : diags )
		if( e.code == "DESIGN_HAND_REPEATED_COPIES" ) { d = &e; break; }
	Check( d != nullptr, "E: condition C still fires on six hand-authored copies" );
	if( !d ) return;

	Check( d->message.find( "`collapse_to_instances` writes it for you" ) != std::string::npos,
	       "E MONEY: the clause NAMES the verb -- the coupling that makes the verb worth more "
	       "than the note alone (step 1's advice was delivered five times and adopted zero times)" );
	Check( d->message.find( "call it with no arguments" ) != std::string::npos,
	       "E: ...and states the zero-argument call, the lowest-friction form" );
	Check( d->message.find( "REFUSES -- changing nothing" ) != std::string::npos,
	       "E: ...and is honest about the refusal, so trying it is knowably free" );
	Check( d->message.find( "count_u 5" ) != std::string::npos &&
	       d->message.find( "are 6, not 5" ) != std::string::npos,
	       "E: ...while still teaching the off-by-one (unchanged from step 1)" );
	Check( d->message.find( "this is fine -- ignore and do not churn" ) != std::string::npos,
	       "E: ...and the ANTI-CHURN escape survives (load-bearing for conditions A and B)" );

	const std::string note = Agent::AgentSession::ComputeDesignNote( body );
	Check( note.find( d->message ) != std::string::npos,
	       "E: the verbatim-copy invariant holds -- the diagnostic message appears BYTE-IDENTICALLY "
	       "inside the render-result note (one shared formatter, two carriers)" );
}

static void TestWireSurface()
{
	std::printf( "F: wire surface -- RPC dispatch + shared chat-codec tool table\n" );
	{
		const std::string defs = Agent::ChatToolDefsFingerprint();
		Check( defs.find( "collapse_to_instances" ) != std::string::npos,
		       "F: the verb is declared in the shared kToolDefs table (so every provider codec "
		       "carries it -- one table, four formatters)" );
	}

	std::string body = Preamble();
	for( int k = 0; k < 6; ++k )
		body += Bottle( std::string( "b" ) + std::to_string( k ),
		                std::to_string( k ) + " 1 0" );
	const std::string tmp = TempPath( "collapse_rpc.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "F: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	Agent::AgentRpcDispatcher disp( std::move( sess ) );
	const std::string resp = disp.HandleLine(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"collapse_to_instances\",\"params\":{}}" );
	Agent::JsonValue env;
	std::string perr;
	Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "F: the response parses" );
	const Agent::JsonValue& result = env.get( "result" );
	Check( result.get( "applied" ).asBool(), "F: the RPC form applied the collapse" );
	Check( result.get( "source" ).asString() == "b0", "F: the result echoes the kept source" );
	Check( static_cast<int>( result.get( "collapsed" ).asNumber() ) == 6, "F: ...and the run size" );
	Check( result.get( "instanceChunks" ).isArray() && result.get( "instanceChunks" ).size() == 1,
	       "F: ...and names the chunk it minted" );

	pJob->release();
	std::remove( tmp.c_str() );
}

int main()
{
	std::printf( "AgentCollapseInstancesTest -- 88 step 2: collapse_to_instances\n" );
	TestLineCollapse();
	TestGridCollapse();
	TestNegativeAndConstantComponents();
	TestRefusals();
	TestNoteNamesTheVerb();
	TestWireSurface();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
