//***************************************************************************
// rise3dsmax - [rise3dsmax.cpp] Sample Plugin Renderer for 3D Studio MAX.
// 
// By Christer Janson - Kinetix
//
// Description:
// Implementation of main render class
//
//***************************************************************************

#include "pch.h"
#include "maxincl.h"
#include "rise3dsmax.h"
#include "rendutil.h"
#include "refenum.h"

//===========================================================================
//
// Class RiseRenderer
//
//===========================================================================

//***************************************************************************
// This is called on File/Reset and we should 
// reset the class variables here.
//***************************************************************************

RiseRenderer::RiseRenderer()
{
	rendParams.renderer = this;
	strcpy( szRenderSettingsFile, "" );
	strcpy( szSupplementarySettingsFile, "" );
}

//***************************************************************************
// This is called on File/Reset and we should 
// reset the class variables here.
//***************************************************************************

void RiseRenderer::ResetParams()
{
	DebugPrint("**** Resetting parameters.\n");

	rendParams.	SetDefaults();
}

//***************************************************************************
// Standard Animatable method
//***************************************************************************

void RiseRenderer::DeleteThis()
{
	// Turf the global log at this point
	RISE::GlobalLogCleanupAndShutdown();
	delete this;
}

//***************************************************************************
// Standard Animatable method
//***************************************************************************

Class_ID RiseRenderer::ClassID()
{
	return RISEREND_CLASS_ID;
}

//***************************************************************************
// Standard Animatable method
//***************************************************************************

void RiseRenderer::GetClassName(TSTR& s)
{
	s = RENDERNAME;
}

//***************************************************************************
// Open the renderer.
// This is called when the rendering is first initiated.
// We should do a couple of things here:
// * Grab the parameters passed in and set our own representation
//   of these parameters
// * Enumerate the scene to create a list of all nodes and lights
// * Get hold of the atmospheric effects used
// * Get hold of all materials used.
// * Call RenderBegin() on all objects
//***************************************************************************

int RiseRenderer::Open(INode *scene,INode *vnode,ViewParams* viewPar,RendParams& rpar,HWND hwnd,DefaultLight* defaultLights,int numDefLights)
{
	int idx;

	// Important!! This has to be done here in MAX Release 2!
	// Also enable it again in Renderer::Close()
	GetCOREInterface()->DisableSceneRedraw();

	if (!rpar.inMtlEdit) {
		BroadcastNotification(NOTIFY_PRE_RENDER, (void*)(RendParams*)&rpar);	// skk
	}

	// Get options from RenderParams
	// These are the options that are common to all renderers
	rendParams.bVideoColorCheck = rpar.colorCheck;
	rendParams.bForce2Sided = rpar.force2Side;
	rendParams.bRenderHidden = rpar.rendHidden;
	rendParams.bSuperBlack = rpar.superBlack;
	rendParams.bRenderFields = rpar.fieldRender;
	rendParams.bNetRender = rpar.isNetRender;
	rendParams.rendType = rpar.rendType;

	// Default lights
	rendParams.nNumDefLights = numDefLights;
	rendParams.pDefaultLights = defaultLights;

	// Support Render effects
	rendParams.effect = rpar.effect;

	// Flag we use when reporting errors
	bFirstFrame = TRUE;
	bUvMessageDone = FALSE;

	// Initialize node counter
	nCurNodeID = -1;  //skk

	// Couldn't hurt to initalize these tables...
	ilist = NULL;
	instTab.ZeroCount();
	instTab.Shrink();
	lightTab.ZeroCount();
	lightTab.Shrink();
	mtls.ZeroCount();
	mtls.Shrink();

	// Get the root of the node hierarchy
	pScene = scene;

	// Viewnode is given if our view is a camera or a light
	pViewNode = vnode;
	theView.pRendParams = &rendParams;

	// Viewpar is there if we render a viewport
	if( viewPar ) {
		view = *viewPar;
	}


	// Enumerate the nodes in the scene
	// nodeEnum will fill in the rNodeTab for us.
	// Please note that the scene itself is not a true node, it is a
	// place holder whose children are the top level nodes..
	DebugPrint("**** Scanning for nodes.\n");
	for( idx = 0; idx < scene->NumberOfChildren(); idx++ ) {
		NodeEnum(scene->GetChildNode(idx));
	}

	DebugPrint("\tFound %d nodes\n", instTab.Count());
	DebugPrint("\tFound %d lights\n", lightTab.Count());

	// If there are no lights in the scene
	// we should use the default lights if we got them.
	if( lightTab.Count() == 0 && defaultLights ) {
		for( idx = 0; idx < numDefLights; idx++ ) {
			RenderLight* rl = new RenderLight(&defaultLights[idx]);
			lightTab.Append(1, &rl);
		}
		DebugPrint("\tUsing %d default lights\n", lightTab.Count());
	}

	// The "top atmospheric" called RenderEnvironment is passed as a member
	// of RendParams.
	// All atmospheric effects are referenced by the RenderEnvironment so we
	// only need to evaluate this "RenderEnvironment" in order to catch all
	// Atmospherics.
	rendParams.atmos = rpar.atmos;

	// The environment Map is a pointer to a Texmap
	rendParams.envMap = rpar.envMap;

	// Add any texture maps in the atmosphere or environment to the mtls list
	GetMaps getmaps(&mtls);

	if( rendParams.atmos ) {
		EnumRefs(rendParams.atmos,getmaps);
	}

	if( rendParams.envMap ) {
		EnumRefs(rendParams.envMap,getmaps);
	}

	BeginThings();				// Call RenderBegin() on all objects

	// Indicate that we have been opened.
	// A developer can initialize the renderer from the interface class
	// and we want to make sure that he has called Open() first.
	bOpen = TRUE;

	return 1; 	
}

