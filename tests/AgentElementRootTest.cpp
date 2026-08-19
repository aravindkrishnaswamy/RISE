//////////////////////////////////////////////////////////////////////
//
//  AgentElementRootTest.cpp - docs/agentic-redesign/87-recursive-scene-graph.md
//    step 5, the AGENT surface: an element becomes a real node in the
//    recursive scene graph, and `place_element` becomes ONE local
//    transform on that node.
//
//  WHAT THIS FILE EXISTS TO PROVE.  Before 87 there was no parent to move,
//  so `place_element` composed the placement ARITHMETICALLY into every
//  object recorded against the element.  That forced four defects, and all
//  four were consequences of the missing node rather than of the verb:
//
//    1. Rotation was APPROXIMATE -- it added Euler triples per axis
//       (`oldRot[i] + rot[i]`), which is the right answer only when both
//       rotations are about one shared axis.  The old code knew, and said so
//       in the result message.
//    2. A `quaternion`-authored object could not be rotated AT ALL
//       (`quaternion` outranks `orientation`), so it was named and skipped.
//    3. Scale was UNIFORM-ONLY, by explicit refusal.
//    4. It was DESTRUCTIVE -- the author's own numbers were overwritten, so
//       re-placing compounded the error.
//
//  Case B is the numeric proof for 1: it composes one element by hand from
//  first principles (scale, then Euler rotation, then translation; child
//  first, then parent) and asserts the ENGINE's world matrix matches to
//  1e-9, then computes what the Euler-sum formula would have produced on
//  the SAME scene and reports how far off it is.  Cases C and D retire 2
//  and 3; case E retires 4.
//
//  Cases:
//    A -- the root node is minted with the element's FIRST object, is a
//         geometry-less container, and every later object of that element
//         is parented to it -- including through an authored sub-hierarchy.
//    B -- THE NUMERIC PROOF: a rotated, non-uniformly-scaled placement over
//         a child that carries a rotation of its own composes EXACTLY,
//         where Euler addition does not.
//    C -- a `quaternion` child and a `matrix` child are CARRIED, not skipped.
//    D -- non-uniform `scale` is admitted; a zero component is still refused.
//    E -- IDEMPOTENCE: placing twice with the same arguments is a no-op, and
//         the objects' OWN authored params are never rewritten.
//    F -- a `csg_object` in an element: its operands are detached from the
//         root (the engine refuses a parented operand) and the COMPOSITE is
//         parented instead, so the composite is carried -- which the pre-87
//         verb never moved at all.
//    G -- the refusal that replaces the four that are gone: an element with
//         no root node says so, names the root it looked for, and changes
//         nothing.
//    H -- the reported bounding box excludes the container root.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::Implementation;

static int gChecks = 0;
static int gFails  = 0;

static void Check( bool cond, const std::string& what )
{
	++gChecks;
	if( !cond ) { ++gFails; std::printf( "  FAILED: %s\n", what.c_str() ); }
}

//----------------------------------------------------------------------
// Fixture scaffolding, deliberately the same shape AgentChunkCrudTest uses
// so a reader moving between the two files is not decoding two idioms.
//----------------------------------------------------------------------

static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n";

static std::string TempPath( const char* leaf )
{
	const char* t = std::getenv( "TMPDIR" );
	std::string dir = ( t && *t ) ? t : "/tmp/";
	if( dir.empty() || dir[ dir.size() - 1 ] != '/' ) dir += '/';
	return dir + leaf;
}

static Job* LoadScene( const std::string& path )
{
	{ std::ofstream o( path.c_str(), std::ios::binary ); o << kScene; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		std::remove( path.c_str() );
		return nullptr;
	}
	return pJob;
}

static std::unique_ptr<Agent::AgentSession> WrapBuilding( Job* pJob )
{
	Agent::AgentSession::SetBuildPlanGateDefaultEnabled( true );
	std::unique_ptr<Agent::AgentSession> s = Agent::AgentSession::WrapJobWithSessionMode(
		pJob, Agent::AgentSession::AgentSessionMode::Building );
	Agent::AgentSession::SetBuildPlanGateDefaultEnabled( false );
	return s;
}

static std::vector<Agent::AgentSession::AgentBuildPlanEntry> OnePartPlan( const char* element )
{
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> p;
	Agent::AgentSession::AgentBuildPlanEntry a;
	a.element      = element;
	a.construction = "primitive";
	a.pieces.push_back( "piece" );
	a.outline      = "0 0; 2 0; 2 2; 0 2";
	p.push_back( a );
	return p;
}

static IObjectPriv* Obj( Job& j, const std::string& name )
{
	const IScene* s = j.GetScene();
	if( !s ) return nullptr;
	const IObjectManager* om = s->GetObjects();
	return om ? om->GetItem( name.c_str() ) : nullptr;
}

//! Does the document declare a param `pname` on the chunk named `cname`, and
//! with what value?  Reads the SESSION's document text, so this is what an
//! author (and the next derive) would see, not a live-object readback.
static std::string DocParam( Agent::AgentSession& sess, const std::string& role,
                             const std::string& cname, const std::string& pname,
                             bool* outFoundChunk = nullptr )
{
	if( outFoundChunk ) *outFoundChunk = false;
	const RISE::Cst::Document doc = RISE::Cst::ParseToCst( sess.ReadDocument() );
	const int n = RISE::Cst::DocItemCount( doc );
	for( int i = 0; i < n; ++i ) {
		const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
		if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
		if( it->role != role ) continue;
		std::string thisName, want;
		bool haveWant = false;
		for( const RISE::Cst::NodeRef& kid : it->kids ) {
			if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
			std::string pn, val;
			for( const RISE::Cst::NodeRef& tk : kid->kids ) {
				if( !tk || tk->kind != RISE::Cst::NodeKind::Token ) continue;
				if( tk->role == "pname" ) pn = tk->text;
				else if( tk->role == "pvalue" ) { if( !val.empty() ) val += ' '; val += tk->text; }
			}
			if( pn == "name" )  thisName = val;
			if( pn == pname ) { want = val; haveWant = true; }
		}
		if( thisName != cname ) continue;
		if( outFoundChunk ) *outFoundChunk = true;
		return haveWant ? want : std::string();
	}
	return std::string();
}

