#include "FireProductionCalibrationMath.h"
#include "FireProductionCalibrationMirror.h"
#include "FireProductionRoundoffWalker.h"
#include "FireProductionRoundoffTraceAdapter.h"
#include "Utilities/FireCase.h"
#include "Utilities/FireProductionAdvection.h"
#include "Utilities/FireProductionForce.h"
#include "Utilities/FireProductionProjection.h"
#include "Utilities/FireProductionTransport.h"
#include "fire_production_fp64/FireProductionAdvection.h"
#include "fire_production_fp64/FireProductionForce.h"
#include "fire_production_fp64/FireProductionProjection.h"
#include "fire_production_fp64/SourceManifest.h"
#include "fire_production_trace/FireProductionAdvection.h"
#include "fire_production_trace/FireProductionForce.h"
#include "fire_production_trace/FireProductionTransport.h"
#include "fire_production_trace/SourceManifest.h"
#include "../tools/fire_simulator_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

extern "C" bool RISEProjectedHeunOwnerTestFailureProbe(const char* name)
{
	const char* requested=std::getenv("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE");
	if(!requested)return false;
	const std::size_t nameLength=std::strlen(name);
	for(const char* token=requested;*token;){
		const char* end=std::strchr(token,',');
		const std::size_t length=end?static_cast<std::size_t>(end-token):std::strlen(token);
		if(length==nameLength&&std::memcmp(token,name,length)==0)return true;
		if(!end)break;token=end+1;
	}
	return false;
}

#if defined(__APPLE__)
namespace RISE
{
	bool FireProductionDualLayoutPackRequiresSerialOwner(bool,bool);
}
#endif

namespace
{
	template<typename T,typename=void>
	struct HasPublicOwnerIdentity : std::false_type {};
	template<typename T>
	struct HasPublicOwnerIdentity<T,std::void_t<decltype(
		std::declval<T&>().ownerIdentity)> > : std::true_type {};

	int failures=0;
	void Check(const bool condition,const char* message)
	{
		if(!condition){std::fprintf(stderr,"FAIL: %s\n",message);++failures;}
	}

	std::size_t FaceCount(const std::size_t n,const std::size_t lines)
	{
		return (n+1u)*lines;
	}

	std::string ReadText(const char* path)
	{
		std::ifstream input(path,std::ios::binary);std::ostringstream output;
		output<<input.rdbuf();return output.str();
	}
	std::size_t CountText(const std::string& text,const std::string& token)
	{
		std::size_t count=0u,position=0u;
		while((position=text.find(token,position))!=std::string::npos){++count;position+=token.size();}
		return count;
	}
	bool ValidateResidentEOSRawEvidence(const std::string& text,const char* librarySourceSHA,
		const char* functionSetSHA)
	{
		std::array<std::array<bool,3>,64> seen={};std::size_t rows=0u;
		std::istringstream input(text);std::string line;
		while(std::getline(input,line)){
			if(line.rfind("RESIDENT_EOS_CELL cell=",0u)!=0u)continue;
			std::size_t cell=64u;char field[64]={};
			if(std::sscanf(line.c_str(),"RESIDENT_EOS_CELL cell=%zu field=%63s",
				&cell,field)!=2||cell>=seen.size())return false;
			std::size_t fieldIndex=3u;
			if(std::strcmp(field,"temperature")==0)fieldIndex=0u;
			if(std::strcmp(field,"represented_pressure_ratio")==0)fieldIndex=1u;
			if(std::strcmp(field,"absolute_eos_deviation")==0)fieldIndex=2u;
			if(fieldIndex>=3u||seen[cell][fieldIndex]||
				line.find("residual_over_enclosure=0")==std::string::npos||
				line.find("bit_equal=1")==std::string::npos)return false;
			seen[cell][fieldIndex]=true;++rows;
		}
		if(rows!=192u)return false;
		for(const auto& cell:seen)for(const bool field:cell)if(!field)return false;
		const std::array<const char*,13> redRecords={{
			"name=r170_hard_bound_30_percent expected=0x00000400 observed=0x00000400",
			"name=pressure_midpoint_rounding_ambiguous expected=0x00000200 observed=0x00000200",
			"name=deviation_midpoint_rounding_ambiguous expected=0x00000200 observed=0x00000200",
			"name=zero_lower_upper_bin_ambiguous expected=0x00000200 observed=0x00000200",
			"name=subnormal_lower_upper_bin_ambiguous expected=0x00000200 observed=0x00000200",
			"name=eos_lower_inversion_endpoint expected=0x000000d0 observed=0x000000d0",
			"name=eos_upper_inversion_endpoint expected=0x00000080 observed=0x00000080",
			"name=r170_exact_above_binary32_rounds_to_bound expected=0x00000400 observed=0x00000400",
			"name=forged_device_stage expected=0x00000050 observed=0x00000050",
			"name=forged_device_precision expected=0x00000050 observed=0x00000050",
			"name=forged_device_attempt expected=0x00000050 observed=0x00000050",
			"name=forged_device_cells expected=0x00000050 observed=0x00000050",
			"name=forged_device_timestep expected=0x00000050 observed=0x00000050"}};
		if(CountText(text,"RESIDENT_EOS_RED ")!=redRecords.size())return false;
		for(const char* record:redRecords){
			const std::size_t position=text.find(std::string("RESIDENT_EOS_RED ")+record);
			if(position==std::string::npos)return false;
			const std::size_t end=text.find('\n',position);
			const std::string lineRecord=text.substr(position,end-position);
			if(lineRecord.find("attempted=1 read=1 commands=1 staging=1")==
				std::string::npos||lineRecord.find("candidate_identity=0 eos_identity=0 passed=1")==
				std::string::npos)return false;
		}
		return text.find("RESIDENT_EOS_R60_COMMIT accepted_energy=2283578 "
			"refused_energy=2283578.25 adjacent=1 accepted_bitmap=0x00000000 "
			"refused_bitmap=0x00000010 passed=1")!=std::string::npos&&
			text.find("RESIDENT_EOS_ARITHMETIC_BOUNDARY_SWEEP samples=4 "
				"temperature_classes=4 composition_classes=main_64_cell_fixture "
				"scale_min=0.750500023 scale_max=1.24950004 passed=1")!=
				std::string::npos&&text.find("RESIDENT_EOS_LOG_ENCLOSURE samples=48693249 "
				"lattice=24346625 midpoints=24346624 max_residual_over_bound="
				"0.5192248117002557 worst_index=1 ln2_binary64_projection_residual=0 "
				"ln2_bound=5.5511151231257827e-17 ln2_high_precision=1 "
				"libm_image=/usr/lib/system/libsystem_m.dylib os_build=25F84 "
				"os_release=25.5.0 machine=arm64 "
				"metal_device_registry_id=0x000000010000087d "
				"metal_device_name=\"Apple M4 Max\" metal_device_family=apple9 "
				"metal_runtime_image=/System/Library/Frameworks/Metal.framework/Versions/A/Metal "
				"metal_runtime_bundle=com.apple.Metal metal_runtime_version=373.2 "
				"metal_language=3.2 metal_math_mode=safe "
				"metal_library_source_sha256="+std::string(librarySourceSHA)+" "
				"metal_function_set_sha256="+std::string(functionSetSHA)+" "
				"metal_kernel=diagnose_eos_log_enclosure thread_execution_width=32 "
				"max_threads_per_threadgroup=1024 static_threadgroup_memory_bytes=0 "
				"metal_identity_consistent=1 passed=1")!=
				std::string::npos&&
			text.find("RESIDENT_EOS_ROUNDING_EDGE name=exact_zero_bin_center "
				"expected_bits=0x00000000 accepted=1 failure=0x00000000 error= passed=1")!=
				std::string::npos&&text.find("RESIDENT_EOS_ROUNDING_EDGE "
				"name=minimum_subnormal_bin_center expected_bits=0x00000001 accepted=1 "
				"failure=0x00000000 error= passed=1")!=std::string::npos&&
			text.find("RESIDENT_EOS passed=1 ")!=std::string::npos;
	}
	bool ValidateResidentTargetRawEvidence(const std::string& text)
	{
		auto fail=[](const char* reason){std::fprintf(stderr,"r200 raw validation: %s\n",reason);
			return false;};
		const std::array<const char*,5> fields={{"tangent","frozen_source",
			"absolute_reference_diagnostic","monitored_absolute_reference",
			"assembled_compatible_target"}};
		auto taggedDouble=[](const std::string& line,const char* tag,double& value){
			const std::size_t beginning=line.find(tag);if(beginning==std::string::npos)return false;
			const char* first=line.c_str()+beginning+std::strlen(tag);char* end=nullptr;
			value=std::strtod(first,&end);return end&&end!=first&&std::isfinite(value)&&
				(*end==' '||*end=='\0');};
		auto taggedUInt64=[](const std::string& line,const char* tag,std::uint64_t& value){
			const std::size_t beginning=line.find(tag);if(beginning==std::string::npos)return false;
			const char* first=line.c_str()+beginning+std::strlen(tag);char* end=nullptr;
			value=std::strtoull(first,&end,10);return end&&end!=first&&(*end==' '||*end=='\0');};
		std::array<std::array<std::array<bool,5>,64>,2> seen={};std::size_t rows=0u;
		std::array<std::array<double,5>,2> rowMaximumResidual={},rowMaximumBound={},
			rowMaximumRatio={};
		std::istringstream input(text);std::string line;
		while(std::getline(input,line)){
			if(line.rfind("RESIDENT_TARGET_CELL topology=",0u)!=0u)continue;
			char topologyName[32]={},field[64]={};std::size_t cell=64u;
			if(std::sscanf(line.c_str(),"RESIDENT_TARGET_CELL topology=%31s field=%63s cell=%zu",
				topologyName,field,&cell)!=3||cell>=64u)return fail("cell prefix");
			const std::size_t topology=std::strcmp(topologyName,"closed")==0?0u:
				(std::strcmp(topologyName,"pressure_open")==0?1u:2u);
			if(topology>=2u)return fail("cell topology");
			std::size_t fieldIndex=fields.size();
			for(std::size_t index=0u;index<fields.size();++index)
				if(std::strcmp(field,fields[index])==0)fieldIndex=index;
			double deviceValue=0.0,mirrorValue=0.0,exactValue=0.0,residual=0.0,bound=0.0,
				ratio=0.0;
			if(fieldIndex>=fields.size()||seen[topology][cell][fieldIndex]||
				line.find("units=s^-1")==std::string::npos||
				!taggedDouble(line,"device=",deviceValue)||
				!taggedDouble(line,"fp64_mirror_binary32_projection=",mirrorValue)||
				!taggedDouble(line,"fp64_exact=",exactValue)||
				!taggedDouble(line,"residual_s^-1=",residual)||
				!taggedDouble(line,"local_termwise_enclosure_s^-1=",bound)||
				!taggedDouble(line,"residual_over_local_bound=",ratio)||
				line.find("bit_equal=1")==std::string::npos||residual<0.0||bound<0.0||
				ratio<0.0||ratio>1.0)return fail("cell criterion");
			const float device=static_cast<float>(deviceValue),mirror=static_cast<float>(mirrorValue);
			const double center=static_cast<double>(mirror);
			const double spacing=std::max(std::fabs(static_cast<double>(std::nextafter(
				mirror,std::numeric_limits<float>::infinity()))-center),
				std::fabs(center-static_cast<double>(std::nextafter(mirror,
					-std::numeric_limits<float>::infinity()))));
			const double recomputedResidual=std::fabs(static_cast<double>(device)-exactValue);
			const double recomputedBound=fieldIndex==1u?0.0:0.5*spacing;
			const double recomputedRatio=recomputedBound>0.0?
				recomputedResidual/recomputedBound:0.0;
			if(std::memcmp(&device,&mirror,sizeof(float))!=0||residual!=recomputedResidual||
				bound!=recomputedBound||ratio!=recomputedRatio||
				(recomputedBound==0.0&&recomputedResidual!=0.0)||
				recomputedResidual>recomputedBound)return fail("cell recomputation");
			seen[topology][cell][fieldIndex]=true;++rows;
			rowMaximumResidual[topology][fieldIndex]=std::max(
				rowMaximumResidual[topology][fieldIndex],residual);
			rowMaximumBound[topology][fieldIndex]=std::max(
				rowMaximumBound[topology][fieldIndex],bound);
			rowMaximumRatio[topology][fieldIndex]=std::max(
				rowMaximumRatio[topology][fieldIndex],ratio);
		}
		if(rows!=640u)return fail("cell count");
		for(const auto& topology:seen)for(const auto& cell:topology)
			for(const bool field:cell)if(!field)return fail("cell coverage");
		if(CountText(text,"RESIDENT_TARGET_TERM topology=")!=10u)return fail("term count");
		const std::array<const char*,2> topologyNames={{"closed","pressure_open"}};
		for(std::size_t topology=0u;topology<topologyNames.size();++topology)
		for(std::size_t fieldIndex=0u;fieldIndex<fields.size();++fieldIndex){
			const char* topologyName=topologyNames[topology];const char* field=fields[fieldIndex];
			const std::size_t position=text.find(std::string("RESIDENT_TARGET_TERM topology=")+
				topologyName+" field="+field+" units=s^-1 scope=every_cell ");
			if(position==std::string::npos)return fail("term prefix");const std::size_t end=text.find('\n',position);
			const std::string record=text.substr(position,end-position);double residual=0.0,bound=0.0,
				ratio=0.0;if(!taggedDouble(record,"max_residual_s^-1=",residual)||
				!taggedDouble(record,"max_local_termwise_enclosure_s^-1=",bound)||
				!taggedDouble(record,"worst_residual_over_local_bound=",ratio)||
				record.find("bit_equal=1 passed=1")==std::string::npos||residual<0.0||bound<0.0||
				ratio<0.0||ratio>1.0||((std::strcmp(field,"frozen_source")==0)!=(bound==0.0))||
				residual!=rowMaximumResidual[topology][fieldIndex]||
				bound!=rowMaximumBound[topology][fieldIndex]||
				ratio!=rowMaximumRatio[topology][fieldIndex])
				return fail("term criterion");}
		const std::array<const char*,12> unsealed={{"unsealed_transport_parent",
			"unsealed_physical_flux_parent","unsealed_EOS_candidate_parent",
			"unsealed_EOS_publication_parent","unsealed_frozen_source_parent",
			"mismatched_frozen_source_packet","stale_target_shape",
			"stale_target_face_offsets","stale_target_cell_width","stale_target_timestep",
			"stale_target_attempt","stale_target_boundary"}};
		for(const char* name:unsealed)if(text.find(std::string(
			"RESIDENT_TARGET_RED name=")+name+" layer=device expected=0x00000800 "
			"observed=0x00002800 attempted=1 read=1 target_identity=0 "
			"consumer_identity=0 passed=1")==std::string::npos)return fail("unsealed RED");
		const std::array<const char*,10> host={{"non_immediate_stale_candidate",
			"EOS_accepted_but_unlinked_candidate","CPU_produced_frozen_source",
			"CPU_private_blit_frozen_source","mismatched_EOS_thermochemistry",
			"CPU_produced_target_surface","CPU_forged_projection_metadata",
			"understated_working_set","wrong_parent_frozen_source_beginning",
			"wrong_parent_frozen_source_case"}};
		for(const char* name:host)if(text.find(std::string(
			"RESIDENT_TARGET_RED name=")+name+" layer=host_preflight attempted=0 "
			"read=0 target_identity=0 passed=1")==std::string::npos)return fail("host RED");
		const std::array<const char*,2> consumer={{"preauthored_projection_target",
			"mismatched_projection_topology"}};
		for(const char* name:consumer){const std::size_t position=text.find(std::string(
			"RESIDENT_TARGET_RED name=")+name+" layer=device expected=0x00002000 "
			"observed=0x00002000 attempted=1 read=1 target_identity=");
			if(position==std::string::npos)return fail("consumer RED prefix");const std::size_t end=text.find('\n',position);
			const std::string record=text.substr(position,end-position);
			if(record.find("target_identity=0 ")!=std::string::npos||
				record.find("consumer_identity=0 passed=1")==std::string::npos)return fail("consumer RED criterion");}
		const std::size_t dormant=text.find("RESIDENT_TARGET_RED "
			"name=dormant_tail_threshold_identity layer=device_identity ");
		if(dormant==std::string::npos)return fail("dormant RED prefix");const std::size_t dormantEnd=text.find('\n',dormant);
		const std::string dormantRecord=text.substr(dormant,dormantEnd-dormant);
		std::uint64_t dormantBaseIdentity=0u,dormantAlternateIdentity=0u;
		const std::size_t identities=text.find("RESIDENT_TARGET_IDENTITIES ");
		if(identities==std::string::npos)return fail("identity record");
		const std::size_t identitiesEnd=text.find('\n',identities);
		const std::string identityRecord=text.substr(identities,identitiesEnd-identities);
		const std::array<const char*,8> identityTags={{"transport=","physical_flux=",
			"candidate=","eos=","frozen_source=","target=","projection_metadata=","consumer="}};
		for(const char* tag:identityTags){std::uint64_t identity=0u;
			if(!taggedUInt64(identityRecord,tag,identity)||identity==0u)
				return fail("identity criterion");}
		double boundREDProjectedValue=0.0,boundREDExact=0.0,boundREDResidual=0.0,
			boundREDEnclosure=0.0;
		const std::size_t boundRED=text.find("RESIDENT_TARGET_BOUND_RED units=s^-1 ");
		if(boundRED==std::string::npos)return fail("bound RED prefix");
		const std::size_t boundREDEnd=text.find('\n',boundRED);
		const std::string boundREDRecord=text.substr(boundRED,boundREDEnd-boundRED);
		return dormantRecord.find("base_accepted=1 alternate_accepted=1 fields_bit_equal=1 ")!=
			std::string::npos&&dormantRecord.find("base_failure=0x00000000 "
			"alternate_failure=0x00000000 ")!=std::string::npos&&
			taggedUInt64(dormantRecord,"base_identity=",dormantBaseIdentity)&&
			taggedUInt64(dormantRecord,"alternate_identity=",dormantAlternateIdentity)&&
			dormantBaseIdentity!=0u&&dormantAlternateIdentity!=0u&&
			dormantBaseIdentity!=dormantAlternateIdentity&&
			dormantRecord.find("passed=1")!=std::string::npos&&
			text.find("RESIDENT_TARGET_THRESHOLD_COVERAGE integrated_positive_below=1 "
			"integrated_positive_above=1 integrated_negative_below=1 "
			"integrated_negative_above=1 mirror_positive_at_no_drain=1 "
			"mirror_negative_at_no_drain=1 passed=1")!=std::string::npos&&
			text.find("RESIDENT_TARGET_THRESHOLD_DEVICE_EQUALITY positive_accepted=1 "
			"negative_accepted=1 positive_tail_positive_zero=1 "
			"negative_tail_positive_zero=1 positive_error= negative_error= passed=1")!=
				std::string::npos&&
			taggedDouble(boundREDRecord,"projected=",boundREDProjectedValue)&&
			taggedDouble(boundREDRecord,"displaced_exact=",boundREDExact)&&
			taggedDouble(boundREDRecord,"residual_s^-1=",boundREDResidual)&&
			taggedDouble(boundREDRecord,"local_rounding_enclosure_s^-1=",boundREDEnclosure)&&
			static_cast<float>(boundREDProjectedValue)==1.0f&&
			boundREDResidual==std::fabs(static_cast<double>(
				static_cast<float>(boundREDProjectedValue))-boundREDExact)&&
			boundREDEnclosure==0.5*std::max(std::fabs(static_cast<double>(std::nextafter(
				static_cast<float>(boundREDProjectedValue),std::numeric_limits<float>::infinity()))-
				static_cast<double>(static_cast<float>(boundREDProjectedValue))),
				std::fabs(static_cast<double>(static_cast<float>(boundREDProjectedValue))-
				static_cast<double>(std::nextafter(static_cast<float>(boundREDProjectedValue),
					-std::numeric_limits<float>::infinity()))))&&
			boundREDResidual>boundREDEnclosure&&boundREDRecord.find("passed=1")!=std::string::npos&&
			identityRecord.find("all_nonzero=1")!=std::string::npos&&
			text.find("RESIDENT_TARGET_RED name=interstage_full_grid_transfer "
			"layer=observed_transfer_ledger attempted=1 read=1 transfers=1 staging=1 ")!=
				std::string::npos&&
			CountText(text,"RESIDENT_TARGET_RED ")==26u&&text.find(
			"RESIDENT_TARGET passed=1 tangent_bit_equal=1 source_bit_equal=1 "
			"absolute_diagnostic_bit_equal=1 tail_bit_equal=1 assembled_bit_equal=1 "
			"unsealed_transport=1 unsealed_physical=1 unsealed_candidate=1 "
			"unsealed_eos=1 unsealed_source=1 stale_candidate=1 unlinked_eos=1 "
			"cpu_source_refused=1 cpu_private_source_refused=1 source_packet_refused=1 "
			"source_beginning_refused=1 source_case_refused=1 "
			"eos_thermo_refused=1 stale_metadata_refused=1 cpu_target_refused=1 "
			"cpu_projection_metadata_refused=1 "
			"preauthored_refused=1 topology_refused=1 dormant_threshold_identity=1 "
			"closed_branch_bitmap=0x03700180 closed_required=0x03700180 "
			"open_branch_bitmap=0x02f00180 open_required=0x02f00180 "
			"command_per_interval=1 reads_per_interval=1 transfers_per_interval=0 "
			"enclosures=1 fixture_ws=1507328 actual_ws=132000 live_ws=196608")!=std::string::npos;
	}
	bool ValidateResidentTargetEvidenceAgainstRaw(const std::string& evidence,
		const std::string& raw)
	{
		auto lineFor=[](const std::string& text,const std::string& prefix){
			const std::size_t beginning=text.find(prefix);if(beginning==std::string::npos)return std::string();
			const std::size_t end=text.find('\n',beginning);return text.substr(beginning,end-beginning);};
		auto taggedDouble=[](const std::string& line,const std::string& tag,double& value){
			const std::size_t beginning=line.find(tag);if(beginning==std::string::npos)return false;
			const char* first=line.c_str()+beginning+tag.size();char* end=nullptr;
			value=std::strtod(first,&end);return end&&end!=first&&std::isfinite(value)&&
				(*end==' '||*end=='\0');};
		auto taggedUInt64=[](const std::string& line,const std::string& tag,std::uint64_t& value){
			const std::size_t beginning=line.find(tag);if(beginning==std::string::npos)return false;
			const char* first=line.c_str()+beginning+tag.size();char* end=nullptr;
			value=std::strtoull(first,&end,10);return end&&end!=first&&(*end==' '||*end=='\0');};
		auto evidenceDouble=[&](const std::string& key,double& value){
			return taggedDouble(lineFor(evidence,key+" "),key+" ",value);};
		auto evidenceUInt64=[&](const std::string& key,std::uint64_t& value){
			return taggedUInt64(lineFor(evidence,key+" "),key+" ",value);};
		std::size_t lineCount=0u;for(const char value:raw)if(value=='\n')++lineCount;
		std::uint64_t recordedLines=0u,recordedCells=0u,recordedTerms=0u,recordedREDs=0u;
		if(!evidenceUInt64("raw_transcript_lines",recordedLines)||recordedLines!=lineCount||
			!evidenceUInt64("raw_transcript_per_cell_rows",recordedCells)||recordedCells!=640u||
			!evidenceUInt64("raw_transcript_term_rows",recordedTerms)||recordedTerms!=10u||
			!evidenceUInt64("raw_transcript_RED_rows",recordedREDs)||
			recordedREDs!=CountText(raw,"RESIDENT_TARGET_RED "))return false;
		const std::string rawSHA=RISE::RISECBOR64::SHA256Hex(
			RISE::RISECBOR64::Bytes(raw.begin(),raw.end()));
		if(evidence.find("raw_transcript_sha256 "+rawSHA+"\n")==std::string::npos)return false;
		std::istringstream rawRows(raw);std::string rawRow,REDRows;
		while(std::getline(rawRows,rawRow))if(rawRow.rfind("RESIDENT_TARGET_RED ",0u)==0u)
			REDRows+=rawRow+"\n";
		const std::string REDSHA=RISE::RISECBOR64::SHA256Hex(
			RISE::RISECBOR64::Bytes(REDRows.begin(),REDRows.end()));
		if(evidence.find("raw_RED_rows_sha256 "+REDSHA+"\n")==std::string::npos)return false;
		const char* topologyNames[2]={"closed","pressure_open"};
		const char* rawFields[5]={"tangent","frozen_source","absolute_reference_diagnostic",
			"monitored_absolute_reference","assembled_compatible_target"};
		const char* evidenceFields[5]={"tangent","frozen_source","absolute_diagnostic",
			"monitored_tail","assembled"};
		for(unsigned int topology=0u;topology<2u;++topology)
		for(unsigned int field=0u;field<5u;++field){
			const std::string record=lineFor(raw,std::string("RESIDENT_TARGET_TERM topology=")+
				topologyNames[topology]+" field="+rawFields[field]+" ");
			double residual=0.0,bound=0.0,ratio=0.0,evidenceResidual=0.0,evidenceBound=0.0,
				evidenceRatio=0.0;
			const std::string key=std::string(topologyNames[topology])+"_"+evidenceFields[field];
			if(record.empty()||!taggedDouble(record,"max_residual_s^-1=",residual)||
				!taggedDouble(record,"max_local_termwise_enclosure_s^-1=",bound)||
				!taggedDouble(record,"worst_residual_over_local_bound=",ratio)||
				!evidenceDouble(key+"_max_residual_s^-1",evidenceResidual)||
				!evidenceDouble(key+"_max_local_bound_s^-1",evidenceBound)||
				!evidenceDouble(key+"_worst_residual_over_local_bound",evidenceRatio)||
				residual!=evidenceResidual||bound!=evidenceBound||ratio!=evidenceRatio)return false;
		}
		const std::string identities=lineFor(raw,"RESIDENT_TARGET_IDENTITIES ");
		const char* rawIdentityTags[8]={"transport=","physical_flux=","candidate=","eos=",
			"frozen_source=","target=","projection_metadata=","consumer="};
		const char* evidenceIdentityKeys[8]={"published_identity_transport",
			"published_identity_physical_flux","published_identity_candidate",
			"published_identity_EOS","published_identity_frozen_source",
			"published_identity_target","published_identity_projection_metadata",
			"published_identity_consumer"};
		for(unsigned int index=0u;index<8u;++index){std::uint64_t rawValue=0u,evidenceValue=0u;
			if(!taggedUInt64(identities,rawIdentityTags[index],rawValue)||
				!evidenceUInt64(evidenceIdentityKeys[index],evidenceValue)||
				rawValue==0u||rawValue!=evidenceValue)return false;}
		const std::string summary=lineFor(raw,"RESIDENT_TARGET passed=1 ");
		const std::string evidenceMeasurement=lineFor(evidence,"measurement RESIDENT_TARGET ");
		if(evidenceMeasurement.rfind("measurement ",0u)!=0u||
			evidenceMeasurement.substr(12u)!=summary)return false;
		const std::string dormant=lineFor(raw,
			"RESIDENT_TARGET_RED name=dormant_tail_threshold_identity ");
		std::uint64_t rawDormantBase=0u,rawDormantAlternate=0u,evidenceDormantBase=0u,
			evidenceDormantAlternate=0u;
		if(!taggedUInt64(dormant,"base_identity=",rawDormantBase)||
			!taggedUInt64(dormant,"alternate_identity=",rawDormantAlternate)||
			!evidenceUInt64("dormant_tail_threshold_base_identity",evidenceDormantBase)||
			!evidenceUInt64("dormant_tail_threshold_alternate_identity",evidenceDormantAlternate)||
			rawDormantBase!=evidenceDormantBase||
			rawDormantAlternate!=evidenceDormantAlternate)return false;
		const char* rawWorkingSetTags[3]={"fixture_ws=","actual_ws=","live_ws="};
		const char* evidenceWorkingSetKeys[3]={"fixture_working_set_bytes",
			"actual_fixture_allocation_bytes","live_increment_working_set_bytes"};
		for(unsigned int index=0u;index<3u;++index){std::uint64_t rawValue=0u,evidenceValue=0u;
			if(!taggedUInt64(summary,rawWorkingSetTags[index],rawValue)||
				!evidenceUInt64(evidenceWorkingSetKeys[index],evidenceValue)||
				rawValue!=evidenceValue)return false;}
		return true;
	}
	bool IndependentR60AndPeriodicCommuting(
		const RISE::FireProductionScalarFCTRequest& request,
		const RISE::FireProductionScalarFCTResult& result)
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		const std::size_t cells=shape.CellCount();
		if(result.accepted.size()!=9u*cells)return false;
		for(const RISE::FireProductionProjectionBoundary boundary:request.boundary)
			if(boundary!=RISE::FireProductionProjectionPeriodic)return false;
		auto inequality=[&](const std::array<float,9>& value,const std::size_t row){
			if(row==0u)return -value[0u];
			if(row==1u){
				float closure=value[0u];
				for(std::size_t species=0u;species<7u;++species)
					closure-=value[1u+species];
				return closure;
			}
			if(row<9u)return -value[row-1u];
			if(row==9u){
				float lower=-value[8u];
				for(std::size_t species=0u;species<7u;++species)
					lower+=request.enthalpyBoundsJPerKG[species]*value[1u+species];
				return lower;
			}
			float upper=value[8u];for(std::size_t species=0u;species<7u;++species)
				upper-=request.enthalpyBoundsJPerKG[7u+species]*value[1u+species];
			return upper;
		};
		for(std::size_t cell=0u;cell<cells;++cell){
			std::array<float,9> value={{}};float scale=1.0f;
			for(std::size_t component=0u;component<9u;++component){
				value[component]=result.accepted[component*cells+cell];
				scale+=std::fabs(value[component]);
			}
			for(std::size_t row=0u;row<11u;++row)
				if(!std::isfinite(inequality(value,row))||
					inequality(value,row)>request.feasibilityFactor*scale)return false;
		}
		std::vector<float> baseGas(cells,0.0f),acceptedGas(cells,0.0f);
		for(std::size_t component=1u;component<=6u;++component)
			for(std::size_t cell=0u;cell<cells;++cell){
				baseGas[cell]+=request.beginning[component*cells+cell]+
					request.sourceDelta[component*cells+cell];
				acceptedGas[cell]+=result.accepted[component*cells+cell];
			}
		RISE::FireProductionCompatibleFCTMomentumRequest identity;
		identity.shape=shape;identity.boundary=request.boundary;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t faces=RISE::FireProductionProjectionFaceCount(shape,axis);
			identity.lowGasFluxKGPerM2S[axis]=result.acceptedGasFluxKGPerM2S[axis];
			identity.highGasFluxKGPerM2S[axis]=result.acceptedGasFluxKGPerM2S[axis];
			identity.sharedFaceAlpha[axis].assign(faces,0.0f);
			identity.frozenVelocityMPerS[axis].assign(faces,1.0f);
		}
		RISE::FireProductionCompatibleFCTMomentumResult rate;std::string error;
		if(!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(identity,rate,&error))
			return false;
		auto cellIndex=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			return (z*shape.ny+y)*shape.nx+x;};
		float maximum=0.0f,scale=1.0f;
		for(unsigned int axis=0u;axis<3u;++axis)
			for(std::size_t face=0u;face<rate.advectionRateKGPerM2S2[axis].size();++face){
				std::size_t x=0u,y=0u,z=0u,coordinate=0u,extent=0u;
				if(axis==0u){x=face%(shape.nx+1u);const std::size_t line=face/(shape.nx+1u);
					y=line%shape.ny;z=line/shape.ny;coordinate=x;extent=shape.nx;}
				if(axis==1u){x=face%shape.nx;const std::size_t line=face/shape.nx;
					y=line%(shape.ny+1u);z=line/(shape.ny+1u);coordinate=y;extent=shape.ny;}
				if(axis==2u){x=face%shape.nx;const std::size_t line=face/shape.nx;
					y=line%shape.ny;z=line/shape.ny;coordinate=z;extent=shape.nz;}
				std::size_t lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
				const std::size_t lowCoordinate=coordinate==0u?extent-1u:coordinate-1u;
				const std::size_t highCoordinate=coordinate==extent?0u:coordinate;
				if(axis==0u){lx=lowCoordinate;hx=highCoordinate;}
				if(axis==1u){ly=lowCoordinate;hy=highCoordinate;}
				if(axis==2u){lz=lowCoordinate;hz=highCoordinate;}
				const float base=0.5f*(baseGas[cellIndex(lx,ly,lz)]+
					baseGas[cellIndex(hx,hy,hz)]);
				const float accepted=0.5f*(acceptedGas[cellIndex(lx,ly,lz)]+
					acceptedGas[cellIndex(hx,hy,hz)]);
				const float advanced=base-request.timeStepS*
					rate.advectionRateKGPerM2S2[axis][face];
				maximum=std::max(maximum,std::fabs(accepted-advanced));
				scale=std::max(scale,std::max(std::fabs(accepted),std::fabs(advanced)));
			}
		return result.commutingIdentityAvailable&&std::fabs(maximum-
			result.maximumCommutingResidualKGPerM3)<=
			8.0f*std::numeric_limits<float>::epsilon()*scale;
	}
	double CSVColumnMaximum(const std::string& text,const std::size_t column,
		std::size_t& rows)
	{
		rows=0u;double maximum=-std::numeric_limits<double>::infinity();
		std::istringstream input(text);std::string line;
		if(!std::getline(input,line))return maximum;
		while(std::getline(input,line)){
			if(line.empty())continue;std::istringstream fields(line);std::string field;
			for(std::size_t index=0u;index<=column;++index)
				if(!std::getline(fields,field,','))return
					-std::numeric_limits<double>::infinity();
			char* end=nullptr;const double value=std::strtod(field.c_str(),&end);
			if(!end||*end!='\0'||!std::isfinite(value))return
				-std::numeric_limits<double>::infinity();
			maximum=std::max(maximum,value);++rows;
		}
		return maximum;
	}

	class ConstantProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(!context.conservativeValues||context.conservativeValues->size()%9u){
				if(error)*error="constant transport context is malformed";return false;
			}
			const std::size_t cells=context.conservativeValues->size()/9u;
			result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
			result.parentCandidateIdentity=context.parentCandidateIdentity;
			result.projectionIdentity=context.projectionIdentity;
			result.diffusivityM2PerS.assign(cells,0.0f);
			result.conductivityWPerMK.assign(cells,0.0f);
			result.molecularKinematicViscosityM2PerS.assign(cells,0.0f);
			result.publicationIdentity=
				RISE::FireProductionProjectedHeunTransportPublicationIdentity(context,result);
			if(error)error->clear();return true;
		}
	};

	class ForgedProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		explicit ForgedProjectedHeunTransport32(const bool staleProjection) :
			staleProjection_(staleProjection),firstProjection_(0u) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			ConstantProjectedHeunTransport32 valid;
			if(!valid.Evaluate(context,result,error))return false;
			if(staleProjection_){
				if(firstProjection_==0u)firstProjection_=context.projectionIdentity;
				result.projectionIdentity=firstProjection_;
			}else ++result.parentCandidateIdentity;
			return true;
		}
	private:
		bool staleProjection_;
		mutable std::uint64_t firstProjection_;
	};

	class ReplayedProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		ReplayedProjectedHeunTransport32() : firstIdentity_(0u) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			ConstantProjectedHeunTransport32 valid;
			if(!valid.Evaluate(context,result,error))return false;
			if(firstIdentity_==0u)firstIdentity_=result.publicationIdentity;
			else result.publicationIdentity=firstIdentity_;
			return true;
		}
	private:
		mutable std::uint64_t firstIdentity_;
	};

	enum class ProjectedHeunContextOperand
	{
		ConservativeState,
		Temperature,
		ProjectedVelocity,
		Stage,
		AttemptIdentity,
		ParentCandidateIdentity,
		ProjectionIdentity,
		ConservativeStatePointer,
		TemperaturePointer,
		ProjectedVelocityPointer
	};

	class MutatingProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		MutatingProjectedHeunTransport32(const RISE::FireProductionProjectedHeunStage stage,
			const ProjectedHeunContextOperand operand,const bool returnSuccess) :
			stage_(stage),operand_(operand),returnSuccess_(returnSuccess),mutated_(false) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(context.stage==stage_&&!mutated_){
				ConstantProjectedHeunTransport32 valid;
				if(!valid.Evaluate(context,result,error))return false;
				float* value=0;
				if(operand_==ProjectedHeunContextOperand::ConservativeState&&
					context.conservativeValues&&!context.conservativeValues->empty())
					value=&const_cast<std::vector<float>*>(context.conservativeValues)->front();
				else if(operand_==ProjectedHeunContextOperand::Temperature&&
					context.temperatureK&&!context.temperatureK->empty())
					value=&const_cast<std::vector<float>*>(context.temperatureK)->front();
				else if(operand_==ProjectedHeunContextOperand::ProjectedVelocity&&
					context.projectedVelocityMPerS)
					for(unsigned int axis=0u;axis<3u&&!value;++axis)if(
						!(*context.projectedVelocityMPerS)[axis].empty())value=&const_cast<
							std::array<std::vector<float>,3>*>(context.projectedVelocityMPerS)->
							at(axis).front();
				auto& mutableContext=const_cast<
					RISE::FireProductionProjectedHeunTransportContext&>(context);
				if(value)*value=std::nextafter(*value,std::numeric_limits<float>::infinity());
				else if(operand_==ProjectedHeunContextOperand::Stage)
					mutableContext.stage=stage_==RISE::FireProductionProjectedHeunStage::R0?
						RISE::FireProductionProjectedHeunStage::R1:
						RISE::FireProductionProjectedHeunStage::R0;
				else if(operand_==ProjectedHeunContextOperand::AttemptIdentity)
					++mutableContext.attemptIdentity;
				else if(operand_==ProjectedHeunContextOperand::ParentCandidateIdentity)
					++mutableContext.parentCandidateIdentity;
				else if(operand_==ProjectedHeunContextOperand::ProjectionIdentity)
					++mutableContext.projectionIdentity;
				else if(operand_==ProjectedHeunContextOperand::ConservativeStatePointer)
					mutableContext.conservativeValues=context.temperatureK;
				else if(operand_==ProjectedHeunContextOperand::TemperaturePointer)
					mutableContext.temperatureK=context.conservativeValues;
				else if(operand_==ProjectedHeunContextOperand::ProjectedVelocityPointer)
					mutableContext.projectedVelocityMPerS=&alternateVelocity_;
				else { if(error)*error="mutation context is malformed";return false; }
				mutated_=true;
				if(!returnSuccess_){
					if(error)*error="injected mutated transport refusal";return false;
				}
				if(error)error->clear();return true;
			}
			ConstantProjectedHeunTransport32 valid;
			return valid.Evaluate(context,result,error);
		}
		bool Mutated() const { return mutated_; }
	private:
		RISE::FireProductionProjectedHeunStage stage_;
		ProjectedHeunContextOperand operand_;
		bool returnSuccess_;
		mutable bool mutated_;
		mutable std::array<std::vector<float>,3> alternateVelocity_;
	};

	class MutatingOuterResultProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		MutatingOuterResultProjectedHeunTransport32(
			RISE::FireProductionProjectedHeunOwnerResult& outer,
			const RISE::FireProductionProjectedHeunOwnerResult& stale) :
			outer_(outer),stale_(stale),mutated_(false) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(context.stage==RISE::FireProductionProjectedHeunStage::R2&&!mutated_){
				outer_=stale_;mutated_=true;
				if(error)*error="injected outer-result mutation";return false;
			}
			ConstantProjectedHeunTransport32 valid;
			return valid.Evaluate(context,result,error);
		}
		bool Mutated() const { return mutated_; }
	private:
		RISE::FireProductionProjectedHeunOwnerResult& outer_;
		const RISE::FireProductionProjectedHeunOwnerResult& stale_;
		mutable bool mutated_;
	};

	class ReentrantProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		ReentrantProjectedHeunTransport32(
			RISE::FireProductionProjectedHeunCPUOwner& owner,
			const RISE::FireProductionProjectedHeunOwnerResult& staleResult) :
			owner_(owner),attempted_(false),r0Rejected_(false),r1Rejected_(false),
			r2Rejected_(false),nestedResult_(staleResult) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			ConstantProjectedHeunTransport32 valid;
			if(!attempted_){
				attempted_=true;std::string nestedError;
				r0Rejected_=!owner_.SolveR0(valid,&nestedError)&&
					nestedError=="projected-Heun R0 is out of order";
				r1Rejected_=!owner_.SolveR1(valid,&nestedError)&&
					nestedError=="projected-Heun R1 is out of order";
				r2Rejected_=!owner_.SolveR2(valid,nestedResult_,&nestedError)&&
					nestedError=="projected-Heun R2 is out of order";
			}
			return valid.Evaluate(context,result,error);
		}
		bool NestedStagesRejected() const
			{ return attempted_&&r0Rejected_&&r1Rejected_&&r2Rejected_; }
		const RISE::FireProductionProjectedHeunOwnerResult& NestedResult() const
			{ return nestedResult_; }
	private:
		RISE::FireProductionProjectedHeunCPUOwner& owner_;
		mutable bool attempted_,r0Rejected_,r1Rejected_,r2Rejected_;
		mutable RISE::FireProductionProjectedHeunOwnerResult nestedResult_;
	};

	class ConstantProjectedHeunTransport64 final :
		public RISEFireProductionFP64::FireProductionProjectedHeunTransportProvider
	{
	public:
		bool Evaluate(
			const RISEFireProductionFP64::FireProductionProjectedHeunTransportContext& context,
			RISEFireProductionFP64::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(!context.conservativeValues||context.conservativeValues->size()%9u){
				if(error)*error="constant fp64 transport context is malformed";return false;
			}
			const std::size_t cells=context.conservativeValues->size()/9u;
			result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
			result.parentCandidateIdentity=context.parentCandidateIdentity;
			result.projectionIdentity=context.projectionIdentity;
			result.diffusivityM2PerS.assign(cells,0.0);
			result.conductivityWPerMK.assign(cells,0.0);
			result.molecularKinematicViscosityM2PerS.assign(cells,0.0);
			result.publicationIdentity=
				RISEFireProductionFP64::FireProductionProjectedHeunTransportPublicationIdentity(
					context,result);
			if(error)error->clear();return true;
		}
	};

	class OracleOpenProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		OracleOpenProjectedHeunTransport32(const RISE::FireProductionProjectionShape& shape,
			const RISE::FireSim::OpenBoundaryConfig3D& boundary) :
			shape_(shape),boundary_(boundary) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			const std::size_t cells=shape_.CellCount();
			if(!context.conservativeValues||context.conservativeValues->size()!=9u*cells||
				!context.temperatureK||context.temperatureK->size()!=cells||
				!context.projectedVelocityMPerS)return false;
			RISE::FireSim::PeriodicMACShape shape;
			shape.nx=shape_.nx;shape.ny=shape_.ny;shape.nz=shape_.nz;
			shape.cellWidthM=shape_.cellWidthM;
			std::vector<RISE::FireSim::ConservativeVector> state(cells);
			std::vector<double> temperature(cells);
			for(std::size_t cell=0u;cell<cells;++cell){
				for(std::size_t component=0u;component<9u;++component)
					state[cell][component]=(*context.conservativeValues)[component*cells+cell];
				temperature[cell]=(*context.temperatureK)[cell];
			}
			RISE::FireSim::OpenMACField3D velocity;
			for(unsigned int axis=0u;axis<3u;++axis)
				velocity.component[axis].assign((*context.projectedVelocityMPerS)[axis].begin(),
					(*context.projectedVelocityMPerS)[axis].end());
			std::vector<double> diffusivity,conductivity,dynamicViscosity;
			if(!RISE::FireSim::BuildOpenStageTransport3D(shape,state,temperature,velocity,
				boundary_,false,RISE::FireSimulationMethaneRecord::PhysicalV1(),
				RISE::FireSimulationTransportRecord::OpenV1(),
				RISE::FireStateProducerPrecision::Binary32,diffusivity,conductivity,
				dynamicViscosity,error))return false;
			result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
			result.parentCandidateIdentity=context.parentCandidateIdentity;
			result.projectionIdentity=context.projectionIdentity;
			result.diffusivityM2PerS.resize(cells);
			result.conductivityWPerMK.resize(cells);
			result.molecularKinematicViscosityM2PerS.resize(cells);
			for(std::size_t cell=0u;cell<cells;++cell){
				double density=0.0;for(std::size_t component=1u;component<=6u;++component)
					density+=state[cell][component];
				result.diffusivityM2PerS[cell]=static_cast<float>(diffusivity[cell]);
				result.conductivityWPerMK[cell]=static_cast<float>(conductivity[cell]);
				result.molecularKinematicViscosityM2PerS[cell]=
					static_cast<float>(dynamicViscosity[cell]/density);
			}
			result.publicationIdentity=
				RISE::FireProductionProjectedHeunTransportPublicationIdentity(context,result);
			if(error)error->clear();return true;
		}
	private:
		RISE::FireProductionProjectionShape shape_;
		RISE::FireSim::OpenBoundaryConfig3D boundary_;
	};
}

int main()
{
	const RISE::FireProductionResidentStepRequest monitoredDefaultRequest;
	Check(monitoredDefaultRequest.monitorManifoldDiagnostics&&
		!monitoredDefaultRequest.enforceManifoldPlateau&&
		monitoredDefaultRequest.restoreManifoldOutliers,
		"production defaults to monitored diagnostics with conditional tail restoration");
	RISE::FireProductionResidentStepResult monitoredEligibility;
	monitoredEligibility.acceptedShape.nx=1u;monitoredEligibility.acceptedShape.ny=1u;
	monitoredEligibility.acceptedShape.nz=1u;monitoredEligibility.acceptedShape.cellWidthM=1.0f;
	monitoredEligibility.conservativeProducerPrecision=
		RISE::FireStateProducerPrecision::Binary32;
	monitoredEligibility.projection.validationPassed=true;
	monitoredEligibility.manifoldDiagnosticsMonitored=true;
	monitoredEligibility.manifoldPlateauPassed=true;
	monitoredEligibility.manifoldDynamicsBoundPassed=true;
	monitoredEligibility.residentProjectionInvocationCount=1u;
	monitoredEligibility.manifoldMapCellCount=1u;
	monitoredEligibility.manifoldScalarDeviceToHostTransferCount=1u;
	monitoredEligibility.maximumManifoldGeneration=0.085895776748657227;
	monitoredEligibility.manifoldStageGeneration[0]=
		monitoredEligibility.maximumManifoldGeneration;
	monitoredEligibility.maximumAcceptedManifoldDeviation=0.085895776748657227;
	monitoredEligibility.acceptedManifoldDeviationP95=0.00071418285369873047;
	monitoredEligibility.acceptedManifoldDeviationP50=1.1920928955078125e-07;
	monitoredEligibility.manifoldAllowanceExceeded=true;
	monitoredEligibility.manifoldCeilingExceeded=true;
	Check(FireProductionResidentStepEligibleForAcceptedManifoldToken(monitoredEligibility),
		"monitored threshold crossings remain eligible after conservation and projection gates");
	RISE::FireProductionResidentStepResult malformedDistribution=monitoredEligibility;
	malformedDistribution.acceptedManifoldDeviationP95=
		malformedDistribution.maximumAcceptedManifoldDeviation+1.0;
	Check(!FireProductionResidentStepEligibleForAcceptedManifoldToken(malformedDistribution),
		"monitored token eligibility rejects a malformed deviation distribution");
	RISE::FireProductionResidentStepResult forgedCrossing=monitoredEligibility;
	forgedCrossing.manifoldCeilingExceeded=false;
	Check(!FireProductionResidentStepEligibleForAcceptedManifoldToken(forgedCrossing),
		"monitored token eligibility binds threshold-crossing identity");
	RISE::FireProductionResidentStepResult targetedEligibility=monitoredEligibility;
	targetedEligibility.physicalProjection.validationPassed=true;
	targetedEligibility.residentProjectionInvocationCount=2u;
	targetedEligibility.manifoldTailRestorationApplied=true;
	targetedEligibility.manifoldTailCellCount=1u;
	targetedEligibility.manifoldTailExcessSum=0.01;
	targetedEligibility.manifoldTailDrainedVolumeM3=1.0e-6;
	Check(FireProductionResidentStepEligibleForAcceptedManifoldToken(targetedEligibility),
		"targeted monitored restoration remains eligible only with two validating projections");
	targetedEligibility.manifoldTailRestorationApplied=false;
	Check(!FireProductionResidentStepEligibleForAcceptedManifoldToken(targetedEligibility),
		"tail population cannot be published without its conditional restoration identity");
	std::array<std::vector<float>,3> zeroTransportVelocity;
	for(std::vector<float>& axis:zeroTransportVelocity)axis.assign(2u,0.0f);
	std::array<std::vector<float>,3> movingTransportVelocity=zeroTransportVelocity;
	movingTransportVelocity[1][0]=1.0f;
	Check(RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>{0.0,0.0,0.0},movingTransportVelocity)&&
		RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>{0.0,0.125,0.0},zeroTransportVelocity)&&
		!RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>{0.0,0.125,0.0},movingTransportVelocity)&&
		!RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>(),zeroTransportVelocity),
		"Eulerian generation is material only from zero beginning or exact rest");
	unsigned int retryCandidate=0u;
	double retryStep=0.0;
	RISE::FireProductionResidentStepResult syntheticRetry;
	syntheticRetry.representedTimeStepS=0.00055692793102934957f;
	syntheticRetry.suggestedManifoldTimeStepS=0.00055690890514272363;
	syntheticRetry.manifoldNextTimeStepAvailable=true;
	RISE::FireProductionResidentStepResult nonReducingRetry=syntheticRetry;
	nonReducingRetry.suggestedManifoldTimeStepS=static_cast<double>(
		nonReducingRetry.representedTimeStepS);
	RISE::FireProductionResidentStepResult unauthoritativeAcceptance=syntheticRetry;
	unauthoritativeAcceptance.manifoldPlateauPassed=true;
	Check(RISE::ClassifyFireProductionResidentStepAttempt(1u,syntheticRetry,retryCandidate,
			retryStep)==RISE::FireProductionResidentStepAttemptDisposition::
			RetryAtSuggestedTimeStep&&retryCandidate==2u&&
		retryStep==syntheticRetry.suggestedManifoldTimeStepS&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			RISE::FireStepRejectionRetryCap-2u,syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::RetryAtSuggestedTimeStep&&
		retryCandidate==RISE::FireStepRejectionRetryCap-1u&&
		retryStep==syntheticRetry.suggestedManifoldTimeStepS&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			RISE::FireStepRejectionRetryCap-1u,syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		retryCandidate==0u&&retryStep==0.0&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			RISE::FireStepRejectionRetryCap,syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			std::numeric_limits<unsigned int>::max(),syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		RISE::ClassifyFireProductionResidentStepAttempt(0u,nonReducingRetry,
			retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		RISE::ClassifyFireProductionResidentStepAttempt(0u,unauthoritativeAcceptance,
			retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected,
		"drain-aware plateau retry is a strict reduction bounded by the existing cap");
	auto floatFromBits=[](const std::uint32_t bits){float value=0.0f;
		std::memcpy(&value,&bits,sizeof(value));return value;};
	RISE::FireProductionResidentStepResult digestCollisionLeft,digestCollisionRight;
	digestCollisionLeft.conservativeValues.assign(3u,1.0f);
	digestCollisionRight.conservativeValues={{floatFromBits(UINT32_C(0x3f800001)),
		floatFromBits(UINT32_C(0x3f800006)),floatFromBits(UINT32_C(0x3f800001))}};
	Check(RISE::FireProductionAcceptedManifoldPayloadDigest(digestCollisionLeft)!=
		RISE::FireProductionAcceptedManifoldPayloadDigest(digestCollisionRight),
		"accepted resident authority digest rejects the retired two-moment three-word collision");
	const double equalTimeProduction=static_cast<double>(
		static_cast<float>(0.000579539999762149));
	const double equalTimeSubstep=equalTimeProduction*0.125;
	std::vector<double> equalTimeSchedule(8u,equalTimeSubstep);
	equalTimeSchedule.back()=equalTimeProduction-
		equalTimeSubstep*static_cast<double>(equalTimeSchedule.size()-1u);
	const std::string equalTimeTerminalTarget="terminal-target";
	const std::string equalTimePenultimateTarget="penultimate-target";
	std::vector<double> mismatchedEqualTimeSchedule=equalTimeSchedule;
	mismatchedEqualTimeSchedule.back()+=equalTimeSubstep;
	Check(FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		equalTimeSchedule,equalTimeProduction,equalTimeProduction,
		equalTimeTerminalTarget,equalTimeTerminalTarget),
		"equal-time reference accepts the exact shared endpoint");
	Check(!FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		mismatchedEqualTimeSchedule,equalTimeProduction,equalTimeProduction,
		equalTimeTerminalTarget,equalTimeTerminalTarget),
		"equal-time reference rejects a mismatched endpoint");
	Check(!FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		equalTimeSchedule,equalTimeProduction,equalTimeSubstep,
		equalTimeTerminalTarget,equalTimeTerminalTarget),
		"equal-time reference refuses a stale-dt terminal target");
	Check(!FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		equalTimeSchedule,equalTimeProduction,equalTimeProduction,
		equalTimeTerminalTarget,equalTimePenultimateTarget),
		"equal-time reference refuses the penultimate target at the current endpoint");
	double zeroAnomalyTarget=0.0,activeAnomalyTarget=0.0;
	const bool zeroAnomalyDerived=RISE::DeriveFireProductionAdvectiveAnomalyTarget(
		0.125,0.125,0.5,0.25,zeroAnomalyTarget);
	const bool activeAnomalyDerived=RISE::DeriveFireProductionAdvectiveAnomalyTarget(
		0.125,0.375,0.5,0.25,activeAnomalyTarget);
	const double missingRampTarget=0.25;
	const double halfRampTarget=0.25+0.5*(0.375-0.125)/0.5;
	const double reversedRampTarget=0.25-(0.375-0.125)/0.5;
	Check(zeroAnomalyDerived&&zeroAnomalyTarget==0.25&&activeAnomalyDerived&&
		activeAnomalyTarget==0.75&&missingRampTarget!=activeAnomalyTarget&&
		halfRampTarget!=activeAnomalyTarget&&reversedRampTarget!=activeAnomalyTarget,
		"advective-anomaly target is an exact zero-anomaly no-op and rejects missing, half, and reversed ramps");
	std::vector<double> longShadowFlat(FireProductionCalibration::LongShadowSteps,0.0025);
	std::vector<double> longShadowSecular(FireProductionCalibration::LongShadowSteps,0.0);
	std::vector<double> longShadowMaskedSecular(
		FireProductionCalibration::LongShadowSteps,0.01);
	for(std::size_t step=0u;step<FireProductionCalibration::LongShadowSteps;++step)
		longShadowSecular[step]=0.001+1.0e-6*static_cast<double>(step);
	const std::size_t longShadowFirst=FireProductionCalibration::LongShadowSteps-
		2u*FireProductionCalibration::LongShadowWindow;
	longShadowMaskedSecular[longShadowFirst]=0.03;
	for(std::size_t step=0u;step<FireProductionCalibration::LongShadowWindow;++step)
		longShadowMaskedSecular[FireProductionCalibration::LongShadowSteps-
			FireProductionCalibration::LongShadowWindow+step]=
			0.01+0.0003*static_cast<double>(step);
	Check(FireProductionCalibration::LongShadowNonsecular(longShadowFlat)&&
		!FireProductionCalibration::LongShadowNonsecular(longShadowSecular)&&
		!FireProductionCalibration::LongShadowNonsecular(longShadowMaskedSecular),
		"long-shadow detector rejects monotone and prior-outlier-masked secular growth");
	auto appendDistributionStatistics=[](std::vector<double> field,
		std::vector<double>& maximum,std::vector<double>& p95,std::vector<double>& p50){
		std::sort(field.begin(),field.end());
		maximum.push_back(field.back());p50.push_back(field[(field.size()-1u)/2u]);
		p95.push_back(field[(95u*field.size()+99u)/100u-1u]);
	};
	std::vector<double> motionPattern(100u,0.0);
	for(std::size_t index=0u;index<motionPattern.size();++index)
		motionPattern[index]=0.001+0.00019*static_cast<double>(index);
	std::vector<double> motionOnlyMaximum,motionOnlyP95,motionOnlyP50;
	std::vector<double> hiddenSecularMaximum,hiddenSecularP95,hiddenSecularP50;
	for(std::size_t step=0u;step<FireProductionCalibration::LongShadowSteps;++step){
		std::rotate(motionPattern.begin(),motionPattern.begin()+1u,motionPattern.end());
		appendDistributionStatistics(motionPattern,motionOnlyMaximum,motionOnlyP95,
			motionOnlyP50);
		std::vector<double> injected(100u,0.002);
		const double growing=0.005+0.00004*static_cast<double>(step);
		std::fill(injected.begin()+50u,injected.begin()+95u,growing);
		std::fill(injected.begin()+95u,injected.end(),0.02);
		appendDistributionStatistics(injected,hiddenSecularMaximum,hiddenSecularP95,
			hiddenSecularP50);
	}
	Check(FireProductionCalibration::LongShadowDistributionNonsecular(
			motionOnlyMaximum,motionOnlyP95,motionOnlyP50)&&
		!FireProductionCalibration::LongShadowDistributionNonsecular(
			hiddenSecularMaximum,hiddenSecularP95,hiddenSecularP50)&&
		FireProductionCalibration::LongShadowNonsecular(hiddenSecularMaximum)&&
		FireProductionCalibration::LongShadowNonsecular(hiddenSecularP50)&&
		!FireProductionCalibration::LongShadowNonsecular(hiddenSecularP95),
		"motion-invariant distribution gate accepts an advected fixed pattern and p95 catches "
		"secular redistribution hidden beneath a flat maximum");
#if defined(__APPLE__)
	Check(!RISE::FireProductionDualLayoutPackRequiresSerialOwner(false,false)&&
		RISE::FireProductionDualLayoutPackRequiresSerialOwner(true,false)&&
		RISE::FireProductionDualLayoutPackRequiresSerialOwner(false,true)&&
		RISE::FireProductionDualLayoutPackRequiresSerialOwner(true,true),
		"dual-layout packing routes around recursive low-priority pool waits");
#endif
	Check(std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionAdvectionSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionProjectionSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionTransportSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionForceSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::Generator)==64u,
		"fp64 mirror carries source and generator SHA-256 identities");
	Check(std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionAdvectionSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionProjectionSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionTransportSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionForceSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::TraceAdapter)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::TraceCore)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::IndependentWalker)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::Generator)==64u,
		"roundoff trace carries source and generator SHA-256 identities");
	const std::string makeRules=ReadText("build/make/rise/Makefile");
	const std::string windowsRules=ReadText("build/cmake/rise-tests/CMakeLists.txt");
	const std::string unixTestDriver=ReadText("run_all_tests.sh");
	const std::string windowsTestDriver=ReadText("run_all_tests.ps1");
	const std::string walkerSource=ReadText("tests/FireProductionRoundoffWalker.h");
	const std::string traceCoreSource=ReadText("tests/FireProductionRoundoffTrace.h");
	const std::string traceAdapterSource=ReadText(
		"tests/FireProductionRoundoffTraceAdapter.h");
	const std::string tracedTransportSource=ReadText(
		"tests/fire_production_trace/FireProductionTransport.cpp");
	const std::string projectionSource=ReadText(
		"src/Library/Utilities/FireProductionProjection.cpp");
	const std::string projectionTestSource=ReadText(
		"tests/FireProductionProjectionTest.cpp");
	const std::string projectionHeader=ReadText(
		"src/Library/Utilities/FireProductionProjection.h");
	const std::string projectionMetal=ReadText(
		"src/Library/Utilities/FireProductionProjectionMac.mm");
	const std::string tracedProjectionSource=ReadText(
		"tests/fire_production_trace/FireProductionProjection.cpp");
	const std::string advectionSource=ReadText(
		"src/Library/Utilities/FireProductionAdvection.cpp");
	Check(CountText(projectionSource,
		"result.maximumPostProjectionResidualPerS=std::max(")==1u&&
		CountText(projectionSource,"std::fabs(residual)")==2u&&
		CountText(projectionHeader,"maximumPostProjectionResidualPerS(0.0f)")==1u&&
		CountText(tracedProjectionSource,
			"EvaluateNonnegativeReductionGuard(maximumResidualPerS)")==2u&&
		CountText(tracedProjectionSource,
			"EvaluateNonnegativeReductionGuard(maximumVelocityMPerS)")==1u,
		"projection guard proof is source-bound to +0 seed, abs/max reduction, and both consumers");
	Check(CountText(advectionSource,
		"const float q6=6.0f*center-3.0f*(left+right);")==2u&&
		CountText(advectionSource,"if( left==center&&right==center )")==2u&&
		CountText(advectionSource,
			"return delta*(left+0.5f*(right-left+q6)*(beginning+end)-")==1u&&
		CountText(advectionSource,
			"return length*(right-0.5f*(right-left-q6)*length-")==1u,
		"flat-integral proof is source-bound to both production polynomial graphs and shortcuts");
	const std::string roundoffStopEvidence=ReadText(
		"rendered/fire_production_calibration/r122_roundoff_derivation/"
		"roundoff_derivation_stop.v1");
	Check(!roundoffStopEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		roundoffStopEvidence.begin(),roundoffStopEvidence.end()))==
		"939e95f8ff916fff6c168d2b6bd30186b50a9b6e7963255b10641481c1b5d5ac",
		"r122 roundoff derivation refusal artifact is durable and byte-bound");
	const std::string branchStopEvidence=ReadText(
		"rendered/fire_production_calibration/r123_branch_obligations/"
		"branch_obligation_stop.v1");
	Check(!branchStopEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		branchStopEvidence.begin(),branchStopEvidence.end()))==
		"2743b6347b2c456530949d2d01df182efa5b1c637f5107ebfd6f794bd4cb3971",
		"r123 branch-obligation census and shared-limiter stop are durable and byte-bound");
	const std::string branchDischargeEvidence=ReadText(
		"rendered/fire_production_calibration/r124_branch_discharge/branch_discharge.v1");
	Check(!branchDischargeEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		branchDischargeEvidence.begin(),branchDischargeEvidence.end()))==
		"238195f1c197b6a5abdcdd4c4862f85d4213a803bb192adfd0c873a5938bee96"&&
		branchDischargeEvidence.find("executed_obligation_instances_pending 3688410")!=
			std::string::npos&&
		branchDischargeEvidence.find("reason independent_site_class_envelopes_not_yet_derived")!=
			std::string::npos,
		"r124 limiter admission and incomplete site-class census are durable and semantic-bound");
	const std::string floorEvidence=ReadText(
		"rendered/fire_production_calibration/r125_floor_partition/floor_partition.v1");
	Check(!floorEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		floorEvidence.begin(),floorEvidence.end()))==
		"c1c273ed66f2af22e5982435beb38600f2ad9481b0c5d57069cf1f48e277588c"&&
		floorEvidence.find("class_floor_pending 0")!=std::string::npos&&
		floorEvidence.find("executed_obligation_instances_pending 3315165")!=
			std::string::npos,
		"r125 floor derivation, zero class pending count, and cumulative census are durable");
	const std::string remainingEvidence=ReadText(
		"rendered/fire_production_calibration/r126_remaining_positive/remaining_positive.v1");
	Check(!remainingEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		remainingEvidence.begin(),remainingEvidence.end()))==
		"9ad1b1de6b35d290753b3c263d170b6e11bb31dfb8e1764c0029b794ad453f68"&&
		remainingEvidence.find("class_remaining_positive_pending 0")!=std::string::npos&&
		remainingEvidence.find("executed_obligation_instances_pending 1691997")!=
			std::string::npos,
		"r126 remaining-positive derivation and cumulative census are durable");
	const std::string inflowEvidence=ReadText(
		"rendered/fire_production_calibration/r127_inflow_transition/inflow_transition.v1");
	Check(!inflowEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		inflowEvidence.begin(),inflowEvidence.end()))==
		"5d95d4d062ae2dd8ec3533d7c802b0369582dcc2222fdc958bd8fde2ecfba198"&&
		inflowEvidence.find("removed_binary_inflow_obligation_instances 353664")!=
			std::string::npos&&
		inflowEvidence.find("power_of_two_width_factor 1")!=std::string::npos&&
		inflowEvidence.find("executed_obligation_instances_pending 1338333")!=
			std::string::npos,
		"r127 inflow reformulation, derived width, and cumulative census are durable");
	const std::string courantEvidence=ReadText(
		"rendered/fire_production_calibration/r128_courant_sign/courant_sign.v1");
	Check(!courantEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		courantEvidence.begin(),courantEvidence.end()))==
		"482b58d038b200bd1a31cb006be76ac99ba64dd3ffa16b6fd68bd68dc1925e7e"&&
		courantEvidence.find("class_courant_pending 0")!=std::string::npos&&
		courantEvidence.find("executed_obligation_instances_pending 992253")!=
			std::string::npos,
		"r128 Courant derivation, signed-zero rule, and cumulative census are durable");
	const std::string fractionEvidence=ReadText(
		"rendered/fire_production_calibration/r129_fractional_tail/fractional_tail.v1");
	Check(!fractionEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		fractionEvidence.begin(),fractionEvidence.end()))==
		"7ed9b0be19306d1448c197d669b206578109a600d13369948ec861a436c6b1db"&&
		fractionEvidence.find("class_fraction_pending 0")!=std::string::npos&&
		fractionEvidence.find("executed_obligation_instances_pending 620493")!=
			std::string::npos,
		"r129 fractional-tail derivation and cumulative census are durable");
	const std::string flatIntegralEvidence=ReadText(
		"rendered/fire_production_calibration/r130_flat_integral/flat_integral.v1");
	Check(!flatIntegralEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(flatIntegralEvidence.begin(),flatIntegralEvidence.end()))==
		"1468fed5cbab33d7f79282fde50b9a83a5d0bbc7b874ca5a8355ca7fd89db608"&&
		flatIntegralEvidence.find("class_flat_integral_pending 0")!=std::string::npos&&
		flatIntegralEvidence.find("executed_obligation_instances_pending 2")!=
			std::string::npos,
		"r130 flat-integral derivation and cumulative census are durable");
	const std::string reductionEvidence=ReadText(
		"rendered/fire_production_calibration/r131_projection_reduction/projection_reduction.v1");
	Check(!reductionEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(reductionEvidence.begin(),reductionEvidence.end()))==
		"d6cdacdbbcd02f9a1d6262553c3098a9abc1cb90001b012840581908bdf7c8a4"&&
		reductionEvidence.find("class_projection_reduction_pending 0")!=
			std::string::npos&&
		reductionEvidence.find("executed_obligation_instances_pending 0")!=
			std::string::npos&&reductionEvidence.find("canonical_exit 240")!=std::string::npos,
		"r131 projection-reduction proof and zero-pending census are durable");
	const std::string interpolationEvidence=ReadText(
		"rendered/fire_production_calibration/r132_projection_interpolation/"
		"projection_interpolation.v1");
	Check(!interpolationEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(interpolationEvidence.begin(),
			interpolationEvidence.end()))==
		"d8df1a96842b52ff053017014f177096a08e0591307a0d196b0bf14b31549198"&&
		interpolationEvidence.find("physical_interpolation_obligations 765")!=
			std::string::npos&&
		interpolationEvidence.find("restoration_interpolation_obligations 720")!=
			std::string::npos&&
		interpolationEvidence.find("physical_floor_branch_envelope 0")!=
			std::string::npos&&
		interpolationEvidence.find("canonical_exit 240")!=std::string::npos,
		"r132 fixed-grid interpolation proof removes the misapplied pressure envelope");
	const std::string bfp32RefusalEvidence=ReadText(
		"rendered/fire_production_calibration/r133_bfp32_projection_refusal/"
		"bfp32_projection_refusal.v1");
	Check(!bfp32RefusalEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(bfp32RefusalEvidence.begin(),
			bfp32RefusalEvidence.end()))==
		"1512191c5966ad3eb981b2a2e6e205f79d9666f26d3cbfb58a3094c47b5c853e"&&
		bfp32RefusalEvidence.find("executed_obligation_instances_pending 3")!=
			std::string::npos&&
		bfp32RefusalEvidence.find("metal_measurement_performed false")!=
			std::string::npos&&
		bfp32RefusalEvidence.find("canonical_exit 237")!=std::string::npos,
		"r133 nonfinite projection amplification refuses B_fp32 before measurement");
	const std::string aposterioriEvidence=ReadText(
		"rendered/fire_production_calibration/r134_projection_aposteriori/"
		"projection_aposteriori.v1");
	Check(!aposterioriEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(aposterioriEvidence.begin(),aposterioriEvidence.end()))==
		"18f0115216fb4654eb4b7a37466ded1788158d9e4b55eacf15811e986b651ec2"&&
		aposterioriEvidence.find("physical_projection_velocity_rms_upper "
			"0.00016499365420669603")!=std::string::npos&&
		aposterioriEvidence.find("restoration_projection_velocity_rms_upper "
			"0.00023357153961206769")!=std::string::npos&&
		aposterioriEvidence.find("executed_obligation_instances_pending 0")!=
			std::string::npos&&
		aposterioriEvidence.find("canonical_exit 241")!=std::string::npos,
		"r134 structural inverse and streaming envelopes close both projection terms");
	const std::string fullStepEvidence=ReadText(
		"rendered/fire_production_calibration/r135_full_step_bfp32/"
		"full_step_bfp32_derivation.v1");
	Check(!fullStepEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(fullStepEvidence.begin(),fullStepEvidence.end()))==
		"843e1f02bfbd7e1a7f5712b987aac128c7cf3ca52e0bdec0cccda371839f32c1"&&
		fullStepEvidence.find("final_velocity_rms_upper 0.021145425678289562")!=
			std::string::npos&&fullStepEvidence.find("derivation_before_metal true")!=
			std::string::npos&&fullStepEvidence.find("metal_measurement_performed false")!=
			std::string::npos&&fullStepEvidence.find("canonical_exit 242")!=std::string::npos,
		"r135 candidate is retained as the pre-review derivation proposal");
	const std::string fullStepRefusal=ReadText(
		"rendered/fire_production_calibration/r136_full_step_refusal/"
		"full_step_bfp32_refusal.v1");
	Check(!fullStepRefusal.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(fullStepRefusal.begin(),fullStepRefusal.end()))==
		"19732a3864fbcbe43a4fa872d4c4311732820248934bfc9bc1627b0ae0ff33ad"&&
		fullStepRefusal.find("fixture_sha256 "
			"b32cd74d7eab2ab0872bf04d25c54ac5279315380837be222bb36079f3f15674")!=
			std::string::npos&&
		fullStepRefusal.find("full_step_proof_gap_bitmap 0xff")!=std::string::npos&&
		fullStepRefusal.find("candidate_is_certified false")!=std::string::npos&&
		fullStepRefusal.find("metal_measurement_performed false")!=std::string::npos&&
		fullStepRefusal.find("canonical_exit 237")!=std::string::npos,
		"r136 source-binds the rejected candidate and refuses B_fp32 before Metal");
	const std::string subdominanceProtocol=ReadText(
		"rendered/fire_production_calibration/r137_subdominance_protocol/"
		"subdominance_protocol.v1");
	Check(!subdominanceProtocol.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(subdominanceProtocol.begin(),
			subdominanceProtocol.end()))==
		"833137b54fbd507fc3b6bdcc960a23b89be60ee835f7b1ca177f93cc57d63922"&&
		subdominanceProtocol.find("amendment_before_measurement true")!=
			std::string::npos&&
		subdominanceProtocol.find("measurement_performed false")!=std::string::npos&&
		subdominanceProtocol.find("subdominance_power_of_two 0.125")!=
			std::string::npos&&
		subdominanceProtocol.find("same_scheme_binary64_mirror_required true")!=
			std::string::npos&&
		subdominanceProtocol.find("oracle_forbidden_for_precision_test true")!=
			std::string::npos&&
		subdominanceProtocol.find("r136_proof_gap_bitmap 0xff")!=std::string::npos,
		"r137 pre-registers the subdominance amendment before any Metal measurement");
	const std::string subdominanceMeasurement=ReadText(
		"rendered/fire_production_calibration/r138_subdominance_measurement/"
		"subdominance_measurement.v1");
	const std::string dyadicFixture=ReadText("tests/FireProductionDyadicCalibrationFixture.h");
	const std::string subdominanceFixture=ReadText(
		"tests/FireProductionSubdominanceFixture.h");
	const std::string mirrorAdapter=ReadText("tests/FireProductionCalibrationMirror.h");
	const std::string advectionMetal=ReadText(
		"src/Library/Utilities/FireProductionAdvectionMac.mm");
	Check(!subdominanceMeasurement.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(subdominanceMeasurement.begin(),
			subdominanceMeasurement.end()))==
		"fbeed0d8a5dad9d8eabcea3236154cec702c6029376fa59456850d42feb7b003"&&
		subdominanceMeasurement.find("measurement_fixture_sha256 "
			"f727861af3b0e70bddca2a97b81c2e854bdf09d45c63c6c800e1846f994dd582")!=
			std::string::npos&&
		subdominanceMeasurement.find("mirror_adapter_sha256 "
			"bb541a79454136422ec19c8d2c091d85a09d9051756105b23746ddae6a731128")!=
			std::string::npos&&
		subdominanceMeasurement.find("fp64_source_manifest_sha256 "
			"22a352d220eaf035e2b94537d976545208dda1912a10ae21da9eac15c6fd0922")!=
			std::string::npos&&
		subdominanceMeasurement.find("metal_advection_sha256 "
			"0f04feacb11066bb6fcc168f26fe3881fd715e87235f757e6f96691a1703633c")!=
			std::string::npos&&
		subdominanceMeasurement.find("metal_force_sha256 "
			"165602c9a142c999ee38a2a4a7321e91204e0ba7ef7a0e6cfd5662dcf3229996")!=
			std::string::npos&&
		subdominanceMeasurement.find("metal_projection_sha256 "
			"2b3e518e7b69a2d3b0f2014fa42099f650d304212987d7d27e2941173494c331")!=
			std::string::npos&&
		subdominanceMeasurement.find("measurement_trace_sha256 "
			"f90a2508803f769665e68fc2c10e7672ea5f7ee5bf647fa2b8ff8b227551cebf")!=
			std::string::npos&&
		subdominanceMeasurement.find("verdict diagnostic_tier6_precision_subdominant_all_152_pilot_gates")!=
			std::string::npos&&
		subdominanceMeasurement.find("golden_slice_class_certified false")!=
			std::string::npos&&
		subdominanceMeasurement.find("full_step_B_fp32_closed false")!=
			std::string::npos&&
		subdominanceMeasurement.find("preliminary_velocity_guard_status "
			"pending_golden_slice_measurement")!=std::string::npos&&
		subdominanceMeasurement.find("measurement_exit 243")!=std::string::npos,
		"r138 byte-binds the tier-6 pilot without claiming golden-slice certification");
	const std::string goldenSubdominanceInputs=ReadText(
		"rendered/fire_production_calibration/r138_golden_subdominance_inputs/"
		"golden_subdominance_inputs.v1");
	const std::string goldenRestorationRefusal=ReadText(
		"rendered/fire_production_calibration/r140_golden_restoration_refusal/"
		"golden_restoration_refusal.v1");
	const std::string goldenCompositionFixture=ReadText(
		"tests/FireProductionGoldenCompositionFixture.h");
	const std::string fireSimulator3DAdvance=ReadText(
		"tools/fire_simulator_3d_advance.h");
	const std::string timestepVelocityBenchmarkOptions=ReadText(
		"rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/"
		"benchmark.options");
	Check(!goldenSubdominanceInputs.empty()&&!goldenRestorationRefusal.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			goldenSubdominanceInputs.begin(),goldenSubdominanceInputs.end()))==
		"e7b37ed66ad02b942fe3db966d90d0d141438cb91dd0dc420c885ea1da6abc66"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			goldenRestorationRefusal.begin(),goldenRestorationRefusal.end()))==
		"ff15255af16cd650606bda2cae1c8cf3f422b1b24211b6c9b55f621ec5aba99a"&&
		goldenSubdominanceInputs.find("measurement_performed false")!=std::string::npos&&
		goldenSubdominanceInputs.find("slice_restart_policy shared_golden_beginning_per_slice")!=
			std::string::npos&&
		goldenRestorationRefusal.find("restoration_post_over_band 9.3318769992623096")!=
			std::string::npos&&
		goldenRestorationRefusal.find("restoration_cycle_count 16")!=std::string::npos&&
		goldenRestorationRefusal.find("precision_measurement_started false")!=
			std::string::npos&&
		goldenRestorationRefusal.find("canonical_exit 244")!=std::string::npos&&
		goldenRestorationRefusal.find("full_step_B_fp32_closed false")!=std::string::npos,
		"r140 binds the sealed golden inputs and fails closed on restoration validation");
	const std::string restorationPlateauProtocol=ReadText(
		"rendered/fire_production_calibration/r141_restoration_plateau_protocol/"
		"restoration_plateau_protocol.v1");
	Check(!restorationPlateauProtocol.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(restorationPlateauProtocol.begin(),
			restorationPlateauProtocol.end()))==
		"24efa0dfcdbf52cdef5561fec87af2aae3266383f609ede39f18729c8e70a599"&&
		restorationPlateauProtocol.find("protocol_before_measurement true")!=
			std::string::npos&&
		restorationPlateauProtocol.find("measurement_performed false")!=
			std::string::npos&&
		restorationPlateauProtocol.find("headroom_power_of_two 0.25")!=
			std::string::npos&&
		restorationPlateauProtocol.find(
			"required_drain_rule r_req=G_field/(eos_ceiling*(1-headroom))")!=
			std::string::npos&&
		restorationPlateauProtocol.find("field_plateau_gate <=0.00075")!=
			std::string::npos&&
		restorationPlateauProtocol.find("production_validation_changed false")!=
			std::string::npos,
		"r141 freezes plateau-derived restoration validation before measurement");
	const std::string restorationCapacityEvidence=ReadText(
		"rendered/fire_production_calibration/r142_burning_plateau_capacity/"
		"restoration_capacity_evidence.v1");
	Check(!restorationCapacityEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(restorationCapacityEvidence.begin(),
			restorationCapacityEvidence.end()))==
		"76fbabda66b63c2a3d732b2a17936d402e5206f4f6b7d357de205e949517fce7"&&
		restorationCapacityEvidence.find("advection_metal_sha256 "
			"0f04feacb11066bb6fcc168f26fe3881fd715e87235f757e6f96691a1703633c")!=
			std::string::npos&&
		restorationCapacityEvidence.find("projection_metal_sha256 "
			"2b3e518e7b69a2d3b0f2014fa42099f650d304212987d7d27e2941173494c331")!=
			std::string::npos&&
		restorationCapacityEvidence.find("golden_fixture_sha256 "
			"cef39b56205f61fc9b589fc69551234000ad487b5c963a7361714dccd5d0b2f8")!=
			std::string::npos&&
		restorationCapacityEvidence.find("solver_test_sha256 "
			"5953171b9ea96481a23b852f4a79db4dd9e91944f9851c48abe494248ecc575a")!=
			std::string::npos&&
		restorationCapacityEvidence.find(
			"burning_G_field 0.0025328069638265172")!=std::string::npos&&
		restorationCapacityEvidence.find(
			"cold_G_field 0.00012031080315666465")!=std::string::npos&&
		restorationCapacityEvidence.find(
			"burning_required_drain 3.3770759517686897")!=std::string::npos&&
		restorationCapacityEvidence.find(
			"burning_delivered_drain_16 0.9533406144549903")!=std::string::npos&&
		restorationCapacityEvidence.find("burning_physical_cycles 17")!=
			std::string::npos&&
		restorationCapacityEvidence.find("cold_physical_cycles 17")!=
			std::string::npos&&
		restorationCapacityEvidence.find("burning_physical_validation_passed true")!=
			std::string::npos&&
		restorationCapacityEvidence.find("cold_physical_validation_passed true")!=
			std::string::npos&&
		restorationCapacityEvidence.find(
			"retained_r138_replay_after_instrumentation exact_exit_243")!=
			std::string::npos&&
		restorationCapacityEvidence.find("no_validation_band_admitted true")!=
			std::string::npos&&
		restorationCapacityEvidence.find("no_cycle_count_can_satisfy true")!=
			std::string::npos&&
		restorationCapacityEvidence.find("canonical_exit 253")!=std::string::npos,
		"r142 byte-binds the burning capacity stop and admits no replacement band");
	const std::string manifoldTimeStepProtocol=ReadText(
		"rendered/fire_production_calibration/r143_manifold_timestep_protocol/"
		"manifold_timestep_protocol.v1");
	Check(!manifoldTimeStepProtocol.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(manifoldTimeStepProtocol.begin(),
			manifoldTimeStepProtocol.end()))==
		"ba03dbda86733b6e0248204ae539ecce8002ac7ecbcdaaa344dd997721e5b5f0"&&
		manifoldTimeStepProtocol.find("protocol_before_measurement true")!=
			std::string::npos&&
		manifoldTimeStepProtocol.find("measurement_performed false")!=
			std::string::npos&&
		manifoldTimeStepProtocol.find(
			"manifold_limit_rule dt_prev*((1-headroom)*eos_ceiling*r_prev)/G_prev")!=
			std::string::npos&&
		manifoldTimeStepProtocol.find(
			"first_step_rule cfl_buoyant_diffusive_limits_only_without_prior_manifold_metadata")!=
			std::string::npos&&
		manifoldTimeStepProtocol.find(
			"realized_plateau_rule max_cell_abs(V(Q_accepted)-1)<=plateau_allowance")!=
			std::string::npos&&
		manifoldTimeStepProtocol.find("prediction_is_not_evidence true")!=
			std::string::npos&&
		manifoldTimeStepProtocol.find("production_changed false")!=std::string::npos,
		"r143 freezes the manifold timestep remedy before production evidence");
	const std::string manifoldPredictorEvidence=ReadText(
		"rendered/fire_production_calibration/r144_manifold_predictor_stop/"
		"manifold_predictor_evidence.v1");
	const std::string forceSource=ReadText("src/Library/Utilities/FireProductionForce.cpp");
	const std::string forceHeader=ReadText("src/Library/Utilities/FireProductionForce.h");
	const std::string forceMetal=ReadText("src/Library/Utilities/FireProductionForceMac.mm");
	const std::string forceUnsupported=ReadText(
		"src/Library/Utilities/FireProductionForceUnsupported.cpp");
	const std::string transportHeader=ReadText(
		"src/Library/Utilities/FireProductionTransport.h");
	Check(!manifoldPredictorEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(manifoldPredictorEvidence.begin(),
			manifoldPredictorEvidence.end()))==
		"93409b4ddc5ba2f064548742901b4ce9c3d5f6aa2ad1ddb1120cb0b308ace2b8"&&
		manifoldPredictorEvidence.find("advection_metal_sha256 "
			"e525d4d62dc9c7aac6b2361c8e1c3da47241adffa796a6d68dcfaba7fe846009")!=
			std::string::npos&&
		manifoldPredictorEvidence.find("force_cpp_sha256 "
			"cc854cbe44ba0bd165d47fa8b2db1f4c7461838ffbfe1b49464c3f0ab2229a1a")!=
			std::string::npos&&
		manifoldPredictorEvidence.find("records_cpp_sha256 "
			"531d538167dc617362ecca21042d70baff931d2a47ce2241fd183c4bb9bc328b")!=
			std::string::npos&&
		manifoldPredictorEvidence.find("golden_fixture_sha256 "
			"b55a105e2000d1aa8e9bacbb90c6b0dc15f8a09ea8bcc5581aac163f7e62c55e")!=
			std::string::npos&&
		manifoldPredictorEvidence.find("canonical_exit 254")!=std::string::npos&&
		manifoldPredictorEvidence.find("normal_path_atomic_rejection true")!=
			std::string::npos&&
		manifoldPredictorEvidence.find("realized_field_over_allowance 3.3442167686406066")!=
			std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r144_manifold_predictor_stop")!=
			std::string::npos&&unixTestDriver.find("PASS (exact exit=254)")!=
			std::string::npos&&unixTestDriver.find("expected 254")!=std::string::npos,
		"r144 binds the exact burning predictor miss and atomic fail-closed publication");
	const std::string priorManifoldClosure=ReadText(
		"rendered/fire_production_calibration/r147_producer_authority_lifecycle/"
		"producer_authority_lifecycle.v1");
	const std::string manifoldClosure=ReadText(
		"rendered/fire_production_calibration/r148_checkpoint_authority_closure/"
		"checkpoint_authority_closure.v1");
	const std::string productionSolverTest=ReadText("tests/FireProductionSolverTest.cpp");
	const std::string sequenceTest=ReadText("tests/FireSequenceTest.cpp");
	const std::string fileEncoderObserverSource=ReadText(
		"src/Library/Rendering/FileEncoderObserver.cpp");
	const std::string fileEncoderObserverHeader=ReadText(
		"src/Library/Rendering/FileEncoderObserver.h");
	const std::string fileRasterizerOutputShimTest=ReadText(
		"tests/FileRasterizerOutputShimTest.cpp");
	const std::string fireCaseSource=ReadText("src/Library/Utilities/FireCase.cpp");
	const std::string fireCaseHeader=ReadText("src/Library/Utilities/FireCase.h");
	const std::string simulationSolverTest=ReadText("tests/FireSimulationSolverTest.cpp");
	const std::string simulationCore=ReadText("tools/fire_simulator_core.h");
	const std::string fp64SourceManifest=ReadText(
		"tests/fire_production_fp64/SourceManifest.h");
	const std::string traceSourceManifest=ReadText(
		"tests/fire_production_trace/SourceManifest.h");
	const std::string fp64ForceHeader=ReadText(
		"tests/fire_production_fp64/FireProductionForce.h");
	const std::string traceForceHeader=ReadText(
		"tests/fire_production_trace/FireProductionForce.h");
	const std::string fp64Generator=ReadText(
		"tools/generate_fire_production_fp64_mirror.py");
	const std::string traceGenerator=ReadText(
		"tools/generate_fire_production_roundoff_trace.py");
	Check(!manifoldClosure.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(manifoldClosure.begin(),manifoldClosure.end()))==
		"f4ae4d1416dd9d478c6b2eb1170de211b9e09f48a17ad842baae9c89a10eefc4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			priorManifoldClosure.begin(),priorManifoldClosure.end()))==
		"c095c05a6fe2e92c58744a9de589f6600e8249a5824537ee4392e1ff0ade21fb"&&
		manifoldClosure.find("retired_v11_tuple_seal_is_not_authority true")!=
			std::string::npos&&
		manifoldClosure.find("public_raw_tuple_restoration_api_absent true")!=
			std::string::npos&&
		manifoldClosure.find("producer_issued_accepted_state_digest true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_state_transplant_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("actual_metal_physical_validation_miss_tokenless true")!=
			std::string::npos&&
		manifoldClosure.find("checkpoint_payload_binding_is_integrity_not_secret_authentication true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_history_without_observation_v13_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("legacy_binary32_v9_v10_v11_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("unaccepted_binary32_checkpoint_forbidden true")!=
			std::string::npos&&
		manifoldClosure.find("coordinated_accepted_metadata_clear_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("coordinated_accepted_metadata_clear_owner_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("coordinated_accepted_metadata_clear_v13_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("first_step_owner_requires_binary64 true")!=
			std::string::npos&&
		manifoldClosure.find("legacy_binary32_v9_v10_v11_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("legacy_binary32_zero_count_v9_v10_v11_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("legacy_binary32_all_zero_v9_v10_v11_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("legacy_binary64_all_zero_v9_v10_v11_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("legacy_binary64_all_zero_v9_v10_v11_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("zero_step_checkpoint_forbidden_all_precisions true")!=
			std::string::npos&&
		manifoldClosure.find("first_step_owner_requires_canonical_analytic_state true")!=
			std::string::npos&&
		manifoldClosure.find("retagged_coordinated_clear_owner_writer_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_temperature_transplant_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_selector_state_transplant_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("production_selector_recomputes_current_state_digest true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_observation_clear_owner_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_history_non_tail_transplant_writer_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_simulation_time_transplant_writer_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_history_exactly_reconstructs_simulation_time true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_timeline_predicate_shared_owner_writer_loader true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_binary32_promotion_v5_through_v13_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("binary64_checkpoint_origin_authority_opaque_payload_bound true")!=
			std::string::npos&&
		manifoldClosure.find("binary64_origin_digest_is_canonical_complete_checkpoint_prefix true")!=
			std::string::npos&&
		manifoldClosure.find("binary64_origin_frame_integral_duration_mutants_writer_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("binary64_current_v13_writer_loader_authority_required true")!=
			std::string::npos&&
		manifoldClosure.find("binary64_modern_v9_through_v12_resume_rejected_without_origin true")!=
			std::string::npos&&
		manifoldClosure.find("intact_accepted_binary64_retag_owner_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_last_step_alias_owner_writer_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("binary64_checkpoint_state_revalidated_in_binary64_envelope true")!=
			std::string::npos&&
		manifoldClosure.find("accepted_history_count_mismatch_v13_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("r148_lifecycle_exact_exit 255")!=std::string::npos&&
		manifoldClosure.find("r142_retained_exact_exit 253")!=std::string::npos&&
		manifoldClosure.find("r144_retained_exact_exit 254")!=std::string::npos,
		"r147 makes accepted-observation authority producer-owned and executes checkpoint resume");
	const std::string manifoldStageBudgetProtocol=ReadText(
		"rendered/fire_production_calibration/r149_manifold_stage_budget_protocol/"
		"manifold_stage_budget_protocol.v1");
	Check(!manifoldStageBudgetProtocol.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(manifoldStageBudgetProtocol.begin(),
			manifoldStageBudgetProtocol.end()))==
		"7b460fd1c09a2a81cd87c4fc850f4a998c303dd91fb8244bd0c84662f7d90d61"&&
		manifoldStageBudgetProtocol.find("protocol_before_measurement true")!=
			std::string::npos&&
		manifoldStageBudgetProtocol.find("measurement_performed false")!=
			std::string::npos&&
		manifoldStageBudgetProtocol.find(
			"time_step_sweep CFL CFL_over_2 CFL_over_4")!=std::string::npos&&
		manifoldStageBudgetProtocol.find(
			"stage_order remap_advection physical_projection restoration_projection")!=
			std::string::npos&&
		manifoldStageBudgetProtocol.find(
			"decision_rule_remap remap_dominant_and_exponent_near_zero_implies_manifold_consistent_reconstruction")!=
			std::string::npos&&
		manifoldStageBudgetProtocol.find(
			"decision_rule_restoration restoration_self_generation_dominant_implies_anomaly_aware_predictor_corrector_target")!=
			std::string::npos&&
		manifoldStageBudgetProtocol.find("production_changed false")!=std::string::npos,
		"r149 freezes the on-device stage-budget campaign and automatic remedy before evidence");
	const std::string manifoldStageBudgetEvidence=ReadText(
		"rendered/fire_production_calibration/r150_manifold_stage_budget/"
		"manifold_stage_budget_evidence.v1");
	Check(!manifoldStageBudgetEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(manifoldStageBudgetEvidence.begin(),
			manifoldStageBudgetEvidence.end()))==
		"eade8e7ab31a71bfb51f75445b03ea5f25822c8b715ba82cb05521d78a7031de"&&
		manifoldStageBudgetEvidence.find("canonical_exit 245")!=std::string::npos&&
		manifoldStageBudgetEvidence.find(
			"baseline_remap_G_level_0 0.0025327801704406738")!=std::string::npos&&
		manifoldStageBudgetEvidence.find(
			"reconstruction_trial shared_alpha_ten_tuple_rhoT_plus_original_conservative_components_rebuild_energy")!=
			std::string::npos,
		"r150 historical stage budget and superseded rho*T diagnostic remain byte-bound");
	const std::string acceptedMapReconstructionEvidence=ReadText(
		"rendered/fire_production_calibration/r151_accepted_map_reconstruction_stop/"
		"accepted_map_reconstruction_evidence.v1");
	const std::string fireSimulatorCore=ReadText("tools/fire_simulator_core.h");
	const std::string fireProductionSourceKernel=ReadText(
		"src/Library/Utilities/FireProductionSourceKernel.h");
	const std::string fireSimulationRecords=ReadText(
		"src/Library/Utilities/FireSimulationRecords.cpp");
	Check(!acceptedMapReconstructionEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(acceptedMapReconstructionEvidence.begin(),
			acceptedMapReconstructionEvidence.end()))==
		"ca28527b85a4da372fdce7776c0d6f1b0d7cfa29c945af94fb2146f2827cc0dc"&&
		acceptedMapReconstructionEvidence.find(
			"parent_r150_evidence_sha256 eade8e7ab31a71bfb51f75445b03ea5f25822c8b715ba82cb05521d78a7031de")!=
			std::string::npos&&
		acceptedMapReconstructionEvidence.find(
			"retired_r150_trial rho_total_times_temperature_is_not_manifold_consistent_when_composition_changes")!=
			std::string::npos,
		"r151 historical post-remap repair diagnostic remains byte-bound");
	const std::string conservativeFaceReconstructionEvidence=ReadText(
		"rendered/fire_production_calibration/r152_conservative_face_reconstruction_stop/"
		"conservative_face_reconstruction_evidence.v1");
	const std::string productionTransportSource=ReadText(
		"src/Library/Utilities/FireProductionTransport.cpp");
	Check(!conservativeFaceReconstructionEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(conservativeFaceReconstructionEvidence.begin(),
			conservativeFaceReconstructionEvidence.end()))==
		"87c1578ba051c022c9b21f961413c28be76cd4ba8de5cf48c5b898ebec7bc1ad"&&
		conservativeFaceReconstructionEvidence.find(
			"parent_r151_evidence_sha256 ca28527b85a4da372fdce7776c0d6f1b0d7cfa29c945af94fb2146f2827cc0dc")!=
			std::string::npos&&
		conservativeFaceReconstructionEvidence.find(
			"reconstruction_trial conservative_EOS_projected_face_flux")!=std::string::npos&&
		conservativeFaceReconstructionEvidence.find(
			"post_remap_cell_energy_repair false")!=std::string::npos&&
		conservativeFaceReconstructionEvidence.find(
			"energy_inventory_ledger face_flux_conservative")!=std::string::npos&&
		conservativeFaceReconstructionEvidence.find(
			"conservative_reconstruction_G_level_0 0.0025328069638265172")!=
			std::string::npos&&
		conservativeFaceReconstructionEvidence.find(
			"conservative_reconstruction_G_over_allowance 3.3770759517686897")!=
			std::string::npos&&
		conservativeFaceReconstructionEvidence.find(
			"contract_level_production_ceiling_ruling_required true")!=std::string::npos,
		"r152 historical pass-evolved auxiliary evidence remains byte-bound");
	const std::string fixedPressureReconstructionEvidence=ReadText(
		"rendered/fire_production_calibration/r153_fixed_pressure_reconstruction_stop/"
		"fixed_pressure_reconstruction_evidence.v1");
	Check(!fixedPressureReconstructionEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(fixedPressureReconstructionEvidence.begin(),
			fixedPressureReconstructionEvidence.end()))==
		"dd62f9224853e5756e8d74b64a996a2b6762edccd6bf05af7e46010fd33098ba"&&
		fixedPressureReconstructionEvidence.find(
			"parent_r152_evidence_sha256 87c1578ba051c022c9b21f961413c28be76cd4ba8de5cf48c5b898ebec7bc1ad")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"retired_r152_inference pass_evolved_nT_is_not_fixed_pressure_after_first_directional_sweep")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"maximum_endpoint_projection_K_level_0 0.35360660028368329")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"endpoint_projection_count_level_0 2307844")!=std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"maximum_energy_ledger_relative_level_2 1.4020231210267571e-10")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"reconstruction_field_sha256_level_0 f750477c4aea40fa2e29fc37b636d3b8d9c2e87468d5db34930a611da6ae6991")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"production_field_sha256_level_0 0d00decc071ff85108435d59f8118a1ab2daba9e2b9f5c8c7cc6a77344383b8c")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"limiter_energy_trace_sha256_level_0 f09eebb2f0f329699928d9d1c5647b367f909dfb557301b1d44c7a073694cd90")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"thermochemistry_domain_action diagnostic_endpoint_projection_only_not_admitted_production_remedy")!=
			std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"contract_level_production_ceiling_ruling_required true")!=std::string::npos&&
		fixedPressureReconstructionEvidence.find(
			"favorable_endpoint_projected_reduces_G false")!=std::string::npos,
		"r153 historical endpoint-obligation evidence remains byte-bound");
	const std::string lowOrderCapacityEvidence=ReadText(
		"rendered/fire_production_calibration/r154_low_order_manifold_capacity_stop/"
		"low_order_manifold_capacity_evidence.v1");
	Check(!lowOrderCapacityEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(lowOrderCapacityEvidence.begin(),
			lowOrderCapacityEvidence.end()))==
		"4de73bbd55b406bcc050f3be949f7ca6eabfa522cc393243e09e988be540e02f"&&
		lowOrderCapacityEvidence.find(
			"parent_r153_evidence_sha256 dd62f9224853e5756e8d74b64a996a2b6762edccd6bf05af7e46010fd33098ba")!=
			std::string::npos&&
		lowOrderCapacityEvidence.find(
			"shared_alpha_domain_cap maximal_binary32_fraction_not_exceeding_exact_temperature_domain_crossing")!=
			std::string::npos&&
		lowOrderCapacityEvidence.find(
			"low_order_lower_infeasible_count_level_0 30381")!=std::string::npos&&
		lowOrderCapacityEvidence.find(
			"low_order_lower_infeasible_count_level_2 130")!=std::string::npos&&
		lowOrderCapacityEvidence.find(
			"low_order_upper_infeasible_count_all_levels 0")!=std::string::npos&&
		lowOrderCapacityEvidence.find(
			"maximum_low_order_lower_excursion_K_level_0 0.3536001375753699")!=
			std::string::npos&&
		lowOrderCapacityEvidence.find(
			"reconstruction_field_sha256_level_0 77ea23b2d9b390dc50ce0f873edc7bea9af62db9def7ae127fd40e7a6169ac04")!=
			std::string::npos&&
		lowOrderCapacityEvidence.find(
			"alpha_zero_nonempty_interval_after_first_pass false")!=std::string::npos&&
		lowOrderCapacityEvidence.find(
			"contract_level_production_ceiling_ruling_required true")!=std::string::npos&&
		lowOrderCapacityEvidence.find(
			"golden_fixture_sha256 8e46164ef0c817ba50860767b94888905299a67cdc5a352b42483f9a1b825d58")!=
			std::string::npos,
		"r154 binds the maximal shared-alpha campaign and composed low-order stop");
	const std::string allLowOrderEvidence=ReadText(
		"rendered/fire_production_calibration/r155_all_low_order_reconstruction_stop/"
		"all_low_order_reconstruction_evidence.v1");
	Check(!allLowOrderEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(allLowOrderEvidence.begin(),allLowOrderEvidence.end()))==
		"e5a2aa56970e1c94137649ade05dcf3f33eff682c442edb12e9c3f8a9d471ea7"&&
		allLowOrderEvidence.find(
			"parent_r154_evidence_sha256 4de73bbd55b406bcc050f3be949f7ca6eabfa522cc393243e09e988be540e02f")!=
			std::string::npos&&
		allLowOrderEvidence.find(
			"r60_endpoint_width_formula kappa32_times_epsilon32_times_AcceptedStateEnergyScale_divided_by_MixtureCertifiedCpLower")!=
			std::string::npos&&
		allLowOrderEvidence.find(
			"all_five_passes_shared_alpha_exact_zero true")!=std::string::npos&&
		allLowOrderEvidence.find(
			"all_low_order_G_level_0 0.0025328069638265172")!=std::string::npos&&
		allLowOrderEvidence.find(
			"outside_r60_endpoint_envelope_count_all_levels 0")!=std::string::npos&&
		allLowOrderEvidence.find(
			"maximum_r60_endpoint_width_K_level_0 0.71929210099316709")!=
			std::string::npos&&
		allLowOrderEvidence.find(
			"maximum_endpoint_excursion_K_level_0 0.3536001375753699")!=
			std::string::npos&&
		allLowOrderEvidence.find(
			"all_low_order_field_sha256_level_0 d4df6114047f27a79bc807ac68ed69d946dfe20a755ae934a91bc5dd013e470b")!=
			std::string::npos&&
		allLowOrderEvidence.find(
			"all_low_order_G_over_allowance 3.3770759517686897")!=std::string::npos&&
		allLowOrderEvidence.find(
			"contract_level_production_ceiling_ruling_required true")!=std::string::npos&&
		allLowOrderEvidence.find(
			"golden_fixture_sha256 00fe09d2d1687e83aa34f263f7e5449ca00fbe5de27d98ecec650a46613a27d2")!=
			std::string::npos,
		"r155 historical Cp-lower campaign remains byte-bound");
	const std::string coupledAlphaEvidence=ReadText(
		"rendered/fire_production_calibration/r156_coupled_alpha_thermochemistry_stop/"
		"coupled_alpha_thermochemistry_evidence.v1");
	Check(!coupledAlphaEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(coupledAlphaEvidence.begin(),
			coupledAlphaEvidence.end()))==
		"f894abb9ed0f350b65c86bdfeb1a90762d6b1c89e4d7f56bdf09f8dbec3a8999"&&
		coupledAlphaEvidence.find(
			"parent_r155_evidence_sha256 e5a2aa56970e1c94137649ade05dcf3f33eff682c442edb12e9c3f8a9d471ea7")!=
			std::string::npos&&
		coupledAlphaEvidence.find("endpoint_inside_proof sufficient_Cp_upper")!=
			std::string::npos&&
		coupledAlphaEvidence.find("first_outside_pass_level_0 1")!=std::string::npos&&
		coupledAlphaEvidence.find("first_witness_level_0_line 29")!=std::string::npos&&
		coupledAlphaEvidence.find(
			"first_witness_level_0_all_zero_donor_sha256 2ac03ae9d4937f86861cc0a5e8c5620c6a166dd189c7a3f35d6d266fcc0346d3")!=
			std::string::npos&&
		coupledAlphaEvidence.find(
			"first_witness_level_0_prior_pass_molar_minimum_KMol_per_m3 0.040632479709723168")!=
			std::string::npos&&
		coupledAlphaEvidence.find(
			"first_witness_level_0_thermochemistry_molar_maximum_KMol_per_m3 0.040621987915680717")!=
			std::string::npos&&
		coupledAlphaEvidence.find("first_witness_level_0_coupled_alpha_feasible false")!=
			std::string::npos&&
		coupledAlphaEvidence.find(
			"endpoint_projected_counterfactual_G_over_allowance 3.3770759517686897")!=
			std::string::npos&&
		coupledAlphaEvidence.find("endpoint_projected_counterfactual_admitted false")!=
			std::string::npos,
		"r156 historical Cp-upper inference remains byte-bound");
	const std::string r60ReconstructionEvidence=ReadText(
		"rendered/fire_production_calibration/r157_r60_reconstruction_capacity_stop/"
		"r60_reconstruction_capacity_evidence.v1");
	Check(!r60ReconstructionEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(r60ReconstructionEvidence.begin(),
			r60ReconstructionEvidence.end()))==
		"41911ceb0e5a6f899fcf1e3e48d9a3ca04fb6adf1876ae3595b3c1516eb841ad"&&
		r60ReconstructionEvidence.find("r156_Cp_upper_converse_rejected true")!=
			std::string::npos&&
		r60ReconstructionEvidence.find(
			"r60_endpoint_policy precision_envelope_completion_not_exact_out_of_domain_thermochemistry")!=
			std::string::npos&&
		r60ReconstructionEvidence.find(
			"reconstruction_validated_faces_level_0 4926768")!=std::string::npos&&
		r60ReconstructionEvidence.find(
			"reconstruction_validated_pass_cells_level_0 4881360")!=std::string::npos&&
		r60ReconstructionEvidence.find(
			"reconstruction_G_over_allowance 3.3770759517686897")!=std::string::npos&&
		r60ReconstructionEvidence.find(
			"verdict r60_admissible_reconstruction_remedy_fails_plateau_contract_level_ceiling_or_thermochemistry_ruling_required")!=
			std::string::npos,
		"r157 historical pre-round/global-alpha evidence remains byte-bound");
	const std::string producerRoundedReconstructionEvidence=ReadText(
		"rendered/fire_production_calibration/r158_producer_rounded_reconstruction_stop/"
		"producer_rounded_reconstruction_evidence.v1");
	Check(!producerRoundedReconstructionEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(producerRoundedReconstructionEvidence.begin(),
			producerRoundedReconstructionEvidence.end()))==
		"67d5660b9b3457a981aea7099cddcdd28a3dc144401b07ce16b6894e86ae8c1b"&&
		producerRoundedReconstructionEvidence.find(
			"r157_global_alpha_thermochemistry_inference_retired true")!=std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"r157_pre_round_face_validation_rejected true")!=std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"producer_rounded_face_energy_validated true")!=std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"below_domain_api_diagnostic_load_bearing_for_capacity false")!=
			std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"global_alpha_thermochemistry_exclusion_claimed false")!=std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"reconstruction_validated_faces_level_0 4926768")!=std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"reconstruction_validated_pass_cells_level_0 4881360")!=std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"reconstruction_G_over_allowance 3.3770759517686897")!=std::string::npos&&
		producerRoundedReconstructionEvidence.find(
			"verdict producer_rounded_r60_admissible_reconstruction_fails_plateau_contract_level_ruling_required")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"domainError!=\"methane thermochemistry lookup is out of domain\"")!=
			std::string::npos&&
		goldenCompositionFixture.find("MANIFOLD_RECONSTRUCTION_FACE_REJECTED")!=
			std::string::npos&&
		goldenCompositionFixture.find("MANIFOLD_RECONSTRUCTION_PASS_REJECTED")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"faceState[MethaneMassStateDimension]=energyFlux[fluxBase]/sweptLength")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"reconstructionValidatedFaceCount[0]==4926768u")!=std::string::npos&&
		goldenCompositionFixture.find(
			"reconstructionValidatedPassCellCount[0]==4881360u")!=std::string::npos,
		"r158 binds the producer-rounded r60 reconstruction stop");
	const std::string timestepVelocityCeilingEvidence=ReadText(
		"rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/"
		"timestep_velocity_ceiling_evidence.v1");
	Check(!timestepVelocityCeilingEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(timestepVelocityCeilingEvidence.begin(),
			timestepVelocityCeilingEvidence.end()))==
		"3a67d2bcca299f4e6ca0224d6ae73c63d56c8c23a5e7df671b7fab09c8164bb6"&&
		timestepVelocityCeilingEvidence.find(
			"accepted_step_implied_velocity_m_per_s 217.37616398903009")!=
			std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"transport_velocity_max_m_per_s 7.4333348274230957")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"physical_projection_velocity_max_m_per_s 7.371121883392334")!=
			std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"final_velocity_max_m_per_s 7.371121883392334")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"restoration_correction_velocity_max_m_per_s 1.7818529158830643e-06")!=
			std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"audited_transport_CFL_step_s 0.0016462660045688639")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"audited_selector_maximum_positive_reduced_gravity_m_per_s2 "
			"48.944695265891369")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"audited_selector_maximum_active_diffusivity_m2_per_s "
			"0.0030345390611787094")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"audited_represented_step_s 0.0016462659696117043")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"audited_selected_force_substep_count 1")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"timed_result_reports_audited_represented_step true")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"serial_parallel_payload_digest_bit_identical true")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"serial_parallel_every_warmup_and_measured_execution_identical true")!=
			std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"legacy_low_priority_nested_pack_routes_serial true")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"oracle_two_class_zeno_fix_already_landed true")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"normal_restoration_projection_validation false")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"derived_production_manifold_ceiling 0.00075")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"calibrating_observed_plateau_over_derived_ceiling 3.3770759517686897")!=
			std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"controlled_serial_wall_p95_ms 202.659166")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"corrected_warm_wall_p95_ms 154.74187499999999")!=std::string::npos&&
		timestepVelocityCeilingEvidence.find(
			"ceiling_verdict thermo_temperature_inversion_domain_finding_stop")!=
			std::string::npos&&
		timestepVelocityBenchmarkOptions=="render_thread_reserve_count 0\n"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			timestepVelocityBenchmarkOptions.begin(),timestepVelocityBenchmarkOptions.end()))==
			"be63f6fcd99666a1d2c611f4d06e6f082e9b3b4223216334a0dea9b2e2684d05"&&
		timestepVelocityCeilingEvidence.find("production_advection_metal_sha256 ")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"selectorMaximum==217.37616398903009")!=std::string::npos&&
		goldenCompositionFixture.find(
			"const double final=measured.projection.velocityMPerS[axis][face]")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"measured.physicalProjection.maximumPostProjectionResidualPerS")!=
			std::string::npos&&
		goldenCompositionFixture.find("BuildOpenStageTransportEvaluations3D")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"auditedSelection.seconds==transportCFL")!=std::string::npos&&
		goldenCompositionFixture.find(
			"timeSelectedStep(\"serial\",true,serialWall,serialDevice)")!=std::string::npos&&
		goldenCompositionFixture.find(
			"timeSelectedStep(\"parallel\",false,auditedWall,auditedDevice)")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"else if(!sameResidentArithmetic(serialResident,auditedResident))")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"auditedResident.representedTimeStepS!=representedAuditedStep")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"serialParallelArithmeticIdentical")!=std::string::npos&&
		goldenCompositionFixture.find(
			"FireProductionAcceptedManifoldPayloadDigest(left)")!=std::string::npos&&
		goldenCompositionFixture.find(
			"FireProductionAcceptedManifoldPayloadDigest(right)")!=std::string::npos&&
		goldenCompositionFixture.find(
			"production timestep velocity audit activation is invalid")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"FireProductionResidentStepMetalCommandCommitCount()==beginningCommandCount")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"measured.residentProjectionInvocationCount==2u")!=std::string::npos&&
		advectionMetal.find("GlobalThreadPool().ParallelFor(9u")!=std::string::npos&&
		advectionMetal.find("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE")!=
			std::string::npos&&
		advectionMetal.find(
			"legacyLowPriority=GlobalOptions().ReadBool(")!=std::string::npos&&
		advectionMetal.find("FireProductionDualLayoutPackRequiresSerialOwner(")!=
			std::string::npos&&
		advectionMetal.find(
			"production timestep velocity audit activation is invalid")!=std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r159_timestep_velocity_audit")!=
			std::string::npos&&
		unixTestDriver.find("RISE_FIRE_TIMESTEP_VELOCITY_AUDIT=malformed")!=
			std::string::npos&&
		unixTestDriver.find("velocity_audit_options")!=std::string::npos&&
		unixTestDriver.find("RISE_OPTIONS_FILE=\"$velocity_audit_options\"")!=
			std::string::npos&&
		unixTestDriver.find("velocity_audit_malformed_rc\" -eq 222")!=
			std::string::npos&&
		unixTestDriver.find("velocity_audit_rc\" -eq 247")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=247)")!=std::string::npos&&
		unixTestDriver.find("expected 247")!=std::string::npos,
		"r159 historical velocity and host-reduction evidence remains byte-bound");
	const std::string lowMachRefusalEvidence=ReadText(
		"rendered/fire_production_calibration/r160_low_mach_audited_step_refusal/"
		"low_mach_audited_step_refusal.v1");
	const std::string calibrationMathSource=ReadText(
		"tests/FireProductionCalibrationMath.h");
	Check(!lowMachRefusalEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(lowMachRefusalEvidence.begin(),
			lowMachRefusalEvidence.end()))==
		"a354c61208e7543d168e56a9534d74b72834f3c7acf408542c23e6a5c5dfa608"&&
		lowMachRefusalEvidence.find(
			"production_pressure_deviation_domain_limit absent")!=std::string::npos&&
		lowMachRefusalEvidence.find(
			"low_mach_validity_ceiling_hex 0x1p-5")!=std::string::npos&&
		lowMachRefusalEvidence.find(
			"audited_step_generation 0.085895776748657227")!=std::string::npos&&
		lowMachRefusalEvidence.find(
			"audited_step_field_over_low_mach_ceiling 2.7486648559570312")!=
			std::string::npos&&
		lowMachRefusalEvidence.find("accepted_state_token_minted false")!=
			std::string::npos&&
		lowMachRefusalEvidence.find("long_shadow_started false")!=std::string::npos&&
		lowMachRefusalEvidence.find(
			"outlier_masked_secular_trend_RED true")!=std::string::npos&&
		lowMachRefusalEvidence.find(
			"subdominance_floor_readmission_risk pre_registered")!=std::string::npos&&
		lowMachRefusalEvidence.find(
			"golden_checkpoint_unchanged true")!=std::string::npos&&
		lowMachRefusalEvidence.find("production_advection_metal_sha256 ")!=
			std::string::npos&&
		fireSimulatorCore.find("equationOfStateResidual > 1.0e-3")!=
			std::string::npos&&
		CountText(forceSource,"1e-3")==1u&&
		projectionSource.find("1.0e-3")==std::string::npos&&
		advectionMetal.find("1.0e-3")==std::string::npos&&
		forceSource.find("ManifoldLowMachValidityCeiling=0x1p-5")!=
			std::string::npos&&
		projectionSource.find("result.requiredDrainFraction=0.0")!=
			std::string::npos&&
		projectionSource.find(
			"result.maximumPostResidualPerS=maximumPreProjectionResidualPerS")!=
			std::string::npos&&
		projectionTestSource.find(
			"r160 restoration mechanism straddles the exact non-amplification boundary")!=
			std::string::npos&&
		productionSolverTest.find(
			"r160 malformed long-shadow activation fails before Metal work")!=
			std::string::npos&&
		goldenCompositionFixture.find("GOLDEN_LONG_SHADOW_REFUSAL")!=
			std::string::npos&&
		goldenCompositionFixture.find("fieldMaximum>LowMachValidityCeiling")!=
			std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r160_golden_long_shadow_admission")!=
			std::string::npos&&
		unixTestDriver.find("long_shadow_malformed_rc\" -eq 249")!=
			std::string::npos&&
		unixTestDriver.find("long_shadow_rc\" -eq 252")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=252, low-Mach refusal)")!=
			std::string::npos,
		"r160 derives the production low-Mach gate and fails the audited step closed");
	const std::string anomalyClosureEvidence=ReadText(
		"rendered/fire_production_calibration/r161_advective_anomaly_closure_stop/"
		"advective_anomaly_closure_evidence.v1");
	Check(!anomalyClosureEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(anomalyClosureEvidence.begin(),
			anomalyClosureEvidence.end()))==
		"2d27aab1d6439b78d73fa4f5a1ff12d4e450e73d7f34f072d5d957f007326076"&&
		anomalyClosureEvidence.find("G_model floor_plus_dose_times_dt")!=
			std::string::npos&&
		anomalyClosureEvidence.find("r158_floor_fit 0.0024982685328926446")!=
			std::string::npos&&
		anomalyClosureEvidence.find("r160_unclosed_dose_branch_per_s 50.65858722417441")!=
			std::string::npos&&
		anomalyClosureEvidence.find("r161_active_CFL_corrected_G 0.066569089889526367")!=
			std::string::npos&&
		anomalyClosureEvidence.find("r161_active_CFL_wall_p95_ms 179.9965")!=
			std::string::npos&&
		anomalyClosureEvidence.find(
			"r161_active_CFL_tier10_wall_projection_hours 0.75927931301360085")!=
			std::string::npos&&
		anomalyClosureEvidence.find("limited_binary64_target_schedule_available false")!=
			std::string::npos&&
		anomalyClosureEvidence.find("plateau_allowance 0.0234375")!=
			std::string::npos&&
		anomalyClosureEvidence.find("limited_target_failure_last 1.44776")!=
			std::string::npos&&
		anomalyClosureEvidence.find(
			"limited_timing_not_run_binary64_target_schedule_unavailable true")!=
			std::string::npos&&
		anomalyClosureEvidence.find("active_CFL_certified_working_set_bytes 1919317208")!=
			std::string::npos&&
		anomalyClosureEvidence.find("active_CFL_actual_metal_allocation_bytes 1630052936")!=
			std::string::npos&&
		anomalyClosureEvidence.find("active_CFL_cell_submaps 10")!=std::string::npos&&
		anomalyClosureEvidence.find("active_CFL_dual_submaps 15")!=std::string::npos&&
		anomalyClosureEvidence.find("zero_anomaly_target_fold_bit_identity true")!=
			std::string::npos&&
		anomalyClosureEvidence.find("zero_anomaly_on_device_payload_bit_identity true")!=
			std::string::npos&&
		anomalyClosureEvidence.find("limited_physical_target_per_s "
			"binary64_oracle_Heun_rederived_at_represented_dt")!=std::string::npos&&
		anomalyClosureEvidence.find("closure_disabled_reproduces_r160_exact_252 true")!=
			std::string::npos&&
		anomalyClosureEvidence.find("r161_limited_stop_exact_exit 219")!=
			std::string::npos&&
		anomalyClosureEvidence.find("r136_retained_trace_digest "
			"aae5a86a249bc72b82b2aefe6219e3193a61a5a900528de17442541d830b2fab")!=
			std::string::npos&&
		anomalyClosureEvidence.find("long_shadow_started false")!=std::string::npos&&
		anomalyClosureEvidence.find("production_advection_metal_sha256 ")!=
			std::string::npos&&
		advectionMetal.find("fold_methane_advective_anomaly_target")!=
			std::string::npos&&
		advectionMetal.find(
			"restorationTarget[gid]+=(deviation.y-deviation.x)*inverseTimeStep")!=
			std::string::npos&&
		advectionMetal.find("maximumPredictedAdvectiveAnomalyFloat==0.0f")!=
			std::string::npos&&
		advectionMetal.find("ProjectFireProductionMetalRestorationResidentState")!=
			std::string::npos&&
		advectionMetal.find("correctorInput.conservativeValues=cellPrivate")!=
			std::string::npos&&
		advectionMetal.find(
			"correctorInput.frozenVelocityMPerS=restorationState.velocityMPerS")!=
			std::string::npos&&
		forceSource.find("ManifoldPlateauAllowance")!=std::string::npos&&
		forceSource.find("DeriveFireProductionAdvectiveAnomalyTarget")!=
			std::string::npos&&
		goldenCompositionFixture.find("production.cellSubmapCount==10u")!=
			std::string::npos&&
		goldenCompositionFixture.find("production.dualSubmapCount==15u")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"production.combinedActualMetalAllocationBytes==1630052936u")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"for(std::size_t sample=0u;sample<wall.size();++sample)")!=
			std::string::npos&&
		productionSolverTest.find(
			"r161 malformed closure activation fails before Metal work")!=
			std::string::npos&&
		productionSolverTest.find(
			"zero-anomaly closure, including an eight-pass diagnostic request, stops after one")!=
			std::string::npos,
		"r161 historical evidence and resident two-pass closure remain byte-bound");
	const std::string equalTimeEvidence=ReadText(
		"rendered/fire_production_calibration/r162_equal_time_composition_stop/"
		"equal_time_composition_evidence.v1");
	Check(!equalTimeEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(equalTimeEvidence.begin(),equalTimeEvidence.end()))==
		"a5e88f780cccd34682c95ea120b981bdb38ec32d157f3ad390a2a4d32e9defa7"&&
		equalTimeEvidence.find("protocol_change pre_registered_before_equal_time_measurement")!=
			std::string::npos&&
		equalTimeEvidence.find("reference_role oracle_flow_reference_not_production_step_operator")!=
			std::string::npos&&
		equalTimeEvidence.find("largest_tested_convergent_dt 7.244249718496576e-05")!=
			std::string::npos&&
		equalTimeEvidence.find("contraction_class discrete_active_set_cycling_not_smooth_noncontraction")!=
			std::string::npos&&
		equalTimeEvidence.find("contraction_trace_sha256 "
			"900a7acc56a0c9c51d07788753132d59269bdb591aee699960a3c62b448a051c")!=
			std::string::npos&&
		equalTimeEvidence.find("reference_schedule_sha256 "
			"e4472da794d084158ccb6a3c2c073e6afce943bfc075429628fa27fc527c97e0")!=
			std::string::npos&&
		equalTimeEvidence.find("terminal_published_target_sha256 "
			"d198eaaaebd5d8322ba7582456ccdb4fd83016a65120379e7cd1c3e1ae6351ec")!=
			std::string::npos&&
		equalTimeEvidence.find("actual_penultimate_target_substitution_RED true")!=
			std::string::npos&&
		equalTimeEvidence.find("limited_corrected_G 0.024358630180358887")!=
			std::string::npos&&
		equalTimeEvidence.find("headroom_met false")!=std::string::npos&&
		equalTimeEvidence.find("limited_tier10_wall_projection_hours 2.1066222640131049")!=
			std::string::npos&&
		equalTimeEvidence.find("two_hour_wall_rule_met false")!=std::string::npos&&
		equalTimeEvidence.find("long_shadow_started false")!=std::string::npos&&
		equalTimeEvidence.find("golden_fixture_sha256 "
			"de785abf887b57c30399eb504bcfcb6d8dbc2b1be22752f3d7c0420e0c3edb16")!=
			std::string::npos&&
		equalTimeEvidence.find("calibration_math_sha256 "
			"41d3723c03f2a55e179b3c777460e826a5f7a347726344a2585fb29aaddd94ec")!=
			std::string::npos&&
		equalTimeEvidence.find("binary64_advance_sha256 "
			"cd03e6596562103227c912222213e7459ab0f1701ba9a0034cac85fce1095fe1")!=
			std::string::npos&&
		equalTimeEvidence.find("unix_test_driver_sha256 "
			"4fb2cb8611210c94d0460977c119e839c7b933a268d8881d75b2ee2401d28e4c")!=
			std::string::npos,
		"r162 binds the contraction ceiling, equal-time schedule, and limited-step stop");
	const std::string predictiveInitialEvidence=ReadText(
		"rendered/fire_production_calibration/r163_predictive_initial_step_refusal/"
		"predictive_initial_step_evidence.v1");
	Check(!predictiveInitialEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(predictiveInitialEvidence.begin(),
			predictiveInitialEvidence.end()))==
		"f3fc61cf8cb6f02212a427dcecf862d4e049847e5281d46db2b9cc9e63c6c503"&&
		predictiveInitialEvidence.find("initial_predictor_formula "
			"dt0_equals_dt_audit_times_allowance_divided_by_G_audit")!=std::string::npos&&
		predictiveInitialEvidence.find("initial_selected_dt_binary32_promoted "
			"0.00055762444389984012")!=std::string::npos&&
		predictiveInitialEvidence.find("headroom_excess 0.000021100044250488281")!=
			std::string::npos&&
		predictiveInitialEvidence.find("accepted_state_token_minted false")!=
			std::string::npos&&
		predictiveInitialEvidence.find("host_residual_campaign_started false")!=
			std::string::npos&&
		predictiveInitialEvidence.find("predictive_refusal_exact_exit 215")!=
			std::string::npos&&
		predictiveInitialEvidence.find("generic_resident_request_authority_claim false")!=
			std::string::npos&&
		predictiveInitialEvidence.find("force_header_sha256 "
			"216e12f19719bd2aed75ba335cdc2545206d479a54f613e6f9a7e213ffb3f307")!=
			std::string::npos&&
		predictiveInitialEvidence.find("golden_fixture_sha256 "
			"7117cad3e099552aff03f6c9043483278a04bcfea4d155dbdb09a4804a33d8cd")!=
			std::string::npos,
		"r163 historical bytes retain the predictive formula and exact refusal");
	const std::string drainAwareRetryEvidence=ReadText(
		"rendered/fire_production_calibration/r164_drain_aware_retry_acceptance/"
		"drain_aware_retry_acceptance.v1");
	const std::string drainAwareRetryRawMeasurement=ReadText(
		"rendered/fire_production_calibration/r164_drain_aware_retry_acceptance/"
		"drain_aware_retry_measurement.raw.v1");
	Check(!drainAwareRetryEvidence.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(drainAwareRetryEvidence.begin(),
			drainAwareRetryEvidence.end()))==
		"d00948b4025960eef1fc209dac62bdca9b909bd670441213dc45d80c38fe08a9"&&
		drainAwareRetryEvidence.find("candidate_0_refusal_reproduced true")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("candidate_1_represented_dt "
			"0.00055692793102934957")!=std::string::npos&&
		drainAwareRetryEvidence.find("candidate_1_field_max 0.023429989814758301")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("candidate_1_headroom_margin "
			"0.0000075101852416992188")!=std::string::npos&&
		drainAwareRetryEvidence.find("candidate_1_accepted_token true")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("retry_disposition "
			"production_owned_generic_accept_retry_reject")!=std::string::npos&&
		drainAwareRetryEvidence.find("retry_cap_precheck "
			"candidate_index_below_cap_before_accept_or_increment")!=std::string::npos&&
		drainAwareRetryEvidence.find("retry_acceptance_authority "
			"producer_token_revalidated_against_current_payload")!=std::string::npos&&
		drainAwareRetryEvidence.find("retry_payload_mutation_RED "
			"authentic_token_retained_mutate_classify_restore_reaccept")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("retry_controller_RED "
			"candidate_1_refusal_classify_branch_continues_candidate_2")!=
			std::string::npos&&
		forceHeader.find("ClassifyFireProductionResidentStepAttempt")!=std::string::npos&&
		drainAwareRetryEvidence.find("accepted_path_final_wall_p95_ms 151.894375")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("final_tier10_wall_projection_hours "
			"1.89400098260743")!=std::string::npos&&
		drainAwareRetryEvidence.find("candidate_0_ordinary_advance_refused true")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("device_metric "
			"queue_DAG_span_not_overlapping_work_sum")!=std::string::npos&&
		drainAwareRetryEvidence.find("controlled_serial_wall_p95_ms 229.521625")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("accepted_path_final_paired_host_residual_p95_ms "
			"34.631833361461759")!=std::string::npos&&
		!drainAwareRetryRawMeasurement.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			drainAwareRetryRawMeasurement.begin(),drainAwareRetryRawMeasurement.end()))==
			"e812eef43817ff7d4039b82177cd7084f9aff680d8d3d82ce19a76fb5a050d7b"&&
		drainAwareRetryEvidence.find("raw_measurement_trace_sha256 "
			"e812eef43817ff7d4039b82177cd7084f9aff680d8d3d82ce19a76fb5a050d7b")!=
			std::string::npos&&
		drainAwareRetryRawMeasurement.find("DRAIN_AWARE_PLATEAU_RETRY_CONTINUE "
			"refused_candidate=0")!=std::string::npos&&
		drainAwareRetryRawMeasurement.find("DRAIN_AWARE_PLATEAU_RETRY_ACCEPTED "
			"candidate=1")!=std::string::npos&&
		drainAwareRetryRawMeasurement.find(
			"DRAIN_AWARE_PLATEAU_RETRY_ACCEPTED_GENERIC candidate=1")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("wall_target_met false")!=std::string::npos&&
		drainAwareRetryEvidence.find("r136_retained_trace_digest "
			"16bba8260bb71a7bc28e5af174efd970b189fa6404d252d755fe5bcd9d0baaf6")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("retry_acceptance_exact_exit 206")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("retry_controller_RED_exact_exit 204")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("force_header_sha256 "
			"9f42b297a85846044283a0a9c2a699a77cdb669a50498df136b7dddb39337175")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("advection_metal_sha256 "
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("golden_fixture_sha256 "
			"5170949314f54165347b3496165dad9e81666570ef34b27e81951db6ee064d9b")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("unix_test_driver_sha256 "
			"183735135701668a01b571f2ee32ff1303ccf4f5100102eed0f3f419687c49c3")!=
			std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			timestepVelocityBenchmarkOptions.begin(),timestepVelocityBenchmarkOptions.end()))==
			"be63f6fcd99666a1d2c611f4d06e6f082e9b3b4223216334a0dea9b2e2684d05"&&
		goldenCompositionFixture.find("DRAIN_AWARE_PLATEAU_RETRY_ACCEPTED")!=
			std::string::npos&&
		drainAwareRetryRawMeasurement.find("accepted_token=1")!=std::string::npos&&
		productionSolverTest.find("r164 host-residual activation outside the limited "
			"predictor path fails before Metal work")!=std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r164_drain_aware_retry")!=
			std::string::npos&&
		drainAwareRetryEvidence.find("retry_acceptance_exact_exit 206")!=
			std::string::npos,
		"r164 historical bytes bind the drain-aware refusal retry, accepted plateau, and host profile");
	const std::string operatingPointAudit=ReadText(
		"rendered/fire_production_calibration/r165_accepted_long_shadow/"
		"accepted_long_shadow_protocol.v1");
	const std::string operatingPointRaw=ReadText(
		"rendered/fire_production_calibration/r165_accepted_long_shadow/"
		"accepted_long_shadow_measurement.raw.v1");
	Check(!operatingPointAudit.empty()&&!operatingPointRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			operatingPointAudit.begin(),operatingPointAudit.end()))==
			"8d3c059efcafb9d7a2bda46a10bd30e8cb39825c060e578e2cf4abd4aa817400"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			operatingPointRaw.begin(),operatingPointRaw.end()))==
			"72be807e6529adc9d56883fe47c82132d6bc9f2ac3f265738c31145ca5f9472b"&&
		operatingPointAudit.find("status review_invalidated_before_long_shadow")!=
			std::string::npos&&
		operatingPointAudit.find("step2_first_candidate_field 0.062683582305908203")!=
			std::string::npos&&
		operatingPointAudit.find("step2_accepted_candidate 6")!=std::string::npos&&
		operatingPointAudit.find("step2_accepted_dt_s 0.00017358525656163692")!=
			std::string::npos&&
		operatingPointAudit.find("tier10_cost_projection_valid false")!=
			std::string::npos&&
		operatingPointAudit.find("equal_time_per_attempt_schedule_bound false")!=
			std::string::npos&&
		operatingPointAudit.find("limiter_observable_authority "
			"exact_zero_beginning_or_exact_transport_rest_only")!=
			std::string::npos&&
		operatingPointAudit.find("ordinary_advance_without_material_authority "
			"atomically_refused")!=std::string::npos&&
		operatingPointAudit.find("accepted_attempt_extrapolation_is_lower_bound false")!=
			std::string::npos&&
		operatingPointAudit.find("material_authority_refusal_exact_exit 214")!=
			std::string::npos&&
		operatingPointAudit.find("material_authority_refusal_replayed true")!=
			std::string::npos&&
		operatingPointAudit.find("material_authority_refusal_accepted_token false")!=
			std::string::npos&&
		operatingPointAudit.find("current_r136_trace_digest "
			"fc67b2530ac93b03a244563ca731b1450a585d83110fee4edd0743453c8cd735")!=
			std::string::npos&&
		operatingPointAudit.find("physical_projection_retry_path 12_to_13_to_14")!=
			std::string::npos&&
		operatingPointAudit.find("retained_exact_exit 209")!=
			std::string::npos&&
		operatingPointAudit.find("long_shadow_accepted false")!=std::string::npos&&
		operatingPointAudit.find("subsequent_milestones_executed false")!=
			std::string::npos&&
		operatingPointRaw.find("step2_refusal5 candidate=5")!=std::string::npos&&
		operatingPointRaw.find("step2_accept candidate=6")!=std::string::npos&&
		operatingPointRaw.find("status invalidated_calibrating_observation")!=
			std::string::npos&&
		operatingPointRaw.find("retry_attempt_costs_measured false")!=
			std::string::npos&&
		operatingPointRaw.find("accepted_attempt_extrapolation_is_lower_bound false")!=
			std::string::npos&&
		operatingPointAudit.find("force_header_sha256 "
			"755807952d9e3137103f0b0dfe155d145215cf6c5edf2f7e19f5bcbaca57eab0")!=
			std::string::npos&&
		operatingPointAudit.find("force_source_sha256 "
			"eef794a21e1b904567cda373d3aeb724b70db7a66b861828cb5a4382cd044daa")!=
			std::string::npos&&
		operatingPointAudit.find("advection_metal_sha256 "
			"350f56363227bb5958047f118716fe9da8da353240c4e3905544fc5cfab6a895")!=
			std::string::npos&&
		operatingPointAudit.find("golden_fixture_sha256 "
			"1743883ee9abc565de937fb71ccc3c7632746f19eddd6b4c1f1469d995a9f1fb")!=
			std::string::npos&&
		operatingPointAudit.find("unix_test_driver_sha256 "
			"8ec329ffa9f436d4b39ea4d87758677a7fbe78e84159be9985347eef01668aeb")!=
			std::string::npos&&
		operatingPointAudit.find("calibration_mirror_sha256 "
			"50d3ebd37bd7fa193d967cacf5c8ce40038dd20ffffcbaddd2e54594a21e5bd7")!=
			std::string::npos&&
		operatingPointAudit.find("roundoff_trace_adapter_sha256 "
			"07164177f6d0c43c71a1a59511b7455d6738ff56bd9b0347899eb872a04d93ed")!=
			std::string::npos,
		"r165 withdraws the invalid trajectory claim and binds the projection retry");
	const std::string distributionShadowEvidence=ReadText(
		"rendered/fire_production_calibration/r166_distribution_long_shadow/"
		"distribution_long_shadow_schedule_stop.v1");
	const std::string distributionShadowRaw=ReadText(
		"rendered/fire_production_calibration/r166_distribution_long_shadow/"
		"accepted_long_shadow.raw.v1");
	Check(!distributionShadowEvidence.empty()&&!distributionShadowRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			distributionShadowEvidence.begin(),distributionShadowEvidence.end()))==
			"2bfa631f9474bcfcef8707bd0a404d5f46406add4bc9ac3b90eeede1b41dc9a8"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			distributionShadowRaw.begin(),distributionShadowRaw.end()))==
			"4979ab6b85cbd72fb4a807f8f8d10d6a67991bbd8543cfa15f46767cb1c113ed"&&
		distributionShadowEvidence.find(
			"owner_ruling nonsecularity_uses_motion_invariant_distribution_observables")!=
			std::string::npos&&
		distributionShadowEvidence.find(
			"eulerian_generation_authority exact_zero_beginning_or_exact_stationary_transport_only")!=
			std::string::npos&&
		distributionShadowEvidence.find(
			"synthetic_motion_only_fixed_distribution_GREEN true")!=std::string::npos&&
		distributionShadowEvidence.find(
			"synthetic_flat_max_secular_p95_RED true")!=std::string::npos&&
		distributionShadowEvidence.find("requested_shadow_steps 104")!=std::string::npos&&
		distributionShadowEvidence.find("accepted_shadow_steps 7")!=std::string::npos&&
		distributionShadowEvidence.find(
			"step_0_field_p95 5.245208740234375e-06")!=std::string::npos&&
		distributionShadowEvidence.find(
			"step_1_field_p95 0.00015151500701904297")!=std::string::npos&&
		distributionShadowEvidence.find(
			"step_6_field_max 0.020704150199890137")!=std::string::npos&&
		distributionShadowEvidence.find(
			"slice_7_candidate_0_field_max 0.033993184566497803")!=std::string::npos&&
		distributionShadowEvidence.find(
			"slice_7_retry_reference_64_last_residual 0.0191989")!=std::string::npos&&
		distributionShadowEvidence.find(
			"slice_7_retry_reference_tolerance 0.000479545")!=std::string::npos&&
		distributionShadowEvidence.find("schedule_refinement_sequence 8_16_32_64")!=
			std::string::npos&&
		distributionShadowEvidence.find("retained_exact_exit 115")!=std::string::npos&&
		distributionShadowEvidence.find("current_r136_trace_digest "
			"93c12abf214faf4d1d8d78bfb790a15c9e3a8ec596385238184f04cf3bcb8a16")!=
			std::string::npos&&
		distributionShadowEvidence.find("long_shadow_classifier_executed false")!=
			std::string::npos&&
		distributionShadowEvidence.find("golden_B_fp32_not_run true")!=std::string::npos&&
		CountText(distributionShadowRaw,"GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=")==7u&&
		distributionShadowRaw.find(
			"GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=0 dt=0.0005569194327108562")!=
			std::string::npos&&
		distributionShadowRaw.find(
			"GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=6 dt=3.7466904814209556e-06")!=
			std::string::npos&&
		distributionShadowRaw.find(
			"EQUAL_TIME_REFERENCE_RETRY slice=7 failed_substeps=8 next_substeps=16")!=
			std::string::npos&&
		distributionShadowRaw.find(
			"EQUAL_TIME_REFERENCE_RETRY slice=7 failed_substeps=16 next_substeps=32")!=
			std::string::npos&&
		distributionShadowRaw.find(
			"EQUAL_TIME_REFERENCE_RETRY slice=7 failed_substeps=32 next_substeps=64")!=
			std::string::npos&&
		distributionShadowRaw.find(
			"production golden parallel reference 7 failed: equal-time reference substep 8: R1")!=
			std::string::npos&&
		distributionShadowRaw.find("GOLDEN_LONG_SHADOW steps=104")==std::string::npos&&
		distributionShadowEvidence.find("force_header_sha256 "
			"f31be06b6b0c7fb96dc4293a037839f2db1a2895d4c93e3748036ec004b9c7df")!=
			std::string::npos&&
		distributionShadowEvidence.find("force_source_sha256 "
			"10d94716ff885183b913f5e6450195382904e5b5722398ea5b892d43d4adba47")!=
			std::string::npos&&
		distributionShadowEvidence.find("advection_metal_sha256 "
			"519eeb31230264ed02342d224a3377c83ef1d4bdda50d9f137351a02142e0b29")!=
			std::string::npos&&
		distributionShadowEvidence.find("calibration_math_sha256 "
			"9ed32df78e065cf85ca6a1fc3bfbd2c7db3e42aa7db658436ff360234ead0ac6")!=
			std::string::npos&&
		distributionShadowEvidence.find("golden_fixture_sha256 "
			"4400b56ea50ccc6cf838aaf4551910045368174e3392405025c11162ef88cd20")!=
			std::string::npos&&
		distributionShadowEvidence.find("dyadic_fixture_sha256 "
			"d2bc33fb05a885198923ca95165e1729ce7a711fd0d043673bf67df1ca7c3acb")!=
			std::string::npos&&
		distributionShadowEvidence.find("solver_test_sha256 "
			"52d0456993a5c942a281783c16d1d39ba0c0f04216ff78d9eed8b45aa58af15c")!=
			std::string::npos&&
		distributionShadowEvidence.find("sequence_test_sha256 "
			"57feb2f93dd1af43afbd286a85b106a91d68d853bdd16865bc5b52919d933d1d")!=
			std::string::npos&&
		distributionShadowEvidence.find("fp64_manifest_sha256 "
			"77ab26c94a6381e5431147ff071c334a7278da8e09e23ebd7e0123f95c582dba")!=
			std::string::npos&&
		distributionShadowEvidence.find("trace_manifest_sha256 "
			"6a2ac2a166c517749f5a3cd43a7413bf0abab51394737ede32d8a768dc24f7d4")!=
			std::string::npos&&
		distributionShadowEvidence.find("unix_test_driver_sha256 "
			"61978e1a4df3b84c7dfb90e05f743337928e246dc0c39f795755f4a41772f1a2")!=
			std::string::npos&&
		advectionMetal.find("select_methane_manifold_high_bins")!=std::string::npos&&
		advectionMetal.find("histogram_methane_manifold_low_bins")!=std::string::npos&&
		advectionMetal.find("select_methane_manifold_low_bins")!=std::string::npos&&
		goldenCompositionFixture.find("longShadowFieldP95")!=std::string::npos&&
		goldenCompositionFixture.find("longShadowFieldP50")!=std::string::npos&&
		unixTestDriver.find("distribution_shadow_rc\" -eq 115")!=std::string::npos&&
		unixTestDriver.find(
			"PASS (exact exit=115, slice-7 equal-time reference schedule refusal)")!=
			std::string::npos,
		"r166 binds device distribution reductions and the slice-7 equal-time schedule stop");
	const std::string closureConvergenceEvidence=ReadText(
		"rendered/fire_production_calibration/r167_anomaly_closure_convergence/"
		"anomaly_closure_convergence_stop.v1");
	const std::string closureConvergenceRaw=ReadText(
		"rendered/fire_production_calibration/r167_anomaly_closure_convergence/"
		"anomaly_closure_convergence.raw.v1");
	const std::string closureConvergencePlot=ReadText(
		"rendered/fire_production_calibration/r167_anomaly_closure_convergence/"
		"anomaly_closure_convergence_curve.svg");
	Check(!closureConvergenceEvidence.empty()&&!closureConvergenceRaw.empty()&&
		!closureConvergencePlot.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			closureConvergenceEvidence.begin(),closureConvergenceEvidence.end()))==
			"5ff3d9db69b84794bc53078c63545dcc12c0d0b77a0da243e2d5bced4eab0ca8"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			closureConvergenceRaw.begin(),closureConvergenceRaw.end()))==
			"9ffb55057dbb07bfb9d8c27bb442b97a08dea757b3e15894e37952a6207f37f0"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			closureConvergencePlot.begin(),closureConvergencePlot.end()))==
			"2e2f8f07f7260230ac507be072d40c1cbe3812e6d27266a609acda4f5d29cdab"&&
		closureConvergenceEvidence.find("curve_plot_sha256 "
			"2e2f8f07f7260230ac507be072d40c1cbe3812e6d27266a609acda4f5d29cdab")!=
			std::string::npos&&
		closureConvergenceEvidence.find("current_r136_trace_digest "
			"fc67b2530ac93b03a244563ca731b1450a585d83110fee4edd0743453c8cd735")!=
			std::string::npos&&
		closureConvergenceEvidence.find("current_r136_exact_exit 237")!=
			std::string::npos&&
		closureConvergenceEvidence.find("current_r136_metal_measurement 0")!=
			std::string::npos&&
		closureConvergencePlot.find("Golden CFL anomaly-closure convergence")!=
			std::string::npos&&
		closureConvergenceEvidence.find(
			"owner_model G_equals_dose_of_dt_plus_feedback_of_deviation")!=
			std::string::npos&&
		closureConvergenceEvidence.find(
			"r166_feedback_slope 0.79258751342062539")!=std::string::npos&&
		closureConvergenceEvidence.find(
			"derived_convergence_tolerance 8.1249999999999996e-05")!=
			std::string::npos&&
		closureConvergenceEvidence.find(
			"fp32_fp64_interpretation scheme_curve_not_precision_floor")!=
			std::string::npos&&
		closureConvergenceEvidence.find(
			"curve_class oscillatory_noncontractive_then_divergent")!=
			std::string::npos&&
		closureConvergenceEvidence.find("multi_pass_closure_adopted false")!=
			std::string::npos&&
		closureConvergenceEvidence.find("retained_exact_exit 201")!=
			std::string::npos&&
		CountText(closureConvergenceRaw,"ANOMALY_CLOSURE_CONVERGENCE pass=")==8u&&
		closureConvergenceRaw.find(
			"ANOMALY_CLOSURE_CONVERGENCE pass=1 tolerance=8.1249999999999996e-05 "
			"G32=0.085895776748657227 field32=0.085895776748657227 "
			"G64=0.085895672361020692")!=std::string::npos&&
		closureConvergenceRaw.find(
			"ANOMALY_CLOSURE_CONVERGENCE pass=2 tolerance=8.1249999999999996e-05 "
			"G32=0.066569089889526367 field32=0.066569089889526367 "
			"G64=0.066569466014946732")!=std::string::npos&&
		closureConvergenceRaw.find(
			"ANOMALY_CLOSURE_CONVERGENCE pass=8 tolerance=8.1249999999999996e-05 "
			"G32=0.16477346420288086 field32=0.16477346420288086 "
			"G64=0.16477412949642256")!=std::string::npos&&
		closureConvergenceRaw.find(
			"ANOMALY_CLOSURE_FEEDBACK_GAIN pairs=6 slope=0.79258751342062539 "
			"intercept=0.022579619687232797 pearson=0.7129565317645592")!=
			std::string::npos&&
		closureConvergenceRaw.find(
			"ANOMALY_CLOSURE_CONVERGENCE_VERDICT tolerance=8.1249999999999996e-05 "
			"G32_pass8=0.16477346420288086 G64_pass8=0.16477412949642256 "
			"converged=0 stalled_above_1e-3=1")!=std::string::npos&&
		closureConvergenceEvidence.find("advection_metal_sha256 "
			"3d68816d42c7f1704f0426df03ac0ffe626aa5d4183695dcd63645ef8feaba38")!=
			std::string::npos&&
		closureConvergenceEvidence.find("calibration_mirror_sha256 "
			"0dbe115c059c4e4567b072040046917b1250b2d1873d942258b381d79b6f1de9")!=
			std::string::npos&&
		closureConvergenceEvidence.find("golden_fixture_sha256 "
			"89d6086bbe798f8504dc3677a63f5eb2ebfdeb942230eb133f30d723af1b7a4f")!=
			std::string::npos&&
		closureConvergenceEvidence.find("solver_test_sha256 "
			"36d7518668de7b73ab40f52d853699bf485a44ce9610e31a40e1d09da797d5c0")!=
			std::string::npos&&
		closureConvergenceEvidence.find("unix_test_driver_sha256 "
			"d9f7b95681bd2c6da4542f17af3d8935f471e1203b5657a302d54e64a9ef12c6")!=
			std::string::npos&&
		advectionMetal.find("RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PASSES")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"ANOMALY_CLOSURE_CONVERGENCE_VERDICT tolerance=")!=
			std::string::npos&&
		productionSolverTest.find(
			"zero-anomaly closure, including an eight-pass diagnostic request, stops after one")!=
			std::string::npos&&
		unixTestDriver.find("closure_convergence_rc\" -eq 201")!=
			std::string::npos&&
		unixTestDriver.find(
			"PASS (exact exit=201, fp32/fp64 closure architecture stop)")!=
			std::string::npos,
		"r167 binds the fp32/fp64 closure curve, free r166 gain fit, and architecture stop");
	const std::string monitoredShadowEvidence=ReadText(
		"rendered/fire_production_calibration/r168_monitored_manifold_shadow/"
		"monitored_manifold_shadow.v1");
	const std::string monitoredShadowRaw=ReadText(
		"rendered/fire_production_calibration/r168_monitored_manifold_shadow/"
		"monitored_shadow.raw.log");
	Check(!monitoredShadowEvidence.empty()&&!monitoredShadowRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			monitoredShadowEvidence.begin(),monitoredShadowEvidence.end()))==
			"99efa9d9a7ed11b37ccf67d8e619a08bfb8ea89cb6fca04efad085a1aad1db2a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			monitoredShadowRaw.begin(),monitoredShadowRaw.end()))==
			"e6d74d5796787e24a4df02d686069a68fbef5b7b749c93993ee1cb4b0e2f8faf"&&
		monitoredShadowEvidence.find(
			"owner_ruling production_returns_to_monitored_manifold_charter")!=
			std::string::npos&&
		monitoredShadowEvidence.find(
			"two_tier_conclusion absolute_manifold_enforcement_costs_oracle_scale_timesteps_or_iterations")!=
			std::string::npos&&
		monitoredShadowEvidence.find(
			"ordinary_policy monitor_distribution_do_not_restore_limit_or_refuse")!=
			std::string::npos&&
		monitoredShadowEvidence.find(
			"manifold_class fidelity_contract_diagnostic")!=std::string::npos&&
		monitoredShadowEvidence.find("accepted_shadow_steps 104")!=std::string::npos&&
		monitoredShadowEvidence.find(
			"field_max_peak 1.423297643661499")!=std::string::npos&&
		monitoredShadowEvidence.find(
			"field_p95_peak 0.0011827945709228516")!=std::string::npos&&
		monitoredShadowEvidence.find(
			"field_p50_peak 2.574920654296875e-05")!=std::string::npos&&
		monitoredShadowEvidence.find("allowance_crossing_steps 104")!=
			std::string::npos&&
		monitoredShadowEvidence.find("legacy_low_mach_crossing_steps 104")!=
			std::string::npos&&
		monitoredShadowEvidence.find("physical_projection_validations 104")!=
			std::string::npos&&
		monitoredShadowEvidence.find("restoration_projection_validations 0")!=
			std::string::npos&&
		monitoredShadowEvidence.find("accepted_state_tokens 104")!=std::string::npos&&
		monitoredShadowEvidence.find("current_r136_exact_exit 237")!=
			std::string::npos&&
		monitoredShadowEvidence.find(
			"current_r136_trace_digest cae3e4e13d11aa036c637b1d88cd269d9fbf73e4a322164bb24a2ab7fa49ffac")!=
			std::string::npos&&
		monitoredShadowEvidence.find("current_r136_proof_gaps 0xff")!=
			std::string::npos&&
		monitoredShadowEvidence.find("current_r136_metal_measurement 0")!=
			std::string::npos&&
		monitoredShadowEvidence.find(
			"tier10_25s_wall_projection_hours 1.6209499069950497")!=
			std::string::npos&&
		monitoredShadowEvidence.find(
			"requested_0.4_to_0.6_wall_hour_expectation_met false")!=
			std::string::npos&&
		CountText(monitoredShadowRaw,"MONITORED_MANIFOLD_SHADOW_STEP step=")==104u&&
		monitoredShadowRaw.find(
			"MONITORED_TARGET_POLICY absolute_reference_pressure_gate=0 "
			"producer_precision=2 strict_pressure_detector_refused=1 affine_RED_refused=1")!=
			std::string::npos&&
		monitoredShadowRaw.find(
			"MONITORED_MANIFOLD_SHADOW_STEP step=0 dt=0.0016462659696117043 "
			"G_eulerian=0.085895776748657227 field_max=0.085895776748657227 "
			"field_p95=0.00071418285369873047 field_p50=1.1920928955078125e-07 "
			"allowance_crossed=1 ceiling_crossed=1 projection_valid=1 "
			"restoration_passes=0 scalar_reads=1 selector=advective_CFL")!=
			std::string::npos&&
		monitoredShadowRaw.find(
			"MONITORED_MANIFOLD_SHADOW_COMPLETE steps=104 first_dt=0.0016462659696117043 "
			"final_dt=6.4414904045406729e-05")!=std::string::npos&&
		monitoredShadowEvidence.find("force_header_sha256 "
			"5dc6f5a264e16cd3b030154065b2776124cebba4fe48d7aedb69ec9d8316771c")!=
			std::string::npos&&
		monitoredShadowEvidence.find("force_source_sha256 "
			"835b09cda3904790aa1cfcc237daa5694d85f33bfff5cbd453fb3f7f3713a7ee")!=
			std::string::npos&&
		monitoredShadowEvidence.find("advection_metal_sha256 "
			"665f2d389c7c0f1d7f51efb242b37f23a593295d9cd11a90581a99cba39f84ac")!=
			std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fireSimulatorCore.begin(),fireSimulatorCore.end()))==
			"21a5cf2c6177cc535d9ff8ced6835a7b0b09b2fe19dfed887c76d7e89c61eae1"&&
		// r168's source bytes remain sealed by its immutable artifact.  r181 now
		// owns the live diagnostic-source binding after adding onset retention.
		monitoredShadowEvidence.find("golden_fixture_sha256 "
			"387b6dc930afc92a7d5480ab7b6b55b8024dedd7cc17a796214368406ee58b6c")!=
			std::string::npos&&
		monitoredShadowEvidence.find("dyadic_fixture_sha256 "
			"ca00922ed3135b8431fc411dc62f84ecfd86f8782ade3e26b732189ca85892cc")!=
			std::string::npos&&
		monitoredShadowEvidence.find("solver_test_sha256 "
			"e355cd70ec04c016ab3ade7e8f72cf2b7894feb17ec61ad17d5ca5964805e754")!=
			std::string::npos&&
		monitoredShadowEvidence.find("sequence_test_sha256 "
			"c1d00d301408d68d8289f16cab2aee11474d410babbedeb3079344b7b8486388")!=
			std::string::npos&&
		monitoredShadowEvidence.find("calibration_mirror_sha256 "
			"77f72a9132d529b1d09e9bbf9a4e8c984f0bcb148d8682a1a5e4340fd3a2db22")!=
			std::string::npos&&
		monitoredShadowEvidence.find("roundoff_trace_adapter_sha256 "
			"a4bf94c30688d8addbf1988c5873438f83a4b9e9a2003bc618102f055056a5fc")!=
			std::string::npos&&
		monitoredShadowEvidence.find("fp64_manifest_sha256 "
			"c523624259a82931b9e5659879fa1910b2a61cc6997daa2076ee64884bfe0f36")!=
			std::string::npos&&
		monitoredShadowEvidence.find("trace_manifest_sha256 "
			"672fba481099fca464d6bb6dc535e9e31de9c3ca0aac9bde7f54cc857792cce6")!=
			std::string::npos&&
		monitoredShadowEvidence.find("unix_test_driver_sha256 "
			"01a4f7d8ed826337426750648741b57943655e24af4cee37478cdd1f2fb1cf57")!=
			std::string::npos&&
		forceHeader.find("restoreManifoldOutliers(true)")!=
			std::string::npos&&
		fireSimulator3DAdvance.find("MonitoredProductionTangentDivergenceTarget3D")!=
			std::string::npos&&
		unixTestDriver.find("monitored_shadow_rc\" -eq 195")!=std::string::npos&&
		unixTestDriver.find("MONITORED_TARGET_POLICY absolute_reference_pressure_gate=0")!=
			std::string::npos&&
		unixTestDriver.find(
			"PASS (exact exit=195, zero-tail monitored step accepted)")!=
			std::string::npos,
		"r168 binds monitored-manifold policy, 104-step distribution evidence, and physics gates");
	const std::string outlierBoundedEvidence=ReadText(
		"rendered/fire_production_calibration/r169_outlier_bounded_manifold_shadow/"
		"outlier_bounded_shadow_stop.v1");
	const std::string outlierBoundedRaw=ReadText(
		"rendered/fire_production_calibration/r169_outlier_bounded_manifold_shadow/"
		"outlier_bounded_shadow.raw.log");
	Check(!outlierBoundedEvidence.empty()&&!outlierBoundedRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			outlierBoundedEvidence.begin(),outlierBoundedEvidence.end()))==
			"de2d76d29bca8737e2d0fc94e37a27fdc2fcbaf2dfb79073ed701de5e44e5c59"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			outlierBoundedRaw.begin(),outlierBoundedRaw.end()))==
			"c72350c3c8c2587aad20c79a6ee4509c9ad5e2250f007b9667dd26a27c09b6da"&&
		outlierBoundedEvidence.find(
			"owner_ruling outlier_bounded_monitored_manifold")!=std::string::npos&&
		outlierBoundedEvidence.find(
			"r160_enumeration_gap absolute_P_consistency_has_no_consumer_but_rho_T_consistency_feeds_M_over_rho_g_and_buoyancy")!=std::string::npos&&
		outlierBoundedEvidence.find("engagement_threshold_abs_deviation 0.125")!=
			std::string::npos&&
		outlierBoundedEvidence.find("dynamics_validity_bound_abs_deviation 0.25")!=
			std::string::npos&&
		outlierBoundedEvidence.find(
			"current_r136_trace_digest 2650eecbab183bd7537c002aa73b565f2af51ff4783bbef5a76e695fbdf07c4a")!=
			std::string::npos&&
		outlierBoundedEvidence.find("current_r136_exact_exit 237")!=std::string::npos&&
		outlierBoundedEvidence.find("current_r136_proof_gaps 0xff")!=std::string::npos&&
		outlierBoundedEvidence.find("current_r136_metal_measurement 0")!=
			std::string::npos&&
		outlierBoundedEvidence.find("accepted_shadow_steps 33")!=std::string::npos&&
		outlierBoundedEvidence.find("restoration_steps 16")!=std::string::npos&&
		outlierBoundedEvidence.find(
			"maximum_velocity_peak_accepted_m_per_s 10.694913864135742")!=
			std::string::npos&&
		outlierBoundedEvidence.find("refusal_step 33")!=std::string::npos&&
		outlierBoundedEvidence.find("refusal_field_max 0.25728172063827515")!=
			std::string::npos&&
		outlierBoundedEvidence.find("ordinary_API_atomic_default_result true")!=
			std::string::npos&&
		outlierBoundedEvidence.find(
			"bulk_restoration_mutant threshold_zero_is_r166_global_absolute_reference_restoration")!=
			std::string::npos&&
		outlierBoundedEvidence.find("bulk_restoration_feedback_slope 0.79258751342062539")!=
			std::string::npos&&
		outlierBoundedEvidence.find("completed_104_step_shadow false")!=std::string::npos&&
		CountText(outlierBoundedRaw,"MONITORED_MANIFOLD_SHADOW_STEP step=")==33u&&
		outlierBoundedRaw.find(
			"MONITORED_MANIFOLD_SHADOW_STEP step=17 dt=0.0013979171635583043 ")!=
			std::string::npos&&
		outlierBoundedRaw.find(
			"restoration_passes=1 tail_cells=1 tail_excess=0.0028437142402717441")!=
			std::string::npos&&
		outlierBoundedRaw.find(
			"OUTLIER_BOUNDED_MANIFOLD_REFUSAL step=33 dt=0.0011971283238381147 "
			"field_max=0.25728172063827515")!=std::string::npos&&
		outlierBoundedRaw.find(
			"physical_valid=1 restoration_valid=1 accepted_token=0 "
			"beginning_max_velocity=10.222167015075684 ordinary_atomic=1")!=
			std::string::npos&&
		monitoredShadowRaw.find(
			"MONITORED_MANIFOLD_SHADOW_STEP step=16 dt=0.0014444440603256226 ")!=
			std::string::npos&&
		monitoredShadowRaw.find(
			"field_max=0.12784385681152344")!=std::string::npos&&
		monitoredShadowRaw.find(
			"MONITORED_MANIFOLD_SHADOW_STEP step=81 dt=0.00011666058708215132 ")!=
			std::string::npos&&
		monitoredShadowRaw.find("max_velocity=104.89614868164062")!=std::string::npos&&
		distributionShadowRaw.find(
			"GOLDEN_LONG_SHADOW_ACCEPT_CANDIDATE step=6 dt=3.7466904814209556e-06")!=
			std::string::npos&&
		forceHeader.find("restoreManifoldOutliers(true)")!=std::string::npos&&
		outlierBoundedEvidence.find(
			"force_source_sha256 dccabba2027704b75857d28690db17034663158722bd807eec1e6624a4129ae5")!=
			std::string::npos&&
		forceSource.find("const double target=-std::copysign")!=std::string::npos&&
		advectionMetal.find("targetedRestorationActive")!=std::string::npos&&
		advectionMetal.find(
			"production realized manifold deviation exceeds the dynamics-validity bound")!=
			std::string::npos&&
		outlierBoundedEvidence.find(
			"solver_test_sha256 1c787cbf3998bc69a2de3e345954334b858c4213a937d77346379fc770b3aedc")!=
			std::string::npos&&
		goldenCompositionFixture.find("OUTLIER_BOUNDED_MANIFOLD_REFUSAL")!=
			std::string::npos&&
		unixTestDriver.find("zero-tail monitored step accepted")!=std::string::npos,
		"r169 binds surgical tail restoration, causal REDs, and the step-33 dynamics stop");
	const std::string twoDoseEvidence=ReadText(
		"rendered/fire_production_calibration/r170_two_dose_tail_margin/"
		"two_dose_tail_margin.v1");
	const std::string twoDoseRaw=ReadText(
		"rendered/fire_production_calibration/r170_two_dose_tail_margin/"
		"two_dose_shadow.raw.log");
	const std::string retiredThresholdRaw=ReadText(
		"rendered/fire_production_calibration/r170_two_dose_tail_margin/"
		"retired_threshold_stop.raw.log");
	const std::string hardBoundRetryRaw=ReadText(
		"rendered/fire_production_calibration/r170_two_dose_tail_margin/"
		"hard_bound_retry.raw.log");
	Check(!twoDoseEvidence.empty()&&!twoDoseRaw.empty()&&!retiredThresholdRaw.empty()&&
		!hardBoundRetryRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			twoDoseEvidence.begin(),twoDoseEvidence.end()))==
			"43bbf3a92e822595854e41356f85d934990c6ad4466cd94571a22d00addfe8e0"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			twoDoseRaw.begin(),twoDoseRaw.end()))==
			"7780d89283627fa12e50b92f86a0378575dcfc43129f90cc4f979c08ca380f3f"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			retiredThresholdRaw.begin(),retiredThresholdRaw.end()))==
			"f12c72fee639729fa073dd677ddcc1d5c84333d9661d7d4bd840cebc9fcaacc1"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			hardBoundRetryRaw.begin(),hardBoundRetryRaw.end()))==
			"265979cf10173d7a1231298405f108d77ec16f7f2dc1f0d273c65faa569d96a5"&&
		twoDoseEvidence.find("force_header_sha256 "
			"c06d70891190c2b2626ff82adc395dc404c73d56f5a27ff7ed6fc121907f9970")!=
			std::string::npos&&
		// r170's current-source pins are historical provenance inside the sealed
		// artifact; r181 binds the superseding live owners.
		twoDoseEvidence.find("golden_fixture_sha256 "
			"9938c899642d6b322c9c2cd0cc8bd25292110b4e1c095a2ddc111f77d4759fb2")!=
			std::string::npos&&
		twoDoseEvidence.find("dyadic_fixture_sha256 "
			"aede82d83a044679ebb806bfac794bf00703a0b9936775369a3b2657b460113b")!=
			std::string::npos&&
		twoDoseEvidence.find("unix_test_driver_sha256 "
			"991c48ca56a5154ad318eab7eb2adda9f4cb73c76bf5d295b33e9dc6f2a72a17")!=
			std::string::npos&&
		twoDoseEvidence.find("solver_doc_sha256 "
			"c5f58f46db307edd4d6164558a49923267c4719e8b8ae7115026e447d9db3480")!=
			std::string::npos&&
		twoDoseEvidence.find("history_doc_sha256 "
			"dbb1c2afa0d97b961d44908ca8cce6c44236798c031d48ac3b192ed0cdadcad4")!=
			std::string::npos&&
		twoDoseEvidence.find("engagement_threshold_hex 0x1p-4")!=std::string::npos&&
		twoDoseEvidence.find("required_headroom_doses 2")!=std::string::npos&&
		twoDoseEvidence.find("hard_bound_retry_cap 20")!=std::string::npos&&
		twoDoseEvidence.find(
			"current_r136_trace_digest 584ff3d14c12d700b06299e81947c3e2d03cf529b2f494fdf48f909a9570cbdd")!=
			std::string::npos&&
		twoDoseEvidence.find("shadow_completed true")!=std::string::npos&&
		twoDoseEvidence.find("shadow_hard_bound_retries 0")!=std::string::npos&&
		twoDoseEvidence.find("shadow_maximum_deviation_peak 0.15430498123168945")!=
			std::string::npos&&
		twoDoseEvidence.find("shadow_maximum_velocity_m_per_s 10.871506690979004")!=
			std::string::npos&&
		twoDoseEvidence.find("shadow_tail_population_peak 9698")!=std::string::npos&&
		twoDoseEvidence.find(
			"shadow_trace_sha256 e3273037f56068efb2c067b8b70ec9524cfbd4edcf742c4ac51084b8bde507fa")!=
			std::string::npos&&
		CountText(twoDoseRaw,"MONITORED_MANIFOLD_SHADOW_STEP step=")==104u&&
		twoDoseRaw.find(
			"MONITORED_MANIFOLD_SHADOW_STEP step=33 dt=0.0011745213996618986")!=
			std::string::npos&&
		twoDoseRaw.find("field_max=0.14402782917022705")!=std::string::npos&&
		twoDoseRaw.find("MONITORED_MANIFOLD_SHADOW_COMPLETE steps=104")!=
			std::string::npos&&
		retiredThresholdRaw.find(
			"OUTLIER_BOUNDED_MANIFOLD_REFUSAL step=33 candidate=0 dt=0.0011971283238381147 field_max=0.25728172063827515")!=
			std::string::npos&&
		hardBoundRetryRaw.find(
			"OUTLIER_BOUNDED_HARD_RETRY_ACCEPTED step=33 candidate=1 dt=0.0011418721405789256 field_max=0.24757766723632812 physical_valid=1 restoration_valid=1 accepted_token=1")!=
			std::string::npos&&
		forceHeader.find("double engagementThreshold=0x1p-4")!=std::string::npos&&
		forceSource.find("magnitude<=engagementThreshold")!=std::string::npos&&
		advectionMetal.find("localDose/headroom")!=std::string::npos&&
		goldenCompositionFixture.find("RISE_FIRE_MANIFOLD_HARD_BOUND_RETRY_RED")!=
			std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r170_${r170_case}")!=std::string::npos,
		"r170 binds the two-dose margin, ordinary hard-bound retry, and completed shadow");
	const std::string goldenSubdominanceProtocol=ReadText(
		"rendered/fire_production_calibration/r171_golden_subdominance_protocol/"
		"golden_subdominance_protocol.v1");
	const std::string goldenSubdominanceEvidence=ReadText(
		"rendered/fire_production_calibration/r171_golden_subdominance/"
		"golden_subdominance.v1");
	const std::string goldenSubdominanceRaw=ReadText(
		"rendered/fire_production_calibration/r171_golden_subdominance/"
		"golden_subdominance.raw.log");
	Check(!goldenSubdominanceProtocol.empty()&&!goldenSubdominanceEvidence.empty()&&
		!goldenSubdominanceRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			goldenSubdominanceProtocol.begin(),goldenSubdominanceProtocol.end()))==
			"7455f7d7e181adb0e3f78ed2e7bb31b1f6fde9b8d1545e01d6431e4ab953b3b5"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			goldenSubdominanceEvidence.begin(),goldenSubdominanceEvidence.end()))==
			"7477788fa4534cd87549d3564c6e5a3263360b564a7785c80af6742d97dc180c"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			goldenSubdominanceRaw.begin(),goldenSubdominanceRaw.end()))==
			"52ebd0e9a401bbb01bcf73f79d2a127b6385aa69cfa16447a8035a59bf58bcb5"&&
		goldenSubdominanceEvidence.find("generator_source_sha256 "
			"fac523acb9ad43f629d4fb2aab132b2ac2156443c10fd1ee3422b8da1b151739")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find("measurement_fixture_sha256 "
			"0a91b12c762fa1beadac41c716d9a9d15b200f42bb77dd9b8f4842b477c2a5c8")!=
			std::string::npos&&
		// The byte-sealed r171 artifact is the immutable authority for its
		// historical owners.  Later live owners are bound by their own rung.
		goldenSubdominanceEvidence.find("force_header_sha256 "
			"0e832ecc8b24d5e32e11cc60b94363f2095f74fdaf1eddee2e4dbc60b8ffc16b")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find("projection_metal_sha256 "
			"bff3315513f88b51f5967c179dbd2995eaa9ab1ccb35a78d2ae223ae6a5f4871")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find("projection_cpu_sha256 "
			"133a41686fcf4538f0fa14d65a675cb6a3b8ad72d83b376d4022efd34e57663d")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find("projection_header_sha256 "
			"e657a44dd08b0b26c7674800ff0e15a28b996d1373f7f80cd57359fcd9475e42")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find("unix_runner_sha256 "
			"1cb7c6ca09c69e57ae10da9307af81400e060b796952e317dfddb27ba2b20524")!=
			std::string::npos&&
		goldenSubdominanceProtocol.find(
			"state_digest_schema rise.fire.production.beginning.v2")!=std::string::npos&&
		goldenSubdominanceProtocol.find("filter_scale_mutation_RED true")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find("projection_metal_sha256 "
			"bff3315513f88b51f5967c179dbd2995eaa9ab1ccb35a78d2ae223ae6a5f4871")!=
			std::string::npos&&
		goldenSubdominanceProtocol.find("admitted_measurement_performed false")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find("gate_count 152")!=std::string::npos&&
		goldenSubdominanceEvidence.find("full_step_B_fp32_closed true")!=
			std::string::npos&&
		goldenSubdominanceEvidence.find(
			"preliminary_velocity_guard_status superseded_by_derived_subdominance_term")!=
			std::string::npos&&
		CountText(goldenSubdominanceRaw,"golden subdominance slice=")==8u&&
		goldenSubdominanceRaw.find(
			"golden subdominance complete trace=1e48343aa5589cded65f2345e74d2ba103508bdf07fb9ab56e5ea7351cd00b61 slices=8 gates=152")!=
			std::string::npos&&
		unixTestDriver.find("--fire-r171-golden-beginnings")!=std::string::npos&&
		unixTestDriver.find("RISE_FIRE_GOLDEN_SUBDOMINANCE=1")!=std::string::npos,
		"r171 seals current golden beginnings, closes all B_fp32 gates, and supersedes the guard");
	const std::string temporalProtocol=ReadText(
		"rendered/fire_production_calibration/r139_temporal_protocol/temporal_protocol.v1");
	Check(!temporalProtocol.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(temporalProtocol.begin(),temporalProtocol.end()))==
		"e58ee48de0c79dc35aa6e6bf344729c12cc74bf7d89cfa3e78cfeaf28cc9630c"&&
		temporalProtocol.find("protocol_before_temporal_evidence true")!=std::string::npos&&
		temporalProtocol.find("dt_baseline_hex 0x1.e54eeep-10")!=std::string::npos&&
		temporalProtocol.find("dt_half_hex 0x1.e54eeep-11")!=std::string::npos&&
		temporalProtocol.find("dt_quarter_hex 0x1.e54eeep-12")!=std::string::npos&&
		temporalProtocol.find("step_counts 8 16 32")!=std::string::npos&&
		temporalProtocol.find("production_formal_temporal_order 1")!=std::string::npos&&
		temporalProtocol.find("oracle_formal_temporal_order 2")!=std::string::npos&&
		temporalProtocol.find("temporal_measurement_performed false")!=std::string::npos&&
		temporalProtocol.find("metal_dispatched false")!=std::string::npos,
		"r139 freezes the exact dyadic temporal instrument before evidence");
	const std::string temporalStop=ReadText(
		"rendered/fire_production_calibration/r172_temporal_refinement_stop/"
		"temporal_refinement_stop.v1");
	const std::string temporalStopRaw=ReadText(
		"rendered/fire_production_calibration/r172_temporal_refinement_stop/"
		"temporal_refinement.raw.log");
	const std::string temporalTargets=ReadText(
		"rendered/fire_production_calibration/r172_temporal_refinement_stop/"
		"temporal_targets.v1");
	const std::string temporalTarget0=ReadText(
		"rendered/fire_production_calibration/r172_temporal_refinement_stop/"
		"temporal_level0_sdiv.f64");
	const std::string temporalTarget1=ReadText(
		"rendered/fire_production_calibration/r172_temporal_refinement_stop/"
		"temporal_level1_sdiv.f64");
	const std::string temporalTarget2=ReadText(
		"rendered/fire_production_calibration/r172_temporal_refinement_stop/"
		"temporal_level2_sdiv.f64");
	const std::string solverDoc=ReadText("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md");
	const std::string historyDoc=ReadText("docs/FIRE_SMOKE_DESIGN_HISTORY.md");
	Check(!temporalStop.empty()&&!temporalStopRaw.empty()&&!temporalTargets.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			temporalStop.begin(),temporalStop.end()))==
			"554fa871ab47c486f4dcc1a431adbf63b9b8ac1d61ae4d166170ff051c314add"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			temporalStopRaw.begin(),temporalStopRaw.end()))==
			"b36fa4200dfdc253f78e25c9ec2a1f2552e26c46ff4718102aab0c54c1f116a3"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			temporalTargets.begin(),temporalTargets.end()))==
			"6e0af5dcb7602b6fd067bc6d4b113378c14444643300ede6375d442c7cdef42c"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			temporalTarget0.begin(),temporalTarget0.end()))==
			"1cf6244040426b2704f8ac2c4b953c32efd1d0c71cdebea7eacef85f2217d05c"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			temporalTarget1.begin(),temporalTarget1.end()))==
			"1f6a95c059bf63224b63697689e3498a05add9418f9470b1e4c3f8d6e9e30cf1"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			temporalTarget2.begin(),temporalTarget2.end()))==
			"95e0f5032efb2171bc412d4e26411e7dedd91962d171b187a752555862112551"&&
		temporalStop.find("fixture_sha256 "
			"36d8ae800891bcd9e3f2e8fb4e57751ce928173184130aba160967ad9491e94d")!=
			std::string::npos&&
		temporalStop.find("owner_sha256 "
			"3f98a148082a4113bedfad8473a271890784cce42be010e08903694fb7f6ce13")!=
			std::string::npos&&
		temporalStop.find("unix_runner_sha256 "
			"6c4297597ff1b09d62042d8d74baee0200dcbbbb5b464536f56e6b43e169a83c")!=
			std::string::npos&&
		temporalStop.find("solver_doc_sha256 "
			"1509d05724ab8e13c802fd53b5f48d7998804b106af3d4bf6f6868a56db542fb")!=
			std::string::npos&&
		temporalStop.find("history_doc_sha256 "
			"1c7783a845a74534d79e72d67a62992d9fc63654344644ac64001cfd8ab7b29d")!=
			std::string::npos&&
		temporalStop.find("exact_exit 193")!=std::string::npos&&
		temporalStop.find("target_seal_exact_exit 194")!=std::string::npos&&
		temporalStop.find("stale_schedule_actual_penultimate_RED true")!=
			std::string::npos&&
		temporalStop.find("missing_no_Metal_owner_RED true")!=std::string::npos&&
		temporalStop.find("producer_precision_mutation_RED true")!=std::string::npos&&
		temporalStop.find("refusal_counts production_0 oracle_1")!=std::string::npos&&
		temporalStop.find("rejected_scalar_components 8_sensible_energy")!=
			std::string::npos&&
		temporalStop.find("eight_slice_readmission_run false")!=std::string::npos&&
		temporalStopRaw.find("temporal scalar component=8 production_D=")!=
			std::string::npos&&
		CountText(temporalStopRaw,"temporal scalar component=")==9u&&
		CountText(temporalStopRaw,"temporal ledger component=")==9u&&
		temporalStop.find("target_replay write_once_payloads_SHA_verified_const_memory")!=
			std::string::npos&&
		temporalStop.find("all_quantities_evaluated_after_first_failure true")!=
			std::string::npos,
		"r172 executes all temporal rows and refuses noncontracting oracle energy");
	const std::string filteredTemporalProtocol=ReadText(
		"rendered/fire_production_calibration/r173_filtered_temporal_protocol/"
		"filtered_temporal_protocol.v1");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			filteredTemporalProtocol.begin(),filteredTemporalProtocol.end()))==
			"5cefe11a97feb182988e1254c15bb9b1bfb4ecc642e8ad4b3ab2a16b462caeb0"&&
		filteredTemporalProtocol.find("filter_owner_source_sha256 "
			"36d8ae800891bcd9e3f2e8fb4e57751ce928173184130aba160967ad9491e94d")!=
			std::string::npos&&
		filteredTemporalProtocol.find("protocol_before_filtered_temporal_evidence true")!=
			std::string::npos&&
		filteredTemporalProtocol.find("filter_width_m 0.04894898570785762")!=
			std::string::npos&&
		filteredTemporalProtocol.find(
			"outcome_plateau exact_r172_oracle_sensible_energy_pair_only_use_nextUp_max_filtered_interlevel_difference_as_upper_bound")!=
			std::string::npos&&
		filteredTemporalProtocol.find("all_other_noncontracting_pairs refuse")!=
			std::string::npos&&
		filteredTemporalProtocol.find("new_numeric_constant_added false")!=std::string::npos&&
		filteredTemporalProtocol.find("temporal_measurement_performed false")!=
			std::string::npos,
		"r173 freezes filtered temporal floor handling before the rerun");
	const std::string filteredTemporalEvidence=ReadText(
		"rendered/fire_production_calibration/r173_filtered_temporal/"
		"filtered_temporal_evidence.v1");
	const std::string filteredTemporalRaw=ReadText(
		"rendered/fire_production_calibration/r173_filtered_temporal/"
		"filtered_temporal.raw.log");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			filteredTemporalEvidence.begin(),filteredTemporalEvidence.end()))==
			"c85364852d2a48cc4d6b147dddb6c040254ef3931153a387b3950b33da93f5f4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			filteredTemporalRaw.begin(),filteredTemporalRaw.end()))==
			"a9036ec70ef6259dd458c53c5232e4b4d5999e75333d0ecca9da2849521363b5"&&
		filteredTemporalEvidence.find("fixture_sha256 "
			"5840a03f6f15d9d7a5ac4ffc5fd37ab5972ea805f303b276858ffa16d2a0f592")!=
			std::string::npos&&
		filteredTemporalEvidence.find("calibration_math_sha256 "
			"b7db0c46f99b309f7b2b86c767067d3b29ae07d6c861b589c6b8d967f33b6bd6")!=
			std::string::npos&&
		filteredTemporalEvidence.find("owner_sha256 "
			"b5293053ebcbcfd12fa0585c412c6dd72e5e64d18d59fd29aefd5af36f1d9668")!=
			std::string::npos&&
		filteredTemporalEvidence.find("unix_runner_sha256 "
			"211c3de0442f83e3f8be70a65efd6891f01b18681dafb03668c80331e89fa847")!=
			std::string::npos&&
		filteredTemporalEvidence.find("floor_upper_bound_terms production_0 oracle_1")!=
			std::string::npos&&
		filteredTemporalEvidence.find("sole_floor_upper_bound "
			"oracle_scalar_component_8_sensible_energy")!=std::string::npos&&
		filteredTemporalEvidence.find("temporal_contract_complete true")!=
			std::string::npos&&
		CountText(filteredTemporalRaw,"temporal scalar component=")==9u&&
		CountText(filteredTemporalRaw,"temporal ledger component=")==9u&&
		CountText(filteredTemporalRaw,"mode=floor_upper_bound")==1u&&
		filteredTemporalRaw.find("refusals=0/0 floor_bounds=0/1 "
			"sole_floor=oracle_scalar_8 accepted=1")!=std::string::npos,
		"r173 completes the uniformly filtered temporal term with one measured-floor bound");
	const std::string equalTimeReadmissionEvidence=ReadText(
		"rendered/fire_production_calibration/r174_equal_time_readmission/"
		"equal_time_readmission_evidence.v1");
	const std::string equalTimeReadmissionRaw=ReadText(
		"rendered/fire_production_calibration/r174_equal_time_readmission/"
		"equal_time_readmission.raw.log");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			equalTimeReadmissionEvidence.begin(),equalTimeReadmissionEvidence.end()))==
			"356c46c03600e8bd3bb769c3bf29f085fa602a8c82068bab26f09351ae4d128a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			equalTimeReadmissionRaw.begin(),equalTimeReadmissionRaw.end()))==
			"89e579173156c4e3ce9f0932e73dac4817c51282d564a2dfc03451a2e86da32e"&&
		// Historical owner provenance comes from the exact evidence bytes above,
		// never from whichever revision is currently live in the checkout.
		equalTimeReadmissionEvidence.find("fixture_sha256 "
			"f0be92f1a5d70bea5729609396dc619f766388fce5b4ccd9e23810fc22a48693")!=
			std::string::npos&&
		equalTimeReadmissionEvidence.find("calibration_math_sha256 "
			"b7db0c46f99b309f7b2b86c767067d3b29ae07d6c861b589c6b8d967f33b6bd6")!=
			std::string::npos&&
		equalTimeReadmissionEvidence.find("unix_runner_sha256 "
			"058e363848e78e9120380eb37806db47eb0cd92b8cafe59b554176dc9d55a47b")!=
			std::string::npos&&
		equalTimeReadmissionEvidence.find("gate_count 152")!=std::string::npos&&
		equalTimeReadmissionEvidence.find("failure_count 0")!=std::string::npos&&
		equalTimeReadmissionEvidence.find(
			"oracle_schedule_endpoint exact_representable_remainder_in_final_substep")!=
			std::string::npos&&
		equalTimeReadmissionEvidence.find(
			"original_excess_verdict pass_under_completed_filtered_equal_time_contract")!=
			std::string::npos&&
		equalTimeReadmissionEvidence.find(
			"manifold_floor_fidelity_flag evaluated_not_implicated")!=std::string::npos&&
		CountText(equalTimeReadmissionRaw,"equal-time readmission slice=")==80u&&
		CountText(equalTimeReadmissionRaw,"accepted=0")==0u&&
		equalTimeReadmissionRaw.find("equal-time readmission complete slices=8 gates=152 "
			"failures=0")!=std::string::npos&&
		solverDoc.find("### 7.56h Equal-time eight-slice readmission (r174)")!=
			std::string::npos&&
		solverDoc.find("0.0018329622432418256")!=std::string::npos&&
		solverDoc.find("0.0081254983789433733")!=std::string::npos&&
		solverDoc.find("0.0032325000146012773")!=std::string::npos&&
		historyDoc.find("the original `83x/28x/8.7x` excess classes now pass")!=
			std::string::npos,
		"r174 admits every sealed slice under the completed filtered equal-time contract");
	const std::string thermoSourceEvidence=ReadText(
		"rendered/fire_production_calibration/r175_thermo_source_maps/"
		"thermo_source_maps_evidence.v1");
	const std::string thermoSourceRaw=ReadText(
		"rendered/fire_production_calibration/r175_thermo_source_maps/"
		"thermo_source_maps.raw.log");
	const std::string firstLightEXR=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/methane_preview.exr");
	const std::string firstLightEXRProvenance=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/"
		"methane_preview.exr.provenance.cbor");
	const std::string firstLightDisplay=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/"
		"methane_preview_display.png");
	const std::string firstLightDisplayProvenance=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/"
		"methane_preview_display.png.provenance.cbor");
	const std::string firstLightAnimation=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/"
		"methane_preview_animation.gif");
	const std::string firstLightAnimationProvenance=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/"
		"methane_preview_animation.gif.provenance.cbor");
	const std::string firstLightReadme=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/"
		"README.preview_primary.txt");
	const std::string firstLightSequenceManifest=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/"
		"sequence_manifest.rise-fire.cbor");
	const std::string firstLightFrame4=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/frame4.vdb");
	const std::string firstLightFrame5=ReadText(
		"rendered/fire_production_first_light/r175_preview_tier6/frame5.vdb");
	const std::array<const char*,8> firstLightFrameHashes={{
		"90f6a7156ebe12be1e1b5649cb4fd23f195b0c38629700344579a2fdd575c5db",
		"fc2840d84b0f7c2b3412411d2bc288a01238b91b3217d4778395b4d5019b895e",
		"f5e127fe3029f5143daead6caca2c4134c73cd3f955baff60ae839d9764a45e8",
		"413210ec46fe277f55438a6b41608fff7a31dd7989178e06da13465e5a4d9e03",
		"6de424e9ae5846aac8bf23b7ff72f769945d5becf3fdc279eca734c7ffe3d2b0",
		"cf6a3ae9f55afb280a38594bd6850ff46af99c595bdc7ec683f110a565fa0ec1",
		"461a420b874976ab013ad554de817b8e435a5834d2fdb2d38d82a876677998b8",
		"bf02ffa4d99d435ee4c9dc01829233f763a3ee8a32c3653ea27d9e926e7182c9"}};
	const std::array<const char*,8> firstLightFrameSidecarHashes={{
		"aa6e07cdff08e31bb34cec64b739326ef74314e352e5720691b238a231a6ca55",
		"bd200ed6ca86ad093effca6b507854d639239c69fa7d45a1afc019d70cc0a614",
		"c9f4202649a243d58e0cdb49b62398474a54c3657517b551ac7e29383a71b348",
		"033a959a4e154c8dff0c4a4a3bd6a182d50caa43db0f574aff85398f2fce800c",
		"e7494b633762fc93991c6c1801da631c1d165c1b36d6a1b4a294a7947ff2232b",
		"cd8d0bcacde00999a1f8f1689884e977678dd77ad7ca1c4cb12f4b7cffb0c96e",
		"34d7aa6d6473464ddcfc884740c119d4f0bb12db5d768a9246f0b3bff216a91a",
		"5e6187b90c718300a6000100d93c100b55c00814372a112e6d7d327b6e46b44e"}};
	bool firstLightFramesBound=true;
	for(std::size_t frame=0u;frame<8u;++frame){
		std::ostringstream index;index<<std::setw(4)<<std::setfill('0')<<frame;
		const std::string framePath="rendered/fire_production_first_light/"
			"r175_preview_tier6/methane_preview_frame"+index.str()+".exr";
		const std::string frameBytes=ReadText(framePath.c_str());
		const std::string sidecarPath=framePath+".provenance.cbor";
		const std::string sidecarBytes=ReadText(sidecarPath.c_str());
		firstLightFramesBound=firstLightFramesBound&&!frameBytes.empty()&&
			!sidecarBytes.empty()&&RISE::RISECBOR64::SHA256Hex(
				RISE::RISECBOR64::Bytes(frameBytes.begin(),frameBytes.end()))==
				firstLightFrameHashes[frame]&&RISE::RISECBOR64::SHA256Hex(
				RISE::RISECBOR64::Bytes(sidecarBytes.begin(),sidecarBytes.end()))==
				firstLightFrameSidecarHashes[frame];
	}
	RISE::RISECBOR64::Value firstLightAnimationEnvelope;
	std::string firstLightAnimationError;
	const bool firstLightAnimationDecoded=RISE::RISECBOR64::DecodeCanonical(
		RISE::RISECBOR64::Bytes(firstLightAnimationProvenance.begin(),
			firstLightAnimationProvenance.end()),firstLightAnimationEnvelope,
			&firstLightAnimationError);
	const RISE::RISECBOR64::Value* firstLightAnimationPayload=
		firstLightAnimationDecoded?firstLightAnimationEnvelope.Find("payload"):nullptr;
	const RISE::RISECBOR64::Value* firstLightAnimationLinks=firstLightAnimationPayload?
		firstLightAnimationPayload->Find("derived_from_frames"):nullptr;
	Check(!thermoSourceEvidence.empty()&&!thermoSourceRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			thermoSourceEvidence.begin(),thermoSourceEvidence.end()))==
			"76aae5b70860fea15b4cf33cad127559ade6d79628e17c37b1ef58d8076d8518"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			thermoSourceRaw.begin(),thermoSourceRaw.end()))==
			"c9f0188fdfd688e10f0edef4c7ef7e9712e4bf71d7cff19ddcba3b5276553865"&&
		thermoSourceEvidence.find("fixture_sha256 "
			"f0be92f1a5d70bea5729609396dc619f766388fce5b4ccd9e23810fc22a48693")!=
			std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fireSimulatorCore.begin(),fireSimulatorCore.end()))==
			"21a5cf2c6177cc535d9ff8ced6835a7b0b09b2fe19dfed887c76d7e89c61eae1"&&
		// r175's exact simulation/Metal owners are preserved by the sealed
		// artifact; r181 binds their superseding diagnostic revisions.
		thermoSourceEvidence.find("records_header_sha256 "
			"b73382076eafc153c4a2b058ed0fc247e4f14ffa20d9700df0aaa8e4b98b6f5c")!=
			std::string::npos&&
		thermoSourceEvidence.find("records_source_sha256 "
			"38762e15cde178da70e15f0762b3dbff1c2ebeaa93994ac78348960df6e84b42")!=
			std::string::npos&&
		// The r175 solver/sequence sources are likewise historical provenance;
		// their current successors are source-bound by r181.
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			unixTestDriver.begin(),unixTestDriver.end()))==
			"e37149731d65eb1c18e0b107150d040d78b3fc03a427d344463bf064985a251b"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fileEncoderObserverSource.begin(),fileEncoderObserverSource.end()))==
			"41e44da6fcb3bceec71e3dae8e6042319d619f33951d7e516e2da76dcbbacc4f"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fileEncoderObserverHeader.begin(),fileEncoderObserverHeader.end()))==
			"59f00ad9d7e6878da481103c0ca9ca5dbb441ecfd000015556b922b573dbb785"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fileRasterizerOutputShimTest.begin(),fileRasterizerOutputShimTest.end()))==
			"58863556252b7df8accefede0d262dc84b2726a9d4cce6c9f38eeb6338866169"&&
		// r186 adds the canonical-source strict-FP build rule.  Preserve the
		// historical r175 build owner through its already sealed evidence bytes.
		thermoSourceEvidence.find("make_rules_sha256 "
			"d81af70fb94b8714b1c4ee7cd9e89ba5f44347a74681bfa82fa266bd9c943625")!=
			std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightReadme.begin(),firstLightReadme.end()))==
			"d81ac9bbbee11082112a353494aa7ba141aa5432d6d67fa555b7c909de763017"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightSequenceManifest.begin(),firstLightSequenceManifest.end()))==
			"a1b708384ce6c4232f43ae8cbbe54334e1b9289c51cdd75714df34cbc27b0455"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightFrame4.begin(),firstLightFrame4.end()))==
			"96fb3a845132bcf3c13c472e4f936f7086506a04dd6e4c1c5ea98f286a457458"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightFrame5.begin(),firstLightFrame5.end()))==
			"b9b0dbdb2a58895a299b614d9296b14cf16f224142e1ebf07018794681a5b664"&&
		// The exact r175 narrative is artifact-bound; the living documents are
		// superseded and directly bound by r181.
		thermoSourceEvidence.find("source_producer_minimum_margin 112.55273459563601")!=
			std::string::npos&&
		thermoSourceEvidence.find("binary32_union_factor_epsilon32 960")!=
			std::string::npos&&
		thermoSourceEvidence.find("step_0_tail_cells_beginning_target 0")!=
			std::string::npos&&
		thermoSourceEvidence.find("step_1_tail_cells_beginning_target 1")!=
			std::string::npos&&
		thermoSourceEvidence.find(
			"step_1_tail_drain_m3_beginning_target 3.4288999032069217e-07")!=
			std::string::npos&&
		thermoSourceEvidence.find("dynamics_hard_bound_passed_both_steps true")!=
			std::string::npos&&
		thermoSourceEvidence.find("accepted_state_tokens_minted 2")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_accepted_steps 523")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_maximum_physical_temperature_K 2284.8533")!=
			std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightEXR.begin(),firstLightEXR.end()))==
			"bf02ffa4d99d435ee4c9dc01829233f763a3ee8a32c3653ea27d9e926e7182c9"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightEXRProvenance.begin(),firstLightEXRProvenance.end()))==
			"5e6187b90c718300a6000100d93c100b55c00814372a112e6d7d327b6e46b44e"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightDisplay.begin(),firstLightDisplay.end()))==
			"53b0cb296e75583fca6d196ca0f701f7032ce7626c0396b2353eedc4f295f3b3"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightDisplayProvenance.begin(),firstLightDisplayProvenance.end()))==
			"53f578f365ed960f7497278f167c860df111bf48a5b0608fd3436039a8e4f5f9"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightAnimation.begin(),firstLightAnimation.end()))==
			"994a5b1a2910f7da4d5b027469cab55b6fbc2226a9521cdd0312cb951eb277f9"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			firstLightAnimationProvenance.begin(),
			firstLightAnimationProvenance.end()))==
			"a7571ff50151de2da0a857bc76ee7eb1760f4d9d4a1e2ef2c0ad9ac45929ce1a"&&
		firstLightFramesBound&&firstLightAnimationPayload&&
		firstLightAnimationPayload->Find("artifact_fidelity")&&
		firstLightAnimationPayload->Find("artifact_fidelity")->GetText()==
			"display_derivative"&&firstLightAnimationLinks&&
		firstLightAnimationLinks->GetType()==RISE::RISECBOR64::Value::Array&&
		firstLightAnimationLinks->GetArray().size()==8u&&
		firstLightReadme.find("eight identity-bearing scene-linear FP32 EXR primaries")!=
			std::string::npos&&
		firstLightReadme.find("`display_derivative`, linked to the preview primaries")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_animation_frames 8")!=std::string::npos&&
		thermoSourceEvidence.find("first_light_animation_format GIF87a_ImageIO_LZW")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_renderer_source_revision "
			"f3b56b90e8349b0246b39d1d5f601b5afd416e30")!=std::string::npos&&
		thermoSourceEvidence.find("first_light_renderer_dirty_state dirty")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_renderer_diff_sha256 "
			"a82c56990355ff463a9b472da1f762982f94f1a1d09e0de3f1d70b841291348c")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_primary_schedule_content_sha256 "
			"f5cd5812f0548d97b9522d0135d227585fd2fbe5a104f620e000c0f6dd8059c2")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_terminal_primary_content_sha256 "
			"8eb3820aa8388a720b7d0174161bb033cca101dbda08e4a7dc321f2fa65f33a9")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_animation_visible_nonzero true")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_animation_temporal_change true")!=
			std::string::npos&&
		thermoSourceEvidence.find("first_light_animation_mode "
			"terminal_state_camera_orbit_and_dolly")!=std::string::npos&&
		thermoSourceEvidence.find(
			"first_light_animation_all_frames_visible_structured_plume true")!=
			std::string::npos&&
		thermoSourceEvidence.find(
			"first_light_animation_adjacent_frames_distinct true")!=std::string::npos&&
		thermoSourceEvidence.find(
			"first_light_animation_minimum_lit_area_range one_twentieth")!=
			std::string::npos&&
		thermoSourceEvidence.find(
			"first_light_animation_minimum_plume_centroid_excursion_display_pixels 2")!=
			std::string::npos&&
		thermoSourceEvidence.find("tier10_25s_device_hours 0.40872554073287515")!=
			std::string::npos&&
		thermoSourceEvidence.find("tier10_25s_wall_hours 0.63840048687616735")!=
			std::string::npos&&
		CountText(thermoSourceRaw,"THERMO_SOURCE_MAP_STEP step=")==2u&&
		CountText(thermoSourceRaw,"accepted=0")==0u&&
		thermoSourceRaw.find("THERMO_SOURCE_MAP_COMPLETE steps=2")!=std::string::npos&&
		fireProductionSourceKernel.find("CertifiedBinary32SourcePacket")!=
			std::string::npos&&
		advectionMetal.find("ValidateFireProductionCellSourceIncrement")!=
			std::string::npos&&
		productionSolverTest.find(
			"production source admission requires a certified ledger and terminal thermochemistry")!=
			std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r175_thermo_source_maps")!=
			std::string::npos&&
		solverDoc.find("### 7.56i Binary32 thermochemistry/source maps")!=
			std::string::npos&&
		historyDoc.find("r175 Binary32 thermo/source maps and preview release")!=
			std::string::npos,
		"r175 admits certified Binary32 sources, remeasures the burning tail, and releases first light");
	const std::string tier6SpectrumCostEvidence=ReadText(
		"rendered/fire_production_calibration/r177_tier6_spectrum_and_tier10_cost/"
		"tier6_spectrum_tier10_cost_evidence.v1");
	const std::string tier10SteadyProfile=ReadText(
		"rendered/fire_production_calibration/r177_tier6_spectrum_and_tier10_cost/"
		"tier10_steady_profile.raw");
	const std::string tier6FullSpectrum=ReadText(
		"rendered/fire_production_calibration/r177_tier6_puffing_spectrum/full_spectrum.csv");
	Check(!tier6SpectrumCostEvidence.empty()&&!tier10SteadyProfile.empty()&&
		!tier6FullSpectrum.empty()&&RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			tier6SpectrumCostEvidence.begin(),tier6SpectrumCostEvidence.end()))==
			"edea4d12db7c8529610a0e8933ccc842c2f938765fd927a183785b8fc60bdea4"&&
		// The tracked raw profile has subsequently acquired non-evidentiary
		// provenance text; preserve r177 through the hash sealed in its exact
		// evidence artifact rather than rebasing history onto mutable bytes.
		tier6SpectrumCostEvidence.find("tier10_steady_profile_raw_sha256 "
			"81dc56f84eb3c5780e1576a49dd0852f07445cf5dc949f6b967580ea1151524d")!=
			std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			tier6FullSpectrum.begin(),tier6FullSpectrum.end()))==
			"0e7f63c92d0f63d88dcf8cf4a5f69fd318f31dd6523f40581b3212e6771ab5ba"&&
		tier6SpectrumCostEvidence.find(
			"tier6_puffing_verdict expected_component_present_but_subdominant_to_slow_domain_mode")!=
			std::string::npos&&
		tier6SpectrumCostEvidence.find(
			"tier10_complete_owner_wall_projection_hours_before_later_state_retries 4.740059356967345")!=
			std::string::npos&&
		tier6SpectrumCostEvidence.find("request_layout_serial_parallel_exact_RED true")!=
			std::string::npos&&tier6SpectrumCostEvidence.find(
			"target_temperature_reinversion_all_cell_exact_RED true")!=std::string::npos&&
		tier6SpectrumCostEvidence.find("target_molecular_reuse_full_vector_exact_RED true")!=
			std::string::npos&&CountText(tier10SteadyProfile,"FIREPROFSTEP step=")==10u&&
		CountText(tier6FullSpectrum,"centerline_heat_release,")==257u&&
		CountText(tier6FullSpectrum,"display_lit_area,")==257u&&
		// r177's historic owner hashes remain in its sealed artifact; r182 binds
		// the current dyadic/sequence diagnostic owners.
		solverDoc.find("### 7.56j Tier-6 full spectrum and complete-owner cost correction (r177)")!=
			std::string::npos&&historyDoc.find(
			"r176/r177 true temporal tier-6 preview and puffing-spectrum diagnosis")!=
			std::string::npos,
		"r177 publishes both tier-6 spectra and corrects tier-10 complete-owner cost");
	const std::string tier10PhysicsStopEvidence=ReadText(
		"rendered/fire_production_calibration/r178_tier10_density_velocity_stop/"
		"tier10_density_velocity_stop_evidence.v1");
	const std::string tier10PhysicsDiagnostic=ReadText(
		"rendered/fire_production_calibration/r178_tier10_density_velocity_stop/"
		"tier10_checkpoint_physics_diagnostic.v1");
	Check(!tier10PhysicsStopEvidence.empty()&&!tier10PhysicsDiagnostic.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			tier10PhysicsStopEvidence.begin(),tier10PhysicsStopEvidence.end()))==
			"bf5baab9e374cecd6b2641a283538651622634844ee483c9d194db3a46d8a896"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			tier10PhysicsDiagnostic.begin(),tier10PhysicsDiagnostic.end()))==
			"fa3a37c9568e66c23bba0f232747418b7223a2680fe86780d012e3492dbfeb0e"&&
		tier10PhysicsStopEvidence.find(
			"localization_density_momentum_corruption_drives_advective_CFL true")!=
			std::string::npos&&
		tier10PhysicsStopEvidence.find("tier10_spectrum_formable false")!=
			std::string::npos&&
		tier10PhysicsStopEvidence.find("tier10_animation_claimed false")!=
			std::string::npos&&
		tier10PhysicsDiagnostic.find("maximum_velocity_m_per_s 255.64169311523438")!=
			std::string::npos&&
		tier10PhysicsDiagnostic.find(
			"source_probe_HRR_relative_ledger_error 3.3485973103748931e-07")!=
			std::string::npos&&
		tier10PhysicsDiagnostic.find("format13_retry_counters_persisted false")!=
			std::string::npos&&
		tier10PhysicsDiagnostic.find(
			"format13_per_step_cost_histories_persisted false")!=std::string::npos&&
		solverDoc.find("### 7.56k Tier-10 density/velocity physics stop (r178)")!=
			std::string::npos&&historyDoc.find(
			"r178 tier-10 density/velocity physics stop")!=std::string::npos,
		"r178 stops tier-10 before statistics and retains its pre-decomposition diagnostic");
	const std::string momentumDecompositionEvidence=ReadText(
		"rendered/fire_production_calibration/r179_r178_momentum_decomposition/"
		"momentum_decomposition_evidence.v1");
	const std::string momentumDecompositionRaw=ReadText(
		"rendered/fire_production_calibration/r179_r178_momentum_decomposition/"
		"momentum_decomposition.raw.csv");
	const std::string momentumExtremeState=ReadText(
		"rendered/fire_production_calibration/r179_r178_momentum_decomposition/"
		"checkpoint_extreme_state.v1");
	const std::string momentumDecompositionPlot=ReadText(
		"rendered/fire_production_calibration/r179_r178_momentum_decomposition/"
		"restoration_velocity_vs_dt.svg");
	Check(!momentumDecompositionEvidence.empty()&&!momentumDecompositionRaw.empty()&&
		!momentumExtremeState.empty()&&!momentumDecompositionPlot.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			momentumDecompositionEvidence.begin(),momentumDecompositionEvidence.end()))==
			"f6e80653cef5eff5b53f0faf02fb003c66d8faf80d5166693dbe053751378bd7"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			momentumDecompositionRaw.begin(),momentumDecompositionRaw.end()))==
			"5a22923813cde771452d3c00ad42f5496bff6d8d91030e2d76ba2a9b0a49d77a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			momentumExtremeState.begin(),momentumExtremeState.end()))==
			"45fbcf4e358af7c506508f6c431dfe2390f19e1adc6a990daf09a5189a5d58e7"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			momentumDecompositionPlot.begin(),momentumDecompositionPlot.end()))==
			"ae0c52f457b0cbfe9622df0341c947462f2a0d443583046cda0a1531ff870f8e"&&
		CountText(momentumDecompositionRaw,"\n")==9u&&
		momentumDecompositionEvidence.find("minimum_density_manifold_consistent false")!=
			std::string::npos&&
		momentumDecompositionEvidence.find("restoration_impulse_dominates false")!=
			std::string::npos&&
		momentumDecompositionEvidence.find("cap_binding false")!=std::string::npos&&
		momentumDecompositionEvidence.find(
			"maximum_restoration_to_physical_velocity_ratio 0.058162335108278389")!=
			std::string::npos&&
		momentumExtremeState.find(
			"maximum_momentum_velocity_compatibility_residual 9.6394360298290849e-06")!=
			std::string::npos&&
		solverDoc.find("### 7.56l r178 momentum decomposition and cap decision (r179)")!=
			std::string::npos&&historyDoc.find("r179 r178 momentum decomposition")!=
			std::string::npos,
		"r179 refutes restoration dominance before changing the tier-10 policy");
	const std::string projectionMomentumBudgetEvidence=ReadText(
		"rendered/fire_production_calibration/r180_projection_momentum_budget/"
		"projection_momentum_budget_stop.v1");
	const std::string projectionMomentumBudgetRaw=ReadText(
		"rendered/fire_production_calibration/r180_projection_momentum_budget/"
		"production_momentum_budget.raw.csv");
	const std::string projectionMomentumColumnRaw=ReadText(
		"rendered/fire_production_calibration/r180_projection_momentum_budget/"
		"production_momentum_budget.raw.csv.column.csv");
	Check(!projectionMomentumBudgetEvidence.empty()&&!projectionMomentumBudgetRaw.empty()&&
		!projectionMomentumColumnRaw.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionMomentumBudgetEvidence.begin(),projectionMomentumBudgetEvidence.end()))==
			"bf985132f8f3252211130953124d12424224b8932fefcea3681016119ea480f9"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionMomentumBudgetRaw.begin(),projectionMomentumBudgetRaw.end()))==
			"d660291c29a51ded842a83ccc6b417a640e66cbc9d136f1f1da49114d887e6d7"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionMomentumColumnRaw.begin(),projectionMomentumColumnRaw.end()))==
			"36f05800f031beeba75f6a09a5df02f99fc178194d3ea5e8ab3184cf87ee2b16"&&
		CountText(projectionMomentumBudgetRaw,"\n")==9u&&
		CountText(projectionMomentumColumnRaw,"\n")==1065u&&
		projectionMomentumBudgetEvidence.find(
			"every_force_substep_impulse_enters_same_step_projected_provisional_momentum true")!=
			std::string::npos&&
		projectionMomentumBudgetEvidence.find(
			"pre_registered_pressure_under_response_relative_to_buoyancy_confirmed false")!=
			std::string::npos&&
		projectionMomentumBudgetEvidence.find(
			"oracle_checkpoint_at_matched_time_preserved false")!=std::string::npos&&
		projectionMomentumBudgetEvidence.find(
			"later_oracle_checkpoint_substituted_as_matched_state false")!=std::string::npos&&
		solverDoc.find("### 7.56m Force-inclusive projection budget and preservation stop (r180)")!=
			std::string::npos&&historyDoc.find("r180 force-inclusive projection budget")!=
			std::string::npos,
		"r180 refutes both conditional projection remedies and stops on missing matched oracle state");
	const std::string onsetBudgetEvidence=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/"
		"onset_momentum_budget_evidence.v1");
	const std::string onsetBudgetSummary=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/onset_budget_summary.csv");
	const std::string onsetBudgetPlot=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/onset_budget.svg");
	const std::string onsetOracleBudget=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/oracle_tier10/"
		"matched_2p1s.raw.csv");
	const std::string onsetOracleColumn=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/oracle_tier10/"
		"matched_2p1s.raw.csv.column.csv");
	const std::string onsetFirstCandidate=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/"
		"tier10_front_coupled_final/onset_campaign_summary.v1");
	const std::string onsetFirstCandidateReplay=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/"
		"tier10_front_coupled_resume_final/onset_campaign_summary.v1");
	const std::string onsetSecondCandidate=ReadText(
		"rendered/fire_production_calibration/r181_onset_campaign/"
		"tier10_velocity_envelope_probe2/onset_campaign_summary.v1");
	auto sourceSHA=[](const char* path){const std::string value=ReadText(path);
		return RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(value.begin(),value.end()));};
	Check(!onsetBudgetEvidence.empty()&&!onsetBudgetSummary.empty()&&!onsetBudgetPlot.empty()&&
		!onsetOracleBudget.empty()&&!onsetOracleColumn.empty()&&!onsetFirstCandidate.empty()&&
		!onsetFirstCandidateReplay.empty()&&!onsetSecondCandidate.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetBudgetEvidence.begin(),onsetBudgetEvidence.end()))==
			"e09c37d72ad453e5d8cb7dded6442941852deed967880871733d3c254b263a19"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetBudgetSummary.begin(),onsetBudgetSummary.end()))==
			"c067a54d93967070f8ca018f3fdcdb03652eac15c502deba7c5dc6cf52c04dde"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetBudgetPlot.begin(),onsetBudgetPlot.end()))==
			"eb66d135b1b817785a2b3b7648dad38d310f8ff0f46f94b4a19b54929aa15e26"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetOracleBudget.begin(),onsetOracleBudget.end()))==
			"c3892692350338ef5da5e0cece9ccc59565e717f9d737f48bebd4e869f8dfdba"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetOracleColumn.begin(),onsetOracleColumn.end()))==
			"13cace37e4c9ae7e991386de0b969898b98a9b6aba5e7bd5a33dd5c0fde6c506"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetFirstCandidate.begin(),onsetFirstCandidate.end()))==
			"ae12480aebff94167d29e7acc564e2a1389c8c747b9fc0ff2c18ed0bb481d92c"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetFirstCandidateReplay.begin(),onsetFirstCandidateReplay.end()))==
			"5c9d39320cd8f3a3119c6b6755238bebdf0bd9f27961d540dac3fc28729b2daf"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			onsetSecondCandidate.begin(),onsetSecondCandidate.end()))==
			"c9b25f101242c94cbe97a1894e8e2484084d1dc012167342d0b9e74959858ac0"&&
		onsetBudgetEvidence.find("tier10_advective_focusing_confirmed true")!=
			std::string::npos&&
		onsetBudgetEvidence.find("first_candidate_verdict delayed_but_did_not_remove_cell_average_advective_focusing")!=std::string::npos&&
		onsetBudgetEvidence.find("second_candidate_verdict no_material_change_rejected")!=
			std::string::npos&&
		onsetBudgetEvidence.find("failed_candidates_retained_in_production false")!=
			std::string::npos&&
		onsetBudgetEvidence.find("oracle_2p2s_claimed false")!=std::string::npos&&
		onsetBudgetEvidence.find("tier10_full_window_authorized false")!=std::string::npos&&
		// The exact r181 artifact above binds its historical owners; current
		// successors are independently source-bound by the newest evidence rung.
		onsetBudgetEvidence.find("fire_simulation_solver_test_sha256 "
			"ffabd7edb2e518c313a58721dc48eb24537447461842ef15c4e90928ef750094")!=
			std::string::npos&&
		onsetBudgetEvidence.find("oracle_advance_header_sha256 "
			"24ad3b942b637bcb05deae6138d315d8b78414192266846bde7a243f2466a3c4")!=
			std::string::npos&&
		onsetBudgetEvidence.find("onset_plot_generator_sha256 "
			"ee6689f9518230db539a75a6bb0a0541e2eaeaa8497cd299fbf2d710b936397d")!=
			std::string::npos&&
		solverDoc.find("### 7.56n Runaway-onset and retained-state campaign (r181)")!=
			std::string::npos&&historyDoc.find("r181 retained onset and resolution diagnosis")!=
			std::string::npos,
		"r181 names fine-grid advective focusing, retains the matched oracle budget, and rejects two ineffective reconstruction candidates");
	const std::string compatibleMomentumEvidence=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"compatible_momentum_flux_evidence.v1");
	const std::string compatibleTier10=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier10_compatible_replay.raw.csv");
	const std::string compatibleTier10Column=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier10_compatible_replay.raw.csv.column.csv");
	const std::string compatibleTier6=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier6_control.raw.csv");
	const std::string compatibleTier6Column=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier6_control.raw.csv.column.csv");
	const std::string compatibleTier8=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier8_control.raw.csv");
	const std::string compatibleTier8Column=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier8_control.raw.csv.column.csv");
	const std::string compatibleFromZero=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_velocity_trajectory.csv");
	const std::string compatibleRetry=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_retry_trajectory.csv");
	const std::string compatibleMomentumEvidenceV2=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"compatible_momentum_flux_evidence.v2");
	const std::string compatibleFixedColumn=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier10_fixed_column_matched.raw.csv");
	const std::string compatibleFixedColumnCells=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"tier10_fixed_column_matched.raw.csv.column.csv");
	const std::string compatibleCorrectedFromZero=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_velocity_trajectory_boundary_corrected.csv");
	const std::string compatibleCorrectedRetry=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_retry_trajectory_boundary_corrected.csv");
	const std::string compatibleCorrectedSummary=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_onset_summary.v2");
	const std::string compatibleCorrectedThreshold15=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_threshold_15.raw.csv");
	const std::string compatibleCorrectedThreshold15Cells=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_threshold_15.raw.csv.column.csv");
	const std::string compatibleCorrectedThreshold30=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_threshold_30.raw.csv");
	const std::string compatibleCorrectedThreshold30Cells=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_threshold_30.raw.csv.column.csv");
	const std::string compatibleCorrectedThreshold60=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_threshold_60.raw.csv");
	const std::string compatibleCorrectedThreshold60Cells=ReadText(
		"rendered/fire_production_calibration/r182_compatible_momentum_flux/"
		"from_zero_threshold_60.raw.csv.column.csv");
	std::size_t compatibleFromZeroRows=0u;
	const double compatibleFromZeroMaximum=CSVColumnMaximum(
		compatibleFromZero,3u,compatibleFromZeroRows);
	std::size_t compatibleCorrectedFromZeroRows=0u;
	const double compatibleCorrectedFromZeroMaximum=CSVColumnMaximum(
		compatibleCorrectedFromZero,3u,compatibleCorrectedFromZeroRows);
	Check(!compatibleMomentumEvidence.empty()&&!compatibleTier10.empty()&&
		!compatibleTier10Column.empty()&&!compatibleTier6.empty()&&
		!compatibleTier6Column.empty()&&!compatibleTier8.empty()&&
		!compatibleTier8Column.empty()&&
		!compatibleFromZero.empty()&&!compatibleRetry.empty()&&
		!compatibleMomentumEvidenceV2.empty()&&!compatibleFixedColumn.empty()&&
		!compatibleFixedColumnCells.empty()&&!compatibleCorrectedFromZero.empty()&&
		!compatibleCorrectedRetry.empty()&&!compatibleCorrectedSummary.empty()&&
		!compatibleCorrectedThreshold15.empty()&&!compatibleCorrectedThreshold15Cells.empty()&&
		!compatibleCorrectedThreshold30.empty()&&!compatibleCorrectedThreshold30Cells.empty()&&
		!compatibleCorrectedThreshold60.empty()&&!compatibleCorrectedThreshold60Cells.empty()&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleMomentumEvidenceV2.begin(),compatibleMomentumEvidenceV2.end()))==
			"0698c8ff36d59d18246b89eb39aba352d3e18bf27499772e2f8d400de1abf053"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleMomentumEvidence.begin(),compatibleMomentumEvidence.end()))==
			"7b1835e5bb44e1d76e0a12c1c5d4d6f52f0449c1d3828db06e87d9d4f4130630"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleTier10.begin(),compatibleTier10.end()))==
			"faba70eccff894a37fe075ae1f0b3dabe709f143580918850863f09cb36cdc35"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleTier10Column.begin(),compatibleTier10Column.end()))==
			"ec1ffc3b6d5f744f45e2049714ebfdd2769034ba0e378a7eb5770e9e194f9c5a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleTier6.begin(),compatibleTier6.end()))==
			"2094bf5fa1364a8db68b1fa17f76f3eb51c9f302d897c32c860b2a365a051ed1"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleTier6Column.begin(),compatibleTier6Column.end()))==
			"24ee90cf1f1a0465b180b94c4070eb2522551ec11cbe95e1fff2851ac1c07d8b"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleTier8.begin(),compatibleTier8.end()))==
			"0cb0ab72baf0cb8cb6d3c65050ba2733ee15e9bd42623cb07a1bf873e74bbc2b"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleTier8Column.begin(),compatibleTier8Column.end()))==
			"e3a1f95186e343ea3642b50d31232a33f8de698eee5e8e1787eb96f773328641"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleFromZero.begin(),compatibleFromZero.end()))==
			"765e7f76cf87b7bf33b802144807bc6fb4cb5b5ac78fd58683599be5957db300"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleRetry.begin(),compatibleRetry.end()))==
			"004dad9b1cdb8299ec45188d11d44a845b80223a54c662ebca3924b9c37346f3"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleFixedColumn.begin(),compatibleFixedColumn.end()))==
			"81fc23a60b59e487379dd6640657e3eaa76a55cf8cdb42df529d106065e86c04"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleFixedColumnCells.begin(),compatibleFixedColumnCells.end()))==
			"339588f6d61d3bdb60fbfcd23b107af35a7b0ea164b9bc3e5f0b646d77ccdbe6"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedFromZero.begin(),compatibleCorrectedFromZero.end()))==
			"2e7f8b37256a059fb519256e9a4994716bb7f6f2e80ee1bdfa39cc21e1d39bbf"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedRetry.begin(),compatibleCorrectedRetry.end()))==
			"ea16647573c564d9e4a8bd8138b1314d335205a00ed56951c202848408a83614"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedSummary.begin(),compatibleCorrectedSummary.end()))==
			"53f14161fa910e5110d4a4443943635e6b74f12d20e6facbdbf52af4c58bc4f9"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedThreshold15.begin(),compatibleCorrectedThreshold15.end()))==
			"a258734f6bfd55b77d0617dfe8d091df288e20f2ae9d0342f552e291f21f7eb0"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedThreshold15Cells.begin(),
			compatibleCorrectedThreshold15Cells.end()))==
			"865a905ae2ac343a36229241a24c0e45994b0cd92899f20aaf610649613acf3d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedThreshold30.begin(),compatibleCorrectedThreshold30.end()))==
			"f2dbd2602bb02413b56296b233a443dfebf95a9004ca3c12c8abacc6a7452199"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedThreshold30Cells.begin(),
			compatibleCorrectedThreshold30Cells.end()))==
			"e309a234b4c6a6b6c4b4505660a6f454ba587aed2e33a11f3a7b2d5422b905b1"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedThreshold60.begin(),compatibleCorrectedThreshold60.end()))==
			"f2dbd2602bb02413b56296b233a443dfebf95a9004ca3c12c8abacc6a7452199"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			compatibleCorrectedThreshold60Cells.begin(),
			compatibleCorrectedThreshold60Cells.end()))==
			"e309a234b4c6a6b6c4b4505660a6f454ba587aed2e33a11f3a7b2d5422b905b1"&&
		CountText(compatibleTier10,"\n")==9u&&CountText(compatibleTier10Column,"\n")==1065u&&
		CountText(compatibleTier6,"\n")==9u&&CountText(compatibleTier6Column,"\n")==649u&&
		CountText(compatibleTier8,"\n")==9u&&CountText(compatibleTier8Column,"\n")==857u&&
		CountText(compatibleFromZero,"\n")==484u&&CountText(compatibleRetry,"\n")==12u&&
		compatibleFromZeroRows==483u&&
		compatibleFromZeroMaximum==5.340585708618164&&
		CountText(compatibleFixedColumn,"\n")==29u&&
		CountText(compatibleFixedColumnCells,"\n")==3725u&&
		CountText(compatibleCorrectedFromZero,"\n")==1223u&&
		CountText(compatibleCorrectedRetry,"\n")==26u&&
		CountText(compatibleCorrectedSummary,"\n")==25u&&
		CountText(compatibleCorrectedThreshold15,"\n")==2u&&
		CountText(compatibleCorrectedThreshold15Cells,"\n")==134u&&
		CountText(compatibleCorrectedThreshold30,"\n")==2u&&
		CountText(compatibleCorrectedThreshold30Cells,"\n")==134u&&
		CountText(compatibleCorrectedThreshold60,"\n")==2u&&
		CountText(compatibleCorrectedThreshold60Cells,"\n")==134u&&
		compatibleCorrectedFromZeroRows==1222u&&
		compatibleCorrectedFromZeroMaximum==4768055.0&&
		compatibleMomentumEvidence.find(
			"formula K_i=I_i(Phi_hat_g)*(u_i_L+u_i_R)/2")!=std::string::npos&&
		compatibleMomentumEvidence.find("pre_registered_63_class_advection_success false")!=
			std::string::npos&&
		compatibleMomentumEvidence.find("r136_current_trace_digest "
			"9eb30df0a2df6e19f61ba0093f78a7254f54a8144af1f4f940ad4ed3c172355a")!=
			std::string::npos&&
		compatibleMomentumEvidence.find("tier10_replay_column_advection_rate_max "
			"494.85054257978283")!=std::string::npos&&
		compatibleMomentumEvidence.find("from_zero_terminal_error "
			"production_manifold_beginning_deviation_exceeds_dynamics_bound")!=
			std::string::npos&&
		compatibleMomentumEvidence.find("r170_shadow_rederived false")!=std::string::npos&&
		compatibleMomentumEvidence.find("tier10_full_window_authorized false")!=
			std::string::npos&&
		compatibleTier10.find("2.1210542431799695,0,0.0014238557778298855")!=
			std::string::npos&&compatibleTier10.find(",494.85054257978283,")!=std::string::npos&&
		compatibleFromZero.find("483,0.79304888390470296,0.0010558557696640491,")!=
			std::string::npos&&
		compatibleRetry.find("0.79199302813503891,9,0.0010558610083535314")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"candidate_activation explicit_AttemptFireProductionCompatibleMomentumDiagnosticMetal_API")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"commuting_identity_pressure_open_wall_CPU_RED true")!=std::string::npos&&
		compatibleMomentumEvidenceV2.find("r136_current_trace_digest "
			"30cd8213578eee200e19e2c994ebf531b21f8b3bcf51567d92080b828764f6be")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"production_matched_column_advection_rate_max 321.34195540099722")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"oracle_matched_column_advection_rate_max 159.00892323854879")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"candidate_activation_summary_sha256 "
			"53f14161fa910e5110d4a4443943635e6b74f12d20e6facbdbf52af4c58bc4f9")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"corrected_candidate_from_zero_first_15_m_per_s_time_s 1.3978566413279623")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"corrected_candidate_from_zero_velocity_max_m_per_s 4768055")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"corrected_candidate_from_zero_terminal_represented_dt_s "
			"4.2156947377414156e-10")!=std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"corrected_candidate_from_zero_stop_reason "
			"first_accepted_velocity_threshold_crossing")!=std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"corrected_candidate_from_zero_next_step_CFL_not_measured true")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"corrected_candidate_from_zero_terminal_pressure_rate_max "
			"10998092339304464")!=std::string::npos&&
		compatibleMomentumEvidenceV2.find(
			"reviewed_hybrid_continuation_withdrawn true")!=
			std::string::npos&&
		compatibleCorrectedSummary.find("operator_mode compatible_momentum_diagnostic")!=
			std::string::npos&&
		compatibleCorrectedSummary.find("completed_target 0")!=std::string::npos&&
		compatibleCorrectedSummary.find("stop_reason velocity_threshold_crossing")!=
			std::string::npos&&
		compatibleCorrectedSummary.find("threshold_60_captured 1")!=
			std::string::npos&&
		compatibleCorrectedThreshold15.find(
			"1.3969225193141028,0,0.00093412201385945082")!=std::string::npos&&
		compatibleCorrectedThreshold30.find(
			"1.7316493930411525,7,4.2156947377414156e-10")!=std::string::npos&&
		compatibleMomentumEvidenceV2.find("v1_moving_column_comparison_withdrawn true")!=
			std::string::npos&&
		// r182 remains cryptographically immutable through its byte-sealed
		// evidence.  Never reinterpret these hashes as requirements on the live
		// r183 owners below.
		compatibleMomentumEvidenceV2.find("transport_header_sha256 "
			"cb9c127eb014a70605feedac9bd8cb0592b808821d0f2b5b402dfb5b209b0d8f")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("transport_source_sha256 "
			"d2f8236bd8bc9e6f065836a80768f51239efa025104f6ed1ac17c2095d64fb6a")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("advection_metal_sha256 "
			"00aaf43d64939db8c72cf2ba81b136b06d53fa0fba26556524e8dceede0164c0")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("force_header_sha256 "
			"0b30618a47a84d0dbabb0e629ea6db39f2339dd55ae10917eb7f97f001bd482f")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("force_unsupported_sha256 "
			"a04589f149c9851265029165b444cd47a7eef5aaa8243686718095e873d66f65")!=
			std::string::npos&&
		compatibleMomentumEvidence.find("force_source_sha256 "
			"d8bfdc76db1a44220c76b0997217ad13ea36a0e581c985ba718d6cacbad53501")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("solver_test_sha256 "
			"e1d7ed26757eb858db5af6b8d2ab76e6a4efa953e072e06bd7797cf9beff36b1")!=
			std::string::npos&&
		(sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"48be718bf8f012dc82fd6a8d7ce6744e0e826469c26a63c07c49b21ba8e1934e"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"e9e0e6e40ea51686a4f34e57ea0ae4102408986c2dc8221dd89ca9ca99c32a79"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"edfb22639f8dac34e0d6f66d021b5b6f4c9ece83fff848e81c1ac2a8b00b326e"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"a2e251a99fd40488ad3387e294b560040fbf41d5015f7ff77776e38faae6a8bf"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"f631e65c3447c6da2201f653955f1cc846e56b0a1a8a37ca19500bcbd651d805"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"df29df47bc44da5da23c37c1cd236e1e92b7163bc196f878e22fa4324ac8c908"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"d0f13862226b2d6da5cd14b2c6bff9b729516524be3d8c6049aa457d00ed5a2f"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"c9f616e09daae141f5cc6cd109e8573b92bc6aca6ba20b783ea9e597d554189f"||
		sourceSHA("tests/FireProductionDyadicCalibrationFixture.h")==
			"88ae7d5ee894db770dc622f1191feee06ee2aadd0a590e3c4773214f10a794fd")&&
		(sourceSHA("tests/FireProductionRoundoffWalker.h")==
			"22259ff8367aeb73ac5b73d8a282b23f18c61d545ca99cad14d856f9e40a4378"||
		sourceSHA("tests/FireProductionRoundoffWalker.h")==
			"43b9bceb346fbbc6fce4a9c889657da3a1d45fa66bd4d0f8ca35361d9c9b6406")&&
		compatibleMomentumEvidenceV2.find("sequence_test_sha256 "
			"96dad4db54da84d47803588eff014e88592a463bc3e41335564009a3ba05b122")!=
			std::string::npos&&
		sourceSHA("tests/FireProductionCalibrationMirror.h")==
			"be18f64d518f63c2f2c770be0535532eb6c26b5df65cc00b905d0d809c1e1709"&&
		(sourceSHA("tests/FireProductionRoundoffTraceAdapter.h")==
			"a4bf94c30688d8addbf1988c5873438f83a4b9e9a2003bc618102f055056a5fc"||
		sourceSHA("tests/FireProductionRoundoffTraceAdapter.h")==
			"6efe1f3ebdd1104fba5b9a0f44ea91edd734985073130dd458942900237e1b4a"||
		sourceSHA("tests/FireProductionRoundoffTraceAdapter.h")==
			"ecd2db837ef0d78ff0c20f0d4004478e90b9bbfffa1817ac471996467bb29bc3")&&
		compatibleMomentumEvidenceV2.find("fp64_manifest_sha256 "
			"d1372a3d1ca544bc63904e8d5f4f43a39c545f10b7ecd4a8c865fa5753701807")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("trace_manifest_sha256 "
			"efe255934e7edab90404826e45e397f8a50ea7d3b1656865426daa039317e5da")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("solver_document_sha256 "
			"3ebbce0bf891384f53930e609293dbdefb9c4f2e1388961c677f4ef267c55d84")!=
			std::string::npos&&
		compatibleMomentumEvidenceV2.find("history_document_sha256 "
			"40577c5c51741da1dd689480e6d0c0a84a46e144cea313027a5bd272b327e65a")!=
			std::string::npos&&
		compatibleMomentumEvidence.find("ordinary_production_compatible_flux_enabled false")!=
			std::string::npos&&
		solverDoc.find("### 7.56o Compatible momentum-flux conformance and measured stop (r182)")!=
			std::string::npos&&historyDoc.find("r182 §3.7 compatible momentum conformance")!=
			std::string::npos,
		"r182 retains the compatible momentum diagnostic and records the corrected fixed-column onset stop");
	const std::string projectedHeunEvidence=ReadText(
		"rendered/fire_production_calibration/r183_projected_heun_bootstrap/"
		"projected_heun_bootstrap_evidence.v1");
	const std::string projectedHeunLiveBinding=ReadText(
		"rendered/fire_production_calibration/r183_projected_heun_bootstrap/"
		"projected_heun_bootstrap_live_binding.v1");
	const std::string projectedHeunOwnerLiveBinding=ReadText(
		"rendered/fire_production_calibration/r190_projected_heun_owner/"
		"projected_heun_owner_live_binding.v1");
	const std::string metalContextLiveBinding=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"metal_context_live_binding.v1");
	const std::string residentEOSCandidateEvidence=ReadText(
		"rendered/fire_production_calibration/r199_resident_eos_candidate_identity/"
		"resident_eos_candidate_identity_evidence.v1");
	const std::string residentEOSCandidateLiveBinding=ReadText(
		"rendered/fire_production_calibration/r199_resident_eos_candidate_identity/"
		"resident_eos_candidate_identity_live_binding.v1");
	const std::string residentEOSRawEvidence=ReadText(
		"rendered/fire_production_calibration/r199_resident_eos_candidate_identity/"
		"resident_eos_metal_sweep.raw");
	const std::string residentTargetEvidence=ReadText(
		"rendered/fire_production_calibration/r200_authenticated_device_target_lineage/"
		"resident_target_lineage_evidence.v1");
	const std::string residentTargetLiveBinding=ReadText(
		"rendered/fire_production_calibration/r200_authenticated_device_target_lineage/"
		"resident_target_lineage_live_binding.v1");
	const std::string residentTargetRawEvidence=ReadText(
		"rendered/fire_production_calibration/r200_authenticated_device_target_lineage/"
		"resident_target_metal_sweep.raw");
	const std::string projectedHeunMetalOwnerEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"projected_heun_metal_owner_evidence.v2");
	const std::string projectedHeunMetalOwnerLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"projected_heun_metal_owner_live_binding.v2");
	const std::string projectedHeunMetalOwnerRaw=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"projected_heun_owner_gate.raw");
	const std::string projectedHeunMetalKernelSweep=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"kernel_sweep.v2.raw");
	const std::string projectedHeunIterationTraceEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201g_continuous_trajectory_gate.v1");
	const std::string projectedHeunIterationTraceLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201g_continuous_trajectory_live_binding.v1");
	const std::string projectedHeunIterationTraceRaw=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_3ebe2974/owner_gate.log");
	const std::string projectedHeunIterationKernelSweep=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_3ebe2974/kernel_sweep.log");
	const std::string projectedHeunResidentParentEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201i_resident_parent_trajectory_gate.v1");
	const std::string projectedHeunResidentParentLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201i_resident_parent_trajectory_live_binding.v1");
	const std::string projectedHeunResidentParentRaw=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_eb7d1d87/owner_gate.log");
	const std::string projectedHeunResidentParentKernelSweep=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_eb7d1d87/kernel_sweep.log");
	const std::string projectedHeunActualCandidateEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201j_actual_candidate_trajectory_gate.v1");
	const std::string projectedHeunActualCandidateLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201j_actual_candidate_trajectory_live_binding.v1");
	const std::string projectedHeunActualCandidateRaw=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_a0c3a824/owner_gate.log");
	const std::string projectedHeunActualCandidateKernelSweep=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_a0c3a824/kernel_sweep.log");
	const std::string projectedHeunFullFieldResidencyEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201k_full_field_residency_gate.v1");
	const std::string projectedHeunFullFieldResidencyLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201k_full_field_residency_live_binding.v1");
	const std::string projectedHeunFullFieldResidencyRaw=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_b9360ae6/owner_gate.log");
	const std::string projectedHeunFullFieldResidencyKernelSweep=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_b9360ae6/kernel_sweep.log");
	const std::string projectedHeunSetupStagedSourceEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201l_setup_staged_source_gate.v1");
	const std::string projectedHeunSetupStagedSourceLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201l_setup_staged_source_live_binding.v1");
	const std::string projectedHeunSetupStagedSourceRaw=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_784a266c/owner_gate.log");
	const std::string projectedHeunSetupStagedSourceKernelSweep=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_784a266c/kernel_sweep.log");
	const std::string projectedHeunTraceVerdictEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201m_owner_iteration_trace_verdict.v1");
	const std::string projectedHeunTraceVerdictLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201m_owner_iteration_trace_live_binding.v1");
	const std::string ownerCostLiveBinding=ReadText(
		"rendered/fire_production_calibration/r202_owner_cost/instrumentation_live_binding.v3");
	const std::string ownerCostGate=ReadText(
		"rendered/fire_production_calibration/r202_owner_cost/campaign_gate.v3");
	const std::string ownerCostOffRaw=ReadText(
		"rendered/fire_production_calibration/r202_owner_cost/exact_827608d5/fixture_off.log");
	const std::string ownerCostOnRaw=ReadText(
		"rendered/fire_production_calibration/r202_owner_cost/exact_827608d5/fixture_on.log");
	const auto ownerCostBound=[&](const char* path){return ownerCostLiveBinding.find(
		std::string("owner ")+path+" sha256 "+sourceSHA(path)+"\n")!=std::string::npos;};
	bool ownerCostAllEntriesBound=true;std::size_t ownerCostEntryCount=0u;
	std::istringstream ownerCostEntries(ownerCostLiveBinding);std::string ownerCostLine;
	while(std::getline(ownerCostEntries,ownerCostLine))if(ownerCostLine.rfind("owner ",0u)==0u){
		std::istringstream entry(ownerCostLine);std::string tag,path,shaTag,sha,extra;
		if(!(entry>>tag>>path>>shaTag>>sha)||shaTag!="sha256"||(entry>>extra)||
			sha!=sourceSHA(path.c_str()))ownerCostAllEntriesBound=false;
		++ownerCostEntryCount;
	}
	Check(sourceSHA("rendered/fire_production_calibration/r202_owner_cost/"
		"instrumentation_live_binding.v3")==
			"d617d2890e70da14836a03a4762465f4326f10a98c85a8a7df641828dfdf819c"&&
		sourceSHA("rendered/fire_production_calibration/r202_owner_cost/campaign_gate.v3")==
			"fe65fc28572ae1ab7ff6bf5561061f7c4de15c067b3067fa8b078d2d7a034c0c"&&
		ownerCostAllEntriesBound&&ownerCostEntryCount==49u&&
		ownerCostBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		ownerCostBound("tests/FireSequenceTest.cpp")&&
		ownerCostBound("tests/FireProductionOwnerConvergenceProbe.h")&&
		ownerCostBound("bin/tests/FireSequenceTest")&&
		ownerCostGate.find("full_picard_focusing_verdict unavailable\n")!=std::string::npos&&
		ownerCostGate.find("fixed_k_selected false\n")!=std::string::npos&&
		ownerCostGate.find("resident_payload_authority_relaxed false\n")!=std::string::npos&&
		ownerCostOffRaw.find("RISE_FIRE_OWNER_PROFILE_V1 ")==std::string::npos&&
		ownerCostOnRaw.find("RISE_FIRE_OWNER_PROFILE_V1 ")!=std::string::npos&&
		ownerCostOffRaw.find("OWNER_CONVERGENCE_PROBE passed=1 error=\n")!=std::string::npos&&
		ownerCostOnRaw.find("OWNER_CONVERGENCE_PROBE passed=1 error=\n")!=std::string::npos&&
		ownerCostOffRaw.find("RESIDENT_TARGET passed=1 ")!=std::string::npos&&
		ownerCostOnRaw.find("RESIDENT_TARGET passed=1 ")!=std::string::npos&&
		ownerCostOffRaw.find("PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
			"criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1\n")!=
			std::string::npos&&
		ownerCostOnRaw.find("PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
			"criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1\n")!=
			std::string::npos,
		"r202 binds observer-only owner instrumentation without promoting a prefix or selecting k");
	const std::string projectedHeunTraceVerdictRaw=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_2189762b/owner_gate.log");
	const std::string projectedHeunTraceVerdictKernelSweep=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_2189762b/kernel_sweep.log");
	const std::string projectedHeunTraceVerdictNumeric=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"repaired_exact_2189762b/numeric_trace.log");
	const std::string projectedHeunRejectedCausalEvidence=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201m_causal_authority_seal_gate.v1");
	const std::string projectedHeunRejectedCausalLiveBinding=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201m_causal_authority_seal_live_binding.v1");
	const std::string projectedHeunRejectedCausalDisposition=ReadText(
		"rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
		"r201m_causal_authority_seal_rejection.v1");
	const auto liveOwnerBound=[&](const std::string& path,const std::string& sha256) {
		const std::string current=sourceSHA(path.c_str());
		const bool bound=projectedHeunLiveBinding.find("owner "+path+" sha256 "+sha256+"\n")!=
			std::string::npos&&(current==sha256||projectedHeunOwnerLiveBinding.find(
				"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
				metalContextLiveBinding.find("owner "+path+" sha256 "+current+"\n")!=
				std::string::npos||residentEOSCandidateLiveBinding.find(
					"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
				residentTargetLiveBinding.find("owner "+path+" sha256 "+current+"\n")!=
					std::string::npos||projectedHeunMetalOwnerLiveBinding.find(
					"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
				projectedHeunIterationTraceLiveBinding.find(
					"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
			projectedHeunResidentParentLiveBinding.find(
				"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
			projectedHeunActualCandidateLiveBinding.find(
				"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
			projectedHeunFullFieldResidencyLiveBinding.find(
				"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
			projectedHeunSetupStagedSourceLiveBinding.find(
				"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
			projectedHeunTraceVerdictLiveBinding.find(
				"owner "+path+" sha256 "+current+"\n")!=std::string::npos||
			ownerCostLiveBinding.find("owner "+path+" sha256 "+current+"\n")!=std::string::npos);
		if(!bound)std::fprintf(stderr,"UNBOUND_R183_OWNER %s current=%s\n",
			path.c_str(),current.c_str());
		return bound;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunEvidence.begin(),projectedHeunEvidence.end()))==
			"425f7e27414fd5ba41e826c71d1ea556e6b29aa205c7447bdc8fc5594275859d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunLiveBinding.begin(),projectedHeunLiveBinding.end()))==
			"2a739cdc61fe928e74e3f2ce96f4f8da41cabe99a9ba4a3a0427f770262efc91"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunOwnerLiveBinding.begin(),projectedHeunOwnerLiveBinding.end()))==
			"0991603c42500bc8ff4314a93168eab8f34c3e73fb130a84671162f168247640"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			metalContextLiveBinding.begin(),metalContextLiveBinding.end()))==
			"ede952577d0b873cb758b0383461d4f9b9e71cb0e1ca340ce3d106e9fe6dc1ee"&&
		projectedHeunLiveBinding.find("schema rise.fire.production.projected_heun_bootstrap.live_binding.v1\n")!=std::string::npos&&
		projectedHeunLiveBinding.find("immutable_evidence_sha256 "
			"425f7e27414fd5ba41e826c71d1ea556e6b29aa205c7447bdc8fc5594275859d\n")!=
			std::string::npos&&
		projectedHeunLiveBinding.find("live_owner_count 36\n")!=std::string::npos&&
		projectedHeunLiveBinding.find("calibration_test_self_binding false\n")!=
			std::string::npos&&
		projectedHeunOwnerLiveBinding.find("live_owner_count 33\n")!=
			std::string::npos&&
		metalContextLiveBinding.find("live_owner_count 17\n")!=std::string::npos&&
		metalContextLiveBinding.find("calibration_test_self_binding false\n")!=
			std::string::npos&&
		(projectedHeunOwnerLiveBinding.find("owner tests/SourceHygieneTest.cpp sha256 "+
			sourceSHA("tests/SourceHygieneTest.cpp")+"\n")!=std::string::npos||
		projectedHeunMetalOwnerLiveBinding.find("owner tests/SourceHygieneTest.cpp sha256 "+
			sourceSHA("tests/SourceHygieneTest.cpp")+"\n")!=std::string::npos||
		projectedHeunIterationTraceLiveBinding.find(
			"owner tests/SourceHygieneTest.cpp sha256 "+
			sourceSHA("tests/SourceHygieneTest.cpp")+"\n")!=std::string::npos||
		projectedHeunResidentParentLiveBinding.find(
			"owner tests/SourceHygieneTest.cpp sha256 "+
			sourceSHA("tests/SourceHygieneTest.cpp")+"\n")!=std::string::npos)&&
		(projectedHeunOwnerLiveBinding.find("owner src/Library/Utilities/"
			"FireProductionAdvectionUnsupported.cpp sha256 "+sourceSHA(
			"src/Library/Utilities/FireProductionAdvectionUnsupported.cpp")+"\n")!=
			std::string::npos||residentTargetLiveBinding.find("owner src/Library/Utilities/"
			"FireProductionAdvectionUnsupported.cpp sha256 "+sourceSHA(
			"src/Library/Utilities/FireProductionAdvectionUnsupported.cpp")+"\n")!=
			std::string::npos)&&
		projectedHeunOwnerLiveBinding.find("owner tests/"
			"FireProductionDyadicCalibrationFixture.h sha256 "+sourceSHA(
			"tests/FireProductionDyadicCalibrationFixture.h")+"\n")!=
			std::string::npos&&
		projectedHeunOwnerLiveBinding.find("owner tests/"
			"FireProductionRoundoffTraceAdapter.h sha256 "
			"6efe1f3ebdd1104fba5b9a0f44ea91edd734985073130dd458942900237e1b4a\n")!=
			std::string::npos&&
		projectedHeunOwnerLiveBinding.find("owner src/Library/Utilities/"
			"FireSequence.cpp sha256 "+sourceSHA(
			"src/Library/Utilities/FireSequence.cpp")+"\n")!=std::string::npos&&
		projectedHeunOwnerLiveBinding.find("owner rendered/fire_production_calibration/"
			"r190_projected_heun_owner/r136_trace_repin_evidence.v1 sha256 "
			"f5f4ddc01cc0c6f2b3d1c3e2a6c66fd11c92d1e274295428b512dd40ba3e895e\n")!=
			std::string::npos&&
		liveOwnerBound("src/Library/Utilities/FireProductionTransport.h",
			"258b92cf142d609c9247942906710233cc52921795cc264f3efc8e4c47ce669e")&&
		liveOwnerBound("src/Library/Utilities/FireProductionTransport.cpp",
			"7788648726d46b23317355d819e545770745647e97afae8df212ca0a52124d22")&&
		liveOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm",
			"2554a0a41feaa9356d3ffb8c17b1a2d8520b0642c7975d4fa62947b4e9fede57")&&
		liveOwnerBound("src/Library/Utilities/FireProductionForce.h",
			"fb36821eae06f9250010edcb5ffaf63eb291f522bdc9254a9d1827ad496ca08c")&&
		liveOwnerBound("src/Library/Utilities/FireProductionForce.cpp",
			"a180f8adc20738759073486d85a36c401694e2c090175196a9262f0c290a4d65")&&
		liveOwnerBound("src/Library/Utilities/FireProductionForceMac.mm",
			"70c15a76a71e607b600ca8b369e1e0bdc0afc6590232a0cab612765591c7f591")&&
		liveOwnerBound("src/Library/Utilities/FireProductionForceUnsupported.cpp",
			"b28fade2a67676b562c6d840b3826cb7a89ffdb61b77ab0f55d750606d2eee83")&&
		liveOwnerBound("src/Library/Utilities/FireProductionProjection.h",
			"dddd52b82198c636240a6954c6e7d82d7d52eb2a9604d22aa55c621dc2d710ca")&&
		liveOwnerBound("src/Library/Utilities/FireProductionProjection.cpp",
			"8a9f5e648d43fdcea7fcad43f9681eaf5aee6b9f011246b5da6ec2b44ca628db")&&
		liveOwnerBound("src/Library/Utilities/FireProductionProjectionMac.mm",
			"6a7b3b70670be107b39ce873e6a4937019c56ed06ac94c85367b7a26c5449d6b")&&
		liveOwnerBound("src/Library/Utilities/FireSimulationRecords.h",
			"8e81fff299ee02af6cec1e9c3a117e19936492ae470c495bd6877cfb006e28dd")&&
		liveOwnerBound("src/Library/Utilities/FireSimulationRecords.cpp",
			"67b0bf8d90f79e733c04da1562a5c7e427cf6fa9d4ab0042c5c4308efdf4aaab")&&
		liveOwnerBound("src/Library/Utilities/FireCase.h",
			"48d640638cc1ee2704be3a880d72eba2e609ff6374ae7b50400a497ab3822f5d")&&
		liveOwnerBound("src/Library/Utilities/FireCase.cpp",
			"ccec8ac875bd2922217a90dad0c114cb2ef1e3ccab47c05bdc65208459adb003")&&
		liveOwnerBound("tools/generate_fire_production_fp64_mirror.py",
			"63470b975fe42d87755e9ce092504520baf52222c10f651d4ea6e2c868dc4d1f")&&
		liveOwnerBound("tools/generate_fire_production_roundoff_trace.py",
			"74a153037b83901fc2a994ff9d591827e2880f8130fd1d3130e15bcd51fed2fc")&&
		liveOwnerBound("tests/FireProductionGoldenCompositionFixture.h",
			"07fa3aa3f438fa5ea3c7e108a8a631120150837e026fffd419e791d85990da19")&&
		liveOwnerBound("tests/FireProductionSolverTest.cpp",
			"7d1964cea53d0f218be4659ac6b78082ce59412677ee7feb88ff404b758c8660")&&
		liveOwnerBound("tests/FireProductionProjectionTest.cpp",
			"bdd7ef8f865287270be6e2f8d48584630500f10f1da1f0121df63e3101873b61")&&
		liveOwnerBound("tests/FireSequenceTest.cpp",
			"1cadffc8309e50aafa0a85b83502c7918e2cfb9e2e9c5f398169e27d83a7ab40")&&
		liveOwnerBound("tests/fire_production_fp64/FireProductionTransport.h",
			"154a41bdb4a253e7273044ef3f05dddbb4a04ed4264e26511ea24d1798408899")&&
		liveOwnerBound("tests/fire_production_fp64/FireProductionTransport.cpp",
			"b5caa87c1d3548093f1f1904304bd9dbbc579a4a02f4c2b1dea316f9ccf4c438")&&
		liveOwnerBound("tests/fire_production_fp64/FireProductionForce.h",
			"6fde03d14af5e74a127ff2246b1f450c9966447161a51851d3779dd7b0ae7963")&&
		liveOwnerBound("tests/fire_production_fp64/FireProductionForce.cpp",
			"f2c2a939d1df07f8af10dc3573adb6972d76cefc4b58409a42f590d7f0d65b2c")&&
		liveOwnerBound("tests/fire_production_fp64/FireProductionProjection.h",
			"fce0ceb6c3110d290c756babb2e25d9bc959ee4003bf78e14cb17bc1189c4cac")&&
		liveOwnerBound("tests/fire_production_fp64/FireProductionProjection.cpp",
			"2611c42e43e5b2e7a4b32e0749bcfbf007d5c20ddc36babddfbfd2c34725c473")&&
		liveOwnerBound("tests/fire_production_fp64/SourceManifest.h",
			"0338dd6c7b55f906b46e25bf07aef4010af9ed20e88e6980e87d8955fc3dece5")&&
		liveOwnerBound("tests/fire_production_trace/FireProductionTransport.h",
			"f3eac8c0c710fbb2eb251322a0a346a1081b7696da6f09e12f6fb490ae919a7e")&&
		liveOwnerBound("tests/fire_production_trace/FireProductionTransport.cpp",
			"a30c764dcc20386eff7ff9303dda9fab2de94df0aebaf1ef0d38e1118bf77b23")&&
		liveOwnerBound("tests/fire_production_trace/FireProductionForce.h",
			"f05b1fa7c74982f036a8c07d1a0180adc4e8035079847628999e8239e0ed7358")&&
		liveOwnerBound("tests/fire_production_trace/FireProductionForce.cpp",
			"23792d610a1e118da3a5fdc917a299fe7df2b226317e097c211668d868ef9a87")&&
		liveOwnerBound("tests/fire_production_trace/FireProductionProjection.h",
			"7828fe103122e300e67abc33a47f2878536c6ae1714eb567a0bc486992bf2cfe")&&
		liveOwnerBound("tests/fire_production_trace/FireProductionProjection.cpp",
			"5cf59d615da1f3022faea6ea5614a85060849b702ad321a1b16b916f0574b384")&&
		liveOwnerBound("tests/fire_production_trace/SourceManifest.h",
			"27e19d81e934515477bf14c0e3608a7804a371b083adb21729c020e4d379a33f")&&
		liveOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md",
			"4031a1705f304dd504831f21c7ba631cbae7896a0f6026e2c8e9606b42de552f")&&
		liveOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md",
			"783f705f095d34c0bd1e8ff5b5c9aaae6d24f0aa9e40cb1c531df7e20ceb7ed1"),
		"r183 preserves its historical binding while r190 amendments bind every changed live owner without self-binding the calibration gate");
	const auto residentEOSOwnerBound=[&](const char* path) {
		const std::string binding=std::string("owner ")+path+" sha256 "+sourceSHA(path)+"\n";
		const bool bound=residentEOSCandidateLiveBinding.find(binding)!=std::string::npos||
			residentTargetLiveBinding.find(binding)!=std::string::npos||
			projectedHeunMetalOwnerLiveBinding.find(binding)!=std::string::npos||
			projectedHeunIterationTraceLiveBinding.find(binding)!=std::string::npos||
			projectedHeunResidentParentLiveBinding.find(binding)!=std::string::npos||
			projectedHeunActualCandidateLiveBinding.find(binding)!=std::string::npos||
			projectedHeunFullFieldResidencyLiveBinding.find(binding)!=std::string::npos||
			projectedHeunSetupStagedSourceLiveBinding.find(binding)!=std::string::npos||
			projectedHeunTraceVerdictLiveBinding.find(binding)!=std::string::npos||
			ownerCostLiveBinding.find(binding)!=std::string::npos;
		if(!bound)std::fprintf(stderr,"UNBOUND_R199_OWNER %s\n",path);
		return bound;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			residentEOSCandidateEvidence.begin(),residentEOSCandidateEvidence.end()))==
			"c2758e870e85328242e2b42ec13a3f4b9a1b1bbee55013051e33c9b9f7a4777b"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			residentEOSRawEvidence.begin(),residentEOSRawEvidence.end()))==
			"f67da702c6472f337da1b6856c237edb18514b27aa1965af7eaa0bc1db2f610c"&&
		ValidateResidentEOSRawEvidence(residentEOSRawEvidence,
			"293dd5e520f9f7c9d330da9e128177127a2b8b90f9d4b31d9c0a3d9b44b990d4",
			"6e409886fb98102be190cc90c4c460a1c1c5e15184a05d21f50442f3575e87b7")&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			residentEOSCandidateLiveBinding.begin(),residentEOSCandidateLiveBinding.end()))==
			"7009336399e88932f5b61a8629b25e1f7d0ec0db86728a982ecc0e4dd0ee0f01"&&
		residentEOSCandidateLiveBinding.find("calibration_test_self_binding false\n")!=
			std::string::npos&&residentEOSCandidateLiveBinding.find(
			"revision r199g_sealed\n")!=std::string::npos&&
		residentEOSCandidateLiveBinding.find("owner_count 14\n")!=
			std::string::npos&&
		residentEOSOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		residentEOSOwnerBound("src/Library/Utilities/FireProductionTransport.cpp")&&
		residentEOSOwnerBound("src/Library/Utilities/FireProductionTransport.h")&&
		residentEOSOwnerBound("tests/FireSequenceTest.cpp")&&
		residentEOSOwnerBound("tests/fire_production_fp64/FireProductionTransport.cpp")&&
		residentEOSOwnerBound("tests/fire_production_fp64/FireProductionTransport.h")&&
		residentEOSOwnerBound("tests/fire_production_fp64/SourceManifest.h")&&
		residentEOSOwnerBound("tests/fire_production_trace/FireProductionTransport.cpp")&&
		residentEOSOwnerBound("tests/fire_production_trace/FireProductionTransport.h")&&
		residentEOSOwnerBound("tests/fire_production_trace/SourceManifest.h")&&
		residentEOSOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		residentEOSOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		residentEOSOwnerBound("rendered/fire_production_calibration/"
			"r199_resident_eos_candidate_identity/resident_eos_metal_sweep.raw")&&
		residentEOSOwnerBound("rendered/fire_production_calibration/"
			"r199_resident_eos_candidate_identity/resident_eos_candidate_identity_evidence.v1")&&
		residentEOSCandidateEvidence.find("authority_producer "
			"Metal_device_complete_source_inclusive_FCT_QStar_commit\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"revision r199g_sealed\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find("rung_complete true\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find("review_boundary_closed true\n")!=
			std::string::npos&&
		residentEOSCandidateEvidence.find("public_candidate_input_present false\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"same_size_mismatched_EOS_table_refused true\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"complete_FCT_minimum_shared_alpha 0.832918644\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"complete_FCT_Metal_CPU_candidate_bit_equal true\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"complete_FCT_Metal_CPU_shared_face_alpha_bit_equal true\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"composite_subtraction_used false\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"withdrawn_bound 2^-41_termwise_arithmetic\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"arithmetic_enclosure connected_error_free_expansion_plus_outward_remainder\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"log_tail_denominator_rounding directed_down_subtraction_and_multiplication\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"fp64_std_log_projection_bound 2^-52_times_max_1_abs_log\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"fp64_std_log_qualification_total_inputs 48693249\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"fp64_std_log_provider_image /usr/lib/system/libsystem_m.dylib\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
				"metal_device_registry_id 0x000000010000087d\n")!=std::string::npos&&
			residentEOSCandidateEvidence.find("metal_device_name Apple_M4_Max\n")!=
				std::string::npos&&residentEOSCandidateEvidence.find(
				"metal_device_family apple9\n")!=std::string::npos&&
			residentEOSCandidateEvidence.find(
				"metal_runtime_bundle_identifier com.apple.Metal\n")!=std::string::npos&&
			residentEOSCandidateEvidence.find("metal_runtime_bundle_version 373.2\n")!=
				std::string::npos&&residentEOSCandidateEvidence.find(
				"metal_language_version 3.2\nmetal_math_mode safe\n")!=std::string::npos&&
			residentEOSCandidateEvidence.find(
				"metal_library_source_sha256 "
				"293dd5e520f9f7c9d330da9e128177127a2b8b90f9d4b31d9c0a3d9b44b990d4\n")!=
				std::string::npos&&residentEOSCandidateEvidence.find(
				"metal_library_function_set_sha256 "
				"6e409886fb98102be190cc90c4c460a1c1c5e15184a05d21f50442f3575e87b7\n")!=
				std::string::npos&&residentEOSCandidateEvidence.find(
				"metal_kernel_name diagnose_eos_log_enclosure\n")!=std::string::npos&&
			residentEOSCandidateEvidence.find(
				"metal_identity_consistent_across_all_batches true\n")!=std::string::npos&&
			residentEOSCandidateEvidence.find(
				"qualification_environment_change_requires_requalification "
				"CPU_libm_or_OS_or_Metal_device_or_runtime_or_language_or_math_mode_or_"
				"library_source_or_function_set_or_pipeline_traits\n")!=
				std::string::npos&&residentEOSCandidateEvidence.find(
			"binary32_publication whole_interval_must_fit_strictly_inside_one_rounding_bin\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
				"raw_transcript_sha256 f67da702c6472f337da1b6856c237edb18514b27aa1965af7eaa0bc1db2f610c\n")!=
			std::string::npos&&
		residentEOSCandidateEvidence.find(
			"deviation_acceptance device_bits_equal_fp64_mirror_binary32_projection_per_cell\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"zero_rounding_bin_center_accepted true\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find(
			"minimum_subnormal_rounding_bin_center_accepted true\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"host_preflight_refusal_observation no_publication_issued_no_device_attempt_no_terminal_read\n")!=
			std::string::npos&&
		residentEOSCandidateEvidence.find(
			"temperature_worst_residual_over_local_projection_enclosure=0 ")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"pressure_worst_residual_over_local_projection_enclosure=0 ")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"deviation_worst_residual_over_local_rounding_aware_enclosure=0 ")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"monitored_20_percent_case accepted\n")!=std::string::npos&&
		residentEOSCandidateEvidence.find("manifold_ceiling_reintroduced false\n")!=
			std::string::npos&&residentEOSCandidateEvidence.find(
			"interstage_full_grid_transfer_count 0\n")!=std::string::npos&&
		residentTargetEvidence.find("transfer_ledger_derived_from_host_read_count false\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"interstage_transfer_RED_observed_count 1\n")!=std::string::npos&&
		residentTargetEvidence.find("case_record_id_reason "
			"no_case_authored_semantic_or_input_change\n")!=std::string::npos&&
		residentTargetEvidence.find("golden_checkpoint_sha256 "
			"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947\n")!=
			std::string::npos&&
		residentEOSCandidateEvidence.find("isolated_r60_failure_bitmap 0x00000010\n")!=
			std::string::npos,
		"r199 binds complete resident QStar lineage, per-cell EOS projection, monitored-manifold scope, and its non-self-referential live owners");
	const auto residentTargetOwnerBound=[&](const char* path) {
		const std::string binding=std::string("owner ")+path+
			" sha256 "+sourceSHA(path)+"\n";
		const bool bound=residentTargetLiveBinding.find(binding)!=std::string::npos||
			projectedHeunMetalOwnerLiveBinding.find(binding)!=std::string::npos||
			projectedHeunIterationTraceLiveBinding.find(binding)!=std::string::npos||
			projectedHeunResidentParentLiveBinding.find(binding)!=std::string::npos||
			projectedHeunActualCandidateLiveBinding.find(binding)!=std::string::npos||
			projectedHeunFullFieldResidencyLiveBinding.find(binding)!=std::string::npos||
			projectedHeunSetupStagedSourceLiveBinding.find(binding)!=std::string::npos||
			projectedHeunTraceVerdictLiveBinding.find(binding)!=std::string::npos||
			ownerCostLiveBinding.find(binding)!=std::string::npos;
		if(!bound)std::fprintf(stderr,"UNBOUND_R200_OWNER %s\n",path);
		return bound;
	};
	const bool residentTargetEvidenceHashValid=RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(residentTargetEvidence.begin(),residentTargetEvidence.end()))==
		"f00212ef98f6bfc4fcc811238a23d86e3bfc5dde5b6d112c90faa77dcf4ad875";
	const bool residentTargetRawHashValid=RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(residentTargetRawEvidence.begin(),residentTargetRawEvidence.end()))==
		"b54ff82ed6d335e1501077b14ba7be81f94809804c363eab3e7a5a760c251c97";
	const bool residentTargetRawStructureValid=
		ValidateResidentTargetRawEvidence(residentTargetRawEvidence);
	const bool residentTargetEvidenceSemanticsValid=
		ValidateResidentTargetEvidenceAgainstRaw(residentTargetEvidence,residentTargetRawEvidence);
	const bool residentTargetBindingHashValid=RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(residentTargetLiveBinding.begin(),residentTargetLiveBinding.end()))==
		"61f2d2ee7f72d9765262faa1a9245d560c5414a32df3a6987ecf4ecdab7eeb90";
	Check(residentTargetEvidenceHashValid,"r200 evidence SHA binding");
	Check(residentTargetRawHashValid,"r200 raw transcript SHA binding");
	Check(residentTargetRawStructureValid,"r200 raw transcript structure and RED battery");
	Check(residentTargetEvidenceSemanticsValid,"r200 evidence fields reproduce raw transcript");
	Check(residentTargetBindingHashValid,"r200 live-owner binding SHA");
	Check(residentTargetEvidenceHashValid&&residentTargetRawHashValid&&
		residentTargetRawStructureValid&&residentTargetEvidenceSemanticsValid&&
		residentTargetBindingHashValid&&
		residentTargetLiveBinding.find("revision r200h_sealed\n")!=std::string::npos&&
		residentTargetLiveBinding.find("calibration_test_self_binding false\n")!=
			std::string::npos&&residentTargetLiveBinding.find("owner_count 19\n")!=
			std::string::npos&&
		residentTargetOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		residentTargetOwnerBound("src/Library/Utilities/FireProductionAdvectionUnsupported.cpp")&&
		residentTargetOwnerBound("src/Library/Utilities/FireProductionTransport.cpp")&&
		residentTargetOwnerBound("src/Library/Utilities/FireProductionTransport.h")&&
		residentTargetOwnerBound("tests/FireSequenceTest.cpp")&&
		residentTargetOwnerBound("tests/fire_production_fp64/FireProductionTransport.cpp")&&
		residentTargetOwnerBound("tests/fire_production_fp64/FireProductionTransport.h")&&
		residentTargetOwnerBound("tests/fire_production_fp64/SourceManifest.h")&&
		residentTargetOwnerBound("tests/fire_production_trace/FireProductionTransport.cpp")&&
		residentTargetOwnerBound("tests/fire_production_trace/FireProductionTransport.h")&&
		residentTargetOwnerBound("tests/fire_production_trace/FireProductionForce.cpp")&&
		residentTargetOwnerBound("tests/fire_production_trace/FireProductionForce.h")&&
		residentTargetOwnerBound("tests/fire_production_trace/SourceManifest.h")&&
		residentTargetOwnerBound("tools/generate_fire_production_roundoff_trace.py")&&
		residentTargetOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		residentTargetOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		residentTargetOwnerBound("rendered/fire_production_calibration/"
			"r200_authenticated_device_target_lineage/resident_target_metal_sweep.raw")&&
		residentTargetOwnerBound("rendered/fire_production_calibration/"
			"r200_authenticated_device_target_lineage/resident_eos_control.raw")&&
		residentTargetOwnerBound("rendered/fire_production_calibration/"
			"r200_authenticated_device_target_lineage/resident_target_lineage_evidence.v1")&&
		residentTargetEvidence.find("review_boundary_closed true\n")!=std::string::npos&&
		residentTargetEvidence.find("final_reviewed_commit a7e45b51\n")!=std::string::npos&&
		residentTargetEvidence.find("final_review_lineage zero_P1_P2\n")!=std::string::npos&&
		residentTargetEvidence.find("final_review_Metal_residency zero_P1_P2\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"final_review_numeric_mirror zero_P1_P2\n")!=std::string::npos&&
		residentTargetEvidence.find("parent_conjunction "
			"transport_and_physical_flux_and_candidate_and_EOS_and_candidate_bound_frozen_source\n")!=
			std::string::npos&&
		residentTargetEvidence.find("frozen_source_CPU_private_blit_forgeable false\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"wrong_beginning_canonical_source_refused true\n")!=std::string::npos&&
		residentTargetEvidence.find("wrong_case_control_canonical_source_refused true\n")!=
			std::string::npos&&residentTargetEvidence.find("target_EOS_thermochemistry_handle_binding "
			"exact_candidate_parent\n")!=std::string::npos&&residentTargetEvidence.find(
			"target_metadata_device_parent_checks shape_face_offsets_cell_width_timestep_attempt_boundary\n")!=
			std::string::npos&&
		residentTargetEvidence.find("r70_candidate_binding "
			"actual_projection_flux_produced_candidate_only\n")!=std::string::npos&&
		residentTargetEvidence.find("absolute_reference_diagnostic_policy "
			"monitored_recorded_not_summed\n")!=std::string::npos&&
		residentTargetEvidence.find("tail_threshold 2^-4\n")!=std::string::npos&&
		residentTargetEvidence.find("tail_magnitude_input "
			"r199_authoritative_absolute_EOS_deviation_publication\n")!=std::string::npos&&
		residentTargetEvidence.find("assembled_pressure_open_rounding "
			"preserve_tangent_and_tail_expansions_then_one_binary32_composition_projection\n")!=
			std::string::npos&&residentTargetEvidence.find("assembled_closed_rounding "
			"preserve_expansions_then_binary32_composition_projection_then_constant_mode_removal_then_binary32_compatibility_projection\n")!=
			std::string::npos&&residentTargetEvidence.find("trace_equal_rounded_different_lineage_RED passed\n")!=
			std::string::npos&&residentTargetEvidence.find("trace_one_bit_rounded_mutation_RED passed\n")!=
			std::string::npos&&residentTargetEvidence.find("trace_force_equal_rounded_different_lineage_RED passed\n")!=
			std::string::npos&&residentTargetEvidence.find("trace_force_one_bit_rounded_mutation_RED passed\n")!=
			std::string::npos&&residentTargetEvidence.find("trace_owner_equal_rounded_different_lineage_RED passed\n")!=
			std::string::npos&&residentTargetEvidence.find("trace_owner_one_bit_rounded_mutation_RED passed\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"projection_consumer_metadata_device_issued_to_private_surface true\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"projection_consumer_metadata_identity_bound_to_target_identity true\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"dormant_threshold_mutation_identity_distinct true\n")!=std::string::npos&&
		residentTargetEvidence.find("closed_required_branch_bitmap 0x03700180\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"pressure_open_required_branch_bitmap 0x02f00180\n")!=std::string::npos&&
		residentTargetEvidence.find("manifold_ceiling_reintroduced false\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"across_field_acceptance_summary_used false\n")!=std::string::npos&&
		residentTargetEvidence.find("bound_depends_on_measured_residual false\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"threshold_exact_equality_device_positive_no_drain true\n")!=std::string::npos&&
		residentTargetEvidence.find("threshold_exact_equality_device_negative_no_drain true\n")!=
			std::string::npos&&
		residentTargetEvidence.find("parent_EOS_current_library_requalified true\n")!=
			std::string::npos&&residentTargetEvidence.find("metal_library_source_sha256 "
			"276e9263b0ae8264ba917eff46e8e845ce2e46efdb52f13d755049950c8edd22\n")!=
			std::string::npos&&residentTargetEvidence.find("metal_library_function_set_sha256 "
			"55c16c115a76267ebf11956d769fbde27b9f245fd91fbd8374748be417440a9c\n")!=
			std::string::npos&&residentTargetEvidence.find("parent_EOS_control_sha256 "
			"5a8378021fc4cf9049c25ce58beb8093c4e76f615d0120e796c0395c369fd3a1\n")!=
			std::string::npos&&ValidateResidentEOSRawEvidence(ReadText(
			"rendered/fire_production_calibration/r200_authenticated_device_target_lineage/"
			"resident_eos_control.raw"),
			"276e9263b0ae8264ba917eff46e8e845ce2e46efdb52f13d755049950c8edd22",
			"55c16c115a76267ebf11956d769fbde27b9f245fd91fbd8374748be417440a9c")&&
		residentTargetEvidence.find("cancellation_sensitive_bound_used false\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"all_five_fields_both_topologies_bit_equal true\n")!=std::string::npos&&
		residentTargetEvidence.find("tolerance_widened false\n")!=std::string::npos&&
		residentTargetEvidence.find("unsealed_transport_parent_failure_bitmap "
			"0x00002800\n")!=std::string::npos&&residentTargetEvidence.find(
			"unsealed_frozen_source_parent_failure_bitmap 0x00002800\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"stale_target_metadata_failure_bitmap_each 0x00002800\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"preauthored_projection_target_failure_bitmap 0x00002000\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"mismatched_projection_topology_failure_bitmap 0x00002000\n")!=
			std::string::npos&&residentTargetEvidence.find(
			"interstage_full_grid_transfer_count 0\n")!=std::string::npos&&
		residentTargetEvidence.find("raw_transcript_sha256 "
			"b54ff82ed6d335e1501077b14ba7be81f94809804c363eab3e7a5a760c251c97\n")!=
			std::string::npos&&solverDoc.find(
			"### 7.56ag Authenticated resident target lineage (r200)")!=std::string::npos&&
		historyDoc.find("r200 authenticated resident target lineage")!=std::string::npos,
		"r200 binds each resident target term and the immediate r70 lineage without CPU substitution or hidden summed-field acceptance");
	const auto projectedHeunMetalOwnerBound=[&](const char* path) {
		const std::string record=std::string("owner ")+path+" sha256 "+sourceSHA(path)+"\n";
		return projectedHeunMetalOwnerLiveBinding.find(record)!=std::string::npos||
			projectedHeunIterationTraceLiveBinding.find(record)!=std::string::npos||
			projectedHeunResidentParentLiveBinding.find(record)!=std::string::npos||
			projectedHeunActualCandidateLiveBinding.find(record)!=std::string::npos||
			projectedHeunFullFieldResidencyLiveBinding.find(record)!=std::string::npos||
			projectedHeunSetupStagedSourceLiveBinding.find(record)!=std::string::npos||
			projectedHeunTraceVerdictLiveBinding.find(record)!=std::string::npos||
			ownerCostLiveBinding.find(record)!=std::string::npos;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunMetalOwnerEvidence.begin(),projectedHeunMetalOwnerEvidence.end()))==
			"b23000fbfe7fd1652ff042d2ff9fc9b0dccaecd77c6cfe056ee278935325f000"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunMetalOwnerLiveBinding.begin(),projectedHeunMetalOwnerLiveBinding.end()))==
			"c0bd54a706993978c33ec3b67ee66f951e590c614c502fdc05fb6113d6cc8aa0"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunMetalOwnerRaw.begin(),projectedHeunMetalOwnerRaw.end()))==
			"cd8a55ebbf02ee9494b5497b0302808ba7e5f789cfd912f0af2c653c00f47f51"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunMetalKernelSweep.begin(),projectedHeunMetalKernelSweep.end()))==
			"f9fc88904afff25ae4a72665dc9b1f2459a11047bc0d1a935b9fbd6578dc97a2"&&
		projectedHeunMetalOwnerLiveBinding.find("source_commit "
			"87bc6dee569b466aa7cd5c526de7d04807a49282\n")!=std::string::npos&&
		projectedHeunMetalOwnerLiveBinding.find("calibration_test_self_binding false\n")!=
			std::string::npos&&projectedHeunMetalOwnerLiveBinding.find("owner_count 44\n")!=
			std::string::npos&&
		projectedHeunMetalOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		projectedHeunMetalOwnerBound("src/Library/Utilities/FireProductionForce.h")&&
		projectedHeunMetalOwnerBound("src/Library/Utilities/FireProductionProjection.h")&&
		projectedHeunMetalOwnerBound("src/Library/Utilities/FireProductionProjectionMac.mm")&&
		projectedHeunMetalOwnerBound("tests/FireSequenceTest.cpp")&&
		projectedHeunMetalOwnerBound("tests/SourceHygieneTest.cpp")&&
		projectedHeunMetalOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		projectedHeunMetalOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		projectedHeunMetalOwnerEvidence.find("r1_momentum_weight 0.5_R0_plus_0.5_R1\n")!=
			std::string::npos&&projectedHeunMetalOwnerEvidence.find(
			"terminal_projection_uses_current_corrected_target true\n")!=std::string::npos&&
		projectedHeunMetalOwnerEvidence.find("stage_consumes_parent_authorities_by_seal true\n")!=
			std::string::npos&&projectedHeunMetalOwnerEvidence.find(
			"monitored_manifold_policy_single_source FireProductionMonitoredManifoldPolicy\n")!=
			std::string::npos&&projectedHeunMetalOwnerEvidence.find("fp64_primary_gate passed\n")!=
			std::string::npos&&projectedHeunMetalOwnerEvidence.find(
			"fp64_across_field_summary_used false\n")!=std::string::npos&&
		projectedHeunMetalOwnerEvidence.find("numeric_bound_uses_admissibility_slack false\n")!=
			std::string::npos&&projectedHeunMetalOwnerEvidence.find(
			"owner_interstage_full_grid_transfers 0\n")!=std::string::npos&&
		projectedHeunMetalOwnerEvidence.find("kernel_sweep_passed true\n")!=std::string::npos&&
		projectedHeunMetalOwnerRaw.find("PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 "
			"r0=1 r1=1 accepted=1 criterion=conjunction_of_per_cell_per_field_same_unit_enclosures "
			"error= passed=1\n")!=std::string::npos&&
		projectedHeunMetalOwnerRaw.find("RESIDENT_TARGET passed=1 ")!=std::string::npos&&
		projectedHeunMetalKernelSweep.find("RESIDENT_TRANSPORT_METAL_FP64 passed=1 ")!=
			std::string::npos&&projectedHeunMetalKernelSweep.find(
			"COMPATIBLE_MOMENTUM_METAL_FP64 passed=1 ")!=std::string::npos&&
		solverDoc.find("### 7.56ah Complete resident projected-Heun owner (r201b)")!=
			std::string::npos&&historyDoc.find("r201b resident-owner repair and exact gate")!=
			std::string::npos,
		"r201b binds the exact resident r190 owner, fp64 primary gate, transfer ledger, and separate kernel sweep");
	const auto r201gOwnerBound=[&](const char* path) {
		const std::string record=std::string("owner ")+path+" sha256 "+sourceSHA(path)+"\n";
		return projectedHeunIterationTraceLiveBinding.find(record)!=std::string::npos||
			projectedHeunResidentParentLiveBinding.find(record)!=std::string::npos||
			projectedHeunActualCandidateLiveBinding.find(record)!=std::string::npos||
			projectedHeunFullFieldResidencyLiveBinding.find(record)!=std::string::npos||
			projectedHeunSetupStagedSourceLiveBinding.find(record)!=std::string::npos||
			projectedHeunTraceVerdictLiveBinding.find(record)!=std::string::npos||
			ownerCostLiveBinding.find(record)!=std::string::npos;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunIterationTraceEvidence.begin(),
			projectedHeunIterationTraceEvidence.end()))==
			"441fa1dd2caf1b8721d622eea7f07de16d87aeb9e6ff0228640406dd52331d80"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunIterationTraceLiveBinding.begin(),
			projectedHeunIterationTraceLiveBinding.end()))==
			"f9f8077d2346230533803b02f3fa3f28308b4cceb2376a2c0b28ed13f0285eed"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunIterationTraceRaw.begin(),projectedHeunIterationTraceRaw.end()))==
			"a5f67e0cca7f9daba8971b51f536959e9f1c7c6d6204aeb45e6d31b884a71ed9"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunIterationKernelSweep.begin(),
			projectedHeunIterationKernelSweep.end()))==
			"dbf019efa5818fc597bcb813fbd92019c5dd9680a8c2ac8391d8398af7ccec99"&&
		projectedHeunIterationTraceLiveBinding.find("source_commit "
			"3ebe297421c72014e2767e54ff05c9c485ba7608\n")!=std::string::npos&&
		projectedHeunIterationTraceLiveBinding.find("owner_count 23\n")!=
			std::string::npos&&projectedHeunIterationTraceLiveBinding.find(
			"calibration_test_self_binding false\n")!=std::string::npos&&
		r201gOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		r201gOwnerBound("src/Library/Utilities/FireProductionTransport.cpp")&&
		r201gOwnerBound("src/Library/Utilities/FireProductionTransport.h")&&
		r201gOwnerBound("src/Library/Utilities/FireProductionForce.h")&&
		r201gOwnerBound("tests/FireSequenceTest.cpp")&&
		r201gOwnerBound("tests/fire_production_fp64/SourceManifest.h")&&
		r201gOwnerBound("tests/fire_production_trace/SourceManifest.h")&&
		r201gOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		r201gOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		projectedHeunIterationTraceEvidence.find("observed_reading continuous\n")!=
			std::string::npos&&projectedHeunIterationTraceEvidence.find(
			"observed_active_class_sequences_all_equal true\n")!=std::string::npos&&
		projectedHeunIterationTraceEvidence.find(
			"historical_r201_continuous_defect_numerically_refused true\n")!=
			std::string::npos&&
		projectedHeunIterationTraceEvidence.find("successor_envelope_activated false\n")!=
			std::string::npos&&projectedHeunIterationTraceEvidence.find(
			"acceptance per_quantity_per_cell_per_field_same_unit_enclosures true\n")!=
			std::string::npos&&projectedHeunIterationTraceEvidence.find(
			"ambiguous_predicate_without_authenticated_certificate_refused true\n")!=
			std::string::npos&&projectedHeunIterationTraceEvidence.find(
			"owner_branch_obligation_implementation not_implemented_under_continuous_ruling\n")!=
			std::string::npos&&
		projectedHeunIterationTraceEvidence.find(
			"alpha_nonadjacent_class_requires_both_separating_predicates true\n")!=
			std::string::npos&&projectedHeunIterationTraceEvidence.find(
			"observed_difference_used_to_inflate_bound false\n")!=std::string::npos&&
		projectedHeunIterationTraceEvidence.find("r201c_16383_ratio descriptive_not_acceptance\n")!=
			std::string::npos&&projectedHeunIterationTraceEvidence.find(
			"kernel_sweep_passed true\n")!=std::string::npos&&
		projectedHeunIterationTraceEvidence.find(
			"cpu_or_unverified_private_endpoint_refused true\n")!=std::string::npos&&
		projectedHeunIterationTraceEvidence.find(
			"noncrossing_predicate_without_authenticated_certificate_refused true\n")!=
			std::string::npos&&
		projectedHeunIterationTraceRaw.find("name=R2_physical_flux_consumes_sealed_input_class "
			"mutant_accepted=0 class_sequences_agree=0 ")!=std::string::npos&&
		projectedHeunIterationTraceRaw.find("name=recorded_r201_continuous_defect_rejected "
			"historical_residual_W_m^-2=10714.999585621501 ")!=std::string::npos&&
		projectedHeunIterationTraceRaw.find("alpha_nonadjacent_requires_both_thresholds=1 ")!=
			std::string::npos&&
		projectedHeunIterationTraceRaw.find("PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 "
			"r0=1 r1=1 accepted=1 criterion=conjunction_of_per_cell_per_field_same_unit_enclosures "
			"error= passed=1\n")!=std::string::npos&&
		projectedHeunIterationTraceRaw.find("RESIDENT_TARGET passed=1 ")!=std::string::npos&&
		projectedHeunIterationKernelSweep.find("RESIDENT_TRANSPORT_METAL_FP64 passed=1 ")!=
			std::string::npos&&projectedHeunIterationKernelSweep.find(
			"COMPATIBLE_MOMENTUM_METAL_FP64 passed=1 ")!=std::string::npos&&
		solverDoc.find("### r201g continuous owner trajectory and non-vacuous transport proof")!=
			std::string::npos&&historyDoc.find(
			"### r201g — the iteration trace selects the continuous contract")!=
			std::string::npos,
		"r201g binds the continuous trajectory, non-vacuous local enclosures, producer-authenticated endpoint classes, and exact green owner/kernel gates");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunResidentParentEvidence.begin(),
			projectedHeunResidentParentEvidence.end()))==
			"13897f0c7130ff3ca3016ca1e5844c0ead7acb21a8756f7173d9ff708effb3ed"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunResidentParentLiveBinding.begin(),
			projectedHeunResidentParentLiveBinding.end()))==
			"044395f39ff2c784d78e600d456a59cb664b4e838e4b5df23b382dc8de8eaa58"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunResidentParentRaw.begin(),projectedHeunResidentParentRaw.end()))==
			"736f9031cba40ee63d6baf0ac10182275bf33ac579b54399e34c849f514d3205"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunResidentParentKernelSweep.begin(),
			projectedHeunResidentParentKernelSweep.end()))==
			"57133981c0519ceeb0c642db35d049e9b6d26420a88a59092fcde74d6306907f"&&
		projectedHeunResidentParentLiveBinding.find("source_commit "
			"eb7d1d87ab9131bbdd6dc009a1ee679fbfd1e0be\n")!=std::string::npos&&
		projectedHeunResidentParentLiveBinding.find("owner_count 23\n")!=std::string::npos&&
		projectedHeunResidentParentLiveBinding.find("calibration_test_self_binding false\n")!=
			std::string::npos&&
		projectedHeunResidentParentEvidence.find(
			"actual_resident_parent_inside_trace true\n")!=std::string::npos&&
		projectedHeunResidentParentEvidence.find(
			"no_CPU_substitution_RED_cpu_surrogate_refused true\n")!=std::string::npos&&
		projectedHeunResidentParentEvidence.find(
			"provisional_momentum_observed_residual_used_to_author_bound false\n")!=
			std::string::npos&&projectedHeunResidentParentEvidence.find(
			"R2_open_head_observed_chord_used false\n")!=std::string::npos&&
		projectedHeunResidentParentEvidence.find("r60_affine_fallback_used false\n")!=
			std::string::npos&&projectedHeunResidentParentEvidence.find(
			"successor_envelope_activated false\n")!=std::string::npos&&
		projectedHeunResidentParentRaw.find(
			"name=cpu_flux_trajectory_substituted_for_resident_parent "
			"device_parent_certificate_passed=1 cpu_surrogate_refused=1 passed=1\n")!=
			std::string::npos&&projectedHeunResidentParentRaw.find(
			"PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
			"criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1\n")!=
			std::string::npos&&projectedHeunResidentParentRaw.find("RESIDENT_TARGET passed=1 ")!=
			std::string::npos&&projectedHeunResidentParentKernelSweep.find(
			"SCALAR_FCT_METAL_STAGES computed=1 passed=1 ")!=std::string::npos&&
		projectedHeunResidentParentKernelSweep.find(
			"COMPATIBLE_MOMENTUM_METAL_FP64 passed=1 ")!=std::string::npos&&
		solverDoc.find("### r201i resident-parent trajectory binding")!=std::string::npos&&
		solverDoc.find("Fresh review rejected r201i")!=std::string::npos&&
		historyDoc.find("### r201i — bind the trace to the resident parent trajectory")!=
			std::string::npos&&historyDoc.find("Fresh review rejected r201i before replay")!=
			std::string::npos,
		"r201i evidence remains immutable and is explicitly rejected by r201j");
	const auto r201jOwnerBound=[&](const char* path) {
		return projectedHeunActualCandidateLiveBinding.find(std::string("owner ")+path+
			" sha256 ")!=std::string::npos;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunActualCandidateEvidence.begin(),
			projectedHeunActualCandidateEvidence.end()))==
			"c1faf624d212657cc509137c4c3cd8df2118af63c29cd1b16e95b8cf564a6e5b"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunActualCandidateLiveBinding.begin(),
			projectedHeunActualCandidateLiveBinding.end()))==
			"2100d39017bccd143dc8d024d4efb671626cc19dea35f7f80c90ccd342a92150"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunActualCandidateRaw.begin(),projectedHeunActualCandidateRaw.end()))==
			"22e9f406a7682e1521eb3867af26ae4d84a4fc6487ce75ce47f05b8474492c45"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunActualCandidateKernelSweep.begin(),
			projectedHeunActualCandidateKernelSweep.end()))==
			"f3d88aa00e702192288f91e3130e3742dc81624e4b379bce00b3fe937a51386c"&&
		projectedHeunActualCandidateLiveBinding.find("source_commit "
			"a0c3a824df43229c87b0be9b230d2c58ddf15df8\n")!=std::string::npos&&
		projectedHeunActualCandidateLiveBinding.find("owner_count 25\n")!=std::string::npos&&
		projectedHeunActualCandidateLiveBinding.find("calibration_test_self_binding false\n")!=
			std::string::npos&&
		r201jOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		r201jOwnerBound("src/Library/Utilities/FireProductionForce.h")&&
		r201jOwnerBound("src/Library/Utilities/FireProductionForce.cpp")&&
		r201jOwnerBound("tests/FireSequenceTest.cpp")&&
		r201jOwnerBound("tests/FireProductionRoundoffWalker.h")&&
		r201jOwnerBound("tests/FireProductionRoundoffTraceAdapter.h")&&
		r201jOwnerBound("tests/fire_production_fp64/SourceManifest.h")&&
		r201jOwnerBound("tests/fire_production_trace/SourceManifest.h")&&
		r201jOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		r201jOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		r201jOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"r201j_actual_candidate_trajectory_gate.v1")&&
		r201jOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_a0c3a824/owner_gate.log")&&
		r201jOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_a0c3a824/kernel_sweep.log")&&
		r201jOwnerBound("bin/tests/FireSequenceTest")&&
		projectedHeunActualCandidateEvidence.find(
			"r201i_acceptance_status rejected_by_fresh_review\n")!=std::string::npos&&
		projectedHeunActualCandidateEvidence.find(
			"candidate_flux_source R0_device_physical_or_R1_device_ownerAverageFlux_output\n")!=
			std::string::npos&&projectedHeunActualCandidateEvidence.find(
			"successor_envelope_activated false\n")!=std::string::npos&&
		projectedHeunActualCandidateRaw.find(
			"name=wrong_device_averaged_flux_parent numerical_gate=reviewed_r190_fp64_owner "
			"mutant_accepted=1 first_field=0 first_index=493 ")!=std::string::npos&&
		projectedHeunActualCandidateRaw.find(
			"name=non_r60_invalid_arithmetic_publication_refused mutated_index=0 "
			"valid_looking_class_metadata=1 refused=1 passed=1\n")!=std::string::npos&&
		projectedHeunActualCandidateRaw.find(
			"PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
			"criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1\n")!=
			std::string::npos&&projectedHeunActualCandidateRaw.find("RESIDENT_TARGET passed=1 ")!=
			std::string::npos&&projectedHeunActualCandidateKernelSweep.find(
			"SCALAR_FCT_METAL_STAGES computed=1 passed=1 ")!=std::string::npos&&
		projectedHeunActualCandidateKernelSweep.find(
			"COMPATIBLE_MOMENTUM_METAL_FP64 passed=1 ")!=std::string::npos&&
		solverDoc.find("### r201j actual resident candidate trajectory")!=std::string::npos&&
		solverDoc.find("Fresh provenance review rejected r201j before replay")!=
			std::string::npos&&
		historyDoc.find("### r201j — bind R1 qualification to the actual averaged device candidate")!=
			std::string::npos&&historyDoc.find("Fresh provenance review rejected r201j before replay")!=
			std::string::npos,
		"r201j evidence remains immutable and is explicitly rejected by r201k");
	const auto r201kOwnerBound=[&](const char* path) {
		return projectedHeunFullFieldResidencyLiveBinding.find(std::string("owner ")+path+
			" sha256 ")!=std::string::npos;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunFullFieldResidencyEvidence.begin(),
			projectedHeunFullFieldResidencyEvidence.end()))==
			"7cdb33414f3932382f031459d51e53cb54f6910592c9ef9d353bf52d1e848cf2"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunFullFieldResidencyLiveBinding.begin(),
			projectedHeunFullFieldResidencyLiveBinding.end()))==
			"4588d98a31c95ad93c5c73242f829ca0c95ef5c3bb9bb751be03583c63b650b6"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunFullFieldResidencyRaw.begin(),
			projectedHeunFullFieldResidencyRaw.end()))==
			"4c9efd612b68dd3656dd64c5238e7d2acadda2d5b5f2255337f45f636b75e370"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunFullFieldResidencyKernelSweep.begin(),
			projectedHeunFullFieldResidencyKernelSweep.end()))==
			"5e5f95712004be93eb8ca1e82db1b9c12b561301054f1610fbe1f207460302e0"&&
		projectedHeunFullFieldResidencyLiveBinding.find("source_commit "
			"b9360ae690f8de19db2fa37a02bff1d9483efac5\n")!=std::string::npos&&
		projectedHeunFullFieldResidencyLiveBinding.find("owner_count 25\n")!=
			std::string::npos&&projectedHeunFullFieldResidencyLiveBinding.find(
			"calibration_test_self_binding false\n")!=std::string::npos&&
		r201kOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		r201kOwnerBound("src/Library/Utilities/FireProductionForce.h")&&
		r201kOwnerBound("src/Library/Utilities/FireProductionForce.cpp")&&
		r201kOwnerBound("tests/FireSequenceTest.cpp")&&
		r201kOwnerBound("tests/FireProductionRoundoffWalker.h")&&
		r201kOwnerBound("tests/FireProductionRoundoffTraceAdapter.h")&&
		r201kOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		r201kOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		r201kOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"r201k_full_field_residency_gate.v1")&&
		r201kOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_b9360ae6/owner_gate.log")&&
		r201kOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_b9360ae6/kernel_sweep.log")&&
		r201kOwnerBound("bin/tests/FireSequenceTest")&&
		projectedHeunFullFieldResidencyEvidence.find(
			"repair_full_grid_unit one_cell_field_cells_times_sizeof_float\n")!=
			std::string::npos&&projectedHeunFullFieldResidencyEvidence.find(
			"repair_directions Private_to_host_and_host_to_Private\n")!=std::string::npos&&
		projectedHeunFullFieldResidencyEvidence.find(
			"named_RED_observed_transfer_count 2\n")!=std::string::npos&&
		projectedHeunFullFieldResidencyEvidence.find(
			"named_RED_reaches_common_publication_gate true\n")!=std::string::npos&&
		projectedHeunFullFieldResidencyRaw.find(
			"name=single_field_bidirectional_transfer_ledger_common_publication_gate "
			"layer=device attempted=0 owner_identity=0 payload_words=0 staging=0 error="
			"projected-Heun atomic publication refuses interstage full-grid transfer: count=2 "
			"passed=1\n")!=std::string::npos&&
		projectedHeunFullFieldResidencyRaw.find(
			"PROJECTED_HEUN_METAL_OWNER_PRODUCTION_ENTRY accepted=1 token=1 token_matches=1 "
			"owner_identity=17359917141162959369 diagnostic_identity=17359917141162959369 "
			"monitored=1 enforced=0 transfers=0 staging=1 error= passed=1\n")!=
			std::string::npos&&projectedHeunFullFieldResidencyRaw.find(
			"PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
			"criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1\n")!=
			std::string::npos&&projectedHeunFullFieldResidencyKernelSweep.find(
			"SCALAR_FCT_METAL_STAGES computed=1 passed=1 ")!=std::string::npos&&
		projectedHeunFullFieldResidencyKernelSweep.find(
			"COMPATIBLE_MOMENTUM_METAL_FP64 passed=1 ")!=std::string::npos&&
		solverDoc.find("### r201k full-field residency publication gate")!=std::string::npos&&
		solverDoc.find("Fresh review rejected r201k before replay")!=std::string::npos&&
		historyDoc.find("### r201k — full-field residency is an atomic publication precondition")!=
			std::string::npos&&historyDoc.find("The r201k review found a second residency escape")!=
			std::string::npos,
		"r201k evidence remains immutable and is explicitly rejected by r201l");
	const auto r201lOwnerBound=[&](const char* path) {
		const std::string record=std::string("owner ")+path+" sha256 "+sourceSHA(path)+"\n";
		return projectedHeunSetupStagedSourceLiveBinding.find(record)!=std::string::npos||
			projectedHeunTraceVerdictLiveBinding.find(record)!=std::string::npos||
			ownerCostLiveBinding.find(record)!=std::string::npos;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunSetupStagedSourceEvidence.begin(),
			projectedHeunSetupStagedSourceEvidence.end()))==
			"cfef2717f2b886885e2a85b7f9a278d7fb793851ee8c0e3c04d38557912b8029"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunSetupStagedSourceLiveBinding.begin(),
			projectedHeunSetupStagedSourceLiveBinding.end()))==
			"58e178dcdccafa5395dccd5809eeaf7672b7c0eea75f6e177bb2129aacfb03da"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunSetupStagedSourceRaw.begin(),
			projectedHeunSetupStagedSourceRaw.end()))==
			"6c4ec360ed5124aa84155c534d323b12eb9973ce9bac864d32745151999b8ba8"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunSetupStagedSourceKernelSweep.begin(),
			projectedHeunSetupStagedSourceKernelSweep.end()))==
			"f603624633d493b4bd5c4d986cd649af967118dd695533d6ecdcf5f0924b4b7b"&&
		projectedHeunSetupStagedSourceLiveBinding.find("source_commit "
			"784a266c60e8db47ae4c08197e6b58192899dcbe\n")!=std::string::npos&&
		projectedHeunSetupStagedSourceLiveBinding.find("owner_count 25\n")!=
			std::string::npos&&projectedHeunSetupStagedSourceLiveBinding.find(
			"calibration_test_self_binding false\n")!=std::string::npos&&
		r201lOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		r201lOwnerBound("src/Library/Utilities/FireProductionForce.h")&&
		r201lOwnerBound("src/Library/Utilities/FireProductionForce.cpp")&&
		r201lOwnerBound("tests/FireSequenceTest.cpp")&&
		r201lOwnerBound("tests/FireProductionRoundoffWalker.h")&&
		r201lOwnerBound("tests/FireProductionRoundoffTraceAdapter.h")&&
		r201lOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		r201lOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		r201lOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"r201l_setup_staged_source_gate.v1")&&
		r201lOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_784a266c/owner_gate.log")&&
		r201lOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_784a266c/kernel_sweep.log")&&
		r201lOwnerBound("bin/tests/FireSequenceTest")&&
		projectedHeunSetupStagedSourceEvidence.find(
			"repair_frozen_source values_staged_once_during_Setup_into_Private\n")!=
			std::string::npos&&projectedHeunSetupStagedSourceEvidence.find(
			"repair_frozen_source_live_CPU_uploads 0\n")!=std::string::npos&&
		projectedHeunSetupStagedSourceEvidence.find(
			"named_RED_observed_transfer_count 5\n")!=std::string::npos&&
		projectedHeunSetupStagedSourceEvidence.find(
			"named_RED_reaches_common_publication_gate true\n")!=std::string::npos&&
		projectedHeunSetupStagedSourceRaw.find(
			"name=direct_source_upload_alpha_upload_and_bidirectional_transfer_common_"
			"publication_gate layer=device attempted=0 owner_identity=0 payload_words=0 staging=0 "
			"error=projected-Heun atomic publication refuses interstage full-grid transfer: count=5 "
			"passed=1\n")!=std::string::npos&&
		projectedHeunSetupStagedSourceRaw.find(
			"PROJECTED_HEUN_METAL_OWNER_PRODUCTION_ENTRY accepted=1 token=1 token_matches=1 "
			"owner_identity=17359917141162959369 diagnostic_identity=17359917141162959369 "
			"monitored=1 enforced=0 transfers=0 staging=1 error= passed=1\n")!=
			std::string::npos&&projectedHeunSetupStagedSourceRaw.find(
			"PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
			"criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1\n")!=
			std::string::npos&&projectedHeunSetupStagedSourceKernelSweep.find(
			"SCALAR_FCT_METAL_STAGES computed=1 passed=1 ")!=std::string::npos&&
		projectedHeunSetupStagedSourceKernelSweep.find(
			"COMPATIBLE_MOMENTUM_METAL_FP64 passed=1 ")!=std::string::npos&&
		solverDoc.find("### r201l setup-staged frozen-source authority")!=std::string::npos&&
		historyDoc.find("### r201l — setup owns the frozen-source upload")!=std::string::npos,
		"r201l stages the sealed source once and refuses every direct full-field upload at common publication");
	const auto r201mOwnerBound=[&](const char* path) {
		return projectedHeunTraceVerdictLiveBinding.find(std::string("owner ")+path+
			" sha256 "+sourceSHA(path)+"\n")!=std::string::npos||
			ownerCostLiveBinding.find(std::string("owner ")+path+
				" sha256 "+sourceSHA(path)+"\n")!=std::string::npos;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunRejectedCausalEvidence.begin(),
			projectedHeunRejectedCausalEvidence.end()))==
			"c23f30c8b830d9f47a116501467a7a91842fdbabfbec6688eecc96b1b889cf64"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunRejectedCausalLiveBinding.begin(),
			projectedHeunRejectedCausalLiveBinding.end()))==
			"13460ae80cba5cde3354bdc457745db923e7536a67ba9bccf7882706b29ca54b"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunRejectedCausalDisposition.begin(),
			projectedHeunRejectedCausalDisposition.end()))==
			"87f7abfd426ea5940ef2a69715e303b66c543e3e9bda6e0661a4dfb55a4cf471"&&
		projectedHeunRejectedCausalDisposition.find(
			"status rejected_before_replay\n")!=std::string::npos&&
		projectedHeunRejectedCausalDisposition.find(
			"disposition rejected_not_repaired\n")!=std::string::npos&&
		projectedHeunRejectedCausalDisposition.find(
			"forward_authority exact_payload_bound_r201l_implementation\n")!=
			std::string::npos,
		"r201m preserves the rejected causal-only evidence and binds its rejection disposition");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunTraceVerdictEvidence.begin(),
			projectedHeunTraceVerdictEvidence.end()))==
			"ab2665519c04b770653071784cf28ce4b5dfc64fcd68eae1a61e9a859b393ee5"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunTraceVerdictLiveBinding.begin(),
			projectedHeunTraceVerdictLiveBinding.end()))==
			"b779662fc12c9b8b033798cbce8b85bead13297a658355f4d79a42536a80b611"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunTraceVerdictRaw.begin(),projectedHeunTraceVerdictRaw.end()))==
			"90c82ad6df513f123494896a0ef510611170487cea00582839054f1b1e801727"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunTraceVerdictKernelSweep.begin(),
			projectedHeunTraceVerdictKernelSweep.end()))==
			"9b07e133d59711f91fdc5eee9c3492ec3424f9fe3bfd8fb1670e0116865564ce"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunTraceVerdictNumeric.begin(),
			projectedHeunTraceVerdictNumeric.end()))==
			"fcf6236caf0b0559084a013c93e2d1fd0f3987000449865fb3f58f1e2d52b77c"&&
		projectedHeunTraceVerdictLiveBinding.find("source_commit "
			"2189762be4f8322af746e31f8f177d76d1cf8bed\n")!=std::string::npos&&
		projectedHeunTraceVerdictLiveBinding.find("owner_count 16\n")!=std::string::npos&&
		projectedHeunTraceVerdictLiveBinding.find(
			"calibration_test_self_binding false\n")!=std::string::npos&&
		r201mOwnerBound("src/Library/Utilities/FireProductionAdvectionMac.mm")&&
		r201mOwnerBound("tests/FireSequenceTest.cpp")&&
		r201mOwnerBound("tests/FireProductionRoundoffWalker.h")&&
		r201mOwnerBound("tests/FireProductionRoundoffTraceAdapter.h")&&
		r201mOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md")&&
		r201mOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md")&&
		r201mOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"r201m_owner_iteration_trace_verdict.v1")&&
		r201mOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_2189762b/owner_gate.log")&&
		r201mOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_2189762b/kernel_sweep.log")&&
		r201mOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"repaired_exact_2189762b/numeric_trace.log")&&
		r201mOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"r201m_causal_authority_seal_gate.v1")&&
		r201mOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"r201m_causal_authority_seal_live_binding.v1")&&
		r201mOwnerBound("rendered/fire_production_calibration/r201_projected_heun_metal_owner/"
			"r201m_causal_authority_seal_rejection.v1")&&
		r201mOwnerBound("bin/tests/FireSequenceTest")&&
		projectedHeunTraceVerdictEvidence.find(
			"observed_first_divergence none\n")!=std::string::npos&&
		projectedHeunTraceVerdictEvidence.find(
			"observed_R0_R1_R2_field_records_passed 434_of_434\n")!=std::string::npos&&
		projectedHeunTraceVerdictEvidence.find(
			"observed_applicable_local_residual_bound_ratios_le_one 418_of_418\n")!=
			std::string::npos&&projectedHeunTraceVerdictEvidence.find(
			"owner_level_branch_obligation_extension_activated false\n")!=std::string::npos&&
		projectedHeunTraceVerdictEvidence.find(
			"r201c_16383_ratio descriptive_not_acceptance\n")!=std::string::npos&&
		projectedHeunTraceVerdictEvidence.find(
			"rejected_action exact_payload_bound_r201l_authority_restored\n")!=
			std::string::npos&&projectedHeunTraceVerdictRaw.find(
			"PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 "
			"criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1\n")!=
			std::string::npos&&projectedHeunTraceVerdictKernelSweep.find(
			"COMPATIBLE_MOMENTUM_METAL_FP64 passed=1 ")!=std::string::npos&&
		solverDoc.find("### r201m owner iteration-trace verdict")!=std::string::npos&&
		historyDoc.find("### r201m — the owner trace selects the existing strict contract")!=
			std::string::npos,
		"r201m binds the no-divergence owner trace and rejects the mutable causal-only authority experiment");
	const std::string authenticatedEOSEvidence=ReadText(
		"rendered/fire_production_calibration/r184_authenticated_eos_prerequisite/"
		"authenticated_eos_prerequisite.v1");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			authenticatedEOSEvidence.begin(),authenticatedEOSEvidence.end()))==
			"43ef57d8e0172d2d196cdd3aa5a845b43d128aa0ff404c964f3b1f125166075e"&&
		authenticatedEOSEvidence.find("caller_authored_temperature_ceiling_absent true")!=
			std::string::npos&&
		authenticatedEOSEvidence.find("retired_oracle_1e_minus_3_production_gate false")!=
			std::string::npos&&
		authenticatedEOSEvidence.find(
			"verdict accepted_cpu_only_eos_prerequisite_full_owner_still_blocked")!=
			std::string::npos,
		"r184 binds the authenticated CPU EOS prerequisite without promoting the projected-Heun owner");
	const std::string exactHeunFluxEvidence=ReadText(
		"rendered/fire_production_calibration/r185_exact_heun_flux_composition/"
		"exact_heun_flux_composition_evidence.v1");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			exactHeunFluxEvidence.begin(),exactHeunFluxEvidence.end()))==
			"609a373b1fff5bc80083499efd8f7cb5b7fdba75ce14617f0c3bd4ea394990a4"&&
		exactHeunFluxEvidence.find("persistent_payload_fields_per_face 30\n")!=
			std::string::npos&&
		exactHeunFluxEvidence.find("average_live_bytes_per_face 360\n")!=
			std::string::npos&&
		exactHeunFluxEvidence.find(
			"compatible_momentum_high_minus_low_reconstruction false\n")!=
			std::string::npos&&
		exactHeunFluxEvidence.find(
			"verdict accepted_cpu_only_exact_flux_composition_full_owner_still_blocked\n")!=
			std::string::npos,
		"r185 binds exact 30F composition and direct-delta momentum without promoting the full owner");
	const double baselineStep=static_cast<double>(0x1.e54eeep-10f);
	Check(baselineStep==0.0018513043178245425&&0.5*baselineStep==
		0.00092565215891227125&&0.25*baselineStep==0.00046282607945613563&&
		8.0*baselineStep==16.0*(0.5*baselineStep)&&
		8.0*baselineStep==32.0*(0.25*baselineStep),
		"r139 dyadic request steps land at one exactly represented horizon");
	const std::string restorationEvidence=ReadText(
		"rendered/fire_production_calibration/r118_restoration/restoration_evidence.v1");
	const std::string spatialEvidence=ReadText(
		"rendered/fire_production_calibration/r119_production_spatial/production_spatial_evidence.v1");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(restorationEvidence.begin(),
		restorationEvidence.end()))==
		"2752dc2075002911f8bec9bf909617fe8b4f641b9de3cbf20399475e0afaf451"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(spatialEvidence.begin(),
			spatialEvidence.end()))==
		"0c481de835c8dbf51044b7246000668fe39e4cde9832f36a95797f3b717eb8de",
		"r124 byte-binds the rerun r118 and r119 evidence artifacts");
	Check(unixTestDriver.find("FireProductionCalibrationOracle.r136")!=std::string::npos&&
		unixTestDriver.find("--fire-production-calibration-diagnose-roundoff")!=std::string::npos&&
		unixTestDriver.find("roundoff_rc\" -eq 237")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=237)")!=std::string::npos&&
		unixTestDriver.find("expected 237")!=std::string::npos&&
		windowsTestDriver.find("FireProductionCalibrationOracle.r136")!=std::string::npos&&
		windowsTestDriver.find("--fire-production-calibration-diagnose-roundoff")!=std::string::npos&&
		windowsTestDriver.find("roundoffRC -eq 237")!=std::string::npos&&
		windowsTestDriver.find("PASS (exact exit=237)")!=std::string::npos&&
		windowsTestDriver.find("expected 237")!=std::string::npos,
		"ordinary Unix and Windows suites execute r136 and accept only the full-step refusal");
	Check(unixTestDriver.find("FireSequenceTest.r138_subdominance")!=std::string::npos&&
		unixTestDriver.find("--fire-production-calibration-measure-subdominance")!=
			std::string::npos&&
		unixTestDriver.find("subdominance_rc\" -eq 243")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=243)")!=std::string::npos&&
		unixTestDriver.find("expected 243")!=std::string::npos,
		"ordinary macOS suite executes and exact-binds the retained r138 precision pilot");
	Check(unixTestDriver.find("FireSequenceTest.r142_burning_plateau_capacity")!=
			std::string::npos&&
		unixTestDriver.find("RISE_FIRE_RESTORATION_PLATEAU_PROBE=1")!=std::string::npos&&
		unixTestDriver.find("--fire-production-golden-composition")!=std::string::npos&&
		unixTestDriver.find("golden_refusal_rc\" -eq 253")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=253)")!=std::string::npos&&
		unixTestDriver.find("expected 253")!=std::string::npos,
		"ordinary macOS suite exact-binds the r142 burning capacity stop");
	const std::size_t noMetalTarget=makeRules.find(
		"$(PATHTESTDEST)FireProductionCalibrationOracle :");
	const std::size_t genericTestTarget=makeRules.find("$(PATHTESTDEST)% :");
	Check(noMetalTarget!=std::string::npos&&genericTestTarget!=std::string::npos&&
		noMetalTarget<genericTestTarget&&makeRules.find("OBJLIB_NOMETAL = $(filter-out")!=
		std::string::npos&&makeRules.find("FireProductionForceMac.o,$(OBJLIB))")!=
		std::string::npos&&makeRules.find("FireProductionForceUnsupported.o")!=
		std::string::npos&&makeRules.find("LDLIBS_NOMETAL = $(subst -framework Metal,,$(LDLIBS))")!=
		std::string::npos&&makeRules.find("otool -L $@ | grep -q 'Metal.framework'")!=
		std::string::npos&&makeRules.find("nm $@ | grep -q 'ProjectFireProductionMetalImpl'")!=
		std::string::npos,"calibration oracle target is source-bound to a no-Metal link audit");
	const std::size_t windowsTraceGenerator=windowsRules.find(
		"generate_fire_production_roundoff_trace.py\" --check");
	const std::size_t windowsTraceCondition=windowsRules.rfind(
		"if(test_name STREQUAL \"FireProductionCalibrationTest\"",windowsTraceGenerator);
	Check(makeRules.find("fire_production_trace/%.o")!=std::string::npos&&
		makeRules.find("-fno-fast-math -ffp-contract=off")!=std::string::npos&&
		makeRules.find("$(FIREPRODUCTIONTRACEOBJECTS) $(OBJDRISE)")!=std::string::npos&&
		makeRules.find("check-fire-production-trace")!=std::string::npos&&
		windowsRules.find("fire_production_trace/*.cpp")!=std::string::npos&&
		windowsTraceGenerator!=std::string::npos&&windowsTraceCondition!=std::string::npos&&
		windowsRules.find("test_name STREQUAL \"FireSequenceTest\"",windowsTraceCondition)<
			windowsTraceGenerator&&windowsRules.find("COMPILE_OPTIONS \"/fp:strict\"")!=
			std::string::npos,
		"roundoff trace is source-check-bound and strict on Make and Windows test surfaces");
	Check(walkerSource.find("RISEFireProductionTrace")==std::string::npos&&
		walkerSource.find("Counters")==std::string::npos&&
		walkerSource.find("fire_production_trace")==std::string::npos,
		"independent topology walker shares neither trace counts nor generated arithmetic code");
	Check(CountText(tracedTransportSource,"SealCellStageAndReset")==1u&&
		CountText(tracedTransportSource,"SealDualStageAndReset")==2u&&
		CountText(tracedTransportSource,"SealStageAndReset")==0u,
		"generated transport trace owns exactly the cell and dual stage-reset seams");
	Check(CountText(tracedProjectionSource,"ProjectionInterpolationScope")==1u&&
		tracedProjectionSource.find("topologyScope(fine,fineExtent,coarseExtent)")!=
			std::string::npos&&
		tracedProjectionSource.find("ScalarProfileScope profileScope(coarse.pressure,8u)")==
			std::string::npos&&
		walkerSource.find("CountProjectionInterpolationObligations")!=std::string::npos,
		"projection floor obligations use fixed-grid topology rather than a pressure envelope");
	Check(CountText(ReadText("tests/fire_production_trace/FireProductionAdvection.cpp"),
		"LocalTransportBranchScope branchScope")==2u&&
		CountText(ReadText("tests/fire_production_trace/FireProductionAdvection.cpp"),
			"FinalizeTransportBranchEnvelope")==4u&&
		traceCoreSource.find("value.ExpandRadius(ActiveCounters->"
			"transportBranchDivergenceBound)")==std::string::npos,
		"branch envelopes attach to their swept integral rather than every stage output");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		traceAdapterSource.begin(),traceAdapterSource.end()))==
		RISEFireProductionTrace::SourceManifest::TraceAdapter&&
		CountText(traceAdapterSource,"ObserveMetricRangeAndReset(")==3u&&
		traceAdapterSource.find("computed.force.momentumKGPerM2S[axis],0u,")!=
			std::string::npos&&
		traceAdapterSource.find("projection.velocityMPerS[axis],0u,"
			"projection.velocityMPerS[axis].size(),9u+axis")!=std::string::npos&&
		traceAdapterSource.find("computed.conservativeValues,component*cells,cells,")!=
			std::string::npos&&
		traceAdapterSource.find("for(const double radius:faceRadius[axis])")!=
			std::string::npos&&
		traceAdapterSource.find("0.5*(faceRadius")==std::string::npos&&
		CountText(tracedProjectionSource,"ProjectionSolveDependencyScope solveScope")==1u&&
		CountText(traceAdapterSource,"ProjectionSolveDependencyScope solveScope")==0u&&
		traceAdapterSource.find("maximumCrossPrecisionResidualUpper")!=std::string::npos&&
		traceAdapterSource.find("boundaryPressure64[side][index]")!=std::string::npos&&
		traceAdapterSource.find("dt64*gradient64")!=std::string::npos&&
		traceAdapterSource.find("for(const double radius:faceRadius64[axis])")!=
			std::string::npos&&
		traceAdapterSource.find("openActiveSetMatches")!=std::string::npos,
		"trace adapter identity and full binary64 terminal metric wiring are source-bound");

	{
		double derivedFactor=0.0,derivedWidth=0.0,mutantFactor=0.0,mutantWidth=0.0;
		Check(FireProductionRoundoffWalker::DeriveInflowTransitionWidth(
			1.7632415612658968e-38,22.033558699237727,1.0,
			derivedFactor,derivedWidth)&&derivedFactor==1.0&&
			derivedWidth==0x1p-24*22.033558699237727&&
			!FireProductionRoundoffWalker::DeriveInflowTransitionWidth(
				1.7632415612658968e-38,22.033558699237727,0.5,
				mutantFactor,mutantWidth)&&mutantFactor==1.0,
			"independent inflow walker derives the one-unit power-of-two width and rejects its half-width mutant");
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat sum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const FireProductionRoundoffTrace::TraceFloat one(1.0f),halfULP(0x1p-24f);
			sum=one+halfULP;
		}
		Check(counters.operation[static_cast<unsigned int>(
			FireProductionRoundoffTrace::Operation::Add)]==1u&&
			std::fabs(static_cast<double>(sum.Rounded())-sum.Center())<=sum.Radius(),
			"roundoff trace outward radius contains a binary32 tie-to-even addition");
	}
	{
		FireProductionRoundoffAdapter::ResidentStepTraceResult::ProjectionStreamingEvidence
			densityEvidence;
		FireProductionRoundoffAdapter::IncludeProjectionDensityEnvelope(
			FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.25,1.125f,0u),
			densityEvidence);
		Check(densityEvidence.densityLower<0.75&&densityEvidence.densityUpper>1.25,
			"projection spectral density envelope uses outward exact center-radius bounds");
	}
	{
		using B64=FireProductionRoundoffWalker::Binary64Interval;
		const auto normal=B64::Exact(0.125)/B64::Exact(1.1);
		const auto tangent=B64::Exact(0.5)*(B64::Exact(-0.25)+B64::Exact(0.75));
		const auto totalHead=B64::Exact(-0.5)*B64::Exact(1.2)*
			(normal*normal+tangent*tangent+tangent*tangent);
		const auto gradient=B64::Exact(2.0)*(B64::Exact(0.03125)-totalHead)/
			B64::Exact(0.04);
		const auto terminal=(B64::Exact(0.2)-B64::Exact(0.002)*gradient)/B64::Exact(1.1);
		Check(totalHead.Radius()>0.0&&gradient.Radius()>totalHead.Radius()&&
			terminal.Radius()>0.0,
			"independent binary64 interval walks total-head, gradient, and terminal division");
	}
	{
		const double radius=1.0;
		const double faceL2PerCell=std::sqrt((radius*radius+radius*radius)/2.0);
		const double cellCenteredRMS=std::sqrt(
			(0.25*radius*radius+0.25*radius*radius)/2.0);
		Check(faceL2PerCell==1.0&&cellCenteredRMS==0.5&&
			faceL2PerCell>cellCenteredRMS,
			"unique-face norm retains an endpoint-only divergence-free error hidden by cell centering");
	}
	{
		const std::array<std::size_t,3> extent={{24u,24u,36u}};
		const std::array<unsigned int,6> allOpen={{1u,1u,1u,1u,1u,1u}};
		FireProductionRoundoffWalker::ProjectionAposterioriCertificate certified,
			doublePoincare,missingCross,missingGate,missingFeedback,missingFace,
			missingTerminal,missingBeginning,zeroTarget,swappedDensity;
		const double h=0x1.4e288ep-5;
		Check(FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			certified)&&certified.dimensionlessEigenvalueLower==0.016975308641975297&&
			certified.operatorEigenvalueLower==8.9899277588426756&&
			certified.inverseOperatorNormUpper==0.11123559908658663&&
			certified.velocityGainUpper==0.47780216517115232&&
			certified.velocityRMSUpper>0.000125&&
			certified.validationPredicateSeparated&&certified.validationPredicateMarginLower>0.0&&
			certified.pressureOpenAnchor&&certified.allConstantsStructural,
			"independent Poincare/density/residual derivation pins the physical projection bound");
		Check(FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			doublePoincare,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				DoublePoincare)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingCross,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingCrossPrecisionResidual)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingGate,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFP64ResidualGate)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingFeedback,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFP64Feedback)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingFace,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFaceStreaming)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingTerminal,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFP64Terminal)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingBeginning,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingBeginningVelocity)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.0,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,zeroTarget)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			swappedDensity,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				SwappedDensityEnvelope)&&
			doublePoincare.velocityRMSUpper<certified.velocityRMSUpper&&
			missingCross.velocityRMSUpper<certified.velocityRMSUpper&&
			missingGate.velocityRMSUpper<certified.velocityRMSUpper&&
			missingFeedback.velocityRMSUpper<certified.velocityRMSUpper&&
			missingFace.velocityRMSUpper<certified.velocityRMSUpper&&
			missingTerminal.velocityRMSUpper<certified.velocityRMSUpper&&
			missingBeginning.velocityRMSUpper<certified.velocityRMSUpper&&
			zeroTarget.velocityRMSUpper<certified.velocityRMSUpper&&
			swappedDensity.velocityRMSUpper<certified.velocityRMSUpper,
			"projection derivation rejects spectrum, cross-residual, fp64-gate, face-norm, and density underbounds");
		FireProductionRoundoffTrace::Counters accepted;
		{
			FireProductionRoundoffTrace::Scope scope(accepted);
			FireProductionRoundoffTrace::ProjectionSolveDependencyScope solveScope;
			FireProductionRoundoffTrace::RecordArithmeticInvalidDomain();
		}
		FireProductionRoundoffTrace::BranchObligation obligation;
		obligation.site=FireProductionRoundoffTrace::BranchSite::ProjectionValidationBand;
		obligation.roundedResult=true;accepted.branchObligations.push_back(obligation);
		accepted.unresolvedBranch=true;
		for(unsigned int channel=9u;channel<12u;++channel){
			accepted.metricOutputCount[channel]=1u;
			accepted.nonfiniteMetricOutputRadiusCount[channel]=1u;}
		Check(FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
			accepted,0.001f,0.002f,0.0001,0.0001,certified.velocityRMSUpper)&&
			accepted.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Aposteriori&&
			accepted.aposterioriProjectionCertified&&!accepted.unresolvedBranch&&
			!accepted.invalidDomain&&accepted.aposterioriMetricBound[9]==
				certified.velocityRMSUpper,
			"accepted validation path consumes the structural residual certificate");
		FireProductionRoundoffTrace::Observation unrelatedInvalid=accepted;
		unrelatedInvalid.branchObligations.front().certificate=
			FireProductionRoundoffTrace::BranchCertificate::None;
		unrelatedInvalid.dischargedBranchObligationCount=0u;
		unrelatedInvalid.arithmeticInvalidDomainCount=2u;
		Check(!FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
			unrelatedInvalid,0.001f,0.002f,0.0001,0.0001,certified.velocityRMSUpper),
			"projection certificate cannot erase an unrelated arithmetic-domain failure");
		FireProductionRoundoffTrace::Observation rejected=accepted;
		rejected.branchObligations.front().certificate=
			FireProductionRoundoffTrace::BranchCertificate::None;
		rejected.branchObligations.front().roundedResult=false;
		rejected.dischargedBranchObligationCount=0u;
		Check(!FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
			rejected,0.001f,0.002f,0.0001,0.0001,certified.velocityRMSUpper)&&
			!FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
				rejected,0.001f,0.0011f,0.0002,0.0,certified.velocityRMSUpper),
			"rejected and over-band validation paths cannot borrow the accepted-solution anchor");
	}
	{
		const auto appendManifestText=[](RISE::RISECBOR64::Bytes& bytes,
			const char* value){
			const std::size_t length=std::strlen(value);
			for(unsigned int shift=0u;shift<64u;shift+=8u)
				bytes.push_back(static_cast<unsigned char>(
					(static_cast<std::uint64_t>(length)>>shift)&0xffu));
			bytes.insert(bytes.end(),value,value+length);
		};
		const auto manifestFields=
			FireProductionRoundoffAdapter::TraceSourceManifestFields();
		RISE::RISECBOR64::Bytes canonicalManifest;
		for(const char* field:manifestFields)appendManifestText(canonicalManifest,field);
		const std::string canonicalManifestDigest=
			RISE::RISECBOR64::SHA256Hex(canonicalManifest);
		bool everyManifestFieldBound=true;
		for(std::size_t field=0u;field<manifestFields.size();++field){
			RISE::RISECBOR64::Bytes mutatedManifest;
			for(std::size_t index=0u;index<manifestFields.size();++index)
				appendManifestText(mutatedManifest,index==field?
					"r136-dependency-mutation":manifestFields[index]);
			everyManifestFieldBound=everyManifestFieldBound&&
				RISE::RISECBOR64::SHA256Hex(mutatedManifest)!=canonicalManifestDigest;
		}
		Check(everyManifestFieldBound&&manifestFields.size()==
			FireProductionRoundoffAdapter::TraceSourceManifestFieldCount,
			"r136 trace identity changes for every primary, dependency, and support manifest field");
	}
	{
		FireProductionRoundoffWalker::FullStepAssumptionRefusal refusal;
		Check(FireProductionRoundoffWalker::RefuteFullStepCandidateAssumptions(refusal)&&
			refusal.conservativeCompressionL2Gain>1.0&&
			refusal.localizedProductRMS>refusal.productOfRMS&&
			refusal.sharedAlphaCrossComponentResponse>0.0,
			"r136 independently rejects r135's unit FCT gain, RMS product, and diagonal-component assumptions");
		std::array<FireProductionRoundoffWalker::FullStepMetricStage,24> stages={};
		for(const unsigned int stage:{1u,2u,3u,4u,5u,21u})for(unsigned int component=0u;
			component<9u;++component){stages[stage].count[component]=8u;
			stages[stage].radiusSum[component]=0.008;stages[stage].radiusSquareSum[component]=
				8.0e-6;stages[stage].maximumRadius[component]=0.001;}
		for(unsigned int axis=0u;axis<3u;++axis){const unsigned int channel=9u+axis;
			stages[0].count[channel]=8u;stages[0].radiusSquareSum[channel]=8.0e-8;
			for(unsigned int offset=0u;offset<5u;++offset){const unsigned int stage=6u+5u*axis+offset;
				stages[stage].count[channel]=8u;stages[stage].radiusSquareSum[channel]=8.0e-8;}}
		FireProductionRoundoffWalker::FullStepRoundoffCertificate certified,missingCell,
			missingDensity,missingPhysical,missingRestoration;
		const auto derive=[&](FireProductionRoundoffWalker::FullStepRoundoffCertificate& output,
			const FireProductionRoundoffWalker::FullStepGraphVariant variant){return
			FireProductionRoundoffWalker::DeriveFullStepRoundoffBound(stages,8u,0.9,1.2,0.04,
				0.002,1.1,0.2,1.0e-7,0.01,0.02,0.001,0.002,output,variant);};
		Check(!derive(certified,FireProductionRoundoffWalker::FullStepGraphVariant::Certified)&&
			!derive(missingCell,FireProductionRoundoffWalker::FullStepGraphVariant::MissingCellStage)&&
			!derive(missingDensity,FireProductionRoundoffWalker::FullStepGraphVariant::
				MissingDensityInteraction)&&!derive(missingPhysical,
				FireProductionRoundoffWalker::FullStepGraphVariant::MissingPhysicalFeedthrough)&&
			!derive(missingRestoration,FireProductionRoundoffWalker::FullStepGraphVariant::
				MissingRestorationFeedthrough)&&certified.proofGapBitmap==0xffu&&
			!certified.proofComplete&&missingCell.scalarFilteredL1Upper[0]<
				certified.scalarFilteredL1Upper[0]&&missingDensity.finalVelocityRMSUpper<
				certified.finalVelocityRMSUpper&&missingPhysical.finalVelocityRMSUpper<
				certified.finalVelocityRMSUpper&&missingRestoration.finalVelocityRMSUpper<
				certified.finalVelocityRMSUpper,
			"r135 candidate remains reproducible but fail-closes all eight proof gaps");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		bool copiedEqual=false,recomputedEqual=false;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const auto source=FireProductionRoundoffTrace::TraceFloat::Raw(
				1.0,0.25,1.0f,1u);
			const auto copied=source;
			const auto recomputed=FireProductionRoundoffTrace::TraceFloat::Raw(
				1.0,0.25,1.0f,1u);
			copiedEqual=copied==source;recomputedEqual=recomputed==source;
		}
		Check(copiedEqual&&recomputedEqual&&counters.comparisonCount==2u&&
			counters.branchObligations.size()==1u,
			"trace identity proves a canonical publication copy while retaining an obligation for independently recomputed equal bytes");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat converted;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			converted=FireProductionRoundoffTrace::TraceFloat(std::uint32_t(16777217u));
		}
		Check(counters.operation[static_cast<unsigned int>(
			FireProductionRoundoffTrace::Operation::Convert)]==1u&&
			converted.Rounded()==16777216.0f&&converted.Radius()>=1.0,
			"roundoff trace records and encloses an inexact integer conversion");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		const auto numerator=FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.0,1.0f,0u);
		const auto uncertainZero=FireProductionRoundoffTrace::TraceFloat::Raw(0.0,1.0,0.0f,0u);
		bool branchResult=false;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const auto quotient=numerator/uncertainZero;
			branchResult=uncertainZero<numerator;
			Check(!std::isfinite(quotient.Radius()),
				"invalid traced division publishes an infinite diagnostic radius");
		}
		Check(branchResult&&counters.invalidDomain&&counters.unresolvedBranch&&
			counters.operation[static_cast<unsigned int>(
				FireProductionRoundoffTrace::Operation::Divide)]==1u&&
			counters.maximumDepth==1u&&
			counters.minimumDenominatorLowerBound<=0.0&&
			counters.unresolvedWitnessRecorded&&counters.invalidDenominatorWitnessRecorded&&
			!FireProductionRoundoffWalker::IntervalsAreSeparated(
				counters.unresolvedLeftCenter,counters.unresolvedLeftRadius,
				counters.unresolvedRightCenter,counters.unresolvedRightRadius)&&
			counters.invalidDenominatorCenter==0.0&&
			counters.invalidDenominatorRadius>=1.0,
			"roundoff trace rejects denominator and branch intervals that cross a decision surface");
	}
	{
		FireProductionRoundoffWalker::BranchWitness witness;
		witness.leftCenter=-7.7486038219110043e-7;
		witness.leftRadius=1.2337798327030971e-6;
		witness.rightCenter=0.0;witness.rightRadius=0.0;
		FireProductionRoundoffWalker::PPMQuadraticZeroCertificate certificate;
		const bool certified=FireProductionRoundoffWalker::CertifyPPMQuadraticZero(
			witness,certificate);
		bool samplesContained=true;
		for(unsigned int sample=0u;sample<=64u;++sample){
			const double s=static_cast<double>(sample)/64.0;
			const double positive=FireProductionRoundoffWalker::PPMQuadraticPathDifference(
				certificate.ambiguityWidth,s);
			const double negative=FireProductionRoundoffWalker::PPMQuadraticPathDifference(
				-certificate.ambiguityWidth,s);
			samplesContained=samplesContained&&std::fabs(positive)<=
				certificate.divergenceBound&&std::fabs(negative)<=
				certificate.divergenceBound;
		}
		const double underBound=0.249*certificate.ambiguityWidth;
		Check(certified&&certificate.continuousAtSwitch&&samplesContained&&
			certificate.arithmeticResidualBound>0.0&&certificate.divergenceBound>
				0.25*certificate.ambiguityWidth&&
			FireProductionRoundoffWalker::PPMQuadraticPathDifference(0.0,0.375)==0.0&&
			std::fabs(FireProductionRoundoffWalker::PPMQuadraticPathDifference(
				certificate.ambiguityWidth,0.5))>underBound,
			"independent PPM branch certificate proves continuity and adds a four-operation rounding residual to the one-quarter envelope");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(
			-7.7486038219110043e-7,1.2337798327030971e-6,
			-7.152557373046875e-7f,1u);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(-1.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto linear=FireProductionRoundoffTrace::TraceFloat(0.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		bool nonzero=false;
		{
			FireProductionRoundoffTrace::Scope traceScope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			{
				FireProductionRoundoffTrace::BranchSiteScope branchScope(
					FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f;
			}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(
				q,linear,endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(nonzero&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			!counters.unresolvedBranch&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations[0].divergenceBound>0.0&&
			minimum.Radius()>0.0&&maximum.Radius()>0.0,
			"traced PPM obligation folds the independent equivalence envelope into both path outputs");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(
			1.0e-7,2.0e-7,1.0e-7f,1u);
		auto linear=FireProductionRoundoffTrace::TraceFloat::Raw(
			-1.0e-7,2.0e-7,-1.0e-7f,1u);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(-1.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			bool nonzero=false;
			{ FireProductionRoundoffTrace::BranchSiteScope site(
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f; }
			FireProductionRoundoffTrace::SetPPMQuadraticAmbiguous(
				FireProductionRoundoffTrace::PPMQuadraticZeroObligationPending());
			if(nonzero){FireProductionRoundoffTrace::CoveredBranchScope covered(true,true);
				auto stationary=-linear/(2.0f*q);
				bool lower=FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryLower,
					[&](){return stationary>0.0f;});
				const bool upper=lower&&FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryUpper,
					[&](){return stationary<1.0f;});
				if(upper){const auto value=(q*stationary+linear)*stationary;
					minimum=std::min(minimum,value);maximum=std::max(maximum,value);}}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(q,linear,
				endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(counters.branchObligations.size()==5u&&
			counters.dischargedBranchObligationCount==5u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero&&
			counters.branchObligations[1].site==
				FireProductionRoundoffTrace::BranchSite::PPMStationaryLower&&
			counters.branchObligations[2].site==
				FireProductionRoundoffTrace::BranchSite::PPMStationaryUpper&&
			counters.branchObligations[3].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[4].site==
				FireProductionRoundoffTrace::BranchSite::MaximumSelection&&
			std::isfinite(minimum.Radius())&&std::isfinite(maximum.Radius()),
			"ambiguous PPM path emits and parent-discharges its quadratic, stationary, and extrema-selector obligations with a finite envelope");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(
			1.0e-4,1.0e-8,1.0e-4f,1u);
		auto linear=FireProductionRoundoffTrace::TraceFloat::Raw(
			0.0,1.0e-8,0.0f,1u);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(-1.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			bool nonzero=false;
			{ FireProductionRoundoffTrace::BranchSiteScope site(
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f; }
			FireProductionRoundoffTrace::SetPPMQuadraticAmbiguous(
				FireProductionRoundoffTrace::PPMQuadraticZeroObligationPending());
			if(nonzero){FireProductionRoundoffTrace::CoveredBranchScope covered(true,true);
				auto stationary=-linear/(2.0f*q);
				FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryLower,
					[&](){return stationary>0.0f;});}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(q,linear,
				endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::PPMStationaryLower&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"resolved nonzero PPM parent still emits and explicitly discharges an ambiguous stationary predicate");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(4.0,1.0e-6,4.0f,1u);
		auto linear=FireProductionRoundoffTrace::TraceFloat::Raw(-4.0,1.0e-6,-4.0f,1u);
		const auto left=FireProductionRoundoffTrace::TraceFloat(1.0f);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(0.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			bool nonzero=false;
			{ FireProductionRoundoffTrace::BranchSiteScope site(
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f; }
			FireProductionRoundoffTrace::SetPPMQuadraticAmbiguous(
				FireProductionRoundoffTrace::PPMQuadraticZeroObligationPending());
			if(nonzero){FireProductionRoundoffTrace::CoveredBranchScope covered(true,true);
				auto stationary=-linear/(2.0f*q);
				const bool lower=FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryLower,
					[&](){return stationary>0.0f;});
				const bool upper=lower&&FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryUpper,
					[&](){return stationary<1.0f;});
				if(upper){const auto value=(q*stationary+linear)*stationary+left;
					minimum=std::min(minimum,value);maximum=std::max(maximum,value);}}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(q,linear,
				endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(counters.comparisonCount==5u&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			std::isfinite(minimum.Radius())&&std::isfinite(maximum.Radius()),
			"resolved PPM parent and stationary predicates still emit and parent-discharge an ambiguous extrema selector");
	}
	{
		double lower=0.0,upper=0.0,divergence=0.0;
		const bool walked=FireProductionRoundoffWalker::ContinuousSelectionHull(
			1.0,0.25,1.1,0.2,true,lower,upper,divergence);
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat selected;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			selected=std::min(FireProductionRoundoffTrace::TraceFloat::Raw(
				1.0,0.25,1.0f,1u),FireProductionRoundoffTrace::TraceFloat::Raw(
				1.1,0.2,1.1f,1u));
		}
		Check(walked&&lower==0.75&&upper==1.25&&divergence>0.45&&
			counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			selected.Center()-selected.Radius()<=lower&&
			selected.Center()+selected.Radius()>=upper,
			"continuous min selector is independently hulled and discharged across an ambiguous predicate");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::CoveredBranchScope covered(true);
			const auto resolved=std::min(FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat(2.0f));
			const auto ambiguous=std::max(
				FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.25,1.0f,1u),
				FireProductionRoundoffTrace::TraceFloat::Raw(1.1,0.2,1.1f,1u));
			Check(resolved.Rounded()==1.0f&&ambiguous.Radius()>0.0,
				"covered-path selector fixtures execute both resolved and ambiguous cases");
		}
		Check(counters.comparisonCount==2u&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::MaximumSelection,
			"covered PPM path counts every selector and emits the continuous hull obligation");
	}
	Check(FireProductionRoundoffWalker::CertifyInactiveLimiter(0.75,0.0031,0.004)&&
		!FireProductionRoundoffWalker::CertifyInactiveLimiter(0.75,0.0029,0.004),
		"independent limiter certificate requires the envelope ratio to dominate the shared limiter");
	{
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat limited;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			limited=FireProductionRoundoffTrace::ApplyLimiterBranch(
				FireProductionRoundoffTrace::TraceFloat::Raw(0.5,0.1,0.5f,1u),
				FireProductionRoundoffTrace::TraceFloat(2.0f),
				FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat::Raw(
					1.0e-7,2.0e-7,1.0e-7f,1u),true);
		}
		Check(limited.Rounded()==0.5f&&!counters.invalidDomain&&
			counters.comparisonCount==2u&&counters.branchObligations.size()==2u&&
			counters.dischargedBranchObligationCount==2u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::LimiterPositive&&
			counters.branchObligations[1].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[1].divergenceBound==0.0,
			"limiter no-effect proof owns and explicitly discharges its nested selector without an infinite hull");
	}
	{
		const float positiveScale=22372.0f,negativeScale=1.002f;
		const double positiveAmbiguity=9292.2824737527444*0x1p-24*
			static_cast<double>(positiveScale);
		const double negativeAmbiguity=8645.4622206683161*0x1p-24*
			static_cast<double>(negativeScale);
		FireProductionRoundoffWalker::ContinuousLimiterCertificate positive,negative;
		const bool positiveCertified=FireProductionRoundoffWalker::
			CertifyContinuousLimiterTransition(positiveAmbiguity,0.0,
				positiveScale,positiveScale,0.0f,positive);
		const bool negativeCertified=FireProductionRoundoffWalker::
			CertifyContinuousLimiterTransition(negativeAmbiguity,0.0,
				negativeScale,negativeScale,0.0f,negative);
		Check(positiveCertified&&negativeCertified&&
			positive.requiredUnitFactor<16384.0&&
			negative.requiredUnitFactor<16384.0&&
			positive.continuousAtNegativeWidth&&positive.continuousAtZero&&
			positive.continuousAtPositiveWidth&&
			positive.monotoneForPositiveConsumption,
			"independent r59 limiter walker proves the power-of-two transition contains both frozen site classes and preserves every join");
		FireProductionRoundoffWalker::ContinuousLimiterCertificate undercount;
		Check(!FireProductionRoundoffWalker::CertifyContinuousLimiterTransition(
			positive.width,positive.width,positiveScale,positiveScale,0.0f,undercount),
			"half-width undercount mutant fails the independent continuous-limiter certificate");
		FireProductionRoundoffWalker::ContinuousLimiterCertificate missingRamp,fixedWidth;
		Check(!FireProductionRoundoffWalker::CertifyContinuousLimiterTransition(
			positiveAmbiguity,0.0,positiveScale,positiveScale,0.0f,missingRamp,
			FireProductionRoundoffWalker::LimiterGraphVariant::MissingNegativeRamp)&&
			!FireProductionRoundoffWalker::CertifyContinuousLimiterTransition(
			positiveAmbiguity,0.0,positiveScale,positiveScale,0.0f,fixedWidth,
			FireProductionRoundoffWalker::LimiterGraphVariant::FixedWidthDenominator),
			"independent limiter graph rejects discontinuous-ramp and nonlegacy denominator mutants");
	}
	{
		std::vector<FireProductionRoundoffTrace::TraceFloat> values(4u,
			FireProductionRoundoffTrace::TraceFloat(3.0f)),left(values),right(values);
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::TransportProfileScope profile(values,left,right,
				0u,4u,FireProductionRoundoffTrace::TraceFloat(0.0f),
				FireProductionRoundoffTrace::TraceFloat(0.0f));
			const auto partition=FireProductionRoundoffTrace::floor(
				FireProductionRoundoffTrace::TraceFloat::Raw(2.0,0.125,2.0f,1u));
			Check(partition.Rounded()==2.0f,"floor partition fixture executes the upper path");
		}
		FireProductionRoundoffWalker::FloorPartitionCertificate certificate;
		const double profileUpper=std::nextafter(3.0,
			std::numeric_limits<double>::infinity());
		const double predicateCenter=counters.branchObligations.empty()?0.0:
			counters.branchObligations[0].predicateCenter;
		const double predicateRadius=counters.branchObligations.empty()?0.0:
			counters.branchObligations[0].predicateRadius;
		const bool certified=FireProductionRoundoffWalker::CertifyFloorPartition(
			predicateCenter,predicateRadius,profileUpper,4u,certificate);
		if(!(certified&&counters.branchObligations.size()==1u&&
			counters.branchObligations[0].divergenceBound==certificate.totalEnvelope))
			std::fprintf(stderr,"floor certificate diagnostic certified=%d obligations=%zu "
				"trace=%.17g walker=%.17g profile=%.17g\n",certified?1:0,
				counters.branchObligations.size(),counters.branchObligations.empty()?0.0:
				counters.branchObligations[0].divergenceBound,certificate.totalEnvelope,
				profileUpper);
		Check(certified&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::FloorBoundary&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations[0].divergenceBound==certificate.totalEnvelope&&
			certificate.continuousAtInteger,
			"independent floor walker matches the traced exact, rounded, and FTZ envelope");
		FireProductionRoundoffWalker::FloorPartitionCertificate half,missingRound,missingCycle;
		Check(!FireProductionRoundoffWalker::CertifyFloorPartition(predicateCenter,
			predicateRadius,profileUpper,4u,
			half,FireProductionRoundoffWalker::FloorGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyFloorPartition(predicateCenter,
				predicateRadius,profileUpper,4u,
				missingRound,FireProductionRoundoffWalker::FloorGraphVariant::MissingRoundedTerm)&&
			!FireProductionRoundoffWalker::CertifyFloorPartition(predicateCenter,
				predicateRadius,profileUpper,4u,
				missingCycle,FireProductionRoundoffWalker::FloorGraphVariant::MissingCyclePath),
			"floor certificate rejects ambiguity, rounded-path, and cycle-topology undercounts");
	}
	{
		std::vector<FireProductionRoundoffTrace::TraceFloat> values(3u,
			FireProductionRoundoffTrace::TraceFloat(2.0f)),left(values),right(values);
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::TransportProfileScope profile(values,left,right,
				0u,3u,FireProductionRoundoffTrace::TraceFloat(0.0f),
				FireProductionRoundoffTrace::TraceFloat(0.0f));
			const auto remaining=FireProductionRoundoffTrace::TraceFloat::Raw(
				0.0,0.0625,0.0f,1u);
			Check(!FireProductionRoundoffTrace::EvaluateRemainingPositiveBranch(
				[&](){return remaining>0.0f;}),
				"remaining-positive fixture executes the rounded inactive path");
		}
		Check(counters.branchObligations.size()==1u,
			"remaining-positive fixture emits exactly one ambiguous obligation");
		const FireProductionRoundoffTrace::BranchObligation obligation=
			counters.branchObligations.empty()?FireProductionRoundoffTrace::BranchObligation():
				counters.branchObligations.front();
		const double profileUpper=std::nextafter(2.0,
			std::numeric_limits<double>::infinity());
		FireProductionRoundoffWalker::RemainingPositiveCertificate certificate;
		const bool certified=FireProductionRoundoffWalker::CertifyRemainingPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,certificate);
		Check(certified&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			obligation.site==FireProductionRoundoffTrace::BranchSite::RemainingPositive&&
			obligation.certificate==FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.zeroWidthAtSwitch,
			"remaining-positive walker matches the zero-width exact, rounded, and FTZ envelope");
		FireProductionRoundoffWalker::RemainingPositiveCertificate half,missingCell,missingFTZ;
		Check(!FireProductionRoundoffWalker::CertifyRemainingPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,half,
			FireProductionRoundoffWalker::RemainingGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyRemainingPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingCell,
				FireProductionRoundoffWalker::RemainingGraphVariant::MissingCellIntegral)&&
			!FireProductionRoundoffWalker::CertifyRemainingPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingFTZ,
				FireProductionRoundoffWalker::RemainingGraphVariant::MissingFTZ),
			"remaining-positive certificate rejects width, body-topology, and FTZ omissions");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const std::vector<FireProductionRoundoffTrace::TraceFloat> values={
				FireProductionRoundoffTrace::TraceFloat(3.0f)};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> left=values,right=values;
			FireProductionRoundoffTrace::TransportProfileScope profile(
				values,left,right,0u,1u,0.0f,0.0f);
			const FireProductionRoundoffTrace::TraceFloat courant=
				FireProductionRoundoffTrace::TraceFloat::Raw(0.0,0.01,0.0f,1u);
			Check(FireProductionRoundoffTrace::EvaluateCourantSignBranch(
				[&](){return courant>=FireProductionRoundoffTrace::TraceFloat(0.0f);}),
				"Courant fixture executes the canonical nonnegative signed-zero path");
		}
		Check(counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations.front().site==
				FireProductionRoundoffTrace::BranchSite::CourantNonnegative&&
			counters.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"trace consumes the Courant two-path equivalence envelope");
		const auto obligation=counters.branchObligations.front();
		FireProductionRoundoffWalker::CourantSignCertificate certificate,half,missingPath,
			missingFTZ,signedZero;
		const double courantProfileUpper=std::nextafter(3.0,
			std::numeric_limits<double>::infinity());
		const bool courantCertified=FireProductionRoundoffWalker::CertifyCourantSign(
			obligation.predicateCenter,obligation.predicateRadius,courantProfileUpper,
			certificate);
		Check(courantCertified&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.continuousAtZero&&certificate.signedZeroCanonical&&
			certificate.commonSetupOperationCount==8u&&
			certificate.positiveSuccessorOperationCount==32u&&
			certificate.negativeSuccessorOperationCount==40u&&
			certificate.alternatePathOperationCount==48u,
			"independent Courant walker matches the traced two-path envelope");
		Check(!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,half,
				FireProductionRoundoffWalker::CourantGraphVariant::HalfAmbiguity),
			"Courant certificate rejects a half-ambiguity mutant");
		Check(
			!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,missingPath,
				FireProductionRoundoffWalker::CourantGraphVariant::MissingNegativePath),
			"Courant certificate rejects a missing-negative-path mutant");
		Check(
			!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,missingFTZ,
				FireProductionRoundoffWalker::CourantGraphVariant::MissingFTZ),
			"Courant certificate rejects a missing-FTZ mutant");
		Check(
			!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,signedZero,
				FireProductionRoundoffWalker::CourantGraphVariant::NoncanonicalSignedZero),
			"Courant certificate rejects a noncanonical signed-zero mutant");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const std::vector<FireProductionRoundoffTrace::TraceFloat> values={
				FireProductionRoundoffTrace::TraceFloat(2.0f)};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> left=values,right=values;
			FireProductionRoundoffTrace::TransportProfileScope profile(
				values,left,right,0u,1u,0.0f,0.0f);
			const FireProductionRoundoffTrace::TraceFloat fraction=
				FireProductionRoundoffTrace::TraceFloat::Raw(0.0,0.01,0.0f,1u);
			Check(!FireProductionRoundoffTrace::EvaluateFractionPositiveBranch(
				[&](){return fraction>FireProductionRoundoffTrace::TraceFloat(0.0f);}),
				"fraction fixture executes the rounded zero-width path");
		}
		Check(counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations.front().site==
				FireProductionRoundoffTrace::BranchSite::FractionPositive&&
			counters.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"trace consumes the fractional-tail equivalence envelope");
		const auto obligation=counters.branchObligations.front();
		const double profileUpper=std::nextafter(2.0,
			std::numeric_limits<double>::infinity());
		FireProductionRoundoffWalker::FractionPositiveCertificate certificate,half,
			missingTail,missingFTZ;
		Check(FireProductionRoundoffWalker::CertifyFractionPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,certificate)&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.zeroWidthAtSwitch&&certificate.tailOperationCount==24u,
			"independent fraction walker matches the traced trailing-slice envelope");
		Check(!FireProductionRoundoffWalker::CertifyFractionPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,half,
			FireProductionRoundoffWalker::FractionGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyFractionPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingTail,
				FireProductionRoundoffWalker::FractionGraphVariant::MissingTrailingIntegral)&&
			!FireProductionRoundoffWalker::CertifyFractionPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingFTZ,
				FireProductionRoundoffWalker::FractionGraphVariant::MissingFTZ),
			"fraction certificate rejects half-width, missing-tail, and FTZ mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		const auto center=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto left=FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.01,1.0f,1u);
		const auto right=FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.02,1.0f,1u);
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const std::vector<FireProductionRoundoffTrace::TraceFloat> values={center};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> leftValues={left};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> rightValues={right};
			FireProductionRoundoffTrace::TransportProfileScope profile(
				values,leftValues,rightValues,0u,1u,0.0f,0.0f);
			Check(FireProductionRoundoffTrace::EvaluateFlatIntegralBranch(left,center,right),
				"flat-integral fixture executes the rounded shortcut with two ambiguous equalities");
		}
		Check(counters.branchObligations.size()==2u&&
			counters.dischargedBranchObligationCount==2u&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations[1].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"trace discharges both executed flat-profile equality obligations");
		const double profileUpper=std::nextafter(std::fabs(right.Center())+right.Radius(),
			std::numeric_limits<double>::infinity());
		FireProductionRoundoffWalker::FlatIntegralCertificate certificate,half,
			missingPolynomial,missingFTZ,discontinuous,mutatedCoefficient;
		Check(FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
			center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,certificate)&&
			counters.branchObligations[0].divergenceBound==certificate.totalEnvelope&&
			counters.branchObligations[1].divergenceBound==certificate.totalEnvelope&&
			certificate.curvedPathOperationCount==32u&&certificate.continuousAtFlatProfile&&
			certificate.productionGraphMatched,
			"independent flat-integral walker matches both traced two-path envelopes");
		Check(!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
			center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,half,
			FireProductionRoundoffWalker::FlatIntegralGraphVariant::HalfDeviation)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,missingPolynomial,
				FireProductionRoundoffWalker::FlatIntegralGraphVariant::MissingCurvedPolynomial)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,missingFTZ,
				FireProductionRoundoffWalker::FlatIntegralGraphVariant::MissingFTZ)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,discontinuous,
				FireProductionRoundoffWalker::FlatIntegralGraphVariant::DiscontinuousFlatPath)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,
				mutatedCoefficient,FireProductionRoundoffWalker::FlatIntegralGraphVariant::
					MutatedQuadraticCoefficient),
			"flat-integral certificate rejects deviation, graph, FTZ, discontinuity, and coefficient mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		Check(!FireProductionRoundoffTrace::TraceFloat::Raw(-1.0,0.0,-1.0f,1u).
			NonnegativeByConstruction()&&
			!(FireProductionRoundoffTrace::TraceFloat(1.0f)-
				FireProductionRoundoffTrace::TraceFloat(2.0f)).NonnegativeByConstruction(),
			"raw and subtractive signed arithmetic cannot inherit positive-zero provenance");
		bool negative=false;{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::TraceFloat maximum=0.0f;
			const std::array<FireProductionRoundoffTrace::TraceFloat,3u> residuals={{
				FireProductionRoundoffTrace::TraceFloat::Raw(-0x1p-24,0x1p-22,-0x1p-24f,1u),
				FireProductionRoundoffTrace::TraceFloat::Raw(0.0,0x1p-23,0.0f,1u),
				FireProductionRoundoffTrace::TraceFloat::Raw(0x1p-25,0x1p-22,0x1p-25f,1u)}};
			for(const auto& residual:residuals)maximum=std::max(maximum,std::fabs(residual));
			negative=FireProductionRoundoffTrace::EvaluateNonnegativeReductionGuard(maximum);
		}
		Check(!negative&&!counters.branchObligations.empty()&&
			counters.branchObligations.back().site==
				FireProductionRoundoffTrace::BranchSite::NonnegativeReductionGuard&&
			counters.branchObligations.back().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations.back().divergenceBound==0.0,
			"trace discharges the negative projection-reduction guard from abs/max provenance");
		const std::vector<double> residuals={-3.0,2.0,-0.5};
		FireProductionRoundoffWalker::NonnegativeReductionCertificate certificate,
			signedLeaf,negativeSeed,subtractive;
		Check(FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
			residuals,certificate)&&certificate.leafCount==3u&&
			certificate.absoluteCount==3u&&certificate.maximumCount==3u&&
			certificate.positiveZeroSeed&&certificate.allLeavesAbsolute&&
			certificate.maximumOnly&&certificate.totalEnvelope==0.0,
			"independent walker proves the positive-zero abs/max reduction topology");
		Check(!FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
			residuals,signedLeaf,
			FireProductionRoundoffWalker::NonnegativeReductionGraphVariant::SignedLeaf)&&
			!FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
				residuals,negativeSeed,
				FireProductionRoundoffWalker::NonnegativeReductionGraphVariant::NegativeSeed)&&
			!FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
				residuals,subtractive,
				FireProductionRoundoffWalker::NonnegativeReductionGraphVariant::SubtractiveReduction),
			"projection-reduction proof rejects signed-leaf, negative-seed, and subtract mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			Check(FireProductionRoundoffTrace::EvaluateContinuousInflowJoin(
				FireProductionRoundoffTrace::TraceFloat::Raw(-0.25,0.01,-0.25f,1u),
				FireProductionRoundoffTrace::TraceFloat(0.25f),
				FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat(3.0f),true),
				"continuous-inflow fixture executes the rounded nearest-donor join");
		}
		Check(counters.branchObligations.size()==1u,
			"continuous-inflow join emits one ambiguous obligation");
		const FireProductionRoundoffTrace::BranchObligation obligation=
			counters.branchObligations.empty()?FireProductionRoundoffTrace::BranchObligation():
				counters.branchObligations.front();
		FireProductionRoundoffWalker::InflowTransitionCertificate certificate;
		const bool certified=FireProductionRoundoffWalker::CertifyInflowTransition(
			obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,
			1.0,0.0,3.0,0.0,certificate);
		Check(certified&&counters.dischargedBranchObligationCount==1u&&
			obligation.site==FireProductionRoundoffTrace::BranchSite::InflowSign&&
			obligation.certificate==FireProductionRoundoffTrace::BranchCertificate::Reformulation&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.continuousAtJoin&&certificate.convexDonorBlend&&
			certificate.legacyOutsideWidth,
			"independent inflow walker matches the traced convex join envelope");
		FireProductionRoundoffTrace::Counters upperCounters;
		{
			FireProductionRoundoffTrace::Scope scope(upperCounters);
			Check(!FireProductionRoundoffTrace::EvaluateContinuousInflowJoin(
				FireProductionRoundoffTrace::TraceFloat::Raw(0.25,0.01,0.249f,1u),
				FireProductionRoundoffTrace::TraceFloat(0.25f),
				FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat(3.0f),false),
				"continuous-inflow fixture executes the rounded ramp side of the ambient join");
		}
		Check(upperCounters.branchObligations.size()==1u&&
			upperCounters.dischargedBranchObligationCount==1u&&
			upperCounters.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Reformulation&&
			upperCounters.branchObligations.front().divergenceBound==
				certificate.totalEnvelope,
			"both continuous donor joins consume the same independently derived envelope");
		FireProductionRoundoffWalker::InflowTransitionCertificate half,missingContrast,
			missingRounded,binary;
		Check(!FireProductionRoundoffWalker::CertifyInflowTransition(
			obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
			3.0,0.0,half,FireProductionRoundoffWalker::InflowGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyInflowTransition(
				obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
				3.0,0.0,missingContrast,
				FireProductionRoundoffWalker::InflowGraphVariant::MissingDonorContrast)&&
			!FireProductionRoundoffWalker::CertifyInflowTransition(
				obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
				3.0,0.0,missingRounded,
				FireProductionRoundoffWalker::InflowGraphVariant::MissingRoundedTerm)&&
			!FireProductionRoundoffWalker::CertifyInflowTransition(
				obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
				3.0,0.0,binary,
				FireProductionRoundoffWalker::InflowGraphVariant::DiscontinuousBinary),
			"inflow certificate rejects width, donor, rounding, and binary-branch mutants");
	}
	{
		FireProductionRoundoffWalker::ProjectionInterpolationCertificate certificate,
			shifted,reassociated;
		const float exactBoundary=(4.0f+0.5f)*5.0f/9.0f-0.5f;
		std::uint64_t physical=0u,restoration=0u;
		Check(exactBoundary==2.0f&&
			FireProductionRoundoffWalker::CertifyProjectionInterpolationFloor(
				4u,9u,5u,2,exactBoundary,certificate)&&
			certificate.exactNumerator==36&&certificate.exactDenominator==18&&
			certificate.exactAndRoundedSameSide&&
			FireProductionRoundoffWalker::CountProjectionInterpolationObligations(
				24u,24u,36u,17u,physical)&&physical==765u&&
			FireProductionRoundoffWalker::CountProjectionInterpolationObligations(
				24u,24u,36u,16u,restoration)&&restoration==720u,
			"independent projection interpolation walker proves fixed-grid floor topology");
		Check(!FireProductionRoundoffWalker::CertifyProjectionInterpolationFloor(
				4u,9u,5u,2,exactBoundary,shifted,
				FireProductionRoundoffWalker::ProjectionInterpolationGraphVariant::ShiftedFine)&&
			!FireProductionRoundoffWalker::CertifyProjectionInterpolationFloor(
				2u,3u,2u,1,(2.0f+0.5f)*2.0f/3.0f-0.5f,reassociated,
				FireProductionRoundoffWalker::ProjectionInterpolationGraphVariant::
					ReassociatedDivision),
			"projection interpolation certificate rejects coordinate and association mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat affected,unrelated;
		{
			FireProductionRoundoffTrace::Scope traceScope(counters);
			{
				FireProductionRoundoffTrace::LocalTransportBranchScope localScope;
				FireProductionRoundoffTrace::RecordBranchDivergence(
					FireProductionRoundoffTrace::BranchSite::FlatIntegral,0.25);
				affected=FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope(
					FireProductionRoundoffTrace::TraceFloat(1.0f));
			}
			unrelated=FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope(
				FireProductionRoundoffTrace::TraceFloat(1.0f));
		}
		Check(affected.Radius()>0.25&&unrelated.Radius()==0.0,
			"local branch envelope affects only the swept integral that owns it");
		std::vector<FireProductionRoundoffTrace::TraceFloat> nonfinite={
			FireProductionRoundoffTrace::TraceFloat::Raw(1.0,
				std::numeric_limits<double>::quiet_NaN(),1.0f,1u)};
		FireProductionRoundoffTrace::Counters metricCounters;{
			FireProductionRoundoffTrace::Scope traceScope(metricCounters);
			FireProductionRoundoffTrace::ObserveMetricRangeAndReset(nonfinite,0u,1u,9u);
		}
		Check(std::isinf(metricCounters.firstNonfiniteMetricOutputCenter[9])==false&&
			metricCounters.nonfiniteMetricOutputRadiusCount[9]==1u&&
			metricCounters.invalidDomain,
			"NaN interval radii canonicalize to a fail-closed unbounded metric");
	}
	using namespace FireProductionCalibration;
	double radius=0.0;
	const RoundoffStage stages[]={{1.25,0x1p-22,24u},{2.0,0x1p-21,48u}};
	Check(ComposeRoundoffBound(stages,2u,radius)&&radius>
		2.0*(1.25*0.0+0x1p-22)+0x1p-21,"outward roundoff recurrence");
	const RoundoffStage invalid[]={{1.0,1.0,16777216u}};
	Check(!ComposeRoundoffBound(invalid,1u,radius)&&radius==0.0,
		"roundoff derivation rejects n*u >= 1");

	double order=0.0,distance=0.0;
	double subdominance=0.0;
	Check(Tier6DistanceFromDyadicPairs(0.005031015340016892,
		0.0041988689735742426,1.8,distance,subdominance)&&
		distance==0.005890459160549289&&subdominance==0.0007363073950686612&&
		!Tier6DistanceFromDyadicPairs(0.0,0.0041988689735742426,1.8,
			distance,subdominance),
		"r137 derives the tier-6 production distance and exact one-eighth subdominance bound");
	const double h5=0.05,h6=1.0/24.0,h7=1.0/28.0;
	const double d56=h5*h5-h6*h6,d67=h6*h6-h7*h7;
	const bool gridRichardson=GeneralizedGridRichardson(d56,d67,h5,h6,h7,2.0,order,distance);
	const double measuredGridEstimate=d67/(1.0-std::pow(h7/h6,2.0));
	if(!(gridRichardson&&std::fabs(order-2.0)<1.0e-12&&distance>measuredGridEstimate))
		std::fprintf(stderr,"grid diagnostic ok=%d p=%.17g E=%.17g expected=%.17g\n",
			gridRichardson?1:0,order,distance,h6*h6);
	Check(gridRichardson&&std::fabs(order-2.0)<1.0e-12&&distance>measuredGridEstimate,
		"generalized three-grid Richardson recovers capped order and tier-6 distance");
	Check(!GeneralizedGridRichardson(d67,d56,h5,h6,h7,2.0,order,distance),
		"non-asymptotic grid differences block calibration");
	const double positiveOrderFloor=std::log(h5/h6)/std::log(h6/h7);
	Check(!GeneralizedGridRichardson(positiveOrderFloor*0.99,1.0,h5,h6,h7,2.0,
		order,distance),"ratio below the positive-order limit blocks calibration");
	Check(GeneralizedGridRichardson(2.0,1.0,h5,h6,h7,2.0,order,distance)&&
		order==2.0,"apparent superconvergence is capped at formal order");
	Check(!GeneralizedGridRichardson(0.95,1.0,h5,h6,h7,2.0,order,distance)&&
		GeneralizedGridRichardson(d56,d67,h5,h6,h7,2.0,order,distance),
		"a non-asymptotic input family cannot stand in for an evolved-output gate");
	DyadicDistanceEstimate pairA,pairB;
	Check(DyadicDistanceAtVerifiedOrder(0.25,1.8,pairA)&&
		DyadicDistanceAtVerifiedOrder(0.20,1.8,pairB)&&
		pairA.coarseDistance>pairA.fineRadius&&
		DyadicLimitBallsOverlap(pairA.fineRadius+pairB.fineRadius,pairA,pairB)&&
		!DyadicLimitBallsOverlap(std::nextafter(NextUp(pairA.fineRadius+pairB.fineRadius),
			std::numeric_limits<double>::infinity()),pairA,pairB),
		"dyadic verified-order estimates own an exact independent-limit overlap rule");
	Check(TemporalRichardson(0.75,0.1875,2.0,order,distance)&&order==2.0&&
		distance>=1.0,"three-level temporal Richardson uses the baseline distance");
	FilteredTemporalDistanceMode temporalMode=FilteredTemporalDistanceMode::Rejected;
	Check(FilteredTemporalDistance(0.75,0.1875,2.0,nullptr,order,distance,temporalMode)&&
		temporalMode==FilteredTemporalDistanceMode::Richardson&&order==2.0&&distance>=1.0,
		"filtered temporal contraction retains the registered Richardson rule");
	const double floorCoarse=0.0012312438866646748;
	const double floorFine=0.001273209006325096;
	const FilteredTemporalPlateauAuthority floorAuthority={floorCoarse,floorFine};
	Check(FilteredTemporalDistance(floorCoarse,floorFine,2.0,&floorAuthority,order,distance,
			temporalMode)&&temporalMode==FilteredTemporalDistanceMode::MeasuredFloorUpperBound&&
		order==0.0&&distance==NextUp(floorFine)&&distance>floorFine&&
		!FilteredTemporalDistance(floorCoarse,floorFine,2.0,nullptr,order,distance,temporalMode)&&
		!FilteredTemporalDistance(floorCoarse,2.0*floorFine,2.0,&floorAuthority,order,
			distance,temporalMode),
		"filtered temporal plateau needs its exact evidence authority and uses the outward floor");
	double tolerance=0.0;
	Check(TriangleTolerance(1.0,2.0,3.0,4.0,5.0,tolerance)&&tolerance>15.0,
		"triangle tolerance is an outward sum rather than a maximum");

	RISE::FireProductionRemapRequest fp32;
	fp32.lineLength=5u;fp32.lineCount=2u;fp32.componentCount=2u;
	fp32.cellWidthM=0.25f;fp32.timeStepS=0.125f;
	fp32.boundary=RISE::FireProductionRemapPeriodic;
	fp32.values.assign(20u,0.125f);fp32.faceVelocityMPerS.assign(12u,0.5f);
	fp32.ambientValues.assign(2u,0.125f);
	RISE::FireProductionRemapResult fp32Result;std::string error;
	Check(RISE::RemapFireProductionCPU(fp32,fp32Result,&error),
		"binary32 remap comparator accepts exact free stream");
	RISEFireProductionFP64::FireProductionRemapRequest fp64;
	fp64.lineLength=fp32.lineLength;fp64.lineCount=fp32.lineCount;
	fp64.componentCount=fp32.componentCount;fp64.cellWidthM=fp32.cellWidthM;
	fp64.timeStepS=fp32.timeStepS;
	fp64.boundary=RISEFireProductionFP64::FireProductionRemapPeriodic;
	fp64.values.assign(fp32.values.begin(),fp32.values.end());
	fp64.faceVelocityMPerS.assign(fp32.faceVelocityMPerS.begin(),fp32.faceVelocityMPerS.end());
	fp64.ambientValues.assign(fp32.ambientValues.begin(),fp32.ambientValues.end());
	RISEFireProductionFP64::FireProductionRemapResult fp64Result;
	Check(RISEFireProductionFP64::RemapFireProductionCPU(fp64,fp64Result,&error)&&
		fp64Result.updatedValues.size()==fp32Result.updatedValues.size()&&
		std::all_of(fp64Result.updatedValues.begin(),fp64Result.updatedValues.end(),
			[](double value){return value==0.125;}),
		"generated binary64 remap preserves the production free-stream topology");

	RISEFireProductionTrace::FireProductionRemapRequest traced;
	traced.lineLength=fp32.lineLength;traced.lineCount=fp32.lineCount;
	traced.componentCount=fp32.componentCount;traced.cellWidthM=fp32.cellWidthM;
	traced.timeStepS=fp32.timeStepS;
	traced.boundary=RISEFireProductionTrace::FireProductionRemapPeriodic;
	traced.values.assign(fp32.values.begin(),fp32.values.end());
	traced.faceVelocityMPerS.assign(fp32.faceVelocityMPerS.begin(),
		fp32.faceVelocityMPerS.end());
	traced.ambientValues.assign(fp32.ambientValues.begin(),fp32.ambientValues.end());
	RISEFireProductionTrace::FireProductionRemapResult tracedResult;
	FireProductionRoundoffTrace::Counters tracedCounters;
	bool tracedOK=false;{
		FireProductionRoundoffTrace::Scope scope(tracedCounters);
		tracedOK=RISEFireProductionTrace::RemapFireProductionCPU(traced,tracedResult,&error);
	}
	bool tracedBytes=tracedResult.updatedValues.size()==fp32Result.updatedValues.size();
	for(std::size_t i=0u;tracedBytes&&i<tracedResult.updatedValues.size();++i)
		tracedBytes=tracedResult.updatedValues[i].Rounded()==fp32Result.updatedValues[i]&&
			tracedResult.updatedValues[i].Radius()>=0.0&&
			std::isfinite(tracedResult.updatedValues[i].Radius());
	std::uint64_t tracedOperations=0u;
	for(const std::uint64_t count:tracedCounters.operation)tracedOperations+=count;
	const std::array<std::uint64_t,13u> expectedTraceKinds={{
		20u,196u,220u,232u,112u,0u,140u,0u,0u,40u,0u,20u,0u}};
	FireProductionRoundoffWalker::Topology walkedTopology;
	const bool walked=FireProductionRoundoffWalker::
		WalkPeriodicPositiveSubcellFreeStreamRemap(fp32.lineLength,fp32.lineCount,
			fp32.componentCount,walkedTopology);
	if(!(walked&&tracedOperations==walkedTopology.operationCount&&
		tracedCounters.maximumDepth==walkedTopology.maximumDepth))
		std::fprintf(stderr,"roundoff topology diagnostic traced=%llu/%u walked=%llu/%u\n",
			static_cast<unsigned long long>(tracedOperations),tracedCounters.maximumDepth,
			static_cast<unsigned long long>(walkedTopology.operationCount),
			walkedTopology.maximumDepth);
	Check(tracedOK&&tracedBytes&&tracedOperations==980u&&
		std::equal(expectedTraceKinds.begin(),expectedTraceKinds.end(),
			std::begin(tracedCounters.operation))&&tracedCounters.maximumDepth==14u&&
		tracedCounters.comparisonCount==644u&&!tracedCounters.unresolvedBranch&&
		tracedCounters.branchObligations.size()==260u&&
		tracedCounters.dischargedBranchObligationCount==260u&&
		tracedCounters.minimumDenominatorLowerBound>0.0&&
		*std::max_element(std::begin(tracedCounters.maximumAbsoluteOperand),
			std::end(tracedCounters.maximumAbsoluteOperand))==5.0&&
		!tracedCounters.invalidDomain&&walked&&
		tracedOperations==walkedTopology.operationCount&&
		std::equal(std::begin(tracedCounters.operation),std::end(tracedCounters.operation),
			std::begin(walkedTopology.operation))&&
		tracedCounters.maximumDepth==walkedTopology.maximumDepth,
		"independent remap graph walk reproduces traced operation count and depth while the trace reproduces fp32 bytes");
	if(!(tracedOK&&tracedBytes&&tracedOperations==980u&&
		tracedCounters.comparisonCount==644u&&!tracedCounters.unresolvedBranch&&
		tracedCounters.branchObligations.size()==260u&&
		tracedCounters.dischargedBranchObligationCount==260u&&
		!tracedCounters.invalidDomain))std::fprintf(stderr,
		"roundoff remap detail ok=%d bytes=%d ops=%llu comparisons=%llu obligations=%zu "
		"discharged=%llu unresolved=%d invalid=%d denominator=%.17g max_operand=%.17g\n",
		tracedOK?1:0,tracedBytes?1:0,static_cast<unsigned long long>(tracedOperations),
		static_cast<unsigned long long>(tracedCounters.comparisonCount),
		tracedCounters.branchObligations.size(),static_cast<unsigned long long>(
			tracedCounters.dischargedBranchObligationCount),
		tracedCounters.unresolvedBranch?1:0,tracedCounters.invalidDomain?1:0,
		tracedCounters.minimumDenominatorLowerBound,
		*std::max_element(std::begin(tracedCounters.maximumAbsoluteOperand),
			std::end(tracedCounters.maximumAbsoluteOperand)));
	FireProductionRoundoffWalker::Topology omittedAdd=walkedTopology;
	--omittedAdd.operation[FireProductionRoundoffWalker::Add];--omittedAdd.operationCount;
	Check(!std::equal(std::begin(tracedCounters.operation),std::end(tracedCounters.operation),
		std::begin(omittedAdd.operation))&&omittedAdd.operationCount!=tracedOperations,
		"independent topology gate rejects an omitted arithmetic operation");

	RISE::FireProductionCellPalindromeRequest cell32;
	cell32.shape.nx=5u;cell32.shape.ny=6u;cell32.shape.nz=7u;
	cell32.shape.cellWidthM=0.2f;cell32.componentCount=9u;cell32.timeStepS=0.01f;
	cell32.boundary.fill(RISE::FireProductionProjectionPressureOpen);
	const std::size_t cellCount=cell32.shape.CellCount();
	cell32.conservativeValues.resize(cell32.componentCount*cellCount);
	cell32.ambientValues.resize(cell32.componentCount);
	for(std::size_t component=0u;component<cell32.componentCount;++component){
		const float base=component==8u?300000.0f:static_cast<float>(component+1u);
		cell32.ambientValues[component]=base;
		for(std::size_t z=0u;z<cell32.shape.nz;++z)
			for(std::size_t y=0u;y<cell32.shape.ny;++y)
				for(std::size_t x=0u;x<cell32.shape.nx;++x){
					const std::size_t cell=(z*cell32.shape.ny+y)*cell32.shape.nx+x;
					cell32.conservativeValues[component*cellCount+cell]=base*(1.0f+
						0.02f*static_cast<float>(x+2u*y+3u*z));
				}
	}
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(cell32.shape,axis);
		cell32.frozenVelocityMPerS[axis].resize(faces);
		for(std::size_t face=0u;face<faces;++face)
			cell32.frozenVelocityMPerS[axis][face]=0.01f*static_cast<float>(axis+1u);
	}
	RISE::FireProductionCellPalindromeResult cell32Result;
	Check(RISE::RemapFireProductionCellPalindromeCPU(cell32,cell32Result,&error),
		"binary32 cell palindrome accepts the stage-reset witness");
	RISEFireProductionTrace::FireProductionCellPalindromeRequest tracedCell;
	tracedCell.shape.nx=cell32.shape.nx;tracedCell.shape.ny=cell32.shape.ny;
	tracedCell.shape.nz=cell32.shape.nz;tracedCell.shape.cellWidthM=cell32.shape.cellWidthM;
	tracedCell.componentCount=cell32.componentCount;tracedCell.timeStepS=cell32.timeStepS;
	for(unsigned int side=0u;side<6u;++side)tracedCell.boundary[side]=
		static_cast<RISEFireProductionTrace::FireProductionProjectionBoundary>(
			cell32.boundary[side]);
	tracedCell.conservativeValues.assign(cell32.conservativeValues.begin(),
		cell32.conservativeValues.end());
	tracedCell.ambientValues.assign(cell32.ambientValues.begin(),cell32.ambientValues.end());
	for(unsigned int axis=0u;axis<3u;++axis)tracedCell.frozenVelocityMPerS[axis].assign(
		cell32.frozenVelocityMPerS[axis].begin(),cell32.frozenVelocityMPerS[axis].end());
	RISEFireProductionTrace::FireProductionCellPalindromeResult tracedCellResult;
	FireProductionRoundoffTrace::Counters tracedCellCounters;bool tracedCellOK=false;{
		FireProductionRoundoffTrace::Scope scope(tracedCellCounters);
		tracedCellOK=RISEFireProductionTrace::RemapFireProductionCellPalindromeCPU(
			tracedCell,tracedCellResult,&error);
	}
	bool tracedCellBytes=tracedCellResult.conservativeValues.size()==
		cell32Result.conservativeValues.size();
	for(std::size_t value=0u;tracedCellBytes&&value<tracedCellResult.conservativeValues.size();++value)
		tracedCellBytes=tracedCellResult.conservativeValues[value].Rounded()==
			cell32Result.conservativeValues[value]&&
			tracedCellResult.conservativeValues[value].Radius()==0.0;
	bool completeStages=tracedCellCounters.sealedStages.size()==5u;
	for(const FireProductionRoundoffTrace::Observation& stage:tracedCellCounters.sealedStages){
		std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)operations+=count;
		completeStages=completeStages&&operations>0u&&stage.maximumDepth>0u&&
			stage.maximumOutputRadius>0.0&&std::isfinite(stage.maximumOutputRadius)&&
			std::isfinite(stage.maximumAbsoluteOutput);
	}
	Check(tracedCellOK&&tracedCellBytes&&completeStages,
		"cell palindrome trace seals five local certificates and resets radii without changing fp32 bytes");

	// Exact Section 3.7 bootstrap.  Every arithmetic operand is dyadic, so the
	// strict-binary32 production seam must equal the live binary64 oracle without
	// a tolerance.  The asymmetric pattern distinguishes low/high/shared-alpha,
	// normal and transverse I_i, frozen carrier velocity, and wall/open handling.
	auto productionFaceIndex=[]( const RISE::FireProductionProjectionShape& shape,
		const unsigned int axis, const std::size_t x, const std::size_t y,
		const std::size_t z ) {
		if(axis==0u)return (z*shape.ny+y)*(shape.nx+1u)+x;
		if(axis==1u)return (z*(shape.ny+1u)+y)*shape.nx+x;
		return (z*shape.ny+y)*shape.nx+x;
	};
	auto sameDoubleBits=[]( const double first, const double second ) {
		std::uint64_t firstBits=0u,secondBits=0u;
		std::memcpy(&firstBits,&first,sizeof(firstBits));
		std::memcpy(&secondBits,&second,sizeof(secondBits));
		return firstBits==secondBits;
	};
	auto sameFloatBits=[]( const float first, const float second ) {
		std::uint32_t firstBits=0u,secondBits=0u;
		std::memcpy(&firstBits,&first,sizeof(firstBits));
		std::memcpy(&secondBits,&second,sizeof(secondBits));
		return firstBits==secondBits;
	};
	RISE::FireProductionCompatibleFCTMomentumRequest openFCT32;
	openFCT32.shape.nx=4u;openFCT32.shape.ny=4u;openFCT32.shape.nz=4u;
	openFCT32.shape.cellWidthM=0.5f;
	openFCT32.boundary={{RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionPressureOpen,
		RISE::FireProductionProjectionPressureOpen,RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionWall,RISE::FireProductionProjectionPressureOpen}};
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(openFCT32.shape,axis);
		openFCT32.lowGasFluxKGPerM2S[axis].assign(faces,0.0f);
		openFCT32.highGasFluxKGPerM2S[axis].assign(faces,0.0f);
		openFCT32.sharedFaceAlpha[axis].assign(faces,0.0f);
		openFCT32.frozenVelocityMPerS[axis].assign(faces,0.0f);
	}
	const auto setOpenX=[&](const std::size_t x,const float low,const float high,
		const float alpha,const float velocity){
		const std::size_t face=productionFaceIndex(openFCT32.shape,0u,x,1u,1u);
		openFCT32.lowGasFluxKGPerM2S[0][face]=low;
		openFCT32.highGasFluxKGPerM2S[0][face]=high;
		openFCT32.sharedFaceAlpha[0][face]=alpha;
		openFCT32.frozenVelocityMPerS[0][face]=velocity;
	};
	setOpenX(0u,0.0f,0.0f,0.0f,0.0f);setOpenX(1u,1.0f,3.0f,0.5f,1.0f);
	setOpenX(2u,2.0f,6.0f,0.25f,3.0f);setOpenX(3u,4.0f,0.0f,1.0f,2.0f);
	setOpenX(4u,8.0f,16.0f,0.0f,4.0f);
	// Duplicate the normal pattern on y=3 so its analytic 9/80 sentinels are
	// isolated from the transverse y-flux pattern used at y=1.
	for(std::size_t x=0u;x<=4u;++x){
		const std::size_t source=productionFaceIndex(openFCT32.shape,0u,x,1u,1u);
		const std::size_t destination=productionFaceIndex(openFCT32.shape,0u,x,3u,1u);
		openFCT32.lowGasFluxKGPerM2S[0][destination]=
			openFCT32.lowGasFluxKGPerM2S[0][source];
		openFCT32.highGasFluxKGPerM2S[0][destination]=
			openFCT32.highGasFluxKGPerM2S[0][source];
		openFCT32.sharedFaceAlpha[0][destination]=openFCT32.sharedFaceAlpha[0][source];
		openFCT32.frozenVelocityMPerS[0][destination]=
			openFCT32.frozenVelocityMPerS[0][source];
	}
	openFCT32.frozenVelocityMPerS[0][productionFaceIndex(
		openFCT32.shape,0u,2u,0u,1u)]=1.0f;
	openFCT32.frozenVelocityMPerS[0][productionFaceIndex(
		openFCT32.shape,0u,2u,2u,1u)]=5.0f;
	const auto setOpenY=[&](const std::size_t x,const std::size_t y,const float low,
		const float high,const float alpha){
		const std::size_t face=productionFaceIndex(openFCT32.shape,1u,x,y,1u);
		openFCT32.lowGasFluxKGPerM2S[1][face]=low;
		openFCT32.highGasFluxKGPerM2S[1][face]=high;
		openFCT32.sharedFaceAlpha[1][face]=alpha;
	};
	setOpenY(1u,1u,0.0f,2.0f,0.5f);setOpenY(2u,1u,2.0f,4.0f,0.5f);
	setOpenY(1u,2u,2.0f,8.0f,0.0f);setOpenY(2u,2u,0.0f,6.0f,1.0f);
	RISE::FireProductionCompatibleFCTMomentumResult openFCTResult;
	const bool openFCTOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		openFCT32,openFCTResult,&error);
	RISE::FireSim::PeriodicMACShape openOracleShape;
	openOracleShape.nx=4u;openOracleShape.ny=4u;openOracleShape.nz=4u;
	openOracleShape.cellWidthM=0.5;
	RISE::FireSim::OpenFluxPair3D openOracleFlux;
	std::array<std::vector<double>,3> openOracleAlpha;
	RISE::FireSim::OpenMACField3D openOracleVelocity;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireSim::OpenMACFaceCount3D(openOracleShape,axis);
		openOracleFlux.low[axis].assign(faces,RISE::FireSim::ConservativeVector());
		openOracleFlux.high[axis].assign(faces,RISE::FireSim::ConservativeVector());
		openOracleFlux.nonadvectiveMass[axis].assign(faces,
			std::array<double,RISE::FireSim::MethaneMassStateDimension>());
		openOracleFlux.nonadvectiveEnergy[axis].assign(faces,0.0);
		openOracleAlpha[axis].resize(faces);
		openOracleVelocity.component[axis].resize(faces);
		for(std::size_t face=0u;face<faces;++face){
			const double physical=openFCT32.physicalGasFluxKGPerM2S[axis].empty()?0.0:
				openFCT32.physicalGasFluxKGPerM2S[axis][face];
			openOracleFlux.low[axis][face][1u+RISE::FireSim::MethaneCH4]=
				openFCT32.lowGasFluxKGPerM2S[axis][face]+physical;
			openOracleFlux.high[axis][face][1u+RISE::FireSim::MethaneCH4]=
				openFCT32.highGasFluxKGPerM2S[axis][face]+physical;
			openOracleFlux.nonadvectiveMass[axis][face][1u+RISE::FireSim::MethaneCH4]=
				physical;
			openOracleAlpha[axis][face]=openFCT32.sharedFaceAlpha[axis][face];
			openOracleVelocity.component[axis][face]=
				openFCT32.frozenVelocityMPerS[axis][face];
		}
	}
	RISE::FireSim::OpenBoundaryConfig3D openOracleBoundary;
	for(unsigned int side=0u;side<6u;++side)openOracleBoundary.kind[side]=
		openFCT32.boundary[side]==RISE::FireProductionProjectionPressureOpen?
		RISE::FireSim::PressureOpenBoundary3D:RISE::FireSim::AdiabaticWallBoundary3D;
	const RISE::FireSim::OpenMACField3D openOracleRate=
		RISE::FireSim::OpenCompatibleMomentumFluxDivergence3D(openOracleShape,
			openOracleFlux,openOracleAlpha,openOracleVelocity,&openOracleBoundary);
	bool openFCTExact=openFCTOK;
	for(unsigned int axis=0u;axis<3u&&openFCTExact;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(openFCT32.shape,axis);
		openFCTExact=openFCTResult.advectionRateKGPerM2S2[axis].size()==faces&&
			openFCTResult.acceptedGasFluxKGPerM2S[axis].size()==faces;
		for(std::size_t face=0u;face<faces&&openFCTExact;++face){
			const double physical=openFCT32.physicalGasFluxKGPerM2S[axis].empty()?0.0:
				openFCT32.physicalGasFluxKGPerM2S[axis][face];
			const double accepted=openFCT32.lowGasFluxKGPerM2S[axis][face]+
				openFCT32.sharedFaceAlpha[axis][face]*(
				openFCT32.highGasFluxKGPerM2S[axis][face]-
				openFCT32.lowGasFluxKGPerM2S[axis][face])+physical;
			openFCTExact=sameDoubleBits(static_cast<double>(
				openFCTResult.acceptedGasFluxKGPerM2S[axis][face]),accepted)&&
				sameDoubleBits(static_cast<double>(
				openFCTResult.advectionRateKGPerM2S2[axis][face]),
				openOracleRate.component[axis][face]);
		}
	}
	const std::size_t openX1=productionFaceIndex(openFCT32.shape,0u,1u,3u,1u);
	const std::size_t openX2=productionFaceIndex(openFCT32.shape,0u,2u,1u,1u);
	const std::size_t openX4=productionFaceIndex(openFCT32.shape,0u,4u,3u,1u);
	const std::size_t openX0=productionFaceIndex(openFCT32.shape,0u,0u,3u,1u);
	Check(openFCTExact&&openFCTResult.advectionRateKGPerM2S2[0][openX1]==9.0f&&
		openFCTResult.advectionRateKGPerM2S2[0][openX2]==21.5f&&
		openFCTResult.advectionRateKGPerM2S2[0][openX4]==40.0f&&
		openFCTResult.advectionRateKGPerM2S2[0][openX0]==0.0f&&
		!std::signbit(openFCTResult.advectionRateKGPerM2S2[0][openX0]),
		"compatible FCT open/wall operator is word-exact to the live oracle and analytic sentinels");

	// Periodic storage has a publication plane while the live oracle owns one
	// canonical face per cell.  Compare every canonical face and the exact seam.
	RISE::FireProductionCompatibleFCTMomentumRequest periodicFCT32;
	periodicFCT32.shape=openFCT32.shape;
	periodicFCT32.boundary.fill(RISE::FireProductionProjectionPeriodic);
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(periodicFCT32.shape,axis);
		periodicFCT32.lowGasFluxKGPerM2S[axis].assign(faces,0.0f);
		periodicFCT32.highGasFluxKGPerM2S[axis].assign(faces,0.0f);
		periodicFCT32.sharedFaceAlpha[axis].assign(faces,0.0f);
		periodicFCT32.frozenVelocityMPerS[axis].assign(faces,0.0f);
	}
	periodicFCT32.physicalGasFluxKGPerM2S[0].assign(
		RISE::FireProductionProjectionFaceCount(periodicFCT32.shape,0u),0.0f);
	const float periodicLow[4]={0.5f,0.5f,1.5f,7.5f};
	const float periodicHigh[4]={2.5f,2.5f,5.5f,0.0f};
	const float periodicAlpha[4]={0.0f,0.5f,0.5f,0.0f};
	const float periodicVelocity[4]={1.0f,3.0f,2.0f,4.0f};
	for(std::size_t x=0u;x<4u;++x){
		const std::size_t face=productionFaceIndex(periodicFCT32.shape,0u,x,1u,1u);
		periodicFCT32.lowGasFluxKGPerM2S[0][face]=periodicLow[x];
		periodicFCT32.highGasFluxKGPerM2S[0][face]=periodicHigh[x];
		periodicFCT32.physicalGasFluxKGPerM2S[0][face]=0.5f;
		periodicFCT32.sharedFaceAlpha[0][face]=periodicAlpha[x];
		periodicFCT32.frozenVelocityMPerS[0][face]=periodicVelocity[x];
	}
	const std::size_t periodicLowSeam=productionFaceIndex(periodicFCT32.shape,0u,0u,1u,1u);
	const std::size_t periodicHighSeam=productionFaceIndex(periodicFCT32.shape,0u,4u,1u,1u);
	periodicFCT32.lowGasFluxKGPerM2S[0][periodicHighSeam]=
		periodicFCT32.lowGasFluxKGPerM2S[0][periodicLowSeam];
	periodicFCT32.highGasFluxKGPerM2S[0][periodicHighSeam]=
		periodicFCT32.highGasFluxKGPerM2S[0][periodicLowSeam];
	periodicFCT32.physicalGasFluxKGPerM2S[0][periodicHighSeam]=
		periodicFCT32.physicalGasFluxKGPerM2S[0][periodicLowSeam];
	periodicFCT32.sharedFaceAlpha[0][periodicHighSeam]=
		periodicFCT32.sharedFaceAlpha[0][periodicLowSeam];
	periodicFCT32.frozenVelocityMPerS[0][periodicHighSeam]=
		periodicFCT32.frozenVelocityMPerS[0][periodicLowSeam];
	RISE::FireProductionCompatibleFCTMomentumResult periodicFCTResult;
	const bool periodicFCTOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		periodicFCT32,periodicFCTResult,&error);
	RISE::FireSim::PeriodicMACShape periodicOracleShape=openOracleShape;
	std::array<std::vector<double>,3> periodicAcceptedAdvection,periodicPhysical;
	RISE::FireSim::PeriodicMACField periodicOracleVelocity;
	for(unsigned int axis=0u;axis<3u;++axis){
		periodicAcceptedAdvection[axis].assign(periodicOracleShape.CellCount(),0.0);
		periodicPhysical[axis].assign(periodicOracleShape.CellCount(),0.0);
		periodicOracleVelocity.component[axis].assign(periodicOracleShape.CellCount(),0.0);
		for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
			for(std::size_t x=0u;x<4u;++x){
				const std::size_t cell=(z*4u+y)*4u+x;
				const std::size_t face=productionFaceIndex(periodicFCT32.shape,axis,x,y,z);
				periodicAcceptedAdvection[axis][cell]=
					periodicFCT32.lowGasFluxKGPerM2S[axis][face]+
					periodicFCT32.sharedFaceAlpha[axis][face]*(
					periodicFCT32.highGasFluxKGPerM2S[axis][face]-
					periodicFCT32.lowGasFluxKGPerM2S[axis][face]);
				periodicPhysical[axis][cell]=periodicFCT32.physicalGasFluxKGPerM2S[axis].empty()?0.0:
					periodicFCT32.physicalGasFluxKGPerM2S[axis][face];
				periodicOracleVelocity.component[axis][cell]=
					periodicFCT32.frozenVelocityMPerS[axis][face];
			}
	}
	const std::array<std::vector<double>,3> periodicOracleRate=
		RISE::FireSim::CompatibleMomentumFluxDivergence3D(periodicOracleShape,
			periodicAcceptedAdvection,periodicPhysical,periodicOracleVelocity);
	bool periodicFCTExact=periodicFCTOK;
	for(unsigned int axis=0u;axis<3u&&periodicFCTExact;++axis){
		for(std::size_t z=0u;z<4u&&periodicFCTExact;++z)
			for(std::size_t y=0u;y<4u&&periodicFCTExact;++y)
				for(std::size_t x=0u;x<4u&&periodicFCTExact;++x){
					const std::size_t cell=(z*4u+y)*4u+x;
					const std::size_t face=productionFaceIndex(periodicFCT32.shape,axis,x,y,z);
					periodicFCTExact=sameDoubleBits(static_cast<double>(periodicFCTResult.
						acceptedGasFluxKGPerM2S[axis][face]),
						periodicAcceptedAdvection[axis][cell]+periodicPhysical[axis][cell])&&
						sameDoubleBits(static_cast<double>(periodicFCTResult.
						advectionRateKGPerM2S2[axis][face]),periodicOracleRate[axis][cell]);
				}
		const std::size_t firstEnd=axis==0u?periodicFCT32.shape.ny:
			periodicFCT32.shape.nx;
		const std::size_t secondEnd=axis==2u?periodicFCT32.shape.ny:
			periodicFCT32.shape.nz;
		for(std::size_t second=0u;second<secondEnd&&periodicFCTExact;++second)
			for(std::size_t first=0u;first<firstEnd&&periodicFCTExact;++first){
				std::size_t lx=axis==0u?0u:first;
				std::size_t ly=axis==0u?first:(axis==1u?0u:second);
				std::size_t lz=axis==2u?0u:second;
				std::size_t hx=lx,hy=ly,hz=lz;
				if(axis==0u)hx=4u;if(axis==1u)hy=4u;if(axis==2u)hz=4u;
				const std::size_t low=productionFaceIndex(
					periodicFCT32.shape,axis,lx,ly,lz);
				const std::size_t high=productionFaceIndex(
					periodicFCT32.shape,axis,hx,hy,hz);
				periodicFCTExact=sameFloatBits(
					periodicFCTResult.acceptedGasFluxKGPerM2S[axis][low],
					periodicFCTResult.acceptedGasFluxKGPerM2S[axis][high])&&
					sameFloatBits(periodicFCTResult.advectionRateKGPerM2S2[axis][low],
					periodicFCTResult.advectionRateKGPerM2S2[axis][high]);
			}
	}
	const std::size_t periodicX0=productionFaceIndex(periodicFCT32.shape,0u,0u,1u,1u);
	const std::size_t periodicX1=productionFaceIndex(periodicFCT32.shape,0u,1u,1u,1u);
	Check(periodicFCTExact&&periodicFCTResult.advectionRateKGPerM2S2[0][periodicX0]==-16.5f&&
		periodicFCTResult.advectionRateKGPerM2S2[0][periodicX1]==9.0f,
		"compatible FCT periodic operator is word-exact to the live oracle, analytic sentinels, and seam");

	// Fail closed before publication, and prove the fixture distinguishes the
	// rejected all-high (shared-alpha ignored) mutation from the admitted form.
	RISE::FireProductionCompatibleFCTMomentumRequest malformedFCT=periodicFCT32;
	malformedFCT.sharedFaceAlpha[0][periodicX1]=1.25f;
	RISE::FireProductionCompatibleFCTMomentumResult refusedFCT;
	refusedFCT.acceptedGasFluxKGPerM2S[0].assign(1u,123.0f);
	const bool invalidAlphaRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error)&&refusedFCT.acceptedGasFluxKGPerM2S[0].empty()&&
		refusedFCT.advectionRateKGPerM2S2[0].empty();
	malformedFCT=periodicFCT32;
	malformedFCT.lowGasFluxKGPerM2S[0][periodicHighSeam]+=1.0f;
	const bool badSeamRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error);
	malformedFCT=periodicFCT32;
	malformedFCT.lowGasFluxKGPerM2S[1][productionFaceIndex(
		malformedFCT.shape,1u,0u,4u,0u)]=-0.0f;
	const bool signedZeroSeamRefused=
		!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
			malformedFCT,refusedFCT,&error);
	malformedFCT=periodicFCT32;
	malformedFCT.boundary[2]=RISE::FireProductionProjectionWall;
	malformedFCT.boundary[3]=RISE::FireProductionProjectionWall;
	const bool hybridRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error);
	malformedFCT=periodicFCT32;
	malformedFCT.physicalGasFluxKGPerM2S[1].assign(1u,0.0f);
	const bool badPhysicalShapeRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error);
	RISE::FireProductionCompatibleFCTMomentumRequest allHighFCT=periodicFCT32;
	for(unsigned int axis=0u;axis<3u;++axis)allHighFCT.lowGasFluxKGPerM2S[axis]=
		allHighFCT.highGasFluxKGPerM2S[axis];
	RISE::FireProductionCompatibleFCTMomentumResult allHighResult;
	const bool allHighComputed=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		allHighFCT,allHighResult,&error);
	Check(invalidAlphaRefused&&badSeamRefused&&signedZeroSeamRefused&&hybridRefused&&
		badPhysicalShapeRefused&&allHighComputed&&
		allHighResult.advectionRateKGPerM2S2[0][periodicX1]!=
		periodicFCTResult.advectionRateKGPerM2S2[0][periodicX1],
		"compatible FCT bootstrap refuses malformed identity inputs and detects an all-high limiter mutant");

	// Full scalar FCT bootstrap.  The synthetic one-coordinate nullspace uses
	// B=(1/2,1/2) and coordinate projector 2, so rho-total and CH4 reconstruct
	// together with exact dyadic arithmetic while all nine tuple rows traverse
	// the same donor/MC, shared-alpha, source-commit stages.
	RISE::FireProductionScalarFCTRequest scalarFCT;
	scalarFCT.shape=periodicFCT32.shape;scalarFCT.timeStepS=0.25f;
	scalarFCT.boundary.fill(RISE::FireProductionProjectionPeriodic);
	const std::size_t scalarCells=scalarFCT.shape.CellCount();
	scalarFCT.beginning.assign(9u*scalarCells,0.0f);
	scalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	const float scalarPattern[4]={1.0f,1.25f,1.5f,1.25f};
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){
			const std::size_t cell=(z*4u+y)*4u+x;
			scalarFCT.beginning[cell]=scalarPattern[x];
			scalarFCT.beginning[scalarCells+cell]=scalarPattern[x];
		}
	scalarFCT.sourceDelta[0u]=0.125f;
	scalarFCT.sourceDelta[scalarCells]=0.125f;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(scalarFCT.shape,axis);
		scalarFCT.frozenVelocityMPerS[axis].assign(faces,axis==0u?1.0f:0.0f);
	}
	for(unsigned int side=0u;side<6u;++side){
		const std::size_t faces=side<2u?scalarFCT.shape.ny*scalarFCT.shape.nz:
			(side<4u?scalarFCT.shape.nx*scalarFCT.shape.nz:
				scalarFCT.shape.nx*scalarFCT.shape.ny);
		scalarFCT.pressureOpenInflow[side].assign(faces,0u);
	}
	scalarFCT.ambient[0]=1.0f;scalarFCT.ambient[1]=1.0f;
	scalarFCT.nullity=1u;scalarFCT.nullspaceBasis.assign(8u,0.0f);
	scalarFCT.nullspaceBasis[0]=0.5f;scalarFCT.nullspaceBasis[1]=0.5f;
	scalarFCT.coordinateProjector.assign(1u,2.0f);
	scalarFCT.feasibilityFactor=1.0f/1024.0f;
	scalarFCT.assemblyReserveFactor=1.0f/2048.0f;
	RISE::FireProductionScalarFCTResult scalarFCTResult;
	const bool scalarFCTOK=RISE::EvaluateFireProductionScalarFCTCPU(
		scalarFCT,scalarFCTResult,&error);
	const std::size_t scalarAllFaces=scalarFCTOK?scalarFCTResult.lowFlux.size()/9u:0u;
	const std::size_t scalarFace2=productionFaceIndex(scalarFCT.shape,0u,2u,0u,0u);
	const std::size_t scalarPackedFace2=scalarFCTResult.packedFaceOffset[0]+scalarFace2;
	bool allScalarAlphaOne=scalarFCTOK;
	for(unsigned int axis=0u;axis<3u&&allScalarAlphaOne;++axis)
		for(const float alpha:scalarFCTResult.sharedFaceAlpha[axis])
			allScalarAlphaOne=allScalarAlphaOne&&alpha==1.0f;
	Check(scalarFCTOK&&allScalarAlphaOne&&scalarFCTResult.fluxDelta[
		scalarPackedFace2]==0.125f&&scalarFCTResult.fluxDelta[
		scalarAllFaces+scalarPackedFace2]==0.125f&&
		scalarFCTResult.commutingIdentityAvailable&&
		scalarFCTResult.maximumCommutingResidualKGPerM3==0.0f,
		"strict fp32 scalar FCT reconstructs the dyadic nullspace, shares alpha, and commutes exactly");

	// Prerequisite-only f_N/J_g producer. It consumes fixed binary32 Q,T,D,k,u,
	// verifies the canonical fp64 N_C N_C^T reference envelope, and retains the
	// binary32 publication residual separately. It is not a projected-Heun owner.
	RISE::FireProductionProjectionShape physicalUnderCap,physicalOverCap;
	physicalUnderCap.nx=76u;physicalUnderCap.ny=279u;physicalUnderCap.nz=560u;
	physicalUnderCap.cellWidthM=1.0f;
	physicalOverCap.nx=132u;physicalOverCap.ny=224u;physicalOverCap.nz=402u;
	physicalOverCap.cellWidthM=1.0f;
	std::uint64_t physicalUnderBytes=0u,physicalOverBytes=0u;
	const bool physicalAdmissionQuery=
		RISE::QueryFireProductionScalarPhysicalFluxPrerequisiteCPUWorkingSetBytes(
			physicalUnderCap,physicalUnderBytes,&error)&&
		RISE::QueryFireProductionScalarPhysicalFluxPrerequisiteCPUWorkingSetBytes(
			physicalOverCap,physicalOverBytes,&error);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest emptyOverCap;
	emptyOverCap.shape=physicalOverCap;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult rejectedOverCap;
	rejectedOverCap.physicalMassFluxKGPerM2S.assign(1u,1.0f);
	rejectedOverCap.fp64ReferenceIdentityVerified=true;
	const bool physicalOverCapCallRejected=
		!RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
			emptyOverCap,rejectedOverCap,&error);
	const bool physicalOverCapCleared=
		rejectedOverCap.physicalMassFluxKGPerM2S.empty()&&
		!rejectedOverCap.fp64ReferenceIdentityVerified;
	const bool physicalOverCapError=
		error.find("working set exceeds two GiB")!=std::string::npos;
	Check(physicalAdmissionQuery&&physicalUnderBytes==UINT64_C(2147483384)&&
		physicalOverBytes==UINT64_C(2147483760)&&
		physicalUnderBytes<=(UINT64_C(2)<<30u)&&
		physicalOverBytes>(UINT64_C(2)<<30u),
		"physical scalar-flux prerequisite admits and rejects the exact two-GiB working-set neighbors without allocation");
	Check(physicalOverCapCallRejected&&physicalOverCapCleared&&physicalOverCapError,
		"physical scalar-flux prerequisite rejects an empty over-cap request before payload validation");

	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest physicalFluxRequest;
	physicalFluxRequest.shape=scalarFCT.shape;
	physicalFluxRequest.boundary.fill(RISE::FireProductionProjectionPeriodic);
	physicalFluxRequest.conservativeValues.assign(9u*scalarCells,0.0f);
	physicalFluxRequest.temperatureK.assign(scalarCells,300.0f);
	physicalFluxRequest.diffusivityM2PerS.assign(scalarCells,0.01f);
	physicalFluxRequest.conductivityWPerMK.assign(scalarCells,0.03f);
	physicalFluxRequest.ambientTemperatureK=300.0f;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){const std::size_t cell=(z*4u+y)*4u+x;
			const float methane=(x&1u)?0.2f:0.1f;
			physicalFluxRequest.conservativeValues[cell]=methane;
			physicalFluxRequest.conservativeValues[scalarCells+cell]=methane;
			physicalFluxRequest.conservativeValues[2u*scalarCells+cell]=0.2f;
			physicalFluxRequest.conservativeValues[3u*scalarCells+cell]=0.8f-methane;
			physicalFluxRequest.temperatureK[cell]=300.0f+25.0f*static_cast<float>(x);
		}
	physicalFluxRequest.ambient[0]=0.1f;physicalFluxRequest.ambient[1]=0.1f;
	physicalFluxRequest.ambient[2]=0.2f;physicalFluxRequest.ambient[3]=0.7f;
	for(unsigned int axis=0u;axis<3u;++axis){
		physicalFluxRequest.frozenVelocityMPerS[axis].assign(
			RISE::FireProductionProjectionFaceCount(physicalFluxRequest.shape,axis),0.0f);
	}
	for(unsigned int side=0u;side<6u;++side)
		physicalFluxRequest.pressureOpenInflow[side].assign(16u,0u);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult physicalFluxResult;
	const bool physicalFluxOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		physicalFluxRequest,physicalFluxResult,&error);
	const std::size_t physicalAllFaces=physicalFluxOK?
		physicalFluxResult.physicalEnergyFluxWPerM2.size():0u;
	bool physicalNonzero=false,physicalSeamExact=physicalFluxOK,
		physicalGasExact=physicalFluxOK;
	for(float value:physicalFluxResult.physicalMassFluxKGPerM2S)
		physicalNonzero=physicalNonzero||value!=0.0f;
	for(float value:physicalFluxResult.physicalEnergyFluxWPerM2)
		physicalNonzero=physicalNonzero||value!=0.0f;
	for(unsigned int axis=0u;axis<3u&&physicalGasExact;++axis)
		for(std::size_t face=0u;face<physicalFluxResult.physicalGasFluxKGPerM2S[axis].size();
			++face){float gas=0.0f;const std::size_t packed=
				physicalFluxResult.packedFaceOffset[axis]+face;
			for(std::size_t component=1u;component<=6u;++component)gas+=
				physicalFluxResult.physicalMassFluxKGPerM2S[component*physicalAllFaces+packed];
			physicalGasExact=physicalGasExact&&sameFloatBits(gas,
				physicalFluxResult.physicalGasFluxKGPerM2S[axis][face]);
		}
	for(unsigned int axis=0u;axis<3u&&physicalSeamExact;++axis){
		const std::size_t extent=4u,firstEnd=4u,secondEnd=4u;
		for(std::size_t second=0u;second<secondEnd&&physicalSeamExact;++second)
			for(std::size_t first=0u;first<firstEnd&&physicalSeamExact;++first){
				std::size_t lx=axis==0u?0u:first,ly=axis==0u?first:(axis==1u?0u:second),
					lz=axis==2u?0u:second,hx=lx,hy=ly,hz=lz;
				if(axis==0u)hx=extent;if(axis==1u)hy=extent;if(axis==2u)hz=extent;
				const std::size_t low=physicalFluxResult.packedFaceOffset[axis]+
					productionFaceIndex(physicalFluxRequest.shape,axis,lx,ly,lz);
				const std::size_t high=physicalFluxResult.packedFaceOffset[axis]+
					productionFaceIndex(physicalFluxRequest.shape,axis,hx,hy,hz);
				for(std::size_t component=0u;component<8u;++component)physicalSeamExact=
					physicalSeamExact&&sameFloatBits(physicalFluxResult.physicalMassFluxKGPerM2S[
						component*physicalAllFaces+low],physicalFluxResult.physicalMassFluxKGPerM2S[
						component*physicalAllFaces+high]);
				physicalSeamExact=physicalSeamExact&&sameFloatBits(
					physicalFluxResult.physicalEnergyFluxWPerM2[low],
					physicalFluxResult.physicalEnergyFluxWPerM2[high]);
			physicalSeamExact=physicalSeamExact&&sameFloatBits(
				physicalFluxResult.physicalGasFluxKGPerM2S[axis][
					productionFaceIndex(physicalFluxRequest.shape,axis,lx,ly,lz)],
				physicalFluxResult.physicalGasFluxKGPerM2S[axis][
					productionFaceIndex(physicalFluxRequest.shape,axis,hx,hy,hz)]);
		}
	}
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest uniformPhysical=physicalFluxRequest;
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=1u;cell<scalarCells;++cell)uniformPhysical.conservativeValues[
			component*scalarCells+cell]=uniformPhysical.conservativeValues[component*scalarCells];
	std::fill(uniformPhysical.temperatureK.begin(),uniformPhysical.temperatureK.end(),300.0f);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult uniformPhysicalResult;
	const bool uniformPhysicalOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		uniformPhysical,uniformPhysicalResult,&error);
	const bool uniformPhysicalZero=uniformPhysicalOK&&std::all_of(
		uniformPhysicalResult.physicalMassFluxKGPerM2S.begin(),
		uniformPhysicalResult.physicalMassFluxKGPerM2S.end(),[](float value){return value==0.0f;})&&
		std::all_of(uniformPhysicalResult.physicalEnergyFluxWPerM2.begin(),
		uniformPhysicalResult.physicalEnergyFluxWPerM2.end(),[](float value){return value==0.0f;});
	Check(physicalFluxOK&&physicalNonzero&&physicalSeamExact&&physicalGasExact&&
		uniformPhysicalZero&&
		physicalFluxResult.fp64ReferenceIdentityVerified&&
		physicalFluxResult.maximumFP64ReferenceResidualKGPerM2S<=
			physicalFluxResult.fp64ReferenceForwardErrorBoundKGPerM2S&&
		physicalFluxResult.maximumConstraintResidualKGPerM2S<=
			physicalFluxResult.constraintForwardErrorBoundKGPerM2S,
		"physical scalar-flux prerequisite verifies fp64 N_C identity, publishes nonzero f_N/J_g, canonicalizes periodic seams, and collapses uniform data to zero");

	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest openPhysical=physicalFluxRequest;
	openPhysical.boundary[0]=RISE::FireProductionProjectionWall;
	openPhysical.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	std::fill(openPhysical.pressureOpenInflow[1].begin(),
		openPhysical.pressureOpenInflow[1].end(),1u);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult openPhysicalResult;
	const bool openPhysicalOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		openPhysical,openPhysicalResult,&error);
	bool wallPhysicalZero=openPhysicalOK,openInflowNonzero=false;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y){
		const std::size_t lowFace=productionFaceIndex(openPhysical.shape,0u,0u,y,z),
			highFace=productionFaceIndex(openPhysical.shape,0u,4u,y,z),
			lowPacked=openPhysicalResult.packedFaceOffset[0]+lowFace,
			highPacked=openPhysicalResult.packedFaceOffset[0]+highFace;
		for(std::size_t component=0u;component<8u;++component){
			wallPhysicalZero=wallPhysicalZero&&
				openPhysicalResult.physicalMassFluxKGPerM2S[
					component*physicalAllFaces+lowPacked]==0.0f;
			openInflowNonzero=openInflowNonzero||
				openPhysicalResult.physicalMassFluxKGPerM2S[
					component*physicalAllFaces+highPacked]!=0.0f;
		}
		wallPhysicalZero=wallPhysicalZero&&
			openPhysicalResult.physicalEnergyFluxWPerM2[lowPacked]==0.0f&&
			openPhysicalResult.physicalGasFluxKGPerM2S[0][lowFace]==0.0f;
		openInflowNonzero=openInflowNonzero||
			openPhysicalResult.physicalEnergyFluxWPerM2[highPacked]!=0.0f||
			openPhysicalResult.physicalGasFluxKGPerM2S[0][highFace]!=0.0f;
	}
	std::fill(openPhysical.pressureOpenInflow[1].begin(),
		openPhysical.pressureOpenInflow[1].end(),0u);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult outflowPhysicalResult;
	const bool outflowPhysicalOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		openPhysical,outflowPhysicalResult,&error);
	bool outflowPhysicalZero=outflowPhysicalOK;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y){
		const std::size_t face=productionFaceIndex(openPhysical.shape,0u,4u,y,z),
			packed=outflowPhysicalResult.packedFaceOffset[0]+face;
		for(std::size_t component=0u;component<8u;++component)outflowPhysicalZero=
			outflowPhysicalZero&&outflowPhysicalResult.physicalMassFluxKGPerM2S[
				component*physicalAllFaces+packed]==0.0f;
		outflowPhysicalZero=outflowPhysicalZero&&
			outflowPhysicalResult.physicalEnergyFluxWPerM2[packed]==0.0f&&
			outflowPhysicalResult.physicalGasFluxKGPerM2S[0][face]==0.0f;
	}
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest badPhysical=physicalFluxRequest;
	badPhysical.diffusivityM2PerS.back()=std::numeric_limits<float>::quiet_NaN();
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult failedPhysical=physicalFluxResult;
	const bool failedPhysicalRejected=!RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		badPhysical,failedPhysical,&error)&&failedPhysical.physicalMassFluxKGPerM2S.empty()&&
		failedPhysical.physicalEnergyFluxWPerM2.empty()&&
		failedPhysical.physicalGasFluxKGPerM2S[0].empty()&&
		!failedPhysical.fp64ReferenceIdentityVerified&&!error.empty();
	auto rejectsPhysicalTemperature=[&](
		const RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest& candidate){
		RISE::FireProductionScalarPhysicalFluxPrerequisiteResult rejected=physicalFluxResult;
		return !RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
			candidate,rejected,&error)&&rejected.physicalMassFluxKGPerM2S.empty()&&
			rejected.physicalEnergyFluxWPerM2.empty()&&
			rejected.physicalGasFluxKGPerM2S[0].empty()&&
			!rejected.fp64ReferenceIdentityVerified;
	};
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest belowCell=physicalFluxRequest,
		aboveCell=physicalFluxRequest,belowAmbient=physicalFluxRequest,
		aboveAmbient=physicalFluxRequest,maximumEndpoint=uniformPhysical;
	belowCell.temperatureK.back()=std::nextafter(300.0f,0.0f);
	aboveCell.temperatureK.back()=std::nextafter(5000.0f,
		std::numeric_limits<float>::infinity());
	belowAmbient.ambientTemperatureK=std::nextafter(300.0f,0.0f);
	aboveAmbient.ambientTemperatureK=std::nextafter(5000.0f,
		std::numeric_limits<float>::infinity());
	std::fill(maximumEndpoint.temperatureK.begin(),maximumEndpoint.temperatureK.end(),5000.0f);
	maximumEndpoint.ambientTemperatureK=5000.0f;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult maximumEndpointResult;
	const bool maximumEndpointAccepted=
		RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
			maximumEndpoint,maximumEndpointResult,&error);
	Check(openPhysicalOK&&wallPhysicalZero&&openInflowNonzero&&outflowPhysicalZero&&
		failedPhysicalRejected&&rejectsPhysicalTemperature(belowCell)&&
		rejectsPhysicalTemperature(aboveCell)&&rejectsPhysicalTemperature(belowAmbient)&&
		rejectsPhysicalTemperature(aboveAmbient)&&maximumEndpointAccepted,
		"physical scalar-flux prerequisite seals boundaries, enforces the inclusive PhysicalV1 temperature domain, and fails atomically");

#if defined(__APPLE__)
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult unsupportedPhysical=physicalFluxResult;
	const bool unsupportedPhysicalRejected=
		!RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteMetal(
			physicalFluxRequest,unsupportedPhysical,&error)&&
		unsupportedPhysical.physicalMassFluxKGPerM2S.empty()&&
		!unsupportedPhysical.fp64ReferenceIdentityVerified&&
		error.find("fp64 identity qualification")!=std::string::npos;
	Check(unsupportedPhysicalRejected,
		"physical scalar-flux Metal prerequisite fails closed until fp64 qualification exists");
#endif

	RISE::FireProductionScalarFCTRequest noSourceScalarFCT=scalarFCT;
	noSourceScalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	RISE::FireProductionScalarFCTResult noSourceScalarResult;
	const bool noSourceScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		noSourceScalarFCT,noSourceScalarResult,&error);
	RISE::FireProductionScalarFCTFluxPair sourcedFluxPair,noSourceFluxPair;
	const bool sourcedFluxPairOK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		scalarFCT,sourcedFluxPair,&error);
	const bool noSourceFluxPairOK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		noSourceScalarFCT,noSourceFluxPair,&error);
	RISE::FireProductionScalarFCTResult stagedSourceCommit;
	const bool stagedSourceCommitOK=sourcedFluxPairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			scalarFCT,sourcedFluxPair,stagedSourceCommit,&error);
	Check(noSourceScalarOK&&sourcedFluxPairOK&&noSourceFluxPairOK&&
		sourcedFluxPair.lowFlux==noSourceFluxPair.lowFlux&&
		sourcedFluxPair.fluxDelta==noSourceFluxPair.fluxDelta&&stagedSourceCommitOK&&
		stagedSourceCommit.accepted==scalarFCTResult.accepted&&
		scalarFCTResult.accepted[0u]-
		noSourceScalarResult.accepted[0u]==0.125f&&
		scalarFCTResult.accepted[scalarCells]-
		noSourceScalarResult.accepted[scalarCells]==0.125f,
		"scalar FCT flux stages exclude source and the final commit applies its dose exactly once");
	// Live-oracle bootstrap: a constant dyadic tuple makes the physical record's
	// binary64 nullspace arithmetic collapse exactly to zero slope.  Production
	// publication face x+1 corresponds to the periodic oracle's upper face x.
	RISE::FireProductionScalarFCTRequest constantScalarFCT=scalarFCT;
	constantScalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	for(std::size_t cell=0u;cell<scalarCells;++cell){
		constantScalarFCT.beginning[cell]=1.0f;
		constantScalarFCT.beginning[scalarCells+cell]=1.0f;
	}
	for(std::size_t component=2u;component<9u;++component)
		std::fill(constantScalarFCT.beginning.begin()+component*scalarCells,
			constantScalarFCT.beginning.begin()+(component+1u)*scalarCells,0.0f);
	RISE::FireProductionScalarFCTResult constantScalarResult;
	const bool constantScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		constantScalarFCT,constantScalarResult,&error);
	RISE::FireProductionScalarFCTFluxPair firstStagePair,secondStagePair,averagedStagePair;
	const bool firstStagePairOK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		constantScalarFCT,firstStagePair,&error);
	RISE::FireProductionScalarFCTRequest nonfiniteStage=constantScalarFCT;
	nonfiniteStage.frozenVelocityMPerS[0][1u]=
		std::numeric_limits<float>::quiet_NaN();
	RISE::FireProductionScalarFCTFluxPair rejectedNonfinitePair=firstStagePair;
	const bool nonfinitePairRejected=!RISE::BuildFireProductionScalarFCTFluxPairCPU(
		nonfiniteStage,rejectedNonfinitePair,&error)&&rejectedNonfinitePair.lowFlux.empty();
	secondStagePair=firstStagePair;
	const std::size_t nonlinearFace=productionFaceIndex(
		constantScalarFCT.shape,0u,2u,0u,0u);
	const std::size_t nonlinearPacked=firstStagePair.packedFaceOffset[0]+nonlinearFace;
	const std::size_t nonlinearAllFaces=firstStagePair.lowFlux.size()/9u;
	secondStagePair.fluxDelta[nonlinearAllFaces+nonlinearPacked]=8.0f;
	const bool averagedStagePairOK=RISE::AverageFireProductionScalarFCTFluxPairsCPU(
		firstStagePair,secondStagePair,averagedStagePair,&error);
	RISE::FireProductionScalarFCTResult firstStageAccepted,secondStageAccepted,
		averagedStageAccepted;
	const bool firstStageAcceptedOK=firstStagePairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			constantScalarFCT,firstStagePair,firstStageAccepted,&error);
	const bool secondStageAcceptedOK=firstStagePairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			constantScalarFCT,secondStagePair,secondStageAccepted,&error);
	const bool averagedStageAcceptedOK=averagedStagePairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			constantScalarFCT,averagedStagePair,averagedStageAccepted,&error);
	RISE::FireProductionScalarFCTRequest mismatchedStageTopology=constantScalarFCT;
	mismatchedStageTopology.boundary.fill(RISE::FireProductionProjectionWall);
	RISE::FireProductionScalarFCTResult rejectedMismatchedStage;
	const bool mismatchedTopologyRejected=!RISE::SolveFireProductionScalarFCTFluxPairCPU(
		mismatchedStageTopology,firstStagePair,rejectedMismatchedStage,&error)&&
		rejectedMismatchedStage.accepted.empty();
	RISE::FireProductionScalarFCTFluxPair mismatchedAveragePair=secondStagePair;
	mismatchedAveragePair.boundary.fill(RISE::FireProductionProjectionWall);
	RISE::FireProductionScalarFCTFluxPair rejectedAveragePair;
	const bool mismatchedAverageRejected=
		!RISE::AverageFireProductionScalarFCTFluxPairsCPU(
			firstStagePair,mismatchedAveragePair,rejectedAveragePair,&error)&&
		rejectedAveragePair.lowFlux.empty();
	RISE::FireProductionScalarFCTFluxPair brokenPeriodicPair=firstStagePair;
	const std::size_t periodicHighFace=firstStagePair.packedFaceOffset[0]+
		productionFaceIndex(constantScalarFCT.shape,0u,constantScalarFCT.shape.nx,0u,0u);
	brokenPeriodicPair.lowFlux[periodicHighFace]+=1.0f;
	RISE::FireProductionScalarFCTResult rejectedPeriodicPair;
	const bool periodicSeamRejected=!RISE::SolveFireProductionScalarFCTFluxPairCPU(
		constantScalarFCT,brokenPeriodicPair,rejectedPeriodicPair,&error)&&
		rejectedPeriodicPair.accepted.empty();
	const float independentlyAveragedAlpha=firstStageAcceptedOK&&secondStageAcceptedOK?
		0.5f*(firstStageAccepted.sharedFaceAlpha[0][nonlinearFace]+
		secondStageAccepted.sharedFaceAlpha[0][nonlinearFace]):0.0f;
	Check(firstStagePairOK&&nonfinitePairRejected&&averagedStagePairOK&&firstStageAcceptedOK&&
		secondStageAcceptedOK&&averagedStageAcceptedOK&&
		mismatchedTopologyRejected&&mismatchedAverageRejected&&periodicSeamRejected&&
		averagedStagePair.fluxDelta[nonlinearAllFaces+nonlinearPacked]==4.0f&&
		averagedStageAccepted.sharedFaceAlpha[0][nonlinearFace]!=
			independentlyAveragedAlpha,
		"projected-Heun scalar staging averages flux pairs and solves a fresh shared alpha, not an averaged alpha");

	// r185 granular composition: build each 30F stage from the same raw Q/u/
	// boundary operands, retain f_N/J_g/Phi_g separately, average every field,
	// and consume the retained advective delta without reconstructing high-low.
	RISE::FireProductionProjectionShape heunPayloadUnder,heunPayloadOver;
	heunPayloadUnder.nx=38u;heunPayloadUnder.ny=191u;heunPayloadUnder.nz=813u;
	heunPayloadUnder.cellWidthM=1.0f;
	heunPayloadOver.nx=18u;heunPayloadOver.ny=500u;heunPayloadOver.nz=650u;
	heunPayloadOver.cellWidthM=1.0f;
	std::uint64_t heunPayloadUnderBytes=0u,heunPayloadOverBytes=0u;
	const bool heunPayloadQuery=
		RISE::QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
			heunPayloadUnder,heunPayloadUnderBytes,&error)&&
		RISE::QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
			heunPayloadOver,heunPayloadOverBytes,&error);
	Check(heunPayloadQuery&&heunPayloadUnderBytes==UINT64_C(2147483640)&&
		heunPayloadOverBytes==UINT64_C(2147484000)&&
		heunPayloadUnderBytes<=(UINT64_C(2)<<30u)&&
		heunPayloadOverBytes>(UINT64_C(2)<<30u),
		"r185 Heun stage payload query exact-binds the 30F two-GiB neighbors");
	RISE::FireProductionScalarFCTRequest overComposeFCT;
	overComposeFCT.shape.nx=36u;overComposeFCT.shape.ny=115u;
	overComposeFCT.shape.nz=736u;overComposeFCT.shape.cellWidthM=1.0f;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest overComposePhysical;
	overComposePhysical.shape=overComposeFCT.shape;
	RISE::FireProductionScalarHeunFluxStage refusedHeunStage;
	refusedHeunStage.physicalEnergyFluxWPerM2.assign(1u,1.0f);
	const bool composeOverCapRejected=
		!RISE::ComposeFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),
			RISE::FireProductionScalarHeunFluxRole::R0,overComposeFCT,
			overComposePhysical,refusedHeunStage,&error)&&
		refusedHeunStage.physicalEnergyFluxWPerM2.empty()&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	RISE::FireProductionScalarHeunFluxStage overAverageFirst,overAverageSecond,
		refusedAverageStage;
	overAverageFirst.compositeFluxPair.shape.nx=25u;
	overAverageFirst.compositeFluxPair.shape.ny=140u;
	overAverageFirst.compositeFluxPair.shape.nz=559u;
	overAverageFirst.compositeFluxPair.shape.cellWidthM=1.0f;
	overAverageSecond.compositeFluxPair.shape=overAverageFirst.compositeFluxPair.shape;
	const bool averageOverCapRejected=
		!RISE::AverageFireProductionScalarHeunFluxStagesCPU(overAverageFirst,
			overAverageSecond,refusedAverageStage,&error)&&
		refusedAverageStage.compositeFluxPair.lowFlux.empty()&&
		error.find("average working set exceeds two GiB")!=std::string::npos;
	RISE::FireProductionScalarHeunFluxStage mismatchedAverageFirst,
		mismatchedAverageSecond;
	mismatchedAverageFirst.compositeFluxPair.shape.nx=4u;
	mismatchedAverageFirst.compositeFluxPair.shape.ny=4u;
	mismatchedAverageFirst.compositeFluxPair.shape.nz=4u;
	mismatchedAverageFirst.compositeFluxPair.shape.cellWidthM=1.0f;
	mismatchedAverageSecond.compositeFluxPair.shape=heunPayloadOver;
	const bool mismatchedLargeAverageRejected=
		!RISE::AverageFireProductionScalarHeunFluxStagesCPU(mismatchedAverageFirst,
			mismatchedAverageSecond,refusedAverageStage,&error)&&
		error.find("average working set exceeds two GiB")!=std::string::npos;
	Check(composeOverCapRejected&&averageOverCapRejected&&
		mismatchedLargeAverageRejected,
		"r185 composition and three-stage averaging reject their exact live-set over-cap shapes before payload access");

	RISE::FireProductionScalarFCTRequest heunR0=constantScalarFCT;
	heunR0.beginning=physicalFluxRequest.conservativeValues;
	heunR0.sourceDelta.assign(9u*scalarCells,0.0f);
	heunR0.ambient=physicalFluxRequest.ambient;
	for(std::size_t species=0u;species<7u;++species){
		heunR0.enthalpyBoundsJPerKG[species]=-1.0e8f;
		heunR0.enthalpyBoundsJPerKG[7u+species]=1.0e8f;
	}
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest heunPhysicalR0=
		physicalFluxRequest;
	const float heunMethanePattern[4]={0.1f,0.125f,0.15f,0.125f};
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){const std::size_t cell=(z*4u+y)*4u+x;
			const float methane=heunMethanePattern[x];
			heunPhysicalR0.conservativeValues[cell]=methane;
			heunPhysicalR0.conservativeValues[scalarCells+cell]=methane;
			heunPhysicalR0.conservativeValues[3u*scalarCells+cell]=0.8f-methane;
		}
	heunR0.beginning=heunPhysicalR0.conservativeValues;
	for(unsigned int axis=0u;axis<3u;++axis){
		const float velocity=axis==0u?0.1f:0.0f;
		std::fill(heunR0.frozenVelocityMPerS[axis].begin(),
			heunR0.frozenVelocityMPerS[axis].end(),velocity);
		heunPhysicalR0.frozenVelocityMPerS[axis]=heunR0.frozenVelocityMPerS[axis];
	}
	RISE::FireProductionScalarFCTRequest heunR1=heunR0;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest heunPhysicalR1=
		heunPhysicalR0;
	for(std::size_t cell=0u;cell<scalarCells;++cell)
		heunPhysicalR1.temperatureK[cell]+=5.0f;
	for(unsigned int axis=0u;axis<3u;++axis){
		const float velocity=axis==0u?0.075f:0.0f;
		std::fill(heunR1.frozenVelocityMPerS[axis].begin(),
			heunR1.frozenVelocityMPerS[axis].end(),velocity);
		heunPhysicalR1.frozenVelocityMPerS[axis]=heunR1.frozenVelocityMPerS[axis];
	}
	RISE::FireProductionScalarHeunFluxStage heunStageR0,heunStageR1,heunStageAverage;
	RISE::FireProductionScalarFCTFluxPair heunAdvectiveR0;
	const bool heunAdvectiveR0OK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		heunR0,heunAdvectiveR0,&error);
	const bool heunR0OK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		UINT64_C(18501),RISE::FireProductionScalarHeunFluxRole::R0,heunR0,
		heunPhysicalR0,heunStageR0,&error);
	const bool heunR1OK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		UINT64_C(18501),RISE::FireProductionScalarHeunFluxRole::R1,heunR1,
		heunPhysicalR1,heunStageR1,&error);
	const bool heunAverageOK=heunR0OK&&heunR1OK&&
		RISE::AverageFireProductionScalarHeunFluxStagesCPU(
			heunStageR0,heunStageR1,heunStageAverage,&error);
	RISE::FireProductionScalarHeunSolveResult heunSolve;
	const bool heunSolveOK=heunAverageOK&&
		RISE::SolveFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),heunR0,
			heunStageAverage,heunSolve,&error);
	const RISE::FireProductionScalarFCTResult& heunAccepted=heunSolve.scalar;
	RISE::FireProductionCompatibleFCTMomentumResult heunMomentumR0,heunMomentumR1;
	const bool heunMomentumR0OK=heunSolveOK&&
		RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(heunStageR0,
			heunSolve,heunR0.frozenVelocityMPerS,heunMomentumR0,&error);
	const bool heunMomentumR1OK=heunSolveOK&&
		RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(heunStageR1,
			heunSolve,heunR1.frozenVelocityMPerS,heunMomentumR1,&error);
	bool heunCompositionExact=heunAdvectiveR0OK&&heunR0OK&&heunR1OK&&heunAverageOK;
	for(std::size_t component=0u;component<8u&&heunCompositionExact;++component)
		for(std::size_t face=0u;face<physicalAllFaces&&heunCompositionExact;++face){
			const std::size_t index=component*physicalAllFaces+face;
			heunCompositionExact=sameFloatBits(
				heunStageR0.compositeFluxPair.lowFlux[index],
				heunAdvectiveR0.lowFlux[index]+heunStageR0.physicalMassFluxKGPerM2S[index])&&
				sameFloatBits(heunStageR0.compositeFluxPair.fluxDelta[index],
					heunAdvectiveR0.fluxDelta[index]);
		}
	const std::size_t heunFace=productionFaceIndex(heunR0.shape,0u,2u,0u,0u);
	const float directExpected=heunStageR0.advectiveGasLowFluxKGPerM2S[0][heunFace]+
		heunAccepted.sharedFaceAlpha[0][heunFace]*
		heunStageR0.advectiveGasFluxDeltaKGPerM2S[0][heunFace]+
		heunStageR0.physicalGasFluxKGPerM2S[0][heunFace];
	const bool directDeltaExact=heunMomentumR0OK&&sameFloatBits(directExpected,
		heunMomentumR0.acceptedGasFluxKGPerM2S[0][heunFace]);
	RISE::FireProductionCompatibleFCTMomentumRequest reconstructedHeun;
	reconstructedHeun.shape=heunR0.shape;reconstructedHeun.boundary=heunR0.boundary;
	reconstructedHeun.lowGasFluxKGPerM2S=heunStageR0.advectiveGasLowFluxKGPerM2S;
	reconstructedHeun.physicalGasFluxKGPerM2S=heunStageR0.physicalGasFluxKGPerM2S;
	reconstructedHeun.sharedFaceAlpha=heunAccepted.sharedFaceAlpha;
	reconstructedHeun.frozenVelocityMPerS=heunR0.frozenVelocityMPerS;
	for(unsigned int axis=0u;axis<3u;++axis){
		reconstructedHeun.highGasFluxKGPerM2S[axis].resize(
			heunStageR0.advectiveGasLowFluxKGPerM2S[axis].size());
		for(std::size_t face=0u;face<reconstructedHeun.highGasFluxKGPerM2S[axis].size();
			++face)reconstructedHeun.highGasFluxKGPerM2S[axis][face]=
				heunStageR0.advectiveGasLowFluxKGPerM2S[axis][face]+
				heunStageR0.advectiveGasFluxDeltaKGPerM2S[axis][face];
	}
	RISE::FireProductionCompatibleFCTMomentumResult reconstructedMomentum;
	const bool reconstructedMomentumOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		reconstructedHeun,reconstructedMomentum,&error);
	bool cancellationWitness=false;
	for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<reconstructedHeun.highGasFluxKGPerM2S[axis].size();
			++face)cancellationWitness=cancellationWitness||!sameFloatBits(
				heunStageR0.advectiveGasFluxDeltaKGPerM2S[axis][face],
				reconstructedHeun.highGasFluxKGPerM2S[axis][face]-
					reconstructedHeun.lowGasFluxKGPerM2S[axis][face]);
	Check(heunR0OK&&heunR1OK&&heunAverageOK,
		"r185 authenticates and composes distinct R0/R1 flux stages");
	Check(heunCompositionExact,
		"r185 composite low adds f_N once while the antidiffusive delta remains advective");
	Check(heunSolveOK,
		"r185 averaged composite accepts one fresh shared-alpha solve");
	Check(heunMomentumR0OK&&heunMomentumR1OK,
		"r185 direct-delta compatible momentum accepts both original stage velocities");
	Check(heunCompositionExact&&heunSolveOK&&heunMomentumR0OK&&heunMomentumR1OK&&
		heunStageAverage.role==RISE::FireProductionScalarHeunFluxRole::HeunAverage&&
		heunStageAverage.attemptIdentity==UINT64_C(18501),
		"r185 retains and averages all 30F fields and solves one fresh alpha");
	Check(directDeltaExact,
		"r185 compatible momentum consumes retained DeltaPhi_g in its exact published operation order");
	Check(reconstructedMomentumOK&&cancellationWitness,
		"r185 cancellation fixture distinguishes direct DeltaPhi_g from reconstructed high-low");

	auto rejectsMutatedHeun=[&](RISE::FireProductionScalarHeunFluxStage candidate){
		RISE::FireProductionCompatibleFCTMomentumResult rejected;
		rejected.acceptedGasFluxKGPerM2S[0].assign(1u,1.0f);
		return !RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(candidate,
			heunSolve,heunR0.frozenVelocityMPerS,rejected,&error)&&
			rejected.acceptedGasFluxKGPerM2S[0].empty();
	};
	RISE::FireProductionScalarHeunFluxStage mutatedHeun=heunStageR0;
	mutatedHeun.physicalMassFluxKGPerM2S[0]=std::nextafter(
		mutatedHeun.physicalMassFluxKGPerM2S[0],std::numeric_limits<float>::infinity());
	const bool mutatedMassRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.physicalEnergyFluxWPerM2[0]=std::nextafter(
		mutatedHeun.physicalEnergyFluxWPerM2[0],std::numeric_limits<float>::infinity());
	const bool mutatedEnergyRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.physicalGasFluxKGPerM2S[0][heunFace]=
		std::nextafter(mutatedHeun.physicalGasFluxKGPerM2S[0][heunFace],
			std::numeric_limits<float>::infinity());
	const bool mutatedJgRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.advectiveGasLowFluxKGPerM2S[0][heunFace]=
		std::nextafter(mutatedHeun.advectiveGasLowFluxKGPerM2S[0][heunFace],
			std::numeric_limits<float>::infinity());
	const bool mutatedPhiLowRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.advectiveGasFluxDeltaKGPerM2S[0][heunFace]=
		std::nextafter(mutatedHeun.advectiveGasFluxDeltaKGPerM2S[0][heunFace],
			std::numeric_limits<float>::infinity());
	const bool mutatedPhiDeltaRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.compositeFluxPair.lowFlux[0]=std::nextafter(
		mutatedHeun.compositeFluxPair.lowFlux[0],std::numeric_limits<float>::infinity());
	const bool mutatedCompositeLowRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.compositeFluxPair.fluxDelta[0]=std::nextafter(
		mutatedHeun.compositeFluxPair.fluxDelta[0],std::numeric_limits<float>::infinity());
	const bool mutatedCompositeDeltaRejected=rejectsMutatedHeun(mutatedHeun);
	std::array<std::vector<float>,3> staleHeunVelocity=heunR0.frozenVelocityMPerS;
	staleHeunVelocity[0][heunFace]=std::nextafter(staleHeunVelocity[0][heunFace],
		std::numeric_limits<float>::infinity());
	RISE::FireProductionCompatibleFCTMomentumResult staleVelocityResult;
	const bool staleVelocityRejected=!RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(
		heunStageR0,heunSolve,staleHeunVelocity,
		staleVelocityResult,&error);
	RISE::FireProductionScalarHeunSolveResult staleAlphaSolve=heunSolve;
	staleAlphaSolve.scalar.sharedFaceAlpha[0][heunFace]=std::nextafter(
		staleAlphaSolve.scalar.sharedFaceAlpha[0][heunFace],0.0f);
	RISE::FireProductionCompatibleFCTMomentumResult staleAlphaResult;
	const bool staleAlphaRejected=!RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(
		heunStageR0,staleAlphaSolve,heunR0.frozenVelocityMPerS,
		staleAlphaResult,&error)&&staleAlphaResult.acceptedGasFluxKGPerM2S[0].empty();
	RISE::FireProductionScalarHeunFluxStage mutatedAverageStage=heunStageAverage;
	mutatedAverageStage.compositeFluxPair.lowFlux[0]=std::nextafter(
		mutatedAverageStage.compositeFluxPair.lowFlux[0],
		std::numeric_limits<float>::infinity());
	RISE::FireProductionScalarHeunSolveResult refusedMutatedAverage;
	const bool mutatedAverageRejected=
		!RISE::SolveFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),heunR0,
			mutatedAverageStage,refusedMutatedAverage,&error)&&
		refusedMutatedAverage.scalar.accepted.empty();
	RISE::FireProductionScalarFCTRequest incompatibleHeunR1=heunR1;
	incompatibleHeunR1.sourceDelta[0]=0.25f;
	RISE::FireProductionScalarHeunFluxStage incompatibleStageR1,
		refusedIncompatibleAverage;
	const bool incompatibleStageBuilt=
		RISE::ComposeFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),
			RISE::FireProductionScalarHeunFluxRole::R1,incompatibleHeunR1,
			heunPhysicalR1,incompatibleStageR1,&error);
	const bool sharedContractRejected=incompatibleStageBuilt&&
		!RISE::AverageFireProductionScalarHeunFluxStagesCPU(heunStageR0,
			incompatibleStageR1,refusedIncompatibleAverage,&error)&&
		refusedIncompatibleAverage.compositeFluxPair.lowFlux.empty();
	RISE::FireProductionScalarHeunFluxStage swappedAverage;
	const bool swappedRolesRejected=!RISE::AverageFireProductionScalarHeunFluxStagesCPU(
		heunStageR1,heunStageR0,swappedAverage,&error);
	Check(mutatedMassRejected&&mutatedEnergyRejected&&mutatedJgRejected&&
		mutatedPhiLowRejected&&mutatedPhiDeltaRejected&&mutatedCompositeLowRejected&&
		mutatedCompositeDeltaRejected,
		"r185 all seven retained field classes are content-identity protected");
	Check(staleVelocityRejected&&staleAlphaRejected&&mutatedAverageRejected,
		"r185 stale velocity, stale alpha, and mutated averaged-stage consumers refuse");
	Check(sharedContractRejected&&swappedRolesRejected,
		"r185 shared FCT contract and R0/R1 ordering are identity-bearing");
	Check(incompatibleStageBuilt,
		"r185 can construct a valid R1 witness carrying a deliberately different shared source contract");
	Check(sharedContractRejected,
		"r185 refuses an R0/R1 shared-contract mismatch before averaging payloads");
	Check(swappedRolesRejected,
		"r185 refuses swapped R1/R0 stage roles");
	Check(mutatedMassRejected&&mutatedEnergyRejected&&mutatedJgRejected&&
		mutatedPhiLowRejected&&mutatedPhiDeltaRejected&&mutatedCompositeLowRejected&&
		mutatedCompositeDeltaRejected&&staleVelocityRejected&&staleAlphaRejected&&
		mutatedAverageRejected&&sharedContractRejected&&swappedRolesRejected,
		"r185 content, shared-contract, fresh-alpha, stage-role, and frozen-velocity identities reject every transplant atomically");
	RISE::FireProductionScalarFCTRequest openScalarFCT=constantScalarFCT;
	openScalarFCT.boundary={{RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionPressureOpen,RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionWall,RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionWall}};
	openScalarFCT.pressureOpenInflow[1].assign(16u,1u);
	openScalarFCT.ambient[0]=2.0f;openScalarFCT.ambient[1]=2.0f;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		openScalarFCT.frozenVelocityMPerS[0][productionFaceIndex(
			openScalarFCT.shape,0u,4u,y,z)]=-1.0f;
	RISE::FireProductionScalarFCTResult openScalarResult;
	const bool openScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		openScalarFCT,openScalarResult,&error);
	const std::size_t openScalarWall=productionFaceIndex(openScalarFCT.shape,0u,0u,0u,0u);
	const std::size_t openScalarInflow=productionFaceIndex(openScalarFCT.shape,0u,4u,0u,0u);
	const std::size_t openScalarAllFaces=openScalarOK?openScalarResult.lowFlux.size()/9u:0u;
	Check(openScalarOK&&openScalarResult.lowFlux[openScalarWall]==0.0f&&
		openScalarResult.fluxDelta[openScalarWall]==0.0f&&
		openScalarResult.lowFlux[openScalarInflow]==-2.0f&&
		openScalarResult.lowFlux[openScalarAllFaces+openScalarInflow]==-2.0f&&
		openScalarResult.fluxDelta[openScalarInflow]==0.0f,
		"scalar FCT donor pair applies wall zero-flux and pressure-open inflow ghosts exactly");
	std::vector<RISE::FireSim::ConservativeVector> constantOracleState(scalarCells);
	for(RISE::FireSim::ConservativeVector& state:constantOracleState){state[0]=1.0;state[1]=1.0;}
	RISE::FireSim::PeriodicMACField constantOracleVelocity;
	for(unsigned int axis=0u;axis<3u;++axis)constantOracleVelocity.component[axis].assign(
		scalarCells,axis==0u?1.0:0.0);
	std::vector<double> constantTemperature(scalarCells,300.0),constantZero(scalarCells,0.0);
	RISE::FireSim::PeriodicFluxPair3D constantOraclePair;
	const RISE::FireSimulationMethaneRecord& physicalRecord=
		RISE::FireSimulationMethaneRecord::PhysicalV1();
	const bool constantOracleOK=RISE::FireSim::BuildPeriodicFluxPair3D(
		periodicOracleShape,constantOracleState,constantTemperature,constantOracleVelocity,
		constantZero,constantZero,physicalRecord,physicalRecord,constantOraclePair,&error);
	bool constantPairExact=constantScalarOK&&constantOracleOK;
	for(unsigned int axis=0u;axis<3u&&constantPairExact;++axis)
		for(std::size_t z=0u;z<4u&&constantPairExact;++z)
			for(std::size_t y=0u;y<4u&&constantPairExact;++y)
				for(std::size_t x=0u;x<4u&&constantPairExact;++x){
					const std::size_t oracleFace=(z*4u+y)*4u+x;
					std::size_t px=x,py=y,pz=z;
					if(axis==0u)++px;if(axis==1u)++py;if(axis==2u)++pz;
					const std::size_t productionFace=productionFaceIndex(
						constantScalarFCT.shape,axis,px,py,pz);
					const std::size_t packed=constantScalarResult.packedFaceOffset[axis]+
						productionFace;
					for(std::size_t component=0u;component<9u&&constantPairExact;++component){
						const double low=constantOraclePair.low[axis][oracleFace][component];
						const double delta=constantOraclePair.high[axis][oracleFace][component]-low;
						constantPairExact=sameDoubleBits(static_cast<double>(constantScalarResult.
							lowFlux[component*constantScalarResult.lowFlux.size()/9u+packed]),low)&&
							sameDoubleBits(static_cast<double>(constantScalarResult.fluxDelta[
							component*constantScalarResult.fluxDelta.size()/9u+packed]),delta);
					}
				}
	Check(constantPairExact,
		"strict fp32 donor/MC pair is word-exact to the live periodic oracle on the dyadic collapse");

	// The nonconstant live bootstrap uses the oracle's actual invariant-MC
	// routine with the exact one-row orthonormal basis e_CH4.  Thus the
	// binary64 operations collapse exactly to binary32 on a nonzero slope,
	// while the zero assembly budget makes the independently-known shared
	// limiter answer alpha=0 on precisely the two antidiffusive faces.
	RISE::FireProductionScalarFCTRequest liveFrontScalarFCT=scalarFCT;
	liveFrontScalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	liveFrontScalarFCT.nullspaceBasis.assign(8u,0.0f);
	liveFrontScalarFCT.nullspaceBasis[1]=1.0f;
	liveFrontScalarFCT.coordinateProjector.assign(1u,1.0f);
	liveFrontScalarFCT.assemblyReserveFactor=liveFrontScalarFCT.feasibilityFactor;
	RISE::FireProductionScalarFCTResult liveFrontScalarResult;
	const bool liveFrontScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		liveFrontScalarFCT,liveFrontScalarResult,&error);
	RISE::FireCertifiedNullspace liveFrontClosure;
	liveFrontClosure.stateDimension=8u;liveFrontClosure.nullity=1u;
	liveFrontClosure.orthonormalBasis.assign(8u,0.0);
	liveFrontClosure.orthonormalBasis[1]=1.0;
	std::vector<RISE::FireSim::ConservativeVector> liveFrontLine(4u);
	for(std::size_t x=0u;x<4u;++x){
		liveFrontLine[x][0]=static_cast<double>(scalarPattern[x]);
		liveFrontLine[x][1]=static_cast<double>(scalarPattern[x]);
	}
	std::vector<std::array<double,8> > liveFrontSlope;
	const bool liveFrontOracleOK=RISE::FireSim::InvariantMCMassSlopes(
		liveFrontLine,liveFrontClosure,liveFrontSlope,&error);
	bool liveFrontPairExact=liveFrontScalarOK&&liveFrontOracleOK;
	for(std::size_t z=0u;z<4u&&liveFrontPairExact;++z)
		for(std::size_t y=0u;y<4u&&liveFrontPairExact;++y)
			for(std::size_t x=0u;x<4u&&liveFrontPairExact;++x){
				const std::size_t face=productionFaceIndex(
					liveFrontScalarFCT.shape,0u,x+1u,y,z);
				const std::size_t packed=liveFrontScalarResult.packedFaceOffset[0]+face;
				for(std::size_t component=0u;component<8u&&liveFrontPairExact;++component){
					const double oracleLow=liveFrontLine[x][component];
					const double oracleDelta=0.5*liveFrontSlope[x][component];
					liveFrontPairExact=sameDoubleBits(static_cast<double>(liveFrontScalarResult.
						lowFlux[component*liveFrontScalarResult.lowFlux.size()/9u+packed]),
						oracleLow)&&sameDoubleBits(static_cast<double>(liveFrontScalarResult.
						fluxDelta[component*liveFrontScalarResult.fluxDelta.size()/9u+packed]),
						oracleDelta);
				}
				const bool antidiffusive=x==1u||x==3u;
				liveFrontPairExact=liveFrontPairExact&&liveFrontScalarResult.
					sharedFaceAlpha[0][face]==(antidiffusive?0.0f:1.0f)&&
					liveFrontScalarResult.acceptedGasFluxKGPerM2S[0][face]==
					liveFrontScalarResult.lowFlux[liveFrontScalarResult.lowFlux.size()/9u+packed];
			}
	for(std::size_t value=0u;value<liveFrontScalarResult.accepted.size()&&
		liveFrontPairExact;++value)liveFrontPairExact=liveFrontScalarResult.accepted[value]==
		liveFrontScalarResult.lowState[value];
	Check(liveFrontPairExact,
		"nonconstant dyadic MC slopes match the live oracle and bind shared-alpha acceptance");

	// The production PPM dose and an evolving carrier are rejected operator
	// mutations: this fixture must distinguish both from the frozen MC/FCT form.
	RISE::FireProductionRemapRequest ppmMutation;
	ppmMutation.lineLength=4u;ppmMutation.lineCount=1u;ppmMutation.componentCount=1u;
	ppmMutation.cellWidthM=1.0f;ppmMutation.timeStepS=scalarFCT.timeStepS;
	ppmMutation.boundary=RISE::FireProductionRemapPeriodic;
	ppmMutation.values.assign(scalarPattern,scalarPattern+4u);
	ppmMutation.faceVelocityMPerS.assign(5u,1.0f);
	ppmMutation.ambientValues.assign(1u,scalarPattern[0]);
	RISE::FireProductionRemapResult ppmMutationResult;
	const bool ppmMutationOK=RISE::RemapFireProductionCPU(
		ppmMutation,ppmMutationResult,&error);
	bool ppmDistinguished=false;
	for(std::size_t face=0u;face<4u&&ppmMutationOK;++face){
		const std::size_t productionFace=productionFaceIndex(scalarFCT.shape,0u,
			face+1u,0u,0u);
		const std::size_t packed=scalarFCTResult.packedFaceOffset[0]+productionFace;
		const float fctDose=scalarFCT.timeStepS*(scalarFCTResult.lowFlux[packed]+
			scalarFCTResult.sharedFaceAlpha[0][productionFace]*
			scalarFCTResult.fluxDelta[packed]);
		ppmDistinguished=ppmDistinguished||ppmMutationResult.faceFluxes[face]!=fctDose;
	}
	RISE::FireProductionCompatibleFCTMomentumRequest frozenCarrierFCT;
	frozenCarrierFCT.shape=scalarFCT.shape;frozenCarrierFCT.boundary=scalarFCT.boundary;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(scalarFCT.shape,axis);
		frozenCarrierFCT.lowGasFluxKGPerM2S[axis].assign(faces,0.0f);
		frozenCarrierFCT.highGasFluxKGPerM2S[axis].assign(faces,0.0f);
		frozenCarrierFCT.sharedFaceAlpha[axis]=scalarFCTResult.sharedFaceAlpha[axis];
		frozenCarrierFCT.frozenVelocityMPerS[axis]=scalarFCT.frozenVelocityMPerS[axis];
		for(std::size_t face=0u;face<faces;++face){
			const std::size_t packed=scalarFCTResult.packedFaceOffset[axis]+face;
			for(std::size_t component=1u;component<=6u;++component){
				frozenCarrierFCT.lowGasFluxKGPerM2S[axis][face]+=
					scalarFCTResult.lowFlux[component*scalarFCTResult.lowFlux.size()/9u+packed];
				frozenCarrierFCT.highGasFluxKGPerM2S[axis][face]+=
					scalarFCTResult.lowFlux[component*scalarFCTResult.lowFlux.size()/9u+packed]+
					scalarFCTResult.fluxDelta[component*scalarFCTResult.fluxDelta.size()/9u+packed];
			}
		}
	}
	RISE::FireProductionCompatibleFCTMomentumResult frozenCarrierRate,evolvingCarrierRate;
	const bool frozenCarrierOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		frozenCarrierFCT,frozenCarrierRate,&error);
	RISE::FireProductionCompatibleFCTMomentumRequest evolvingCarrierFCT=frozenCarrierFCT;
	evolvingCarrierFCT.frozenVelocityMPerS[0][scalarFace2]=2.0f;
	const bool evolvingCarrierOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		evolvingCarrierFCT,evolvingCarrierRate,&error);
	bool evolvingCarrierDistinguished=false;
	for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<frozenCarrierRate.advectionRateKGPerM2S2[axis].size();++face)
			evolvingCarrierDistinguished=evolvingCarrierDistinguished||
				frozenCarrierRate.advectionRateKGPerM2S2[axis][face]!=
				evolvingCarrierRate.advectionRateKGPerM2S2[axis][face];
	Check(ppmMutationOK&&ppmDistinguished&&frozenCarrierOK&&evolvingCarrierOK&&
		evolvingCarrierDistinguished,
		"scalar FCT bootstrap detects the rejected PPM-dose and evolving-carrier mutations");

	RISE::FireProductionScalarFCTRequest malformedScalarFCT=scalarFCT;
	malformedScalarFCT.nullspaceBasis.pop_back();
	RISE::FireProductionScalarFCTResult refusedScalarFCT;
	refusedScalarFCT.accepted.assign(1u,123.0f);
	const bool badBasisRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error)&&refusedScalarFCT.accepted.empty();
	malformedScalarFCT=scalarFCT;malformedScalarFCT.sourceDelta.pop_back();
	const bool badSourceRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=scalarFCT;malformedScalarFCT.pressureOpenInflow[0][0]=2u;
	const bool badInflowRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=scalarFCT;malformedScalarFCT.pressureOpenInflow[0][0]=1u;
	const bool inactiveInflowRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=constantScalarFCT;
	malformedScalarFCT.sourceDelta[scalarCells]=-2.0f;
	const bool infeasibleLowRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=scalarFCT;malformedScalarFCT.frozenVelocityMPerS[0].back()=-0.0f;
	const bool badVelocitySeamRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	Check(badBasisRefused&&badSourceRefused&&badInflowRefused&&inactiveInflowRefused&&
		infeasibleLowRefused&&badVelocitySeamRefused,
		"scalar FCT stages fail closed on malformed certificate, low state, inflow, and seam identity");

	RISE::FireProductionDualMomentumRequest dual32;
	dual32.shape=cell32.shape;dual32.timeStepS=cell32.timeStepS;
	dual32.ambientDensityKGPerM3=1.0f;dual32.boundary=cell32.boundary;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(dual32.shape,axis);
		dual32.beginningFaceDensity[axis].resize(faces);
		dual32.beginningMomentum[axis].resize(faces);
		dual32.frozenVelocityMPerS[axis].resize(faces);
		for(std::size_t face=0u;face<faces;++face){
			const float density=1.0f+0.001f*static_cast<float>(face%17u);
			const float velocity=0.01f*static_cast<float>(axis+1u);
			dual32.beginningFaceDensity[axis][face]=density;
			dual32.beginningMomentum[axis][face]=density*velocity;
			dual32.frozenVelocityMPerS[axis][face]=velocity;
		}
	}
	RISE::FireProductionDualMomentumResult dual32Result;
	const bool dual32OK=RISE::RemapFireProductionDualMomentumCPU(dual32,dual32Result,&error);
	if(!dual32OK)std::fprintf(stderr,"dual stage witness failed: %s\n",error.c_str());
	Check(dual32OK,
		"binary32 dual palindrome accepts the stage-reset witness");
	RISEFireProductionTrace::FireProductionDualMomentumRequest tracedDual;
	tracedDual.shape=tracedCell.shape;tracedDual.timeStepS=dual32.timeStepS;
	tracedDual.ambientDensityKGPerM3=dual32.ambientDensityKGPerM3;
	for(unsigned int side=0u;side<6u;++side)tracedDual.boundary[side]=
		static_cast<RISEFireProductionTrace::FireProductionProjectionBoundary>(
			dual32.boundary[side]);
	for(unsigned int axis=0u;axis<3u;++axis){
		tracedDual.beginningFaceDensity[axis].assign(dual32.beginningFaceDensity[axis].begin(),
			dual32.beginningFaceDensity[axis].end());
		tracedDual.beginningMomentum[axis].assign(dual32.beginningMomentum[axis].begin(),
			dual32.beginningMomentum[axis].end());
		tracedDual.frozenVelocityMPerS[axis].assign(dual32.frozenVelocityMPerS[axis].begin(),
			dual32.frozenVelocityMPerS[axis].end());
	}
	RISEFireProductionTrace::FireProductionDualMomentumResult tracedDualResult;
	FireProductionRoundoffTrace::Counters tracedDualCounters;bool tracedDualOK=false;{
		FireProductionRoundoffTrace::Scope scope(tracedDualCounters);
		tracedDualOK=RISEFireProductionTrace::RemapFireProductionDualMomentumCPU(
			tracedDual,tracedDualResult,&error);
	}
	bool tracedDualBytes=true;
	for(unsigned int axis=0u;axis<3u&&tracedDualBytes;++axis){
		tracedDualBytes=tracedDualResult.auxiliaryFaceDensity[axis].size()==
			dual32Result.auxiliaryFaceDensity[axis].size()&&
			tracedDualResult.momentum[axis].size()==dual32Result.momentum[axis].size();
		for(std::size_t face=0u;tracedDualBytes&&
			face<tracedDualResult.momentum[axis].size();++face)
			tracedDualBytes=tracedDualResult.auxiliaryFaceDensity[axis][face].Rounded()==
				dual32Result.auxiliaryFaceDensity[axis][face]&&
				tracedDualResult.momentum[axis][face].Rounded()==dual32Result.momentum[axis][face]&&
				tracedDualResult.auxiliaryFaceDensity[axis][face].Radius()==0.0&&
				tracedDualResult.momentum[axis][face].Radius()==0.0;
	}
	bool completeDualStages=tracedDualCounters.sealedStages.size()==15u;
	for(const FireProductionRoundoffTrace::Observation& stage:tracedDualCounters.sealedStages){
		std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)operations+=count;
		completeDualStages=completeDualStages&&operations>0u&&stage.maximumDepth>0u&&
			stage.maximumOutputRadius>0.0&&std::isfinite(stage.maximumOutputRadius);
	}
	Check(tracedDualOK&&tracedDualBytes&&completeDualStages,
		"dual palindrome trace seals fifteen local certificates and resets radii without changing fp32 bytes");

	RISE::FireProductionProjectionRequest projection32;
	projection32.shape.nx=4u;projection32.shape.ny=4u;projection32.shape.nz=4u;
	projection32.shape.cellWidthM=0.5f;projection32.timeStepS=0.01f;
	projection32.ambientDensityKGPerM3=1.0f;
	projection32.boundary.fill(RISE::FireProductionProjectionPeriodic);
	projection32.gasDensityKGPerM3.assign(64u,1.0f);
	projection32.provisionalMomentumKGPerM2S[0].assign(FaceCount(4u,16u),0.0f);
	projection32.provisionalMomentumKGPerM2S[1].assign(FaceCount(4u,16u),0.0f);
	projection32.provisionalMomentumKGPerM2S[2].assign(FaceCount(4u,16u),0.0f);
	projection32.divergenceTargetPerS.assign(64u,0.0f);
	RISE::FireProductionProjectionResult projection32Result;
	Check(RISE::ProjectFireProductionCPU(projection32,projection32Result,&error)&&
		projection32Result.maximumPostProjectionResidualPerS==0.0f,
		"binary32 production projection exact-zero owner");
	RISEFireProductionFP64::FireProductionProjectionRequest projection64;
	projection64.shape.nx=4u;projection64.shape.ny=4u;projection64.shape.nz=4u;
	projection64.shape.cellWidthM=0.5;projection64.timeStepS=0.01;
	projection64.ambientDensityKGPerM3=1.0;
	projection64.boundary.fill(RISEFireProductionFP64::FireProductionProjectionPeriodic);
	projection64.gasDensityKGPerM3.assign(64u,1.0);
	for(unsigned int axis=0u;axis<3u;++axis)
		projection64.provisionalMomentumKGPerM2S[axis].assign(FaceCount(4u,16u),0.0);
	projection64.divergenceTargetPerS.assign(64u,0.0);
	RISEFireProductionFP64::FireProductionProjectionResult projection64Result;
	Check(RISEFireProductionFP64::ProjectFireProductionCPU(projection64,projection64Result,&error)&&
		projection64Result.maximumPostProjectionResidualPerS==0.0,
		"generated binary64 projection preserves exact zero and one fixed solve");

	RISE::FireProductionResidentStepRequest step;
	step.force.shape=projection32.shape;step.force.timeStepS=projection32.timeStepS;
	step.force.ambientDensityKGPerM3=1.0f;step.force.vremanCoefficient=0.0f;
	step.force.boundary=projection32.boundary;
	step.force.cellGasDensityKGPerM3.assign(64u,1.0f);
	step.force.molecularKinematicViscosityM2PerS.assign(64u,0.0f);
	step.cellTransport.shape=projection32.shape;step.cellTransport.componentCount=9u;
	step.cellTransport.timeStepS=projection32.timeStepS;step.cellTransport.boundary=projection32.boundary;
	step.cellTransport.conservativeValues.assign(9u*64u,0.0f);
	step.cellTransport.ambientValues.assign(9u,0.0f);
	for(std::size_t cell=0u;cell<64u;++cell){
		step.cellTransport.conservativeValues[64u+cell]=1.0f;
		step.cellTransport.conservativeValues[8u*64u+cell]=300000.0f;
	}
	step.dualTransport.shape=projection32.shape;step.dualTransport.timeStepS=projection32.timeStepS;
	step.dualTransport.ambientDensityKGPerM3=1.0f;step.dualTransport.boundary=projection32.boundary;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(projection32.shape,axis);
		step.force.faceDensityKGPerM3[axis].assign(faces,1.0f);
		step.force.beginningMomentumKGPerM2S[axis].assign(faces,0.0f);
		step.cellTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		step.dualTransport.beginningFaceDensity[axis].assign(faces,1.0f);
		step.dualTransport.beginningMomentum[axis].assign(faces,0.0f);
		step.dualTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		step.momentumSourceIncrement[axis].assign(faces,0.0f);
	}
	step.cellSourceIncrement.assign(9u*64u,0.0f);
	step.divergenceTargetPerS.assign(64u,0.0f);
	step.restorationDivergenceTargetPerS.assign(64u,0.0f);
	step.monitorManifoldDiagnostics=true;
	step.enforceManifoldPlateau=true;
	FireProductionCalibration::ResidentStep64Result step64;
	Check(FireProductionCalibration::AdvanceResidentStep64(step,0.0,step64,&error)&&
		step64.force.schedule.substepCount==1u&&step64.cell.executedSubmapCount==5u&&
		step64.dual.executedSubmapCount==15u&&
		step64.projection.executedVCycleCount==12u&&
		step64.projection.maximumPostProjectionResidualPerS==0.0,
		"binary64 mirror composes force, five cell maps, fifteen dual maps, sources, and one P2");
	RISE::FireProductionResidentStepRequest nonDefaultCycleStep=step;
	nonDefaultCycleStep.physicalOpenProjectionVCycleCount=19u;
	nonDefaultCycleStep.force.shape.nx=5u;nonDefaultCycleStep.force.shape.ny=5u;
	nonDefaultCycleStep.force.shape.nz=5u;
	nonDefaultCycleStep.cellTransport.shape=nonDefaultCycleStep.force.shape;
	nonDefaultCycleStep.dualTransport.shape=nonDefaultCycleStep.force.shape;
	nonDefaultCycleStep.force.boundary[0]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.force.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.cellTransport.boundary[0]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.cellTransport.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.dualTransport.boundary[0]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.dualTransport.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	const std::size_t nonDefaultCells=nonDefaultCycleStep.force.shape.CellCount();
	nonDefaultCycleStep.force.cellGasDensityKGPerM3.assign(nonDefaultCells,1.0f);
	nonDefaultCycleStep.force.molecularKinematicViscosityM2PerS.assign(nonDefaultCells,0.0f);
	nonDefaultCycleStep.cellTransport.conservativeValues.assign(9u*nonDefaultCells,0.0f);
	for(std::size_t cell=0u;cell<nonDefaultCells;++cell){
		nonDefaultCycleStep.cellTransport.conservativeValues[nonDefaultCells+cell]=1.0f;
		nonDefaultCycleStep.cellTransport.conservativeValues[8u*nonDefaultCells+cell]=300000.0f;
	}
	nonDefaultCycleStep.cellSourceIncrement.assign(9u*nonDefaultCells,0.0f);
	nonDefaultCycleStep.divergenceTargetPerS.assign(nonDefaultCells,0.0f);
	nonDefaultCycleStep.restorationDivergenceTargetPerS.assign(nonDefaultCells,0.0f);
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(
			nonDefaultCycleStep.force.shape,axis);
		nonDefaultCycleStep.force.faceDensityKGPerM3[axis].assign(faces,1.0f);
		nonDefaultCycleStep.force.beginningMomentumKGPerM2S[axis].assign(faces,0.0f);
		nonDefaultCycleStep.cellTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		nonDefaultCycleStep.dualTransport.beginningFaceDensity[axis].assign(faces,1.0f);
		nonDefaultCycleStep.dualTransport.beginningMomentum[axis].assign(faces,0.0f);
		nonDefaultCycleStep.dualTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		nonDefaultCycleStep.momentumSourceIncrement[axis].assign(faces,0.0f);
	}
	FireProductionCalibration::ResidentStep64Result nonDefaultCycleStep64;
	const bool nonDefaultCycleMirrorOK=FireProductionCalibration::AdvanceResidentStep64(
		nonDefaultCycleStep,0.0,nonDefaultCycleStep64,&error);
	if(!nonDefaultCycleMirrorOK||nonDefaultCycleStep64.physicalProjection.executedVCycleCount!=19u||
		nonDefaultCycleStep64.projection.executedVCycleCount!=16u)
		std::fprintf(stderr,"non-default mirror detail ok=%d physical=%u restoration=%u error=%s\n",
			nonDefaultCycleMirrorOK?1:0,
			nonDefaultCycleStep64.physicalProjection.executedVCycleCount,
			nonDefaultCycleStep64.projection.executedVCycleCount,error.c_str());
	Check(nonDefaultCycleMirrorOK&&
		nonDefaultCycleStep64.physicalProjection.executedVCycleCount==19u&&
		nonDefaultCycleStep64.projection.executedVCycleCount==16u,
		"binary64 mirror propagates the request-owned non-default physical V-cycle count");
	RISE::FireProductionResidentStepRequest monitoredStep=nonDefaultCycleStep;
	monitoredStep.enforceManifoldPlateau=false;
	FireProductionCalibration::ResidentStep64Result monitoredStep64;
	const bool monitoredStepOK=FireProductionCalibration::AdvanceResidentStep64(
		monitoredStep,0.0,monitoredStep64,&error);
	if(!monitoredStepOK||monitoredStep64.physicalProjection.executedVCycleCount!=0u||
		monitoredStep64.projection.executedVCycleCount!=16u)
		std::fprintf(stderr,"monitored mirror detail ok=%d physical=%u terminal=%u error=%s\n",
			monitoredStepOK?1:0,
			monitoredStep64.physicalProjection.executedVCycleCount,
			monitoredStep64.projection.executedVCycleCount,error.c_str());
	Check(monitoredStepOK&&
		monitoredStep64.physicalProjection.executedVCycleCount==0u&&
		monitoredStep64.projection.executedVCycleCount==16u,
		"binary64 mirror executes the monitored standalone one-projection topology");
	FireProductionRoundoffAdapter::ResidentStepTraceResult tracedStep;
	const bool tracedStepOK=FireProductionRoundoffAdapter::AdvanceResidentStepTrace(
		step,0.0f,tracedStep,&error);
	bool tracedStepStages=tracedStep.stages.size()==24u;
	for(std::size_t stageIndex=0u;stageIndex<tracedStep.stages.size();++stageIndex){
		const FireProductionRoundoffTrace::Observation& stage=tracedStep.stages[stageIndex];
		std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)operations+=count;
		if(!(operations>0u&&stage.maximumDepth>0u&&stage.maximumOutputRadius>=0.0&&
			std::isfinite(stage.maximumOutputRadius)))std::fprintf(stderr,
			"roundoff stage detail index=%zu operations=%llu depth=%u radius=%.17g branch=%.17g\n",
			stageIndex,static_cast<unsigned long long>(operations),stage.maximumDepth,
			stage.maximumOutputRadius,stage.transportBranchDivergenceBound);
		tracedStepStages=tracedStepStages&&operations>0u&&stage.maximumDepth>0u&&
			stage.maximumOutputRadius>=0.0&&std::isfinite(stage.maximumOutputRadius);
	}
	const bool tracedStepBytes=tracedStep.conservativeValues.size()==
		step64.conservativeValues.size()&&std::all_of(tracedStep.conservativeValues.begin(),
		tracedStep.conservativeValues.end(),[](const FireProductionRoundoffTrace::TraceFloat& value){
			return value.Radius()==0.0&&std::isfinite(value.Rounded());});
	Check(tracedStepOK&&tracedStepStages&&tracedStepBytes&&
		tracedStep.force.schedule.substepCount==1u&&
		tracedStep.cell.executedSubmapCount==5u&&tracedStep.dual.executedSubmapCount==15u&&
		tracedStep.physicalProjection.maximumPostProjectionResidualPerS.Rounded()==0.0f&&
		tracedStep.projection.maximumPostProjectionResidualPerS.Rounded()==0.0f,
		"roundoff adapter seals the complete force-transport-source-two-projection graph before measurement");
	FireProductionRoundoffAdapter::ResidentStepTraceResult nonDefaultCycleTracedStep;
	const bool nonDefaultCycleTraceOK=FireProductionRoundoffAdapter::AdvanceResidentStepTrace(
		nonDefaultCycleStep,0.0f,nonDefaultCycleTracedStep,&error);
	if(!nonDefaultCycleTraceOK||nonDefaultCycleTracedStep.physicalProjection.executedVCycleCount!=19u||
		nonDefaultCycleTracedStep.projection.executedVCycleCount!=16u)
		std::fprintf(stderr,"non-default trace detail ok=%d physical=%u restoration=%u error=%s\n",
			nonDefaultCycleTraceOK?1:0,
			nonDefaultCycleTracedStep.physicalProjection.executedVCycleCount,
			nonDefaultCycleTracedStep.projection.executedVCycleCount,error.c_str());
	Check(nonDefaultCycleTraceOK&&
		nonDefaultCycleTracedStep.physicalProjection.executedVCycleCount==19u&&
		nonDefaultCycleTracedStep.projection.executedVCycleCount==16u,
		"roundoff adapter propagates the request-owned non-default physical V-cycle count");
	FireProductionRoundoffAdapter::ResidentStepTraceResult monitoredTrace;
	Check(FireProductionRoundoffAdapter::AdvanceResidentStepTrace(
		monitoredStep,0.0f,monitoredTrace,&error)&&
		monitoredTrace.projection.executedVCycleCount==19u&&
		monitoredTrace.conservativeValues.size()==
			nonDefaultCycleTracedStep.conservativeValues.size(),
		"roundoff trace executes the same monitored one-projection topology as production");
	if(!(tracedStepOK&&tracedStepStages&&tracedStepBytes))
		std::fprintf(stderr,"roundoff full-step detail ok=%d stages=%d bytes=%d count=%zu error=%s\n",
			tracedStepOK?1:0,tracedStepStages?1:0,tracedStepBytes?1:0,
			tracedStep.stages.size(),error.c_str());

	// r184 prerequisite: a completed Q* or Q^{n+1} is not accepted merely
	// because it passed r60.  Its signed inversion, EOS residual, case ceiling,
	// and exact state/attempt/role identity must all agree.
	const RISE::FireSimulationMethaneRecord& eosRecord=
		RISE::FireSimulationMethaneRecord::PhysicalV1();
	RISE::FireCase::AuthoredV1 eosAuthored;
	eosAuthored.fuelRecordId=eosRecord.RecordId();eosAuthored.poolDiameterM=0.03;
	eosAuthored.heatReleaseRateKW=0.10;
	eosAuthored.envelope={{0.0,0.0},{0.5,1.0},{1.0,1.0}};
	eosAuthored.durationS=1.0;eosAuthored.quality="draft";eosAuthored.seed=184u;
	eosAuthored.outputFramesPerS=4.0;
	RISE::FireCase::RecordV1 eosCase;
	const bool eosCaseOK=RISE::FireCase::BuildMethaneV1(eosAuthored,eosRecord,
		{eosRecord.RecordId()},eosCase,error);
	Check(eosCaseOK&&eosCase.derived.pilotAmbientTemperatureK==300.0&&
		eosCase.derived.maximumAcceptedTemperatureK==2300.0,
		"accepted-state EOS gate derives its bounds from an authenticated case record");
	auto eosTuple=[&](const double temperatureK){
		std::array<float,9> tuple={{}};
		const std::vector<double>& massFraction=eosRecord.AmbientMassFractions();
		double reciprocalWeight=0.0;
		for(std::size_t species=0u;species<7u;++species){
			const RISE::FireThermochemistrySpecies* property=eosRecord.FindSpecies(
				eosRecord.SpeciesOrder()[species].c_str());
			reciprocalWeight+=massFraction[species]/property->molecularWeightKGPerKMol;
		}
		const double density=eosRecord.ThermodynamicPressurePa()/
			(8314.46261815324*temperatureK*reciprocalWeight);
		for(std::size_t species=0u;species<7u;++species)
			tuple[1u+species]=static_cast<float>(density*massFraction[species]);
		std::array<double,7> enthalpy;
		eosRecord.SensibleEnthalpiesBySpeciesOrderJPerKG(temperatureK,
			enthalpy.data(),enthalpy.size(),&error);
		double energy=0.0;for(std::size_t species=0u;species<7u;++species)
			energy+=static_cast<double>(tuple[1u+species])*enthalpy[species];
		tuple[8u]=static_cast<float>(energy);return tuple;
	};
	auto eosRequestFromTuple=[&](const std::array<float,9>& tuple){
		RISE::FireProductionScalarEOSAcceptanceRequest request;
		request.shape.nx=4u;request.shape.ny=5u;request.shape.nz=4u;
		request.shape.cellWidthM=0.01f;request.timeStepS=0.0005f;
		request.attemptIdentity=UINT64_C(0x1840000000000001);
		request.stage=RISE::FireProductionScalarEOSStage::QStar;
		request.producerPrecision=RISE::FireStateProducerPrecision::Binary32;
		request.methaneRecordId=eosRecord.RecordId();
		request.caseRecordEnvelope=eosCase.envelopeBytes;
		const std::size_t cells=request.shape.CellCount();
		request.conservativeValues.resize(9u*cells);
		for(std::size_t component=0u;component<9u;++component)
			for(std::size_t cell=0u;cell<cells;++cell)
				request.conservativeValues[component*cells+cell]=tuple[component];
		return request;
	};
	const std::array<float,9> eosPhysicalTuple=eosTuple(900.0);
	RISE::FireProductionScalarEOSAcceptanceRequest eosRequest=
		eosRequestFromTuple(eosPhysicalTuple);
	std::vector<RISE::FireSim::ConservativeVector> eosOracleState(
		eosRequest.shape.CellCount());
	for(std::size_t cell=0u;cell<eosOracleState.size();++cell)
		for(std::size_t component=0u;component<9u;++component)
			eosOracleState[cell][component]=static_cast<double>(
				eosRequest.conservativeValues[component*eosOracleState.size()+cell]);
	std::vector<double> eosOracleTemperature;
	const bool eosOracleOK=RISE::FireSim::InvertPeriodicTemperaturesWithinBounds(
		eosOracleState,eosRecord,eosCase.derived.pilotAmbientTemperatureK,
		eosCase.derived.maximumAcceptedTemperatureK,
		RISE::FireStateProducerPrecision::Binary32,eosOracleTemperature,&error);
	RISE::FireProductionScalarEOSAcceptanceResult eosAccepted;
	const bool eosAcceptedOK=RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(
		eosRequest,eosAccepted,&error);
	RISE::FireSim::MethaneCellState eosOracleCell=RISE::FireSim::FromConservativeVector(
		eosOracleState[0],RISE::FireStateProducerPrecision::Binary32);
	if(!eosAccepted.temperatureK.empty())eosOracleCell.temperatureK=
		static_cast<double>(eosAccepted.temperatureK[0]);
	double eosOracleResidual=0.0;
	const bool eosOracleResidualOK=!eosAccepted.temperatureK.empty()&&
		RISE::FireSim::EquationOfStateResidual(eosOracleCell,eosRecord,
			RISE::FireStateProducerPrecision::Binary32,eosOracleResidual,&error);
	bool eosTemperatureExact=eosAccepted.temperatureK.size()==eosOracleTemperature.size();
	for(std::size_t cell=0u;cell<eosAccepted.temperatureK.size()&&eosTemperatureExact;++cell)
		eosTemperatureExact=sameFloatBits(eosAccepted.temperatureK[cell],
			static_cast<float>(eosOracleTemperature[cell]));
	Check(eosOracleOK&&eosAcceptedOK&&eosOracleResidualOK&&eosTemperatureExact&&
		eosAccepted.accepted&&eosAccepted.maximumEOSResidual==eosOracleResidual&&
		eosAccepted.maximumEOSResidual<=1.0e-3&&
		RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosAccepted),
		"accepted-state EOS gate agrees exactly with the Section 3.7 binary32 oracle");

	RISE::FireProductionScalarEOSAcceptanceRequest eosFinalRequest=eosRequest;
	eosFinalRequest.stage=RISE::FireProductionScalarEOSStage::QNPlus1;
	RISE::FireProductionScalarEOSAcceptanceResult eosFinalAccepted;
	Check(RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosFinalRequest,
		eosFinalAccepted,&error)&&
		RISE::FireProductionScalarEOSAcceptanceMatches(eosFinalRequest,eosFinalAccepted)&&
		eosFinalAccepted.acceptanceIdentity!=eosAccepted.acceptanceIdentity,
		"accepted-state EOS identity distinguishes QStar from QNPlus1");

	RISE::FireProductionScalarEOSAcceptanceRequest eosScaled=eosRequest;
	for(float& value:eosScaled.conservativeValues)value*=1.01f;
	RISE::FireProductionScalarEOSAcceptanceResult eosScaledAccepted;
	Check(RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosScaled,
		eosScaledAccepted,&error)&&eosScaledAccepted.accepted&&
		eosScaledAccepted.maximumEOSResidual>0.009&&
		eosScaledAccepted.maximumEOSResidual<0x1p-5&&
		RISE::FireProductionScalarEOSAcceptanceMatches(eosScaled,eosScaledAccepted),
		"production monitors a one-percent P0 deviation without resurrecting the oracle 1e-3 detector");

	RISE::FireProductionScalarEOSAcceptanceRequest eosCeiling=
		eosRequestFromTuple(eosTuple(2300.0));
	RISE::FireProductionScalarEOSAcceptanceResult eosRejected=eosAccepted;
	Check(!RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosCeiling,eosRejected,&error)&&
		error.find("case ceiling")!=std::string::npos&&!eosRejected.accepted&&
		eosRejected.temperatureK.empty(),
		"accepted-state EOS gate enforces the strict case temperature ceiling atomically");

	std::array<float,9> eosNegativeTuple=eosPhysicalTuple;
	double eosMassScale=0.0;for(std::size_t component=0u;component<8u;++component)
		eosMassScale+=std::fabs(static_cast<double>(eosNegativeTuple[component]));
	const double eosNegativeBound=eosRecord.AcceptedStateFeasibilityEnvelope().kappaEpsilon32*
		static_cast<double>(std::numeric_limits<float>::epsilon())*std::max(1.0,eosMassScale);
	eosNegativeTuple[7u]=static_cast<float>(-0.125*eosNegativeBound);
	std::array<double,7> eosNegativeEnthalpy;
	eosRecord.SensibleEnthalpiesBySpeciesOrderJPerKG(900.0,eosNegativeEnthalpy.data(),
		eosNegativeEnthalpy.size(),&error);
	double eosNegativeEnergy=0.0;for(std::size_t species=0u;species<7u;++species)
		eosNegativeEnergy+=static_cast<double>(eosNegativeTuple[1u+species])*
			eosNegativeEnthalpy[species];
	eosNegativeTuple[8u]=static_cast<float>(eosNegativeEnergy);
	RISE::FireProductionScalarEOSAcceptanceRequest eosNegative=
		eosRequestFromTuple(eosNegativeTuple);
	RISE::FireProductionScalarEOSAcceptanceResult eosNegativeAccepted;
	Check(RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosNegative,
		eosNegativeAccepted,&error)&&eosNegativeAccepted.accepted,
		"accepted-state EOS gate preserves signed inversion for an envelope-negative tuple");

	bool eosMutationsRejected=true;
	auto eosMutationRejected=[&](RISE::FireProductionScalarEOSAcceptanceRequest mutation){
		return !RISE::FireProductionScalarEOSAcceptanceMatches(mutation,eosAccepted);
	};
	RISE::FireProductionScalarEOSAcceptanceRequest eosMutation=eosRequest;
	eosMutation.conservativeValues[0]=std::nextafter(eosMutation.conservativeValues[0],1.0f);
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.timeStepS=std::nextafter(eosMutation.timeStepS,1.0f);
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;++eosMutation.attemptIdentity;
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.stage=RISE::FireProductionScalarEOSStage::QNPlus1;
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.caseRecordEnvelope.back()^=1u;
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.methaneRecordId.back()=
		eosMutation.methaneRecordId.back()=='a'?'b':'a';
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;std::swap(eosMutation.shape.nx,eosMutation.shape.ny);
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	RISE::FireProductionScalarEOSAcceptanceResult eosPublishedMutation=eosAccepted;
	eosPublishedMutation.temperatureK[0]=std::nextafter(eosPublishedMutation.temperatureK[0],
		1000.0f);
	eosMutationsRejected=eosMutationsRejected&&
		!RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosPublishedMutation);
	eosPublishedMutation=eosAccepted;
	eosPublishedMutation.maximumEOSResidual=std::nextafter(
		eosPublishedMutation.maximumEOSResidual,1.0);
	eosMutationsRejected=eosMutationsRejected&&
		!RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosPublishedMutation);
	eosPublishedMutation=eosAccepted;
	eosPublishedMutation.upperTemperatureK=2500.0;
	eosMutationsRejected=eosMutationsRejected&&
		!RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosPublishedMutation);
	Check(eosMutationsRejected,
		"accepted-state EOS identity rejects state, output, dt, attempt, role, record, bound, and shape mutations");

	RISE::FireProductionProjectionShape eosUnderCap,eosOverCap;
	eosUnderCap.nx=63u;eosUnderCap.ny=884u;eosUnderCap.nz=964u;
	eosUnderCap.cellWidthM=1.0f;eosOverCap.nx=116u;eosOverCap.ny=634u;
	eosOverCap.nz=730u;eosOverCap.cellWidthM=1.0f;
	std::uint64_t eosUnderBytes=0u,eosOverBytes=0u;
	const bool eosUnderQuery=RISE::QueryFireProductionScalarEOSAcceptanceCPUWorkingSetBytes(
		eosUnderCap,eosUnderBytes,&error);
	const bool eosOverQuery=RISE::QueryFireProductionScalarEOSAcceptanceCPUWorkingSetBytes(
		eosOverCap,eosOverBytes,&error);
	RISE::FireProductionScalarEOSAcceptanceRequest eosOversized=eosRequest;
	eosOversized.shape=eosOverCap;eosOversized.conservativeValues.clear();eosRejected=eosAccepted;
	Check(eosUnderQuery&&eosOverQuery&&eosUnderBytes==UINT64_C(2147483520)&&
		eosOverBytes==UINT64_C(2147484800)&&
		!RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosOversized,eosRejected,&error)&&
		error.find("working set exceeds two GiB")!=std::string::npos&&
		!eosRejected.accepted&&eosRejected.temperatureK.empty(),
		"accepted-state EOS exact working-set query enforces the two-GiB cap before payload access");

	// r186 prerequisite: only the canonical grid source producer can mint the
	// opaque production packet seal.  Its input surface contains controls and a
	// beginning state, never a caller-authored conservative dose or pilot pair.
	const RISE::FireSimulationTransportRecord& sourceTransport=
		RISE::FireSimulationTransportRecord::OpenV1();
	const RISE::FireSimulationGasOpacityRecord& sourceOpacity=
		RISE::FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	RISE::FireCase::RecordV1 sourceCase;
	const bool sourceCaseOK=RISE::FireCase::BuildMethaneV1(eosAuthored,eosRecord,
		{eosRecord.RecordId(),sourceTransport.RecordId(),sourceOpacity.RecordId()},
		sourceCase,error);
	RISE::FireProductionFrozenMethaneSourceRequest sourceRequest;
	sourceRequest.shape.nx=4u;sourceRequest.shape.ny=4u;sourceRequest.shape.nz=4u;
	sourceRequest.shape.cellWidthM=0.01f;sourceRequest.timeStepS=0.0005f;
	sourceRequest.beginningTimeS=0.0;
	sourceRequest.attemptIdentity=UINT64_C(0x1860000000000001);
	sourceRequest.caseRecordEnvelope=sourceCase.envelopeBytes;
	const std::size_t sourceCells=sourceRequest.shape.CellCount();
	const std::array<float,9> sourceBeginningTuple=eosTuple(300.0);
	sourceRequest.beginningConservativeValues.resize(9u*sourceCells);
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=0u;cell<sourceCells;++cell)
			sourceRequest.beginningConservativeValues[component*sourceCells+cell]=
				sourceBeginningTuple[component];
	sourceRequest.pilotCommandMask.assign(sourceCells,0u);
	sourceRequest.pilotCommandMask[0u]=1u;
	sourceRequest.mixingTimeS.assign(sourceCells,sourceTransport.ChemicalTimeS());
	sourceRequest.predictiveRadiation=false;sourceRequest.workerCount=2u;
	RISE::FireProductionFrozenSourcePacketSeal sourceSeal;
	const bool sourceSealOK=sourceCaseOK&&
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceRequest,sourceSeal,&error);
	if(!sourceSealOK)std::fprintf(stderr,"canonical source detail: %s\n",error.c_str());
	RISE::FireSim::ConservativeVector sourceBeginningVector;
	for(std::size_t index=0u;index<9u;++index)sourceBeginningVector[index]=
		static_cast<double>(sourceBeginningTuple[index]);
	RISE::FireSim::MethaneCellState sourceBeginning=
		RISE::FireSim::FromConservativeVector(sourceBeginningVector,
			RISE::FireStateProducerPrecision::Binary32);
	sourceBeginning.temperatureK=300.0;
	double sourcePilotCommand=0.0;
	const bool sourcePilotCommandOK=RISE::FireCase::EvaluatePilotSetpointTemperatureK(
		sourceCase.derived,true,sourceRequest.beginningTimeS,
		sourceRequest.beginningTimeS+sourceRequest.timeStepS,sourcePilotCommand,error);
	const double sourcePilotTarget=std::min(sourcePilotCommand,
		sourceBeginning.temperatureK*sourceCase.derived.pilotExpansionVolumeRatioCap);
	double sourcePilotBeginningEnergy=0.0,sourcePilotTargetEnergy=0.0;
	const bool sourcePilotEnergyOK=RISE::FireSim::SignedMixtureSensibleEnergy(sourceBeginning,
		sourceBeginning.temperatureK,eosRecord,sourcePilotBeginningEnergy,&error)&&
		RISE::FireSim::SignedMixtureSensibleEnergy(sourceBeginning,sourcePilotTarget,
			eosRecord,sourcePilotTargetEnergy,&error);
	const double sourcePilotVolume=sourcePilotTarget/sourceBeginning.temperatureK;
	const double sourcePilotExpected=(sourcePilotTargetEnergy-sourcePilotBeginningEnergy)/
		sourcePilotVolume;
	bool sourceDivergenceExact=sourceSealOK&&
		sourceSeal.DivergenceTargetPerS().size()==sourceCells;
	for(std::size_t cell=0u;cell<sourceCells&&sourceDivergenceExact;++cell){
		RISE::FireSim::MethaneSourcePacket packet;
		for(std::size_t species=0u;species<7u;++species)
			packet.constituentDelta[species]=static_cast<double>(
				sourceSeal.SourceDelta()[(1u+species)*sourceCells+cell]);
		packet.sensibleEnergyDeltaJPerM3=static_cast<double>(
			sourceSeal.SourceDelta()[8u*sourceCells+cell]);
		packet.pilotEnergyDeltaJPerM3=sourceSeal.PilotEnergyDeltaJPerM3()[cell];
		packet.pilotExpansionIntegral=sourceSeal.PilotExpansionIntegral()[cell];
		double scaled=0.0;
		sourceDivergenceExact=RISE::FireSim::FrozenSourcePacketExpansionAdmissible(
			sourceBeginningVector,sourceBeginning.temperatureK,packet,
			sourceRequest.timeStepS,eosRecord,RISE::FireStateProducerPrecision::Binary32,
			&scaled,&error)&&sameFloatBits(sourceSeal.DivergenceTargetPerS()[cell],
				static_cast<float>(scaled/static_cast<double>(sourceRequest.timeStepS)));
	}
	Check(sourceSealOK&&sourceSeal.IsSealed()&&
		RISE::FireProductionFrozenSourcePacketSealMatches(sourceSeal,&error)&&
		sourceSeal.SourceDelta().size()==9u*sourceCells&&
		sourceDivergenceExact&&
		sourceSeal.PilotEnergyDeltaJPerM3().size()==sourceCells&&
		sourceSeal.PilotExpansionIntegral().size()==sourceCells&&
		sourceSeal.PacketIdentity()!=0u&&sourceSeal.PacketContentIdentity()!=0u&&
		sourceSeal.GlobalRadiationIdentity()!=0u,
		"canonical grid producer mints one complete identity-bearing frozen source seal");
	Check(sourceSealOK&&sourcePilotCommandOK&&sourcePilotEnergyOK&&sourcePilotVolume>1.0&&
		sourceSeal.PilotEnergyDeltaJPerM3()[0u]==sourcePilotExpected&&
		sourceSeal.PilotExpansionIntegral()[0u]==1.0-1.0/sourcePilotVolume,
		"canonical source seal retains the exact pilot 1/V-prime energy and expansion pair");
	// The trace mirror must compare represented binary32 values, never the wider
	// TraceFloat object's center/radius/identity storage.  These two REDs cover
	// both historical escape directions: distinct trace lineage with identical
	// rounded bytes passes, while a one-bit rounded mutation refuses.
	std::vector<FireProductionRoundoffTrace::TraceFloat> representedFirst={
		FireProductionRoundoffTrace::TraceFloat::Raw(300.0,0.0,300.0f,1u),
		FireProductionRoundoffTrace::TraceFloat::Raw(900.0,0.0,900.0f,2u)};
	std::vector<FireProductionRoundoffTrace::TraceFloat> representedSame={
		FireProductionRoundoffTrace::TraceFloat::Raw(301.0,2.0,300.0f,7u),
		FireProductionRoundoffTrace::TraceFloat::Raw(899.0,3.0,900.0f,9u)};
	std::vector<FireProductionRoundoffTrace::TraceFloat> representedOneBit=representedSame;
	const float changed=std::nextafter(representedOneBit[0u].Rounded(),1000.0f);
	representedOneBit[0u]=FireProductionRoundoffTrace::TraceFloat::Raw(
		static_cast<double>(changed),0.0,changed,0u);
	const bool equalRoundedDifferentTraceAccepted=
		RISEFireProductionTrace::CalibrationSameRepresentedFloatVectorBits(
			representedFirst,representedSame);
	const bool oneBitRoundedMutationRefused=
		!RISEFireProductionTrace::CalibrationSameRepresentedFloatVectorBits(
			representedFirst,representedOneBit);
	const bool equalRoundedForceAccepted=
		RISEFireProductionTrace::CalibrationSameRepresentedForceFloatBits(
			representedFirst[0u],representedSame[0u]);
	const bool oneBitForceRefused=
		!RISEFireProductionTrace::CalibrationSameRepresentedForceFloatBits(
			representedFirst[0u],representedOneBit[0u]);
	const bool equalRoundedOwnerAccepted=
		RISEFireProductionTrace::CalibrationSameRepresentedOwnerFloatVectorBits(
			representedFirst,representedSame);
	const bool oneBitOwnerRefused=
		!RISEFireProductionTrace::CalibrationSameRepresentedOwnerFloatVectorBits(
			representedFirst,representedOneBit);
	Check(equalRoundedDifferentTraceAccepted&&oneBitRoundedMutationRefused&&
		equalRoundedForceAccepted&&oneBitForceRefused&&equalRoundedOwnerAccepted&&
		oneBitOwnerRefused,
		"roundoff transport, force, and owner mirrors compare represented binary32 values, not TraceFloat storage");
	RISE::FireProductionFrozenMethaneSourceRequest sourceAttemptMutation=sourceRequest;
	++sourceAttemptMutation.attemptIdentity;
	RISE::FireProductionFrozenSourcePacketSeal sourceAttemptSeal;
	const bool sourceAttemptOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		sourceAttemptMutation,sourceAttemptSeal,&error);
	Check(sourceSealOK&&sourceAttemptOK&&
		sourceSeal.SourceDelta()==sourceAttemptSeal.SourceDelta()&&
		sourceSeal.PacketContentIdentity()==sourceAttemptSeal.PacketContentIdentity()&&
		sourceSeal.PacketIdentity()!=sourceAttemptSeal.PacketIdentity()&&
		sourceSeal.SourceInputIdentity()!=sourceAttemptSeal.SourceInputIdentity(),
		"canonical source seal binds attempt identity without perturbing physical packet bytes");
	RISE::FireProductionFrozenMethaneSourceRequest sourceSerialRequest=sourceRequest;
	sourceSerialRequest.workerCount=1u;
	RISE::FireProductionFrozenSourcePacketSeal sourceSerialSeal;
	const bool sourceSerialOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		sourceSerialRequest,sourceSerialSeal,&error);
	Check(sourceSealOK&&sourceSerialOK&&
		sourceSeal.SourceDelta()==sourceSerialSeal.SourceDelta()&&
		sourceSeal.PacketIdentity()==sourceSerialSeal.PacketIdentity()&&
		sourceSeal.GlobalRadiationIdentity()==sourceSerialSeal.GlobalRadiationIdentity(),
		"canonical source packet is byte-identical across admitted worker schedules");
	RISE::FireProductionFrozenMethaneSourceRequest sourceControlMutation=sourceRequest;
	sourceControlMutation.mixingTimeS[1u]=std::nextafter(
		sourceControlMutation.mixingTimeS[1u],1.0);
	RISE::FireProductionFrozenSourcePacketSeal sourceControlSeal;
	const bool sourceControlOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		sourceControlMutation,sourceControlSeal,&error);
	Check(sourceControlOK&&sourceControlSeal.ReactionControlIdentity()!=
		sourceSeal.ReactionControlIdentity()&&sourceControlSeal.PacketIdentity()!=
		sourceSeal.PacketIdentity(),
		"canonical source seal binds every stage-derived mixing-control byte");
	RISE::FireProductionFrozenMethaneSourceRequest badSourceRequest=sourceRequest;
	badSourceRequest.pilotCommandMask[0u]=2u;
	RISE::FireProductionFrozenSourcePacketSeal rejectedSource=sourceSeal;
	const bool badMaskRejected=!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		badSourceRequest,rejectedSource,&error)&&!rejectedSource.IsSealed();
	badSourceRequest=sourceRequest;badSourceRequest.caseRecordEnvelope.back()^=1u;
	const bool badCaseRejected=!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		badSourceRequest,rejectedSource,&error)&&!rejectedSource.IsSealed();
	RISE::FireProductionFrozenSourcePacketSeal emptySourceSeal;
	Check(badMaskRejected&&badCaseRejected&&
		!RISE::FireProductionFrozenSourcePacketSealMatches(emptySourceSeal,&error)&&
		!std::is_aggregate<RISE::FireProductionFrozenSourcePacketSeal>::value,
		"raw controls, corrupt case bytes, and public empty objects cannot mint source authority");
	RISE::FireProductionFrozenMethaneSourceRequest sourceCeiling=sourceRequest;
	const std::array<float,9> sourceCeilingTuple=eosTuple(2300.0);
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=0u;cell<sourceCells;++cell)
			sourceCeiling.beginningConservativeValues[component*sourceCells+cell]=
				sourceCeilingTuple[component];
	Check(!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceCeiling,rejectedSource,&error)&&!rejectedSource.IsSealed()&&
		error.find("strict case ceiling")!=std::string::npos,
		"canonical source producer derives temperature from Q and refuses the exact case ceiling");
	RISE::FireProductionFrozenMethaneSourceRequest sourceBelowCase=sourceRequest;
	std::array<float,9> sourceBelowCaseTuple=eosTuple(300.0);
	// The thermochemistry domain begins at 300 K, so constructing a 299 K tuple
	// through the public enthalpy API would itself be invalid.  Exercise the
	// promised below-endpoint roundoff clamp with the nearest binary32 energy
	// strictly below the exact 300 K energy for the encoded constituent tuple.
	std::array<double,7> sourceLowerEnthalpy;
	const bool sourceLowerEnthalpyOK=eosRecord.SensibleEnthalpiesBySpeciesOrderJPerKG(
		300.0,sourceLowerEnthalpy.data(),sourceLowerEnthalpy.size(),&error);
	double sourceLowerEnergy=0.0;
	for(std::size_t species=0u;species<7u;++species)
		sourceLowerEnergy+=static_cast<double>(sourceBelowCaseTuple[1u+species])*
			sourceLowerEnthalpy[species];
	float sourceBelowEnergy=static_cast<float>(sourceLowerEnergy);
	while(static_cast<double>(sourceBelowEnergy)>=sourceLowerEnergy)
		sourceBelowEnergy=std::nextafter(sourceBelowEnergy,
			-std::numeric_limits<float>::infinity());
	sourceBelowCaseTuple[8u]=sourceBelowEnergy;
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=0u;cell<sourceCells;++cell)
			sourceBelowCase.beginningConservativeValues[component*sourceCells+cell]=
				sourceBelowCaseTuple[component];
	RISE::FireProductionFrozenSourcePacketSeal sourceBelowCaseSeal;
	RISE::FireProductionScalarEOSAcceptanceResult eosBelowCaseAccepted;
	const bool eosBelowCaseOK=RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(
		eosRequestFromTuple(sourceBelowCaseTuple),eosBelowCaseAccepted,&error);
	const bool sourceBelowCaseOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceBelowCase,sourceBelowCaseSeal,&error);
	Check(sourceLowerEnthalpyOK&&
		static_cast<double>(sourceBelowCaseTuple[8u])<sourceLowerEnergy&&
		eosBelowCaseOK&&eosBelowCaseAccepted.accepted&&
		eosBelowCaseAccepted.temperatureK[0u]==300.0f&&sourceBelowCaseOK&&
		sourceBelowCaseSeal.IsSealed(),
		"canonical source producer shares r184's inclusive lower-bound inversion semantics");
	std::uint64_t sourceWorkingSetBytes=0u,sourceAdjacentBytes=0u,sourceOversizedBytes=0u;
	RISE::FireProductionProjectionShape sourceAdjacentShape=sourceRequest.shape;
	++sourceAdjacentShape.nz;
	RISE::FireProductionProjectionShape sourceOversizedShape;
	sourceOversizedShape.nx=1024u;sourceOversizedShape.ny=1024u;
	sourceOversizedShape.nz=1024u;sourceOversizedShape.cellWidthM=0.01f;
	RISE::FireProductionFrozenMethaneSourceRequest sourceOversized=sourceRequest;
	sourceOversized.shape=sourceOversizedShape;
	sourceOversized.beginningConservativeValues.clear();
	sourceOversized.pilotCommandMask.clear();sourceOversized.mixingTimeS.clear();
	const bool sourceWorkingSetOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceRequest.shape,sourceRequest.workerCount,sourceWorkingSetBytes,&error);
	const bool sourceAdjacentQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceAdjacentShape,sourceRequest.workerCount,sourceAdjacentBytes,&error);
	const bool sourceOversizedQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceOversized.shape,sourceOversized.workerCount,sourceOversizedBytes,&error);
	const std::uint64_t sourceBytesPerCell=29u*sizeof(float)+12u*sizeof(double)+
		5u*sizeof(RISE::FireSim::MethaneCellState)+sizeof(RISE::FireSim::MethaneReactionStep)+
		3u*sizeof(RISE::FireSim::MethaneSourcePacket)+5u;
	const std::uint64_t sourceWorkerBytes=static_cast<std::uint64_t>(
		RISE::FireSim::FireWorkerCapacity())*(UINT64_C(8)<<20u);
	const std::uint64_t sourceExpected=UINT64_C(1048576)+sourceWorkerBytes+
		static_cast<std::uint64_t>(sourceCells)*sourceBytesPerCell;
	const std::uint64_t sourceAdjacentExpected=UINT64_C(1048576)+sourceWorkerBytes+
		static_cast<std::uint64_t>(sourceAdjacentShape.CellCount())*sourceBytesPerCell;
	RISE::FireProductionFrozenMethaneSourceRequest sourceHugeEnvelope=sourceRequest;
	sourceHugeEnvelope.caseRecordEnvelope.resize(UINT64_C(1048576)+1u,0u);
	RISE::FireProductionFrozenMethaneSourceRequest sourceTooManyWorkers=sourceRequest;
	sourceTooManyWorkers.workerCount=RISE::FireSim::FireWorkerCapacity()+1u;
	const bool sourceOversizedRejected=
		!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceOversized,rejectedSource,&error)&&!rejectedSource.IsSealed()&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	const bool sourceEnvelopeRejected=
		!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceHugeEnvelope,rejectedSource,&error)&&!rejectedSource.IsSealed()&&
		error.find("case envelope exceeds one MiB")!=std::string::npos;
	const bool sourceWorkerCountRejected=
		!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceTooManyWorkers,rejectedSource,&error)&&!rejectedSource.IsSealed();
	std::uint64_t sourceHighWorkerBytes=0u,sourceLowAfterHighBytes=0u;
	const bool sourceHighWorkerQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceRequest.shape,RISE::FireSim::FireWorkerCapacity(),sourceHighWorkerBytes,&error);
	const bool sourceLowAfterHighQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceRequest.shape,1u,sourceLowAfterHighBytes,&error);
	Check(sourceWorkingSetOK&&sourceAdjacentQuery&&sourceOversizedQuery&&
		sourceWorkingSetBytes==sourceExpected&&sourceAdjacentBytes==sourceAdjacentExpected&&
		sourceOversizedBytes>(UINT64_C(2)<<30u)&&
		sourceOversizedRejected&&sourceEnvelopeRejected&&sourceWorkerCountRejected&&
		sourceHighWorkerQuery&&sourceLowAfterHighQuery&&
		sourceHighWorkerBytes==sourceLowAfterHighBytes&&
		sourceLowAfterHighBytes==sourceWorkingSetBytes,
		"canonical source producer exactly accounts live storage and rejects oversized inputs early");

	// r188 prerequisite: the base projection target is rebuilt from one
	// authenticated source packet plus the same stage's retained f_N divergence.
	// It is opaque and deliberately has no projection-consumer overload.
	RISE::FireProductionFrozenMethaneSourceRequest baseTargetSourceRequest=sourceRequest;
	baseTargetSourceRequest.attemptIdentity=UINT64_C(0x1880000000000001);
	baseTargetSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	baseTargetSourceRequest.pilotCommandMask[0u]=1u;
	std::vector<float> baseTargetTemperature(sourceCells,0.0f);
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){
			const std::size_t cell=(z*4u+y)*4u+x;
			const std::array<float,9> tuple=eosTuple(300.0+25.0*static_cast<double>(x));
			std::array<double,9> represented={{}};
			for(std::size_t component=0u;component<9u;++component){
				baseTargetSourceRequest.beginningConservativeValues[component*sourceCells+cell]=
					tuple[component];
				represented[component]=static_cast<double>(tuple[component]);
			}
			double temperatureK=0.0,pressureRatio=0.0;
			if(eosRecord.InvertAcceptedConservativeStateByComponentOrder(represented.data(),
				represented.size(),eosRecord.TemperatureMinK(),eosRecord.TemperatureMaxK(),
				RISE::FireStateProducerPrecision::Binary32,temperatureK,pressureRatio,&error))
				baseTargetTemperature[cell]=static_cast<float>(temperatureK);
		}
	RISE::FireProductionFrozenSourcePacketSeal baseTargetSource;
	const bool baseTargetSourceOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			baseTargetSourceRequest,baseTargetSource,&error);
	if(baseTargetSourceOK)baseTargetTemperature=baseTargetSource.BeginningTemperatureK();
	RISE::FireProductionScalarFCTRequest baseTargetAdvective=constantScalarFCT;
	baseTargetAdvective.shape=baseTargetSourceRequest.shape;
	baseTargetAdvective.timeStepS=baseTargetSourceRequest.timeStepS;
	baseTargetAdvective.beginning=baseTargetSourceRequest.beginningConservativeValues;
	baseTargetAdvective.sourceDelta=baseTargetSource.SourceDelta();
	baseTargetAdvective.boundary.fill(RISE::FireProductionProjectionPeriodic);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest baseTargetPhysical=
		physicalFluxRequest;
	baseTargetPhysical.shape=baseTargetAdvective.shape;
	baseTargetPhysical.boundary=baseTargetAdvective.boundary;
	baseTargetPhysical.conservativeValues=baseTargetAdvective.beginning;
	baseTargetPhysical.temperatureK=baseTargetTemperature;
	baseTargetPhysical.diffusivityM2PerS.assign(sourceCells,0.01f);
	baseTargetPhysical.conductivityWPerMK.assign(sourceCells,0.03f);
	baseTargetPhysical.ambientTemperatureK=300.0f;
	const std::array<float,9> baseAmbientTuple=eosTuple(300.0);
	baseTargetAdvective.ambient=baseAmbientTuple;
	baseTargetPhysical.ambient=baseAmbientTuple;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(
			baseTargetAdvective.shape,axis);
		baseTargetAdvective.frozenVelocityMPerS[axis].assign(faces,0.0f);
		baseTargetPhysical.frozenVelocityMPerS[axis]=
			baseTargetAdvective.frozenVelocityMPerS[axis];
	}
	for(unsigned int side=0u;side<6u;++side){
		const std::size_t count=16u;
		baseTargetAdvective.pressureOpenInflow[side].assign(count,0u);
		baseTargetPhysical.pressureOpenInflow[side]=
			baseTargetAdvective.pressureOpenInflow[side];
	}
	RISE::FireProductionScalarDivergenceTargetSeal baseTargetR0;
	const bool baseTargetR0OK=baseTargetSourceOK&&
		RISE::ComposeFireProductionBaseDivergenceTargetCPU(
			baseTargetSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,
			baseTargetAdvective,baseTargetPhysical,baseTargetSource,baseTargetR0,&error);
	RISE::FireProductionScalarHeunFluxStage baseTargetStage;
	const bool baseTargetStageOK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		baseTargetSourceRequest.attemptIdentity,RISE::FireProductionScalarHeunFluxRole::R0,
		baseTargetAdvective,baseTargetPhysical,baseTargetStage,&error);
	auto targetMatchesIndependentTangent=[&](
		const RISE::FireProductionScalarFCTRequest& advective,
		const RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest& physical,
		const RISE::FireProductionScalarHeunFluxStage& stage,
		const RISE::FireProductionScalarDivergenceTargetSeal& target){
		bool exact=true;
		const std::size_t allFaces=stage.compositeFluxPair.packedFaceOffset[2]+
			RISE::FireProductionProjectionFaceCount(advective.shape,2u);
		const double inverseWidth=1.0/static_cast<double>(advective.shape.cellWidthM);
		for(std::size_t z=0u;z<4u&&exact;++z)
			for(std::size_t y=0u;y<4u&&exact;++y)
				for(std::size_t x=0u;x<4u&&exact;++x){
					const std::size_t cell=(z*4u+y)*4u+x;
					std::array<double,9> state={{}},physicalRate={{}};
					for(std::size_t component=0u;component<9u;++component)
						state[component]=advective.beginning[component*sourceCells+cell];
					for(unsigned int axis=0u;axis<3u;++axis){
						std::size_t rx=x,ry=y,rz=z;
						if(axis==0u)++rx;else if(axis==1u)++ry;else ++rz;
						const std::size_t left=stage.compositeFluxPair.packedFaceOffset[axis]+
							productionFaceIndex(advective.shape,axis,x,y,z);
						const std::size_t right=stage.compositeFluxPair.packedFaceOffset[axis]+
							productionFaceIndex(advective.shape,axis,rx,ry,rz);
						for(std::size_t component=0u;component<8u;++component)
							physicalRate[component]+=inverseWidth*(static_cast<double>(
								stage.physicalMassFluxKGPerM2S[component*allFaces+left])-
								static_cast<double>(stage.physicalMassFluxKGPerM2S[
									component*allFaces+right]));
						physicalRate[8u]+=inverseWidth*(static_cast<double>(
							stage.physicalEnergyFluxWPerM2[left])-static_cast<double>(
							stage.physicalEnergyFluxWPerM2[right]));
					}
					static const char* speciesNames[7]={
						"CH4","O2","N2","CO2","H2O","CO","C(gr)"};
					std::array<double,7> propertyDensity={{}},enthalpy={{}};
					double gasDensity=0.0,inverseMeanWeightSum=0.0,heatCapacity=0.0;
					for(std::size_t species=0u;species<7u;++species){
						propertyDensity[species]=std::max(0.0,state[1u+species]);
						double cp=0.0;
						exact=exact&&eosRecord.CpJPerKGK(speciesNames[species],
							physical.temperatureK[cell],cp,&error)&&
							eosRecord.SensibleEnthalpyJPerKG(speciesNames[species],
								physical.temperatureK[cell],enthalpy[species],&error);
						heatCapacity+=propertyDensity[species]*cp;
						if(species<6u){
							const RISE::FireThermochemistrySpecies* property=
								eosRecord.FindSpecies(speciesNames[species]);
							exact=exact&&property;
							gasDensity+=propertyDensity[species];
							inverseMeanWeightSum+=propertyDensity[species]/
								property->molecularWeightKGPerKMol;
						}
					}
					const double meanWeight=gasDensity/inverseMeanWeightSum;
					const double heatCapacityTemperature=heatCapacity*
						physical.temperatureK[cell];
					double physicalPerS=physicalRate[8u]/heatCapacityTemperature;
					for(std::size_t species=0u;species<6u;++species){
						const RISE::FireThermochemistrySpecies* property=
							eosRecord.FindSpecies(speciesNames[species]);
						physicalPerS+=(meanWeight/(gasDensity*
							property->molecularWeightKGPerKMol)-enthalpy[species]/
							heatCapacityTemperature)*physicalRate[1u+species];
					}
					physicalPerS-=enthalpy[6u]/heatCapacityTemperature*physicalRate[7u];
					exact=exact&&std::isfinite(physicalPerS);
					const float expected=static_cast<float>(physicalPerS+
						static_cast<double>(baseTargetSource.DivergenceTargetPerS()[cell]));
					exact=exact&&sameFloatBits(target.TargetPerS()[cell],expected);
				}
		return exact;
	};
	const bool baseTargetOracleExact=baseTargetR0OK&&baseTargetStageOK&&
		targetMatchesIndependentTangent(baseTargetAdvective,baseTargetPhysical,
			baseTargetStage,baseTargetR0);
	bool baseTargetIncludesPhysicalFlux=false;
	for(std::size_t cell=0u;cell<sourceCells;++cell)
		baseTargetIncludesPhysicalFlux=baseTargetIncludesPhysicalFlux||!sameFloatBits(
			baseTargetR0.TargetPerS()[cell],baseTargetSource.DivergenceTargetPerS()[cell]);
	Check(baseTargetR0OK&&baseTargetStageOK&&baseTargetOracleExact&&
		baseTargetIncludesPhysicalFlux&&baseTargetR0.IsSealed()&&
		RISE::FireProductionScalarDivergenceTargetSealMatches(baseTargetR0,&error)&&
		baseTargetR0.SourcePacketIdentity()==baseTargetSource.PacketIdentity()&&
		baseTargetR0.FluxCompositionIdentity()==baseTargetStage.compositionIdentity,
		"r188 base target adds the canonical f_N tangent to the frozen absolute source target");
	RISE::FireProductionScalarFCTRequest baseTargetR1Advective=baseTargetAdvective;
	for(float& value:baseTargetR1Advective.beginning)value*=1.001f;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest baseTargetR1Physical=
		baseTargetPhysical;
	baseTargetR1Physical.conservativeValues=baseTargetR1Advective.beginning;
	for(std::size_t cell=0u;cell<sourceCells;++cell){
		std::array<double,9> represented={{}};
		for(std::size_t component=0u;component<9u;++component)
			represented[component]=baseTargetR1Advective.beginning[component*sourceCells+cell];
		double temperatureK=0.0,pressureRatio=0.0;
		if(eosRecord.InvertAcceptedConservativeStateByComponentOrder(represented.data(),
			represented.size(),eosRecord.TemperatureMinK(),eosRecord.TemperatureMaxK(),
			RISE::FireStateProducerPrecision::Binary32,temperatureK,pressureRatio,&error))
			baseTargetR1Physical.temperatureK[cell]=static_cast<float>(temperatureK);
	}
	RISE::FireProductionScalarDivergenceTargetSeal baseTargetR1;
	const bool baseTargetR1OK=RISE::ComposeFireProductionBaseDivergenceTargetCPU(
		baseTargetSourceRequest.attemptIdentity,
		RISE::FireProductionScalarDivergenceTargetRole::R1Base,
		baseTargetR1Advective,baseTargetR1Physical,baseTargetSource,baseTargetR1,&error);
	RISE::FireProductionScalarHeunFluxStage baseTargetR1Stage;
	const bool baseTargetR1StageOK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		baseTargetSourceRequest.attemptIdentity,RISE::FireProductionScalarHeunFluxRole::R1,
		baseTargetR1Advective,baseTargetR1Physical,baseTargetR1Stage,&error);
	const bool baseTargetR1OracleExact=baseTargetR1OK&&baseTargetR1StageOK&&
		targetMatchesIndependentTangent(baseTargetR1Advective,baseTargetR1Physical,
			baseTargetR1Stage,baseTargetR1);
	Check(baseTargetR1OracleExact&&baseTargetR1.TargetPerS()!=baseTargetR0.TargetPerS()&&
		baseTargetR1.TargetIdentity()!=baseTargetR0.TargetIdentity()&&
		baseTargetR1.SourcePacketIdentity()==baseTargetR0.SourcePacketIdentity(),
		"r188 R1 changes only the physical tangent while retaining the frozen source authority");
	RISE::FireProductionFrozenMethaneSourceRequest zeroSourceRequest=
		baseTargetSourceRequest;
	zeroSourceRequest.attemptIdentity=UINT64_C(0x1880000000000002);
	zeroSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	for(float& value:zeroSourceRequest.beginningConservativeValues)value*=1.01f;
	RISE::FireProductionFrozenSourcePacketSeal zeroSource;
	const bool zeroSourceOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		zeroSourceRequest,zeroSource,&error);
	bool zeroSourceDeltaCanonical=zeroSourceOK;
	for(const float value:zeroSource.SourceDelta())
		zeroSourceDeltaCanonical=zeroSourceDeltaCanonical&&value==0.0f&&!std::signbit(value);
	RISE::FireProductionScalarFCTRequest zeroAdvective=baseTargetAdvective;
	zeroAdvective.timeStepS=zeroSourceRequest.timeStepS;
	zeroAdvective.beginning=zeroSourceRequest.beginningConservativeValues;
	zeroAdvective.sourceDelta=zeroSource.SourceDelta();
	for(auto& velocity:zeroAdvective.frozenVelocityMPerS)
		std::fill(velocity.begin(),velocity.end(),0.0f);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest zeroPhysical=
		baseTargetPhysical;
	zeroPhysical.conservativeValues=zeroAdvective.beginning;
	zeroPhysical.temperatureK=zeroSource.BeginningTemperatureK();
	zeroPhysical.diffusivityM2PerS.assign(sourceCells,0.0f);
	zeroPhysical.conductivityWPerMK.assign(sourceCells,0.0f);
	for(auto& velocity:zeroPhysical.frozenVelocityMPerS)
		std::fill(velocity.begin(),velocity.end(),0.0f);
	RISE::FireProductionScalarDivergenceTargetSeal zeroTarget;
	const bool zeroTargetOK=zeroSourceOK&&
		RISE::ComposeFireProductionBaseDivergenceTargetCPU(zeroSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,zeroAdvective,zeroPhysical,
			zeroSource,zeroTarget,&error);
	bool zeroSourceAbsoluteBranch=zeroTargetOK&&zeroSourceDeltaCanonical;
	bool zeroSourceNonzero=false;
	for(std::size_t cell=0u;cell<sourceCells&&zeroSourceAbsoluteBranch;++cell){
		zeroSourceAbsoluteBranch=sameFloatBits(zeroTarget.TargetPerS()[cell],
			zeroSource.DivergenceTargetPerS()[cell]);
		zeroSourceNonzero=zeroSourceNonzero||zeroTarget.TargetPerS()[cell]!=0.0f;
	}
	Check(zeroSourceAbsoluteBranch&&zeroSourceNonzero,
		"r188 zero source still carries the beginning-referenced absolute restoration branch");
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest staleBaseTemperature=
		baseTargetPhysical;
	staleBaseTemperature.temperatureK[0u]=std::nextafter(
		staleBaseTemperature.temperatureK[0u],1000.0f);
	RISE::FireProductionScalarDivergenceTargetSeal refusedBaseTarget;
	const bool staleBaseTemperatureRejected=
		!RISE::ComposeFireProductionBaseDivergenceTargetCPU(
			baseTargetSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,
			baseTargetAdvective,staleBaseTemperature,baseTargetSource,refusedBaseTarget,&error)&&
		!refusedBaseTarget.IsSealed();
	RISE::FireProductionScalarFCTRequest staleBaseSource=baseTargetAdvective;
	staleBaseSource.sourceDelta[0u]=std::nextafter(staleBaseSource.sourceDelta[0u],1.0f);
	const bool staleBaseSourceRejected=
		!RISE::ComposeFireProductionBaseDivergenceTargetCPU(
			baseTargetSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,
			staleBaseSource,baseTargetPhysical,baseTargetSource,refusedBaseTarget,&error)&&
		!refusedBaseTarget.IsSealed();
	RISE::FireProductionScalarFCTRequest overBaseTarget;
	overBaseTarget.shape=heunPayloadUnder;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest overBasePhysical;
	overBasePhysical.shape=overBaseTarget.shape;
	const bool overBaseRejected=!RISE::ComposeFireProductionBaseDivergenceTargetCPU(
		baseTargetSourceRequest.attemptIdentity,
		RISE::FireProductionScalarDivergenceTargetRole::R0Base,
		overBaseTarget,overBasePhysical,baseTargetSource,refusedBaseTarget,&error)&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	std::uint64_t baseTargetPayloadBytes=0u;
	const FireProductionRoundoffTrace::TraceFloat traceTemperature(300.0f);
	const FireProductionRoundoffTrace::TraceFloat traceStaleTemperature(
		std::nextafter(300.0f,1000.0f));
	Check(staleBaseTemperatureRejected&&staleBaseSourceRejected&&overBaseRejected&&
		traceTemperature!=traceStaleTemperature&&
		RISE::QueryFireProductionBaseDivergenceTargetCPUPayloadBytes(
			baseTargetAdvective.shape,baseTargetPayloadBytes,&error)&&
		baseTargetPayloadBytes==sourceCells*sizeof(float)&&
		!std::is_aggregate<RISE::FireProductionScalarDivergenceTargetSeal>::value,
		"r188 refuses stale temperature/source parents and preflights the combined live set");

	// r189 projection-consumer prerequisite: the base target may enter only as
	// iteration zero of an authenticated chain. The r70 child requires the later
	// complete owner because only that owner can bind Q* to this exact projection.
	RISE::FireProductionScalarProjectionTargetSeal initialProjectionTarget;
	const bool initialProjectionTargetOK=
		RISE::ComposeFireProductionInitialProjectionTargetCPU(
			baseTargetR0,initialProjectionTarget,&error);
	double initialProjectionTargetMean=0.0;
	for(const float value:initialProjectionTarget.TargetPerS())
		initialProjectionTargetMean+=static_cast<double>(value);
	if(!initialProjectionTarget.TargetPerS().empty())initialProjectionTargetMean/=
		static_cast<double>(initialProjectionTarget.TargetPerS().size());
	Check(initialProjectionTargetOK&&
		std::fabs(initialProjectionTargetMean)<=std::numeric_limits<float>::epsilon()&&
		initialProjectionTarget.Boundary()==baseTargetR0.Boundary()&&
		initialProjectionTarget.BaseTargetIdentity()==baseTargetR0.TargetIdentity()&&
		RISE::FireProductionScalarProjectionTargetSealMatches(
			initialProjectionTarget,&error),
		"r189 initial projection target inherits r188 lineage and closes the periodic constant mode");

	RISE::FireProductionProjectionRequest authenticatedProjection;
	authenticatedProjection.shape=baseTargetAdvective.shape;
	authenticatedProjection.timeStepS=baseTargetAdvective.timeStepS;
	authenticatedProjection.ambientDensityKGPerM3=1.2f;
	authenticatedProjection.boundary=baseTargetAdvective.boundary;
	authenticatedProjection.gasDensityKGPerM3.assign(sourceCells,1.2f);
	authenticatedProjection.residentPhysicalOpenVCycleCount=1u;
	for(unsigned int axis=0u;axis<3u;++axis)
		authenticatedProjection.provisionalMomentumKGPerM2S[axis].assign(
			RISE::FireProductionProjectionFaceCount(authenticatedProjection.shape,axis),0.0f);
	RISE::FireProductionProjectionResult authenticatedProjectionResult,directProjectionResult;
	const bool authenticatedProjectionOK=RISE::ProjectFireProductionScalarTargetCPU(
		authenticatedProjection,initialProjectionTarget,authenticatedProjectionResult,&error);
	RISE::FireProductionProjectionRequest directProjection=authenticatedProjection;
	directProjection.divergenceTargetPerS=initialProjectionTarget.TargetPerS();
	const bool directProjectionOK=RISE::ProjectFireProductionCPU(
		directProjection,directProjectionResult,&error);
	bool projectionBitExact=authenticatedProjectionOK&&directProjectionOK&&
		authenticatedProjectionResult.pressurePa==directProjectionResult.pressurePa&&
		sameFloatBits(authenticatedProjectionResult.maximumPreProjectionResidualPerS,
			directProjectionResult.maximumPreProjectionResidualPerS)&&
		sameFloatBits(authenticatedProjectionResult.maximumPostProjectionResidualPerS,
			directProjectionResult.maximumPostProjectionResidualPerS);
	for(unsigned int axis=0u;axis<3u;++axis)projectionBitExact=projectionBitExact&&
		authenticatedProjectionResult.velocityMPerS[axis]==
			directProjectionResult.velocityMPerS[axis]&&
		authenticatedProjectionResult.momentumKGPerM2S[axis]==
			directProjectionResult.momentumKGPerM2S[axis];
	Check(projectionBitExact,
		"r189 authenticated projection consumer is bit-identical to the raw CPU oracle on matched inputs");

	RISE::FireProductionProjectionRequest staleAuthoredTarget=authenticatedProjection;
	staleAuthoredTarget.divergenceTargetPerS.assign(sourceCells,0.0f);
	RISE::FireProductionProjectionResult refusedAuthenticatedProjection;
	const bool staleAuthoredTargetRejected=
		!RISE::ProjectFireProductionScalarTargetCPU(staleAuthoredTarget,
			initialProjectionTarget,refusedAuthenticatedProjection,&error)&&
		refusedAuthenticatedProjection.pressurePa.empty();
	RISE::FireProductionScalarProjectionTargetSeal refusedProjectionTarget;
	const bool unsealedParentRejected=
		!RISE::ComposeFireProductionInitialProjectionTargetCPU(
			RISE::FireProductionScalarDivergenceTargetSeal(),refusedProjectionTarget,&error)&&
		!refusedProjectionTarget.IsSealed();
	RISE::FireProductionProjectionRequest wrongTopologyProjection=authenticatedProjection;
	wrongTopologyProjection.boundary.fill(RISE::FireProductionProjectionWall);
	const bool validDifferentTopologyRejected=
		!RISE::ProjectFireProductionScalarTargetCPU(wrongTopologyProjection,
			initialProjectionTarget,refusedAuthenticatedProjection,&error)&&
		refusedAuthenticatedProjection.pressurePa.empty();
	Check(staleAuthoredTargetRejected&&unsealedParentRejected&&
		validDifferentTopologyRejected&&
		!std::is_aggregate<RISE::FireProductionScalarProjectionTargetSeal>::value,
		"r189 capability refuses preauthored targets, valid-but-different topology, and unsealed parents");

	// r190 complete CPU owner: every mutable stage operand is produced after the
	// immediately preceding projection.  The no-source uniform witness makes the
	// expected result exact while still executing R0/R1 Picard, one fresh Heun
	// limiter, both compatible momentum evaluations, EOS gates, and R2.
	RISE::FireProductionFrozenMethaneSourceRequest ownerSourceRequest=sourceRequest;
	ownerSourceRequest.attemptIdentity=UINT64_C(0x1900000000000001);
	ownerSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	RISE::FireProductionFrozenSourcePacketSeal ownerSource;
	const bool ownerSourceOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		ownerSourceRequest,ownerSource,&error);
	RISE::FireProductionProjectedHeunOwnerRequest ownerRequest;
	ownerRequest.attemptIdentity=ownerSourceRequest.attemptIdentity;
	ownerRequest.source=ownerSource;ownerRequest.caseRecordEnvelope=sourceCase.envelopeBytes;
	ownerRequest.beginningConservativeValues=ownerSourceRequest.beginningConservativeValues;
	ownerRequest.scalarContract=baseTargetAdvective;
	ownerRequest.scalarContract.shape=ownerSourceRequest.shape;
	ownerRequest.scalarContract.timeStepS=ownerSourceRequest.timeStepS;
	ownerRequest.scalarContract.beginning.clear();ownerRequest.scalarContract.sourceDelta.clear();
	for(auto& axis:ownerRequest.scalarContract.frozenVelocityMPerS)axis.clear();
	for(auto& side:ownerRequest.scalarContract.pressureOpenInflow)side.clear();
	ownerRequest.physicalContract=baseTargetPhysical;
	ownerRequest.physicalContract.shape=ownerSourceRequest.shape;
	ownerRequest.physicalContract.conservativeValues.clear();
	ownerRequest.physicalContract.temperatureK.clear();
	ownerRequest.physicalContract.diffusivityM2PerS.clear();
	ownerRequest.physicalContract.conductivityWPerMK.clear();
	for(auto& axis:ownerRequest.physicalContract.frozenVelocityMPerS)axis.clear();
	for(auto& side:ownerRequest.physicalContract.pressureOpenInflow)side.clear();
	ownerRequest.forceContract.shape=ownerSourceRequest.shape;
	ownerRequest.forceContract.timeStepS=ownerSourceRequest.timeStepS;
	ownerRequest.forceContract.ambientDensityKGPerM3=1.2f;
	ownerRequest.forceContract.vremanCoefficient=0.07f;
	ownerRequest.forceContract.boundary=ownerRequest.scalarContract.boundary;
	ownerRequest.forceContract.gravityMPerS2.fill(0.0f);
	ownerRequest.projectionTolerancePerS=0.0f;
	ownerRequest.endpointVelocityToleranceMPerS=0.0f;
	ownerRequest.maximumPicardIterations=4u;
	for(unsigned int axis=0u;axis<3u;++axis)
		ownerRequest.beginningMomentumKGPerM2S[axis].assign(
			RISE::FireProductionProjectionFaceCount(ownerSourceRequest.shape,axis),0.0f);
	ConstantProjectedHeunTransport32 ownerTransport;
	RISE::FireProductionProjectedHeunCPUOwner owner;
	RISE::FireProductionProjectedHeunOwnerResult ownerResult;
	const bool ownerBegin=ownerSourceOK&&owner.Begin(ownerRequest,&error);
	const bool ownerR0=ownerBegin&&owner.SolveR0(ownerTransport,&error);
	if(!ownerR0)std::fprintf(stderr,"projected-Heun R0 detail: %s\n",error.c_str());
	const bool ownerR1=ownerR0&&owner.SolveR1(ownerTransport,&error);
	if(!ownerR1)std::fprintf(stderr,"projected-Heun R1 detail: %s\n",error.c_str());
	const bool ownerR2=ownerR1&&owner.SolveR2(ownerTransport,ownerResult,&error);
	if(!ownerR2)std::fprintf(stderr,"projected-Heun R2 detail: %s\n",error.c_str());
	bool ownerExact=ownerR2&&ownerResult.accepted&&ownerResult.OwnerIdentity()!=0u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(ownerResult,ownerSource)&&
		ownerResult.conservativeValues==ownerRequest.beginningConservativeValues&&
		ownerResult.r0.target.CorrectionIteration()>=2u&&
		ownerResult.r1.target.CorrectionIteration()>=2u&&
		ownerResult.r2.target.CorrectionIteration()==0u&&
		ownerResult.r2.acceptedIterationCount>=2u&&
		!ownerResult.r2.endpointPhysicalFlux.physicalEnergyFluxWPerM2.empty()&&
		ownerResult.r0.target.AcceptedCandidateIdentity()==
			ownerResult.r0.acceptedCandidateIdentity&&
		ownerResult.r1.target.AcceptedCandidateIdentity()==
			ownerResult.r1.acceptedCandidateIdentity&&
		ownerResult.r0.target.ParentTargetIdentity()==
			ownerResult.r0.projectionTarget.TargetIdentity()&&
		ownerResult.r1.target.ParentTargetIdentity()==
			ownerResult.r1.projectionTarget.TargetIdentity()&&
		ownerResult.r0.projectionTarget.TargetIdentity()!=ownerResult.r0.target.TargetIdentity()&&
		ownerResult.r1.projectionTarget.TargetIdentity()!=ownerResult.r1.target.TargetIdentity()&&
		ownerResult.r2.projectionTarget.TargetPerS()==ownerResult.r2.target.TargetPerS();
	for(unsigned int axis=0u;axis<3u&&ownerExact;++axis)for(const float value:
		ownerResult.velocityMPerS[axis])ownerExact=ownerExact&&value==0.0f;
	RISE::FireProductionProjectedHeunOwnerResult forgedOwnerPublication=ownerResult;
	if(!forgedOwnerPublication.conservativeValues.empty())
		forgedOwnerPublication.conservativeValues[0u]=std::nextafter(
			forgedOwnerPublication.conservativeValues[0u],
			std::numeric_limits<float>::infinity());
	const bool publicResigningRejected=
		!HasPublicOwnerIdentity<RISE::FireProductionProjectedHeunOwnerResult>::value&&
		forgedOwnerPublication.OwnerIdentity()==ownerResult.OwnerIdentity()&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			forgedOwnerPublication,ownerSource);
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest ownerRequest64;
	ownerRequest64.attemptIdentity=ownerRequest.attemptIdentity;
	ownerRequest64.source=
		RISEFireProductionFP64::FireProductionFrozenSourcePacketSeal::CalibrationImport(
			ownerSource);
	ownerRequest64.caseRecordEnvelope=ownerRequest.caseRecordEnvelope;
	ownerRequest64.beginningConservativeValues.assign(
		ownerRequest.beginningConservativeValues.begin(),
		ownerRequest.beginningConservativeValues.end());
	auto copyShape64=[](const RISE::FireProductionProjectionShape& from,
		RISEFireProductionFP64::FireProductionProjectionShape& to){
		to.nx=from.nx;to.ny=from.ny;to.nz=from.nz;to.cellWidthM=from.cellWidthM;};
	copyShape64(ownerRequest.scalarContract.shape,ownerRequest64.scalarContract.shape);
	ownerRequest64.scalarContract.timeStepS=ownerRequest.scalarContract.timeStepS;
	for(unsigned int side=0u;side<6u;++side){
		ownerRequest64.scalarContract.boundary[side]=static_cast<
			RISEFireProductionFP64::FireProductionProjectionBoundary>(
				ownerRequest.scalarContract.boundary[side]);
		ownerRequest64.physicalContract.boundary[side]=
			ownerRequest64.scalarContract.boundary[side];
		ownerRequest64.forceContract.boundary[side]=
			ownerRequest64.scalarContract.boundary[side];
	}
	for(std::size_t component=0u;component<9u;++component){
		ownerRequest64.scalarContract.ambient[component]=
			ownerRequest.scalarContract.ambient[component];
		ownerRequest64.physicalContract.ambient[component]=
			ownerRequest.physicalContract.ambient[component];
	}
	ownerRequest64.scalarContract.nullity=ownerRequest.scalarContract.nullity;
	ownerRequest64.scalarContract.nullspaceBasis.assign(
		ownerRequest.scalarContract.nullspaceBasis.begin(),
		ownerRequest.scalarContract.nullspaceBasis.end());
	ownerRequest64.scalarContract.coordinateProjector.assign(
		ownerRequest.scalarContract.coordinateProjector.begin(),
		ownerRequest.scalarContract.coordinateProjector.end());
	for(std::size_t value=0u;value<14u;++value)
		ownerRequest64.scalarContract.enthalpyBoundsJPerKG[value]=
			ownerRequest.scalarContract.enthalpyBoundsJPerKG[value];
	ownerRequest64.scalarContract.feasibilityFactor=
		ownerRequest.scalarContract.feasibilityFactor;
	ownerRequest64.scalarContract.assemblyReserveFactor=
		ownerRequest.scalarContract.assemblyReserveFactor;
	copyShape64(ownerRequest.physicalContract.shape,ownerRequest64.physicalContract.shape);
	ownerRequest64.physicalContract.ambientTemperatureK=
		ownerRequest.physicalContract.ambientTemperatureK;
	copyShape64(ownerRequest.forceContract.shape,ownerRequest64.forceContract.shape);
	ownerRequest64.forceContract.timeStepS=ownerRequest.forceContract.timeStepS;
	ownerRequest64.forceContract.ambientDensityKGPerM3=
		ownerRequest.forceContract.ambientDensityKGPerM3;
	ownerRequest64.forceContract.vremanCoefficient=ownerRequest.forceContract.vremanCoefficient;
	for(unsigned int axis=0u;axis<3u;++axis){
		ownerRequest64.forceContract.gravityMPerS2[axis]=
			ownerRequest.forceContract.gravityMPerS2[axis];
		ownerRequest64.beginningMomentumKGPerM2S[axis].assign(
			ownerRequest.beginningMomentumKGPerM2S[axis].begin(),
			ownerRequest.beginningMomentumKGPerM2S[axis].end());
	}
	ownerRequest64.projectionTolerancePerS=ownerRequest.projectionTolerancePerS;
	ownerRequest64.endpointVelocityToleranceMPerS=
		ownerRequest.endpointVelocityToleranceMPerS;
	ownerRequest64.maximumPicardIterations=ownerRequest.maximumPicardIterations;
	ConstantProjectedHeunTransport64 ownerTransport64;
	RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner owner64;
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerResult ownerResult64;
	const bool owner64OK=owner64.Begin(ownerRequest64,&error)&&
		owner64.SolveR0(ownerTransport64,&error)&&owner64.SolveR1(ownerTransport64,&error)&&
		owner64.SolveR2(ownerTransport64,ownerResult64,&error);
	if(!owner64OK)std::fprintf(stderr,"projected-Heun fp64 mirror detail: %s\n",error.c_str());
	double ownerOracleMaximumDifference=0.0;
	auto compareOwnerVector=[&](const std::vector<float>& fp32,
		const std::vector<double>& fp64){
		if(fp32.size()!=fp64.size()){
			ownerOracleMaximumDifference=std::numeric_limits<double>::infinity();return;}
		for(std::size_t value=0u;value<fp32.size();++value)
			ownerOracleMaximumDifference=std::max(ownerOracleMaximumDifference,std::fabs(
				static_cast<double>(fp32[value])-fp64[value]));
	};
	if(ownerR2&&owner64OK){
		compareOwnerVector(ownerResult.conservativeValues,ownerResult64.conservativeValues);
		compareOwnerVector(ownerResult.stepAveragePressurePa,ownerResult64.stepAveragePressurePa);
		compareOwnerVector(ownerResult.r0.target.TargetPerS(),ownerResult64.r0.target.TargetPerS());
		compareOwnerVector(ownerResult.r1.target.TargetPerS(),ownerResult64.r1.target.TargetPerS());
		compareOwnerVector(ownerResult.r2.target.TargetPerS(),ownerResult64.r2.target.TargetPerS());
		compareOwnerVector(ownerResult.r0.projectionTarget.TargetPerS(),
			ownerResult64.r0.projectionTarget.TargetPerS());
		compareOwnerVector(ownerResult.r1.projectionTarget.TargetPerS(),
			ownerResult64.r1.projectionTarget.TargetPerS());
		compareOwnerVector(ownerResult.r2.projectionTarget.TargetPerS(),
			ownerResult64.r2.projectionTarget.TargetPerS());
		for(unsigned int axis=0u;axis<3u;++axis){
			compareOwnerVector(ownerResult.momentumKGPerM2S[axis],
				ownerResult64.momentumKGPerM2S[axis]);
			compareOwnerVector(ownerResult.velocityMPerS[axis],ownerResult64.velocityMPerS[axis]);
			compareOwnerVector(ownerResult.r0.scalarAcceptance.sharedFaceAlpha[axis],
				ownerResult64.r0.scalarAcceptance.sharedFaceAlpha[axis]);
			compareOwnerVector(ownerResult.r1.scalarAcceptance.sharedFaceAlpha[axis],
				ownerResult64.r1.scalarAcceptance.sharedFaceAlpha[axis]);
		}
	}
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerResult=ownerResult;
	if(!mutatedOwnerResult.stepAveragePressurePa.empty())mutatedOwnerResult.stepAveragePressurePa[0u]=
		std::nextafter(mutatedOwnerResult.stepAveragePressurePa[0u],1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerVelocity=ownerResult;
	if(!mutatedOwnerVelocity.r1.projection.velocityMPerS[0u].empty())
		mutatedOwnerVelocity.r1.projection.velocityMPerS[0u][0u]=std::nextafter(
			mutatedOwnerVelocity.r1.projection.velocityMPerS[0u][0u],1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerAlpha=ownerResult;
	if(!mutatedOwnerAlpha.r0.scalarAcceptance.sharedFaceAlpha[0u].empty())
		mutatedOwnerAlpha.r0.scalarAcceptance.sharedFaceAlpha[0u][0u]=std::nextafter(
			mutatedOwnerAlpha.r0.scalarAcceptance.sharedFaceAlpha[0u][0u],0.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerEOS=ownerResult;
	if(!mutatedOwnerEOS.committedEOS.temperatureK.empty())
		mutatedOwnerEOS.committedEOS.temperatureK[0u]=std::nextafter(
			mutatedOwnerEOS.committedEOS.temperatureK[0u],1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerFluxMetadata=ownerResult;
	mutatedOwnerFluxMetadata.averagedFlux.compositeFluxPair.timeStepS=std::nextafter(
		mutatedOwnerFluxMetadata.averagedFlux.compositeFluxPair.timeStepS,1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerPhysicalMetadata=ownerResult;
	++mutatedOwnerPhysicalMetadata.r2.endpointPhysicalFlux.shape.nx;
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerEOSMetadata=ownerResult;
	++mutatedOwnerEOSMetadata.committedEOS.shape.nz;
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerProjectionTarget=ownerResult;
	mutatedOwnerProjectionTarget.r0.projectionTarget=ownerResult.r0.target;
	RISE::FireProductionFrozenMethaneSourceRequest nextOwnerSourceRequest=ownerSourceRequest;
	nextOwnerSourceRequest.attemptIdentity=ownerSourceRequest.attemptIdentity+1u;
	RISE::FireProductionFrozenSourcePacketSeal nextOwnerSource;
	const bool nextOwnerSourceOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		nextOwnerSourceRequest,nextOwnerSource,&error);
	std::uint64_t ownerWorkingSetBytes=0u;
	RISE::FireProductionProjectionShape oversizedOwnerShape;
	oversizedOwnerShape.nx=1024u;oversizedOwnerShape.ny=1024u;
	oversizedOwnerShape.nz=1024u;oversizedOwnerShape.cellWidthM=0.01f;
	std::uint64_t oversizedOwnerWorkingSetBytes=0u;
	std::vector<float> r60SignedDensityState=ownerRequest.beginningConservativeValues;
	const float replacedConstituent=r60SignedDensityState[sourceCells];
	const float negativeRoundoff=-std::numeric_limits<float>::epsilon();
	r60SignedDensityState[sourceCells]=negativeRoundoff;
	r60SignedDensityState[2u*sourceCells]+=
		replacedConstituent-negativeRoundoff;
	std::vector<float> r60SignedDensity;
	const bool r60SignedDensityOK=RISE::ComputeFireProductionGasDensityCPU(
		ownerSourceRequest.shape,r60SignedDensityState,r60SignedDensity,&error);
	float r60SignedExpected=0.0f;
	for(std::size_t component=1u;component<=6u;++component)
		r60SignedExpected+=r60SignedDensityState[component*sourceCells];
	float r60ClampedCounterfactual=0.0f;
	for(std::size_t component=1u;component<=6u;++component)
		r60ClampedCounterfactual+=std::max(0.0f,
			r60SignedDensityState[component*sourceCells]);
	const std::uint64_t ownerFaceCount=
		static_cast<std::uint64_t>(ownerSourceRequest.shape.nx+1u)*
			ownerSourceRequest.shape.ny*ownerSourceRequest.shape.nz+
		static_cast<std::uint64_t>(ownerSourceRequest.shape.nx)*
			(ownerSourceRequest.shape.ny+1u)*ownerSourceRequest.shape.nz+
		static_cast<std::uint64_t>(ownerSourceRequest.shape.nx)*
			ownerSourceRequest.shape.ny*(ownerSourceRequest.shape.nz+1u);
	const std::uint64_t expectedOwnerWorkingSetBytes=(274u*sourceCells+
		97u*ownerFaceCount)*sizeof(float);
	Check(ownerExact&&publicResigningRejected&&owner64OK&&ownerOracleMaximumDifference<=
		32.0*std::numeric_limits<float>::epsilon()&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerResult,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerVelocity,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerAlpha,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerEOS,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerFluxMetadata,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerPhysicalMetadata,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerEOSMetadata,ownerSource)&&nextOwnerSourceOK&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerProjectionTarget,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(ownerResult,nextOwnerSource)&&
		RISE::FireProductionProjectedHeunCPUOwnerWorkingSetBytes(
			ownerSourceRequest.shape,ownerWorkingSetBytes)&&
		ownerWorkingSetBytes==expectedOwnerWorkingSetBytes&&
		RISE::FireProductionProjectedHeunCPUOwnerWorkingSetBytes(
			oversizedOwnerShape,oversizedOwnerWorkingSetBytes)&&
		oversizedOwnerWorkingSetBytes>(UINT64_C(2)<<30u)&&r60SignedDensityOK&&
		r60SignedDensity[0u]==r60SignedExpected&&
		r60SignedDensity[0u]!=r60ClampedCounterfactual,
		"r190 owner binds every R0/R1/R2 publication and rejects mutated accepted output");

	// Independent oracle differential on the complete 4^3 pressure-open owner.
	// The exact canonical source packet is reconstructed from the sealed
	// production publication; fire_simulator_core owns the fp64 schedule.
	RISE::FireProductionFrozenMethaneSourceRequest differentialSourceRequest=
		ownerSourceRequest;
	differentialSourceRequest.attemptIdentity=UINT64_C(0x1900000000000002);
	differentialSourceRequest.timeStepS=1.0e-4f;
	differentialSourceRequest.beginningTimeS=std::max(0.0,
		sourceCase.derived.flowThroughTimeS-differentialSourceRequest.timeStepS);
	differentialSourceRequest.pilotCommandMask.assign(sourceCells,1u);
	differentialSourceRequest.predictiveRadiation=false;
	const double differentialPilotTemperatureK=900.0;
	std::array<float,9> differentialPilotBeginning=eosTuple(differentialPilotTemperatureK);
	differentialPilotBeginning[8u]=std::nextafter(differentialPilotBeginning[8u],
		-std::numeric_limits<float>::infinity());
	for(std::size_t component=0u;component<9u;++component)for(std::size_t cell=0u;
		cell<sourceCells;++cell)differentialSourceRequest.beginningConservativeValues[
			component*sourceCells+cell]=differentialPilotBeginning[component];
	RISE::FireProductionFrozenSourcePacketSeal differentialSource;
	const bool differentialSourceOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			differentialSourceRequest,differentialSource,&error);
	RISE::FireProductionProjectedHeunOwnerRequest differentialRequest=ownerRequest;
	differentialRequest.attemptIdentity=differentialSourceRequest.attemptIdentity;
	differentialRequest.source=differentialSource;
	differentialRequest.beginningConservativeValues=
		differentialSourceRequest.beginningConservativeValues;
	differentialRequest.scalarContract.timeStepS=differentialSourceRequest.timeStepS;
	differentialRequest.forceContract.timeStepS=differentialSourceRequest.timeStepS;
	differentialRequest.scalarContract.ambient=differentialPilotBeginning;
	differentialRequest.physicalContract.ambient=differentialPilotBeginning;
	differentialRequest.physicalContract.ambientTemperatureK=
		static_cast<float>(differentialPilotTemperatureK);
	for(std::size_t species=0u;species<7u;++species){
		differentialRequest.scalarContract.enthalpyBoundsJPerKG[species]=-1.0e8f;
		differentialRequest.scalarContract.enthalpyBoundsJPerKG[7u+species]=1.0e8f;
	}
	differentialRequest.projectionTolerancePerS=5.0e-3f;
	differentialRequest.maximumPicardIterations=64u;
	differentialRequest.scalarContract.boundary.fill(
		RISE::FireProductionProjectionPressureOpen);
	differentialRequest.scalarContract.boundary[4u]=RISE::FireProductionProjectionWall;
	differentialRequest.physicalContract.boundary=differentialRequest.scalarContract.boundary;
	differentialRequest.forceContract.boundary=differentialRequest.scalarContract.boundary;
	for(unsigned int axis=0u;axis<3u;++axis)
		std::fill(differentialRequest.beginningMomentumKGPerM2S[axis].begin(),
			differentialRequest.beginningMomentumKGPerM2S[axis].end(),0.0f);
	RISE::FireSim::OpenBoundaryConfig3D differentialBoundary;
	differentialBoundary.kind.fill(RISE::FireSim::PressureOpenBoundary3D);
	differentialBoundary.kind[4u]=RISE::FireSim::AdiabaticWallBoundary3D;
	differentialBoundary.ambientDensityKGPerM3=0.0;
	for(std::size_t component=1u;component<=6u;++component)
		differentialBoundary.ambientDensityKGPerM3+=
			differentialPilotBeginning[component];
	differentialBoundary.injectedGasDensityKGPerM3=
		differentialBoundary.ambientDensityKGPerM3;
	for(std::size_t component=0u;component<9u;++component){
		differentialBoundary.ambientState[component]=differentialPilotBeginning[component];
		differentialBoundary.injectedState[component]=differentialPilotBeginning[component];
	}
	differentialBoundary.velocityToleranceMPerS=
		differentialRequest.endpointVelocityToleranceMPerS;
	differentialBoundary.pressureTolerancePa=1.0e-4;
	OracleOpenProjectedHeunTransport32 differentialTransport(
		ownerSourceRequest.shape,differentialBoundary);
	RISE::FireProductionProjectedHeunCPUOwner differentialOwner;
	RISE::FireProductionProjectedHeunOwnerResult differentialResult;
	const bool differentialProductionOK=differentialSourceOK&&
		differentialOwner.Begin(differentialRequest,&error)&&
		differentialOwner.SolveR0(differentialTransport,&error)&&
		differentialOwner.SolveR1(differentialTransport,&error)&&
		differentialOwner.SolveR2(differentialTransport,differentialResult,&error);
	if(!differentialProductionOK)std::fprintf(stderr,
		"projected-Heun independent production detail: %s\n",error.c_str());
	RISE::FireSim::PeriodicMACShape differentialShape;
	differentialShape.nx=ownerSourceRequest.shape.nx;
	differentialShape.ny=ownerSourceRequest.shape.ny;
	differentialShape.nz=ownerSourceRequest.shape.nz;
	differentialShape.cellWidthM=ownerSourceRequest.shape.cellWidthM;
	std::vector<RISE::FireSim::ConservativeVector> differentialBeginning(sourceCells);
	std::vector<RISE::FireSim::MethaneSourcePacket> differentialPackets(sourceCells);
	for(std::size_t cell=0u;cell<sourceCells;++cell){
		for(std::size_t component=0u;component<9u;++component){
			differentialBeginning[cell][component]=
				differentialRequest.beginningConservativeValues[component*sourceCells+cell];
		}
		for(std::size_t species=0u;species<7u;++species)
			differentialPackets[cell].constituentDelta[species]=
				differentialSource.SourceDelta()[(1u+species)*sourceCells+cell];
		differentialPackets[cell].sensibleEnergyDeltaJPerM3=
			differentialSource.SourceDelta()[8u*sourceCells+cell];
		differentialPackets[cell].reactedFuelKGPerM3=
			differentialSource.ReactedFuelKGPerM3()[cell];
		differentialPackets[cell].oxidizedCarbonKGPerM3=
			differentialSource.OxidizedCarbonKGPerM3()[cell];
		differentialPackets[cell].grossCarbonFormedKGPerM3=
			differentialSource.GrossCarbonFormedKGPerM3()[cell];
		differentialPackets[cell].gasHeatReleaseWPerM3=
			differentialSource.GasHeatReleaseWPerM3()[cell];
		differentialPackets[cell].sootHeatReleaseWPerM3=
			differentialSource.SootHeatReleaseWPerM3()[cell];
		differentialPackets[cell].pilotEnergyDeltaJPerM3=
			differentialSource.PilotEnergyDeltaJPerM3()[cell];
		differentialPackets[cell].pilotExpansionIntegral=
			differentialSource.PilotExpansionIntegral()[cell];
		differentialPackets[cell].radiativeCoolingWPerM3=
			differentialSource.RadiativeCoolingWPerM3()[cell];
	}
	RISE::FireSim::PeriodicMACField differentialMomentum;
	for(unsigned int axis=0u;axis<3u;++axis)
		differentialMomentum.component[axis].assign(
			differentialRequest.beginningMomentumKGPerM2S[axis].begin(),
			differentialRequest.beginningMomentumKGPerM2S[axis].end());
	RISE::FireSim::ConservativeAdvance3DConfig differentialConfig;
	differentialConfig.transport.cellWidthM=ownerSourceRequest.shape.cellWidthM;
	differentialConfig.transport.deltaTimeS=differentialSourceRequest.timeStepS;
	differentialConfig.transport.ambientTemperatureK=differentialPilotTemperatureK;
	differentialConfig.transport.adiabaticTemperatureK=
		sourceCase.derived.maximumAcceptedTemperatureK;
	differentialConfig.transport.ambientGasDensityKGPerM3=
		differentialBoundary.ambientDensityKGPerM3;
	differentialConfig.transport.producerPrecision=RISE::FireStateProducerPrecision::Binary32;
	differentialConfig.projectionTolerancePerS=differentialRequest.projectionTolerancePerS;
	differentialConfig.periodicBoundaries=false;differentialConfig.retainStageDiagnostics=true;
	differentialConfig.workerCount=1u;differentialConfig.openBoundary=differentialBoundary;
	differentialConfig.injectedTemperatureK=differentialPilotTemperatureK;
	RISE::FireSim::ConservativeAdvance3DResult differentialOracle;
	const bool differentialOracleOK=RISE::FireSim::AdvanceConservative3D(
		differentialShape,differentialBeginning,differentialMomentum,differentialPackets,
		differentialConfig,eosRecord,eosRecord,RISE::FireSimulationTransportRecord::OpenV1(),
		differentialOracle,&error);
	if(!differentialOracleOK)std::fprintf(stderr,
		"projected-Heun independent oracle detail: %s\n",error.c_str());
	RISE::FireSim::ConservativeAdvance3DConfig differentialFineConfig=differentialConfig;
	differentialFineConfig.projectionTolerancePerS=
		differentialConfig.projectionTolerancePerS/16.0;
	RISE::FireSim::ConservativeAdvance3DConfig differentialFinerConfig=differentialConfig;
	differentialFinerConfig.projectionTolerancePerS=
		differentialConfig.projectionTolerancePerS/256.0;
	RISE::FireSim::ConservativeAdvance3DResult differentialFineOracle,
		differentialFinerOracle;
	const bool differentialFineOracleOK=RISE::FireSim::AdvanceConservative3D(
		differentialShape,differentialBeginning,differentialMomentum,differentialPackets,
		differentialFineConfig,eosRecord,eosRecord,
		RISE::FireSimulationTransportRecord::OpenV1(),differentialFineOracle,&error);
	const bool differentialFinerOracleOK=RISE::FireSim::AdvanceConservative3D(
		differentialShape,differentialBeginning,differentialMomentum,differentialPackets,
		differentialFinerConfig,eosRecord,eosRecord,
		RISE::FireSimulationTransportRecord::OpenV1(),differentialFinerOracle,&error);
	if(!differentialFineOracleOK||!differentialFinerOracleOK)std::fprintf(stderr,
		"projected-Heun converged oracle reference detail: fine=%u finer=%u %s\n",
		differentialFineOracleOK?1u:0u,differentialFinerOracleOK?1u:0u,error.c_str());
	double independentOwnerMaximumDifference=0.0;
	bool independentOwnerWithinFP32Bound=true;
	const double independentFP32ForwardFactor=64.0;
	double independentStageVelocityDifference=0.0;
	double independentStageMomentumDifference=0.0;
	double independentStagePressureDifference=0.0;
	double independentStagePressureOffset=0.0;
	double independentStagePressureAbsoluteDifference=0.0;
	double independentStagePressureReferenceBand=0.0;
	double independentStagePressureReferenceContraction=0.0;
	double independentStagePressureReferenceTail=0.0;
	double independentStagePressureConvergedDifference=0.0;
	bool independentStagePressureCertified=true;
	bool independentSourceActive=false;
	for(const float dose:differentialSource.SourceDelta())
		independentSourceActive=independentSourceActive||dose!=0.0f;
	if(differentialProductionOK&&differentialOracleOK)for(std::size_t cell=0u;
		cell<sourceCells;++cell){
		for(std::size_t component=0u;component<9u;++component){
			const double oracleValue=differentialOracle.conservative[cell][component];
			const double difference=std::fabs(static_cast<double>(differentialResult.
				conservativeValues[component*sourceCells+cell])-oracleValue);
			independentOwnerMaximumDifference=std::max(independentOwnerMaximumDifference,difference);
			independentOwnerWithinFP32Bound=independentOwnerWithinFP32Bound&&difference<=
				independentFP32ForwardFactor*std::numeric_limits<float>::epsilon()*
					std::max(1.0,std::fabs(oracleValue));
		}
	}
	if(differentialProductionOK&&differentialOracleOK)for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<differentialResult.velocityMPerS[axis].size();++face){
			const double oracleValue=differentialOracle.velocityMPerS.component[axis][face];
			const double difference=std::fabs(static_cast<double>(
				differentialResult.velocityMPerS[axis][face])-oracleValue);
			independentOwnerMaximumDifference=std::max(independentOwnerMaximumDifference,difference);
			independentOwnerWithinFP32Bound=independentOwnerWithinFP32Bound&&difference<=
				independentFP32ForwardFactor*std::numeric_limits<float>::epsilon()*
					std::max(1.0,std::fabs(oracleValue));
		}
	if(differentialProductionOK&&differentialOracleOK&&differentialFineOracleOK&&
		differentialFinerOracleOK){
		const auto compareStage=[&](
			const RISE::FireProductionProjectedHeunCoupledStageResult& production,
			const RISE::FireSim::ConservativeStage3D& oracle,
			const RISE::FireSim::ConservativeStage3D& fineOracle,
			const RISE::FireSim::ConservativeStage3D& finerOracle){
			for(unsigned int axis=0u;axis<3u;++axis){
				for(std::size_t face=0u;face<production.projection.velocityMPerS[axis].size();++face){
					const double oracleVelocity=oracle.openProjection.velocityMPerS.component[axis][face];
					const double velocityDifference=std::fabs(static_cast<double>(production.projection.
						velocityMPerS[axis][face])-oracleVelocity);
					const double oracleMomentum=oracle.openProjection.momentumKGPerM2S.component[axis][face];
					const double momentumDifference=std::fabs(static_cast<double>(production.projection.
						momentumKGPerM2S[axis][face])-oracleMomentum);
					independentStageVelocityDifference=std::max(independentStageVelocityDifference,
						velocityDifference);
					independentStageMomentumDifference=std::max(independentStageMomentumDifference,
						momentumDifference);
					independentOwnerWithinFP32Bound=independentOwnerWithinFP32Bound&&
						velocityDifference<=independentFP32ForwardFactor*
							std::numeric_limits<float>::epsilon()*
							std::max(1.0,std::fabs(oracleVelocity))&&
						momentumDifference<=independentFP32ForwardFactor*
							std::numeric_limits<float>::epsilon()*
							std::max(1.0,std::fabs(oracleMomentum));
				}
			}
			std::vector<double> pressureDifference(production.projection.pressurePa.size(),0.0);
			double pressureOffset=0.0;
			for(std::size_t cell=0u;cell<pressureDifference.size();++cell){
				const double coarse=oracle.openProjection.stepAverageDynamicPressurePa[cell];
				const double fine=fineOracle.openProjection.stepAverageDynamicPressurePa[cell];
				const double finer=finerOracle.openProjection.stepAverageDynamicPressurePa[cell];
				const double productionPressure=static_cast<double>(
					production.projection.pressurePa[cell]);
				pressureDifference[cell]=productionPressure-coarse;
				independentStagePressureAbsoluteDifference=std::max(
					independentStagePressureAbsoluteDifference,std::fabs(pressureDifference[cell]));
				const double contraction=std::fabs(coarse-fine),tail=std::fabs(fine-finer);
				const double convergedDifference=std::fabs(productionPressure-finer);
				const double referenceBand=std::ldexp(1.0,-10);
				independentStagePressureReferenceContraction=std::max(
					independentStagePressureReferenceContraction,contraction);
				independentStagePressureReferenceTail=std::max(
					independentStagePressureReferenceTail,tail);
				independentStagePressureConvergedDifference=std::max(
					independentStagePressureConvergedDifference,convergedDifference);
				independentStagePressureReferenceBand=std::max(
					independentStagePressureReferenceBand,referenceBand);
				independentStagePressureCertified=independentStagePressureCertified&&
					convergedDifference<=referenceBand;
				pressureOffset+=pressureDifference[cell];
			}
			if(!pressureDifference.empty())pressureOffset/=pressureDifference.size();
			independentStagePressureOffset=std::max(independentStagePressureOffset,
				std::fabs(pressureOffset));
			for(const double difference:pressureDifference)
				independentStagePressureDifference=std::max(independentStagePressureDifference,
					std::fabs(difference-pressureOffset));
		};
		compareStage(differentialResult.r0,differentialOracle.r0,
			differentialFineOracle.r0,differentialFinerOracle.r0);
		compareStage(differentialResult.r1,differentialOracle.r1,
			differentialFineOracle.r1,differentialFinerOracle.r1);
		compareStage(differentialResult.r2,differentialOracle.r2,
			differentialFineOracle.r2,differentialFinerOracle.r2);
		independentOwnerMaximumDifference=std::max({independentOwnerMaximumDifference,
			independentStageVelocityDifference,independentStageMomentumDifference});
	}
	const bool independentPressureReferenceConverged=
		independentStagePressureReferenceTail<=
		0.25*independentStagePressureReferenceContraction;
	if(differentialProductionOK&&differentialOracleOK&&
		(!independentOwnerWithinFP32Bound||!independentStagePressureCertified||
		!independentPressureReferenceConverged))std::fprintf(stderr,
		"projected-Heun independent stage max: all=%.17g velocity=%.17g momentum=%.17g pressure=%.17g pressure_gradient=%.17g pressure_offset=%.17g pressure_band=%.17g converged_pressure=%.17g oracle_coarse_fine=%.17g oracle_fine_finer=%.17g\n",
		independentOwnerMaximumDifference,independentStageVelocityDifference,
		independentStageMomentumDifference,independentStagePressureAbsoluteDifference,
		independentStagePressureDifference,independentStagePressureOffset,
		independentStagePressureReferenceBand,
		independentStagePressureConvergedDifference,
		independentStagePressureReferenceContraction,independentStagePressureReferenceTail);
	Check(differentialProductionOK&&differentialOracleOK&&differentialFineOracleOK&&
		differentialFinerOracleOK&&independentSourceActive&&independentOwnerWithinFP32Bound&&
		independentStagePressureCertified&&independentPressureReferenceConverged,
		"r190 source-active pressure-open owner matches every projected stage of the independent fp64 oracle schedule");
	const auto setOwnerFailure=[](const char* value){
#if defined(_WIN32)
		return _putenv_s("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE",value?value:"")==0;
#else
		return value?setenv("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE",value,1)==0:
			unsetenv("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE")==0;
#endif
	};
	const auto vectorsEmpty=[](const auto& vectors){
		for(const auto& values:vectors)if(!values.empty())return false;
		return true;
	};
	const auto shapeDefault=[](const RISE::FireProductionProjectionShape& shape){
		return shape.nx==0u&&shape.ny==0u&&shape.nz==0u&&shape.cellWidthM==0.0f;
	};
	const auto boundariesDefault=[](const auto& boundary){
		for(const auto side:boundary)if(side!=RISE::FireProductionProjectionWall)return false;
		return true;
	};
	const auto offsetsDefault=[](const std::array<std::size_t,3>& offsets){
		return offsets[0]==0u&&offsets[1]==0u&&offsets[2]==0u;
	};
	const auto projectionDefault=[&](const RISE::FireProductionProjectionResult& projection){
		return vectorsEmpty(projection.faceDensityKGPerM3)&&
			vectorsEmpty(projection.velocityMPerS)&&vectorsEmpty(projection.momentumKGPerM2S)&&
			projection.pressurePa.empty()&&vectorsEmpty(projection.pressureOpenInflow)&&
			projection.maximumPreProjectionResidualPerS==0.0f&&
			projection.maximumPostProjectionResidualPerS==0.0f&&
			projection.validationBandPerS==0.0f&&
			projection.maximumOpenComplementarityDiscrepancyMPerS==0.0f&&
			projection.removedFineRightHandSideMean==0.0f&&
			projection.executedVCycleCount==0u&&projection.executedJacobiSweepCount==0u&&
			projection.residentUploadStagingCount==0u&&
			projection.residentInterstageDeviceToHostTransferCount==0u&&
			projection.residentTerminalStagingCount==0u&&
			projection.residentCommandCommitCount==0u&&
			projection.residentProjectionInvocationCount==0u&&
			projection.residentCertifiedWorkingSetBytes==0u&&
			projection.residentActualMetalAllocationBytes==0u&&!projection.validationPassed&&
			projection.deviceElapsedMS==0.0&&projection.deviceStartTimeS==0.0&&
			projection.deviceEndTimeS==0.0;
	};
	const auto physicalFluxDefault=[&](
		const RISE::FireProductionScalarPhysicalFluxPrerequisiteResult& flux){
		return shapeDefault(flux.shape)&&boundariesDefault(flux.boundary)&&
			offsetsDefault(flux.packedFaceOffset)&&flux.physicalMassFluxKGPerM2S.empty()&&
			flux.physicalEnergyFluxWPerM2.empty()&&vectorsEmpty(flux.physicalGasFluxKGPerM2S)&&
			flux.methaneRecordId.empty()&&flux.maximumFP64ReferenceResidualKGPerM2S==0.0&&
			flux.fp64ReferenceForwardErrorBoundKGPerM2S==0.0&&
			flux.maximumConstraintResidualKGPerM2S==0.0&&
			flux.constraintForwardErrorBoundKGPerM2S==0.0&&
			!flux.fp64ReferenceIdentityVerified;
	};
	const auto scalarAcceptanceDefault=[&](const RISE::FireProductionScalarFCTResult& scalar){
		return offsetsDefault(scalar.packedFaceOffset)&&scalar.lowFlux.empty()&&
			scalar.fluxDelta.empty()&&scalar.lowState.empty()&&scalar.limiterRatio.empty()&&
			vectorsEmpty(scalar.sharedFaceAlpha)&&scalar.accepted.empty()&&
			vectorsEmpty(scalar.acceptedGasFluxKGPerM2S)&&
			scalar.maximumCommutingResidualKGPerM3==0.0f&&
			scalar.commutingIdentityScaleKGPerM3==0.0f&&
			scalar.commutingIdentityRestrictedAcceptedKGPerM3==0.0f&&
			scalar.commutingIdentityAdvancedKGPerM3==0.0f&&
			scalar.commutingIdentityComponent==0u&&scalar.commutingIdentityFace==0u&&
			!scalar.commutingIdentityAvailable;
	};
	const auto fluxPairDefault=[&](const RISE::FireProductionScalarFCTFluxPair& pair){
		return shapeDefault(pair.shape)&&pair.timeStepS==0.0f&&
			boundariesDefault(pair.boundary)&&offsetsDefault(pair.packedFaceOffset)&&
			pair.lowFlux.empty()&&pair.fluxDelta.empty();
	};
	const auto heunFluxDefault=[&](const RISE::FireProductionScalarHeunFluxStage& flux){
		return fluxPairDefault(flux.compositeFluxPair)&&flux.physicalMassFluxKGPerM2S.empty()&&
			flux.physicalEnergyFluxWPerM2.empty()&&vectorsEmpty(flux.physicalGasFluxKGPerM2S)&&
			vectorsEmpty(flux.advectiveGasLowFluxKGPerM2S)&&
			vectorsEmpty(flux.advectiveGasFluxDeltaKGPerM2S)&&flux.methaneRecordId.empty()&&
			flux.attemptIdentity==0u&&static_cast<unsigned int>(flux.role)==0u&&
			flux.stageInputIdentity==0u&&flux.sharedFCTContractIdentity==0u&&
			flux.fctRequestIdentity==0u&&flux.frozenVelocityIdentity==0u&&
			flux.parentCompositionIdentity[0]==0u&&flux.parentCompositionIdentity[1]==0u&&
			flux.compositionIdentity==0u&&flux.physicalConstraintForwardErrorBoundKGPerM2S==0.0&&
			flux.physicalGasAveragingForwardErrorBoundKGPerM2S==0.0&&
			!flux.fp64ReferenceIdentityVerified;
	};
	const auto heunSolveDefault=[&](const RISE::FireProductionScalarHeunSolveResult& solve){
		return scalarAcceptanceDefault(solve.scalar)&&solve.attemptIdentity==0u&&
			solve.averageCompositionIdentity==0u&&solve.sharedFCTContractIdentity==0u&&
			solve.parentCompositionIdentity[0]==0u&&solve.parentCompositionIdentity[1]==0u&&
			solve.alphaIdentity==0u;
	};
	const auto eosDefault=[&](const RISE::FireProductionScalarEOSAcceptanceResult& eos){
		return shapeDefault(eos.shape)&&eos.timeStepS==0.0f&&eos.attemptIdentity==0u&&
			static_cast<unsigned int>(eos.stage)==0u&&
			eos.producerPrecision==RISE::FireStateProducerPrecision::Binary32&&
			eos.methaneRecordId.empty()&&eos.caseRecordId.empty()&&
			eos.lowerTemperatureK==0.0&&eos.upperTemperatureK==0.0&&
			eos.temperatureK.empty()&&eos.maximumEOSResidual==0.0&&eos.stateDigest==0u&&
			eos.temperatureDigest==0u&&eos.acceptanceIdentity==0u&&!eos.accepted;
	};
	const auto nonpressureDefault=[&](
		const RISE::FireProductionNonpressureMomentumRHSResult& nonpressure){
		return nonpressure.eddyKinematicViscosityM2PerS.empty()&&
			nonpressure.effectiveDynamicViscosityPaS.empty()&&
			vectorsEmpty(nonpressure.buoyancyMomentumRateKGPerM2S2)&&
			vectorsEmpty(nonpressure.stressMomentumRateKGPerM2S2)&&
			vectorsEmpty(nonpressure.phaseSourceMomentumRateKGPerM2S2)&&
			vectorsEmpty(nonpressure.combinedMomentumRateKGPerM2S2);
	};
	const auto targetDefault=[&](const RISE::FireProductionScalarProjectionTargetSeal& target){
		return shapeDefault(target.Shape())&&target.TimeStepS()==0.0f&&
			target.AttemptIdentity()==0u&&static_cast<unsigned int>(target.Role())==0u&&
			boundariesDefault(target.Boundary())&&target.TargetPerS().empty()&&
			target.BaseTargetIdentity()==0u&&target.ParentTargetIdentity()==0u&&
			target.AcceptedCandidateIdentity()==0u&&target.CorrectionIteration()==0u&&
			target.TargetIdentity()==0u&&!target.IsSealed();
	};
	const auto stageDefault=[&](const RISE::FireProductionProjectedHeunCoupledStageResult& stage){
		return static_cast<unsigned int>(stage.stage)==0u&&
			projectionDefault(stage.projection)&&heunFluxDefault(stage.flux)&&
			physicalFluxDefault(stage.endpointPhysicalFlux)&&
			scalarAcceptanceDefault(stage.scalarAcceptance)&&
			nonpressureDefault(stage.nonpressure)&&targetDefault(stage.projectionTarget)&&
			targetDefault(stage.target)&&
			stage.picardResidualPerS.empty()&&stage.parentCandidateIdentity==0u&&
			stage.acceptedCandidateIdentity==0u&&stage.acceptedIterationCount==0u&&
			stage.activeSetCycleLength==0u&&stage.activeSetDifferingFaceCount==0u&&
			stage.activeSetCanonicalProjectionCount==0u&&
			stage.maximumActiveSetComplementarityDiscrepancyMPerS==0.0f&&
			stage.maximumLimiterClassDiscrepancy==0.0f&&
			!stage.activeSetDiscontinuousClass&&!stage.limiterDiscontinuousClass;
	};
	const auto ownerResultDefault=[&](const RISE::FireProductionProjectedHeunOwnerResult& value){
		return value.conservativeValues.empty()&&vectorsEmpty(value.momentumKGPerM2S)&&
			vectorsEmpty(value.velocityMPerS)&&value.stepAveragePressurePa.empty()&&
			eosDefault(value.predictorEOS)&&eosDefault(value.committedEOS)&&
			heunFluxDefault(value.averagedFlux)&&heunSolveDefault(value.heunSolve)&&
			stageDefault(value.r0)&&stageDefault(value.r1)&&stageDefault(value.r2)&&
			value.sourcePacketIdentity==0u&&value.OwnerIdentity()==0u&&!value.accepted;
	};
	const auto mutatingTransportRefusalRetries=[&](
		const RISE::FireProductionProjectedHeunStage stage,
		const ProjectedHeunContextOperand operand,const bool callbackReturnsSuccess){
		RISE::FireProductionProjectedHeunCPUOwner mutationOwner;
		if(!mutationOwner.Begin(ownerRequest,&error))return false;
		if(stage!=RISE::FireProductionProjectedHeunStage::R0&&
			!mutationOwner.SolveR0(ownerTransport,&error))return false;
		if(stage==RISE::FireProductionProjectedHeunStage::R2&&
			!mutationOwner.SolveR1(ownerTransport,&error))return false;
		MutatingProjectedHeunTransport32 mutating(stage,operand,callbackReturnsSuccess);
		RISE::FireProductionProjectedHeunOwnerResult refused=ownerResult;
		bool rejected=false;
		if(stage==RISE::FireProductionProjectedHeunStage::R0)
			rejected=!mutationOwner.SolveR0(mutating,&error);
		else if(stage==RISE::FireProductionProjectedHeunStage::R1)
			rejected=!mutationOwner.SolveR1(mutating,&error);
		else rejected=!mutationOwner.SolveR2(mutating,refused,&error)&&
			ownerResultDefault(refused);
		if(!rejected||!mutating.Mutated()||
			error.find("transport context was mutated")==std::string::npos)return false;
		RISE::FireProductionProjectedHeunOwnerResult retried;
		if(stage==RISE::FireProductionProjectedHeunStage::R0&&
			!mutationOwner.SolveR0(ownerTransport,&error))return false;
		if(stage!=RISE::FireProductionProjectedHeunStage::R2&&
			!mutationOwner.SolveR1(ownerTransport,&error))return false;
		if(!mutationOwner.SolveR2(ownerTransport,retried,&error))return false;
		return RISE::FireProductionProjectedHeunOwnerResultMatches(retried,ownerSource)&&
			retried.OwnerIdentity()==ownerResult.OwnerIdentity();
	};
	bool mutatingTransportMatrix=true;
	const std::array<RISE::FireProductionProjectedHeunStage,3> mutatingStages={{
		RISE::FireProductionProjectedHeunStage::R0,
		RISE::FireProductionProjectedHeunStage::R1,
		RISE::FireProductionProjectedHeunStage::R2}};
	const std::array<ProjectedHeunContextOperand,10> mutatingOperands={{
		ProjectedHeunContextOperand::ConservativeState,
		ProjectedHeunContextOperand::Temperature,
		ProjectedHeunContextOperand::ProjectedVelocity,
		ProjectedHeunContextOperand::Stage,
		ProjectedHeunContextOperand::AttemptIdentity,
		ProjectedHeunContextOperand::ParentCandidateIdentity,
		ProjectedHeunContextOperand::ProjectionIdentity,
		ProjectedHeunContextOperand::ConservativeStatePointer,
		ProjectedHeunContextOperand::TemperaturePointer,
		ProjectedHeunContextOperand::ProjectedVelocityPointer}};
	for(const auto stage:mutatingStages)for(const auto operand:mutatingOperands)
		for(const bool callbackReturnsSuccess:{false,true})mutatingTransportMatrix=
			mutatingTransportMatrix&&mutatingTransportRefusalRetries(
				stage,operand,callbackReturnsSuccess);

	RISE::FireProductionProjectedHeunCPUOwner outOfOrderOwner;
	RISE::FireProductionProjectedHeunOwnerResult refusedOwnerResult=ownerResult;
	const bool outOfOrderBegun=outOfOrderOwner.Begin(ownerRequest,&error);
	const bool outOfOrderR1Rejected=outOfOrderBegun&&
		!outOfOrderOwner.SolveR1(ownerTransport,&error)&&
		error=="projected-Heun R1 is out of order";
	const bool outOfOrderR2Rejected=outOfOrderR1Rejected&&
		!outOfOrderOwner.SolveR2(ownerTransport,refusedOwnerResult,&error)&&
		error=="projected-Heun R2 is out of order"&&
		ownerResultDefault(refusedOwnerResult);
	const bool outOfOrderRejected=outOfOrderR2Rejected&&
		outOfOrderOwner.SolveR0(ownerTransport,&error)&&
		outOfOrderOwner.SolveR1(ownerTransport,&error)&&
		outOfOrderOwner.SolveR2(ownerTransport,refusedOwnerResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			refusedOwnerResult,ownerSource);
	ForgedProjectedHeunTransport32 staleOwnerTransport(true),forgedOwnerTransport(false);
	ReplayedProjectedHeunTransport32 replayedOwnerTransport;
	RISE::FireProductionProjectedHeunCPUOwner staleOwner,forgedOwner,replayedOwner;
	const bool staleCandidateRejectedCore=staleOwner.Begin(ownerRequest,&error)&&
		!staleOwner.SolveR0(staleOwnerTransport,&error)&&
		error=="projected-Heun transport publication lineage differs";
	const bool staleCandidateRejected=staleCandidateRejectedCore&&
		staleOwner.SolveR0(ownerTransport,&error);
	const bool forgedLineageRejectedCore=forgedOwner.Begin(ownerRequest,&error)&&
		!forgedOwner.SolveR0(forgedOwnerTransport,&error)&&
		error=="projected-Heun transport publication lineage differs";
	const bool forgedLineageRejected=forgedLineageRejectedCore&&
		forgedOwner.SolveR0(ownerTransport,&error);
	const bool replayedPayloadRejectedCore=replayedOwner.Begin(ownerRequest,&error)&&
		!replayedOwner.SolveR0(replayedOwnerTransport,&error)&&
		error=="projected-Heun transport publication payload differs";
	const bool replayedPayloadRejected=replayedPayloadRejectedCore&&
		replayedOwner.SolveR0(ownerTransport,&error);
	const auto r2TransportRefusalRetries=[&](
		auto& refusedTransport,const char* expectedError){
		RISE::FireProductionProjectedHeunCPUOwner refusedOwner;
		RISE::FireProductionProjectedHeunOwnerResult refusedResult=ownerResult;
		const bool prepared=refusedOwner.Begin(ownerRequest,&error)&&
			refusedOwner.SolveR0(ownerTransport,&error)&&
			refusedOwner.SolveR1(ownerTransport,&error);
		const bool rejected=prepared&&
			!refusedOwner.SolveR2(refusedTransport,refusedResult,&error)&&
			error==expectedError&&ownerResultDefault(refusedResult);
		return rejected&&refusedOwner.SolveR2(ownerTransport,refusedResult,&error)&&
			RISE::FireProductionProjectedHeunOwnerResultMatches(
				refusedResult,ownerSource);
	};
	ForgedProjectedHeunTransport32 staleR2Transport(true),forgedR2Transport(false);
	ReplayedProjectedHeunTransport32 replayedR2Transport;
	const bool staleR2CandidateRejected=r2TransportRefusalRetries(
		staleR2Transport,"projected-Heun R2 transport lineage differs");
	const bool forgedR2LineageRejected=r2TransportRefusalRetries(
		forgedR2Transport,"projected-Heun R2 transport lineage differs");
	const bool replayedR2PayloadRejected=r2TransportRefusalRetries(
		replayedR2Transport,"projected-Heun R2 transport payload differs");
	RISE::FireProductionProjectedHeunCPUOwner reentrantOwner;
	RISE::FireProductionProjectedHeunOwnerResult reentrantResult;
	ReentrantProjectedHeunTransport32 reentrantTransport(reentrantOwner,ownerResult);
	const bool reentrantPrepared=reentrantOwner.Begin(ownerRequest,&error);
	const bool reentrantR0=reentrantPrepared&&
		reentrantOwner.SolveR0(reentrantTransport,&error);
	const bool reentrantRefused=reentrantR0&&reentrantTransport.NestedStagesRejected()&&
		ownerResultDefault(reentrantTransport.NestedResult());
	const bool reentrantRetryable=reentrantRefused&&
		reentrantOwner.SolveR1(ownerTransport,&error)&&
		reentrantOwner.SolveR2(ownerTransport,reentrantResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(reentrantResult,ownerSource);
	const auto r0ProjectionValidationRejected=[&](const char* failure){
		RISE::FireProductionProjectedHeunCPUOwner validationOwner;
		const bool injected=setOwnerFailure(failure);
		const bool rejected=injected&&validationOwner.Begin(ownerRequest,&error)&&
			!validationOwner.SolveR0(ownerTransport,&error)&&
			error.find("projection validation")!=std::string::npos;
		return setOwnerFailure(0)&&rejected;
	};
	const bool initialValidationRejected=
		r0ProjectionValidationRejected("projection-validation-initial");
	const bool iterativeValidationRejected=
		r0ProjectionValidationRejected("projection-validation-iterative");
	const bool terminalValidationRejected=
		r0ProjectionValidationRejected("projection-validation-terminal");
	RISE::FireProductionProjectedHeunCPUOwner r2ValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2ValidationResult;
	const bool r2ValidationPrepared=r2ValidationOwner.Begin(ownerRequest,&error)&&
		r2ValidationOwner.SolveR0(ownerTransport,&error)&&
		r2ValidationOwner.SolveR1(ownerTransport,&error);
	const bool r2ValidationInjected=setOwnerFailure("projection-validation-r2-endpoint");
	const bool r2ValidationRejectedCore=r2ValidationPrepared&&r2ValidationInjected&&
		!r2ValidationOwner.SolveR2(ownerTransport,r2ValidationResult,&error)&&
		error.find("R2 projection validation")!=std::string::npos;
	const bool r2ValidationRejected=setOwnerFailure(0)&&r2ValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2TerminalValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2TerminalValidationResult;
	const bool r2TerminalValidationPrepared=
		r2TerminalValidationOwner.Begin(ownerRequest,&error)&&
		r2TerminalValidationOwner.SolveR0(ownerTransport,&error)&&
		r2TerminalValidationOwner.SolveR1(ownerTransport,&error);
	const bool r2TerminalValidationInjected=setOwnerFailure(
		"projection-validation-r2-terminal");
	const bool r2TerminalValidationRejectedCore=r2TerminalValidationPrepared&&
		r2TerminalValidationInjected&&!r2TerminalValidationOwner.SolveR2(
			ownerTransport,r2TerminalValidationResult,&error)&&
		error.find("R2 terminal projection validation")!=std::string::npos&&
		ownerResultDefault(r2TerminalValidationResult);
	const bool r2TerminalValidationRejected=setOwnerFailure(0)&&
		r2TerminalValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2BootstrapValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2BootstrapValidationResult;
	const bool r2BootstrapPrepared=r2BootstrapValidationOwner.Begin(ownerRequest,&error)&&
		r2BootstrapValidationOwner.SolveR0(ownerTransport,&error)&&
		r2BootstrapValidationOwner.SolveR1(ownerTransport,&error);
	const bool r2BootstrapInjected=setOwnerFailure("projection-validation-r2-bootstrap");
	const bool r2BootstrapRejectedCore=r2BootstrapPrepared&&r2BootstrapInjected&&
		!r2BootstrapValidationOwner.SolveR2(ownerTransport,r2BootstrapValidationResult,&error)&&
		error.find("bootstrap projection validation")!=std::string::npos;
	const bool r2BootstrapRejected=setOwnerFailure(0)&&r2BootstrapRejectedCore;
	const bool r2BootstrapRetry=r2BootstrapRejected&&
		r2BootstrapValidationOwner.SolveR2(ownerTransport,r2BootstrapValidationResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			r2BootstrapValidationResult,ownerSource);
	RISE::FireProductionProjectedHeunCPUOwner workingSetOwner;
	const bool workingSetInjected=setOwnerFailure("working-set-preflight");
	const bool workingSetAdmissionRejectedCore=workingSetInjected&&
		!workingSetOwner.Begin(ownerRequest,&error)&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	const bool workingSetAdmissionRejected=setOwnerFailure(0)&&
		workingSetAdmissionRejectedCore;
	RISE::FireProductionFrozenMethaneSourceRequest limiterSourceRequest=
		differentialSourceRequest;
	limiterSourceRequest.attemptIdentity=UINT64_C(0x1900000000000003);
	limiterSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	for(std::size_t cell=0u;cell<sourceCells;++cell){
		const std::array<float,9> local=eosTuple(
			differentialPilotTemperatureK+0.1*static_cast<double>(cell%4u));
		for(std::size_t component=0u;component<9u;++component)
			limiterSourceRequest.beginningConservativeValues[component*sourceCells+cell]=
				local[component];
	}
	RISE::FireProductionFrozenSourcePacketSeal limiterSource;
	const bool limiterSourceOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			limiterSourceRequest,limiterSource,&error);
	RISE::FireProductionProjectedHeunOwnerRequest limiterRequest=differentialRequest;
	limiterRequest.attemptIdentity=limiterSourceRequest.attemptIdentity;
	limiterRequest.source=limiterSource;
	limiterRequest.beginningConservativeValues=
		limiterSourceRequest.beginningConservativeValues;
	limiterRequest.scalarContract.boundary.fill(RISE::FireProductionProjectionPeriodic);
	limiterRequest.physicalContract.boundary=limiterRequest.scalarContract.boundary;
	limiterRequest.forceContract.boundary=limiterRequest.scalarContract.boundary;
	for(unsigned int axis=0u;axis<3u;++axis)
		std::fill(limiterRequest.beginningMomentumKGPerM2S[axis].begin(),
			limiterRequest.beginningMomentumKGPerM2S[axis].end(),axis==0u?0.0002f:0.0f);
	limiterRequest.projectionTolerancePerS=5.0e-3f;
	limiterRequest.maximumPicardIterations=64u;
	RISE::FireProductionProjectedHeunCPUOwner limiterBaselineOwner;
	RISE::FireProductionProjectedHeunOwnerResult limiterBaselineResult;
	const bool limiterBaselineOK=limiterSourceOK&&
		limiterBaselineOwner.Begin(limiterRequest,&error)&&
		limiterBaselineOwner.SolveR0(ownerTransport,&error)&&
		limiterBaselineOwner.SolveR1(ownerTransport,&error)&&
		limiterBaselineOwner.SolveR2(ownerTransport,limiterBaselineResult,&error);
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest limiterRequest64=
		ownerRequest64;
	limiterRequest64.attemptIdentity=limiterRequest.attemptIdentity;
	limiterRequest64.source=
		RISEFireProductionFP64::FireProductionFrozenSourcePacketSeal::CalibrationImport(
			limiterSource);
	limiterRequest64.beginningConservativeValues.assign(
		limiterRequest.beginningConservativeValues.begin(),
		limiterRequest.beginningConservativeValues.end());
	limiterRequest64.scalarContract.timeStepS=limiterRequest.scalarContract.timeStepS;
	limiterRequest64.forceContract.timeStepS=limiterRequest.forceContract.timeStepS;
	for(unsigned int side=0u;side<6u;++side){
		limiterRequest64.scalarContract.boundary[side]=static_cast<
			RISEFireProductionFP64::FireProductionProjectionBoundary>(
				limiterRequest.scalarContract.boundary[side]);
		limiterRequest64.physicalContract.boundary[side]=
			limiterRequest64.scalarContract.boundary[side];
		limiterRequest64.forceContract.boundary[side]=
			limiterRequest64.scalarContract.boundary[side];
	}
	for(std::size_t component=0u;component<9u;++component){
		limiterRequest64.scalarContract.ambient[component]=
			limiterRequest.scalarContract.ambient[component];
		limiterRequest64.physicalContract.ambient[component]=
			limiterRequest.physicalContract.ambient[component];
	}
	for(std::size_t value=0u;value<14u;++value)
		limiterRequest64.scalarContract.enthalpyBoundsJPerKG[value]=
			limiterRequest.scalarContract.enthalpyBoundsJPerKG[value];
	limiterRequest64.physicalContract.ambientTemperatureK=
		limiterRequest.physicalContract.ambientTemperatureK;
	for(unsigned int axis=0u;axis<3u;++axis)
		limiterRequest64.beginningMomentumKGPerM2S[axis].assign(
			limiterRequest.beginningMomentumKGPerM2S[axis].begin(),
			limiterRequest.beginningMomentumKGPerM2S[axis].end());
	limiterRequest64.projectionTolerancePerS=limiterRequest.projectionTolerancePerS;
	limiterRequest64.endpointVelocityToleranceMPerS=
		limiterRequest.endpointVelocityToleranceMPerS;
	limiterRequest64.maximumPicardIterations=limiterRequest.maximumPicardIterations;
	RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner limiterMirrorOwner;
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerResult limiterMirrorResult;
	const bool limiterMirrorOK=limiterMirrorOwner.Begin(limiterRequest64,&error)&&
		limiterMirrorOwner.SolveR0(ownerTransport64,&error)&&
		limiterMirrorOwner.SolveR1(ownerTransport64,&error)&&
		limiterMirrorOwner.SolveR2(ownerTransport64,limiterMirrorResult,&error);
	RISE::FireProductionProjectedHeunOwnerRequest limiterFineRequest=limiterRequest;
	limiterFineRequest.projectionTolerancePerS=
		limiterRequest.projectionTolerancePerS/16.0f;
	RISE::FireProductionProjectedHeunCPUOwner limiterFineOwner;
	RISE::FireProductionProjectedHeunOwnerResult limiterFineResult;
	const bool limiterFineOK=limiterFineOwner.Begin(limiterFineRequest,&error)&&
		limiterFineOwner.SolveR0(ownerTransport,&error)&&
		limiterFineOwner.SolveR1(ownerTransport,&error)&&
		limiterFineOwner.SolveR2(ownerTransport,limiterFineResult,&error);
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest limiterFineRequest64=
		limiterRequest64;
	limiterFineRequest64.projectionTolerancePerS=
		limiterRequest64.projectionTolerancePerS/16.0;
	RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner limiterFineOwner64;
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerResult limiterFineResult64;
	const bool limiterFine64OK=limiterFineOwner64.Begin(limiterFineRequest64,&error)&&
		limiterFineOwner64.SolveR0(ownerTransport64,&error)&&
		limiterFineOwner64.SolveR1(ownerTransport64,&error)&&
		limiterFineOwner64.SolveR2(ownerTransport64,limiterFineResult64,&error);
	double limiterMirrorMaximumDifference=0.0;
	double limiterMirrorResidualDifference=0.0;
	double limiterMirrorFluxDifference=0.0,limiterMirrorFluxScale=1.0;
	double limiterMirrorTargetDifference=0.0,limiterProductionTargetRefinement=0.0,
		limiterFP64TargetRefinement=0.0,limiterFineTargetPrecision=0.0,
		limiterTargetScale=1.0;
	double limiterMirrorMaximumFP32=0.0,limiterMirrorMaximumFP64=0.0;
	bool limiterMirrorEndpointMetadata=true;
	const char* limiterMirrorMaximumField="none";
	auto compareLimiterMirror=[&](const char* field,const std::vector<float>& fp32,
		const std::vector<double>& fp64){
		if(fp32.size()!=fp64.size()){
			limiterMirrorMaximumDifference=std::numeric_limits<double>::infinity();return;}
		for(std::size_t value=0u;value<fp32.size();++value){
			const double difference=std::fabs(static_cast<double>(fp32[value])-
				fp64[value])/std::max(1.0,std::fabs(fp64[value]));
			if(difference>limiterMirrorMaximumDifference){
				limiterMirrorMaximumDifference=difference;
				limiterMirrorMaximumField=field;
				limiterMirrorMaximumFP32=fp32[value];limiterMirrorMaximumFP64=fp64[value];
			}
		}
	};
	auto compareLimiterFlux=[&](const std::vector<float>& fp32,
		const std::vector<double>& fp64){
		if(fp32.size()!=fp64.size()){
			limiterMirrorFluxDifference=std::numeric_limits<double>::infinity();return;}
		for(std::size_t value=0u;value<fp32.size();++value){
			limiterMirrorFluxDifference=std::max(limiterMirrorFluxDifference,
				std::fabs(static_cast<double>(fp32[value])-fp64[value]));
			limiterMirrorFluxScale=std::max(limiterMirrorFluxScale,
				std::max(std::fabs(static_cast<double>(fp32[value])),std::fabs(fp64[value])));
		}
	};
	auto compareLimiterFluxCertificate=[&](const double fp32,const double fp64){
		limiterMirrorFluxDifference=std::max(limiterMirrorFluxDifference,
			std::fabs(fp32-fp64));
		limiterMirrorFluxScale=std::max(limiterMirrorFluxScale,
			std::max(std::fabs(fp32),std::fabs(fp64)));
	};
	auto compareEndpointPhysicalFlux=[&](const auto& fp32,const auto& fp64){
		compareLimiterFlux(fp32.physicalMassFluxKGPerM2S,
			fp64.physicalMassFluxKGPerM2S);
		compareLimiterFlux(fp32.physicalEnergyFluxWPerM2,
			fp64.physicalEnergyFluxWPerM2);
		for(unsigned int axis=0u;axis<3u;++axis)
			compareLimiterFlux(fp32.physicalGasFluxKGPerM2S[axis],
				fp64.physicalGasFluxKGPerM2S[axis]);
		compareLimiterFluxCertificate(fp32.maximumFP64ReferenceResidualKGPerM2S,
			fp64.maximumFP64ReferenceResidualKGPerM2S);
		compareLimiterFluxCertificate(fp32.fp64ReferenceForwardErrorBoundKGPerM2S,
			fp64.fp64ReferenceForwardErrorBoundKGPerM2S);
		compareLimiterFluxCertificate(fp32.maximumConstraintResidualKGPerM2S,
			fp64.maximumConstraintResidualKGPerM2S);
		compareLimiterFluxCertificate(fp32.constraintForwardErrorBoundKGPerM2S,
			fp64.constraintForwardErrorBoundKGPerM2S);
		limiterMirrorEndpointMetadata=limiterMirrorEndpointMetadata&&
			fp32.shape.nx==fp64.shape.nx&&fp32.shape.ny==fp64.shape.ny&&
			fp32.shape.nz==fp64.shape.nz&&fp32.shape.cellWidthM==fp64.shape.cellWidthM&&
			fp32.packedFaceOffset==fp64.packedFaceOffset&&
			fp32.methaneRecordId==fp64.methaneRecordId&&
			fp32.fp64ReferenceIdentityVerified==fp64.fp64ReferenceIdentityVerified;
		for(unsigned int side=0u;side<6u;++side)
			limiterMirrorEndpointMetadata=limiterMirrorEndpointMetadata&&
				static_cast<unsigned int>(fp32.boundary[side])==
				static_cast<unsigned int>(fp64.boundary[side]);
	};
	auto targetDistance=[&](const auto& first,const auto& second){
		double maximum=0.0;
		if(first.size()!=second.size())return std::numeric_limits<double>::max();
		for(std::size_t value=0u;value<first.size();++value){
			maximum=std::max(maximum,std::fabs(static_cast<double>(first[value])-second[value]));
			limiterTargetScale=std::max(limiterTargetScale,std::max(
				std::fabs(static_cast<double>(first[value])),
				std::fabs(static_cast<double>(second[value]))));
		}
		return maximum;
	};
	if(limiterBaselineOK&&limiterMirrorOK){
		compareLimiterMirror("conservative",limiterBaselineResult.conservativeValues,
			limiterMirrorResult.conservativeValues);
		compareLimiterMirror("step_pressure",limiterBaselineResult.stepAveragePressurePa,
			limiterMirrorResult.stepAveragePressurePa);
		const RISE::FireProductionProjectedHeunCoupledStageResult* stages32[3]={
			&limiterBaselineResult.r0,&limiterBaselineResult.r1,&limiterBaselineResult.r2};
		const RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			stages64[3]={&limiterMirrorResult.r0,&limiterMirrorResult.r1,
				&limiterMirrorResult.r2};
		for(unsigned int stage=0u;stage<3u;++stage){
			limiterMirrorTargetDifference=std::max(limiterMirrorTargetDifference,
				targetDistance(stages32[stage]->target.TargetPerS(),
					stages64[stage]->target.TargetPerS()));
			compareLimiterMirror("stage_pressure",stages32[stage]->projection.pressurePa,
				stages64[stage]->projection.pressurePa);
			if(stages32[stage]->picardResidualPerS.size()!=
				stages64[stage]->picardResidualPerS.size())
				limiterMirrorResidualDifference=std::numeric_limits<double>::infinity();
			else for(std::size_t value=0u;
				value<stages32[stage]->picardResidualPerS.size();++value)
				limiterMirrorResidualDifference=std::max(limiterMirrorResidualDifference,
					std::fabs(static_cast<double>(stages32[stage]->picardResidualPerS[value])-
						stages64[stage]->picardResidualPerS[value]));
			for(unsigned int axis=0u;axis<3u;++axis){
				compareLimiterMirror("velocity",stages32[stage]->projection.velocityMPerS[axis],
					stages64[stage]->projection.velocityMPerS[axis]);
				compareLimiterMirror("momentum",stages32[stage]->projection.momentumKGPerM2S[axis],
					stages64[stage]->projection.momentumKGPerM2S[axis]);
				compareLimiterMirror("nonpressure",stages32[stage]->nonpressure.
					combinedMomentumRateKGPerM2S2[axis],stages64[stage]->nonpressure.
					combinedMomentumRateKGPerM2S2[axis]);
				compareLimiterMirror("alpha",stages32[stage]->scalarAcceptance.sharedFaceAlpha[axis],
					stages64[stage]->scalarAcceptance.sharedFaceAlpha[axis]);
			}
			compareLimiterFlux(stages32[stage]->flux.compositeFluxPair.lowFlux,
				stages64[stage]->flux.compositeFluxPair.lowFlux);
			compareLimiterFlux(stages32[stage]->flux.compositeFluxPair.fluxDelta,
				stages64[stage]->flux.compositeFluxPair.fluxDelta);
			compareEndpointPhysicalFlux(stages32[stage]->endpointPhysicalFlux,
				stages64[stage]->endpointPhysicalFlux);
		}
	}
	if(limiterBaselineOK&&limiterMirrorOK&&limiterFineOK&&limiterFine64OK){
		const RISE::FireProductionProjectedHeunCoupledStageResult* coarse32[3]={
			&limiterBaselineResult.r0,&limiterBaselineResult.r1,&limiterBaselineResult.r2};
		const RISE::FireProductionProjectedHeunCoupledStageResult* fine32[3]={
			&limiterFineResult.r0,&limiterFineResult.r1,&limiterFineResult.r2};
		const RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			coarse64[3]={&limiterMirrorResult.r0,&limiterMirrorResult.r1,
				&limiterMirrorResult.r2};
		const RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			fine64[3]={&limiterFineResult64.r0,&limiterFineResult64.r1,
				&limiterFineResult64.r2};
		for(unsigned int stage=0u;stage<3u;++stage){
			limiterProductionTargetRefinement=std::max(
				limiterProductionTargetRefinement,targetDistance(
					coarse32[stage]->target.TargetPerS(),fine32[stage]->target.TargetPerS()));
			limiterFP64TargetRefinement=std::max(limiterFP64TargetRefinement,
				targetDistance(coarse64[stage]->target.TargetPerS(),
					fine64[stage]->target.TargetPerS()));
			limiterFineTargetPrecision=std::max(limiterFineTargetPrecision,
				targetDistance(fine32[stage]->target.TargetPerS(),
					fine64[stage]->target.TargetPerS()));
		}
	}
	double limiterStateScale=1.0;
	for(const float value:limiterRequest.beginningConservativeValues)
		limiterStateScale=std::max(limiterStateScale,std::fabs(static_cast<double>(value)));
	const double limiterMirrorFluxBound=limiterStateScale*
		limiterRequest.scalarContract.shape.cellWidthM*
		limiterRequest.projectionTolerancePerS+128.0*
		std::numeric_limits<float>::epsilon()*limiterMirrorFluxScale;
	const double limiterTargetAdditiveBound=limiterProductionTargetRefinement+
		limiterFP64TargetRefinement+limiterFineTargetPrecision+128.0*
		std::numeric_limits<float>::epsilon()*limiterTargetScale;
	RISE::FireProductionProjectedHeunCPUOwner limiterDiscontinuousOwner;
	RISE::FireProductionProjectedHeunOwnerResult limiterDiscontinuousResult;
	const bool limiterDiscontinuousInjected=setOwnerFailure("limiter-discontinuity");
	const bool limiterDiscontinuousAcceptedCore=limiterBaselineOK&&
		limiterDiscontinuousInjected&&limiterDiscontinuousOwner.Begin(limiterRequest,&error)&&
		limiterDiscontinuousOwner.SolveR0(ownerTransport,&error)&&
		limiterDiscontinuousOwner.SolveR1(ownerTransport,&error)&&
		limiterDiscontinuousOwner.SolveR2(ownerTransport,limiterDiscontinuousResult,&error);
	if(!limiterDiscontinuousAcceptedCore)std::fprintf(stderr,
		"r190 limiter-discontinuity detail: %s\n",error.c_str());
	RISE::FireProductionScalarFCTRequest limiterCertifiedRequest=
		limiterRequest.scalarContract;
	limiterCertifiedRequest.beginning=limiterRequest.beginningConservativeValues;
	limiterCertifiedRequest.sourceDelta=limiterSource.SourceDelta();
	const bool limiterDiscontinuousAccepted=setOwnerFailure(0)&&
		limiterDiscontinuousAcceptedCore&&limiterMirrorOK&&limiterFineOK&&limiterFine64OK&&
		limiterMirrorMaximumDifference<=128.0*std::numeric_limits<float>::epsilon()&&
		limiterMirrorResidualDifference<=limiterRequest.projectionTolerancePerS&&
		limiterMirrorFluxDifference<=limiterMirrorFluxBound&&
		limiterMirrorEndpointMetadata&&
		limiterMirrorTargetDifference<=limiterTargetAdditiveBound&&
		limiterDiscontinuousResult.r1.limiterDiscontinuousClass&&
		limiterDiscontinuousResult.r1.scalarAcceptance.commutingIdentityAvailable&&
		IndependentR60AndPeriodicCommuting(limiterCertifiedRequest,
			limiterDiscontinuousResult.r1.scalarAcceptance)&&
		limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha!=
			limiterBaselineResult.r1.scalarAcceptance.sharedFaceAlpha&&
		limiterDiscontinuousResult.heunSolve.scalar.sharedFaceAlpha==
			limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha&&
		limiterDiscontinuousResult.heunSolve.scalar.accepted==
			limiterDiscontinuousResult.conservativeValues;
	if(limiterDiscontinuousAcceptedCore&&!limiterDiscontinuousAccepted)std::fprintf(stderr,
		"r190 limiter-discontinuity publication flags: class=%u commuting=%u alpha=%u state=%u r60=%u alpha_changed=%u mirror=%u mirror_max=%.17g\n",
		limiterDiscontinuousResult.r1.limiterDiscontinuousClass?1u:0u,
		limiterDiscontinuousResult.r1.scalarAcceptance.commutingIdentityAvailable?1u:0u,
		limiterDiscontinuousResult.heunSolve.scalar.sharedFaceAlpha==
			limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha?1u:0u,
		limiterDiscontinuousResult.heunSolve.scalar.accepted==
			limiterDiscontinuousResult.conservativeValues?1u:0u,
		IndependentR60AndPeriodicCommuting(limiterCertifiedRequest,
			limiterDiscontinuousResult.r1.scalarAcceptance)?1u:0u,
		limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha!=
			limiterBaselineResult.r1.scalarAcceptance.sharedFaceAlpha?1u:0u,
		limiterMirrorOK?1u:0u,limiterMirrorMaximumDifference);
	if(limiterDiscontinuousAcceptedCore&&!limiterDiscontinuousAccepted)
		std::fprintf(stderr,"r190 limiter mirror maximum field: %s residual_max=%.17g fp32=%.17g fp64=%.17g\n",
			limiterMirrorMaximumField,limiterMirrorResidualDifference,
			limiterMirrorMaximumFP32,limiterMirrorMaximumFP64);
	RISE::FireProductionProjectedHeunCPUOwner activeCycleOwner;
	RISE::FireProductionProjectedHeunOwnerResult activeCycleResult;
	const bool activeCycleInjected=setOwnerFailure("active-cycle");
	const bool activeCycleAcceptedCore=activeCycleInjected&&
		activeCycleOwner.Begin(differentialRequest,&error)&&
		activeCycleOwner.SolveR0(differentialTransport,&error)&&
		activeCycleOwner.SolveR1(differentialTransport,&error)&&
		activeCycleOwner.SolveR2(differentialTransport,activeCycleResult,&error);
	const bool activeCycleAccepted=setOwnerFailure(0)&&activeCycleAcceptedCore&&
		activeCycleResult.r0.activeSetDiscontinuousClass&&
		activeCycleResult.r0.activeSetCycleLength==2u&&
		activeCycleResult.r0.activeSetCanonicalProjectionCount==4u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			activeCycleResult,differentialSource);
	RISE::FireProductionProjectedHeunCPUOwner thirdClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult thirdClassResult;
	const bool thirdClassInjected=setOwnerFailure(
		"active-cycle,post-freeze-third-class-r0");
	const bool thirdClassAcceptedCore=thirdClassInjected&&
		thirdClassOwner.Begin(differentialRequest,&error)&&
		thirdClassOwner.SolveR0(differentialTransport,&error)&&
		thirdClassOwner.SolveR1(differentialTransport,&error)&&
		thirdClassOwner.SolveR2(differentialTransport,thirdClassResult,&error);
	const bool thirdClassAccepted=setOwnerFailure(0)&&thirdClassAcceptedCore&&
		thirdClassResult.r0.activeSetDiscontinuousClass&&
		thirdClassResult.r0.activeSetCycleLength>=3u&&
		thirdClassResult.r0.activeSetCanonicalProjectionCount>=5u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			thirdClassResult,differentialSource);
	RISE::FireProductionProjectedHeunCPUOwner selectedCycleValidationOwner;
	const bool selectedCycleValidationInjected=setOwnerFailure(
		"active-cycle,projection-validation-selected-cycle");
	const bool selectedCycleValidationRejectedCore=selectedCycleValidationInjected&&
		selectedCycleValidationOwner.Begin(differentialRequest,&error)&&
		!selectedCycleValidationOwner.SolveR0(differentialTransport,&error)&&
		error.find("canonical active class validation")!=std::string::npos;
	const bool selectedCycleValidationRejected=setOwnerFailure(0)&&
		selectedCycleValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2CycleValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2CycleValidationResult;
	const bool r2CycleValidationPrepared=
		r2CycleValidationOwner.Begin(differentialRequest,&error)&&
		r2CycleValidationOwner.SolveR0(differentialTransport,&error)&&
		r2CycleValidationOwner.SolveR1(differentialTransport,&error);
	const bool r2CycleValidationInjected=setOwnerFailure(
		"active-cycle-r2,projection-validation-selected-cycle");
	const bool r2CycleValidationRejectedCore=r2CycleValidationPrepared&&
		r2CycleValidationInjected&&
		!r2CycleValidationOwner.SolveR2(differentialTransport,
			r2CycleValidationResult,&error)&&
		error=="projected-Heun R2 canonical active class validation failed";
	const bool r2CycleValidationRejected=setOwnerFailure(0)&&
		r2CycleValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2ThirdClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2ThirdClassResult;
	const bool r2ThirdClassPrepared=r2ThirdClassOwner.Begin(differentialRequest,&error)&&
		r2ThirdClassOwner.SolveR0(differentialTransport,&error)&&
		r2ThirdClassOwner.SolveR1(differentialTransport,&error);
	const bool r2ThirdClassInjected=setOwnerFailure(
		"active-cycle-r2,post-freeze-third-class-r2");
	const bool r2ThirdClassAcceptedCore=r2ThirdClassPrepared&&r2ThirdClassInjected&&
		r2ThirdClassOwner.SolveR2(differentialTransport,r2ThirdClassResult,&error);
	const bool r2ThirdClassAccepted=setOwnerFailure(0)&&r2ThirdClassAcceptedCore&&
		r2ThirdClassResult.r2.activeSetDiscontinuousClass&&
		r2ThirdClassResult.r2.activeSetCycleLength>=3u&&
		r2ThirdClassResult.r2.activeSetCanonicalProjectionCount>=3u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			r2ThirdClassResult,differentialSource);
	if(!thirdClassAccepted||!r2ThirdClassAccepted)std::fprintf(stderr,
		"r190 post-freeze third-class RED: r0=%u core=%u cycle=%u count=%u r2=%u core=%u cycle=%u count=%u error=%s\n",
		thirdClassAccepted?1u:0u,thirdClassAcceptedCore?1u:0u,
		thirdClassResult.r0.activeSetCycleLength,
		thirdClassResult.r0.activeSetCanonicalProjectionCount,
		r2ThirdClassAccepted?1u:0u,r2ThirdClassAcceptedCore?1u:0u,
		r2ThirdClassResult.r2.activeSetCycleLength,
		r2ThirdClassResult.r2.activeSetCanonicalProjectionCount,error.c_str());
	if(!r2CycleValidationRejected)std::fprintf(stderr,
		"r190 R2 selected-cycle RED: prepared=%u injected=%u error=%s\n",
		r2CycleValidationPrepared?1u:0u,r2CycleValidationInjected?1u:0u,error.c_str());
	RISE::FireProductionProjectedHeunCPUOwner r2UsedClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2UsedClassResult;
	const bool r2UsedClassPrepared=r2UsedClassOwner.Begin(differentialRequest,&error)&&
		r2UsedClassOwner.SolveR0(differentialTransport,&error)&&
		r2UsedClassOwner.SolveR1(differentialTransport,&error);
	const bool r2UsedClassInjected=setOwnerFailure(
		"active-cycle-r2,r2-used-class-discrepancy");
	const bool r2UsedClassAcceptedCore=r2UsedClassPrepared&&r2UsedClassInjected&&
		r2UsedClassOwner.SolveR2(differentialTransport,r2UsedClassResult,&error);
	const float r2KnownUsedClassDiscrepancy=
		std::nextafter(0.25f,-std::numeric_limits<float>::infinity());
	const bool r2UsedClassAccepted=setOwnerFailure(0)&&r2UsedClassAcceptedCore&&
		r2UsedClassResult.r2.activeSetDiscontinuousClass&&
		r2UsedClassResult.r2.activeSetCycleLength>=2u&&
		r2UsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS>=
			r2KnownUsedClassDiscrepancy;
	if(!r2UsedClassAccepted)std::fprintf(stderr,
		"r190 R2 used-class RED: core=%u class=%u cycle=%u known=%.9g published=%.9g error=%s\n",
		r2UsedClassAcceptedCore?1u:0u,
		r2UsedClassResult.r2.activeSetDiscontinuousClass?1u:0u,
		r2UsedClassResult.r2.activeSetCycleLength,r2KnownUsedClassDiscrepancy,
		r2UsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS,
		error.c_str());
	RISE::FireProductionProjectedHeunCPUOwner r2BootstrapUsedClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2BootstrapUsedClassResult;
	const bool r2BootstrapUsedClassPrepared=
		r2BootstrapUsedClassOwner.Begin(differentialRequest,&error)&&
		r2BootstrapUsedClassOwner.SolveR0(differentialTransport,&error)&&
		r2BootstrapUsedClassOwner.SolveR1(differentialTransport,&error);
	const bool r2BootstrapUsedClassInjected=setOwnerFailure(
		"r2-bootstrap-used-class-discrepancy");
	const bool r2BootstrapUsedClassAcceptedCore=r2BootstrapUsedClassPrepared&&
		r2BootstrapUsedClassInjected&&r2BootstrapUsedClassOwner.SolveR2(
			differentialTransport,r2BootstrapUsedClassResult,&error);
	const bool r2BootstrapUsedClassAccepted=setOwnerFailure(0)&&
		r2BootstrapUsedClassAcceptedCore&&
		r2BootstrapUsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS>=
			r2KnownUsedClassDiscrepancy;
	if(!r2BootstrapUsedClassAccepted)std::fprintf(stderr,
		"r190 R2 bootstrap used-class RED: core=%u known=%.9g published=%.9g error=%s\n",
		r2BootstrapUsedClassAcceptedCore?1u:0u,r2KnownUsedClassDiscrepancy,
		r2BootstrapUsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS,
		error.c_str());
	RISE::FireProductionProjectedHeunCPUOwner biasedCycleOwner;
	RISE::FireProductionProjectedHeunOwnerResult biasedCycleResult;
	const bool biasedCycleInjected=setOwnerFailure(
		"active-cycle,active-cycle-selection-bias");
	const bool biasedCycleAcceptedCore=biasedCycleInjected&&
		biasedCycleOwner.Begin(differentialRequest,&error)&&
		biasedCycleOwner.SolveR0(differentialTransport,&error)&&
		biasedCycleOwner.SolveR1(differentialTransport,&error)&&
		biasedCycleOwner.SolveR2(differentialTransport,biasedCycleResult,&error);
	const bool biasedCycleAccepted=setOwnerFailure(0)&&biasedCycleAcceptedCore&&
		biasedCycleResult.r0.activeSetCanonicalProjectionCount==4u&&
		biasedCycleResult.r0.projection.pressureOpenInflow==
			differentialResult.r0.projection.pressureOpenInflow;
	if(!(activeCycleAccepted&&biasedCycleAccepted))std::fprintf(stderr,
		"r190 R0 cycle REDs: active=%u core=%u class=%u cycle=%u count=%u biased=%u core=%u count=%u\n",
		activeCycleAccepted?1u:0u,activeCycleAcceptedCore?1u:0u,
		activeCycleResult.r0.activeSetDiscontinuousClass?1u:0u,
		activeCycleResult.r0.activeSetCycleLength,
		activeCycleResult.r0.activeSetCanonicalProjectionCount,
		biasedCycleAccepted?1u:0u,biasedCycleAcceptedCore?1u:0u,
		biasedCycleResult.r0.activeSetCanonicalProjectionCount);
	const auto runDifferentialOwnerHook=[&](const char* hook,
		RISE::FireProductionProjectedHeunOwnerResult& hookResult){
		RISE::FireProductionProjectedHeunCPUOwner hookOwner;
		const bool injected=setOwnerFailure(hook);
		const bool core=injected&&hookOwner.Begin(differentialRequest,&error)&&
			hookOwner.SolveR0(differentialTransport,&error)&&
			hookOwner.SolveR1(differentialTransport,&error)&&
			hookOwner.SolveR2(differentialTransport,hookResult,&error);
		const bool reset=setOwnerFailure(0);
		return core&&reset&&RISE::FireProductionProjectedHeunOwnerResultMatches(
			hookResult,differentialSource);
	};
	RISE::FireProductionProjectedHeunOwnerResult terminalTransitionR0Result,
		terminalTransitionR1Result,terminalTransitionR2Result;
	const bool terminalTransitionR0=runDifferentialOwnerHook(
		"terminal-first-transition-r0",terminalTransitionR0Result)&&
		terminalTransitionR0Result.r0.acceptedIterationCount>
			differentialResult.r0.acceptedIterationCount&&
		!terminalTransitionR0Result.r0.activeSetDiscontinuousClass&&
		terminalTransitionR0Result.r0.activeSetCanonicalProjectionCount==0u;
	const bool terminalTransitionR1=runDifferentialOwnerHook(
		"terminal-first-transition-r1",terminalTransitionR1Result)&&
		terminalTransitionR1Result.r1.acceptedIterationCount>
			differentialResult.r1.acceptedIterationCount&&
		!terminalTransitionR1Result.r1.activeSetDiscontinuousClass&&
		terminalTransitionR1Result.r1.activeSetCanonicalProjectionCount==0u;
	const bool terminalTransitionR2=runDifferentialOwnerHook(
		"terminal-first-transition-r2",terminalTransitionR2Result)&&
		terminalTransitionR2Result.r2.acceptedIterationCount>
			differentialResult.r2.acceptedIterationCount&&
		!terminalTransitionR2Result.r2.activeSetDiscontinuousClass&&
		terminalTransitionR2Result.r2.activeSetCanonicalProjectionCount==0u;
	RISE::FireProductionProjectedHeunOwnerResult terminalWinnerR0Result,
		terminalWinnerR1Baseline,terminalWinnerR1Result,
		terminalWinnerR2Baseline,terminalWinnerR2Result;
	const bool terminalWinnerR0=runDifferentialOwnerHook(
		"active-cycle,terminal-canonical-winner-bias",terminalWinnerR0Result)&&
		terminalWinnerR0Result.r0.activeSetCanonicalProjectionCount>=2u&&
		terminalWinnerR0Result.r0.projection.pressureOpenInflow!=
			activeCycleResult.r0.projection.pressureOpenInflow;
	const bool terminalWinnerR1=runDifferentialOwnerHook(
		"active-cycle-r1",terminalWinnerR1Baseline)&&
		runDifferentialOwnerHook(
			"active-cycle-r1,terminal-canonical-winner-bias",terminalWinnerR1Result)&&
		terminalWinnerR1Result.r1.activeSetCanonicalProjectionCount>=2u&&
		terminalWinnerR1Result.r1.projection.pressureOpenInflow!=
			terminalWinnerR1Baseline.r1.projection.pressureOpenInflow;
	const bool terminalWinnerR2=runDifferentialOwnerHook(
		"active-cycle-r2",terminalWinnerR2Baseline)&&
		runDifferentialOwnerHook(
			"active-cycle-r2,terminal-canonical-winner-bias-r2",terminalWinnerR2Result)&&
		terminalWinnerR2Result.r2.activeSetCanonicalProjectionCount>=2u&&
		terminalWinnerR2Result.r2.projection.pressureOpenInflow!=
			terminalWinnerR2Baseline.r2.projection.pressureOpenInflow;
	if(!(terminalTransitionR0&&terminalTransitionR1&&terminalTransitionR2&&
		terminalWinnerR0&&terminalWinnerR1&&terminalWinnerR2))std::fprintf(stderr,
		"r190 terminal active-set REDs: transition=%u/%u/%u iterations=%u>%u,%u>%u,%u>%u class=%u/%u/%u counts=%u/%u/%u winners=%u/%u/%u counts=%u/%u/%u\n",
		terminalTransitionR0?1u:0u,terminalTransitionR1?1u:0u,
		terminalTransitionR2?1u:0u,terminalTransitionR0Result.r0.acceptedIterationCount,
		differentialResult.r0.acceptedIterationCount,
		terminalTransitionR1Result.r1.acceptedIterationCount,
		differentialResult.r1.acceptedIterationCount,
		terminalTransitionR2Result.r2.acceptedIterationCount,
		differentialResult.r2.acceptedIterationCount,
		terminalTransitionR0Result.r0.activeSetDiscontinuousClass?1u:0u,
		terminalTransitionR1Result.r1.activeSetDiscontinuousClass?1u:0u,
		terminalTransitionR2Result.r2.activeSetDiscontinuousClass?1u:0u,
		terminalTransitionR0Result.r0.activeSetCanonicalProjectionCount,
		terminalTransitionR1Result.r1.activeSetCanonicalProjectionCount,
		terminalTransitionR2Result.r2.activeSetCanonicalProjectionCount,
		terminalWinnerR0?1u:0u,terminalWinnerR1?1u:0u,terminalWinnerR2?1u:0u,
		terminalWinnerR0Result.r0.activeSetCanonicalProjectionCount,
		terminalWinnerR1Result.r1.activeSetCanonicalProjectionCount,
		terminalWinnerR2Result.r2.activeSetCanonicalProjectionCount);
	RISE::FireProductionProjectedHeunCPUOwner staleCorrectionOwner;
	RISE::FireProductionProjectedHeunOwnerResult staleCorrectionResult;
	const bool staleCorrectionInjected=setOwnerFailure("stale-candidate");
	const bool staleCorrectionRejectedCore=staleCorrectionInjected&&
		staleCorrectionOwner.Begin(ownerRequest,&error)&&
		!staleCorrectionOwner.SolveR0(ownerTransport,&error)&&
		error=="projected-Heun r70 candidate lineage is invalid";
	const bool staleCorrectionReset=setOwnerFailure(0);
	const bool staleCorrectionRejected=staleCorrectionReset&&
		staleCorrectionRejectedCore&&
		staleCorrectionOwner.SolveR0(ownerTransport,&error)&&
		staleCorrectionOwner.SolveR1(ownerTransport,&error)&&
		staleCorrectionOwner.SolveR2(ownerTransport,staleCorrectionResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			staleCorrectionResult,ownerSource);
	RISE::FireProductionProjectedHeunCPUOwner outerMutationOwner;
	RISE::FireProductionProjectedHeunOwnerResult outerMutationResult;
	const bool outerMutationPrepared=outerMutationOwner.Begin(ownerRequest,&error)&&
		outerMutationOwner.SolveR0(ownerTransport,&error)&&
		outerMutationOwner.SolveR1(ownerTransport,&error);
	MutatingOuterResultProjectedHeunTransport32 outerMutationTransport(
		outerMutationResult,ownerResult);
	const bool outerMutationRejected=outerMutationPrepared&&
		!outerMutationOwner.SolveR2(outerMutationTransport,outerMutationResult,&error)&&
		outerMutationTransport.Mutated()&&ownerResultDefault(outerMutationResult);
	const bool outerMutationRetry=outerMutationRejected&&
		outerMutationOwner.SolveR2(ownerTransport,outerMutationResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			outerMutationResult,ownerSource);
	RISE::FireProductionProjectedHeunCPUOwner atomicOwner;
	RISE::FireProductionProjectedHeunOwnerResult atomicResult=ownerResult;
	const bool atomicPrepared=atomicOwner.Begin(ownerRequest,&error)&&
		atomicOwner.SolveR0(ownerTransport,&error)&&atomicOwner.SolveR1(ownerTransport,&error);
	const bool atomicInjected=setOwnerFailure("result-copy-mid");
	const bool atomicFailure=atomicPrepared&&atomicInjected&&
		!atomicOwner.SolveR2(ownerTransport,atomicResult,&error)&&
		error=="projected-Heun R2 allocation failed"&&ownerResultDefault(atomicResult);
	const bool atomicReset=setOwnerFailure(0);
	const bool atomicRetry=atomicFailure&&atomicOwner.SolveR2(ownerTransport,atomicResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(atomicResult,ownerSource);
	Check(outOfOrderRejected&&staleCandidateRejected&&forgedLineageRejected&&
		replayedPayloadRejected&&staleR2CandidateRejected&&forgedR2LineageRejected&&
		replayedR2PayloadRejected&&reentrantRetryable&&staleCorrectionRejected&&
		outerMutationRetry&&atomicReset&&atomicRetry&&mutatingTransportMatrix&&
		!std::is_aggregate<RISE::FireProductionProjectedHeunCPUOwner>::value,
		"r190 owner refuses stale payload/lineage and callback mutation, blocks reentry, preserves order, and retries atomically");
	Check(initialValidationRejected&&iterativeValidationRejected&&
		terminalValidationRejected&&r2ValidationRejected&&r2TerminalValidationRejected&&
		r2BootstrapRetry&&
		selectedCycleValidationRejected&&r2CycleValidationRejected&&
		workingSetAdmissionRejected,
		"r190 owner behaviorally refuses every projection-validation path and its live-set preflight");
	Check(limiterDiscontinuousAccepted,
		"r190 nonuniform owner revalidates the r59 selected alpha through r60 and compatible commuting identity");
	Check(activeCycleAccepted&&thirdClassAccepted&&r2ThirdClassAccepted&&
		biasedCycleAccepted&&terminalTransitionR0&&
		terminalTransitionR1&&terminalTransitionR2&&terminalWinnerR0&&
		terminalWinnerR1&&terminalWinnerR2,
		"r190 owner continues first terminal class transitions and reselects every cycle branch at the accepted target");
	Check(r2UsedClassAccepted&&r2BootstrapUsedClassAccepted,
		"r190 R2 trajectory scores bootstrap and iterative flips against the classes actually projected");
	const std::string projectedHeunOwnerEvidence=ReadText(
		"rendered/fire_production_calibration/r190_projected_heun_owner/"
		"projected_heun_owner_evidence.v1");
	const std::string projectedHeunMetalManifest=ReadText(
		"rendered/fire_production_calibration/r190_projected_heun_owner/"
		"metal_host_manifest.v2");
	const std::string projectedHeunR136Repin=ReadText(
		"rendered/fire_production_calibration/r190_projected_heun_owner/"
		"r136_trace_repin_evidence.v1");
	std::vector<std::string> manifestRecords;
	{
		std::istringstream records(projectedHeunMetalManifest);std::string record;
		while(std::getline(records,record))manifestRecords.push_back(record);
	}
	const auto recordIndex=[&](const std::string& record,const std::size_t start){
		if(start>=manifestRecords.size())return manifestRecords.size();
		const auto found=std::find(manifestRecords.begin()+start,
			manifestRecords.end(),record);
		return found==manifestRecords.end()?manifestRecords.size():
			static_cast<std::size_t>(found-manifestRecords.begin());
	};
	const auto recordCount=[&](const std::string& record){
		return static_cast<std::size_t>(std::count(manifestRecords.begin(),
			manifestRecords.end(),record));
	};
	const auto manifestStage=[&](const std::string& header){
		const std::size_t begin=recordIndex(header,0u);
		const std::size_t end=recordIndex("stage_end",begin+1u);
		if(begin==manifestRecords.size()||end==manifestRecords.size())
			return std::vector<std::string>();
		return std::vector<std::string>(manifestRecords.begin()+begin,
			manifestRecords.begin()+end+1u);
	};
	const std::string stage1Header=
		"stage 1 track_a_tier8_full_window_spectrum_animation_delivery";
	const std::string stage2Header=
		"stage 2 track_b_live_metal_consumers_and_kernel_sweep";
	const std::string stage3Header="stage 3 track_b_tier10_onset";
	const std::string stage4Header="stage 4 regression_rederivation";
	const std::string stage5Header=
		"stage 5 tier10_full_window_spectrum_rows_animation_report";
	const std::vector<std::string> tier8MovieStage=manifestStage(stage1Header);
	const std::vector<std::string> tier10MovieStage=manifestStage(stage5Header);
	const auto movieStageComplete=[](const std::vector<std::string>& stage,
		const char* tier,
		const char* movie){
		const std::string movieName(movie);
		const std::string base=std::string(
			"rendered/fire_production_calibration/r190_tier")+tier+"_temporal";
		const std::string provenancePrefix=std::string(
			"rendered/fire_production_calibration/r190_tier")+tier+
			"_temporal/preview/temporal_primary_*.exr.provenance.cbor";
		const std::string contiguousDigestCommand="  command /usr/bin/shasum -a 256 "+
			movieName+" "+provenancePrefix+" > "+movieName+".evidence.sha256";
		const std::array<std::string,6> commands={{
			"  command ./bin/tests/FireSequenceTest --fire-production-temporal-capstone "+
				std::string(tier)+" 0.0625 "+base,
			"  command ./bin/tests/FireSequenceTest --fire-production-temporal-preview "+
				base+" "+base+"/preview",
			"  command python3 tools/encode_pq_prores.py "+base+
				"/preview/temporal_primary_*.exr | ffmpeg -y -f rawvideo -pix_fmt "
				"yuv444p10le -s 64x64 -r 16 -i - -vf \"setparams=color_primaries="
				"bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv\" -c:v "
				"prores_ks -profile:v 4444 -movflags +write_colr "+movieName,
			"  command ffprobe -v trace "+movieName+
				" 2>&1 | grep \"nclc: pri 9 trc 16 matrix 9\"",
			contiguousDigestCommand,
			"  command ./bin/tests/FireSequenceTest --fire-production-puffing-spectrum "+
				std::string(tier)+" "+base+"/production_final.checkpoint "+base+" "+
				base+"/preview "+base+"/spectrum"}};
		return !stage.empty()&&std::all_of(commands.begin(),commands.end(),
			[&](const std::string& command){return std::find(stage.begin(),stage.end(),
				command)!=stage.end();});
	};
	const bool tier8MovieStageComplete=movieStageComplete(tier8MovieStage,"8",
		"rendered/fire_production_calibration/r190_tier8_temporal/"
		"tier8_preview_hdr10_prores4444.mov");
	const bool tier10MovieStageComplete=movieStageComplete(tier10MovieStage,"10",
		"rendered/fire_production_calibration/r190_tier10_temporal/"
		"tier10_preview_hdr10_prores4444.mov");
	const std::size_t manifestStage1=recordIndex(stage1Header,0u);
	const std::size_t manifestStage2=recordIndex(stage2Header,0u);
	const std::size_t manifestStage3=recordIndex(stage3Header,0u);
	const std::size_t manifestStage4=recordIndex(stage4Header,0u);
	const std::size_t manifestStage5=recordIndex(stage5Header,0u);
	const auto stageEnd=[&](const std::size_t stage){
		return recordIndex("stage_end",stage<manifestRecords.size()?stage+1u:stage);
	};
	const std::size_t manifestStageEnd1=stageEnd(manifestStage1);
	const std::size_t manifestStageEnd2=stageEnd(manifestStage2);
	const std::size_t manifestStageEnd3=stageEnd(manifestStage3);
	const std::size_t manifestStageEnd4=stageEnd(manifestStage4);
	const std::size_t manifestStageEnd5=stageEnd(manifestStage5);
	const bool manifestTier8DeliveryStageLocal=std::find(tier8MovieStage.begin(),
		tier8MovieStage.end(),
		"  delivery send movie and preview_primary sidecars to owner immediately")!=
		tier8MovieStage.end();
	const bool manifestScopeAndOrder=
		recordCount("current_host_has_required_device 0")==1u&&
		recordCount("run_stages_executed_on_current_host 0")==1u&&
		recordCount(stage1Header)==1u&&recordCount(stage2Header)==1u&&
		recordCount(stage3Header)==1u&&recordCount(stage4Header)==1u&&
		recordCount(stage5Header)==1u&&recordCount("stage_end")==5u&&
		manifestStage1<manifestStageEnd1&&manifestStageEnd1<manifestStage2&&
		manifestStage2<manifestStageEnd2&&manifestStageEnd2<manifestStage3&&
		manifestStage3<manifestStageEnd3&&manifestStageEnd3<manifestStage4&&
		manifestStage4<manifestStageEnd4&&manifestStageEnd4<manifestStage5&&
		manifestStage5<manifestStageEnd5&&
		manifestTier8DeliveryStageLocal;
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		projectedHeunOwnerEvidence.begin(),projectedHeunOwnerEvidence.end()))==
			"80e08c2a58ecbbca03892ae067bf4637527e56eafe9328a137f7f1aac53324f8"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunMetalManifest.begin(),projectedHeunMetalManifest.end()))==
			"c9c48a6eb64d195235f59bb73783296f54f61507f2fb6271280da07d07433f5c"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectedHeunR136Repin.begin(),projectedHeunR136Repin.end()))==
			"f5f4ddc01cc0c6f2b3d1c3e2a6c66fd11c92d1e274295428b512dd40ba3e895e"&&
		projectedHeunOwnerEvidence.find("r70_authority public false\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find("red_stale_projection_identity true\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"result_payload_verifier full_publication_v5\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"result_payload_identity_domain v5_source_hygiene_bound\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"result_acceptance_seal private_owner_minted_read_only\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"result_stage_callback_reentry in_progress_state_refusal_with_raii_rollback\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"transport_callback_context_postcondition exact_scalar_pointer_and_payload_bits_unchanged_on_success_and_refusal\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"working_set_exact_formula_bytes (274C_plus_97F)*sizeof(float)\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_stale_whole_result_source_attempt true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_oversized_combined_live_set_begin_refusal true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_projection_validation_r2_endpoint true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_projection_validation_r2_terminal true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_terminal_accepted_target_winner_change_r0_r1_r2 true\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_transport_context_mutation_r0_r1_r2_payload_scalar_identity_pointer_rebinding_success_and_refusal_60_cases true\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_active_set_post_freeze_third_class_r0_ordinary_collector true\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"r189_transport_source_changed true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_r59_independent_r60_and_commuting_recompute true\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_spectrum_tier_exact_record_line true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_manifest_tier8_delivery_stage_local true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"r136_live_trace_digest 7736ec4adb7fd3b0cb3c1bf7bddd5b1d2a050e7e0fbd56bc31e928b7f9faa22d\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_r2_bootstrap_used_class_flip_discrepancy true\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_reentrant_transport_nested_r0_r1_r2 true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_result_complete_default_exhaustive true\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"metal_spectrum_tier explicit_6_8_10_validated_against_checkpoint_grid\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_spectrum_tier_serialized_evidence_8_and_10 true\n")!=
			std::string::npos&&
		projectedHeunOwnerEvidence.find(
			"red_manifest_movie_pipeline_stage_local_tier8_and_tier10 true\n")!=
			std::string::npos&&
		projectedHeunR136Repin.find(
			"value_changed_manifest_fields FireProductionTransportHeader,FireProductionForceHeader,FireProductionForceSource,TraceAdapter\n")!=
			std::string::npos&&
		projectedHeunR136Repin.find(
			"newly_consumed_manifest_fields FireSimulationRecordsHeader,FireSimulationRecordsSource,FireCaseHeader,FireCaseSource\n")!=
			std::string::npos&&
		projectedHeunR136Repin.find(
			"historical_exit_code 237\n")!=std::string::npos&&
		projectedHeunR136Repin.find(
			"current_exit_code 237\n")!=std::string::npos&&
		projectedHeunR136Repin.find(
			"branch_obligations_discharged 3972326\n")!=std::string::npos&&
		tier8MovieStageComplete&&tier10MovieStageComplete&&manifestScopeAndOrder&&
		projectedHeunMetalManifest.find(
			"command ./bin/tests/FireProductionProjectionTest\n")!=std::string::npos&&
		projectedHeunMetalManifest.find(
			"command ./bin/tests/FireProductionSolverTest\n")!=std::string::npos&&
		projectedHeunOwnerEvidence.find("metal_run_claim false\n")!=std::string::npos,
		"r190 evidence seals the private authority, RED battery, and honest Metal scope");
	RISE::FireSim::PersistentFireWorkerPool exceptionPool;
	const unsigned int exceptionWorkers=std::min(2u,RISE::FireSim::FireWorkerCapacity());
	bool workerExceptionPropagated=false;
	try {
		exceptionPool.Run(exceptionWorkers,[](const unsigned int worker){
			if(worker==0u)throw std::runtime_error("injected worker failure");
		});
	} catch(const std::runtime_error&) { workerExceptionPropagated=true; }
	std::atomic<unsigned int> recoveredWorkers(0u);
	exceptionPool.Run(exceptionWorkers,[&](const unsigned int){++recoveredWorkers;});
	Check(workerExceptionPropagated&&recoveredWorkers.load()==exceptionWorkers,
		"persistent fire worker pool propagates task failure and remains reusable after its barrier");
	const std::string sourceAuthorityImplementation=ReadText(
		"src/Library/Utilities/FireProductionSource.cpp");
	const std::string sourceKernelImplementation=ReadText(
		"src/Library/Utilities/FireProductionSourceKernel.h");
	const std::string toolCoreSource=ReadText("tools/fire_simulator_core.h");
	Check(CountText(sourceAuthorityImplementation,
		"FireProductionCanonicalSourceAuthority::Build")==1u&&
		CountText(sourceKernelImplementation,"BuildFrozenMethaneSourcePackets(")==1u&&
		CountText(toolCoreSource,"FireProductionCanonicalSourceAuthority::Build")==0u&&
		CountText(toolCoreSource,"BuildFrozenMethaneSourcePackets(")==0u,
		"compiled production source owns the sole source authority and shared canonical grid kernel");
	const std::string canonicalSourceAuthorityEvidence=ReadText(
		"rendered/fire_production_calibration/r186_canonical_source_authority/"
		"canonical_source_authority_evidence.v1");
	const std::string canonicalSourceAuthorityLiveBinding=ReadText(
		"rendered/fire_production_calibration/r186_canonical_source_authority/"
		"canonical_source_authority_live_binding.v1");
	const auto canonicalSourceOwnerBound=[&](const char* path,const char* sha256) {
		const std::string current=sourceSHA(path);
		const bool bound=canonicalSourceAuthorityLiveBinding.find(std::string("owner ")+path+
			" sha256 "+sha256+"\n")!=std::string::npos&&(current==sha256||
			projectedHeunOwnerLiveBinding.find(std::string("owner ")+path+" sha256 "+
				current+"\n")!=std::string::npos||metalContextLiveBinding.find(
				std::string("owner ")+path+" sha256 "+current+"\n")!=
				std::string::npos||residentEOSCandidateLiveBinding.find(
					std::string("owner ")+path+" sha256 "+current+"\n")!=
					std::string::npos||residentTargetLiveBinding.find(
						std::string("owner ")+path+" sha256 "+current+"\n")!=
						std::string::npos||projectedHeunMetalOwnerLiveBinding.find(
						std::string("owner ")+path+" sha256 "+current+"\n")!=
							std::string::npos||projectedHeunIterationTraceLiveBinding.find(
								std::string("owner ")+path+" sha256 "+current+"\n")!=
					std::string::npos||projectedHeunResidentParentLiveBinding.find(
						std::string("owner ")+path+" sha256 "+current+"\n")!=
					std::string::npos||projectedHeunActualCandidateLiveBinding.find(
						std::string("owner ")+path+" sha256 "+current+"\n")!=
						std::string::npos||projectedHeunFullFieldResidencyLiveBinding.find(
							std::string("owner ")+path+" sha256 "+current+"\n")!=
							std::string::npos||projectedHeunSetupStagedSourceLiveBinding.find(
								std::string("owner ")+path+" sha256 "+current+"\n")!=
								std::string::npos||projectedHeunTraceVerdictLiveBinding.find(
									std::string("owner ")+path+" sha256 "+current+"\n")!=
									std::string::npos||ownerCostLiveBinding.find(
									std::string("owner ")+path+" sha256 "+current+"\n")!=
									std::string::npos);
		if(!bound)std::fprintf(stderr,"UNBOUND_R186_OWNER %s current=%s\n",path,
			current.c_str());
		return bound;
	};
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
		canonicalSourceAuthorityEvidence.begin(),canonicalSourceAuthorityEvidence.end()))==
		"248e8da89dac1f5754d9a352ad15d43fd9726e237eb1e994f0c7d2fdad5a135a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			canonicalSourceAuthorityLiveBinding.begin(),
			canonicalSourceAuthorityLiveBinding.end()))==
			"787d88212f6f367aceb962995c7c48a94892410b5e1aa485a6fd086d906ab097"&&
		canonicalSourceAuthorityLiveBinding.find("live_owner_count 22\n")!=
			std::string::npos&&
		canonicalSourceOwnerBound("src/Library/Utilities/FireProductionSourceKernel.h",
			"17ba9d611f4f391e57884e47b4a88d4c728aefa947d66e01314ed43ce2aa9274")&&
		canonicalSourceOwnerBound("src/Library/Utilities/FireProductionSource.cpp",
			"112020e369f5b67e7dba98d87c5caf10bca10b1b6daf7694489c76602918b6f6")&&
		canonicalSourceOwnerBound("src/Library/Utilities/FireProductionTransport.h",
			"258b92cf142d609c9247942906710233cc52921795cc264f3efc8e4c47ce669e")&&
		canonicalSourceOwnerBound("src/Library/Utilities/FireProductionTransport.cpp",
			"7788648726d46b23317355d819e545770745647e97afae8df212ca0a52124d22")&&
		canonicalSourceOwnerBound("src/Library/Utilities/FireSimulationRecords.h",
			"8e81fff299ee02af6cec1e9c3a117e19936492ae470c495bd6877cfb006e28dd")&&
		canonicalSourceOwnerBound("src/Library/Utilities/FireSimulationRecords.cpp",
			"67b0bf8d90f79e733c04da1562a5c7e427cf6fa9d4ab0042c5c4308efdf4aaab")&&
		canonicalSourceOwnerBound("tools/fire_simulator_core.h",
			"21a5cf2c6177cc535d9ff8ced6835a7b0b09b2fe19dfed887c76d7e89c61eae1")&&
		canonicalSourceOwnerBound("build/make/rise/Filelist",
			"8517dff07dee064c6e682c1f53b7bf8ae9eae70ef2d31d87309cefd3e80617a4")&&
		canonicalSourceOwnerBound("build/make/rise/Makefile",
			"b36ad42da2bf5ef25a454ad2d84dd5c034bfb581459379cd0293c9debfd4ce65")&&
		canonicalSourceOwnerBound("build/cmake/rise-android/CMakeLists.txt",
			"0b0bac8387f25bf4157a3f5b50f978ead49031e685368628949711ddb68419f9")&&
		canonicalSourceOwnerBound("build/cmake/rise-android/rise_sources.cmake",
			"83789553843a1d84aa1cdae132354de03c9b9615fc052700e9d1050dfde7b5b6")&&
		canonicalSourceOwnerBound("build/VS2022/Library/Library.vcxproj",
			"c5190cc329a33bd25751752a68a2cd057be460fc9122a55155f2d7be511321d1")&&
		canonicalSourceOwnerBound("build/VS2022/Library/Library.vcxproj.filters",
			"9c76ca266ee430294234a0dea1c6808102efc9cf7210132b0c43553168703114")&&
		canonicalSourceOwnerBound("build/XCode/rise/rise.xcodeproj/project.pbxproj",
			"c9c0439a88b23d23fb08d696d8ad79353fe8e48ce21a25ede8239b390ca7fefa")&&
		canonicalSourceOwnerBound("tests/fire_production_fp64/FireProductionTransport.h",
			"154a41bdb4a253e7273044ef3f05dddbb4a04ed4264e26511ea24d1798408899")&&
		canonicalSourceOwnerBound("tests/fire_production_fp64/FireProductionTransport.cpp",
			"b5caa87c1d3548093f1f1904304bd9dbbc579a4a02f4c2b1dea316f9ccf4c438")&&
		canonicalSourceOwnerBound("tests/fire_production_fp64/SourceManifest.h",
			"0338dd6c7b55f906b46e25bf07aef4010af9ed20e88e6980e87d8955fc3dece5")&&
		canonicalSourceOwnerBound("tests/fire_production_trace/FireProductionTransport.h",
			"f3eac8c0c710fbb2eb251322a0a346a1081b7696da6f09e12f6fb490ae919a7e")&&
		canonicalSourceOwnerBound("tests/fire_production_trace/FireProductionTransport.cpp",
			"a30c764dcc20386eff7ff9303dda9fab2de94df0aebaf1ef0d38e1118bf77b23")&&
		canonicalSourceOwnerBound("tests/fire_production_trace/SourceManifest.h",
			"27e19d81e934515477bf14c0e3608a7804a371b083adb21729c020e4d379a33f")&&
		canonicalSourceOwnerBound("docs/FIRE_SMOKE_PRODUCTION_SOLVER.md",
			"4031a1705f304dd504831f21c7ba631cbae7896a0f6026e2c8e9606b42de552f")&&
		canonicalSourceOwnerBound("docs/FIRE_SMOKE_DESIGN_HISTORY.md",
			"783f705f095d34c0bd1e8ff5b5c9aaae6d24f0aa9e40cb1c531df7e20ceb7ed1")&&
		canonicalSourceAuthorityEvidence.find(
			"verdict accepted_cpu_prerequisite\n")!=std::string::npos&&
		canonicalSourceAuthorityEvidence.find(
			"rejected_raw_validator true\n")!=std::string::npos&&
		canonicalSourceAuthorityEvidence.find(
			"rejected_inline_tool_friend_authority true\n")!=std::string::npos&&
		canonicalSourceAuthorityEvidence.find(
			"serial_parallel_packet_bytes_identical true\n")!=std::string::npos&&
		canonicalSourceAuthorityEvidence.find("tier10_claim false\n")!=std::string::npos,
		"r186 preserves its historical source-authority binding while r190 binds every changed live consumer");
	const std::string packetDivergenceEvidence=ReadText(
		"rendered/fire_production_calibration/r187_packet_divergence_target/"
		"packet_divergence_target_evidence.v1");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			packetDivergenceEvidence.begin(),packetDivergenceEvidence.end()))==
		"9a04c48b31ab5646724393e94de01b4569208e98c75ff9e353c4c3362ccdcc04"&&
		packetDivergenceEvidence.find("caller_authored_divergence_target false\n")!=
			std::string::npos&&
		packetDivergenceEvidence.find(
			"target_RED reconstruct_packet_and_require_bit_identity\n")!=std::string::npos&&
		packetDivergenceEvidence.find("projection_consumer false\n")!=std::string::npos&&
		packetDivergenceEvidence.find("tier10_claim false\n")!=std::string::npos,
		"r187 publishes packet-derived divergence targets without claiming projection consumption");
	const std::string baseDivergenceTargetEvidence=ReadText(
		"rendered/fire_production_calibration/r188_base_divergence_target/"
		"base_divergence_target_evidence.v1");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			baseDivergenceTargetEvidence.begin(),baseDivergenceTargetEvidence.end()))==
			"3ce69ec82d30acf1c244dbdfa150559dae96f96e725abd16636b66ee5871ebb0"&&
		baseDivergenceTargetEvidence.find(
			"formula dV(Q_stage)[D(f_N_stage)]+S_div_source(Q_n,Delta_Q_source)\n")!=
			std::string::npos&&
		baseDivergenceTargetEvidence.find("distinct_R1_stage_RED true\n")!=
			std::string::npos&&
		baseDivergenceTargetEvidence.find("zero_source_absolute_branch_RED true\n")!=
			std::string::npos&&
		baseDivergenceTargetEvidence.find("projection_consumer false\n")!=
			std::string::npos&&
		baseDivergenceTargetEvidence.find("tier10_claim false\n")!=std::string::npos,
		"r188 binds the tangent-plus-frozen-source derivation and honest prerequisite scope");
	const std::string authenticatedProjectionTargetEvidence=ReadText(
		"rendered/fire_production_calibration/r189_authenticated_projection_target/"
		"authenticated_projection_target_evidence.v1");
	const std::string metalHostManifest=ReadText(
		"rendered/fire_production_calibration/r189_metal_host_manifest/"
		"metal_host_manifest.v1");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			authenticatedProjectionTargetEvidence.begin(),
			authenticatedProjectionTargetEvidence.end()))==
			"743ba191756f03fd97a52f4513beb5d908bbbdb34fb3508ca7707fd936ce473a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			metalHostManifest.begin(),metalHostManifest.end()))==
			"c5dcaacd6c5f858fbaefaaa611c02fe213481a2dbde0ba45355c67b3a7ee42d5"&&
		authenticatedProjectionTargetEvidence.find(
			"authenticated_consumer_preauthored_target accepted_false\n")!=std::string::npos&&
		authenticatedProjectionTargetEvidence.find(
			"valid_different_topology_RED true\n")!=std::string::npos&&
		authenticatedProjectionTargetEvidence.find(
			"r70_correction false\n")!=std::string::npos&&
		authenticatedProjectionTargetEvidence.find(
			"terminal_Picard_acceptance false\n")!=std::string::npos&&
		authenticatedProjectionTargetEvidence.find(
			"metal_run_executed false\n")!=std::string::npos&&
		metalHostManifest.find("stage 1 track_a_tier8_full_window_and_movie\n")!=
			std::string::npos&&
		metalHostManifest.find("track_a_precedes_track_b_metal 1\n")!=
			std::string::npos&&
		metalHostManifest.find("current_host_has_required_device 0\n")!=
			std::string::npos,
		"r189 binds the CPU projection prerequisite and tier-8-first Metal-host manifest");
	const std::string metalContextEvidence=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"metal_context_evidence.v1");
	const std::string correctedMetalManifest=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"metal_host_manifest.v3");
	const std::string metalContextProbe=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"metal_context_probe_source.v1.mm");
	const std::string rawProbeSandbox=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"raw_probe_sandbox.transcript.v1");
	const std::string rawProbeUnrestricted=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"raw_probe_unrestricted.transcript.v1");
	const std::string capabilitySandbox=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"capability_sandbox.transcript.v1");
	const std::string capabilityUnrestricted=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"capability_unrestricted.transcript.v1");
	const std::string kernelSweep=ReadText(
		"rendered/fire_production_calibration/r191_metal_context_reclassification/"
		"kernel_sweep.transcript.v1");
	const std::size_t correctedStage1=correctedMetalManifest.find(
		"stage 1 track_a_tier8_full_window_spectrum_animation_delivery\n");
	const std::size_t correctedStage2=correctedMetalManifest.find(
		"stage 2 track_b_live_metal_consumers_and_kernel_sweep\n");
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			metalContextEvidence.begin(),metalContextEvidence.end()))==
			"3e95fb0a3de25997f96e6ec0abe137940aabca4378481f2c9f95d5d319e65a89"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			correctedMetalManifest.begin(),correctedMetalManifest.end()))==
			"109a18a5e7803679bf35acf5adb3b34d89fb3548a549889f043414937d1e2763"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			metalContextProbe.begin(),metalContextProbe.end()))==
			"b3cf419c987c82de631e7bc8b19f44619dd26780ce7cb32e95f4b9afb51e3a54"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			rawProbeSandbox.begin(),rawProbeSandbox.end()))==
			"f0b40293d5d5bb5378ade367ba65f184fa3715fb3be93855070c46c8f646998d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			rawProbeUnrestricted.begin(),rawProbeUnrestricted.end()))==
			"45caefa25c676f36f07aba163082431ade167e76972d022742f9c9f5b080efaa"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			capabilitySandbox.begin(),capabilitySandbox.end()))==
			"857e8e648da0db8c801ea21ecada78827368257a49a48d0f7c46d1e2be40f9dd"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			capabilityUnrestricted.begin(),capabilityUnrestricted.end()))==
			"ac2652e894df2505341e72dc321cba5c69e91810f3e7712140ace61516701480"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			kernelSweep.begin(),kernelSweep.end()))==
			"24d7c95b90887ce1b3c9ef229b7dbc3832bfbc262c9bb674e13152090737c011"&&
		metalContextEvidence.find(
			"finding workspace_sandbox_execution_context_blocked_Metal_discovery\n")!=
			std::string::npos&&
		metalContextEvidence.find("workspace_sandbox_enumerated_count 0\n")!=
			std::string::npos&&
		metalContextEvidence.find("unrestricted_enumerated_count 1\n")!=
			std::string::npos&&
		correctedMetalManifest.find("current_host_has_required_device 1\n")!=
			std::string::npos&&
		correctedMetalManifest.find("execution_context unrestricted\n")!=
			std::string::npos&&
		correctedMetalManifest.find("supersedes_manifest_sha256 "
			"c9c48a6eb64d195235f59bb73783296f54f61507f2fb6271280da07d07433f5c\n")!=
			std::string::npos&&correctedStage1!=std::string::npos&&
		correctedStage2!=std::string::npos&&correctedStage1<correctedStage2&&
		correctedMetalManifest.find(
			"command ./bin/tests/FireSequenceTest --fire-production-metal-fp64-kernel-sweep\n")!=
			std::string::npos&&
		rawProbeSandbox.find("executable_sha256 "
			"4650040b2451e09e5f5f80b828e9ee0162bc855336a2ae0faa6a9ae349343cde\n")!=
			std::string::npos&&rawProbeUnrestricted.find("executable_sha256 "
			"4650040b2451e09e5f5f80b828e9ee0162bc855336a2ae0faa6a9ae349343cde\n")!=
			std::string::npos&&sourceSHA("rendered/fire_production_calibration/"
			"r191_metal_context_reclassification/metal_context_probe.v1")==
			"4650040b2451e09e5f5f80b828e9ee0162bc855336a2ae0faa6a9ae349343cde"&&
		rawProbeSandbox.find("exit 0\ndefault_present 0\nall_device_count 0\n")!=
			std::string::npos&&capabilitySandbox.find("exit 86\n")!=std::string::npos&&
		kernelSweep.find("classification pre_manifest_development_qualification_not_"
			"stage_2_execution\n")!=std::string::npos&&
		kernelSweep.find("fp64_rounded_exact=1 fp64_mismatches=0")!=std::string::npos&&
		kernelSweep.find("SCALAR_FCT_METAL_MIXED passed=1 fp32_byte_exact=1 "
			"fp64_envelope=1")!=std::string::npos&&
		kernelSweep.find("ratio_interior=18 ratio_min_margin=0.10898987999238696")!=
			std::string::npos&&kernelSweep.find("alpha_interior=21 alpha_min_margin="
			"0.10898987999238696")!=std::string::npos&&
		kernelSweep.find("boundary_adjacent_limited_alpha=4")!=std::string::npos&&
		kernelSweep.find("metal_cpu_byte_exact=1 signed_zero_red=1 "
			"fp64_gamma128_envelope=1")!=
			std::string::npos&&
		metalContextProbe.find("MTLCreateSystemDefaultDevice()")!=std::string::npos&&
		metalContextProbe.find("MTLCopyAllDevices()")!=std::string::npos,
		"r191 distinguishes execution-context denial from absent Metal hardware and preserves tier-8-first ordering");
	const std::string residentCandidateLineageContract=ReadText(
		"rendered/fire_production_calibration/r196_resident_candidate_lineage_contract/"
		"resident_candidate_lineage_contract.v1");
	const std::array<std::size_t,8> residentRungs={{
		residentCandidateLineageContract.find("rung_1 device_resident_transport_coefficients\n"),
		residentCandidateLineageContract.find("rung_2 resident_physical_flux_authority\n"),
		residentCandidateLineageContract.find("rung_3 resident_EOS_candidate_identity\n"),
		residentCandidateLineageContract.find(
			"rung_4 authenticated_device_target_lineage_through_r70\n"),
		residentCandidateLineageContract.find("rung_5 complete_R0_R1_R2_owner_live_on_Metal\n"),
		residentCandidateLineageContract.find(
			"rung_6 Metal_kernel_sweep_against_fp64_mirrors\n"),
		residentCandidateLineageContract.find("rung_7 sealed_tier8_ported_replay\n"),
		residentCandidateLineageContract.find("rung_8 three_way_verdict\n")}};
	bool residentRungsOrdered=residentRungs[0]!=std::string::npos;
	for(std::size_t rung=1u;rung<residentRungs.size();++rung)
		residentRungsOrdered=residentRungsOrdered&&residentRungs[rung]!=std::string::npos&&
			residentRungs[rung-1u]<residentRungs[rung];
	Check(RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			residentCandidateLineageContract.begin(),residentCandidateLineageContract.end()))==
			"594cd3978d22bec423c7e8132b7d85bbe99cb97e67bbb0add51620560fb40253"&&
		residentCandidateLineageContract.find("contract_kind provenance_extension\n")!=
			std::string::npos&&
		residentCandidateLineageContract.find("relaxation_permitted false\n")!=
			std::string::npos&&
		residentCandidateLineageContract.find(
			"live_path_cpu_substitution_admissible false\n")!=std::string::npos&&
		residentCandidateLineageContract.find(
			"cpu_value_forgery_RED required_per_surface\n")!=std::string::npos&&
		residentCandidateLineageContract.find(
			"cancellation_sensitive_bound_admissible false\n")!=std::string::npos&&
		residentCandidateLineageContract.find(
			"interstage_full_grid_transfer_count 0\n")!=std::string::npos&&
		residentCandidateLineageContract.find(
			"case_record_id_regeneration required_when_case_authored_semantics_or_inputs_change\n")!=
			std::string::npos&&residentCandidateLineageContract.find(
			"producer_run_identity_regeneration required_when_solver_semantics_change\n")!=
			std::string::npos&&residentCandidateLineageContract.find(
			"commit_boundary one_rung_per_pathspec_commit\n")!=std::string::npos&&
		residentCandidateLineageContract.find("RED_battery required_per_rung\n")!=
			std::string::npos&&residentCandidateLineageContract.find(
			"fresh_review required_per_rung\n")!=std::string::npos&&residentRungsOrdered&&
		residentCandidateLineageContract.find("post_verdict_queue onset_criteria_"
			"tier10_and_tier8_then_windows_spectra_rows_movies_report_reviewers_digest\n")!=
			std::string::npos&&
		residentCandidateLineageContract.find(
			"rung_7_aligned_advection_criterion_class 159.01\n")!=std::string::npos,
		"r196 authorizes only a resident, non-forgeable, mirror-validated lineage extension");
	if(failures){std::fprintf(stderr,"FireProductionCalibrationTest: %d failure(s)\n",failures);return 1;}
	std::printf("FireProductionCalibrationTest passed\n");
	return 0;
}
