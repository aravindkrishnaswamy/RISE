#include "FireProductionSharedOwnerInput.h"
#include "../src/Library/Utilities/FireCase.h"
#include <cstdio>
#include <iostream>

int main()
{
    using namespace RISE;
    using namespace FireProductionSharedOwnerInput;
    const auto& fuel=FireSimulationMethaneRecord::PhysicalV1();
    std::string error;FireCase::AuthoredV1 authored;
    authored.fuelRecordId=fuel.RecordId();authored.poolDiameterM=0.03;
    authored.heatReleaseRateKW=0.1;authored.envelope={{0.0,0.0},{0.5,1.0},{1.0,1.0}};
    authored.durationS=1.0;authored.quality="draft";authored.seed=200u;authored.outputFramesPerS=4.0;
    FireCase::RecordV1 record;
    if(!FireCase::BuildMethaneV1(authored,fuel,{fuel.RecordId(),
        FireSimulationTransportRecord::OpenV1().RecordId(),
        FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordId()},record,error)){
        std::cerr<<error<<'\n';return 1;}
    FireProductionFrozenMethaneSourceRequest input;
    input.shape.nx=4u;input.shape.ny=4u;input.shape.nz=4u;input.shape.cellWidthM=0.025f;
    input.timeStepS=0x1p-16f;input.beginningTimeS=0.5*record.derived.pilotDurationMultiplier*
        record.derived.flowThroughTimeS;input.attemptIdentity=0x214u;
    input.caseRecordEnvelope=record.envelopeBytes;
    const std::size_t cells=input.shape.CellCount();
    double inverseWeight=0.0;
    for(std::size_t species=0u;species<6u;++species)
        inverseWeight+=fuel.AmbientMassFractions()[species]/
            fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str())->molecularWeightKGPerKMol;
    const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*300.0*inverseWeight);
    std::array<float,9> ambient{};std::array<double,7> enthalpy{};
    if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(300.0,enthalpy.data(),enthalpy.size(),&error))return 1;
    double energy=0.0;for(std::size_t species=0u;species<6u;++species){
        ambient[1u+species]=static_cast<float>(density*fuel.AmbientMassFractions()[species]);
        energy+=static_cast<double>(ambient[1u+species])*enthalpy[species];}
    // Component zero is rho*Z, not gas density.
    ambient[0]=0.0f;ambient[8]=static_cast<float>(energy);
    input.beginningConservativeValues.resize(9u*cells);
    for(std::size_t component=0u;component<9u;++component)
        for(std::size_t cell=0u;cell<cells;++cell)
            input.beginningConservativeValues[component*cells+cell]=ambient[component];
    input.eligibilityBeginningConservativeValues=input.beginningConservativeValues;
    input.pilotCommandMask.assign(cells,1u);input.sourceBoundaryContactMask.assign(cells,0u);
    input.mixingTimeS.assign(cells,1.0);
    CanonicalSourceCapture source;
    if(!source.Build(input,error)){std::cerr<<"canonical source: "<<error<<'\n';return 1;}
    FireProductionProjectedHeunMetalOwnerRequest live;
    auto& eos=live.lineage.eos;auto& flux=eos.physicalFlux;auto& transport=flux.transport;
    transport.shape=input.shape;transport.stage=FireProductionProjectedHeunStage::R0;
    transport.attemptIdentity=source.Source().AttemptIdentity();
    transport.parentCandidateIdentity=source.Source().BeginningStateIdentity();
    transport.projectionIdentity=source.Source().PacketIdentity();
    transport.conservativeValues=input.beginningConservativeValues;
    transport.temperatureK=source.Source().BeginningTemperatureK();
    transport.boundary.fill(FireProductionProjectionPressureOpen);
    for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=FireProductionProjectionFaceCount(input.shape,axis);
        live.beginningMomentumKGPerM2S[axis].resize(faces);
        for(std::size_t face=0u;face<faces;++face)
            live.beginningMomentumKGPerM2S[axis][face]=static_cast<float>(0.001*(1u+face+axis));
        transport.projectedVelocityMPerS[axis].assign(faces,0.0f);}
    for(unsigned int side=0u;side<6u;++side){transport.fuelInletBoundaryFace[side].assign(16u,0u);
        flux.pressureOpenInflow[side].assign(16u,0u);}
    flux.ambient=ambient;flux.ambientTemperatureK=300.0f;
    const auto& basis=fuel.ConservativeReconstruction();flux.nullity=basis.nullity;
    flux.nullspaceBasis.assign(basis.orthonormalBasis.begin(),basis.orthonormalBasis.end());
    flux.coordinateProjector.assign(flux.nullity*flux.nullity,0.0f);
    for(std::size_t i=0u;i<flux.nullity;++i)flux.coordinateProjector[i*flux.nullity+i]=1.0f;
    eos.sourceDelta=source.Source().SourceDelta();eos.producingStage=FireProductionScalarEOSStage::QStar;
    eos.producerPrecision=FireStateProducerPrecision::Binary32;eos.caseRecordEnvelope=record.envelopeBytes;
    eos.candidateTimeStepS=input.timeStepS;live.lineage.frozenSource=source.Source();
    live.ambientDensityKGPerM3=static_cast<float>(density);live.projectionTolerancePerS=1.0e-3f;
    live.gravityMPerS2={{0.0f,0.0f,-9.81f}};
    live.endpointVelocityToleranceMPerS=1.0e-4f;live.maximumPicardIterations=16u;
    OwnerCapture captured;
    if(!captured.Capture(source,input,live,error)){std::cerr<<"owner capture: "<<error<<'\n';return 1;}
    bool passed=captured.IsCaptured()&&!captured.Payload().empty();
    const auto& mirror=captured.Mirror();
    for(std::size_t i=0u;i<input.beginningConservativeValues.size();++i)
        passed=passed&&mirror.beginningConservativeValues[i]==static_cast<double>(input.beginningConservativeValues[i]);
    for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t i=0u;i<live.beginningMomentumKGPerM2S[axis].size();++i)
        passed=passed&&mirror.beginningMomentumKGPerM2S[axis][i]==static_cast<double>(live.beginningMomentumKGPerM2S[axis][i]);
    passed=passed&&mirror.source.SourceDelta()==std::vector<double>(source.Source().SourceDelta().begin(),source.Source().SourceDelta().end());
    std::size_t nonzeroSource=0u;for(const auto value:source.Source().SourceDelta())if(value!=0.0f)++nonzeroSource;
    passed=passed&&nonzeroSource>0u&&mirror.scalarContract.shape.nx==4u&&
        mirror.physicalContract.shape.cellWidthM==static_cast<double>(input.shape.cellWidthM)&&
        mirror.forceContract.shape.nz==4u&&mirror.scalarContract.timeStepS==static_cast<double>(input.timeStepS)&&
        mirror.forceContract.timeStepS==static_cast<double>(input.timeStepS)&&
        mirror.attemptIdentity==input.attemptIdentity&&mirror.caseRecordEnvelope==record.envelopeBytes&&
        mirror.forceContract.gravityMPerS2[2]==static_cast<double>(live.gravityMPerS2[2])&&
        mirror.forceContract.vremanCoefficient==static_cast<double>(live.vremanCoefficient)&&
        mirror.forceContract.ambientDensityKGPerM3==static_cast<double>(live.ambientDensityKGPerM3)&&
        mirror.physicalContract.ambientTemperatureK==static_cast<double>(flux.ambientTemperatureK)&&
        mirror.projectionTolerancePerS==static_cast<double>(live.projectionTolerancePerS)&&
        mirror.endpointVelocityToleranceMPerS==static_cast<double>(live.endpointVelocityToleranceMPerS)&&
        mirror.maximumPicardIterations==live.maximumPicardIterations&&mirror.scalarContract.nullity==flux.nullity&&
        mirror.scalarContract.nullspaceBasis==std::vector<double>(flux.nullspaceBasis.begin(),flux.nullspaceBasis.end())&&
        mirror.scalarContract.coordinateProjector==std::vector<double>(flux.coordinateProjector.begin(),flux.coordinateProjector.end());
    for(unsigned int side=0u;side<6u;++side)
        passed=passed&&mirror.scalarContract.boundary[side]==static_cast<RISEFireProductionFP64::FireProductionProjectionBoundary>(transport.boundary[side])&&
            mirror.physicalContract.boundary[side]==mirror.scalarContract.boundary[side]&&
            mirror.forceContract.boundary[side]==mirror.scalarContract.boundary[side];
    for(std::size_t component=0u;component<9u;++component)
        passed=passed&&mirror.scalarContract.ambient[component]==static_cast<double>(ambient[component])&&
            mirror.physicalContract.ambient[component]==static_cast<double>(ambient[component]);
    // Same packet object and bytes, different omitted control: refuse before a
    // mirror can be admitted. This is not an expected numerical dose difference.
    auto changed=input;changed.eligibilityBeginningConservativeValues[0]=0.25f;
    OwnerCapture eligibilityRED;passed=passed&&!eligibilityRED.Capture(source,changed,live,error)&&
        !eligibilityRED.IsCaptured()&&eligibilityRED.Payload().empty();
    changed=input;changed.sourceBoundaryContactMask[0]=1u;
    OwnerCapture contactRED;passed=passed&&!contactRED.Capture(source,changed,live,error);
    changed=input;changed.mixingTimeS[0]=0.5;
    OwnerCapture mixingRED;passed=passed&&!mixingRED.Capture(source,changed,live,error);
    auto stale=live;stale.lineage.eos.physicalFlux.transport.parentCandidateIdentity^=1u;
    OwnerCapture staleRED;passed=passed&&!staleRED.Capture(source,input,stale,error);
    auto dose=live;dose.lineage.eos.sourceDelta[0]=-0.0f;
    OwnerCapture doseRED;passed=passed&&!doseRED.Capture(source,input,dose,error);
    auto mutant=live;mutant.lineage.eos.qualificationCPUProducedCandidate=true;
    OwnerCapture nestedRED;passed=passed&&!nestedRED.Capture(source,input,mutant,error);
    CanonicalSourceCapture unsealed;OwnerCapture unsealedRED;
    passed=passed&&!unsealedRED.Capture(unsealed,input,live,error);
    passed=passed&&!source.Build(input,error)&&!captured.Capture(source,input,live,error);
    auto changedMomentum=live;changedMomentum.beginningMomentumKGPerM2S[0][7]=0.25f;
    OwnerCapture momentum;passed=passed&&momentum.Capture(source,input,changedMomentum,error)&&
        momentum.Payload()!=captured.Payload()&&momentum.Mirror().beginningMomentumKGPerM2S[0][7]==0.25;
    RISEFireProductionFP64::FireProductionProjectionShape tier8;
    tier8.nx=69u;tier8.ny=69u;tier8.nz=106u;tier8.cellWidthM=0.025;
    std::uint64_t bytes=0u;
    passed=passed&&RISEFireProductionFP64::FireProductionProjectedHeunCPUOwnerWorkingSetBytes(tier8,bytes)&&
        bytes==UINT64_C(2296136184)&&bytes>(UINT64_C(2)<<30u);
    const auto capacity=RISEFireProductionFP64::FireProductionFP64QualificationOwnerCapacityBytes;
    passed=passed&&capacity==(UINT64_C(4)<<30u)&&
        capacity/sizeof(double)==(UINT64_C(2)<<30u)/sizeof(float)&&
        RISEFireProductionFP64::FireProductionFP64QualificationOwnerWorkingSetFits(bytes)&&
        RISEFireProductionFP64::FireProductionFP64QualificationOwnerWorkingSetFits(capacity)&&
        !RISEFireProductionFP64::FireProductionFP64QualificationOwnerWorkingSetFits(capacity+1u)&&
        !RISEFireProductionFP64::FireProductionFP64QualificationOwnerWorkingSetFits(UINT64_MAX);
    RISEFireProductionFP64::FireProductionProjectionShape oversized=tier8;
    oversized.nx=1024u;oversized.ny=1024u;oversized.nz=1024u;std::uint64_t oversizedBytes=0u;
    passed=passed&&RISEFireProductionFP64::FireProductionProjectedHeunCPUOwnerWorkingSetBytes(oversized,oversizedBytes)&&
        !RISEFireProductionFP64::FireProductionFP64QualificationOwnerWorkingSetFits(oversizedBytes);
    RISE::FireProductionProjectionShape originalTier8;originalTier8.nx=tier8.nx;
    originalTier8.ny=tier8.ny;originalTier8.nz=tier8.nz;originalTier8.cellWidthM=0.025f;
    std::uint64_t originalBytes=0u;
    passed=passed&&RISE::FireProductionProjectedHeunCPUOwnerWorkingSetBytes(originalTier8,originalBytes)&&
        originalBytes==UINT64_C(1148068092)&&originalBytes*2u==bytes;
    std::printf("SHARED_OWNER_CAPTURE canonical_source=1 cpu_r190_begin=1 cells=%zu nonzero_source=%zu payload_bytes=%zu "
        "source_control_reds=3 lineage_reds=4 immutable_reds=2 tier8_mirror_bytes=%llu passed=%d\n",
        cells,nonzeroSource,captured.Payload().size(),static_cast<unsigned long long>(bytes),passed?1:0);
    return passed?0:1;
}
