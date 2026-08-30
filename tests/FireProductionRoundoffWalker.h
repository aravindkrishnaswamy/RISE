#ifndef FIRE_PRODUCTION_ROUNDOFF_WALKER_H
#define FIRE_PRODUCTION_ROUNDOFF_WALKER_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

namespace FireProductionRoundoffWalker
{
	// Independent binary64 interval arithmetic for the projection terminal DAG.
	// Inputs are exact promotions of stored binary32 bytes.  Each operation rounds
	// both interval endpoints outward by one representable binary64 value, so the
	// resulting radius covers the actual CPU64 operation without borrowing any
	// production arithmetic implementation.
	struct Binary64Interval
	{
		double center=0.0,lower=0.0,upper=0.0;
		static Binary64Interval Exact(const double value)
		{
			Binary64Interval result;result.center=result.lower=result.upper=value;
			return result;
		}
		double Radius() const
		{
			return std::nextafter(std::max(center-lower,upper-center),
				std::numeric_limits<double>::infinity());
		}
	};

	inline Binary64Interval Binary64FromBounds(const double center,const double lower,
		const double upper)
	{
		Binary64Interval result;result.center=center;
		result.lower=std::nextafter(lower,-std::numeric_limits<double>::infinity());
		result.upper=std::nextafter(upper,std::numeric_limits<double>::infinity());
		return result;
	}
	inline Binary64Interval operator+(const Binary64Interval& first,
		const Binary64Interval& second)
	{
		return Binary64FromBounds(first.center+second.center,
			first.lower+second.lower,first.upper+second.upper);
	}
	inline Binary64Interval operator-(const Binary64Interval& first,
		const Binary64Interval& second)
	{
		return Binary64FromBounds(first.center-second.center,
			first.lower-second.upper,first.upper-second.lower);
	}
	inline Binary64Interval operator-(const Binary64Interval& value)
	{
		return Binary64FromBounds(-value.center,-value.upper,-value.lower);
	}
	inline Binary64Interval operator*(const Binary64Interval& first,
		const Binary64Interval& second)
	{
		const double products[4]={first.lower*second.lower,first.lower*second.upper,
			first.upper*second.lower,first.upper*second.upper};
		return Binary64FromBounds(first.center*second.center,
			*std::min_element(products,products+4),*std::max_element(products,products+4));
	}
	inline Binary64Interval operator/(const Binary64Interval& first,
		const Binary64Interval& second)
	{
		if(second.lower<=0.0&&second.upper>=0.0){Binary64Interval invalid;
			invalid.lower=-std::numeric_limits<double>::infinity();
			invalid.upper=std::numeric_limits<double>::infinity();return invalid;}
		const double quotients[4]={first.lower/second.lower,first.lower/second.upper,
			first.upper/second.lower,first.upper/second.upper};
		return Binary64FromBounds(first.center/second.center,
			*std::min_element(quotients,quotients+4),
			*std::max_element(quotients,quotients+4));
	}

	enum OperationKind : unsigned int
	{
		Convert,Add,Subtract,Multiply,Divide,SquareRoot,Absolute,Minimum,Maximum,
		Floor,Ceil,Remainder,NextAfter,OperationKindCount
	};

	struct Topology
	{
		std::uint64_t operation[OperationKindCount]={};
		std::uint64_t operationCount=0u;
		std::uint32_t maximumDepth=0u;
	};

	inline bool CheckedAddProduct(std::uint64_t& total,const std::uint64_t factor,
		const std::uint64_t count)
	{
		if(count&&factor>std::numeric_limits<std::uint64_t>::max()/count)return false;
		const std::uint64_t term=factor*count;
		if(total>std::numeric_limits<std::uint64_t>::max()-term)return false;
		total+=term;return true;
	}

	inline bool CheckedAddOperation(Topology& topology,const OperationKind kind,
		const std::uint64_t factor,const std::uint64_t count)
	{
		return CheckedAddProduct(topology.operation[kind],factor,count)&&
			CheckedAddProduct(topology.operationCount,factor,count);
	}

	inline bool CheckedProduct(const std::uint64_t first,const std::uint64_t second,
		std::uint64_t& product)
	{
		product=0u;
		if(second&&first>std::numeric_limits<std::uint64_t>::max()/second)return false;
		product=first*second;return true;
	}

	inline std::size_t NextPowerOfTwo(const std::size_t value)
	{
		std::size_t result=1u;while(result<value)result<<=1u;return result;
	}

	inline bool IntervalsAreSeparated(const double leftCenter,const double leftRadius,
		const double rightCenter,const double rightRadius)
	{
		if(!(leftRadius>=0.0&&rightRadius>=0.0))return false;
		return (leftRadius==0.0&&rightRadius==0.0)||
			std::fabs(leftCenter-rightCenter)>leftRadius+rightRadius;
	}

	struct BranchWitness
	{
		std::size_t line=0u,cell=0u,component=0u;
		double leftCenter=0.0,leftRadius=0.0,rightCenter=0.0,rightRadius=0.0;
		double linearCenter=0.0,linearRadius=0.0;
		double endpointAbsoluteUpper=0.0,endpointRadius=0.0;
		float leftRounded=0.0f,rightRounded=0.0f;
		bool roundedResult=false;
	};

	struct PPMQuadraticZeroCertificate
	{
		bool continuousAtSwitch=false;
		double ambiguityWidth=0.0;
		double arithmeticResidualBound=0.0;
		double divergenceBound=0.0;
	};
	struct ProjectionInterpolationCertificate
	{
		std::int64_t exactNumerator=0,exactDenominator=0,boundary=0;
		float roundedPosition=0.0f;
		bool exactAndRoundedSameSide=false;
	};
	struct ProjectionAposterioriCertificate
	{
		double densityLower=0.0,densityUpper=0.0;
		double dimensionlessEigenvalueLower=0.0,operatorEigenvalueLower=0.0;
		double inverseOperatorNormUpper=0.0,velocityGainUpper=0.0;
		double acceptedRoundedResidual=0.0,residualEvaluationRoundingUpper=0.0;
		double crossPrecisionResidualUpper=0.0,fp64ResidualGateUpper=0.0;
		double fp64ResidualEvaluationUpper=0.0,fp64FeedbackFactor=0.0;
		double fp64TerminalFaceL2PerCellUpper=0.0;
		double beginningVelocityRoundingUpper=0.0;
		double validationToleranceRounded=0.0,validationToleranceRoundingUpper=0.0;
		double validationPredicateMarginLower=0.0;
		double streamingFaceVelocityL2PerCellUpper=0.0,velocityRMSUpper=0.0;
		bool validationPredicateSeparated=false;
		bool pressureOpenAnchor=false,allConstantsStructural=false;
	};
	struct FullStepMetricStage
	{
		std::array<double,12> radiusSum={},radiusSquareSum={},maximumRadius={};
		std::array<std::uint64_t,12> count={};
	};
	struct FullStepRoundoffCertificate
	{
		std::array<double,9> scalarFilteredL1Upper={},scalarRMSUpper={},
			inventoryPerVolumeUpper={};
		std::array<double,3> momentumL2PerCellUpper={};
		double gasDensityRMSUpper=0.0,densityRelativeUpper=0.0;
		double provisionalVelocityL2PerCellUpper=0.0;
		double physicalProjectionVelocityUpper=0.0,finalVelocityRMSUpper=0.0;
		double openBoundaryInteractionUpper=0.0;
		std::uint32_t proofGapBitmap=0u;
		bool scalarTransportNonexpansive=false,dualTransportNonexpansive=false,
			projectionInteractionsIncluded=false,proofComplete=false;
	};
	enum FullStepProofGap : std::uint32_t
	{
		FullStepNonlinearFCTGain=1u<<0u,
		FullStepDensityLInfinity=1u<<1u,
		FullStepProjectionResolvent=1u<<2u,
		FullStepOpenQuadraticLInfinity=1u<<3u,
		FullStepMomentumSourceMetric=1u<<4u,
		FullStepGasReductionMetric=1u<<5u,
		FullStepAllSlices=1u<<6u,
		FullStepIndependentCardinality=1u<<7u
	};
	struct FullStepAssumptionRefusal
	{
		double conservativeCompressionL2Gain=0.0;
		double localizedProductRMS=0.0,productOfRMS=0.0;
		double sharedAlphaCrossComponentResponse=0.0;
		bool fctUnitGainRejected=false,rmsProductRejected=false,
			componentDiagonalRejected=false;
	};
	enum class FullStepGraphVariant { Certified,MissingCellStage,
		MissingDensityInteraction,MissingPhysicalFeedthrough,MissingRestorationFeedthrough };
	enum class ProjectionAposterioriGraphVariant { Certified,DoublePoincare,
		MissingCrossPrecisionResidual,MissingFP64ResidualGate,MissingFP64Feedback,
		MissingFP64Terminal,MissingBeginningVelocity,
		MissingFaceStreaming,
		SwappedDensityEnvelope };
	enum class ProjectionInterpolationGraphVariant { Certified,ShiftedFine,
		ReassociatedDivision };