//***************************************************************************
// Close is the last thing called after rendering all frames.
// Things to do here includes:
// * Deleting anything we have allocated.
// * Call RenderEnd() on all objects
//***************************************************************************

void RiseRenderer::Close(HWND hwnd)
{
	int idx;
	DebugPrint("**** Renderer going down.\n");

	EndThings();				// Call RenderEnd() on all objects

	for (idx=0; idx<instTab.Count(); idx++) {
		delete instTab[idx];
	}
	instTab.ZeroCount();
	instTab.Shrink();

	for (idx=0; idx<lightTab.Count(); idx++) {
		delete lightTab[idx];
	}
	lightTab.ZeroCount();
	lightTab.Shrink();

	// Shouldn't delete materials. The Dummy materials are already deleted,
	// and the rest of them aren't allocated/created by us.
	mtls.ZeroCount();
	mtls.Shrink();

	if (!rendParams.inMtlEdit) {
		BroadcastNotification(NOTIFY_POST_RENDER);	// skk
		}

	// Important!! Don't forget to enable screen redraws when the renderer closes.
	GetCOREInterface()->EnableSceneRedraw();
	bOpen = FALSE;
}

//***************************************************************************
// Call RenderBegin() on all objects.
// We need to call this on each object in the reference hierarchy.
// This needs to be done in order to let the object prepare itself
// for rendering.
// Particle systems for example will change to the number of
// particles used to rendering (instead of for viewport),
// and the optimize modifier have different options for 
// viewport and rendering as well.
//***************************************************************************

