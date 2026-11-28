//////////////////////////////////////////////////////////////////////
//
//  SimulationMesh.cpp - Binary codecs for "mesh.v1", "mesh.v2", and static topology
//
//  Supports inline ("RISEMESH" v1/v2) and out-of-band static topology/UV
//  ("RISEMTOP" v1) payloads with SHA-256 topology integrity verification.
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SimulationMesh.h"
#include "../Utilities/RISECBOR64.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr char Magic[] = "RISEMESH";
constexpr char StaticMagic[] = "RISEMTOP";
constexpr std::uint32_t FlagVelocities = 1, FlagNormals = 2, FlagStaticTopology = 4;
constexpr std::uint32_t KnownFlags = FlagVelocities|FlagNormals;
constexpr std::uint32_t KnownFlagsV2 = FlagVelocities|FlagNormals|FlagStaticTopology;
constexpr std::size_t DigestBytes = 32;
constexpr unsigned MaxComponents = 16;

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
	budget.Take(Product(f.uvSets.capacity(),sizeof(MeshUVSet)));
	for (const auto& s : f.uvSets) {
		budget.Take(s.name.capacity()+s.units.capacity()+2);
		budget.Take(Product(s.values.capacity(),sizeof(std::uint32_t)));
	}
	budget.Take(Product(f.attributes.capacity(),sizeof(MeshAttribute)));
	for (const auto& a : f.attributes) {
		budget.Take(a.name.capacity()+a.units.capacity()+2);
		budget.Take(Product(a.values.capacity(),sizeof(std::uint32_t)));
	}
}

void CheckText(const std::string& s, Budget scratch)
{
	Require(!s.empty() && s.size()<=65535 && s.find('\0')==std::string::npos, "invalid mesh units or name text");
	if (std::all_of(s.begin(),s.end(),[](unsigned char c) { return c<0x80; })) return;
	scratch.Take(Product(s.size(),18*sizeof(std::uint32_t)));
	Require(RISE::RISECBOR64::IsUTF8NFC(s), "mesh units or name text is not NFC UTF-8");
}

bool Finite(std::uint32_t bits) { return ((bits>>23)&0xff)!=0xff; }

template<class T> void CheckNamed(const std::vector<T>& items, Budget& scratch)
{
	Require(items.size()<=65535, "too many mesh UV sets or attributes");
	for (std::size_t i=0;i<items.size();++i) {
		CheckText(items[i].name,scratch);
		CheckText(items[i].units,scratch);
		Require(i==0 || items[i-1].name<items[i].name, "mesh UV sets and attributes must be sorted by unique name");
		Require(std::all_of(items[i].values.begin(),items[i].values.end(),Finite), "nonfinite mesh sample");
	}
}

// Structural validation shared by encoder and decoder.
void CheckFrame(const MeshFrame& f, Budget& scratch, bool v2)
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
	if (!v2) {
		Require(f.uvSets.empty() && f.attributes.empty(), "mesh.v1 carries no UV sets or attributes (use mesh.v2)");
		return;
	}
	CheckNamed(f.uvSets,scratch);
	CheckNamed(f.attributes,scratch);
	for (const auto& s : f.uvSets) Require(s.values.size()==Product(vertices,2), "mesh UV set count mismatch");
	for (const auto& a : f.attributes)
		Require(a.components>=1 && a.components<=MaxComponents && a.values.size()==Product(vertices,a.components),
			"mesh attribute components or count mismatch");
}

// Two-pass little-endian writer (null `data` sizes payload before allocation).
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

// mesh.v2 frame and mesh.topology.v1 static record. `uvs` are the frame's own
// UV sets; `ref` (32 raw bytes) replaces the inline indices when given.
void WriteV2(Sink& dst,const MeshFrame& f,const std::uint8_t* topology,const std::uint8_t* ref,
	const std::vector<const MeshUVSet*>& uvs)
{
	const std::uint32_t flags=(f.velocities.empty()?0:FlagVelocities)|(f.normals.empty()?0:FlagNormals)|
		(ref?FlagStaticTopology:0);
	dst.Raw(Magic,8); dst.Word(2,4); dst.Word(flags,4);
	dst.Text(f.positionUnits);
	if (flags&FlagVelocities) dst.Text(f.velocityUnits);
	dst.Raw(topology,DigestBytes);
	dst.Word(f.positions.size()/3,4); dst.Word(f.indices.size()/3,4);
	if (ref) dst.Raw(ref,DigestBytes);
	else dst.Words(f.indices);
	dst.Words(f.positions); dst.Words(f.velocities); dst.Words(f.normals);
	dst.Word(uvs.size(),2);
	for (const auto* s : uvs) { dst.Text(s->name); dst.Text(s->units); dst.Words(s->values); }
	dst.Word(f.attributes.size(),2);
	for (const auto& a : f.attributes) { dst.Text(a.name); dst.Text(a.units); dst.Word(a.components,1); dst.Words(a.values); }
}