	inline bool CertifyProjectionInterpolationFloor(const std::size_t fine,
		const std::size_t fineExtent,const std::size_t coarseExtent,
		const std::int64_t boundary,const float tracedRoundedPosition,
		ProjectionInterpolationCertificate& certificate,
		const ProjectionInterpolationGraphVariant variant=
			ProjectionInterpolationGraphVariant::Certified)
	{
		certificate=ProjectionInterpolationCertificate();
		if(!fineExtent||!coarseExtent||fine>=fineExtent||
			fineExtent>std::size_t(std::numeric_limits<std::int64_t>::max()/2u)||
			coarseExtent>std::size_t(std::numeric_limits<std::int64_t>::max()/
				(2u*fine+1u)))return false;
		const std::size_t evaluatedFine=variant==ProjectionInterpolationGraphVariant::ShiftedFine?
			(fine+1u)%fineExtent:fine;
		certificate.exactNumerator=static_cast<std::int64_t>((2u*evaluatedFine+1u)*
			coarseExtent)-static_cast<std::int64_t>(fineExtent);
		certificate.exactDenominator=static_cast<std::int64_t>(2u*fineExtent);
		certificate.boundary=boundary;
		if(variant==ProjectionInterpolationGraphVariant::ReassociatedDivision)
			certificate.roundedPosition=(static_cast<float>(evaluatedFine)+0.5f)*
				(static_cast<float>(coarseExtent)/static_cast<float>(fineExtent))-0.5f;
		else certificate.roundedPosition=(static_cast<float>(evaluatedFine)+0.5f)*
			static_cast<float>(coarseExtent)/static_cast<float>(fineExtent)-0.5f;
		const bool exactAbove=certificate.exactNumerator>=
			boundary*certificate.exactDenominator;
		const bool roundedAbove=certificate.roundedPosition>=static_cast<float>(boundary);
		certificate.exactAndRoundedSameSide=exactAbove==roundedAbove&&
			certificate.roundedPosition==tracedRoundedPosition;
		return certificate.exactAndRoundedSameSide;
	}

	inline bool CountProjectionInterpolationObligations(std::size_t nx,std::size_t ny,
		std::size_t nz,const std::uint32_t cycles,std::uint64_t& result)
	{
		result=0u;if(!nx||!ny||!nz||!cycles)return false;
		std::vector<std::array<std::size_t,3> > hierarchy;
		hierarchy.push_back({{nx,ny,nz}});
		while(nx>4u||ny>4u||nz>4u){if(nx>4u)nx=(nx+1u)/2u;
			if(ny>4u)ny=(ny+1u)/2u;if(nz>4u)nz=(nz+1u)/2u;
			hierarchy.push_back({{nx,ny,nz}});}
		std::uint64_t perCycle=0u;
		for(std::size_t level=0u;level+1u<hierarchy.size();++level){
			const std::array<std::size_t,3>& fine=hierarchy[level];
			const std::array<std::size_t,3>& coarse=hierarchy[level+1u];
			for(unsigned int axis=0u;axis<3u;++axis){
				if(fine[axis]==coarse[axis])continue;
				std::uint64_t boundaryCoordinates=0u;
				for(std::size_t coordinate=0u;coordinate<fine[axis];++coordinate){
					const std::int64_t numerator=static_cast<std::int64_t>(
						(2u*coordinate+1u)*coarse[axis])-static_cast<std::int64_t>(fine[axis]);
					const std::int64_t denominator=static_cast<std::int64_t>(2u*fine[axis]);
					if(numerator>0&&numerator<static_cast<std::int64_t>(coarse[axis]-1u)*
						denominator&&numerator%denominator==0)++boundaryCoordinates;
				}
				std::uint64_t repetitions=1u;
				for(unsigned int other=0u;other<3u;++other)if(other!=axis){
					std::uint64_t product=0u;
					if(!CheckedProduct(repetitions,fine[other],product))return false;
					repetitions=product;
				}
				if(!CheckedAddProduct(perCycle,boundaryCoordinates,repetitions))return false;
			}
		}
		return CheckedProduct(perCycle,cycles,result);
	}

	struct ContinuousLimiterCertificate
	{
		static constexpr double UnitRoundoff=0x1p-24;
		static constexpr double WidthFactor=16384.0;
		double scale=0.0,width=0.0,requiredUnitFactor=0.0;
		bool ambiguityContained=false,continuousAtNegativeWidth=false;
		bool continuousAtZero=false,continuousAtPositiveWidth=false;
		bool monotoneForPositiveConsumption=false;
	};
	struct FloorPartitionCertificate
	{
		double ambiguityWidth=0.0,exactBranchTerm=0.0;
		double roundedTerm=0.0,ftzTerm=0.0,totalEnvelope=0.0;
		std::uint64_t alternatePathOperationCount=0u;
		bool continuousAtInteger=false;
	};
	enum class FloorGraphVariant { Certified,HalfAmbiguity,MissingRoundedTerm,
		MissingCyclePath };
	struct RemainingPositiveCertificate
	{
		double ambiguityWidth=0.0,exactBranchTerm=0.0;
		double roundedTerm=0.0,ftzTerm=0.0,totalEnvelope=0.0;
		std::uint64_t alternatePathOperationCount=0u;
		bool zeroWidthAtSwitch=false;
	};
	enum class RemainingGraphVariant { Certified,HalfAmbiguity,MissingCellIntegral,
		MissingFTZ };
	struct CourantSignCertificate
	{
		double ambiguityWidth=0.0,profileAbsoluteUpper=0.0;
		std::uint64_t commonSetupOperationCount=0u,positiveSuccessorOperationCount=0u;
		std::uint64_t negativeSuccessorOperationCount=0u,alternatePathOperationCount=0u;
		double exactBranchTerm=0.0,roundedTerm=0.0,ftzTerm=0.0,totalEnvelope=0.0;
		bool continuousAtZero=false,signedZeroCanonical=false;
	};
	enum class CourantGraphVariant { Certified,HalfAmbiguity,MissingNegativePath,
		MissingFTZ,NoncanonicalSignedZero };
	struct FractionPositiveCertificate
	{
		double ambiguityWidth=0.0,exactBranchTerm=0.0,roundedTerm=0.0,ftzTerm=0.0;
		double totalEnvelope=0.0;std::uint64_t tailOperationCount=0u;
		bool zeroWidthAtSwitch=false;
	};
	enum class FractionGraphVariant { Certified,HalfAmbiguity,MissingTrailingIntegral,
		MissingFTZ };
	struct FlatIntegralCertificate
	{
		double deviationUpper=0.0,exactBranchTerm=0.0,roundedTerm=0.0,ftzTerm=0.0;
		double totalEnvelope=0.0;std::uint64_t curvedPathOperationCount=0u;
		bool continuousAtFlatProfile=false,productionGraphMatched=false;
	};
	enum class FlatIntegralGraphVariant { Certified,HalfDeviation,MissingCurvedPolynomial,
		MissingFTZ,DiscontinuousFlatPath,MutatedQuadraticCoefficient };
	struct NonnegativeReductionCertificate
	{
		double exactBranchTerm=0.0,roundedTerm=0.0,ftzTerm=0.0,totalEnvelope=0.0;
		std::uint64_t leafCount=0u,absoluteCount=0u,maximumCount=0u;
		bool positiveZeroSeed=false,allLeavesAbsolute=false,maximumOnly=false;
	};
	enum class NonnegativeReductionGraphVariant { Certified,SignedLeaf,NegativeSeed,
		SubtractiveReduction };
	struct InflowTransitionCertificate
	{
		double ambiguityWidth=0.0,widthLower=0.0,donorContrast=0.0;
		double exactBranchTerm=0.0,roundedTerm=0.0,ftzTerm=0.0,totalEnvelope=0.0;
		bool continuousAtJoin=false,convexDonorBlend=false,legacyOutsideWidth=false;
	};
	enum class InflowGraphVariant { Certified,HalfAmbiguity,MissingDonorContrast,
		MissingRoundedTerm,DiscontinuousBinary };
	enum class LimiterGraphVariant { Certified,MissingNegativeRamp,FixedWidthDenominator };