void RiseRenderer::BeginThings()
{
	int idx;

	ClearFlags clearFlags;
	BeginEnum beginEnum(rendParams.time);

	// First clear the A_WORK1 flag for each object
	for (idx = 0; idx < instTab.Count(); idx++) {
		ReferenceMaker* rm = instTab[idx]->GetINode();
		EnumRefs(rm, clearFlags);
	}

	// Clear reference hierarchy from Atmospherics
	if( rendParams.atmos ) {
		EnumRefs(rendParams.atmos, clearFlags);
	}

	// Clear reference hierarchy from Environment map
	if( rendParams.envMap ) {
		EnumRefs(rendParams.envMap, clearFlags);
	}

	// Call RenderBegin() and set the A_WORK1 flag on each object.
	// We need to set the flag so we don't call RenderBegin on the
	// same object twice.
	for( idx = 0; idx < instTab.Count(); idx++ ) {
		ReferenceMaker* rm = instTab[idx]->GetINode();
		EnumRefs(rm, beginEnum);
	}

	// reference hierarchy from Atmospherics
	if( rendParams.atmos ) {
		EnumRefs(rendParams.atmos, beginEnum);
	}

	// reference hierarchy from Environment map
	if( rendParams.envMap ) {
		EnumRefs(rendParams.envMap, beginEnum);
	}
}

//***************************************************************************
// Call RenderEnd() on all objects. See above how we called
// RenderBegin() for information.
//***************************************************************************

void RiseRenderer::EndThings()
{
	int idx;

	ClearFlags clearFlags;
	EndEnum endEnum(rendParams.time);

	for (idx = 0; idx < instTab.Count(); idx++) {
		ReferenceMaker* rm = instTab[idx]->GetINode();
		EnumRefs(rm, clearFlags);
	}

	if (rendParams.atmos)
		EnumRefs(rendParams.atmos, clearFlags);

	if (rendParams.envMap)
		EnumRefs(rendParams.envMap, clearFlags);


	for (idx = 0; idx < instTab.Count(); idx++) {
		ReferenceMaker* rm = instTab[idx]->GetINode();
		EnumRefs(rm, endEnum);
	}

	if (rendParams.atmos)
		EnumRefs(rendParams.atmos, endEnum);

	if (rendParams.envMap)
		EnumRefs(rendParams.envMap, endEnum);
}

//***************************************************************************
// This is used as a callback for aborting a potentially lengthy operation.
//***************************************************************************

class MyCallback: public CheckAbortCallback
{
	RiseRenderer *renderer;
public:
	MyCallback(RiseRenderer *r) { renderer = r; }
	virtual BOOL Check() { return renderer->CheckAbort(0,0); }
	virtual BOOL Progress(int done, int total ) { return renderer->CheckAbort(done,total); }
	virtual void SetTitle(const TCHAR *title) { renderer->SetProgTitle(title); }
};

int RiseRenderer::CheckAbort(int done, int total) {
	if( rendParams.progCallback ) {
		if (rendParams.progCallback->Progress(done,total)==RENDPROG_ABORT) {
			return 1;
		}
	}
	return 0;
}

void RiseRenderer::SetProgTitle(const TCHAR *title) {
	if( rendParams.progCallback ) { 
		rendParams.progCallback->SetTitle(title);
	}
}

//***************************************************************************
// Render() is called by MAX to render each frame.
// We get a time, an output bitmap, some parameters and a progress
// callback passed into us here.
//***************************************************************************

