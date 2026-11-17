//////////////////////////////////////////////////////////////////////
//
//  ObjectManagerPrepareCensusTest.cpp - ObjectManager::PrepareForRendering
//    memoizes its per-object SMS / DL-382 policy census across
//    preparations (ObjectManager::PrepareCensus).  The memo keys on
//    (object, material) and must never answer for a stale material or a
//    removed object; visibility and the material's painter-dependent facts
//    (an in-place index edit) are read live.  Each row changes ONE input
//    between two preparations of the same manager and checks the published
//    policy against a fresh manager's answer for the same state (interior
//    media are read live too but no row changes one).
//
//    Why it exists: before the memo, every preparation paid ~25 RTTI
//    queries per object (CstEditCostTest's NON-spatial prepare cost as
//    much as a TLAS rebuild, first bad commit ccc56b7b9).  A memo that is
//    fast but stale would silently mis-route extended SMS.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Materials/PerfectRefractorMaterial.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/Reference.h"
#include "TestTempDir.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

//! A uniform value that does NOT certify position independence -- what an
//! editor-assigned spatially varying index painter reports.  SMS's extended
//! policy rejects a refractive caster whose index is not certified uniform.
class PositionDependentScalar : public UniformScalarPainter
{
public:
	PositionDependentScalar() : UniformScalarPainter( 1.5 ) {}
	bool IsPositionIndependent() const override { return false; }
};

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool ok, const std::string& what )
{
	if( ok ) {
		++g_pass;
	} else {
		++g_fail;
		std::printf( "  FAIL: %s\n", what.c_str() );
	}
}

// One sphere `obj` (Lambertian) plus every material the rows swap in.
// `objMaterial` names obj's initial material.
static std::string Scene( const char* objMaterial )
{
	std::string s =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
		"lambertian_material\n{\nname diffuse\nreflectance white\n}\n"
		"perfectrefractor_material\n{\nname glass\nrefractance white\nior 1.5\n}\n"
		"perfectreflector_material\n{\nname mirror\nreflectance white\n}\n"
		"composite_material\n{\nname coat\ntop glass\nbottom diffuse\n}\n"
		"sphere_geometry\n{\nname sg\nradius 1\n}\n"
		"sphere_geometry\n{\nname sg2\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry sg\nmaterial ";
	s += objMaterial;
	s += "\n}\n";
	s += "standard_object\n{\nname other\ngeometry sg2\nmaterial diffuse\nposition 5 0 0\n}\n";
	return s;
}

static IJobPriv* Load( const std::string& text )
{
	const std::string path = RISE::TestSupport::ScratchBaseDir() + "/om_census_" + std::to_string( (int)getpid() ) + ".RISEscene";
	{
		std::ofstream f( path );
		f << text;
	}
	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) {
		std::remove( path.c_str() );
		return nullptr;
	}
	const bool ok = job->LoadAsciiSceneViaCst( path.c_str() );
	std::remove( path.c_str() );
	if( !ok ) {
		safe_release( job );
		return nullptr;
	}
	return job;
}

struct Policy
{
	bool allowed = false;
	bool rejected = false;
	size_t casters = 0;
	bool operator==( const Policy& o ) const { return allowed == o.allowed && rejected == o.rejected && casters == o.casters; }
};

static Policy Read( IJobPriv& job )
{
	job.GetObjects()->PrepareForRendering();
	const ObjectManager* om = dynamic_cast<const ObjectManager*>( job.GetObjects() );
	Policy p;
	if( om ) {
		p.allowed = om->ExtendedSMSAllowed();
		p.rejected = om->HasRejectedTransmissiveCaster();
		p.casters = om->ExtendedSMSCasters().size();
	}
	return p;
}

static std::string Str( const Policy& p )
{
	char b[96];
	std::snprintf( b, sizeof(b), "allowed=%d rejected=%d casters=%zu", (int)p.allowed, (int)p.rejected, p.casters );
	return b;
}

// Fresh-manager reference for `obj` holding `material`.
static Policy Reference_( const char* material )
{
	IJobPriv* job = Load( Scene( material ) );
	Policy p;
	if( job ) {
		p = Read( *job );
		safe_release( job );
	}
	return p;
}