	inline bool CertifyContinuousLimiterTransition(const double predicateCenter,
		const double predicateRadius,const float center,const float envelope,
		const float signedConsumption,ContinuousLimiterCertificate& certificate,
		const LimiterGraphVariant variant=LimiterGraphVariant::Certified)
	{
		certificate=ContinuousLimiterCertificate();
		if(!(predicateRadius>=0.0)||!std::isfinite(predicateCenter)||
			!std::isfinite(predicateRadius)||!std::isfinite(center)||
			!std::isfinite(envelope)||!std::isfinite(signedConsumption))return false;
		certificate.scale=std::max(static_cast<double>(std::numeric_limits<float>::min()),
			std::max(std::fabs(static_cast<double>(center)),std::max(
			std::fabs(static_cast<double>(envelope)),
			std::fabs(static_cast<double>(signedConsumption)))));
		certificate.width=0x1p-10*certificate.scale;
		const double ambiguity=std::nextafter(std::fabs(predicateCenter)+predicateRadius,
			std::numeric_limits<double>::infinity());
		certificate.requiredUnitFactor=ambiguity/
			(ContinuousLimiterCertificate::UnitRoundoff*certificate.scale);
		certificate.ambiguityContained=ambiguity<=certificate.width;
		auto cap=[&](const double headroom,const double consumption){
			const double numerator=headroom+(variant==LimiterGraphVariant::MissingNegativeRamp?
				0.0:std::max(-consumption,0.0));
			const double denominator=variant==LimiterGraphVariant::FixedWidthDenominator?
				certificate.width:std::max(consumption,certificate.width);
			return std::min(1.0,numerator/denominator);
		};
		const double qValues[]={0.0,0.5*certificate.width,2.0*certificate.width};
		certificate.continuousAtNegativeWidth=true;
		certificate.continuousAtZero=true;
		certificate.continuousAtPositiveWidth=true;
		certificate.monotoneForPositiveConsumption=true;
		for(const double q:qValues){
			certificate.continuousAtNegativeWidth&=cap(q,-certificate.width)==1.0;
			certificate.continuousAtZero&=cap(q,-0.0)==cap(q,+0.0);
			certificate.continuousAtPositiveWidth&=
				cap(q,certificate.width)==std::min(1.0,q/certificate.width);
			const double positive[]={0.25*certificate.width,0.5*certificate.width,
				certificate.width,2.0*certificate.width};
			for(const double d:positive)certificate.monotoneForPositiveConsumption&=
				cap(q,d)*d<=q&&cap(q,d)>=0.0&&cap(q,d)<=1.0;
			certificate.monotoneForPositiveConsumption&=
				cap(q,-2.0*certificate.width)==1.0&&
				cap(q,2.0*certificate.width)==std::min(1.0,q/(2.0*certificate.width));
		}
		return certificate.ambiguityContained&&certificate.continuousAtNegativeWidth&&
			certificate.continuousAtZero&&certificate.continuousAtPositiveWidth&&
			certificate.monotoneForPositiveConsumption;
	}

	namespace Detail
	{
		inline double NextUp(const double value)
		{
			return std::nextafter(value,std::numeric_limits<double>::infinity());
		}

		inline double RoundRadius(const double magnitude)
		{
			const double unit=0x1p-24;
			return NextUp(unit/(1.0-unit)*std::fabs(magnitude)+
				static_cast<double>(std::numeric_limits<float>::min()));
		}

		struct Interval
		{
			double center=0.0,radius=0.0;
			float rounded=0.0f;
			Interval()=default;
			explicit Interval(const float value):center(value),rounded(value){}
		};

		inline Interval Make(const double center,const double propagated,const float rounded)
		{
			Interval result;result.center=center;result.rounded=rounded;
			result.radius=NextUp(NextUp(propagated+
				RoundRadius(std::fabs(center)+propagated)));
			return result;
		}

		inline Interval Add(const Interval& a,const Interval& b)
		{
			return Make(a.center+b.center,a.radius+b.radius,
				static_cast<float>(a.rounded+b.rounded));
		}

		inline Interval Subtract(const Interval& a,const Interval& b)
		{
			return Make(a.center-b.center,a.radius+b.radius,
				static_cast<float>(a.rounded-b.rounded));
		}

		inline Interval Multiply(const Interval& a,const Interval& b)
		{
			const double propagated=NextUp(std::fabs(a.center)*b.radius+
				std::fabs(b.center)*a.radius+a.radius*b.radius);
			return Make(a.center*b.center,propagated,
				static_cast<float>(a.rounded*b.rounded));
		}

		inline Interval Divide(const Interval& a,const Interval& b)
		{
			const double lower=std::fabs(b.center)-b.radius;
			if(!(lower>0.0)){Interval result;result.center=a.center/b.center;
				result.radius=std::numeric_limits<double>::infinity();
				result.rounded=static_cast<float>(a.rounded/b.rounded);return result;}
			const double propagated=NextUp(a.radius/lower+
				(std::fabs(a.center)+a.radius)*b.radius/(lower*lower));
			return Make(a.center/b.center,propagated,
				static_cast<float>(a.rounded/b.rounded));
		}

		inline bool CaptureIfUnresolved(const Interval& left,const Interval& right,
			const bool roundedResult,const std::size_t line,const std::size_t cell,
			const std::size_t component,BranchWitness& witness)
		{
			if(IntervalsAreSeparated(left.center,left.radius,right.center,right.radius))
				return false;
			witness.line=line;witness.cell=cell;witness.component=component;
			witness.leftCenter=left.center;witness.leftRadius=left.radius;
			witness.rightCenter=right.center;witness.rightRadius=right.radius;
			witness.leftRounded=left.rounded;witness.rightRounded=right.rounded;
			witness.roundedResult=roundedResult;return true;
		}
	}

