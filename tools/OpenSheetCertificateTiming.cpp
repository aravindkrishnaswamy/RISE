#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include <chrono>
#include <cmath>
#include <iostream>
using namespace RISE; using namespace RISE::Implementation;
int main(int argc,char**argv){
	GlobalLogPriv()->RemoveAllPrinters();
	const int N = argc>1?atoi(argv[1]):700;
	for(int curved=0;curved<3;++curved){
	VerticesListType v; NormalsListType n; TexCoordsListType c; IndexTriangleListType t;
	for(int j=0;j<=N;++j) for(int i=0;i<=N;++i){ double x=-1+2.0*i/N, y=-1+2.0*j/N; if(curved==2){ double th=3.14159265358979*j/N, ph=2*3.14159265358979*i/N; v.push_back(Point3(std::sin(th)*std::cos(ph),std::sin(th)*std::sin(ph),std::cos(th))); } else v.push_back(Point3(x,y,curved?0.2*(x*x+y*y):0)); n.push_back(Vector3(0,0,1)); c.push_back(Point2(0,0)); }
	auto id=[&](int i,int j){return (unsigned)(j*(N+1)+i);};
	for(int j=0;j<N;++j) for(int i=0;i<N;++i){ IndexedTriangle a; a.iVertices[0]=a.iNormals[0]=a.iCoords[0]=id(i,j); a.iVertices[1]=a.iNormals[1]=a.iCoords[1]=id(i+1,j); a.iVertices[2]=a.iNormals[2]=a.iCoords[2]=id(i+1,j+1); t.push_back(a);
		IndexedTriangle b; b.iVertices[0]=b.iNormals[0]=b.iCoords[0]=id(i,j); b.iVertices[1]=b.iNormals[1]=b.iCoords[1]=id(i+1,j+1); b.iVertices[2]=b.iNormals[2]=b.iCoords[2]=id(i,j+1); t.push_back(b);}
	auto* m=new TriangleMeshGeometryIndexed(true,true); m->addref();
	auto t0=std::chrono::steady_clock::now();
	m->BeginIndexedTriangles(); m->AddVertices(v); m->AddNormals(n); m->AddTexCoords(c); m->AddIndexedTriangles(t); m->DoneIndexedTriangles();
	auto t1=std::chrono::steady_clock::now();
	m->UpdateVertices(v, NormalsListType());
	auto t2=std::chrono::steady_clock::now();
    for(auto& p : v) p.z += 0.03*(p.x*p.x+p.y*p.y);
    m->UpdateVertices(v, NormalsListType());
    auto t3=std::chrono::steady_clock::now();
	std::cout<<(curved==2?"sphere(closed-ish)":curved?"curved":"planar")<<" tris="<<t.size()<<" open="<<m->IsProvablyOpenSheet()<<" build ms="<<std::chrono::duration<double,std::milli>(t1-t0).count()<<" update ms="<<std::chrono::duration<double,std::milli>(t2-t1).count()<<" deformation ms="<<std::chrono::duration<double,std::milli>(t3-t2).count()<<"\n";
	m->release(); }
}
