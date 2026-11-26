//////////////////////////////////////////////////////////////////////
//
//  NamedMeshAttributeTest.cpp - Unit and integration tests for Slice 1:
//    - ITriangleMeshGeometryIndexed5 named per-corner RGBA attributes
//    - ISurfaceSignalProvider::EvaluateNamedAttribute (+ ReliefModifier
//      object-space triangle-plane re-projection via vReliefObjStep)
//    - ExpressionEval VM `mesh_attr3("name")` & `mesh_attr_alpha("name")`
//      (including \xHH / \t escapes and .RISEscene CST round-trip)
//    - Object::IntersectRay, MappingPainter(Proj_Object), collinear/sliver
//      triangles, non-indexed mesh & SDF fallbacks
//    - BDPTVertex / PathVertexEval rebuild & L2 ExpressionMemo separation
//    - PainterRGBScalarPainter (`RISE_API_CreatePainterRGBScalarPainter`,
//      `scalar_painter { painter ... channel RGB }`, ReadsWorldPosition,
//      ConnectionLegality requireSingle enforcement)
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Painters/MappingPainter.h"
#include "../src/Library/Painters/PainterChannelScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Modifiers/ReliefModifier.h"
#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/ConnectionLegality.h"
#include "../src/Library/Interfaces/IJob.h"

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

	bool CompileExpr( const std::string& expr, bool ctxVars, ExpressionProgram& out, std::string* errOut = nullptr )
	{
		ExpressionProgram::Builder b;
		b.EnableContextVars( ctxVars );
		const bool ok = b.Finalize( expr, out );
		if( !ok && errOut ) {
			*errOut = out.Error();
		}
		return ok;
	}

	struct ScopedTempFile
	{
		std::string path;
		explicit ScopedTempFile( const std::string& content, const char* tag )
		{
			static unsigned int s_seq = 0;
			const auto stamp = static_cast<unsigned long long>(
				std::chrono::steady_clock::now().time_since_epoch().count() );
			const std::string fname = std::string( "rise_nma_" ) + tag + "_" +
				std::to_string( stamp ) + "_" + std::to_string( ++s_seq ) + ".RISEscene";
			path = ( std::filesystem::temp_directory_path() / fname ).string();
			FILE* f = std::fopen( path.c_str(), "wb" );
			if( f ) {
				std::fwrite( content.data(), 1, content.size(), f );
				std::fclose( f );
			}
		}
		~ScopedTempFile()
		{
			if( !path.empty() ) {
				std::remove( path.c_str() );
			}
		}
	};

	// Build a unit quad on z=0: (0,0,0), (1,0,0), (1,1,0), (0,1,0)
	// Tri 0: (0, 1, 2)  Tri 1: (0, 2, 3)
	TriangleMeshGeometryIndexed* MakeQuadMesh()
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, true );
		mesh->BeginIndexedTriangles();
		mesh->AddVertex( Point3( 0, 0, 0 ) );
		mesh->AddVertex( Point3( 1, 0, 0 ) );
		mesh->AddVertex( Point3( 1, 1, 0 ) );
		mesh->AddVertex( Point3( 0, 1, 0 ) );
		mesh->AddTexCoord( Point2( 0, 0 ) );
		mesh->AddTexCoord( Point2( 1, 0 ) );
		mesh->AddTexCoord( Point2( 1, 1 ) );
		mesh->AddTexCoord( Point2( 0, 1 ) );

		IndexedTriangle t0;
		t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
		t0.iNormals[0]  = 0; t0.iNormals[1]  = 0; t0.iNormals[2]  = 0;
		t0.iCoords[0]   = 0; t0.iCoords[1]   = 1; t0.iCoords[2]   = 2;
		mesh->AddIndexedTriangle( t0 );

		IndexedTriangle t1;
		t1.iVertices[0] = 0; t1.iVertices[1] = 2; t1.iVertices[2] = 3;
		t1.iNormals[0]  = 0; t1.iNormals[1]  = 0; t1.iNormals[2]  = 0;
		t1.iCoords[0]   = 0; t1.iCoords[1]   = 2; t1.iCoords[2]   = 3;
		mesh->AddIndexedTriangle( t1 );

		mesh->DoneIndexedTriangles();
		return mesh;
	}

	// Minimal dummy ISurfaceSignalProvider that inherits the default
	// EvaluateNamedAttribute() -> false (same contract as SDFGeometry / SkeletonGeometry).
	class DummySignalProvider : public ISurfaceSignalProvider
	{
	public:
		bool ComputeOcclusion( const SurfaceSignalInfo&, const Scalar, const bool, Scalar& ) const override { return false; }
		bool ComputeThickness( const SurfaceSignalInfo&, const Scalar, const bool, Scalar& ) const override { return false; }
		bool ComputeConvexity( const SurfaceSignalInfo&, const Scalar, const bool, Scalar& ) const override { return false; }
	};

	void TestParserAndTokenizer()
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		std::string err;

		// Basic compilation and return types
		Check( CompileExpr( "mesh_attr3(\"distance\")", true, prog, &err ), "parse mesh_attr3" );
		Check( prog.ResultType() == ExpressionProgram::kVec3, "mesh_attr3 returns vec3" );
		Check( prog.UsesMeshAttributes(), "mesh_attr3 sets UsesMeshAttributes" );
		Check( prog.UsesSurfaceSignals(), "mesh_attr3 sets UsesSurfaceSignals" );
		Check( !prog.UsesCrossObject(), "mesh_attr3 does not set UsesCrossObject" );
		Check( prog.MeshAttributeNames().size() == 1 && prog.MeshAttributeNames()[0] == "distance", "mesh_attr3 records attr name" );

		Check( CompileExpr( "mesh_attr_alpha(\"distance\")", true, prog, &err ), "parse mesh_attr_alpha" );
		Check( prog.ResultType() == ExpressionProgram::kScalar, "mesh_attr_alpha returns scalar" );
		Check( prog.UsesMeshAttributes(), "mesh_attr_alpha sets UsesMeshAttributes" );
		Check( prog.UsesSurfaceSignals(), "mesh_attr_alpha sets UsesSurfaceSignals" );

		// Escaped quotes, backslashes, tabs, and \xHH hex escapes
		Check( CompileExpr( "mesh_attr3(\"a\\\"b\\\\c\\td\")", true, prog, &err ), "parse escaped string literal" );
		Check( prog.MeshAttributeNames().size() == 1 && prog.MeshAttributeNames()[0] == "a\"b\\c\td", "unescaped string literal matches" );

		Check( CompileExpr( "mesh_attr3(\"UV\\x231\\x20\\x20\\x7b0\\x7d\\x2f*x\")", true, prog, &err ), "parse \\xHH hex escapes" );
		Check( prog.MeshAttributeNames().size() == 1 && prog.MeshAttributeNames()[0] == "UV#1  {0}/*x", "\\xHH decodes CST-hostile chars exactly" );

		// Reject invalid \xHH forms
		Check( !CompileExpr( "mesh_attr3(\"bad\\x00nul\")", true, prog, &err ), "reject \\x00 NUL byte" );
		Check( !CompileExpr( "mesh_attr3(\"bad\\xZZ\")", true, prog, &err ), "reject non-hex \\xZZ" );
		Check( !CompileExpr( "mesh_attr3(\"bad\\x2\")", true, prog, &err ), "reject truncated \\x2" );

		// Deduplication across calls and def stages
		{
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			Check( b.AddDef( "c", "mesh_attr3(\"col\")" ), "def with mesh_attr3" );
			Check( b.Finalize( "c + vec3(mesh_attr_alpha(\"col\"), mesh_attr_alpha(\"other\"), 0)", prog ), "dedupe mesh attr names" );
			Check( prog.MeshAttributeNames().size() == 2, "deduplicated attr count == 2" );
			Check( prog.MeshAttributeNames()[0] == "col" && prog.MeshAttributeNames()[1] == "other", "attr order preserved" );
		}

		// Context-vars disabled (expression_function2d) rejected
		Check( !CompileExpr( "mesh_attr3(\"distance\")", false, prog, &err ), "mesh_attr3 rejected without context vars" );
		Check( !CompileExpr( "mesh_attr_alpha(\"distance\")", false, prog, &err ), "mesh_attr_alpha rejected without context vars" );

		// Error cases
		Check( !CompileExpr( "mesh_attr3(\"\")", true, prog, &err ), "empty attribute name rejected" );
		Check( !CompileExpr( "mesh_attr3(distance)", true, prog, &err ), "unquoted identifier rejected" );
		Check( !CompileExpr( "mesh_attr3(\"unterminated)", true, prog, &err ), "unterminated string rejected" );
		Check( !CompileExpr( "mesh_attr3(\"bad\\nescape\")", true, prog, &err ), "unknown escape rejected" );
		Check( !CompileExpr( "mesh_attr3(\"a\", 1)", true, prog, &err ), "extra argument rejected" );
		Check( !CompileExpr( "\"bare_string\"", true, prog, &err ), "bare string literal rejected" );

		// ExpressionScalarPainter position-independence & graded-medium checks
		Check( CompileExpr( "mesh_attr_alpha(\"distance\")", true, prog, &err ), "compile scalar mesh_attr_alpha" );
		{
			std::vector<ParamSpec> specs;
			ExpressionScalarPainter* sp = new ExpressionScalarPainter( prog, specs );
			Check( !sp->IsPositionIndependent(), "ExpressionScalarPainter with mesh_attr_alpha is not position-independent" );
			Check( !sp->IsWorldPositionField(), "ExpressionScalarPainter with mesh_attr_alpha is not world-position field" );
			sp->release();
		}
	}

	void TestPerCornerSeamValidationAndSanitization()
	{
		TriangleMeshGeometryIndexed* mesh = MakeQuadMesh();
		ITriangleMeshGeometryIndexed5* m5 = dynamic_cast<ITriangleMeshGeometryIndexed5*>( mesh );
		Check( m5 != nullptr, "TriangleMeshGeometryIndexed implements ITriangleMeshGeometryIndexed5" );

		// Reject invalid calls (P1-3 / P2-1)
		Check( !m5->AddNamedCornerAttribute( nullptr, nullptr, 0 ), "reject null name/data" );
		Check( !m5->AddNamedCornerAttribute( "", nullptr, 6 ), "reject empty name" );
		float dummy[36] = {};
		Check( !m5->AddNamedCornerAttribute( "x", dummy, 0 ), "reject zero corners" );
		Check( !m5->AddNamedCornerAttribute( "x", dummy, 5 ), "reject non-multiple-of-3 corners" );
		Check( !m5->AddNamedCornerAttribute( "x", dummy, 3 ), "reject post-finalize short corner count (3 != 6)" );
		Check( !m5->AddNamedCornerAttribute( "x", dummy, 9 ), "reject post-finalize oversized corner count (9 != 6)" );
		Check( m5->numNamedCornerAttributes() == 0, "rejected calls leave count == 0" );

		// Tri 0: v0=(0,0,0), v1=(1,0,0), v2=(1,1,0) -> distinct RGB & alpha per corner
		// Tri 1: v0=(0,0,0), v1=(1,1,0), v2=(0,1,0) -> constant (0.5, 0.5, 0.5, 0.9) even at shared v0/v2!
		const float colRGBA[24] = {
			1.0f, 0.0f, 0.0f, 0.2f,
			0.0f, 1.0f, 0.0f, 0.4f,
			0.0f, 0.0f, 1.0f, 0.6f,
			0.5f, 0.5f, 0.5f, 0.9f,
			0.5f, 0.5f, 0.5f, 0.9f,
			0.5f, 0.5f, 0.5f, 0.9f,
		};
		Check( m5->AddNamedCornerAttribute( "col", colRGBA, 6 ), "attach per-corner attr 'col'" );
		Check( m5->numNamedCornerAttributes() == 1, "numNamedCornerAttributes == 1" );

		// Non-finite sanitization test
		const float nanVal = std::numeric_limits<float>::quiet_NaN();
		const float infVal = std::numeric_limits<float>::infinity();
		const float badRGBA[24] = {
			nanVal, 0.25f, infVal, 0.75f,
			0.0f,   0.25f, 0.0f,   0.75f,
			0.0f,   0.25f, 0.0f,   0.75f,
			0.0f,   0.0f,  0.0f,   0.0f,
			0.0f,   0.0f,  0.0f,   0.0f,
			0.0f,   0.0f,  0.0f,   0.0f,
		};
		Check( m5->AddNamedCornerAttribute( "bad", badRGBA, 6 ), "attach attr with NaN/Inf" );
		Check( m5->numNamedCornerAttributes() == 2, "numNamedCornerAttributes == 2" );

		// Direct mesh->IntersectRay WITHOUT manually patching ri.ptObjIntersec (P1-1 proof!)
		RayIntersectionGeometric ri0( Ray( Point3( 0.7, 0.2, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		mesh->IntersectRay( ri0, true, true, false );
		Check( ri0.bHit && ri0.signals.primId == 0, "hit tri 0" );

		ExpressionProgram progCol = ExpressionProgram::Invalid();
		ExpressionProgram progAlpha = ExpressionProgram::Invalid();
		ExpressionProgram progMissing = ExpressionProgram::Invalid();
		ExpressionProgram progBad = ExpressionProgram::Invalid();
		Check( CompileExpr( "mesh_attr3(\"col\")", true, progCol ), "compile col" );
		Check( CompileExpr( "mesh_attr_alpha(\"col\")", true, progAlpha ), "compile alpha" );
		Check( CompileExpr( "mesh_attr3(\"missing\")", true, progMissing ), "compile missing" );
		Check( CompileExpr( "mesh_attr3(\"bad\")", true, progBad ), "compile bad" );

		// Even with ptObjIntersec == (0,0,0) != signals.ptObject, vReliefObjStep == 0 so exact barys are used!
		ExprEvalContext ctx0;
		ctx0.Po = Vector3( 0, 0, 0 );
		ctx0.signals = ri0.signals;
		ctx0.pHit = &ri0;
		const Vector3 c0 = progCol.EvalVec3( ctx0 );
		const Scalar a0 = progAlpha.Eval( ctx0 );
		CheckClose( c0.x, 0.3, 1e-6, "tri 0 R == 1-a-b == 0.3 (even when ptObjIntersec=(0,0,0))" );
		CheckClose( c0.y, 0.5, 1e-6, "tri 0 G == a == 0.5" );
		CheckClose( c0.z, 0.2, 1e-6, "tri 0 B == b == 0.2" );
		CheckClose( a0, 0.3*0.2 + 0.5*0.4 + 0.2*0.6, 1e-6, "tri 0 alpha interpolated" );

		// Missing attribute -> 0
		const Vector3 cm = progMissing.EvalVec3( ctx0 );
		CheckClose( cm.x, 0.0, 1e-9, "missing attr R == 0" );
		CheckClose( cm.y, 0.0, 1e-9, "missing attr G == 0" );
		CheckClose( cm.z, 0.0, 1e-9, "missing attr B == 0" );

		// Sanitized NaN/Inf -> 0 while finite green stays 0.25
		const Vector3 cb = progBad.EvalVec3( ctx0 );
		CheckClose( cb.x, 0.0, 1e-9, "NaN corner sanitized to 0" );
		CheckClose( cb.y, 0.25, 1e-6, "finite channel preserved beside NaN" );
		CheckClose( cb.z, 0.0, 1e-9, "Inf corner sanitized to 0" );

		// Non-finite hit.baryA/baryB rejected directly inside provider (P2-5)
		{
			SurfaceSignalInfo badHit = ri0.signals;
			badHit.baryA = std::numeric_limits<Scalar>::quiet_NaN();
			Scalar out[4] = { 1, 1, 1, 1 };
			Check( !mesh->EvaluateNamedAttribute( badHit, "col", out, nullptr ), "provider rejects NaN baryA" );
		}

		// Intersect Tri 1 at (0.2, 0.7, 0) -> proves sharp seam across shared vertices
		RayIntersectionGeometric ri1( Ray( Point3( 0.2, 0.7, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		mesh->IntersectRay( ri1, true, true, false );
		Check( ri1.bHit && ri1.signals.primId == 1, "hit tri 1" );

		ExprEvalContext ctx1;
		ctx1.signals = ri1.signals;
		ctx1.pHit = &ri1;
		const Vector3 c1 = progCol.EvalVec3( ctx1 );
		const Scalar a1 = progAlpha.Eval( ctx1 );
		CheckClose( c1.x, 0.5, 1e-6, "tri 1 seam R == 0.5" );
		CheckClose( c1.y, 0.5, 1e-6, "tri 1 seam G == 0.5" );
		CheckClose( c1.z, 0.5, 1e-6, "tri 1 seam B == 0.5" );
		CheckClose( a1, 0.9, 1e-6, "tri 1 seam alpha == 0.9" );

		// Overwrite invalidates L2 ExpressionMemo (with memo explicitly forced ON!)
		ExpressionMemo::SetEnabledOverride( 1 );
		{
			ExpressionProgram memoProg = ExpressionProgram::Invalid();
			Check( CompileExpr( "mesh_attr3(\"col\") + vec3(perlin(Po)*0.0, 0, 0)", true, memoProg ), "compile memo prog" );
			Check( memoProg.MemoWorthy(), "perlin+mesh_attr3 is MemoWorthy" );
			std::vector<ParamSpec> specs;
			ExpressionPainter* ep = new ExpressionPainter( memoProg, specs, Scalar( 0 ) );
			const RISEPel before = ep->GetColor( ri0 );
			CheckClose( before[0], 0.3, 1e-6, "memoed pre-overwrite R == 0.3" );

			const float colRGBA2[24] = {
				0.2f, 0.3f, 0.4f, 0.8f,
				0.2f, 0.3f, 0.4f, 0.8f,
				0.2f, 0.3f, 0.4f, 0.8f,
				0.2f, 0.3f, 0.4f, 0.8f,
				0.2f, 0.3f, 0.4f, 0.8f,
				0.2f, 0.3f, 0.4f, 0.8f,
			};
			Check( m5->AddNamedCornerAttribute( "col", colRGBA2, 6 ), "overwrite attr 'col'" );
			Check( m5->numNamedCornerAttributes() == 2, "overwrite keeps count == 2" );
			const RISEPel after = ep->GetColor( ri0 );
			CheckClose( after[0], 0.2, 1e-6, "memo invalidated on overwrite R == 0.2" );
			CheckClose( after[1], 0.3, 1e-6, "memo invalidated on overwrite G == 0.3" );
			CheckClose( after[2], 0.4, 1e-6, "memo invalidated on overwrite B == 0.4" );
			ep->release();
		}
		ExpressionMemo::SetEnabledOverride( -1 );

		// BeginIndexedTriangles clears named corner attributes
		mesh->BeginIndexedTriangles();
		Check( m5->numNamedCornerAttributes() == 0, "BeginIndexedTriangles clears named attributes" );

		mesh->release();
	}

	void TestPreFinalizeMismatchAndRejectedTriangleCompaction()
	{
		// Case 1: rejected == 0, pre-attached attribute with wrong corner count MUST be dropped in DoneIndexedTriangles
		{
			TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, true );
			mesh->BeginIndexedTriangles();
			mesh->AddVertex( Point3( 0, 0, 0 ) );
			mesh->AddVertex( Point3( 1, 0, 0 ) );
			mesh->AddVertex( Point3( 0, 1, 0 ) );
			IndexedTriangle t0;
			t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
			t0.iNormals[0] = t0.iNormals[1] = t0.iNormals[2] = 0;
			t0.iCoords[0]  = t0.iCoords[1]  = t0.iCoords[2]  = 0;
			mesh->AddIndexedTriangle( t0 );
			const float wrongSize[24] = {}; // 6 corners for 1 triangle (expected 3)
			Check( mesh->AddNamedCornerAttribute( "mismatch", wrongSize, 6 ), "pre-finalize attach accepts provisional count" );
			mesh->DoneIndexedTriangles();
			Check( mesh->numNamedCornerAttributes() == 0, "DoneIndexedTriangles drops mismatched attribute when rejected==0" );
			mesh->release();
		}

		// Case 2: rejected > 0 compacts surviving triangle attributes
		{
			TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, true );
			mesh->BeginIndexedTriangles();
			mesh->AddVertex( Point3( 0, 0, 0 ) );
			mesh->AddVertex( Point3( 1, 0, 0 ) );
			mesh->AddVertex( Point3( 0, 1, 0 ) );
			mesh->AddVertex( Point3( 2, 0, 0 ) );
			mesh->AddVertex( Point3( 3, 0, 0 ) );
			mesh->AddVertex( Point3( 2, 1, 0 ) );

			IndexedTriangle t0;
			t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
			t0.iNormals[0] = t0.iNormals[1] = t0.iNormals[2] = 0;
			t0.iCoords[0]  = t0.iCoords[1]  = t0.iCoords[2]  = 0;
			mesh->AddIndexedTriangle( t0 );

			IndexedTriangle tBad;
			tBad.iVertices[0] = 0; tBad.iVertices[1] = 99; tBad.iVertices[2] = 2;
			tBad.iNormals[0] = tBad.iNormals[1] = tBad.iNormals[2] = 0;
			tBad.iCoords[0]  = tBad.iCoords[1]  = tBad.iCoords[2]  = 0;
			mesh->AddIndexedTriangle( tBad );

			IndexedTriangle t2;
			t2.iVertices[0] = 3; t2.iVertices[1] = 4; t2.iVertices[2] = 5;
			t2.iNormals[0] = t2.iNormals[1] = t2.iNormals[2] = 0;
			t2.iCoords[0]  = t2.iCoords[1]  = t2.iCoords[2]  = 0;
			mesh->AddIndexedTriangle( t2 );

			const float attrData[36] = {
				0.1f, 0, 0, 1,  0.1f, 0, 0, 1,  0.1f, 0, 0, 1,
				0.5f, 0, 0, 1,  0.5f, 0, 0, 1,  0.5f, 0, 0, 1,
				0.9f, 0, 0, 1,  0.9f, 0, 0, 1,  0.9f, 0, 0, 1,
			};
			Check( mesh->AddNamedCornerAttribute( "tag", attrData, 9 ), "pre-finalize attach" );
			mesh->DoneIndexedTriangles();

			// Post-finalize attach with uncompacted count (9 != 6) MUST now be rejected!
			Check( !mesh->AddNamedCornerAttribute( "late_uncompacted", attrData, 9 ),
				"post-finalize attach rejects uncompacted count 9 != 6" );

			RayIntersectionGeometric ri( Ray( Point3( 2.2, 0.2, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			mesh->IntersectRay( ri, true, true, false );
			Check( ri.bHit && ri.signals.primId == 1, "hit surviving tri 2 at primId 1" );
			Scalar rgba[4] = { 0, 0, 0, 0 };
			Check( ri.signals.NamedAttribute( "tag", rgba ), "eval compacted attr" );
			CheckClose( rgba[0], 0.9, 1e-6, "compacted primId 1 reads original tri 2 value 0.9" );

			mesh->release();
		}
	}

	void TestSliverTriangleGuard()
	{
		// P1-2 red-proof: strictly collinear / near-collinear needle triangle must NOT emit ~1e12 garbage under pQueryObjPos
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( true, true );
		mesh->BeginIndexedTriangles();
		mesh->AddVertex( Point3( 0.0, 0.0, 0.0 ) );
		mesh->AddVertex( Point3( 1.0, 0.0, 0.0 ) );
		mesh->AddVertex( Point3( 2.0, 1e-9, 0.0 ) ); // sin^2(theta) ~ 1e-18 << 1e-12
		IndexedTriangle t0;
		t0.iVertices[0] = 0; t0.iVertices[1] = 1; t0.iVertices[2] = 2;
		t0.iNormals[0] = t0.iNormals[1] = t0.iNormals[2] = 0;
		t0.iCoords[0]  = t0.iCoords[1]  = t0.iCoords[2]  = 0;
		mesh->AddIndexedTriangle( t0 );
		mesh->DoneIndexedTriangles();

		const float colRGBA[12] = {
			0.1f, 0.0f, 0.0f, 1.0f,
			0.5f, 0.0f, 0.0f, 1.0f,
			0.9f, 0.0f, 0.0f, 1.0f,
		};
		Check( mesh->AddNamedCornerAttribute( "s", colRGBA, 3 ), "attach attr on sliver tri" );

		SurfaceSignalInfo hit;
		hit.pProvider = mesh;
		hit.primId    = 0;
		hit.baryA     = Scalar( 0.25 );
		hit.baryB     = Scalar( 0.25 );
		hit.ptObject  = Point3( 0.75, 0.25e-9, 0.0 );
		const Point3 shifted( 0.75, 0.01, 0.0 ); // transverse offset across needle
		Scalar rgba[4] = { 0, 0, 0, 0 };
		Check( hit.NamedAttribute( "s", rgba, &shifted ), "sliver tri evaluates cleanly" );
		// Must fall back to exact hit.baryA/baryB: 0.1 + 0.25*0.4 + 0.25*0.8 = 0.4
		CheckClose( rgba[0], 0.4, 1e-6, "sliver tri falls back to hit barys (no 1e12 blowup)" );

		mesh->release();
	}

	void TestTransformedObjectMappingAndRelief()
	{
		TriangleMeshGeometryIndexed* mesh = MakeQuadMesh();
		// Linear ramp h(x,y) = x across both triangles
		const float heightRGBA[24] = {
			0.0f, 0.0f, 0.0f, 1.0f,
			1.0f, 0.0f, 0.0f, 1.0f,
			1.0f, 0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 0.0f, 1.0f,
			1.0f, 0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 0.0f, 1.0f,
		};
		Check( mesh->AddNamedCornerAttribute( "h", heightRGBA, 6 ), "attach height attr" );

		// Wrap mesh in a real Object with non-uniform scale (2, 1, 1), rotation +90 deg around Z, and translation (1, 2, 3)
		// Object X -> World +Y scaled by 2 (so dh/dy_world = 1/2 = 0.5, dh/dx_world = 0)
		// Object +Z -> World +Z
		Object* obj = new Object( mesh );
		obj->SetPosition( Point3( 1.0, 2.0, 3.0 ) );
		obj->SetStretch( Vector3( 2.0, 1.0, 1.0 ) );
		obj->SetOrientation( Vector3( 0.0, 0.0, PI / 2.0 ) );
		obj->FinalizeTransformations();

		// Object point (0.5, 0.25, 0) -> stretched (1.0, 0.25, 0) -> rotated +90Z (-0.25, 1.0, 0) -> translated (0.75, 3.0, 3.0)
		RayIntersection ri( Ray( Point3( 0.75, 3.0, 5.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		obj->IntersectRay( ri, Scalar( 100.0 ), true, true, false );
		Check( ri.geometric.bHit, "hit transformed Object" );

		// 1) Normal Object::IntersectRay hit (where ptObjIntersec != signals.ptObject by SURFACE_INTERSEC_ERROR):
		//    exact attribute value at object x = 0.5 is 0.5, AND MappingPainter(Proj_Object) with huge translation
		//    must NOT extrapolate or corrupt the attribute (P1-1 red-proof!)
		ExpressionProgram progH = ExpressionProgram::Invalid();
		Check( CompileExpr( "mesh_attr3(\"h\")", true, progH ), "compile h vec3" );
		std::vector<ParamSpec> specs;
		ExpressionPainter* epH = new ExpressionPainter( progH, specs, Scalar( 0 ) );
		const RISEPel directVal = epH->GetColor( ri.geometric );
		CheckClose( directVal[0], 0.5, 1e-6, "transformed Object hit reads exact attr 0.5" );

		MappingPainter* mpObj = new MappingPainter( *epH, MappingPainter::Proj_Object,
			Vector3( 5.0, 5.0, 5.0 ), Vector3( 0, 0, 0 ), Vector3( 100.0, -50.0, 0.0 ), Scalar( 1.0 ) );
		const RISEPel mappedVal = mpObj->GetColor( ri.geometric );
		CheckClose( mappedVal[0], 0.5, 1e-6, "MappingPainter(Proj_Object) does not extrapolate mesh_attr3" );

		// 2) ReliefModifier on transformed Object with memo-worthy expression + ExpressionMemo ON:
		//    Analytical Blinn normal: N_world = (0,0,1), grad_world(h) = (0, 0.5, 0), scale = 0.8
		//    => unnormalized N' = (0, 0, 1) - 0.8 * (0, 0.5, 0) = (0, -0.4, 1)
		//    => normalized N' = (0, -0.4, 1) / sqrt(1.16)
		ExpressionMemo::SetEnabledOverride( 1 );
		{
			ExpressionProgram progRelief = ExpressionProgram::Invalid();
			Check( CompileExpr( "mesh_attr3(\"h\").x + perlin(Po) * 0.0", true, progRelief ), "compile memo-worthy relief expr" );
			Check( progRelief.MemoWorthy(), "relief expr is MemoWorthy" );
			ExpressionScalarPainter* heightPainter = new ExpressionScalarPainter( progRelief, specs );
			ReliefModifier* relief = new ReliefModifier( *heightPainter, Scalar( 0.8 ), ReliefDomain::Surface, Scalar( 0.01 ), Scalar( 10.0 ) );

			RayIntersectionGeometric riRelief = ri.geometric;
			relief->Modify( riRelief );
			const double invLen = 1.0 / std::sqrt( 1.0 + 0.4 * 0.4 );
			CheckClose( riRelief.vNormal.x, 0.0,            1e-5, "transformed ReliefModifier analytical Nx == 0" );
			CheckClose( riRelief.vNormal.y, -0.4 * invLen,  1e-5, "transformed ReliefModifier analytical Ny == -0.4/sqrt(1.16)" );
			CheckClose( riRelief.vNormal.z,  1.0 * invLen,  1e-5, "transformed ReliefModifier analytical Nz == 1.0/sqrt(1.16)" );

			relief->release();
			heightPainter->release();
		}
		ExpressionMemo::SetEnabledOverride( -1 );

		mpObj->release();
		epH->release();
		obj->release();
		mesh->release();
	}

	void TestUnsupportedGeometryFallbacks()
	{
		ExpressionProgram progRGB = ExpressionProgram::Invalid();
		ExpressionProgram progA = ExpressionProgram::Invalid();
		Check( CompileExpr( "mesh_attr3(\"distance\")", true, progRGB ), "compile fallback rgb" );
		Check( CompileExpr( "mesh_attr_alpha(\"distance\")", true, progA ), "compile fallback alpha" );

		// 1) Non-indexed TriangleMeshGeometry (pProvider == nullptr)
		{
			TriangleMeshGeometry* nonIdx = new TriangleMeshGeometry( true );
			nonIdx->BeginTriangles();
			Triangle tri;
			tri.vertices[0] = Point3( 0, 0, 0 );
			tri.vertices[1] = Point3( 1, 0, 0 );
			tri.vertices[2] = Point3( 0, 1, 0 );
			tri.normals[0] = tri.normals[1] = tri.normals[2] = Vector3( 0, 0, 1 );
			tri.coords[0] = Point2( 0, 0 ); tri.coords[1] = Point2( 1, 0 ); tri.coords[2] = Point2( 0, 1 );
			nonIdx->AddTriangle( tri );
			nonIdx->DoneTriangles();

			RayIntersectionGeometric ri( Ray( Point3( 0.25, 0.25, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			nonIdx->IntersectRay( ri, true, true, false );
			Check( ri.bHit && ri.signals.pProvider == nullptr, "non-indexed mesh has null provider" );
			ExprEvalContext ctx;
			ctx.signals = ri.signals;
			ctx.pHit = &ri;
			const Vector3 rgb = progRGB.EvalVec3( ctx );
			const Scalar a = progA.Eval( ctx );
			CheckClose( rgb.x + rgb.y + rgb.z, 0.0, 1e-9, "non-indexed mesh returns (0,0,0)" );
			CheckClose( a, 0.0, 1e-9, "non-indexed mesh alpha returns 0" );
			nonIdx->release();
		}

		// 2) Provider inheriting default ISurfaceSignalProvider::EvaluateNamedAttribute() -> false (SDF/Skeleton contract)
		{
			DummySignalProvider dummy;
			ExprEvalContext ctx;
			ctx.signals.pProvider = &dummy;
			ctx.signals.primId = -1;
			const Vector3 rgb = progRGB.EvalVec3( ctx );
			const Scalar a = progA.Eval( ctx );
			CheckClose( rgb.x + rgb.y + rgb.z, 0.0, 1e-9, "default provider returns (0,0,0)" );
			CheckClose( a, 0.0, 1e-9, "default provider alpha returns 0" );
		}
	}

	void TestBDPTVertexAndMemoSeparation()
	{
		TriangleMeshGeometryIndexed* mesh = MakeQuadMesh();
		const float colRGBA[24] = {
			0.1f, 0.2f, 0.3f, 0.4f,
			0.5f, 0.6f, 0.7f, 0.8f,
			0.9f, 0.4f, 0.2f, 0.1f,
			0.8f, 0.7f, 0.6f, 0.5f,
			0.8f, 0.7f, 0.6f, 0.5f,
			0.8f, 0.7f, 0.6f, 0.5f,
		};
		Check( mesh->AddNamedCornerAttribute( "col", colRGBA, 6 ), "attach col for BDPT/memo" );

		ExpressionMemo::SetEnabledOverride( 1 );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileExpr( "mesh_attr3(\"col\") + vec3(perlin(Po) * 0.0, 0, 0)", true, prog ), "compile memo-worthy attr expr" );
		Check( prog.MemoWorthy(), "perlin + mesh_attr3 is MemoWorthy" );

		std::vector<ParamSpec> specs;
		ExpressionPainter* painter = new ExpressionPainter( prog, specs, Scalar( 0 ) );

		RayIntersectionGeometric ri0( Ray( Point3( 0.7, 0.2, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		mesh->IntersectRay( ri0, true, true, false );
		ri0.ptIntersection = ri0.signals.ptObject;
		ri0.ptObjIntersec  = ri0.signals.ptObject;

		BDPTVertex v0;
		v0.position      = ri0.ptIntersection;
		v0.normal        = ri0.vNormal;
		v0.ptCoord       = ri0.ptCoord;
		v0.ptObjIntersec = ri0.ptObjIntersec;
		v0.signals       = ri0.signals;
		RayIntersectionGeometric rebuilt0( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		PathVertexEval::PopulateRIGFromVertex( v0, rebuilt0 );

		const RISEPel pLive0    = painter->GetColor( ri0 );
		const RISEPel pRebuilt0 = painter->GetColor( rebuilt0 );
		CheckClose( pRebuilt0[0], pLive0[0], 1e-9, "BDPT rebuilt tri 0 R matches" );
		CheckClose( pRebuilt0[1], pLive0[1], 1e-9, "BDPT rebuilt tri 0 G matches" );
		CheckClose( pRebuilt0[2], pLive0[2], 1e-9, "BDPT rebuilt tri 0 B matches" );

		// Memo separation: same Po/P, different primId
		RayIntersectionGeometric ri1 = ri0;
		ri1.signals.primId = 1;
		const RISEPel pSepPrim = painter->GetColor( ri1 );
		CheckClose( pSepPrim[0], 0.8, 1e-6, "L2 memo separates primId 0 vs 1" );

		// Memo separation: same Po/P/primId, different baryA
		RayIntersectionGeometric riBary = ri0;
		riBary.signals.baryA = Scalar( 0.0 );
		riBary.signals.baryB = Scalar( 0.0 );
		const RISEPel pSepBary = painter->GetColor( riBary );
		CheckClose( pSepBary[0], 0.1, 1e-6, "L2 memo separates baryA/baryB" );

		ExpressionMemo::SetEnabledOverride( -1 );
		painter->release();
		mesh->release();
	}

	void TestPainterRGBScalarPainterAndSceneRoundTrip()
	{
		// 1) UniformColorPainter source + non-finite guard
		{
			UniformColorPainter* src = new UniformColorPainter( RISEPel( 0.8, 0.5, 0.2 ) );
			IScalarPainter* rgbSp = nullptr;
			Check( RISE_API_CreatePainterRGBScalarPainter( &rgbSp, *src, Scalar( 0.5 ), Scalar( 0.1 ) ),
				"RISE_API_CreatePainterRGBScalarPainter succeeds" );
			Check( rgbSp != nullptr && rgbSp->HasPerChannelVariation(), "PainterRGBScalarPainter HasPerChannelVariation == true" );
			Check( !rgbSp->ReadsWorldPosition(), "UniformColorPainter-backed RGB scalar does not read P" );

			RayIntersectionGeometric ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			const ScalarTriple vals = rgbSp->GetValuesAt( ri );
			CheckClose( vals.v[0], 0.50, 1e-9, "PainterRGBScalarPainter R" );
			CheckClose( vals.v[1], 0.35, 1e-9, "PainterRGBScalarPainter G" );
			CheckClose( vals.v[2], 0.20, 1e-9, "PainterRGBScalarPainter B" );

			// Spectral piecewise-linear check + NaN nm guard
			CheckClose( rgbSp->GetValueAtNM( ri, 400.0 ), 0.20,  1e-9, "NM 400 clamped to B" );
			CheckClose( rgbSp->GetValueAtNM( ri, 450.0 ), 0.20,  1e-9, "NM 450 == B" );
			CheckClose( rgbSp->GetValueAtNM( ri, 500.0 ), 0.275, 1e-9, "NM 500 midpoint(B,G)" );
			CheckClose( rgbSp->GetValueAtNM( ri, 550.0 ), 0.35,  1e-9, "NM 550 == G" );
			CheckClose( rgbSp->GetValueAtNM( ri, 600.0 ), 0.425, 1e-9, "NM 600 midpoint(G,R)" );
			CheckClose( rgbSp->GetValueAtNM( ri, 650.0 ), 0.50,  1e-9, "NM 650 == R" );
			CheckClose( rgbSp->GetValueAtNM( ri, 700.0 ), 0.50,  1e-9, "NM 700 clamped to R" );
			CheckClose( rgbSp->GetValueAtNM( ri, std::numeric_limits<Scalar>::quiet_NaN() ), 0.35, 1e-9, "NaN nm falls back to G" );

			rgbSp->release();

			// Non-finite scale sanitized to 0
			IScalarPainter* nanSp = nullptr;
			Check( RISE_API_CreatePainterRGBScalarPainter( &nanSp, *src, std::numeric_limits<Scalar>::quiet_NaN(), Scalar( 0 ) ),
				"create PainterRGBScalarPainter with NaN scale" );
			const ScalarTriple nanVals = nanSp->GetValuesAt( ri );
			CheckClose( nanVals.v[0] + nanVals.v[1] + nanVals.v[2], 0.0, 1e-9, "NaN scale sanitized to 0" );
			nanSp->release();
			src->release();
		}

		// 2) Spatially-varying ExpressionPainter source + ReadsWorldPosition() propagation
		{
			ExpressionProgram progP = ExpressionProgram::Invalid();
			Check( CompileExpr( "vec3(P.x, P.y, 0.25)", true, progP ), "compile P-reading expr" );
			std::vector<ParamSpec> specs;
			ExpressionPainter* epP = new ExpressionPainter( progP, specs, Scalar( 0 ) );
			Check( epP->ReadsWorldPosition(), "ExpressionPainter reading P reports ReadsWorldPosition()==true" );

			IScalarPainter* rgbSp = nullptr;
			Check( RISE_API_CreatePainterRGBScalarPainter( &rgbSp, *epP, Scalar( 1.0 ), Scalar( 0.0 ) ), "wrap ExpressionPainter" );
			Check( rgbSp->ReadsWorldPosition(), "PainterRGBScalarPainter forwards ReadsWorldPosition()==true" );

			RayIntersectionGeometric ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			ri.ptIntersection = Point3( 0.4, 0.7, 0.0 );
			const ScalarTriple v = rgbSp->GetValuesAt( ri );
			CheckClose( v.v[0], 0.4, 1e-9, "spatial RGB scalar R == P.x" );
			CheckClose( v.v[1], 0.7, 1e-9, "spatial RGB scalar G == P.y" );
			CheckClose( v.v[2], 0.25, 1e-9, "spatial RGB scalar B == 0.25" );

			rgbSp->release();
			epP->release();
		}

		// 3) .RISEscene CST round-trip: mesh_attr3/mesh_attr_alpha with \xHH escapes + channel RGB verification
		{
			const std::string scene =
				"RISE ASCII SCENE 7\n"
				"expression_painter\n"
				"{\n"
				"    name ep_attr\n"
				"    def a mesh_attr_alpha(\"Col\\x231\\x20\\x20\\x7b0\\x7d\")\n"
				"    expr mesh_attr3(\"Col\\x231\\x20\\x20\\x7b0\\x7d\") + vec3(a, 0, 0)\n"
				"}\n"
				"scalar_painter\n"
				"{\n"
				"    name sp_rgb\n"
				"    painter ep_attr\n"
				"    channel RGB\n"
				"    scale 0.5\n"
				"    bias 0.1\n"
				"}\n"
				"dielectric_material\n"
				"{\n"
				"    name m_water\n"
				"    tau sp_rgb\n"
				"    ior 1.33\n"
				"}\n";
			ScopedTempFile tmp( scene, "roundtrip" );
			IJob* pJob = nullptr;
			Check( RISE_CreateJob( &pJob ) && pJob != nullptr, "create job for CST round-trip" );
			Check( pJob->LoadAsciiSceneAuto( tmp.path.c_str() ), "CST load mesh_attr3 \\xHH + channel RGB + dielectric" );

			IJobPriv* pPriv = dynamic_cast<IJobPriv*>( pJob );
			Check( pPriv != nullptr, "IJobPriv available" );
			IScalarPainter* sp = pPriv ? pPriv->GetScalarPainters()->GetItem( "sp_rgb" ) : nullptr;
			Check( sp != nullptr && sp->HasPerChannelVariation(), "parsed sp_rgb has HasPerChannelVariation()==true" );

			// Evaluate parsed sp_rgb on a real mesh carrying attribute "Col#1  {0}"!
			TriangleMeshGeometryIndexed* mesh = MakeQuadMesh();
			const float attrRGBA[24] = {
				0.2f, 0.4f, 0.6f, 0.3f,
				0.2f, 0.4f, 0.6f, 0.3f,
				0.2f, 0.4f, 0.6f, 0.3f,
				0.2f, 0.4f, 0.6f, 0.3f,
				0.2f, 0.4f, 0.6f, 0.3f,
				0.2f, 0.4f, 0.6f, 0.3f,
			};
			Check( mesh->AddNamedCornerAttribute( "Col#1  {0}", attrRGBA, 6 ), "attach CST-hostile attr name" );
			RayIntersectionGeometric ri( Ray( Point3( 0.5, 0.25, 1.0 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			mesh->IntersectRay( ri, true, true, false );
			if( sp ) {
				const ScalarTriple sv = sp->GetValuesAt( ri );
				// ep_attr = (0.2+0.3, 0.4, 0.6) = (0.5, 0.4, 0.6)
				// sp_rgb  = 0.1 + 0.5 * (0.5, 0.4, 0.6) = (0.35, 0.30, 0.40)
				CheckClose( sv.v[0], 0.35, 1e-6, "CST round-trip sp_rgb R == 0.35" );
				CheckClose( sv.v[1], 0.30, 1e-6, "CST round-trip sp_rgb G == 0.30" );
				CheckClose( sv.v[2], 0.40, 1e-6, "CST round-trip sp_rgb B == 0.40" );
			}
			mesh->release();
			pJob->release();
		}

		// 4) ConnectionLegality + derive rejection of `channel RGB` into requireSingle slot & P-reading IOR
		{
			const std::string badSingleSlot =
				"RISE ASCII SCENE 7\n"
				"uniformcolor_painter\n"
				"{\n"
				"    name p_src\n"
				"    color 0.5 0.5 0.5\n"
				"}\n"
				"scalar_painter\n"
				"{\n"
				"    name sp_rgb\n"
				"    painter p_src\n"
				"    channel RGB\n"
				"}\n"
				"sheen_material\n"
				"{\n"
				"    name m_sheen\n"
				"    sheen_color p_src\n"
				"    sheen_roughness sp_rgb\n"
				"}\n";
			// Static CST ConnectionLegality MUST reject wiring sp_rgb -> sheen_roughness!
			const Cst::Document doc = Cst::ParseToCst( badSingleSlot );
			const ConnectionVerdict verdict = ConnectionLegality::CheckConnectionByName(
				doc, ChunkCategory::Material, String( "m_sheen" ), String( "sheen_roughness" ),
				ChunkCategory::Painter, String( "sp_rgb" ) );
			Check( !verdict.legal, "ConnectionLegality rejects scalar_painter channel RGB in requireSingle slot" );

			// And full scene load MUST also reject it
			ScopedTempFile tmp( badSingleSlot, "badsingle" );
			IJob* pJob = nullptr;
			RISE_CreateJob( &pJob );
			Check( !pJob->LoadAsciiSceneAuto( tmp.path.c_str() ), "scene load rejects channel RGB in requireSingle slot" );
			pJob->release();
		}

		// 5) DL-292: P-reading PainterRGBScalarPainter bound to dielectric_material.ior MUST be rejected
		{
			const std::string badIor =
				"RISE ASCII SCENE 7\n"
				"expression_painter\n"
				"{\n"
				"    name ep_p\n"
				"    expr vec3(1.3 + P.x * 0.1, 1.3, 1.3)\n"
				"}\n"
				"scalar_painter\n"
				"{\n"
				"    name sp_ior_p\n"
				"    painter ep_p\n"
				"    channel RGB\n"
				"}\n"
				"dielectric_material\n"
				"{\n"
				"    name m_bad_ior\n"
				"    tau 0.9\n"
				"    ior sp_ior_p\n"
				"}\n";
			ScopedTempFile tmp( badIor, "badior" );
			IJob* pJob = nullptr;
			RISE_CreateJob( &pJob );
			Check( !pJob->LoadAsciiSceneAuto( tmp.path.c_str() ), "DL-292 rejects P-reading PainterRGBScalarPainter in dielectric ior" );
			pJob->release();
		}
	}
}

int main()
{
	TestParserAndTokenizer();
	TestPerCornerSeamValidationAndSanitization();
	TestPreFinalizeMismatchAndRejectedTriangleCompaction();
	TestSliverTriangleGuard();
	TestTransformedObjectMappingAndRelief();
	TestUnsupportedGeometryFallbacks();
	TestBDPTVertexAndMemoSeparation();
	TestPainterRGBScalarPainterAndSceneRoundTrip();

	std::printf( "NamedMeshAttributeTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