//----------------------------------------------------------------------
// The hand-composed oracle for case B.
//
// Written out from first principles rather than by calling Matrix4Ops, so
// that a change in the engine's composition cannot make the oracle agree
// with it by construction.  Two facts about the convention, both READ OFF
// the tree rather than assumed, and both independently pinned by
// SceneGraphParentTest case A:
//
//   * ROW-VECTOR.  A world matrix carries its translation in row 3
//     (`m._30/_31/_32` is where the node's local origin lands), so rows
//     0..2 are the images of the local basis vectors.
//   * `local = P * O * Stretch * S` applied in that reading order means
//     SCALE acts first, then the Euler rotation, then the translation; and
//     `world = parent.world * local` means the CHILD's local transform acts
//     before the parent's.  SceneGraphParentTest A3 is the pin: a leaf at
//     local (1,0,0) under a parent scaled 2 lands 2 units out, not 1.
//   * The Euler triple is applied X first, then Y, then Z (the composition
//     Transformable::SetOrientation builds).
//----------------------------------------------------------------------

static void RotEulerDeg( const double deg[3], const double p[3], double out[3] )
{
	const double d2r = 3.14159265358979323846 / 180.0;
	const double cx = std::cos( deg[0]*d2r ), sx = std::sin( deg[0]*d2r );
	const double cy = std::cos( deg[1]*d2r ), sy = std::sin( deg[1]*d2r );
	const double cz = std::cos( deg[2]*d2r ), sz = std::sin( deg[2]*d2r );
	double x = p[0], y = p[1], z = p[2];
	// X
	double ny = cx*y - sx*z, nz = sx*y + cx*z;   y = ny; z = nz;
	// Y
	double nx = cy*x + sy*z;  nz = -sy*x + cy*z; x = nx; z = nz;
	// Z
	nx = cz*x - sz*y;  ny = sz*x + cz*y;
	out[0] = nx; out[1] = ny; out[2] = z;
}

//! Push one local point through child-local then parent-local, by hand.
static void ComposeByHand( const double childRot[3], const double childPos[3],
                           const double rootScale[3], const double rootRot[3],
                           const double rootPos[3],
                           const double localPoint[3], bool isDirection,
                           double out[3] )
{
	// Child local: scale (none here) -> rotate -> translate.
	double q[3];
	RotEulerDeg( childRot, localPoint, q );
	if( !isDirection ) { q[0] += childPos[0]; q[1] += childPos[1]; q[2] += childPos[2]; }
	// Root local: scale -> rotate -> translate.
	const double sq[3] = { q[0]*rootScale[0], q[1]*rootScale[1], q[2]*rootScale[2] };
	RotEulerDeg( rootRot, sq, out );
	if( !isDirection ) { out[0] += rootPos[0]; out[1] += rootPos[1]; out[2] += rootPos[2]; }
}

static double MaxAbsDiff3( const double a[3], const double b[3] )
{
	double m = 0.0;
	for( int k = 0; k < 3; ++k ) {
		const double d = std::fabs( a[k] - b[k] );
		if( d > m ) m = d;
	}
	return m;
}

