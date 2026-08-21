#ifndef FIRE_PRODUCTION_ROUNDOFF_WALKER_H
#define FIRE_PRODUCTION_ROUNDOFF_WALKER_H

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
		float leftRounded=0.0f,rightRounded=0.0f;
		bool roundedResult=false;
	};

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
					line,cell,component,witness))return true;
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
			CheckedAddOperation(topology,Add,5u,values)&&
			CheckedAddOperation(topology,Add,2u*treeEdges,lineComponents)&&
			CheckedAddOperation(topology,Subtract,9u,values)&&
			CheckedAddOperation(topology,Multiply,9u,values)&&
			CheckedAddOperation(topology,Multiply,1u,faces)&&
			CheckedAddOperation(topology,Divide,3u,values)&&
			CheckedAddOperation(topology,Divide,1u,faces)&&
			CheckedAddOperation(topology,Absolute,1u,values)&&
			CheckedAddOperation(topology,Floor,2u,values)&&
			CheckedAddOperation(topology,Remainder,1u,swept)&&
			(topology.maximumDepth=14u,true);
	}
}

#endif
