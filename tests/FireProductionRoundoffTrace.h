#ifndef FIRE_PRODUCTION_ROUNDOFF_TRACE_H
#define FIRE_PRODUCTION_ROUNDOFF_TRACE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace FireProductionRoundoffTrace
{
	enum class Operation : unsigned int { Convert,Add,Subtract,Multiply,Divide,Sqrt,Absolute,
		Minimum,Maximum,Floor,Ceil,Remainder,NextAfter,Count };

	struct Observation
	{
		std::uint64_t operation[static_cast<unsigned int>(Operation::Count)]={};
		double maximumAbsoluteOperand[static_cast<unsigned int>(Operation::Count)]={};
		double maximumResultRadius[static_cast<unsigned int>(Operation::Count)]={};
		std::uint32_t maximumDepth=0u;
		std::uint64_t comparisonCount=0u;
		double minimumBranchMargin=std::numeric_limits<double>::infinity();
		double minimumDenominatorLowerBound=std::numeric_limits<double>::infinity();
		double minimumSqrtDomainLowerBound=std::numeric_limits<double>::infinity();
		bool unresolvedBranch=false;
		bool invalidDomain=false;
		bool unresolvedWitnessRecorded=false;
		double unresolvedLeftCenter=0.0,unresolvedLeftRadius=0.0;
		double unresolvedRightCenter=0.0,unresolvedRightRadius=0.0;
		float unresolvedLeftRounded=0.0f,unresolvedRightRounded=0.0f;
		bool unresolvedRoundedResult=false;
		bool invalidDenominatorWitnessRecorded=false;
		double invalidDenominatorCenter=0.0,invalidDenominatorRadius=0.0;
		float invalidDenominatorRounded=0.0f;
		double maximumAbsoluteOutput=0.0;
		double maximumOutputRadius=0.0;
	};

	struct Counters : Observation
	{
		std::vector<Observation> sealedStages;
	};

	inline thread_local Counters* ActiveCounters=nullptr;

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

	class TraceFloat
	{
	public:
		TraceFloat():center_(0.0),radius_(0.0),rounded_(0.0f),depth_(0u){}
		TraceFloat(const float value):center_(value),radius_(0.0),rounded_(value),depth_(0u){}
		TraceFloat(const double value):TraceFloat(Convert(value)){}
		template<class Integer,typename std::enable_if<std::is_integral<Integer>::value,int>::type=0>
		TraceFloat(const Integer value):TraceFloat(Convert(static_cast<double>(value))){}

		static TraceFloat Raw(const double center,const double radius,const float rounded,
			const std::uint32_t depth)
		{
			TraceFloat result;result.center_=center;result.radius_=NextUp(radius);
			result.rounded_=rounded;result.depth_=depth;return result;
		}

		double Center()const{return center_;}
		double Radius()const{return radius_;}
		float Rounded()const{return rounded_;}
		std::uint32_t Depth()const{return depth_;}
		explicit operator float()const{return rounded_;}
		explicit operator double()const{return static_cast<double>(rounded_);}
		template<class Integer,typename std::enable_if<std::is_integral<Integer>::value,int>::type=0>
		explicit operator Integer()const{return static_cast<Integer>(rounded_);}

		TraceFloat& operator+=(const TraceFloat& other){*this=*this+other;return *this;}
		TraceFloat& operator-=(const TraceFloat& other){*this=*this-other;return *this;}
		TraceFloat& operator*=(const TraceFloat& other){*this=*this*other;return *this;}
		TraceFloat& operator/=(const TraceFloat& other){*this=*this/other;return *this;}

		friend TraceFloat operator+(const TraceFloat& a,const TraceFloat& b)
		{
			const double center=a.center_+b.center_;
			return Make(Operation::Add,center,a.radius_+b.radius_,
				static_cast<float>(a.rounded_+b.rounded_),a,b);
		}
		friend TraceFloat operator-(const TraceFloat& a,const TraceFloat& b)
		{
			const double center=a.center_-b.center_;
			return Make(Operation::Subtract,center,a.radius_+b.radius_,
				static_cast<float>(a.rounded_-b.rounded_),a,b);
		}
		friend TraceFloat operator*(const TraceFloat& a,const TraceFloat& b)
		{
			const double center=a.center_*b.center_;
			const double propagated=NextUp(std::fabs(a.center_)*b.radius_+
				std::fabs(b.center_)*a.radius_+a.radius_*b.radius_);
			return Make(Operation::Multiply,center,propagated,
				static_cast<float>(a.rounded_*b.rounded_),a,b);
		}
		friend TraceFloat operator/(const TraceFloat& a,const TraceFloat& b)
		{
			const double lower=std::fabs(b.center_)-b.radius_;
			const std::uint32_t depth=1u+std::max(a.depth_,b.depth_);
			if(ActiveCounters)ActiveCounters->minimumDenominatorLowerBound=std::min(
				ActiveCounters->minimumDenominatorLowerBound,lower);
			if(!(lower>0.0)){if(ActiveCounters){ActiveCounters->invalidDomain=true;
				if(!ActiveCounters->invalidDenominatorWitnessRecorded){
					ActiveCounters->invalidDenominatorWitnessRecorded=true;
					ActiveCounters->invalidDenominatorCenter=b.center_;
					ActiveCounters->invalidDenominatorRadius=b.radius_;
					ActiveCounters->invalidDenominatorRounded=b.rounded_;}
				RecordOperation(Operation::Divide,depth,std::max(
					std::fabs(a.center_)+a.radius_,std::fabs(b.center_)+b.radius_),
					std::numeric_limits<double>::infinity());}
				return Raw(a.center_/b.center_,std::numeric_limits<double>::infinity(),
					static_cast<float>(a.rounded_/b.rounded_),depth);}
			const double center=a.center_/b.center_;
			const double propagated=NextUp(a.radius_/lower+
				(std::fabs(a.center_)+a.radius_)*b.radius_/(lower*lower));
			return Make(Operation::Divide,center,propagated,
				static_cast<float>(a.rounded_/b.rounded_),a,b);
		}
		friend TraceFloat operator-(const TraceFloat& a)
		{
			return Raw(-a.center_,a.radius_,-a.rounded_,a.depth_);
		}

		friend bool operator<(const TraceFloat& a,const TraceFloat& b){return OrderedCompare(a,b,a.rounded_<b.rounded_);}
		friend bool operator>(const TraceFloat& a,const TraceFloat& b){return OrderedCompare(a,b,a.rounded_>b.rounded_);}
		friend bool operator<=(const TraceFloat& a,const TraceFloat& b){return OrderedCompare(a,b,a.rounded_<=b.rounded_);}
		friend bool operator>=(const TraceFloat& a,const TraceFloat& b){return OrderedCompare(a,b,a.rounded_>=b.rounded_);}
		friend bool operator==(const TraceFloat& a,const TraceFloat& b){return EqualityCompare(a,b,a.rounded_==b.rounded_);}
		friend bool operator!=(const TraceFloat& a,const TraceFloat& b){return EqualityCompare(a,b,a.rounded_!=b.rounded_);}

		static TraceFloat Unary(const Operation operation,const TraceFloat& a,
			const double center,const double propagated,const float rounded)
		{
			const double radius=NextUp(propagated+RoundRadius(std::fabs(center)+propagated));
			if(ActiveCounters)RecordOperation(operation,a.depth_+1u,
				std::fabs(a.center_)+a.radius_,NextUp(radius));
			return Raw(center,radius,rounded,a.depth_+1u);
		}

	private:
		static TraceFloat Convert(const double value)
		{
			const float rounded=static_cast<float>(value);
			const double radius=std::fabs(value-static_cast<double>(rounded));
			if(ActiveCounters)RecordOperation(Operation::Convert,1u,std::fabs(value),NextUp(radius));
			return Raw(value,NextUp(radius),rounded,1u);
		}
		static void RecordOperation(const Operation operation,const std::uint32_t depth,
			const double absoluteOperandUpper,const double resultRadius)
		{
			const unsigned int index=static_cast<unsigned int>(operation);
			++ActiveCounters->operation[index];
			ActiveCounters->maximumDepth=std::max(ActiveCounters->maximumDepth,depth);
			ActiveCounters->maximumAbsoluteOperand[index]=std::max(
				ActiveCounters->maximumAbsoluteOperand[index],absoluteOperandUpper);
			ActiveCounters->maximumResultRadius[index]=std::max(
				ActiveCounters->maximumResultRadius[index],resultRadius);
		}
		static TraceFloat Make(const Operation operation,const double center,
			const double propagated,const float rounded,const TraceFloat& a,const TraceFloat& b)
		{
			const std::uint32_t depth=1u+std::max(a.depth_,b.depth_);
			const double radius=NextUp(propagated+RoundRadius(std::fabs(center)+propagated));
			if(ActiveCounters)RecordOperation(operation,depth,std::max(
				std::fabs(a.center_)+a.radius_,std::fabs(b.center_)+b.radius_),NextUp(radius));
			return Raw(center,radius,rounded,depth);
		}
		static void RecordComparison(const TraceFloat& a,const TraceFloat& b,
			const bool resolved,const bool roundedResult)
		{
			if(ActiveCounters){++ActiveCounters->comparisonCount;
				const double margin=std::fabs(a.center_-b.center_)-a.radius_-b.radius_;
				ActiveCounters->minimumBranchMargin=std::min(
					ActiveCounters->minimumBranchMargin,margin);
				if(!resolved){ActiveCounters->unresolvedBranch=true;
					if(!ActiveCounters->unresolvedWitnessRecorded){
						ActiveCounters->unresolvedWitnessRecorded=true;
						ActiveCounters->unresolvedLeftCenter=a.center_;
						ActiveCounters->unresolvedLeftRadius=a.radius_;
						ActiveCounters->unresolvedRightCenter=b.center_;
						ActiveCounters->unresolvedRightRadius=b.radius_;
						ActiveCounters->unresolvedLeftRounded=a.rounded_;
						ActiveCounters->unresolvedRightRounded=b.rounded_;
						ActiveCounters->unresolvedRoundedResult=roundedResult;}}}
		}
		static bool OrderedCompare(const TraceFloat& a,const TraceFloat& b,const bool result)
		{
			RecordComparison(a,b,(a.radius_==0.0&&b.radius_==0.0)||
				std::fabs(a.center_-b.center_)>a.radius_+b.radius_,result);return result;
		}
		static bool EqualityCompare(const TraceFloat& a,const TraceFloat& b,const bool result)
		{
			RecordComparison(a,b,(a.radius_==0.0&&b.radius_==0.0)||
				std::fabs(a.center_-b.center_)>a.radius_+b.radius_,result);return result;
		}
		double center_,radius_;
		float rounded_;
		std::uint32_t depth_;
	};

	inline TraceFloat abs(const TraceFloat& value)
	{
		return TraceFloat::Unary(Operation::Absolute,value,std::fabs(value.Center()),
			value.Radius(),std::fabs(value.Rounded()));
	}
	inline TraceFloat fabs(const TraceFloat& value){return abs(value);}
	inline TraceFloat sqrt(const TraceFloat& value)
	{
		const double lower=value.Center()-value.Radius();
		if(ActiveCounters)ActiveCounters->minimumSqrtDomainLowerBound=std::min(
			ActiveCounters->minimumSqrtDomainLowerBound,lower);
		if(!(lower>=0.0)){if(ActiveCounters)ActiveCounters->invalidDomain=true;}
		const double center=std::sqrt(std::max(0.0,value.Center()));
		const double upper=std::sqrt(std::max(0.0,value.Center()+value.Radius()));
		const double low=std::sqrt(std::max(0.0,lower));
		return TraceFloat::Unary(Operation::Sqrt,value,center,
			std::max(center-low,upper-center),std::sqrt(value.Rounded()));
	}
	inline TraceFloat floor(const TraceFloat& value)
	{
		const double center=std::floor(value.Center());
		const double low=std::floor(value.Center()-value.Radius());
		const double high=std::floor(value.Center()+value.Radius());
		if(low!=high&&ActiveCounters)ActiveCounters->unresolvedBranch=true;
		return TraceFloat::Unary(Operation::Floor,value,center,0.0f,
			std::floor(value.Rounded()));
	}
	inline TraceFloat ceil(const TraceFloat& value)
	{
		const double center=std::ceil(value.Center());
		const double low=std::ceil(value.Center()-value.Radius());
		const double high=std::ceil(value.Center()+value.Radius());
		if(low!=high&&ActiveCounters)ActiveCounters->unresolvedBranch=true;
		return TraceFloat::Unary(Operation::Ceil,value,center,0.0f,
			std::ceil(value.Rounded()));
	}
	inline TraceFloat fmod(const TraceFloat& a,const TraceFloat& b)
	{
		const double denominatorLower=std::fabs(b.Center())-b.Radius();
		if(ActiveCounters)ActiveCounters->minimumDenominatorLowerBound=std::min(
			ActiveCounters->minimumDenominatorLowerBound,denominatorLower);
		if(!(denominatorLower>0.0)&&ActiveCounters){ActiveCounters->invalidDomain=true;
			if(!ActiveCounters->invalidDenominatorWitnessRecorded){
				ActiveCounters->invalidDenominatorWitnessRecorded=true;
				ActiveCounters->invalidDenominatorCenter=b.Center();
				ActiveCounters->invalidDenominatorRadius=b.Radius();
				ActiveCounters->invalidDenominatorRounded=b.Rounded();}}
		const double center=std::fmod(a.Center(),b.Center());
		const double propagated=a.Radius()+
			std::ceil(std::fabs(a.Center()/b.Center()))*b.Radius();
		const std::uint32_t depth=1u+std::max(a.Depth(),b.Depth());
		const double radius=NextUp(propagated+RoundRadius(std::fabs(center)+propagated));
		if(ActiveCounters){const unsigned int index=static_cast<unsigned int>(Operation::Remainder);
			++ActiveCounters->operation[index];
			ActiveCounters->maximumDepth=std::max(ActiveCounters->maximumDepth,depth);
			ActiveCounters->maximumAbsoluteOperand[index]=std::max(
				ActiveCounters->maximumAbsoluteOperand[index],std::max(
					std::fabs(a.Center())+a.Radius(),std::fabs(b.Center())+b.Radius()));
			ActiveCounters->maximumResultRadius[index]=std::max(
				ActiveCounters->maximumResultRadius[index],NextUp(radius));}
		return TraceFloat::Raw(center,radius,std::fmod(a.Rounded(),b.Rounded()),
			depth);
	}
	inline bool isfinite(const TraceFloat& value){return std::isfinite(value.Rounded());}

	inline void ObserveAndReset(std::vector<TraceFloat>& values)
	{
		for(TraceFloat& value:values){
			if(ActiveCounters){ActiveCounters->maximumAbsoluteOutput=std::max(
				ActiveCounters->maximumAbsoluteOutput,std::fabs(value.Center())+value.Radius());
				ActiveCounters->maximumOutputRadius=std::max(
					ActiveCounters->maximumOutputRadius,value.Radius());}
			value=TraceFloat(value.Rounded());
		}
	}

	inline void SealStageAndReset(std::vector<TraceFloat>& first)
	{
		ObserveAndReset(first);
		if(ActiveCounters){ActiveCounters->sealedStages.push_back(
			static_cast<const Observation&>(*ActiveCounters));
			static_cast<Observation&>(*ActiveCounters)=Observation();}
	}

	inline void SealStageAndReset(std::vector<TraceFloat>& first,
		std::vector<TraceFloat>& second)
	{
		ObserveAndReset(first);ObserveAndReset(second);
		if(ActiveCounters){ActiveCounters->sealedStages.push_back(
			static_cast<const Observation&>(*ActiveCounters));
			static_cast<Observation&>(*ActiveCounters)=Observation();}
	}

	inline void SealCurrentStage()
	{
		if(ActiveCounters){ActiveCounters->sealedStages.push_back(
			static_cast<const Observation&>(*ActiveCounters));
			static_cast<Observation&>(*ActiveCounters)=Observation();}
	}

	template<std::size_t Count> inline void SealStageAndReset(
		std::array<std::vector<TraceFloat>,Count>& first)
	{
		for(std::vector<TraceFloat>& values:first)ObserveAndReset(values);
		SealCurrentStage();
	}

	template<std::size_t Count> inline void SealStageAndReset(
		std::array<std::vector<TraceFloat>,Count>& first,
		std::array<std::vector<TraceFloat>,Count>& second)
	{
		for(std::vector<TraceFloat>& values:first)ObserveAndReset(values);
		for(std::vector<TraceFloat>& values:second)ObserveAndReset(values);
		SealCurrentStage();
	}

	class Scope
	{
	public:
		explicit Scope(Counters& counters):previous_(ActiveCounters){counters=Counters();ActiveCounters=&counters;}
		~Scope(){ActiveCounters=previous_;}
	private:Counters* previous_;
	};
}

