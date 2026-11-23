#include "pch.h"
#include "SimulationMesh.h"
#include "../Utilities/RISECBOR64.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr char Magic[] = "RISEMESH";
constexpr std::uint32_t FlagVelocities = 1, FlagNormals = 2, KnownFlags = FlagVelocities|FlagNormals;
constexpr std::size_t DigestBytes = 32;

void Require(bool ok, const char* message)
{
	if (!ok) throw std::runtime_error(message);
}

std::size_t Product(std::size_t a, std::size_t b)
{
	Require(a==0 || b <= (std::numeric_limits<std::size_t>::max)()/a, "mesh size overflow");
	return a*b;
}

struct Budget {
	std::size_t left;
	void Take(std::size_t bytes) {
		Require(bytes<=left, "mesh memory budget exceeded");
		left-=bytes;
	}
};

void ChargeStorage(const MeshFrame& f, Budget& budget)
{
	budget.Take(f.positionUnits.capacity()+1);
	budget.Take(f.velocityUnits.capacity()+1);
	for (const auto* v : {&f.positions,&f.velocities,&f.normals,&f.indices})
		budget.Take(Product(v->capacity(),sizeof(std::uint32_t)));
}

void CheckText(const std::string& s, Budget scratch)
{
	Require(!s.empty() && s.size()<=65535 && s.find('\0')==std::string::npos, "invalid mesh units text");
	if (std::all_of(s.begin(),s.end(),[](unsigned char c) { return c<0x80; })) return;
	scratch.Take(Product(s.size(),18*sizeof(std::uint32_t)));
	Require(RISE::RISECBOR64::IsUTF8NFC(s), "mesh units text is not NFC UTF-8");
}

bool Finite(std::uint32_t bits) { return ((bits>>23)&0xff)!=0xff; }

// Structural validation shared by encoder and decoder.
void CheckFrame(const MeshFrame& f, Budget& scratch)
{
	CheckText(f.positionUnits,scratch);
	Require(f.positions.size()%3==0 && f.indices.size()%3==0, "mesh arrays are not xyz triples");
	const std::size_t vertices=f.positions.size()/3, triangles=f.indices.size()/3;
	Require(vertices<=(std::numeric_limits<std::uint32_t>::max)() &&
		triangles<=(std::numeric_limits<std::uint32_t>::max)(), "too many mesh elements");
	Require(f.velocities.empty() || f.velocities.size()==f.positions.size(), "mesh velocity count mismatch");
	Require(f.normals.empty() || f.normals.size()==f.positions.size(), "mesh normal count mismatch");
	Require(f.velocities.empty()==f.velocityUnits.empty(), "mesh velocity units without velocities or vice versa");
	if (!f.velocities.empty()) CheckText(f.velocityUnits,scratch);
	for (const auto* v : {&f.positions,&f.velocities,&f.normals})
		Require(std::all_of(v->begin(),v->end(),Finite), "nonfinite mesh sample");
	for (std::size_t t=0;t<triangles;++t) {
		const auto a=f.indices[3*t], b=f.indices[3*t+1], c=f.indices[3*t+2];
		Require(a<vertices && b<vertices && c<vertices, "mesh index out of range");
		Require(a!=b && b!=c && a!=c, "degenerate mesh triangle");
	}
}

