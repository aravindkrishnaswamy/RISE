#include "pch.h"
#include "SimulationGrid.h"
#include "../Utilities/RISECBOR64.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
using Origin = std::array<std::int32_t,3>;
using Dims = std::array<std::uint32_t,3>;
constexpr char Magic[] = "RISEGRID";
static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559,
	"native grid transforms require IEEE binary64");

void Require(bool ok, const char* message)
{
	if (!ok) throw std::runtime_error(message);
}

std::size_t Product(std::size_t a, std::size_t b)
{
	Require(a==0 || b <= (std::numeric_limits<std::size_t>::max)()/a, "grid size overflow");
	return a*b;
}

struct Budget {
	std::size_t left;
	void Take(std::size_t bytes) {
		Require(bytes<=left, "grid memory budget exceeded");
		left-=bytes;
	}
};

std::size_t MaskSize(std::size_t n) { return n/8 + (n%8!=0); }

void ChargeStorage(const GridFrame& frame,Budget& budget)
{
	budget.Take(Product(frame.channels.capacity(),sizeof(GridChannel)));
	for (const auto& c:frame.channels) {
		budget.Take(c.name.capacity()+1);
		budget.Take(c.units.capacity()+1);
		budget.Take(Product(c.blocks.capacity(),sizeof(GridBlock)));
		for (const auto& b:c.blocks) {
			budget.Take(b.active.capacity());
			budget.Take(Product(b.values.capacity(),sizeof(std::uint32_t)));
		}
	}
}

void CheckText(const std::string& s, Budget scratch)
{
	Require(!s.empty() && s.size()<=65535 && s.find('\0')==std::string::npos, "invalid grid string");
	if (std::all_of(s.begin(),s.end(),[](unsigned char c) { return c<0x80; })) return;
	// NFC validation holds input code points, canonical decompositions (up
	// to four per scalar) and recomposition, including vector growth.
	scratch.Take(Product(s.size(),18*sizeof(std::uint32_t)));
	Require(RISE::RISECBOR64::IsUTF8NFC(s), "grid string is not NFC UTF-8");
}

// Every finite binary64 is an integer mantissa times 2^(shift-1074).
// The six triple-products of a 3x3 determinant therefore share the base
// 2^-3222. Accumulate positive and negative magnitudes separately in base
// 2^16. Even the sum of six maximal triples occupies < 6300 bits.
using Magnitude = std::array<std::uint16_t,394>;

struct Dyadic {
	std::uint64_t mantissa;
	unsigned shift;
	bool negative;
};

Dyadic Parts(double value)
{
	std::uint64_t bits;
	std::memcpy(&bits,&value,8);
	const unsigned exp=static_cast<unsigned>((bits>>52)&0x7ff);
	const auto fraction=bits&0xfffffffffffffULL;
	return {exp ? fraction | (1ULL<<52) : fraction, exp ? exp-1 : 0, (bits>>63)!=0};
}

void AddProduct(Magnitude& sum,const Dyadic& a,const Dyadic& b,const Dyadic& c)
{
	const unsigned shift=a.shift+b.shift+c.shift;
	for (unsigned i=0;i<4;++i) for (unsigned j=0;j<4;++j) for (unsigned k=0;k<4;++k) {
		std::uint64_t partial=((a.mantissa>>(i*16))&65535) *
			((b.mantissa>>(j*16))&65535) * ((c.mantissa>>(k*16))&65535);
		partial<<=shift%16;
		unsigned limb=shift/16+i+j+k;
		while (partial) {
			partial+=sum[limb];
			sum[limb++]=static_cast<std::uint16_t>(partial);
			partial>>=16;
		}
	}
}

bool ValidTransform(const std::array<double,16>& m)
{
	for (double value:m) if (!std::isfinite(value)) return false;
	if (m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1) return false;
	Magnitude positive{}, negative{};
	for (unsigned i=0;i<3;++i) for (unsigned flip=0;flip<2;++flip) {
		const unsigned j=(i+1+flip)%3, k=(i+2-flip)%3;
		const auto a=Parts(m[i]), b=Parts(m[4+j]), c=Parts(m[8+k]);
		const bool subtract=(flip!=0)^a.negative^b.negative^c.negative;
		AddProduct(subtract ? negative : positive,a,b,c);
	}
	return positive!=negative;
}

