//////////////////////////////////////////////////////////////////////
//
//  GroupPanelCategoryTest.cpp - docs/agentic-redesign/86-object-grouping.md
//    slice 4 (the reverse index) + the CORE half of slice 5 (the GUI model
//    layer).  `group` becomes a first-class SceneEditController::Category, so
//    a shell can enumerate groups, walk a group's members, show the selected
//    group's transform, and edit it.
//
//    Cases:
//      [G1 reverse-index]  IJob::EnumerateGroupNames / GetGroupMemberCount /
//                          GetGroupMemberName / GetGroupOwnTransform.  Groups
//                          enumerate LEX-ordered (deliberately NOT document
//                          order -- the fixture declares zebra BEFORE alpha);
//                          MEMBERS enumerate in AUTHORED order (deliberately
//                          NOT sorted -- every group in the fixture lists its
//                          members in non-alphabetical order).
//      [G2 own-vs-accum]   MONEY: a group's OWN matrix is not the member-keyed
//                          ACCUMULATED one.  obj_a is in BOTH groups, so its
//                          accumulated G is `G_alpha * G_zebra`, which equals
//                          NEITHER group's own matrix.  Also pins the "own is
//                          recorded once, not composed per member" rule: a
//                          per-member compose would store G^2 for both of the
//                          fixture's two-member groups.
//      [G3 multi-group]    a member of two groups appears under BOTH in the
//                          forward index (the direction GetGroupMembership's
//                          comma-joined DIAGNOSTIC string cannot answer).
//      [G4 enumeration]    CategoryEntityCount/Name(Category::Group) surface
//                          the declared groups, and EntitySourceLocation
//                          resolves one (pins RoleKindSuffixForCategory's
//                          "group" arm).
//      [G5 panel]          SetSelection(Group, ...) round-trips and the
//                          properties panel carries position/orientation/scale
//                          with the CURRENT document values.  Also pins the
//                          KNOWN, INTENDED gap: `member` is a REPEATABLE
//                          descriptor param, and CstIntrospection::Inspect
//                          skips repeatables -- so there is NO `member` row (a
//                          garbled one-row-per-member would be a defect).
//      [G6 edit]           MONEY: SetPropertyForCategory(Group, "position", ...)
//                          applies, the CST carries the new value, and the
//                          MEMBERS' final world transforms move by exactly the
//                          group delta while an UNGROUPED control object does
//                          not.  Re-derive keeps the index consistent (still 2
//                          groups, still 2 members each, own matrix updated) --
//                          a stale/appended index would show 4 members.
//      [G8 controller]     MONEY for stage B: SceneEditController::GroupMemberCount /
//                          GroupMemberName / GroupMemberNames / GroupOwnTransform --
//                          the pass-through the GUI shells actually call (they hold a
//                          controller, not a Job).  Pins authored member order, the
//                          multi-group member, the own-vs-accumulated distinction, the
//                          negative directions, and -- because these serve a SNAPSHOT
//                          rather than a live read -- that the snapshot FOLLOWS edits
//                          and undo instead of freezing at first read.
//      [G9 reverse-map]    MONEY for the click-in-text -> reveal-in-outliner
//                          direction: SourceRefAtByteOffset on an offset inside
//                          a `group` chunk round-trips to (Category::Group,
//                          name) -- the ONLY exercise of CategoryForChunkKeyword's
//                          Group arm (SourceTraceTest's fixture has no group).
//      [G10 removal]       MONEY for the DROP direction: removing the `group`
//                          chunk shrinks EnumerateGroupNames, zeroes
//                          GroupMemberCount (which also proves
//                          RefreshGroupSnapshot_ actually PRUNES a deleted group
//                          rather than serving it forever from a name-keyed
//                          cache), and -- via SelectionStillResolves' Group arm
//                          -- drops a stale Group selection on the next
//                          undo/redo re-validation.
//      [G11 dup names]     Two `group` chunks may legally share a `name` (the
//                          parser has no cross-chunk collision check).  Members
//                          fold; the OWN transform is AMBIGUOUS and both the Job
//                          and the controller REFUSE it, leaving the caller's
//                          buffer untouched -- rather than serving a first-wins
//                          matrix that is wrong for the second chunk's members.
//                          Same name + IDENTICAL transform still resolves.
//      [G12 variant epoch] GUI-fix-round P2 (F3): RefreshGroupSnapshot_'s cheap
//                          validity stamp is (uuid, revision, mSceneEpoch); a
//                          scene_variant switch (Job::RederiveCstWithVariant) is
//                          the ONE re-derive path that restores uuid+revision
//                          VERBATIM while still bumping the epoch, so the epoch
//                          term is the SOLE guard across it.  ARCHITECTURAL
//                          FINDING pinned here (not assumed): a scene_variant
//                          switch CANNOT change a group's existence or members
//                          under the current derive -- `group`'s ChunkDescriptor
//                          never calls AddVariantTagParam (unlike every
//                          Material-category descriptor), and Cst.cpp's
//                          variant-override bake-scan is hard-gated to
//                          `ChunkCategory::Material` chunks only.  So this case
//                          cannot construct group content that diverges across a
//                          real epoch-only re-derive; it instead drives the exact
//                          API a shell uses to activate a variant
//                          (SetSelection(Category::SceneVariant, ...)) and pins
//                          what IS true and reachable: the switch preserves
//                          uuid+revision verbatim while bumping the epoch ("only
//                          the epoch moves"), and the group snapshot survives the
//                          switch's full Scene+managers teardown/rebuild intact.
//                          See the case body for the RED-PROVE result, which is
//                          reported rather than asserted in-tree.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IEnumCallback.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/CstIntrospection.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::Implementation;
using Category = SceneEditController::Category;
using AgentCommitResult = SceneEditController::AgentCommitResult;