//======================================================================
// A -- the root node itself.
//======================================================================
static void TestRootNodeIsMinted()
{
	std::printf( "A: the element root is minted with the FIRST object and every later object is "
	             "parented to it...\n" );
	const std::string tmp = TempPath( "agent_elem_root_a.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "A fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "wizard" ) ).ok, "A the plan files" );

	const std::string root = Agent::AgentSession::ElementRootName( "wizard" );
	Check( root == "wizard_element_root", "A the root name is <prefix>element_root" );

	// A non-object chunk must NOT mint a root: a painter has no transform.
	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname wizard_pnt\n\tcolor 0.4 0.3 0.6\n}" ).applied,
		"A a painter inserts" );
	{
		bool found = false;
		DocParam( *sess, "standard_object", root, "name", &found );
		Check( !found,
		       "A MONEY ASSERTION: a painter does NOT mint the root -- the root is minted by the "
		       "first chunk that has a transform to compose, not by the first chunk of any kind" );
	}

	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname wizard_mat\n\treflectance wizard_pnt\n}" ).applied,
		"A a material inserts" );
	Check( sess->InsertChunk(
		"box_geometry\n{\n\tname wizard_body_geo\n\twidth 1\n\theight 2\n\tdepth 1\n}" ).applied,
		"A a geometry inserts" );

	Check( sess->InsertChunk(
		"standard_object\n{\n\tname wizard_body\n\tgeometry wizard_body_geo\n"
		"\tmaterial wizard_mat\n\tposition 0 1 0\n}" ).applied,
		"A the first object inserts" );
	{
		bool found = false;
		const std::string geom = DocParam( *sess, "standard_object", root, "geometry", &found );
		Check( found, "A MONEY ASSERTION: the first OBJECT minted the element's root node" );
		Check( geom.empty(),
		       "A the root is a pure CONTAINER -- no `geometry`, so it is world-invisible and "
		       "takes no surface binding" );
	}
	Check( DocParam( *sess, "standard_object", "wizard_body", "parent" ) == root,
	       "A MONEY ASSERTION: the object carries `parent <root>`, so the element is a real subtree "
	       "in the authored graph and not a session-only ledger" );

	// DOCUMENT ORDER is the load-bearing property: `parent` is
	// declare-before-use, and Object-tier chunks APPEND, so a root minted
	// after its children would be a forward reference the derive refuses.
	{
		const std::string doc = sess->ReadDocument();
		const std::size_t rootAt = doc.find( "name " + root );
		const std::size_t bodyAt = doc.find( "name wizard_body\n" );
		Check( rootAt != std::string::npos && bodyAt != std::string::npos && rootAt < bodyAt,
		       "A MONEY ASSERTION: the root is declared BEFORE its first child in the document" );
	}

	// A second object reuses the SAME root; it is not re-minted.
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname wizard_hat\n\tgeometry wizard_body_geo\n"
		"\tmaterial wizard_mat\n\tposition 0 2.2 0\n\tscale 1.4 0.3 1.4\n}" ).applied,
		"A a second object inserts" );
	Check( DocParam( *sess, "standard_object", "wizard_hat", "parent" ) == root,
	       "A the second object is parented to the same root" );
	{
		const std::string doc = sess->ReadDocument();
		std::size_t at = 0, n = 0;
		while( ( at = doc.find( "name " + root, at ) ) != std::string::npos ) { ++n; at += 1; }
		Check( n == 1, "A the root is minted ONCE, not once per object" );
	}

	// An AUTHORED parent inside the element wins, and still rides the root
	// transitively -- which is what makes real internal hierarchy possible.
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname wizard_brim\n\tgeometry wizard_body_geo\n"
		"\tmaterial wizard_mat\n\tparent wizard_hat\n\tposition 0 0.2 0\n}" ).applied,
		"A a hand-parented object inserts" );
	Check( DocParam( *sess, "standard_object", "wizard_brim", "parent" ) == "wizard_hat",
	       "A MONEY ASSERTION: an AUTHORED `parent` is never overruled -- the harness supplies a "
	       "default parent, it does not take the choice away" );

	const Agent::AgentSession::AgentPlaceElementResult pr =
		sess->PlaceElement( "wizard", "5 0 -2" );
	Check( pr.ok, "A the placement applies (" + pr.message + ")" );
	Check( pr.root == root, "A the result names the root it wrote to" );
	Check( pr.objects.size() == 3 && pr.skipped.empty(),
	       "A MONEY ASSERTION: all three objects are carried, including the one two levels down -- "
	       "the reach is established by WALKING the live parent links, not assumed" );
	Check( pr.patchResults.size() == 3,
	       "A the whole placement is THREE patches on ONE chunk, not up to three per object" );
	// The authored numbers are untouched: placement is now separable from
	// construction, which is defect 4's real cost.
	Check( DocParam( *sess, "standard_object", "wizard_body", "position" ) == "0 1 0" &&
	       DocParam( *sess, "standard_object", "wizard_hat",  "position" ) == "0 2.2 0" &&
	       DocParam( *sess, "standard_object", "wizard_hat",  "scale" )    == "1.4 0.3 1.4",
	       "A MONEY ASSERTION: NOT ONE authored parameter was rewritten by the placement" );
	{
		IObjectPriv* brim = Obj( *pJob, "wizard_brim" );
		Check( brim != nullptr, "A the deepest child resolves" );
		if( brim ) {
			const Matrix4 w = brim->GetFinalTransformMatrix();
			Check( std::fabs( w._30 - 5.0 ) < 1e-9 && std::fabs( w._32 + 2.0 ) < 1e-9,
			       "A the deepest child moved with the root" );
		}
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

//======================================================================
// B -- THE NUMERIC PROOF.
//======================================================================
static void TestExactComposition()
{
	std::printf( "B: a rotated, non-uniformly-scaled placement composes EXACTLY through the root, "
	             "where Euler addition does not...\n" );
	const std::string tmp = TempPath( "agent_elem_root_b.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "B fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "arm" ) ).ok, "B the plan files" );

	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname arm_pnt\n\tcolor 0.5 0.5 0.5\n}" ).applied, "B painter" );
	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname arm_mat\n\treflectance arm_pnt\n}" ).applied, "B material" );
	Check( sess->InsertChunk(
		"box_geometry\n{\n\tname arm_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}" ).applied, "B geometry" );

	// THE CHILD CARRIES A ROTATION OF ITS OWN, about a DIFFERENT axis than
	// the placement's.  That is what separates matrix composition from Euler
	// addition: rotations about different axes do not commute, so the sum of
	// two Euler triples is not the composition of their rotations.
	const double childRot[3] = { 0.0, 0.0, 55.0 };
	const double childPos[3] = { 1.0, 2.0, 0.0 };
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname arm_link\n\tgeometry arm_geo\n\tmaterial arm_mat\n"
		"\tposition 1 2 0\n\torientation 0 0 55\n}" ).applied, "B the child inserts" );

	const double rootPos[3]   = { 5.0, -1.0, 3.0 };
	const double rootRot[3]   = { 37.0, 0.0, 0.0 };
	const double rootScale[3] = { 2.0, 1.0, 3.0 };

	const Agent::AgentSession::AgentPlaceElementResult pr =
		sess->PlaceElement( "arm", "5 -1 3", "2 1 3", "37 0 0" );
	Check( pr.ok, "B the placement applies (" + pr.message + ")" );
	Check( pr.skipped.empty(), "B nothing is skipped" );

	IObjectPriv* link = Obj( *pJob, "arm_link" );
	Check( link != nullptr, "B the child resolves" );
	if( !link ) { sess.reset(); pJob->release(); std::remove( tmp.c_str() ); return; }

	const Matrix4 w = link->GetFinalTransformMatrix();

	// -------- the ORIGIN, against the hand-composed chain --------
	{
		const double zero[3] = { 0.0, 0.0, 0.0 };
		double expect[3];
		ComposeByHand( childRot, childPos, rootScale, rootRot, rootPos, zero, false, expect );
		const double got[3] = { w._30, w._31, w._32 };
		const double err = MaxAbsDiff3( got, expect );
		std::printf( "    world origin: engine (%.12f, %.12f, %.12f)\n", got[0], got[1], got[2] );
		std::printf( "                  hand   (%.12f, %.12f, %.12f)   max|diff| = %.3e\n",
		             expect[0], expect[1], expect[2], err );
		Check( err < 1e-9,
		       "B MONEY ASSERTION: the composed world ORIGIN matches the hand-composed chain to 1e-9" );
	}

	// -------- the LINEAR part, basis vector by basis vector --------
	{
		const double basis[3][3] = { { 1,0,0 }, { 0,1,0 }, { 0,0,1 } };
		const double rows[3][3]  = { { w._00, w._01, w._02 },
		                             { w._10, w._11, w._12 },
		                             { w._20, w._21, w._22 } };
		double worst = 0.0;
		for( int b = 0; b < 3; ++b ) {
			double expect[3];
			ComposeByHand( childRot, childPos, rootScale, rootRot, rootPos, basis[b], true, expect );
			const double err = MaxAbsDiff3( rows[b], expect );
			if( err > worst ) worst = err;
			std::printf( "    e%d -> engine (%.12f, %.12f, %.12f)  hand (%.12f, %.12f, %.12f)\n",
			             b, rows[b][0], rows[b][1], rows[b][2], expect[0], expect[1], expect[2] );
		}
		std::printf( "    linear part max|diff| = %.3e\n", worst );
		Check( worst < 1e-9,
		       "B MONEY ASSERTION: every basis vector's image matches the hand-composed chain to "
		       "1e-9 -- including the SHEAR a non-uniform parent scale between two rotations "
		       "produces, which no per-object Euler/scale patch can express at all" );
	}

	// -------- what the pre-87 Euler-SUM formula would have produced --------
	//
	// The old code wrote `orientation = oldRot + rot` onto the child and
	// scaled/rotated its position.  Reproduced here on the same numbers, with
	// the scale made UNIFORM (2 2 2) so the comparison is against a placement
	// the old verb would actually have ACCEPTED -- a non-uniform scale it
	// refused outright.  The gap is the size of defect 1.
	{
		const double uniform[3] = { 2.0, 2.0, 2.0 };
		const double sumRot[3]  = { childRot[0] + rootRot[0],
		                            childRot[1] + rootRot[1],
		                            childRot[2] + rootRot[2] };
		const double ex[3] = { 1.0, 0.0, 0.0 };
		double truth[3], oldWay[3];
		ComposeByHand( childRot, childPos, uniform, rootRot, rootPos, ex, true, truth );
		RotEulerDeg( sumRot, ex, oldWay );
		oldWay[0] *= uniform[0]; oldWay[1] *= uniform[1]; oldWay[2] *= uniform[2];
		const double gap = MaxAbsDiff3( truth, oldWay );
		std::printf( "    Euler-SUM check (uniform scale 2, the case the old verb accepted):\n"
		             "      true  R_root(S*R_child(ex)) = (%.9f, %.9f, %.9f)\n"
		             "      old   S*R(childRot+rootRot)(ex) = (%.9f, %.9f, %.9f)\n"
		             "      max|diff| = %.6f\n",
		             truth[0], truth[1], truth[2], oldWay[0], oldWay[1], oldWay[2], gap );
		Check( gap > 0.1,
		       "B MONEY ASSERTION: the Euler-SUM formula the pre-87 verb used is measurably WRONG "
		       "on this scene -- so the exactness above is a real change of answer, not a "
		       "restatement of the same arithmetic in another place" );
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

//======================================================================
// C -- quaternion and matrix children are carried, not skipped.
//======================================================================
static void TestQuaternionAndMatrixChildren()
{
	std::printf( "C: a `quaternion` child and a `matrix` child are CARRIED (both were refused "
	             "before)...\n" );
	const std::string tmp = TempPath( "agent_elem_root_c.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "C fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "rig" ) ).ok, "C the plan files" );

	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname rig_pnt\n\tcolor 0.5 0.5 0.5\n}" ).applied, "C painter" );
	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname rig_mat\n\treflectance rig_pnt\n}" ).applied, "C material" );
	Check( sess->InsertChunk(
		"box_geometry\n{\n\tname rig_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}" ).applied, "C geometry" );

	// A quaternion child: 90 degrees about Z, glTF xyzw.
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname rig_quat\n\tgeometry rig_geo\n\tmaterial rig_mat\n"
		"\tposition 2 0 0\n\tquaternion 0 0 0.7071067811865476 0.7071067811865476\n}" ).applied,
		"C the quaternion child inserts" );
	// A matrix child: identity linear part with a translation, so its own
	// pose is unambiguous whichever way the 16 doubles are read.
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname rig_mtx\n\tgeometry rig_geo\n\tmaterial rig_mat\n"
		"\tmatrix 1 0 0 0  0 1 0 0  0 0 1 0  0 3 0 1\n}" ).applied,
		"C the matrix child inserts" );

	Check( DocParam( *sess, "standard_object", "rig_quat", "parent" ) ==
	       Agent::AgentSession::ElementRootName( "rig" ), "C the quaternion child is parented" );
	Check( DocParam( *sess, "standard_object", "rig_mtx", "parent" ) ==
	       Agent::AgentSession::ElementRootName( "rig" ),
	       "C MONEY ASSERTION: the MATRIX child is parented too -- a `matrix` is the node's LOCAL "
	       "transform and composes with the parent's, where the pre-87 verb had to skip it because "
	       "it bypasses the position/orientation/scale params that verb patched" );

	// Where each child sits BEFORE the placement, so the assertions below are
	// about the placement's effect and not about the authored pose.
	double quatBefore[3] = { 0, 0, 0 }, mtxBefore[3] = { 0, 0, 0 };
	{
		IObjectPriv* q = Obj( *pJob, "rig_quat" );
		IObjectPriv* m = Obj( *pJob, "rig_mtx" );
		Check( q && m, "C both children resolve before the placement" );
		if( q ) { const Matrix4 a = q->GetFinalTransformMatrix();
		          quatBefore[0] = a._30; quatBefore[1] = a._31; quatBefore[2] = a._32; }
		if( m ) { const Matrix4 a = m->GetFinalTransformMatrix();
		          mtxBefore[0] = a._30; mtxBefore[1] = a._31; mtxBefore[2] = a._32; }
	}

	const Agent::AgentSession::AgentPlaceElementResult pr =
		sess->PlaceElement( "rig", "0 0 0", "", "0 90 0" );
	Check( pr.ok, "C the placement applies (" + pr.message + ")" );
	Check( pr.skipped.empty(),
	       "C MONEY ASSERTION: NOTHING is skipped -- the `matrix` skip and the `quaternion` "
	       "not-rotated skip are both gone, because neither was ever a property of the scene, only "
	       "of patching each object's own params" );
	Check( pr.objects.size() == 2, "C both children are reported as carried" );
	Check( pr.message.find( "Euler" ) == std::string::npos &&
	       pr.message.find( "ADDED" ) == std::string::npos,
	       "C the message no longer warns about approximated rotation, because none happens" );

	// A 90-degree yaw sends +X to -Z and +Z to +X (X first, then Y, then Z;
	// only Y is non-zero here).  Both children must obey it.
	{
		IObjectPriv* q = Obj( *pJob, "rig_quat" );
		IObjectPriv* m = Obj( *pJob, "rig_mtx" );
		if( q && m ) {
			const Matrix4 qa = q->GetFinalTransformMatrix();
			const Matrix4 ma = m->GetFinalTransformMatrix();
			const double rot[3] = { 0.0, 90.0, 0.0 };
			double qExpect[3], mExpect[3];
			RotEulerDeg( rot, quatBefore, qExpect );
			RotEulerDeg( rot, mtxBefore,  mExpect );
			const double qGot[3] = { qa._30, qa._31, qa._32 };
			const double mGot[3] = { ma._30, ma._31, ma._32 };
			std::printf( "    quaternion child: (%.9f, %.9f, %.9f) -> (%.9f, %.9f, %.9f)\n",
			             quatBefore[0], quatBefore[1], quatBefore[2], qGot[0], qGot[1], qGot[2] );
			std::printf( "    matrix     child: (%.9f, %.9f, %.9f) -> (%.9f, %.9f, %.9f)\n",
			             mtxBefore[0], mtxBefore[1], mtxBefore[2], mGot[0], mGot[1], mGot[2] );
			Check( MaxAbsDiff3( qGot, qExpect ) < 1e-9,
			       "C the quaternion child's origin is rotated exactly by the placement" );
			Check( MaxAbsDiff3( mGot, mExpect ) < 1e-9,
			       "C the matrix child's origin is rotated exactly by the placement" );
			// The quaternion child's own rotation must SURVIVE the placement:
			// under the pre-87 verb it was left alone entirely.
			Check( std::fabs( qa._00 ) < 1e-9 || std::fabs( qa._01 ) < 1e-9,
			       "C the quaternion child's own rotation is still in its world matrix" );
		}
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

//======================================================================
// D -- non-uniform scale admitted; a zero component still refused.
//======================================================================
static void TestScaleForms()
{
	std::printf( "D: `scale` takes one factor or three; a zero component is still refused...\n" );
	const std::string tmp = TempPath( "agent_elem_root_d.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "D fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "slab" ) ).ok, "D the plan files" );
	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname slab_pnt\n\tcolor 0.5 0.5 0.5\n}" ).applied, "D painter" );
	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname slab_mat\n\treflectance slab_pnt\n}" ).applied, "D material" );
	Check( sess->InsertChunk(
		"box_geometry\n{\n\tname slab_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}" ).applied, "D geometry" );
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname slab_obj\n\tgeometry slab_geo\n\tmaterial slab_mat\n"
		"\tposition 1 0 0\n}" ).applied, "D object" );

	const std::string root = Agent::AgentSession::ElementRootName( "slab" );

	const Agent::AgentSession::AgentPlaceElementResult uniform =
		sess->PlaceElement( "slab", "0 0 0", "2" );
	Check( uniform.ok, "D one factor still means uniform" );
	Check( DocParam( *sess, "standard_object", root, "scale" ) == "2 2 2",
	       "D a single factor is written to all three axes" );

	const Agent::AgentSession::AgentPlaceElementResult nonUniform =
		sess->PlaceElement( "slab", "0 0 0", "3 0.5 1" );
	Check( nonUniform.ok,
	       "D MONEY ASSERTION: a NON-UNIFORM scale is admitted -- a parent node expresses it "
	       "directly, so the refusal that existed only because per-object arithmetic could not "
	       "is lifted (" + nonUniform.message + ")" );
	Check( DocParam( *sess, "standard_object", root, "scale" ) == "3 0.5 1",
	       "D the three factors reach the root verbatim" );
	{
		IObjectPriv* o = Obj( *pJob, "slab_obj" );
		Check( o != nullptr, "D the object resolves" );
		if( o ) {
			const Matrix4 w = o->GetFinalTransformMatrix();
			Check( std::fabs( w._30 - 3.0 ) < 1e-9,
			       "D the child's own offset (1,0,0) is scaled by the X factor alone" );
		}
	}

	// STILL REFUSED, and for a stated reason: a zero component makes the
	// composed matrix singular for the whole subtree, and Matrix4Ops::Inverse
	// returns its input unchanged at det == 0 (87 section 4).
	{
		const std::string before = sess->ReadDocument();
		const Agent::AgentSession::AgentPlaceElementResult zero =
			sess->PlaceElement( "slab", "0 0 0", "1 0 1" );
		Check( !zero.ok, "D a zero scale component is refused" );
		Check( zero.message.find( "singular" ) != std::string::npos,
		       "D and the refusal says WHY -- the composed transform goes singular for everything "
		       "under the root" );
		Check( sess->ReadDocument() == before, "D with the document byte-identical" );
	}
	{
		const std::string before = sess->ReadDocument();
		const Agent::AgentSession::AgentPlaceElementResult bad =
			sess->PlaceElement( "slab", "0 0 0", "1 2" );
		Check( !bad.ok, "D a two-component scale is refused" );
		Check( sess->ReadDocument() == before, "D with the document byte-identical" );
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

//======================================================================
// E -- idempotence.
//======================================================================
static void TestIdempotence()
{
	std::printf( "E: placing twice with the same arguments is a NO-OP...\n" );
	const std::string tmp = TempPath( "agent_elem_root_e.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "E fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "post" ) ).ok, "E the plan files" );
	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname post_pnt\n\tcolor 0.5 0.5 0.5\n}" ).applied, "E painter" );
	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname post_mat\n\treflectance post_pnt\n}" ).applied, "E material" );
	Check( sess->InsertChunk(
		"box_geometry\n{\n\tname post_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}" ).applied, "E geometry" );
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname post_obj\n\tgeometry post_geo\n\tmaterial post_mat\n"
		"\tposition 0 1 0\n}" ).applied, "E object" );

	Check( sess->PlaceElement( "post", "5 0 -2", "2", "0 30 0" ).ok, "E the first placement applies" );
	const std::string afterFirst = sess->ReadDocument();
	double first[3] = { 0, 0, 0 };
	{
		IObjectPriv* o = Obj( *pJob, "post_obj" );
		Check( o != nullptr, "E the object resolves" );
		if( o ) { const Matrix4 w = o->GetFinalTransformMatrix();
		          first[0] = w._30; first[1] = w._31; first[2] = w._32; }
	}

	Check( sess->PlaceElement( "post", "5 0 -2", "2", "0 30 0" ).ok, "E the second placement applies" );
	Check( sess->ReadDocument() == afterFirst,
	       "E MONEY ASSERTION: the document is BYTE-IDENTICAL after the repeat -- the pre-87 verb "
	       "read each object's CURRENT pose and added to it, so a repeat doubled the translation, "
	       "squared the scale and added the rotation again" );
	{
		IObjectPriv* o = Obj( *pJob, "post_obj" );
		if( o ) {
			const Matrix4 w = o->GetFinalTransformMatrix();
			const double second[3] = { w._30, w._31, w._32 };
			std::printf( "    after 1st (%.9f, %.9f, %.9f); after 2nd (%.9f, %.9f, %.9f)\n",
			             first[0], first[1], first[2], second[0], second[1], second[2] );
			Check( MaxAbsDiff3( first, second ) < 1e-12, "E and so is the composed world transform" );
		}
	}

	// A DIFFERENT placement replaces rather than compounds.
	Check( sess->PlaceElement( "post", "0 0 0" ).ok, "E a third, different placement applies" );
	Check( DocParam( *sess, "standard_object", Agent::AgentSession::ElementRootName( "post" ),
	                 "scale" ) == "1 1 1",
	       "E MONEY ASSERTION: an omitted `scale` RESETS to 1 rather than leaving the previous "
	       "call's 2 in place -- every param is written every call, which is what makes the "
	       "placement a statement about where the element IS" );

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

