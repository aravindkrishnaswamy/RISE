//////////////////////////////////////////////////////////////////////
// FireCase.cpp - Canonical fire-case-v1 record and r54 derivations
//////////////////////////////////////////////////////////////////////

#include "FireCase.h"
#include "FireSimulationRecords.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

using RISE::RISECBOR64::Value;

namespace
{
	bool Fail(std::string& error,const char* text) { error=text; return false; }
	bool FinitePositive(const double x) { return std::isfinite(x) && x>0.0; }
	std::uint64_t CeilCells(const double extent,const double dx)
	{
		return static_cast<std::uint64_t>(std::ceil(extent/dx));
	}
	Value FloatArray(const std::vector<double>& values)
	{
		Value::Values result; for(const double x:values) result.push_back(Value::Float(x));
		return Value::ArrayValue(result);
	}
	bool ExactKeys(const Value& value,const std::vector<std::string>& expected)
	{
		if(value.GetType()!=Value::Map || value.GetMap().size()!=expected.size()) return false;
		for(const std::string& key:expected) if(!value.Find(key.c_str())) return false;
		return true;
	}
	bool ReadFloat(const Value* value,double& result)
	{
		if(!value || value->GetType()!=Value::Float64 || !std::isfinite(value->GetFloat())) return false;
		result=value->GetFloat(); return true;
	}
	bool IsDigest(const std::string& value)
	{
		if(value.size()!=64u) return false;
		for(const char c:value) if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
		return true;
	}
}

std::uint64_t RISE::FireCase::SplitMix64(std::uint64_t value)
{
	value += UINT64_C(0x9E3779B97F4A7C15);
	value = (value^(value>>30))*UINT64_C(0xBF58476D1CE4E5B9);
	value = (value^(value>>27))*UINT64_C(0x94D049BB133111EB);
	return value^(value>>31);
}

bool RISE::FireCase::BuildSourcePattern(const AuthoredV1& authored,
	const DerivedV1& derived,std::vector<double>& result,std::string& error)
{
	if(!derived.nx || !derived.ny || !derived.nz || !FinitePositive(derived.cellWidthM))
		return Fail(error,"fire case source pattern has an invalid grid");
	result.assign(static_cast<std::size_t>(derived.nx*derived.ny),0.0);
	std::vector<std::size_t> mask;
	const double cx=0.5*derived.extentXM, cy=0.5*derived.extentYM;
	const double diameter=authored.sourceKind=="patch" ? 2.0*std::sqrt(
		authored.patchXM*authored.patchZM/3.141592653589793238462643383279502884) :
		authored.poolDiameterM;
	const double r2=0.25*diameter*diameter;
	double mean=0.0;
	for(std::uint64_t j=0;j<derived.ny;++j) for(std::uint64_t i=0;i<derived.nx;++i) {
		const double x=(static_cast<double>(i)+0.5)*derived.cellWidthM;
		const double y=(static_cast<double>(j)+0.5)*derived.cellWidthM;
		if((x-cx)*(x-cx)+(y-cy)*(y-cy)<=r2) {
			const std::uint64_t linear=i+derived.nx*j;
			const std::uint64_t bits=SplitMix64(authored.seed^linear);
			const double unit=static_cast<double>(bits>>11)*0x1.0p-53;
			result[static_cast<std::size_t>(linear)]=2.0*unit-1.0;
			mean+=result[static_cast<std::size_t>(linear)]; mask.push_back(static_cast<std::size_t>(linear));
		}
	}
	if(mask.empty()) return Fail(error,"fire case source mask contains no cell centres");
	mean/=static_cast<double>(mask.size());
	// The centre-selected mask generally does not have the exact authored
	// geometric area.  Normalize the multiplicative factors to that area so
	// sum(m''_nominal * factor * dx^2) is the authored total fuel flow.
	const double targetSum=derived.sourceAreaM2/
		(derived.cellWidthM*derived.cellWidthM);
	const double base=targetSum/static_cast<double>(mask.size());
	for(const std::size_t index:mask)
		result[index]=base*(1.0+0.01*(result[index]-mean));
	for(unsigned int correction=0;correction<64u;++correction) {
		double sum=0.0;
		for(const std::size_t index:mask) sum+=result[index];
		const double residual=targetSum-sum;
		if(residual==0.0) break;
		result[mask.back()]=std::nextafter(result[mask.back()],residual>0.0 ?
			std::numeric_limits<double>::infinity():-
			std::numeric_limits<double>::infinity());
	}
	RISECBOR64::Bytes bytes(mask.size()*sizeof(double));
	for(std::size_t n=0;n<mask.size();++n)
		std::memcpy(bytes.data()+n*sizeof(double),&result[mask[n]],sizeof(double));
	return true;
}

