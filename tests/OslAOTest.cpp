//////////////////////////////////////////////////////////////////////
//
//  OslAOTest.cpp - Closed-form, parity, concurrency, and red-proof tests
//  for Slice 2: `osl_ao(distance, samples, normal, flags)` ray-traced
//  ambient occlusion (docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md Part III
//  & Round 3 P1-11/P2-4).
//
//  Tests:
//    1. Parse-time validation, SurfaceSignalDemand lifecycle, and
//       non-vacuous fallbacks on a LIVE occluded hit (dist<=0, NaN, +Inf,
//       zero custom N, and pHit==nullptr using ctx.N)
//    2. Closed-form parallel planes (h/R)^2 == 0.25 + large-coordinate
//       contact shadow (1000,1000,1000) + huge distance (1e39)
//    3. Closed-form 90-degree concave corner == 0.50
//    4. `inside = true` (flag 1) on convex box edge (worn_wooden_box motif)
//    5. `only_local`, `casts_shadows=false` (Round 3 P2-4), sibling
//       geometry instance, non-uniform scale, and CSGObject
//    6. Pre-modifier N0 parity after ReliefModifier (P1-1 red-proof)
//    7. Sampled-light FillEmitterRecord & BDPTVertex::LIGHT root facing
//       outward, not inverted into box interior (P1-2 red-proof)
//    8. Originating-triangle self-hit exclusion on smooth-shaded tilted
//       mesh & 53-deg tilted custom normal in open space (P1-3 red-proof)
//    9. Near-tangent custom normal, real single-sided back-face hit,
//       ExpressionPainter colour pipe + time + motion-blur TransformPoseEpoch
//   10. ReliefModifier neutrality (`osl_ao` flat; `P.x * osl_ao` gradient)
//   11. TLAS BVH4 (> 4 objects) + 8-thread bit-exact determinism stress test
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Modifiers/ReliefModifier.h"
#include "../src/Library/Utilities/ExpressionMemo.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void Check( bool cond, const char* label )
	{
		if( cond ) {
			++g_pass;
		} else {
			++g_fail;
			std::fprintf( stderr, "FAIL: %s\n", label );
		}
	}

	void CheckClose( double a, double b, double tol, const char* label )
	{
		if( std::fabs( a - b ) <= tol ) {
			++g_pass;
		} else {
			++g_fail;
			std::fprintf( stderr, "FAIL: %s (got %.9g, expected %.9g, tol %.3g)\n", label, a, b, tol );
		}
	}

	bool CompileExpr( const std::string& expr, bool ctxVars, ExpressionProgram& out )
	{
		ExpressionProgram::Builder b;
		b.EnableContextVars( ctxVars );
		return b.Finalize( expr, out );
	}

	TriangleMeshGeometryIndexed* MakePlaneQuad( Scalar z0, Scalar half, bool doubleSided = true )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( doubleSided, true );
		mesh->BeginIndexedTriangles();
		mesh->AddVertex( Point3( -half, -half, z0 ) );
		mesh->AddVertex( Point3(  half, -half, z0 ) );
		mesh->AddVertex( Point3(  half,  half, z0 ) );
		mesh->AddVertex( Point3( -half,  half, z0 ) );
		mesh->AddTexCoord( Point2( 0, 0 ) );
		mesh->AddTexCoord( Point2( 1, 0 ) );
		mesh->AddTexCoord( Point2( 1, 1 ) );
		mesh->AddTexCoord( Point2( 0, 1 ) );

		IndexedTriangle t0;
		t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
		t0.iNormals[0] = t0.iNormals[1] = t0.iNormals[2] = 0;
		t0.iCoords[0] = 0; t0.iCoords[1] = 1; t0.iCoords[2] = 2;
		mesh->AddIndexedTriangle( t0 );

		IndexedTriangle t1;
		t1.iVertices[0] = 0; t1.iVertices[1] = 2; t1.iVertices[2] = 3;
		t1.iNormals[0] = t1.iNormals[1] = t1.iNormals[2] = 0;
		t1.iCoords[0] = 0; t1.iCoords[1] = 2; t1.iCoords[2] = 3;
		mesh->AddIndexedTriangle( t1 );

		mesh->DoneIndexedTriangles();
		return mesh;
	}

	TriangleMeshGeometryIndexed* MakeVerticalQuadX( Scalar x0, Scalar half, Scalar zMin, Scalar zMax )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, true );
		mesh->BeginIndexedTriangles();
		mesh->AddVertex( Point3( x0, -half, zMin ) );
		mesh->AddVertex( Point3( x0,  half, zMin ) );
		mesh->AddVertex( Point3( x0,  half, zMax ) );
		mesh->AddVertex( Point3( x0, -half, zMax ) );
		mesh->AddTexCoord( Point2( 0, 0 ) );
		mesh->AddTexCoord( Point2( 1, 0 ) );
		mesh->AddTexCoord( Point2( 1, 1 ) );
		mesh->AddTexCoord( Point2( 0, 1 ) );

		IndexedTriangle t0;
		t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
		t0.iNormals[0] = t0.iNormals[1] = t0.iNormals[2] = 0;
		t0.iCoords[0] = 0; t0.iCoords[1] = 1; t0.iCoords[2] = 2;
		mesh->AddIndexedTriangle( t0 );

		IndexedTriangle t1;
		t1.iVertices[0] = 0; t1.iVertices[1] = 2; t1.iVertices[2] = 3;
		t1.iNormals[0] = t1.iNormals[1] = t1.iNormals[2] = 0;
		t1.iCoords[0] = 0; t1.iCoords[1] = 2; t1.iCoords[2] = 3;
		mesh->AddIndexedTriangle( t1 );

		mesh->DoneIndexedTriangles();
		return mesh;
	}

	void TestParseDemandAndLiveFallbacks()
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(0.5, 16, vec3(0,0,0), 0)", true, prog ), "osl_ao compiles with context vars" );
		Check( prog.ResultType() == ExpressionProgram::kScalar, "osl_ao returns scalar" );
		Check( prog.MemoWorthy(), "osl_ao is MemoWorthy" );
		Check( prog.UsesSurfaceSignals(), "osl_ao sets UsesSurfaceSignals" );
		Check( prog.UsesSpaceTransform(), "osl_ao sets UsesSpaceTransform" );
		Check( !prog.UsesCrossObject(), "osl_ao does not set UsesCrossObject" );

		// Context-vars disabled rejected; literal bounds enforced
		Check( !CompileExpr( "osl_ao(0.5, 16, vec3(0,0,0), 0)", false, prog ), "osl_ao rejected without context vars" );
		Check( !CompileExpr( "osl_ao(0.5, 0, vec3(0,0,0), 0)", true, prog ), "samples=0 rejected" );
		Check( !CompileExpr( "osl_ao(0.5, 129, vec3(0,0,0), 0)", true, prog ), "samples=129 rejected" );
		Check( !CompileExpr( "osl_ao(0.5, 16, vec3(0,0,0), -1)", true, prog ), "flags=-1 rejected" );
		Check( !CompileExpr( "osl_ao(0.5, 16, vec3(0,0,0), 8)", true, prog ), "flags=8 rejected" );
		Check( !CompileExpr( "osl_ao(0.5, 16 + u, vec3(0,0,0), 0)", true, prog ), "non-literal samples rejected" );
		Check( !CompileExpr( "osl_ao(0.5, 16, vec3(0,0,0), u)", true, prog ), "non-literal flags rejected" );

		// SurfaceSignalDemand lifecycle on BOTH ExpressionScalarPainter and ExpressionPainter
		Check( CompileExpr( "osl_ao(0.5, 16, vec3(0,0,0), 0)", true, prog ), "recompile valid osl_ao" );
		Check( !SurfaceSignalDemand::Any(), "SurfaceSignalDemand initially inactive" );
		{
			std::vector<ParamSpec> specs;
			ExpressionScalarPainter* sp = new ExpressionScalarPainter( prog, specs );
			Check( SurfaceSignalDemand::Any(), "ExpressionScalarPainter activates SurfaceSignalDemand" );
			Check( !sp->IsPositionIndependent(), "ExpressionScalarPainter with osl_ao is not position-independent" );
			Check( !sp->IsWorldPositionField(), "ExpressionScalarPainter with osl_ao is not world-position field" );
			sp->release();
		}
		Check( !SurfaceSignalDemand::Any(), "SurfaceSignalDemand deactivates after scalar painter release" );
		{
			std::vector<ParamSpec> specs;
			ExpressionPainter* ep = new ExpressionPainter( prog, specs, Scalar( 0 ) );
			Check( SurfaceSignalDemand::Any(), "ExpressionPainter activates SurfaceSignalDemand" );
			ep->release();
		}
		Check( !SurfaceSignalDemand::Any(), "SurfaceSignalDemand deactivates after color painter release" );

		// Non-vacuous fallbacks on a LIVE OCCLUDED hit (floor + ceiling at h=0.1 -> valid AO < 0.1!)
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 10.0 );
		TriangleMeshGeometryIndexed* ceilGeo  = MakePlaneQuad( 0.1, 10.0 );
		Object* floorObj = new Object( floorGeo );
		Object* ceilObj  = new Object( ceilGeo );
		om->AddItem( floorObj, "floor" );
		om->AddItem( ceilObj, "ceil" );
		om->PrepareForRendering();

		RayIntersection ri( Ray( Point3( 0, 0, 0.05 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		om->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit && ri.pObject == floorObj, "live occluded hit for fallbacks" );

		std::vector<ParamSpec> specs;
		ExpressionProgram pValid = ExpressionProgram::Invalid();
		ExpressionProgram pZeroD = ExpressionProgram::Invalid();
		ExpressionProgram pNegD  = ExpressionProgram::Invalid();
		ExpressionProgram pNaND  = ExpressionProgram::Invalid();
		ExpressionProgram pInfD  = ExpressionProgram::Invalid();
		ExpressionProgram pZeroN = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(1.0, 32, vec3(0,0,0), 0)", true, pValid ), "compile valid" );
		Check( CompileExpr( "osl_ao(0.0, 32, vec3(0,0,0), 0)", true, pZeroD ), "compile zero dist" );
		Check( CompileExpr( "osl_ao(-1.0, 32, vec3(0,0,0), 0)", true, pNegD ), "compile neg dist" );
		Check( CompileExpr( "osl_ao(log(-1.0), 32, vec3(0,0,0), 0)", true, pNaND ), "compile NaN dist" );
		Check( CompileExpr( "osl_ao(exp(1000.0), 32, vec3(0,0,0), 0)", true, pInfD ), "compile +Inf dist" );
		Check( CompileExpr( "osl_ao(1.0, 32, vec3(0,0,0), 4)", true, pZeroN ), "compile zero custom N" );

		ExpressionScalarPainter* spValid = new ExpressionScalarPainter( pValid, specs );
		ExpressionScalarPainter* spZeroD = new ExpressionScalarPainter( pZeroD, specs );
		ExpressionScalarPainter* spNegD  = new ExpressionScalarPainter( pNegD, specs );
		ExpressionScalarPainter* spNaND  = new ExpressionScalarPainter( pNaND, specs );
		ExpressionScalarPainter* spInfD  = new ExpressionScalarPainter( pInfD, specs );
		ExpressionScalarPainter* spZeroN = new ExpressionScalarPainter( pZeroN, specs );

		Check( spValid->GetValuesAt( ri.geometric ).v[0] < 0.1, "live baseline hit is heavily occluded (< 0.1)" );
		CheckClose( spZeroD->GetValuesAt( ri.geometric ).v[0], 1.0, 1e-9, "live hit: distance==0 returns 1.0" );
		CheckClose( spNegD->GetValuesAt( ri.geometric ).v[0],  1.0, 1e-9, "live hit: distance<0 returns 1.0" );
		CheckClose( spNaND->GetValuesAt( ri.geometric ).v[0],  1.0, 1e-9, "live hit: NaN distance returns 1.0" );
		CheckClose( spInfD->GetValuesAt( ri.geometric ).v[0],  1.0, 1e-9, "live hit: +Inf distance returns 1.0" );
		CheckClose( spZeroN->GetValuesAt( ri.geometric ).v[0], 1.0, 1e-9, "live hit: zero custom normal returns 1.0" );

		// Direct ExpressionProgram::Eval(ctx) with pHit == nullptr uses ctx.N!
		{
			ExprEvalContext ctx;
			ctx.N = Vector3( 0, 0, 1 );
			ctx.signals = ri.geometric.signals;
			ctx.pHit = nullptr;
			Check( pValid.Eval( ctx ) < 0.1, "Eval(ctx) with pHit==nullptr uses ctx.N and sees ceiling (< 0.1)" );
		}

		spValid->release(); spZeroD->release(); spNegD->release();
		spNaND->release(); spInfD->release(); spZeroN->release();
		floorObj->release(); ceilObj->release();
		floorGeo->release(); ceilGeo->release();
		om->release();
	}

	void TestClosedFormParallelPlanesAndExtremeScales()
	{
		// 1) Parallel planes: floor z=0, ceiling z=h=0.5, R=1.0 -> exact (h/R)^2 = 0.25
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 100.0 );
		TriangleMeshGeometryIndexed* ceilGeo  = MakePlaneQuad( 0.5, 100.0 );
		Object* floorObj = new Object( floorGeo );
		Object* ceilObj  = new Object( ceilGeo );
		om->AddItem( floorObj, "floor" );
		om->AddItem( ceilObj, "ceil" );
		om->PrepareForRendering();

		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(1.0, 64, vec3(0,0,0), 0)", true, prog ), "compile parallel planes AO" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* aoSp = new ExpressionScalarPainter( prog, specs );

		double sum = 0.0;
		const int gridN = 8;
		for( int ix = 0; ix < gridN; ++ix ) {
			for( int iy = 0; iy < gridN; ++iy ) {
				const Scalar x = -1.0 + 2.0 * ( ix + 0.5 ) / gridN;
				const Scalar y = -1.0 + 2.0 * ( iy + 0.5 ) / gridN;
				RayIntersection ri( Ray( Point3( x, y, 0.25 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
				om->IntersectRay( ri, true, true, false );
				Check( ri.geometric.bHit && ri.pObject == floorObj, "parallel plane hit floor" );
				sum += static_cast<double>( aoSp->GetValuesAt( ri.geometric ).v[0] );
			}
		}
		CheckClose( sum / ( gridN * gridN ), 0.25, 0.025, "parallel planes closed-form AO == (h/R)^2 == 0.25" );

		// Huge finite distance (1e39 > FLT_MAX) must not overflow BVH float bounds or produce NaN!
		ExpressionProgram progHuge = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(1e39, 32, vec3(0,0,0), 0)", true, progHuge ), "compile huge dist 1e39" );
		ExpressionScalarPainter* hugeSp = new ExpressionScalarPainter( progHuge, specs );
		{
			RayIntersection ri( Ray( Point3( 0, 0, 0.25 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			const Scalar v = hugeSp->GetValuesAt( ri.geometric ).v[0];
			Check( v >= Scalar( 0 ) && v < Scalar( 0.05 ), "huge distance 1e39 stays finite and sees ceiling (< 0.05)" );
		}

		hugeSp->release();
		aoSp->release();
		floorObj->release(); ceilObj->release();
		floorGeo->release(); ceilGeo->release();
		om->release();

		// 2) Large-coordinate scene (ptWorld = (1000, 1000, 1000)) with 5mm contact gap (0.005 m)
		//    and R = 50.0 (P2-1 red-proof: eps=1e-7*3001=0.0003 << 0.005, never teleports past 5mm gap!)
		{
			ObjectManager* omBig = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometryIndexed* fGeo = MakePlaneQuad( 0.0, 20.0 );
			TriangleMeshGeometryIndexed* cGeo = MakePlaneQuad( 0.005, 20.0 );
			Object* fObj = new Object( fGeo );
			Object* cObj = new Object( cGeo );
			fObj->SetPosition( Point3( 1000.0, 1000.0, 1000.0 ) ); fObj->FinalizeTransformations();
			cObj->SetPosition( Point3( 1000.0, 1000.0, 1000.0 ) ); cObj->FinalizeTransformations();
			omBig->AddItem( fObj, "f_big" );
			omBig->AddItem( cObj, "c_big" );
			omBig->PrepareForRendering();

			ExpressionProgram pBig = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_ao(50.0, 32, vec3(0,0,0), 0)", true, pBig ), "compile large-coord AO" );
			ExpressionScalarPainter* spBig = new ExpressionScalarPainter( pBig, specs );

			RayIntersection ri( Ray( Point3( 1000.0, 1000.0, 1000.002 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			omBig->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit && ri.pObject == fObj, "hit large-coord floor" );
			Check( spBig->GetValuesAt( ri.geometric ).v[0] < 0.05,
				"P2-1: large-coord (1000,1000,1000) AO does NOT jump over 5mm contact gap" );

			spBig->release();
			fObj->release(); cObj->release();
			fGeo->release(); cGeo->release();
			omBig->release();
		}
	}

	void TestClosedFormRightAngleCorner()
	{
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 100.0 );
		TriangleMeshGeometryIndexed* wallGeo  = MakeVerticalQuadX( 0.0, 100.0, 0.0, 100.0 );
		Object* floorObj = new Object( floorGeo );
		Object* wallObj  = new Object( wallGeo );
		om->AddItem( floorObj, "floor" );
		om->AddItem( wallObj, "wall" );
		om->PrepareForRendering();

		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(100.0, 64, vec3(0,0,0), 0)", true, prog ), "compile corner AO" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* aoSp = new ExpressionScalarPainter( prog, specs );

		double sum = 0.0;
		const int nPts = 64;
		for( int i = 0; i < nPts; ++i ) {
			const Scalar y = -5.0 + 10.0 * ( i + 0.5 ) / nPts;
			RayIntersection ri( Ray( Point3( 0.005, y, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit && ri.pObject == floorObj, "corner hit floor" );
			sum += static_cast<double>( aoSp->GetValuesAt( ri.geometric ).v[0] );
		}
		CheckClose( sum / nPts, 0.50, 0.025, "90-degree concave corner closed-form AO == 0.50" );

		aoSp->release();
		floorObj->release(); wallObj->release();
		floorGeo->release(); wallGeo->release();
		om->release();
	}

	void TestInsideOnlyLocalScaleAndCSG()
	{
		std::vector<ParamSpec> specs;

		// 1) Convex box edge inside=true (worn_wooden_box) + sibling instance sharing BoxGeometry
		{
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			BoxGeometry* boxGeo = new BoxGeometry( 1.0, 1.0, 1.0 );
			Object* boxA = new Object( boxGeo );
			boxA->SetPosition( Point3( 0.5, 0.5, 0.5 ) );
			boxA->FinalizeTransformations();
			// Sibling instance parked 0.05 m above boxA
			Object* boxB = new Object( boxGeo );
			boxB->SetPosition( Point3( 0.5, 0.5, 1.55 ) );
			boxB->FinalizeTransformations();
			om->AddItem( boxA, "boxA" );
			om->AddItem( boxB, "boxB" );
			om->PrepareForRendering();

			ExpressionProgram pOutScene = ExpressionProgram::Invalid();
			ExpressionProgram pOutLocal = ExpressionProgram::Invalid();
			ExpressionProgram pInLocal  = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_ao(0.5, 32, vec3(0,0,0), 0)", true, pOutScene ), "compile scene out" );
			Check( CompileExpr( "osl_ao(0.5, 32, vec3(0,0,0), 2)", true, pOutLocal ), "compile local out" );
			Check( CompileExpr( "osl_ao(0.07, 32, vec3(0,0,0), 3)", true, pInLocal ), "compile local inside" );
			ExpressionScalarPainter* spOutScene = new ExpressionScalarPainter( pOutScene, specs );
			ExpressionScalarPainter* spOutLocal = new ExpressionScalarPainter( pOutLocal, specs );
			ExpressionScalarPainter* spInLocal  = new ExpressionScalarPainter( pInLocal, specs );

			RayIntersection riCenter( Ray( Point3( 0.5, 0.5, 1.02 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( riCenter, true, true, false );
			Check( riCenter.geometric.bHit && riCenter.pObject == boxA, "hit boxA top center" );
			Check( spOutScene->GetValuesAt( riCenter.geometric ).v[0] < 0.1, "scene AO at center sees sibling instance boxB (< 0.1)" );
			CheckClose( spOutLocal->GetValuesAt( riCenter.geometric ).v[0], 1.0, 1e-9, "only_local ignores sibling instance sharing same geometry" );

			RayIntersection riEdge( Ray( Point3( 0.02, 0.5, 1.02 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( riEdge, true, true, false );
			Check( riEdge.geometric.bHit && riEdge.pObject == boxA, "hit boxA top edge" );
			// At x=0.02, boxB covers the +X half (>50% of hemisphere) while -X escapes
			Check( spOutScene->GetValuesAt( riEdge.geometric ).v[0] < 0.65, "scene AO at edge partially occluded by boxB (< 0.65)" );
			Check( spInLocal->GetValuesAt( riEdge.geometric ).v[0] < 0.75, "convex box edge inside+only_local darkens (< 0.75)" );

			// Round 3 P2-4: casts_shadows=false on boxA still self-occludes under only_local,
			// while casts_shadows=false on boxB makes scene-wide AO ignore boxB.
			boxA->SetShadowParams( false, true );
			boxB->SetShadowParams( false, true );
			ExpressionMemo::Invalidate();
			CheckClose( spOutScene->GetValuesAt( riCenter.geometric ).v[0], 1.0, 1e-9, "scene AO ignores casts_shadows=false boxB" );
			Check( spInLocal->GetValuesAt( riEdge.geometric ).v[0] < 0.75, "Round 3 P2-4: only_local ignores casts_shadows=false on self" );

			spOutScene->release(); spOutLocal->release(); spInLocal->release();
			boxA->release(); boxB->release();
			boxGeo->release();
			om->release();
		}

		// 2) Non-uniformly scaled Object (scale 10, 0.2, 1) with only_local + inside:
		//    unit box scaled to [0..10] x [0..0.2] x [0..1], hit at top face center (5, 0.1, 1.0):
		//    world distance to +/-Y walls is 0.10 m! With world R=0.25 -> occluded (< 0.7);
		//    with world R=0.05 -> unoccluded (== 1.0).
		{
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			BoxGeometry* boxGeo = new BoxGeometry( 1.0, 1.0, 1.0 );
			Object* scaledBox = new Object( boxGeo );
			scaledBox->SetStretch( Vector3( 10.0, 0.2, 1.0 ) );
			scaledBox->SetPosition( Point3( 5.0, 0.1, 0.5 ) );
			scaledBox->FinalizeTransformations();
			om->AddItem( scaledBox, "scaled_box" );
			om->PrepareForRendering();

			ExpressionProgram pLong  = ExpressionProgram::Invalid();
			ExpressionProgram pShort = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_ao(0.25, 32, vec3(0,0,0), 3)", true, pLong ), "compile scaled local inside 0.25" );
			Check( CompileExpr( "osl_ao(0.05, 32, vec3(0,0,0), 3)", true, pShort ), "compile scaled local inside 0.05" );
			ExpressionScalarPainter* spLong  = new ExpressionScalarPainter( pLong, specs );
			ExpressionScalarPainter* spShort = new ExpressionScalarPainter( pShort, specs );

			RayIntersection ri( Ray( Point3( 5.0, 0.1, 2.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit && ri.pObject == scaledBox, "hit non-uniformly scaled box" );
			Check( spLong->GetValuesAt( ri.geometric ).v[0] < 0.75, "non-uniform scale world R=0.25 hits 0.10m Y walls" );
			CheckClose( spShort->GetValuesAt( ri.geometric ).v[0], 1.0, 1e-9, "non-uniform scale world R=0.05 misses 0.10m Y walls" );

			spLong->release(); spShort->release();
			scaledBox->release(); boxGeo->release();
			om->release();
		}

		// 3) CSGObject union with only_local + inside
		{
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			BoxGeometry* bGeo = new BoxGeometry( 1.0, 1.0, 1.0 );
			Object* b1 = new Object( bGeo );
			b1->SetPosition( Point3( 0.5, 0.5, 0.5 ) ); b1->FinalizeTransformations();
			Object* b2 = new Object( bGeo );
			b2->SetPosition( Point3( 1.5, 0.5, 0.5 ) ); b2->FinalizeTransformations();
			CSGObject* csg = new CSGObject( CSG_UNION );
			csg->AssignObjects( b1, b2 );
			csg->FinalizeTransformations();
			om->AddItem( csg, "csg_box" );
			om->PrepareForRendering();

			ExpressionProgram pCsgIn = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_ao(0.1, 32, vec3(0,0,0), 3)", true, pCsgIn ), "compile CSG local inside" );
			ExpressionScalarPainter* spCsgIn = new ExpressionScalarPainter( pCsgIn, specs );

			RayIntersection ri( Ray( Point3( 0.02, 0.5, 2.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit && ri.pObject == csg, "hit CSG union near edge" );
			Check( spCsgIn->GetValuesAt( ri.geometric ).v[0] < 0.75, "CSGObject only_local+inside self-occludes near edge" );

			spCsgIn->release();
			csg->release(); b1->release(); b2->release(); bGeo->release();
			om->release();
		}
	}

	void TestPreModifierN0Parity()
	{
		// P1-1 red-proof: ReliefModifier perturbs ri.vNormal strongly toward -X (away from wall at +X),
		// while recording N0 in coatDecodeOnb. Default-normal osl_ao MUST still read N0=(0,0,1)
		// and return the EXACT same AO as before ReliefModifier ran!
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 10.0 );
		TriangleMeshGeometryIndexed* wallGeo  = MakeVerticalQuadX( 0.15, 10.0, 0.0, 2.0 );
		Object* floorObj = new Object( floorGeo );
		Object* wallObj  = new Object( wallGeo );
		om->AddItem( floorObj, "floor" );
		om->AddItem( wallObj, "wall" );
		om->PrepareForRendering();

		ExpressionProgram progAO = ExpressionProgram::Invalid();
		ExpressionProgram progRamp = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(1.0, 32, vec3(0,0,0), 0)", true, progAO ), "compile P1-1 AO" );
		Check( CompileExpr( "P.x", true, progRamp ), "compile linear height ramp" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* spAO   = new ExpressionScalarPainter( progAO, specs );
		ExpressionScalarPainter* spRamp = new ExpressionScalarPainter( progRamp, specs );
		ReliefModifier* relief = new ReliefModifier( *spRamp, Scalar( 2.0 ), ReliefDomain::Surface, Scalar( 0.01 ), Scalar( 10.0 ) );

		RayIntersection ri( Ray( Point3( 0.0, 0.0, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		om->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit && ri.pObject == floorObj, "P1-1 hit floor" );

		const Scalar aoBefore = spAO->GetValuesAt( ri.geometric ).v[0];
		RayIntersectionGeometric riMod = ri.geometric;
		relief->Modify( riMod );
		Check( riMod.vNormal.x < -0.5, "ReliefModifier tilted vNormal strongly toward -X" );
		Check( riMod.bHasCoatDecodeOnb, "ReliefModifier recorded pre-modifier frame" );

		const Scalar aoAfter = spAO->GetValuesAt( riMod ).v[0];
		CheckClose( aoAfter, aoBefore, 1e-9, "P1-1: default-normal osl_ao reads pre-modifier N0 identically after ReliefModifier" );

		relief->release();
		spAO->release(); spRamp->release();
		floorObj->release(); wallObj->release();
		floorGeo->release(); wallGeo->release();
		om->release();
	}

	void TestEmitterAndBDPTLightRootFacing()
	{
		// P1-2 red-proof: A closed unit box emitter in open space.
		// Direct hit, FillEmitterRecord (NEE/SampleLight), and BDPTVertex::LIGHT rebuild
		// MUST ALL trace AO OUTWARD (== 1.0), NEVER inverted into the box interior (0.0)!
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		BoxGeometry* boxGeo = new BoxGeometry( 0.4, 0.4, 0.4 );
		Object* boxObj = new Object( boxGeo );
		boxObj->SetPosition( Point3( 0.0, 0.0, 0.0 ) );
		boxObj->FinalizeTransformations();
		om->AddItem( boxObj, "box_emitter" );
		om->PrepareForRendering();

		ExpressionProgram progAO = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(2.0, 32, vec3(0,0,0), 0)", true, progAO ), "compile P1-2 emitter AO" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* spAO = new ExpressionScalarPainter( progAO, specs );

		// 1) Direct camera hit on top face (0, 0, 0.2)
		RayIntersection riDirect( Ray( Point3( 0.0, 0.0, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		om->IntersectRay( riDirect, true, true, false );
		Check( riDirect.geometric.bHit, "direct hit on box emitter" );
		CheckClose( spAO->GetValuesAt( riDirect.geometric ).v[0], 1.0, 1e-9, "direct hit on box emitter reads AO == 1.0" );

		// 2) LightSampler::FillEmitterRecord with OUTGOING ray (Dot(vGeomNormal, ray.Dir()) > 0!)
		{
			RayIntersectionGeometric rig( Ray( Point3( 0.0, 0.0, 0.2 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
			LightSampler::FillEmitterRecord( rig, Point3( 0.0, 0.0, 0.2 ), Vector3( 0, 0, 1 ),
				Point2( 0.5, 0.5 ), Point3( 0.0, 0.0, 0.2 ), boxObj, nullptr );
			rig.signals = riDirect.geometric.signals;
			Check( rig.GeomNormalOpposesArrival(), "P1-2: FillEmitterRecord stamps arrivalGeomFacing=-1" );
			CheckClose( spAO->GetValuesAt( rig ).v[0], 1.0, 1e-9,
				"P1-2: FillEmitterRecord (outgoing ray) traces AO OUTWARD == 1.0 (not 0.0 inside box)" );
		}

		// 3) BDPTVertex::LIGHT root rebuilt via PopulateRIGFromVertex
		{
			BDPTVertex v;
			v.type = BDPTVertex::LIGHT;
			v.position = Point3( 0.0, 0.0, 0.2 );
			v.normal = Vector3( 0, 0, 1 );
			v.geomNormal = Vector3( 0, 0, 1 );
			v.bGeomNormalOpposesArrival = true;
			v.onb.CreateFromW( v.normal );
			v.signals = riDirect.geometric.signals;

			RayIntersectionGeometric rebuilt( Ray( Point3( 0.0, 0.0, 0.2 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
			PathVertexEval::PopulateRIGFromVertex( v, rebuilt );
			CheckClose( spAO->GetValuesAt( rebuilt ).v[0], 1.0, 1e-9,
				"P1-2: BDPTVertex::LIGHT rebuild traces AO OUTWARD == 1.0" );
		}

		spAO->release();
		boxObj->release(); boxGeo->release();
		om->release();
	}

	void TestSelfPrimExclusionOnSmoothAndTiltedNormals()
	{
		// P1-3 red-proof: Isolated floor quad in completely empty space.
		// (A) 53-degree tilted custom normal vec3(-0.8, 0, 0.6): without primId exclusion,
		//     20% of rays dip below z=0 and self-hit the originating quad (AO ~ 0.80).
		//     With ScopedSelfPrimExclusion, AO MUST be EXACTLY 1.0!
		// (B) Smooth-shaded mesh triangle whose vertex normals tilt 35 degrees from face normal:
		//     with zero occluders, AO MUST be EXACTLY 1.0!
		std::vector<ParamSpec> specs;

		// (A) Tilted custom normal on isolated quad
		{
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 10.0 );
			Object* floorObj = new Object( floorGeo );
			om->AddItem( floorObj, "floor" );
			om->PrepareForRendering();

			ExpressionProgram pTilt = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_ao(2.0, 64, vec3(-0.8, 0.0, 0.6), 4)", true, pTilt ), "compile 53-deg custom N" );
			ExpressionScalarPainter* spTilt = new ExpressionScalarPainter( pTilt, specs );

			// Hit inside triangle 0 away from diagonal edge (x = 2.0, y = -2.0)
			RayIntersection ri( Ray( Point3( 2.0, -2.0, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit && ri.geometric.signals.primId == 0, "hit tri 0 interior" );
			CheckClose( spTilt->GetValuesAt( ri.geometric ).v[0], 1.0, 1e-9,
				"P1-3: 53-deg tilted custom normal in empty space reads EXACT 1.0 (zero originating-tri self-hits)" );

			spTilt->release();
			floorObj->release(); floorGeo->release();
			om->release();
		}

		// (B) Smooth-shaded single triangle with vertex normals tilted 35 degrees from face normal
		{
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, false );
			mesh->BeginIndexedTriangles();
			mesh->AddVertex( Point3( -5, -5, 0 ) );
			mesh->AddVertex( Point3(  5, -5, 0 ) );
			mesh->AddVertex( Point3(  0,  5, 0 ) );
			const Vector3 tiltedN = Vector3Ops::Normalize( Vector3( 0.7, 0.0, 1.0 ) ); // ~35 deg from +Z
			mesh->AddNormal( tiltedN );
			mesh->AddNormal( tiltedN );
			mesh->AddNormal( tiltedN );
			mesh->AddTexCoord( Point2( 0, 0 ) );
			mesh->AddTexCoord( Point2( 1, 0 ) );
			mesh->AddTexCoord( Point2( 0.5, 1 ) );
			IndexedTriangle t0;
			t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
			t0.iNormals[0]  = 0; t0.iNormals[1]  = 1; t0.iNormals[2]  = 2;
			t0.iCoords[0]   = 0; t0.iCoords[1]   = 1; t0.iCoords[2]   = 2;
			mesh->AddIndexedTriangle( t0 );
			mesh->DoneIndexedTriangles();

			Object* obj = new Object( mesh );
			om->AddItem( obj, "smooth_tri" );
			om->PrepareForRendering();

			ExpressionProgram pDef = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_ao(2.0, 64, vec3(0,0,0), 0)", true, pDef ), "compile smooth tri default N" );
			ExpressionScalarPainter* spDef = new ExpressionScalarPainter( pDef, specs );

			RayIntersection ri( Ray( Point3( 0, 0, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit, "hit smooth-shaded tri" );
			CheckClose( spDef->GetValuesAt( ri.geometric ).v[0], 1.0, 1e-9,
				"P1-3: smooth-shaded 35-deg tilted normal in empty space reads EXACT 1.0" );

			spDef->release();
			obj->release(); mesh->release();
			om->release();
		}

		// (C) Round 2 P1-1 red-proof: Non-indexed TriangleMeshGeometry with smooth-shaded
		//     35-deg tilted vertex normals AND 53-deg tilted custom normal in empty space.
		{
			ObjectManager* om = new ObjectManager( true, false, 4, 16 );
			TriangleMeshGeometry* rawMesh = new TriangleMeshGeometry( true );
			rawMesh->BeginTriangles();
			Triangle tri;
			tri.vertices[0] = Point3( -5, -5, 0 );
			tri.vertices[1] = Point3(  5, -5, 0 );
			tri.vertices[2] = Point3(  0,  5, 0 );
			const Vector3 tiltedN = Vector3Ops::Normalize( Vector3( 0.7, 0.0, 1.0 ) );
			tri.normals[0] = tiltedN;
			tri.normals[1] = tiltedN;
			tri.normals[2] = tiltedN;
			tri.coords[0] = Point2( 0, 0 );
			tri.coords[1] = Point2( 1, 0 );
			tri.coords[2] = Point2( 0.5, 1 );
			rawMesh->AddTriangle( tri );
			rawMesh->DoneTriangles();

			Object* obj = new Object( rawMesh );
			om->AddItem( obj, "raw_smooth_tri" );
			om->PrepareForRendering();

			ExpressionProgram pDef = ExpressionProgram::Invalid();
			ExpressionProgram pTilt = ExpressionProgram::Invalid();
			Check( CompileExpr( "osl_ao(2.0, 64, vec3(0,0,0), 0)", true, pDef ), "compile raw tri default N" );
			Check( CompileExpr( "osl_ao(2.0, 64, vec3(-0.8, 0.0, 0.6), 4)", true, pTilt ), "compile raw tri custom N" );
			ExpressionScalarPainter* spDef = new ExpressionScalarPainter( pDef, specs );
			ExpressionScalarPainter* spTilt = new ExpressionScalarPainter( pTilt, specs );

			RayIntersection ri( Ray( Point3( 0, 0, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			Check( ri.geometric.bHit && ri.geometric.signals.pProvider == 0 && ri.geometric.signals.primId == 0,
				"hit non-indexed TriangleMeshGeometry stamps primId==0, pProvider==nullptr" );
			CheckClose( spDef->GetValuesAt( ri.geometric ).v[0], 1.0, 1e-9,
				"R2 P1-1: non-indexed smooth-shaded 35-deg tilted normal in empty space == 1.0" );
			CheckClose( spTilt->GetValuesAt( ri.geometric ).v[0], 1.0, 1e-9,
				"R2 P1-1: non-indexed 53-deg tilted custom normal in empty space == 1.0" );

			spDef->release(); spTilt->release();
			obj->release(); rawMesh->release();
			om->release();
		}
	}

	void TestNearTangentSingleSidedColorPipeAndMotionBlur()
	{
		std::vector<ParamSpec> specs;
		ExpressionMemo::SetEnabledOverride( 1 );

		// 1) Near-tangent custom normal (|Dot(ng, customN)| <= 1e-3) + real single-sided back-face hit
		//    + motion-blur TransformPoseEpoch automatic memo invalidation + ExpressionPainter color pipe
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		// Single-sided floor at z=0 (geometric normal +Z)
		TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 10.0, /*doubleSided=*/false );
		TriangleMeshGeometryIndexed* ceilGeo  = MakePlaneQuad( 0.2, 10.0, true );
		Object* floorObj = new Object( floorGeo );
		Object* ceilObj  = new Object( ceilGeo );
		om->AddItem( floorObj, "floor_ss" );
		om->AddItem( ceilObj, "ceil" );
		om->PrepareForRendering();

		ExpressionProgram progDefault = ExpressionProgram::Invalid();
		ExpressionProgram progNearTan = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(1.0, 32, vec3(0,0,0), 0)", true, progDefault ), "compile default N" );
		Check( CompileExpr( "osl_ao(1.0, 32, vec3(1.0, 0.0, 0.0005), 4)", true, progNearTan ), "compile near-tangent custom N" );

		ExpressionPainter* epDef = new ExpressionPainter( progDefault, specs, Scalar( 0 ) );
		ExpressionScalarPainter* spNearTan = new ExpressionScalarPainter( progNearTan, specs );

		// Hit single-sided floor from +Z (front face) -> sees ceiling (< 0.2)
		RayIntersection riFront( Ray( Point3( 0, 0, 0.1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		om->IntersectRay( riFront, true, true, false );
		Check( riFront.geometric.bHit && riFront.pObject == floorObj, "hit single-sided floor front" );
		const RISEPel cFront = epDef->GetColor( riFront.geometric );
		Check( cFront[0] < 0.2 && cFront[1] == cFront[0] && cFront[2] == cFront[0],
			"ExpressionPainter color pipe front face sees ceiling (< 0.2)" );

		// Near-tangent custom normal (0.0005 < 1e-3): standoff uses arrival side (+Z), cleanly sees ceiling (~0.5 of hemi)
		const Scalar aoNearTan = spNearTan->GetValuesAt( riFront.geometric ).v[0];
		Check( aoNearTan > 0.3 && aoNearTan < 0.7, "P2-2: near-tangent custom normal has clean standoff (0.3..0.7)" );

		// Hit SINGLE-SIDED floor from -Z (real back face where Dot(vGeomNormal, ray.Dir()) > 0!)
		RayIntersection riBack( Ray( Point3( 0, 0, -0.5 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		om->IntersectRay( riBack, true, true, false );
		Check( riBack.geometric.bHit && riBack.pObject == floorObj, "hit single-sided floor back face" );
		Check( !riBack.geometric.GeomNormalOpposesArrival(), "single-sided back face has GeomNormalOpposesArrival()==false" );
		const RISEPel cBack = epDef->GetColor( riBack.geometric );
		CheckClose( cBack[0], 1.0, 1e-9, "single-sided back face flips default N to -Z open hemisphere (== 1.0)" );

		// Motion-blur / transform-pose epoch invalidation: move ceiling far away via FinalizeTransformations()
		// WITHOUT calling ExpressionMemo::Invalidate() manually!
		ceilObj->SetPosition( Point3( 0, 0, 50.0 ) );
		ceilObj->FinalizeTransformations();
		const RISEPel cAfterMove = epDef->GetColor( riFront.geometric );
		CheckClose( cAfterMove[0], 1.0, 1e-9,
			"TransformPoseEpoch automatically invalidates L2 + L1 AO caches when occluder moves" );

		ExpressionMemo::SetEnabledOverride( -1 );

		epDef->release();
		spNearTan->release();
		floorObj->release(); ceilObj->release();
		floorGeo->release(); ceilGeo->release();
		om->release();
	}

	void TestReliefModifierNeutrality()
	{
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 10.0 );
		TriangleMeshGeometryIndexed* wallGeo  = MakeVerticalQuadX( 0.3, 10.0, 0.0, 2.0 );
		Object* floorObj = new Object( floorGeo );
		Object* wallObj  = new Object( wallGeo );
		om->AddItem( floorObj, "floor" );
		om->AddItem( wallObj, "wall" );
		om->PrepareForRendering();

		ExpressionProgram progAO = ExpressionProgram::Invalid();
		ExpressionProgram progColorAO = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(1.0, 32, vec3(0,0,0), 0)", true, progAO ), "compile relief AO" );
		Check( CompileExpr( "P.x * osl_ao(1.0, 32, vec3(0,0,0), 0)", true, progColorAO ), "compile relief P.x * AO" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* spAO = new ExpressionScalarPainter( progAO, specs );
		ExpressionScalarPainter* spColorAO = new ExpressionScalarPainter( progColorAO, specs );

		ReliefModifier* relAO = new ReliefModifier( *spAO, Scalar( 1.0 ), ReliefDomain::Surface, Scalar( 0.02 ), Scalar( 10.0 ) );
		ReliefModifier* relColorAO = new ReliefModifier( *spColorAO, Scalar( 1.0 ), ReliefDomain::Surface, Scalar( 0.02 ), Scalar( 10.0 ) );

		RayIntersection ri( Ray( Point3( 0.0, 0.0, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		om->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit && ri.pObject == floorObj, "relief test hit floor" );

		const Scalar aoVal = spAO->GetValuesAt( ri.geometric ).v[0];
		Check( aoVal > 0.2 && aoVal < 0.99, "relief test point has partial occlusion" );

		RayIntersectionGeometric riFlat = ri.geometric;
		relAO->Modify( riFlat );
		CheckClose( riFlat.vNormal.x, 0.0, 1e-9, "osl_ao alone in ReliefModifier has zero Nx gradient" );
		CheckClose( riFlat.vNormal.y, 0.0, 1e-9, "osl_ao alone in ReliefModifier has zero Ny gradient" );
		CheckClose( riFlat.vNormal.z, 1.0, 1e-9, "osl_ao alone in ReliefModifier keeps Nz == 1" );

		RayIntersectionGeometric riGrad = ri.geometric;
		relColorAO->Modify( riGrad );
		const double expectedNx = -static_cast<double>( aoVal ) / std::sqrt( 1.0 + static_cast<double>( aoVal * aoVal ) );
		CheckClose( riGrad.vNormal.x, expectedNx, 1e-5, "P.x * osl_ao in ReliefModifier preserves exact Color*AO gradient" );
		CheckClose( riGrad.vNormal.y, 0.0, 1e-5, "P.x * osl_ao Ny == 0" );

		relAO->release(); relColorAO->release();
		spAO->release(); spColorAO->release();
		floorObj->release(); wallObj->release();
		floorGeo->release(); wallGeo->release();
		om->release();
	}

	void TestBVH4MultiThreadedDeterminism()
	{
		// Build a scene with 10 objects (> nMaxObjectsPerNode=4) so TLAS BVH4 is genuinely traversed!
		ObjectManager* om = new ObjectManager( true, false, 4, 16 );
		TriangleMeshGeometryIndexed* floorGeo = MakePlaneQuad( 0.0, 20.0 );
		Object* floorObj = new Object( floorGeo );
		om->AddItem( floorObj, "bvh_floor" );

		std::vector<BoxGeometry*> boxGeos;
		std::vector<Object*> boxObjs;
		for( int i = 0; i < 9; ++i ) {
			BoxGeometry* bg = new BoxGeometry( 0.3, 0.3, 0.6 );
			Object* bo = new Object( bg );
			const Scalar bx = -1.2 + 0.3 * i;
			const Scalar by = ( ( i % 3 ) - 1 ) * 0.45;
			bo->SetPosition( Point3( bx, by, 0.3 ) );
			bo->FinalizeTransformations();
			const std::string nm = "pillar_" + std::to_string( i );
			om->AddItem( bo, nm.c_str() );
			boxGeos.push_back( bg );
			boxObjs.push_back( bo );
		}
		om->PrepareForRendering();

		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileExpr( "osl_ao(1.0, 32, vec3(0,0,0), 0)", true, prog ), "compile BVH4 MT AO" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* sp = new ExpressionScalarPainter( prog, specs );

		// Collect 32 floor hits and compute single-threaded reference
		std::vector<RayIntersectionGeometric> hits;
		std::vector<Scalar> refVals;
		for( int i = 0; i < 32; ++i ) {
			const Scalar x = -1.5 + 3.0 * ( i + 0.5 ) / 32.0;
			const Scalar y = -0.8 + 1.6 * ( ( i * 7 ) % 32 + 0.5 ) / 32.0;
			RayIntersection ri( Ray( Point3( x, y, 2.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			om->IntersectRay( ri, true, true, false );
			if( ri.geometric.bHit && ri.pObject == floorObj ) {
				hits.push_back( ri.geometric );
				refVals.push_back( sp->GetValuesAt( ri.geometric ).v[0] );
			}
		}
		Check( hits.size() >= 16, "BVH4 scene produced >= 16 floor hits" );

		// Run 8 concurrent worker threads evaluating all hits and assert BIT-EXACT agreement!
		const int nThreads = 8;
		std::vector<int> mismatches( nThreads, 0 );
		std::vector<std::thread> threads;
		for( int t = 0; t < nThreads; ++t ) {
			threads.emplace_back( [&, t]() {
				for( int rep = 0; rep < 4; ++rep ) {
					for( std::size_t k = 0; k < hits.size(); ++k ) {
						const Scalar v = sp->GetValuesAt( hits[k] ).v[0];
						if( v != refVals[k] ) {
							++mismatches[t];
						}
					}
				}
			} );
		}
		for( auto& th : threads ) th.join();
		int totalMismatch = 0;
		for( int m : mismatches ) totalMismatch += m;
		Check( totalMismatch == 0, "8-thread BVH4 concurrent osl_ao is bit-exact with single-thread reference" );

		sp->release();
		floorObj->release(); floorGeo->release();
		for( Object* bo : boxObjs ) bo->release();
		for( BoxGeometry* bg : boxGeos ) bg->release();
		om->release();
	}
}

int main()
{
	TestParseDemandAndLiveFallbacks();
	TestClosedFormParallelPlanesAndExtremeScales();
	TestClosedFormRightAngleCorner();
	TestInsideOnlyLocalScaleAndCSG();
	TestPreModifierN0Parity();
	TestEmitterAndBDPTLightRootFacing();
	TestSelfPrimExclusionOnSmoothAndTiltedNormals();
	TestNearTangentSingleSidedColorPipeAndMotionBlur();
	TestReliefModifierNeutrality();
	TestBVH4MultiThreadedDeterminism();

	std::printf( "OslAOTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