	// The cell-centred pressure operator is G^T rho_f^-1 G.  Replacing every
	// face coefficient by 1/rho_max gives a Loewner lower bound.  For the
	// one-dimensional cell-centred Laplacian, sin(x)>=2x/pi yields the rational
	// Poincare bounds 4/n^2 for two pressure-open ends and 1/n^2 for one.
	// Tensor-product summation supplies the three-dimensional eigenvalue bound.
	// The factor two in the velocity gain covers the doubled pressure-open face
	// gradient; no fitted or measured constant enters this certificate.
	inline bool DeriveProjectionAposterioriBound(const std::array<std::size_t,3>& extent,
		const std::array<unsigned int,6>& boundary,const double spacing,
		const double densityLower,const double densityUpper,
		const double acceptedRoundedResidual,const double residualEvaluationRoundingUpper,
		const double crossPrecisionResidualUpper,const double maximumRoundedVelocity,
		const double maximumRoundedTarget,const double beginningVelocityRoundingUpper,
		const bool restoration,
		const double streamingFaceVelocityL2PerCellUpper,
		const double fp64TerminalFaceL2PerCellUpper,
		const double validationToleranceRounded,
		const double validationToleranceRoundingUpper,
		ProjectionAposterioriCertificate& certificate,
		const ProjectionAposterioriGraphVariant variant=
			ProjectionAposterioriGraphVariant::Certified)
	{
		certificate=ProjectionAposterioriCertificate();
		if(!(spacing>0.0&&densityLower>0.0&&densityUpper>=densityLower&&
			acceptedRoundedResidual>=0.0&&residualEvaluationRoundingUpper>=0.0&&
			crossPrecisionResidualUpper>=0.0&&maximumRoundedVelocity>=0.0&&
			maximumRoundedTarget>=0.0&&beginningVelocityRoundingUpper>=0.0&&
			streamingFaceVelocityL2PerCellUpper>=0.0&&validationToleranceRounded>=0.0&&
			fp64TerminalFaceL2PerCellUpper>=0.0&&
			validationToleranceRoundingUpper>=0.0)||!std::isfinite(spacing)||
			!std::isfinite(densityLower)||!std::isfinite(densityUpper)||
			!std::isfinite(acceptedRoundedResidual)||
			!std::isfinite(residualEvaluationRoundingUpper)||
			!std::isfinite(crossPrecisionResidualUpper)||
			!std::isfinite(maximumRoundedVelocity)||
			!std::isfinite(maximumRoundedTarget)||
			!std::isfinite(beginningVelocityRoundingUpper)||
			!std::isfinite(streamingFaceVelocityL2PerCellUpper)||
			!std::isfinite(fp64TerminalFaceL2PerCellUpper)||
			!std::isfinite(validationToleranceRounded)||
			!std::isfinite(validationToleranceRoundingUpper))return false;
		double dimensionlessLower=0.0;bool hasOpen=false;
		for(unsigned int axis=0u;axis<3u;++axis){
			if(!extent[axis])return false;
			const unsigned int openCount=(boundary[2u*axis]==2u?1u:0u)+
				(boundary[2u*axis+1u]==2u?1u:0u);
			hasOpen=hasOpen||openCount!=0u;
			if(openCount){const double n=static_cast<double>(extent[axis]);
				double term=static_cast<double>(openCount==2u?4u:1u)/(n*n);
				if(variant==ProjectionAposterioriGraphVariant::DoublePoincare)term*=2.0;
				term=std::nextafter(term,-std::numeric_limits<double>::infinity());
				dimensionlessLower=std::nextafter(dimensionlessLower+term,
					-std::numeric_limits<double>::infinity());}
		}
		if(!hasOpen||!(dimensionlessLower>0.0))return false;
		const bool swap=variant==ProjectionAposterioriGraphVariant::SwappedDensityEnvelope;
		const double coefficientDensity=swap?densityLower:densityUpper;
		const double gainDensity=swap?densityUpper:densityLower;
		const double denominator=coefficientDensity*spacing*spacing;
		const double eigenLower=std::nextafter(dimensionlessLower/denominator,
			-std::numeric_limits<double>::infinity());
		if(!(eigenLower>0.0))return false;
		const double inverseUpper=Detail::NextUp(1.0/eigenLower);
		const double gainSquared=Detail::NextUp(2.0/(gainDensity*eigenLower));
		const double gainUpper=Detail::NextUp(std::sqrt(gainSquared));
		const double evaluation=residualEvaluationRoundingUpper;
		const double residualUpper=Detail::NextUp(acceptedRoundedResidual+evaluation);
		const double toleranceLower=std::nextafter(validationToleranceRounded-
			validationToleranceRoundingUpper,-std::numeric_limits<double>::infinity());
		const double predicateMargin=std::nextafter(toleranceLower-residualUpper,
			-std::numeric_limits<double>::infinity());
		if(!(predicateMargin>0.0))return false;
		const double cross=variant==ProjectionAposterioriGraphVariant::
			MissingCrossPrecisionResidual?0.0:crossPrecisionResidualUpper;
		const double unit64=0x1p-53;
		const double residualOperations=restoration?14.0:7.0;
		const double gammaResidual=Detail::NextUp(residualOperations*unit64/
			(1.0-residualOperations*unit64));
		const double gammaTolerance=Detail::NextUp((restoration?1.0:3.0)*unit64/
			(1.0-(restoration?1.0:3.0)*unit64));
		const double cells=static_cast<double>(extent[0]*extent[1]*extent[2]);
		const double faceToMaximum=Detail::NextUp(std::sqrt(cells));
		const double divergenceVelocityFactor=(restoration?12.0:6.0)/spacing;
		const double selectedBeginning=variant==ProjectionAposterioriGraphVariant::
			MissingBeginningVelocity?0.0:beginningVelocityRoundingUpper;
		const double velocityBase=Detail::NextUp(maximumRoundedVelocity+selectedBeginning);
		const double residualBase=Detail::NextUp(gammaResidual*Detail::NextUp(
			divergenceVelocityFactor*velocityBase+maximumRoundedTarget));
		const double residualFeedback=Detail::NextUp(gammaResidual*Detail::NextUp(
			(6.0/spacing)*faceToMaximum));
		const double gammaTerminal=Detail::NextUp(8.0*unit64/(1.0-8.0*unit64));
		const double uniqueFaces=static_cast<double>((extent[0]+1u)*extent[1]*extent[2]+
			extent[0]*(extent[1]+1u)*extent[2]+extent[0]*extent[1]*(extent[2]+1u));
		const double faceMetricGain=Detail::NextUp(std::sqrt(uniqueFaces/cells));
		const double terminal64Factor=Detail::NextUp(gammaTerminal*faceMetricGain);
		const double terminal64Base=fp64TerminalFaceL2PerCellUpper;
		const double terminal64Feedback=Detail::NextUp(terminal64Factor*faceToMaximum);
		double toleranceBase=0.0,toleranceFeedback=0.0;
		if(restoration)toleranceBase=Detail::NextUp(Detail::NextUp(
			0.005*maximumRoundedTarget)*(1.0+gammaTolerance));
		else{
			const double maximumExtent=static_cast<double>(std::max(extent[0],
				std::max(extent[1],extent[2])));
			const double lengthLower=std::nextafter(spacing*maximumExtent*
				(1.0-unit64),-std::numeric_limits<double>::infinity());
			if(!(lengthLower>0.0))return false;
			const double toleranceFactor=Detail::NextUp(Detail::NextUp(0.005/
				lengthLower)*(1.0+gammaTolerance));
			toleranceBase=Detail::NextUp(toleranceFactor*velocityBase);
			toleranceFeedback=Detail::NextUp(toleranceFactor*faceToMaximum);
		}
		const double fp64Base=Detail::NextUp(toleranceBase+residualBase);
		const double fp64Feedback=Detail::NextUp(toleranceFeedback+residualFeedback);
		const double selectedTerminalBase=variant==ProjectionAposterioriGraphVariant::
			MissingFP64Terminal?0.0:terminal64Base;
		const double selectedTerminalFeedback=variant==ProjectionAposterioriGraphVariant::
			MissingFP64Terminal?0.0:terminal64Feedback;
		const double feedbackFactor=Detail::NextUp(gainUpper*fp64Feedback+
			selectedTerminalFeedback);
		const double streaming=variant==ProjectionAposterioriGraphVariant::
			MissingFaceStreaming?0.0:streamingFaceVelocityL2PerCellUpper;
		const double selectedFP64Base=variant==ProjectionAposterioriGraphVariant::
			MissingFP64ResidualGate?0.0:fp64Base;
		const double selectedFeedback=(variant==ProjectionAposterioriGraphVariant::
			MissingFP64ResidualGate||variant==ProjectionAposterioriGraphVariant::
			MissingFP64Feedback)?0.0:feedbackFactor;
		if(!(selectedFeedback<1.0))return false;
		const double numerator=Detail::NextUp(gainUpper*Detail::NextUp(
			cross+selectedFP64Base)+streaming+selectedTerminalBase);
		const double velocityUpper=Detail::NextUp(numerator/(1.0-selectedFeedback));
		const double fp64Gate=Detail::NextUp(fp64Base+fp64Feedback*velocityUpper);
		if(!std::isfinite(velocityUpper))return false;
		certificate.densityLower=densityLower;certificate.densityUpper=densityUpper;
		certificate.dimensionlessEigenvalueLower=dimensionlessLower;
		certificate.operatorEigenvalueLower=eigenLower;
		certificate.inverseOperatorNormUpper=inverseUpper;
		certificate.velocityGainUpper=gainUpper;
		certificate.acceptedRoundedResidual=acceptedRoundedResidual;
		certificate.residualEvaluationRoundingUpper=evaluation;
		certificate.crossPrecisionResidualUpper=crossPrecisionResidualUpper;
		certificate.fp64ResidualGateUpper=fp64Gate;
		certificate.fp64ResidualEvaluationUpper=Detail::NextUp(residualBase+
			residualFeedback*velocityUpper);
		certificate.fp64FeedbackFactor=feedbackFactor;
		certificate.fp64TerminalFaceL2PerCellUpper=Detail::NextUp(terminal64Base+
			terminal64Feedback*velocityUpper);
		certificate.beginningVelocityRoundingUpper=beginningVelocityRoundingUpper;
		certificate.validationToleranceRounded=validationToleranceRounded;
		certificate.validationToleranceRoundingUpper=validationToleranceRoundingUpper;
		certificate.validationPredicateMarginLower=predicateMargin;
		certificate.streamingFaceVelocityL2PerCellUpper=
			streamingFaceVelocityL2PerCellUpper;
		certificate.velocityRMSUpper=velocityUpper;
		certificate.validationPredicateSeparated=true;
		certificate.pressureOpenAnchor=true;certificate.allConstantsStructural=true;
		return true;
	}

	inline bool RefuteFullStepCandidateAssumptions(FullStepAssumptionRefusal& refusal)
	{
		refusal=FullStepAssumptionRefusal();
		// A positive conservative compression matrix can have unit column sum and
		// L2 gain sqrt(2); conservation alone never proves the discarded row bound.
		refusal.conservativeCompressionL2Gain=Detail::NextUp(std::sqrt(2.0));
		// On four equal-volume cells, a=b=(2,0,0,0) has RMS(a)=RMS(b)=1
		// but RMS(a*b)=2.  RMS products therefore do not control coefficient terms.
		refusal.localizedProductRMS=2.0;refusal.productOfRMS=1.0;
		// y=c+min(alpha_a,alpha_b)d: moving alpha_b from .5 to .6 changes
		// component a by .1d.  Shared alpha makes the stage Jacobian non-diagonal.
		refusal.sharedAlphaCrossComponentResponse=0.1;
		refusal.fctUnitGainRejected=refusal.conservativeCompressionL2Gain>1.0;
		refusal.rmsProductRejected=refusal.localizedProductRMS>
			refusal.productOfRMS;
		refusal.componentDiagonalRejected=
			refusal.sharedAlphaCrossComponentResponse>0.0;
		return refusal.fctUnitGainRejected&&refusal.rmsProductRejected&&
			refusal.componentDiagonalRejected;
	}

