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
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IGeometry.h"

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

// Active camera's eye-Z off the LIVE scene.
static double CamZ( Job& j )
{
	const IScene* sc = j.GetScene();
	const ICamera* cam = sc ? sc->GetCamera() : 0;
	return cam ? cam->GetLocation().z : -999.0;
}

// Object accessors off the LIVE scene -- compare bound dependency identity to the manager item.
static const IObject* Obj( Job& j, const char* n )
{
	const IScene* sc = j.GetScene();
	const IObjectManager* om = sc ? sc->GetObjects() : 0;
	return om ? om->GetItem( n ) : 0;
}
static bool ObjMatIs( Job& j, const char* obj, const char* mat )
{
	const IObject* o = Obj( j, obj );
	return o && j.GetMaterials() && o->GetMaterial() == j.GetMaterials()->GetItem( mat );
}
static bool ObjGeomIs( Job& j, const char* obj, const char* geom )
{
	const IObject* o = Obj( j, obj );
	return o && o->GetGeometry() == j.GetGeometry( geom );
}
static int ObjCasts( Job& j, const char* obj )
{
	const IObject* o = Obj( j, obj );
	return o ? ( o->DoesCastShadows() ? 1 : 0 ) : -1;
}

// Snapshot an object's final world transform as 16 doubles (declaration order = the col-major encoding).
static void ObjMat16( Job& j, const char* obj, double out[16] )
{
	for( int i = 0; i < 16; ++i ) out[i] = 0.0;
	const IObject* o = Obj( j, obj );
	if( !o ) return;
	Matrix4 m = o->GetFinalTransformMatrix();
	out[ 0] = m._00; out[ 1] = m._01; out[ 2] = m._02; out[ 3] = m._03;
	out[ 4] = m._10; out[ 5] = m._11; out[ 6] = m._12; out[ 7] = m._13;
	out[ 8] = m._20; out[ 9] = m._21; out[10] = m._22; out[11] = m._23;
	out[12] = m._30; out[13] = m._31; out[14] = m._32; out[15] = m._33;
}
static bool Mat16Eq( const double a[16], const double b[16] )
{
	for( int i = 0; i < 16; ++i ) if( std::fabs( a[i] - b[i] ) > 1e-9 ) return false;
	return true;
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

	// ---- C1: a CAMERA edit on an UNNAMED camera (exercises the resolve-by-position fallback) survives a
	//      material D2 -- the camera data-loss closure ----
	{
		const char* tc = "cst_s3_cam.RISEscene";
		{ std::ofstream o( tc );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "film\n{\nwidth 64\nheight 64\n}\n"
		       "pinhole_camera\n{\nlocation 0 0 3\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"   // UNNAMED
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tc ), "C1: loads variant + unnamed-camera scene via CST" );
		Check( std::fabs( CamZ( *j ) - 3.0 ) < 1e-6, "C1: camera z is 3 before the edit" );
		SceneEditController c( *j, 0 );
		Check( c.SetPropertyForCategory( Cat::Camera, String( "location" ), String( "0 0 9" ) ), "C1: camera location edit applies" );
		Check( std::fabs( CamZ( *j ) - 9.0 ) < 1e-6, "C1: after edit, camera z is 9" );
		// Material D2 rebuilds cameras from the Document; the (unnamed) camera edit must survive.
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "C1: material edit applies (D2)" );
		Check( std::fabs( CamZ( *j ) - 9.0 ) < 1e-6,
		       "C1: the UNNAMED-camera edit SURVIVED the material D2 (resolve-by-position fallback + data-loss closed)" );
		j->release();
		std::remove( tc );
	}

	// ---- OB: object BINDING edits (material / geometry / shadow flags) are CST-routed -> take effect,
	//      PERSIST to the Document, and SURVIVE a subsequent material-edit D2 (data-loss closure) ----
	{
		const char* tb = "cst_s3_objbind.RISEscene";
		{ std::ofstream o( tb );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "lambertian_material\n{\nname m2\nreflectance p2\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "sphere_geometry\n{\nname g2\nradius 2\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tb ), "OB: loads variant + 2-material/2-geom scene via CST" );
		Check( ObjMatIs( *j, "o", "m" ), "OB: object o bound to material m initially" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "o" ) );

		// Apply all three binding kinds (live-effect checks), then prove they reached the CST Document by
		// surviving a MATERIAL-edit D2 (a full re-derive from the retained Document) below.  NOTE: we do NOT call
		// j->RederiveCstWithVariant directly here -- that ClearAll's the scene WITHOUT rebinding the controller's
		// editor (only the controller's variant-switch path rebinds it), so reusing `c` afterward would deref freed
		// managers.  The material D2 below routes THROUGH the controller (RouteCstParamEdit_ self-rebinds), so it's
		// the correct in-band way to force a from-Document rebuild and check persistence + the data-loss closure.

		// OB1: material rebind m -> m2.
		Check( c.SetPropertyForCategory( Cat::Object, String( "material" ), String( "m2" ) ), "OB1: object material rebind applies" );
		Check( ObjMatIs( *j, "o", "m2" ), "OB1: object o now bound to m2" );

		// OB2: geometry rebind g -> g2.
		Check( c.SetPropertyForCategory( Cat::Object, String( "geometry" ), String( "g2" ) ), "OB2: object geometry rebind applies" );
		Check( ObjGeomIs( *j, "o", "g2" ), "OB2: object o now uses geometry g2" );

		// OB3: shadow-flags edit (casts_shadows true -> false).
		Check( ObjCasts( *j, "o" ) == 1, "OB3: object casts shadows (default true) initially" );
		Check( c.SetPropertyForCategory( Cat::Object, String( "casts_shadows" ), String( "false" ) ), "OB3: shadow-flags edit applies" );
		Check( ObjCasts( *j, "o" ) == 0, "OB3: object now casts no shadows" );

		// OB4: the data-loss closure + persistence proof -- ALL object binding edits SURVIVE a subsequent
		// MATERIAL-edit D2 (red-provable: disable the forward binding route and the D2 reverts them to the authored
		// chunk -> these checks fail).
		c.SetSelection( Cat::Material, String( "m2" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p1" ) ), "OB4: material edit applies (D2)" );
		Check( ObjMatIs( *j, "o", "m2" ), "OB4: object material rebind SURVIVED the material D2 (data-loss closed)" );
		Check( ObjGeomIs( *j, "o", "g2" ), "OB4: object geometry rebind SURVIVED the material D2" );
		Check( ObjCasts( *j, "o" ) == 0, "OB4: object shadow-flags edit SURVIVED the material D2" );
		j->release();
		std::remove( tb );
	}

	// ---- OT: object TRANSFORM edits commit to the authoritative `matrix` param at the edit/undo boundary, take
	//      effect, and SURVIVE a subsequent material-edit D2 (data-loss closure for transforms).  The authored
	//      object carries an `orientation` so the commit exercises the strip of the now-dead component params. ----
	{
		const char* tx = "cst_s3_objxform.RISEscene";
		{ std::ofstream o( tx );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\norientation 30 0 0\nposition 0 0 0\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tx ), "OT: loads variant + oriented-object scene via CST" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "o" ) );

		double authored[16]; ObjMat16( *j, "o", authored );
		// OT1: a PANEL position edit takes effect (the transform changes) and PERSISTS across a material D2.
		Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "0 5 0" ) ), "OT1: object position edit applies" );
		double moved[16]; ObjMat16( *j, "o", moved );
		Check( !Mat16Eq( authored, moved ), "OT1: the position edit changed the object transform" );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "OT1: material edit applies (D2)" );
		double afterD2[16]; ObjMat16( *j, "o", afterD2 );
		Check( Mat16Eq( moved, afterD2 ), "OT1: object transform SURVIVED the material D2 (data-loss closed, matrix committed)" );

		// OT2: UNDO back through the two OT1 edits (material, then position).  Undoing the position edit restores
		// the authored transform AND commits it to the CST (the undo-side commit), so a further material D2 keeps it
		// there.  Red-provable: without the undo-side commit the CST still holds the dragged matrix and the D2
		// re-applies the dragged pose.
		c.Undo();   // undo the material edit (reflectance p2 -> p1)
		c.Undo();   // undo the position edit -> RestoreTransformState(authored) + commit the authored matrix
		double undone[16]; ObjMat16( *j, "o", undone );
		Check( Mat16Eq( authored, undone ), "OT2: undo restored the authored object transform" );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "OT2: a material edit applies (D2)" );
		double afterUndoD2[16]; ObjMat16( *j, "o", afterUndoD2 );
		Check( Mat16Eq( undone, afterUndoD2 ), "OT2: the UNDONE transform SURVIVED a material D2 (undo stayed Document-consistent)" );
		j->release();
		std::remove( tx );
	}

	std::remove( tmp );
	std::cout << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
