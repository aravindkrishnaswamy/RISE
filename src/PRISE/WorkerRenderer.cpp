//////////////////////////////////////////////////////////////////////
//
//  WorkerRenderer.cpp - Implements the renderer on the worker side
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WorkerRenderer.h"
#include "../Interfaces/ILog.h"
#include "../AsciiSceneParser.h"
#include "../PRISE/PRISEMeshGeometry.h"
#include "../Objects/Object.h"
#include "../Materials/MaterialContainer.h"
#include "../Utilities/Time.h"

using namespace Implementation;

WorkerRenderer::WorkerRenderer( const char * szScene, const std::deque<MODEL_INFO>& models, SchedulerConnection* pScheduler_ ) : 
  pRise( 0 ), 
  pScheduler( pScheduler_ ),
  pMesh( 0 )
{
	if( pScheduler ) {
		pScheduler->AddRef();
	}

	GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, "WorkerRenderer:: Working on scene [%s]", szScene );

	pRise = new RISE();

	ISceneParser* sceneParser = new Implementation::AsciiSceneParser( szScene );
	if( !sceneParser->ParseAndLoadScene( pRise ) ) {
		GlobalLog()->PrintEasyError( "WorkerRenderer:: Given scene file doesn't exist on this machine, aborting" );
		sceneParser->RemoveRef();
	} else {

		Job*	pJob = pRise->GetJob();

		ptPosition = pJob->pScene->GetCamera()->GetLocation();

		// Load the given models properly
		// !@@ Support only one mesh
		std::deque<MODEL_INFO>::const_iterator	it = models.begin();
//		for( it=models.begin(); it!=models.end(); it++ ) {
			PRISEMeshGeometry*		pGeom = new PRISEMeshGeometry(0,0,0,0);
			GlobalLog()->PrintNew( pGeom, __FILE__, __LINE__, "geometry" );

			MemoryBuffer*			pBuffer = new MemoryBuffer( it->szFilename.c_str() );
			GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );

			pGeom->Deserialize( *pBuffer );
			pBuffer->RemoveRef();

			Object*	obj = new Object( pGeom );
			GlobalLog()->PrintNew( obj, __FILE__, __LINE__, "object" );

			obj->SetPosition( it->ptPosition );
			obj->SetOrientation( it->vOrientation * DEG_TO_RAD );
			obj->FinalizeTransformations();

			MaterialContainer* pMaterial = new MaterialContainer( it->material.c_str(), pJob->pMatManager );
			GlobalLog()->PrintNew( pMaterial, __FILE__, __LINE__, "material container" );
			obj->AssignMaterial( pMaterial );
			pMaterial->RemoveRef();

			pMesh = obj;

			pGeom->RemoveRef();
//		}
	}
}

WorkerRenderer::~WorkerRenderer( )
{
	if( pRise ) {
		pRise->RemoveRef();
		pRise = 0;
	}

	if( pScheduler ) {
		pScheduler->RemoveRef();
		pScheduler = 0;
	}

	if( pMesh ) {
		pMesh->RemoveRef();
		pMesh = 0;
	}
}

void WorkerRenderer::IntersectObjects( RayIntersection&	ri )
{
	const IScene*			pScene = pRise->GetJob()->pScene;
	pScene->GetObjects()->IntersectRay( ri, true, false, false );

	RayIntersection			myri;
	myri.geometric.ray = ri.geometric.ray;
	pMesh->IntersectRay( myri, ri.geometric.range, true, false, false );

	if( myri.geometric.bHit ) {
		ri = myri;
	} else if( (MemoryBuffer*)myri.geometric.custom != 0 ) {
		ri.geometric.bHit = false;
		ri.geometric.custom = (void*)(MemoryBuffer*)myri.geometric.custom;
	}
}

bool WorkerRenderer::ShadowCheck( const Ray& ray, Scalar maxdist )
{
	const IScene*			pScene = pRise->GetJob()->pScene;
	if( !pScene->GetObjects()->IntersectRay_IntersectionOnly( ray, maxdist, true, false ) ) {
		return pMesh->IntersectRay_IntersectionOnly( ray, maxdist, true, false );
	}

	return true;
}