//======================================================================
// F -- a csg_object in an element.
//======================================================================
static void TestCsgElement()
{
	std::printf( "F: a csg_object's operands are detached from the root and the COMPOSITE is "
	             "parented instead...\n" );
	const std::string tmp = TempPath( "agent_elem_root_f.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "F fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "lens" ) ).ok, "F the plan files" );
	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname lens_pnt\n\tcolor 0.5 0.5 0.5\n}" ).applied, "F painter" );
	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname lens_mat\n\treflectance lens_pnt\n}" ).applied, "F material" );
	Check( sess->InsertChunk(
		"sphere_geometry\n{\n\tname lens_geo\n\tradius 1\n}" ).applied, "F geometry" );
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname lens_a\n\tgeometry lens_geo\n\tmaterial lens_mat\n"
		"\tposition -0.5 0 0\n}" ).applied, "F operand A inserts" );
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname lens_b\n\tgeometry lens_geo\n\tmaterial lens_mat\n"
		"\tposition 0.5 0 0\n}" ).applied, "F operand B inserts" );

	const std::string root = Agent::AgentSession::ElementRootName( "lens" );
	Check( DocParam( *sess, "standard_object", "lens_a", "parent" ) == root,
	       "F the operands-to-be are parented like any other object at the time they are written" );

	// THE ENGINE'S OWN RULE: `Job::AddCSGObject` refuses a parented operand
	// ("an operand's transform is interpreted in this csg_object's frame, not
	// the world's.  Parent the csg_object instead").  Without the repair this
	// insert is refused and a csg-construction element cannot be built at all.
	const Agent::AgentChunkResult cr = sess->InsertChunk(
		"csg_object\n{\n\tname lens_comp\n\tobja lens_a\n\tobjb lens_b\n"
		"\toperation intersection\n\tmaterial lens_mat\n}" );
	Check( cr.applied,
	       "F MONEY ASSERTION: the composite INSERTS -- the operands were detached from the root "
	       "first, which is exactly what the engine's own refusal message prescribes (" +
	       cr.message + ")" );
	Check( DocParam( *sess, "standard_object", "lens_a", "parent" ) == "none" &&
	       DocParam( *sess, "standard_object", "lens_b", "parent" ) == "none",
	       "F both operands were detached, and only they" );
	Check( DocParam( *sess, "csg_object", "lens_comp", "parent" ) == root,
	       "F the COMPOSITE carries the parent instead" );

	const Agent::AgentSession::AgentPlaceElementResult pr =
		sess->PlaceElement( "lens", "4 0 0" );
	Check( pr.ok, "F the placement applies (" + pr.message + ")" );
	bool carriesComposite = false;
	for( std::size_t i = 0; i < pr.objects.size(); ++i )
		if( pr.objects[i] == "lens_comp" ) carriesComposite = true;
	Check( carriesComposite,
	       "F MONEY ASSERTION: the COMPOSITE is carried by the placement -- the pre-87 verb "
	       "transformed `standard_object` chunks only, so a csg-construction element was never "
	       "moved by place_element at all" );
	Check( pr.skipped.size() == 2,
	       "F and the two operands are NAMED as not carried rather than silently dropped" );
	{
		IObjectPriv* c = Obj( *pJob, "lens_comp" );
		Check( c != nullptr, "F the composite resolves" );
		if( c ) {
			const Matrix4 w = c->GetFinalTransformMatrix();
			Check( std::fabs( w._30 - 4.0 ) < 1e-9, "F and it actually moved" );
		}
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

//======================================================================
// G -- the refusal that replaces the four that are gone.
//======================================================================
static void TestNoRootRefusal()
{
	std::printf( "G: an element with no root node says so, and changes nothing...\n" );
	const std::string tmp = TempPath( "agent_elem_root_g.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "G fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "ghost" ) ).ok, "G the plan files" );
	// A painter only: nothing with a transform, so no root.
	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname ghost_pnt\n\tcolor 0.2 0.2 0.2\n}" ).applied, "G painter" );

	const std::string before = sess->ReadDocument();
	const Agent::AgentSession::AgentPlaceElementResult pr = sess->PlaceElement( "ghost", "1 2 3" );
	Check( !pr.ok, "G the placement refuses" );
	Check( pr.message.find( "ghost_element_root" ) != std::string::npos,
	       "G MONEY ASSERTION: the refusal NAMES the root it looked for and says when one is "
	       "created -- silently doing nothing on a document whose objects have no parent is the "
	       "one outcome this must never have" );
	Check( sess->ReadDocument() == before, "G with the document byte-identical" );

	// AND THE LAST SILENT NO-OP: a root someone has given a `matrix` or a
	// `quaternion` would swallow what this verb writes (transform precedence
	// is `matrix` > `quaternion` > `orientation`).  That is the pre-87
	// per-object skip cause, surviving at exactly one place -- so it is
	// refused there rather than applied into nothing.
	{
		Check( sess->InsertChunk(
			"box_geometry\n{\n\tname ghost_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}" ).applied,
			"G a geometry inserts" );
		Check( sess->InsertChunk(
			"lambertian_material\n{\n\tname ghost_mat\n\treflectance ghost_pnt\n}" ).applied,
			"G a material inserts" );
		Check( sess->InsertChunk(
			"standard_object\n{\n\tname ghost_obj\n\tgeometry ghost_geo\n\tmaterial ghost_mat\n"
			"\tposition 0 0 0\n}" ).applied, "G an object inserts, minting the root" );

		Agent::AgentSetPatch mp;
		mp.target = Agent::AgentSession::ElementRootName( "ghost" );
		mp.kind   = "standard_object";
		mp.param  = "quaternion";
		mp.value  = "0 0 0.7071067811865476 0.7071067811865476";
		Check( sess->ProposePatch( mp ).applied, "G a quaternion is patched onto the root" );

		const std::string beforeQ = sess->ReadDocument();
		const Agent::AgentSession::AgentPlaceElementResult qr =
			sess->PlaceElement( "ghost", "1 2 3" );
		Check( !qr.ok, "G MONEY ASSERTION: the placement REFUSES rather than writing params the "
		               "root's transform precedence would discard" );
		Check( qr.message.find( "quaternion" ) != std::string::npos &&
		       qr.message.find( "no effect" ) != std::string::npos,
		       "G and the refusal names the parameter and says what it would have cost" );
		Check( sess->ReadDocument() == beforeQ, "G with the document byte-identical" );
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

//======================================================================
// H -- the reported bounding box excludes the container root.
//======================================================================
static void TestBoundsExcludeRoot()
{
	std::printf( "H: the reported element bounding box excludes the container root...\n" );
	const std::string tmp = TempPath( "agent_elem_root_h.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "H fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "brick" ) ).ok, "H the plan files" );
	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname brick_pnt\n\tcolor 0.5 0.5 0.5\n}" ).applied, "H painter" );
	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname brick_mat\n\treflectance brick_pnt\n}" ).applied, "H material" );
	Check( sess->InsertChunk(
		"box_geometry\n{\n\tname brick_geo\n\twidth 1\n\theight 1\n\tdepth 1\n}" ).applied, "H geometry" );
	// FAR from the origin on every axis, so a container folded in at the
	// element's origin would visibly widen the box.
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname brick_obj\n\tgeometry brick_geo\n\tmaterial brick_mat\n"
		"\tposition 20 20 20\n}" ).applied, "H object" );

	const Agent::AgentSession::AgentPlaceElementResult pr = sess->PlaceElement( "brick", "0 0 0" );
	Check( pr.ok, "H the placement applies" );
	Check( pr.bboxValid, "H a bounding box is reported" );
	if( pr.bboxValid ) {
		std::printf( "    bbox (%.6f, %.6f, %.6f) .. (%.6f, %.6f, %.6f)\n",
		             pr.bboxMin[0], pr.bboxMin[1], pr.bboxMin[2],
		             pr.bboxMax[0], pr.bboxMax[1], pr.bboxMax[2] );
		Check( pr.bboxMin[0] > 19.0 && pr.bboxMin[1] > 19.0 && pr.bboxMin[2] > 19.0,
		       "H MONEY ASSERTION: the box is the OBJECT's, not the object's unioned with a "
		       "container whose default box sits at the element's origin" );
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}


