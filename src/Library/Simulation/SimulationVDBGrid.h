//////////////////////////////////////////////////////////////////////
//
//  SimulationVDBGrid.h - Import-only OpenVDB-to-grid.v1 projection.
//
//  Include exclusively in OpenVDB-enabled translation units. Preflights
//  the archive preimage, decodes float/vec3f grids, and packs active and
//  non-background inactive values under a caller-supplied byte budget.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
#ifndef RISE_SIMULATION_VDB_GRID_H
#define RISE_SIMULATION_VDB_GRID_H

#if defined(RISE_ENABLE_OPENVDB)
#include "SimulationGrid.h"
#include "SimulationVDBBounds.h"
#include <openvdb/openvdb.h>
#include <openvdb/io/Stream.h>
#include <istream>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace RISE { namespace Simulation {
namespace VDBProjection {
// Bit-exact binary32 storage preserving signed zero across scalar/vec3f values.
using Bits = std::array<std::uint32_t,3>;

inline Bits Tuple(float value) {
	Bits out{};
	std::memcpy(out.data(),&value,4);
	return out;
}
inline Bits Tuple(const openvdb::Vec3f& value) {
	Bits out{};
	for (unsigned i=0;i<3;++i) out[i]=Tuple(value[i])[0];
	return out;
}

inline void Spend(std::size_t& budget,std::size_t bytes) {
	if (bytes>budget) throw std::runtime_error("VDB projection exceeds budget");
	budget-=bytes;
}

// Converts an inclusive OpenVDB CoordBBox into grid.v1 origin and positive extents.
inline void Extents(const openvdb::CoordBBox& box,std::array<std::int32_t,3>& origin,
	std::array<std::uint32_t,3>& dimensions)
{
	for (unsigned i=0;i<3;++i) {
		origin[i]=box.min()[i];
		const auto length=static_cast<std::int64_t>(box.max()[i])-origin[i]+1;
		if (length<=0 || length>(std::numeric_limits<std::uint32_t>::max)())
			throw std::runtime_error("VDB bounds cannot be represented by grid.v1");
		dimensions[i]=static_cast<std::uint32_t>(length);
	}
}

// Doubles block vector capacity while charging peak reallocation against the budget.
inline void GrowBlocks(GridChannel& c,std::size_t& budget) {
	if (c.blocks.size()<c.blocks.capacity()) return;
	const auto old=c.blocks.capacity();
	if (old>(std::numeric_limits<std::size_t>::max)()/2/sizeof(GridBlock))
		throw std::runtime_error("VDB block count overflow");
	const auto capacity=old ? old*2 : 1;
	Spend(budget,capacity*sizeof(GridBlock));
	c.blocks.reserve(capacity);
	budget+=old*sizeof(GridBlock);
}
}

// The caller owns a private byte preimage through the end of decode. Seek
// within the same buffer; an OpenVDB archive may have absolute grid offsets.
inline openvdb::GridPtrVecPtr DecodeVDBPreimage(std::string& bytes)
{
	VDBArchiveBounds(bytes).Check();
	class Buffer final : public std::streambuf {
	public:
		explicit Buffer(std::string& data) {
			setg(&data[0],&data[0],&data[0]+data.size());
		}
	protected:
		pos_type seekoff(off_type offset,std::ios_base::seekdir dir,
			std::ios_base::openmode mode) override
		{
			if (!(mode & std::ios_base::in)) return pos_type(off_type(-1));
			const off_type start=dir==std::ios_base::beg ? 0 :
				(dir==std::ios_base::end ? egptr()-eback() : gptr()-eback());
			if (offset < -start || offset > egptr()-eback()-start) return pos_type(off_type(-1));
			setg(eback(),eback()+start+offset,egptr());
			return pos_type(start+offset);
		}
		pos_type seekpos(pos_type pos,std::ios_base::openmode mode) override {
			return seekoff(off_type(pos),std::ios_base::beg,mode);
		}
	};
	Buffer buffer(bytes);
	std::istream input(&buffer);
	openvdb::initialize();
#if OPENVDB_LIBRARY_MAJOR_VERSION_NUMBER >= 14
	openvdb::io::Stream stream(input);
#else
	openvdb::io::Stream stream(input,false);
#endif
	return stream.getGrids();
}

