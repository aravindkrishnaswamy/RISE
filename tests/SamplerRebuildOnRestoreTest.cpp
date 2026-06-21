//////////////////////////////////////////////////////////////////////
//
//  SamplerRebuildOnRestoreTest.cpp (feature/gui-snapshot-prototype,
//    increment 2b(a)): proof that the RayCaster's LightSampler is
//    REBUILT after a Scene::RestoreFromSnapshot (and after any in-place
//    light edit) on a REUSED ray caster — i.e. the same-IScene-pointer
//    early-return in RayCaster::AttachScene no longer pins a stale
//    sampler when the scene's light state has changed.
//
//  THE BUG this pins (diagnosed in #2a, documented as the caveat in
//  Scene::RestoreFromSnapshot): the LuminaryManager / LightSampler /
//  EnvironmentSampler are owned by the RayCaster and (re)built inside
//  RayCaster::AttachScene AFTER its `if (pScene == pScene_) return;`
//  early-return.  Restore (or an editor light edit) changes the scene
//  CONTENTS but not the Scene POINTER, so a render reusing the cached
//  caster kept PRE-restore sampler state (wrong lighting / NEE).
//
//  THE FIX: a Scene-level light/structure generation counter that bumps
//  in RestoreFromSnapshot and on light add/remove + in-place light edits.
//  RayCaster::AttachScene keeps the fast path when nothing changed, but
//  rebuilds the samplers when the SAME Scene pointer is presented with an
//  ADVANCED generation.  The check is O(1) and a no-op when the
//  generation is unchanged (no per-render rebuild regression).
//
//  Observables (no rendering — sampler queries only):
//    - LightSampler::GetPositionalLightCount()      (changes on add/remove)
//    - LightSampler::GetPositionalLightExitance(0)  (changes on energy edit)
//    - RayCaster::GetSamplerRebuildCount()          (static; proves rebuild
//                                                    fired vs fast-path)
//
//  Tests:
//    T-restore-rebuilds   - attach; snapshot; mutate live lights (energy +
//                           add a 2nd light); restore; re-attach the SAME
//                           caster; assert the sampler now reflects the
//                           RESTORED lights (count back to 1, exitance back
//                           to baseline) AND a rebuild fired.
//    T-no-spurious-rebuild- attach twice with NO change in between; assert
//                           the second attach did NOT rebuild (generation
//                           unchanged -> fast path).
//    T-inplace-edit-rebuild- attach; edit a live light's energy in place
//                           (the editor path); re-attach the SAME caster;
//                           assert the sampler's exitance tracks the edit
//                           AND a rebuild fired (proves the bug is the
//                           general in-place-edit case, not restore-only).
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/ILightPriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Utilities/RadianceMapConfig.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
		std::cout << "  ok:   " << testName << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Scene builder — programmatic Job with one positional (omni) light
// "key", a Lambertian-bound sphere, plus filler objects.  Mirrors
// SceneRestoreTest's MakeRestoreScene but also registers a "global"
// shader so we can build a RayCaster against it (the caster needs a
// default shader; we never render).
//////////////////////////////////////////////////////////////////////
static Job* MakeSamplerScene()
{
	Job* pJob = new Job();

	const double white[3] = { 0.8, 0.8, 0.8 };
	pJob->AddUniformColorPainter( "p_white", white, "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "mat", "p_white" );
	pJob->AddSphereGeometry( "geom", 1.0 );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };

	const double posObj[3] = { 0, 0, 0 };
	pJob->AddObject( "ball", "geom", "mat", nullptr, nullptr,
		nilRMap, posObj, orient, one3, true, true );

	// A positional (point-omni) light — surfaces in
	// LightSampler::GetPositionalLightCount / GetPositionalLightExitance.
	const double lc[3] = { 1.0, 1.0, 1.0 };
	const double lp[3] = { 0, 10, 0 };
	pJob->AddPointOmniLight( "key", /*power*/ 5.0, lc, lp, /*shootPhotons*/ false );

	// A "global" shader so RISE_API_CreateRayCaster has a default shader.
	// DefaultDirectLighting is created by Job::InitializeContainers.
	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );

	return pJob;
}