namespace std
{
	template<> class numeric_limits<FireProductionRoundoffTrace::TraceFloat>:
		public numeric_limits<float> {};
	inline bool isfinite(const FireProductionRoundoffTrace::TraceFloat& value)
		{return FireProductionRoundoffTrace::isfinite(value);}
	inline FireProductionRoundoffTrace::TraceFloat fabs(
		const FireProductionRoundoffTrace::TraceFloat& value)
		{return FireProductionRoundoffTrace::fabs(value);}
	inline FireProductionRoundoffTrace::TraceFloat sqrt(
		const FireProductionRoundoffTrace::TraceFloat& value)
		{return FireProductionRoundoffTrace::sqrt(value);}
	inline FireProductionRoundoffTrace::TraceFloat floor(
		const FireProductionRoundoffTrace::TraceFloat& value)
		{return FireProductionRoundoffTrace::floor(value);}
	inline FireProductionRoundoffTrace::TraceFloat ceil(
		const FireProductionRoundoffTrace::TraceFloat& value)
		{return FireProductionRoundoffTrace::ceil(value);}
	inline FireProductionRoundoffTrace::TraceFloat fmod(
		const FireProductionRoundoffTrace::TraceFloat& a,
		const FireProductionRoundoffTrace::TraceFloat& b)
		{return FireProductionRoundoffTrace::fmod(a,b);}
}

#endif
