#ifndef FIRE_PRODUCTION_ROUNDOFF_WALKER_H
#define FIRE_PRODUCTION_ROUNDOFF_WALKER_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace FireProductionRoundoffWalker
{
	struct Topology
	{
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
		return CheckedAddProduct(topology.operationCount,1u,values)&&
			CheckedAddProduct(topology.operationCount,2u,faces)&&
			CheckedAddProduct(topology.operationCount,2u,values)&&
			CheckedAddProduct(topology.operationCount,5u,values)&&
			CheckedAddProduct(topology.operationCount,6u,values)&&
			CheckedAddProduct(topology.operationCount,2u*treeEdges,lineComponents)&&
			CheckedAddProduct(topology.operationCount,14u,swept)&&
			CheckedAddProduct(topology.operationCount,3u,values)&&
			(topology.maximumDepth=14u,true);
	}
}

#endif
