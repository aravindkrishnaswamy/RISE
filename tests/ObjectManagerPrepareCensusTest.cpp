//////////////////////////////////////////////////////////////////////
//
//  ObjectManagerPrepareCensusTest.cpp - ObjectManager::PrepareForRendering
//    memoizes its per-object SMS / DL-382 policy census across
//    preparations (ObjectManager::PrepareCensus).  The memo keys on
//    (object, material) and must never answer for a stale material or a
//    removed object; visibility and the material's painter-dependent facts
//    (an in-place index edit) are read live.  Each row changes ONE input
//    between two preparations of the same manager: material swaps are
//    checked against a fresh manager's answer for the same state, the
//    others (visibility, in-place index edits, an added or a removed
//    object) against the expected policy written out.  A fresh manager runs
//    the same census code, so an independent oracle
//    (SMSDomainReplay::PotentialCaster) checks the caster decision itself.
//    Interior media are read live too but no row changes one.
//
//    Why it exists: before the memo, every preparation paid ~25 RTTI
//    queries per object (the NON-spatial prepare cost several times a TLAS
//    rebuild, as measured by the since-removed CstEditCostTest; first bad
//    commit ccc56b7b9).  A memo that is
//    fast but stale would silently mis-route extended SMS.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Materials/PerfectRefractorMaterial.h"
#include "../src/Library/Materials/DielectricMaterial.h"
#include "../src/Library/Materials/PolishedMaterial.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/RadianceMapConfig.h"
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
		"dielectric_material\n{\nname dielectric\ntau 1\nior 1.33\nscattering 100000\n}\n"
		"polished_material\n{\nname polish\nreflectance white\ntau 1\nior 1.5\nscattering 1000\n}\n"
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
	const std::string path = RISE::TestSupport::ScratchBaseDir() + "/om_census_" + std::to_string( RISE::TestSupport::CurrentProcessId() ) + ".RISEscene";
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

// Fresh-manager reference for `obj` holding `material`.  A fresh manager runs
// the same census code, so it can only catch STALENESS; `oracleCaster` is the
// independent check: SMSDomainReplay::PotentialCaster, the predicate the
// memoized census (type gate + typed pointers) must reproduce.
static Policy Reference_( const char* material, int* oracleCaster = nullptr )
{
	IJobPriv* job = Load( Scene( material ) );
	Policy p;
	if( job ) {
		p = Read( *job );
		if( oracleCaster ) {
			const IMaterial* m = job->GetMaterials()->GetItem( material );
			*oracleCaster = m && SMSDomainReplay::PotentialCaster( *m ) ? 1 : 0;
		}
		safe_release( job );
	}
	return p;
}

