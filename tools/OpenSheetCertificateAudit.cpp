#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Objects/CSGObject.h"
#include <iostream>
#include <vector>
#include <string>
#include <typeinfo>
using namespace RISE;
using namespace RISE::Implementation;
struct Names : public IEnumCallback<const char*> { std::vector<std::string> v; bool operator()(const char* const& s) override { v.push_back(s); return true; } };
static void Report(const std::string& scene, const std::string& name, const IObject* o)
{
	if(!o) return;
	if(const auto* csg=dynamic_cast<const CSGObject*>(o)) {
		Report(scene,name+"/A",csg->GetOperandA()); Report(scene,name+"/B",csg->GetOperandB()); return;
	}
	const IGeometry* g=o->GetGeometry();
	const IMaterial* m=o->GetMaterial();
	const ISPF* f=m?m->GetSPF():nullptr;
	std::string spf=f?typeid(*f).name():"none";
	std::string mat=m?typeid(*m).name():"none";
	if(const auto* t=dynamic_cast<const TriangleMeshGeometryIndexed*>(g))
		std::cout<<scene<<"\t"<<name<<"\tIDX\twt="<<t->IsCertifiedWatertight()<<"\topen="<<t->IsProvablyOpenSheet()<<"\tuncOrient="<<t->HasUncertainSMSNormalOrientation()<<"\t"<<mat<<"\n";
	else if(const auto* t2=dynamic_cast<const TriangleMeshGeometry*>(g))
		std::cout<<scene<<"\t"<<name<<"\tRAW\topen="<<t2->IsProvablyOpenSheet()<<"\tuncOrient="<<t2->HasUncertainSMSNormalOrientation()<<"\t"<<mat<<"\n";
	else if(const auto* d=dynamic_cast<const DisplacedGeometry*>(g))
		std::cout<<scene<<"\t"<<name<<"\tDISP\tuncertOpen="<<d->HasUncertifiedOpenness()<<"\t"<<mat<<"\n";
}
int main(int argc,char** argv)
{
	GlobalLogPriv()->RemoveAllPrinters();
	for(int a=1;a<argc;++a){
		IJobPriv* job=nullptr;
		if(!RISE_CreateJobPriv(&job)) continue;
		if(!job->LoadAsciiSceneViaCst(argv[a])) { std::cout<<argv[a]<<"\tPARSEFAIL\n"; safe_release(job); continue; }
		Names n; job->GetScene()->GetObjects()->EnumerateItemNames(n);
		for(const auto& s:n.v) Report(argv[a],s,job->GetScene()->GetObjects()->GetItem(s.c_str()));
		safe_release(job);
	}
	return 0;
}
