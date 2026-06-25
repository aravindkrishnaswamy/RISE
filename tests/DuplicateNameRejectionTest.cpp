//////////////////////////////////////////////////////////////////////
//
//  DuplicateNameRejectionTest.cpp - regression test for the duplicate-name
//  honoring fix in Job::Add* (the AddItem-bool audit).
//
//  Managers key entities by name (GenericManager::AddItem REFUSES a duplicate
//  and returns false).  Historically every Job::Add* IGNORED that bool and
//  returned true, so a second entity sharing a name was silently dropped
//  (first-wins) while the add reported SUCCESS -- no parse- or derive-time
//  error.  Job::Add* now honors the bool (returns false + logs a clear,
//  kind-specific diagnostic), which the legacy parser turns into a hard
//  scene-load failure and the CST derive turns into a refused apply.
//
//  This test pins all three layers:
//    A. Job API direct -- a second Add* under an existing name returns false,
//       and the FIRST definition survives intact (first-wins preserved).
//    B. Legacy parser -- a scene with a real intra-manager duplicate fails to
//       load (return false); the same scene with a unique name loads (true).
//    C. CST derive -- a duplicate-name document is refused (diagnostics emitted)
//       and, because BOTH paths refuse identically, the derived Job still
//       matches the legacy parse (the differential gate stays green even here).
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static const std::string HDR = "RISE ASCII SCENE 6\n";

// Legacy parse a scene string into a fresh Job; returns ParseAndLoadScene's bool.
static bool LegacyLoads( const std::string& body )
{
	Job* j = new Job();
	const bool ok = ParseLegacy( HDR + body, *j );
	j->release();
	return ok;
}

// A scene whose unique-named twin loads but whose duplicate-named form does NOT.
// `dupBody` has two same-manager chunks sharing `name n`; `uniqueBody` renames the
// second.  The unique twin proves the scene is otherwise well-formed (controls for
// any unrelated parse requirement), so the only difference is the duplicate name.
static void ParserRejectsDup( const std::string& dupBody, const std::string& uniqueBody, const char* kind )
{
	Check( LegacyLoads( uniqueBody ), kind );          // control: unique names load
	Check( !LegacyLoads( dupBody ),   kind );          // duplicate name -> hard scene-load failure
}