void WorkerRenderer::Shade( RayIntersection& ri, IFXColor& c )
{
	// If there is an intersection modifier, then get it to modify
	// the intersection information,
	if( ri.pModifier ) {
		ri.pModifier->Modify( ri.geometric );
	}

	OrthonormalBasis3D	onb;			// Construct the orthonormal basis
	onb.CreateFromW		( ri.geometric.vNormal );

	// Account for lighting
	IFXPel		diffuse, specular;
	{
		// Very basic lighting...
		// Compute direct lighting

		// Ignore all lights in the scene, juse use a point light from the viewer
		Vector3D	vToLight( ptPosition, ri.geometric.ptIntersection );
		Scalar		fDistFromLight = ~vToLight;
		vToLight.Normalize();
		
		Scalar		fDot = vToLight % ri.geometric.vNormal;
		if( fDot > 0.0 ) {
		
			// Shadow check
			Ray		rayToLight;
			rayToLight.origin = ri.geometric.ptIntersection;
			rayToLight.dir = vToLight;

//			if( !ShadowCheck( rayToLight, fDistFromLight ) ) {
			
				// No shadow
				OrthonormalBasis3D	onb;
				onb.CreateFromW( ri.geometric.vNormal );

				if( ri.pMaterial ) {
					diffuse = (fDot * ri.pMaterial->getBRDF()->value( vToLight, ri.geometric, onb ));
				} else {
					diffuse = IFXPel(fDot,fDot,fDot);
				}
//			}				
		}

		// Modulate the color of the material by the diffuse light
		c.base = c.base * diffuse;
	}
}

void WorkerRenderer::Render( const Ray& ray, const unsigned int x, const unsigned int y )
{
	IFXColor	c = IFXColor( 0,0,0,0 );

	// Cast the ray into the scene
	RayIntersection	ri;
	ri.geometric.bHit = false;

	ri.geometric.ray = ray;
	IntersectObjects( ri );
	if( ri.geometric.bHit )
	{
		c = IFXColor( COLORSPACE::White );
		Shade( ri, c );

	} else if( (MemoryBuffer*)ri.geometric.custom != 0 ) {
		MemoryBuffer*	pTraversalBuf = (MemoryBuffer*)ri.geometric.custom;

		// The could have been a hit, but went into an octant node for which 
		// we don't have any data, thus we send it back to the scheduler to let it 
		// decide where to send it
		if( !pScheduler->SendUnresolvedRay( ray, x, y, pTraversalBuf ) ) {
			GlobalLog()->PrintEasyError( "WorkerRenderer:: Could not send unresolved ray to scheduler" );
		}
	}

	// Miss

	if( !pScheduler->SendResult( x, y, c ) ) {
		GlobalLog()->PrintEasyError( "Error, couldn't send result to server" );
	}
}

void WorkerRenderer::IntersectObjectsFinishIncomplete( RayIntersection& ri, IMemoryBuffer* mb )
{
	ri.geometric.custom = (void*)mb;
	pMesh->IntersectRay( ri, INFINITY, true, false, false );

	if( !ri.geometric.bHit && (MemoryBuffer*)ri.geometric.custom != 0 ) {
		return;
	}

	const IScene*			pScene = pRise->GetJob()->pScene;
	RayIntersection			myri;
	myri.geometric.ray = ri.geometric.ray;

	pScene->GetObjects()->IntersectRay( myri, true, false, false );

	if( myri.geometric.bHit && myri.geometric.range < ri.geometric.range ) {
		ri = myri;
	}
}

void WorkerRenderer::FinishIncompleteRay( const Ray& ray, const unsigned int x, const unsigned int y, IMemoryBuffer* mb )
{
	// Finish this incomplete ray
	RayIntersection	ri;
	ri.geometric.bHit = false;
	ri.geometric.ray = ray;
	IntersectObjectsFinishIncomplete( ri, mb );

	if( ri.geometric.bHit )
	{
		IFXColor c = IFXColor( COLORSPACE::White );
		Shade( ri, c );
		
		// We had a hit, so report it to the scheduler through our communicator
		if( !pScheduler->SendResult( x, y, c ) ) {
			GlobalLog()->PrintEasyError( "Error, couldn't send result to server" );
		}
	} else if( (MemoryBuffer*)ri.geometric.custom != 0 ) {
		MemoryBuffer*	pTraversalBuf = (MemoryBuffer*)ri.geometric.custom;

		// The could have been a hit, but went into an octant node for which 
		// we don't have any data, thus we send it back to the scheduler to let it 
		// decide where to send it
		if( !pScheduler->SendUnresolvedRay( ray, x, y, pTraversalBuf ) ) {
			GlobalLog()->PrintEasyError( "WorkerRenderer:: Could not send unresolved ray to scheduler" );
		}
	}
	// Miss
}