struct Box {
	std::array<std::int64_t,3> low{}, high{}; // high exclusive
};

Box Bounds(const Origin& origin,const Dims& dims)
{
	Box bounds;
	for (unsigned i=0;i<3;++i) {
		bounds.low[i]=origin[i];
		bounds.high[i]=bounds.low[i]+dims[i];
		Require(dims[i] && bounds.high[i]-1 <= (std::numeric_limits<std::int32_t>::max)(),
			"invalid grid bounds");
	}
	return bounds;
}

std::size_t Cells(const Dims& dims) { return Product(Product(dims[0],dims[1]),dims[2]); }

void CheckChannel(const GridChannel& c,Budget& remaining)
{
	CheckText(c.name,remaining);
	CheckText(c.units,remaining);
	Require(c.components==1 || c.components==3,"unsupported grid component count");
	Require(ValidTransform(c.indexToWorld),"invalid grid transform");
	Bounds(c.origin,c.dimensions);
	const auto cells=Cells(c.dimensions);
	remaining.Take(Product(Product(cells,c.components),sizeof(std::uint32_t)));
	remaining.Take(MaskSize(cells));
	for (unsigned i=c.components;i<3;++i) Require(c.background[i]==0,"unused background component");
}

struct BlockSizes { std::size_t mask, values, cells; };

BlockSizes Sizes(const GridChannel& c,const GridBlock& b)
{
	const Box domain=Bounds(c.origin,c.dimensions), box=Bounds(b.origin,b.dimensions);
	for (unsigned i=0;i<3;++i) Require(box.low[i]>=domain.low[i] && box.high[i]<=domain.high[i],
		"grid block outside domain");
	const auto cells=Cells(b.dimensions);
	return {b.constant ? 1 : MaskSize(cells), Product(b.constant ? 1 : cells,c.components),cells};
}

bool Intersects(const Box& a,const Box& b)
{
	for (unsigned i=0;i<3;++i) if (a.low[i]>=b.high[i] || b.low[i]>=a.high[i]) return false;
	return true;
}

// Balanced bounds tree for rectangular tiles. A sweep on x alone becomes
// quadratic on a single yz plane of disjoint voxels.
class OverlapCheck {
	struct Node {
		Box box;
		std::size_t left=0, right=0;
	};
	std::vector<Node> nodes;
	std::vector<Box> boxes;

	std::size_t Build(std::size_t first,std::size_t end) {
		Box bounds=boxes[first];
		std::array<std::int64_t,3> minCenter{},maxCenter{};
		for (unsigned axis=0;axis<3;++axis)
			minCenter[axis]=maxCenter[axis]=bounds.low[axis]+bounds.high[axis];
		for (std::size_t j=first+1;j<end;++j) for (unsigned axis=0;axis<3;++axis) {
			const auto center=boxes[j].low[axis]+boxes[j].high[axis];
			minCenter[axis]=(std::min)(center,minCenter[axis]);
			maxCenter[axis]=(std::max)(center,maxCenter[axis]);
			bounds.low[axis]=(std::min)(bounds.low[axis],boxes[j].low[axis]);
			bounds.high[axis]=(std::max)(bounds.high[axis],boxes[j].high[axis]);
		}
		const std::size_t index=nodes.size();
		nodes.push_back({bounds,0,0});
		if (end-first>1) {
			unsigned axis=0;
			for (unsigned i=1;i<3;++i)
				if (maxCenter[i]-minCenter[i]>maxCenter[axis]-minCenter[axis]) axis=i;
			const auto mid=first+(end-first)/2;
			std::nth_element(boxes.begin()+first,boxes.begin()+mid,boxes.begin()+end,
				[axis](const Box& a,const Box& b) {
					return a.low[axis]+a.high[axis]<b.low[axis]+b.high[axis];
				});
			nodes[index].left=Build(first,mid);
			nodes[index].right=Build(mid,end);
		}
		return index;
	}

