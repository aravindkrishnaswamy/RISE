#include "FireProductionAcceptedFuelBudget.h"
#include <iostream>
struct Shape {std::size_t nx=2,ny=3,nz=4;float cellWidthM=.5f;};
struct Trace {
    std::uint32_t iteration=UINT32_C(0x80000002);
    std::uint64_t acceptedCandidateIdentity=19;
    std::vector<float> acceptedConservativeValues,scalarLowFlux,scalarFluxDelta;
    std::array<std::vector<float>,3> sharedFaceAlpha;
};
struct Owner {
    bool accepted=true;
    std::array<std::vector<Trace>,3> qualificationIterationTrace;
    std::array<std::uint32_t,3> acceptedPicardIterations={{0,3,0}};
    std::array<std::uint64_t,3> candidatePublicationIdentity={{0,19,0}};
    std::vector<float> conservativeValues;
};
int main(){
    Shape shape;constexpr std::size_t cells=24,faces=98;
    const std::array<std::size_t,3> counts={{36,32,30}},offsets={{0,36,68}};
    Owner owner;owner.conservativeValues.assign(9*cells,0);
    std::vector<float> beginning(9*cells,0),source(9*cells,0);
    Trace trace;trace.acceptedConservativeValues=owner.conservativeValues;
    trace.scalarLowFlux.assign(9*faces,0);trace.scalarFluxDelta.assign(9*faces,0);
    for(unsigned axis=0;axis<3;++axis)trace.sharedFaceAlpha[axis].assign(counts[axis],.5f);
    owner.qualificationIterationTrace[1].push_back(trace);
    FireProductionAcceptedFuelBudget::Result result;std::string error;
    int failed=0;auto check=[&](bool good,const char* label){if(!good){++failed;std::cerr<<label<<" "<<error<<'\n';}};
    auto run=[&](){return FireProductionAcceptedFuelBudget::Reduce(shape,.25,beginning,source,owner,result,error);};
    check(run()&&result.closureResidualKG==0,"zero fixture");
    const std::array<std::size_t,6> boundaryFaces={{0,2,0,6,0,24}};
    for(unsigned side=0;side<6;++side)for(float alpha:{0.f,.5f,1.f})for(float sign:{-1.f,1.f}){
        auto& t=owner.qualificationIterationTrace[1].back();t=trace;
        const unsigned axis=side/2;const auto face=boundaryFaces[side];
        t.scalarLowFlux[faces+offsets[axis]+face]=2*sign;
        t.scalarFluxDelta[faces+offsets[axis]+face]=4*sign;
        t.sharedFaceAlpha[axis][face]=alpha;
        const double outwardSign=side%2?1:-1;
        const double expectedLow=outwardSign*.25*2*sign;
        const double expectedCorrection=outwardSign*.25*4*sign*alpha;
        check(run(),"single boundary admitted");
        check(result.lowOutwardKGPerS[side]==expectedLow&&
            result.correctionOutwardKGPerS[side]==expectedCorrection,"axis sign and alpha");
        check(result.boundaryTransportKG==-(expectedLow+expectedCorrection)*.25&&
            result.divergenceTransportKG==result.boundaryTransportKG,"boundary/divergence telescope");
        check(result.closureResidualKG==-result.boundaryTransportKG,"reported nonzero closure");
        for(unsigned other=0;other<6;++other)if(other!=side)
            check(result.lowOutwardKGPerS[other]==0&&result.correctionOutwardKGPerS[other]==0,"no other side");
    }
    auto& t=owner.qualificationIterationTrace[1].back();t=trace;
    // Internal x face: cancels globally, not a boundary export.
    t.scalarLowFlux[faces+1]=8;t.scalarFluxDelta[faces+1]=-4;
    check(run()&&result.boundaryTransportKG==0&&result.divergenceTransportKG==0,"interior cancellation");
    t=trace;source[cells]=.5f;owner.conservativeValues[cells]=.5f;
    t.acceptedConservativeValues=owner.conservativeValues;
    check(run()&&result.frozenSourceKG==.0625&&result.stateChangeKG==.0625&&result.closureResidualKG==0,"source/state units");
    auto valid=t;
    t.iteration=2;check(!run(),"nonterminal refused");t=valid;
    t.iteration=UINT32_C(0x80000001);check(!run(),"stale iteration refused");t=valid;
    t.acceptedCandidateIdentity=20;check(!run(),"wrong candidate refused");t=valid;
    t.acceptedConservativeValues[cells]=.25f;check(!run(),"publication mismatch refused");t=valid;
    owner.accepted=false;check(!run(),"unaccepted owner refused");owner.accepted=true;
    owner.qualificationIterationTrace[0].push_back(t);owner.qualificationIterationTrace[1].clear();
    check(!run(),"wrong stage refused");owner.qualificationIterationTrace[1].push_back(valid);
    owner.qualificationIterationTrace[1].back().sharedFaceAlpha[0][0]=2;
    check(!run(),"invalid alpha refused");
    owner.qualificationIterationTrace[1].back()=valid;
    shape.nx=std::numeric_limits<std::size_t>::max();
    check(!run()&&error=="fuel budget geometry/time invalid","overflow geometry refused");shape.nx=2;
    auto rejectedAndCleared=[&](const char* label){result.stateChangeKG=17;
        result.lowOutwardKGPerS.fill(17);
        const bool refused=!run();bool cleared=result.stateChangeKG==0;
        for(double value:result.lowOutwardKGPerS)cleared=cleared&&value==0;
        check(refused&&cleared,label);
    };
    beginning[cells]=std::numeric_limits<float>::infinity();rejectedAndCleared("nonfinite beginning");beginning[cells]=0;
    source[cells]=std::numeric_limits<float>::infinity();rejectedAndCleared("nonfinite source");source[cells]=.5f;
    owner.conservativeValues[cells]=std::numeric_limits<float>::infinity();
    owner.qualificationIterationTrace[1].back().acceptedConservativeValues=owner.conservativeValues;
    rejectedAndCleared("nonfinite matched publication");owner.conservativeValues[cells]=.5f;
    owner.qualificationIterationTrace[1].back()=valid;
    owner.qualificationIterationTrace[1].back().scalarLowFlux[faces]=std::numeric_limits<float>::infinity();
    rejectedAndCleared("nonfinite low flux");owner.qualificationIterationTrace[1].back()=valid;
    owner.qualificationIterationTrace[1].back().scalarFluxDelta[faces]=std::numeric_limits<float>::infinity();
    rejectedAndCleared("nonfinite delta flux");owner.qualificationIterationTrace[1].back()=valid;
    owner.qualificationIterationTrace[1].back().sharedFaceAlpha[0][0]=std::numeric_limits<float>::infinity();
    rejectedAndCleared("nonfinite alpha");owner.qualificationIterationTrace[1].back()=valid;
    source.pop_back();rejectedAndCleared("source shape");source.push_back(0);
    owner.qualificationIterationTrace[1].back()=valid;
    auto& bottom=owner.qualificationIterationTrace[1].back();
    bottom.scalarLowFlux[faces+offsets[2]]=4;
    bottom.scalarLowFlux[faces+offsets[2]+5]=-8;
    std::vector<unsigned char> footprint(6,0);footprint[5]=1;
    check(FireProductionAcceptedFuelBudget::Reduce(shape,.25,beginning,source,owner,result,error,footprint)&&
        result.bottomFootprintAvailable&&result.bottomFootprintFaces==1&&
        result.bottomFootprintLowKGPerS[0]==-1&&result.bottomFootprintLowKGPerS[1]==2&&
        result.bottomFootprintLowKGPerS[0]+result.bottomFootprintLowKGPerS[1]==result.lowOutwardKGPerS[4],
        "bottom footprint partition with opposite signs");
    footprint[0]=2;check(!FireProductionAcceptedFuelBudget::Reduce(shape,.25,beginning,source,owner,result,error,footprint),"nonbinary footprint");
    footprint.pop_back();check(!FireProductionAcceptedFuelBudget::Reduce(shape,.25,beginning,source,owner,result,error,footprint),"footprint shape");
    using FireProductionAcceptedFuelBudget::DiagnosticComplete;
    check(DiagnosticComplete(true,"",6489,6497,true,true),"complete diagnostic");
    check(!DiagnosticComplete(true,"refused",6489,6497,true,true),"inherited success with error refused");
    check(!DiagnosticComplete(true,"",6489,6496,true,true),"short diagnostic refused");
    check(!DiagnosticComplete(true,"",6489,6497,false,true),"missing budget refused");
    check(!DiagnosticComplete(true,"",6489,6497,true,false),"changed input refused");
    std::cout<<"accepted fuel budget failures="<<failed<<'\n';return failed?1:0;
}
