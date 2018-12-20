//////////////////////////////////////////////////////////////////////
//
//  WorkerRenderer.h - The renderer on the worker side of things
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WORKER_RENDERER_
#define WORKER_RENDERER_

class WorkerRenderer;

#include "SchedulerConnection.h"
#include "../Interfaces/IObject.h"
#include "../Utilities/Reference.h"
#include "../Utilities/String.h"
#include "../RISE/RISE.h"
#include <deque>

class WorkerRenderer : public virtual Implementation::Reference
{
public:
	RISE*					pRise;
	Point3D					ptPosition;

protected:
	virtual ~WorkerRenderer();

	unsigned int			nWorkerID;
	SchedulerConnection*	pScheduler;
	IObject*				pMesh;

public:
	WorkerRenderer( const char * szScene, const std::deque<MODEL_INFO>& models, SchedulerConnection* pScheduler_ );

	inline SchedulerConnection* GetScheduler(){ return pScheduler; };

	void IntersectObjects( RayIntersection&	ri );
	void IntersectObjectsFinishIncomplete( RayIntersection& ri, IMemoryBuffer* mb );
	bool ShadowCheck( const Ray& ray, Scalar maxdist );

	// Tells us to render that particular ray
	virtual void Shade( RayIntersection& ri, IFXColor& c );
	virtual void Render( const Ray& ray, const unsigned int x, const unsigned int y );	
	virtual void FinishIncompleteRay( const Ray& ray, const unsigned int x, const unsigned int y, IMemoryBuffer* mb );
};

#endif

