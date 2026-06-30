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
//    Cases:
//      L1 -- a LIGHT edit takes effect + PERSISTS to the Document (fresh re-derive still shows it).
//      L2 -- a light edit SURVIVES a subsequent material-edit D2 (the data-loss closure).
//      C1 -- an UNNAMED-camera edit survives a material D2 (exercises the resolve-by-position fallback).
//      OB1-OB4 -- object BINDING edits (material/geometry/shadow) survive a material D2.
//      OT1-OT2 -- object TRANSFORM edits commit to the `matrix` param + survive a D2, fwd + undo.
//      OV1 -- an object edit resolves the BASE standard_object, not a same-named override_object.
//    Each survive-D2 / persistence assertion is red-provable by reverting the corresponding route.
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
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/SceneEditor/MediaIntrospection.h"

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

// Does a NAMED camera exist on the LIVE scene?
static bool CamExists( Job& j, const char* name )
{
	const IScene* sc = j.GetScene();
	const ICameraManager* cams = sc ? sc->GetCameras() : 0;
	return cams && cams->GetItem( name ) != 0;
}

// A named camera's rest-location Z (the authored `location` z), off the LIVE scene; -999 if absent.
static double NamedCamZ( Job& j, const char* name )
{
	const IScene* sc = j.GetScene();
	const ICameraManager* cams = sc ? sc->GetCameras() : 0;
	const ICamera* cam = cams ? cams->GetItem( name ) : 0;
	return cam ? cam->GetLocation().z : -999.0;
}

// Active camera's eye-Z off the LIVE scene.
static double CamZ( Job& j )
{
	const IScene* sc = j.GetScene();
	const ICamera* cam = sc ? sc->GetCamera() : 0;
	return cam ? cam->GetLocation().z : -999.0;
}