struct Sink {
	Bytes* data;
	std::size_t count=0;
	void Raw(const void* ptr,std::size_t n) {
		Require(n <= (std::numeric_limits<std::size_t>::max)()-count,"mesh wire size overflow");
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
	void Words(const std::vector<std::uint32_t>& v) {
		if (data) for (auto w:v) Word(w,4);
		else Raw(nullptr,Product(v.size(),4));
	}
};

// Returns the offset of the digest slot; the digested region follows it.
std::size_t Write(Sink& dst,const MeshFrame& f)
{
	const std::uint32_t flags=(f.velocities.empty()?0:FlagVelocities)|(f.normals.empty()?0:FlagNormals);
	dst.Raw(Magic,8); dst.Word(1,4); dst.Word(flags,4);
	dst.Text(f.positionUnits);
	if (flags&FlagVelocities) dst.Text(f.velocityUnits);
	const std::size_t slot=dst.count;
	const std::uint8_t zero[DigestBytes]={};
	dst.Raw(zero,DigestBytes);
	dst.Word(f.positions.size()/3,4); dst.Word(f.indices.size()/3,4);
	dst.Words(f.indices);
	dst.Words(f.positions); dst.Words(f.velocities); dst.Words(f.normals);
	return slot;
}

std::string Hex(const std::uint8_t* p)
{
	static const char digits[]="0123456789abcdef";
	std::string s(2*DigestBytes,'0');
	for (std::size_t i=0;i<DigestBytes;++i) { s[2*i]=digits[p[i]>>4]; s[2*i+1]=digits[p[i]&15]; }
	return s;
}

void Unhex(const std::string& hex,std::uint8_t* p)
{
	auto nibble=[](char c) { return static_cast<std::uint8_t>(c<='9' ? c-'0' : c-'a'+10); };
	for (std::size_t i=0;i<DigestBytes;++i) p[i]=static_cast<std::uint8_t>((nibble(hex[2*i])<<4)|nibble(hex[2*i+1]));
}

struct Source {
	const Bytes& bytes;
	std::size_t pos=0;
	std::size_t Left() const { return bytes.size()-pos; }
	const std::uint8_t* Read(std::size_t n) {
		Require(n<=Left(),"truncated mesh payload");
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
		Budget scratch=budget;
		scratch.Take(2*n+64);
		std::string text(reinterpret_cast<const char*>(p),n);
		budget.Take(text.capacity()+1);
		return text;
	}
	void Words(std::vector<std::uint32_t>& v,std::size_t n,Budget& budget) {
		Require(n<=Left()/4,"truncated mesh arrays");
		budget.Take(Product(n,4));
		v.resize(n);
		for (auto& w:v) w=static_cast<std::uint32_t>(Word(4));
	}
};

struct Header {
	std::uint32_t flags=0;
	std::size_t digestAt=0, vertices=0, triangles=0;
};

Header ReadHeader(Source& src,Budget& budget,MeshFrame* frame)
{
	Header h;
	Require(src.Left()>=8 && std::memcmp(src.Read(8),Magic,8)==0 && src.Word(4)==1,"unsupported mesh payload");
	h.flags=static_cast<std::uint32_t>(src.Word(4));
	Require((h.flags&~KnownFlags)==0,"unknown mesh flags");
	std::string units=src.Text(budget), velocityUnits;
	if (h.flags&FlagVelocities) velocityUnits=src.Text(budget);
	h.digestAt=src.pos;
	src.Read(DigestBytes);
	h.vertices=static_cast<std::size_t>(src.Word(4));
	h.triangles=static_cast<std::size_t>(src.Word(4));
	if (frame) { frame->positionUnits=std::move(units); frame->velocityUnits=std::move(velocityUnits); }
	return h;
}
}

std::string RISE::Simulation::MeshTopologyDigest(std::uint32_t vertices,const std::vector<std::uint32_t>& indices)
{
	Bytes region;
	Sink dst{&region};
	dst.Word(vertices,4); dst.Word(indices.size()/3,4); dst.Words(indices);
	return RISE::RISECBOR64::SHA256Hex(region.data(),region.size());
}

bool RISE::Simulation::EncodeMesh(const MeshFrame& frame,std::size_t budget,Bytes& out,std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(out.capacity());
		ChargeStorage(frame,remaining);
		CheckFrame(frame,remaining);
		Sink sizing{nullptr};
		Write(sizing,frame);
		remaining.Take(sizing.count);
		Bytes candidate;
		candidate.reserve(sizing.count);
		Sink dst{&candidate};
		const auto slot=Write(dst,frame);
		const auto region=slot+DigestBytes;
		Unhex(RISE::RISECBOR64::SHA256Hex(candidate.data()+region,8+frame.indices.size()*4),candidate.data()+slot);
		out.swap(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}

bool RISE::Simulation::DecodeMesh(const Bytes& bytes,std::size_t budget,MeshFrame& out,std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(bytes.capacity());
		ChargeStorage(out,remaining);
		Source src{bytes};
		MeshFrame candidate;
		const auto h=ReadHeader(src,remaining,&candidate);
		// Bound every array by the bytes actually present before allocating.
		const std::size_t perVertex=12*(1+((h.flags&FlagVelocities)?1:0)+((h.flags&FlagNormals)?1:0));
		Require(h.triangles<=src.Left()/12 && h.vertices<=(src.Left()-12*h.triangles)/perVertex,
			"invalid mesh element counts");
		const auto regionAt=h.digestAt+DigestBytes;
		src.Words(candidate.indices,3*h.triangles,remaining);
		const auto digest=RISE::RISECBOR64::SHA256Hex(bytes.data()+regionAt,src.pos-regionAt);
		Require(digest==Hex(bytes.data()+h.digestAt),"mesh topology digest mismatch");
		src.Words(candidate.positions,3*h.vertices,remaining);
		if (h.flags&FlagVelocities) src.Words(candidate.velocities,3*h.vertices,remaining);
		if (h.flags&FlagNormals) src.Words(candidate.normals,3*h.vertices,remaining);
		Require(src.Left()==0,"trailing mesh payload bytes");
		CheckFrame(candidate,remaining);
		out=std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}

bool RISE::Simulation::PeekMeshTopology(const Bytes& bytes,MeshTopology& out,std::string& error)
{
	try {
		Budget scratch{(std::numeric_limits<std::size_t>::max)()};
		Source src{bytes};
		const auto h=ReadHeader(src,scratch,nullptr);
		MeshTopology candidate;
		candidate.vertices=static_cast<std::uint32_t>(h.vertices);
		candidate.triangles=static_cast<std::uint32_t>(h.triangles);
		candidate.digest=Hex(bytes.data()+h.digestAt);
		out=std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}
