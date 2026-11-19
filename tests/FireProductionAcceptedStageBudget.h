#ifndef RISE_TEST_FIRE_PRODUCTION_ACCEPTED_STAGE_BUDGET_H
#define RISE_TEST_FIRE_PRODUCTION_ACCEPTED_STAGE_BUDGET_H

#include <algorithm>
#include <utility>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// Observation only. Rows 0..8 are Z tracer, seven species (including carbon),
// and sensible enthalpy. The tracer is NOT added to total species mass.
// Rates are the real-arithmetic interpretation of the accepted binary32
// low + alpha*delta operands; the residual retains solver rounding.
namespace FireProductionAcceptedStageBudget {
struct Row {
    long double beginning=0, ending=0, source=0, closure=0, interstepChange=0;
    std::array<long double,6> inflow{}, outflow{};
    // Bottom footprint separately from the rest of the bottom side.
    std::array<long double,2> bottomInflow{}, bottomOutflow{};
};
using Rows=std::array<Row,10>; // row 9 sums species 1..7, never tracer or energy
struct Step {
    Rows rows{};
    std::string inputRoot, publicationRoot, fluxRoot;
    double beginTime=0, dt=0;
    long double chemicalJ=0,radiatedJ=0,pilotJ=0,reactedFuelKG=0;
    std::uint64_t index=0, candidate=0;
};
inline std::string Hex(const unsigned char* bytes) {
    std::string result; const char* alphabet="0123456789abcdef";
    for(unsigned i=0;i<32;++i){result+=alphabet[bytes[i]>>4];result+=alphabet[bytes[i]&15];}
    return result;
}
inline float FloatAt(const std::vector<unsigned char>& bytes,std::size_t offset) {
    float value;std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
}
inline void Add(Row& to,const Row& from,long double scale) {
    to.beginning+=scale*from.beginning;to.ending+=scale*from.ending;
    to.source+=scale*from.source;to.closure+=scale*from.closure;to.interstepChange+=scale*from.interstepChange;
    for(unsigned side=0;side<6;++side){to.inflow[side]+=scale*from.inflow[side];to.outflow[side]+=scale*from.outflow[side];}
    for(unsigned region=0;region<2;++region){to.bottomInflow[region]+=scale*from.bottomInflow[region];to.bottomOutflow[region]+=scale*from.bottomOutflow[region];}
}

template<class Request,class Owner>
bool Reduce(const Request& request,const Owner& owner,std::uint64_t index,
    const std::vector<unsigned char>& footprint,Step& output,std::string& error) {
    output=Step();
    auto fail=[&](const char* message){error=message;return false;};
    const auto& transport=request.lineage.eos.physicalFlux.transport;
    const auto& shape=transport.shape;
    const std::size_t nx=shape.nx,ny=shape.ny,nz=shape.nz;
    const std::size_t limit=std::numeric_limits<std::size_t>::max()/1024u;
    if(!nx||!ny||!nz||nx>=limit||ny>=limit||nz>=limit||nx>limit/ny||nx*ny>limit/nz)
        return fail("accepted budget geometry overflow");
    const std::size_t cells=nx*ny*nz;
    const std::array<std::size_t,3> counts={{(nx+1)*ny*nz,nx*(ny+1)*nz,nx*ny*(nz+1)}};
    const std::array<std::size_t,3> offsets={{0,counts[0],counts[0]+counts[1]}};
    const std::size_t faces=counts[0]+counts[1]+counts[2];
    const double dt=request.lineage.frozenSource.TimeStepS();
    const double begin=request.lineage.frozenSource.BeginningTimeS();
    const double dx=shape.cellWidthM;
    if(!owner.accepted||!index||!std::isfinite(begin)||begin<0||!std::isfinite(dt)||dt<=0||
        !std::isfinite(dx)||dx<=0||!std::isfinite(begin+dt))return fail("accepted budget requires accepted finite event");
    const auto& payload=owner.acceptedBudgetPayload;
    if(payload.size()!=72u+26u*faces*sizeof(float)||owner.acceptedFaceAlpha.size()!=faces||
        transport.conservativeValues.size()!=9*cells||owner.conservativeValues.size()!=9*cells||
        request.lineage.frozenSource.SourceDelta().size()!=9*cells||footprint.size()!=nx*ny)
        return fail("accepted budget missing operands");
    std::uint64_t candidate=0;std::memcpy(&candidate,payload.data()+64u,8u);
    if(!candidate||candidate!=owner.candidatePublicationIdentity[1]||
        owner.inputPayloadRootSHA256.size()!=64u||owner.publicationPayloadRootSHA256.size()!=64u||
        owner.acceptedBudgetPayloadRootSHA256.size()!=64u||
        Hex(payload.data())!=owner.inputPayloadRootSHA256||
        Hex(payload.data()+32u)!=owner.publicationPayloadRootSHA256)
        return fail("accepted budget publication binding mismatch");
    for(unsigned char bit:footprint)if(bit>1)return fail("accepted budget footprint invalid");
    for(float alpha:owner.acceptedFaceAlpha)if(!std::isfinite(alpha)||alpha<0||alpha>1)
        return fail("accepted budget alpha invalid");
    Step result;result.index=index;result.beginTime=begin;result.dt=dt;result.candidate=candidate;
    result.inputRoot=owner.inputPayloadRootSHA256;result.publicationRoot=owner.publicationPayloadRootSHA256;
    result.fluxRoot=owner.acceptedBudgetPayloadRootSHA256;
    const long double area=static_cast<long double>(dx)*dx,volume=area*dx;
    for(unsigned field=0;field<9;++field){
        Row& row=result.rows[field];
        for(std::size_t cell=0;cell<cells;++cell){
            const auto at=field*cells+cell;
            const float first=transport.conservativeValues[at],last=owner.conservativeValues[at],
                source=request.lineage.frozenSource.SourceDelta()[at];
            if(!std::isfinite(first)||!std::isfinite(last)||!std::isfinite(source))return fail("accepted budget nonfinite state");
            row.beginning+=first;row.ending+=last;row.source+=source;
        }
        row.beginning*=volume;row.ending*=volume;row.source*=volume;
        for(unsigned side=0;side<6;++side){
            const unsigned axis=side/2u;
            const std::size_t uCount=axis==0?ny:nx,vCount=axis==2?ny:nz;
            for(std::size_t v=0;v<vCount;++v)for(std::size_t u=0;u<uCount;++u){
                const std::size_t x=axis==0?(side%2?nx:0):u;
                const std::size_t y=axis==1?(side%2?ny:0):(axis==0?u:v);
                const std::size_t z=axis==2?(side%2?nz:0):v;
                const std::size_t face=offsets[axis]+x+(nx+(axis==0))*(y+(ny+(axis==1))*z);
                const std::size_t at=field*faces+face;
                const float low=FloatAt(payload,72u+at*sizeof(float));
                const float delta=FloatAt(payload,72u+(9u*faces+at)*sizeof(float));
                if(!std::isfinite(low)||!std::isfinite(delta))return fail("accepted budget nonfinite boundary flux");
                const long double signedAmount=(static_cast<long double>(low)+
                    static_cast<long double>(owner.acceptedFaceAlpha[face])*delta)*area*dt*(side%2?1:-1);
                row.inflow[side]+=std::max(0.0L,-signedAmount);row.outflow[side]+=std::max(0.0L,signedAmount);
                if(side==4){const unsigned region=footprint[y*nx+x];
                    row.bottomInflow[region]+=std::max(0.0L,-signedAmount);
                    row.bottomOutflow[region]+=std::max(0.0L,signedAmount);}
            }
        }
        row.closure=row.ending-row.beginning-row.source;
        for(unsigned side=0;side<6;++side)row.closure+=row.outflow[side]-row.inflow[side];
        if(!std::isfinite(row.closure))return fail("accepted budget reduction overflow");
    }
    const auto& source=request.lineage.frozenSource;
    if(source.GasHeatReleaseWPerM3().size()!=cells||source.SootHeatReleaseWPerM3().size()!=cells||
        source.RadiativeCoolingWPerM3().size()!=cells||source.PilotEnergyDeltaJPerM3().size()!=cells||
        source.ReactedFuelKGPerM3().size()!=cells)return fail("accepted budget source ledger missing");
    for(std::size_t cell=0;cell<cells;++cell){
        result.chemicalJ+=(static_cast<long double>(source.GasHeatReleaseWPerM3()[cell])+source.SootHeatReleaseWPerM3()[cell])*volume*dt;
        result.radiatedJ+=static_cast<long double>(source.RadiativeCoolingWPerM3()[cell])*volume*dt;
        result.pilotJ+=static_cast<long double>(source.PilotEnergyDeltaJPerM3()[cell])*volume;
        result.reactedFuelKG+=static_cast<long double>(source.ReactedFuelKGPerM3()[cell])*volume;
    }
    if(!std::isfinite(result.chemicalJ)||!std::isfinite(result.radiatedJ)||!std::isfinite(result.pilotJ)||
        !std::isfinite(result.reactedFuelKG))return fail("accepted budget source ledger nonfinite");
    for(unsigned species=1;species<=7;++species)Add(result.rows[9],result.rows[species],1);
    // Total mass direction is the sign of the sum of constituent fluxes at
    // EACH face. Summing species inflows separately would count counterdiffusion
    // as bulk entrainment. Keep the individual species directions above.
    Row& mass=result.rows[9];mass.inflow.fill(0);mass.outflow.fill(0);
    mass.bottomInflow.fill(0);mass.bottomOutflow.fill(0);
    for(unsigned side=0;side<6;++side){
        const unsigned axis=side/2;
        const std::size_t uCount=axis==0?ny:nx,vCount=axis==2?ny:nz;
        for(std::size_t v=0;v<vCount;++v)for(std::size_t u=0;u<uCount;++u){
            const std::size_t x=axis==0?(side%2?nx:0):u;
            const std::size_t y=axis==1?(side%2?ny:0):(axis==0?u:v);
            const std::size_t z=axis==2?(side%2?nz:0):v;
            const std::size_t face=offsets[axis]+x+(nx+(axis==0))*(y+(ny+(axis==1))*z);
            long double amount=0;
            for(unsigned field=1;field<=7;++field){const std::size_t at=field*faces+face;
                amount+=static_cast<long double>(FloatAt(payload,72u+at*sizeof(float)))+
                    static_cast<long double>(owner.acceptedFaceAlpha[face])*FloatAt(payload,72u+(9u*faces+at)*sizeof(float));}
            amount*=area*dt*(side%2?1:-1);
            mass.inflow[side]+=std::max(0.0L,-amount);mass.outflow[side]+=std::max(0.0L,amount);
            if(side==4){const unsigned region=footprint[y*nx+x];
                mass.bottomInflow[region]+=std::max(0.0L,-amount);mass.bottomOutflow[region]+=std::max(0.0L,amount);}
        }
    }
    output=std::move(result);error.clear();return true;
}

// The pending observation becomes a ledger entry only after all launcher
// acceptance gates have passed. Rejected attempts, duplicates, gaps and a
// changed clock cannot contaminate either ledger.
struct Ledger {
    std::uint64_t count=0;
    double endTime=0;
    Rows full{},statistics{};
    long double chemicalJ=0,radiatedJ=0,pilotJ=0,reactedFuelKG=0;
    long double statisticsChemicalJ=0,statisticsRadiatedJ=0,statisticsPilotJ=0,statisticsReactedFuelKG=0;
    bool statisticsBegun=false;
    bool Commit(const Step& step,bool accepted,double statisticsStart,std::string& error) {
        if(!accepted||step.index!=count+1||step.beginTime!=endTime||
            !(step.dt>0)||!std::isfinite(step.dt)||!std::isfinite(step.beginTime+step.dt)||
            !std::isfinite(statisticsStart)||statisticsStart<0){error="budget ledger noncontiguous/unaccepted event";return false;}
        const long double fraction=std::max(0.0,step.beginTime+step.dt-
            std::max(step.beginTime,statisticsStart))/step.dt;
        for(unsigned field=0;field<10;++field){
            const long double fullBegin=count?full[field].beginning:step.rows[field].beginning;
            const long double interstep=count?step.rows[field].beginning-full[field].ending:0;
            Add(full[field],step.rows[field],1);
            full[field].interstepChange+=interstep;full[field].closure+=interstep;
            full[field].beginning=fullBegin;full[field].ending=step.rows[field].ending;
            if(fraction>0){
                // The clipped first statistics step uses a fraction of its
                // accepted increment, not an invented interior solver state.
                const long double statsBegin=statisticsBegun?statistics[field].beginning:
                    step.rows[field].ending-fraction*(step.rows[field].ending-step.rows[field].beginning);
                const long double statsInterstep=statisticsBegun?step.rows[field].beginning-statistics[field].ending:0;
                Add(statistics[field],step.rows[field],fraction);
                statistics[field].interstepChange+=statsInterstep;statistics[field].closure+=statsInterstep;
                statistics[field].beginning=statsBegin;statistics[field].ending=step.rows[field].ending;
            }
        }
        statisticsBegun=statisticsBegun||fraction>0;
        chemicalJ+=step.chemicalJ;radiatedJ+=step.radiatedJ;pilotJ+=step.pilotJ;reactedFuelKG+=step.reactedFuelKG;
        statisticsChemicalJ+=fraction*step.chemicalJ;statisticsRadiatedJ+=fraction*step.radiatedJ;
        statisticsPilotJ+=fraction*step.pilotJ;statisticsReactedFuelKG+=fraction*step.reactedFuelKG;
        ++count;endTime=step.beginTime+step.dt;error.clear();return true;
    }
};
inline bool Fixture() {
    struct Shape {std::size_t nx=2,ny=3,nz=4;double cellWidthM=0.5;};
    struct Source {
        std::vector<float> delta=std::vector<float>(9*24,0.5f),zero=std::vector<float>(24,0);
        double TimeStepS() const{return 0.25;} double BeginningTimeS() const{return 0;}
        const auto& SourceDelta() const{return delta;}
        const auto& GasHeatReleaseWPerM3() const{return zero;}
        const auto& SootHeatReleaseWPerM3() const{return zero;}
        const auto& RadiativeCoolingWPerM3() const{return zero;}
        const auto& PilotEnergyDeltaJPerM3() const{return zero;}
        const auto& ReactedFuelKGPerM3() const{return zero;}
    };
    struct Transport {Shape shape;std::vector<float> conservativeValues=std::vector<float>(216,10);};
    struct Physical {Transport transport;};struct EOS {Physical physicalFlux;};
    struct Lineage {EOS eos;Source frozenSource;};struct Request {Lineage lineage;};
    struct Owner {
        bool accepted=true;
        std::vector<float> conservativeValues=std::vector<float>(216,10.5f);
        std::vector<float> acceptedFaceAlpha=std::vector<float>(98,0.5f);
        std::vector<unsigned char> acceptedBudgetPayload=std::vector<unsigned char>(72+26*98*4,0);
        std::string inputPayloadRootSHA256=std::string(64,'0'),publicationPayloadRootSHA256=std::string(64,'0'),
            acceptedBudgetPayloadRootSHA256=std::string(64,'a');
        std::array<std::uint64_t,3> candidatePublicationIdentity={{0,123,0}};
    };
    Request request;Owner owner;
    std::memcpy(owner.acceptedBudgetPayload.data()+64,&owner.candidatePublicationIdentity[1],8);
    const std::array<std::size_t,3> offsets={{0,36,68}},counts={{36,32,30}};
    auto put=[&](Owner& target,std::size_t field,std::size_t face,float low,float delta){
        std::memcpy(target.acceptedBudgetPayload.data()+72+(field*98+face)*4,&low,4);
        std::memcpy(target.acceptedBudgetPayload.data()+72+(9*98+field*98+face)*4,&delta,4);};
    // Uniform but distinct axis/species flux, with nonzero limiter correction.
    // Negative O2 creates genuine counterdiffusion, independently of bulk flow.
    for(unsigned field=0;field<9;++field)for(unsigned axis=0;axis<3;++axis)
        for(std::size_t face=0;face<counts[axis];++face)
            put(owner,field,offsets[axis]+face,field==2?-20.0f:float(2*(field+axis+1)),2.0f);
    std::vector<unsigned char> footprint(6,0);footprint[0]=1;
    Step step;std::string error;
    if(!Reduce(request,owner,1,footprint,step,error))return false;
    const long double areas[]={12*0.25L,8*0.25L,6*0.25L};
    auto expected=[&](unsigned field,unsigned axis){return (field==2?-19.0L:2.0L*(field+axis+1)+1)*areas[axis]*0.25L;};
    for(unsigned field=0;field<9;++field){const Row& row=step.rows[field];
        if(row.beginning!=30||row.ending!=31.5L||row.source!=1.5L||row.closure!=0)return false;
        for(unsigned axis=0;axis<3;++axis){const auto rate=expected(field,axis);
            if(row.inflow[2*axis]!=std::max(0.0L,rate)||row.outflow[2*axis+1]!=std::max(0.0L,rate)||
                row.outflow[2*axis]!=std::max(0.0L,-rate)||row.inflow[2*axis+1]!=std::max(0.0L,-rate))return false;}
    }
    for(unsigned axis=0;axis<3;++axis){long double mass=0;for(unsigned field=1;field<=7;++field)mass+=expected(field,axis);
        if(step.rows[9].inflow[2*axis]!=mass||step.rows[9].outflow[2*axis+1]!=mass||step.rows[9].outflow[2*axis]!=0)return false;}
    if(step.rows[1].bottomInflow[1]!=9*0.25L*0.25L||step.rows[1].bottomInflow[0]!=5*9*0.25L*0.25L)return false;
    // Independent asymmetric expected side values catch sign and side swaps.
    auto wrongSign=owner;put(wrongSign,1,0,-4,-2);Step mutant;
    if(!Reduce(request,wrongSign,1,footprint,mutant,error)||mutant.rows[1].inflow[0]==step.rows[1].inflow[0])return false;
    auto wrongSide=owner;put(wrongSide,1,0,6,2);put(wrongSide,1,36,4,2);
    if(!Reduce(request,wrongSide,1,footprint,mutant,error)||mutant.rows[1].inflow[0]==step.rows[1].inflow[0])return false;
    auto missing=owner;missing.acceptedBudgetPayload.pop_back();
    if(Reduce(request,missing,1,footprint,mutant,error))return false;
    missing=owner;missing.candidatePublicationIdentity[1]++;
    if(Reduce(request,missing,1,footprint,mutant,error))return false;
    missing=owner;missing.accepted=false;
    if(Reduce(request,missing,1,footprint,mutant,error))return false;
    Ledger ledger;
    if(ledger.Commit(step,false,0.125,error)||ledger.count||ledger.endTime!=0)return false;
    if(!ledger.Commit(step,true,0.125,error)||ledger.count!=1||ledger.endTime!=0.25||
        ledger.full[1].source!=1.5L||ledger.statistics[1].source!=0.75L||
        ledger.statistics[1].beginning!=30.75L||ledger.statistics[1].ending!=31.5L)return false;
    if(ledger.Commit(step,true,0.125,error)||ledger.count!=1)return false;
    auto gap=step;gap.index=3;gap.beginTime=0.25;
    if(ledger.Commit(gap,true,0.125,error)||ledger.count!=1)return false;
    gap.index=2;gap.beginTime=0.5;
    if(ledger.Commit(gap,true,0.125,error)||ledger.count!=1)return false;
    step.index=2;step.beginTime=0.25;
    if(!ledger.Commit(step,true,0.125,error)||ledger.full[1].source!=3||ledger.statistics[1].source!=2.25L||ledger.full[1].interstepChange!=-1.5L||ledger.full[1].closure!=-1.5L)return false;
    return true;
}

}
#endif