	// This function retains the rejected r135 candidate arithmetic so the exact
	// refusal is reproducible.  It must not publish a certificate until all eight
	// proof gaps below are replaced by independent, state-local bounds.
	inline bool DeriveFullStepRoundoffBound(const std::array<FullStepMetricStage,24>& stages,
		const std::size_t cellCount,const double densityLower,const double densityUpper,
		const double spacing,const double timeStep,const double ambientDensity,
		const double maximumBeginningVelocity,const double beginningDivisionMaximumRadius,
		const double physicalCorrectionL2,const double restorationCorrectionL2,
		const double physicalLocalProjectionBound,const double restorationLocalProjectionBound,
		FullStepRoundoffCertificate& certificate,
		const FullStepGraphVariant variant=FullStepGraphVariant::Certified)
	{
		certificate=FullStepRoundoffCertificate();
		if(!cellCount||!(densityLower>0.0&&densityUpper>=densityLower&&spacing>0.0&&
			timeStep>0.0&&ambientDensity>0.0&&maximumBeginningVelocity>=0.0&&
			beginningDivisionMaximumRadius>=0.0&&physicalCorrectionL2>=0.0&&
			restorationCorrectionL2>=0.0&&physicalLocalProjectionBound>=0.0&&
			restorationLocalProjectionBound>=0.0))return false;
		const unsigned int scalarStages[]={1u,2u,3u,4u,5u,21u};
		for(unsigned int component=0u;component<9u;++component){
			for(const unsigned int stage:scalarStages){
				if(!stages[stage].count[component])return false;
				if(variant==FullStepGraphVariant::MissingCellStage&&stage==3u)continue;
				const double count=static_cast<double>(stages[stage].count[component]);
				certificate.scalarFilteredL1Upper[component]=Detail::NextUp(
					certificate.scalarFilteredL1Upper[component]+
					stages[stage].radiusSum[component]/count);
				certificate.scalarRMSUpper[component]=Detail::NextUp(
					certificate.scalarRMSUpper[component]+std::sqrt(
						stages[stage].radiusSquareSum[component]/count));
			}
			certificate.inventoryPerVolumeUpper[component]=
				certificate.scalarFilteredL1Upper[component];
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			const unsigned int channel=9u+axis;
			const unsigned int first=axis?6u+5u*axis:0u;
			if(!stages[0].count[channel])return false;
			certificate.momentumL2PerCellUpper[axis]=Detail::NextUp(std::sqrt(
				stages[0].radiusSquareSum[channel]/static_cast<double>(cellCount)));
			for(unsigned int offset=0u;offset<5u;++offset){const unsigned int stage=
				axis==0u?6u+offset:first+offset;
				if(!stages[stage].count[channel])return false;
				certificate.momentumL2PerCellUpper[axis]=Detail::NextUp(
					certificate.momentumL2PerCellUpper[axis]+std::sqrt(
						stages[stage].radiusSquareSum[channel]/static_cast<double>(cellCount)));
			}
		}
		for(unsigned int component=1u;component<=6u;++component)
			certificate.gasDensityRMSUpper=Detail::NextUp(
				certificate.gasDensityRMSUpper+certificate.scalarRMSUpper[component]);
		certificate.densityRelativeUpper=variant==FullStepGraphVariant::MissingDensityInteraction?
			0.0:Detail::NextUp(certificate.gasDensityRMSUpper/densityLower);
		double momentumVector=0.0;
		for(const double value:certificate.momentumL2PerCellUpper)
			momentumVector=Detail::NextUp(momentumVector+value*value);
		momentumVector=Detail::NextUp(std::sqrt(momentumVector));
		const double uniqueFaces=static_cast<double>(stages[0].count[9]+stages[0].count[10]+
			stages[0].count[11]);
		const double faceMetricGain=Detail::NextUp(std::sqrt(uniqueFaces/
			static_cast<double>(cellCount)));
		const double divisionRounding=Detail::NextUp(beginningDivisionMaximumRadius*
			faceMetricGain);
		certificate.provisionalVelocityL2PerCellUpper=Detail::NextUp(
			momentumVector/densityLower+divisionRounding+
			certificate.densityRelativeUpper*maximumBeginningVelocity);
		const double conditionGain=Detail::NextUp(std::sqrt(densityUpper/densityLower));
		const double coefficientPhysical=Detail::NextUp(
			certificate.densityRelativeUpper*physicalCorrectionL2);
		// All unique faces is a structural upper bound on the pressure-open subset.
		const double boundaryMetric=faceMetricGain;
		certificate.openBoundaryInteractionUpper=Detail::NextUp(
			(2.0*timeStep/spacing)*(ambientDensity/densityLower)*boundaryMetric*
			Detail::NextUp(2.0*maximumBeginningVelocity*
				certificate.provisionalVelocityL2PerCellUpper+
				certificate.provisionalVelocityL2PerCellUpper*
				certificate.provisionalVelocityL2PerCellUpper));
		const double physicalInput=variant==FullStepGraphVariant::MissingPhysicalFeedthrough?
			0.0:conditionGain*certificate.provisionalVelocityL2PerCellUpper;
		certificate.physicalProjectionVelocityUpper=Detail::NextUp(physicalInput+
			coefficientPhysical+certificate.openBoundaryInteractionUpper+
			physicalLocalProjectionBound);
		const double restorationInput=variant==FullStepGraphVariant::
			MissingRestorationFeedthrough?0.0:
			conditionGain*certificate.physicalProjectionVelocityUpper;
		certificate.finalVelocityRMSUpper=Detail::NextUp(restorationInput+
			certificate.densityRelativeUpper*restorationCorrectionL2+
			restorationLocalProjectionBound);
		certificate.proofGapBitmap=FullStepNonlinearFCTGain|
			FullStepDensityLInfinity|FullStepProjectionResolvent|
			FullStepOpenQuadraticLInfinity|FullStepMomentumSourceMetric|
			FullStepGasReductionMetric|FullStepAllSlices|
			FullStepIndependentCardinality;
		certificate.scalarTransportNonexpansive=false;
		certificate.dualTransportNonexpansive=false;
		certificate.projectionInteractionsIncluded=false;
		certificate.proofComplete=false;
		return false;
	}

	// The periodic/open swept integral is continuous when its integer partition
	// crosses a floor boundary: the cell moved between the fractional tail and
	// whole-cell path is the same PPM polynomial integral.  The exact excursion
	// is bounded by 2*M*delta (the common transport derivative sharpens this to
	// M, while projection interpolation may span values of opposite sign). The
	// alternate prefix/full-cell evaluation has at
	// most 32 scalar operations plus 24 per cell; gamma_n and an explicit FTZ
	// allowance cover its independently rounded realization.
	inline bool CertifyFloorPartition(const double predicateCenter,
		const double predicateRadius,const double profileAbsoluteUpper,
		const std::size_t lineLength,FloorPartitionCertificate& certificate,
		const FloorGraphVariant variant=FloorGraphVariant::Certified)
	{
		certificate=FloorPartitionCertificate();
		if(!(predicateRadius>=0.0&&profileAbsoluteUpper>=0.0)||!lineLength||
			!std::isfinite(predicateCenter)||!std::isfinite(predicateRadius)||
			!std::isfinite(profileAbsoluteUpper))return false;
		const double fullAmbiguity=Detail::NextUp(std::fabs(predicateCenter)+
			predicateRadius);
		certificate.ambiguityWidth=variant==FloorGraphVariant::HalfAmbiguity?
			0.5*fullAmbiguity:fullAmbiguity;
		certificate.alternatePathOperationCount=32u+(variant==
			FloorGraphVariant::MissingCyclePath?0u:24u*lineLength);
		certificate.exactBranchTerm=Detail::NextUp(2.0*profileAbsoluteUpper*
			certificate.ambiguityWidth);
		const double product=static_cast<double>(certificate.alternatePathOperationCount)*
			0x1p-24;
		if(!(product<1.0))return false;
		const double gamma=Detail::NextUp(product/(1.0-product));
		const double magnitude=Detail::NextUp(profileAbsoluteUpper*
			(static_cast<double>(lineLength)+4.0));
		certificate.roundedTerm=variant==FloorGraphVariant::MissingRoundedTerm?0.0:
			Detail::NextUp(gamma*magnitude);
		certificate.ftzTerm=Detail::NextUp(static_cast<double>(
			certificate.alternatePathOperationCount)*static_cast<double>(
				std::numeric_limits<float>::min()));
		certificate.totalEnvelope=Detail::NextUp(certificate.exactBranchTerm+
			certificate.roundedTerm+certificate.ftzTerm);
		certificate.continuousAtInteger=true;
		const double requiredExact=Detail::NextUp(2.0*profileAbsoluteUpper*fullAmbiguity);
		const std::uint64_t requiredOperations=32u+24u*lineLength;
		const double requiredProduct=static_cast<double>(requiredOperations)*0x1p-24;
		const double requiredRounded=Detail::NextUp(requiredProduct/(1.0-requiredProduct)*
			magnitude);
		const double requiredFTZ=Detail::NextUp(static_cast<double>(requiredOperations)*
			static_cast<double>(std::numeric_limits<float>::min()));
		return certificate.continuousAtInteger&&std::isfinite(certificate.totalEnvelope)&&
			certificate.exactBranchTerm>=requiredExact&&
			certificate.roundedTerm>=requiredRounded&&certificate.ftzTerm>=requiredFTZ;
	}

