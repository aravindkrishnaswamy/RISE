#ifndef FIRE_PRODUCTION_ROUNDOFF_WALKER_H
#define FIRE_PRODUCTION_ROUNDOFF_WALKER_H

#include <cstddef>
#include <cstdint>
#include <limits>

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
