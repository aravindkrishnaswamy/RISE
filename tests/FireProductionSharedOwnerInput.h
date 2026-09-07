// Qualification-only capture of the actual canonical source construction and
// live owner request. This is not a checkpoint, migration, or resident authority.
#ifndef RISE_TEST_FIRE_PRODUCTION_SHARED_OWNER_INPUT_H
#define RISE_TEST_FIRE_PRODUCTION_SHARED_OWNER_INPUT_H

#include "../src/Library/Utilities/FireProductionForce.h"
#include "../src/Library/Utilities/FireProductionTransport.h"
#include "fire_production_fp64/FireProductionForce.h"
#include <cstring>
#include <limits>

namespace FireProductionSharedOwnerInput
{
using Bytes=RISE::RISECBOR64::Bytes;

inline void UInt(Bytes& bytes,const std::uint64_t value)
{ for(unsigned int i=0u;i<8u;++i)bytes.push_back(static_cast<unsigned char>(value>>(8u*i))); }
inline void Number(Bytes& bytes,const float value)
{ std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
  for(unsigned int i=0u;i<4u;++i)bytes.push_back(static_cast<unsigned char>(bits>>(8u*i))); }
inline void Number(Bytes& bytes,const double value)
{ std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));UInt(bytes,bits); }
inline void Text(Bytes& bytes,const std::string& value)
{ UInt(bytes,value.size());bytes.insert(bytes.end(),value.begin(),value.end()); }
template<class T> inline void Numbers(Bytes& bytes,const T& values)
{ UInt(bytes,values.size());for(const auto value:values)Number(bytes,value); }
inline void Octets(Bytes& bytes,const Bytes& values)
{ UInt(bytes,values.size());bytes.insert(bytes.end(),values.begin(),values.end()); }
inline void Shape(Bytes& bytes,const RISE::FireProductionProjectionShape& value)
{ UInt(bytes,value.nx);UInt(bytes,value.ny);UInt(bytes,value.nz);Number(bytes,value.cellWidthM); }
inline bool Fail(std::string& error,const char* message)
{ error=message;return false; }

// The current shared-state campaign is the capstone's wall/open domain. Do
// not silently admit periodic seams until the native capture endpoint has a
// seam-qualified path. These are the resident-only fields absent from CPU Begin.
inline bool CapstoneResidentInputSurfaces(const RISE::FireProductionResidentPhysicalFluxComparatorRequest& flux,
    std::string& error)
{
    const auto& transport=flux.transport;const auto& shape=transport.shape;
    for(unsigned int axis=0u;axis<3u;++axis){
        if(transport.projectedVelocityMPerS[axis].size()!=RISE::FireProductionProjectionFaceCount(shape,axis))
            return Fail(error,"shared owner projected velocity extent differs");
        for(const auto value:transport.projectedVelocityMPerS[axis])if(!std::isfinite(value))
            return Fail(error,"shared owner projected velocity is nonfinite");
    }
    for(unsigned int side=0u;side<6u;++side){
        const auto boundary=transport.boundary[side];
        if(boundary!=RISE::FireProductionProjectionWall&&boundary!=RISE::FireProductionProjectionPressureOpen)
            return Fail(error,"shared owner capture supports only qualified capstone wall/open boundaries");
        const std::size_t faces=side<2u?shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
        if(transport.fuelInletBoundaryFace[side].size()!=faces||flux.pressureOpenInflow[side].size()!=faces)
            return Fail(error,"shared owner resident boundary classification extent differs");
        for(const auto value:transport.fuelInletBoundaryFace[side])if(value>1u||
            (value!=0u&&(side!=4u||boundary!=RISE::FireProductionProjectionWall)))
            return Fail(error,"shared owner resident inlet classification is invalid");
        for(const auto value:flux.pressureOpenInflow[side])if(value>1u||
            (value!=0u&&boundary!=RISE::FireProductionProjectionPressureOpen))
            return Fail(error,"shared owner resident pressure classification is invalid");
    }
    const auto& fuel=RISE::FireSimulationMethaneRecord::PhysicalV1();
    if(flux.nullity==0u||flux.nullity>8u||flux.nullspaceBasis.size()!=8u*flux.nullity||
        flux.coordinateProjector.size()!=flux.nullity*flux.nullity||!std::isfinite(flux.ambientTemperatureK)||
        flux.ambientTemperatureK<fuel.TemperatureMinK()||flux.ambientTemperatureK>fuel.TemperatureMaxK())
        return Fail(error,"shared owner physical-flux input contract is invalid");
    for(const auto value:flux.ambient)if(!std::isfinite(value))return Fail(error,"shared owner ambient is nonfinite");
    for(const auto value:flux.nullspaceBasis)if(!std::isfinite(value))return Fail(error,"shared owner basis is nonfinite");
    for(const auto value:flux.coordinateProjector)if(!std::isfinite(value))return Fail(error,"shared owner projector is nonfinite");
    return true;
}

