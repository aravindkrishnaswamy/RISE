//////////////////////////////////////////////////////////////////////
//
// FireProductionSource.cpp - compiled canonical methane source authority
//
// License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionTransport.h"
#include "FireProductionSourceKernel.h"
#include "FireCase.h"

#include <new>
#include <system_error>

namespace RISE
{
	namespace FireSim
	{
		namespace
		{
			bool SourceAuthorityFailWithoutThrow(std::string* error,const char* message) noexcept
			{
				if(error)try{*error=message;}catch(...){}
				return false;
			}
		}
		//! The only authority allowed to mint a production frozen-source seal.  It
		//! wraps the already qualified canonical grid producer; callers cannot seal
		//! a precomputed delta, pilot pair, or radiation factor.
		void FireProductionCanonicalSourceAuthority::HashByte(
			std::uint64_t& hash,const unsigned char value)
			{
				hash^=value;hash*=UINT64_C(1099511628211);
			}
		void FireProductionCanonicalSourceAuthority::HashUInt64(
			std::uint64_t& hash,const std::uint64_t value)
			{
				for(unsigned int byte=0u;byte<8u;++byte)
					HashByte(hash,static_cast<unsigned char>(value>>(8u*byte)));
			}
		void FireProductionCanonicalSourceAuthority::HashDouble(
			std::uint64_t& hash,const double value)
			{
				std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));HashUInt64(hash,bits);
			}
		void FireProductionCanonicalSourceAuthority::HashFloat(
			std::uint64_t& hash,const float value)
			{
				HashDouble(hash,static_cast<double>(value));
			}
		void FireProductionCanonicalSourceAuthority::HashString(
			std::uint64_t& hash,const std::string& value)
			{
				HashUInt64(hash,value.size());for(const unsigned char byte:value)HashByte(hash,byte);
			}
		void FireProductionCanonicalSourceAuthority::HashFloatValues(std::uint64_t& hash,
				const std::vector<float>& values)
			{
				HashUInt64(hash,values.size());for(const float value:values)HashFloat(hash,value);
			}
		void FireProductionCanonicalSourceAuthority::HashDoubleValues(std::uint64_t& hash,
				const std::vector<double>& values)
			{
				HashUInt64(hash,values.size());for(const double value:values)HashDouble(hash,value);
			}
		void FireProductionCanonicalSourceAuthority::HashByteValues(std::uint64_t& hash,
				const std::vector<unsigned char>& values)
			{
				HashUInt64(hash,values.size());for(const unsigned char value:values)HashByte(hash,value);
			}
		void FireProductionCanonicalSourceAuthority::HashDomain(
			std::uint64_t& hash,const char* domain)
			{
				for(const unsigned char* value=reinterpret_cast<const unsigned char*>(domain);
					*value;++value)HashByte(hash,*value);
			}

		bool FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			const FireProductionProjectionShape& shape,const unsigned int workerCount,
			std::uint64_t& bytes,std::string* error )
		{
			bytes=0u;
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||workerCount==0u||
				workerCount>FireWorkerCapacity())return Fail(error,
					"production canonical source working-set shape is invalid");
			const std::uint64_t cells=static_cast<std::uint64_t>(shape.nx)*shape.ny*shape.nz;
			// Request Q/mixing/mask, sealed 11F+8D result, and the simultaneously
			// live canonical two-pass reaction/radiation scratch. vector<bool> is
			// deliberately charged as one full byte for each of its three maps.
			const std::uint64_t bytesPerCell=29u*sizeof(float)+
				12u*sizeof(double)+5u*sizeof(MethaneCellState)+
				sizeof(MethaneReactionStep)+3u*sizeof(MethaneSourcePacket)+5u;
			// The persistent pool is topology-bounded.  Charge an explicit conservative
			// stack reservation for every admitted worker; sizeof(std::thread) alone is
			// not an allocation certificate for an OS thread.
			// The process-global pool retains its high-water worker set.  Charge the
			// complete topology capacity on every query so a later low-worker request
			// cannot understate stacks retained by an earlier high-worker build.
			const std::uint64_t workerBytes=static_cast<std::uint64_t>(
				FireWorkerCapacity())*
				(UINT64_C(8)<<20u);
			const std::uint64_t fixedRecordScratch=UINT64_C(1048576);
			if(bytesPerCell&&cells>(std::numeric_limits<std::uint64_t>::max()-
				fixedRecordScratch-workerBytes)/bytesPerCell)return Fail(error,
					"production canonical source working set overflows");
			bytes=fixedRecordScratch+workerBytes+cells*bytesPerCell;
			if(error)error->clear();return true;
		}

		bool FireProductionCanonicalSourceAuthority::Build(
				const FireProductionFrozenMethaneSourceRequest& request,
				FireProductionFrozenSourcePacketSeal& result,
				std::string* error )
			{
				result=FireProductionFrozenSourcePacketSeal();
				try {
				if(request.caseRecordEnvelope.size()>UINT64_C(1048576))return Fail(error,
					"production canonical source case envelope exceeds one MiB");
				std::uint64_t workingSetBytes=0u;
				if(!WorkingSetBytes(request.shape,request.workerCount,workingSetBytes,error))
					return false;
				if(workingSetBytes>(UINT64_C(2)<<30u))return Fail(error,
					"production canonical source working set exceeds two GiB");
				const std::size_t cells=request.shape.CellCount();
				if(request.shape.nx<4u||request.shape.nx>1024u||
					request.shape.ny<4u||request.shape.ny>1024u||
					request.shape.nz<4u||request.shape.nz>1024u||
					!(request.shape.cellWidthM>0.0f)||!std::isfinite(request.shape.cellWidthM)||
					!(request.timeStepS>0.0f)||!std::isfinite(request.timeStepS)||
					!std::isfinite(request.beginningTimeS)||request.beginningTimeS<0.0||
					request.attemptIdentity==0u||request.workerCount==0u||
					request.workerCount>FireWorkerCapacity()||
					request.beginningConservativeValues.size()!=9u*cells||
					(!request.sourceEvaluationTemperatureK.empty()&&
					 request.sourceEvaluationTemperatureK.size()!=cells)||
					(!request.eligibilityBeginningConservativeValues.empty()&&
					 request.eligibilityBeginningConservativeValues.size()!=9u*cells)||
					request.pilotCommandMask.size()!=cells||
					(!request.sourceBoundaryContactMask.empty()&&
					 request.sourceBoundaryContactMask.size()!=cells)||
					request.mixingTimeS.size()!=cells)
					return Fail(error,"production canonical source request is malformed");
				const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
				const FireSimulationTransportRecord& transport=FireSimulationTransportRecord::OpenV1();
				const FireSimulationGasOpacityRecord& opacity=
					FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
				FireCase::RecordV1 fireCase;std::string caseError;
				if(!fuel.IsValid()||!transport.IsValid()||!opacity.IsValid()||
					!FireCase::ValidateMethaneEnvelopeV1(request.caseRecordEnvelope,fuel,
						fireCase,caseError))return Fail(error,
						"production canonical source records are invalid");
				auto hasReference=[&](const std::string& identity){return std::find(
					fireCase.referencedRecordIds.begin(),fireCase.referencedRecordIds.end(),
					identity)!=fireCase.referencedRecordIds.end();};
				if(!hasReference(fuel.RecordId())||!hasReference(transport.RecordId())||
					!hasReference(opacity.RecordId()))return Fail(error,
						"production canonical source case omits a required record identity");
				std::vector<MethaneCellState> beginning(cells),sourceBeginning(cells),
					eligibilityBeginning(cells);
				std::vector<float> canonicalTemperatureK(cells,0.0f);
				const std::vector<float>& eligibilityValues=
					request.eligibilityBeginningConservativeValues.empty()?
					request.beginningConservativeValues:
					request.eligibilityBeginningConservativeValues;
				for(std::size_t cell=0u;cell<cells;++cell){
					ConservativeVector tuple,eligibilityTuple;
					for(std::size_t component=0u;component<9u;++component){
						const float value=request.beginningConservativeValues[component*cells+cell];
						const float eligibilityValue=eligibilityValues[component*cells+cell];
						if(!std::isfinite(value)||(value==0.0f&&std::signbit(value)))return Fail(error,
							"production canonical source beginning state is noncanonical");
						if(!std::isfinite(eligibilityValue)||(eligibilityValue==0.0f&&
							std::signbit(eligibilityValue)))return Fail(error,
							"production canonical source eligibility state is noncanonical");
						tuple[component]=static_cast<double>(value);
						eligibilityTuple[component]=static_cast<double>(eligibilityValue);
					}
					beginning[cell]=FromConservativeVector(tuple,FireStateProducerPrecision::Binary32);
					double invertedTemperatureK=0.0,pressureRatio=0.0;
					if(!fuel.InvertAcceptedConservativeStateByComponentOrder(tuple.value.data(),
						tuple.value.size(),fireCase.derived.pilotAmbientTemperatureK,
						fireCase.derived.maximumAcceptedTemperatureK,
						FireStateProducerPrecision::Binary32,invertedTemperatureK,
						pressureRatio,error))return false;
					canonicalTemperatureK[cell]=static_cast<float>(invertedTemperatureK);
					if(!std::isfinite(canonicalTemperatureK[cell])||
						canonicalTemperatureK[cell]>=fireCase.derived.maximumAcceptedTemperatureK){
						std::ostringstream message;message<<
							"production canonical source temperature reaches the strict case ceiling: cell="
							<<cell<<" inverted="<<invertedTemperatureK<<" published="<<
							canonicalTemperatureK[cell]<<" energy="<<tuple[8u];
						return Fail(error,message.str());
					}
					beginning[cell].temperatureK=static_cast<double>(canonicalTemperatureK[cell]);
					sourceBeginning[cell]=beginning[cell];
					if(!request.sourceEvaluationTemperatureK.empty()){
						const double evaluationTemperatureK=
							request.sourceEvaluationTemperatureK[cell];
						if(!std::isfinite(evaluationTemperatureK)||
							evaluationTemperatureK<fuel.TemperatureMinK()||
							evaluationTemperatureK>fuel.TemperatureMaxK())return Fail(error,
							"production canonical source evaluation temperature is invalid");
						sourceBeginning[cell].temperatureK=evaluationTemperatureK;
						double representedEnergy=0.0;
						if(!SignedMixtureSensibleEnergy(sourceBeginning[cell],evaluationTemperatureK,
							fuel,representedEnergy,error)||static_cast<float>(representedEnergy)!=
								request.beginningConservativeValues[8u*cells+cell]){
							std::ostringstream message;message.precision(17);
							message<<"production canonical source evaluation temperature fails forward EOS closure"
								<<" cell="<<cell<<" evaluation_T_K="<<evaluationTemperatureK
								<<" inverted_T_K="<<invertedTemperatureK<<" canonical_T_K="<<canonicalTemperatureK[cell]
								<<" forward_energy_J_m3="<<representedEnergy
								<<" rounded_forward_energy_J_m3="<<static_cast<float>(representedEnergy)
								<<" stored_energy_J_m3="<<tuple[8u]<<" tuple=";
							for(std::size_t component=0u;component<9u;++component)message<<tuple[component]<<',';
							return Fail(error,message.str());
						}
					}
					eligibilityBeginning[cell]=FromConservativeVector(eligibilityTuple,
						FireStateProducerPrecision::Binary32);
					double eligibilityTemperatureK=0.0,eligibilityPressureRatio=0.0;
					if(!fuel.InvertAcceptedConservativeStateByComponentOrder(
						eligibilityTuple.value.data(),eligibilityTuple.value.size(),
						fireCase.derived.pilotAmbientTemperatureK,
						fireCase.derived.maximumAcceptedTemperatureK,
						FireStateProducerPrecision::Binary32,eligibilityTemperatureK,
						eligibilityPressureRatio,error))return false;
					eligibilityBeginning[cell].temperatureK=eligibilityTemperatureK;
					if(!ValidateCellState(beginning[cell],error)||
						!AcceptedMethaneCellStateAdmissible(beginning[cell],fuel,
							FireStateProducerPrecision::Binary32,error)||
						!ValidateCellState(sourceBeginning[cell],error)||
						!ValidateCellState(eligibilityBeginning[cell],error)||
						!AcceptedMethaneCellStateAdmissible(eligibilityBeginning[cell],fuel,
							FireStateProducerPrecision::Binary32,error))return false;
					if((request.pilotCommandMask[cell]!=0u&&request.pilotCommandMask[cell]!=1u)||
						(!request.sourceBoundaryContactMask.empty()&&
						 request.sourceBoundaryContactMask[cell]!=0u&&
						 request.sourceBoundaryContactMask[cell]!=1u)||
						!std::isfinite(request.mixingTimeS[cell])||request.mixingTimeS[cell]<=0.0)
						return Fail(error,"production canonical source control is invalid");
				}
				IgnitionGrid ignition;ignition.nx=request.shape.nx;ignition.ny=request.shape.ny;
				ignition.nz=request.shape.nz;ignition.cells=eligibilityBeginning;
				ignition.pilotMask.resize(cells,false);
				for(std::size_t cell=0u;cell<cells;++cell)
					ignition.pilotMask[cell]=request.pilotCommandMask[cell]!=0u;
				std::vector<bool> eligible;
				if(!BuildIgnitionEligibility(ignition,fuel,fuel,transport,eligible,error))return false;
				if(!request.sourceBoundaryContactMask.empty()){
					const std::vector<double>& ambientMassFraction=fuel.AmbientMassFractions();
					const double ambientProductMassFraction=ambientMassFraction[MethaneCO2]+
						ambientMassFraction[MethaneH2O];
					const double productWitness=std::nextafter(ambientProductMassFraction,
						std::numeric_limits<double>::infinity());
					for(std::size_t cell=0u;cell<cells;++cell){
						if(request.sourceBoundaryContactMask[cell]==0u)continue;
						const MethaneCellState& candidate=sourceBeginning[cell];
						const double gasDensity=candidate.GasDensity();
						if(!(gasDensity>0.0)||candidate.constituent[MethaneCH4]<=0.0||
							candidate.constituent[MethaneO2]<=0.0)continue;
						if(!request.pilotEstablished&&
							(candidate.constituent[MethaneCO2]+candidate.constituent[MethaneH2O])/
								gasDensity<productWitness)continue;
						bool passes=false;
						if(!StoichiometricParcelPassesCFT(candidate,fuel,fuel,
							transport.CriticalFlameTemperatureK(),passes,error))return false;
						if(passes)eligible[cell]=true;
					}
				}
				std::vector<MethaneReactionStep> reactions(cells);
				for(std::size_t cell=0u;cell<cells;++cell){
					reactions[cell].deltaTimeS=static_cast<double>(request.timeStepS);
					reactions[cell].mixingTimeS=request.mixingTimeS[cell];
					reactions[cell].maximumAcceptedTemperatureK=
						fireCase.derived.maximumAcceptedTemperatureK;
					reactions[cell].primaryEligible=eligible[cell];
					reactions[cell].sootOxidationEnabled=true;
					if(!FireCase::EvaluatePilotSetpointTemperatureK(fireCase.derived,
						request.pilotCommandMask[cell]!=0u,request.beginningTimeS,
						request.beginningTimeS+static_cast<double>(request.timeStepS),
						reactions[cell].pilotSetpointTemperatureK,caseError))return Fail(error,
							"production canonical source pilot command is invalid");
					reactions[cell].pilotExpansionVolumeRatioCap=
						reactions[cell].pilotSetpointTemperatureK>0.0?
						fireCase.derived.pilotExpansionVolumeRatioCap:0.0;
				}
				const double volume=std::pow(static_cast<double>(request.shape.cellWidthM),3.0);
				std::vector<double> cellVolume(cells,volume);
				std::vector<MethaneSourcePacket> packets;RadiationEscapeFactor factor;
				if(!BuildFrozenMethaneSourcePackets(sourceBeginning,reactions,cellVolume,
					fireCase.derived.pilotAmbientTemperatureK,
					fireCase.derived.referenceHeatReleaseRateW,
					fireCase.derived.effectiveRadiativeFraction,request.predictiveRadiation,
					fuel,fuel,opacity,packets,factor,error,request.workerCount))return false;
				// r270: the radiatively adiabatic floor returns its convective flux
				// to the first gas layer as a frozen energy dose, q/dz, before the
				// expansion target is derived from the packet.
				const bool floorRadiation=fireCase.derived.floorEmissivity>0.0;
				const std::size_t floorFaces=request.shape.nx*request.shape.ny;
				std::vector<double> floorHeating,floorIrradiance,floorWall,floorFlux;
				if(!floorRadiation){
					if(!request.floorAbsorbingFaceMask.empty()||
						!request.floorTangentialSpeedMPerS.empty())return Fail(error,
						"production canonical source has floor inputs without floor radiation");
				} else {
					if(request.floorAbsorbingFaceMask.size()!=floorFaces||
						request.floorTangentialSpeedMPerS.size()!=floorFaces)return Fail(error,
						"production canonical source floor radiation inputs are malformed");
					std::vector<double> emission(cells);
					for(std::size_t cell=0u;cell<cells;++cell)
						emission[cell]=packets[cell].radiativeCoolingWPerM3;
					const double width=static_cast<double>(request.shape.cellWidthM);
					if(!ComputeFloorIrradianceWPerM2(request.shape.nx,request.shape.ny,
						request.shape.nz,width,emission,request.workerCount,floorIrradiance,error))
						return false;
					floorHeating.assign(cells,0.0);floorWall.assign(floorFaces,0.0);
					floorFlux.assign(floorFaces,0.0);
					std::vector<unsigned char> faceFailed(floorFaces,0u);
					std::vector<std::string> faceError(floorFaces);
					ParallelFireSlices(floorFaces,request.workerCount,[&](const std::size_t face){
						const unsigned char mask=request.floorAbsorbingFaceMask[face];
						const MethaneCellState& gas=sourceBeginning[face];
						floorWall[face]=gas.temperatureK;
						if(mask==0u)return;
						const float speed=request.floorTangentialSpeedMPerS[face];
						if(mask!=1u||!std::isfinite(speed)||speed<0.0f){
							faceFailed[face]=1u;faceError[face]="floor radiation face input is invalid";return;}
						const double gasDensity=gas.GasDensity();
						std::array<double,MethaneCarbon> massFractions;
						for(std::size_t species=0u;species<MethaneCarbon;++species)
							massFractions[species]=std::max(0.0,gas.constituent[species])/gasDensity;
						auto properties=[&](const double temperatureK,double& viscosity,double& conductivity){
							return transport.MixturePropertiesBySpeciesOrder(massFractions.data(),
								massFractions.size(),fuel,temperatureK,viscosity,conductivity,&faceError[face]);
						};
						FloorRadiationFace solved;
						if(!SolveFloorSurfaceBalance(floorIrradiance[face],gas.temperatureK,gasDensity,
							static_cast<double>(speed),fireCase.derived.floorEmissivity,
							fireCase.derived.pilotAmbientTemperatureK,width,properties,solved,
							&faceError[face])){faceFailed[face]=1u;return;}
						floorWall[face]=solved.wallTemperatureK;
						floorFlux[face]=solved.convectiveFluxWPerM2+0.0;
						floorHeating[face]=solved.convectiveFluxWPerM2/width+0.0;
					});
					for(std::size_t face=0u;face<floorFaces;++face)if(faceFailed[face])
						return Fail(error,"production canonical source floor radiation failed: "+faceError[face]);
					for(std::size_t face=0u;face<floorFaces;++face){
						if(floorHeating[face]==0.0){floorHeating[face]=0.0;floorFlux[face]=0.0;continue;}
						packets[face].sensibleEnergyDeltaJPerM3+=
							floorHeating[face]*static_cast<double>(request.timeStepS);
						// The divergence target must derive from the dose the resident
						// update applies, so the floor-augmented packet is re-represented.
						RepresentMethaneSourcePacketBinary32(packets[face]);
						if(!CertifiedBinary32SourcePacket(packets[face],fuel))return Fail(error,
							"production canonical source floor dose failed its Binary32 certificate");
					}
				}
				FireProductionFrozenSourcePacketSeal candidate;
				candidate.shape_=request.shape;candidate.timeStepS_=request.timeStepS;
				candidate.beginningTimeS_=request.beginningTimeS;
				candidate.attemptIdentity_=request.attemptIdentity;
				candidate.methaneRecordId_=fuel.RecordId();
				candidate.transportRecordId_=transport.RecordId();
				candidate.opacityRecordId_=opacity.RecordId();
				candidate.caseRecordId_=fireCase.caseRecordId;
				candidate.sourceDelta_.assign(9u*cells,0.0f);
				candidate.divergenceTargetPerS_.resize(cells);
				candidate.reactedFuelKGPerM3_.resize(cells);
				candidate.oxidizedCarbonKGPerM3_.resize(cells);
				candidate.grossCarbonFormedKGPerM3_.resize(cells);
				candidate.gasHeatReleaseWPerM3_.resize(cells);
				candidate.sootHeatReleaseWPerM3_.resize(cells);
				candidate.pilotEnergyDeltaJPerM3_.resize(cells);
				candidate.pilotExpansionIntegral_.resize(cells);
				candidate.radiativeCoolingWPerM3_.resize(cells);
				candidate.maximumScaledExpansion_=-std::numeric_limits<double>::infinity();
				for(std::size_t cell=0u;cell<cells;++cell){
					for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
						candidate.sourceDelta_[(1u+species)*cells+cell]=
							static_cast<float>(packets[cell].constituentDelta[species]);
					candidate.sourceDelta_[8u*cells+cell]=
						static_cast<float>(packets[cell].sensibleEnergyDeltaJPerM3);
					candidate.reactedFuelKGPerM3_[cell]=packets[cell].reactedFuelKGPerM3;
					candidate.oxidizedCarbonKGPerM3_[cell]=packets[cell].oxidizedCarbonKGPerM3;
					candidate.grossCarbonFormedKGPerM3_[cell]=packets[cell].grossCarbonFormedKGPerM3;
					candidate.gasHeatReleaseWPerM3_[cell]=packets[cell].gasHeatReleaseWPerM3;
					candidate.sootHeatReleaseWPerM3_[cell]=packets[cell].sootHeatReleaseWPerM3;
					candidate.pilotEnergyDeltaJPerM3_[cell]=packets[cell].pilotEnergyDeltaJPerM3;
					candidate.pilotExpansionIntegral_[cell]=packets[cell].pilotExpansionIntegral;
					candidate.radiativeCoolingWPerM3_[cell]=packets[cell].radiativeCoolingWPerM3;
					double scaled=0.0;
					if(!FrozenSourcePacketExpansionAdmissible(ToConservativeVector(sourceBeginning[cell]),
						sourceBeginning[cell].temperatureK,packets[cell],request.timeStepS,fuel,
						FireStateProducerPrecision::Binary32,&scaled,error))return false;
					candidate.maximumScaledExpansion_=std::max(
						candidate.maximumScaledExpansion_,scaled);
					const float divergenceTargetPerS=static_cast<float>(scaled/
						static_cast<double>(request.timeStepS));
					if(!std::isfinite(divergenceTargetPerS)||
						(divergenceTargetPerS==0.0f&&std::signbit(divergenceTargetPerS)))
						return Fail(error,
							"production canonical source divergence target is noncanonical");
					candidate.divergenceTargetPerS_[cell]=divergenceTargetPerS;
				}
				candidate.floorHeatingWPerM3_=std::move(floorHeating);
				candidate.floorIrradianceWPerM2_=std::move(floorIrradiance);
				candidate.floorWallTemperatureK_=std::move(floorWall);
				candidate.floorConvectiveFluxWPerM2_=std::move(floorFlux);
				candidate.radiationBeta_=factor.beta;candidate.radiationGamma_=factor.gamma;
				candidate.radiationEscapeFactor_=factor.accepted;
				candidate.beginningStateIdentity_=UINT64_C(14695981039346656037);
				HashDomain(candidate.beginningStateIdentity_,
					"RISE canonical frozen source beginning v1");
				HashUInt64(candidate.beginningStateIdentity_,request.shape.nx);
				HashUInt64(candidate.beginningStateIdentity_,request.shape.ny);
				HashUInt64(candidate.beginningStateIdentity_,request.shape.nz);
				HashFloat(candidate.beginningStateIdentity_,request.shape.cellWidthM);
				HashFloatValues(candidate.beginningStateIdentity_,
					request.beginningConservativeValues);
				HashFloatValues(candidate.beginningStateIdentity_,canonicalTemperatureK);
				candidate.beginningTemperatureK_=std::move(canonicalTemperatureK);
				candidate.reactionControlIdentity_=UINT64_C(14695981039346656037);
				HashDomain(candidate.reactionControlIdentity_,
					"RISE canonical frozen source reaction control v2");
				HashDoubleValues(candidate.reactionControlIdentity_,request.mixingTimeS);
				HashByteValues(candidate.reactionControlIdentity_,request.pilotCommandMask);
				HashDoubleValues(candidate.reactionControlIdentity_,
					request.sourceEvaluationTemperatureK);
				HashFloatValues(candidate.reactionControlIdentity_,eligibilityValues);
				HashByteValues(candidate.reactionControlIdentity_,request.sourceBoundaryContactMask);
				HashUInt64(candidate.reactionControlIdentity_,request.pilotEstablished?1u:0u);
				HashDouble(candidate.reactionControlIdentity_,
					fireCase.derived.maximumAcceptedTemperatureK);
				HashDouble(candidate.reactionControlIdentity_,
					fireCase.derived.pilotExpansionVolumeRatioCap);
				for(const MethaneReactionStep& reaction:reactions){
					HashDouble(candidate.reactionControlIdentity_,reaction.pilotSetpointTemperatureK);
					HashUInt64(candidate.reactionControlIdentity_,reaction.primaryEligible?1u:0u);
				}
				HashString(candidate.reactionControlIdentity_,transport.RecordId());
				candidate.sourceInputIdentity_=UINT64_C(14695981039346656037);
				HashDomain(candidate.sourceInputIdentity_,
					"RISE canonical frozen source raw input v2");
				HashUInt64(candidate.sourceInputIdentity_,request.attemptIdentity);
				HashFloat(candidate.sourceInputIdentity_,request.timeStepS);
				HashDouble(candidate.sourceInputIdentity_,request.beginningTimeS);
				HashUInt64(candidate.sourceInputIdentity_,candidate.beginningStateIdentity_);
				HashUInt64(candidate.sourceInputIdentity_,candidate.reactionControlIdentity_);
				HashString(candidate.sourceInputIdentity_,fuel.RecordId());
				HashString(candidate.sourceInputIdentity_,transport.RecordId());
				HashString(candidate.sourceInputIdentity_,opacity.RecordId());
				HashString(candidate.sourceInputIdentity_,fireCase.caseRecordId);
				HashDouble(candidate.sourceInputIdentity_,volume);
				HashDouble(candidate.sourceInputIdentity_,
					fireCase.derived.pilotAmbientTemperatureK);
				HashDouble(candidate.sourceInputIdentity_,
					fireCase.derived.referenceHeatReleaseRateW);
				HashDouble(candidate.sourceInputIdentity_,
					fireCase.derived.effectiveRadiativeFraction);
				HashUInt64(candidate.sourceInputIdentity_,request.predictiveRadiation?1u:0u);
				if(floorRadiation){
					HashDomain(candidate.sourceInputIdentity_,"RISE canonical frozen source floor input v1");
					HashDouble(candidate.sourceInputIdentity_,fireCase.derived.floorEmissivity);
					HashByteValues(candidate.sourceInputIdentity_,request.floorAbsorbingFaceMask);
					HashFloatValues(candidate.sourceInputIdentity_,request.floorTangentialSpeedMPerS);
				}
				candidate.FinalizeIdentities();
				if(!candidate.Matches(error))return false;
				result=std::move(candidate);return true;
				} catch(const std::bad_alloc&) {
					result=FireProductionFrozenSourcePacketSeal();
					return SourceAuthorityFailWithoutThrow(error,
						"production canonical source allocation failed");
				} catch(const std::system_error&) {
					result=FireProductionFrozenSourcePacketSeal();
					return SourceAuthorityFailWithoutThrow(error,
						"production canonical source worker creation failed");
				} catch(const std::exception&) {
					result=FireProductionFrozenSourcePacketSeal();
					return SourceAuthorityFailWithoutThrow(error,
						"production canonical source worker task failed");
				} catch(...) {
					result=FireProductionFrozenSourcePacketSeal();
					return SourceAuthorityFailWithoutThrow(error,
						"production canonical source failed with an unknown exception");
				}
			}

	}
}
