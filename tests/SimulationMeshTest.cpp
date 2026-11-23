// mesh.v1 codec: golden bytes, round trip, canonicality, malformed-input
// refusal, budgets, topology digest, and cache acceptance of the type.
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "TestTempDir.h"

#include <cstdio>
#include <cstring>
#include <limits>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
int failures=0;
constexpr std::size_t Budget=16*1024*1024;

void Check(bool condition,const char* message,const std::string& detail={})
{
	if (!condition) { ++failures; std::fprintf(stderr,"FAIL: %s %s\n",message,detail.c_str()); }
}

std::uint32_t Bits(float f) { std::uint32_t b; std::memcpy(&b,&f,4); return b; }

Bytes Hex(const char* hex)
{
	auto digit=[](char c){ return c>='a' ? c-'a'+10 : c-'0'; };
	Bytes result;
	for (const char* p=hex; *p; p+=2) result.push_back(static_cast<std::uint8_t>(16*digit(p[0])+digit(p[1])));
	return result;
}

// Independent fixture assembled from the wire table with Python struct and
// hashlib: one triangle, velocities, no normals.
Bytes Golden()
{
	return Hex(
		"524953454d455348010000000100000001006d03006d2f739c6eb9ab5cc157f4"
		"fa4f21539d5fe086be26ef1824c92bc40d303e5c90b502540300000001000000"
		"0000000001000000020000000000000000000000000000000000803f00000000"
		"000000000000000000000040000000bf00000000000000000000803f00000000"
		"000000000000803f0000000000000000000080bf");
}

MeshFrame Fixture()
{
	MeshFrame m;
	m.positionUnits="m";
	m.velocityUnits="m/s";
	m.positions={Bits(0),Bits(0),Bits(0), Bits(1),Bits(0),Bits(0), Bits(0),Bits(2),Bits(-.5f)};
	m.velocities={Bits(0),Bits(0),Bits(1), Bits(0),Bits(0),Bits(1), Bits(0),Bits(0),Bits(-1)};
	m.indices={0,1,2};
	return m;
}

bool Same(const MeshFrame& a,const MeshFrame& b)
{
	return a.positionUnits==b.positionUnits && a.velocityUnits==b.velocityUnits && a.positions==b.positions &&
		a.velocities==b.velocities && a.normals==b.normals && a.indices==b.indices;
}

MeshFrame Sheet(std::uint32_t n,float lift)
{
	MeshFrame m;
	for (std::uint32_t j=0;j<n;++j) for (std::uint32_t i=0;i<n;++i) {
		m.positions.insert(m.positions.end(),{Bits(float(i)),Bits(float(j)),Bits(lift*float(i*j))});
		m.normals.insert(m.normals.end(),{Bits(0),Bits(0),Bits(1)});
	}
	for (std::uint32_t j=0;j+1<n;++j) for (std::uint32_t i=0;i+1<n;++i) {
		const std::uint32_t a=i+n*j;
		m.indices.insert(m.indices.end(),{a,a+1,a+n+1, a,a+n+1,a+n});
	}
	return m;
}

void GoldenAndRoundTrip()
{
	std::string error;
	Bytes encoded;
	Check(EncodeMesh(Fixture(),Budget,encoded,error) && encoded==Golden(),"encoder matches independent golden bytes",error);
	MeshFrame decoded;
	Check(DecodeMesh(Golden(),Budget,decoded,error) && Same(decoded,Fixture()),"golden decodes to fixture",error);
	MeshTopology topology;
	Check(PeekMeshTopology(Golden(),topology,error) && topology.vertices==3 && topology.triangles==1 &&
		topology.digest=="9c6eb9ab5cc157f4fa4f21539d5fe086be26ef1824c92bc40d303e5c90b50254" &&
		topology.digest==MeshTopologyDigest(3,{0,1,2}),"topology header and digest",error);

	// Negative zero and other bit patterns survive; normals present.
	auto sheet=Sheet(5,.25f);
	sheet.positions[2]=0x80000000u;
	Bytes a,b;
	MeshFrame back;
	Check(EncodeMesh(sheet,Budget,a,error) && DecodeMesh(a,Budget,back,error) && Same(back,sheet) &&
		EncodeMesh(back,Budget,b,error) && a==b,"round trip is bit exact and canonical",error);

	// Fixed topology: positions change, digest does not; connectivity change alters it.
	MeshTopology t1,t2,t3;
	Bytes moved,rewired;
	auto other=Sheet(5,.75f);
	auto flipped=sheet;
	std::swap(flipped.indices[1],flipped.indices[2]);
	Check(EncodeMesh(other,Budget,moved,error) && EncodeMesh(flipped,Budget,rewired,error) &&
		PeekMeshTopology(a,t1,error) && PeekMeshTopology(moved,t2,error) && PeekMeshTopology(rewired,t3,error) &&
		t1.digest==t2.digest && t1.digest!=t3.digest && a!=moved,"topology digest detects fixed connectivity");

	MeshFrame empty;
	Bytes none;
	Check(EncodeMesh(empty,Budget,none,error) && DecodeMesh(none,Budget,back,error) && back.indices.empty(),
		"empty mesh is a valid frame",error);
}

