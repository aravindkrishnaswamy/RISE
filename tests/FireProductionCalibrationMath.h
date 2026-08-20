#ifndef FIRE_PRODUCTION_CALIBRATION_MATH_H
#define FIRE_PRODUCTION_CALIBRATION_MATH_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace FireProductionCalibration
{
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
		if(target<lowerRatio||target>NextUp(upperRatio))return false;
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