// Every source input is included, notably the pre-injection eligibility state.
// The source packet can remain numerically unchanged when an inactive control
// changes; the capture identity must still change in that case.
inline Bytes SourceInputs(const RISE::FireProductionFrozenMethaneSourceRequest& source)
{
    Bytes bytes;Text(bytes,"rise.fire.qualification.shared_source_inputs.v1");
    Shape(bytes,source.shape);Number(bytes,source.timeStepS);Number(bytes,source.beginningTimeS);
    UInt(bytes,source.attemptIdentity);Octets(bytes,source.caseRecordEnvelope);
    Numbers(bytes,source.beginningConservativeValues);Numbers(bytes,source.sourceEvaluationTemperatureK);
    Numbers(bytes,source.eligibilityBeginningConservativeValues);
    Octets(bytes,source.pilotCommandMask);Octets(bytes,source.sourceBoundaryContactMask);
    UInt(bytes,source.pilotEstablished);Numbers(bytes,source.mixingTimeS);
    UInt(bytes,source.predictiveRadiation);UInt(bytes,source.workerCount);return bytes;
}

class CanonicalSourceCapture
{
    RISE::FireProductionFrozenSourcePacketSeal source_;
    Bytes inputs_;
    bool captured_=false;
public:
    // Exactly one construction, using the existing canonical producer. This
    // wrapper belongs only in a qualification entry point, not the live step.
    bool Build(const RISE::FireProductionFrozenMethaneSourceRequest& request,std::string& error)
    {
        if(captured_)return Fail(error,"shared source capture cannot be rebuilt");
        RISE::FireProductionFrozenSourcePacketSeal source;
        if(!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(request,source,&error))return false;
        inputs_=SourceInputs(request);source_=source;captured_=true;return true;
    }
    const RISE::FireProductionFrozenSourcePacketSeal& Source() const { return source_; }
    const Bytes& Inputs() const { return inputs_; }
    bool Matches(const RISE::FireProductionFrozenMethaneSourceRequest& input,
        const RISE::FireProductionFrozenSourcePacketSeal& source,std::string& error) const
    {
        if(!captured_||inputs_!=SourceInputs(input))
            return Fail(error,"shared source eligibility/control inputs changed after construction");
        if(!RISE::FireProductionFrozenSourcePacketSealMatches(source_,&error)||
            !RISE::FireProductionFrozenSourcePacketSealMatches(source,&error))return false;
        if(source.PacketIdentity()!=source_.PacketIdentity()||
            source.SourceInputIdentity()!=source_.SourceInputIdentity()||
            source.ReactionControlIdentity()!=source_.ReactionControlIdentity())
            return Fail(error,"shared source publication differs from the captured producer");
        return true;
    }
};

