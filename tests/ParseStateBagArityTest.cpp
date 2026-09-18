//////////////////////////////////////////////////////////////////////
//
//  ParseStateBagArityTest.cpp - DL-32 (docs/DEBT_LEDGER.md) root-cause
//    red-proof and regression guard, at the PRIMITIVE rather than at one
//    field.
//
//  StandardObjectScaleTest.cpp covers `standard_object`/`override_object`
//  `scale` specifically (the field the original DL-32 report named).  A
//  review pass on that fix found the same defect still live at every
//  OTHER `DoubleVec3`/`DoubleVec4`/`DoubleMat4` parameter (~130 call
//  sites in ChunkParserRegistry.cpp): `ParseStateBag::GetVec3`/`GetVec4`/
//  `GetMat4` zero-initialized their output and `sscanf`'d into it, so a
//  value with FEWER tokens than the arity "succeeded" with the missing
//  components silently 0, and a value with MORE tokens than the arity
//  silently ignored the extras -- `position -4 4` derived `(-4, 4, 0)`
//  with no diagnostic anywhere.
//
//  The fix moved the check into the three accessors themselves (so every
//  declared DoubleVec3/DoubleVec4/DoubleMat4 parameter is protected by
//  construction, not by each Finalize() remembering to check): a token
//  count that does not EXACTLY match the accessor's arity is now a hard
//  parse error (ParseStateBag::HadHardError(), surfaced through
//  kVectorArityFmt), which the two live Finalize()-invoking call sites
//  (IAsciiChunkParser::ParseChunk's default implementation, and
//  Cst.cpp's three direct `parser->Finalize(...)` call sites) AND into
//  their success check -- so the whole chunk fails instead of silently
//  applying a partially zero-filled vector.  The lone sanctioned
//  exception is `standard_object`/`override_object`'s `scale`, whose
//  single-number uniform-scale broadcast is resolved by
//  `ResolveScaleVec3` BEFORE GetVec3 is ever called for that case (see
//  its own comment in ChunkParserRegistry.cpp) -- covered by
//  StandardObjectScaleTest, reconfirmed here (case F) as a regression.
//
//  Cases:
//    A -- a short `position` (2 of 3 tokens) on `standard_object`.
//    B -- a short camera `lookat` (2 of 3 tokens) on `pinhole_camera`.
//    C -- a short painter `scale` (2 of 3 tokens) on `mapping_painter`,
//         with a sibling chunk proving PASS-2's "continue past a failing
//         chunk" contract still holds (the sibling still applies).
//    D -- a 5-token `quaternion` (DoubleVec4, expects exactly 4) on
//         `standard_object`.
//    E -- well-formed values (exactly 3 / exactly 4 tokens, including one
//         with a trailing `# comment` -- the same inline-comment idiom
//         `AllTokensAreFiniteNumbers` already tolerates elsewhere) are
//         UNCHANGED.
//    F -- `standard_object`'s single-number `scale` broadcast (DL-32's
//         original fix) still works: GetVec3 is never reached for that
//         case, so this generic arity check cannot regress it.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job, IObject/manager interfaces, DumpJob

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static int DeriveCst( const std::string& scene, Job& job, std::vector<std::string>* diags = nullptr )
{
	Document d = ParseToCst( scene );
	return DeriveToJob( d, job, diags );
}

static const std::string HDR = "RISE ASCII SCENE 7\n";

static bool AnyDiagContains( const std::vector<std::string>& diags, const char* needle )
{
	for( const std::string& d : diags )
		if( d.find( needle ) != std::string::npos ) return true;
	return false;
}

static bool GetObjectTransform( Job& job, const char* name, Matrix4& out )
{
	IJobPriv* priv = dynamic_cast<IJobPriv*>( &job );
	if( !priv ) return false;
	IObjectManager* objs = priv->GetObjects();
	if( !objs ) return false;
	IObjectPriv* obj = objs->GetItem( name );
	if( !obj ) return false;
	out = obj->GetFinalTransformMatrix();
	return true;
}