// Writes the out-of-band "RISEMTOP" v1 static record (counts, indices, static UVs).
void WriteStatic(Sink& dst,const MeshFrame& f,const std::vector<const MeshUVSet*>& uvs)
{
	dst.Raw(StaticMagic,8); dst.Word(1,4);
	dst.Word(f.positions.size()/3,4); dst.Word(f.indices.size()/3,4);
	dst.Words(f.indices);
	dst.Word(uvs.size(),2);
	for (const auto* s : uvs) { dst.Text(s->name); dst.Text(s->units); dst.Words(s->values); }
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
	unsigned version=1;
	std::size_t digestAt=0, vertices=0, triangles=0, referenceAt=0;
};

Header ReadHeader(Source& src,Budget& budget,MeshFrame* frame)
{
	Header h;
	Require(src.Left()>=8 && std::memcmp(src.Read(8),Magic,8)==0,"unsupported mesh payload");
	const auto version=src.Word(4);
	Require(version==1 || version==2,"unsupported mesh payload");
	h.version=static_cast<unsigned>(version);
	h.flags=static_cast<std::uint32_t>(src.Word(4));
	Require((h.flags&~(h.version==1?KnownFlags:KnownFlagsV2))==0,"unknown mesh flags");
	std::string units=src.Text(budget), velocityUnits;
	if (h.flags&FlagVelocities) velocityUnits=src.Text(budget);
	h.digestAt=src.pos;
	src.Read(DigestBytes);
	h.vertices=static_cast<std::size_t>(src.Word(4));
	h.triangles=static_cast<std::size_t>(src.Word(4));
	if (h.flags&FlagStaticTopology) { h.referenceAt=src.pos; src.Read(DigestBytes); }
	if (frame) { frame->positionUnits=std::move(units); frame->velocityUnits=std::move(velocityUnits); }
	return h;
}

template<class T> void ReadNamed(Source& src,Budget& budget,std::size_t vertices,bool,std::vector<T>& out)
{
	// UV sets: name, units, 2V samples.
	const auto n=static_cast<std::size_t>(src.Word(2));
	Require(n<=src.Left()/6,"invalid mesh UV-set count");
	budget.Take(Product(n,sizeof(T)));
	out.resize(n);
	for (auto& item : out) {
		item.name=src.Text(budget);
		item.units=src.Text(budget);
		Require(vertices<=src.Left()/8,"truncated mesh UV sets");
		src.Words(item.values,2*vertices,budget);
	}
}
template<> void ReadNamed<MeshAttribute>(Source& src,Budget& budget,std::size_t vertices,bool,std::vector<MeshAttribute>& out)
{
	const auto n=static_cast<std::size_t>(src.Word(2));
	Require(n<=src.Left()/7,"invalid mesh attribute count");
	budget.Take(Product(n,sizeof(MeshAttribute)));
	out.resize(n);
	for (auto& item : out) {
		item.name=src.Text(budget);
		item.units=src.Text(budget);
		const auto components=static_cast<std::size_t>(src.Word(1));
		Require(components>=1 && components<=MaxComponents,"invalid mesh attribute components");
		item.components=static_cast<std::uint8_t>(components);
		Require(vertices<=src.Left()/4/components,"truncated mesh attributes");
		src.Words(item.values,vertices*components,budget);
	}
}

// Decodes "mesh.v1" (inline indices + positions/velocities/normals) and verifies topology hash.
void DecodeV1(const Bytes& bytes,Source& src,const Header& h,Budget& remaining,MeshFrame& candidate)
{
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
	CheckFrame(candidate,remaining,false);
}