	void Between(std::size_t a,std::size_t b) const {
		if (!Intersects(nodes[a].box,nodes[b].box)) return;
		if (nodes[a].left) {
			Between(nodes[a].left,b);
			Between(nodes[a].right,b);
		} else if (nodes[b].left) {
			Between(a,nodes[b].left);
			Between(a,nodes[b].right);
		} else throw std::runtime_error("overlapping grid blocks");
	}

public:
	void Run(const GridChannel& c,Budget scratch) {
		const auto n=c.blocks.size();
		if (n<2) return;
		const auto count=Product(n,2)-1;
		scratch.Take(Product(n,sizeof(Box)));
		scratch.Take(Product(count,sizeof(Node)));
		nodes.reserve(count);
		boxes.reserve(n);
		for (const auto& b:c.blocks) boxes.push_back(Bounds(b.origin,b.dimensions));
		Build(0,n);
		for (const auto& node:nodes) if (node.left) Between(node.left,node.right);
	}
};

void CheckBlocks(const GridChannel& c,Budget scratch)
{
	Require(c.blocks.size() <= (std::numeric_limits<std::uint32_t>::max)(), "too many grid blocks");
	for (std::size_t i=0;i<c.blocks.size();++i) {
		const auto& b=c.blocks[i];
		Require(i==0 || c.blocks[i-1].origin<b.origin, "unordered grid block origins");
		const auto sizes=Sizes(c,b);
		Require(b.active.size()==sizes.mask && b.values.size()==sizes.values,"grid block array size mismatch");
		if (b.constant) Require(b.active.front()<=1,"invalid constant grid activity");
		else if (sizes.cells%8) Require((b.active.back()>>(sizes.cells%8))==0,"nonzero grid mask padding");
	}
	OverlapCheck().Run(c,scratch);
}

struct Sink {
	Bytes* data;
	std::size_t count=0;
	void Raw(const void* ptr,std::size_t n) {
		Require(n <= (std::numeric_limits<std::size_t>::max)()-count,"grid wire size overflow");
		count+=n;
		if (data && n) {
			const auto p=static_cast<const std::uint8_t*>(ptr);
			data->insert(data->end(),p,p+n);
		}
	}
	void Word(std::uint64_t value,unsigned width) {
		std::uint8_t bytes[8];
		for (unsigned i=0;i<width;++i) bytes[i]=static_cast<std::uint8_t>(value>>(8*i));
		Raw(bytes,width);
	}
	void Text(const std::string& s) { Word(s.size(),2); Raw(s.data(),s.size()); }
	void Extent(const Origin& origin,const Dims& dims) {
		for (auto i:origin) Word(static_cast<std::uint32_t>(i),4);
		for (auto d:dims) Word(d,4);
	}
};

void Write(Sink& dst,const GridFrame& frame)
{
	dst.Raw(Magic,8); dst.Word(1,4); dst.Word(frame.channels.size(),4);
	for (const auto& c:frame.channels) {
		dst.Text(c.name); dst.Text(c.units); dst.Word(c.components,1);
		dst.Extent(c.origin,c.dimensions);
		for (auto m:c.indexToWorld) {
			std::uint64_t word;
			std::memcpy(&word,&m,8);
			dst.Word(word,8);
		}
		for (unsigned i=0;i<c.components;++i) dst.Word(c.background[i],4);
		dst.Word(c.blocks.size(),4);
		for (const auto& b:c.blocks) {
			dst.Extent(b.origin,b.dimensions); dst.Word(b.constant,1);
			dst.Raw(b.active.data(),b.active.size());
			if (dst.data) for (auto sample:b.values) dst.Word(sample,4);
			else dst.Raw(nullptr,Product(b.values.size(),4));
		}
	}
}

