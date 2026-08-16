#include "../src/Library/Utilities/FireCase.h"
#include "../src/Library/Utilities/FireSimulationRecords.h"
#include <cmath>
#include <cstdio>
#include <limits>

using namespace RISE;

namespace
{
	int failures=0;
	void Check(bool ok,const char* text) { if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);++failures;} }
	RISECBOR64::Value Replace(const RISECBOR64::Value& map,const char* key,
		const RISECBOR64::Value& value)
	{
		RISECBOR64::Value::Members members=map.GetMap();
		for(auto& member:members) if(member.first==key) member.second=value;
		return RISECBOR64::Value::MapValue(members);
	}
}

int main()
{
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	FireCase::AuthoredV1 authored;
	authored.fuelRecordId=fuel.RecordId(); authored.poolDiameterM=0.03;
	authored.heatReleaseRateKW=0.10; authored.envelope={{0.0,0.0},{0.5,1.0},{1.0,1.0}};
	authored.durationS=1.0; authored.quality="draft"; authored.seed=1234;
	authored.outputFramesPerS=4.0;
	std::string error; FireCase::RecordV1 record;
	Check(FireCase::BuildMethaneV1(authored,fuel,{fuel.RecordId()},record,error),
		"r54 methane case derives and encodes");
	FireCase::RecordV1 decoded;
	Check(FireCase::ValidateEnvelopeV1(record.envelopeBytes,decoded,error) &&
		decoded.caseRecordId==record.caseRecordId,"case one-preimage envelope validates");
	Check(record.derived.resolutionTier==4.0 && record.derived.nx>0 && record.derived.nz>0 &&
		record.derived.extentXM>=7.0*authored.poolDiameterM &&
		record.derived.effectiveFlameHeightM>=authored.poolDiameterM,
		"r54 tier, upward extent rounding, and small-fire fallback hold");
	std::printf("case=%s grid=%llux%llux%llu dx=%.9g qref=%.9g\n",record.caseRecordId.c_str(),
		static_cast<unsigned long long>(record.derived.nx),static_cast<unsigned long long>(record.derived.ny),
		static_cast<unsigned long long>(record.derived.nz),record.derived.cellWidthM,
		record.derived.referenceHeatReleaseRateW);
	Check(record.derived.windowKind=="none" && record.derived.preRollOrDiscardS==0.0 &&
		record.derived.effectiveRadiativeFraction==fuel.DefaultRadiativeFraction(),
		"cold-start window and inherited methane chi_r derive exactly");
	std::vector<std::uint8_t> pilotMask;
	Check(FireCase::BuildPilotMask(authored,record.derived,pilotMask,error),
		"r55 pilot mask derives from the canonical lattice");
	std::size_t pilotCells=0;
	bool pilotOnlyFirstLayer=true,pilotAnnulusExact=true;
	const double centerX=0.5*record.derived.extentXM;
	const double centerY=0.5*record.derived.extentYM;
	const double inner=0.5*authored.poolDiameterM;
	const double outer=inner+2.0*record.derived.cellWidthM;
	for(std::uint64_t k=0;k<record.derived.nz;++k)
		for(std::uint64_t j=0;j<record.derived.ny;++j)
			for(std::uint64_t i=0;i<record.derived.nx;++i) {
				const std::size_t index=static_cast<std::size_t>(i+record.derived.nx*(j+record.derived.ny*k));
				if(!pilotMask[index]) continue;
				++pilotCells;pilotOnlyFirstLayer=pilotOnlyFirstLayer&&k==0;
				const double x=(i+0.5)*record.derived.cellWidthM;
				const double y=(j+0.5)*record.derived.cellWidthM;
				const double radius=std::sqrt((x-centerX)*(x-centerX)+(y-centerY)*(y-centerY));
				pilotAnnulusExact=pilotAnnulusExact&&radius>=inner&&radius<=outer;
			}
	Check(pilotCells>0&&pilotOnlyFirstLayer&&pilotAnnulusExact&&
		record.derived.pilotModelVersion==
			"prescribed_isothermal_kernel_ordinary_tableau_restored_manifold_v5"&&
		record.derived.pilotMaskRule==
			"first_layer_center_annulus_D_over_2_to_D_over_2_plus_2dx"&&
		record.derived.pilotSetpointTemperatureK==900.0&&
		record.derived.pilotExpansionVolumeRatioCap==17.0/16.0&&
		record.derived.pilotDurationMultiplier==1.0,
		"r64 pilot block pins the exact-pair annulus, setpoint, expansion cap, and duration");
	Check(record.derived.limiterAcceptanceModelVersion=="two_class_face_infimum_v1",
		"r59 case identity echoes the canonical two-class limiter acceptance rule");
	double pilotSetpoint=0.0;
	Check(FireCase::EvaluatePilotSetpointTemperatureK(record.derived,true,0.0,
		pilotSetpoint,error)&&pilotSetpoint==900.0,
		"r62 pilot activates the 900 K setpoint in a masked cell");
	Check(FireCase::EvaluatePilotSetpointTemperatureK(record.derived,false,0.0,
		pilotSetpoint,error)&&pilotSetpoint==0.0,
		"r62 pilot leaves an unmasked cell inactive");
	Check(FireCase::EvaluatePilotSetpointTemperatureK(record.derived,true,
		record.derived.flowThroughTimeS,pilotSetpoint,error)&&pilotSetpoint==0.0,
		"r62 pilot is off at the exact one-flow-through endpoint");
	std::vector<double> pattern;
	Check(FireCase::BuildSourcePattern(authored,record.derived,pattern,error),
		"SplitMix64 source pattern builds");
	double weighted=0.0; std::size_t active=0;
	for(double x:pattern) if(x!=0.0){weighted+=x;++active;}
	Check(active>1 && std::fabs(weighted*record.derived.cellWidthM*record.derived.cellWidthM-
		record.derived.sourceAreaM2)<=4.0*std::numeric_limits<double>::epsilon()*
		record.derived.sourceAreaM2,
		"mean-subtracted source pattern preserves authored mass flux");
	Check(record.derived.perturbationDigest==
		"e399780e495fd7495707a40190f9bf737d22c153aa006fad356b9d0bf87eb338",
		"seed 1234 has a frozen r57 source-pattern digest");
	FireCase::AuthoredV1 omittedEnvelope=authored; omittedEnvelope.envelope.clear();
	FireCase::RecordV1 steadyRecord;
	Check(FireCase::BuildMethaneV1(omittedEnvelope,fuel,{fuel.RecordId()},steadyRecord,error) &&
		steadyRecord.authored.envelope.size()==1u && steadyRecord.authored.envelope[0].timeS==0.0 &&
		steadyRecord.authored.envelope[0].value==1.0,
		"omitted envelope expands canonically to steady per schema v1");
	Check(steadyRecord.derived.preRollOrDiscardS==5.0*steadyRecord.derived.flowThroughTimeS&&
		steadyRecord.derived.pilotDurationMultiplier*steadyRecord.derived.flowThroughTimeS<
			steadyRecord.derived.preRollOrDiscardS,
		"r55 pilot is off before the exact five-flow-through statistics window begins");
	Check(FireCase::SelectTimeStepS(0.1,2.0,8.0,0.01,1.0)==0.025,
		"advective r54 timestep limit wins exactly");
	Check(std::fabs(FireCase::SelectTimeStepS(0.1,0.0,8.0,0.01,0.01)-0.011)<1e-17,
		"r54 timestep growth is capped at x1.1");
	FireCase::AuthoredV1 overrideCase=authored; overrideCase.hasRadiativeFractionOverride=true;
	overrideCase.radiativeFractionOverride=0.14; FireCase::RecordV1 overridden;
	Check(FireCase::BuildMethaneV1(overrideCase,fuel,{fuel.RecordId()},overridden,error) &&
		overridden.derived.effectiveRadiativeFraction==0.14 &&
		overridden.caseRecordId!=record.caseRecordId,"chi_r override is identity-bearing");
	FireCase::AuthoredV1 patchCase=authored;
	patchCase.sourceKind="patch"; patchCase.patchXM=0.03; patchCase.patchZM=0.02;
	patchCase.poolDiameterM=0.0; patchCase.intensityKind="mass_flux";
	patchCase.fuelMassFluxKGPerM2S=0.01; patchCase.heatReleaseRateKW=0.0;
	patchCase.quality="dstar"; patchCase.numericDStarTier=12.5;
	FireCase::RecordV1 patchRecord;
	Check(FireCase::BuildMethaneV1(patchCase,fuel,{fuel.RecordId()},patchRecord,error) &&
		patchRecord.derived.resolutionTier==12.5 &&
		patchRecord.derived.sourceAreaM2==patchCase.patchXM*patchCase.patchZM &&
		patchRecord.derived.nominalFuelFluxKGPerM2S==0.01 &&
		FireCase::ValidateMethaneEnvelopeV1(patchRecord.envelopeBytes,fuel,decoded,error),
		"case schema v1 canonicalizes patch, mass-flux, and numeric dstar alternatives");
	FireCase::AuthoredV1 inadmissible=authored;
	inadmissible.poolDiameterM=0.01;inadmissible.heatReleaseRateKW=10.0;
	inadmissible.quality="dstar";inadmissible.numericDStarTier=6.0;
	FireCase::RecordV1 rejected;
	error.clear();
	Check(!FireCase::BuildMethaneV1(inadmissible,fuel,{fuel.RecordId()},rejected,error)&&
		error.find("smallest admissible dstar tier is")!=std::string::npos,
		"r57 rejects the archived 10 mm tier-6 extinguished case at generation and names the admissible tier");
	RISECBOR64::Bytes corrupt=record.envelopeBytes; if(!corrupt.empty()) corrupt.back()^=1;
	Check(!FireCase::ValidateEnvelopeV1(corrupt,decoded,error),"case byte mutation rejects");
	RISECBOR64::Value envelope;
	Check(RISECBOR64::DecodeCanonical(record.envelopeBytes,envelope,&error),
		"case semantic RED decodes the valid envelope");
	const RISECBOR64::Value* payload=envelope.Find("payload");
	const RISECBOR64::Value* derived=payload?payload->Find("derived"):nullptr;
	if(payload&&derived) {
		const RISECBOR64::Value changedDerived=Replace(*derived,"qdot_ref_W",
			RISECBOR64::Value::Float(record.derived.referenceHeatReleaseRateW+1.0));
		const RISECBOR64::Value changedPayload=Replace(*payload,"derived",changedDerived);
		RISECBOR64::Bytes payloadBytes,changed;
		Check(RISECBOR64::Encode(changedPayload,payloadBytes,&error) &&
			RISECBOR64::Encode(RISECBOR64::Value::MapValue({
				{"case_record_id",RISECBOR64::Value::String(RISECBOR64::SHA256Hex(payloadBytes))},
				{"payload",changedPayload}}),changed,&error),
			"self-consistent but false derived case mutation rehashes");
		Check(FireCase::ValidateEnvelopeV1(changed,decoded,error) &&
			!FireCase::ValidateMethaneEnvelopeV1(changed,fuel,decoded,error),
			"case hash integrity cannot certify false methane derivations");
		const RISECBOR64::Value* pilot=derived->Find("pilot");
		if(pilot) {
			const RISECBOR64::Value changedExpansionCap=Replace(*pilot,
				"maximum_eos_volume_ratio_per_step",RISECBOR64::Value::Float(3.0));
			const RISECBOR64::Value capDerived=Replace(*derived,"pilot",changedExpansionCap);
			const RISECBOR64::Value capPayload=Replace(*payload,"derived",capDerived);
			RISECBOR64::Bytes capPayloadBytes,capEnvelope;
			Check(RISECBOR64::Encode(capPayload,capPayloadBytes,&error)&&
				RISECBOR64::Encode(RISECBOR64::Value::MapValue({
					{"case_record_id",RISECBOR64::Value::String(
						RISECBOR64::SHA256Hex(capPayloadBytes))},{"payload",capPayload}}),
					capEnvelope,&error)&&!FireCase::ValidateMethaneEnvelopeV1(
						capEnvelope,fuel,decoded,error),
				"self-consistent pilot-cap mutation changes identity but fails r63 reproduction");
			const RISECBOR64::Value changedPilot=Replace(*pilot,"setpoint_temperature_K",
				RISECBOR64::Value::Float(901.0));
			const RISECBOR64::Value pilotDerived=Replace(*derived,"pilot",changedPilot);
			const RISECBOR64::Value pilotPayload=Replace(*payload,"derived",pilotDerived);
			RISECBOR64::Bytes pilotPayloadBytes,pilotEnvelope;
			Check(RISECBOR64::Encode(pilotPayload,pilotPayloadBytes,&error)&&
				RISECBOR64::Encode(RISECBOR64::Value::MapValue({
					{"case_record_id",RISECBOR64::Value::String(
						RISECBOR64::SHA256Hex(pilotPayloadBytes))},{"payload",pilotPayload}}),
					pilotEnvelope,&error)&&!FireCase::ValidateMethaneEnvelopeV1(
						pilotEnvelope,fuel,decoded,error),
				"self-consistent pilot setpoint mutation changes identity but fails r63 semantic reproduction");
			const RISECBOR64::Value changedThermostat=Replace(*pilot,
				"model_version",RISECBOR64::Value::String("retired_power_source_v1"));
			const RISECBOR64::Value thermostatDerived=Replace(*derived,"pilot",changedThermostat);
			const RISECBOR64::Value thermostatPayload=Replace(*payload,"derived",thermostatDerived);
			RISECBOR64::Bytes thermostatPayloadBytes,thermostatEnvelope;
			Check(RISECBOR64::Encode(thermostatPayload,thermostatPayloadBytes,&error)&&
				RISECBOR64::Encode(RISECBOR64::Value::MapValue({
					{"case_record_id",RISECBOR64::Value::String(
						RISECBOR64::SHA256Hex(thermostatPayloadBytes))},
					{"payload",thermostatPayload}}),thermostatEnvelope,&error)&&
				!FireCase::ValidateMethaneEnvelopeV1(thermostatEnvelope,fuel,decoded,error),
				"self-consistent pilot-model mutation changes identity but fails r63 semantic reproduction");
		}
		const RISECBOR64::Value* timestep=derived->Find("timestep_policy");
		if(timestep) {
			const RISECBOR64::Value changedTimestep=Replace(*timestep,"limiter_acceptance",
				RISECBOR64::Value::String("under_relaxation_rejected"));
			const RISECBOR64::Value timestepDerived=Replace(*derived,"timestep_policy",
				changedTimestep);
			const RISECBOR64::Value timestepPayload=Replace(*payload,"derived",timestepDerived);
			RISECBOR64::Bytes timestepPayloadBytes,timestepEnvelope;
			Check(RISECBOR64::Encode(timestepPayload,timestepPayloadBytes,&error)&&
				RISECBOR64::Encode(RISECBOR64::Value::MapValue({
					{"case_record_id",RISECBOR64::Value::String(
						RISECBOR64::SHA256Hex(timestepPayloadBytes))},
					{"payload",timestepPayload}}),timestepEnvelope,&error)&&
				!FireCase::ValidateMethaneEnvelopeV1(timestepEnvelope,fuel,decoded,error),
				"self-consistent limiter mutation changes identity but fails r59 semantic reproduction");
		}
	}
	std::printf("FireCaseTest: %d failures\n",failures);
	return failures?1:0;
}
