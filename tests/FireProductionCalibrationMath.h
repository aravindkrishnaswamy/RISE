#ifndef FIRE_PRODUCTION_CALIBRATION_MATH_H
#define FIRE_PRODUCTION_CALIBRATION_MATH_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace FireProductionCalibration
{
	constexpr std::size_t LongShadowSteps=104u;
	constexpr std::size_t LongShadowWindow=32u;

	inline bool EqualTimeReferenceSchedule(const double productionDurationS,
		const std::vector<double>& referenceSubstepsS,const double referenceEndS,
		const double terminalTargetTimeS,const std::string& terminalTargetDigest,
		const std::string& consumedTargetDigest)
	{
		if(!(productionDurationS>0.0)||!std::isfinite(productionDurationS)||
			referenceSubstepsS.empty())return false;
		const double expectedSubstep=productionDurationS/
			static_cast<double>(referenceSubstepsS.size());
		double accumulated=0.0;
		for(const double step:referenceSubstepsS){
			if(!(step>0.0)||!std::isfinite(step)||step!=expectedSubstep)return false;
			accumulated+=step;
		}
		return std::isfinite(accumulated)&&accumulated==productionDurationS&&
			referenceEndS==productionDurationS&&terminalTargetTimeS==referenceEndS&&
			!terminalTargetDigest.empty()&&consumedTargetDigest==terminalTargetDigest;
	}

	inline bool LongShadowNonsecular(const std::vector<double>& values)
	{
		if(values.size()!=LongShadowSteps)return false;
		const std::size_t first=values.size()-2u*LongShadowWindow;
		const double prior=*std::max_element(values.begin()+first,
			values.begin()+first+LongShadowWindow);
		const double terminal=*std::max_element(values.begin()+first+LongShadowWindow,
			values.end());
		double terminalTrend=0.0;
		for(std::size_t offset=0u;offset<LongShadowWindow/2u;++offset){
			const double weight=static_cast<double>(LongShadowWindow-1u-2u*offset);
			terminalTrend+=weight*(values[values.size()-1u-offset]-
				values[values.size()-LongShadowWindow+offset]);
		}
		return std::isfinite(prior)&&std::isfinite(terminal)&&terminal<=prior&&
			std::isfinite(terminalTrend)&&terminalTrend<=0.0&&terminal<=0x1p-5;
	}

	inline double NextUp(const double value)
	{
		return std::nextafter(value,std::numeric_limits<double>::infinity());
	}

	struct RoundoffStage
	{
		double amplification;
		double localRadius;
		std::size_t operationCount;
	};

	inline bool ComposeRoundoffBound(const RoundoffStage* stages,const std::size_t count,
		double& radius)
	{
		radius=0.0;
		const double unitRoundoff=0x1p-24;
		for(std::size_t stage=0u;stage<count;++stage){
			const RoundoffStage& current=stages[stage];
			if(!(current.amplification>=0.0&&current.localRadius>=0.0)||
				!std::isfinite(current.amplification)||!std::isfinite(current.localRadius)||
				static_cast<double>(current.operationCount)*unitRoundoff>=1.0)return false;
			radius=NextUp(NextUp(current.amplification*radius)+current.localRadius);
			if(!std::isfinite(radius))return false;
		}
		return true;
	}

	inline double GridRatioForOrder(const double order,const double h5,const double h6,
		const double h7)
	{
		return (std::pow(h5,order)-std::pow(h6,order))/
			(std::pow(h6,order)-std::pow(h7,order));
	}

	inline bool GeneralizedGridRichardson(const double difference56,const double difference67,
		const double h5,const double h6,const double h7,const double formalOrder,
		double& measuredOrder,double& tier6Distance)
	{
		measuredOrder=0.0;tier6Distance=0.0;
		if(!(difference56>difference67&&difference67>0.0&&h5>h6&&h6>h7&&h7>0.0&&
			formalOrder>0.0))return false;
		const double target=difference56/difference67;
		double lower=0x1p-20,upper=formalOrder;
		const double lowerRatio=GridRatioForOrder(lower,h5,h6,h7);
		const double upperRatio=GridRatioForOrder(upper,h5,h6,h7);
		if(target<lowerRatio)return false;
		if(target>=upperRatio){
			measuredOrder=formalOrder;
			const double denominator=1.0-std::pow(h7/h6,measuredOrder);
			tier6Distance=NextUp(difference67/denominator);
			return std::isfinite(tier6Distance);
		}
		for(unsigned int iteration=0u;iteration<96u;++iteration){
			const double middle=0.5*(lower+upper);
			if(GridRatioForOrder(middle,h5,h6,h7)<target)lower=middle;else upper=middle;
		}
		measuredOrder=std::min(formalOrder,0.5*(lower+upper));
		const double denominator=1.0-std::pow(h7/h6,measuredOrder);
		if(!(denominator>0.0))return false;
		tier6Distance=NextUp(difference67/denominator);
		return std::isfinite(tier6Distance);
	}

	inline bool TemporalRichardson(const double coarseDifference,const double fineDifference,
		const double formalOrder,double& measuredOrder,double& baselineDistance)
	{
		measuredOrder=0.0;baselineDistance=0.0;
		if(!(coarseDifference>fineDifference&&fineDifference>0.0&&formalOrder>0.0))return false;
		measuredOrder=std::min(formalOrder,std::log2(coarseDifference/fineDifference));
		const double denominator=1.0-std::pow(2.0,-measuredOrder);
		if(!(measuredOrder>0.0&&denominator>0.0))return false;
		baselineDistance=NextUp(coarseDifference/denominator);
		return std::isfinite(baselineDistance);
	}

	struct DyadicDistanceEstimate
	{
		double coarseDistance=0.0;
		double fineRadius=0.0;
	};

	inline bool DyadicDistanceAtVerifiedOrder(const double difference,const double verifiedOrder,
		DyadicDistanceEstimate& estimate)
	{
		estimate=DyadicDistanceEstimate();
		if(!(difference>0.0&&verifiedOrder>0.0)||!std::isfinite(difference)||
			!std::isfinite(verifiedOrder))return false;
		const double refinement=std::pow(2.0,verifiedOrder);
		if(!(refinement>1.0)||!std::isfinite(refinement))return false;
		estimate.coarseDistance=NextUp(difference/(1.0-1.0/refinement));
		estimate.fineRadius=NextUp(difference/(refinement-1.0));
		return std::isfinite(estimate.coarseDistance)&&std::isfinite(estimate.fineRadius);
	}

	inline bool DyadicLimitBallsOverlap(const double extrapolatedDifference,
		const DyadicDistanceEstimate& first,const DyadicDistanceEstimate& second)
	{
		if(!(extrapolatedDifference>=0.0)||!std::isfinite(extrapolatedDifference))return false;
		return extrapolatedDifference<=NextUp(first.fineRadius+second.fineRadius);
	}

	inline bool Tier6DistanceFromDyadicPairs(const double difference5To10,
		const double difference6To12,const double verifiedOrder,double& distance,
		double& subdominanceBound)
	{
		distance=0.0;subdominanceBound=0.0;
		DyadicDistanceEstimate first,second;
		if(!DyadicDistanceAtVerifiedOrder(difference5To10,verifiedOrder,first)||
			!DyadicDistanceAtVerifiedOrder(difference6To12,verifiedOrder,second))return false;
		const double rescaled=NextUp(first.coarseDistance*
			std::pow(5.0/6.0,verifiedOrder));
		distance=std::max(rescaled,second.coarseDistance);
		subdominanceBound=NextUp(0.125*distance);
		return std::isfinite(distance)&&std::isfinite(subdominanceBound)&&distance>0.0&&
			subdominanceBound>0.0;
	}

	inline bool TriangleTolerance(const double productionGrid,const double productionTime,
		const double oracleGrid,const double oracleTime,const double rounding,double& tolerance)
	{
		const double terms[5]={productionGrid,productionTime,oracleGrid,oracleTime,rounding};
		tolerance=0.0;
		for(const double term:terms){
			if(!(term>=0.0)||!std::isfinite(term))return false;
			tolerance=NextUp(tolerance+term);
		}
		return std::isfinite(tolerance);
	}
}

#endif