struct Source {
	const Bytes& bytes;
	std::size_t pos=0;
	std::size_t Left() const { return bytes.size()-pos; }
	const std::uint8_t* Read(std::size_t n) {
		Require(n<=Left(),"truncated grid payload");
		auto p=bytes.data()+pos;
		pos+=n;
		return p;
	}
	std::uint64_t Word(unsigned width) {
		const auto p=Read(width);
		std::uint64_t value=0;
		for (unsigned i=0;i<width;++i) value|=std::uint64_t(p[i])<<(8*i);
		return value;
	}
	std::string Text(Budget& budget) {
		const auto n=static_cast<std::size_t>(Word(2));
		const auto p=Read(n);
		// A sized string constructor can round capacity up. Allow for growth
		// and alignment on libc++, libstdc++, and MSVC before allocating.
		// Charge the actual capacity for the rest of this decode.
		Budget scratch=budget;
		scratch.Take(2*n+64); // n is a u16
		std::string text(reinterpret_cast<const char*>(p),n);
		budget.Take(text.capacity()+1);
		return text;
	}
	void Extent(Origin& origin,Dims& dims) {
		for (auto& i:origin) {
			const auto raw=static_cast<std::int64_t>(Word(4));
			i=static_cast<std::int32_t>(raw>=0x80000000LL ? raw-0x100000000LL : raw);
		}
		for (auto& d:dims) d=static_cast<std::uint32_t>(Word(4));
	}
};

void ReadFrame(Source& src,Budget budget,GridFrame& frame)
{
	Require(std::memcmp(src.Read(8),Magic,8)==0 && src.Word(4)==1,"unsupported grid payload");
	const auto count=static_cast<std::size_t>(src.Word(4));
	// Minimum sizes include one-byte name/units and one-component background.
	Require(count && count<=src.Left()/167,"invalid grid channel count");
	budget.Take(Product(count,sizeof(GridChannel)));
	frame.channels.resize(count);
	for (std::size_t i=0;i<count;++i) {
		auto& c=frame.channels[i];
		c.name=src.Text(budget);
		c.units=src.Text(budget);
		Require(!i || frame.channels[i-1].name<c.name,"unordered grid channels");
		c.components=static_cast<std::uint8_t>(src.Word(1));
		src.Extent(c.origin,c.dimensions);
		for (auto& m:c.indexToWorld) {
			const auto word=src.Word(8);
			std::memcpy(&m,&word,8);
		}
		CheckChannel(c,budget);
		for (unsigned k=0;k<c.components;++k) c.background[k]=static_cast<std::uint32_t>(src.Word(4));
		const auto blocks=static_cast<std::size_t>(src.Word(4));
		Require(blocks<=src.Left()/30,"invalid grid block count");
		budget.Take(Product(blocks,sizeof(GridBlock)));
		c.blocks.resize(blocks);
		for (auto& b:c.blocks) {
			src.Extent(b.origin,b.dimensions);
			const auto flag=src.Word(1);
			Require(flag<=1,"invalid grid block type");
			b.constant=flag==1;
			const auto sizes=Sizes(c,b);
			const auto mask=src.Read(sizes.mask);
			Require(sizes.values<=src.Left()/4,"truncated grid values");
			budget.Take(sizes.mask);
			budget.Take(Product(sizes.values,4));
			b.active.assign(mask,mask+sizes.mask);
			b.values.resize(sizes.values);
			for (auto& v:b.values) v=static_cast<std::uint32_t>(src.Word(4));
		}
		CheckBlocks(c,budget);
	}
	Require(src.Left()==0,"trailing grid payload bytes");
}
}

bool RISE::Simulation::EncodeGrid(const GridFrame& frame,std::size_t budget,Bytes& out,std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(out.capacity());
		ChargeStorage(frame,remaining);
		Require(!frame.channels.empty() && frame.channels.size()<=(std::numeric_limits<std::uint32_t>::max)(),
			"invalid grid channel count");
		for (std::size_t i=0;i<frame.channels.size();++i) {
			const auto& c=frame.channels[i];
			Require(!i || frame.channels[i-1].name<c.name,"unordered grid channels");
			CheckChannel(c,remaining);
			CheckBlocks(c,remaining);
		}
		Sink sizing{nullptr};
		Write(sizing,frame);
		remaining.Take(sizing.count);
		Bytes candidate;
		candidate.reserve(sizing.count);
		Sink dst{&candidate};
		Write(dst,frame);
		out.swap(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}

bool RISE::Simulation::DecodeGrid(const Bytes& bytes,std::size_t budget,GridFrame& out,std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(bytes.capacity());
		ChargeStorage(out,remaining);
		Source src{bytes};
		GridFrame candidate;
		ReadFrame(src,remaining,candidate);
		out=std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}