int RiseRenderer::Render(TimeValue t, Bitmap* tobm, FrameRendParams &frp, HWND hwnd, RendProgressCallback* prog, ViewParams* viewPar)
{
	int i;
	int nExitStatus = 1;

	if (!tobm || !bOpen) {
		return 0; // No output bitmap, not much we can do.
	}
	
	DebugPrint("**** Rendering frame. TimeValue: %d.\n", t);

	// Update progress window
	if (prog) {
		prog->SetTitle("Preparing to render...");
	}

	// Render progress callback
	rendParams.progCallback = prog;	

	// Setup ViewParams:
	rendParams.devWidth = tobm->Width();
	rendParams.devHeight = tobm->Height();
	rendParams.devAspect = tobm->Aspect();

	// These are moved from rendparams to FrameRendParams for R3
	rendParams.nRegxmin = frp.regxmin;
	rendParams.nRegymin = frp.regymin;
	rendParams.nRegxmax = frp.regxmax;
	rendParams.nRegymax = frp.regymax;

	// Get the frame
	rendParams.time = t;
	// Get the FrameRenderParams. These are parameters that can be animated
	// so they are different every frame
	rendParams.pFrp = &frp;

	// Viewpar is there if we render a viewport
	if( viewPar ) {
		view = *viewPar;
	}

	// Setup the view parameters
	if( pViewNode ) {
		GetViewParams(pViewNode, view, t);
	}

	rendParams.ComputeViewParams(view);

	// This renderer supports NORMAL or REGION rendering.
	// You should also support SELECTED, and BLOWUP
	if (rendParams.rendType == RENDTYPE_REGION) {
		if (rendParams.nRegxmin<0) rendParams.nRegxmin = 0;
		if (rendParams.nRegymin<0) rendParams.nRegymin = 0;
		if (rendParams.nRegxmax>rendParams.devWidth)
			rendParams.nRegxmax = rendParams.devWidth;
		if (rendParams.nRegymax>rendParams.devHeight)
			rendParams.nRegymax = rendParams.devHeight;
		rendParams.nMinx = rendParams.nRegxmin;
		rendParams.nMiny = rendParams.nRegymin;
		rendParams.nMaxx = rendParams.nRegxmax;
		rendParams.nMaxy = rendParams.nRegymax;
	}
	else {
		rendParams.nMinx = 0;
		rendParams.nMiny = 0;
		rendParams.nMaxx = rendParams.devWidth;
		rendParams.nMaxy = rendParams.devHeight;
	}

	// We need to scan and manually load each map in the system.
	// We will only report any errors on the first frame so we pass in
	// a flag indicating if this is the first frame
	// If some maps are missing this method will ask the user if
	// rendering should continue or be aborted.
	if( !LoadMapFiles(t, hwnd, bFirstFrame) ) {
		return 0;
	}

	// Update the Atmospheric effects
	if( rendParams.atmos ) {
		rendParams.atmos->Update(t, FOREVER);
	}

	// Update the environment map
	if( rendParams.envMap ) {
		rendParams.envMap->Update(t, FOREVER);
	}

	// Update the Materials.
	// Only top-level materials need to be updated.
	// Also we get the mesh for all nodes and count the number
	// of faces (to give status information to the user)
	for( i = 0; i < instTab.Count(); i++ ) {
		instTab[i]->mtl->Update(t, FOREVER);
		instTab[i]->Update(t, theView, this);
	}

	// Update the Lights for every frame
	// The light descriptor updates its internal transformation here so
	// if you transform the light nodes into camera space in your renderer
	// you need to call Update() on the light while it is still in 
	// world space.
	// In this renderer we don't transform lights into camera space so
	// this is not a problem here
	for( i = 0; i < lightTab.Count(); i++ ) {
		lightTab[i]->Update(t, this);
		lightTab[i]->UpdateViewDepParams(rendParams.worldToCam);
	}

	// Transform geometry from object space to Camera (view) space	
	for (i = 0; i < instTab.Count(); i++) {
		instTab[i]->UpdateViewTM(view.affineTM);
		instTab[i]->mesh->buildRenderNormals();
	}

	BroadcastNotification(NOTIFY_PRE_RENDERFRAME, (void*)(RenderGlobalContext*)&rendParams);

	// We don't build map files
	if (!BuildMapFiles(t))	{
		return 0;
	}

	// Display something informative in the progress bar
	if (prog) {
		prog->SetTitle("Rendering...");
		prog->SetSceneStats(lightTab.Count(), 0, 0, instTab.Count(), nNumFaces);
		nNumFaces = 0;	// reset the face count
	}

	// And now the moment we've all been waiting for (drumroll please)....

	nExitStatus = RenderImage(rendParams, t, tobm, prog, hwnd);

	BroadcastNotification(NOTIFY_POST_RENDERFRAME, (void*)(RenderGlobalContext*)&rendParams);

	// Write RendInfo to output bitmap
	RenderInfo* ri = tobm->AllocRenderInfo();
	if (ri) {
		ri->projType = rendParams.projType?ProjParallel:ProjPerspective;
		ri->kx = rendParams.xscale;
		ri->ky = rendParams.yscale;
		ri->xc = (float)rendParams.xc;
		ri->yc = (float)rendParams.xc;
		ri->fieldRender = FALSE;	// We don't support field rendering...
		ri->fieldOdd = FALSE;		// We don't support field rendering...
		ri->renderTime[0] = rendParams.time;
		ri->worldToCam[0] = rendParams.worldToCam;
		ri->camToWorld[0] = rendParams.camToWorld;
	}

	bool needFinalRefresh = false;

	// Do render effects here
	if (nExitStatus && rendParams.effect&&rendParams.effect->Active(t))  {
		MyCallback cb(this);
		rendParams.effect->Apply(t, tobm, &rendParams, &cb);
		tobm->RefreshWindow(NULL);
		if (tobm->GetWindow()) {
			UpdateWindow(tobm->GetWindow());
		}
		needFinalRefresh = false;
	}

	if (needFinalRefresh) {
		tobm->RefreshWindow(NULL);
		if (tobm->GetWindow()) {
			UpdateWindow(tobm->GetWindow());
		}
	}

	// Now it's not the first frame anymore
	bFirstFrame = FALSE;

	// Display something else in the progress bar
	if (prog) {
		prog->SetTitle("Done.");
	}

	return nExitStatus;
}

