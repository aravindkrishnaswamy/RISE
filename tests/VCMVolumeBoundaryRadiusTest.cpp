// DL-474 exact single-boundary normalization; corner pin remains unimplemented.
#define main VCMVolumeMergeRegressionMain
#include "VCMVolumeMergeTest.cpp"
#undef main
#include "../src/Library/Shaders/VCMIntegrator.h"
#include "../src/Library/Interfaces/IObjectManager.h"

static bool SameFactors(const VCMNormalization& a,const VCMNormalization& b) {
    return a.mVolumeMergeRadius==b.mVolumeMergeRadius && a.mVolumeMergeRadiusSq==b.mVolumeMergeRadiusSq &&
           a.mVolumeNormalization==b.mVolumeNormalization && a.mMisVolumeWeightFactor==b.mMisVolumeWeightFactor &&
           a.mVmNormalization==b.mVmNormalization && a.mMisVmWeightFactor==b.mMisVmWeightFactor &&
           a.mMisVcWeightFactor==b.mMisVcWeightFactor && a.mVolumeStoreProbability==b.mVolumeStoreProbability;
}
int main(int argc,char** argv) {
    IJobPriv* job=nullptr;
    const std::string path=WriteScene(FogBoxScene(false,1,true),"474unit");
    if(!RISE_CreateJobPriv(&job) || !job || !job->LoadAsciiSceneViaCst(path.c_str())) return 2;
    std::remove(path.c_str());
    job->GetObjects()->PrepareForRendering();
    BDPTVertex v;v.type=BDPTVertex::MEDIUM;v.pMediumObject=job->GetObjects()->GetItem("wall");
    VCMNormalization n=ComputeNormalization(24,24,.01,true,true);
    SetVolumeMergeRadius(n,.2,.01,17);n.mVolumeObjects=job->GetObjects();
    v.position=Point3(0,0,1.05);
    const VCMNormalization sphere=VCMIntegrator::BoundaryVolumeNormalization(n,v);
    // Independent standard two-sphere lens expression, not production cap heights.
    const double R=1,r=.2,d=1.05;
    const double lens=PI*std::pow(R+r-d,2)*(d*d+2*d*(R+r)-3*std::pow(R-r,2))/(12*d);
    const double phi=1-lens/((4.0/3)*PI*r*r*r);
    Check(std::fabs(sphere.mVolumeNormalization/n.mVolumeNormalization-1/phi)<1e-12,"sphere lens kernel");
    Check(std::fabs(sphere.mMisVolumeWeightFactor/n.mMisVolumeWeightFactor-phi)<1e-12,"sphere lens q eta MIS");
    Check(sphere.mVolumeMergeRadius==n.mVolumeMergeRadius,"sphere query radius unchanged");
    v.position=Point3(0,0,1.45);
    const VCMNormalization plane=VCMIntegrator::BoundaryVolumeNormalization(n,v);
    const double planePhi=.68359375; // distance/r=1/4: 1/2+3a/4-a^3/4
    Check(std::fabs(plane.mVolumeNormalization/n.mVolumeNormalization-1/planePhi)<1e-12,"single box face kernel");
    Check(std::fabs(plane.mMisVolumeWeightFactor/n.mMisVolumeWeightFactor-planePhi)<1e-12,"single box face MIS");
    for(const double gap:{1e-6,.1,.19}) {
        v.position=Point3(0,0,1+gap);
        const auto result=VCMIntegrator::BoundaryVolumeNormalization(n,v);
        const double D=1+gap;
        const double overlap=PI*std::pow(R+r-D,2)*(D*D+2*D*(R+r)-3*std::pow(R-r,2))/(12*D);
        const double fraction=1-overlap/((4.0/3)*PI*r*r*r);
        Check(std::fabs(result.mVolumeNormalization/n.mVolumeNormalization-1/fraction)<1e-12,
              "sphere cap range against independent lens");
    }
    for(const Point3& point:{Point3(1.45,0,0),Point3(-1.45,0,0),Point3(0,1.45,0),
                           Point3(0,-1.45,0),Point3(0,0,1.45),Point3(0,0,-1.45)}) {
        v.position=point;
        const auto result=VCMIntegrator::BoundaryVolumeNormalization(n,v);
        Check(std::fabs(result.mVolumeNormalization/n.mVolumeNormalization-1/planePhi)<1e-12,
              "all six box faces");
    }
    SetVolumeMergeRadius(n,.01,1,17);n.mVolumeObjects=job->GetObjects();v.position=Point3(0,0,1.2);
    Check(SameFactors(n,VCMIntegrator::BoundaryVolumeNormalization(n,v)),"no nearby cut factors bit-identical");
    SetVolumeMergeRadius(n,.2,1,17);n.mVolumeObjects=job->GetObjects();v.position=Point3(1.45,1.45,1.45);
    const VCMNormalization corner=VCMIntegrator::BoundaryVolumeNormalization(n,v);
    Check(SameFactors(n,corner),"multiple box cuts retain legacy kernel (residual)");
    if(argc==2 && std::string(argv[1])=="--strict-corner-pin")
        Check(corner.mVolumeNormalization>n.mVolumeNormalization,"strict corner missing-mass normalization");
    std::cout << "sphere phi " << phi << " plane phi " << planePhi << "\n" << passCount << " passed, " << failCount << " failed\n";
    safe_release(job);
    return failCount ? 1 : 0;
}