	// At remaining==0 the taken loop body integrates a zero-width interval.
	// Across an ambiguous positive remainder, the exact omitted/extra slice is
	// bounded by M*delta. One PPM cell integral plus the loop updates has forty
	// scalar operations; its gamma_40 and FTZ terms are carried separately.
	inline bool CertifyRemainingPositive(const double predicateCenter,
		const double predicateRadius,const double profileAbsoluteUpper,
		RemainingPositiveCertificate& certificate,const RemainingGraphVariant variant=
			RemainingGraphVariant::Certified)
	{
		certificate=RemainingPositiveCertificate();
		if(!(predicateRadius>=0.0&&profileAbsoluteUpper>=0.0)||
			!std::isfinite(predicateCenter)||!std::isfinite(predicateRadius)||
			!std::isfinite(profileAbsoluteUpper))return false;
		const double fullAmbiguity=Detail::NextUp(std::fabs(predicateCenter)+
			predicateRadius);
		certificate.ambiguityWidth=variant==RemainingGraphVariant::HalfAmbiguity?
			0.5*fullAmbiguity:fullAmbiguity;
		certificate.alternatePathOperationCount=variant==
			RemainingGraphVariant::MissingCellIntegral?8u:40u;
		certificate.exactBranchTerm=Detail::NextUp(profileAbsoluteUpper*
			certificate.ambiguityWidth);
		const double product=static_cast<double>(certificate.alternatePathOperationCount)*
			0x1p-24;
		if(!(product<1.0))return false;
		const double gamma=Detail::NextUp(product/(1.0-product));
		const double magnitude=Detail::NextUp(profileAbsoluteUpper*
			(4.0+certificate.ambiguityWidth));
		certificate.roundedTerm=Detail::NextUp(gamma*magnitude);
		certificate.ftzTerm=variant==RemainingGraphVariant::MissingFTZ?0.0:
			Detail::NextUp(static_cast<double>(certificate.alternatePathOperationCount)*
				static_cast<double>(std::numeric_limits<float>::min()));
		certificate.totalEnvelope=Detail::NextUp(certificate.exactBranchTerm+
			certificate.roundedTerm+certificate.ftzTerm);
		certificate.zeroWidthAtSwitch=true;
		const double requiredExact=Detail::NextUp(profileAbsoluteUpper*fullAmbiguity);
		const double requiredProduct=40.0*0x1p-24;
		const double requiredRounded=Detail::NextUp(requiredProduct/(1.0-requiredProduct)*
			Detail::NextUp(profileAbsoluteUpper*(4.0+fullAmbiguity)));
		const double requiredFTZ=Detail::NextUp(40.0*static_cast<double>(
			std::numeric_limits<float>::min()));
		return certificate.zeroWidthAtSwitch&&std::isfinite(certificate.totalEnvelope)&&
			certificate.exactBranchTerm>=requiredExact&&
			certificate.roundedTerm>=requiredRounded&&certificate.ftzTerm>=requiredFTZ;
	}

	// Both swept-volume orientations vanish at Courant zero. Across ambiguity
	// delta their one-cell donor slopes can differ by at most 2M, hence 2Mdelta.
	// The independently enumerated common setup plus the longer negative/local
	// successor is bounded by 48 scalar operations. The >= comparison also sends
	// both IEEE signed zeros through the positive path.
	inline bool CertifyCourantSign(const double predicateCenter,
		const double predicateRadius,const double profileAbsoluteUpper,
		CourantSignCertificate& certificate,const CourantGraphVariant variant=
			CourantGraphVariant::Certified)
	{
		certificate=CourantSignCertificate();
		if(!(predicateRadius>=0.0&&profileAbsoluteUpper>=0.0)||
			!std::isfinite(predicateCenter)||!std::isfinite(predicateRadius)||
			!std::isfinite(profileAbsoluteUpper))return false;
		const double fullAmbiguity=Detail::NextUp(std::fabs(predicateCenter)+
			predicateRadius);
		certificate.ambiguityWidth=variant==CourantGraphVariant::HalfAmbiguity?
			0.5*fullAmbiguity:fullAmbiguity;
		certificate.profileAbsoluteUpper=profileAbsoluteUpper;
		certificate.commonSetupOperationCount=8u;
		certificate.positiveSuccessorOperationCount=32u;
		certificate.negativeSuccessorOperationCount=variant==
			CourantGraphVariant::MissingNegativePath?0u:40u;
		certificate.alternatePathOperationCount=certificate.commonSetupOperationCount+
			std::max(certificate.positiveSuccessorOperationCount,
				certificate.negativeSuccessorOperationCount);
		certificate.exactBranchTerm=Detail::NextUp(2.0*profileAbsoluteUpper*
			certificate.ambiguityWidth);
		const double product=static_cast<double>(certificate.alternatePathOperationCount)*
			0x1p-24;
		if(!(product<1.0))return false;
		const double magnitude=Detail::NextUp(profileAbsoluteUpper*
			(8.0+certificate.ambiguityWidth));
		const double gamma=Detail::NextUp(product/(1.0-product));
		certificate.roundedTerm=Detail::NextUp(gamma*magnitude);
		certificate.ftzTerm=variant==CourantGraphVariant::MissingFTZ?0.0:
			Detail::NextUp(static_cast<double>(certificate.alternatePathOperationCount)*
				static_cast<double>(std::numeric_limits<float>::min()));
		certificate.totalEnvelope=Detail::NextUp(certificate.exactBranchTerm+
			certificate.roundedTerm+certificate.ftzTerm);
		certificate.continuousAtZero=true;
		certificate.signedZeroCanonical=variant!=CourantGraphVariant::NoncanonicalSignedZero;
		const double requiredExact=Detail::NextUp(2.0*profileAbsoluteUpper*fullAmbiguity);
		const double requiredProduct=48.0*0x1p-24;
		const double requiredGamma=Detail::NextUp(requiredProduct/(1.0-requiredProduct));
		const double requiredRounded=Detail::NextUp(requiredGamma*
			Detail::NextUp(profileAbsoluteUpper*(8.0+fullAmbiguity)));
		const double requiredFTZ=Detail::NextUp(48.0*static_cast<double>(
			std::numeric_limits<float>::min()));
		return certificate.continuousAtZero&&certificate.signedZeroCanonical&&
			std::isfinite(certificate.totalEnvelope)&&
			certificate.exactBranchTerm>=requiredExact&&
			certificate.roundedTerm>=requiredRounded&&certificate.ftzTerm>=requiredFTZ;
	}

	// At fractional==0 the optional trailing PPM interval has zero measure.
	// Across ambiguity delta its exact contribution is Mdelta. The independent
	// trailing-polynomial and accumulation DAG has 24 scalar operations.
	inline bool CertifyFractionPositive(const double predicateCenter,
		const double predicateRadius,const double profileAbsoluteUpper,
		FractionPositiveCertificate& certificate,const FractionGraphVariant variant=
			FractionGraphVariant::Certified)
	{
		certificate=FractionPositiveCertificate();
		if(!(predicateRadius>=0.0&&profileAbsoluteUpper>=0.0)||
			!std::isfinite(predicateCenter)||!std::isfinite(predicateRadius)||
			!std::isfinite(profileAbsoluteUpper))return false;
		const double fullAmbiguity=Detail::NextUp(std::fabs(predicateCenter)+
			predicateRadius);
		certificate.ambiguityWidth=variant==FractionGraphVariant::HalfAmbiguity?
			0.5*fullAmbiguity:fullAmbiguity;
		certificate.tailOperationCount=variant==FractionGraphVariant::MissingTrailingIntegral?
			6u:24u;
		certificate.exactBranchTerm=Detail::NextUp(profileAbsoluteUpper*
			certificate.ambiguityWidth);
		const double product=static_cast<double>(certificate.tailOperationCount)*0x1p-24;
		if(!(product<1.0))return false;
		const double gamma=Detail::NextUp(product/(1.0-product));
		const double magnitude=Detail::NextUp(profileAbsoluteUpper*
			(4.0+certificate.ambiguityWidth));
		certificate.roundedTerm=Detail::NextUp(gamma*magnitude);
		certificate.ftzTerm=variant==FractionGraphVariant::MissingFTZ?0.0:
			Detail::NextUp(static_cast<double>(certificate.tailOperationCount)*
				static_cast<double>(std::numeric_limits<float>::min()));
		certificate.totalEnvelope=Detail::NextUp(certificate.exactBranchTerm+
			certificate.roundedTerm+certificate.ftzTerm);
		certificate.zeroWidthAtSwitch=true;
		const double requiredExact=Detail::NextUp(profileAbsoluteUpper*fullAmbiguity);
		const double requiredProduct=24.0*0x1p-24;
		const double requiredGamma=Detail::NextUp(requiredProduct/(1.0-requiredProduct));
		const double requiredRounded=Detail::NextUp(requiredGamma*
			Detail::NextUp(profileAbsoluteUpper*(4.0+fullAmbiguity)));
		const double requiredFTZ=Detail::NextUp(24.0*static_cast<double>(
			std::numeric_limits<float>::min()));
		return certificate.zeroWidthAtSwitch&&std::isfinite(certificate.totalEnvelope)&&
			certificate.exactBranchTerm>=requiredExact&&
			certificate.roundedTerm>=requiredRounded&&certificate.ftzTerm>=requiredFTZ;
	}