//***************************************************************************
// Get the parameters for the view
//***************************************************************************

void RiseRenderer::GetViewParams(INode* vnode, ViewParams& vp, TimeValue t)
{
	Interval iv;
	const ObjectState& os = vnode->EvalWorldState(t);
	switch (os.obj->SuperClassID()) {
		case CAMERA_CLASS_ID: {
			// compute camera transform
			CameraState cs;
			CameraObject *cam = (CameraObject *)os.obj;
			iv.SetInfinite();

			// Grab the Camera transform from the node.
			Matrix3 camtm = vnode->GetObjTMAfterWSM(t,&iv);

			RemoveScaling(camtm);
			vp.affineTM = Inverse(camtm);
			cam->EvalCameraState(t,iv,&cs);
			if (cs.manualClip) {
				vp.hither = cs.hither;
				vp.yon = cs.yon;
			}
			else {
			    vp.hither	= 0.1f;
		    	vp.yon	  	= -BIGFLOAT;
			}
			vp.projType = PROJ_PERSPECTIVE;
			vp.fov = cs.fov;
			vp.nearRange = rendParams.nearRange = cs.nearRange;
			vp.farRange = rendParams.farRange = cs.farRange;
			}
			break;
		case LIGHT_CLASS_ID: {

			iv.SetInfinite();
			Matrix3 ltm = vnode->GetObjTMAfterWSM(t,&iv);
			vp.affineTM = Inverse(ltm);
			
			LightState ls;
			LightObject *ltob = (LightObject *)os.obj;
			ltob->EvalLightState(t,iv,&ls);

			float aspect = ls.shape?1.0f:ls.aspect;
			switch(ls.type) {
				case SPOT_LGT:			
					vp.projType = PROJ_PERSPECTIVE;      
					vp.fov = DegToRad(ls.fallsize);  
					vp.fov = 2.0f* (float)atan(tan(vp.fov*0.5f)*sqrt(aspect));
					rendParams.devAspect = (float(rendParams.devHeight)/float(rendParams.devWidth))*aspect;
					break;
				case DIRECT_LGT:
					vp.projType = PROJ_PARALLEL; 
					rendParams.devAspect = (float(rendParams.devHeight)/float(rendParams.devWidth))*aspect;
					break;
			}
		    vp.hither	= 0.1f;
	    	vp.yon	  	= -BIGFLOAT;  // so  it doesn't get used

			vp.nearRange = rendParams.nearRange = 0.0f;
			vp.farRange = rendParams.farRange = 500.0f;
			}
			break;
		default:
			vp.nearRange = rendParams.nearRange = 0.0f;
			vp.farRange = rendParams.farRange = 500.0f;
			break;
	}	
}

