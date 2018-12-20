//***************************************************************************
// rise3dsmax - [rise3dsmax.h] Sample Plugin Renderer for 3D Studio MAX.
// 
// By Christer Janson - Kinetix
//
// May     11, 1996	CCJ Initial coding
// October 28, 1996	CCJ Texture mapping, raycasting etc.
// December    1996	CCJ General cleanup
// May         1997 CCJ Substantial rewrite for Version 2.0
//
// Description:
// Main renderer header.
//
//***************************************************************************

#ifndef __rise3dsmax_H__
#define __rise3dsmax_H__

// IMPORTANT: If you use this sample, don't forget to change the ClassID!!!
#define RISEREND_CLASS_ID	Class_ID(88,1);
#define DUMMTL_CLASS_ID		Class_ID(0x10707f49, 0x40f5a40)

#define RENDERNAME "R.I.S.E. Renderer"
#define BIGFLOAT (float(1.0e30))

class RiseRenderer;
class Instance;
class RenderLight;
class SContext;

// Render parameters. Add whatever parameters you need here.
// These are typically parameters that needs to be accessed during the
// setup of the renderer and during the rendering.
class RiseRendererParams : public RenderGlobalContext {
public:
	RendType	rendType;				// View, blowup, region etc.
	int			nMinx;
	int			nMiny;
	int			nMaxx;
	int			nMaxy;
	int			nNumDefLights;			// The default lights passed into the renderer
	int			nRegxmin;				// Coords for render blowup etc.
	int			nRegxmax;				// Coords for render blowup etc.
	int			nRegymin;				// Coords for render blowup etc.
	int			nRegymax;				// Coords for render blowup etc.
	Point2		scrDUV;
	DefaultLight*	pDefaultLights;
	FrameRendParams*	pFrp;			// Frame specific members
	RendProgressCallback*	progCallback;	// Render progress callback

	ViewParams viewparams;

	// Custom options
	// These options are specific to the RISE renderer

	// Standard options
	// These options are configurable for all plugin renderers
	BOOL		bVideoColorCheck;
	BOOL		bForce2Sided;
	BOOL		bRenderHidden;
	BOOL		bSuperBlack;
	BOOL		bRenderFields;
	BOOL		bNetRender;

	// Render effects
	Effect*		effect;

	RiseRendererParams();
	void		SetDefaults();
	void		ComputeViewParams(const ViewParams& vp);
	Point3		RayDirection(float sx, float sy);

	int				NumRenderInstances();
	RenderInstance*	GetRenderInstance(int i);

	ViewParams* GetViewParams();
};

// For calculation of view to screen coords
class MyView : public View {
public:
	RiseRendererParams*	pRendParams;
	Point2		ViewToScreen(Point3 p);
};


class RISEMapsContext : public RenderMapsContext {
public:
	RISEMapsContext(RiseRenderer* r, Instance* i) { renderer = r; inst = i; }
	INode *GetNode();
	int NodeRenderID();
	void GetCurrentViewParams(ViewParams &vp);
	void GetSubRendParams(SubRendParams &srp);
	int SubMtlIndex();
	void SetSubMtlIndex(int mindex);
	void FindMtlPlane(float pl[4]);
	void FindMtlScreenBox(Rect &sbox, Matrix3* viewTM=NULL,int mtlIndex=-1);
	Box3 CameraSpaceBoundingBox();
	Box3 ObjectSpaceBoundingBox();
	Matrix3 ObjectToWorldTM();
	RenderGlobalContext *GetGlobalContext();

	// ClipPlanes is a pointer to an array of Point4's,  each of which
	// represents a clip plane.  nClip Planes is the number of planes (up to 6);
	// The planes are in View space.
	int Render(Bitmap *bm, ViewParams &vp, SubRendParams &srp, Point4 *clipPlanes=NULL, int nClipPlanes=0);

private:
	int subMtl;
	RiseRenderer* renderer;
	Instance* inst;
};


