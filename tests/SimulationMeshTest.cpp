// mesh.v1 and mesh.v2 codecs: golden bytes (independent fixtures), round
// trip, canonicality, malformed-input refusal, budgets, topology digest, the
// static topology record and its resolution, and the cache's static payloads
// (round trip, verification failure, budget, undeclared references, a v1
// manifest kept byte-compatible without them).
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "TestTempDir.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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
	g=golden; g[8]=3; refuseDecode(g,"unknown version refused");
	g=golden; g[8]=2; refuseDecode(g,"a mesh.v1 body read as mesh.v2 refused");
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
	Check(IsNativePayloadType("mesh.v1") && IsNativePayloadType("grid.v1") && IsNativePayloadType("mesh.v2") &&
		IsNativePayloadType("field.v1") && !IsNativePayloadType("mesh.v3") && IsNativeStaticType(MeshTopologyStaticType),
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
	// A cache without static payloads or evidence scope keeps the v1 manifest format.
	{
		std::ifstream f(root/"cache"/"manifest.cbor",std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
		Check(text.find("rise.simulation.cache.v1")!=std::string::npos && text.find("statics")==std::string::npos,
			"a cache without statics keeps the v1 manifest");
	}
	std::error_code ignored;
	fs::remove_all(root,ignored);
}

// ---------------------------------------------------------------- mesh.v2

// Independent fixtures (tools: Python struct and hashlib from the wire table
// in docs/SIMULATION_MESH_FORMAT.md).
// (a) self-contained: the v1 fixture's triangle with velocities, inline
//     indices, a frame UV set "st" [1] and an attribute "thickness" [m].
Bytes GoldenV2Inline()
{
	return Hex(
		"524953454d455348020000000100000001006d03006d2f739c6eb9ab5cc157f4"
		"fa4f21539d5fe086be26ef1824c92bc40d303e5c90b502540300000001000000"
		"0000000001000000020000000000000000000000000000000000803f00000000"
		"000000000000000000000040000000bf00000000000000000000803f00000000"
		"000000000000803f0000000000000000000080bf010002007374010031000000"
		"00000000000000803f00000000000000000000803f01000900746869636b6e65"
		"737301006d016f12833a6f12033ba69b443b");
}
// (b) positions only, the connectivity and a static UV set "material" [m]
//     in a mesh.topology.v1 record referenced by its SHA-256.
Bytes GoldenV2Static()
{
	return Hex(
		"524953454d455348020000000400000001006d9c6eb9ab5cc157f4fa4f21539d"
		"5fe086be26ef1824c92bc40d303e5c90b502540300000001000000c64f0c9db3"
		"510d00e32bf4b45eea96371f630a79cea2766942e474c22f8ba9310000000000"
		"000000000000000000803f00000000000000000000000000000040000000bf00"
		"000000");
}
Bytes GoldenTopology()
{
	return Hex(
		"524953454d544f50010000000300000001000000000000000100000002000000"
		"010008006d6174657269616c01006d00000000000000000000803f0000000000"
		"00000000000040");
}
const char* GoldenTopologyDigest = "c64f0c9db3510d00e32bf4b45eea96371f630a79cea2766942e474c22f8ba931";

MeshFrame FixtureV2Inline()
{
	MeshFrame m=Fixture();
	m.uvSets.push_back({"st","1",{Bits(0),Bits(0),Bits(1),Bits(0),Bits(0),Bits(1)}});
	m.attributes.push_back({"thickness","m",1,{Bits(.001f),Bits(.002f),Bits(.003f)}});
	return m;
}
MeshFrame FixtureV2Static()
{
	MeshFrame m=Fixture();
	m.velocities.clear(); m.velocityUnits.clear();
	m.uvSets.push_back({"material","m",{Bits(0),Bits(0),Bits(1),Bits(0),Bits(0),Bits(2)}});
	return m;
}

bool SameV2(const MeshFrame& a,const MeshFrame& b)
{
	if (!Same(a,b) || a.uvSets.size()!=b.uvSets.size() || a.attributes.size()!=b.attributes.size()) return false;
	for (std::size_t i=0;i<a.uvSets.size();++i)
		if (a.uvSets[i].name!=b.uvSets[i].name || a.uvSets[i].units!=b.uvSets[i].units || a.uvSets[i].values!=b.uvSets[i].values)
			return false;
	for (std::size_t i=0;i<a.attributes.size();++i)
		if (a.attributes[i].name!=b.attributes[i].name || a.attributes[i].units!=b.attributes[i].units ||
			a.attributes[i].components!=b.attributes[i].components || a.attributes[i].values!=b.attributes[i].values)
			return false;
	return true;
}

// A resolver over a fixed set of static payloads (what a cache reader does).
StaticPayloadResolver Statics(std::vector<std::pair<std::string,Bytes>> statics,int* calls=nullptr)
{
	return [statics,calls](const std::string& digest,std::size_t budget,std::shared_ptr<const Bytes>& out,std::string& error) {
		if (calls) ++*calls;
		for (const auto& s : statics) if (s.first==digest) {
			if (s.second.size()>budget) { error="over budget"; return false; }
			out=std::make_shared<const Bytes>(s.second);
			return true;
		}
		error="undeclared";
		return false;
	};
}

void MeshV2()
{
	std::string error;
	Bytes frame,fixed;
	std::string digest;
	MeshStaticLayout inline_;
	Check(EncodeMeshV2(FixtureV2Inline(),inline_,Budget,frame,fixed,digest,error) && frame==GoldenV2Inline() &&
		fixed.empty() && digest.empty(),"self-contained mesh.v2 encoder matches the independent golden bytes",error);
	MeshFrame decoded;
	Check(DecodeMesh(GoldenV2Inline(),Budget,decoded,error) && SameV2(decoded,FixtureV2Inline()),
		"self-contained mesh.v2 golden decodes without a resolver",error);
	MeshTopology t;
	Check(PeekMeshTopology(GoldenV2Inline(),t,error) && t.version==2 && t.reference.empty() && t.vertices==3 &&
		t.triangles==1 && t.digest=="9c6eb9ab5cc157f4fa4f21539d5fe086be26ef1824c92bc40d303e5c90b50254",
		"mesh.v2 topology digest is the mesh.v1 digest of the same connectivity",error);

	MeshStaticLayout layout;
	layout.topology=true;
	layout.staticUVSets={"material"};
	Check(EncodeMeshV2(FixtureV2Static(),layout,Budget,frame,fixed,digest,error) && frame==GoldenV2Static() &&
		fixed==GoldenTopology() && digest==GoldenTopologyDigest &&
		digest==RISE::RISECBOR64::SHA256Hex(fixed),"static-topology encoder matches the independent golden bytes",error);
	Check(!DecodeMesh(GoldenV2Static(),Budget,decoded,error) && SameV2(decoded,FixtureV2Inline()),
		"a referencing frame is refused without a resolver, output untouched");
	int calls=0;
	const auto resolve=Statics({{GoldenTopologyDigest,GoldenTopology()}},&calls);
	Check(DecodeMesh(GoldenV2Static(),Budget,resolve,decoded,error) && SameV2(decoded,FixtureV2Static()) && calls==1,
		"static topology resolves through the resolver",error);
	std::vector<std::string> refs;
	Check(PeekMeshTopology(GoldenV2Static(),t,error) && t.reference==GoldenTopologyDigest &&
		MeshPayloadReferences(GoldenV2Static(),refs,error) && refs==std::vector<std::string>{GoldenTopologyDigest} &&
		MeshPayloadReferences(Golden(),refs,error) && refs.empty(),"references are readable from the header",error);
	// Canonical: re-encoding the decoded frame with the same layout reproduces both payloads.
	Bytes again,againFixed;
	Check(EncodeMeshV2(decoded,layout,Budget,again,againFixed,digest,error) && again==GoldenV2Static() &&
		againFixed==GoldenTopology(),"mesh.v2 round trip is canonical",error);

	// A prepared static record writes the golden frame without re-encoding or hashing the static payload.
	MeshStaticRecord record;
	Bytes prepared;
	Check(PrepareMeshStatic(FixtureV2Static(),layout,Budget,record,error) && record.payload==GoldenTopology() &&
		record.digest==GoldenTopologyDigest && record.topologyDigest==t.digest &&
		EncodeMeshV2(FixtureV2Static(),layout,Budget,record,prepared,error) && prepared==GoldenV2Static(),
		"a prepared static record encodes the identical golden frame",error);
	MeshStaticLayout inlineLayout;
	Check(!PrepareMeshStatic(FixtureV2Static(),inlineLayout,Budget,record,error) &&
		!EncodeMeshV2(FixtureV2Static(),inlineLayout,Budget,record,prepared,error),
		"a prepared static record needs a static-topology layout");

	// Verification failures.
	auto bad=GoldenTopology(); bad[16]=2; // T = 2 in the static record
	Check(!DecodeMesh(GoldenV2Static(),Budget,Statics({{GoldenTopologyDigest,bad}}),decoded,error),
		"a static record whose counts differ from the frame is refused",error);
	bad=GoldenTopology(); bad[28]=1; // indices {0,1,1}... index 2 -> 1
	Check(!DecodeMesh(GoldenV2Static(),Budget,Statics({{GoldenTopologyDigest,bad}}),decoded,error),
		"a static record whose connectivity differs from the frame's topology digest is refused",error);
	Check(!DecodeMesh(GoldenV2Static(),Budget,Statics({}),decoded,error),"an unresolvable reference is refused");
	Check(!DecodeMesh(GoldenV2Static(),GoldenV2Static().size()+GoldenTopology().size()+64,resolve,decoded,error),
		"the static payload is charged to the decode budget");
	// Name collision between a static and a frame UV set.
	MeshFrame clash=FixtureV2Static();
	clash.uvSets.push_back({"material","1",clash.uvSets[0].values});
	Check(!EncodeMeshV2(clash,layout,Budget,frame,fixed,digest,error),"duplicate UV-set names refused");
	MeshFrame unsorted=FixtureV2Inline();
	unsorted.attributes.push_back({"a","1",1,{Bits(1),Bits(2),Bits(3)}});
	Check(!EncodeMeshV2(unsorted,inline_,Budget,frame,fixed,digest,error),"unsorted attributes refused");
	MeshFrame wide=FixtureV2Inline();
	wide.attributes[0].components=17;
	wide.attributes[0].values.resize(51);
	Check(!EncodeMeshV2(wide,inline_,Budget,frame,fixed,digest,error),"attribute with more than 16 components refused");
	MeshStaticLayout loose;
	loose.staticUVSets={"st"};
	Check(!EncodeMeshV2(FixtureV2Inline(),loose,Budget,frame,fixed,digest,error),"static UV sets need a static topology");
	Check(!EncodeMesh(FixtureV2Inline(),Budget,frame,error),"mesh.v1 refuses UV sets and attributes");
	const auto golden=GoldenV2Inline();
	for (std::size_t n=0;n<golden.size();++n) {
		Bytes cut(golden.begin(),golden.begin()+static_cast<std::ptrdiff_t>(n));
		if (DecodeMesh(cut,Budget,decoded,error)) { Check(false,"truncated mesh.v2 refused"); break; }
	}
	auto g=golden; g.push_back(0);
	Check(!DecodeMesh(g,Budget,decoded,error),"trailing mesh.v2 byte refused");
	g=golden; g[12]=8;
	Check(!DecodeMesh(g,Budget,decoded,error),"unknown mesh.v2 flag refused");
}

// Static payloads through the disk cache: declared once, written once,
// verified like frames, budgeted, and required before a referencing frame.
void CacheStatics()
{
	namespace fs=std::filesystem;
	const fs::path root=RISE::TestSupport::ScratchRunDir("rise_sim_mesh_statics_","suite");
	std::string error;
	CacheDescription d;
	d.key=std::string(64,'5'); d.epoch=std::string(64,'6');
	d.producer="mesh-fixture"; d.producerVersion="2"; d.count=2;
	d.outputs={{"surface","mesh.v2",{{"position","m","f32x3"}}}};
	const fs::path dir=root/"cache";
	{
		CacheWriter writer;
		std::string digest;
		Check(writer.Begin(dir,d,"",false,Budget,error),"begin static cache",error);
		Check(!writer.Append(0,"surface",0,GoldenV2Static(),error) && error.find("undeclared static")!=std::string::npos,
			"a frame referencing an undeclared static payload is refused",error);
		Check(writer.AppendStatic(MeshTopologyStaticType,GoldenTopology(),digest,error) && digest==GoldenTopologyDigest,
			"static payload declared",error);
		Check(writer.AppendStatic(MeshTopologyStaticType,GoldenTopology(),digest,error) && writer.Snapshot().Statics().size()==1,
			"publishing a static payload again is a no-op",error);
		Check(!writer.AppendStatic("other.type",GoldenTopology(),digest,error),"a conflicting static declaration is refused");
		Check(writer.Append(0,"surface",0,GoldenV2Static(),error) && writer.Append(1,"surface",0.5,GoldenV2Static(),error) &&
			writer.Seal(error),"frames referencing the static payload append and seal",error);
		Check(writer.AppendStatic(MeshTopologyStaticType,GoldenTopology(),digest,error),
			"re-declaring an existing static payload on a sealed cache is idempotent",error);
	}
	int files=0;
	for (const auto& e : fs::directory_iterator(dir))
		if (e.path().extension()==".static") ++files;
	Check(files==1,"the static payload is stored once for both frames");
	CacheSnapshot snapshot;
	Check(CacheSnapshot::Open(dir,Budget,snapshot,error) && snapshot.Statics().size()==1 &&
		snapshot.Statics()[0].type==MeshTopologyStaticType && snapshot.Statics()[0].size==GoldenTopology().size(),
		"the manifest declares the static payload",error);
	std::shared_ptr<const Bytes> bytes,fixed;
	MeshFrame decoded;
	Check(snapshot.Read(1,"surface",Budget,bytes,error)==FrameStatus::Ready &&
		DecodeMesh(*bytes,Budget,snapshot.Resolver(),decoded,error) && SameV2(decoded,FixtureV2Static()),
		"a cached frame decodes through the snapshot's resolver",error);
	Check(snapshot.ReadStatic(GoldenTopologyDigest,GoldenTopology().size()-1,fixed,error)==FrameStatus::BudgetExceeded,
		"static reads honour the budget");
	Check(snapshot.ReadStatic(std::string(64,'a'),Budget,fixed,error)==FrameStatus::Unavailable,"an undeclared static is unavailable");
	// Tamper with the stored static payload: verified like a frame.
	{
		const auto path=dir/(std::string(GoldenTopologyDigest)+".static");
		fs::permissions(path,fs::perms::owner_write,fs::perm_options::add);
		Bytes forged=GoldenTopology(); forged.back()^=1;
		std::ofstream f(path,std::ios::binary|std::ios::trunc);
		f.write(reinterpret_cast<const char*>(forged.data()),static_cast<std::streamsize>(forged.size()));
	}
	Check(snapshot.ReadStatic(GoldenTopologyDigest,Budget,fixed,error)==FrameStatus::Corrupt,"a tampered static payload is corrupt");
	Check(!DecodeMesh(*bytes,Budget,snapshot.Resolver(),decoded,error),"a frame over a tampered static payload is refused");
	std::error_code ignored;
	fs::remove_all(root,ignored);
}
}

int main()
{
	GoldenAndRoundTrip();
	Refusals();
	CacheAcceptsMesh();
	MeshV2();
	CacheStatics();
	if (failures) { std::fprintf(stderr,"%d mesh codec check(s) failed\n",failures); return 1; }
	std::printf("Simulation mesh codec checks passed\n");
	return 0;
}
