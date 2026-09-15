#ifndef RISE_TEST_FIRE_PRODUCTION_ACCEPTED_FUEL_BUDGET_H
#define RISE_TEST_FIRE_PRODUCTION_ACCEPTED_FUEL_BUDGET_H

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// Host-only observation of already captured accepted R1 operands. This is
// neither a solver gate nor authority to publish or resume a state.
namespace FireProductionAcceptedFuelBudget {
inline bool DiagnosticComplete(bool accepted,const std::string& error,std::size_t beginningSteps,
    std::size_t finalSteps,bool budgetPresent,bool inputUnchanged)
{
    return accepted&&error.empty()&&beginningSteps<=std::numeric_limits<std::size_t>::max()-8u&&
        finalSteps==beginningSteps+8u&&budgetPresent&&inputUnchanged;
}
struct Result {
    std::array<double,6> lowOutwardKGPerS{}, correctionOutwardKGPerS{};
    double boundaryTransportKG=0, divergenceTransportKG=0;
    double stateChangeKG=0, frozenSourceKG=0, closureResidualKG=0;
    bool bottomFootprintAvailable=false;
    std::size_t bottomFootprintFaces=0;
    std::array<double,2> bottomFootprintLowKGPerS{}, bottomFootprintCorrectionKGPerS{};
};

template<class Shape, class Owner>
bool Reduce(const Shape& shape, double dt, const std::vector<float>& beginning,
    const std::vector<float>& source, const Owner& owner, Result& output, std::string& error,
    const std::vector<unsigned char>& bottomFootprint={})
{
    output=Result();
    auto fail=[&](const char* reason){error=reason;return false;};
    const std::size_t nx=shape.nx,ny=shape.ny,nz=shape.nz;
    const double dx=shape.cellWidthM;
    // Bound arithmetic before multiplying packed field sizes.
    const std::size_t limit=std::numeric_limits<std::size_t>::max()/128u;
    if(!nx||!ny||!nz||nx>=limit||ny>=limit||nz>=limit||
        nx>limit/ny||nx*ny>limit/nz||!std::isfinite(dx)||dx<=0||
        !std::isfinite(dt)||dt<=0)return fail("fuel budget geometry/time invalid");
    const std::size_t cells=nx*ny*nz;
    if(!bottomFootprint.empty()&&bottomFootprint.size()!=nx*ny)
        return fail("fuel budget bottom footprint shape mismatch");
    for(unsigned char value:bottomFootprint)if(value>1u)return fail("fuel budget bottom footprint invalid");
    const std::array<std::size_t,3> counts={{(nx+1)*ny*nz,nx*(ny+1)*nz,nx*ny*(nz+1)}};
    const std::array<std::size_t,3> offsets={{0,counts[0],counts[0]+counts[1]}};
    const std::size_t faces=counts[0]+counts[1]+counts[2];
    if(!owner.accepted||owner.qualificationIterationTrace[1].empty())
        return fail("fuel budget requires accepted R1 observer");
    const auto& trace=owner.qualificationIterationTrace[1].back();
    if(trace.iteration==UINT32_MAX||(trace.iteration&UINT32_C(0x80000000))==0||
        (trace.iteration&UINT32_C(0x7fffffff))+1u!=owner.acceptedPicardIterations[1]||
        trace.acceptedCandidateIdentity==0||
        trace.acceptedCandidateIdentity!=owner.candidatePublicationIdentity[1])
        return fail("fuel budget terminal R1 identity mismatch");
    if(beginning.size()!=9*cells||source.size()!=9*cells||
        trace.acceptedConservativeValues.size()!=9*cells||owner.conservativeValues.size()!=9*cells||
        trace.scalarLowFlux.size()!=9*faces||trace.scalarFluxDelta.size()!=9*faces)
        return fail("fuel budget packed shape mismatch");
    if(std::memcmp(trace.acceptedConservativeValues.data(),owner.conservativeValues.data(),
        9*cells*sizeof(float))!=0)return fail("fuel budget publication bytes mismatch");
    for(unsigned axis=0;axis<3;++axis)
        if(trace.sharedFaceAlpha[axis].size()!=counts[axis])return fail("fuel budget alpha shape mismatch");
    for(std::size_t cell=0;cell<cells;++cell)
        if(!std::isfinite(beginning[cells+cell])||!std::isfinite(source[cells+cell])||
            !std::isfinite(owner.conservativeValues[cells+cell]))return fail("fuel budget state nonfinite");
    for(unsigned axis=0;axis<3;++axis)for(std::size_t face=0;face<counts[axis];++face){
        const float alpha=trace.sharedFaceAlpha[axis][face];
        if(!std::isfinite(alpha)||alpha<0||alpha>1||
            !std::isfinite(trace.scalarLowFlux[faces+offsets[axis]+face])||
            !std::isfinite(trace.scalarFluxDelta[faces+offsets[axis]+face]))
            return fail("fuel budget flux/alpha invalid");
    }
    auto faceIndex=[&](unsigned axis,std::size_t x,std::size_t y,std::size_t z){
        return axis==0?(z*ny+y)*(nx+1)+x:axis==1?(z*(ny+1)+y)*nx+x:(z*ny+y)*nx+x;
    };
    auto low=[&](unsigned axis,std::size_t face)->long double{
        return trace.scalarLowFlux[faces+offsets[axis]+face];
    };
    auto correction=[&](unsigned axis,std::size_t face)->long double{
        return static_cast<long double>(trace.sharedFaceAlpha[axis][face])*
            trace.scalarFluxDelta[faces+offsets[axis]+face];
    };
    std::array<long double,6> lowSum{},correctionSum{};
    std::array<long double,2> footprintLow{},footprintCorrection{};
    long double divergence=0,stateChange=0,sourceSum=0;
    for(std::size_t z=0;z<nz;++z)for(std::size_t y=0;y<ny;++y)for(std::size_t x=0;x<nx;++x){
        const std::size_t cell=(z*ny+y)*nx+x;
        stateChange+=static_cast<long double>(owner.conservativeValues[cells+cell])-beginning[cells+cell];
        sourceSum+=source[cells+cell];
        const std::array<std::size_t,3> position={{x,y,z}},extent={{nx,ny,nz}};
        for(unsigned axis=0;axis<3;++axis){
            const auto lower=faceIndex(axis,x,y,z);
            const auto upper=faceIndex(axis,x+(axis==0),y+(axis==1),z+(axis==2));
            divergence+=low(axis,lower)-low(axis,upper)+correction(axis,lower)-correction(axis,upper);
            if(position[axis]==0){lowSum[2*axis]-=low(axis,lower);correctionSum[2*axis]-=correction(axis,lower);}
            if(axis==2&&z==0&&!bottomFootprint.empty()){
                const unsigned region=bottomFootprint[y*nx+x]?1u:0u;
                footprintLow[region]-=low(axis,lower);
                footprintCorrection[region]-=correction(axis,lower);
            }
            if(position[axis]+1==extent[axis]){lowSum[2*axis+1]+=low(axis,upper);correctionSum[2*axis+1]+=correction(axis,upper);}
        }
    }
    const long double area=static_cast<long double>(dx)*dx,volume=area*dx;
    Result result;long double outward=0;
    result.bottomFootprintAvailable=!bottomFootprint.empty();
    for(unsigned char value:bottomFootprint)result.bottomFootprintFaces+=value;
    for(unsigned region=0;region<2;++region){
        result.bottomFootprintLowKGPerS[region]=static_cast<double>(footprintLow[region]*area);
        result.bottomFootprintCorrectionKGPerS[region]=static_cast<double>(footprintCorrection[region]*area);
        if(!std::isfinite(result.bottomFootprintLowKGPerS[region])||
            !std::isfinite(result.bottomFootprintCorrectionKGPerS[region]))return fail("fuel budget footprint overflow");
    }
    for(unsigned side=0;side<6;++side){
        result.lowOutwardKGPerS[side]=static_cast<double>(lowSum[side]*area);
        result.correctionOutwardKGPerS[side]=static_cast<double>(correctionSum[side]*area);
        outward+=lowSum[side]+correctionSum[side];
    }
    result.boundaryTransportKG=static_cast<double>(-outward*area*dt);
    result.divergenceTransportKG=static_cast<double>(divergence*area*dt);
    result.stateChangeKG=static_cast<double>(stateChange*volume);
    result.frozenSourceKG=static_cast<double>(sourceSum*volume);
    result.closureResidualKG=static_cast<double>((stateChange-sourceSum)*volume+outward*area*dt);
    if(!std::isfinite(result.closureResidualKG)||!std::isfinite(result.boundaryTransportKG)||
        !std::isfinite(result.divergenceTransportKG)||!std::isfinite(result.stateChangeKG)||
        !std::isfinite(result.frozenSourceKG))return fail("fuel budget reduction overflow");
    for(unsigned side=0;side<6;++side)if(!std::isfinite(result.lowOutwardKGPerS[side])||
        !std::isfinite(result.correctionOutwardKGPerS[side]))return fail("fuel budget side overflow");
    output=result;error.clear();return true;
}
}
#endif