// This is the renderer class definition.
class RiseRenderer: public Renderer {
public:
	BOOL		bOpen;					// Indicate that Open() has been called
	int			nCurNodeID;				// Node counter
	int			nNumFaces;				// Face counter
	INode*		pScene;					// Root node of scene
	INode*		pViewNode;				// Camera node if available
	ViewParams	view;					// View parameters
	MyView		theView;				// View to screen calculation
	MtlBaseLib	mtls;					// List of all materials

	// These are flags we use so if something is wrong we will report
	// errors once only (and not every frame)
	BOOL		bFirstFrame;			// Indicate that this is the first frame
	BOOL		bUvMessageDone;			// indicate that we have already warned the 
										// user that object requires UV coords
	Instance*	ilist;
	Tab<Instance *>		instTab;		// Keep track of instances
	Tab<RenderLight *>	lightTab;		// Keep track of lights

	RiseRendererParams	rendParams;			// Render parameters

	char szRenderSettingsFile[256];
	char szSupplementarySettingsFile[256];

	RiseRenderer();
	void		AddInstance(INode* node);
	void		NodeEnum(INode* node);
	int			RenderImage(RiseRendererParams& rp, TimeValue t, Bitmap* tobm, RendProgressCallback *prog, HWND hwnd);
	void		GetViewParams(INode* vnode, ViewParams& vp, TimeValue t);
	void		BeginThings();			// Called before rendering has started
	void		EndThings();			// Called after rendering has finished
	int			LoadMapFiles(TimeValue t, HWND hWnd, BOOL firstFrame);
	int			BuildMapFiles(TimeValue t);
	IOResult	Save(ISave *isave);
	IOResult	Load(ILoad *iload);

	int			Open(INode* scene, INode* vnode, ViewParams* viewPar, RendParams& rpar, HWND hwnd, DefaultLight* defaultLights=NULL, int numDefLights=0);
	int			Render(TimeValue t, Bitmap* tobm, FrameRendParams &frp, HWND hwnd, RendProgressCallback* prog, ViewParams* viewPar);
	void		Close(HWND hwnd);

	int			CheckAbort(int done, int total);
	void		SetProgTitle(const TCHAR *title);

	RefTargetHandle Clone(RemapDir &remap);

	// Create the use interface dialog
	RendParamDlg*	CreateParamDialog(IRendParams *ir,BOOL prog);
	// Called by Max when Max is reset
	void		ResetParams();
	void		DeleteThis();
	Class_ID	ClassID();
	void		GetClassName(TSTR& s);
};

class Instance : public RenderInstance {
	BOOL deleteMesh;

public:

	Instance(INode* node, MtlBaseLib* mtls, int nodeID);
	~Instance();

	// Methods inherited from RenderInstance
	RenderInstance*	Next();
	Interval	MeshValidity();
	int			NumLights();
	LightDesc*	Light(int n);
	int			NumShadLights();
	LightDesc*	ShadLight(int n);
	INode*		GetINode();
	Object*		GetEvalObject();
	unsigned long	MtlRequirements(int mtlNum, int faceNum);
	Point3		GetFaceNormal(int faceNum);
	Point3		GetFaceVertNormal(int faceNum, int vertNum);
	void		GetFaceVertNormals(int faceNum, Point3 n[3]);
	Point3		GetCamVert(int vertNum);
	void		GetObjVerts(int fnum, Point3 obp[3]);
	void		GetCamVerts(int fnum, Point3 cp[3]);

	// New for R3
	int			CastsShadowsFrom(const ObjLightDesc& lt);

	// Access methods
	TCHAR*		GetName();

	int			Update(TimeValue t, View& gview, RiseRenderer* pRenderer);
	void		UpdateViewTM(Matrix3 affineTM);
	void		TransformGeometry(Matrix3 pointMat, Matrix3 vecMat);
	void		CalcBoundingSphere();
	void		FreeAll();

	Mtl*		GetMtl(int faceNum);

	Mesh*		GetMesh();

	Matrix3		normalCamToObj;
	INode*		pNode;
	Object*		pObject;
	Matrix3		objToWorld;
	Box3		camBox;
	Instance*	next;
};

