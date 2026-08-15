//////////////////////////////////////////////////////////////////////
//
//  GroupChunkTest.cpp - docs/agentic-redesign/86-object-grouping.md Slice 1:
//    the `group` chunk.  `group { name g  member a  member b  position ...
//    orientation ... scale ... }` composes ONE transform into EACH named
//    member at derive time (position * orientation-from-Euler-degrees *
//    stretch, matching standard_object's own composition), then
//    PushBottomTransStack + FinalizeTransformations so the group lands
//    OUTSIDE the member's own transform: final = groupMatrix * memberMatrix.
//    `group` creates NO manager entity -- the CST document itself is the
//    index (the side-index decision, design doc §3).
//
//    WHAT THE EXACT-MATRIX ASSERTIONS ACTUALLY PIN (fix round 2026-08-15, F3/F4).
//    An exact-matrix assertion is only as discriminating as the scene it runs on:
//      * PushBottom vs PushTop.  A ONE-element transform stack is identical under
//        push_front and push_back, so a single group over a member with no stack of
//        its own cannot tell them apart.  Two composed matrices can -- but only if
//        they DO NOT COMMUTE.  `[two-groups]` (a rotation and a translation) and
//        `[matrix-member]` (an authored `matrix` under a rotating group) are therefore
//        both deliberately non-commuting; either one FAILS if the production call
//        flips to PushTopTransStack.  (RED-PROVEN both directions, 2026-08-15.)
//      * Euler axis order.  A SINGLE-AXIS rotation is invariant to Rx*Ry*Rz vs
//        Rz*Ry*Rx, so a one-axis group orientation cannot pin the order.  `[compose]`
//        uses a MULTI-AXIS group orientation (10 20 30) and computes its expectation
//        from a hand-multiplied Euler product in this file -- NOT by calling the same
//        Matrix4Ops::{X,Y,Z}Rotation composition production uses -- so transposing the
//        order in the parser FAILS it.  (RED-PROVEN, 2026-08-15.)
//
//    Cases:
//      [compose]       group(T,R,S) over a member with its OWN non-identity
//                       transform == groupMatrix * memberMatrix, with a MULTI-AXIS
//                       group orientation and an INDEPENDENTLY built expectation
//                       (pins the Euler axis order).
//      [multi-member]  every listed member receives the SAME group matrix.
//      [two-groups]    two NON-COMMUTING groups over one member compose in DOCUMENT
//                       ORDER (documented, ALLOWED behaviour -- not a conflict), which
//                       is also what pins the push side.
//      [matrix-member] a member authored via `matrix` also composes correctly
//                       (SetFinalTransformMatrix clears the stack to just the
//                       authored matrix; the group's push lands on top of it) --
//                       under a ROTATING group, so it too pins the push side.
//      [round-trip]    a `group` scene round-trips byte-for-byte, WITH blank/comment
//                       trivia interleaved between the repeated `member` occurrences
//                       (the CstRepeatGroupTest convention: proves the repeated-param
//                       structure is preserved, not re-serialized canonically).
//      [side-index]    the Job-level membership index records the accumulated group
//                       matrix per member, composes multi-group members in document
//                       order, and is CLEARED on a re-derive.
//      [commit-rt]     THE F1 ROUND TRIP: committing `G^-1 * final` as the member's
//                       `matrix` param re-derives back to EXACTLY `final` (the GUI
//                       transform-commit arithmetic; the composed matrix would give
//                       G*G*M instead).
//      [reject]        no members; unknown member; member declared AFTER the group;
//                       duplicate member; member naming the group (but NOT a group name
//                       colliding with an UNRELATED object's name -- H3(b), accepted); a
//                       NESTED group; a SINGULAR group `scale` (H1, 2026-08-15 -- plus a
//                       near-degenerate companion that must NOT reject); an override_object
//                       `matrix`/`quaternion` on a grouped member.
//      [incremental]   editing the group itself, or a member (whose closure
//                       pulls the group in as a dependent), both refuse on
//                       the O(closure) path -> full-derive fallback.
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static const std::string HDR = "RISE ASCII SCENE 7\n";

// A scene with the geometry/material the group's members reference, + a body.
static std::string Scene( const std::string& body )
{
	return HDR
		+ "sphere_geometry\n{\nname geo\nradius 1\n}\n"
		+ "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		+ "lambertian_material\n{\nname m\nreflectance p\n}\n"
		+ body;
}
static std::string Obj( const std::string& name, const std::string& position = "0 0 0",
	const std::string& orientation = "0 0 0", const std::string& scale = "1 1 1" )
{
	return "standard_object\n{\nname " + name + "\ngeometry geo\nmaterial m\nposition " + position +
		"\norientation " + orientation + "\nscale " + scale + "\n}\n";
}
static std::string ObjMatrix( const std::string& name, const std::string& matrix16 )
{
	return "standard_object\n{\nname " + name + "\ngeometry geo\nmaterial m\nmatrix " + matrix16 + "\n}\n";
}
static std::string Grp( const std::string& name, const std::vector<std::string>& members,
	const std::string& position = "0 0 0", const std::string& orientation = "0 0 0", const std::string& scale = "1 1 1" )
{
	std::string s = "group\n{\nname " + name + "\n";
	for( const std::string& m : members ) s += "member " + m + "\n";
	s += "position " + position + "\norientation " + orientation + "\nscale " + scale + "\n}\n";
	return s;
}