	// The general PPM integral and its flat shortcut coincide for l=c=r. Over
	// independently enclosed endpoint deviations D, the unit-cell polynomial
	// integral differs from the flat value by at most 8D. The curved polynomial
	// and accumulation DAG has 32 scalar operations.
	inline bool CertifyFlatIntegral(const double leftCenter,const double leftRadius,
		const double centerCenter,const double centerRadius,const double rightCenter,
		const double rightRadius,const double profileAbsoluteUpper,
		FlatIntegralCertificate& certificate,const FlatIntegralGraphVariant variant=
			FlatIntegralGraphVariant::Certified)
	{
		certificate=FlatIntegralCertificate();
		if(!(leftRadius>=0.0&&centerRadius>=0.0&&rightRadius>=0.0&&
			profileAbsoluteUpper>=0.0)||!std::isfinite(leftCenter)||
			!std::isfinite(centerCenter)||!std::isfinite(rightCenter)||
			!std::isfinite(profileAbsoluteUpper))return false;
		const double fullDeviation=std::max(
			Detail::NextUp(std::fabs(leftCenter-centerCenter)+leftRadius+centerRadius),
			Detail::NextUp(std::fabs(rightCenter-centerCenter)+rightRadius+centerRadius));
		certificate.deviationUpper=variant==FlatIntegralGraphVariant::HalfDeviation?
			0.5*fullDeviation:fullDeviation;
		const double qCenter=6.0;
		const double qLeft=variant==FlatIntegralGraphVariant::MutatedQuadraticCoefficient?
			-2.0:-3.0;
		const double qRight=-3.0;
		certificate.productionGraphMatched=qCenter==6.0&&qLeft==-3.0&&qRight==-3.0&&
			qCenter+qLeft+qRight==0.0;
		certificate.curvedPathOperationCount=variant==
			FlatIntegralGraphVariant::MissingCurvedPolynomial?8u:32u;
		certificate.exactBranchTerm=Detail::NextUp(8.0*certificate.deviationUpper);
		const double product=static_cast<double>(certificate.curvedPathOperationCount)*0x1p-24;
		if(!(product<1.0))return false;
		const double gamma=Detail::NextUp(product/(1.0-product));
		const double magnitude=Detail::NextUp(profileAbsoluteUpper*
			(8.0+certificate.deviationUpper));
		certificate.roundedTerm=Detail::NextUp(gamma*magnitude);
		certificate.ftzTerm=variant==FlatIntegralGraphVariant::MissingFTZ?0.0:
			Detail::NextUp(static_cast<double>(certificate.curvedPathOperationCount)*
				static_cast<double>(std::numeric_limits<float>::min()));
		certificate.totalEnvelope=Detail::NextUp(certificate.exactBranchTerm+
			certificate.roundedTerm+certificate.ftzTerm);
		certificate.continuousAtFlatProfile=variant!=
			FlatIntegralGraphVariant::DiscontinuousFlatPath;
		const double requiredExact=Detail::NextUp(8.0*fullDeviation);
		const double requiredProduct=32.0*0x1p-24;
		const double requiredGamma=Detail::NextUp(requiredProduct/(1.0-requiredProduct));
		const double requiredRounded=Detail::NextUp(requiredGamma*
			Detail::NextUp(profileAbsoluteUpper*(8.0+fullDeviation)));
		const double requiredFTZ=Detail::NextUp(32.0*static_cast<double>(
			std::numeric_limits<float>::min()));
		return certificate.productionGraphMatched&&certificate.continuousAtFlatProfile&&
			std::isfinite(certificate.totalEnvelope)&&
			certificate.exactBranchTerm>=requiredExact&&
			certificate.roundedTerm>=requiredRounded&&certificate.ftzTerm>=requiredFTZ;
	}

	// Independent topology walk for max_i(abs(r_i)). IEEE absolute value and
	// maximum preserve nonnegativity exactly, including underflow/FTZ, so the
	// negative guard's alternate path is unreachable and all three envelope
	// terms are exactly zero. This proof is about sign topology, not magnitude.
	inline bool CertifyNonnegativeMaximumReduction(const std::vector<double>& residuals,
		NonnegativeReductionCertificate& certificate,
		const NonnegativeReductionGraphVariant variant=
			NonnegativeReductionGraphVariant::Certified)
	{
		certificate=NonnegativeReductionCertificate();
		if(residuals.empty())return false;
		certificate.leafCount=residuals.size();
		certificate.positiveZeroSeed=variant!=NonnegativeReductionGraphVariant::NegativeSeed;
		certificate.allLeavesAbsolute=variant!=NonnegativeReductionGraphVariant::SignedLeaf;
		certificate.maximumOnly=variant!=NonnegativeReductionGraphVariant::SubtractiveReduction;
		double reduced=certificate.positiveZeroSeed?0.0:-1.0;
		for(const double residual:residuals){
			if(!std::isfinite(residual))return false;
			const double leaf=certificate.allLeavesAbsolute?std::fabs(residual):residual;
			++certificate.absoluteCount;
			reduced=certificate.maximumOnly?std::max(reduced,leaf):reduced-leaf;
			++certificate.maximumCount;
		}
		certificate.exactBranchTerm=0.0;certificate.roundedTerm=0.0;
		certificate.ftzTerm=0.0;certificate.totalEnvelope=0.0;
		return certificate.positiveZeroSeed&&certificate.allLeavesAbsolute&&
			certificate.maximumOnly&&reduced>=0.0&&certificate.absoluteCount==residuals.size()&&
			certificate.maximumCount==residuals.size();
	}

	// The r127 donor is a convex linear transition from nearest to ambient over
	// [-w,+w]. Each outer branch meets the ramp exactly at its join. The exact
	// alternate-path excursion is donorContrast*delta/(2*wLower); twelve rounded
	// scalar operations and their FTZ allowance are carried independently.
	inline bool CertifyInflowTransition(const double predicateCenter,
		const double predicateRadius,const double widthCenter,const double widthRadius,
		const double nearestCenter,const double nearestRadius,const double ambientCenter,
		const double ambientRadius,InflowTransitionCertificate& certificate,
		const InflowGraphVariant variant=InflowGraphVariant::Certified)
	{
		certificate=InflowTransitionCertificate();
		if(!(predicateRadius>=0.0&&widthRadius>=0.0&&nearestRadius>=0.0&&
			ambientRadius>=0.0)||!std::isfinite(predicateCenter)||
			!std::isfinite(predicateRadius)||!std::isfinite(widthCenter)||
			!std::isfinite(widthRadius)||!std::isfinite(nearestCenter)||
			!std::isfinite(ambientCenter))return false;
		certificate.widthLower=widthCenter-widthRadius;
		if(!(certificate.widthLower>0.0))return false;
		const double fullAmbiguity=Detail::NextUp(std::fabs(predicateCenter)+
			predicateRadius);
		certificate.ambiguityWidth=variant==InflowGraphVariant::HalfAmbiguity?
			0.5*fullAmbiguity:fullAmbiguity;
		certificate.donorContrast=variant==InflowGraphVariant::MissingDonorContrast?0.0:
			Detail::NextUp(std::fabs(ambientCenter-nearestCenter)+ambientRadius+nearestRadius);
		certificate.exactBranchTerm=Detail::NextUp(certificate.donorContrast*
			certificate.ambiguityWidth/(2.0*certificate.widthLower));
		const double maximumDonor=std::max(std::fabs(nearestCenter)+nearestRadius,
			std::fabs(ambientCenter)+ambientRadius);
		const double magnitude=Detail::NextUp(4.0*maximumDonor+
			Detail::NextUp(std::fabs(ambientCenter-nearestCenter)+ambientRadius+nearestRadius));
		const double product=12.0*0x1p-24;
		certificate.roundedTerm=variant==InflowGraphVariant::MissingRoundedTerm?0.0:
			Detail::NextUp(product/(1.0-product)*magnitude);
		certificate.ftzTerm=Detail::NextUp(12.0*static_cast<double>(
			std::numeric_limits<float>::min()));
		certificate.totalEnvelope=Detail::NextUp(certificate.exactBranchTerm+
			certificate.roundedTerm+certificate.ftzTerm);
		certificate.continuousAtJoin=variant!=InflowGraphVariant::DiscontinuousBinary;
		certificate.convexDonorBlend=true;certificate.legacyOutsideWidth=true;
		const double requiredContrast=Detail::NextUp(std::fabs(ambientCenter-nearestCenter)+
			ambientRadius+nearestRadius);
		const double requiredExact=Detail::NextUp(requiredContrast*fullAmbiguity/
			(2.0*certificate.widthLower));
		const double requiredRounded=Detail::NextUp(product/(1.0-product)*magnitude);
		return certificate.continuousAtJoin&&certificate.convexDonorBlend&&
			certificate.legacyOutsideWidth&&std::isfinite(certificate.totalEnvelope)&&
			certificate.exactBranchTerm>=requiredExact&&
			certificate.roundedTerm>=requiredRounded;
	}

	inline bool DeriveInflowTransitionWidth(const double predicateAmbiguity,
		const double naturalScale,const double proposedPowerOfTwoFactor,
		double& derivedPowerOfTwoFactor,double& derivedWidth)
	{
		derivedPowerOfTwoFactor=0.0;derivedWidth=0.0;
		if(!(predicateAmbiguity>=0.0)||!(naturalScale>0.0)||
			!std::isfinite(predicateAmbiguity)||!std::isfinite(naturalScale)||
			!(proposedPowerOfTwoFactor>0.0)||!std::isfinite(proposedPowerOfTwoFactor))
			return false;
		const double normalized=std::max(1.0,predicateAmbiguity/(0x1p-24*naturalScale));
		derivedPowerOfTwoFactor=std::exp2(std::ceil(std::log2(normalized)));
		derivedWidth=derivedPowerOfTwoFactor*0x1p-24*naturalScale;
		return std::isfinite(derivedWidth)&&derivedWidth>0.0&&
			proposedPowerOfTwoFactor==derivedPowerOfTwoFactor;
	}