namespace
{
	int g_pass = 0, g_fail = 0;
	void Check( bool c, const std::string& what )
	{
		if( c ) { ++g_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else    { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	// The fixture.  Two groups, BOTH deliberately adversarial to a naive index:
	//   * document order is grp_zebra THEN grp_alpha, so a document-ordered
	//     enumeration and a lex-ordered one disagree.
	//   * each group lists its members in NON-alphabetical order, so an
	//     authored-order list and a sorted one disagree.
	//   * obj_a belongs to BOTH groups (the multi-group case), and obj_free
	//     belongs to NEITHER (the ungrouped control for the edit test).
	const char* const kScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 8\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt\ncolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\nname mat\nreflectance pnt\n}\n\n"
		"sphere_geometry\n{\nname sph\nradius 0.5\n}\n\n"
		"standard_object\n{\nname obj_a\ngeometry sph\nmaterial mat\nposition 1 0 0\n}\n\n"
		"standard_object\n{\nname obj_b\ngeometry sph\nmaterial mat\nposition 0 1 0\n}\n\n"
		"standard_object\n{\nname obj_c\ngeometry sph\nmaterial mat\nposition 0 0 1\n}\n\n"
		"standard_object\n{\nname obj_free\ngeometry sph\nmaterial mat\nposition 3 3 3\n}\n\n"
		"group\n{\nname grp_zebra\nmember obj_c\nmember obj_a\nposition 0 5 0\n}\n\n"
		"group\n{\nname grp_alpha\nmember obj_b\nmember obj_a\nposition 2 0 0\n"
			"orientation 0 90 0\nscale 1 2 3\n}\n";

	std::string TempPath( const char* name )
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		return dir + name;
	}

	Job* LoadScene( const char* text, const std::string& path )
	{
		{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
		Job* pJob = new Job();
		if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) )
		{
			pJob->release();
			std::remove( path.c_str() );
			return nullptr;
		}
		return pJob;
	}

	// ---- matrix helpers (CstSliceThreeExpansionTest's ObjMat16 / Mat16Eq shape) ----

	void Mat16Of( const Matrix4& m, double out[16] )
	{
		out[ 0] = m._00; out[ 1] = m._01; out[ 2] = m._02; out[ 3] = m._03;
		out[ 4] = m._10; out[ 5] = m._11; out[ 6] = m._12; out[ 7] = m._13;
		out[ 8] = m._20; out[ 9] = m._21; out[10] = m._22; out[11] = m._23;
		out[12] = m._30; out[13] = m._31; out[14] = m._32; out[15] = m._33;
	}

	// An object's FINAL world transform, re-looked-up from the job every call --
	// a group edit falls back to a FULL re-derive, which REPLACES the Scene and
	// its managers, so a cached IObject* would dangle.
	bool ObjMat16( Job& j, const char* obj, double out[16] )
	{
		for( int i = 0; i < 16; ++i ) out[i] = 0.0;
		IScenePriv* s = j.GetScene();
		if( !s ) return false;
		const IObjectManager* m = s->GetObjects();
		if( !m ) return false;
		const IObject* o = const_cast<IObjectManager*>( m )->GetItem( obj );
		if( !o ) return false;
		Mat16Of( o->GetFinalTransformMatrix(), out );
		return true;
	}

	bool Mat16Eq( const double a[16], const double b[16] )
	{
		for( int i = 0; i < 16; ++i ) if( std::fabs( a[i] - b[i] ) > 1e-9 ) return false;
		return true;
	}

	// The SAME composition GroupAsciiChunkParser::Finalize performs.  This test is
	// about the INDEX and the CONTROLLER surface, not about re-pinning the parser's
	// Euler axis order -- GroupChunkTest's [compose] case already does that against
	// an independently hand-multiplied expectation.
	Matrix4 GroupTRS( double px, double py, double pz,
	                  double ox, double oy, double oz,
	                  double sx, double sy, double sz )
	{
		return Matrix4Ops::Translation( Vector3( px, py, pz ) ) *
			( Matrix4Ops::XRotation( ox * DEG_TO_RAD ) *
			  Matrix4Ops::YRotation( oy * DEG_TO_RAD ) *
			  Matrix4Ops::ZRotation( oz * DEG_TO_RAD ) ) *
			Matrix4Ops::Stretch( Vector3( sx, sy, sz ) );
	}

	std::vector<std::string> GroupNames( Job& j )
	{
		struct Cb : public IEnumCallback<const char*> {
			std::vector<std::string> names;
			bool operator()( const char* const& n ) override { if( n ) names.push_back( n ); return true; }
		};
		Cb cb;
		j.EnumerateGroupNames( cb );
		return cb.names;
	}

	std::vector<std::string> GroupMembers( Job& j, const char* group )
	{
		std::vector<std::string> out;
		const unsigned int n = j.GetGroupMemberCount( group );
		for( unsigned int i = 0; i < n; ++i ) {
			char buf[256] = { 0 };
			if( j.GetGroupMemberName( group, i, buf, sizeof( buf ) ) ) out.push_back( buf );
		}
		return out;
	}

	bool Contains( const std::vector<std::string>& v, const char* s )
	{
		for( const std::string& e : v ) if( e == s ) return true;
		return false;
	}

	int RowIndexFor( SceneEditController& ctrl, Category cat, const std::string& rowName )
	{
		const unsigned int n = ctrl.PropertyCountFor( cat );
		for( unsigned int i = 0; i < n; ++i )
			if( std::string( ctrl.PropertyNameFor( cat, i ).c_str() ) == rowName ) return static_cast<int>( i );
		return -1;
	}

	std::string CstValueOf( Job* pJob, const char* entity, const char* suffix, const char* param )
	{
		const std::vector<CameraProperty> rows = CstIntrospection::Inspect(
			pJob->GetCstDocument(), *pJob, String( entity ), suffix, "type" );
		for( const CameraProperty& r : rows )
			if( std::string( r.name.c_str() ) == param ) return std::string( r.value.c_str() );
		return std::string( "<missing>" );
	}
}

