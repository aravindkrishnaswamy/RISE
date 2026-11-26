#include "../tools/SimulationVDBImport.h"
#include <cstdio>

#if defined(RISE_ENABLE_OPENVDB)
#include <openvdb/io/File.h>
#include <chrono>

namespace {
using namespace RISE::Simulation;
namespace fs=std::filesystem;
constexpr std::size_t Budget=64u<<20;

void WriteFixture(const fs::path& path,float hot,unsigned compression=openvdb::io::COMPRESS_BLOSC|openvdb::io::COMPRESS_ACTIVE_MASK)
{
	auto scalar=openvdb::FloatGrid::create(0.0f);
	scalar->setName("density");
	auto matrix=openvdb::math::Mat4d::identity();
	matrix(0,1)=0.5;
	matrix.setTranslation(openvdb::Vec3d(10,-4,2));
	scalar->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
	scalar->tree().setValueOn(openvdb::Coord(-2,3,0),0.0f);
	scalar->tree().setValueOff(openvdb::Coord(-1,3,0),hot);
	scalar->tree().addTile(1,openvdb::Coord(8,0,0),0.125f,false);
	auto velocity=openvdb::Vec3fGrid::create(openvdb::Vec3f(0,0,0));
	velocity->setName("velocity");
	velocity->tree().setValueOn(openvdb::Coord(3,-1,2),openvdb::Vec3f(-2,1,4));
	openvdb::io::File file(path.string());
	file.setCompression(compression);
	file.write(openvdb::GridPtrVec{velocity,scalar});
	file.close();
}

void Require(bool condition,const char* invariant)
{
	if (!condition) throw std::runtime_error(invariant);
}

int Import(const fs::path& dir,const fs::path& a,const fs::path& b,const std::string& unit)
{
	std::vector<std::string> args{"rise-cache-import","--vdb",dir.string(),"0.125",
		"velocity=m/s","density="+unit,"--",a.string(),b.string()};
	std::vector<char*> ptrs;
	for (auto& arg:args) ptrs.push_back(arg.data());
	return ImportForeignVDB(static_cast<int>(ptrs.size()),ptrs.data());
}

// Independent file-index fixture with two root tiles and no leaf buffers.
// Invalid variants are presented only to the non-allocating archive scanner.
void CheckArchiveBounds()
{
	std::string bytes;
	auto word=[&](std::uint64_t value,unsigned width) {
		for (unsigned i=0;i<width;++i) bytes.push_back(static_cast<char>(value>>(8*i)));
	};
	auto name=[&](const std::string& v) { word(v.size(),4); bytes+=v; };
	word(0x56444220,8); word(225,4); word(14,4); word(0,4); word(1,1);
	bytes+=std::string(36,'0'); word(0,4); word(1,4);
	name("test"); name("Tree_float_5_4_3"); name("");
	const auto index=bytes.size(); bytes.resize(index+24);
	word(0,4); word(0,4); name("UniformScaleMap"); bytes+=std::string(120,0);
	word(1,4); word(0,4);
	const auto count=bytes.size();
	word(2,4); word(0,4);
	const auto first=bytes.size();
	word(0xfffff000u,4); word(0,4); word(0,4); word(0,4); word(0,1);
	word(0,4); word(0,4); word(0,4); word(0,4); word(1,1);
	auto patch=[](std::string& s,std::size_t offset,std::uint64_t v,unsigned width) {
		for (unsigned i=0;i<width;++i) s[offset+i]=static_cast<char>(v>>(8*i));
	};
	patch(bytes,index,index+24,8); patch(bytes,index+8,bytes.size(),8); patch(bytes,index+16,bytes.size(),8);
	VDBArchiveBounds(bytes).Check();
	auto reject=[&](const std::string& sample,std::uint64_t budget=4ull<<30) {
		try { VDBArchiveBounds(sample,budget).Check(); }
		catch (const std::runtime_error&) { return; }
		throw std::runtime_error("invalid or over-budget VDB passed archive preflight");
	};
	reject(bytes,32);
	for (const auto& field:std::vector<std::pair<std::size_t,std::uint32_t>>{
			{count,0xffffffffu},{first,0},{first,0xfffff001u},{first,4096},
			{index+8,0},{57,0xffffffffu},{61,0xffffffffu}}) {
		auto bad=bytes;
		patch(bad,field.first,field.second,4);
		reject(bad);
	}
	reject(bytes.substr(0,bytes.size()-1));
}

void Run(const fs::path& root)
{
	openvdb::initialize();
	CheckArchiveBounds();
	for (const unsigned flags:{0u,1u,2u,3u,4u,6u}) {
		const auto path=root/("compression-"+std::to_string(flags)+".vdb");
		WriteFixture(path,2,flags);
		auto bytes=ForeignVDB::ReadSource(path);
		VDBArchiveBounds(bytes).Check();
		Require(DecodeVDBPreimage(bytes)->size()==2,"supported compressor round trip");
	}
	const auto a=root/"a.vdb", b=root/"b.vdb", dir=root/"cache";
	WriteFixture(a,2); WriteFixture(b,4);
	Require(Import(dir,a,b,"kg/m3")==0,"whole foreign sequence imports");
	CacheSnapshot cache;
	std::string error;
	Require(CacheSnapshot::Open(dir,Budget,cache,error),"published cache opens");
	const auto& d=cache.Description();
	Require(cache.Complete() && d.fidelity=="preview" && d.evidence.empty() && d.count==2 &&
		d.interval==0.125 && d.producer=="external.vdb","foreign lineage and declared times");
	Require(cache.Entries().size()==2 && cache.Entries()[0].time==0 && cache.Entries()[1].time==0.125 &&
		cache.Entries()[0].digest!=cache.Entries()[1].digest,"inactive nonbackground changes second payload");
	for (std::int64_t i=0;i<2;++i) {
		std::shared_ptr<const Bytes> payload;
		GridFrame frame;
		Require(cache.Read(i,"fields",Budget,payload,error)==FrameStatus::Ready &&
			DecodeGrid(*payload,Budget,frame,error) && frame.channels.size()==2,"verify foreign payload");
		const auto& c=frame.channels[0];
		Require(c.name=="density" && c.units=="kg/m3" && c.origin==std::array<std::int32_t,3>{{-2,0,0}} &&
			c.dimensions==std::array<std::uint32_t,3>{{18,8,8}},"signed domain includes inactive tile");
		Require(c.indexToWorld[4]==0.5 && c.indexToWorld[3]==10 && c.indexToWorld[7]==-4 &&
			c.indexToWorld[11]==2,"transpose affine VDB mapping into column-vector convention");
		Require(c.blocks.size()==2 && c.blocks[0].active==Bytes{1} && c.blocks[0].values.size()==2 &&
			c.blocks[0].values[0]==0 && c.blocks[0].values[1]==(i==0?0x40000000u:0x40800000u),
			"active background and inactive nonbackground remain distinct");
		Require(c.blocks[1].constant && c.blocks[1].active==Bytes{0} &&
			c.blocks[1].values==std::vector<std::uint32_t>{0x3e000000},"inactive uniform tile stays compact");
		const auto& v=frame.channels[1];
		Require(v.name=="velocity" && v.components==3 && v.blocks.size()==1 &&
			v.blocks[0].values==std::vector<std::uint32_t>{0xc0000000,0x3f800000,0x40800000},
			"vec3f channel order and bits");
	}
	Require(Import(dir,a,b,"kg/m3")==0,"identical offline import revalidates sealed output");
	Require(Import(dir,b,a,"kg/m3")!=0,"input order participates in cache key");
	Require(Import(dir,a,b,"g/m3")!=0,"declared units participate in cache key");
	CacheSnapshot after;
	Require(CacheSnapshot::Open(dir,Budget,after,error) && after.Digest()==cache.Digest(),
		"mismatched import cannot replace a sealed cache");
}
}
#endif

int main()
{
#if defined(RISE_ENABLE_OPENVDB)
	const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
	auto dir=fs::temp_directory_path()/("rise-foreign-vdb-"+std::to_string(stamp));
	if (!fs::create_directory(dir)) return 1;
	int result=0;
	try { Run(dir); }
	catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); result=1; }
	std::error_code ec;
	fs::remove_all(dir,ec);
	if (!result) std::puts("SimulationVDBImportTest passed");
	return result;
#else
	std::puts("SimulationVDBImportTest skipped: worker built without OpenVDB");
	return 0;
#endif
}
