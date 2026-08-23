#include "FireProductionCalibrationMath.h"
#include "FireProductionCalibrationMirror.h"
#include "FireProductionRoundoffWalker.h"
#include "FireProductionRoundoffTraceAdapter.h"
#include "Utilities/FireProductionAdvection.h"
#include "Utilities/FireProductionProjection.h"
#include "Utilities/FireProductionTransport.h"
#include "fire_production_fp64/FireProductionAdvection.h"
#include "fire_production_fp64/FireProductionProjection.h"
#include "fire_production_fp64/SourceManifest.h"
#include "fire_production_trace/FireProductionAdvection.h"
#include "fire_production_trace/FireProductionTransport.h"
#include "fire_production_trace/SourceManifest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

namespace
{
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
}

int main()
{
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
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			mirrorAdapter.begin(),mirrorAdapter.end()))==
		"bb541a79454136422ec19c8d2c091d85a09d9051756105b23746ddae6a731128"&&
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
	const std::string recordsSource=ReadText(
		"src/Library/Utilities/FireSimulationRecords.cpp");
	const std::string recordsHeader=ReadText(
		"src/Library/Utilities/FireSimulationRecords.h");
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
	RISE::FireProductionResidentStepResult tokenEligibility;
	tokenEligibility.physicalProjection.validationPassed=true;
	tokenEligibility.projection.validationPassed=true;
	tokenEligibility.manifoldPlateauPassed=true;
	tokenEligibility.conservativeProducerPrecision=RISE::FireStateProducerPrecision::Binary32;
	tokenEligibility.residentProjectionInvocationCount=2u;
	const bool eligibleTokenState=
		RISE::FireProductionResidentStepEligibleForAcceptedManifoldToken(tokenEligibility);
	tokenEligibility.physicalProjection.validationPassed=false;
	const bool physicalValidationMissCannotMint=
		!RISE::FireProductionResidentStepEligibleForAcceptedManifoldToken(tokenEligibility);
	Check(!manifoldClosure.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(manifoldClosure.begin(),manifoldClosure.end()))==
		"bcccdc5dd236075fc01f41fdccab8eccb33ddc73c2474c61dd83df9a3771e35d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			priorManifoldClosure.begin(),priorManifoldClosure.end()))==
		"c095c05a6fe2e92c58744a9de589f6600e8249a5824537ee4392e1ff0ade21fb"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
		"102212336dc0c1602ca9ec977c4397b4c445a1991fd8fdeee90988033fa11c50"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceSource.begin(),forceSource.end()))==
		"f6a6a871857c6dd6ede5b6d61ac8b0aa0818c03f7ed6c42a861f769ec13618d2"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceHeader.begin(),forceHeader.end()))==
		"5eeb1ce935672709a53ce3c4c389f703cffbe7cf76baf42ebd2e6760ecbffb17"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionSource.begin(),projectionSource.end()))==
		"058b986102af2ff34e0ddf66eb64bc3dbab89981a3c60ffb4e83cb2d071ed876"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionHeader.begin(),projectionHeader.end()))==
		"8282c0b4ebf83b813618d94ec3cfff8f7b23bc2115f77238b911318207bee308"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionMetal.begin(),projectionMetal.end()))==
		"2b3e518e7b69a2d3b0f2014fa42099f650d304212987d7d27e2941173494c331"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fireCaseSource.begin(),fireCaseSource.end()))==
		"ccec8ac875bd2922217a90dad0c114cb2ef1e3ccab47c05bdc65208459adb003"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fireCaseHeader.begin(),fireCaseHeader.end()))==
		"48d640638cc1ee2704be3a880d72eba2e609ff6374ae7b50400a497ab3822f5d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			recordsSource.begin(),recordsSource.end()))==
		"04d7f0b21ae1e078f125759a2a08d8b82da68c7a2bdc1a1c8b8aff8f4c8ddc96"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			recordsHeader.begin(),recordsHeader.end()))==
		"804077bacc6a7048e40a1fa9d66962a55257566e598ecb9137f7c99bbc8f3e08"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			dyadicFixture.begin(),dyadicFixture.end()))==
		"6faa4c0c121a50d5e2e6ddefc71d0f982cdbd7ce5f661a721401f8f0ffd2fd17"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			subdominanceFixture.begin(),subdominanceFixture.end()))==
		"73aadc1787fcb9bdb3908d7200a368473fe8a53f9dc632b14bf972ea2a98fed4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			goldenCompositionFixture.begin(),goldenCompositionFixture.end()))==
		"df492334d48bd7b1c0cb05eca927f75043d22f0c665d149a2abb6bd11df6276a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			productionSolverTest.begin(),productionSolverTest.end()))==
		"67160cad4587fbec048963a2c76c7e229b2972ca976820893128539ba7a053c4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			sequenceTest.begin(),sequenceTest.end()))==
		"19c54b17a6201bb2011ede91a22e3c8eb92f2f79d9a765dceca0133b6b9712d9"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			simulationSolverTest.begin(),simulationSolverTest.end()))==
		"34251daf960a8f5adb6596b1a6445999dbde7f4a5657a5921fd2bf8d57269703"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			simulationCore.begin(),simulationCore.end()))==
		"f34c24143d12f60d429964f16301f3b70518f80a62a7e7f1e676169f5eee7800"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fp64SourceManifest.begin(),fp64SourceManifest.end()))==
		"6e22cd76208694922adf053f4530e19b99410fc17d33e41c2a7bcf95277a1437"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			traceSourceManifest.begin(),traceSourceManifest.end()))==
		"de87b6e02a693a37acd51385a81c9d2f7495663ff854c138c38ab398c7bb00a3"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fp64Generator.begin(),fp64Generator.end()))==
		"1c4853f28771cf4a159e4bda1d837e5f371003e902568db9dec1a4ec7b0ede53"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			traceGenerator.begin(),traceGenerator.end()))==
		"34f2cf9aefaf0f786702fe5edae9b130fda02c22b16802f3a3562156529b748a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			unixTestDriver.begin(),unixTestDriver.end()))==
		"6e6099a1e97c343b926f3339039dd37836b357093c4af219110279a8094a12b0"&&
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
		manifoldClosure.find("accepted_history_without_observation_v12_writer_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("legacy_binary32_accepted_history_v9_v10_loader_rejected true")!=
			std::string::npos&&
		manifoldClosure.find("r148_lifecycle_exact_exit 255")!=std::string::npos&&
		manifoldClosure.find("r142_retained_exact_exit 253")!=std::string::npos&&
		manifoldClosure.find("r144_retained_exact_exit 254")!=std::string::npos&&
		fp64ForceHeader.find("PublishFireProductionAcceptedManifoldObservation")==
			std::string::npos&&
		traceForceHeader.find("PublishFireProductionAcceptedManifoldObservation")==
			std::string::npos&&
		eligibleTokenState&&physicalValidationMissCannotMint&&
		!std::is_aggregate<RISE::FireProductionAcceptedManifoldObservation>::value&&
		std::is_final<RISE::FireProductionCheckpointManifoldAccess>::value&&
		!std::is_default_constructible<RISE::FireProductionCheckpointManifoldAccess>::value&&
		forceHeader.find("RestoreValidatedCheckpointRecord")==std::string::npos&&
		forceHeader.find("RestoreValidatedCheckpointFile")!=std::string::npos&&
		advectionMetal.find("RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE")!=
			std::string::npos&&
		dyadicFixture.find("physicalValidationOwnerRED")!=std::string::npos&&
		sequenceTest.find("version!=12u")!=std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r148_manifold_lifecycle")!=std::string::npos&&
		unixTestDriver.find("RISE_FIRE_MANIFOLD_LIFECYCLE_PROBE=1")!=std::string::npos&&
		unixTestDriver.find("lifecycle_rc\" -eq 255")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=255)")!=std::string::npos&&
		unixTestDriver.find("expected 255")!=std::string::npos,
		"r147 makes accepted-observation authority producer-owned and executes checkpoint resume");
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
		CountText(tracedTransportSource,"SealDualStageAndReset")==1u&&
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
		const std::array<unsigned int,6> allOpen={{2u,2u,2u,2u,2u,2u}};
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
	FireProductionCalibration::ResidentStep64Result step64;
	Check(FireProductionCalibration::AdvanceResidentStep64(step,0.0,step64,&error)&&
		step64.force.schedule.substepCount==1u&&step64.cell.executedSubmapCount==5u&&
		step64.dual.executedSubmapCount==15u&&
		step64.projection.executedVCycleCount==12u&&
		step64.projection.maximumPostProjectionResidualPerS==0.0,
		"binary64 mirror composes force, five cell maps, fifteen dual maps, sources, and one P2");
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
	if(!(tracedStepOK&&tracedStepStages&&tracedStepBytes))
		std::fprintf(stderr,"roundoff full-step detail ok=%d stages=%d bytes=%d count=%zu error=%s\n",
			tracedStepOK?1:0,tracedStepStages?1:0,tracedStepBytes?1:0,
			tracedStep.stages.size(),error.c_str());
	if(failures){std::fprintf(stderr,"FireProductionCalibrationTest: %d failure(s)\n",failures);return 1;}
	std::printf("FireProductionCalibrationTest passed\n");
	return 0;
}