int main()
{
	std::printf( "=== GroupPanelCategoryTest ===\n" );
	const std::string tmp = TempPath( "group_panel_category.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture scene loads via CST" );
	if( !pJob ) { std::printf( "=== GroupPanelCategoryTest: %d passed, %d failed ===\n", g_pass, g_fail + 1 ); return 1; }

	// The two group matrices, as authored.
	const Matrix4 gZebra = GroupTRS( 0, 5, 0,  0,  0, 0,  1, 1, 1 );
	const Matrix4 gAlpha = GroupTRS( 2, 0, 0,  0, 90, 0,  1, 2, 3 );

	{
		SceneEditController ctrl( *pJob, nullptr );   // skeleton: no interactive rasterizer

		// ---------- G1: the reverse index ----------
		std::printf( "G1: forward (group -> members) index...\n" );
		{
			const std::vector<std::string> names = GroupNames( *pJob );
			Check( names.size() == 2, "EnumerateGroupNames yields 2 groups" );
			// The fixture declares grp_zebra FIRST; lex order puts grp_alpha first.
			Check( names.size() == 2 && names[0] == "grp_alpha" && names[1] == "grp_zebra",
				"MONEY (G1): groups enumerate LEX-ordered, not document-ordered" );

			const std::vector<std::string> mz = GroupMembers( *pJob, "grp_zebra" );
			Check( mz.size() == 2 && mz[0] == "obj_c" && mz[1] == "obj_a",
				"MONEY (G1): grp_zebra members are AUTHORED order [obj_c, obj_a], not sorted" );
			const std::vector<std::string> ma = GroupMembers( *pJob, "grp_alpha" );
			Check( ma.size() == 2 && ma[0] == "obj_b" && ma[1] == "obj_a",
				"MONEY (G1): grp_alpha members are AUTHORED order [obj_b, obj_a], not sorted" );

			// Negative directions.
			Check( pJob->GetGroupMemberCount( "nope" ) == 0, "unknown group has 0 members" );
			char buf[64] = { 0 };
			Check( !pJob->GetGroupMemberName( "grp_zebra", 2, buf, sizeof( buf ) ),
				"out-of-range member index returns false" );
			Check( !pJob->GetGroupMemberName( "nope", 0, buf, sizeof( buf ) ),
				"unknown group's member lookup returns false" );
			double junk[16];
			Check( !pJob->GetGroupOwnTransform( "nope", junk ), "unknown group has no own transform" );
			// An OBJECT name is not a group name (the index is keyed by group, and a
			// member must never masquerade as one).
			Check( pJob->GetGroupMemberCount( "obj_a" ) == 0, "a member name is not a group name" );
		}

		// ---------- G2: the group's OWN matrix vs the member-keyed ACCUMULATED one ----------
		std::printf( "G2: own transform vs accumulated membership transform...\n" );
		{
			double ownZ[16] = { 0 }, ownA[16] = { 0 };
			Check( pJob->GetGroupOwnTransform( "grp_zebra", ownZ ), "grp_zebra own transform reads" );
			Check( pJob->GetGroupOwnTransform( "grp_alpha", ownA ), "grp_alpha own transform reads" );

			double expZ[16], expA[16];
			Mat16Of( gZebra, expZ );
			Mat16Of( gAlpha, expA );
			// A per-member compose (`own = G * own` inside NoteGroupMembership, which
			// fires ONCE PER MEMBER) would store G^2 here -- both fixture groups have
			// exactly two members, so both of these fail on that bug.
			Check( Mat16Eq( ownZ, expZ ),
				"MONEY (G2): grp_zebra own == Translation(0,5,0) (recorded once, not composed per member)" );
			Check( Mat16Eq( ownA, expA ),
				"MONEY (G2): grp_alpha own == T(2,0,0)*Ry(90)*S(1,2,3)" );

			// obj_a is in BOTH groups, so its ACCUMULATED matrix is G_alpha * G_zebra --
			// which is neither group's own matrix.  This is the whole reason the forward
			// index had to be added rather than reused from GetGroupMembership.
			double accumA[16] = { 0 };
			char owners[256] = { 0 };
			Check( pJob->GetGroupMembership( "obj_a", accumA, owners, sizeof( owners ) ),
				"obj_a has a membership record" );
			double expAccum[16];
			Mat16Of( gAlpha * gZebra, expAccum );
			Check( Mat16Eq( accumA, expAccum ),
				"obj_a accumulated == G_alpha * G_zebra (later group outermost)" );
			Check( !Mat16Eq( accumA, ownA ) && !Mat16Eq( accumA, ownZ ),
				"MONEY (G2): the accumulated member matrix equals NEITHER group's own matrix" );
		}

		// ---------- G3: a member of two groups ----------
		std::printf( "G3: multi-group member...\n" );
		{
			Check( Contains( GroupMembers( *pJob, "grp_zebra" ), "obj_a" ) &&
			       Contains( GroupMembers( *pJob, "grp_alpha" ), "obj_a" ),
				"MONEY (G3): obj_a appears under BOTH groups in the forward index" );
			Check( !Contains( GroupMembers( *pJob, "grp_zebra" ), "obj_b" ) &&
			       !Contains( GroupMembers( *pJob, "grp_alpha" ), "obj_c" ),
				"G3: single-group members do not leak across groups" );
			Check( !Contains( GroupMembers( *pJob, "grp_zebra" ), "obj_free" ) &&
			       !Contains( GroupMembers( *pJob, "grp_alpha" ), "obj_free" ),
				"G3: the ungrouped control belongs to no group" );
		}

		// ---------- G4: Category::Group enumeration ----------
		std::printf( "G4: Category::Group enumeration...\n" );
		{
			Check( ctrl.CategoryEntityCount( Category::Group ) == 2,
				"CategoryEntityCount(Group) == 2" );
			Check( std::string( ctrl.CategoryEntityName( Category::Group, 0 ).c_str() ) == "grp_alpha" &&
			       std::string( ctrl.CategoryEntityName( Category::Group, 1 ).c_str() ) == "grp_zebra",
				"MONEY (G4): the controller lists groups in the documented lex order" );
			Check( ctrl.CategoryEntityName( Category::Group, 99 ).size() <= 1,
				"G4: out-of-range group index returns empty" );
			Check( ctrl.CategoryActiveName( Category::Group ).size() <= 1,
				"G4: Group has no `active` entity (like Painter/Geometry)" );

			// Pins RoleKindSuffixForCategory's Group arm: resolving (Group, name) to a
			// chunk goes through it, and there is no `*_group` suffix -- the match is
			// RoleMatchesKindConstraint's `role == roleKindSuffix` fast path.
			std::uint64_t off = 0; std::uint32_t line = 0;
			Check( ctrl.EntitySourceLocation( Category::Group, String( "grp_alpha" ), off, line ) && line > 1,
				"MONEY (G4): EntitySourceLocation resolves a group chunk (role-kind suffix `group`)" );
			Check( !ctrl.EntitySourceLocation( Category::Group, String( "nope" ), off, line ),
				"G4: an unknown group name does not resolve to a source location" );
		}

		// ---------- G5: selection + property rows ----------
		std::printf( "G5: selection round-trip and property rows...\n" );
		{
			Check( ctrl.SetSelection( Category::Group, String( "grp_alpha" ) ),
				"SetSelection(Group, grp_alpha) succeeds" );
			Check( ctrl.GetSelectionCategory() == Category::Group &&
			       std::string( ctrl.GetSelectionName().c_str() ) == "grp_alpha",
				"MONEY (G5): the (Group, grp_alpha) selection round-trips" );
			Check( std::string( ctrl.GetSelectionNameForCategory( Category::Group ).c_str() ) == "grp_alpha",
				"G5: the per-category selection slot carries the group" );
			Check( ctrl.IsSectionExpanded( Category::Group ), "G5: the Group section expands on selection" );
			// Painter/Geometry precedent: no dedicated PanelMode, so no header.
			Check( ctrl.CurrentPanelMode() == SceneEditController::PanelMode::None,
				"G5: Group keeps the Painter/Geometry PanelMode::None convention" );

			ctrl.RefreshProperties();
			const int posIdx = RowIndexFor( ctrl, Category::Group, "position" );
			const int oriIdx = RowIndexFor( ctrl, Category::Group, "orientation" );
			const int sclIdx = RowIndexFor( ctrl, Category::Group, "scale" );
			Check( posIdx >= 0 && oriIdx >= 0 && sclIdx >= 0,
				"MONEY (G5): the panel surfaces position / orientation / scale rows" );
			if( posIdx >= 0 && oriIdx >= 0 && sclIdx >= 0 )
			{
				Check( std::string( ctrl.PropertyValueFor( Category::Group, posIdx ).c_str() ) == "2 0 0" &&
				       std::string( ctrl.PropertyValueFor( Category::Group, oriIdx ).c_str() ) == "0 90 0" &&
				       std::string( ctrl.PropertyValueFor( Category::Group, sclIdx ).c_str() ) == "1 2 3",
					"MONEY (G5): the rows carry the CURRENT document values" );
				Check( ctrl.PropertyKindFor( Category::Group, posIdx ) == static_cast<int>( ValueKind::DoubleVec3 ),
					"G5: `position` is a TYPED DoubleVec3 row (descriptor-driven)" );
				Check( ctrl.PropertyEditableFor( Category::Group, posIdx ),
					"G5: `position` is editable" );
			}
			const int typeIdx = RowIndexFor( ctrl, Category::Group, "type" );
			Check( typeIdx == 0 && !ctrl.PropertyEditableFor( Category::Group, 0 ) &&
			       std::string( ctrl.PropertyValueFor( Category::Group, 0 ).c_str() ) == "group",
				"G5: leading read-only `type` identity row reads `group`" );
			// KNOWN, INTENDED gap (confirmed, not a defect): `member` is a REPEATABLE
			// descriptor param and CstIntrospection::Inspect skips repeatables, so it
			// produces NO row -- rather than one garbled row per member.  Members are a
			// tree, shown by the outliner (stage B/C) via GetGroupMemberName.
			Check( RowIndexFor( ctrl, Category::Group, "member" ) < 0,
				"MONEY (G5): no `member` row -- Inspect skips REPEATABLE params by design" );
			Check( ctrl.PropertyCountFor( Category::Group ) == 4,
				"G5: exactly 4 rows (type + position + orientation + scale); no per-member row crept in" );
			// The shells' single-panel path reads the PRIMARY snapshot.
			Check( ctrl.PropertyCount() > 0,
				"G5: the PRIMARY property snapshot carries the group rows (the shells' panel path)" );
		}

		// ---------- G6: edit a group's transform ----------
		std::printf( "G6: group transform edit moves its members...\n" );
		{
			double aBefore[16], cBefore[16], freeBefore[16], bBefore[16];
			Check( ObjMat16( *pJob, "obj_a", aBefore ) && ObjMat16( *pJob, "obj_c", cBefore ) &&
			       ObjMat16( *pJob, "obj_b", bBefore ) && ObjMat16( *pJob, "obj_free", freeBefore ),
				"G6: member + control transforms snapshot" );

			Check( ctrl.SetSelection( Category::Group, String( "grp_zebra" ) ),
				"G6: grp_zebra selected" );
			Check( ctrl.SetPropertyForCategory( Category::Group, String( "position" ), String( "0 9 0" ) ),
				"MONEY (G6): SetPropertyForCategory(Group, position) APPLIES" );
			Check( CstValueOf( pJob, "grp_zebra", "group", "position" ) == "0 9 0",
				"MONEY (G6): the CST now carries `position 0 9 0`" );

			// obj_c is in grp_zebra ONLY, so its final transform is G_zebra * M_c and the
			// edit is exactly a +4 pre-multiplied Y translation.
			double cAfter[16];
			Check( ObjMat16( *pJob, "obj_c", cAfter ), "G6: obj_c still resolves after the re-derive" );
			double cExpected[16];
			{
				const Matrix4 before(
					cBefore[ 0], cBefore[ 1], cBefore[ 2], cBefore[ 3],
					cBefore[ 4], cBefore[ 5], cBefore[ 6], cBefore[ 7],
					cBefore[ 8], cBefore[ 9], cBefore[10], cBefore[11],
					cBefore[12], cBefore[13], cBefore[14], cBefore[15] );
				Mat16Of( Matrix4Ops::Translation( Vector3( 0, 4, 0 ) ) * before, cExpected );
			}
			Check( Mat16Eq( cAfter, cExpected ),
				"MONEY (G6): the single-group member moved by EXACTLY the group delta (no double-compose)" );

			// obj_a is in BOTH groups: final = G_alpha * G_zebra' * M_a.
			double aAfter[16];
			Check( ObjMat16( *pJob, "obj_a", aAfter ), "G6: obj_a still resolves" );
			double aExpected[16];
			Mat16Of( gAlpha * GroupTRS( 0, 9, 0, 0, 0, 0, 1, 1, 1 ) *
			         Matrix4Ops::Translation( Vector3( 1, 0, 0 ) ), aExpected );
			Check( Mat16Eq( aAfter, aExpected ),
				"MONEY (G6): the multi-group member recomposed as G_alpha * G_zebra' * M_a" );

			// The other group's exclusive member and the ungrouped control must not move.
			double bAfter[16], freeAfter[16];
			ObjMat16( *pJob, "obj_b", bAfter );
			ObjMat16( *pJob, "obj_free", freeAfter );
			Check( Mat16Eq( bAfter, bBefore ), "G6: a member of the OTHER group did not move" );
			Check( Mat16Eq( freeAfter, freeBefore ), "MONEY (G6): the UNGROUPED control did not move" );
			Check( !Mat16Eq( aAfter, aBefore ) && !Mat16Eq( cAfter, cBefore ),
				"G6: both edited-group members actually moved" );

			// The index survived the re-derive CLEANLY: it is cleared in
			// InitializeContainers and rebuilt.  A missing clear would append the
			// members again (4 per group) and keep the stale own matrix.
			Check( GroupNames( *pJob ).size() == 2, "MONEY (G6): still exactly 2 groups after the re-derive" );
			const std::vector<std::string> mz = GroupMembers( *pJob, "grp_zebra" );
			Check( mz.size() == 2 && mz[0] == "obj_c" && mz[1] == "obj_a",
				"MONEY (G6): grp_zebra still has exactly 2 members in authored order (index was CLEARED, not appended)" );
			double ownZ[16], expZ[16];
			Check( pJob->GetGroupOwnTransform( "grp_zebra", ownZ ), "G6: grp_zebra own transform re-reads" );
			Mat16Of( GroupTRS( 0, 9, 0, 0, 0, 0, 1, 1, 1 ), expZ );
			Check( Mat16Eq( ownZ, expZ ), "MONEY (G6): the own matrix reflects the EDITED position" );

			// The controller surface is consistent post-edit too.
			Check( ctrl.CategoryEntityCount( Category::Group ) == 2,
				"G6: the Group category still enumerates 2 groups" );
			ctrl.RefreshProperties();
			const int posIdx = RowIndexFor( ctrl, Category::Group, "position" );
			Check( posIdx >= 0 &&
			       std::string( ctrl.PropertyValueFor( Category::Group, posIdx ).c_str() ) == "0 9 0",
				"G6: the panel row reflects the edited value" );

			// Undo restores the authored transform end-to-end.
			ctrl.Undo();
			Check( CstValueOf( pJob, "grp_zebra", "group", "position" ) == "0 5 0",
				"MONEY (G6): Undo restores `position 0 5 0` in the CST" );
			double cUndone[16];
			Check( ObjMat16( *pJob, "obj_c", cUndone ) && Mat16Eq( cUndone, cBefore ),
				"MONEY (G6): Undo restores the member's world transform" );
		}

		// ---------- G7: an empty selection edits nothing ----------
		std::printf( "G7: empty-target guard...\n" );
		{
			ctrl.ForTest_SetSelection( Category::Group, String() );
			Check( !ctrl.SetPropertyForCategory( Category::Group, String( "position" ), String( "1 1 1" ) ),
				"G7: a Group edit with no group selected is refused" );
		}

		// ---------- G8: the CONTROLLER's group-membership pass-through ----------
		// arc-86 slice 5 stage B.  The GUI shells hold a SceneEditController*, not
		// a Job*, so the IJob virtuals G1-G3 exercised are unreachable from them;
		// these three forwarders are the only way a shell can render the outliner's
		// third tree level.  They serve a SNAPSHOT (same try_lock + stale-fallback
		// discipline as CategoryEntityCount/Name), so this block also pins that the
		// snapshot actually TRACKS the document rather than freezing at first read.
		//
		// Runs LAST deliberately: G6 re-derived the scene (a group edit always
		// falls back to a FULL derive, replacing the Scene and its managers) and
		// then Undid it, so a snapshot that had cached the pre-edit -- or the
		// mid-edit -- state would be visibly wrong here.
		std::printf( "G8: SceneEditController group pass-through...\n" );
		{
			Check( ctrl.GroupMemberCount( String( "grp_zebra" ) ) == 2 &&
			       ctrl.GroupMemberCount( String( "grp_alpha" ) ) == 2,
				"G8: the controller reports 2 members per group" );

			// AUTHORED order, not sorted -- the whole reason this is a separate
			// surface from CategoryEntityName (which is LEX-ordered, pinned in G4).
			Check( std::string( ctrl.GroupMemberName( String( "grp_zebra" ), 0 ).c_str() ) == "obj_c" &&
			       std::string( ctrl.GroupMemberName( String( "grp_zebra" ), 1 ).c_str() ) == "obj_a",
				"MONEY (G8): controller grp_zebra members are AUTHORED order [obj_c, obj_a]" );
			Check( std::string( ctrl.GroupMemberName( String( "grp_alpha" ), 0 ).c_str() ) == "obj_b" &&
			       std::string( ctrl.GroupMemberName( String( "grp_alpha" ), 1 ).c_str() ) == "obj_a",
				"MONEY (G8): controller grp_alpha members are AUTHORED order [obj_b, obj_a]" );

			// The multi-group member, through the controller: obj_a under BOTH.
			Check( std::string( ctrl.GroupMemberName( String( "grp_zebra" ), 1 ).c_str() ) == "obj_a" &&
			       std::string( ctrl.GroupMemberName( String( "grp_alpha" ), 1 ).c_str() ) == "obj_a",
				"MONEY (G8): a member of TWO groups is served under both by the controller" );

			// Negatives: unknown group, out-of-range index, empty name, and an
			// OBJECT name (a member must never resolve as a group).
			Check( ctrl.GroupMemberCount( String( "nope" ) ) == 0, "G8: unknown group has 0 members" );
			Check( ctrl.GroupMemberCount( String( "obj_a" ) ) == 0, "G8: a member name is not a group name" );
			Check( ctrl.GroupMemberCount( String() ) == 0, "G8: an empty group name has 0 members" );
			Check( ctrl.GroupMemberName( String( "grp_zebra" ), 2 ).size() <= 1,
				"G8: out-of-range member index returns empty" );
			Check( ctrl.GroupMemberName( String( "nope" ), 0 ).size() <= 1,
				"G8: unknown group's member lookup returns empty" );

			// The group's OWN matrix, not the member-keyed accumulated one.
			double ownA[16] = { 0 }, ownZ[16] = { 0 }, expA[16], expZ[16];
			Check( ctrl.GroupOwnTransform( String( "grp_alpha" ), ownA ), "G8: grp_alpha own transform reads" );
			Check( ctrl.GroupOwnTransform( String( "grp_zebra" ), ownZ ), "G8: grp_zebra own transform reads" );
			Mat16Of( gAlpha, expA );
			Mat16Of( gZebra, expZ );
			Check( Mat16Eq( ownA, expA ),
				"MONEY (G8): controller grp_alpha own == T(2,0,0)*Ry(90)*S(1,2,3)" );
			Check( Mat16Eq( ownZ, expZ ),
				"MONEY (G8): controller grp_zebra own == T(0,5,0) (the G6 edit was UNDONE)" );

			// obj_a's ACCUMULATED matrix is G_alpha * G_zebra -- which the
			// controller surface must NOT be serving in place of either own matrix.
			double accum[16];
			Mat16Of( gAlpha * gZebra, accum );
			Check( !Mat16Eq( ownA, accum ) && !Mat16Eq( ownZ, accum ),
				"MONEY (G8): neither own matrix is the accumulated multi-group product" );

			double junk[16];
			Check( !ctrl.GroupOwnTransform( String( "nope" ), junk ),
				"G8: unknown group has no own transform" );
			Check( !ctrl.GroupOwnTransform( String(), junk ),
				"G8: an empty group name has no own transform" );

			// The snapshot TRACKS the document.  A cache that primed once and
			// never refreshed passes every assertion above and fails these.
			Check( ctrl.SetSelection( Category::Group, String( "grp_alpha" ) ), "G8: grp_alpha selected" );
			Check( ctrl.SetPropertyForCategory( Category::Group, String( "position" ), String( "7 0 0" ) ),
				"G8: grp_alpha position edit applies" );
			double ownA2[16] = { 0 }, expA2[16];
			Check( ctrl.GroupOwnTransform( String( "grp_alpha" ), ownA2 ), "G8: grp_alpha own re-reads post-edit" );
			Mat16Of( GroupTRS( 7, 0, 0, 0, 90, 0, 1, 2, 3 ), expA2 );
			Check( Mat16Eq( ownA2, expA2 ),
				"MONEY (G8): the controller's own-transform snapshot FOLLOWED the edit (not frozen)" );
			Check( ctrl.GroupMemberCount( String( "grp_alpha" ) ) == 2 &&
			       std::string( ctrl.GroupMemberName( String( "grp_alpha" ), 0 ).c_str() ) == "obj_b",
				"MONEY (G8): the member list survived the re-derive intact (not appended/duplicated)" );
			ctrl.Undo();
			double ownA3[16] = { 0 };
			Check( ctrl.GroupOwnTransform( String( "grp_alpha" ), ownA3 ) && Mat16Eq( ownA3, expA ),
				"MONEY (G8): Undo is reflected in the controller's snapshot too" );

			// The list accessor the shells actually call (F1): ONE refresh, ONE
			// lock hold, internally consistent -- as opposed to the count-then-N
			// walk above, which reads the snapshot N+1 separate times.  Must agree
			// with the indexed reads exactly.
			const std::vector<String> zl = ctrl.GroupMemberNames( String( "grp_zebra" ) );
			Check( zl.size() == 2 &&
			       std::string( zl[0].c_str() ) == "obj_c" &&
			       std::string( zl[1].c_str() ) == "obj_a",
				"MONEY (G8): GroupMemberNames returns the whole AUTHORED-order list in one call" );
			const std::vector<String> al = ctrl.GroupMemberNames( String( "grp_alpha" ) );
			Check( al.size() == ctrl.GroupMemberCount( String( "grp_alpha" ) ) &&
			       al.size() == 2 && std::string( al[1].c_str() ) == "obj_a",
				"G8: GroupMemberNames agrees with GroupMemberCount/Name" );
			Check( ctrl.GroupMemberNames( String( "nope" ) ).empty() &&
			       ctrl.GroupMemberNames( String() ).empty() &&
			       ctrl.GroupMemberNames( String( "obj_a" ) ).empty(),
				"G8: GroupMemberNames is empty for unknown / empty / member-as-group names" );
			// No entry may be blank: a blank name would render as a clickable
			// outliner row addressing an object that does not exist.
			bool anyBlank = false;
			for( const String& m : zl ) if( m.size() <= 1 ) anyBlank = true;
			for( const String& m : al ) if( m.size() <= 1 ) anyBlank = true;
			Check( !anyBlank, "MONEY (G8): GroupMemberNames never yields a blank member name" );
		}

		// ---------- G9: click-in-text -> (Category::Group, name) ----------
		// Pins CategoryForChunkKeyword's `case ChunkCategory::Group` arm, which is
		// what makes a cursor landing inside a `group` chunk reveal that group in
		// the outliner.  SourceTraceTest's fixture contains no `group` chunk, so
		// without this the arm is dead-testable: deleting it passes that suite.
		std::printf( "G9: reverse map (byte offset -> Category::Group)...\n" );
		{
			std::uint64_t off = 0; std::uint32_t line = 0;
			Check( ctrl.EntitySourceLocation( Category::Group, String( "grp_alpha" ), off, line ),
				"G9: grp_alpha resolves to a source location" );

			Category rcat = Category::None;
			String rname, rparam;
			Check( ctrl.SourceRefAtByteOffset( off, rcat, rname, rparam ) &&
			       rcat == Category::Group &&
			       std::string( rname.c_str() ) == "grp_alpha",
				"MONEY (G9): an offset on the `group` chunk header round-trips to (Group, grp_alpha)" );

			// And a PARAM offset inside the same chunk, forward-then-reverse: the
			// full round-trip the text editor's cursor actually performs.
			SceneEditController::SourceSpan span;
			Check( ctrl.ResolveSourceSpan( Category::Group, String( "grp_alpha" ),
			                               String( "position" ), 0, span ) && span.present,
				"G9: grp_alpha's `position` param resolves to a span" );
			if( span.present )
			{
				Category pcat = Category::None;
				String pname, pparam;
				Check( ctrl.SourceRefAtByteOffset( span.byteOffset, pcat, pname, pparam ) &&
				       pcat == Category::Group &&
				       std::string( pname.c_str() ) == "grp_alpha" &&
				       std::string( pparam.c_str() ) == "position",
					"MONEY (G9): forward->reverse round-trips to (Group, grp_alpha, position)" );
			}
		}

		// ---------- G10: removing the group chunk ----------
		// Three things at once, all of them otherwise untested:
		//   * EnumerateGroupNames / the Group category SHRINK (the index is
		//     rebuilt, not appended to, across a structural edit);
		//   * the controller's group snapshot PRUNES the removed group -- the
		//     stated reason RefreshGroupSnapshot_ rebuilds wholesale.  A cache
		//     that only ever ADDED entries passes every other case in this file;
		//   * SelectionStillResolves' `case Cat::Group` arm drops a stale Group
		//     selection.  Without it the switch falls through to `default: return
		//     true` and the panel keeps addressing a group that no longer derives.
		std::printf( "G10: group removal drops the group, its members, and the selection...\n" );
		{
			Check( GroupNames( *pJob ).size() == 2 && ctrl.CategoryEntityCount( Category::Group ) == 2,
				"G10: two groups before the removal" );
			Check( ctrl.GroupMemberCount( String( "grp_zebra" ) ) == 2,
				"G10: grp_zebra has members before the removal" );

			const AgentCommitResult rm = ctrl.RemoveEntity( Category::Group, String( "grp_zebra" ) );
			Check( rm.applied, "G10: RemoveEntity(Group, grp_zebra) applies" );

			Check( GroupNames( *pJob ).size() == 1 &&
			       GroupNames( *pJob )[0] == "grp_alpha",
				"MONEY (G10): EnumerateGroupNames SHRANK to just grp_alpha" );
			Check( ctrl.CategoryEntityCount( Category::Group ) == 1,
				"G10: the Group category enumerates one group" );
			Check( ctrl.GroupMemberCount( String( "grp_zebra" ) ) == 0 &&
			       ctrl.GroupMemberNames( String( "grp_zebra" ) ).empty(),
				"MONEY (G10): the removed group's members are PRUNED from the snapshot (not served stale)" );
			double gone[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
			Check( !ctrl.GroupOwnTransform( String( "grp_zebra" ), gone ) &&
			       gone[0] == 1.0 && gone[15] == 16.0,
				"MONEY (G10): GroupOwnTransform refuses a removed group and leaves outMatrix UNTOUCHED" );
			// The surviving group is unaffected.
			Check( ctrl.GroupMemberCount( String( "grp_alpha" ) ) == 2,
				"G10: the surviving group is untouched" );

			// The stale-selection arm.  Force a selection at the now-gone group
			// (RemoveEntity clears its own selection), then drive the
			// re-validation that Undo/Redo perform unconditionally.
			ctrl.ForTest_SetSelection( Category::Group, String( "grp_zebra" ) );
			Check( std::string( ctrl.GetSelectionName().c_str() ) == "grp_zebra",
				"G10: the stale Group selection is in place" );
			ctrl.Redo();   // DropStaleSelection_ runs on ANY redo attempt, work or not
			Check( ctrl.GetSelectionName().size() <= 1,
				"MONEY (G10): the stale Group selection was DROPPED (SelectionStillResolves' Group arm)" );
			Check( std::string( ctrl.GetSelectionNameForCategory( Category::Group ).c_str() ).empty(),
				"G10: the per-category Group selection slot cleared too" );
		}
	}

	pJob->release();
	std::remove( tmp.c_str() );

	// ---------- G11: DUPLICATE group names ----------
	// Two `group` chunks may legally share a `name` -- a group creates no manager
	// entity, so the parser performs NO cross-chunk name-collision check (see the
	// `group` descriptor, which says so explicitly).  The forward index folds them
	// into one record: members concatenate, and `own` can only be one of the two
	// matrices.  A first-wins `own` is simply WRONG for the second chunk's members,
	// so the record is marked ambiguous and GetGroupOwnTransform REFUSES -- matching
	// how the properties path already degrades (DocFindByNameAnyRole finds 2,
	// returns 0, panel blanks, edit refused).  Separate fixture + Job: the collision
	// has to exist from the first derive.
	std::printf( "G11: duplicate group names...\n" );
	{
		const char* const kDupScene =
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
			"film\n{\nwidth 16\nheight 16\n}\n\n"
			"pinhole_camera\n{\nlocation 0 0 8\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
			"uniformcolor_painter\n{\nname pnt\ncolor 0.5 0.5 0.5\n}\n\n"
			"lambertian_material\n{\nname mat\nreflectance pnt\n}\n\n"
			"sphere_geometry\n{\nname sph\nradius 0.5\n}\n\n"
			"standard_object\n{\nname obj_p\ngeometry sph\nmaterial mat\nposition 1 0 0\n}\n\n"
			"standard_object\n{\nname obj_q\ngeometry sph\nmaterial mat\nposition 0 1 0\n}\n\n"
			"standard_object\n{\nname obj_r\ngeometry sph\nmaterial mat\nposition 0 0 1\n}\n\n"
			"standard_object\n{\nname obj_s\ngeometry sph\nmaterial mat\nposition 2 2 2\n}\n\n"
			// SAME name, DIFFERENT transform -> ambiguous.
			"group\n{\nname dup\nmember obj_p\nposition 0 5 0\n}\n\n"
			"group\n{\nname dup\nmember obj_q\nposition 0 -5 0\n}\n\n"
			// SAME name, SAME transform -> NOT ambiguous (one matrix is correct for
			// every member, so refusing would be over-strict).
			"group\n{\nname twin\nmember obj_r\nposition 1 1 1\n}\n\n"
			"group\n{\nname twin\nmember obj_s\nposition 1 1 1\n}\n";

		const std::string dupTmp = TempPath( "group_panel_dupname.RISEscene" );
		Job* pDup = LoadScene( kDupScene, dupTmp );
		Check( pDup != nullptr, "G11: duplicate-name fixture loads (the parser does NOT reject it)" );
		if( pDup )
		{
			SceneEditController dctrl( *pDup, nullptr );

			// The name is still DECLARED and its members still enumerate: folding the
			// lists is the honest answer to "which objects are in a group called dup".
			Check( pDup->IsGroupDeclared( "dup" ), "G11: the duplicated name is declared" );
			const std::vector<std::string> dm = GroupMembers( *pDup, "dup" );
			Check( dm.size() == 2 && dm[0] == "obj_p" && dm[1] == "obj_q",
				"G11: both chunks' members fold into one list, in document order" );
			Check( GroupNames( *pDup ).size() == 2,
				"G11: the two same-named chunks are ONE entry (dup + twin)" );

			// MONEY: the own transform is refused, and the caller's buffer is untouched.
			double sentinel[16];
			for( int k = 0; k < 16; ++k ) sentinel[k] = 99.0 + k;
			Check( !pDup->GetGroupOwnTransform( "dup", sentinel ),
				"MONEY (G11): IJob::GetGroupOwnTransform REFUSES an ambiguous duplicate name" );
			bool untouched = true;
			for( int k = 0; k < 16; ++k ) if( sentinel[k] != 99.0 + k ) untouched = false;
			Check( untouched, "MONEY (G11): the refusal left outMatrix untouched" );
			double cSentinel[16];
			for( int k = 0; k < 16; ++k ) cSentinel[k] = 55.0 + k;
			Check( !dctrl.GroupOwnTransform( String( "dup" ), cSentinel ),
				"MONEY (G11): the CONTROLLER refuses it too (ownValid, not a zero matrix)" );
			bool cUntouched = true;
			for( int k = 0; k < 16; ++k ) if( cSentinel[k] != 55.0 + k ) cUntouched = false;
			Check( cUntouched, "MONEY (G11): the controller's refusal left outMatrix untouched too" );

			// The identical-transform twin is NOT ambiguous: one matrix is right for both.
			double twinM[16] = { 0 }, twinExp[16];
			Check( pDup->GetGroupOwnTransform( "twin", twinM ),
				"MONEY (G11): same name + SAME transform is NOT ambiguous -- still resolves" );
			Mat16Of( GroupTRS( 1, 1, 1, 0, 0, 0, 1, 1, 1 ), twinExp );
			Check( Mat16Eq( twinM, twinExp ), "G11: and it is the correct matrix" );
			Check( dctrl.GroupMemberCount( String( "twin" ) ) == 2,
				"G11: the twin's members fold too" );

			pDup->release();
		}
		std::remove( dupTmp.c_str() );
	}

	// ---------- G12: groupsStampEpoch across a scene_variant switch ----------
	// See the file header's [G12] entry for the full ARCHITECTURAL FINDING this
	// case is built on: a scene_variant switch cannot make group content diverge
	// under the current derive (group chunks don't support the `variant` tag, and
	// Cst.cpp's override bake is hard-restricted to Material-category chunks), so
	// this case cannot RED-PROVE the epoch term via a content mismatch the way the
	// GUI-fix-round prompt's illustrative wording ("adds or removes a group")
	// suggested.  It instead drives the real API a shell calls to activate a
	// variant and pins the two things that ARE true and reachable.
	std::printf( "G12: groupsStampEpoch across a real scene_variant switch...\n" );
	{
		const char* const kVariantScene =
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
			"film\n{\nwidth 16\nheight 16\n}\n\n"
			"pinhole_camera\n{\nlocation 0 0 8\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
			"uniformcolor_painter\n{\nname pnt\ncolor 0.5 0.5 0.5\n}\n\n"
			"lambertian_material\n{\nname mat\nreflectance pnt\n}\n\n"
			"sphere_geometry\n{\nname sph\nradius 0.5\n}\n\n"
			"standard_object\n{\nname obj_x\ngeometry sph\nmaterial mat\nposition 1 0 0\n}\n\n"
			"standard_object\n{\nname obj_y\ngeometry sph\nmaterial mat\nposition 0 1 0\n}\n\n"
			"group\n{\nname grp_v\nmember obj_x\nmember obj_y\nposition 4 0 0\n}\n\n"
			"scene_variant\n{\nname night\n}\n";

		const std::string vTmp = TempPath( "group_panel_variant.RISEscene" );
		Job* pVJob = LoadScene( kVariantScene, vTmp );
		Check( pVJob != nullptr, "G12: variant fixture loads via CST" );
		if( pVJob )
		{
			SceneEditController vctrl( *pVJob, nullptr );

			// Prime the snapshot BEFORE the switch (mirrors how the outliner
			// primes on first paint).
			const std::vector<String> before = vctrl.GroupMemberNames( String( "grp_v" ) );
			Check( before.size() == 2 &&
			       std::string( before[0].c_str() ) == "obj_x" &&
			       std::string( before[1].c_str() ) == "obj_y",
				"G12: pre-switch group snapshot primes with the authored members" );
			double ownBefore[16] = { 0 };
			Check( vctrl.GroupOwnTransform( String( "grp_v" ), ownBefore ),
				"G12: pre-switch own transform primes" );

			const RISE::Cst::CstHeadVersion hvBefore = pVJob->GetCstHeadVersion();

			// The exact API a shell calls to activate a variant --
			// SetSelectionInner_'s Category::SceneVariant arm, which is what
			// invokes Job::RederiveCstWithVariant.
			Check( vctrl.SetSelection( Category::SceneVariant, String( "night" ) ),
				"G12: SetSelection(SceneVariant, night) applies" );

			const RISE::Cst::CstHeadVersion hvAfter = pVJob->GetCstHeadVersion();
			Check( hvAfter.uuid == hvBefore.uuid && hvAfter.revision == hvBefore.revision,
				"MONEY (G12): RederiveCstWithVariant restores uuid+revision VERBATIM across the "
				"switch -- confirms the header doc's \"only the epoch moves\"" );

			// ARCHITECTURAL FINDING, asserted rather than assumed: group content is
			// byte-identical after the switch, because `group` chunks are variant-inert.
			const std::vector<String> after = vctrl.GroupMemberNames( String( "grp_v" ) );
			Check( after.size() == before.size() &&
			       std::string( after[0].c_str() ) == "obj_x" &&
			       std::string( after[1].c_str() ) == "obj_y",
				"MONEY (G12): group content is UNCHANGED by the variant switch (architectural finding, "
				"not a defect -- see the file header's [G12] entry)" );
			double ownAfter[16] = { 0 };
			Check( vctrl.GroupOwnTransform( String( "grp_v" ), ownAfter ) && Mat16Eq( ownAfter, ownBefore ),
				"G12: the own transform is unchanged by the switch too" );

			// What the switch DOES exercise: RederiveCstWithVariant's ClearAll()
			// tears down and rebuilds the Scene + every manager (including the
			// Job's group index) in place.  This pins that the outliner's snapshot
			// survives that teardown/rebuild cleanly -- not corrupted, not
			// duplicated, not blanked -- one group, two members, same values.
			Check( vctrl.CategoryEntityCount( Category::Group ) == 1,
				"MONEY (G12): the Group category still enumerates exactly 1 group post-switch "
				"(the full Scene+managers rebuild didn't drop or duplicate it)" );
			Check( vctrl.GroupMemberCount( String( "grp_v" ) ) == 2,
				"G12: GroupMemberCount agrees with GroupMemberNames post-switch" );

			pVJob->release();
		}
		std::remove( vTmp.c_str() );
	}
	// RED-PROVE (F3, reported in the session, not asserted in-tree): with the
	// `mUi.groupsStampEpoch == epoch` conjunct temporarily deleted from
	// RefreshGroupSnapshot_'s gate in SceneEditController.cpp, G12 above -- and the
	// full GroupPanelCategoryTest suite -- STILL PASS.  This is consistent with the
	// architectural finding above, generalized: every operation in this codebase
	// that changes group content (a group property edit, group add/remove via
	// chunk CRUD, undo/redo of either) ALSO bumps CstHeadVersion.revision, which
	// alone would still force a rebuild through the OTHER two stamp conjuncts.  The
	// ONE operation that bumps the epoch WITHOUT bumping revision -- a scene_variant
	// switch -- is proven inert for `group` chunks by the same two facts pinned in
	// [G12]'s header entry.  So, on the reachable state space of the codebase TODAY,
	// groupsStampEpoch does not currently guard against an observable stale read for
	// GROUPS specifically; it is a defensive/forward-looking term (protects a FUTURE
	// `group` variant-tag, or any other as-yet-unwritten epoch-only content
	// mutator) rather than a live-bug guard.  Deleting it is still NOT recommended:
	// the header doc's own "conservative by construction" framing treats a
	// redundant rebuild as the acceptable failure mode of an under-stamped term, and
	// removing the term trades that for a landmine the day a variant-aware `group`
	// (or any other epoch-only group mutator) is added.

	std::printf( "=== GroupPanelCategoryTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail > 0 ? 1 : 0;
}
