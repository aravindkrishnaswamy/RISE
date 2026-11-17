//////////////////////////////////////////////////////////////////////
//
//  StandardObjectScaleTest.cpp - DL-32 (docs/DEBT_LEDGER.md) red-proof
//    and regression guard.
//
//  `standard_object`'s (and `override_object`'s) `scale` is declared
//  `DoubleVec3`.  Before this fix, `ParseStateBag::GetVec3` zero-filled
//  every unread component before `sscanf`, so `scale 0.35` silently
//  derived a DEGENERATE transform `(0.35, 0, 0)` -- no diagnostic, the
//  object vanishes from the render, and `Object::DistanceToSurface`
//  refuses every `proximity()`/`interior()` query against it
//  (`sigma_min <= 0`).  docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §8.3 trap 4
//  / §10 records the trap; this is its dedicated regression.
//
//  Fix: a single number is now an explicit UNIFORM-scale broadcast (with
//  a log warning); anything else that is not exactly one or three finite
//  numbers is a hard parse error instead of a silent zero-fill.
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

//! Fetch the named object's final transform matrix.  Returns false if the
//! object (or the manager plumbing) cannot be found.
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
	std::printf( "StandardObjectScaleTest -- DL-32 standard_object/override_object `scale` broadcast + hard-error\n" );

	//----------------------------------------------------------------------
	// [red-proof] `scale 0.35` (ONE number) on a bare standard_object.
	// Pre-fix this derived successfully (n == 1) with a DEGENERATE
	// transform: diagonal (0.35, 0, 0) -- two axes silently zeroed, no
	// diagnostic.  Post-fix it must derive successfully with a UNIFORM
	// diagonal (0.35, 0.35, 0.35).
	//----------------------------------------------------------------------
	std::printf( "[uniform-broadcast] `scale 0.35` derives a UNIFORM transform, not a degenerate one\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nscale 0.35\n}\n",
			*j, &diags );
		Check( n == 2, "`scale 0.35` derives successfully (n == 2: geometry + object)" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._00 - 0.35 ) < kEps, "diagonal[0] == 0.35 (broadcast, not zero-filled)" );
			Check( std::fabs( m._11 - 0.35 ) < kEps, "diagonal[1] == 0.35 (RED pre-fix: was 0.0, the degenerate zero-fill)" );
			Check( std::fabs( m._22 - 0.35 ) < kEps, "diagonal[2] == 0.35 (RED pre-fix: was 0.0, the degenerate zero-fill)" );
		}
		j->release();
	}

	//----------------------------------------------------------------------
	// [regression] a well-formed three-number `scale` is untouched.
	//----------------------------------------------------------------------
	std::printf( "[three-number-unchanged] `scale 2 3 4` derives exactly as before\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nscale 2 3 4\n}\n",
			*j, &diags );
		Check( n == 2, "`scale 2 3 4` derives successfully (n == 2)" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._00 - 2.0 ) < kEps, "diagonal[0] == 2" );
			Check( std::fabs( m._11 - 3.0 ) < kEps, "diagonal[1] == 3" );
			Check( std::fabs( m._22 - 4.0 ) < kEps, "diagonal[2] == 4" );
		}
		j->release();
	}

	//----------------------------------------------------------------------
	// [default-unchanged] no `scale` line at all still defaults to (1,1,1).
	//----------------------------------------------------------------------
	std::printf( "[default-unchanged] no `scale` line defaults to (1,1,1)\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\n}\n",
			*j, &diags );
		Check( n == 2, "no-scale object derives successfully (n == 2)" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._00 - 1.0 ) < kEps && std::fabs( m._11 - 1.0 ) < kEps && std::fabs( m._22 - 1.0 ) < kEps,
				"default scale is (1,1,1)" );
		}
		j->release();
	}

	//----------------------------------------------------------------------
	// [hard-error] a malformed `scale` (two numbers) is now a HARD parse
	// error at APPLY time (Finalize) -- `ValueKind::DoubleVec3`'s PASS-1
	// validation only requires "one or more finite numbers", so a 2-number
	// value clears PASS-1 and is caught by ResolveScaleVec3 inside
	// Finalize instead (a PASS-2 failure: per DeriveToJob's own contract,
	// PASS-2 "CONTINUES PAST a failing chunk" rather than refusing the
	// whole document, so the sibling `sphere_geometry` chunk still
	// applies and `n == 1`) -- instead of a silent zero-fill or
	// (pre-DL-32) a successful-but-degenerate derive.
	//----------------------------------------------------------------------
	std::printf( "[hard-error] `scale 0.35 0.5` (two numbers) fails the standard_object chunk\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nscale 0.35 0.5\n}\n",
			*j, &diags );
		Check( n == 1, "two-number `scale`: only the sibling geometry chunk applies (n == 1) "
			"(RED pre-fix: n == 2, a successful-but-degenerate derive)" );
		Matrix4 m;
		Check( !GetObjectTransform( *j, "s", m ), "object `s` was never created (its chunk failed)" );
		bool sawDL32 = false;
		for( const std::string& d : diags )
			if( d.find( "DL-32" ) != std::string::npos ) sawDL32 = true;
		Check( sawDL32, "diagnostic names DL-32" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [hard-error] a non-numeric `scale` is refused at PASS-1 (caught by
	// the generic DoubleVec3 numeric-token validation before Finalize even
	// runs), so this one IS a refuse-all: n == 0.
	//----------------------------------------------------------------------
	std::printf( "[hard-error] `scale abc` (non-numeric) refuses the whole document\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nscale abc\n}\n",
			*j, &diags );
		Check( n == 0, "non-numeric `scale` refuses the whole document (PASS-1, n == 0)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [quaternion-path] the uniform broadcast also applies on the
	// quaternion transform-composition branch, not just the Euler one.
	//----------------------------------------------------------------------
	std::printf( "[quaternion-path] `scale 0.5` broadcasts under a `quaternion` transform too\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\nquaternion 0 0 0 1\nscale 0.5\n}\n",
			*j, &diags );
		Check( n == 2, "`scale 0.5` under `quaternion` derives successfully (n == 2)" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._00 - 0.5 ) < kEps && std::fabs( m._11 - 0.5 ) < kEps && std::fabs( m._22 - 0.5 ) < kEps,
				"quaternion-branch diagonal is uniformly 0.5 (RED pre-fix: (0.5, 0, 0))" );
		}
		j->release();
	}

	//----------------------------------------------------------------------
	// [override_object] the same broadcast rule applies to override_object's
	// `scale` -- its own descriptor claims "matches standard_object
	// semantics", which was FALSE for a single-number value before this fix
	// (override_object hard-refused it via HasExactNumericArity, while
	// standard_object silently degenerated -- two different bugs on the
	// same claimed-identical field).
	//----------------------------------------------------------------------
	std::printf( "[override_object] `scale 0.35` broadcasts uniformly, matching standard_object\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname g\nradius 1\n}\n"
			      "standard_object\n{\nname s\ngeometry g\n}\n"
			      "override_object\n{\nname s\nscale 0.35\n}\n",
			*j, &diags );
		Check( n == 3, "override_object derives successfully alongside its target (n == 3: geometry + object + override)" );
		Matrix4 m;
		const bool found = GetObjectTransform( *j, "s", m );
		Check( found, "object `s` exists after derive" );
		if( found ) {
			const double kEps = 1e-9;
			Check( std::fabs( m._00 - 0.35 ) < kEps && std::fabs( m._11 - 0.35 ) < kEps && std::fabs( m._22 - 0.35 ) < kEps,
				"override_object `scale 0.35` broadcasts to a uniform (0.35,0.35,0.35) (RED pre-fix: hard parse error)" );
		}
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