// The copy is a qualification observation. No function in this header publishes
// an accepted checkpoint or a resident token, and the fp64 owner is the reviewed
// generated r190 implementation, not another implementation of its arithmetic.
class OwnerCapture
{
    RISE::FireProductionProjectedHeunMetalOwnerRequest resident_;
    RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest mirror_;
    Bytes payload_;
    bool captured_=false;
public:
    const RISE::FireProductionProjectedHeunMetalOwnerRequest& Resident() const { return resident_; }
    const RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest& Mirror() const { return mirror_; }
    const Bytes& Payload() const { return payload_; }
    bool IsCaptured() const { return captured_; }
    bool Capture(const CanonicalSourceCapture& canonical,
        const RISE::FireProductionFrozenMethaneSourceRequest& sourceInputs,
        const RISE::FireProductionProjectedHeunMetalOwnerRequest& live,std::string& error)
    {
        if(captured_)return Fail(error,"shared owner capture cannot be rebuilt");
        const auto& eos=live.lineage.eos;const auto& flux=eos.physicalFlux;
        const auto& transport=flux.transport;const auto& source=live.lineage.frozenSource;
        if(!canonical.Matches(sourceInputs,source,error))return false;
        const auto& lineage=live.lineage;
        const auto unbounded=std::numeric_limits<std::uint64_t>::max();
        if(lineage.qualificationUnsealedTransportParent||lineage.qualificationUnsealedPhysicalFluxParent||
            lineage.qualificationUnsealedEOSCandidateParent||lineage.qualificationUnsealedEOSParent||
            lineage.qualificationUnsealedFrozenSourceParent||lineage.qualificationNonImmediateCandidate||
            lineage.qualificationCPUProducedFrozenSource||lineage.qualificationCPUPrivateBlitFrozenSource||
            lineage.qualificationMismatchedFrozenSourcePacket||lineage.qualificationMismatchedEOSThermochemistry||
            lineage.qualificationCPUProducedTarget||lineage.qualificationCPUForgedProjectionMetadata||
            lineage.qualificationPreauthoredProjectionTarget||lineage.qualificationMismatchedProjectionTopology||
            lineage.qualificationAlternateDormantTailThreshold||lineage.qualificationExactPositiveTailThreshold||
            lineage.qualificationExactNegativeTailThreshold||lineage.qualificationEOSAcceptedButUnlinked||
            lineage.qualificationInjectInterstageFullGridTransfer||lineage.qualificationCertifiedContinuousEnclosure||
            lineage.qualificationCorruptContinuousEnclosurePublication||lineage.qualificationStaleTargetMetadataField||
            lineage.qualificationWorkingSetLimitBytes!=unbounded||
            eos.qualificationUnsealedParentFlux||eos.qualificationMismatchedParentFlux||
            eos.qualificationCPUProducedCandidate||eos.qualificationShortCandidateSurface||
            eos.qualificationMismatchedEOSThermochemistry||eos.qualificationMismatchedDeviceStage||
            eos.qualificationMismatchedDevicePrecision||eos.qualificationMismatchedDeviceAttempt||
            eos.qualificationMismatchedDeviceCells||eos.qualificationMismatchedDeviceTimeStep||
            eos.qualificationMismatchedDeviceCase||eos.qualificationAmbiguousPressureRounding||
            eos.qualificationAmbiguousDeviationRounding||eos.qualificationExactZeroDeviationRounding||
            eos.qualificationMinimumSubnormalDeviationRounding||eos.qualificationAmbiguousZeroDeviationRounding||
            eos.qualificationAmbiguousSubnormalDeviationRounding||eos.qualificationTwoCellDistinctEOSFailures||
            eos.qualificationCorruptEndpointTable||eos.qualificationWorkingSetLimitBytes!=unbounded||
            flux.qualificationMutateHighNonadvective||flux.qualificationMismatchedParentCandidate||
            flux.qualificationShortInflowSurface||flux.qualificationOversizedPhysicalBasisSurface||
            flux.qualificationWorkingSetLimitBytes!=unbounded)
            return Fail(error,"shared owner nested authority contains a qualification mutation");
        if(!RISE::FireProductionFrozenSourcePacketSealMatchesBeginningState(source,
            transport.shape,transport.conservativeValues,transport.temperatureK,&error))return false;
        if(!CapstoneResidentInputSurfaces(flux,error))return false;
        if(transport.stage!=RISE::FireProductionProjectedHeunStage::R0||
            transport.attemptIdentity!=source.AttemptIdentity()||
            transport.parentCandidateIdentity!=source.BeginningStateIdentity()||
            transport.projectionIdentity!=source.PacketIdentity()||
            eos.producingStage!=RISE::FireProductionScalarEOSStage::QStar||
            eos.producerPrecision!=RISE::FireStateProducerPrecision::Binary32||
            eos.candidateTimeStepS!=source.TimeStepS()||
            eos.caseRecordEnvelope!=sourceInputs.caseRecordEnvelope||
            eos.sourceDelta.size()!=source.SourceDelta().size()||
            std::memcmp(eos.sourceDelta.data(),source.SourceDelta().data(),
                eos.sourceDelta.size()*sizeof(float))!=0)
            return Fail(error,"shared owner immediate source/candidate lineage differs");
        if(live.qualificationStaleCandidate||live.qualificationOutOfOrderStage||
            live.qualificationForgedLineage||live.qualificationCallbackMutation||
            live.qualificationAtomicPublicationFailure||live.qualificationInjectInterstageTransfer||
            live.qualificationDivergentManifoldPolicy||live.qualificationStaleTargetPublication||
            live.qualificationUnverifiedPrivateLineageBuffer||live.qualificationForcedActiveCycleStage||
            live.qualificationDisableCanonicalCycle||live.qualificationForceLimiterDiscontinuity||
            live.qualificationDisableLimiterCertification||live.qualificationThreeQuarterHeunWeighting||
            live.qualificationReuseR0LimiterAlpha||live.qualificationWrongAveragedFluxParent||
            live.qualificationR2SealedClassPhysicalFlux||live.qualificationUnverifiedEndpointClassBuffer||
            live.qualificationWorkingSetLimitBytes)
            return Fail(error,"shared owner request contains a qualification mutation");
        RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest mirror;
        mirror.attemptIdentity=transport.attemptIdentity;
        mirror.source=RISEFireProductionFP64::FireProductionFrozenSourcePacketSeal::CalibrationImport(source);
        mirror.caseRecordEnvelope=eos.caseRecordEnvelope;
        mirror.beginningConservativeValues.assign(transport.conservativeValues.begin(),transport.conservativeValues.end());
        const auto copyShape=[&](RISEFireProductionFP64::FireProductionProjectionShape& to){
            to.nx=transport.shape.nx;to.ny=transport.shape.ny;to.nz=transport.shape.nz;
            to.cellWidthM=transport.shape.cellWidthM;};
        copyShape(mirror.scalarContract.shape);copyShape(mirror.physicalContract.shape);copyShape(mirror.forceContract.shape);
        mirror.scalarContract.timeStepS=eos.candidateTimeStepS;mirror.forceContract.timeStepS=eos.candidateTimeStepS;
        for(unsigned int side=0u;side<6u;++side){const auto boundary=static_cast<
            RISEFireProductionFP64::FireProductionProjectionBoundary>(transport.boundary[side]);
            mirror.scalarContract.boundary[side]=boundary;mirror.physicalContract.boundary[side]=boundary;
            mirror.forceContract.boundary[side]=boundary;}
        for(std::size_t component=0u;component<9u;++component){
            mirror.scalarContract.ambient[component]=flux.ambient[component];
            mirror.physicalContract.ambient[component]=flux.ambient[component];}
        mirror.scalarContract.nullity=flux.nullity;
        mirror.scalarContract.nullspaceBasis.assign(flux.nullspaceBasis.begin(),flux.nullspaceBasis.end());
        mirror.scalarContract.coordinateProjector.assign(flux.coordinateProjector.begin(),flux.coordinateProjector.end());
        const auto& fuel=RISE::FireSimulationMethaneRecord::PhysicalV1();
        std::array<double,7> lower,upper;
        if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),lower.data(),lower.size(),&error)||
            !fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),upper.data(),upper.size(),&error))return false;
        for(std::size_t species=0u;species<7u;++species){
            mirror.scalarContract.enthalpyBoundsJPerKG[species]=lower[species];
            mirror.scalarContract.enthalpyBoundsJPerKG[7u+species]=upper[species];}
        const auto& envelope=fuel.AcceptedStateFeasibilityEnvelope();
        mirror.scalarContract.feasibilityFactor=envelope.kappaEpsilon32*std::numeric_limits<float>::epsilon();
        mirror.scalarContract.assemblyReserveFactor=envelope.remapFactorEpsilon32*std::numeric_limits<float>::epsilon();
        mirror.physicalContract.ambientTemperatureK=flux.ambientTemperatureK;
        mirror.forceContract.ambientDensityKGPerM3=live.ambientDensityKGPerM3;
        mirror.forceContract.vremanCoefficient=live.vremanCoefficient;
        for(unsigned int axis=0u;axis<3u;++axis){mirror.forceContract.gravityMPerS2[axis]=live.gravityMPerS2[axis];
            mirror.beginningMomentumKGPerM2S[axis].assign(live.beginningMomentumKGPerM2S[axis].begin(),
                live.beginningMomentumKGPerM2S[axis].end());}
        mirror.projectionTolerancePerS=live.projectionTolerancePerS;
        mirror.endpointVelocityToleranceMPerS=live.endpointVelocityToleranceMPerS;
        mirror.maximumPicardIterations=live.maximumPicardIterations;
        mirror.qualificationCaptureIterationTrace=live.qualificationCaptureIterationTrace;
        // Begin is the existing r190 structural gate, not a newly co-authored validator.
        RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner preflight;
        if(!preflight.Begin(mirror,&error))return false;
        Bytes payload;Text(payload,"rise.fire.qualification.shared_owner_request.v1");
        Octets(payload,canonical.Inputs());UInt(payload,source.PacketIdentity());
        Numbers(payload,transport.conservativeValues);
        UInt(payload,source.SourceInputIdentity());UInt(payload,source.ReactionControlIdentity());
        UInt(payload,source.BeginningStateIdentity());UInt(payload,source.GlobalRadiationIdentity());
        UInt(payload,source.PacketContentIdentity());
        Text(payload,source.MethaneRecordId());Text(payload,source.TransportRecordId());
        Text(payload,source.OpacityRecordId());Text(payload,source.CaseRecordId());
        Numbers(payload,source.SourceDelta());Numbers(payload,source.DivergenceTargetPerS());
        Numbers(payload,source.BeginningTemperatureK());Numbers(payload,source.ReactedFuelKGPerM3());
        Numbers(payload,source.OxidizedCarbonKGPerM3());Numbers(payload,source.GrossCarbonFormedKGPerM3());
        Numbers(payload,source.GasHeatReleaseWPerM3());Numbers(payload,source.SootHeatReleaseWPerM3());
        Numbers(payload,source.PilotEnergyDeltaJPerM3());Numbers(payload,source.PilotExpansionIntegral());
        Numbers(payload,source.RadiativeCoolingWPerM3());Number(payload,source.RadiationBeta());
        Number(payload,source.RadiationGamma());Number(payload,source.RadiationEscapeFactor());
        Number(payload,source.MaximumScaledExpansion());
        for(unsigned int side=0u;side<6u;++side){UInt(payload,transport.boundary[side]);
            Octets(payload,transport.fuelInletBoundaryFace[side]);Octets(payload,flux.pressureOpenInflow[side]);}
        for(unsigned int axis=0u;axis<3u;++axis){Numbers(payload,live.beginningMomentumKGPerM2S[axis]);
            Numbers(payload,transport.projectedVelocityMPerS[axis]);}
        Numbers(payload,flux.ambient);Number(payload,flux.ambientTemperatureK);UInt(payload,flux.nullity);
        Numbers(payload,flux.nullspaceBasis);Numbers(payload,flux.coordinateProjector);
        Number(payload,live.ambientDensityKGPerM3);Number(payload,live.vremanCoefficient);
        Numbers(payload,live.gravityMPerS2);Number(payload,live.projectionTolerancePerS);
        Number(payload,live.endpointVelocityToleranceMPerS);UInt(payload,live.maximumPicardIterations);
        UInt(payload,live.qualificationCaptureIterationTrace);UInt(payload,live.qualificationProductionStageTokens);
        // Explicitly bind the mirror's record-derived (not fp32-rounded) contract terms.
        Numbers(payload,mirror.scalarContract.enthalpyBoundsJPerKG);
        Number(payload,mirror.scalarContract.feasibilityFactor);Number(payload,mirror.scalarContract.assemblyReserveFactor);
        resident_=live;mirror_=mirror;payload_=payload;captured_=true;error.clear();return true;
    }
};
}
#endif