int main()
{
	std::printf( "ParseStateBagArityTest -- DL-32 root-cause fix: GetVec3/GetVec4/GetMat4 hard-error on wrong arity\n" );

	//----------------------------------------------------------------------
	// [A] short `position` (2 of 3 tokens) on standard_object.
	//----------------------------------------------------------------------
	std::printf( "[A] `position -4 4` (2 tokens) hard-fails the standard_object chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nposition -4 4\n}\n",
			*j, &diags );
		Check( n == 1, "only the sibling geometry chunk applies (n == 1) "
			"(RED pre-fix: n == 2, `position` silently derived (-4, 4, 0), no diagnostic)" );
		// NOTE: per Cst.h's "CANONICAL STATEMENT" (KEPT LIVE BUT LOUD), a chunk that
		// fails PARTWAY through Finalize (as this one does -- GetVec3 fails but
		// Finalize keeps running and still calls AddObject) can still leave its
		// object live in the Job; `n` (the "chunks that counted as applied"
		// bookkeeping) and `diags` are the load-bearing signals a caller gates on,
		// not whether the manager happens to hold a stray entry -- so this test
		// does not assert `s` is absent, only that the derive is DIAGNOSED and NOT
		// counted as successful (matching every other Finalize-failure in this file).
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "position" ), "diagnostic names the parameter (`position`)" );
		Check( AnyDiagContains( diags, "standard_object" ), "diagnostic names the chunk (`standard_object`)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [B] short camera `lookat` (2 of 3 tokens).
	//----------------------------------------------------------------------
	std::printf( "[B] `lookat 0 0` (2 tokens) hard-fails the pinhole_camera chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0\n}\n",
			*j, &diags );
		Check( n == 0, "the malformed camera chunk applies nothing (n == 0) "
			"(RED pre-fix: n == 1, `lookat` silently derived (0, 0, 0))" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "lookat" ), "diagnostic names the parameter (`lookat`)" );
		Check( AnyDiagContains( diags, "pinhole_camera" ), "diagnostic names the chunk (`pinhole_camera`)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [C] short painter `scale` (2 of 3 tokens) on mapping_painter, with a
	// sibling chunk proving PASS-2 still continues past the failure.
	//----------------------------------------------------------------------
	std::printf( "[C] `scale 0.5 0.5` (2 tokens) hard-fails mapping_painter; sibling still applies\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "uniformcolor_painter\n{\nname basecolor\ncolor 1 1 1\n}\n"
			      "mapping_painter\n{\nname m\nsource basecolor\nscale 0.5 0.5\n}\n",
			*j, &diags );
		Check( n == 1, "only the sibling uniformcolor_painter applies (n == 1) "
			"(RED pre-fix: n == 2, `scale` silently derived (0.5, 0.5, 0))" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "scale" ), "diagnostic names the parameter (`scale`)" );
		Check( AnyDiagContains( diags, "mapping_painter" ), "diagnostic names the chunk (`mapping_painter`)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [D] a 5-token `quaternion` (DoubleVec4, expects exactly 4).
	//----------------------------------------------------------------------
	std::printf( "[D] `quaternion 0 0 0 1 0` (5 tokens) hard-fails the standard_object chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nquaternion 0 0 0 1 0\n}\n",
			*j, &diags );
		Check( n == 1, "only the sibling geometry chunk applies (n == 1) "
			"(RED pre-fix: n == 2, the fifth token silently ignored, no diagnostic)" );
		// See the [A] note above -- KEPT LIVE BUT LOUD: not asserting `s` is absent.
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "quaternion" ), "diagnostic names the parameter (`quaternion`)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [E] well-formed values (exact arity, incl. a trailing inline comment
	// on a DoubleVec3) are unaffected.
	//----------------------------------------------------------------------
	std::printf( "[E] well-formed position/lookat/scale/quaternion values are unaffected\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nposition 1 2 3\nquaternion 0 0 0 1\n}\n",
			*j, &diags );
		Check( n == 2, "well-formed 3-token position + 4-token quaternion derive successfully (n == 2)" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._30 - 1.0 ) < kEps && std::fabs( m._31 - 2.0 ) < kEps && std::fabs( m._32 - 3.0 ) < kEps,
				"translation column is (1, 2, 3)" );
		}
		j->release();
	}
	std::printf( "[E] a trailing `# comment` on a DoubleVec3 value still parses (matches AllTokensAreFiniteNumbers' idiom)\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nposition 1 2 3 # meters\n}\n",
			*j, &diags );
		Check( n == 2, "`position 1 2 3 # meters` derives successfully (n == 2) -- the comment is not counted as a 4th token" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._30 - 1.0 ) < kEps && std::fabs( m._31 - 2.0 ) < kEps && std::fabs( m._32 - 3.0 ) < kEps,
				"translation column is (1, 2, 3), comment ignored" );
		}
		j->release();
	}

	//----------------------------------------------------------------------
	// [F] standard_object's single-number `scale` broadcast (DL-32's
	// original, field-specific fix) still works: ResolveScaleVec3 never
	// calls GetVec3 for the 1-token case, so this generic accessor-level
	// arity check cannot regress it.
	//----------------------------------------------------------------------
	std::printf( "[F] `scale 0.35` (ONE number, the sanctioned broadcast) is unaffected by the generic arity check\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nscale 0.35\n}\n",
			*j, &diags );
		Check( n == 2, "`scale 0.35` still derives successfully (n == 2)" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._00 - 0.35 ) < kEps && std::fabs( m._11 - 0.35 ) < kEps && std::fabs( m._22 - 0.35 ) < kEps,
				"diagonal is uniformly 0.35 (broadcast still intact)" );
		}
		Check( !AnyDiagContains( diags, "DL-32" ), "no DL-32 diagnostic fires for the sanctioned single-number broadcast" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