int main()
{
	std::printf( "ObjectManagerPrepareCensusTest\n" );

	const Policy refDiffuse = Reference_( "diffuse" );
	const Policy refGlass   = Reference_( "glass" );
	const Policy refMirror  = Reference_( "mirror" );
	const Policy refCoat    = Reference_( "coat" );
	std::printf( "  reference diffuse: %s\n  reference glass:   %s\n  reference mirror:  %s\n  reference coat:    %s\n",
		Str( refDiffuse ).c_str(), Str( refGlass ).c_str(), Str( refMirror ).c_str(), Str( refCoat ).c_str() );

	// The references must actually differ, or the swap rows below prove
	// nothing about staleness.
	Check( refDiffuse.casters == 0 && refGlass.casters == 1 && refMirror.casters == 1,
		"premise: a refractor / reflector sphere is an extended-SMS caster, a diffuse one is not" );
	Check( refDiffuse.allowed && !refCoat.allowed,
		"premise: a composite material makes extended SMS inert, a diffuse one does not" );

	IJobPriv* job = Load( Scene( "diffuse" ) );
	Check( job != nullptr, "scene loads" );
	if( !job ) {
		std::printf( "%d passed, %d failed\n", g_pass, g_fail );
		return 1;
	}
	IObjectPriv* obj = job->GetObjects()->GetItem( "obj" );
	IMaterialManager* mats = job->GetMaterials();
	Check( obj && mats, "object and material manager found" );
	if( obj && mats ) {
		Check( Read( *job ) == refDiffuse, "initial preparation == fresh reference (diffuse)" );
		Check( Read( *job ) == refDiffuse, "second, memoized preparation unchanged (diffuse)" );

		// Material swaps between preparations of the SAME manager: each must
		// be re-censused, not answered from the previous material's entry.
		const char* order[] = { "glass", "coat", "mirror", "diffuse", "coat", "glass" };
		for( const char* name : order ) {
			IMaterial* m = mats->GetItem( name );
			Check( m != nullptr, std::string( "material " ) + name + " found" );
			if( !m ) continue;
			obj->AssignMaterial( *m );	// returns false unconditionally (Object.cpp); the policy rows below are the check
			const Policy expect = !std::string( name ).compare( "glass" ) ? refGlass
				: !std::string( name ).compare( "coat" ) ? refCoat
				: !std::string( name ).compare( "mirror" ) ? refMirror : refDiffuse;
			const Policy got = Read( *job );
			Check( got == expect, std::string( "after swapping obj to " ) + name + ": " + Str( got ) + " (fresh: " + Str( expect ) + ")" );
		}

		// Visibility is read live: hiding the glass caster drops it.
		obj->SetWorldVisible( false );
		Check( Read( *job ).casters == 0, "a hidden caster is not an extended-SMS caster (visibility read live)" );
		obj->SetWorldVisible( true );
		Check( Read( *job ) == refGlass, "un-hiding restores the glass policy" );

		// An IN-PLACE material edit (the editor's MaterialIntrospection::SetSlot
		// calls the material's own setter) keeps the material pointer, so the
		// memo key does not change.  The index painter's position independence
		// is read live: swapping glass's index for a painter that does not
		// certify it must reject the caster exactly as a fresh manager does,
		// and restoring a uniform index must restore the caster.
		{
			PerfectRefractorMaterial* glass = dynamic_cast<PerfectRefractorMaterial*>( mats->GetItem( "glass" ) );
			Check( glass != nullptr, "glass is a PerfectRefractorMaterial" );
			if( glass ) {
				PositionDependentScalar* varying = new PositionDependentScalar();
				glass->SetIOR( *varying );
				const Policy got = Read( *job );
				Check( got.rejected && got.casters == 0,
					"after an in-place SetIOR to a position-dependent index: rejected, no caster (" + Str( got ) + ")" );
				UniformScalarPainter* uniform = new UniformScalarPainter( 1.5 );
				glass->SetIOR( *uniform );
				Check( Read( *job ) == refGlass, "restoring a uniform index in place restores the glass policy" );
				safe_release( varying );
				safe_release( uniform );
			}
		}

		// Removal drops the memo; the remaining objects are re-censused.
		Check( job->GetObjects()->RemoveItem( "obj" ), "remove obj" );
		const Policy afterRemove = Read( *job );
		Check( afterRemove.casters == 0 && afterRemove.allowed,
			"after removing the caster: no casters, extended SMS allowed (" + Str( afterRemove ) + ")" );
	}
	safe_release( job );

	std::printf( "%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