void Refusals()
{
	std::string error;
	Bytes out{1,2,3};
	const Bytes keep=out;
	auto refuse=[&](MeshFrame m,const char* what) {
		Check(!EncodeMesh(m,Budget,out,error) && out==keep,what);
	};
	auto m=Fixture(); m.indices={0,1,3}; refuse(m,"index out of range refused");
	m=Fixture(); m.indices={0,1,1}; refuse(m,"degenerate triangle refused");
	m=Fixture(); m.positions[4]=0x7fc00000u; refuse(m,"NaN position refused");
	m=Fixture(); m.velocities[0]=0x7f800000u; refuse(m,"infinite velocity refused");
	m=Fixture(); m.velocityUnits.clear(); refuse(m,"velocities without units refused");
	m=Fixture(); m.velocities.clear(); refuse(m,"velocity units without velocities refused");
	m=Fixture(); m.normals={0}; refuse(m,"normal count mismatch refused");
	m=Fixture(); m.positions.pop_back(); refuse(m,"partial vertex refused");
	m=Fixture(); m.positionUnits=std::string("m\0",2); refuse(m,"NUL in units refused");
	m=Fixture(); m.positionUnits="A\xcc\x8a"; refuse(m,"non-NFC units refused");
	Check(!EncodeMesh(Fixture(),64,out,error) && out==keep,"encode budget refusal leaves output untouched");

	MeshFrame previous=Fixture(), decoded=previous;
	auto refuseDecode=[&](const Bytes& bytes,const char* what) {
		Check(!DecodeMesh(bytes,Budget,decoded,error) && Same(decoded,previous),what,error);
	};
	const auto golden=Golden();
	for (std::size_t n=0;n<golden.size();++n) {
		Bytes cut(golden.begin(),golden.begin()+static_cast<std::ptrdiff_t>(n));
		if (DecodeMesh(cut,Budget,decoded,error)) { Check(false,"truncated payload refused"); break; }
	}
	Check(Same(decoded,previous),"truncation leaves output untouched");
	auto g=golden; g.push_back(0); refuseDecode(g,"trailing byte refused");
	g=golden; g[8]=2; refuseDecode(g,"unknown version refused");
	g=golden; g[12]=5; refuseDecode(g,"unknown flag refused");
	g=golden; g[0]='X'; refuseDecode(g,"bad magic refused");
	g=golden; g[30]^=1; refuseDecode(g,"forged topology digest refused");
	// Layout: digest at 24..55, vertex count 56, triangle count 60, indices 64..75.
	auto redigest=[](Bytes& b) {
		const auto hex=RISE::RISECBOR64::SHA256Hex(b.data()+56,20);
		for (std::size_t i=0;i<32;++i) b[24+i]=static_cast<std::uint8_t>(std::stoi(hex.substr(2*i,2),nullptr,16));
	};
	g=golden; redigest(g); Check(g==golden,"test re-digest reproduces the golden digest");
	g=golden; g[72]=3; redigest(g); refuseDecode(g,"index out of range refused with a consistent digest");
	g=golden; g[72]=1; redigest(g); refuseDecode(g,"degenerate triangle refused with a consistent digest");
	// Hostile counts must fail before allocating.
	g=golden; g[56]=0xff; g[57]=0xff; g[58]=0xff; g[59]=0x7f; refuseDecode(g,"hostile vertex count refused");
	g=golden; g[60]=0xff; g[61]=0xff; g[62]=0xff; g[63]=0xff; refuseDecode(g,"hostile triangle count refused");
	Check(!DecodeMesh(golden,golden.size(),decoded,error) && Same(decoded,previous),"decode budget charges input and output");
	MeshTopology topology;
	Check(!PeekMeshTopology(Bytes(golden.begin(),golden.begin()+40),topology,error),"peek refuses a truncated header");
}

void CacheAcceptsMesh()
{
	namespace fs=std::filesystem;
	const fs::path root=RISE::TestSupport::ScratchRunDir("rise_sim_mesh_","suite");
	std::string error;
	CacheDescription d;
	d.key=std::string(64,'3'); d.epoch=std::string(64,'4');
	d.producer="mesh-fixture"; d.producerVersion="1"; d.count=1;
	d.outputs={{"surface","mesh.v1",{{"position","m","f32x3"},{"velocity","m/s","f32x3"}}}};
	Bytes payload;
	Check(IsNativePayloadType("mesh.v1") && IsNativePayloadType("grid.v1") && !IsNativePayloadType("mesh.v2"),
		"native payload registry");
	Check(EncodeMesh(Fixture(),Budget,payload,error),"encode cached mesh",error);
	{
		CacheWriter writer;
		Check(writer.Begin(root/"cache",d,"",false,Budget,error) && writer.Append(0,"surface",0,payload,error) &&
			writer.Seal(error),"write mesh cache",error);
	}
	CacheSnapshot snapshot;
	std::shared_ptr<const Bytes> bytes;
	MeshFrame decoded;
	Check(CacheSnapshot::Open(root/"cache",Budget,snapshot,error) &&
		snapshot.Read(0,"surface",Budget,bytes,error)==FrameStatus::Ready && DecodeMesh(*bytes,Budget,decoded,error) &&
		Same(decoded,Fixture()),"cache serves mesh.v1 payloads",error);
	std::error_code ignored;
	fs::remove_all(root,ignored);
}
}

int main()
{
	GoldenAndRoundTrip();
	Refusals();
	CacheAcceptsMesh();
	if (failures) { std::fprintf(stderr,"%d mesh codec check(s) failed\n",failures); return 1; }
	std::printf("Simulation mesh codec checks passed\n");
	return 0;
}
