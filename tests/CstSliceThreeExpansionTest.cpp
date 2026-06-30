//////////////////////////////////////////////////////////////////////
//
//  CstSliceThreeExpansionTest.cpp - Model-B P5 Slice 3 EXPANSION: route
//    non-material edits (light, then camera/object) through the canonical
//    CST too, so the retained Document stays COMPLETE.  This closes the
//    round-5 cross-boundary data-loss: before the expansion, a direct
//    light/camera/object edit was NOT recorded in the CST, so a later
//    material edit's D2 full-re-derive (on a variant/animated scene)
//    rebuilt the scene from the incomplete Document and silently reverted
//    the direct edit.  With the edit CST-routed, the D2 re-derives it back.
//
//    Case L1: a LIGHT edit takes effect, PERSISTS to the Document (a fresh
//    re-derive still shows it), and SURVIVES a subsequent material-edit D2
//    (red-provable: revert IsCstRoutedOp(SetLightProperty) and L1 fails).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "CstRenderEquivalence.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/LightIntrospection.h"
#include "../src/Library/Interfaces/ILightManager.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n ) { if( c ) ++passCount; else { ++failCount; std::cout << "  FAIL: " << n << std::endl; } }

// Read a light's scalar property (e.g. "power") off the LIVE scene via the same
// descriptor introspection the panel uses; -999 if absent.
static double LightProp( Job& j, const char* lightName, const char* prop )
{
	const IScene* sc = j.GetScene();
	const ILightManager* lm = sc ? sc->GetLights() : 0;
	const ILight* l = lm ? lm->GetItem( lightName ) : 0;
	if( !l ) return -999.0;
	const std::vector<CameraProperty> props = LightIntrospection::Inspect( String( lightName ), *l );
	for( size_t i = 0; i < props.size(); ++i )
		if( props[i].name == String( prop ) ) return std::atof( props[i].value.c_str() );
	return -999.0;
}

static const char* SCENE =
	"RISE ASCII SCENE 6\n"
	"scene_variant\n{\nname night\n}\n"                       // declares a variant -> material edits take D2
	"omni_light\n{\nname l\npower 4\ncolor 1 1 1\nposition 0 0 5\n}\n"
	"uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
	"uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
	"lambertian_material\n{\nname m\nreflectance p1\n}\n"
	"sphere_geometry\n{\nname g\nradius 1\n}\n"
	"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

int main()
{
	using Cat = SceneEditController::Category;
	std::cout << "CstSliceThreeExpansionTest" << std::endl;
	const char* tmp = "cst_s3_exp.RISEscene";
	{ std::ofstream o( tmp ); o << SCENE; }

	// ---- L1: a light edit is CST-routed -> takes effect, persists, survives a material D2 ----
	{
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "L1: loads variant scene via CST" );
		Check( std::fabs( LightProp( *j, "l", "power" ) - 4.0 ) < 1e-6, "L1: light power is 4 before the edit" );

		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Light, String( "l" ) );
		Check( c.SetPropertyForCategory( Cat::Light, String( "power" ), String( "8" ) ), "L1: light power edit applies" );
		Check( std::fabs( LightProp( *j, "l", "power" ) - 8.0 ) < 1e-6, "L1: after edit, light power is 8" );

		// PERSISTENCE: a fresh re-derive of the retained Document still shows 8 (the edit went through the CST,
		// not just a Job mutation -- else it would revert to 4).
		Check( j->RederiveCstWithVariant( "none" ), "L1: re-derive of the retained Document succeeds" );
		Check( std::fabs( LightProp( *j, "l", "power" ) - 8.0 ) < 1e-6, "L1: the light edit PERSISTED to the CST Document" );
		j->release();
	}

	// ---- L2: the data-loss closure -- a light edit SURVIVES a subsequent material-edit D2 ----
	{
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "L2: loads variant scene via CST" );
		SceneEditController c( *j, 0 );

		// (1) Edit the light (CST-routed now).
		c.SetSelection( Cat::Light, String( "l" ) );
		Check( c.SetPropertyForCategory( Cat::Light, String( "power" ), String( "8" ) ), "L2: light power edit applies" );
		Check( std::fabs( LightProp( *j, "l", "power" ) - 8.0 ) < 1e-6, "L2: light power is 8 after the edit" );

		// (2) Now edit a MATERIAL on this variant scene -> forces the D2 full re-derive from the Document.
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "L2: material edit applies (D2)" );

		// (3) The light edit must SURVIVE the D2 (it's in the Document now).  Pre-expansion this reverted to 4.
		Check( std::fabs( LightProp( *j, "l", "power" ) - 8.0 ) < 1e-6,
		       "L2: the light edit SURVIVED the material-edit D2 (data-loss closed)" );
		j->release();
	}

	std::remove( tmp );
	std::cout << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