// Decodes "mesh.v2": resolves optional "RISEMTOP" static topology/UVs, verifies
// the topology digest, and merges static + per-frame UV sets in sorted name order.
void DecodeV2(const Bytes& bytes,Source& src,const Header& h,Budget& remaining,const StaticPayloadResolver* resolve,
	MeshFrame& candidate)
{
	std::vector<MeshUVSet> staticUVs;
	std::shared_ptr<const Bytes> fixed;
	if (h.flags&FlagStaticTopology) {
		Require(resolve && *resolve,"mesh.v2 frame references a static topology payload and no resolver was given");
		const std::string reference=Hex(bytes.data()+h.referenceAt);
		std::string why;
		if (!(*resolve)(reference,remaining.left,fixed,why) || !fixed)
			throw std::runtime_error("mesh static topology "+reference+": "+why);
		remaining.Take(fixed->capacity());
		Source s{*fixed};
		Require(s.Left()>=20 && std::memcmp(s.Read(8),StaticMagic,8)==0 && s.Word(4)==1,"unsupported mesh topology payload");
		Require(s.Word(4)==h.vertices && s.Word(4)==h.triangles,"mesh topology payload counts differ from the frame");
		Require(h.triangles<=s.Left()/12,"truncated mesh topology payload");
		s.Words(candidate.indices,3*h.triangles,remaining);
		ReadNamed(s,remaining,h.vertices,false,staticUVs);
		Require(s.Left()==0,"trailing mesh topology payload bytes");
	} else {
		Require(h.triangles<=src.Left()/12,"invalid mesh element counts");
		src.Words(candidate.indices,3*h.triangles,remaining);
	}
	Require(h.vertices<=(std::numeric_limits<std::uint32_t>::max)(),"too many mesh elements");
	Require(MeshTopologyDigest(static_cast<std::uint32_t>(h.vertices),candidate.indices)==Hex(bytes.data()+h.digestAt),
		"mesh topology digest mismatch");
	const std::size_t perVertex=12*(1+((h.flags&FlagVelocities)?1:0)+((h.flags&FlagNormals)?1:0));
	Require(h.vertices<=src.Left()/perVertex,"invalid mesh element counts");
	src.Words(candidate.positions,3*h.vertices,remaining);
	if (h.flags&FlagVelocities) src.Words(candidate.velocities,3*h.vertices,remaining);
	if (h.flags&FlagNormals) src.Words(candidate.normals,3*h.vertices,remaining);
	std::vector<MeshUVSet> frameUVs;
	ReadNamed(src,remaining,h.vertices,false,frameUVs);
	ReadNamed(src,remaining,h.vertices,true,candidate.attributes);
	Require(src.Left()==0,"trailing mesh payload bytes");
	// Each list is canonical on its own; the merged list must not repeat a name.
	Budget scratch=remaining;
	CheckNamed(staticUVs,scratch);
	CheckNamed(frameUVs,scratch);
	remaining.Take(Product(staticUVs.size()+frameUVs.size(),sizeof(MeshUVSet)));
	candidate.uvSets.reserve(staticUVs.size()+frameUVs.size());
	std::merge(std::make_move_iterator(staticUVs.begin()),std::make_move_iterator(staticUVs.end()),
		std::make_move_iterator(frameUVs.begin()),std::make_move_iterator(frameUVs.end()),
		std::back_inserter(candidate.uvSets),[](const MeshUVSet& a,const MeshUVSet& b){return a.name<b.name;});
	CheckFrame(candidate,remaining,true);
}

bool Decode(const Bytes& bytes,std::size_t budget,const StaticPayloadResolver* resolve,MeshFrame& out,std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(bytes.capacity());
		ChargeStorage(out,remaining);
		Source src{bytes};
		MeshFrame candidate;
		const auto h=ReadHeader(src,remaining,&candidate);
		if (h.version==1) DecodeV1(bytes,src,h,remaining,candidate);
		else DecodeV2(bytes,src,h,remaining,resolve,candidate);
		out=std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}
}

// ------------------------------------------------------------------ public API

// Canonical SHA-256 topology digest over LE (u32 vertices, u32 triangles, u32[] indices).
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
		CheckFrame(frame,remaining,false);
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

namespace {
bool EncodeV2(const MeshFrame& frame,const MeshStaticLayout& layout,std::size_t budget,
	const RISE::Simulation::MeshStaticRecord* prepared,Bytes& out,Bytes& staticPayload,std::string& staticDigest,
	std::string& error);
}

bool RISE::Simulation::EncodeMeshV2(const MeshFrame& frame,const MeshStaticLayout& layout,std::size_t budget,
	Bytes& out,Bytes& staticPayload,std::string& staticDigest,std::string& error)
{
	return EncodeV2(frame,layout,budget,nullptr,out,staticPayload,staticDigest,error);
}

// Pre-encodes and digests static topology/UVs once for reuse across fixed-topology frames.
bool RISE::Simulation::PrepareMeshStatic(const MeshFrame& frame,const MeshStaticLayout& layout,std::size_t budget,
	MeshStaticRecord& record,std::string& error)
{
	if (!layout.topology) { error="a static record needs layout.topology"; return false; }
	Bytes ignored;
	MeshStaticRecord candidate;
	if (!EncodeV2(frame,layout,budget,nullptr,ignored,candidate.payload,candidate.digest,error)) return false;
	candidate.topologyDigest=MeshTopologyDigest(static_cast<std::uint32_t>(frame.positions.size()/3),frame.indices);
	record=std::move(candidate);
	return true;
}

