//////////////////////////////////////////////////////////////////////
//
//  SMSDomainReplayFixture.h - Shared scene/geometry/intersection
//    fixture helpers for SMSDomainReplayTest and related geometry/SMS
//    tests.
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_TESTS_SMS_DOMAIN_REPLAY_FIXTURE_H
#define RISE_TESTS_SMS_DOMAIN_REPLAY_FIXTURE_H

#include "SMSRenderTestSupport.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/PathTracingShaderOp.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include <sstream>
#include <cstring>
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"

inline bool Near( double a, double b )
{
	return std::fabs( a - b ) < 1e-11;
}

inline std::string Materials( bool composite )
{
	std::string s = "RISE ASCII SCENE 7\nuniformcolor_painter\n{\n name white\n color 1 1 1\n}\n"
		"scalar_painter\n{\n name triple\n values 1.3 1.5 1.9\n}\n"
		"perfectrefractor_material\n{\n name glass\n refractance white\n ior triple\n}\n"
		"perfectrefractor_material\n{\n name inner\n refractance white\n ior 1.2\n}\n";
	if( composite ) s += "composite_material\n{\n name layers\n top glass\n bottom inner\n thickness 0.02\n extinction 0\n}\n";
	return s;
}

inline std::string Mesh( bool closed, bool reverse )
{
	std::ostringstream s;
	s << "indexedmesh_geometry\n{\n name shape\n double_sided TRUE\n face_normals TRUE\n";
	const Point3 vertices[] = {
		Point3( -1, -1, -1 ), Point3( 1, -1, -1 ), Point3( 1, 1, -1 ), Point3( -1, 1, -1 ),
		Point3( -1, -1,  1 ), Point3( 1, -1,  1 ), Point3( 1, 1,  1 ), Point3( -1, 1,  1 )
	};
	for( const auto& p : vertices ) s << " vertex " << p.x << ' ' << p.y << ' ' << p.z << '\n';
	for( int i = 0; i < 8; ++i ) s << " uv " << ( i % 2 ) << ' ' << ( ( i / 2 ) % 2 ) << '\n';
	const int triangles[][3] = {
		{0,2,1},{0,3,2},{4,5,6},{4,6,7},
		{0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}
	};
	for( int i = 0; i < ( closed ? 12 : 2 ); ++i )
		s << " triangle " << triangles[i][0] << ' '
		  << triangles[i][reverse ? 2 : 1] << ' ' << triangles[i][reverse ? 1 : 2] << '\n';
	return s.str() + "}\n";
}

struct LoadedScene
{
	IJobPriv* job = nullptr;
	explicit LoadedScene( const std::string& text )
	{
		const auto path = TestTempPath( "sms_domain_" + std::to_string( ::getpid() ) + ".RISEscene" );
		{ std::ofstream out( path ); out << text; }
		if( RISE_CreateJobPriv( &job ) ) Check( job->LoadAsciiSceneViaCst( path.c_str() ), "domain fixture parses" );
		std::remove( path.c_str() );
		if( job ) job->GetScene()->GetObjects()->PrepareForRendering();
	}
	~LoadedScene() { safe_release( job ); }
	const IScene& Scene() const { return *job->GetScene(); }
	const IObject* Object( const char* name ) const { return Scene().GetObjects()->GetItem( name ); }
};

inline RayIntersection Hit(
	const IObject& object,
	Point3 origin,
	Vector3 direction,
	const RasterizerState& raster = nullRasterizerState )
{
	RayIntersection hit( Ray( origin, direction ), raster );
	object.IntersectRay( hit, RISE_INFINITY, true, true, false );
	Check( hit.geometric.bHit, "actual geometry intersection" );
	return hit;
}

#endif
