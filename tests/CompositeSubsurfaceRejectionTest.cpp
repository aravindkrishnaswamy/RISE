// DL-422: unsupported subsurface stacks must fail construction rather than
// silently exposing only their surface Fresnel lobe. No layer transport model
// is assumed by this contract; an ordinary glass/Lambertian stack stays valid.
#include <iostream>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IJob.h"
using namespace RISE;
using namespace RISE::Implementation;
int main()
{
    auto* zero = new UniformScalarPainter(0);
    auto* one = new UniformScalarPainter(1);
    auto* index = new UniformScalarPainter(1.5);
    auto* scattering = new UniformScalarPainter(2);
    auto* sharp = new UniformScalarPainter(1000000);
    IPainter* white = nullptr;
    IMaterial *glass=nullptr,*lamb=nullptr,*rw=nullptr,*diffusion=nullptr;
    bool ready = RISE_API_CreateUniformColorPainter(&white,RISEPel(1));
    ready = ready && RISE_API_CreateLambertianMaterial(&lamb,*white);
    ready = ready && RISE_API_CreateDielectricMaterial(&glass,*one,*index,*sharp,false);
    ready = ready && RISE_API_CreateRandomWalkSSSMaterial(&rw,*index,*zero,*scattering,0,0,8192);
    ready = ready && RISE_API_CreateSubSurfaceScatteringMaterial(&diffusion,*index,*zero,*scattering,0,0);
    int failures = ready ? 0 : 1;
    if( ready ) {
        IMaterial* layers[2] = {rw,diffusion};
        for( IMaterial* sss : layers ) for(int side=0;side<2;++side) {
            IMaterial* result=nullptr;
            const bool accepted = RISE_API_CreateCompositeMaterial(&result,side ? *glass : *sss,
                side ? *sss : *lamb,8,8,8,8,8,0,*zero);
            if( accepted || result ) { ++failures; std::cerr << "FAIL: subsurface stack accepted, side=" << side << '\n'; }
            if( result ) result->release();
        }
        IMaterial* control=nullptr;
        if( !RISE_API_CreateCompositeMaterial(&control,*glass,*lamb,8,8,8,8,8,0,*zero) || !control ) ++failures;
        if( control ) control->release();
    }
    IJob* job=nullptr;
    const double color[3]={1,1,1};
    const bool jobReady = RISE_CreateJob(&job) && job &&
        job->AddUniformColorPainter("white",color,"Rec709RGB_Linear") &&
        job->AddLambertianMaterial("lamb","white") &&
        job->AddRandomWalkSSSMaterial("rw","1.5","0","2","0","0","8192");
    if( !jobReady ) ++failures;
    else {
        if( job->AddCompositeMaterial("unsupported","rw","lamb",8,8,8,8,8,0,"0") ) ++failures;
        // Failed construction must not leave a name registered: reuse it.
        if( !job->AddCompositeMaterial("unsupported","lamb","lamb",8,8,8,8,8,0,"0") ) ++failures;
    }
    if( job ) job->release();
    if( diffusion ) diffusion->release();
    if( rw ) rw->release();
    if( lamb ) lamb->release();
    if( glass ) glass->release();
    if( white ) white->release();
    sharp->release();scattering->release();index->release();one->release();zero->release();
    std::cout << "DL-422 checks: 7 failures: " << failures << '\n';
    return failures ? 1 : 0;
}
