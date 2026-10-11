// DL-482 close-out measurement only: no transport change and no radiance claim.
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Materials/RandomWalkSSSMaterial.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/BDPTUtilities.h"
#include <cstdio>
#include <cstring>
using namespace RISE;
using namespace RISE::Implementation;
int main(int argc,char** argv) {
    const bool strict=argc==2 && std::strcmp(argv[1],"--strict-reflection-pin")==0;
    auto* eta=new UniformScalarPainter(1.3);
    auto* absorption=new UniformScalarPainter(.1);
    auto* scattering=new UniformScalarPainter(4);
    IMaterial* materials[2]={new SubSurfaceScatteringMaterial(*eta,*absorption,*scattering,0,.2),
        new RandomWalkSSSMaterial(*eta,*absorption,*scattering,0,.2,256)};
    eta->release();absorption->release();scattering->release();
    int failures=0;
    for(unsigned kind=0;kind<2;++kind) {
        RandomNumberGenerator rng(48211);IndependentSampler sampler(rng);IORStack stack(1);
        RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
        ri.bHit=true;ri.ptIntersection=Point3(0,0,0);ri.vNormal=ri.vGeomNormal=Vector3(0,0,1);
        ri.onb.CreateFromW(ri.vNormal);
        for(bool nm:{false,true}) {
            unsigned reflected=0,wrong=0;
            for(unsigned draw=0;draw<512;++draw) {
                ScatteredRayContainer rays;
                if(nm) materials[kind]->GetSPF()->ScatterNM(ri,sampler,550,rays,stack);
                else materials[kind]->GetSPF()->Scatter(ri,sampler,rays,stack);
                Scalar prob=0;const auto* ray=rays.RandomlySelect(sampler.Get1D(),nm,&prob);
                if(ray && !ray->isDelta && prob>0) {
                    ++reflected;if(ray->type!=ScatteredRay::eRayReflection) ++wrong;
                }
            }
            if(!reflected || wrong) ++failures;
            std::printf("material=%u NM=%d reflection draws=%u wrong labels=%u /512\n",kind,int(nm),reflected,wrong);
        }
        // C-a-b-c-L. b is the same finite reflection as an interior walk
        // vertex or an endpoint of the (s=3,t=2) connection. a/c are diffuse.
        BDPTVertex eye[5],light[3];eye[0].type=BDPTVertex::CAMERA;eye[4].type=BDPTVertex::LIGHT;
        for(unsigned i=1;i<4;++i) {
            eye[i].type=BDPTVertex::SURFACE;eye[i].isConnectible=true;
            eye[i].capType=ScatteredRay::eRayDiffuse;
        }
        eye[2].pMaterial=materials[kind];eye[2].capType=ScatteredRay::eRayReflection;
        for(unsigned i=0;i<3;++i) light[i]=eye[4-i];
        const auto caps=BDPTUtilities::MakeBounceTypeCaps(UINT_MAX,0,UINT_MAX,UINT_MAX,16,16);
        BDPTUtilities::TypeCapPlan walk,connection;
        const auto a=BDPTUtilities::JoinedTypeCapPlan(nullptr,eye,0,5,caps,walk);
        const auto b=BDPTUtilities::JoinedTypeCapPlan(light,eye,3,2,caps,connection);
        std::printf("material=%u walk reflection=%u status=%d endpoint reflection=%u status=%d n=1 SD=0 (metadata, not energy)\n",
            kind,walk.base.n[1],int(a),connection.base.n[1],int(b));
        // The ordinary mode validates the witness, and also accepts a future
        // corrected endpoint count. Only the opt-in strict pin demands parity.
        const bool endpointValid=(connection.base.n[1]==0 && b==BDPTUtilities::eTypeCapOK) ||
            (connection.base.n[1]==1 && b==BDPTUtilities::eTypeCapOver);
        if(walk.base.n[1]!=1 || a!=BDPTUtilities::eTypeCapOver || !endpointValid) ++failures;
        if(strict && (a!=b || walk.base.n[1]!=connection.base.n[1])) ++failures;
        materials[kind]->release();
    }
    std::printf("%d failures\n",failures);return failures?1:0;
}
