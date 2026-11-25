// Allocation preflight for the float32/binary32-vector OpenVDB import profile.
// This cursor only inspects an immutable byte preimage. It never inflates data.
#ifndef RISE_SIMULATION_VDB_BOUNDS_H
#define RISE_SIMULATION_VDB_BOUNDS_H

#if defined(RISE_ENABLE_OPENVDB)
#include <blosc.h>
#include <algorithm>
#include <array>
#include <set>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace RISE { namespace Simulation {

class VDBArchiveBounds {
public:
	explicit VDBArchiveBounds(std::string_view input, std::uint64_t heapBudget = 4ull << 30)
		: bytes(input), budget(heapBudget) {}

	void Check()
	{
		Require(Number(8)==0x56444220u,"archive magic");
		const auto version=Number(4);
		Require(version>=223 && version<=225,"unsupported archive version");
		Take(8); // writer library major/minor
		Require(Number(1)==1,"absolute grid offsets required");
		Take(36); // UUID
		Metadata();
		const auto grids=Number(4);
		Require(grids>0 && grids<=256,"grid count");
		for (unsigned i=0;i<grids;++i) Grid();
		Require(cursor==bytes.size(),"archive end offset");
	}

private:
	std::string_view bytes;
	std::uint64_t budget;
	std::size_t cursor=0;
	unsigned width=0,compression=0;
	std::vector<std::string_view> leaves;
	std::set<std::string_view> gridNames;
	std::uint64_t delayedCount=0;

	static void Require(bool ok,const char* reason) {
		if (!ok) throw std::runtime_error(std::string("VDB preflight: ")+reason);
	}

	std::string_view Take(std::uint64_t n) {
		Require(n<=bytes.size()-cursor,"truncated archive");
		const auto view=bytes.substr(cursor,static_cast<std::size_t>(n));
		cursor+=view.size();
		return view;
	}

	static std::uint64_t Integer(std::string_view v) {
		std::uint64_t out=0;
		for (std::size_t i=0;i<v.size();++i)
			out|=std::uint64_t(static_cast<unsigned char>(v[i]))<<(8*i);
		return out;
	}

	std::uint64_t Number(unsigned size) { return Integer(Take(size)); }

	std::string_view Name() {
		const auto length=Number(4);
		Require(length<=4096,"name length");
		const auto v=Take(length);
		Require(v.find('\0')==std::string_view::npos,"embedded null in name");
		return v;
	}

	void Charge(std::uint64_t n) {
		Require(n<=budget,"tree allocation budget");
		budget-=n;
	}

	static void CheckBlosc(std::string_view chunk,std::size_t expected) {
		Require(chunk.size()>=16,"Blosc header");
		std::size_t validated=0;
		Require(blosc_cbuffer_validate(chunk.data(),chunk.size(),&validated)==0 &&
			validated==expected,"invalid Blosc chunk");
		const auto plain=Integer(chunk.substr(4,4)),block=Integer(chunk.substr(8,4));
		Require(plain==expected && Integer(chunk.substr(12,4))==chunk.size() &&
			block>=1 && block<=std::max<std::size_t>(expected,1),"Blosc buffer sizes");
	}

	void DelayedMetadata(std::uint64_t size) {
		const auto start=cursor;
		delayedCount=Number(4);
		Charge(delayedCount*32);
		for (const unsigned stride:{1u,8u}) {
			const auto length=Number(4),expected=delayedCount*stride;
			if (length==0xffffffffu) continue; // omitted table
			if (length==0) Take(expected);
			else CheckBlosc(Take(length),static_cast<std::size_t>(expected));
		}
		Require(cursor-start==size,"delayed-load metadata length");
	}

	void Metadata() {
		const auto count=Number(4);
		Require(count<=1024,"metadata count");
		std::set<std::string_view> names;
		for (unsigned i=0;i<count;++i) {
			const auto name=Name(),type=Name();
			Require(names.insert(name).second,"duplicate metadata");
			const auto size=Number(4);
			if (type=="__delayedload" && name=="file_delayed_load") {
				Require(size<=1u<<24,"delayed-load metadata length");
				Charge(1024+4*size);
				DelayedMetadata(size);
				continue;
			}
			Require(size<=65536,"metadata length");
			unsigned expected=0;
			if (type=="string") expected=static_cast<unsigned>(size);
			else if (type=="bool") expected=1;
			else if (type=="int32" || type=="float") expected=4;
			else if (type=="int64" || type=="double" || type=="vec2i" || type=="vec2s") expected=8;
			else if (type=="vec3s" || type=="vec3i") expected=12;
			else if (type=="vec4s" || type=="vec4i" || type=="vec2d") expected=16;
			else if (type=="vec3d") expected=24;
			else if (type=="vec4d") expected=32;
			else if (type=="mat4s") expected=64;
			else if (type=="mat4d") expected=128;
			else Require(false,"unsupported metadata type");
			Require(size==expected,"metadata type length");
			const auto value=Take(size);
			if (type=="bool") Require(Integer(value)<=1,"bool metadata value");
			if (name=="is_saved_as_half") Require(type=="bool" && Integer(value)==0,"half storage");
			Charge(1024+4*(size+name.size()+type.size()));
		}
	}