//***************************************************************************
// This method enumerates the external maps needed by the objects
// and materials.
// If a map is missing its name is appended to a nameTab and we
// report the problem.
// If a map is found it is appended to a map list and
// when done, we manually load each map into the system.
// See the rendutil module for the enumerators.
//***************************************************************************

#define ENUMMISSING FILE_ENUM_MISSING_ONLY|FILE_ENUM_1STSUB_MISSING

int RiseRenderer::LoadMapFiles(TimeValue t, HWND hWnd, BOOL firstFrame)
{
	NameTab mapFiles;
	CheckFileNames checkNames(&mapFiles);

	int i;

	// Check the nodes
	for (i = 0; i < instTab.Count(); i++) {
		instTab[i]->GetINode()->EnumAuxFiles(checkNames, ENUMMISSING);
	}

	// Check the lights
	for (i = 0; i < lightTab.Count(); i++) {
		if (lightTab[i]->pLight != NULL) {
			lightTab[i]->pLight->EnumAuxFiles(checkNames, ENUMMISSING);
		}
	}

	// Check atmospherics and environment.
	if (rendParams.envMap) rendParams.envMap->EnumAuxFiles(checkNames, ENUMMISSING );
	if (rendParams.atmos) rendParams.atmos->EnumAuxFiles(checkNames, ENUMMISSING);

	// If we have any missing maps we report it to the user.
	// We should only report these errors on the first frame.
	// Also, if we are network rendering, we do not prompt the user,
	// instead we are writing the status to the log file.

	if (mapFiles.Count() && firstFrame) {
		// TBD: Use new log system
		// Updated to new logging system - GG: 01/29/99
		if (rendParams.bNetRender) {
			// Write out error report to file.
			Interface *ci = GetCOREInterface();
			for ( i=0; i<mapFiles.Count(); i++ )
				ci->Log()->LogEntry(SYSLOG_ERROR,NO_DIALOG,NULL,"Missing Map: %d",mapFiles[i]);
			return 0;
		}
		else {
			if (MessageBox(hWnd, "There are missing maps.\nDo you want to render anyway?", "Warning!", MB_YESNO) != IDYES) {
				return 0;
			}
		}

	}

	// Load the maps
	MapLoadEnum mapload(t);
	for (i=0; i<mtls.Count(); i++) {
		EnumMaps(mtls[i],-1, mapload);
	}

	return 1;
}

//***************************************************************************
// This method enumerates the external maps needed by the objects
// and materials.
// If a map is missing its name is appended to a nameTab and we
// report the problem.
// If a map is found it is appended to a map list and
// when done, we manually load each map into the system.
// See the rendutil module for the enumerators.
//***************************************************************************

int RiseRenderer::BuildMapFiles(TimeValue t)
{
	Instance* inst;

	for (inst = ilist; inst!=NULL; inst = inst->next) {
		// Load the maps
		MapSetupEnum mapsetup(t, this, inst);
		EnumMtlTree(inst->mtl, -1, mapsetup);
	}

	return 1;
}



void RiseRenderer::AddInstance(INode* node)
{
	nCurNodeID++;
	Instance* pInst = new Instance(node, &mtls, nCurNodeID);

	pInst->next = ilist;
	ilist = pInst;
	// TBD - initialize stuff here

	instTab.Append(1, &pInst, 25);
}

//***************************************************************************
// A node enumerator.
// These classes enumerates the nodes in the scene for access at render time.
// When enumerated, we create an object of class RenderNode for each node.
// This RenderNode contains the node, assigned materials etc.
// We store a list of the RenderNodes in a list RiseRenderer::rNodeTab
// We also have another list RiseRenderer::lightTab where we store all
// the lights in the scene.
//***************************************************************************

