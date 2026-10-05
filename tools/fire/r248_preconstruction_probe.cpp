// Qualification of actual source storage observation and native preconstructor
// refusal placement. No owner numerical work or full resource admission.
#include "src/Library/Utilities/FireProductionForce.h"
#include "src/Library/Utilities/FireProductionSourceKernel.h"
#include "src/Library/Utilities/FireCase.h"
#include <cstdio>
#include <cstdlib>
#include <new>

// Single-thread fixture injection, armed only around refusal publication.
static thread_local int allocationFailureCountdown=-1;
void* operator new(std::size_t bytes)
{
    if(allocationFailureCountdown==0){allocationFailureCountdown=-1;throw std::bad_alloc();}
    if(allocationFailureCountdown>0)--allocationFailureCountdown;
    if(void* memory=std::malloc(bytes==0u?1u:bytes))return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {std::free(memory);}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete[](void* memory) noexcept {::operator delete(memory);}
using namespace RISE;
using namespace RISE::FireSim;

#if defined(__APPLE__)
static bool sourceSeal(std::size_t nx,std::size_t ny,std::size_t nz,
    FireProductionFrozenSourcePacketSeal& seal,std::string& error)
{
    const auto& fuel=FireSimulationMethaneRecord::PhysicalV1();
    const auto& transport=FireSimulationTransportRecord::OpenV1();
    const auto& opacity=FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
    FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
    authored.poolDiameterM=0.03;authored.heatReleaseRateKW=0.10;
    authored.envelope={{0.0,0.0},{0.5,1.0},{1.0,1.0}};
    authored.durationS=1.0;authored.quality="draft";authored.seed=200u;authored.outputFramesPerS=4.0;
    FireCase::RecordV1 record;
    if(!FireCase::BuildMethaneV1(authored,fuel,
        {fuel.RecordId(),transport.RecordId(),opacity.RecordId()},record,error))return false;
    MethaneCellState ambient;ambient.temperatureK=300.0;
    ambient.producerPrecision=FireStateProducerPrecision::Binary32;
    double inverse=0.0;for(std::size_t i=0u;i<6u;++i){
        ambient.constituent[i]=fuel.AmbientMassFractions()[i];
        const auto* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
        if(!species)return false;inverse+=ambient.constituent[i]/species->molecularWeightKGPerKMol;}
    const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*300.0*inverse);
    for(double& value:ambient.constituent)value*=density;
    if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(ambient),300.0,
        ambient.sensibleEnergyJPerM3,&error))return false;
    FireProductionFrozenMethaneSourceRequest request;
    request.shape.nx=nx;request.shape.ny=ny;request.shape.nz=nz;request.shape.cellWidthM=0.025f;
    request.timeStepS=0x1p-16f;request.attemptIdentity=UINT64_C(0x2480000000000001);
    request.caseRecordEnvelope=record.envelopeBytes;const auto cells=request.shape.CellCount();
    request.beginningConservativeValues.resize(9u*cells);const auto tuple=ToConservativeVector(ambient);
    for(std::size_t c=0u;c<9u;++c)std::fill_n(request.beginningConservativeValues.begin()+c*cells,
        cells,static_cast<float>(tuple[c]));
    request.pilotCommandMask.assign(cells,0u);request.mixingTimeS.assign(cells,1.0);
    request.predictiveRadiation=false;request.workerCount=1u;
    return FireProductionCanonicalSourceAuthority::Build(request,seal,&error)&&
        FireProductionFrozenSourcePacketSealMatches(seal,&error);
}
#endif
int main()
{
#if defined(__APPLE__)
    std::string error;unsigned int green=0u,red=0u;
    for(const auto& shape:std::array<std::array<std::size_t,3>,3>{{{{4u,4u,4u}},{{6u,8u,5u}},{{8u,8u,8u}}}}){
        FireProductionFrozenSourcePacketSeal seal;
        if(!sourceSeal(shape[0],shape[1],shape[2],seal,error)){std::fprintf(stderr,"source failed: %s\n",error.c_str());return 2;}
        FireProductionPreconstructionResourceSample sample;
        if(!CaptureFireProductionPreconstructionResources(seal,sample)||
            sample.cells!=shape[0]*shape[1]*shape[2]||sample.sourceElementBytes!=108u*sample.cells||
            sample.sourceVectorCapacityBytes<sample.sourceElementBytes)return 3;
        for(std::size_t i=0u;i<11u;++i)if(sample.sourceSize[i]!=(i==1u?9u:1u)*sample.cells||
            sample.sourceCapacity[i]<sample.sourceSize[i])return 4;
        if(sample.processPeakRSSBytes>=(UINT64_C(256)<<20u))return 12;
        ++green;
        FireProductionProjectedHeunMetalOwnerRequest owner;owner.lineage.frozenSource=seal;
        owner.qualificationPreconstructionResourceProbe=true;
        FireProductionProjectedHeunMetalOwnerResult result;
        if(AttemptFireProductionProjectedHeunMetalOwner(owner,result,&error)||
            error!="preconstruction resource probe refuses owner construction"||result.accepted||
            result.commandCommitCount!=0u||!result.conservativeValues.empty()||
            !result.publicationPayloadRootSHA256.empty()||!result.preconstructionResources.sourceLayoutKnown)return 5;
        ++red;
        owner.qualificationPreconstructionSourceElementLimitBytes=1u;
        if(AttemptFireProductionProjectedHeunMetalOwner(owner,result,&error)||
            error!="preconstruction source element budget refused"||result.accepted||result.commandCommitCount!=0u)return 6;
        ++red;
        FireProductionResidentStepResult live;
        if(AttemptFireProductionProjectedHeunResidentStepMetal(owner,live,&result,&error)||
            error!="production resident step refuses qualification-only owner controls"||
            result.preconstructionResources.hostTelemetryKnown)return 7;
        ++red;
        if(!FireProductionFrozenSourcePacketSealMatches(seal,&error)||
            !FireProductionFrozenSourcePacketSealMatches(owner.lineage.frozenSource,&error)||
            seal.GasHeatReleaseWPerM3().data()==owner.lineage.frozenSource.GasHeatReleaseWPerM3().data())return 11;
        ++green;
        owner.qualificationPreconstructionResourceProbe=false;
        if(AttemptFireProductionProjectedHeunMetalOwner(owner,result,&error)||
            error!="preconstruction element limit requires resource probe"||result.preconstructionResources.hostTelemetryKnown)return 8;
        ++red;
        if(std::printf("{\"cells\":%llu,\"element_bytes\":%llu,\"capacity_bytes\":%llu,\"rss\":%llu,\"footprint\":%llu,\"peak_rss\":%llu,\"device_allocated\":%llu,\"device_limit\":%llu,\"max_buffer\":%llu}\n",
            static_cast<unsigned long long>(sample.cells),static_cast<unsigned long long>(sample.sourceElementBytes),
            static_cast<unsigned long long>(sample.sourceVectorCapacityBytes),static_cast<unsigned long long>(sample.currentRSSBytes),
            static_cast<unsigned long long>(sample.physicalFootprintBytes),static_cast<unsigned long long>(sample.processPeakRSSBytes),
            static_cast<unsigned long long>(sample.deviceAllocatedBytes),static_cast<unsigned long long>(sample.deviceWorkingSetLimitBytes),
            static_cast<unsigned long long>(sample.deviceMaxBufferLengthBytes))<0)return 9;
    }
    // Both result-reset and diagnostic allocation failures use the real entries.
    for(int countdown:std::array<int,2>{{0,1}}){
        FireProductionProjectedHeunMetalOwnerRequest refused;
        refused.qualificationPreconstructionSourceElementLimitBytes=1u;
        FireProductionProjectedHeunMetalOwnerResult stale;stale.accepted=true;
        stale.ownerPublicationIdentity=1u;stale.publicationPayloadRootSHA256="stale";
        error.clear();error.shrink_to_fit();allocationFailureCountdown=countdown;
        const bool accepted=AttemptFireProductionProjectedHeunMetalOwner(refused,stale,&error);
        const bool injected=allocationFailureCountdown==-1;allocationFailureCountdown=-1;
        if(accepted||!injected||stale.accepted||stale.ownerPublicationIdentity!=0u||
            !stale.publicationPayloadRootSHA256.empty())return 13;
        ++red;
        FireProductionResidentStepResult live;stale.accepted=true;stale.ownerPublicationIdentity=1u;
        stale.publicationPayloadRootSHA256="stale";error.clear();error.shrink_to_fit();
        allocationFailureCountdown=countdown;
        const bool productionAccepted=AttemptFireProductionProjectedHeunResidentStepMetal(refused,live,&stale,&error);
        const bool productionInjected=allocationFailureCountdown==-1;allocationFailureCountdown=-1;
        if(productionAccepted||!productionInjected||stale.accepted||stale.ownerPublicationIdentity!=0u||
            !stale.publicationPayloadRootSHA256.empty()||stale.preconstructionResources.hostTelemetryKnown)return 14;
        ++red;
    }
    FireProductionFrozenSourcePacketSeal empty;FireProductionPreconstructionResourceSample unavailable;
    if(CaptureFireProductionPreconstructionResources(empty,unavailable)||unavailable.sourceLayoutKnown||
        unavailable.sourceElementBytes!=0u)return 10;
    ++red;
    if(std::printf("{\"green\":%u,\"red\":%u,\"owner_numerical_work\":false,\"complete_admission\":false}\n",green,red)<0||std::fflush(stdout)!=0)return 9;
    return 0;
#else
    return 77;
#endif
}