static double LiveLightEnergy( ILightManager* lights, const char* name )
{
	ILightPriv* l = lights->GetItem( name );
	if( !l ) { return -1.0e30; }   // finite poison: NaN sentinel folds under -ffast-math (keystone disease)
	return (double)l->emissionEnergy();
}

// Edit a live light's energy through the same keyframe path the editor's
// SetLightProperty uses (KeyframeFromParameters -> SetIntermediateValue
// -> RegenerateData) AND bump the scene's light-topology generation, as
// the editor light-edit path now must.  Returns true on success.
static bool EditLightEnergy( Scene* scene, ILightManager* lights,
	const char* name, const char* energyStr )
{
	ILightPriv* l = lights->GetItem( name );
	if( !l ) { return false; }
	IKeyframeParameter* p =
		l->KeyframeFromParameters( String( "energy" ), String( energyStr ) );
	if( !p ) { return false; }
	l->SetIntermediateValue( *p );
	safe_release( p );
	l->RegenerateData();
	scene->BumpLightTopologyGeneration();
	return true;
}

// Fetch the caster's LightSampler (concrete-typed accessor).
static const LightSampler* SamplerOf( IRayCaster* caster )
{
	RayCaster* rc = dynamic_cast<RayCaster*>( caster );
	return rc ? rc->GetLightSampler() : nullptr;
}