void RiseRenderer::NodeEnum(INode* node)
{
	// For each child of this node, we recurse into ourselves 
	// until no more children are found.
	for (int c = 0; c < node->NumberOfChildren(); c++) {
		NodeEnum(node->GetChildNode(c));
	}

	// Is the node hidden?
	BOOL nodeHidden = node->IsNodeHidden(TRUE);
	
	// Get the ObjectState.
	// The ObjectState is the structure that flows up the pipeline.
	// It contains a matrix, a material index, some flags for channels,
	// and a pointer to the object in the pipeline.
	ObjectState ostate = node->EvalWorldState(0);
	if (ostate.obj==NULL) 
		return;

	// Examine the superclass ID in order to figure out what kind
	// of object we are dealing with.
	switch (ostate.obj->SuperClassID()) {

		// It's a light.
		case LIGHT_CLASS_ID: { 

			// Get the light object from the ObjectState
			LightObject *light = (LightObject*)ostate.obj;

			// Is this light turned on?
			if (light->GetUseLight()) {
				switch (light->GetShadowMethod()) {
					case LIGHTSHADOW_MAPPED:
						// Mapped shadows
						break;
					case LIGHTSHADOW_RAYTRACED:
						// Ratraced shadows
						break;
					}
				}
				RenderLight* rl = new RenderLight(node, &mtls);
				// Create a RenderLight and append it to our list of lights
				lightTab.Append(1, &rl);
			}
			break;
		case SHAPE_CLASS_ID:	// To support renderable shapes
		case GEOMOBJECT_CLASS_ID: {
			// This is an object in the scene

			// If we are not rendering hidden objects, return now
			if (nodeHidden && !rendParams.bRenderHidden)
				return;

			// If this object cannot render, skip it
			if (!ostate.obj->IsRenderable()) 
				return;
			if (!node->Renderable()) 
				return;

			// Handle motion blur etc...
			
			// Add the node to our list
			AddInstance(node);
			break;
		}
	}
}

RefTargetHandle RiseRenderer::Clone(RemapDir &remap)
{
	RiseRenderer *newRend = new RiseRenderer;
	BaseClone(this, newRend, remap);
	return newRend;
}

//***************************************************************************
// Loading and Saving render parameters
// Chunk ID's for loading and saving render data
//***************************************************************************

#define RISE_RENDMAT_FILENAME_CHUNKID 001
#define RISE_SUPPL_FILENAME_CHUNKID 002

//***************************************************************************
//	Save the render options in the scene
//***************************************************************************

IOResult RiseRenderer::Save(ISave *isave)
{
	// Save our settings to our chunk
	isave->BeginChunk( RISE_RENDMAT_FILENAME_CHUNKID );
	isave->WriteCString( szRenderSettingsFile );
	isave->EndChunk();

	isave->BeginChunk( RISE_SUPPL_FILENAME_CHUNKID );
	isave->WriteCString( szSupplementarySettingsFile );
	isave->EndChunk();
	
	return IO_OK;
}


//***************************************************************************
//	Load the render options from the scene
//***************************************************************************

IOResult RiseRenderer::Load(ILoad *iload)
{
	int id;
	char *buf;

	// Load our settings
	IOResult res;
	while( IO_OK==(res=iload->OpenChunk())) {
		switch(id = iload->CurChunkID())  {
			case RISE_RENDMAT_FILENAME_CHUNKID:	
				if (IO_OK==iload->ReadCStringChunk(&buf))  {
					strcpy( szRenderSettingsFile, buf );
				}
				break;
			case RISE_SUPPL_FILENAME_CHUNKID:	
				if (IO_OK==iload->ReadCStringChunk(&buf))  {
					strcpy( szSupplementarySettingsFile, buf );
				}
				break;
			}
		iload->CloseChunk();
		if (res!=IO_OK) 
			return res;
		}

	return IO_OK;
}