// Derive a Document into a fresh Job; caller owns the returned Job (release()).
static Job* DeriveJob( const std::string& scene, std::vector<std::string>* outDiags = nullptr )
{
	Job* j = new Job();
	Document d = ParseToCst( scene );
	std::vector<std::string> diags;
	DeriveToJob( d, *j, &diags );
	if( outDiags ) *outDiags = diags;
	return j;
}

// Snapshot an object's final world transform as 16 doubles (declaration order = the col-major
// encoding) -- same pattern as CstSliceThreeExpansionTest.cpp's ObjMat16.
static void ObjMat16( Job& j, const char* obj, double out[16] )
{
	for( int i = 0; i < 16; ++i ) out[i] = 0.0;
	const IScene* sc = j.GetScene();
	const IObjectManager* om = sc ? sc->GetObjects() : 0;
	const IObject* o = om ? om->GetItem( obj ) : 0;
	if( !o ) return;
	Matrix4 m = o->GetFinalTransformMatrix();
	out[ 0] = m._00; out[ 1] = m._01; out[ 2] = m._02; out[ 3] = m._03;
	out[ 4] = m._10; out[ 5] = m._11; out[ 6] = m._12; out[ 7] = m._13;
	out[ 8] = m._20; out[ 9] = m._21; out[10] = m._22; out[11] = m._23;
	out[12] = m._30; out[13] = m._31; out[14] = m._32; out[15] = m._33;
}
static void Mat16FromMatrix4( const Matrix4& m, double out[16] )
{
	out[ 0] = m._00; out[ 1] = m._01; out[ 2] = m._02; out[ 3] = m._03;
	out[ 4] = m._10; out[ 5] = m._11; out[ 6] = m._12; out[ 7] = m._13;
	out[ 8] = m._20; out[ 9] = m._21; out[10] = m._22; out[11] = m._23;
	out[12] = m._30; out[13] = m._31; out[14] = m._32; out[15] = m._33;
}
static bool Mat16Eq( const double a[16], const double b[16] )
{
	for( int i = 0; i < 16; ++i ) if( std::fabs( a[i] - b[i] ) > 1e-9 ) return false;
	return true;
}

