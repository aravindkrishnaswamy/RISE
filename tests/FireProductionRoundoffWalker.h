#ifndef FIRE_PRODUCTION_ROUNDOFF_WALKER_H
#define FIRE_PRODUCTION_ROUNDOFF_WALKER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

namespace FireProductionRoundoffWalker
{
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