// The RenderLight is the container for all lights in the scene.
class RenderLight {
public:
    // Constructor for normal lights
    RenderLight(INode* node, MtlBaseLib* mtls);
    // Alternative constructor for default lights
    RenderLight(DefaultLight* light);
    ~RenderLight();

    void Update(TimeValue t, RiseRenderer* renderer);
    void UpdateViewDepParams(Matrix3 world2cam);

    INode* pNode;
    ObjLightDesc* pDesc;
    LightObject *pLight;
};


//***************************************************************************
//* Dummy Material : Simple Phong shader using Node color
//* This material is assigned to each node that does not have a material
//* previously assigned. The diffuse color is assigned based on the 
//* wireframe color.
//* This way we can assume that all nodes have a material assigned.
//***************************************************************************

#define DUMSHINE .20f	//.25f
#define DUMSPEC .20f	//.50f

class DumMtl: public Mtl {
	Color diff, spec;
	float phongexp;
	public:
		DumMtl(Color c);
		void Update(TimeValue t, Interval& valid);
		void Reset();
		Interval Validity(TimeValue t);
		ParamDlg* CreateParamDlg(HWND hwMtlEdit, IMtlParams *imp);
		Color GetAmbient(int mtlNum=0, BOOL backFace=FALSE);
		Color GetDiffuse(int mtlNum=0, BOOL backFace=FALSE);
		Color GetSpecular(int mtlNum=0, BOOL backFace=FALSE);
		float GetShininess(int mtlNum=0, BOOL backFace=FALSE);
		float GetShinStr(int mtlNum=0, BOOL backFace=FALSE);
		float GetXParency(int mtlNum=0, BOOL backFace=FALSE);
		void SetAmbient(Color c, TimeValue t);
		void SetDiffuse(Color c, TimeValue t);
		void SetSpecular(Color c, TimeValue t);
		void SetShininess(float v, TimeValue t);
		Class_ID ClassID();
		void DeleteThis();
    	RefResult NotifyRefChanged(Interval changeInt, RefTargetHandle hTarget, 
	    	PartID& partID,  RefMessage message);
		void Shade(ShadeContext& sc);
};


//***************************************************************************
//* RendContext is used to evaluate the lights
//***************************************************************************

 class RContext: public RendContext {
	public:
		RContext(RiseRenderer *r) { renderer = r; }
		Matrix3 WorldToCam() const { return renderer->rendParams.worldToCam; }
		Color GlobalLightLevel() const;
		int Progress(int done, int total) {
			return 1;
		}
		RiseRenderer *renderer;
};


//***************************************************************************
//* The is the Light descriptor object for default lights
//***************************************************************************
class DefObjLight : public ObjLightDesc 
{
	public:
		Color	intensCol;   // intens*color 
		Point3	lightDir;
		bool	bViewOriented;

		DefObjLight(DefaultLight *l);
		void	DeleteThis() {delete this;}
		int		Update(TimeValue t, const RendContext& rc, RenderGlobalContext* rgc, BOOL shadows, BOOL shadowGeomChanged);
		int		UpdateViewDepParams(const Matrix3& worldToCam);
		BOOL	Illuminate(ShadeContext& sc, Point3& normal, Color& color, Point3 &dir, float &dot_nl, float& diffuseCoef);
};


//***************************************************************************
//* The is the Light descriptor object for NULL lights
//***************************************************************************
class NullObjLight : public ObjLightDesc 
{
	public:
		NullObjLight() : ObjLightDesc(NULL){};
		void	DeleteThis() {delete this;}
		int		Update(TimeValue t, const RendContext& rc, RenderGlobalContext* rgc, BOOL shadows, BOOL shadowGeomChanged){return 1;};
		int		UpdateViewDepParams(const Matrix3& worldToCam){return 1;};
		BOOL	Illuminate(ShadeContext& sc, Point3& normal, Color& color, Point3 &dir, float &dot_nl, float& diffuseCoef){return FALSE;}
};

#endif // __rise3dsmax_H__
