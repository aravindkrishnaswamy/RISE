// DL-314: frozen-volume/table models must reject painters that depend on
// position, instead of evaluating them at a construction-time dummy hit.
#include <iostream>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/RandomWalkSSSMaterial.h"
using namespace RISE;
using namespace RISE::Implementation;
class PositionPainter : public UniformScalarPainter {
public:
    explicit PositionPainter(Scalar v=1.5) : UniformScalarPainter(v) {}
    ScalarTriple GetValuesAt(const RayIntersectionGeometric& ri) const override {
        return ScalarTriple(value + ri.ptIntersection.x * 0.1);
    }
    Scalar GetValueAtNM(const RayIntersectionGeometric& ri,Scalar) const override { return GetValuesAt(ri).v[0]; }
    bool IsPositionIndependent() const override { return false; }
};
int main()
{
    int failures=0,checks=0;
    auto* varying = new PositionPainter;
    auto* index = new UniformScalarPainter(1.5);
    auto* zero = new UniformScalarPainter(0);
    auto* scattering = new UniformScalarPainter(2);
    const IScalarPainter* base[3] = {index,zero,scattering};
    for(int slot=0;slot<3;++slot) {
        const IScalarPainter* p[3]={base[0],base[1],base[2]};p[slot]=varying;
        IMaterial* m=nullptr;
        const bool ok=RISE_API_CreateRandomWalkSSSMaterial(&m,*p[0],*p[1],*p[2],0,0,8192);
        ++checks;if(ok || m) { ++failures;std::cerr << "FAIL RW slot " << slot << '\n'; }
        if(m)m->release();
    }
    const Scalar defaults[9]={0.025,0.8,0,0,0.002,0.025,1.4,1.38,0.75};
    UniformScalarPainter* skin[9];
    for(int i=0;i<9;++i)skin[i]=new UniformScalarPainter(defaults[i]);
    for(int slot=0;slot<9;++slot) {
        auto* skinVarying=new PositionPainter(defaults[slot]);
        const IScalarPainter* p[9];for(int i=0;i<9;++i)p[i]=i==slot ? static_cast<IScalarPainter*>(skinVarying) : skin[i];
        IMaterial* m=nullptr;
        const bool ok=RISE_API_CreateDonnerJensenSkinBSSRDFMaterial(&m,*p[0],*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],0);
        ++checks;if(ok || m) { ++failures;std::cerr << "FAIL skin slot " << slot << '\n'; }
        if(m)m->release();
        skinVarying->release();
    }
    IMaterial* control=nullptr;
    bool valid=RISE_API_CreateRandomWalkSSSMaterial(&control,*index,*zero,*scattering,0,0,8192);
    ++checks;if(!valid || !control)++failures;
    if(control) {
        auto* rw=dynamic_cast<RandomWalkSSSMaterial*>(control);
        ++checks;if(!rw)++failures;
        if(rw) {
            rw->SetIOR(*varying);
            ++checks;if(&rw->GetIOR()!=index || rw->GetRandomWalkSSSParams()->ior!=1.5)++failures;
        }
        control->release();
    }
    IMaterial* skinControl=nullptr;
    const bool skinOK=RISE_API_CreateDonnerJensenSkinBSSRDFMaterial(&skinControl,*skin[0],*skin[1],*skin[2],*skin[3],*skin[4],*skin[5],*skin[6],*skin[7],*skin[8],0);
    ++checks;if(!skinOK || !skinControl)++failures;
    if(skinControl)skinControl->release();
    IJobPriv* job=nullptr;
    const bool jobReady=RISE_CreateJobPriv(&job) && job && job->GetScalarPainters()->AddItem(varying,"position");
    ++checks;if(!jobReady)++failures;
    if(jobReady) {
        ++checks;if(job->AddRandomWalkSSSMaterial("rw","position","0","2","0","0","8192"))++failures;
        ++checks;if(!job->AddRandomWalkSSSMaterial("rw","1.5","0","2","0","0","8192"))++failures;
        ++checks;if(job->AddDonnerJensenSkinBSSRDFMaterial("skin","0.025","0.8","0","0","0.002","0.025","position","1.38","0.75","0"))++failures;
        ++checks;if(!job->AddDonnerJensenSkinBSSRDFMaterial("skin","0.025","0.8","0","0","0.002","0.025","1.4","1.38","0.75","0"))++failures;
    }
    if(job)job->release();
    for(auto* p:skin)p->release();
    varying->release();index->release();zero->release();scattering->release();
    std::cout << "DL-314 checks " << checks << " failures " << failures << '\n';
    return failures?1:0;
}