int main()
{
	std::printf( "DuplicateNameRejectionTest -- Job::Add* honors AddItem's bool (no silent first-win)\n" );

	//----------------------------------------------------------------------
	std::printf( "[A] Job API: a second Add* under an existing name returns false; first wins\n" );
	//----------------------------------------------------------------------
	{
		Job* j = new Job();
		// Geometry (single-register manager).  Distinct radii so first-wins is observable.
		Check(  j->AddSphereGeometry( "g", 1.0 ), "geometry: first add succeeds" );
		Check( !j->AddSphereGeometry( "g", 9.0 ), "geometry: duplicate add returns false" );
		Check(  j->GetGeometries()->getItemCount() == 1, "geometry: duplicate not registered (count stays 1)" );
		{
			IGeometry* g = j->GetGeometries()->GetItem( "g" );
			Point3 c; Scalar r = 0; if( g ) g->GenerateBoundingSphere( c, r );
			Check( g && r == (Scalar)1.0, "geometry: FIRST definition survives (radius 1, not 9)" );
		}
		// Shader operation (single-register; simplest signature -- name only).
		Check(  j->AddReflectionShaderOp( "rop" ), "shaderop: first add succeeds" );
		Check( !j->AddReflectionShaderOp( "rop" ), "shaderop: duplicate add returns false" );
		// Painter (DUAL-register: colour manager primary + function-2D index).
		const double white[3] = { 1.0, 1.0, 1.0 };
		Check(  j->AddUniformColorPainter( "p", white, "sRGB" ), "painter: first add succeeds" );
		Check( !j->AddUniformColorPainter( "p", white, "sRGB" ), "painter: duplicate add returns false" );
		Check(  j->GetPainters()->getItemCount() == 2,          // "none" default + "p"
		        "painter: duplicate not registered (count = none + p)" );
		// Painter dual-register: the colour manager is PRIMARY; the function-2D
		// manager is a SECONDARY index.  A successful painter add must populate
		// BOTH; a refused duplicate must leave the secondary at exactly one entry.
		Check(  j->GetFunction2Ds()->GetItem( "p" ) != 0, "painter: dual-register secondary index populated (resolvable as func2d)" );
		Check(  j->GetFunction2Ds()->getItemCount() == 1, "painter: secondary index has exactly `p` (duplicate left no stray entry)" );
		// Medium (mediaMap-backed -- bypasses GenericManager -- so the duplicate
		// guard is explicit in the adder).  Previously a duplicate silently
		// LAST-won and returned success; now it is refused like every other kind.
		const double sa[3] = { 0.01, 0.01, 0.01 }, ss[3] = { 0.02, 0.02, 0.02 };
		Check(  j->AddHomogeneousMedium( "fog", sa, ss, "isotropic", 0.0 ), "medium: first add succeeds" );
		Check( !j->AddHomogeneousMedium( "fog", sa, ss, "isotropic", 0.0 ), "medium: duplicate add returns false (no silent last-win)" );
		j->release();
	}

	//----------------------------------------------------------------------
	std::printf( "[B] Legacy parser: a real intra-manager duplicate fails the whole load\n" );
	//----------------------------------------------------------------------
	ParserRejectsDup(
		"sphere_geometry\n{\nname g\nradius 1\n}\n" "sphere_geometry\n{\nname g\nradius 2\n}\n",
		"sphere_geometry\n{\nname g\nradius 1\n}\n" "sphere_geometry\n{\nname g2\nradius 2\n}\n",
		"geometry duplicate" );
	ParserRejectsDup(
		"uniformcolor_painter\n{\nname p\ncolor 1 0 0\n}\n" "uniformcolor_painter\n{\nname p\ncolor 0 1 0\n}\n",
		"uniformcolor_painter\n{\nname p\ncolor 1 0 0\n}\n" "uniformcolor_painter\n{\nname q\ncolor 0 1 0\n}\n",
		"painter duplicate" );
	ParserRejectsDup(
		"uniformcolor_painter\n{\nname red\ncolor 1 0 0\n}\n"
		"lambertian_material\n{\nname m\nreflectance red\n}\n" "lambertian_material\n{\nname m\nreflectance red\n}\n",
		"uniformcolor_painter\n{\nname red\ncolor 1 0 0\n}\n"
		"lambertian_material\n{\nname m\nreflectance red\n}\n" "lambertian_material\n{\nname m2\nreflectance red\n}\n",
		"material duplicate" );
	ParserRejectsDup(
		"omni_light\n{\nname L\npower 100\nposition 0 5 0\ncolor 1 1 1\n}\n" "omni_light\n{\nname L\npower 50\nposition 0 1 0\ncolor 1 1 1\n}\n",
		"omni_light\n{\nname L\npower 100\nposition 0 5 0\ncolor 1 1 1\n}\n" "omni_light\n{\nname L2\npower 50\nposition 0 1 0\ncolor 1 1 1\n}\n",
		"light duplicate" );
	ParserRejectsDup(
		"sphere_geometry\n{\nname g\nradius 1\n}\n"
		"standard_object\n{\nname o\ngeometry g\n}\n" "standard_object\n{\nname o\ngeometry g\n}\n",
		"sphere_geometry\n{\nname g\nradius 1\n}\n"
		"standard_object\n{\nname o\ngeometry g\n}\n" "standard_object\n{\nname o2\ngeometry g\n}\n",
		"object duplicate" );
	ParserRejectsDup(
		"homogeneous_medium\n{\nname fog\nabsorption 0.01 0.01 0.01\nscattering 0.02 0.02 0.02\nphase hg 0.5\n}\n"
		"homogeneous_medium\n{\nname fog\nabsorption 0.03 0.03 0.03\nscattering 0.04 0.04 0.04\nphase hg 0.5\n}\n",
		"homogeneous_medium\n{\nname fog\nabsorption 0.01 0.01 0.01\nscattering 0.02 0.02 0.02\nphase hg 0.5\n}\n"
		"homogeneous_medium\n{\nname fog2\nabsorption 0.03 0.03 0.03\nscattering 0.04 0.04 0.04\nphase hg 0.5\n}\n",
		"medium duplicate" );

	//----------------------------------------------------------------------
	std::printf( "[C] CST derive: duplicate-name document refused; differential stays green\n" );
	//----------------------------------------------------------------------
	{
		const std::string dup = HDR +
			"sphere_geometry\n{\nname g\nradius 1\n}\n" "sphere_geometry\n{\nname g\nradius 2\n}\n";

		// CST derive refuses the second chunk and diagnoses it.
		Job* jc = new Job();
		Document d = ParseToCst( dup );
		std::vector<std::string> diags;
		const int applied = DeriveToJob( d, *jc, &diags );
		Check( !diags.empty(), "CST derive: duplicate diagnosed (not silently applied)" );
		Check( applied == 1, "CST derive: exactly the first chunk applied (count not inflated)" );
		Check( jc->GetGeometries()->getItemCount() == 1, "CST derive: only the first geometry registered" );
		std::string dc = DumpJob( *jc );
		jc->release();

		// Legacy parse refuses identically -> the two Jobs still match (gate green).
		Job* jl = new Job();
		ParseLegacy( dup, *jl );
		std::string dl = DumpJob( *jl );
		jl->release();
		Check( dc == dl, "CST==legacy on a duplicate scene (both refuse the second, keep the first)" );
		if( dc != dl ) std::printf( "    legacy=[%s]\n    cst   =[%s]\n", dl.c_str(), dc.c_str() );
	}

	//----------------------------------------------------------------------
	std::printf( "[D] painter / standalone function_2d coexistence (dual-register secondary)\n" );
	//----------------------------------------------------------------------
	// A standalone function_2d and a same-named painter legitimately COEXIST: the
	// painter's PRIMARY (colour) slot is free, so the add SUCCEEDS; the SECONDARY
	// func-2D index is already occupied by the standalone function and is left
	// untouched (a secondary collision is NOT a painter-add failure).  This is the
	// subtle branch RegisterPainterDual exists for -- it must NOT hard-fail here.
	{
		Job* j2 = new Job();
		double fx[2] = { 0.0, 1.0 }, fy[2] = { 0.0, 1.0 };
		Check( j2->AddPiecewiseLinearFunction( "src1d", fx, fy, 2, false, 0 ), "coexist setup: function1d src1d" );
		double qx[2] = { 0.0, 1.0 }; char nm1d[] = "src1d"; char* ynames[2] = { nm1d, nm1d };
		Check( j2->AddPiecewiseLinearFunction2D( "q", qx, ynames, 2 ), "coexist setup: standalone function2d q" );
		const double white2[3] = { 1.0, 1.0, 1.0 };
		Check( j2->AddUniformColorPainter( "q", white2, "sRGB" ),
		       "coexist: painter over a standalone function_2d of the SAME name SUCCEEDS (secondary collision is not a failure)" );
		Check( j2->GetPainters()->GetItem( "q" ) != 0,    "coexist: painter q registered in the colour (primary) manager" );
		Check( j2->GetFunction2Ds()->GetItem( "q" ) != 0, "coexist: standalone function2d q still present in the secondary index" );
		j2->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