bool RISE::FireCase::BuildPilotMask(const AuthoredV1& authored,
	const DerivedV1& derived,std::vector<std::uint8_t>& result,std::string& error)
{
	if(!derived.nx || !derived.ny || !derived.nz || !FinitePositive(derived.cellWidthM))
		return Fail(error,"fire case pilot mask has an invalid grid");
	const double diameter=authored.sourceKind=="patch" ? 2.0*std::sqrt(
		authored.patchXM*authored.patchZM/3.141592653589793238462643383279502884) :
		authored.poolDiameterM;
	if(!FinitePositive(diameter)) return Fail(error,"fire case pilot mask has an invalid source");
	result.assign(static_cast<std::size_t>(derived.nx*derived.ny*derived.nz),0u);
	const double cx=0.5*derived.extentXM,cy=0.5*derived.extentYM;
	const double inner=0.5*diameter,outer=inner+2.0*derived.cellWidthM;
	const double inner2=inner*inner,outer2=outer*outer;
	std::size_t active=0;
	for(std::uint64_t j=0;j<derived.ny;++j) for(std::uint64_t i=0;i<derived.nx;++i) {
		const double x=(static_cast<double>(i)+0.5)*derived.cellWidthM;
		const double y=(static_cast<double>(j)+0.5)*derived.cellWidthM;
		const double radius2=(x-cx)*(x-cx)+(y-cy)*(y-cy);
		if(radius2>=inner2 && radius2<=outer2) {
			result[static_cast<std::size_t>(i+derived.nx*j)]=1u;
			++active;
		}
	}
	return active>0 || Fail(error,"fire case pilot annulus contains no cell centres");
}

bool RISE::FireCase::EvaluatePilotSetpointTemperatureK(const DerivedV1& derived,
	const bool maskCell,const double simulationTimeS,double& result,std::string& error)
{
	result=0.0;
	if(!FinitePositive(derived.pilotSetpointTemperatureK) ||
		!FinitePositive(derived.pilotExpansionVolumeRatioCap) ||
		derived.pilotExpansionVolumeRatioCap!=17.0/16.0 ||
		!FinitePositive(derived.pilotDurationMultiplier) ||
		!FinitePositive(derived.flowThroughTimeS) ||
		!std::isfinite(simulationTimeS) || simulationTimeS<0.0)
		return Fail(error,"fire case pilot evaluation has invalid inputs");
	const double endS=derived.pilotDurationMultiplier*derived.flowThroughTimeS;
	if(!std::isfinite(endS)) return Fail(error,"fire case pilot duration is invalid");
	if(maskCell && simulationTimeS<endS) result=derived.pilotSetpointTemperatureK;
	return true;
}

double RISE::FireCase::SelectTimeStepS(const double dx,const double speed,
	const double reducedGravity,const double diffusivity,const double previous)
{
	if(!FinitePositive(dx) || speed<0.0 || reducedGravity<0.0 || diffusivity<0.0 ||
		!std::isfinite(speed) || !std::isfinite(reducedGravity) || !std::isfinite(diffusivity)) return 0.0;
	double step=std::numeric_limits<double>::infinity();
	if(speed>0.0) step=std::min(step,0.5*dx/speed);
	if(reducedGravity>0.0) step=std::min(step,0.5*std::sqrt(2.0*dx/reducedGravity));
	if(diffusivity>0.0) step=std::min(step,dx*dx/(8.0*diffusivity));
	if(FinitePositive(previous)) step=std::min(step,1.1*previous);
	return std::isfinite(step) && step>0.0 ? step : 0.0;
}