//////////////////////////////////////////////////////////////////////
// T-restore-rebuilds
//////////////////////////////////////////////////////////////////////
static void TestRestoreRebuildsSampler()
{
	std::cout << "Test: sampler rebuilds after restore on a reused caster" << std::endl;

	Job* pJob = MakeSamplerScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[restore] scene downcast" ); pJob->release(); return; }

	IShaderManager* pShaders = pJob->GetShaders();
	IShader* pShader = pShaders ? pShaders->GetItem( "global" ) : nullptr;
	if( !pShader ) { Check( false, "[restore] global shader present" ); pJob->release(); return; }

	ILightManager* lights = pJob->GetLights();

	// Build a caster and attach the scene the first time.
	IRayCaster* caster = nullptr;
	RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
	if( !caster ) { Check( false, "[restore] caster create" ); pJob->release(); return; }

	// First attach: the caster's sampler is built against the INITIAL light
	// state (key @ energy 5).  This is the state a stale sampler would keep.
	caster->AttachScene( pScene );
	const LightSampler* s0 = SamplerOf( caster );
	Check( s0 != nullptr, "[restore] sampler built on first attach" );
	if( !s0 ) { safe_release( caster ); pJob->release(); return; }
	Check( s0->GetPositionalLightCount() == 1, "[restore] sampler sees 1 light at first build" );
	const double initExit = (double)s0->GetPositionalLightExitance( 0 );
	Check( initExit > 0.0, "[restore] first-build positional exitance > 0" );

	// Drive the live scene to a DISTINCT "working copy" state that differs
	// from the caster's first-build state in BOTH count and exitance: bump
	// key WAY up (5 -> 500) and add a second light.  Snapshot THAT as W.
	// (We do not re-attach the caster here — its sampler stays at the
	// first-build state, which is exactly the stale state restore must beat.)
	Check( EditLightEnergy( pScene, lights, "key", "500.0" ), "[restore] key -> 500 for working copy" );
	{
		const double lc2[3] = { 0.5, 0.5, 0.5 };
		const double lp2[3] = { 5, 5, 5 };
		pJob->AddPointOmniLight( "key2", /*power*/ 7.0, lc2, lp2, false );
		// AddPointOmniLight routes through Job (not the editor path) — bump
		// the generation as a structural light-list mutation must.
		pScene->BumpLightTopologyGeneration();
	}
	Check( lights->getItemCount() == 2, "[restore] 2 live lights in working copy" );

	SceneSnapshot* snap = pScene->CreateSnapshot();
	if( !snap ) { Check( false, "[restore] snapshot non-null" ); safe_release( caster ); pJob->release(); return; }

	// Now drive the live scene somewhere ELSE again (key -> 9, single light)
	// so the snapshot W differs from both the live scene AND the caster's
	// stale first-build state.
	{
		ILightPriv* k2 = lights->GetItem( "key2" );
		(void)k2;
		// Remove the second light via the manager (structural), then edit key.
		ILightManager* lm = lights;
		lm->RemoveItem( "key2" );
		pScene->BumpLightTopologyGeneration();
	}
	Check( EditLightEnergy( pScene, lights, "key", "9.0" ), "[restore] key -> 9 (divergent live)" );
	Check( lights->getItemCount() == 1, "[restore] 1 live light in divergent state" );

	// Restore W — live light state becomes { key @ 500, key2 @ 7 } again.
	pScene->RestoreFromSnapshot( *snap );
	safe_release( snap );
	Check( lights->getItemCount() == 2, "[restore] 2 live lights after restoring W" );
	Check( std::abs( LiveLightEnergy( lights, "key" ) - 500.0 ) < 1e-9,
	       "[restore] live key energy 500 after restoring W" );

	// Re-attach the SAME caster against the SAME Scene pointer.  WITHOUT the
	// fix this hits the same-scene early-return and the sampler stays at the
	// FIRST-BUILD state (1 light, energy-5 exitance) — observably wrong now
	// that W has 2 lights at energy 500.
	const unsigned int rbBefore = RayCaster::GetSamplerRebuildCount();
	caster->AttachScene( pScene );
	const unsigned int rbAfter  = RayCaster::GetSamplerRebuildCount();

	const LightSampler* s1 = SamplerOf( caster );
	Check( s1 != nullptr, "[restore] sampler present after re-attach" );
	if( s1 ) {
		const unsigned int postCount = s1->GetPositionalLightCount();
		// DECISIVE #1: count reflects the RESTORED W (2 lights), not the
		// caster's stale first-build state (1 light).  FAILS without the fix.
		Check( postCount == 2,
		       "[restore] re-attached sampler sees 2 lights (restored W), not stale 1" );
		if( postCount >= 1 ) {
			// DECISIVE #2: the maximum positional exitance reflects key@500,
			// which is FAR above the stale first-build key@5 exitance.  A
			// stale sampler would still report ~initExit -> FAIL.
			double maxExit = 0.0;
			for( unsigned int i = 0; i < postCount; ++i ) {
				const double e = (double)s1->GetPositionalLightExitance( i );
				if( e > maxExit ) { maxExit = e; }
			}
			Check( maxExit > initExit * 10.0,
			       "[restore] re-attached sampler exitance reflects restored key@500, not stale key@5" );
		}
	}
	// And a rebuild actually fired (proves the generation gate tripped the
	// rebuild rather than the fast path silently keeping the stale sampler).
	Check( rbAfter == rbBefore + 1,
	       "[restore] exactly one sampler rebuild fired on the re-attach" );

	safe_release( caster );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-no-spurious-rebuild: with nothing changed, a second attach must NOT
// rebuild (generation unchanged -> O(1) fast path).  This is the
// production-render no-regression guard.
//////////////////////////////////////////////////////////////////////
static void TestNoSpuriousRebuild()
{
	std::cout << "Test: no rebuild on a no-change re-attach (fast path preserved)" << std::endl;

	Job* pJob = MakeSamplerScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[nospur] scene downcast" ); pJob->release(); return; }

	IShaderManager* pShaders = pJob->GetShaders();
	IShader* pShader = pShaders ? pShaders->GetItem( "global" ) : nullptr;
	if( !pShader ) { Check( false, "[nospur] global shader present" ); pJob->release(); return; }

	IRayCaster* caster = nullptr;
	RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
	if( !caster ) { Check( false, "[nospur] caster create" ); pJob->release(); return; }

	caster->AttachScene( pScene );          // first attach (full build)

	// Re-attach repeatedly with NO scene change between calls.  Each must be
	// the fast path: no sampler rebuild.
	const unsigned int rbBefore = RayCaster::GetSamplerRebuildCount();
	caster->AttachScene( pScene );
	caster->AttachScene( pScene );
	caster->AttachScene( pScene );
	const unsigned int rbAfter  = RayCaster::GetSamplerRebuildCount();

	Check( rbAfter == rbBefore,
	       "[nospur] three no-change re-attaches did NOT rebuild the sampler" );

	safe_release( caster );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-inplace-edit-rebuild: the general in-place light-edit case (not just
// restore), driven through the REAL editor controller so it exercises the
// engine-side generation bump wired into SceneEditor::Apply(SetLightProperty).
// Edit a live light's power via SceneEditController::SetProperty; re-attach
// the SAME caster; assert the sampler tracks the edit AND a rebuild fired.
//////////////////////////////////////////////////////////////////////
static void TestInPlaceEditRebuild()
{
	std::cout << "Test: sampler rebuilds after an in-place light edit (real editor path)"
	          << std::endl;

	Job* pJob = MakeSamplerScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[edit] scene downcast" ); pJob->release(); return; }

	IShaderManager* pShaders = pJob->GetShaders();
	IShader* pShader = pShaders ? pShaders->GetItem( "global" ) : nullptr;
	if( !pShader ) { Check( false, "[edit] global shader present" ); pJob->release(); return; }

	IRayCaster* caster = nullptr;
	RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
	if( !caster ) { Check( false, "[edit] caster create" ); pJob->release(); return; }

	caster->AttachScene( pScene );
	const LightSampler* s0 = SamplerOf( caster );
	if( !s0 || s0->GetPositionalLightCount() != 1 ) {
		Check( false, "[edit] baseline sampler with 1 light" );
		safe_release( caster ); pJob->release(); return;
	}
	const double baseExit = (double)s0->GetPositionalLightExitance( 0 );

	// Edit the live light's power WAY up (5 -> 500) through the real editor
	// controller (no Start() — "queue edits, no rendering" mode, same as the
	// SceneEditorLightFullCoverageTest).  This routes through
	// SceneEditController::SetProperty -> SceneEditor::Apply(SetLightProperty),
	// which now bumps the Scene light generation.  NO direct
	// BumpLightTopologyGeneration() here — the whole point is to prove the
	// editor wiring does it.
	{
		SceneEditController ctrl( *pJob, /*interactiveRasterizer*/ 0 );
		ctrl.ForTest_SetSelection( SceneEditController::Category::Light, String( "key" ) );
		Check( ctrl.SetProperty( String( "power" ), String( "500.0" ) ),
		       "[edit] editor SetProperty(power=500) returns true" );
	}

	const unsigned int rbBefore = RayCaster::GetSamplerRebuildCount();
	caster->AttachScene( pScene );          // same pointer, advanced generation
	const unsigned int rbAfter  = RayCaster::GetSamplerRebuildCount();

	const LightSampler* s1 = SamplerOf( caster );
	Check( s1 != nullptr, "[edit] sampler present after re-attach" );
	if( s1 && s1->GetPositionalLightCount() == 1 ) {
		const double postExit = (double)s1->GetPositionalLightExitance( 0 );
		// Exitance is monotone in energy; a 100x power bump must raise the
		// sampler's cached exitance well above baseline.  A stale sampler
		// would report the baseline exitance unchanged -> FAIL.
		Check( postExit > baseExit * 10.0,
		       "[edit] re-attached sampler exitance tracks the 100x power edit (not stale)" );
	} else {
		Check( false, "[edit] re-attached sampler has 1 light" );
	}
	Check( rbAfter == rbBefore + 1, "[edit] exactly one sampler rebuild fired" );

	safe_release( caster );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// T-job-material-bump (P2a): a Job-level material retarget that changes
// the emitter set must bump the generation so a reused caster rebuilds.
//////////////////////////////////////////////////////////////////////
static void TestJobSetObjectMaterialBumpsGeneration()
{
	std::cout << "Test: Job::SetObjectMaterial (emitter change) rebuilds a reused caster's sampler (P2a)" << std::endl;
	Job* pJob = MakeSamplerScene();
	const double glow[3]={1.0,1.0,1.0}; pJob->AddUniformColorPainter("p_glow",glow,"Rec709RGB_Linear");
	pJob->AddLambertianLuminaireMaterial("mat_emit","p_glow","mat",1.0);
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[p2a] scene downcast" ); pJob->release(); return; }
	IShader* pShader = pJob->GetShaders() ? pJob->GetShaders()->GetItem("global") : nullptr;
	if( !pShader ) { Check( false, "[p2a] global shader" ); pJob->release(); return; }
	IRayCaster* caster=nullptr; RISE_API_CreateRayCaster(&caster,false,10,*pShader,true);
	if( !caster ) { Check( false, "[p2a] caster create" ); pJob->release(); return; }
	caster->AttachScene( pScene );
	Check( SamplerOf(caster) != nullptr, "[p2a] baseline sampler built" );
	const unsigned int rbBefore = RayCaster::GetSamplerRebuildCount();
	// Job-level material retarget: bind the non-emissive ball to an EMISSIVE
	// material -> the luminary set changes -> generation must bump.
	Check( pJob->SetObjectMaterial("ball","mat_emit"), "[p2a] Job::SetObjectMaterial applied" );
	caster->AttachScene( pScene );   // same Scene pointer; P2a bump -> rebuild
	const unsigned int rbAfter = RayCaster::GetSamplerRebuildCount();
	Check( rbAfter == rbBefore+1,
	       "[p2a] sampler rebuilt after Job::SetObjectMaterial emitter change (generation bumped) (P2a)" );
	safe_release( caster ); pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-job-addlight-bump (P2a extension): Job::AddPointOmniLight changes the
// light set -> reused caster must rebuild.
//////////////////////////////////////////////////////////////////////
static void TestJobAddLightBumpsGeneration()
{
	std::cout << "Test: Job::AddPointOmniLight rebuilds a reused caster's sampler (P2a extension)" << std::endl;
	Job* pJob = MakeSamplerScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[p2a2] scene" ); pJob->release(); return; }
	IShader* pShader = pJob->GetShaders() ? pJob->GetShaders()->GetItem("global") : nullptr;
	if( !pShader ) { Check( false, "[p2a2] shader" ); pJob->release(); return; }
	IRayCaster* caster=nullptr; RISE_API_CreateRayCaster(&caster,false,10,*pShader,true);
	if( !caster ) { Check( false, "[p2a2] caster" ); pJob->release(); return; }
	caster->AttachScene( pScene );
	const unsigned int rbBefore = RayCaster::GetSamplerRebuildCount();
	const double lc[3]={1,1,1}, lp[3]={3,3,3};
	Check( pJob->AddPointOmniLight("key2",7.0,lc,lp,false), "[p2a2] AddPointOmniLight applied" );
	caster->AttachScene( pScene );
	Check( RayCaster::GetSamplerRebuildCount() == rbBefore+1,
	       "[p2a2] sampler rebuilt after Job::AddPointOmniLight (light set changed) (P2a ext)" );
	safe_release( caster ); pJob->release();
}

static void TestJobRemoveLightBumpsGeneration()
{
	std::cout << "Test: Job::RemoveLight rebuilds a reused caster's sampler (H3 self-invalidation)" << std::endl;
	Job* pJob = MakeSamplerScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[h3rm] scene" ); pJob->release(); return; }
	IShader* pShader = pJob->GetShaders() ? pJob->GetShaders()->GetItem("global") : nullptr;
	if( !pShader ) { Check( false, "[h3rm] shader" ); pJob->release(); return; }
	const double lc[3]={1,1,1}, lp[3]={3,3,3};
	pJob->AddPointOmniLight("key2",7.0,lc,lp,false);   // 2nd light so one remains after removal
	IRayCaster* caster=nullptr; RISE_API_CreateRayCaster(&caster,false,10,*pShader,true);
	if( !caster ) { Check( false, "[h3rm] caster" ); pJob->release(); return; }
	caster->AttachScene( pScene );
	const unsigned int rbBefore = RayCaster::GetSamplerRebuildCount();
	Check( pJob->RemoveLight("key2"), "[h3rm] RemoveLight applied" );
	caster->AttachScene( pScene );
	Check( RayCaster::GetSamplerRebuildCount() == rbBefore+1,
	       "[h3rm] sampler rebuilt after Job::RemoveLight (light set changed) (H3 self-invalidation)" );
	safe_release( caster ); pJob->release();
}

int main()
{
	std::cout << "=== SamplerRebuildOnRestoreTest ===" << std::endl;

	TestRestoreRebuildsSampler();
	TestNoSpuriousRebuild();
	TestInPlaceEditRebuild();
	TestJobSetObjectMaterialBumpsGeneration();
	TestJobAddLightBumpsGeneration();
	TestJobRemoveLightBumpsGeneration();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