// Preserve the union of active values and inactive values distinct from the
// background, including signed zero. Optional declared bounds must contain
// that union; a manifest can include an inactive border beyond stored values.
template<class Grid>
bool ExtractVDBGrid(const Grid& grid,const std::string& units,
	const openvdb::CoordBBox* declaredDomain,std::size_t& remaining,GridChannel& out,std::string& error)
{
	using namespace VDBProjection;
	using Value=typename Grid::ValueType;
	constexpr bool scalar=std::is_same<Value,float>::value;
	static_assert(scalar || std::is_same<Value,openvdb::Vec3f>::value,"grid.v1 supports f32 and vec3f");
	try {
		std::size_t budget=remaining;
		GridChannel c;
		c.name=grid.getName(); c.units=units; c.components=scalar ? 1 : 3;
		Spend(budget,sizeof(GridChannel)+c.name.size()+c.units.size()+2);
		if (!grid.transform().isLinear()) throw std::runtime_error("non-affine VDB transform");
		const auto matrix=grid.transform().baseMap()->getAffineMap()->getMat4();
		for (unsigned i=0;i<4;++i) for (unsigned j=0;j<4;++j) c.indexToWorld[i*4+j]=matrix(j,i);
		c.background=Tuple(grid.background());
		openvdb::CoordBBox coverage;
		auto include=[&](const openvdb::CoordBBox& box) {
			if (declaredDomain && (!declaredDomain->isInside(box.min()) || !declaredDomain->isInside(box.max())))
				throw std::runtime_error("stored VDB values outside declared domain");
			coverage.expand(box);
		};
		auto tile=grid.tree().cbeginValueAll();
		tile.setMaxDepth(Grid::TreeType::DEPTH-2);
		for (;tile;++tile) {
			const auto tuple=Tuple(*tile);
			if (!tile.isValueOn() && tuple==c.background) continue;
			const auto box=tile.getBoundingBox();
			include(box);
			GrowBlocks(c,budget);
			auto& b=c.blocks.emplace_back();
			Extents(box,b.origin,b.dimensions);
			b.constant=true;
			Spend(budget,1+4*c.components);
			b.active={static_cast<std::uint8_t>(tile.isValueOn())};
			b.values.assign(tuple.begin(),tuple.begin()+c.components);
		}
		for (auto leaf=grid.tree().cbeginLeaf();leaf;++leaf) {
			openvdb::CoordBBox box;
			for (auto voxel=leaf->cbeginValueAll();voxel;++voxel)
				if (voxel.isValueOn() || Tuple(*voxel)!=c.background) box.expand(voxel.getCoord());
			if (box.empty()) continue;
			include(box);
			GrowBlocks(c,budget);
			auto& b=c.blocks.emplace_back();
			Extents(box,b.origin,b.dimensions);
			// A leaf is bounded by the fixed tree leaf dimensions.
			const std::size_t cells=std::size_t(b.dimensions[0])*b.dimensions[1]*b.dimensions[2];
			Spend(budget,(cells+7)/8+cells*c.components*4);
			b.active.resize((cells+7)/8);
			b.values.resize(cells*c.components);
			std::size_t ordinal=0;
			for (std::uint32_t z=0;z<b.dimensions[2];++z)
				for (std::uint32_t y=0;y<b.dimensions[1];++y)
					for (std::uint32_t x=0;x<b.dimensions[0];++x,++ordinal) {
						const openvdb::Coord position(box.min().x()+static_cast<int>(x),
							box.min().y()+static_cast<int>(y),box.min().z()+static_cast<int>(z));
						if (leaf->isValueOn(position)) b.active[ordinal/8]|=static_cast<std::uint8_t>(1u<<(ordinal%8));
						const auto tuple=Tuple(leaf->getValue(position));
						std::copy_n(tuple.begin(),c.components,b.values.begin()+ordinal*c.components);
					}
		}
		if (declaredDomain) coverage=*declaredDomain;
		if (coverage.empty()) coverage.expand(openvdb::Coord(0,0,0));
		Extents(coverage,c.origin,c.dimensions);
		std::sort(c.blocks.begin(),c.blocks.end(),[](const GridBlock& a,const GridBlock& b) {
			return a.origin<b.origin;
		});
		out=std::move(c);
		remaining=budget;
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}
} }
#endif // RISE_ENABLE_OPENVDB
#endif