	static unsigned Count(std::string_view mask) {
		unsigned count=0;
		for (unsigned char v:mask) {
			v=static_cast<unsigned char>(v-((v>>1)&0x55));
			v=static_cast<unsigned char>((v&0x33)+((v>>2)&0x33));
			count+=(v+(v>>4))&0x0f;
		}
		return count;
	}

	void Values(std::string_view mask) {
		const auto code=Number(1);
		Require(code<=6,"node mask encoding");
		if (code==2 || code==4) Take(width);
		if (code==5) Take(2*width);
		if (code>=3 && code<=5) Take(mask.size());
		const std::size_t count=(compression&2) && code!=6 ? Count(mask) : mask.size()*8;
		const std::size_t expected=count*width;
		if (!(compression&5)) { Take(expected); return; }
		const auto length=Number(8);
		if (length>>63) {
			Require((~length+1)==expected,"raw chunk size");
			Take(expected);
			return;
		}
		Require(length<=expected*2+64,"compressed chunk size");
		const auto chunk=Take(length);
		if (compression&4) CheckBlosc(chunk,expected);
	}

	void Node(unsigned slots) {
		// This covers the node table, temporary decoded values, masks and slop
		// for the allocator; all nodes are charged cumulatively across grids.
		Charge(4096+64ull*slots);
		const auto children=Take(slots/8),active=Take(slots/8);
		for (std::size_t i=0;i<children.size();++i)
			Require((children[i]&active[i])==0,"child also marked as active tile");
		Values(active);
		const auto count=Count(children);
		for (unsigned i=0;i<count;++i) {
			if (slots==32768) Node(4096);
			else {
				Charge(1024+512ull*(width+4));
				leaves.push_back(Take(64));
			}
		}
	}

	using Key=std::array<std::int64_t,3>;
	Key RootKey() {
		Key key;
		for (auto& axis:key) {
			const auto raw=Number(4);
			Require((raw&4095)==0,"unaligned root origin");
			axis=static_cast<std::int64_t>(raw)-static_cast<std::int64_t>(raw>>31)*INT64_C(4294967296);
		}
		return key;
	}

	void Grid() {
		Charge(1u<<20);
		Require(gridNames.insert(Name()).second,"duplicate grid identifier");
		const auto type=Name();
		if (type=="Tree_float_5_4_3") width=4;
		else if (type=="Tree_vec3s_5_4_3") width=12;
		else Require(false,"unsupported tree type");
		Require(Name().empty(),"instanced tree");
		const auto start=Number(8),buffers=Number(8),end=Number(8);
		Require(start==cursor && start<=buffers && buffers<=end && end<=bytes.size(),"grid offsets");
		delayedCount=0;
		compression=static_cast<unsigned>(Number(4));
		Require(compression<=6 && (compression&5)!=5,"compression flags");
		Metadata();
		const auto map=Name();
		if (map=="UniformScaleMap" || map=="ScaleMap") Take(120);
		else if (map=="UniformScaleTranslateMap" || map=="ScaleTranslateMap") Take(144);
		else if (map=="AffineMap" || map=="UnitaryMap") Take(128);
		else if (map=="TranslationMap") Take(24);
		else Require(false,"unsupported transform type");
		Require(Number(4)==1,"tree buffer count");
		Take(width);
		const auto tiles=Number(4),children=Number(4);
		Charge(256*(tiles+children));
		std::vector<Key> tileKeys;
		Require(tiles<=(bytes.size()-cursor)/(12+width+1),"truncated root tiles");
		tileKeys.reserve(static_cast<std::size_t>(tiles));
		for (std::uint64_t i=0;i<tiles;++i) {
			const auto key=RootKey();
			Require(tileKeys.empty() || tileKeys.back()<key,"root tile order");
			tileKeys.push_back(key);
			Take(width);
			Require(Number(1)<=1,"root tile activity");
		}
		leaves.clear();
		Key last{};
		for (std::uint64_t i=0;i<children;++i) {
			const auto key=RootKey();
			Require((i==0 || last<key) && !std::binary_search(tileKeys.begin(),tileKeys.end(),key),
				"root child order or collision");
			last=key;
			Node(32768);
		}
		Require(!delayedCount || delayedCount==leaves.size(),"delayed-load leaf count");
		Require(cursor==buffers,"leaf data offset");
		for (const auto topology:leaves) {
			const auto active=Take(64);
			Require(active==topology,"inconsistent leaf masks");
			Values(active);
		}
		Require(cursor==end,"grid end offset");
	}
};

}}
#endif // RISE_ENABLE_OPENVDB
#endif
