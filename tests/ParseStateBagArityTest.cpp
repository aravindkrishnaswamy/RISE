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

	//----------------------------------------------------------------------
	// [G]-[M]: DL-32 ROUND 3 (docs/DEBT_LEDGER.md) -- the round-2 fix above
	// only protects a site that actually CALLS GetVec3/GetVec4/GetMat4.  A
	// full-file audit found 15 Finalize() sites that read a fixed-2-token
	// value via a RAW `sscanf` on `bag.GetString(key).c_str()`, bypassing
	// every accessor (and DispatchChunkParameters's own finite-number gate)
	// entirely -- exactly as unprotected as pre-round-1 `scale`.  Fixed by
	// adding `ValueKind::DoubleVec2` + `ParseStateBag::GetVec2` (same
	// hard-error contract as GetVec3) and re-routing all 15 sites through it.
	// Cases G-M below cover the primitive plus one representative site per
	// distinct Finalize() (the four `composite_function2d_painter` twins and
	// the four camera `target_orientation` sites share one code shape each,
	// so one case per shape is the load-bearing regression, not sixteen).
	//----------------------------------------------------------------------
	std::printf( "[G] `perlin2d_painter`'s `scale`/`shift` (GetVec2 direct): short/long hard-fail, well-formed derives\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "perlin2d_painter\n{\nname p\nscale 1\n}\n",
			*j, &diags );
		Check( n == 1, "short `scale` (1 of 2 tokens) hard-fails perlin2d_painter; sibling geometry still applies (n == 1) "
			"(RED pre-fix: n == 2, `scale` silently derived (1, 1) -- the second component held at its unrelated pre-set default)" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "scale" ), "diagnostic names the parameter (`scale`)" );
		Check( AnyDiagContains( diags, "perlin2d_painter" ), "diagnostic names the chunk (`perlin2d_painter`)" );
		j->release();
	}
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "perlin2d_painter\n{\nname p\nshift 1 2 3\n}\n",
			*j, &diags );
		Check( n == 1, "long `shift` (3 of 2 tokens) hard-fails perlin2d_painter; sibling geometry still applies (n == 1) "
			"(RED pre-fix: n == 2, the third token silently discarded, no diagnostic)" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "shift" ), "diagnostic names the parameter (`shift`)" );
		j->release();
	}
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "perlin2d_painter\n{\nname p\nscale 2 3\nshift 0.1 0.2\n}\n",
			*j, &diags );
		Check( n == 1, "well-formed 2-token `scale`/`shift` on perlin2d_painter derives successfully (n == 1)" );
		Check( diags.empty(), "no diagnostics for the well-formed case" );
		j->release();
	}

	std::printf( "[H] `controlled_smoothness2d_painter`'s `center` (short, 1 token) hard-fails the chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "controlled_smoothness2d_painter\n{\nname c\ncenter 0.5\n}\n",
			*j, &diags );
		Check( n == 1, "short `center` hard-fails controlled_smoothness2d_painter; sibling geometry still applies (n == 1) "
			"(RED pre-fix: n == 2, silently derived (0.5, <unrelated default>))" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "center" ), "diagnostic names the parameter (`center`)" );
		Check( AnyDiagContains( diags, "controlled_smoothness2d_painter" ), "diagnostic names the chunk" );
		j->release();
	}

	std::printf( "[I] `gerstnerwave_painter`'s `wind_dir` (long, 3 tokens) hard-fails the chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "gerstnerwave_painter\n{\nname w\nwind_dir 1 0 0\n}\n",
			*j, &diags );
		Check( n == 1, "long `wind_dir` hard-fails gerstnerwave_painter; sibling geometry still applies (n == 1) "
			"(RED pre-fix: n == 2, the third token silently discarded)" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "wind_dir" ), "diagnostic names the parameter (`wind_dir`)" );
		j->release();
	}

	std::printf( "[J] `polynomial_function2d_painter`'s `center`/`scale` (short, 1 token) hard-fail the chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "polynomial_function2d_painter\n{\nname f\ncenter 0.5\n}\n",
			*j, &diags );
		Check( n == 1, "short `center` hard-fails polynomial_function2d_painter; sibling geometry still applies (n == 1)" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		j->release();
	}
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "polynomial_function2d_painter\n{\nname f\nscale 0.5 0.5 0.5\n}\n",
			*j, &diags );
		Check( n == 1, "long `scale` hard-fails polynomial_function2d_painter; sibling geometry still applies (n == 1)" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		j->release();
	}

	std::printf( "[K] `composite_function2d_painter`'s `uv_scale_a` (short, 1 token) hard-fails the chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "composite_function2d_painter\n{\nname c\nuv_scale_a 2\n}\n",
			*j, &diags );
		Check( n == 1, "short `uv_scale_a` hard-fails composite_function2d_painter; sibling geometry still applies (n == 1) "
			"(the sibling `uv_offset_a`/`uv_scale_b`/`uv_offset_b` share the identical GetVec2 call, not independently red-proved here)" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "uv_scale_a" ), "diagnostic names the parameter (`uv_scale_a`)" );
		j->release();
	}

	std::printf( "[L] a camera's `target_orientation` (long, 3 tokens) hard-fails the chunk (shared by all 4 camera parsers)\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\ntarget_orientation 0.1 0.2 0.3\n}\n",
			*j, &diags );
		Check( n == 0, "the malformed camera chunk applies nothing (n == 0) "
			"(RED pre-fix: n == 1, the third token silently discarded, no diagnostic)" );
		Check( AnyDiagContains( diags, "DL-32" ), "diagnostic names DL-32" );
		Check( AnyDiagContains( diags, "target_orientation" ), "diagnostic names the parameter" );
		Check( AnyDiagContains( diags, "pinhole_camera" ), "diagnostic names the chunk" );
		j->release();
	}

	std::printf( "[M] `orthographic_camera`'s `viewport_scale`: 3 tokens hard-fails; ONE token is a sanctioned uniform broadcast\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "orthographic_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\nviewport_scale 1 2 3\n}\n",
			*j, &diags );
		Check( n == 0, "3-token `viewport_scale` hard-fails the chunk (n == 0)" );
		Check( AnyDiagContains( diags, "DL-32" ) == false && AnyDiagContains( diags, "viewport_scale" ),
			"diagnostic names `viewport_scale` (this one is ResolveVec2UniformBroadcast's own message, not the shared DL-32 kVectorArityFmt text -- it never reaches GetVec2)" );
		j->release();
	}
	{
		// The corpus (AgentProposeRenderTest.cpp, AgentViewModeRenderTest.cpp)
		// already authors `viewport_scale <ONE number>` expecting a uniform
		// broadcast; ResolveVec2UniformBroadcast (mirroring ResolveScaleVec3)
		// keeps that spelling legal rather than breaking it under the new
		// strict-arity GetVec2 rule.
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "orthographic_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\nviewport_scale 2.5\n}\n",
			*j, &diags );
		Check( n == 1, "single-number `viewport_scale 2.5` still derives successfully (n == 1) -- the sanctioned broadcast shorthand" );
		Check( diags.empty(), "no diagnostics for the sanctioned single-number broadcast" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [N] STICKY DIAGNOSTIC (DL-32 round-2 review, P3): ReportVectorArity's
	// first-arity-failure-wins write to g_cstFinalizeDiagSink used to be
	// clobbered by any LATER unconditional writer inside the SAME Finalize()
	// call (~17 such sites in ChunkParserRegistry.cpp) -- a chunk with BOTH
	// a wrong-arity vector AND a second, unrelated validation failure would
	// report the LESS specific, later-checked reason instead of the actual
	// root cause.  `SetFinalizeDiagIfEmpty` (GenericManager.h) makes every
	// writer first-wins.  `standard_object`'s `position` (checked early, in
	// the transform branch) and `mirror` (checked later) both fail here;
	// the diagnostic that survives must be the `position` one.
	//----------------------------------------------------------------------
	std::printf( "[N] sticky diagnostic: an early `position` arity failure survives a LATER `mirror` failure in the same Finalize\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nposition -4 4\nmirror bogus\n}\n",
			*j, &diags );
		Check( n == 1, "only the sibling geometry chunk applies (n == 1)" );
		Check( AnyDiagContains( diags, "DL-32" ), "the SURVIVING diagnostic is the position arity failure (names DL-32)" );
		Check( AnyDiagContains( diags, "position" ), "the surviving diagnostic names `position`, the EARLIER failure" );
		Check( !AnyDiagContains( diags, "mirror" ), "the LATER `mirror` failure did NOT clobber the earlier diagnostic "
			"(RED pre-fix: the diagnostic named `mirror`, not `position` -- the later, less specific reason won)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [O] STICKY DIAGNOSTIC ACROSS A NESTED CALL (DL-32 round-2 review round
	// 2, P2-1): `SetFinalizeDiagIfEmpty` converted every raw
	// `g_cstFinalizeDiagSink` writer in ChunkParserRegistry.cpp but MISSED 7
	// raw unconditional writers in Job.cpp (one per Add* factory --
	// AddExpressionPainter, AddVoronoi2DPainter, AddVoronoi3DPainter{,WithSpace},
	// ImportGLTFScene, AddFileRasterizerOutput, AddKeyframeToAnimation),
	// plus 3 more in RISE_API.cpp found by extending the grep tree-wide.
	// `gltf_import`'s Finalize calls `bag.GetVec3("emissive_tint", ...)`
	// BEFORE calling `pJob.ImportGLTFScene(...)` -- so a malformed
	// `emissive_tint` on a chunk whose `name_prefix` collides with an
	// earlier import fires the arity diagnostic FIRST, then (pre-fix)
	// ImportGLTFScene's raw collision-message write clobbered it.
	//----------------------------------------------------------------------
	std::printf( "[O] gltf_import: an early `emissive_tint` arity failure survives ImportGLTFScene's LATER name_prefix-collision failure\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "gltf_import\n{\nfile nonexistent1.gltf\n}\n"
			      "gltf_import\n{\nfile nonexistent2.gltf\nemissive_tint 1 2\n}\n",
			*j, &diags );
		Check( n == 0, "both the file-not-found first import and the malformed/colliding second import apply nothing (n == 0)" );
		Check( AnyDiagContains( diags, "DL-32" ), "the SURVIVING diagnostic for the second chunk names DL-32 (the emissive_tint arity failure)" );
		Check( AnyDiagContains( diags, "emissive_tint" ), "the surviving diagnostic names `emissive_tint`, the EARLIER failure" );
		Check( !AnyDiagContains( diags, "name_prefix" ), "the LATER name_prefix collision did NOT clobber the earlier diagnostic "
			"(RED pre-fix: the diagnostic named `name_prefix`, not `emissive_tint` -- the later, deeper-nested reason won)" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