// Encodes "mesh.v2" against a precomputed static record, skipping static re-serialization.
bool RISE::Simulation::EncodeMeshV2(const MeshFrame& frame,const MeshStaticLayout& layout,std::size_t budget,
	const MeshStaticRecord& record,Bytes& out,std::string& error)
{
	if (!layout.topology || record.digest.size()!=2*DigestBytes || record.topologyDigest.size()!=2*DigestBytes) {
		error="an invalid prepared static record";
		return false;
	}
	Bytes noPayload;
	std::string noDigest;
	return EncodeV2(frame,layout,budget,&record,out,noPayload,noDigest,error);
}

namespace {
bool EncodeV2(const MeshFrame& frame,const MeshStaticLayout& layout,std::size_t budget,
	const MeshStaticRecord* prepared,Bytes& out,Bytes& staticPayload,std::string& staticDigest,std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(out.capacity());
		remaining.Take(staticPayload.capacity());
		ChargeStorage(frame,remaining);
		CheckFrame(frame,remaining,true);
		Require(layout.topology || layout.staticUVSets.empty(),"static UV sets need a static topology");
		std::vector<const MeshUVSet*> inFrame, inStatic;
		for (const auto& s : frame.uvSets)
			(std::find(layout.staticUVSets.begin(),layout.staticUVSets.end(),s.name)!=layout.staticUVSets.end() ?
				inStatic : inFrame).push_back(&s);
		Require(inStatic.size()==layout.staticUVSets.size(),"a static UV set named by the layout is missing or repeated");
		std::uint8_t topology[DigestBytes], reference[DigestBytes];
		Unhex(prepared ? prepared->topologyDigest :
			MeshTopologyDigest(static_cast<std::uint32_t>(frame.positions.size()/3),frame.indices),topology);
		Bytes fixed;
		std::string fixedDigest;
		if (prepared) {
			Unhex(prepared->digest,reference);
		} else if (layout.topology) {
			Sink sizing{nullptr};
			WriteStatic(sizing,frame,inStatic);
			remaining.Take(sizing.count);
			fixed.reserve(sizing.count);
			Sink dst{&fixed};
			WriteStatic(dst,frame,inStatic);
			fixedDigest=RISE::RISECBOR64::SHA256Hex(fixed);
			Unhex(fixedDigest,reference);
		}
		Sink sizing{nullptr};
		WriteV2(sizing,frame,topology,layout.topology?reference:nullptr,inFrame);
		remaining.Take(sizing.count);
		Bytes candidate;
		candidate.reserve(sizing.count);
		Sink dst{&candidate};
		WriteV2(dst,frame,topology,layout.topology?reference:nullptr,inFrame);
		out.swap(candidate);
		staticPayload.swap(fixed);
		staticDigest.swap(fixedDigest);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}
} // namespace

bool RISE::Simulation::DecodeMesh(const Bytes& bytes,std::size_t budget,MeshFrame& out,std::string& error)
{
	return Decode(bytes,budget,nullptr,out,error);
}

bool RISE::Simulation::DecodeMesh(const Bytes& bytes,std::size_t budget,const StaticPayloadResolver& resolve,
	MeshFrame& out,std::string& error)
{
	return Decode(bytes,budget,&resolve,out,error);
}

// Header-only topology and static-reference inspection without decoding vertex arrays.
bool RISE::Simulation::PeekMeshTopology(const Bytes& bytes,MeshTopology& out,std::string& error)
{
	try {
		Budget scratch{(std::numeric_limits<std::size_t>::max)()};
		Source src{bytes};
		const auto h=ReadHeader(src,scratch,nullptr);
		MeshTopology candidate;
		Require(h.vertices<=(std::numeric_limits<std::uint32_t>::max)() &&
			h.triangles<=(std::numeric_limits<std::uint32_t>::max)(),"too many mesh elements");
		candidate.vertices=static_cast<std::uint32_t>(h.vertices);
		candidate.triangles=static_cast<std::uint32_t>(h.triangles);
		candidate.digest=Hex(bytes.data()+h.digestAt);
		if (h.flags&FlagStaticTopology) candidate.reference=Hex(bytes.data()+h.referenceAt);
		candidate.version=h.version;
		out=std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}

// Extracts static payload digests referenced by a "mesh.v2" frame for cache dependency checks.
bool RISE::Simulation::MeshPayloadReferences(const Bytes& bytes,std::vector<std::string>& digests,std::string& error)
{
	MeshTopology t;
	if (!PeekMeshTopology(bytes,t,error)) return false;
	digests.clear();
	if (!t.reference.empty()) digests.push_back(t.reference);
	return true;
}