bool RISE::FireCase::BuildMethaneV1(const AuthoredV1& a,
	const FireSimulationMethaneRecord& fuel,const std::vector<std::string>& recordIds,
	RecordV1& output,std::string& error)
{
	output=RecordV1();
	if(!fuel.IsValid() || a.fuelRecordId!=fuel.RecordId() || !FinitePositive(a.durationS) ||
		!FinitePositive(a.outputFramesPerS))
		return Fail(error,"fire case authored fields are invalid");
	std::vector<EnvelopeKnot> envelope=a.envelope;
	if(envelope.empty()) envelope.push_back({0.0,1.0});
	double diameter=0.0,sourceArea=0.0;
	if(a.sourceKind=="pool" && FinitePositive(a.poolDiameterM)) {
		diameter=a.poolDiameterM;
		sourceArea=0.25*3.141592653589793238462643383279502884*diameter*diameter;
	} else if(a.sourceKind=="patch" && FinitePositive(a.patchXM) && FinitePositive(a.patchZM)) {
		sourceArea=a.patchXM*a.patchZM;
		diameter=2.0*std::sqrt(sourceArea/3.141592653589793238462643383279502884);
	} else return Fail(error,"fire case source is invalid");
	double nominalHeatReleaseKW=0.0,nominalFuelFlux=0.0;
	if(a.intensityKind=="hrr" && FinitePositive(a.heatReleaseRateKW)) {
		nominalHeatReleaseKW=a.heatReleaseRateKW;
		nominalFuelFlux=1000.0*nominalHeatReleaseKW/(sourceArea*fuel.LowerHeatingValueJPerKG());
	} else if(a.intensityKind=="mass_flux" && FinitePositive(a.fuelMassFluxKGPerM2S)) {
		nominalFuelFlux=a.fuelMassFluxKGPerM2S;
		nominalHeatReleaseKW=sourceArea*nominalFuelFlux*fuel.LowerHeatingValueJPerKG()/1000.0;
	} else return Fail(error,"fire case intensity is invalid");
	double tierValue=0.0;
	if(a.quality=="draft") tierValue=4; else if(a.quality=="standard") tierValue=10;
	else if(a.quality=="high") tierValue=16;
	else if(a.quality=="dstar" && a.numericDStarTier>=4.0 && a.numericDStarTier<=16.0 &&
		std::isfinite(a.numericDStarTier)) tierValue=a.numericDStarTier;
	else return Fail(error,"fire case quality is unknown or candle DNS is outside the LES capstone");
	double peak=0.0, previous=-1.0;
	for(const EnvelopeKnot& knot:envelope) {
		if(!std::isfinite(knot.timeS)||!std::isfinite(knot.value)||knot.timeS<0.0||
			knot.value<0.0||knot.value>1.0||knot.timeS<=previous)
			return Fail(error,"fire case envelope is not strictly ordered in [0,1]");
		peak=std::max(peak,knot.value); previous=knot.timeS;
	}
	if(envelope.front().timeS!=0.0 || envelope.back().timeS>a.durationS || peak<=0.0)
		return Fail(error,"fire case envelope does not cover a valid output interval");
	const double puffPeriod=std::sqrt(diameter)/1.5;
	for(std::size_t i=1;i<envelope.size();++i)
		if(envelope[i].timeS-envelope[i-1].timeS<puffPeriod)
			return Fail(error,"fire case envelope knots are closer than one puffing period");

	DerivedV1 d; d.resolutionTier=tierValue; d.peakEnvelope=peak;
	d.limiterAcceptanceModelVersion="two_class_face_infimum_v1";
	d.pilotModelVersion="prescribed_isothermal_kernel_ordinary_tableau_v4";
	d.pilotMaskRule="first_layer_center_annulus_D_over_2_to_D_over_2_plus_2dx";
	d.pilotSetpointTemperatureK=900.0;
	d.pilotExpansionVolumeRatioCap=17.0/16.0;
	d.pilotDurationMultiplier=1.0;
	d.sourceAreaM2=sourceArea;
	d.referenceHeatReleaseRateW=1000.0*nominalHeatReleaseKW*peak;
	d.nominalFuelFluxKGPerM2S=nominalFuelFlux;
	d.effectiveRadiativeFraction=a.hasRadiativeFractionOverride ?
		a.radiativeFractionOverride:fuel.DefaultRadiativeFraction();
	if(!(d.effectiveRadiativeFraction>=0.0 && d.effectiveRadiativeFraction<=1.0) ||
		!std::isfinite(d.effectiveRadiativeFraction)) return Fail(error,"fire case chi_r is invalid");
	const double ambientT=fuel.ReferenceTemperatureK();
	d.ambientCpJPerKGK=0.0;
	for(std::size_t i=0;i<fuel.SpeciesOrder().size();++i) {
		double cp=0.0; if(!fuel.CpJPerKGK(fuel.SpeciesOrder()[i].c_str(),ambientT,cp,&error)) return false;
		d.ambientCpJPerKGK+=fuel.AmbientMassFractions()[i]*cp;
	}
	double invW=0.0;
	for(std::size_t i=0;i<fuel.SpeciesOrder().size();++i) {
		const FireThermochemistrySpecies* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
		if(species && fuel.SpeciesOrder()[i]!="C(gr)")
			invW+=fuel.AmbientMassFractions()[i]/species->molecularWeightKGPerKMol;
	}
	d.ambientDensityKGPerM3=fuel.ThermodynamicPressurePa()/(8314.46261815324*ambientT*invW);
	d.flameHeightM=0.235*std::pow(nominalHeatReleaseKW*peak,0.4)-1.02*diameter;
	d.effectiveFlameHeightM=std::max(d.flameHeightM,diameter);
	d.characteristicDiameterM=std::pow(d.referenceHeatReleaseRateW/
		(d.ambientDensityKGPerM3*d.ambientCpJPerKGK*ambientT*std::sqrt(9.80665)),0.4);
	d.cellWidthM=d.characteristicDiameterM/tierValue;
	if(d.cellWidthM>0.25*diameter) {
		const std::uint64_t smallestTier=static_cast<std::uint64_t>(
			std::ceil(4.0*d.characteristicDiameterM/diameter));
		std::ostringstream message;
		message << "fire case cell width exceeds D/4; smallest admissible dstar tier is "
			<< smallestTier;
		return Fail(error,message.str().c_str());
	}
	const double sourceX=a.sourceKind=="patch"?a.patchXM:diameter;
	const double sourceY=a.sourceKind=="patch"?a.patchZM:diameter;
	const double requiredX=sourceX+6.0*diameter;
	const double requiredY=sourceY+6.0*diameter;
	const double requiredZ=(a.plumeLaw?5.0:2.0)*d.effectiveFlameHeightM;
	d.nx=CeilCells(requiredX,d.cellWidthM); d.ny=CeilCells(requiredY,d.cellWidthM);
	d.nz=CeilCells(requiredZ,d.cellWidthM);
	d.extentXM=d.nx*d.cellWidthM; d.extentYM=d.ny*d.cellWidthM; d.extentZM=d.nz*d.cellWidthM;
	d.flowThroughTimeS=d.extentZM/std::sqrt(9.80665*d.characteristicDiameterM);
	const bool constant=std::all_of(envelope.begin(),envelope.end(),
		[peak](const EnvelopeKnot& k){return k.value==peak;});
	if(constant) { d.windowKind="discard"; d.preRollOrDiscardS=5.0*d.flowThroughTimeS; }
	else if(envelope.front().value>0.0) { d.windowKind="pre_roll"; d.preRollOrDiscardS=5.0*d.flowThroughTimeS; }
	else { d.windowKind="none"; d.preRollOrDiscardS=0.0; }
	std::vector<double> pattern;
	if(!BuildSourcePattern(a,d,pattern,error)) return false;
	std::vector<std::uint8_t> pilotMask;
	if(!BuildPilotMask(a,d,pilotMask,error)) return false;
	Value::Values activePattern;
	for(const double value:pattern) if(value!=0.0) activePattern.push_back(Value::Float(value));
	RISECBOR64::Bytes patternBytes;
	if(!RISECBOR64::Encode(Value::ArrayValue(activePattern),patternBytes,&error)) return false;
	d.perturbationDigest=RISECBOR64::SHA256Hex(patternBytes);

	Value::Values knots; for(const EnvelopeKnot& k:envelope) knots.push_back(Value::MapValue({
		{"time_s",Value::Float(k.timeS)},{"value",Value::Float(k.value)}}));
	std::vector<std::string> sortedRecordIds=recordIds;
	sortedRecordIds.push_back(fuel.RecordId());
	std::sort(sortedRecordIds.begin(),sortedRecordIds.end());
	sortedRecordIds.erase(std::unique(sortedRecordIds.begin(),sortedRecordIds.end()),
		sortedRecordIds.end());
	Value::Values refs;
	for(const std::string& id:sortedRecordIds) {
		if(!IsDigest(id)) return Fail(error,"fire case referenced record ID is not SHA-256");
		refs.push_back(Value::String(id));
	}
	const Value source=Value::MapValue({
		{"diameter_m",a.sourceKind=="pool"?Value::Float(a.poolDiameterM):Value()},
		{"kind",Value::String(a.sourceKind)},
		{"x_m",a.sourceKind=="patch"?Value::Float(a.patchXM):Value()},
		{"z_m",a.sourceKind=="patch"?Value::Float(a.patchZM):Value()}});
	const Value intensity=Value::MapValue({
		{"kind",Value::String(a.intensityKind)},
		{"value",Value::Float(a.intensityKind=="mass_flux"?a.fuelMassFluxKGPerM2S:
			a.heatReleaseRateKW)}});
	const Value quality=Value::MapValue({
		{"dstar_tier",a.quality=="dstar"?Value::Float(a.numericDStarTier):Value()},
		{"kind",Value::String(a.quality)}});
	const Value authored=Value::MapValue({
		{"chi_r",a.hasRadiativeFractionOverride?Value::Float(a.radiativeFractionOverride):Value()},
		{"duration_s",Value::Float(a.durationS)}, {"envelope",Value::ArrayValue(knots)},
		{"fuel_record_id",Value::String(a.fuelRecordId)}, {"intensity",intensity},
		{"output_frames_per_s",Value::Float(a.outputFramesPerS)}, {"plume_law",Value::Bool(a.plumeLaw)},
		{"quality",quality}, {"seed",Value::Unsigned(a.seed)}, {"source",source} });
	const Value derived=Value::MapValue({
		{"ambient_cp_J_per_kg_K",Value::Float(d.ambientCpJPerKGK)},
		{"ambient_density_kg_per_m3",Value::Float(d.ambientDensityKGPerM3)},
		{"cell_width_m",Value::Float(d.cellWidthM)}, {"chi_r_effective",Value::Float(d.effectiveRadiativeFraction)},
		{"d_star_m",Value::Float(d.characteristicDiameterM)},
		{"domain_cells",Value::ArrayValue({Value::Unsigned(d.nx),Value::Unsigned(d.ny),Value::Unsigned(d.nz)})},
		{"domain_extent_m",FloatArray({d.extentXM,d.extentYM,d.extentZM})},
		{"flame_height_effective_m",Value::Float(d.effectiveFlameHeightM)},
		{"flame_height_heskestad_m",Value::Float(d.flameHeightM)},
		{"flow_through_time_s",Value::Float(d.flowThroughTimeS)},
		{"fuel_mass_flux_kg_per_m2_s",Value::Float(d.nominalFuelFluxKGPerM2S)},
		{"pre_roll_or_discard_s",Value::Float(d.preRollOrDiscardS)},
		{"pilot",Value::MapValue({
			{"duration_multiplier_t_ft",Value::Float(d.pilotDurationMultiplier)},
			{"mask_rule",Value::String(d.pilotMaskRule)},
			{"maximum_eos_volume_ratio_per_step",Value::Float(
				d.pilotExpansionVolumeRatioCap)},
			{"model_version",Value::String(d.pilotModelVersion)},
			{"setpoint_temperature_K",Value::Float(d.pilotSetpointTemperatureK)}})},
		{"qdot_ref_W",Value::Float(d.referenceHeatReleaseRateW)},
		{"resolution_tier",Value::Float(d.resolutionTier)}, {"source_area_m2",Value::Float(d.sourceAreaM2)},
		{"source_pattern_sha256",Value::String(d.perturbationDigest)},
		{"timestep_policy",Value::MapValue({{"advective_cfl",Value::Float(0.5)},
			{"buoyant_coefficient",Value::Float(0.5)},{"diffusive_denominator",Value::Float(8.0)},
			{"growth_limit",Value::Float(1.1)},
			{"limiter_acceptance",Value::String(d.limiterAcceptanceModelVersion)}})},
		{"window_kind",Value::String(d.windowKind)} });
	const Value payload=Value::MapValue({{"authored",authored},{"derived",derived},
		{"record_kind",Value::String("fire-case-v1")},{"referenced_record_ids",Value::ArrayValue(refs)},
		{"schema_version",Value::Unsigned(1)}});
	if(!RISECBOR64::Encode(payload,output.payloadBytes,&error)) return false;
	output.caseRecordId=RISECBOR64::SHA256Hex(output.payloadBytes);
	if(!RISECBOR64::Encode(Value::MapValue({{"case_record_id",Value::String(output.caseRecordId)},
		{"payload",payload}}),output.envelopeBytes,&error)) return false;
	output.authored=a; output.authored.envelope=envelope; output.derived=d;
	output.referencedRecordIds=sortedRecordIds; return true;
}

