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
		std::strlen(RISEFireProductionTrace::SourceManifest::TraceCore)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::IndependentWalker)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::Generator)==64u,
		"roundoff trace carries source and generator SHA-256 identities");
	const std::string makeRules=ReadText("build/make/rise/Makefile");
	const std::string windowsRules=ReadText("build/cmake/rise-tests/CMakeLists.txt");
	const std::string unixTestDriver=ReadText("run_all_tests.sh");
	const std::string windowsTestDriver=ReadText("run_all_tests.ps1");
	const std::string walkerSource=ReadText("tests/FireProductionRoundoffWalker.h");
	const std::string tracedTransportSource=ReadText(
		"tests/fire_production_trace/FireProductionTransport.cpp");
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
	Check(unixTestDriver.find("FireProductionCalibrationOracle.r123")!=std::string::npos&&
		unixTestDriver.find("--fire-production-calibration-diagnose-roundoff")!=std::string::npos&&
		unixTestDriver.find("roundoff_rc\" -eq 237")!=std::string::npos&&
		windowsTestDriver.find("FireProductionCalibrationOracle.r123")!=std::string::npos&&
		windowsTestDriver.find("--fire-production-calibration-diagnose-roundoff")!=std::string::npos&&
		windowsTestDriver.find("roundoffRC -eq 237")!=std::string::npos,
		"ordinary Unix and Windows suites execute the exact r123 refusal and accept only exit 237");
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
	Check(makeRules.find("fire_production_trace/%.o")!=std::string::npos&&
		makeRules.find("-fno-fast-math -ffp-contract=off")!=std::string::npos&&
		makeRules.find("$(FIREPRODUCTIONTRACEOBJECTS) $(OBJDRISE)")!=std::string::npos&&
		makeRules.find("check-fire-production-trace")!=std::string::npos&&
		windowsRules.find("fire_production_trace/*.cpp")!=std::string::npos&&
		windowsRules.find("generate_fire_production_roundoff_trace.py\" --check")!=
			std::string::npos&&windowsRules.find("COMPILE_OPTIONS \"/fp:strict\"")!=
			std::string::npos,
		"roundoff trace is source-check-bound and strict on Make and Windows test surfaces");
	Check(walkerSource.find("RISEFireProductionTrace")==std::string::npos&&
		walkerSource.find("Counters")==std::string::npos&&
		walkerSource.find("fire_production_trace")==std::string::npos,
		"independent topology walker shares neither trace counts nor generated arithmetic code");
	const std::size_t firstStageSeam=tracedTransportSource.find("SealStageAndReset");
	Check(firstStageSeam!=std::string::npos&&tracedTransportSource.find(
		"SealStageAndReset",firstStageSeam+1u)!=std::string::npos&&
		tracedTransportSource.find("SealStageAndReset",tracedTransportSource.find(
			"SealStageAndReset",firstStageSeam+1u)+1u)==std::string::npos,
		"generated transport trace owns exactly the cell and dual stage-reset seams");

	{
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
	using namespace FireProductionCalibration;
	double radius=0.0;
	const RoundoffStage stages[]={{1.25,0x1p-22,24u},{2.0,0x1p-21,48u}};
	Check(ComposeRoundoffBound(stages,2u,radius)&&radius>
		2.0*(1.25*0.0+0x1p-22)+0x1p-21,"outward roundoff recurrence");
	const RoundoffStage invalid[]={{1.0,1.0,16777216u}};
	Check(!ComposeRoundoffBound(invalid,1u,radius)&&radius==0.0,
		"roundoff derivation rejects n*u >= 1");

	double order=0.0,distance=0.0;
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
		20u,156u,180u,192u,72u,0u,20u,0u,0u,40u,0u,20u,0u}};
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
	Check(tracedOK&&tracedBytes&&tracedOperations==700u&&
		std::equal(expectedTraceKinds.begin(),expectedTraceKinds.end(),
			std::begin(tracedCounters.operation))&&tracedCounters.maximumDepth==14u&&
		tracedCounters.comparisonCount==404u&&tracedCounters.unresolvedBranch&&
		tracedCounters.branchObligations.size()==180u&&
		tracedCounters.dischargedBranchObligationCount==60u&&
		tracedCounters.minimumDenominatorLowerBound==0.25&&
		*std::max_element(std::begin(tracedCounters.maximumAbsoluteOperand),
			std::end(tracedCounters.maximumAbsoluteOperand))==5.0&&
		!tracedCounters.invalidDomain&&walked&&
		tracedOperations==walkedTopology.operationCount&&
		std::equal(std::begin(tracedCounters.operation),std::end(tracedCounters.operation),
			std::begin(walkedTopology.operation))&&
		tracedCounters.maximumDepth==walkedTopology.maximumDepth,
		"independent remap graph walk reproduces traced operation count and depth while the trace reproduces fp32 bytes");
	if(!(tracedOK&&tracedBytes&&tracedOperations==700u&&
		tracedCounters.comparisonCount==404u&&tracedCounters.unresolvedBranch&&
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
	for(const FireProductionRoundoffTrace::Observation& stage:tracedStep.stages){
		std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)operations+=count;
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
	if(failures){std::fprintf(stderr,"FireProductionCalibrationTest: %d failure(s)\n",failures);return 1;}
	std::printf("FireProductionCalibrationTest passed\n");
	return 0;
}
