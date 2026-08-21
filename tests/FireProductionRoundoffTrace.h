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
	enum class BranchSite : unsigned int { Unknown,PPMQuadraticZero,PPMStationaryLower,
		PPMStationaryUpper,MinimumSelection,
		MaximumSelection,FloorBoundary,CeilBoundary,FlatStencil,FlatIntegral,
		RemainingPositive,CourantNonnegative,FractionPositive,LimiterPositive,
		LimiterNegative,InflowSign,Count };
	enum class BranchCertificate : unsigned int { None,Equivalence,Reformulation };

	struct BranchObligation
	{
		std::uint64_t comparisonOrdinal=0u;
		BranchSite site=BranchSite::Unknown;
		double predicateCenter=0.0,predicateRadius=0.0;
		bool roundedResult=false;
		BranchCertificate certificate=BranchCertificate::None;
		double divergenceBound=0.0;
		double proofLower=0.0,proofRequired=0.0;
		float inactiveResultRounded=0.0f,activeResultRounded=0.0f;
	};

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
		std::vector<BranchObligation> branchObligations;
		std::uint64_t dischargedBranchObligationCount=0u;
	};

	struct Counters : Observation
	{
		std::vector<Observation> sealedStages;
	};

	inline thread_local Counters* ActiveCounters=nullptr;
	inline thread_local BranchSite ActiveBranchSite=BranchSite::Unknown;
	inline thread_local std::size_t LastScopedObligation=std::numeric_limits<std::size_t>::max();
	inline thread_local unsigned int CoveredBranchDepth=0u;
	inline thread_local std::size_t PPMObligationStart=std::numeric_limits<std::size_t>::max();
	inline thread_local bool PPMQuadraticAmbiguous=false;

	class BranchSiteScope
	{
	public:
		explicit BranchSiteScope(const BranchSite site):previous_(ActiveBranchSite)
		{
			ActiveBranchSite=site;LastScopedObligation=std::numeric_limits<std::size_t>::max();
		}
		~BranchSiteScope(){ActiveBranchSite=previous_;}
	private:
		BranchSite previous_;
	};

	class CoveredBranchScope
	{
	public:
		explicit CoveredBranchScope(const bool covered):covered_(covered)
			{if(covered_)++CoveredBranchDepth;}
		~CoveredBranchScope(){if(covered_)--CoveredBranchDepth;}
	private:bool covered_;
	};

	template<class Predicate> inline bool EvaluateBranch(const BranchSite site,
		const Predicate& predicate)
	{
		BranchSiteScope scope(site);return predicate();
	}

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
		void ExpandRadius(const double amount)
		{
			if(amount>0.0)radius_=NextUp(radius_+amount);
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
			if(!(lower>0.0)){if(ActiveCounters&&CoveredBranchDepth==0u){ActiveCounters->invalidDomain=true;
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
			if(CoveredBranchDepth!=0u)return;
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
			if(ActiveCounters){const std::uint64_t ordinal=ActiveCounters->comparisonCount++;
				if(CoveredBranchDepth!=0u&&(!PPMQuadraticAmbiguous||
					(ActiveBranchSite!=BranchSite::PPMStationaryLower&&
					 ActiveBranchSite!=BranchSite::PPMStationaryUpper)))return;
				const double margin=std::fabs(a.center_-b.center_)-a.radius_-b.radius_;
				ActiveCounters->minimumBranchMargin=std::min(
					ActiveCounters->minimumBranchMargin,margin);
				if(!resolved){ActiveCounters->unresolvedBranch=true;
					BranchObligation obligation;obligation.comparisonOrdinal=ordinal;
					obligation.site=ActiveBranchSite;
					obligation.predicateCenter=a.center_-b.center_;
					obligation.predicateRadius=NextUp(a.radius_+b.radius_);
					obligation.roundedResult=roundedResult;
					ActiveCounters->branchObligations.push_back(obligation);
					if(ActiveBranchSite!=BranchSite::Unknown)
						LastScopedObligation=ActiveCounters->branchObligations.size()-1u;
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

	inline bool PPMQuadraticZeroObligationPending()
	{
		return ActiveCounters&&LastScopedObligation!=std::numeric_limits<std::size_t>::max()&&
			LastScopedObligation<ActiveCounters->branchObligations.size()&&
			ActiveCounters->branchObligations[LastScopedObligation].site==
				BranchSite::PPMQuadraticZero&&
			ActiveCounters->branchObligations[LastScopedObligation].certificate==
				BranchCertificate::None;
	}

	inline void BeginPPMBranchEnvelope()
	{
		PPMObligationStart=ActiveCounters?ActiveCounters->branchObligations.size():
			std::numeric_limits<std::size_t>::max();
		PPMQuadraticAmbiguous=false;
	}
	inline void SetPPMQuadraticAmbiguous(const bool ambiguous)
		{PPMQuadraticAmbiguous=ambiguous;}

	inline void ApplyPPMQuadraticZeroCertificate(const TraceFloat& quadratic,
		const TraceFloat& linear,
		const TraceFloat& endpointMinimum,const TraceFloat& endpointMaximum,
		TraceFloat& minimum,TraceFloat& maximum)
	{
		if(!ActiveCounters||PPMObligationStart==std::numeric_limits<std::size_t>::max()||
			PPMObligationStart>ActiveCounters->branchObligations.size())return;
		const double quadraticUpper=NextUp(std::fabs(quadratic.Center())+
			quadratic.Radius());
		const double endpointAbsoluteUpper=NextUp(std::max(
			std::fabs(endpointMinimum.Center())+endpointMinimum.Radius(),
			std::fabs(endpointMaximum.Center())+endpointMaximum.Radius()));
		const double endpointRadius=std::max(endpointMinimum.Radius(),
			endpointMaximum.Radius());
		const double unit=0x1p-24;
		const double gamma4=NextUp((4.0*unit)/(1.0-4.0*unit));
		const double operationMagnitude=NextUp(quadraticUpper+
			std::fabs(linear.Center())+linear.Radius()+endpointAbsoluteUpper);
		const double underflowAllowance=4.0*
			static_cast<double>(std::numeric_limits<float>::min());
		const double arithmeticResidual=NextUp(quadratic.Radius()+linear.Radius()+
			endpointRadius+gamma4*operationMagnitude+underflowAllowance);
		double divergence=NextUp(0.25*quadraticUpper+arithmeticResidual);
		bool hasObligation=false;
		for(std::size_t index=PPMObligationStart;
			index<ActiveCounters->branchObligations.size();++index){
			const BranchObligation& obligation=ActiveCounters->branchObligations[index];
			if(obligation.certificate!=BranchCertificate::None)continue;
			if(obligation.site==BranchSite::PPMQuadraticZero||
				obligation.site==BranchSite::PPMStationaryLower||
				obligation.site==BranchSite::PPMStationaryUpper)hasObligation=true;
		}
		if(!hasObligation){PPMObligationStart=std::numeric_limits<std::size_t>::max();
			PPMQuadraticAmbiguous=false;return;}
		const double minimumRadius=NextUp(endpointMinimum.Radius()+divergence);
		const double maximumRadius=NextUp(endpointMaximum.Radius()+divergence);
		if(std::fabs(static_cast<double>(minimum.Rounded())-endpointMinimum.Center())>
			minimumRadius||std::fabs(static_cast<double>(maximum.Rounded())-
			endpointMaximum.Center())>maximumRadius){ActiveCounters->invalidDomain=true;
			PPMObligationStart=std::numeric_limits<std::size_t>::max();
			PPMQuadraticAmbiguous=false;return;}
		minimum=TraceFloat::Raw(endpointMinimum.Center(),minimumRadius,minimum.Rounded(),
			std::max(endpointMinimum.Depth(),minimum.Depth()));
		maximum=TraceFloat::Raw(endpointMaximum.Center(),maximumRadius,maximum.Rounded(),
			std::max(endpointMaximum.Depth(),maximum.Depth()));
		for(std::size_t index=PPMObligationStart;
			index<ActiveCounters->branchObligations.size();++index){
			BranchObligation& obligation=ActiveCounters->branchObligations[index];
			if((obligation.site==BranchSite::PPMQuadraticZero||
				obligation.site==BranchSite::PPMStationaryLower||
				obligation.site==BranchSite::PPMStationaryUpper)&&
				obligation.certificate==BranchCertificate::None){
				obligation.certificate=BranchCertificate::Equivalence;
				obligation.divergenceBound=divergence;
				++ActiveCounters->dischargedBranchObligationCount;
			}
		}
		ActiveCounters->unresolvedBranch=ActiveCounters->dischargedBranchObligationCount<
			ActiveCounters->branchObligations.size();
		LastScopedObligation=std::numeric_limits<std::size_t>::max();
		PPMObligationStart=std::numeric_limits<std::size_t>::max();
		PPMQuadraticAmbiguous=false;
	}

	inline void RecordDiscreteBoundaryObligation(const BranchSite site,
		const TraceFloat& value,const double boundary,const bool roundedResult)
	{
		if(!ActiveCounters)return;
		const std::uint64_t ordinal=ActiveCounters->comparisonCount++;
		const double center=value.Center()-boundary;
		const double radius=value.Radius();
		ActiveCounters->minimumBranchMargin=std::min(
			ActiveCounters->minimumBranchMargin,std::fabs(center)-radius);
		ActiveCounters->unresolvedBranch=true;
		BranchObligation obligation;obligation.comparisonOrdinal=ordinal;
		obligation.site=site;obligation.predicateCenter=center;
		obligation.predicateRadius=radius;obligation.roundedResult=roundedResult;
		ActiveCounters->branchObligations.push_back(obligation);
		if(!ActiveCounters->unresolvedWitnessRecorded){
			ActiveCounters->unresolvedWitnessRecorded=true;
			ActiveCounters->unresolvedLeftCenter=value.Center();
			ActiveCounters->unresolvedLeftRadius=value.Radius();
			ActiveCounters->unresolvedRightCenter=boundary;
			ActiveCounters->unresolvedRightRadius=0.0;
			ActiveCounters->unresolvedLeftRounded=value.Rounded();
			ActiveCounters->unresolvedRightRounded=static_cast<float>(boundary);
			ActiveCounters->unresolvedRoundedResult=roundedResult;
		}
	}

	inline TraceFloat CertifiedSelection(const TraceFloat& first,const TraceFloat& second,
		const bool minimum)
	{
		const bool chooseSecond=minimum?second.Rounded()<first.Rounded():
			first.Rounded()<second.Rounded();
		const TraceFloat& chosen=chooseSecond?second:first;
		const double predicateCenter=first.Center()-second.Center();
		const double predicateRadius=NextUp(first.Radius()+second.Radius());
		const bool resolved=(first.Radius()==0.0&&second.Radius()==0.0)||
			std::fabs(predicateCenter)>predicateRadius;
		if(!ActiveCounters||CoveredBranchDepth!=0u)return chosen;
		const std::uint64_t ordinal=ActiveCounters->comparisonCount++;
		ActiveCounters->minimumBranchMargin=std::min(
			ActiveCounters->minimumBranchMargin,std::fabs(predicateCenter)-predicateRadius);
		if(resolved)return chosen;
		BranchObligation obligation;obligation.comparisonOrdinal=ordinal;
		obligation.site=minimum?BranchSite::MinimumSelection:BranchSite::MaximumSelection;
		obligation.predicateCenter=predicateCenter;
		obligation.predicateRadius=predicateRadius;
		obligation.roundedResult=chooseSecond;
		obligation.certificate=BranchCertificate::Equivalence;
		obligation.divergenceBound=predicateRadius;
		ActiveCounters->branchObligations.push_back(obligation);
		++ActiveCounters->dischargedBranchObligationCount;
		const double firstLow=first.Center()-first.Radius();
		const double firstHigh=first.Center()+first.Radius();
		const double secondLow=second.Center()-second.Radius();
		const double secondHigh=second.Center()+second.Radius();
		const double low=minimum?std::min(firstLow,secondLow):std::max(firstLow,secondLow);
		const double high=minimum?std::min(firstHigh,secondHigh):std::max(firstHigh,secondHigh);
		const double radius=NextUp(std::max(std::max(std::fabs(chosen.Center()-low),
			std::fabs(high-chosen.Center())),std::fabs(
				static_cast<double>(chosen.Rounded())-chosen.Center())));
		return TraceFloat::Raw(chosen.Center(),radius,chosen.Rounded(),
			std::max(first.Depth(),second.Depth()));
	}

	inline TraceFloat ApplyLimiterBranch(const TraceFloat& alpha,
		const TraceFloat& envelope,const TraceFloat& center,
		const TraceFloat& signedDeviation,const bool positive)
	{
		const TraceFloat magnitude=positive?signedDeviation:-signedDeviation;
		bool active=false;
		{
			BranchSiteScope scope(positive?BranchSite::LimiterPositive:
				BranchSite::LimiterNegative);
			active=positive?signedDeviation>0.0f:signedDeviation<0.0f;
		}
		const bool pending=ActiveCounters&&
			LastScopedObligation!=std::numeric_limits<std::size_t>::max()&&
			LastScopedObligation<ActiveCounters->branchObligations.size();
		const double numeratorCenter=positive?envelope.Center()-center.Center():
			center.Center()-envelope.Center();
		const double numeratorRadius=NextUp(envelope.Radius()+center.Radius());
		const double numeratorLower=numeratorCenter-numeratorRadius;
		const double magnitudeUpper=NextUp(std::fabs(magnitude.Center())+magnitude.Radius());
		const double alphaUpper=NextUp(std::fabs(alpha.Center())+alpha.Radius());
		const bool noEffect=pending&&numeratorLower>=0.0&&
			numeratorLower>=NextUp(alphaUpper*magnitudeUpper);
		if(pending){BranchObligation& obligation=
			ActiveCounters->branchObligations[LastScopedObligation];
			obligation.proofLower=numeratorLower;
			obligation.proofRequired=NextUp(alphaUpper*magnitudeUpper);
			obligation.inactiveResultRounded=alpha.Rounded();}
		TraceFloat result=alpha;
		if(active){
			CoveredBranchScope covered(noEffect);
			const TraceFloat numerator=positive?envelope-center:center-envelope;
			result=CertifiedSelection(alpha,numerator/magnitude,true);
		}
		if(pending)ActiveCounters->branchObligations[LastScopedObligation].
			activeResultRounded=result.Rounded();
		if(!noEffect)return result;
		BranchObligation& obligation=ActiveCounters->branchObligations[LastScopedObligation];
		obligation.certificate=BranchCertificate::Equivalence;
		obligation.divergenceBound=0.0;
		++ActiveCounters->dischargedBranchObligationCount;
		const double radius=NextUp(std::max(alpha.Radius(),std::fabs(
			static_cast<double>(result.Rounded())-alpha.Center())));
		result=TraceFloat::Raw(alpha.Center(),radius,result.Rounded(),
			std::max(alpha.Depth(),result.Depth()));
		ActiveCounters->unresolvedBranch=ActiveCounters->dischargedBranchObligationCount<
			ActiveCounters->branchObligations.size();
		LastScopedObligation=std::numeric_limits<std::size_t>::max();
		return result;
	}

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
		if(low!=high&&ActiveCounters)for(double boundary=low+1.0;
			boundary<=high;boundary+=1.0)RecordDiscreteBoundaryObligation(
				BranchSite::FloorBoundary,value,boundary,value.Rounded()>=boundary);
		return TraceFloat::Unary(Operation::Floor,value,center,0.0f,
			std::floor(value.Rounded()));
	}
	inline TraceFloat ceil(const TraceFloat& value)
	{
		const double center=std::ceil(value.Center());
		const double low=std::ceil(value.Center()-value.Radius());
		const double high=std::ceil(value.Center()+value.Radius());
		if(low!=high&&ActiveCounters)for(double boundary=low;
			boundary<high;boundary+=1.0)RecordDiscreteBoundaryObligation(
				BranchSite::CeilBoundary,value,boundary,value.Rounded()>boundary);
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
			if(CoveredBranchDepth==0u){
			ActiveCounters->maximumAbsoluteOperand[index]=std::max(
				ActiveCounters->maximumAbsoluteOperand[index],std::max(
					std::fabs(a.Center())+a.Radius(),std::fabs(b.Center())+b.Radius()));
			ActiveCounters->maximumResultRadius[index]=std::max(
				ActiveCounters->maximumResultRadius[index],NextUp(radius));}}
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
	inline FireProductionRoundoffTrace::TraceFloat min(
		const FireProductionRoundoffTrace::TraceFloat& a,
		const FireProductionRoundoffTrace::TraceFloat& b)
		{return FireProductionRoundoffTrace::CertifiedSelection(a,b,true);}
	inline FireProductionRoundoffTrace::TraceFloat max(
		const FireProductionRoundoffTrace::TraceFloat& a,
		const FireProductionRoundoffTrace::TraceFloat& b)
		{return FireProductionRoundoffTrace::CertifiedSelection(a,b,false);}
}

#endif
