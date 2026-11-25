//////////////////////////////////////////////////////////////////////
//
//  OslSpaceTransformTest.cpp - Unit and integration tests for Group 7
//    live space-transform builtins (`osl_to_object_*`, `osl_to_world_*`,
//    `osl_to_camera_*`, `osl_camera_to_world_*`).
//
//  Tests:
//    1. Compile-time checks: context-vars gating, arity, type checking,
//       UsesSpaceTransform(), not memoWorthy on their own.
//    2. Object<->World transforms under non-uniform scale + rotation +
//       translation:
//       - point/vector forward & inverse exact round-trip
//       - inverse-transpose normal preserves orthogonality (n · t == 0)
//         under strong non-uniform scale (scale 0.25, 3.0, 7.0) and
//         unit length
//       - zero normal stays (0,0,0) (no NaN)
//       - mirror (negative scale) behavior
//    3. World<->Camera transforms:
//       - left-handed Cycles convention (+X right, +Y up, +Z forward)
//       - round-trip to_camera <-> camera_to_world for point/vector/normal
//       - live camera pose mutation (PinholeCamera::SetLocation / SetLookAt /
//         RegenerateData) changes output WITHOUT recompiling the expression
//       - all 4 RISE camera types (Pinhole, ThinLens, Orthographic, Fisheye)
//    4. Null/absent hit/matrix/camera fallbacks (identity for point/vector,
//       safe-normalized for camera normal, finite on NaN/inf inputs).
//    5. L2 ExpressionMemo soundness:
//       - two differently-transformed instances sharing one memo-worthy
//         ExpressionPainter (osl_snoise + osl_to_object_point) never collide
//       - in-place camera pose move on a memo-worthy ExpressionPainter
//         (osl_snoise + osl_to_camera_point) immediately produces the new
//         camera-space value even without calling ExpressionMemo::Invalidate()
//    6. End-to-end Scene + ObjectManager::IntersectRay integration:
//       - real Object with non-uniform scale + rotation + translation inside
//         a real Scene with a PinholeCamera stamps `pmxWorldToObject`,
//         `pmxObjectToWorld`, and live `pCamera` onto the hit; moving the
//         camera updates `osl_to_camera_point(P)` on the next raycast.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Cameras/OrthographicCamera.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Cameras/FisheyeCamera.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Managers/CameraManager.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Scene.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void Check( bool cond, const char* msg )
	{
		if( cond ) {
			++g_pass;
		} else {
			++g_fail;
			std::cerr << "FAIL: " << msg << "\n";
		}
	}

	bool Near( Scalar a, Scalar b, Scalar tol = 1e-10 )
	{
		return std::fabs( a - b ) <= tol;
	}

	bool NearVec( const Vector3& a, const Vector3& b, Scalar tol = 1e-10 )
	{
		return Near( a.x, b.x, tol ) && Near( a.y, b.y, tol ) && Near( a.z, b.z, tol );
	}

	bool CompileExpr( const std::string& expr, ExpressionProgram& out, bool ctxVars = true )
	{
		ExpressionProgram::Builder b;
		b.EnableContextVars( ctxVars );
		return b.Finalize( expr, out );
	}

	void TestCompileChecks()
	{
		const char* names[] = {
			"osl_to_object_point", "osl_to_object_vector", "osl_to_object_normal",
			"osl_to_world_point", "osl_to_world_vector", "osl_to_world_normal",
			"osl_to_camera_point", "osl_to_camera_vector", "osl_to_camera_normal",
			"osl_camera_to_world_point", "osl_camera_to_world_vector", "osl_camera_to_world_normal",
		};
		for( size_t i = 0; i < sizeof( names ) / sizeof( names[0] ); ++i ) {
			ExpressionProgram p = ExpressionProgram::Invalid();
			// Must reject when context vars are disabled
			Check( !CompileExpr( std::string( names[i] ) + "(vec3(1,2,3))", p, false ),
				"space transform rejected without context vars" );
			// Must reject scalar arg
			Check( !CompileExpr( std::string( names[i] ) + "(1.0)", p, true ),
				"space transform rejected with scalar arg" );
			// Must reject wrong arity
			Check( !CompileExpr( std::string( names[i] ) + "(P, P)", p, true ),
				"space transform rejected with 2 args" );
			// Must compile with context vars enabled
			Check( CompileExpr( std::string( names[i] ) + "(P)", p, true ),
				"space transform compiles with context vars" );
			Check( p.ResultType() == ExpressionProgram::kVec3, "space transform returns vec3" );
			Check( p.UsesSpaceTransform(), "UsesSpaceTransform() is true" );
			Check( !p.MemoWorthy(), "bare space transform is not memoWorthy" );
		}

		// Non-space program has UsesSpaceTransform() == false
		ExpressionProgram plain = ExpressionProgram::Invalid();
		Check( CompileExpr( "P + vec3(1,2,3)", plain, true ), "plain compiles" );
		Check( !plain.UsesSpaceTransform(), "plain program has UsesSpaceTransform() == false" );
	}

	void TestObjectWorldTransforms()
	{
		// Build a non-uniformly scaled, rotated, translated matrix
		const Matrix4 S = Matrix4Ops::Stretch( Vector3( 0.25, 3.0, 7.0 ) );
		const Matrix4 Rx = Matrix4Ops::XRotation( 0.43 );
		const Matrix4 Ry = Matrix4Ops::YRotation( -0.81 );
		const Matrix4 Rz = Matrix4Ops::ZRotation( 1.17 );
		const Matrix4 T = Matrix4Ops::Translation( Vector3( 12.5, -4.25, 8.75 ) );
		const Matrix4 mxO2W = S * Rx * Ry * Rz * T;
		const Matrix4 mxW2O = Matrix4Ops::Inverse( mxO2W );

		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		ri.pmxObjectToWorld = &mxO2W;
		ri.pmxWorldToObject = &mxW2O;

		ExprEvalContext ctx;
		ctx.P = Vector3( 3.1, -2.7, 5.4 );
		ctx.N = Vector3Ops::Normalize( Vector3( 0.3, -0.7, 0.6 ) );
		ctx.pHit = &ri;

		// Point round-trip: to_world_point(to_object_point(P)) == P
		ExpressionProgram pRT = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_world_point(osl_to_object_point(P))", pRT ), "point RT compiles" );
		Check( NearVec( pRT.EvalVec3( ctx ), ctx.P, 1e-11 ), "point world->obj->world round-trip" );

		// Point world->object matches Point3Ops::Transform(mxW2O, P)
		ExpressionProgram pW2O = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_object_point(P)", pW2O ), "to_object_point compiles" );
		const Point3 expObjPt = Point3Ops::Transform( mxW2O, Point3( ctx.P.x, ctx.P.y, ctx.P.z ) );
		Check( NearVec( pW2O.EvalVec3( ctx ), Vector3( expObjPt.x, expObjPt.y, expObjPt.z ), 1e-11 ),
			"to_object_point matches Matrix4" );

		// Vector ignores translation and round-trips
		ExpressionProgram vRT = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_world_vector(osl_to_object_vector(P))", vRT ), "vector RT compiles" );
		Check( NearVec( vRT.EvalVec3( ctx ), ctx.P, 1e-11 ), "vector world->obj->world round-trip" );

		ExpressionProgram vZero = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_world_vector(vec3(0,0,0))", vZero ), "vector(0) compiles" );
		Check( NearVec( vZero.EvalVec3( ctx ), Vector3( 0, 0, 0 ), 1e-14 ), "vector transform ignores translation" );

		// Normal inverse-transpose orthogonality under strong non-uniform scale (0.25, 3.0, 7.0)
		// Construct an object-space normal and an orthogonal object-space tangent
		const Vector3 nObj = Vector3Ops::Normalize( Vector3( 1.0, 2.0, -3.0 ) );
		const Vector3 tObj = Vector3Ops::Normalize( Vector3Ops::Cross( nObj, Vector3( 0, 1, 0 ) ) );
		Check( std::fabs( Vector3Ops::Dot( nObj, tObj ) ) < 1e-14, "nObj perp tObj" );

		ctx.Po = nObj;
		ExpressionProgram nToW = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_world_normal(Po)", nToW ), "to_world_normal compiles" );
		const Vector3 nWorld = nToW.EvalVec3( ctx );
		Check( Near( Vector3Ops::Magnitude( nWorld ), 1.0, 1e-12 ), "to_world_normal is unit length" );

		ctx.Po = tObj;
		ExpressionProgram tToW = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_world_vector(Po)", tToW ), "to_world_vector compiles" );
		const Vector3 tWorld = tToW.EvalVec3( ctx );
		Check( std::fabs( Vector3Ops::Dot( nWorld, tWorld ) ) < 1e-12,
			"to_world_normal preserves n·t == 0 under non-uniform scale" );

		// Round-trip normal back to object space
		ctx.N = nWorld;
		ExpressionProgram nBack = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_object_normal(N)", nBack ), "to_object_normal compiles" );
		const Vector3 nObjBack = nBack.EvalVec3( ctx );
		Check( Near( Vector3Ops::Magnitude( nObjBack ), 1.0, 1e-12 ), "to_object_normal is unit length" );
		Check( NearVec( nObjBack, nObj, 1e-11 ), "normal obj->world->obj round-trip" );

		// Zero normal stays (0,0,0) with no NaN
		ExpressionProgram nZero = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_object_normal(vec3(0,0,0)) + osl_to_world_normal(vec3(0,0,0))", nZero ),
			"zero normal compiles" );
		Check( NearVec( nZero.EvalVec3( ctx ), Vector3( 0, 0, 0 ), 1e-14 ), "zero normal returns (0,0,0)" );

		// Negative-scale mirror transform: orthogonality (n·t == 0), unit length, and exact round-trip
		const Matrix4 mxMirrorO2W = Matrix4Ops::Stretch( Vector3( -1.0, 2.5, 4.0 ) ) * Ry * T;
		const Matrix4 mxMirrorW2O = Matrix4Ops::Inverse( mxMirrorO2W );
		RayIntersectionGeometric riMir( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		riMir.pmxObjectToWorld = &mxMirrorO2W;
		riMir.pmxWorldToObject = &mxMirrorW2O;
		ctx.pHit = &riMir;
		ctx.Po = nObj;
		const Vector3 nMirW = nToW.EvalVec3( ctx );
		ctx.Po = tObj;
		const Vector3 tMirW = tToW.EvalVec3( ctx );
		Check( Near( Vector3Ops::Magnitude( nMirW ), 1.0, 1e-12 ), "mirror to_world_normal is unit length" );
		Check( std::fabs( Vector3Ops::Dot( nMirW, tMirW ) ) < 1e-12,
			"mirror to_world_normal preserves n·t == 0" );
		ctx.N = nMirW;
		Check( NearVec( nBack.EvalVec3( ctx ), nObj, 1e-11 ), "mirror normal obj->world->obj round-trip" );
	}

	void TestCameraTransformsAndLiveMove()
	{
		// Camera at (2, 3, 10) looking at (2, 3, 0) along -Z with +Y up:
		// Cycles camera space: +X = right = (+1, 0, 0), +Y = up = (0, +1, 0), +Z = forward = (0, 0, -1).
		PinholeCamera* cam = new PinholeCamera(
			Point3( 2, 3, 10 ), Point3( 2, 3, 0 ), Vector3( 0, 1, 0 ),
			45.0 * DEG_TO_RAD, 320, 240, 1.0, 0.0, 0.0, 0.0,
			Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );

		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		ri.pCamera = cam;

		ExprEvalContext ctx;
		// Point at (5, 7, 4): offset from camera (2,3,10) is (+3, +4, -6).
		// In camera space: x_cam = +3 (right), y_cam = +4 (up), z_cam = +6 (in front!).
		ctx.P = Vector3( 5, 7, 4 );
		ctx.pHit = &ri;

		ExpressionProgram pToCam = ExpressionProgram::Invalid();
		ExpressionProgram pCamToW = ExpressionProgram::Invalid();
		ExpressionProgram vToCam = ExpressionProgram::Invalid();
		ExpressionProgram nToCam = ExpressionProgram::Invalid();
		ExpressionProgram cRT = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_camera_point(P)", pToCam ), "to_camera_point compiles" );
		Check( CompileExpr( "osl_camera_to_world_point(osl_to_camera_point(P))", cRT ), "camera point RT compiles" );
		Check( CompileExpr( "osl_to_camera_vector(P)", vToCam ), "to_camera_vector compiles" );
		Check( CompileExpr( "osl_to_camera_normal(vec3(0, 0, -2))", nToCam ), "to_camera_normal compiles" );
		Check( CompileExpr( "osl_camera_to_world_vector(osl_to_camera_vector(P))", pCamToW ), "camera vec RT compiles" );

		Check( NearVec( pToCam.EvalVec3( ctx ), Vector3( 3, 4, 6 ), 1e-11 ),
			"to_camera_point matches Cycles (+X right, +Y up, +Z forward)" );
		Check( NearVec( cRT.EvalVec3( ctx ), ctx.P, 1e-11 ), "camera point round-trip" );
		Check( NearVec( vToCam.EvalVec3( ctx ), Vector3( 5, 7, -4 ), 1e-11 ),
			"to_camera_vector ignores camera origin" );
		Check( NearVec( nToCam.EvalVec3( ctx ), Vector3( 0, 0, 1 ), 1e-11 ),
			"to_camera_normal transforms and normalizes" );
		Check( NearVec( pCamToW.EvalVec3( ctx ), ctx.P, 1e-11 ), "camera vector round-trip" );

		ExpressionProgram nCamToW = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_camera_to_world_normal(vec3(0, 0, 3))", nCamToW ), "camera_to_world_normal compiles" );
		Check( NearVec( nCamToW.EvalVec3( ctx ), Vector3( 0, 0, -1 ), 1e-11 ),
			"camera_to_world_normal transforms +Z camera normal to -Z world and normalizes" );
		ExpressionProgram nCamRT = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_camera_to_world_normal(osl_to_camera_normal(vec3(0.3, -0.7, 0.6)))", nCamRT ),
			"camera normal RT compiles" );
		Check( NearVec( nCamRT.EvalVec3( ctx ), Vector3Ops::Normalize( Vector3( 0.3, -0.7, 0.6 ) ), 1e-11 ),
			"camera normal world->cam->world round-trip" );

		// LIVE CAMERA MOVE without recompiling `pToCam`!
		// Move camera to (0, 0, 0) looking along +X (1, 0, 0) with +Z up:
		// forward = (1, 0, 0), up = (0, 0, 1), right = forward x up = (0, -1, 0).
		cam->SetLocation( Point3( 0, 0, 0 ) );
		cam->SetLookAt( Point3( 1, 0, 0 ) );
		cam->SetUp( Vector3( 0, 0, 1 ) );
		cam->RegenerateData();

		// Same point (5, 7, 4): right=(0,-1,0) -> -7, up=(0,0,1) -> +4, forward=(1,0,0) -> +5
		Check( NearVec( pToCam.EvalVec3( ctx ), Vector3( -7, 4, 5 ), 1e-11 ),
			"live camera move updates osl_to_camera_point without recompiling" );
		Check( NearVec( cRT.EvalVec3( ctx ), ctx.P, 1e-11 ),
			"camera point round-trip after live move" );

		cam->release();

		// Verify all other concrete camera types expose GetCameraFrame
		OrthographicCamera* ortho = new OrthographicCamera(
			Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			320, 240, Vector2( 1, 1 ), 1.0, 0, 0, 0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );
		Point3 o; Vector3 r, u, f;
		Check( ortho->GetCameraFrame( o, r, u, f ) && NearVec( r, Vector3( 1, 0, 0 ) )
			&& NearVec( u, Vector3( 0, 1, 0 ) ) && NearVec( f, Vector3( 0, 0, -1 ) ),
			"OrthographicCamera::GetCameraFrame matches convention" );
		ortho->release();

		FisheyeCamera* fish = new FisheyeCamera(
			Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			320, 240, 1.0, 0, 0, 0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ), 1.0 );
		Check( fish->GetCameraFrame( o, r, u, f ) && NearVec( f, Vector3( 0, 0, -1 ) ),
			"FisheyeCamera::GetCameraFrame matches convention" );
		fish->release();

		ThinLensCamera* thin = new ThinLensCamera(
			Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			36.0, 50.0, 2.8, 5.0, 1.0,
			320, 240, 1.0, 0, 0, 0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
			0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );
		Check( thin->GetCameraFrame( o, r, u, f ) && NearVec( r, Vector3( 1, 0, 0 ) )
			&& NearVec( u, Vector3( 0, 1, 0 ) ) && NearVec( f, Vector3( 0, 0, -1 ) ),
			"ThinLensCamera::GetCameraFrame matches convention" );
		thin->release();
	}

	void TestNullFallbacksAndNonFinite()
	{
		ExpressionProgram pObj = ExpressionProgram::Invalid();
		ExpressionProgram pCam = ExpressionProgram::Invalid();
		ExpressionProgram pCamN = ExpressionProgram::Invalid();
		ExpressionProgram pObjN = ExpressionProgram::Invalid();
		ExpressionProgram pWldN = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_object_point(P)", pObj ), "fallback obj compiles" );
		Check( CompileExpr( "osl_to_camera_point(P)", pCam ), "fallback cam compiles" );
		Check( CompileExpr( "osl_to_camera_normal(P)", pCamN ), "fallback cam normal compiles" );
		Check( CompileExpr( "osl_to_object_normal(P)", pObjN ), "fallback obj normal compiles" );
		Check( CompileExpr( "osl_to_world_normal(P)", pWldN ), "fallback world normal compiles" );

		ExprEvalContext ctx;
		ctx.P = Vector3( 3, 4, 0 );
		ctx.pHit = nullptr;
		Check( NearVec( pObj.EvalVec3( ctx ), ctx.P, 1e-14 ), "null pHit object fallback is identity" );
		Check( NearVec( pCam.EvalVec3( ctx ), ctx.P, 1e-14 ), "null pHit camera point fallback is identity" );
		Check( NearVec( pCamN.EvalVec3( ctx ), Vector3( 0.6, 0.8, 0.0 ), 1e-12 ),
			"null pHit camera normal fallback safe-normalizes" );
		Check( NearVec( pObjN.EvalVec3( ctx ), Vector3( 0.6, 0.8, 0.0 ), 1e-12 ),
			"null pHit object normal fallback safe-normalizes" );
		Check( NearVec( pWldN.EvalVec3( ctx ), Vector3( 0.6, 0.8, 0.0 ), 1e-12 ),
			"null pHit world normal fallback safe-normalizes" );

		// Non-finite inputs clamp cleanly to 0 via Finite()
		ctx.P = Vector3( std::numeric_limits<Scalar>::quiet_NaN(),
			std::numeric_limits<Scalar>::infinity(), -std::numeric_limits<Scalar>::infinity() );
		const Vector3 res = pObj.EvalVec3( ctx );
		Check( NearVec( res, Vector3( 0, 0, 0 ), 1e-14 ), "non-finite input yields finite 0" );
	}

	void TestMemoSoundness()
	{
		// Build a memo-worthy program that combines osl_snoise (memoWorthy=true)
		// with osl_to_object_point and osl_to_camera_point.
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileExpr( "vec3(osl_snoise(osl_to_object_point(P) + osl_to_camera_point(P), 0, 3), 0, 0)", prog ),
			"memo-worthy space-transform program compiles" );
		Check( prog.MemoWorthy(), "combined snoise + space transform is memoWorthy" );
		Check( prog.UsesSpaceTransform(), "combined program has UsesSpaceTransform() == true" );

		ExpressionPainter* painter = new ExpressionPainter( prog, std::vector<ParamSpec>(), Scalar( 0 ) );

		// Two instances with different transforms evaluated at identical P/Po/N/(u,v)
		const Matrix4 mxA = Matrix4Ops::Translation( Vector3( 1.0, 2.0, 3.0 ) );
		const Matrix4 mxAInv = Matrix4Ops::Inverse( mxA );
		const Matrix4 mxB = Matrix4Ops::Translation( Vector3( -7.3, 4.1, 1.9 ) );
		const Matrix4 mxBInv = Matrix4Ops::Inverse( mxB );

		PinholeCamera* cam = new PinholeCamera(
			Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			45.0 * DEG_TO_RAD, 320, 240, 1.0, 0, 0, 0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );

		RayIntersectionGeometric riA( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		riA.ptIntersection = Point3( 0.3, 0.4, 0.5 );
		riA.pmxObjectToWorld = &mxA;
		riA.pmxWorldToObject = &mxAInv;
		riA.pCamera = cam;

		RayIntersectionGeometric riB = riA;
		riB.pmxObjectToWorld = &mxB;
		riB.pmxWorldToObject = &mxBInv;

		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel cA_memo = painter->GetColor( riA );
		const RISEPel cB_memo = painter->GetColor( riB );

		ExpressionMemo::SetEnabledOverride( 0 );
		const RISEPel cA_direct = painter->GetColor( riA );
		const RISEPel cB_direct = painter->GetColor( riB );

		Check( Near( cA_memo[0], cA_direct[0], 1e-14 ), "instance A memo == direct" );
		Check( Near( cB_memo[0], cB_direct[0], 1e-14 ), "instance B memo == direct (no cross-instance collision)" );
		Check( std::fabs( cA_memo[0] - cB_memo[0] ) > 1e-4, "instance A and B genuinely differ" );

		// Now test moving the camera IN-PLACE with memo ON (without manually calling Invalidate,
		// and even if RegenerateData didn't bump — SpaceTransformHitKey keys on the live camera frame)
		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel beforeMove = painter->GetColor( riA );
		cam->SetLocation( Point3( 3.7, -2.1, 8.4 ) );
		cam->RegenerateData();
		const RISEPel afterMove_memo = painter->GetColor( riA );
		ExpressionMemo::SetEnabledOverride( 0 );
		const RISEPel afterMove_direct = painter->GetColor( riA );
		ExpressionMemo::SetEnabledOverride( -1 );

		Check( Near( afterMove_memo[0], afterMove_direct[0], 1e-14 ), "post-move memo == direct" );
		Check( std::fabs( beforeMove[0] - afterMove_memo[0] ) > 1e-4, "camera move changed memoized result" );

		// Two different cameras swapped on riA WITHOUT calling RegenerateData() or Invalidate()
		PinholeCamera* cam2 = new PinholeCamera(
			Point3( -9.0, 5.0, 2.0 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			45.0 * DEG_TO_RAD, 320, 240, 1.0, 0, 0, 0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );
		RayIntersectionGeometric riCam2 = riA;
		riCam2.pCamera = cam2;
		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel cCam1 = painter->GetColor( riA );
		const RISEPel cCam2_memo = painter->GetColor( riCam2 );
		ExpressionMemo::SetEnabledOverride( 0 );
		const RISEPel cCam2_direct = painter->GetColor( riCam2 );
		ExpressionMemo::SetEnabledOverride( -1 );
		Check( Near( cCam2_memo[0], cCam2_direct[0], 1e-14 ),
			"two-camera swap memo == direct without RegenerateData()" );
		Check( std::fabs( cCam1[0] - cCam2_memo[0] ) > 1e-4,
			"two-camera swap produces distinct memoized values" );
		cam2->release();

		// R1 P1-1: Outer memo-worthy ExpressionPainter using sample(child) where the
		// child ExpressionPainter uses osl_to_object_point must NOT collide across instances!
		ExpressionProgram childProg = ExpressionProgram::Invalid();
		Check( CompileExpr( "vec3(osl_snoise(osl_to_object_point(P), 0, 3), 0, 0)", childProg ),
			"child space-transform program compiles" );
		ExpressionPainter* childPainter = new ExpressionPainter( childProg, std::vector<ParamSpec>(), Scalar( 0 ) );
		ExpressionProgram parentProg = ExpressionProgram::Invalid();
		{
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			Check( b.Finalize( "sample(child) + vec3(osl_snoise(Po, 0, 3), 0, 0)", parentProg ),
				"parent sample() + snoise program compiles" );
			std::vector<IPainter*> colorRefs( 1, childPainter );
			std::vector<IScalarPainter*> scalarRefs;
			Check( parentProg.BindPainterRefs( colorRefs, scalarRefs ),
				"parent sample() binds child painter ref" );
		}
		Check( parentProg.MemoWorthy(), "parent sample()+snoise program is memoWorthy" );
		ExpressionPainter* parentPainter = new ExpressionPainter( parentProg, std::vector<ParamSpec>(), Scalar( 0 ) );
		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel pA_memo = parentPainter->GetColor( riA );
		const RISEPel pB_memo = parentPainter->GetColor( riB );
		ExpressionMemo::SetEnabledOverride( 0 );
		const RISEPel pA_dir = parentPainter->GetColor( riA );
		const RISEPel pB_dir = parentPainter->GetColor( riB );
		ExpressionMemo::SetEnabledOverride( -1 );
		Check( Near( pA_memo[0], pA_dir[0], 1e-14 ) && Near( pB_memo[0], pB_dir[0], 1e-14 ),
			"R1 P1-1: sample() wrapping space-transform child matches direct across instances" );
		Check( std::fabs( pA_memo[0] - pB_memo[0] ) > 1e-4,
			"R1 P1-1: sample() wrapping space-transform child distinguishes instances" );
		parentPainter->release();
		childPainter->release();

		// R1 P1-2: In-place Object::FinalizeTransformations() mutating m_mxFinalTrans at the
		// SAME pointer address must NOT hit stale L2 memo entries!
		SphereGeometry* mutSphere = new SphereGeometry( 1.0 );
		Object* mutObj = new Object( mutSphere );
		mutObj->SetPosition( Point3( 0.0, 0.0, 0.0 ) );
		mutObj->FinalizeTransformations();
		RayIntersection mutRi( Ray( Point3( 0.2, 0.1, 3.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		mutObj->IntersectRay( mutRi, 100.0, true, true, false );
		mutRi.geometric.pCamera = cam;
		const Matrix4* ptrBefore = mutRi.geometric.pmxObjectToWorld;
		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel mutBefore = painter->GetColor( mutRi.geometric );
		// Mutate the object transform IN-PLACE (keeps identical m_mxFinalTrans pointer address!)
		mutObj->SetPosition( Point3( 4.2, -1.7, 2.9 ) );
		mutObj->FinalizeTransformations();
		Check( mutRi.geometric.pmxObjectToWorld == ptrBefore, "in-place mutation preserves matrix pointer address" );
		const RISEPel mutAfter_memo = painter->GetColor( mutRi.geometric );
		ExpressionMemo::SetEnabledOverride( 0 );
		const RISEPel mutAfter_dir = painter->GetColor( mutRi.geometric );
		ExpressionMemo::SetEnabledOverride( -1 );
		Check( Near( mutAfter_memo[0], mutAfter_dir[0], 1e-14 ),
			"R1 P1-2: in-place FinalizeTransformations() memo == direct at same matrix pointer" );
		Check( std::fabs( mutBefore[0] - mutAfter_memo[0] ) > 1e-4,
			"R1 P1-2: in-place FinalizeTransformations() updates memoized space transform" );
		mutObj->release();
		mutSphere->release();

		painter->release();
		cam->release();
	}

	void TestSceneIntegration()
	{
		Scene* scene = new Scene();
		ObjectManager* om = new ObjectManager( false, false, 4, 16 );
		CameraManager* cm = new CameraManager();
		scene->SetObjectManager( om );
		scene->SetCameraManager( cm );

		PinholeCamera* cam = new PinholeCamera(
			Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			45.0 * DEG_TO_RAD, 320, 240, 1.0, 0, 0, 0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );
		scene->AddCamera( "main", cam );

		SphereGeometry* sphere = new SphereGeometry( 1.0 );
		Object* obj = new Object( sphere );
		obj->SetStretch( Vector3( 2.0, 0.5, 1.0 ) );
		obj->SetPosition( Point3( 0.0, 0.0, 0.0 ) );
		obj->FinalizeTransformations();
		om->AddItem( obj, "ellipsoid" );

		RayIntersection ri( Ray( Point3( 0.5, 0.2, 5.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		ri.geometric.pCamera = scene->GetCamera();
		om->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit, "scene ray hits ellipsoid" );
		Check( ri.geometric.pmxWorldToObject != 0, "Object::IntersectRay stamped pmxWorldToObject" );
		Check( ri.geometric.pmxObjectToWorld != 0, "Object::IntersectRay stamped pmxObjectToWorld" );
		Check( ri.geometric.pCamera == cam, "PropagateCastInputs preserved live scene camera through ObjectManager" );

		// osl_to_object_point(P) should match Po on the hit!
		ExpressionProgram pObj = ExpressionProgram::Invalid();
		ExpressionProgram pCam = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_to_object_point(P)", pObj ), "scene to_object_point compiles" );
		Check( CompileExpr( "osl_to_camera_point(P)", pCam ), "scene to_camera_point compiles" );

		ExprEvalContext ctx;
		ctx.P = Vector3( ri.geometric.ptIntersection.x, ri.geometric.ptIntersection.y, ri.geometric.ptIntersection.z );
		ctx.Po = Vector3( ri.geometric.ptObjIntersec.x, ri.geometric.ptObjIntersec.y, ri.geometric.ptObjIntersec.z );
		ctx.pHit = &ri.geometric;
		Check( NearVec( pObj.EvalVec3( ctx ), ctx.Po, 1e-9 ), "scene osl_to_object_point(P) == Po" );

		const Vector3 camPos1 = pCam.EvalVec3( ctx );
		Check( camPos1.z > 3.5 && camPos1.z < 5.0, "hit is in front of camera (+Z)" );

		// Move camera backward by +10 in Z -> hit z_cam increases by +10!
		cam->SetLocation( Point3( 0, 0, 15 ) );
		cam->RegenerateData();
		const Vector3 camPos2 = pCam.EvalVec3( ctx );
		Check( Near( camPos2.z - camPos1.z, 10.0, 1e-11 ),
			"moving camera in Scene shifts osl_to_camera_point(P).z by exact delta" );

		// CSGObject clears pmxObjectToWorld = 0 so operand-local pointer is never leaked
		SphereGeometry* csgSphA = new SphereGeometry( 1.0 );
		SphereGeometry* csgSphB = new SphereGeometry( 1.0 );
		Object* csgObjA = new Object( csgSphA );
		Object* csgObjB = new Object( csgSphB );
		csgObjB->SetPosition( Point3( 0.5, 0.0, 0.0 ) );
		csgObjB->FinalizeTransformations();
		CSGObject* csg = new CSGObject( CSG_UNION );
		csg->AssignObjects( csgObjA, csgObjB );
		RayIntersection csgRi( Ray( Point3( 0.0, 0.0, 5.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		csg->IntersectRay( csgRi, 100.0, true, true, false );
		Check( csgRi.geometric.bHit, "CSGObject ray hits union" );
		Check( csgRi.geometric.pmxObjectToWorld == 0,
			"CSGObject::IntersectRay clears pmxObjectToWorld to 0" );
		csg->release();
		csgObjA->release();
		csgObjB->release();
		csgSphA->release();
		csgSphB->release();

		obj->release();
		sphere->release();
		cam->release();
		cm->release();
		om->release();
		scene->release();
	}

	void TestObjectInfoBuiltins()
	{
		// Compile-time checks: context-vars gating and 0-arg arity on ALL 4 builtins
		{
			ExpressionProgram p = ExpressionProgram::Invalid();
			ExpressionProgram::Builder bNoCtx;
			bNoCtx.EnableContextVars( false );
			Check( !bNoCtx.Finalize( "osl_object_random()", p ),
				"osl_object_random rejected when context vars disabled" );
			Check( !bNoCtx.Finalize( "osl_object_color()", p ),
				"osl_object_color rejected when context vars disabled" );
			Check( !bNoCtx.Finalize( "osl_object_alpha()", p ),
				"osl_object_alpha rejected when context vars disabled" );
			Check( !bNoCtx.Finalize( "osl_object_index()", p ),
				"osl_object_index rejected when context vars disabled" );
			Check( !CompileExpr( "osl_object_random(1)", p ),
				"osl_object_random rejects extra arg" );
			Check( !CompileExpr( "osl_object_color(1)", p ),
				"osl_object_color rejects extra arg" );
			Check( !CompileExpr( "osl_object_alpha(1)", p ),
				"osl_object_alpha rejects extra arg" );
			Check( !CompileExpr( "osl_object_index(1)", p ),
				"osl_object_index rejects extra arg" );
		}

		ExpressionProgram pColor = ExpressionProgram::Invalid();
		ExpressionProgram pAlpha = ExpressionProgram::Invalid();
		ExpressionProgram pIndex = ExpressionProgram::Invalid();
		ExpressionProgram pRand  = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_object_color()", pColor ), "osl_object_color compiles" );
		Check( CompileExpr( "osl_object_alpha()", pAlpha ), "osl_object_alpha compiles" );
		Check( CompileExpr( "osl_object_index()", pIndex ), "osl_object_index compiles" );
		Check( CompileExpr( "osl_object_random()", pRand ), "osl_object_random compiles" );
		Check( pColor.UsesSpaceTransform() && pAlpha.UsesSpaceTransform()
			&& pIndex.UsesSpaceTransform() && pRand.UsesSpaceTransform(),
			"osl_object_* builtins set UsesSpaceTransform()" );
		Check( pColor.ResultType() == ExpressionProgram::kVec3
			&& pAlpha.ResultType() == ExpressionProgram::kScalar
			&& pIndex.ResultType() == ExpressionProgram::kScalar
			&& pRand.ResultType() == ExpressionProgram::kScalar,
			"osl_object_* return types (vec3 for color, scalar for alpha/index/random)" );
		Check( !pColor.MemoWorthy() && !pAlpha.MemoWorthy() && !pIndex.MemoWorthy() && !pRand.MemoWorthy(),
			"osl_object_* builtins are not memoWorthy on their own" );

		// Null pHit AND non-null pHit with null pObjectInfo yield 0 (Cycles OBJECT_NONE)
		ExprEvalContext ctx;
		ctx.pHit = nullptr;
		Check( NearVec( pColor.EvalVec3( ctx ), Vector3( 0, 0, 0 ), 1e-14 ), "null pHit object_color is 0" );
		Check( Near( pAlpha.Eval( ctx ), 0.0, 1e-14 ), "null pHit object_alpha is 0" );
		Check( Near( pIndex.Eval( ctx ), 0.0, 1e-14 ), "null pHit object_index is 0" );
		Check( Near( pRand.Eval( ctx ), 0.0, 1e-14 ), "null pHit object_random is 0" );

		RayIntersectionGeometric emptyRig( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		ctx.pHit = &emptyRig;
		Check( NearVec( pColor.EvalVec3( ctx ), Vector3( 0, 0, 0 ), 1e-14 ), "null pObjectInfo object_color is 0" );
		Check( Near( pAlpha.Eval( ctx ), 0.0, 1e-14 ), "null pObjectInfo object_alpha is 0" );
		Check( Near( pIndex.Eval( ctx ), 0.0, 1e-14 ), "null pObjectInfo object_index is 0" );
		Check( Near( pRand.Eval( ctx ), 0.0, 1e-14 ), "null pObjectInfo object_random is 0" );

		// Non-finite ObjectInfoData fields clamp cleanly to 0 via Finite()
		ObjectInfoData nanInfo;
		nanInfo.color[0] = std::numeric_limits<float>::quiet_NaN();
		nanInfo.color[1] = std::numeric_limits<float>::infinity();
		nanInfo.color[2] = -std::numeric_limits<float>::infinity();
		nanInfo.alpha = std::numeric_limits<float>::quiet_NaN();
		nanInfo.objectIndex = std::numeric_limits<float>::infinity();
		nanInfo.random = -std::numeric_limits<float>::infinity();
		emptyRig.pObjectInfo = &nanInfo;
		Check( NearVec( pColor.EvalVec3( ctx ), Vector3( 0, 0, 0 ), 1e-14 ), "non-finite object_color yields 0" );
		Check( Near( pAlpha.Eval( ctx ), 0.0, 1e-14 ), "non-finite object_alpha yields 0" );
		Check( Near( pIndex.Eval( ctx ), 0.0, 1e-14 ), "non-finite object_index yields 0" );
		Check( Near( pRand.Eval( ctx ), 0.0, 1e-14 ), "non-finite object_random yields 0" );

		// Object::SetObjectInfo stamped on Object::IntersectRay and preserved by
		// CloneFull / CloneGeometric / CloneSnapshot
		SphereGeometry* sphere = new SphereGeometry( 1.0 );
		Object* obj = new Object( sphere );
		ObjectInfoData info;
		info.color[0] = 0.25f;
		info.color[1] = 0.50f;
		info.color[2] = 0.75f;
		info.alpha = 0.40f;
		info.objectIndex = 7.0f;
		info.random = 0.6180339f;
		info.particleRandom = 0.37f;
		obj->SetObjectInfo( info );
		obj->FinalizeTransformations();

		RayIntersection ri( Ray( Point3( 0, 0, 4 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		obj->IntersectRay( ri, 100.0, true, true, false );
		Check( ri.geometric.bHit && ri.geometric.pObjectInfo != 0,
			"Object::IntersectRay stamps pObjectInfo" );
		ctx.pHit = &ri.geometric;
		Check( NearVec( pColor.EvalVec3( ctx ), Vector3( 0.25, 0.50, 0.75 ), 1e-6 ),
			"osl_object_color reads ObjectInfoData.color" );
		Check( Near( pAlpha.Eval( ctx ), 0.40, 1e-6 ),
			"osl_object_alpha reads ObjectInfoData.alpha" );
		Check( Near( pIndex.Eval( ctx ), 7.0, 1e-6 ),
			"osl_object_index reads ObjectInfoData.objectIndex" );
		Check( Near( pRand.Eval( ctx ), 0.6180339, 1e-6 ),
			"osl_object_random reads ObjectInfoData.random" );

		auto CheckObjInfoEq = [&]( const ObjectInfoData* ci, const ObjectInfoData& ref, const char* label ) {
			Check( ci != 0 && ci->color[0] == ref.color[0] && ci->color[1] == ref.color[1]
				&& ci->color[2] == ref.color[2] && ci->alpha == ref.alpha
				&& ci->objectIndex == ref.objectIndex && ci->random == ref.random
				&& ci->particleRandom == ref.particleRandom, label );
		};

		// CloneFull, CloneGeometric, CloneSnapshot all preserve ObjectInfoData
		if( IObjectPriv* cloned = obj->CloneFull() ) {
			CheckObjInfoEq( cloned->GetObjectInfo(), info, "Object::CloneFull preserves all ObjectInfoData fields" );
			cloned->release();
		} else {
			Check( false, "Object::CloneFull returned null" );
		}
		if( IObjectPriv* cloned = obj->CloneGeometric() ) {
			CheckObjInfoEq( cloned->GetObjectInfo(), info, "Object::CloneGeometric preserves all ObjectInfoData fields" );
			cloned->release();
		} else {
			Check( false, "Object::CloneGeometric returned null" );
		}
		if( IObjectPriv* cloned = obj->CloneSnapshot() ) {
			CheckObjInfoEq( cloned->GetObjectInfo(), info, "Object::CloneSnapshot preserves all ObjectInfoData fields" );
			cloned->release();
		} else {
			Check( false, "Object::CloneSnapshot returned null" );
		}

		// CSGObject::IntersectRay stamps its own ObjectInfoData (not operand's),
		// and CSGObject::CloneFull / CloneGeometric / CloneSnapshot preserve it!
		SphereGeometry* sA = new SphereGeometry( 1.0 );
		SphereGeometry* sB = new SphereGeometry( 1.0 );
		Object* oA = new Object( sA );
		Object* oB = new Object( sB );
		ObjectInfoData opInfo;
		opInfo.random = 0.11f;
		oA->SetObjectInfo( opInfo );
		oB->SetPosition( Point3( 0.5, 0, 0 ) );
		oB->FinalizeTransformations();
		CSGObject* csg = new CSGObject( CSG_UNION );
		csg->AssignObjects( oA, oB );
		ObjectInfoData csgInfo;
		csgInfo.color[0] = 0.9f; csgInfo.color[1] = 0.2f; csgInfo.color[2] = 0.1f;
		csgInfo.alpha = 0.8f; csgInfo.objectIndex = 42.0f; csgInfo.random = 0.77f; csgInfo.particleRandom = 0.63f;
		csg->SetObjectInfo( csgInfo );
		RayIntersection csgRi( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		csg->IntersectRay( csgRi, 100.0, true, true, false );
		Check( csgRi.geometric.bHit && csgRi.geometric.pObjectInfo != 0,
			"CSGObject::IntersectRay stamps pObjectInfo" );
		ctx.pHit = &csgRi.geometric;
		Check( Near( pRand.Eval( ctx ), 0.77, 1e-6 ) && Near( pIndex.Eval( ctx ), 42.0, 1e-6 ),
			"CSGObject stamps its own ObjectInfoData, not operand's" );
		Check( csg->GetFinalTransformMatrixPtr() == 0 && csg->GetFinalInverseTransformMatrixPtr() == 0,
			"CSGObject returns null transform matrix pointers" );
		if( IObjectPriv* cClone = csg->CloneFull() ) {
			CheckObjInfoEq( cClone->GetObjectInfo(), csgInfo, "CSGObject::CloneFull preserves ObjectInfoData" );
			cClone->release();
		} else {
			Check( false, "CSGObject::CloneFull returned null" );
		}
		if( IObjectPriv* cClone = csg->CloneGeometric() ) {
			CheckObjInfoEq( cClone->GetObjectInfo(), csgInfo, "CSGObject::CloneGeometric preserves ObjectInfoData" );
			cClone->release();
		} else {
			Check( false, "CSGObject::CloneGeometric returned null" );
		}
		if( IObjectPriv* cClone = csg->CloneSnapshot() ) {
			CheckObjInfoEq( cClone->GetObjectInfo(), csgInfo, "CSGObject::CloneSnapshot preserves ObjectInfoData" );
			cClone->release();
		} else {
			Check( false, "CSGObject::CloneSnapshot returned null" );
		}
		csg->release(); oA->release(); oB->release(); sA->release(); sB->release();

		// P1-2: LightSampler::FillEmitterRecord & AverageLuminaryExitance stamp pObjectInfo
		// and object transform pointers so emissive meshes reading ObjectInfo / Location work in NEE.
		{
			ExpressionProgram emProg = ExpressionProgram::Invalid();
			Check( CompileExpr( "vec3(osl_object_color().x, osl_object_random(), osl_to_world_point(vec3(0, 0, 0)).x)", emProg ),
				"emitter ObjectInfo + Location program compiles" );
			ExpressionPainter* emPainter = new ExpressionPainter( emProg, std::vector<ParamSpec>(), Scalar( 0 ) );
			LambertianMaterial* emBaseMat = new LambertianMaterial( *emPainter );
			LambertianLuminaireMaterial* emMat = new LambertianLuminaireMaterial( *emPainter, 1.0, *emBaseMat );

			SphereGeometry* emSphere = new SphereGeometry( 0.5 );
			Object* emObj = new Object( emSphere );
			emObj->AssignMaterial( *emMat );
			emObj->SetPosition( Point3( 0.75, 0.0, 0.0 ) );
			ObjectInfoData emInfo;
			emInfo.color[0] = 0.6f; emInfo.random = 0.35f;
			emObj->SetObjectInfo( emInfo );
			emObj->FinalizeTransformations();

			RayIntersectionGeometric emRig( Ray(), nullRasterizerState );
			LightSampler::FillEmitterRecord( emRig, Point3( 0.75, 0.5, 0.0 ), Vector3( 0, 1, 0 ),
				Point2( 0, 0 ), Point3( 0, 0.5, 0 ), emObj, 0 );
			Check( emRig.pObjectInfo == emObj->GetObjectInfo()
				&& emRig.pmxObjectToWorld == emObj->GetFinalTransformMatrixPtr()
				&& emRig.pmxWorldToObject == emObj->GetFinalInverseTransformMatrixPtr(),
				"LightSampler::FillEmitterRecord stamps pObjectInfo and object matrices" );

			const LightSampler::LuminaryExitance avg = LightSampler::AverageLuminaryExitance( emObj );
			Check( !avg.uniform && Near( avg.average.r, 0.6, 1e-4 ) && Near( avg.average.g, 0.35, 1e-4 )
				&& Near( avg.average.b, 0.75, 1e-4 ),
				"LightSampler::AverageLuminaryExitance reads live ObjectInfo + Location and clears uniform" );

			emObj->release(); emSphere->release(); emMat->release(); emBaseMat->release(); emPainter->release();
		}

		// L2 ExpressionMemo soundness: single-field isolation (alpha, color.g, color.b, random)
		// AND nested sample(child) wrapping an osl_object_* child!
		ExpressionProgram memoProg = ExpressionProgram::Invalid();
		Check( CompileExpr( "vec3(osl_snoise(Po + vec3(osl_object_random() * 10.0, 0, 0), 0, 3) + osl_object_alpha(), osl_object_color().y, osl_object_color().z + osl_object_index())", memoProg ),
			"memo-worthy ObjectInfo program compiles" );
		Check( memoProg.MemoWorthy() && memoProg.UsesSpaceTransform(),
			"ObjectInfo + snoise program is memoWorthy and UsesSpaceTransform" );
		ExpressionPainter* painter = new ExpressionPainter( memoProg, std::vector<ParamSpec>(), Scalar( 0 ) );

		ObjectInfoData infoBase = info;
		RayIntersectionGeometric riBase = ri.geometric;
		riBase.pObjectInfo = &infoBase;

		// Vary ONLY alpha
		ObjectInfoData infoAlpha = infoBase;
		infoAlpha.alpha = 0.91f;
		RayIntersectionGeometric riAlpha = riBase;
		riAlpha.pObjectInfo = &infoAlpha;

		// Vary ONLY color[1]
		ObjectInfoData infoG = infoBase;
		infoG.color[1] = 0.88f;
		RayIntersectionGeometric riG = riBase;
		riG.pObjectInfo = &infoG;

		// Vary ONLY color[2]
		ObjectInfoData infoB = infoBase;
		infoB.color[2] = 0.12f;
		RayIntersectionGeometric riB = riBase;
		riB.pObjectInfo = &infoB;

		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel mBase  = painter->GetColor( riBase );
		const RISEPel mAlpha = painter->GetColor( riAlpha );
		const RISEPel mG     = painter->GetColor( riG );
		const RISEPel mB     = painter->GetColor( riB );
		ExpressionMemo::SetEnabledOverride( 0 );
		const RISEPel dBase  = painter->GetColor( riBase );
		const RISEPel dAlpha = painter->GetColor( riAlpha );
		const RISEPel dG     = painter->GetColor( riG );
		const RISEPel dB     = painter->GetColor( riB );
		ExpressionMemo::SetEnabledOverride( -1 );

		Check( Near( mBase[0], dBase[0], 1e-14 ) && Near( mAlpha[0], dAlpha[0], 1e-14 )
			&& std::fabs( mBase[0] - mAlpha[0] ) > 0.4,
			"ObjectInfo memo isolates alpha-only change" );
		Check( Near( mG[1], dG[1], 1e-14 ) && std::fabs( mBase[1] - mG[1] ) > 0.3,
			"ObjectInfo memo isolates color.g-only change" );
		Check( Near( mB[2], dB[2], 1e-14 ) && std::fabs( mBase[2] - mB[2] ) > 0.5,
			"ObjectInfo memo isolates color.b-only change" );

		// Nested sample(child) wrapping an osl_object_random() child must also separate!
		ExpressionProgram parentProg = ExpressionProgram::Invalid();
		{
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			Check( b.Finalize( "sample(child) + vec3(osl_snoise(Po, 0, 3), 0, 0)", parentProg ),
				"parent sample(child) with ObjectInfo child compiles" );
			std::vector<IPainter*> colorRefs( 1, painter );
			std::vector<IScalarPainter*> scalarRefs;
			Check( parentProg.BindPainterRefs( colorRefs, scalarRefs ), "parent binds ObjectInfo child" );
		}
		ExpressionPainter* parentPainter = new ExpressionPainter( parentProg, std::vector<ParamSpec>(), Scalar( 0 ) );
		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel parBase  = parentPainter->GetColor( riBase );
		const RISEPel parAlpha = parentPainter->GetColor( riAlpha );
		ExpressionMemo::SetEnabledOverride( 0 );
		const RISEPel parAlphaDir = parentPainter->GetColor( riAlpha );
		ExpressionMemo::SetEnabledOverride( -1 );
		Check( Near( parAlpha[0], parAlphaDir[0], 1e-14 ) && std::fabs( parBase[0] - parAlpha[0] ) > 0.4,
			"sample(child) wrapping osl_object_* child matches direct and separates instances" );
		parentPainter->release();

		painter->release();
		obj->release();
		sphere->release();
	}

	void TestPhase1Builtins()
	{
		// 1. osl_backfacing() & osl_true_normal() & osl_shading_normal() & osl_particle_random()
		ExpressionProgram pBack = ExpressionProgram::Invalid();
		ExpressionProgram pTrueN = ExpressionProgram::Invalid();
		ExpressionProgram pShadN = ExpressionProgram::Invalid();
		ExpressionProgram pPartR = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_backfacing()", pBack ), "osl_backfacing compiles" );
		Check( CompileExpr( "osl_true_normal()", pTrueN ), "osl_true_normal compiles" );
		Check( CompileExpr( "osl_shading_normal()", pShadN ), "osl_shading_normal compiles" );
		Check( CompileExpr( "osl_particle_random()", pPartR ), "osl_particle_random compiles" );
		Check( pBack.UsesSpaceTransform() && pTrueN.UsesSpaceTransform()
			&& pShadN.UsesSpaceTransform() && pPartR.UsesSpaceTransform(),
			"Phase 1 hit-context builtins set UsesSpaceTransform" );

		RayIntersectionGeometric riFront( Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		riFront.vGeomNormal = Vector3( 0, 0, 1 );
		riFront.vNormal = Vector3( 0, 0, 1 );
		riFront.arrivalGeomFacing = 0;
		ObjectInfoData infoA, infoB;
		infoA.particleRandom = 0.25f;
		infoB.particleRandom = 0.85f;
		riFront.pObjectInfo = &infoA;

		RayIntersectionGeometric riBack = riFront;
		riBack.ray = Ray( Point3( 0, 0, -2 ), Vector3( 0, 0, 1 ) );
		riBack.pObjectInfo = &infoB;

		ExprEvalContext ctxFront, ctxBack;
		ctxFront.pHit = &riFront;
		ctxBack.pHit = &riBack;
		Check( Near( pBack.Eval( ctxFront ), 0.0, 1e-14 ), "osl_backfacing == 0 on front hit" );
		Check( Near( pBack.Eval( ctxBack ), 1.0, 1e-14 ), "osl_backfacing == 1 on back hit" );
		Check( NearVec( pTrueN.EvalVec3( ctxFront ), Vector3( 0, 0, 1 ), 1e-14 ), "osl_true_normal points +Z on front hit" );
		Check( NearVec( pTrueN.EvalVec3( ctxBack ), Vector3( 0, 0, -1 ), 1e-14 ), "osl_true_normal flips to -Z on back hit" );
		Check( Near( pPartR.Eval( ctxFront ), 0.25, 1e-6 ), "osl_particle_random reads particleRandom A" );
		Check( Near( pPartR.Eval( ctxBack ), 0.85, 1e-6 ), "osl_particle_random reads particleRandom B" );

		// osl_shading_normal returns pre-modifier N0 even when normalMapOnb perturbed vNormal
		{
			RayIntersectionGeometric riNorm = riFront;
			riNorm.vNormal = Vector3Ops::Normalize( Vector3( 0.6, 0.0, 0.8 ) );
			riNorm.normalMapOnb.CreateFromW( Vector3( 0, 0, 1 ) );
			riNorm.bHasNormalMapFrame = true;
			ExprEvalContext ctxNorm;
			ctxNorm.N = riNorm.vNormal;
			ctxNorm.pHit = &riNorm;
			Check( NearVec( pShadN.EvalVec3( ctxNorm ), Vector3( 0, 0, 1 ), 1e-14 ),
				"osl_shading_normal returns pre-modifier N0 (normalMapOnb.w) when normal-mapped" );
		}

		// Real double-sided TriangleMeshGeometryIndexed front & back IntersectRay (DL-70 flip test!)
		{
			TriangleMeshGeometryIndexed* dsMesh = new TriangleMeshGeometryIndexed( true, false );
			dsMesh->BeginIndexedTriangles();
			dsMesh->AddVertex( Point3( -1, -1, 0 ) );
			dsMesh->AddVertex( Point3(  1, -1, 0 ) );
			dsMesh->AddVertex( Point3(  0,  1, 0 ) );
			dsMesh->AddNormal( Vector3( 0, 0, 1 ) );
			dsMesh->AddTexCoord( Point2( 0, 0 ) );
			IndexedTriangle tri;
			tri.iVertices[0] = 0; tri.iVertices[1] = 1; tri.iVertices[2] = 2;
			tri.iNormals[0] = 0;  tri.iNormals[1] = 0;  tri.iNormals[2] = 0;
			tri.iCoords[0] = 0;   tri.iCoords[1] = 0;   tri.iCoords[2] = 0;
			dsMesh->AddIndexedTriangle( tri );
			dsMesh->DoneIndexedTriangles();

			RayIntersectionGeometric dsFront( Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			dsMesh->IntersectRay( dsFront, true, true, false );
			RayIntersectionGeometric dsBack( Ray( Point3( 0, 0, -2 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
			dsMesh->IntersectRay( dsBack, true, true, false );
			Check( dsFront.bHit && dsBack.bHit, "double-sided triangle hit from both front and back" );
			Check( !dsFront.bGeomNormalOrientedToRay && dsBack.bGeomNormalOrientedToRay,
				"double-sided backface hit sets bGeomNormalOrientedToRay (DL-70)" );
			ExprEvalContext cDsF, cDsB;
			cDsF.pHit = &dsFront;
			cDsB.pHit = &dsBack;
			Check( Near( pBack.Eval( cDsF ), 0.0, 1e-14 ),
				"osl_backfacing == 0 on real double-sided mesh front hit" );
			Check( Near( pBack.Eval( cDsB ), 1.0, 1e-14 ),
				"osl_backfacing == 1 on real double-sided mesh back hit despite flipped vGeomNormal" );

			// Hair-like ray-derived normal (HasTrueGeomSide() == false) must return 0.0
			RayIntersectionGeometric dsHair = dsBack;
			dsHair.bGeomNormalRayDerived = true;
			ExprEvalContext cHair;
			cHair.pHit = &dsHair;
			Check( Near( pBack.Eval( cHair ), 0.0, 1e-14 ),
				"osl_backfacing == 0 when bGeomNormalRayDerived (HasTrueGeomSide == false)" );

			// L2 memo must separate double-sided front vs back hits even with identical Po/vNormal/geomOpposesArrival
			ExpressionProgram pDsMemo = ExpressionProgram::Invalid();
			Check( CompileExpr( "vec3(osl_backfacing(), osl_snoise(Po, 0, 3), 0)", pDsMemo ),
				"double-sided memo backfacing expression compiles" );
			ExpressionPainter* epDs = new ExpressionPainter( pDsMemo, std::vector<ParamSpec>(), Scalar( 0 ) );
			RayIntersectionGeometric dsBackSameN = dsFront;
			dsBackSameN.bGeomNormalOrientedToRay = true;
			ExpressionMemo::SetEnabledOverride( 1 );
			const RISEPel pF = epDs->GetColor( dsFront );
			const RISEPel pB = epDs->GetColor( dsBackSameN );
			ExpressionMemo::SetEnabledOverride( -1 );
			Check( Near( pF[0], 0.0, 1e-14 ) && Near( pB[0], 1.0, 1e-14 ),
				"L2 memo separates double-sided front vs back via SpaceTransformHitKey::geomBackfacing" );
			epDs->release();
			dsMesh->release();
		}

		// Memo separation on particleRandom and backfacing (with memo-worthy osl_snoise)
		ExpressionProgram pMemo = ExpressionProgram::Invalid();
		Check( CompileExpr( "vec3(osl_backfacing(), osl_particle_random(), osl_snoise(Po, 0, 3))", pMemo ),
			"memo-worthy Phase 1 hit-context expression compiles" );
		ExpressionPainter* ep = new ExpressionPainter( pMemo, std::vector<ParamSpec>(), Scalar( 0 ) );
		ExpressionMemo::SetEnabledOverride( 1 );
		const RISEPel mFrontA = ep->GetColor( riFront );
		riFront.pObjectInfo = &infoB;
		const RISEPel mFrontB = ep->GetColor( riFront );
		const RISEPel mBackB = ep->GetColor( riBack );
		ExpressionMemo::SetEnabledOverride( -1 );
		Check( Near( mFrontA[1], 0.25, 1e-6 ) && Near( mFrontB[1], 0.85, 1e-6 ),
			"L2 memo separates particleRandom-only change" );
		Check( Near( mFrontB[0], 0.0, 1e-14 ) && Near( mBackB[0], 1.0, 1e-14 ),
			"L2 memo separates frontfacing vs backfacing hit" );
		ep->release();

		// 2. osl_gradient across all 7 Cycles types (matching Cycles NodeGradientType enum order)
		{
			ExpressionProgram pLin = ExpressionProgram::Invalid();
			ExpressionProgram pQuad = ExpressionProgram::Invalid();
			ExpressionProgram pEase = ExpressionProgram::Invalid();
			ExpressionProgram pDiag = ExpressionProgram::Invalid();
			ExpressionProgram pRad = ExpressionProgram::Invalid();
			ExpressionProgram pQSph = ExpressionProgram::Invalid();
			ExpressionProgram pSph = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_gradient(Po, 0)", pLin ), "osl_gradient LINEAR compiles" );
			Check( CompileExpr( "osl_gradient(Po, 1)", pQuad ), "osl_gradient QUADRATIC compiles" );
			Check( CompileExpr( "osl_gradient(Po, 2)", pEase ), "osl_gradient EASING compiles" );
			Check( CompileExpr( "osl_gradient(Po, 3)", pDiag ), "osl_gradient DIAGONAL compiles" );
			Check( CompileExpr( "osl_gradient(Po, 4)", pRad ), "osl_gradient RADIAL compiles" );
			Check( CompileExpr( "osl_gradient(Po, 5)", pQSph ), "osl_gradient QUADRATIC_SPHERE compiles" );
			Check( CompileExpr( "osl_gradient(Po, 6)", pSph ), "osl_gradient SPHERICAL compiles" );
			ExpressionProgram pBad = ExpressionProgram::Invalid();
			Check( !CompileExpr( "osl_gradient(Po, 7)", pBad ), "osl_gradient rejects out-of-range type 7" );

			ExprEvalContext c;
			c.Po = Vector3( 0.5, 0.25, 0.0 );
			Check( Near( pLin.Eval( c ), 0.5, 1e-6 ), "gradient LINEAR(0.5)" );
			Check( Near( pQuad.Eval( c ), 0.25, 1e-6 ), "gradient QUADRATIC(0.5)" );
			Check( Near( pEase.Eval( c ), 0.5, 1e-6 ), "gradient EASING(0.5)" );
			Check( Near( pDiag.Eval( c ), 0.375, 1e-6 ), "gradient DIAGONAL(0.5, 0.25)" );
			const double d = std::sqrt( 0.5 * 0.5 + 0.25 * 0.25 );
			const double sph = 0.999999 - d;
			const double rad = std::atan2( 0.25, 0.5 ) / ( 2.0 * 3.14159265358979323846 ) + 0.5;
			Check( Near( pRad.Eval( c ), rad, 1e-5 ), "gradient RADIAL" );
			Check( Near( pQSph.Eval( c ), sph * sph, 1e-5 ), "gradient QUADRATIC_SPHERE" );
			Check( Near( pSph.Eval( c ), sph, 1e-5 ), "gradient SPHERICAL" );
			// Unit length spherical bias yields exact 0
			c.Po = Vector3( 1.0, 0.0, 0.0 );
			Check( Near( pSph.Eval( c ), 0.0, 1e-7 ), "gradient SPHERICAL at unit radius is 0" );
		}

		// 3. osl_brick: mortar vs interior, offset/squash, and degenerate zero-width guard
		{
			ExpressionProgram pBrick = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_brick(Po, 0.05, 0.0, 0.0, 0.5, 0.25, 0.5, 2, 1.0, 2)", pBrick ),
				"osl_brick compiles" );
			ExprEvalContext c;
			// Center of brick (0,0) on row 0 (offset=0.25 -> x in [0, 0.5], y in [0, 0.25])
			c.Po = Vector3( 0.0, 0.125, 0.0 );
			const Vector3 bCenter = pBrick.EvalVec3( c );
			Check( bCenter.x >= 0.0 && bCenter.x <= 1.0, "osl_brick tint in [0, 1]" );
			Check( Near( bCenter.y, 0.0, 1e-6 ), "osl_brick interior mortar == 0" );
			// Right on row boundary y = 0.01 < mortar_size(0.05) -> mortar == 1
			c.Po = Vector3( 0.0, 0.01, 0.0 );
			const Vector3 bMortar = pBrick.EvalVec3( c );
			Check( Near( bMortar.y, 1.0, 1e-6 ), "osl_brick mortar seam == 1" );
			// Zero brick_width / row_height must return finite 0 without UB
			ExpressionProgram pZero = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_brick(Po, 0.05, 0.0, 0.0, 0.0, 0.25, 0.5, 2, 1.0, 2)", pZero ),
				"osl_brick zero-width compiles" );
			Check( NearVec( pZero.EvalVec3( c ), Vector3( 0, 0, 0 ), 1e-14 ), "osl_brick zero-width returns 0" );
		}
	}

	// Custom test IPainter returning RGB = (u, v, footprintValid?1:0) and Alpha = 0.25*u + 0.5*v.
	class UvAlphaProbePainter : public Painter
	{
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ri ) const override
		{
			return RISEPel( ri.ptCoord.x, ri.ptCoord.y, ri.txFootprint.valid ? Scalar(1) : Scalar(0) );
		}
		Scalar GetAlpha( const RayIntersectionGeometric& ri ) const override
		{
			return Scalar(0.25) * ri.ptCoord.x + Scalar(0.5) * ri.ptCoord.y;
		}
		Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar ) const override { return Scalar(0); }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) override {}
		void RegenerateData() override {}
	};

	void TestSampleUvAndAlpha()
	{
		UvAlphaProbePainter* probe = new UvAlphaProbePainter();
		std::vector<IPainter*> colorRefs( 1, probe );
		std::vector<IScalarPainter*> scalarRefs;

		// 1. sample_alpha(tex), sample_uv(tex, u, v), sample_uv_alpha(tex, u, v)
		ExpressionProgram pAlpha = ExpressionProgram::Invalid();
		ExpressionProgram pUv = ExpressionProgram::Invalid();
		ExpressionProgram pUvAlpha = ExpressionProgram::Invalid();
		Check( CompileExpr( "sample_alpha(tex)", pAlpha ), "sample_alpha compiles" );
		Check( CompileExpr( "sample_uv(tex, u * 0.5 + 0.1, v * 0.25 + 0.2)", pUv ), "sample_uv compiles" );
		Check( CompileExpr( "sample_uv_alpha(tex, u + 0.2, v + 0.3)", pUvAlpha ), "sample_uv_alpha compiles" );
		Check( pAlpha.UsesPainterSample() && pUv.UsesPainterSample() && pUvAlpha.UsesPainterSample(),
			"sample_alpha/sample_uv/sample_uv_alpha set UsesPainterSample()" );
		Check( pAlpha.MemoWorthy() && pUv.MemoWorthy() && pUvAlpha.MemoWorthy(),
			"sample_alpha/sample_uv/sample_uv_alpha are MemoWorthy()" );
		Check( pAlpha.BindPainterRefs( colorRefs, scalarRefs ), "bind sample_alpha" );
		Check( pUv.BindPainterRefs( colorRefs, scalarRefs ), "bind sample_uv" );
		Check( pUvAlpha.BindPainterRefs( colorRefs, scalarRefs ), "bind sample_uv_alpha" );

		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		ri.ptCoord = Point2( 0.4, 0.6 );
		ri.txFootprint.valid = true;
		ExprEvalContext ctx;
		ctx.u = 0.4; ctx.v = 0.6;
		ctx.pHit = &ri;

		// sample_alpha reads live ri.ptCoord -> 0.25*0.4 + 0.5*0.6 = 0.4
		Check( Near( pAlpha.Eval( ctx ), 0.4, 1e-12 ), "sample_alpha reads GetAlpha at hit UV" );
		// sample_uv overrides ptCoord -> (0.3, 0.35) and clears txFootprint.valid (z == 0)
		Check( NearVec( pUv.EvalVec3( ctx ), Vector3( 0.3, 0.35, 0.0 ), 1e-12 ),
			"sample_uv overrides ptCoord and clears txFootprint.valid" );
		// sample_uv_alpha overrides ptCoord -> (0.6, 0.9) -> 0.25*0.6 + 0.5*0.9 = 0.6
		Check( Near( pUvAlpha.Eval( ctx ), 0.6, 1e-12 ), "sample_uv_alpha reads GetAlpha at overridden UV" );
		// Original hit record must remain untouched!
		Check( Near( ri.ptCoord.x, 0.4, 1e-14 ) && ri.txFootprint.valid,
			"sample_uv leaves caller RayIntersectionGeometric untouched" );

		// 2. Non-finite UV arguments guard cleanly to (0, 0)
		ExprEvalContext ctxNan = ctx;
		ctxNan.u = std::numeric_limits<double>::quiet_NaN();
		ctxNan.v = std::numeric_limits<double>::infinity();
		Check( NearVec( pUv.EvalVec3( ctxNan ), Vector3( 0.0, 0.0, 0.0 ), 1e-12 ),
			"sample_uv guards non-finite UV to (0, 0)" );

		// 3. ExtractSampleRefs & RewriteSampleCallRefs cover all new sample_* forms
		{
			const std::string body = "sample(foo) + sample_uv(foo, u, v) + vec3(sample_alpha(foo), sample_uv_alpha(foo, 0.1, 0.2), sample_scalar(foo))";
			std::vector<ExpressionProgram::Builder::SampleRef> refs;
			Check( ExpressionProgram::Builder::ExtractSampleRefs( body, refs ), "ExtractSampleRefs succeeds" );
			Check( refs.size() == 5, "ExtractSampleRefs finds all 5 sample* calls" );
			const std::string rewritten = ExpressionProgram::Builder::RewriteSampleCallRefs( body, false, "foo", "bar" );
			Check( rewritten == "sample(bar) + sample_uv(bar, u, v) + vec3(sample_alpha(bar), sample_uv_alpha(bar, 0.1, 0.2), sample_scalar(foo))",
				"RewriteSampleCallRefs rewrites all 4 color sample* forms while preserving sample_scalar" );
		}

		// 4. Context-var gating: sample_uv/sample_alpha rejected when EnableContextVars(false)
		{
			ExpressionProgram::Builder b;
			b.EnableContextVars( false );
			ExpressionProgram bad = ExpressionProgram::Invalid();
			Check( !b.Finalize( "sample_uv(tex, u, v)", bad ), "sample_uv rejected without context vars" );
			ExpressionProgram::Builder b2;
			b2.EnableContextVars( false );
			Check( !b2.Finalize( "sample_alpha(tex)", bad ), "sample_alpha rejected without context vars" );
		}

		// 5. pHit == nullptr (BuildSyntheticRi fallback) for sample_alpha / sample_uv / sample_uv_alpha
		{
			ExprEvalContext ctxNull;
			ctxNull.u = 0.4; ctxNull.v = 0.6;
			ctxNull.pHit = 0;
			Check( Near( pAlpha.Eval( ctxNull ), 0.4, 1e-12 ), "sample_alpha works with pHit == nullptr" );
			Check( NearVec( pUv.EvalVec3( ctxNull ), Vector3( 0.3, 0.35, 0.0 ), 1e-12 ),
				"sample_uv works with pHit == nullptr" );
			Check( Near( pUvAlpha.Eval( ctxNull ), 0.6, 1e-12 ), "sample_uv_alpha works with pHit == nullptr" );
		}

		// 6. SampleGuard cycle/recursion protection for sample_uv / sample_alpha / sample_uv_alpha
		{
			struct RecursiveProbePainter : public Painter {
				const ExpressionProgram* progUv = 0;
				const ExpressionProgram* progAlpha = 0;
				int calls = 0;
				RISEPel GetColor( const RayIntersectionGeometric& ri ) const override {
					const_cast<RecursiveProbePainter*>( this )->calls++;
					ExprEvalContext c; c.u = ri.ptCoord.x; c.v = ri.ptCoord.y; c.pHit = &ri;
					const Vector3 v = progUv ? progUv->EvalVec3( c ) : Vector3( 0, 0, 0 );
					return RISEPel( v.x, v.y, v.z );
				}
				Scalar GetAlpha( const RayIntersectionGeometric& ri ) const override {
					const_cast<RecursiveProbePainter*>( this )->calls++;
					ExprEvalContext c; c.u = ri.ptCoord.x; c.v = ri.ptCoord.y; c.pHit = &ri;
					return progAlpha ? progAlpha->Eval( c ) : Scalar( 1 );
				}
				SpectralPacket GetSpectrum( const RayIntersectionGeometric& ) const override { return SpectralPacket(); }
				Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar ) const override { return 0; }
				IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
				void SetIntermediateValue( const IKeyframeParameter& ) override {}
				void RegenerateData() override {}
			};
			RecursiveProbePainter* rec = new RecursiveProbePainter();
			std::vector<IPainter*> recRefs( 1, rec );

			ExpressionProgram pSelfUv = ExpressionProgram::Invalid();
			ExpressionProgram pSelfAlpha = ExpressionProgram::Invalid();
			ExpressionProgram pSelfUvAlpha = ExpressionProgram::Invalid();
			Check( CompileExpr( "sample_uv(self_p, u, v)", pSelfUv ), "self sample_uv compiles" );
			Check( CompileExpr( "sample_alpha(self_p)", pSelfAlpha ), "self sample_alpha compiles" );
			Check( CompileExpr( "sample_uv_alpha(self_p, u, v)", pSelfUvAlpha ), "self sample_uv_alpha compiles" );
			Check( pSelfUv.BindPainterRefs( recRefs, scalarRefs ), "bind recursive sample_uv" );
			Check( pSelfAlpha.BindPainterRefs( recRefs, scalarRefs ), "bind recursive sample_alpha" );
			Check( pSelfUvAlpha.BindPainterRefs( recRefs, scalarRefs ), "bind recursive sample_uv_alpha" );

			rec->progUv = &pSelfUv;
			rec->progAlpha = &pSelfAlpha;
			const Vector3 cycUv = pSelfUv.EvalVec3( ctx );
			const Scalar cycA = pSelfAlpha.Eval( ctx );
			rec->progAlpha = &pSelfUvAlpha;
			const Scalar cycUvA = pSelfUvAlpha.Eval( ctx );
			Check( std::isfinite( cycUv.x ) && std::isfinite( cycUv.y ) && std::isfinite( cycUv.z ) && rec->calls > 0 && rec->calls <= 64,
				"SampleGuard terminates infinite recursion on sample_uv/sample_alpha/sample_uv_alpha" );
			Check( std::isfinite( cycA ) && std::isfinite( cycUvA ),
				"SampleGuard returns finite neutral on recursive alpha calls" );
			rec->release();
		}

		// 7. Spectral (GetColorNM) path + L2 ExpressionMemo soundness (including same-ptCoord SpaceTransformHitKey separation)
		{
			ExpressionPainter* ep = new ExpressionPainter( pUv, std::vector<ParamSpec>(), Scalar( 0 ) );
			RayIntersectionGeometric riA = ri;
			riA.ptCoord = Point2( 0.2, 0.4 );
			RayIntersectionGeometric riB = ri;
			riB.ptCoord = Point2( 0.8, 0.6 );
			ExpressionMemo::SetEnabledOverride( 1 );
			const RISEPel cA = ep->GetColor( riA );
			const RISEPel cB = ep->GetColor( riB );
			const Scalar nm550 = ep->GetColorNM( riA, Scalar( 550.0 ) );
			ExpressionMemo::SetEnabledOverride( -1 );
			Check( Near( cA[0], 0.2, 1e-12 ) && Near( cA[1], 0.3, 1e-12 ), "sample_uv memo hit A" );
			Check( Near( cB[0], 0.5, 1e-12 ) && Near( cB[1], 0.35, 1e-12 ), "sample_uv memo separates hit B" );
			Check( std::isfinite( nm550 ) && nm550 > 0.0, "ExpressionPainter::GetColorNM works with sample_uv" );
			ep->release();

			// Same ptCoord, different SpaceTransformHitKey (objectInfo.random) -> must NOT collide in L2 memo!
			ExpressionProgram pUvRand = ExpressionProgram::Invalid();
			Check( CompileExpr( "sample_uv(tex, osl_object_random(), 0.25)", pUvRand ), "sample_uv with osl_object_random compiles" );
			Check( pUvRand.BindPainterRefs( colorRefs, scalarRefs ), "bind sample_uv(osl_object_random)" );
			ExpressionPainter* epRand = new ExpressionPainter( pUvRand, std::vector<ParamSpec>(), Scalar( 0 ) );
			ObjectInfoData infoA, infoB;
			infoA.random = 0.2f;
			infoB.random = 0.7f;
			RayIntersectionGeometric riSameA = ri;
			riSameA.ptCoord = Point2( 0.5, 0.5 );
			riSameA.pObjectInfo = &infoA;
			RayIntersectionGeometric riSameB = riSameA;
			riSameB.pObjectInfo = &infoB;
			ExpressionMemo::SetEnabledOverride( 1 );
			const RISEPel rA = epRand->GetColor( riSameA );
			const RISEPel rB = epRand->GetColor( riSameB );
			ExpressionMemo::SetEnabledOverride( -1 );
			Check( Near( rA[0], 0.2, 1e-6 ) && Near( rB[0], 0.7, 1e-6 ),
				"L2 memo separates same-ptCoord sample_uv driven by SpaceTransformHitKey" );
			epRand->release();
		}

		// 8. ExpressionScalarPainter::IsPositionIndependent & IsWorldPositionField reject UsesSpaceTransform
		{
			ExpressionProgram pRandS = ExpressionProgram::Invalid();
			ExpressionProgram pObjP = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_object_random()", pRandS ), "osl_object_random scalar compiles" );
			Check( CompileExpr( "osl_to_object_point(P).x", pObjP ), "osl_to_object_point(P).x compiles" );
			ExpressionScalarPainter* spRand = new ExpressionScalarPainter( pRandS, std::vector<ParamSpec>() );
			ExpressionScalarPainter* spObjP = new ExpressionScalarPainter( pObjP, std::vector<ParamSpec>() );
			Check( !spRand->IsPositionIndependent(), "ExpressionScalarPainter::IsPositionIndependent false for UsesSpaceTransform" );
			Check( !spObjP->IsWorldPositionField(), "ExpressionScalarPainter::IsWorldPositionField false for UsesSpaceTransform" );
			spRand->release();
			spObjP->release();
		}

		probe->release();
	}

	void TestPhase3LayerWeightFresnelCameraRayAndBlendedModifier()
	{
		// 1. osl_is_camera_ray() + L2 memo separation
		{
			ExpressionProgram pCam = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_is_camera_ray()", pCam ), "osl_is_camera_ray compiles" );
			Check( pCam.UsesSpaceTransform(), "osl_is_camera_ray sets UsesSpaceTransform" );
			RayIntersectionGeometric riCam( Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			riCam.bIsCameraRay = true;
			RayIntersectionGeometric riBounce = riCam;
			riBounce.bIsCameraRay = false;
			ExprEvalContext cCam, cBounce, cNull;
			cCam.pHit = &riCam;
			cBounce.pHit = &riBounce;
			cNull.pHit = 0;
			Check( Near( pCam.Eval( cCam ), 1.0, 1e-12 ), "osl_is_camera_ray == 1 on camera ray" );
			Check( Near( pCam.Eval( cBounce ), 0.0, 1e-12 ), "osl_is_camera_ray == 0 on bounce/shadow ray" );
			Check( Near( pCam.Eval( cNull ), 0.0, 1e-12 ), "osl_is_camera_ray == 0 on null hit" );

			// L2 memo separation at identical hit point / UV / normal
			ExpressionProgram pCamMemo = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_is_camera_ray() + 0.0 * perlin(P)", pCamMemo ), "memo-worthy osl_is_camera_ray compiles" );
			Check( pCamMemo.MemoWorthy(), "pCamMemo is MemoWorthy" );
			ExpressionPainter* epCam = new ExpressionPainter( pCamMemo, std::vector<ParamSpec>(), Scalar( 0 ) );
			ExpressionMemo::SetEnabledOverride( 1 );
			const RISEPel vCam = epCam->GetColor( riCam );
			const RISEPel vBounce = epCam->GetColor( riBounce );
			ExpressionMemo::SetEnabledOverride( -1 );
			Check( Near( vCam[0], 1.0, 1e-12 ) && Near( vBounce[0], 0.0, 1e-12 ),
				"L2 memo separates bIsCameraRay=true vs false at identical hit" );
			epCam->release();
		}

		// 2. osl_layer_weight (Fresnel & Facing) + osl_fresnel + L2 memo separation on ray.Dir()
		{
			ExpressionProgram pLwFresnel = ExpressionProgram::Invalid();
			ExpressionProgram pLwFacing = ExpressionProgram::Invalid();
			ExpressionProgram pFresnel = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_layer_weight(0.5, vec3(0, 0, 0), 0)", pLwFresnel ), "osl_layer_weight Fresnel compiles" );
			Check( CompileExpr( "osl_layer_weight(0.5, vec3(0, 0, 0), 1)", pLwFacing ), "osl_layer_weight Facing compiles" );
			Check( CompileExpr( "osl_fresnel(1.5, vec3(0, 0, 0), 0)", pFresnel ), "osl_fresnel compiles" );
			Check( pLwFresnel.UsesViewRay() && pFresnel.UsesViewRay(), "view-ray builtins set UsesViewRay()" );

			// Head-on (wi = (0,0,1), N = (0,0,1), cos=1):
			//   LayerWeight Fresnel(blend=0.5): eta = 1 / 0.5 = 2.0 -> F0 = ((2-1)/(2+1))^2 = 1/9 = 0.111111
			//   LayerWeight Facing(blend=0.5): 1 - |cos| = 0.0
			//   Fresnel(ior=1.5): F0 = ((1.5-1)/(1.5+1))^2 = 0.04
			RayIntersectionGeometric riHead( Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			riHead.vNormal = Vector3( 0, 0, 1 );
			riHead.vGeomNormal = Vector3( 0, 0, 1 );
			ExprEvalContext cHead; cHead.pHit = &riHead;
			Check( Near( pLwFresnel.Eval( cHead ), 1.0 / 9.0, 1e-5 ), "osl_layer_weight Fresnel head-on matches Cycles (1/9)" );
			Check( Near( pLwFacing.Eval( cHead ), 0.0, 1e-6 ), "osl_layer_weight Facing head-on == 0" );
			Check( Near( pFresnel.Eval( cHead ), 0.04, 1e-5 ), "osl_fresnel(1.5) head-on == 0.04" );

			// Grazing (wi = (1,0,0), N = (0,0,1), cos=0): all three -> 1.0
			RayIntersectionGeometric riGraze( Ray( Point3( 2, 0, 0 ), Vector3( -1, 0, 0 ) ), nullRasterizerState );
			riGraze.vNormal = Vector3( 0, 0, 1 );
			riGraze.vGeomNormal = Vector3( 0, 0, 1 );
			ExprEvalContext cGraze; cGraze.pHit = &riGraze;
			Check( Near( pLwFresnel.Eval( cGraze ), 1.0, 1e-5 ), "osl_layer_weight Fresnel grazing == 1" );
			Check( Near( pLwFacing.Eval( cGraze ), 1.0, 1e-5 ), "osl_layer_weight Facing grazing == 1" );
			Check( Near( pFresnel.Eval( cGraze ), 1.0, 1e-5 ), "osl_fresnel grazing == 1" );

			// L2 memo separation for same hit position/normal with different incoming ray directions
			ExpressionProgram pLwMemo = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_layer_weight(0.5, vec3(0, 0, 0), 1) + 0.0 * perlin(P)", pLwMemo ), "memo-worthy osl_layer_weight compiles" );
			ExpressionPainter* epLw = new ExpressionPainter( pLwMemo, std::vector<ParamSpec>(), Scalar( 0 ) );
			ExpressionMemo::SetEnabledOverride( 1 );
			const RISEPel lwHead = epLw->GetColor( riHead );
			const RISEPel lwGraze = epLw->GetColor( riGraze );
			ExpressionMemo::SetEnabledOverride( -1 );
			Check( Near( lwHead[0], 0.0, 1e-5 ) && Near( lwGraze[0], 1.0, 1e-5 ),
				"L2 memo separates osl_layer_weight across different incoming ray directions" );
			epLw->release();
		}

		// 3. BlendedModifier + Job::AddBlendedModifier + blended_modifier chunk
		{
			struct FixedNormalModifier : public virtual IRayIntersectionModifier, public virtual Reference {
				Vector3 targetN;
				explicit FixedNormalModifier( const Vector3& n ) : targetN( Vector3Ops::Normalize( n ) ) {}
				void Modify( RayIntersectionGeometric& ri ) const override {
					ri.vNormal = targetN;
					ri.onb.CreateFromW( targetN );
				}
			};
			FixedNormalModifier* modX = new FixedNormalModifier( Vector3( 1, 0, 1 ) );
			FixedNormalModifier* modY = new FixedNormalModifier( Vector3( 0, 1, 1 ) );
			IScalarPainter* spHalf = 0;
			RISE_API_CreateUniformScalarPainter( &spHalf, Scalar( 0.5 ) );
			IRayIntersectionModifier* pBlend = 0;
			Check( RISE_API_CreateBlendedModifier( &pBlend, modX, modY, *spHalf ), "RISE_API_CreateBlendedModifier succeeds" );

			RayIntersectionGeometric riM( Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			riM.vNormal = Vector3( 0, 0, 1 );
			riM.onb.CreateFromWU( Vector3( 0, 0, 1 ), Vector3( 1, 0, 0 ) );
			riM.bShadingTangentFromGeometry = true;
			riM.bHasShadingTangent = true;
			pBlend->Modify( riM );
			const Vector3 expected = Vector3Ops::Normalize( Vector3Ops::Normalize( Vector3( 1, 0, 1 ) ) * 0.5 + Vector3Ops::Normalize( Vector3( 0, 1, 1 ) ) * 0.5 );
			Check( NearVec( riM.vNormal, expected, 1e-9 ), "BlendedModifier(0.5) matches normalized midpoint normal" );
			Check( riM.bHasCoatDecodeOnb && NearVec( riM.coatDecodeOnb.w(), Vector3( 0, 0, 1 ), 1e-9 ),
				"BlendedModifier records pre-modifier frame (DL-302)" );

			// Null modA blends unperturbed normal with modB
			IRayIntersectionModifier* pBlendNullA = 0;
			Check( RISE_API_CreateBlendedModifier( &pBlendNullA, 0, modX, *spHalf ), "BlendedModifier allows null modA" );
			RayIntersectionGeometric riM2( Ray( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			riM2.vNormal = Vector3( 0, 0, 1 );
			riM2.onb.CreateFromW( Vector3( 0, 0, 1 ) );
			pBlendNullA->Modify( riM2 );
			const Vector3 expected2 = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) * 0.5 + Vector3Ops::Normalize( Vector3( 1, 0, 1 ) ) * 0.5 );
			Check( NearVec( riM2.vNormal, expected2, 1e-9 ), "BlendedModifier(null, modX, 0.5) blends unperturbed normal" );

			// Both null rejected
			IRayIntersectionModifier* pBlendBad = 0;
			Check( !RISE_API_CreateBlendedModifier( &pBlendBad, 0, 0, *spHalf ), "BlendedModifier rejects both null" );

			pBlendNullA->release();
			pBlend->release();
			spHalf->release();
			modX->release();
			modY->release();
		}
	}
}

int main()
{
	TestCompileChecks();
	TestObjectWorldTransforms();
	TestCameraTransformsAndLiveMove();
	TestNullFallbacksAndNonFinite();
	TestMemoSoundness();
	TestSceneIntegration();
	TestObjectInfoBuiltins();
	TestPhase1Builtins();
	TestSampleUvAndAlpha();
	TestPhase3LayerWeightFresnelCameraRayAndBlendedModifier();

	std::cout << "OslSpaceTransformTest: " << g_pass << " passed, " << g_fail << " failed.\n";
	return g_fail == 0 ? 0 : 1;
}