//======================================================================
// I -- THE E1 GATE MUST STILL SEE THE CSG OBJECT.
//
// This is a REFUSAL guard, and a refusal guard cannot be caught by any
// correctness assertion: unguarded, the insert still lands the RIGHT
// chunk and the scene still renders.  Only an assertion demanding the
// DEGRADED answer -- "the gate finds nothing" -- discriminates.
//
// The mechanism.  `CheckNonSamplingEmitterGateForInsert` DERIVES head +
// candidate into a throwaway Job and then asks that Job's object manager
// for the csg_object by name (`CollectNullGeometryEmitters_`).  Inside an
// element window the composite's operands are auto-parented, and
// `Job::AddCSGObject` REFUSES a parented operand -- so the candidate does
// not derive, the composite is never registered, `GetItem` returns null,
// the finder returns empty, and the gate passes an unacknowledged
// non-sampling emitter straight through.  It is silent: nothing logs, the
// insert succeeds, and the only symptom is an emissive object that never
// gets light-sampled.
//
// The fix splits the operand repair into a PLAN (pure) that the gate
// judges and an APPLY that runs after every gate.  Fold the two back
// together -- have the gate read `snap.document` instead of the computed
// head -- and case I(a) goes green->red while every other assertion in
// this file, and every correctness test in the suite, stays green.
//
// I(a) is the discriminator; I(b) is the positive control that proves the
// fixture really is a null-geometry emitter and that the GATE is what
// refused it, not some unrelated element-window rule.
//======================================================================
static void TestEmitterGateSeesCsgInsideAnElement()
{
	std::printf( "I: the non-sampling-emitter gate still fires on a csg_object built INSIDE an "
	             "element window...\n" );
	const std::string tmp = TempPath( "agent_elem_root_i.RISEscene" );
	Job* pJob = LoadScene( tmp );
	Check( pJob != nullptr, "I fixture loads" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = WrapBuilding( pJob );
	Check( sess->FileBuildPlan( OnePartPlan( "beacon" ) ).ok, "I the plan files" );

	Check( sess->InsertChunk(
		"uniformcolor_painter\n{\n\tname beacon_pnt\n\tcolor 1 0.9 0.7\n}" ).applied, "I painter" );
	Check( sess->InsertChunk(
		"lambertian_material\n{\n\tname beacon_matte\n\treflectance beacon_pnt\n}" ).applied,
		"I matte material" );
	// The emissive material the composite will bind.  A csg_object is the
	// ONLY chunk that can produce a null-GEOMETRY object carrying one --
	// which is the whole reason the E1 gate exists.
	Check( sess->InsertChunk(
		"lambertian_luminaire_material\n{\n\tname beacon_glow\n\texitance beacon_pnt\n"
		"\tscale 30.0\n\tmaterial none\n}" ).applied, "I luminaire material" );
	Check( sess->InsertChunk(
		"sphere_geometry\n{\n\tname beacon_geo_a\n\tradius 0.6\n}" ).applied, "I geometry A" );
	Check( sess->InsertChunk(
		"sphere_geometry\n{\n\tname beacon_geo_b\n\tradius 0.6\n}" ).applied, "I geometry B" );
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname beacon_opA\n\tgeometry beacon_geo_a\n"
		"\tmaterial beacon_matte\n}" ).applied, "I operand A" );
	Check( sess->InsertChunk(
		"standard_object\n{\n\tname beacon_opB\n\tgeometry beacon_geo_b\n"
		"\tmaterial beacon_matte\n\tposition 0.35 0 0\n}" ).applied, "I operand B" );

	// THE PRECONDITION that makes this test the one that matters: the
	// operands really are parented, so the head the gate would naively read
	// really is a document that cannot derive.
	const std::string root = Agent::AgentSession::ElementRootName( "beacon" );
	Check( DocParam( *sess, "standard_object", "beacon_opA", "parent" ) == root &&
	       DocParam( *sess, "standard_object", "beacon_opB", "parent" ) == root,
	       "I PRECONDITION: both operands are auto-parented to the element root, which is what "
	       "makes the naive candidate document underivable" );

	// ---- I(a): THE DISCRIMINATOR ----
	{
		const std::string headBefore = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion vBefore = sess->HeadVersion();

		const Agent::AgentChunkResult r = sess->InsertChunk(
			"csg_object\n{\n\tname beacon_lamp\n\tobja beacon_opA\n\tobjb beacon_opB\n"
			"\toperation union\n\tmaterial beacon_glow\n}" );

		Check( !r.applied && r.status == "rejected",
		       "I(a) MONEY ASSERTION: the emitter gate FIRES on a csg_object inserted inside an "
		       "element window -- with the operand repair folded into the apply instead of planned "
		       "for the gate, the candidate derive fails, the composite is never registered, the "
		       "finder returns empty and this insert LANDS unacknowledged" );
		// On the gate's OWN findings, not on a message that could read the
		// same way for another reason: these three clauses are composed by
		// DescribeUnacknowledgedNullGeometryEmitters_ and by nothing else in
		// this file.
		Check( r.message.find( "NOT act as an area light for next-event estimation" ) != std::string::npos,
		       "I(a) and it is THE EMITTER GATE that refused -- its own consequence clause" );
		Check( r.message.find( "allow_non_sampling_emitter" ) != std::string::npos,
		       "I(a) its own escape clause" );
		Check( r.message.find( "beacon_lamp" ) != std::string::npos,
		       "I(a) naming the composite it found" );

		// THE SECOND HALF OF WHAT THE SPLIT BOUGHT.  The repair is a PLAN at
		// gate time, so a refused insert performs no detach -- the operands
		// keep their parent and not one byte moves.  An implementation that
		// detached first and asked afterwards would pass every assertion
		// above and silently fail these.
		Check( sess->ReadDocument() == headBefore,
		       "I(a) MONEY ASSERTION: the refusal leaves the document BYTE-IDENTICAL -- the operand "
		       "detach is computed for the gate, never applied to reach it" );
		Check( sess->HeadVersion() == vBefore, "I(a) and the revision unmoved" );
		Check( DocParam( *sess, "standard_object", "beacon_opA", "parent" ) == root &&
		       DocParam( *sess, "standard_object", "beacon_opB", "parent" ) == root,
		       "I(a) with both operands still parented" );
	}

	// ---- I(b): THE POSITIVE CONTROL ----
	//
	// The SAME insert with the acknowledgement flag APPLIES.  Without this,
	// I(a)'s `!applied` could be satisfied by any unrelated element-window
	// refusal and the test would pin nothing.  It also exercises the repair
	// end to end on the accepted path.
	{
		const Agent::AgentChunkResult r2 = sess->InsertChunk(
			"csg_object\n{\n\tname beacon_lamp\n\tobja beacon_opA\n\tobjb beacon_opB\n"
			"\toperation union\n\tmaterial beacon_glow\n\tallow_non_sampling_emitter TRUE\n}" );
		Check( r2.applied && r2.status == "applied",
		       "I(b) MONEY ASSERTION: the SAME chunk WITH the acknowledgement flag APPLIES -- so "
		       "I(a) is the emitter gate refusing, not an element-window rule refusing every "
		       "csg_object (" + r2.message + ")" );
		Check( DocParam( *sess, "standard_object", "beacon_opA", "parent" ) == "none" &&
		       DocParam( *sess, "standard_object", "beacon_opB", "parent" ) == "none",
		       "I(b) and on the ACCEPTED path the planned detach is applied for real" );
		Check( DocParam( *sess, "csg_object", "beacon_lamp", "parent" ) == root,
		       "I(b) with the composite parented in their place" );
	}

	sess.reset(); pJob->release(); std::remove( tmp.c_str() );
}

int main()
{
	std::printf( "AgentElementRootTest -- 87 step 5: the element becomes a scene-graph node\n" );
	TestRootNodeIsMinted();
	TestExactComposition();
	TestQuaternionAndMatrixChildren();
	TestScaleForms();
	TestIdempotence();
	TestCsgElement();
	TestNoRootRefusal();
	TestBoundsExcludeRoot();
	TestEmitterGateSeesCsgInsideAnElement();
	std::printf( "%d checks, %d failures\n", gChecks, gFails );
	if( gFails ) { std::printf( "FAILED\n" ); return 1; }
	std::printf( "Passed!\n" );
	return 0;
}