int main()
{
	std::printf( "ObjectManagerPrepareCensusTest\n" );

	int oDiffuse = -1, oGlass = -1, oMirror = -1, oDiel = -1, oPolish = -1;
	const Policy refDiffuse = Reference_( "diffuse", &oDiffuse );
	const Policy refGlass   = Reference_( "glass", &oGlass );
	const Policy refMirror  = Reference_( "mirror", &oMirror );
	const Policy refCoat    = Reference_( "coat" );
	const Policy refDiel    = Reference_( "dielectric", &oDiel );
	const Policy refPolish  = Reference_( "polish", &oPolish );
	// The census's caster decision equals SMSDomainReplay::PotentialCaster for
	// every caster kind (a visible unit sphere can be an area light).
	Check( (int)refDiffuse.casters == oDiffuse && (int)refGlass.casters == oGlass && (int)refMirror.casters == oMirror
		&& (int)refDiel.casters == oDiel && (int)refPolish.casters == oPolish,
		"census caster decision == SMSDomainReplay::PotentialCaster for diffuse / refractor / reflector / dielectric / polished" );
	std::printf( "  reference diffuse: %s\n  reference glass:   %s\n  reference mirror:  %s\n  reference coat:    %s\n  reference dielectric: %s\n  reference polish:  %s\n",
		Str( refDiffuse ).c_str(), Str( refGlass ).c_str(), Str( refMirror ).c_str(), Str( refCoat ).c_str(),
		Str( refDiel ).c_str(), Str( refPolish ).c_str() );

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
		// Every caster kind the memo distinguishes (refractor, dielectric,
		// polished, reflector) and the two non-casters.
		const char* order[] = { "glass", "coat", "mirror", "diffuse", "dielectric", "polish", "coat", "glass" };
		for( const char* name : order ) {
			IMaterial* m = mats->GetItem( name );
			Check( m != nullptr, std::string( "material " ) + name + " found" );
			if( !m ) continue;
			obj->AssignMaterial( *m );	// returns false unconditionally (Object.cpp); the policy rows below are the check
			const std::string n( name );
			const Policy expect = n == "glass" ? refGlass : n == "coat" ? refCoat
				: n == "mirror" ? refMirror : n == "dielectric" ? refDiel
				: n == "polish" ? refPolish : refDiffuse;
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

		// The same in-place index swap for the dielectric and polished caster
		// kinds, whose live reads go through their own typed census branch.
		// A position-dependent index rejects the caster (or, for a polished
		// coat, drops it) exactly as SMSDomainReplay::PotentialCaster says; a
		// branch that ignored the live read would keep it.
		{
			struct Swap { const char* name; IMaterial* m; };
			for( const Swap& sw : { Swap{ "dielectric", mats->GetItem( "dielectric" ) }, Swap{ "polish", mats->GetItem( "polish" ) } } ) {
				Check( sw.m != nullptr, std::string( sw.name ) + " found" );
				if( !sw.m ) continue;
				obj->AssignMaterial( *sw.m );
				const Policy before = Read( *job );
				PositionDependentScalar* varying = new PositionDependentScalar();
				UniformScalarPainter* uniform = new UniformScalarPainter( 1.5 );
				if( auto* d = dynamic_cast<DielectricMaterial*>( sw.m ) ) d->SetIOR( *varying );
				else if( auto* p = dynamic_cast<PolishedMaterial*>( sw.m ) ) p->SetIOR( *varying );
				const bool oracleVarying = SMSDomainReplay::PotentialCaster( *sw.m );
				const Policy got = Read( *job );
				Check( !oracleVarying && got.casters == 0 && before.casters == 1,
					std::string( "after an in-place SetIOR on " ) + sw.name + " to a position-dependent index: no caster, as PotentialCaster says (" + Str( got ) + ")" );
				if( auto* d = dynamic_cast<DielectricMaterial*>( sw.m ) ) d->SetIOR( *uniform );
				else if( auto* p = dynamic_cast<PolishedMaterial*>( sw.m ) ) p->SetIOR( *uniform );
				Check( Read( *job ) == before, std::string( "restoring a uniform index on " ) + sw.name + " restores its policy" );
				safe_release( varying );
				safe_release( uniform );
			}
			IMaterial* glass = mats->GetItem( "glass" );
			if( glass ) obj->AssignMaterial( *glass );
			Check( Read( *job ) == refGlass, "back to glass" );
		}

		// An object ADDED between preparations, named to sort first so every
		// existing entry shifts position: the census refresh falls back to its
		// pointer index, keeps the existing entries, and censuses the new one.
		{
			const double pos[3] = { -5, 0, 0 }, orient[3] = { 0, 0, 0 }, scale[3] = { 1, 1, 1 };
			Check( job->AddObject( "aaa_added", "sg2", "mirror", nullptr, nullptr, RadianceMapConfig(), pos, orient, scale, true, true ),
				"add an object that sorts before the existing ones" );
			const Policy got = Read( *job );
			Check( got.allowed && !got.rejected && got.casters == refGlass.casters + 1,
				"after adding a mirror object: one more caster (" + Str( got ) + ")" );
			Check( job->GetObjects()->RemoveItem( "aaa_added" ), "remove the added object" );
			Check( Read( *job ) == refGlass, "removing it restores the glass policy" );
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