// Active camera's POST-orbit eye position (the rendered-from point).
static void CamLoc( Job& j, double out[3] )
{
	out[0] = out[1] = out[2] = -999.0;
	const IScene* sc = j.GetScene();
	const ICamera* cam = sc ? sc->GetCamera() : 0;
	if( !cam ) return;
	Point3 p = cam->GetLocation();
	out[0] = p.x; out[1] = p.y; out[2] = p.z;
}
static bool LocEq( const double a[3], const double b[3] )
{
	for( int i = 0; i < 3; ++i ) if( std::fabs( a[i] - b[i] ) > 1e-6 ) return false;
	return true;
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

// A medium's absorption-R off the LIVE scene; -999 if absent.
static double MedAbsR( Job& j, const char* med )
{
	const IMedium* m = j.GetMedium( med );
	if( !m ) return -999.0;
	MediumSlotValue v = MediaIntrospection::GetSlotValue( *m, String( "absorption" ) );
	return ( v.kind == MediumSlotValue::Vec3 ) ? v.v3[0] : -999.0;
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
	using Tool = SceneEditController::Tool;
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

	// ---- OV: an object edit must resolve the BASE standard_object, not a same-named override_object (the
	//      `> modify` / variant override layer, which also ends in `_object` and carries the same name).  Before
	//      the exact-keyword fix this resolved to occ=2 -> refused -> the object edit silently failed (data-loss). ----
	{
		const char* tv = "cst_s3_objoverride.RISEscene";
		{ std::ofstream o( tv );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\nposition 0 0 0\n}\n"
		       "override_object\n{\nname o\nposition 0 1 0\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tv ), "OV: loads standard_object + same-named override_object via CST" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "o" ) );
		// A transform edit must route (resolve the base standard_object) and SURVIVE a material D2 -- red-provable:
		// resolve via the loose "object" suffix instead and the commit refuses (occ=2) -> the D2 reverts the move.
		double before[16]; ObjMat16( *j, "o", before );
		Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ), "OV1: object position edit applies" );
		double moved[16]; ObjMat16( *j, "o", moved );
		Check( !Mat16Eq( before, moved ), "OV1: the edit changed the object transform" );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "OV1: material edit applies (D2)" );
		double afterD2[16]; ObjMat16( *j, "o", afterD2 );
		Check( Mat16Eq( moved, afterD2 ), "OV1: object edit resolved the BASE standard_object + SURVIVED the D2 (override_object collision avoided)" );
		j->release();
		std::remove( tv );
	}

	// ---- MD: a MEDIUM absorption edit is CST-routed -> takes effect and SURVIVES a material D2 (data-loss
	//      closure for absorption/scattering; emission stays transient -- no chunk param). ----
	{
		const char* tm = "cst_s3_medium.RISEscene";
		{ std::ofstream o( tm );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "homogeneous_medium\n{\nname fog\nabsorption 0.1 0.1 0.1\nscattering 0.2 0.2 0.2\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\ninterior_medium fog\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tm ), "MD: loads variant + medium scene via CST" );
		Check( std::fabs( MedAbsR( *j, "fog" ) - 0.1 ) < 1e-6, "MD: fog absorption is 0.1 before the edit" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Medium, String( "fog" ) );
		Check( c.SetPropertyForCategory( Cat::Medium, String( "absorption" ), String( "0.5 0.5 0.5" ) ), "MD1: medium absorption edit applies" );
		Check( std::fabs( MedAbsR( *j, "fog" ) - 0.5 ) < 1e-6, "MD1: after edit, fog absorption is 0.5" );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "MD1: material edit applies (D2)" );
		Check( std::fabs( MedAbsR( *j, "fog" ) - 0.5 ) < 1e-6, "MD1: medium absorption edit SURVIVED the material D2 (data-loss closed)" );
		j->release();
		std::remove( tm );
	}

	// ---- CD: a CAMERA DRAG (orbit gesture) commits the NET pose to the CST at drag-end and SURVIVES a
	//      material D2 (data-loss closure for camera drags -- the pose params, not just panel SetCameraProperty). ----
	{
		const char* tc = "cst_s3_camdrag.RISEscene";
		{ std::ofstream o( tc );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "film\n{\nwidth 64\nheight 64\n}\n"
		       "pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tc ), "CD: loads variant + named-camera scene via CST" );
		SceneEditController c( *j, 0 );
		double pre[3]; CamLoc( *j, pre );
		// Simulate an orbit drag gesture (down -> move-right -> up).  OnPointerUp commits the net pose to the CST.
		c.SetTool( Tool::OrbitCamera );
		c.OnPointerDown( Point2( 100, 100 ) );
		c.OnPointerMove( Point2( 160, 100 ) );
		c.OnPointerUp(   Point2( 160, 100 ) );
		double orbited[3]; CamLoc( *j, orbited );
		Check( !LocEq( pre, orbited ), "CD: the orbit drag changed the camera eye position" );
		// Material D2 rebuilds the camera from the Document; the orbited pose must survive.
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "CD1: material edit applies (D2)" );
		double afterD2[3]; CamLoc( *j, afterD2 );
		Check( LocEq( orbited, afterD2 ), "CD1: camera orbit SURVIVED the material D2 (drag pose committed to CST, data-loss closed)" );
		j->release();
		std::remove( tc );
	}

	// ---- CADD: CLONING a camera on a CST scene INSERTS a faithful camera chunk into the retained Document, so the
	//      clone SURVIVES a material-edit D2 AND a save->reload; UNDO removes the chunk (the clone stays gone across a
	//      later D2); REDO re-inserts it.  Closes the round-5 data-loss for camera clones (CloneActiveCamera used to
	//      register the live camera but never record it in the CST).  The variant scene makes material edits take D2. ----
	{
		const char* tk = "cst_s3_camclone.RISEscene";
		{ std::ofstream o( tk );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"                                    // declares a variant -> material edits take D2
		       "film\n{\nwidth 64\nheight 64\n}\n"
		       "pinhole_camera\n{\nname cam\nlocation 0 0 7\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"   // NAMED + clonable
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tk ), "CADD: loads variant + named-camera scene via CST" );

		SceneEditController c( *j, 0 );
		// Select the source camera (the sole named one) so it's active, then clone it.
		c.SetSelection( Cat::Camera, String( "cam" ) );
		char buf[256]; buf[0] = '\0';
		Check( c.CloneActiveCamera( String( "cam_copy" ), buf, sizeof( buf ) ), "CADD: clone of the active camera succeeds" );
		const std::string cloneName( buf );
		Check( !cloneName.empty(), "CADD: the clone got a non-empty name" );

		// (1) The new camera exists on the LIVE scene.
		Check( CamExists( *j, cloneName.c_str() ), "CADD1: the cloned camera exists on the live scene" );
		// The clone is a faithful copy: same authored rest-location z (7).
		Check( std::fabs( NamedCamZ( *j, cloneName.c_str() ) - 7.0 ) < 1e-6, "CADD1: the clone copied the source location (z=7)" );

		// (2) It SURVIVES a material-edit D2 (the variant scene re-derives the whole Document; the clone must
		//     still be present because its chunk is now IN the Document).  RED-PROVE: disable the forward insert
		//     (ApplyCstInsertCameraChunk call in SceneEditor's AddCamera branch) and this D2 drops the clone.
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "CADD2: material edit applies (D2)" );
		Check( CamExists( *j, cloneName.c_str() ), "CADD2: the clone SURVIVED the material D2 (chunk recorded in the CST)" );
		Check( std::fabs( NamedCamZ( *j, cloneName.c_str() ) - 7.0 ) < 1e-6, "CADD2: the re-derived clone matches the original (faithful chunk)" );

		// (3) It SURVIVES save->reload: save the Document, then load into a FRESH Job -- the clone chunk must be there.
		const SaveResult res = c.RequestSave( std::string( tk ) );
		Check( res.status == SaveResult::Status::Saved, "CADD3: SaveEngine reported Saved" );
		j->release();
		Job* j2 = new Job();
		Check( j2->LoadAsciiSceneViaCst( tk ), "CADD3: reloads the saved file via CST" );
		Check( CamExists( *j2, cloneName.c_str() ), "CADD3: the clone PERSISTED through save->reload (CST chunk serialized)" );
		Check( std::fabs( NamedCamZ( *j2, cloneName.c_str() ) - 7.0 ) < 1e-6, "CADD3: the reloaded clone is faithful (z=7)" );
		j2->release();

		// (4) UNDO + (5) REDO on a fresh load (so the history is the single clone op).  Re-clone, then exercise undo/redo.
		{
			const char* tk2 = "cst_s3_camclone2.RISEscene";
			{ std::ofstream o( tk2 );
			  o << "RISE ASCII SCENE 6\n"
			       "scene_variant\n{\nname night\n}\n"
			       "film\n{\nwidth 64\nheight 64\n}\n"
			       "pinhole_camera\n{\nname cam\nlocation 0 0 7\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
			       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
			       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
			       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
			       "sphere_geometry\n{\nname g\nradius 1\n}\n"
			       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
			Job* ju = new Job();
			Check( ju->LoadAsciiSceneViaCst( tk2 ), "CADD: (undo/redo) loads via CST" );
			SceneEditController cu( *ju, 0 );
			cu.SetSelection( Cat::Camera, String( "cam" ) );
			char ub[256]; ub[0] = '\0';
			Check( cu.CloneActiveCamera( String( "cam_copy" ), ub, sizeof( ub ) ), "CADD: (undo/redo) clone succeeds" );
			const std::string un( ub );
			Check( CamExists( *ju, un.c_str() ), "CADD: the clone exists before undo" );

			// (4) UNDO -> the clone is gone live.  Then a material D2 must keep it gone (proves the chunk was REMOVED
			//     from the Document, not just the live camera).  RED-PROVE: disable the remove (ApplyCstRemoveCameraChunk
			//     in SceneEditor's revert AddCamera branch) and the D2 re-adds the clone from the still-present chunk.
			//     NOTE: the material edit below CLEARS the redo stack, so REDO (case 5) is exercised on a fresh load.
			cu.Undo();
			Check( !CamExists( *ju, un.c_str() ), "CADD4: undo removed the clone live" );
			cu.SetSelection( Cat::Material, String( "m" ) );
			Check( cu.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "CADD4: material edit applies (D2)" );
			Check( !CamExists( *ju, un.c_str() ), "CADD4: the clone STAYED gone across the D2 (chunk removed from the Document)" );
			ju->release();
			std::remove( tk2 );
		}

		// (5) REDO -> the clone is back (redo replays the forward AddCamera, re-inserting the chunk).  Exercised on a
		//     FRESH load with NO intervening edit (an intervening edit clears the redo stack).  Then a material D2
		//     must KEEP the redone clone (proves redo re-inserted the chunk into the Document, not just the live cam).
		{
			const char* tk3 = "cst_s3_camclone3.RISEscene";
			{ std::ofstream o( tk3 );
			  o << "RISE ASCII SCENE 6\n"
			       "scene_variant\n{\nname night\n}\n"
			       "film\n{\nwidth 64\nheight 64\n}\n"
			       "pinhole_camera\n{\nname cam\nlocation 0 0 7\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
			       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
			       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
			       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
			       "sphere_geometry\n{\nname g\nradius 1\n}\n"
			       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
			Job* jr = new Job();
			Check( jr->LoadAsciiSceneViaCst( tk3 ), "CADD: (redo) loads via CST" );
			SceneEditController cr( *jr, 0 );
			cr.SetSelection( Cat::Camera, String( "cam" ) );
			char rb[256]; rb[0] = '\0';
			Check( cr.CloneActiveCamera( String( "cam_copy" ), rb, sizeof( rb ) ), "CADD: (redo) clone succeeds" );
			const std::string rn( rb );
			cr.Undo();
			Check( !CamExists( *jr, rn.c_str() ), "CADD5: clone gone after undo (pre-redo)" );
			cr.Redo();
			Check( CamExists( *jr, rn.c_str() ), "CADD5: redo brought the clone back" );
			Check( std::fabs( NamedCamZ( *jr, rn.c_str() ) - 7.0 ) < 1e-6, "CADD5: the redone clone is faithful (z=7)" );
			// And the redone clone is back IN the Document: a material D2 keeps it.
			cr.SetSelection( Cat::Material, String( "m" ) );
			Check( cr.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "CADD5: material edit applies (D2)" );
			Check( CamExists( *jr, rn.c_str() ), "CADD5: the redone clone SURVIVED a material D2 (redo re-inserted the chunk)" );
			jr->release();
			std::remove( tk3 );
		}
		std::remove( tk );
	}

	// ---- CSG: a transform edit on a csg_object (pickable, but it authors only position/orientation -- NO matrix
	//      param) must be REFUSED on a CST scene, not applied-live-then-silently-lost.  csg_object's transform
	//      cannot be committed to the CST via `matrix`, so the editor refuses the edit up front (no divergence). ----
	{
		const char* tg = "cst_s3_csg.RISEscene";
		{ std::ofstream o( tg );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname a\ngeometry g\nmaterial m\nposition -1 0 0\n}\n"
		       "standard_object\n{\nname b\ngeometry g\nmaterial m\nposition 1 0 0\n}\n"
		       "csg_object\n{\nname cu\nobja a\nobjb b\noperation union\nmaterial m\nposition 0 0 0\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tg ), "CSG: loads variant + csg_object scene via CST" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "cu" ) );
		double before[16]; ObjMat16( *j, "cu", before );

		// CSG1: a csg POSITION edit (translate) routes via the csg `position` param and SURVIVES a material D2.
		Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ), "CSG1: csg position edit applies (translate is committable)" );
		double moved[16]; ObjMat16( *j, "cu", moved );
		Check( !Mat16Eq( before, moved ), "CSG1: the position edit moved the csg object" );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "CSG1: material edit applies (D2)" );
		double afterD2[16]; ObjMat16( *j, "cu", afterD2 );
		Check( Mat16Eq( moved, afterD2 ), "CSG1: csg position edit SURVIVED the material D2 (decomposed to position param; data-loss closed)" );

		// CSG2: a csg ORIENTATION edit (rotate) routes via the csg `orientation` param and survives a D2.
		c.SetSelection( Cat::Object, String( "cu" ) );
		Check( c.SetPropertyForCategory( Cat::Object, String( "orientation" ), String( "0 0 90" ) ), "CSG2: csg orientation edit applies (rotate is committable)" );
		double rotated[16]; ObjMat16( *j, "cu", rotated );
		Check( !Mat16Eq( moved, rotated ), "CSG2: the orientation edit rotated the csg object" );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p1" ) ), "CSG2: material edit applies (D2)" );
		double afterD2b[16]; ObjMat16( *j, "cu", afterD2b );
		Check( Mat16Eq( rotated, afterD2b ), "CSG2: csg orientation edit SURVIVED the material D2" );

		// CSG3: a csg SCALE edit is REFUSED (csg_object has no scale param -- not representable, would silently lose).
		c.SetSelection( Cat::Object, String( "cu" ) );
		double preScale[16]; ObjMat16( *j, "cu", preScale );
		Check( !c.SetPropertyForCategory( Cat::Object, String( "scale" ), String( "2 2 2" ) ), "CSG3: a csg SCALE edit is REFUSED (no scale param)" );
		double postScale[16]; ObjMat16( *j, "cu", postScale );
		Check( Mat16Eq( preScale, postScale ), "CSG3: the refused scale left the csg transform unchanged (no divergence)" );

		// CSG4: a csg rotation that lands on GIMBAL-LOCK (Y=90 deg, not decomposable to Euler position/orientation)
		// is REFUSED by the post-mutate decomposability check, leaving the csg transform unchanged (no divergence).
		// Red-provable: remove the post-mutate DecomposeRigid check and the Y=90 rotation applies live, then the
		// commit's decompose fails and a later D2 reverts it.
		c.SetSelection( Cat::Object, String( "cu" ) );
		double preGimbal[16]; ObjMat16( *j, "cu", preGimbal );
		Check( !c.SetPropertyForCategory( Cat::Object, String( "orientation" ), String( "0 90 0" ) ), "CSG4: a Y=90deg (gimbal-lock) csg rotation is REFUSED" );
		double postGimbal[16]; ObjMat16( *j, "cu", postGimbal );
		Check( Mat16Eq( preGimbal, postGimbal ), "CSG4: the refused gimbal rotation left the csg transform unchanged (no divergence)" );
		j->release();
		std::remove( tg );
	}

	// ---- SV: Slice 4 -- a CST-routed edit is SERIALIZED by the SaveEngine and survives a save->reload round-trip.
	//      The legacy byte-splice path would write the ORIGINAL bytes back (CST-load populates no source spans) and
	//      lose the edit; the CST branch in SaveEngine::Save serializes the retained Document instead. ----
	{
		const char* ts = "cst_s4_save.RISEscene";
		{ std::ofstream o( ts ); o << SCENE; }   // light `l` power 4 (+ a declared variant -> edits take D2)
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( ts ), "SV: loads via CST" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Light, String( "l" ) );
		Check( c.SetPropertyForCategory( Cat::Light, String( "power" ), String( "8" ) ), "SV: light power edit applies" );
		const SaveResult res = c.RequestSave( std::string( ts ) );
		Check( res.status == SaveResult::Status::Saved, "SV1: SaveEngine reported Saved" );
		j->release();
		// Reload the saved file into a FRESH Job: the edit must be present (proving the SaveEngine serialized the
		// CST Document, not the legacy byte-splice).  Red-provable: disable the CST branch in SaveEngine::Save and
		// the legacy path writes the original power-4 bytes -> this reload reads 4.
		Job* j2 = new Job();
		Check( j2->LoadAsciiSceneViaCst( ts ), "SV: reloads the saved file via CST" );
		Check( std::fabs( LightProp( *j2, "l", "power" ) - 8.0 ) < 1e-6, "SV1: the edit PERSISTED through save->reload (CST serialized, not byte-spliced)" );
		j2->release();
		// A second save with NO further edits is a NoOp (serialized bytes already on disk).
		Job* j3 = new Job();
		Check( j3->LoadAsciiSceneViaCst( ts ), "SV: reloads for the NoOp check" );
		SceneEditController c3( *j3, 0 );
		const SaveResult res3 = c3.RequestSave( std::string( ts ) );
		Check( res3.status == SaveResult::Status::NoOp, "SV2: re-saving an unedited CST scene is a NoOp (byte-identical)" );
		j3->release();

		// SV3: an in-place save REFUSES when the loaded file was modified externally after load -- the CST
		// re-serialize must NOT silently clobber those on-disk edits (reviewer P1).  Red-provable: disable the
		// in-place external-mod guard in SaveEngine::Save and the save reports Saved + overwrites the external edit.
		{
			const char* te = "cst_s4_extmod.RISEscene";
			{ std::ofstream o( te ); o << SCENE; }
			Job* je = new Job();
			Check( je->LoadAsciiSceneViaCst( te ), "SV3: loads via CST" );
			SceneEditController ce( *je, 0 );
			ce.SetSelection( Cat::Light, String( "l" ) );
			Check( ce.SetPropertyForCategory( Cat::Light, String( "power" ), String( "8" ) ), "SV3: light power edit applies" );
			// Simulate an EXTERNAL edit to the file on disk after load (a different SIZE -> a guaranteed mtime/size mismatch).
			{ std::ofstream o( te, std::ios::app ); o << "\n/* externally appended after load */\n"; }
			const SaveResult rese = ce.RequestSave( std::string( te ) );
			Check( rese.status == SaveResult::Status::Refused, "SV3: in-place save REFUSES on external modification (no clobber)" );
			// The on-disk external edit must still be present (the save did not overwrite it).
			{ std::ifstream in( te ); std::stringstream ss; ss << in.rdbuf(); const std::string disk = ss.str();
			  Check( disk.find( "externally appended" ) != std::string::npos, "SV3: the external edit is preserved on disk (not overwritten)" ); }
			je->release();
			std::remove( te );
		}

		// SV4: a SECOND in-place save in the same session is NOT falsely refused -- the first save RE-BASELINES the
		// file identity to what it wrote (and that survives the second edit's D2).  Red-provable: drop the post-save
		// RefreshCstLoadFileIdentity and the second save compares the just-written file against the stale load
		// identity -> Refused.
		{
			const char* tr = "cst_s4_resave.RISEscene";
			{ std::ofstream o( tr ); o << SCENE; }
			Job* jr = new Job();
			Check( jr->LoadAsciiSceneViaCst( tr ), "SV4: loads via CST" );
			SceneEditController cr( *jr, 0 );
			cr.SetSelection( Cat::Light, String( "l" ) );
			Check( cr.SetPropertyForCategory( Cat::Light, String( "power" ), String( "8" ) ), "SV4: first edit applies" );
			Check( cr.RequestSave( std::string( tr ) ).status == SaveResult::Status::Saved, "SV4: first in-place save Saved" );
			Check( cr.SetPropertyForCategory( Cat::Light, String( "power" ), String( "12" ) ), "SV4: second edit applies" );
			Check( cr.RequestSave( std::string( tr ) ).status == SaveResult::Status::Saved, "SV4: second in-place save is NOT falsely refused (identity re-baselined, survives D2)" );
			jr->release();
			Job* jr2 = new Job();
			Check( jr2->LoadAsciiSceneViaCst( tr ), "SV4: reloads" );
			Check( std::fabs( LightProp( *jr2, "l", "power" ) - 12.0 ) < 1e-6, "SV4: the second edit persisted" );
			jr2->release();
			std::remove( tr );
		}

		// SV5: after a Save-As, the identity RE-ANCHORS to the new target -- a re-save to it isn't refused, and an
		// external edit to the NEW file is now guarded.
		{
			const char* ta = "cst_s4_srcA.RISEscene"; const char* tb = "cst_s4_dstB.RISEscene";
			{ std::ofstream o( ta ); o << SCENE; }
			std::remove( tb );
			Job* ja = new Job();
			Check( ja->LoadAsciiSceneViaCst( ta ), "SV5: loads source A via CST" );
			SceneEditController ca( *ja, 0 );
			ca.SetSelection( Cat::Light, String( "l" ) );
			Check( ca.SetPropertyForCategory( Cat::Light, String( "power" ), String( "8" ) ), "SV5: edit applies" );
			Check( ca.RequestSave( std::string( tb ) ).status == SaveResult::Status::Saved, "SV5: Save-As to a new path B Saved" );
			Check( ca.SetPropertyForCategory( Cat::Light, String( "power" ), String( "12" ) ), "SV5: edit applies again" );
			Check( ca.RequestSave( std::string( tb ) ).status == SaveResult::Status::Saved, "SV5: re-save to B is not refused (identity re-anchored to B)" );
			{ std::ofstream o( tb, std::ios::app ); o << "\n/* externally appended to B */\n"; }
			Check( ca.SetPropertyForCategory( Cat::Light, String( "power" ), String( "16" ) ), "SV5: edit applies once more" );
			Check( ca.RequestSave( std::string( tb ) ).status == SaveResult::Status::Refused, "SV5: an external edit to the Save-As target B is now guarded (Refused)" );
			ja->release();
			std::remove( ta ); std::remove( tb );
		}
		std::remove( ts );
	}

	std::remove( tmp );
	std::cout << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
