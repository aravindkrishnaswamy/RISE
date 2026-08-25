#include "FireProductionCalibrationMath.h"
#include "FireProductionCalibrationMirror.h"
#include "FireProductionRoundoffWalker.h"
#include "FireProductionRoundoffTraceAdapter.h"
#include "Utilities/FireProductionAdvection.h"
#include "Utilities/FireProductionForce.h"
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

#if defined(__APPLE__)
namespace RISE
{
	bool FireProductionDualLayoutPackRequiresSerialOwner(bool,bool);
}
#endif

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
	const std::vector<double> equalTimeSchedule(8u,equalTimeSubstep);
	const std::string equalTimeTerminalTarget="terminal-target";
	const std::string equalTimePenultimateTarget="penultimate-target";
	std::vector<double> mismatchedEqualTimeSchedule=equalTimeSchedule;
	mismatchedEqualTimeSchedule.back()=std::nextafter(mismatchedEqualTimeSchedule.back(),0.0);
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
	tokenEligibility.acceptedShape.nx=4u;tokenEligibility.acceptedShape.ny=4u;
	tokenEligibility.acceptedShape.nz=4u;tokenEligibility.acceptedShape.cellWidthM=1.0f;
	tokenEligibility.manifoldMapCellCount=64u;
	tokenEligibility.manifoldScalarDeviceToHostTransferCount=2u;
	tokenEligibility.maximumPredictedAdvectiveManifoldAnomaly=0.0;
	tokenEligibility.advectiveAnomalyClosurePassCount=1u;
	const bool eligibleTokenState=
		RISE::FireProductionResidentStepEligibleForAcceptedManifoldToken(tokenEligibility);
	tokenEligibility.physicalProjection.validationPassed=false;
	const bool physicalValidationMissCannotMint=
		!RISE::FireProductionResidentStepEligibleForAcceptedManifoldToken(tokenEligibility);
	tokenEligibility.physicalProjection.validationPassed=true;
	tokenEligibility.manifoldPlateauPassed=false;
	const bool plateauMissCannotMint=
		!RISE::FireProductionResidentStepEligibleForAcceptedManifoldToken(tokenEligibility);
	Check(!manifoldClosure.empty()&&RISE::RISECBOR64::SHA256Hex(
		RISE::RISECBOR64::Bytes(manifoldClosure.begin(),manifoldClosure.end()))==
		"f4ae4d1416dd9d478c6b2eb1170de211b9e09f48a17ad842baae9c89a10eefc4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			priorManifoldClosure.begin(),priorManifoldClosure.end()))==
		"c095c05a6fe2e92c58744a9de589f6600e8249a5824537ee4392e1ff0ade21fb"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceSource.begin(),forceSource.end()))==
			"d05d0cb4a7f276dd72b1028ea1328d3f411e52973dcfe47829449f1246354cb7"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceHeader.begin(),forceHeader.end()))==
			"9f42b297a85846044283a0a9c2a699a77cdb669a50498df136b7dddb39337175"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionSource.begin(),projectionSource.end()))==
			"4c23b91b99e415d950a84c9176bb884b3ddaf6c1c55294cdff0031a0195ff2b4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionHeader.begin(),projectionHeader.end()))==
		"9691348b8fdb77388be25381aca43a6091144c75c8434cf1245d4a48514b0183"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionMetal.begin(),projectionMetal.end()))==
		"4903c314519715f0cf61ade04c615cb8a6f999d34e2b6513ff42dbec0bc98bbe"&&
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
			"ad7bc0252c959d0bb1e871a76c9944ad9754e5f07093d16dd57b96b2acb9de27"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			subdominanceFixture.begin(),subdominanceFixture.end()))==
		"73aadc1787fcb9bdb3908d7200a368473fe8a53f9dc632b14bf972ea2a98fed4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			productionSolverTest.begin(),productionSolverTest.end()))==
			"bfeecf41506b9c78cdd4d6d6ba51c5d1130406ac541a05ea4cd2c730fbb3290d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			sequenceTest.begin(),sequenceTest.end()))==
			"18b82f2ec14f37bc8cc3f615f99b4447f312e6f7a40ea0f643e90e2c4de2b731"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			simulationSolverTest.begin(),simulationSolverTest.end()))==
		"34251daf960a8f5adb6596b1a6445999dbde7f4a5657a5921fd2bf8d57269703"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			simulationCore.begin(),simulationCore.end()))==
		"f34c24143d12f60d429964f16301f3b70518f80a62a7e7f1e676169f5eee7800"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fp64SourceManifest.begin(),fp64SourceManifest.end()))==
			"74fe3221dd717cd0be2d61b5634d14a8dd96477888bd4fe5bea5a4027ac6be2a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			traceSourceManifest.begin(),traceSourceManifest.end()))==
			"f8ce31ee45872933069bd7af818ab3747b3152032dc724e9544b96520115bb91"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fp64Generator.begin(),fp64Generator.end()))==
		"a51504801f8cfc9e1caa57521f984952579c690d831c5eadba238e95e74b0a6d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			traceGenerator.begin(),traceGenerator.end()))==
		"9d8d20964412fd2ab06195ada05b890813d3e58956a18836056b27c60c1cbb6a"&&
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
		manifoldClosure.find("r144_retained_exact_exit 254")!=std::string::npos&&
		fp64ForceHeader.find("PublishFireProductionAcceptedManifoldObservation")==
			std::string::npos&&
		traceForceHeader.find("PublishFireProductionAcceptedManifoldObservation")==
			std::string::npos&&
		eligibleTokenState&&physicalValidationMissCannotMint&&plateauMissCannotMint&&
		!std::is_aggregate<RISE::FireProductionAcceptedManifoldObservation>::value&&
		std::is_final<RISE::FireProductionCheckpointManifoldAccess>::value&&
		!std::is_default_constructible<RISE::FireProductionCheckpointManifoldAccess>::value&&
		forceHeader.find("RestoreValidatedCheckpointRecord")==std::string::npos&&
		forceHeader.find("RestoreValidatedCheckpointFile")!=std::string::npos&&
		advectionMetal.find("RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE")!=
			std::string::npos&&
		dyadicFixture.find("physicalValidationOwnerRED")!=std::string::npos&&
		sequenceTest.find("version!=13u")!=std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r148_manifold_lifecycle")!=std::string::npos&&
		unixTestDriver.find("RISE_FIRE_MANIFOLD_LIFECYCLE_PROBE=1")!=std::string::npos&&
		unixTestDriver.find("lifecycle_rc\" -eq 255")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=255)")!=std::string::npos&&
		unixTestDriver.find("expected 255")!=std::string::npos,
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
			"contract_level_production_ceiling_ruling_required true")!=std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fireSimulatorCore.begin(),fireSimulatorCore.end()))==
			"f34c24143d12f60d429964f16301f3b70518f80a62a7e7f1e676169f5eee7800"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fireSimulationRecords.begin(),fireSimulationRecords.end()))==
			"04d7f0b21ae1e078f125759a2a08d8b82da68c7a2bdc1a1c8b8aff8f4c8ddc96"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionSource.begin(),advectionSource.end()))==
			"389eb83c649375ec8649b372bb1db33bb62785d1b6324c6cc29943d21c2bfa58"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			productionTransportSource.begin(),productionTransportSource.end()))==
			"89e7d007e106b751f7faa75dab2f66648c1bbd125e36cde0f37207218db1f056"&&
		goldenCompositionFixture.find("transportedAmbient[8]=manifoldNT")!=
			std::string::npos&&
		goldenCompositionFixture.find("energyFlux[fluxBase]=static_cast<float>(fluxEnergy)")!=
			std::string::npos&&
		goldenCompositionFixture.find("reconstructionGeneration[0]==0.0025328069638265172")!=
			std::string::npos&&
		goldenCompositionFixture.find("device.back()<=75.0")!=std::string::npos&&
		goldenCompositionFixture.find("wall.back()<=200.0")!=std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r150_manifold_stage_budget")!=
			std::string::npos&&
		unixTestDriver.find("RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE=malformed")!=
			std::string::npos&&
		unixTestDriver.find("stage_budget_malformed_rc\" -eq 223")!=std::string::npos&&
		unixTestDriver.find("\"$timeout_bin\" \"$RISE_TEST_TIMEOUT\" \"$stage_budget_path\"")!=
			std::string::npos&&
		unixTestDriver.find("RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE=1")!=
			std::string::npos&&
		unixTestDriver.find("stage_budget_rc\" -eq 245")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=245)")!=std::string::npos&&
		unixTestDriver.find("expected 245")!=std::string::npos,
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
			std::string::npos&&
		goldenCompositionFixture.find(
			"alphaFraction[line*length+donor]")!=std::string::npos&&
		goldenCompositionFixture.find(
			"representedFraction=std::nextafter(representedFraction,0.0f)")!=
			std::string::npos&&
		goldenCompositionFixture.find(
			"donorTemperature<lowerEndpointExterior")!=std::string::npos&&
		goldenCompositionFixture.find(
			"reconstructionLowOrderLowerInfeasibleCount[0]==30381u")!=
			std::string::npos&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionSource.begin(),advectionSource.end()))==
			"389eb83c649375ec8649b372bb1db33bb62785d1b6324c6cc29943d21c2bfa58"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			productionTransportSource.begin(),productionTransportSource.end()))==
			"89e7d007e106b751f7faa75dab2f66648c1bbd125e36cde0f37207218db1f056",
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
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621"&&
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
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceSource.begin(),forceSource.end()))==
			"d05d0cb4a7f276dd72b1028ea1328d3f411e52973dcfe47829449f1246354cb7"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionSource.begin(),projectionSource.end()))==
			"4c23b91b99e415d950a84c9176bb884b3ddaf6c1c55294cdff0031a0195ff2b4"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionHeader.begin(),projectionHeader.end()))==
		"9691348b8fdb77388be25381aca43a6091144c75c8434cf1245d4a48514b0183"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionTestSource.begin(),projectionTestSource.end()))==
			"8d97d9f5112f4b6dcefedd4781d7b857e160cfbb47737d875c102540e10a4177"&&
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
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceSource.begin(),forceSource.end()))==
			"d05d0cb4a7f276dd72b1028ea1328d3f411e52973dcfe47829449f1246354cb7"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceHeader.begin(),forceHeader.end()))==
			"9f42b297a85846044283a0a9c2a699a77cdb669a50498df136b7dddb39337175"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionMetal.begin(),projectionMetal.end()))==
		"4903c314519715f0cf61ade04c615cb8a6f999d34e2b6513ff42dbec0bc98bbe"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionHeader.begin(),projectionHeader.end()))==
		"9691348b8fdb77388be25381aca43a6091144c75c8434cf1245d4a48514b0183"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			productionSolverTest.begin(),productionSolverTest.end()))==
			"bfeecf41506b9c78cdd4d6d6ba51c5d1130406ac541a05ea4cd2c730fbb3290d"&&
		advectionMetal.find("fold_methane_advective_anomaly_target")!=
			std::string::npos&&
		advectionMetal.find(
			"restorationTarget[gid]+=(deviation.y-deviation.x)*inverseTimeStep")!=
			std::string::npos&&
		advectionMetal.find("maximumPredictedAdvectiveAnomalyFloat>0.0f")!=
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
			"zero-anomaly closure skips the corrector and is byte-identical")!=
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
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceHeader.begin(),forceHeader.end()))==
			"9f42b297a85846044283a0a9c2a699a77cdb669a50498df136b7dddb39337175"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceSource.begin(),forceSource.end()))==
			"d05d0cb4a7f276dd72b1028ea1328d3f411e52973dcfe47829449f1246354cb7"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			advectionMetal.begin(),advectionMetal.end()))==
			"36e31af7c1e0ddb91d3ff9552efc41ac9828ada42201889bbad4ad09f718b621"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceMetal.begin(),forceMetal.end()))==
			"9c893f1bcb880f40f10d93b83c384cf6ee31254b753b9e620ae85f19b786345c"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			forceUnsupported.begin(),forceUnsupported.end()))==
			"24c18bdc613b24497f9af03aed723f67a8c1b7c3df0faab1c4b01856eb4a9573"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionHeader.begin(),projectionHeader.end()))==
			"9691348b8fdb77388be25381aca43a6091144c75c8434cf1245d4a48514b0183"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			projectionMetal.begin(),projectionMetal.end()))==
			"4903c314519715f0cf61ade04c615cb8a6f999d34e2b6513ff42dbec0bc98bbe"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			transportHeader.begin(),transportHeader.end()))==
			"7f9bf4b42a020251c17b090130dc348d81a6d39c3462272294db0afa74e4c96a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			calibrationMathSource.begin(),calibrationMathSource.end()))==
			"0d77432eb9e4ccf4e76829ce230f82037435abfb33a437abb062096dde6f9057"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			goldenCompositionFixture.begin(),goldenCompositionFixture.end()))==
			"5170949314f54165347b3496165dad9e81666570ef34b27e81951db6ee064d9b"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			productionSolverTest.begin(),productionSolverTest.end()))==
			"bfeecf41506b9c78cdd4d6d6ba51c5d1130406ac541a05ea4cd2c730fbb3290d"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			dyadicFixture.begin(),dyadicFixture.end()))==
			"ad7bc0252c959d0bb1e871a76c9944ad9754e5f07093d16dd57b96b2acb9de27"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			sequenceTest.begin(),sequenceTest.end()))==
			"18b82f2ec14f37bc8cc3f615f99b4447f312e6f7a40ea0f643e90e2c4de2b731"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			fp64SourceManifest.begin(),fp64SourceManifest.end()))==
			"74fe3221dd717cd0be2d61b5634d14a8dd96477888bd4fe5bea5a4027ac6be2a"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			traceSourceManifest.begin(),traceSourceManifest.end()))==
			"f8ce31ee45872933069bd7af818ab3747b3152032dc724e9544b96520115bb91"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			unixTestDriver.begin(),unixTestDriver.end()))==
			"183735135701668a01b571f2ee32ff1303ccf4f5100102eed0f3f419687c49c3"&&
		RISE::RISECBOR64::SHA256Hex(RISE::RISECBOR64::Bytes(
			timestepVelocityBenchmarkOptions.begin(),timestepVelocityBenchmarkOptions.end()))==
			"be63f6fcd99666a1d2c611f4d06e6f082e9b3b4223216334a0dea9b2e2684d05"&&
		goldenCompositionFixture.find("DRAIN_AWARE_PLATEAU_RETRY_ACCEPTED")!=
			std::string::npos&&
		goldenCompositionFixture.find("return 206")!=std::string::npos&&
		productionSolverTest.find("r164 host-residual activation outside the limited "
			"predictor path fails before Metal work")!=std::string::npos&&
		unixTestDriver.find("FireSequenceTest.r164_drain_aware_retry")!=
			std::string::npos&&
		unixTestDriver.find("closure_limited_rc\" -eq 206")!=std::string::npos&&
		unixTestDriver.find("PASS (exact exit=206, drain-aware retry acceptance)")!=
			std::string::npos,
		"r164 binds the drain-aware refusal retry, accepted plateau, and host profile");
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
