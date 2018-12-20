//***************************************************************************
// rise3dsmax - [instance.cpp] Sample Plugin Renderer for 3D Studio MAX.
// 
// By Christer Janson - Kinetix
//
// Description:
// Implementation of the Instance and RenderLight classes
//
//***************************************************************************

#include "pch.h"
#include "maxincl.h"
#include "rise3dsmax.h"
#include "rendutil.h"
#include "refenum.h"

Instance::Instance(INode* node, MtlBaseLib* mtls, int nodeID)
{
	pNode	= node;
	mesh	= NULL;
	flags	= 0;

	// Get material for node
	mtl = node->GetMtl();
	if (!mtl) {
		// Node has no material, create a dummy material based on wireframe color
		// This is done so the  renderer does not have to worry about nodes
		// without materials. It will, in effect, assure that every node has
		// a valid material.
		mtl = new DumMtl(Color(node->GetWireColor()));
		DebugPrint("\tCreated dummy material for: %s.\n", node->GetName());
	}

	// Add material to our list
	mtls->AddMtl(mtl);

	this->nodeID = nodeID;

	deleteMesh = FALSE;
}

Instance::~Instance()
{
	FreeAll();

	// If this is a dummy material we need to delete it
	if (mtl->ClassID() == DUMMTL_CLASS_ID) {
		delete mtl;
		DebugPrint("\tDeleted dummy material.\n");
	}
}


int Instance::Update(TimeValue t, View& vw, RiseRenderer* pRenderer)
{
	FreeAll();

	// Check visibility
	vis = pNode->GetVisibility(t);
	if (vis < 0.0f) {
		vis = 0.0f;
		SetFlag(INST_HIDE, 1);
		return 1;
	}
	if (vis > 1.0f) vis = 1.0f;
	SetFlag(INST_HIDE, 0);

	// TM's
	Interval tmValid(FOREVER);
	objToWorld = pNode->GetObjTMAfterWSM(t,&tmValid);

	// Is this node negatively scaled
	SetFlag(INST_TM_NEGPARITY, TMNegParity(objToWorld));

	// Get Object
	ObjectState os = pNode->EvalWorldState(t);
	pObject = os.obj;

	mesh = ((GeomObject*)pObject)->GetRenderMesh(t, pNode, vw, deleteMesh);

	pRenderer->nNumFaces+=mesh->numFaces;

	return 1;
}

void Instance::UpdateViewTM(Matrix3 affineTM)
{
	objToCam = objToWorld * affineTM;
	camToObj = Inverse(objToCam);
	normalObjToCam.IdentityMatrix();

	// Calculate the inverse-transpose of objToCam for transforming normals.
	for (int it=0; it<3; it++) {
		Point4 p = Inverse(objToCam).GetColumn(it);
		normalObjToCam.SetRow(it,Point3(p[0],p[1],p[2]));
	}

	normalCamToObj = Inverse(normalObjToCam);
}

RenderInstance* Instance::Next()
{
	return next;
}

Interval Instance::MeshValidity()
{
	return FOREVER;
}

int Instance::NumLights()
{
	return 0;
}

LightDesc* Instance::Light(int n)
{
	return NULL;
}

int Instance::NumShadLights()
{
	return NULL;
}

LightDesc* Instance::ShadLight(int n)
{
	return NULL;
}

INode* Instance::GetINode()
{
	return pNode;
}

Object* Instance::GetEvalObject()
{
	return pObject;
}

unsigned long Instance::MtlRequirements(int mtlNum, int faceNum)
{
	return mtl->Requirements(mtlNum);
}

Mtl* Instance::GetMtl(int faceNum)
{
	return NULL;
}

Point3 Instance::GetFaceNormal(int faceNum)
{
	return Point3(0,0,0);
}

Point3 Instance::GetFaceVertNormal(int faceNum, int vertNum)
{
	return Point3(0,0,0);
}

void Instance::GetFaceVertNormals(int faceNum, Point3 n[3])
{
}

Point3 Instance::GetCamVert(int vertNum)
{
	return Point3(0,0,0);
}


void Instance::GetObjVerts(int fnum, Point3 obp[3])
{
	Face* f = &mesh->faces[fnum];
	obp[0] = mesh->verts[f->v[0]];
	obp[1] = mesh->verts[f->v[1]];
	obp[2] = mesh->verts[f->v[2]];
}

void Instance::GetCamVerts(int fnum, Point3 cp[3])
{
	Face* f = &mesh->faces[fnum];
	cp[0] = objToCam*mesh->verts[f->v[0]];
	cp[1] = objToCam*mesh->verts[f->v[1]];
	cp[2] = objToCam*mesh->verts[f->v[2]];
}

TCHAR* Instance::GetName()
{
	return pNode->GetName();
}

int Instance::CastsShadowsFrom(const ObjLightDesc& lt)
{
	return TRUE;
}

void Instance::FreeAll()
{
	if (mesh) {
		if( deleteMesh ) {
			mesh->DeleteThis();
		}
		mesh = NULL;
	}
}

Mesh* Instance::GetMesh()
{
	return mesh;
}