bool RISE::FireCase::ValidateEnvelopeV1(const RISECBOR64::Bytes& bytes,
	RecordV1& output,std::string& error)
{
	output=RecordV1(); Value envelope;
	if(!RISECBOR64::DecodeCanonical(bytes,envelope,&error) ||
		!ExactKeys(envelope,{"case_record_id","payload"})) return Fail(error,"fire case envelope is not exact v1");
	const Value* payload=envelope.Find("payload"); const Value* id=envelope.Find("case_record_id");
	if(!payload || !id || id->GetType()!=Value::Text || payload->Find("case_record_id") ||
		!ExactKeys(*payload,{"authored","derived","record_kind","referenced_record_ids","schema_version"}) ||
		!payload->Find("record_kind") || payload->Find("record_kind")->GetText()!="fire-case-v1")
		return Fail(error,"fire case payload is outside schema v1");
	if(!RISECBOR64::Encode(*payload,output.payloadBytes,&error) ||
		RISECBOR64::SHA256Hex(output.payloadBytes)!=id->GetText()) return Fail(error,"fire case identity mismatch");
	output.envelopeBytes=bytes; output.caseRecordId=id->GetText(); return true;
}

bool RISE::FireCase::ValidateMethaneEnvelopeV1(const RISECBOR64::Bytes& bytes,
	const FireSimulationMethaneRecord& fuel,RecordV1& output,std::string& error)
{
	RecordV1 structural;
	if(!ValidateEnvelopeV1(bytes,structural,error)) return false;
	Value envelope;
	if(!RISECBOR64::DecodeCanonical(bytes,envelope,&error)) return false;
	const Value* payload=envelope.Find("payload");
	const Value* authored=payload?payload->Find("authored"):nullptr;
	const Value* refs=payload?payload->Find("referenced_record_ids"):nullptr;
	if(!authored || !ExactKeys(*authored,{"chi_r","duration_s","envelope","fuel_record_id",
		"intensity","output_frames_per_s","plume_law","quality","seed","source"}) ||
		!refs || refs->GetType()!=Value::Array) return Fail(error,
		"fire case methane authored schema is not exact v1");
	AuthoredV1 parsed;
	const Value* fuelId=authored->Find("fuel_record_id");
	const Value* quality=authored->Find("quality");
	const Value* seed=authored->Find("seed");
	const Value* plume=authored->Find("plume_law");
	const Value* source=authored->Find("source");
	const Value* intensity=authored->Find("intensity");
	if(!fuelId || fuelId->GetType()!=Value::Text || !quality ||
		!ExactKeys(*quality,{"dstar_tier","kind"}) || !quality->Find("kind") ||
		quality->Find("kind")->GetType()!=Value::Text || !intensity ||
		!ExactKeys(*intensity,{"kind","value"}) || !intensity->Find("kind") ||
		intensity->Find("kind")->GetType()!=Value::Text ||
		!seed || seed->GetType()!=Value::UnsignedInteger || !plume || plume->GetType()!=Value::Boolean ||
		!source || !ExactKeys(*source,{"diameter_m","kind","x_m","z_m"}) || !source->Find("kind") ||
		source->Find("kind")->GetType()!=Value::Text ||
		!ReadFloat(intensity->Find("value"),parsed.heatReleaseRateKW) ||
		!ReadFloat(authored->Find("duration_s"),parsed.durationS) ||
		!ReadFloat(authored->Find("output_frames_per_s"),parsed.outputFramesPerS)) return Fail(error,
		"fire case methane authored values are malformed");
	parsed.fuelRecordId=fuelId->GetText(); parsed.quality=quality->Find("kind")->GetText();
	parsed.sourceKind=source->Find("kind")->GetText();
	if(parsed.sourceKind=="pool") {
		if(!ReadFloat(source->Find("diameter_m"),parsed.poolDiameterM)) return Fail(error,
			"fire case methane pool source is malformed");
	} else if(parsed.sourceKind=="patch") {
		if(!ReadFloat(source->Find("x_m"),parsed.patchXM) ||
			!ReadFloat(source->Find("z_m"),parsed.patchZM)) return Fail(error,
			"fire case methane patch source is malformed");
	} else return Fail(error,"fire case methane source kind is unknown");
	parsed.intensityKind=intensity->Find("kind")->GetText();
	if(parsed.intensityKind=="mass_flux") {
		parsed.fuelMassFluxKGPerM2S=parsed.heatReleaseRateKW; parsed.heatReleaseRateKW=0.0;
	} else if(parsed.intensityKind!="hrr") return Fail(error,
		"fire case methane intensity kind is unknown");
	if(parsed.quality=="dstar") {
		if(!ReadFloat(quality->Find("dstar_tier"),parsed.numericDStarTier)) return Fail(error,
			"fire case methane numeric quality is malformed");
	}
	parsed.seed=seed->GetIntegerArgument(); parsed.plumeLaw=plume->GetBoolean();
	const Value* chi=authored->Find("chi_r");
	if(chi && chi->GetType()!=Value::Null) {
		if(!ReadFloat(chi,parsed.radiativeFractionOverride)) return Fail(error,
			"fire case methane chi_r is malformed");
		parsed.hasRadiativeFractionOverride=true;
	}
	const Value* knots=authored->Find("envelope");
	if(!knots || knots->GetType()!=Value::Array) return Fail(error,
		"fire case methane envelope is malformed");
	for(const Value& knot:knots->GetArray()) {
		EnvelopeKnot parsedKnot;
		if(!ExactKeys(knot,{"time_s","value"}) || !ReadFloat(knot.Find("time_s"),parsedKnot.timeS) ||
			!ReadFloat(knot.Find("value"),parsedKnot.value)) return Fail(error,
			"fire case methane envelope knot is malformed");
		parsed.envelope.push_back(parsedKnot);
	}
	std::vector<std::string> recordIds;
	for(const Value& id:refs->GetArray()) {
		if(id.GetType()!=Value::Text) return Fail(error,"fire case referenced record ID is malformed");
		recordIds.push_back(id.GetText());
	}
	RecordV1 rebuilt;
	if(!BuildMethaneV1(parsed,fuel,recordIds,rebuilt,error)) return false;
	if(rebuilt.envelopeBytes!=bytes) return Fail(error,
		"fire case authored and derived bytes do not reproduce from the methane records");
	output=std::move(rebuilt); return true;
}