	// Independent branch-equivalence proof for ParabolaDeviationRange.  With
	// endpoint deviations l and r, the quadratic path is the endpoint chord
	// minus q*s*(1-s), q=3(l+r).  It therefore coincides with the endpoint-only
	// path at q=0 and differs by at most |q|/4 on s in [0,1].
	inline bool CertifyPPMQuadraticZero(const BranchWitness& witness,
		PPMQuadraticZeroCertificate& certificate)
	{
		certificate=PPMQuadraticZeroCertificate();
		if(witness.rightCenter!=0.0||witness.rightRadius!=0.0||
			IntervalsAreSeparated(witness.leftCenter,witness.leftRadius,
				witness.rightCenter,witness.rightRadius))return false;
		certificate.continuousAtSwitch=true;
		certificate.ambiguityWidth=Detail::NextUp(std::fabs(witness.leftCenter)+
			witness.leftRadius);
		const double unit=0x1p-24;
		const double gamma4=Detail::NextUp((4.0*unit)/(1.0-4.0*unit));
		const double operationMagnitude=Detail::NextUp(certificate.ambiguityWidth+
			std::fabs(witness.linearCenter)+witness.linearRadius+
			witness.endpointAbsoluteUpper);
		const double underflowAllowance=4.0*
			static_cast<double>(std::numeric_limits<float>::min());
		certificate.arithmeticResidualBound=Detail::NextUp(witness.leftRadius+
			witness.linearRadius+witness.endpointRadius+
			gamma4*operationMagnitude+underflowAllowance);
		certificate.divergenceBound=Detail::NextUp(
			0.25*certificate.ambiguityWidth+certificate.arithmeticResidualBound);
		return std::isfinite(certificate.divergenceBound)&&
			certificate.divergenceBound>=0.0;
	}

	inline double PPMQuadraticPathDifference(const double quadratic,const double s)
	{
		return -quadratic*s*(1.0-s);
	}

	inline bool ContinuousSelectionHull(const double firstCenter,const double firstRadius,
		const double secondCenter,const double secondRadius,const bool minimum,
		double& lower,double& upper,double& divergence)
	{
		lower=upper=divergence=0.0;
		if(!(firstRadius>=0.0&&secondRadius>=0.0))return false;
		const double firstLow=firstCenter-firstRadius,firstHigh=firstCenter+firstRadius;
		const double secondLow=secondCenter-secondRadius,secondHigh=secondCenter+secondRadius;
		lower=minimum?std::min(firstLow,secondLow):std::max(firstLow,secondLow);
		upper=minimum?std::min(firstHigh,secondHigh):std::max(firstHigh,secondHigh);
		divergence=Detail::NextUp(firstRadius+secondRadius);
		return lower<=upper&&std::isfinite(lower)&&std::isfinite(upper)&&
			std::isfinite(divergence);
	}

	inline bool CertifyInactiveLimiter(const double alphaUpper,
		const double numeratorLower,const double deviationUpper)
	{
		return alphaUpper>=0.0&&numeratorLower>=0.0&&deviationUpper>=0.0&&
			numeratorLower>=Detail::NextUp(alphaUpper*deviationUpper);
	}

	// Fail-fast independent walk of the first x-half-step limiter branch in the
	// resident cell transport graph.  It repacks the public SoA bytes and
	// re-derives the unlimited PPM edge DAG locally; it does not call the traced
	// or production remap implementation.  The r120 derivation cannot proceed
	// past a branch found here, so later-stage topology need not be trusted for
	// this refusal.
	inline bool WalkFirstCellXLimiterBranch(const std::size_t nx,const std::size_t ny,
		const std::size_t nz,const std::size_t componentCount,const bool periodic,
		const bool lowerOpen,const bool upperOpen,const std::vector<float>& values,
		const std::vector<float>& ambient,const std::vector<float>& xVelocity,
		BranchWitness& witness)
	{
		witness=BranchWitness();
		if(nx<4u||!ny||!nz||!componentCount||values.size()!=nx*ny*nz*componentCount||
			ambient.size()!=componentCount||xVelocity.size()!=(nx+1u)*ny*nz)return false;
		const std::size_t cells=nx*ny*nz,lines=ny*nz;
		auto sample=[&](const std::size_t component,const std::size_t line,long x){
			if(periodic){const long count=static_cast<long>(nx);x%=count;if(x<0)x+=count;}
			else if(x<0){
				if(lowerOpen&&xVelocity[line*(nx+1u)]>0.0f)
					return Detail::Interval(ambient[component]);
				x=0;
			}
			else if(x>=static_cast<long>(nx)){if(upperOpen&&
				xVelocity[line*(nx+1u)+nx]<0.0f)return Detail::Interval(ambient[component]);
				x=static_cast<long>(nx)-1;}
			const std::size_t y=line%ny,z=line/ny;
			return Detail::Interval(values[component*cells+(z*ny+y)*nx+
				static_cast<std::size_t>(x)]);
		};
		const Detail::Interval zero(0.0f),two(2.0f),three(3.0f),seven(7.0f),
			twelve(12.0f);
		for(std::size_t line=0u;line<lines;++line)for(std::size_t cell=0u;cell<nx;++cell)
			for(std::size_t component=0u;component<componentCount;++component){
				const long index=static_cast<long>(cell);
				const Detail::Interval im2=sample(component,line,index-2);
				const Detail::Interval im1=sample(component,line,index-1);
				const Detail::Interval center=sample(component,line,index);
				const Detail::Interval ip1=sample(component,line,index+1);
				const Detail::Interval ip2=sample(component,line,index+2);
				if(im2.rounded==center.rounded&&im1.rounded==center.rounded&&
					ip1.rounded==center.rounded&&ip2.rounded==center.rounded)continue;
				const Detail::Interval left=Detail::Divide(Detail::Subtract(
					Detail::Multiply(seven,Detail::Add(im1,center)),Detail::Add(im2,ip1)),
					twelve);
				const Detail::Interval right=Detail::Divide(Detail::Subtract(
					Detail::Multiply(seven,Detail::Add(center,ip1)),Detail::Add(im1,ip2)),
					twelve);
				const Detail::Interval leftDeviation=Detail::Subtract(left,center);
				const Detail::Interval rightDeviation=Detail::Subtract(right,center);
				if(Detail::CaptureIfUnresolved(rightDeviation,leftDeviation,
					rightDeviation.rounded<leftDeviation.rounded,line,cell,component,witness))
					return true;
				if(Detail::CaptureIfUnresolved(leftDeviation,rightDeviation,
					leftDeviation.rounded<rightDeviation.rounded,line,cell,component,witness))
					return true;
				const Detail::Interval quadratic=Detail::Multiply(three,
					Detail::Add(leftDeviation,rightDeviation));
				const Detail::Interval linear=Detail::Subtract(
					Detail::Multiply(Detail::Interval(-4.0f),leftDeviation),
					Detail::Multiply(two,rightDeviation));
				if(Detail::CaptureIfUnresolved(quadratic,zero,quadratic.rounded!=0.0f,
					line,cell,component,witness)){
					witness.linearCenter=linear.center;witness.linearRadius=linear.radius;
					witness.endpointAbsoluteUpper=std::max(
						std::fabs(leftDeviation.center)+leftDeviation.radius,
						std::fabs(rightDeviation.center)+rightDeviation.radius);
					witness.endpointRadius=std::max(
						leftDeviation.radius,rightDeviation.radius);
					return true;
				}
				// The first canonical unresolved branch occurs above.  A resolved
				// quadratic would require the remaining stationary-point graph, which
				// belongs to the eventual full derivation rather than this fail-fast RED.
				(void)linear;
			}
		return false;
	}

	// Independent graph walk for the exact positive-subcell periodic free-stream
	// owner fixture.  Counts below come from the mathematical DAG, not from the
	// traced implementation: validation Courant products/quotients; two limiter
	// deviations; the five-operation parabola range; two three-operation limited
	// edges; the two Blelloch trees; the positive fractional swept integral; and
	// the terminal conservative flux difference.
	inline bool WalkPeriodicPositiveSubcellFreeStreamRemap(const std::size_t lineLength,
		const std::size_t lineCount,const std::size_t componentCount,Topology& topology)
	{
		topology=Topology();if(lineLength<2u||!lineCount||!componentCount)return false;
		const std::uint64_t lines=lineCount,components=componentCount,cells=lineLength;
		std::uint64_t lineComponents=0u,values=0u,faces=0u;
		if(!CheckedProduct(lines,components,lineComponents)||
			!CheckedProduct(lineComponents,cells,values)||
			cells==std::numeric_limits<std::uint64_t>::max()||
			!CheckedProduct(lines,cells+1u,faces))return false;
		const std::uint64_t swept=values;
		const std::uint64_t treeEdges=NextPowerOfTwo(lineLength)-1u;
		return CheckedAddOperation(topology,Convert,1u,values)&&
			CheckedAddOperation(topology,Add,7u,values)&&
			CheckedAddOperation(topology,Add,2u*treeEdges,lineComponents)&&
			CheckedAddOperation(topology,Subtract,11u,values)&&
			CheckedAddOperation(topology,Multiply,11u,values)&&
			CheckedAddOperation(topology,Multiply,1u,faces)&&
			CheckedAddOperation(topology,Divide,5u,values)&&
			CheckedAddOperation(topology,Divide,1u,faces)&&
			CheckedAddOperation(topology,Absolute,7u,values)&&
			CheckedAddOperation(topology,Floor,2u,values)&&
			CheckedAddOperation(topology,Remainder,1u,swept)&&
			(topology.maximumDepth=14u,true);
	}
}

#endif
