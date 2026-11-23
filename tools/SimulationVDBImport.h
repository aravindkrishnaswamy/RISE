// Foreign VDB import command. The calling executable sets worker resource limits.
#ifndef RISE_TOOLS_SIMULATION_VDB_IMPORT_H
#define RISE_TOOLS_SIMULATION_VDB_IMPORT_H

#include <cstdio>
#ifdef RISE_ENABLE_OPENVDB
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationVDBGrid.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>

namespace RISE { namespace Simulation { namespace ForeignVDB {
namespace fs = std::filesystem;
using Units = std::map<std::string,std::string>;
using Value = RISECBOR64::Value;
constexpr std::size_t SourceLimit=std::size_t(1)<<30;
constexpr std::size_t FrameBudget=std::size_t(2)<<30;
constexpr std::size_t ManifestBudget=std::size_t(128)<<20;

inline void Need(bool ok,const char* message)
{
	if (!ok) throw std::runtime_error(message);
}

inline std::string ReadSource(const fs::path& path)
{
	Need(fs::is_regular_file(path),"VDB input must be a regular file");
	std::ifstream file(path,std::ios::binary|std::ios::ate);
	const auto size=file.tellg();
	Need(size>0 && size<=static_cast<std::streamoff>(SourceLimit),"VDB source missing or exceeds input byte limit");
	std::string bytes(static_cast<std::size_t>(size),'\0');
	file.seekg(0);
	Need(file.read(&bytes[0],size).good() && file.peek()==std::ifstream::traits_type::eof(),"VDB source changed during read");
	return bytes;
}

inline std::string Digest(const std::string& bytes)
{
	return RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());
}

inline GridFrame Project(std::string& bytes,const Units& units)
{
	auto grids=DecodeVDBPreimage(bytes);
	Need(grids && grids->size()==units.size(),"VDB channel set differs from name=unit arguments");
	GridFrame frame;
	std::size_t remaining=FrameBudget;
	std::string error;
	for (const auto& grid:*grids) {
		Need(bool(grid),"VDB contains an empty grid");
		const auto u=units.find(grid->getName());
		Need(u!=units.end(),"VDB channel missing a declared unit");
		auto& channel=frame.channels.emplace_back();
		bool ok=false;
		if (auto f=openvdb::gridPtrCast<openvdb::FloatGrid>(grid))
			ok=ExtractVDBGrid(*f,u->second,nullptr,remaining,channel,error);
		else if (auto v=openvdb::gridPtrCast<openvdb::Vec3fGrid>(grid))
			ok=ExtractVDBGrid(*v,u->second,nullptr,remaining,channel,error);
		else throw std::runtime_error("foreign VDB channel must be float32 or vec3_float32");
		if (!ok) throw std::runtime_error(error);
	}
	std::sort(frame.channels.begin(),frame.channels.end(),[](const auto& a,const auto& b) {
		return a.name<b.name;
	});
	return frame;
}

inline int Run(int argc,char** argv)
{
	if (argc<7) throw std::runtime_error("usage: rise-cache-import --vdb CACHE INTERVAL name=units... -- FILE.vdb...");
	const fs::path root=argv[2];
	char* end=nullptr;
	errno=0;
	const double dt=std::strtod(argv[3],&end);
	Need(!errno && end!=argv[3] && *end=='\0' && std::isfinite(dt) && dt>0,"invalid VDB frame interval");
	Units units;
	int cursor=4;
	for (;cursor<argc && std::string(argv[cursor])!="--";++cursor) {
		std::string text=argv[cursor];
		const auto sep=text.find('=');
		Need(sep!=std::string::npos && sep>0 && sep+1<text.size(),"expected a nonempty channel name=units");
		auto name=text.substr(0,sep),unit=text.substr(sep+1);
		Need(RISECBOR64::IsUTF8NFC(name) && RISECBOR64::IsUTF8NFC(unit) &&
			name.size()<=65535 && unit.size()<=65535,"invalid channel name or units");
		Need(units.emplace(name,unit).second,"duplicate VDB channel name");
	}
	Need(!units.empty() && cursor<argc-1,"expected units and -- followed by VDB files");
	const int first=cursor+1;
	Need(std::isfinite(dt*(argc-first-1)),"VDB sequence time range exceeds binary64");
	CacheDescription desc;
	desc.producer="external.vdb";
	desc.producerVersion=std::string("1.openvdb.")+openvdb::getLibraryVersionString();
	desc.interval=dt; desc.count=static_cast<std::uint64_t>(argc-first);
	desc.fidelity="preview";
	std::vector<std::pair<std::string,std::string>> inputs;
	for (int i=first;i<argc;++i)
		inputs.emplace_back(std::to_string(i-first),Digest(ReadSource(argv[i])));
	Value::Members parameters;
	for (const auto& item:units) parameters.emplace_back(item.first,Value::String(item.second));
	std::string error;
	if (!MakeCacheKey(desc.producer,desc.producerVersion,Value::ArrayValue({Value::Float(dt),
		Value::MapValue(parameters)}),inputs,desc.key,error)) throw std::runtime_error(error);
	desc.epoch=desc.key;
	const auto state=InspectCache(root,ManifestBudget,error);
	Need(state!=CacheState::Baking && state!=CacheState::Corrupt,"destination is busy or corrupt");
	CacheSnapshot previous;
	if (state!=CacheState::Missing) {
		if (!CacheSnapshot::Open(root,ManifestBudget,previous,error)) throw std::runtime_error(error);
		Need(previous.Description().key==desc.key,"destination contains different inputs; choose a new cache directory");
	}
	CacheWriter writer;
	for (int i=first;i<argc;++i) {
		const auto index=static_cast<std::size_t>(i-first);
		auto source=ReadSource(argv[i]);
		Need(Digest(source)==inputs[index].second,"VDB source changed after computing the cache key");
		auto frame=Project(source,units);
		std::string().swap(source);
		Bytes bytes;
		if (!EncodeGrid(frame,FrameBudget,bytes,error)) throw std::runtime_error(error);
		CacheOutput output{"fields","grid.v1",{}};
		for (const auto& c:frame.channels)
			output.channels.push_back({c.name,c.units,c.components==1?"float32":"vec3_float32"});
		if (i==first) {
			desc.outputs={output};
			if (state!=CacheState::Complete &&
				!writer.Begin(root,desc,previous.Digest(),state==CacheState::Partial,FrameBudget,error))
				throw std::runtime_error(error);
		} else {
			const auto& expected=desc.outputs.front().channels;
			for (std::size_t j=0;j<expected.size();++j)
				Need(output.channels[j].name==expected[j].name &&
					output.channels[j].representation==expected[j].representation,"VDB sequence channel schema changed");
		}
		if (state==CacheState::Complete) {
			const auto& d=previous.Description();
			Need(d.producer==desc.producer && d.producerVersion==desc.producerVersion &&
				d.count==desc.count && d.interval==dt && d.fidelity=="preview" && d.epoch==desc.epoch &&
				d.startTime==0 && d.sceneOrigin==0 && d.timeScale==1 && d.firstIndex==0 && d.hold &&
				d.physicalRecords.empty() && d.evidence.empty() && d.outputs.size()==1 &&
				d.outputs[0].name==output.name && d.outputs[0].type==output.type &&
				d.outputs[0].channels.size()==output.channels.size(),"sealed VDB cache description differs");
			for (std::size_t j=0;j<output.channels.size();++j) {
				const auto& a=d.outputs[0].channels[j]; const auto& b=output.channels[j];
				Need(a.name==b.name && a.units==b.units && a.representation==b.representation,
					"sealed VDB cache channels differ");
			}
			std::shared_ptr<const Bytes> old;
			Need(index<previous.Entries().size() && previous.Entries()[index].time==index*dt &&
				previous.Read(index,output.name,FrameBudget,old,error)==FrameStatus::Ready &&
				*old==bytes,"sealed VDB cache differs from source");
		} else if (!writer.Append(index,output.name,index*dt,bytes,error)) throw std::runtime_error(error);
	}
	if (state!=CacheState::Complete && !writer.Seal(error)) throw std::runtime_error(error);
	std::puts((state==CacheState::Complete ? previous.Digest() : writer.Snapshot().Digest()).c_str());
	return 0;
}
} } }
#endif

inline int ImportForeignVDB(int argc,char** argv)
{
#ifdef RISE_ENABLE_OPENVDB
	try { return RISE::Simulation::ForeignVDB::Run(argc,argv); }
	catch (const std::exception& e) {
		std::fprintf(stderr,"foreign VDB import: %s\n",e.what());
		return 1;
	}
#else
	(void)argc; (void)argv;
	std::fputs("foreign VDB import requires the OpenVDB-capable worker\n",stderr);
	return 1;
#endif
}
#endif