//===========================================================================
//
// Class RiseRendererParams : public RenderGlobalContext
//
//===========================================================================

//***************************************************************************
// Initialize our custom options.
//***************************************************************************

RiseRendererParams::RiseRendererParams()
{
	SetDefaults();

	envMap = NULL;
	atmos = NULL;
	rendType = RENDTYPE_NORMAL;
	nMinx = 0;
	nMiny = 0;
	nMaxx = 0;
	nMaxy = 0;
	nNumDefLights = 0;
	nRegxmin = 0;
	nRegxmax = 0;
	nRegymin = 0;
	nRegymax = 0;
	scrDUV = Point2(0.0f, 0.0f);
	pDefaultLights = NULL;
	pFrp = NULL;
	bVideoColorCheck = 0;
	bForce2Sided = FALSE;
	bRenderHidden = FALSE;
	bSuperBlack = FALSE;
	bRenderFields = FALSE;
	bNetRender = FALSE;

	renderer = NULL;
	projType = PROJ_PERSPECTIVE;
	devWidth = 0;
	devHeight = 0;
	xscale = 0;
	yscale = 0;
	xc = 0;
	yc = 0;
	antialias = FALSE;
	nearRange = 0;
	farRange = 0;
	devAspect = 0;
	frameDur = 0;
	time = 0;
	wireMode = FALSE;
	inMtlEdit = FALSE;
	fieldRender = FALSE;
	first_field = FALSE;
	field_order = FALSE;
	objMotBlur = FALSE;
	nBlurFrames = 0;
}

void RiseRendererParams::SetDefaults()
{
}

//***************************************************************************
// These values can be assumed to be correct.
// See the SDK help for class ViewParams for an explanation.
//***************************************************************************

#define VIEW_DEFAULT_WIDTH ((float)400.0)

void RiseRendererParams::ComputeViewParams(const ViewParams&vp)
{
	worldToCam = vp.affineTM;
	camToWorld = Inverse(worldToCam);

	xc = devWidth / 2.0f;
	yc = devHeight / 2.0f;

	scrDUV.x = 1.0f/(float)devWidth;
	scrDUV.y = 1.0f/(float)devHeight;

	projType = vp.projType;

	if (projType == PROJ_PERSPECTIVE) {
		float fac =  -(float)(1.0 / tan(0.5*(double)vp.fov));
		xscale =  fac*xc;
		yscale = -devAspect*xscale;
	}
	else {
		xscale = (float)devWidth/(VIEW_DEFAULT_WIDTH*vp.zoom);
		yscale = -devAspect*xscale;
	}

	this->viewparams = vp;

	// TBD: Do Blowup calculation here.
}

ViewParams* RiseRendererParams::GetViewParams()
{
    return &viewparams;   
}

//***************************************************************************
// Calculate the direction of a ray going through pixels sx, sy
//***************************************************************************

Point3 RiseRendererParams::RayDirection(float sx, float sy)
{
	Point3 p;
	p.x = -(sx-xc)/xscale; 
	p.y = -(sy-yc)/yscale; 
	p.z = -1.0f;
	return Normalize(p);
}

//***************************************************************************
// Render Instances (from RenderGlobalContext)
//***************************************************************************

int RiseRendererParams::NumRenderInstances()
{
	return ((RiseRenderer*)renderer)->instTab.Count();
}

RenderInstance* RiseRendererParams::GetRenderInstance(int i)
{
	if (i<NumRenderInstances()) {
		return ((RiseRenderer*)renderer)->instTab[i];
	}

	return NULL;
}


//===========================================================================
//
// Class MyView
//
//===========================================================================

//***************************************************************************
// View to screen implementation for the view class
//***************************************************************************

Point2 MyView::ViewToScreen(Point3 p)
{
	return pRendParams->MapToScreen(p);
}