// INDEPENDENT (test-side) TRS composition mirroring standard_object's own
// (position * orientation-from-Euler-degrees * stretch).
//
// F4: this deliberately does NOT call Matrix4Ops::{X,Y,Z}Rotation or Matrix4's
// operator* to build the rotation -- the previous version did, so it shared the
// production Euler-axis ORDER by construction and a transposed order in the parser
// would have passed.  Instead the three textbook right-handed rotation matrices are
// written out from sin/cos here, multiplied out BY HAND in ordinary [row][col]
// indexing, and only then transcribed into RISE's `_<col><row>` field layout (see
// SceneEditor.cpp's FormatMatrix16 comment for that convention).  A transposed Euler
// order in production now fails [compose].
static void EulerXYZRowMajor( double rxDeg, double ryDeg, double rzDeg, double R[3][3] )
{
	const double kDeg = 3.14159265358979323846 / 180.0;
	const double cx = std::cos( rxDeg * kDeg ), sx = std::sin( rxDeg * kDeg );
	const double cy = std::cos( ryDeg * kDeg ), sy = std::sin( ryDeg * kDeg );
	const double cz = std::cos( rzDeg * kDeg ), sz = std::sin( rzDeg * kDeg );
	const double Rx[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
	const double Ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
	const double Rz[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
	double T[3][3];
	for( int i = 0; i < 3; ++i ) for( int j = 0; j < 3; ++j ) {
		T[i][j] = Rx[i][0]*Ry[0][j] + Rx[i][1]*Ry[1][j] + Rx[i][2]*Ry[2][j];
	}
	for( int i = 0; i < 3; ++i ) for( int j = 0; j < 3; ++j ) {
		R[i][j] = T[i][0]*Rz[0][j] + T[i][1]*Rz[1][j] + T[i][2]*Rz[2][j];
	}
}
static Matrix4 TRS( double px, double py, double pz, double rxDeg, double ryDeg, double rzDeg,
	double sx, double sy, double sz )
{
	double R[3][3];
	EulerXYZRowMajor( rxDeg, ryDeg, rzDeg, R );
	// M = T * R * S (column-vector convention): the linear part is R with column j
	// scaled by s_j, and the translation occupies column 3.  `_ij` is column i, row j.
	Matrix4 m;   // default ctor is identity
	m._00 = R[0][0]*sx; m._01 = R[1][0]*sx; m._02 = R[2][0]*sx;
	m._10 = R[0][1]*sy; m._11 = R[1][1]*sy; m._12 = R[2][1]*sy;
	m._20 = R[0][2]*sz; m._21 = R[1][2]*sz; m._22 = R[2][2]*sz;
	m._30 = px; m._31 = py; m._32 = pz;
	return m;
}

static const char* SDIAG( const std::vector<std::string>& d ) { return d.empty() ? "(none)" : d[0].c_str(); }

int main()
{
	std::printf( "GroupChunkTest -- docs/agentic-redesign/86-object-grouping.md Slice 1: the `group` chunk\n" );

	// [compose] the money assertion: group(T,R,S) over a member with its OWN non-identity
	// transform == groupMatrix * memberMatrix.  The group's orientation is MULTI-AXIS
	// (10 20 30) and the expectation is built by the independent hand-multiplied Euler
	// above, so this case pins the Euler AXIS ORDER as well as the composition side (F4).
	{
		const std::string scene = Scene(
			Obj( "cart_body", "1 0 0", "0 45 0", "1 1 1" ) +
			Grp( "cart", { "cart_body" }, "2 0 -1", "10 20 30", "2 1 1" ) );
		std::vector<std::string> diags;
		Job* j = DeriveJob( scene, &diags );
		Check( diags.empty(), "compose: the derive succeeds" );

		const Matrix4 memberM = TRS( 1, 0, 0,  0, 45, 0,  1, 1, 1 );
		const Matrix4 groupM  = TRS( 2, 0, -1,  10, 20, 30,  2, 1, 1 );
		const Matrix4 expectM = groupM * memberM;
		double expect[16]; Mat16FromMatrix4( expectM, expect );
		double actual[16]; ObjMat16( *j, "cart_body", actual );
		Check( Mat16Eq( expect, actual ), "compose: member final transform == groupMatrix * memberMatrix" );

		// Sanity: the WRONG order (memberMatrix * groupMatrix) must NOT match -- proves this
		// scene's T/R/S choices are non-commuting enough for the assertion to be discriminating.
		const Matrix4 wrongM = memberM * groupM;
		double wrong[16]; Mat16FromMatrix4( wrongM, wrong );
		Check( !Mat16Eq( wrong, actual ), "compose: the reversed-order product does NOT match (assertion is discriminating)" );
		j->release();
	}

	// [multi-member] every listed member receives the SAME group matrix.
	{
		const std::string scene = Scene(
			Obj( "a" ) + Obj( "b" ) + Obj( "c" ) +
			Grp( "g", { "a", "b", "c" }, "5 0 0", "0 0 0", "1 1 1" ) );
		std::vector<std::string> diags;
		Job* j = DeriveJob( scene, &diags );
		Check( diags.empty(), "multi-member: the derive succeeds" );
		const Matrix4 groupM = TRS( 5, 0, 0, 0, 0, 0, 1, 1, 1 );
		double expect[16]; Mat16FromMatrix4( groupM, expect );
		for( const char* n : { "a", "b", "c" } ) {
			double actual[16]; ObjMat16( *j, n, actual );
			Check( Mat16Eq( expect, actual ), "multi-member: every member gets the identical group matrix" );
		}
		j->release();
	}

	// [two-groups] two groups over ONE member compose in DOCUMENT ORDER (documented, ALLOWED --
	// not flagged as a conflict).  final = group2Matrix * group1Matrix * memberMatrix.
	//
	// F3: g1 is a pure ROTATION and g2 a pure TRANSLATION, so g2*g1 != g1*g2.  That
	// non-commutativity is what makes this case discriminate PushBottomTransStack from
	// PushTopTransStack -- with two stack entries the push side decides the fold order,
	// and with the earlier all-translation scene the two orders were numerically equal.
	{
		const std::string scene = Scene(
			Obj( "shared", "1 0 0" ) +
			Grp( "g1", { "shared" }, "0 0 0", "0 0 90" ) +
			Grp( "g2", { "shared" }, "1 0 0" ) );
		std::vector<std::string> diags;
		Job* j = DeriveJob( scene, &diags );
		Check( diags.empty(), "two-groups: the derive succeeds (no duplicate-member/self-ref false positive across groups)" );
		const Matrix4 memberM = TRS( 1, 0, 0, 0, 0, 0, 1, 1, 1 );
		const Matrix4 g1M     = TRS( 0, 0, 0, 0, 0, 90, 1, 1, 1 );
		const Matrix4 g2M     = TRS( 1, 0, 0, 0, 0, 0, 1, 1, 1 );
		const Matrix4 expectM = g2M * g1M * memberM;
		double expect[16]; Mat16FromMatrix4( expectM, expect );
		double actual[16]; ObjMat16( *j, "shared", actual );
		Check( Mat16Eq( expect, actual ), "two-groups: compose in document order (g2 outside g1 outside member)" );

		// The push side is only pinned if the two group matrices genuinely disagree when
		// swapped -- assert that, so a future edit that makes them commute again fails LOUDLY
		// here rather than silently un-pinning PushBottom-vs-PushTop.
		const Matrix4 swappedM = g1M * g2M * memberM;
		double swapped[16]; Mat16FromMatrix4( swappedM, swapped );
		Check( !Mat16Eq( swapped, expect ),
			"two-groups: the two group matrices do NOT commute (so this case really pins the push side)" );
		Check( !Mat16Eq( swapped, actual ), "two-groups: the PushTop fold order does NOT match the live transform" );
		j->release();
	}

	// [matrix-member] a member authored via `matrix` also composes correctly: SetFinalTransformMatrix
	// clears the component transforms + replaces the stack with just the authored matrix; the group's
	// PushBottomTransStack lands on top of it (final = groupMatrix * authoredMatrix).
	//
	// F3: the group ROTATES (and translates) so it does NOT commute with the authored translation.
	// This is a genuine TWO-entry stack (the authored matrix is entry 0, the group's push entry 1),
	// so like [two-groups] it discriminates PushBottom from PushTop -- which the all-translation
	// version of this case did not.
	{
		// Column-major, translation (3,4,5) -- same convention as ComposeTRS_QuaternionGltf/
		// BuildMatrix4FromColumnMajor (translation in elements 12,13,14).
		const std::string matrix16 = "1 0 0 0 0 1 0 0 0 0 1 0 3 4 5 1";
		const std::string scene = Scene(
			ObjMatrix( "mo", matrix16 ) +
			Grp( "g", { "mo" }, "0 0 10", "0 90 0" ) );
		std::vector<std::string> diags;
		Job* j = DeriveJob( scene, &diags );
		Check( diags.empty(), "matrix-member: the derive succeeds" );
		const Matrix4 memberM( 1,0,0,0,  0,1,0,0,  0,0,1,0,  3,4,5,1 );
		const Matrix4 groupM = TRS( 0, 0, 10, 0, 90, 0, 1, 1, 1 );
		const Matrix4 expectM = groupM * memberM;
		double expect[16]; Mat16FromMatrix4( expectM, expect );
		double actual[16]; ObjMat16( *j, "mo", actual );
		Check( Mat16Eq( expect, actual ), "matrix-member: group composes on top of an authored `matrix` transform" );
		const Matrix4 swappedM = memberM * groupM;
		double swapped[16]; Mat16FromMatrix4( swappedM, swapped );
		Check( !Mat16Eq( swapped, expect ),
			"matrix-member: the authored matrix and the group matrix do NOT commute (this case pins the push side too)" );
		j->release();
	}

	// [round-trip] a `group` scene round-trips byte-for-byte (the document IS the index -- load-bearing:
	// with no manager entity, the serialized text is the only durable record of the grouping).
	//
	// F6: trivia (a blank line and a comment) is interleaved BETWEEN the repeated `member`
	// occurrences, matching the convention CstRepeatGroupTest sets for repeatable params.  A
	// serializer that re-emitted repeated params canonically -- collapsing them into a contiguous
	// run and dropping the interleaved trivia -- would pass a trivia-free round trip and fail this one.
	{
		const std::string scene = Scene(
			Obj( "a" ) + Obj( "b" ) + Obj( "c" ) +
			"group\n{\nname g\nmember a\n\n# the middle member, with trivia on both sides\n\nmember b\n"
			"# a trailing comment before the last member\nmember c\nposition 1 2 3\norientation 10 20 30\nscale 1 1 1\n}\n" );
		Check( SerializeCst( ParseToCst( scene ) ) == scene,
			"round-trip: a group scene round-trips byte-for-byte WITH trivia interleaved between `member` occurrences" );
		// And it still derives -- the trivia is not smuggled into a member name.
		std::vector<std::string> d;
		Job* j = DeriveJob( scene, &d );
		Check( d.empty(), "round-trip: the trivia-laden group still derives cleanly" );
		j->release();
	}

	// [side-index] the Job-level membership side index (IJob::NoteGroupMembership /
	// GetGroupMembership) -- the concrete form of the design's §3 "side index" decision, and the
	// thing the F1 transform-commit fix and the F2 override_object refusal both read.
	{
		const std::string scene = Scene(
			Obj( "m1", "1 0 0" ) + Obj( "loner" ) +
			Grp( "g1", { "m1" }, "0 0 0", "0 0 90" ) +
			Grp( "g2", { "m1" }, "1 0 0" ) );
		std::vector<std::string> diags;
		Job* j = DeriveJob( scene, &diags );
		Check( diags.empty(), "side-index: the derive succeeds" );

		double g16[16]; char gname[256] = {0};
		Check( j->GetGroupMembership( "m1", g16, gname, (unsigned int)sizeof( gname ) ),
			"side-index: a grouped member is found in the index" );
		Check( !j->GetGroupMembership( "loner", g16, gname, (unsigned int)sizeof( gname ) ),
			"side-index: a NON-member is not in the index" );
		Check( !j->GetGroupMembership( "nosuchobject", nullptr, nullptr, 0 ),
			"side-index: an unknown name is not in the index" );

		// Re-read (the previous call may have clobbered the buffers on the miss path).
		j->GetGroupMembership( "m1", g16, gname, (unsigned int)sizeof( gname ) );
		Check( std::string( gname ) == "g1, g2",
			"side-index: a multi-group member records BOTH owning group names in document order" );
		// The accumulated matrix must be G_last * G_first -- the same fold the transform stack does.
		const Matrix4 g1M = TRS( 0, 0, 0, 0, 0, 90, 1, 1, 1 );
		const Matrix4 g2M = TRS( 1, 0, 0, 0, 0, 0, 1, 1, 1 );
		double expect[16]; Mat16FromMatrix4( g2M * g1M, expect );
		Check( Mat16Eq( expect, g16 ),
			"side-index: the accumulated matrix is G_last * G_first (matches the stack's fold order)" );

		Check( j->IsGroupDeclared( "g1" ) && j->IsGroupDeclared( "g2" ),
			"side-index: declared group names are recorded" );
		Check( !j->IsGroupDeclared( "m1" ) && !j->IsGroupDeclared( "nope" ),
			"side-index: an object name / unknown name is NOT reported as a declared group" );

		// DERIVE-SCOPED: a fresh full derive of a GROUPLESS document onto the SAME Job must leave
		// no trace.  A stale index would make the transform commit divide by a group that no
		// longer exists -- silently corrupting every subsequent commit on that object.  ClearAll
		// (-> DestroyContainers + InitializeContainers) is the exact seam every real full re-derive
		// goes through -- Job::DeriveEditedCstDocument_ / RederiveCstWithVariant / a reopen all call
		// it -- and it is where the index reset was placed, alongside m_objectOverrideCount's.
		j->ClearAll();
		Check( !j->GetGroupMembership( "m1", nullptr, nullptr, 0 ),
			"side-index: ClearAll alone clears the index" );
		Document d2 = ParseToCst( Scene( Obj( "m1", "1 0 0" ) ) );
		std::vector<std::string> dd2;
		DeriveToJob( d2, *j, &dd2 );
		Check( dd2.empty(), "side-index: the group-free re-derive succeeds" );
		Check( !j->GetGroupMembership( "m1", nullptr, nullptr, 0 ),
			"side-index: a full re-derive CLEARS the index (no stale membership across derives)" );
		Check( !j->IsGroupDeclared( "g1" ),
			"side-index: a full re-derive CLEARS the declared-group set too" );
		j->release();
	}

	// [commit-rt] THE F1 ROUND TRIP.  SceneEditor::CommitPendingCstObjectTransforms writes an
	// object's NET world transform to its `matrix` param; on a grouped member the net transform
	// already CONTAINS the group's matrix G, and the re-derive applies the group's push AGAIN --
	// so committing it verbatim gives G*G*M, and every drag compounds another G without bound.
	// The fix divides G out first.  This case performs exactly that arithmetic against the real
	// derive and asserts the round trip is numerically exact.
	{
		const std::string scene = Scene(
			Obj( "cart_body", "1 0.5 0", "0 45 0", "1 1 1" ) +
			Grp( "cart", { "cart_body" }, "2 0 -1", "0 45 0", "2 1 1" ) );
		Document doc = ParseToCst( scene );
		Job* j = new Job(); std::vector<std::string> d0;
		DeriveToJob( doc, *j, &d0 );
		Check( d0.empty(), "commit-rt: the base derive succeeds" );

		double final0[16]; ObjMat16( *j, "cart_body", final0 );
		double g16[16];
		Check( j->GetGroupMembership( "cart_body", g16, nullptr, 0 ), "commit-rt: the member is indexed" );
		const Matrix4 G(
			g16[ 0], g16[ 1], g16[ 2], g16[ 3],  g16[ 4], g16[ 5], g16[ 6], g16[ 7],
			g16[ 8], g16[ 9], g16[10], g16[11],  g16[12], g16[13], g16[14], g16[15] );
		const Matrix4 finalM(
			final0[ 0], final0[ 1], final0[ 2], final0[ 3],  final0[ 4], final0[ 5], final0[ 6], final0[ 7],
			final0[ 8], final0[ 9], final0[10], final0[11],  final0[12], final0[13], final0[14], final0[15] );

		// What the fixed commit writes: G^-1 * final (the member's GROUP-LOCAL matrix).
		const Matrix4 groupLocal = Matrix4Ops::Inverse( G ) * finalM;
		double gl[16]; Mat16FromMatrix4( groupLocal, gl );
		char buf[ 1024 ]; buf[0] = '\0';
		for( int i = 0; i < 16; ++i ) {
			char one[64]; std::snprintf( one, sizeof( one ), "%s%.17g", i ? " " : "", gl[i] );
			std::strncat( buf, one, sizeof( buf ) - std::strlen( buf ) - 1 );
		}
		const NodeId memberId = DocFindByName( doc, "standard_object/cart_body" );
		Check( memberId != 0, "commit-rt: the member chunk resolves by name" );
		Document d1 = DocRemoveParam( doc, memberId, "position" );
		d1 = DocRemoveParam( d1, memberId, "orientation" );
		d1 = DocRemoveParam( d1, memberId, "scale" );
		d1 = DocSetOrAddParamValue( d1, memberId, "matrix", 0, std::string( buf ) );

		Job* j2 = new Job(); std::vector<std::string> d2;
		DeriveToJob( d1, *j2, &d2 );
		Check( d2.empty(), "commit-rt: the committed document re-derives cleanly" );
		double final1[16]; ObjMat16( *j2, "cart_body", final1 );
		double worst = 0.0;
		for( int i = 0; i < 16; ++i ) worst = std::max( worst, std::fabs( final1[i] - final0[i] ) );
		Check( worst <= 1e-12,
			"commit-rt: committing G^-1*final round-trips to EXACTLY final (max |delta| <= 1e-12)" );
		std::printf( "  [commit-rt] max |re-derived - original| = %.3g\n", worst );

		// RED-PROOF of the bug this guards: committing the COMPOSED matrix verbatim (what the
		// pre-fix code did) re-derives to G*final, NOT final.
		double fm[16]; Mat16FromMatrix4( finalM, fm );
		char buf2[ 1024 ]; buf2[0] = '\0';
		for( int i = 0; i < 16; ++i ) {
			char one[64]; std::snprintf( one, sizeof( one ), "%s%.17g", i ? " " : "", fm[i] );
			std::strncat( buf2, one, sizeof( buf2 ) - std::strlen( buf2 ) - 1 );
		}
		Document d3 = DocRemoveParam( doc, memberId, "position" );
		d3 = DocRemoveParam( d3, memberId, "orientation" );
		d3 = DocRemoveParam( d3, memberId, "scale" );
		d3 = DocSetOrAddParamValue( d3, memberId, "matrix", 0, std::string( buf2 ) );
		Job* j3 = new Job(); std::vector<std::string> d4;
		DeriveToJob( d3, *j3, &d4 );
		double squared[16]; ObjMat16( *j3, "cart_body", squared );
		double expectSquared[16]; Mat16FromMatrix4( G * finalM, expectSquared );
		Check( !Mat16Eq( squared, final0 ) && Mat16Eq( squared, expectSquared ),
			"commit-rt: committing the COMPOSED matrix instead re-derives to G*final (the F1 bug, pinned)" );
		j->release(); j2->release(); j3->release();
	}

	// [reject: ...] every rejection asserts a DISTINCTIVE substring of the actual diagnostic, not
	// merely that something was diagnosed -- a bare `!d.empty()` passes on ANY unrelated derive
	// failure (a typo in the fixture scene, an unrelated regression) and on a diagnostic that
	// misattributes the cause, which is exactly the failure mode the nested-group arm below fixes.

	// [reject: no members]
	{
		const std::string scene = Scene( "group\n{\nname g\nposition 0 0 0\n}\n" );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		Check( !d.empty() && std::string( SDIAG( d ) ).find( "at least one `member` is required" ) != std::string::npos,
			"reject: a group with no members diagnoses the MISSING-MEMBER reason specifically" );
	}

	// [reject: unknown member]
	{
		const std::string scene = Scene( Obj( "a" ) + Grp( "g", { "a", "nosuchobject" } ) );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		Check( !d.empty() && std::string( SDIAG( d ) ).find( "nosuchobject" ) != std::string::npos,
			"reject: an unknown member names the offender in the diagnostic" );
	}

	// [reject: member declared AFTER the group] -- chunk order matters.
	{
		const std::string scene = Scene( Grp( "g", { "late" } ) + Obj( "late" ) );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		Check( !d.empty() && std::string( SDIAG( d ) ).find( "late" ) != std::string::npos,
			"reject: a member declared AFTER the group diagnoses (names the offender)" );
	}

	// [reject: duplicate member] -- a bare find("a") here was near-tautological (the diagnostic
	// contains the group name, the chunk keyword, and the word "member", all of which contain 'a').
	{
		const std::string scene = Scene( Obj( "wheel" ) + Grp( "g", { "wheel", "wheel" } ) );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		Check( !d.empty() && std::string( SDIAG( d ) ).find( "`wheel` is listed more than once" ) != std::string::npos,
			"reject: a member listed twice diagnoses the DUPLICATE reason and names the offender" );
	}

	// [reject: member names the group itself] -- the group-name/object-name COLLISION hard-reject.
	// Slice 3's `build_element` will naturally want one name for both the group and a representative
	// object; this is the restriction that forbids it, so its diagnostic must be specific.
	{
		const std::string scene = Scene( Obj( "g" ) + "group\n{\nname g\nmember g\n}\n" );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		Check( !d.empty() && std::string( SDIAG( d ) ).find( "names the group itself" ) != std::string::npos,
			"reject: a member naming the group's own name diagnoses the SELF-REFERENCE reason specifically" );
	}

	// [group name == an UNRELATED object's name -- ACCEPTED] -- H3(b), 2026-08-15.  The only
	// name-collision check `group` performs is member-names-the-group (above).  A group's `name`
	// matching some OTHER object's name (not one of its own members) is a different, unchecked
	// case: a `standard_object cart` alongside a `group cart { member wheel }` is harmless because
	// a group creates no manager entity, so there is nothing for the shared name to collide with.
	// This pins the descriptor's precise wording against a future "tighten the collision check"
	// edit that would make it a false positive.
	{
		const std::string scene = Scene(
			Obj( "cart" ) + Obj( "wheel", "1 0 0" ) + Grp( "cart", { "wheel" }, "2 0 0" ) );
		std::vector<std::string> d;
		Job* j = DeriveJob( scene, &d );
		Check( d.empty(), "group/object name collision (unrelated object): the derive succeeds -- only member-names-the-group is checked" );
		const Matrix4 expectM = TRS( 2, 0, 0, 0, 0, 0, 1, 1, 1 ) * TRS( 1, 0, 0, 0, 0, 0, 1, 1, 1 );
		double expect[16]; Mat16FromMatrix4( expectM, expect );
		double actual[16]; ObjMat16( *j, "wheel", actual );
		Check( Mat16Eq( expect, actual ), "group/object name collision (unrelated object): the group still composes normally onto its actual member" );
		j->release();
	}

	// [reject: nested group] -- V1 forbids groups containing groups (design doc §3 "Nesting depth").
	// Without the IsGroupDeclared side index this produced the generic "declare members BEFORE the
	// group" ordering diagnostic, which MISATTRIBUTES the cause: the chunks are already in the right
	// order and reordering them cannot help.  Assert the nesting-specific wording AND assert that the
	// misleading ordering wording is ABSENT.
	{
		const std::string scene = Scene(
			Obj( "a" ) + Grp( "inner", { "a" }, "1 0 0" ) + Grp( "outer", { "inner" }, "0 1 0" ) );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		const std::string diag = SDIAG( d );
		Check( !d.empty() && diag.find( "names another `group`" ) != std::string::npos
			&& diag.find( "cannot contain a group" ) != std::string::npos,
			"reject: a group naming another GROUP as a member diagnoses the NESTING rule specifically" );
		Check( diag.find( "Chunk order matters" ) == std::string::npos,
			"reject: the nested-group diagnostic does NOT misattribute the cause to chunk ordering" );
	}

	// [reject: singular group scale] -- H1 hardening (2026-08-15).  A group whose composed transform
	// is not invertible (a zero/degenerate `scale` component) used to derive FINE, with the
	// singularity only caught much later -- on a completely different code path with no test
	// coverage of its own -- by SceneEditor.cpp's commit-time `G * G^-1 == I` check, at GUI-drag
	// time, on an object far from the group chunk that actually caused it.  Now it is refused HERE,
	// at the authoring site, naming the group and (cheaply, since scale is the only one of the
	// group's three fields that can singularize `Translation * Rotation * Stretch`) the degenerate
	// scale axis.
	{
		const std::string scene = Scene( Obj( "a" ) + Grp( "g", { "a" }, "0 0 0", "0 0 0", "0 1 1" ) );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		const std::string diag = SDIAG( d );
		Check( !d.empty()
			&& diag.find( "group `g`" ) != std::string::npos
			&& diag.find( "not invertible" ) != std::string::npos
			&& diag.find( "x-component" ) != std::string::npos,
			"reject: a group with a degenerate `scale 0 1 1` refuses at parse time, naming the group and the degenerate scale axis" );
	}

	// [near-degenerate group scale still derives] -- the invertibility check must not be over-eager:
	// a small-but-nonzero scale (0.001) is a valid, if extreme, group and must still succeed. Guards
	// against a check that used a determinant epsilon too loose/tight instead of the real G*G^-1==I
	// verification.
	{
		const std::string scene = Scene( Obj( "a" ) + Grp( "g", { "a" }, "0 0 0", "0 0 0", "0.001 1 1" ) );
		std::vector<std::string> d;
		Job* j = DeriveJob( scene, &d );
		Check( d.empty(), "reject: a near-degenerate (but invertible) group `scale 0.001 1 1` still derives cleanly" );
		j->release();
	}

	// [reject: override_object matrix on a grouped member] -- F2.  The absolute `matrix`/`quaternion`
	// arms route through SetFinalTransformMatrix -> ReplaceFinalStack_, which CLEARS the transform
	// stack and so silently drops the group's entry.  They must REFUSE on a grouped member.
	{
		const std::string scene = Scene(
			Obj( "a", "1 0 0" ) + Grp( "g", { "a" }, "2 0 0" ) +
			"override_object\n{\nname a\nmatrix 1 0 0 0 0 1 0 0 0 0 1 0 9 9 9 1\n}\n" );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		const std::string diag = SDIAG( d );
		Check( !d.empty() && diag.find( "member of group `g`" ) != std::string::npos
			&& diag.find( "matrix" ) != std::string::npos,
			"reject: override_object `matrix` on a grouped member REFUSES, naming the object's group" );
	}
	{
		const std::string scene = Scene(
			Obj( "a", "1 0 0" ) + Grp( "g", { "a" }, "2 0 0" ) +
			"override_object\n{\nname a\nquaternion 0 0 0 1\n}\n" );
		std::vector<std::string> d;
		DeriveJob( scene, &d );
		const std::string diag = SDIAG( d );
		Check( !d.empty() && diag.find( "member of group `g`" ) != std::string::npos
			&& diag.find( "quaternion" ) != std::string::npos,
			"reject: override_object `quaternion` on a grouped member REFUSES too" );
	}

	// [override_object per-field on a grouped member] -- the SURVIVING arm.  It replaces only the
	// named component, so the group's stack entry survives and the override is interpreted
	// GROUP-LOCALLY: final == groupMatrix * (overridden component transform).
	{
		const std::string scene = Scene(
			Obj( "a", "1 0 0" ) + Grp( "g", { "a" }, "0 0 0", "0 0 90" ) +
			"override_object\n{\nname a\nposition 0 4 0\n}\n" );
		std::vector<std::string> d;
		Job* j = DeriveJob( scene, &d );
		Check( d.empty(), "override(per-field): a per-field override on a grouped member still derives" );
		const Matrix4 expectM = TRS( 0, 0, 0, 0, 0, 90, 1, 1, 1 ) * TRS( 0, 4, 0, 0, 0, 0, 1, 1, 1 );
		double expect[16]; Mat16FromMatrix4( expectM, expect );
		double actual[16]; ObjMat16( *j, "a", actual );
		Check( Mat16Eq( expect, actual ),
			"override(per-field): the override composes GROUP-LOCALLY (group transform survives outside it)" );
		j->release();
	}

	// [override_object BEFORE the group] -- not a clobber: the group's push composes on top of the
	// override, which is the intended layering.  The refusal must NOT fire here.
	{
		const std::string scene = Scene(
			Obj( "a", "1 0 0" ) +
			"override_object\n{\nname a\nmatrix 1 0 0 0 0 1 0 0 0 0 1 0 3 0 0 1\n}\n" +
			Grp( "g", { "a" }, "0 5 0" ) );
		std::vector<std::string> d;
		Job* j = DeriveJob( scene, &d );
		Check( d.empty(), "override(before group): an absolute override PRECEDING the group is ALLOWED" );
		const Matrix4 overriddenM( 1,0,0,0,  0,1,0,0,  0,0,1,0,  3,0,0,1 );
		const Matrix4 expectM = TRS( 0, 5, 0, 0, 0, 0, 1, 1, 1 ) * overriddenM;
		double expect[16]; Mat16FromMatrix4( expectM, expect );
		double actual[16]; ObjMat16( *j, "a", actual );
		Check( Mat16Eq( expect, actual ),
			"override(before group): the group composes on TOP of the earlier absolute override" );
		j->release();
	}

	// [incremental: group edit] editing the group ITSELF refuses on the O(closure) path -> full-derive.
	{
		const std::string scene = Scene( Obj( "a" ) + Grp( "g", { "a" }, "1 0 0" ) );
		Document d = ParseToCst( scene );
		Job* j = new Job(); std::vector<std::string> dd;
		DeriveToJob( d, *j, &dd );
		Check( dd.empty(), "incremental(group): the full derive succeeds" );
		const NodeId groupId = DocFindByName( d, "group/g" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d, *j, std::vector<NodeId>( 1, groupId ), &di );
		Check( groupId != 0 && applied == 0 && !di.empty(),
			"incremental(group): a group in the closure refuses (applied=0) -> full-derive fallback" );
		j->release();
	}

	// [incremental: member edit] editing a MEMBER pulls the group into its closure (member -> group is
	// a traced Reference edge); the closure {member, group} still refuses, because the group's Finalize
	// touches every member it lists, not just the edited one.
	{
		const std::string scene = Scene( Obj( "a", "1 0 0" ) + Obj( "b", "2 0 0" ) + Grp( "g", { "a", "b" }, "1 0 0" ) );
		Document d = ParseToCst( scene );
		Job* j = new Job(); std::vector<std::string> dd;
		DeriveToJob( d, *j, &dd );
		Check( dd.empty(), "incremental(member): the full derive succeeds" );
		const NodeId memberId = DocFindByName( d, "standard_object/a" );
		Check( memberId != 0, "incremental(member): the member chunk resolves by name" );
		const std::vector<NodeId> closure = DocEditClosure( d, memberId );
		const NodeId groupId = DocFindByName( d, "group/g" );
		bool closureHasGroup = false;
		for( NodeId id : closure ) if( id == groupId ) closureHasGroup = true;
		Check( closureHasGroup, "incremental(member): editing a member's closure includes the group (traced Reference edge)" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d, *j, closure, &di );
		Check( applied == 0 && !di.empty(),
			"incremental(member): a member edit whose closure includes a group still refuses -> full-derive fallback" );
		j->release();
	}

	// [incremental: unrelated edit stays incremental] an edit whose closure does NOT touch any group
	// (a scene that HAS a group elsewhere) is unaffected -- the guard is per-closure, not document-wide.
	{
		const std::string scene = Scene( Obj( "a" ) + Obj( "unrelated" ) + Grp( "g", { "a" }, "1 0 0" ) );
		Document d = ParseToCst( scene );
		Job* j = new Job(); std::vector<std::string> dd;
		DeriveToJob( d, *j, &dd );
		Check( dd.empty(), "incremental(unrelated): the full derive succeeds" );
		const NodeId unrelatedId = DocFindByName( d, "standard_object/unrelated" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d, *j, std::vector<NodeId>( 1, unrelatedId ), &di );
		Check( unrelatedId != 0 && applied >= 1 && di.empty(),
			"incremental(unrelated): an edit whose closure has no group stays incremental even though the DOCUMENT has one" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
