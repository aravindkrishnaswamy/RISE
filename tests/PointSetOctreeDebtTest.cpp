// DL-441/442/443: exact quadrature mass and clean partial-construction unwind.
#include "SMSRenderTestSupport.h"
#include "../src/Library/Shaders/SSS/PointSetOctree.h"
#include <new>
#include <cstring>

namespace {
    bool inject=false,thrown=false;
    std::size_t allocation=0,failAt=0;
    void* Allocate(std::size_t size) {
        if(inject && allocation++==failAt) {thrown=true;throw std::bad_alloc();}
        void* pointer=std::malloc(size?size:1);
        if(!pointer) throw std::bad_alloc();
        if(inject) std::memset(pointer,0xa5,size);
        return pointer;
    }
    class UnitKernel final : public ISubSurfaceExtinctionFunction, public Reference {
    public:
        Scalar GetMaximumDistanceForError(Scalar error) const override {return error>0?1e30:0;}
        RISEPel ComputeTotalExtinction(Scalar) const override {return RISEPel(1);}
    };
    Scalar Evaluate(PointSetOctree::PointSet points,bool collapse,bool* tagged=nullptr) {
        PointSetOctree tree(BoundingBox(Point3(-1,-1,-1),Point3(1,1,1)),1);
        Check(tree.AddElements(points,5),"octree probe builds");
        UnitKernel kernel;RISEPel value(0.0);
        RayIntersectionGeometric hit(Ray(Point3(10,10,10),Vector3(0,0,-1)),nullRasterizerState);
        tree.Evaluate(value,Point3(10,10,10),kernel,collapse?0:1,nullptr,hit,nullptr,1,tagged);
        Check(value[0]==value[1]&&value[1]==value[2],"octree probe preserves RGB channel equality");
        return value[0];
    }
    PointSetOctree::SamplePoint Point(Scalar x,Scalar y,Scalar z,Scalar energy=1,bool tag=false) {
        PointSetOctree::SamplePoint point;point.ptPosition=Point3(x,y,z);
        point.irrad=RISEPel(energy);point.smsReferenceRadiance=tag;return point;
    }
}
// Poison allocator contents so an unwritten child slot cannot pass by luck.
void* operator new(std::size_t size) {return Allocate(size);}
void* operator new[](std::size_t size) {return Allocate(size);}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void operator delete[](void* pointer) noexcept {std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept {std::free(pointer);}
void operator delete[](void* pointer,std::size_t) noexcept {std::free(pointer);}

int main(int argc,char** argv) {
    const std::string section=argc>1?argv[1]:"all";
    if(section=="all"||section=="dl441") {
        const Scalar planar=Evaluate({Point(-.4,-.4,0),Point(.4,-.4,0),Point(-.4,.4,0),Point(.4,.4,0)},false);
        const Scalar split=Evaluate({Point(0,.4,.4),Point(.4,.4,.4)},false);
        const Scalar edge=Evaluate({Point(-1,-1,-1),Point(1,1,1)},false);
        std::cout<<"DL-441 planar="<<planar<<" split="<<split<<" boundary="<<edge<<'\n';
        Check(planar==4 && split==2 && edge==2,"DL-441 unique child ownership retains boundary samples");
    }
    if(section=="all"||section=="dl442") {
        PointSetOctree::PointSet points;
        for(unsigned child=0;child<8;++child) for(unsigned i=0;i<2;++i)
            points.push_back(Point((child&1)?.6:-.6,(child&2)?.6:-.6,(child&4)?.6:-.6));
        const Scalar leaves=Evaluate(points,false),collapsed=Evaluate(points,true);
        bool tagged=false;
        const Scalar unequal=Evaluate({Point(-.8,-.8,-.8,1),Point(-.7,-.7,-.7,2,true),Point(.6,.6,.6,7)},true,&tagged);
        std::cout<<"DL-442 leaves="<<leaves<<" collapsed="<<collapsed<<" unequal="<<unequal<<'\n';
        Check(leaves==16 && collapsed==16 && unequal==10 && tagged,"DL-442 collapse preserves population-weighted mass and reference tag");
    }
    if(section=="dl443") {
        failAt=argc>2?std::stoul(argv[2]):32;
        PointSetOctree::PointSet points;
        for(unsigned i=0;i<16;++i) points.push_back(Point(-.9+i*.11,-.7+i*.07,-.8+i*.09));
        // Initialize logger and prove a healthy construction before injection.
        Evaluate(points,false);
        bool caught=false;
        try {
            PointSetOctree tree(BoundingBox(Point3(-1,-1,-1),Point3(1,1,1)),1);
            inject=true;tree.AddElements(points,8);inject=false;
        } catch(const std::bad_alloc&) {inject=false;caught=true;}
        Check(thrown && caught,"DL-443 injected bad_alloc unwinds without poisoned child access");
        Check(Evaluate(points,false)==16,"DL-443 a subsequent octree build succeeds");
        std::cout<<"DL-443 failure position="<<failAt<<" caught="<<caught<<'\n';
    }
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
