#include "../src/Library/Simulation/SimulationCache.h"
#include "TestTempDir.h"
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <cstdlib>

namespace {
using namespace RISE::Simulation;
namespace fs = std::filesystem;
using V = RISE::RISECBOR64::Value;
constexpr std::size_t budget=4u<<20;

void Check(bool condition,const char* contract,const std::string& detail={})
{
	if (!condition) {
		std::cerr<<"FAIL: "<<contract<<" "<<detail<<"\n";
		throw std::runtime_error(contract);
	}
}

void Write(const fs::path& path,const Bytes& bytes)
{
	std::ofstream stream(path,std::ios::binary|std::ios::trunc);
	stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
	Check(stream.good(),"fixture writes successfully");
}

IReadOnlyCacheSnapshot::Qualification QualificationFor(const std::string& trustedDigest)
{
	return {
		[trustedDigest](const IReadOnlyCacheSnapshot& snapshot,std::string&){
			return snapshot.Digest()==trustedDigest && snapshot.Description().producer=="qualified-fixture" &&
				snapshot.Description().evidence==std::vector<std::string>{std::string(64,'e')};
		},
		[](const CacheEntry& entry,const Bytes& bytes,std::string&){
			// These are the fixture producer's three exact reference outputs.
			if (entry.index==-2) return bytes==Bytes({1,2,3});
			if (entry.index==-1) return bytes==Bytes({4,5});
			return entry.index==0 && bytes==Bytes({6});
		}
	};
}

CacheDescription Description()
{
	CacheDescription d;
	d.key=std::string(64,'1'); d.epoch=std::string(64,'2');
	d.producer="qualified-fixture"; d.producerVersion="v1";
	d.firstIndex=-2; d.count=3; d.startTime=0; d.interval=.25; d.sceneOrigin=0; d.timeScale=2;
	d.outputs={{"fields","grid.v1",{{"density","kg/m3","f32"},{"velocity","m/s","f32x3"}}}};
	d.fidelity="predictive"; d.evidence={std::string(64,'e')};
	return d;
}

class LiveMesh final : public IReadOnlyCacheSnapshot {
	CacheDescription d_;
	std::vector<CacheEntry> entries_;
	std::shared_ptr<const Bytes> bytes_;
	std::string digest_;
public:
	explicit LiveMesh(const Bytes& source) : bytes_(std::make_shared<const Bytes>(source)) {
		d_=DescriptionForMesh();
		digest_=RISE::RISECBOR64::SHA256Hex(*bytes_);
		entries_.push_back({0,"surface",0,bytes_->size(),digest_,{}});
	}
	static CacheDescription DescriptionForMesh() {
		auto d=::Description();
		d.count=1; d.firstIndex=0; d.fidelity="preview"; d.evidence.clear();
		d.outputs={{"surface","mesh.fixed.v1",{{"position","m","f32x3"},{"velocity","m/s","f32x3"}}}};
		return d;
	}
	const CacheDescription& Description() const override {return d_;}
	const std::vector<CacheEntry>& Entries() const override {return entries_;}
	const std::string& Digest() const override {return digest_;}
	bool Complete() const override {return true;}
	FrameStatus Read(std::int64_t index,const std::string& output,std::size_t limit,
		std::shared_ptr<const Bytes>& bytes,std::string&) const override {
		if (output!="surface") return FrameStatus::Unsupported;
		if (index!=0) return FrameStatus::Unavailable;
		if (bytes_->size()>limit) return FrameStatus::BudgetExceeded;
		bytes=bytes_; return FrameStatus::Ready;
	}
};

// Independent schema fixture: a portable player can read a cache even on a
// volume where it cannot promise durable publication. No writer code is used.
void PortablePlayback(const fs::path& dir)
{
	fs::create_directories(dir);
	const Bytes data{5,7};
	const auto hash=RISE::RISECBOR64::SHA256Hex(data);
	const V channel=V::ArrayValue({V::String("height"),V::String("m"),V::String("f32")});
	const V out=V::ArrayValue({V::String("surface"),V::String("grid.v1"),V::ArrayValue({channel})});
	const V record=V::MapValue({
		{"producer",V::String("fixture")},{"version",V::String("1")},{"key",V::String(std::string(64,'a'))},
		{"epoch",V::String(std::string(64,'f'))},{"count",V::Unsigned(1)},{"first",V::Signed(-1)},
		{"start",V::Float(0)},{"origin",V::Float(0)},{"scale",V::Float(1)},{"interval",V::Float(.5)},
		{"hold",V::Bool(true)},{"fidelity",V::String("preview")},{"outputs",V::ArrayValue({out})},
		{"evidence",V::ArrayValue({})},{"records",V::BytesValue({})}
	});
	const V entry=V::ArrayValue({V::Signed(-1),V::String("surface"),V::Float(0),V::Unsigned(data.size()),
		V::String(hash),V::BytesValue({4})});
	const V manifest=V::MapValue({{"format",V::String("rise.simulation.cache.v1")},{"description",record},
		{"frames",V::ArrayValue({entry})},{"complete",V::Bool(true)}});
	Bytes encoded; std::string error;
	Check(RISE::RISECBOR64::Encode(manifest,encoded,&error),"encode portable schema fixture",error);
	Write(dir/"manifest.cbor",encoded); Write(dir/(hash+".frame"),data);
	CacheSnapshot disk;
	std::shared_ptr<const Bytes> result;
	Check(CacheSnapshot::Open(dir,budget,disk,error) &&
		disk.Read(-1,"surface",budget,result,error)==FrameStatus::Ready && *result==data &&
		disk.Entries().front().receipt==Bytes({4}),"portable read-only cache fixture",error);
}

void KeyAndLive()
{
	std::string key1,key2,error;
	const std::string a(64,'a'), b(64,'b');
	const V params=V::MapValue({{"speed",V::Float(1)},{"gain",V::Float(2)}});
	Check(MakeCacheKey("test","v1",params,{{"left",a},{"right",b}},key1,error),"derive input key",error);
	Check(MakeCacheKey("test","v1",V::MapValue({{"gain",V::Float(2)},{"speed",V::Float(1)}}),
		{{"left",a},{"right",b}},key2,error)&&key1==key2,"canonical parameter key");
	Check(MakeCacheKey("test","v1",params,{{"right",b},{"left",a}},key2,error)&&key1!=key2,
		"input order contributes to staleness key");
	Check(!MakeCacheKey("test","v1",params,{{"left",a},{"left",b}},key2,error),"reject ambiguous input name");
	Bytes producer{1,2,3};
	LiveMesh live(producer);
	const IReadOnlyCacheSnapshot& reader=live;
	producer.assign(3,9);
	std::shared_ptr<const Bytes> frame;
	Check(reader.Description().outputs[0].channels.size()==2 &&
		reader.Read(0,"surface",budget,frame,error)==FrameStatus::Ready && *frame==Bytes({1,2,3}),
		"live mesh shares reader contract and owns published geometry");
}

#ifndef _WIN32
// A process exit releases the real OS lock without executing the writer
// destructor. Construct each on-disk cut of the publication protocol: before
// commit only the old manifest is visible, afterwards the new one is complete.
void PublicationCuts(const fs::path& root)
{
	const Bytes first{1},second{2};
	for (unsigned cut=0;cut<6;++cut) {
		auto d=Description();
		d.count=2; d.firstIndex=0;
		const auto dir=root/("cut"+std::to_string(cut));
		const pid_t child=::fork();
		Check(child>=0,"fork publication interruption fixture");
		if (child==0) {
			std::string why;
			CacheWriter writer;
			if (!writer.Begin(dir,d,"",false,budget,why) ||
				!writer.Append(0,"fields",0,first,why)) _exit(2);
			const auto payload=dir/(RISE::RISECBOR64::SHA256Hex(second)+".frame");
			const auto temp=dir/".cache.pending";
			if (cut==0) Write(temp,{});
			if (cut==1) Write(temp,second);
			if (cut==2 || cut==3) Write(payload,second);
			if (cut==3) Write(temp,{0xa4}); // torn manifest staging file
			if (cut>=4 && !writer.Append(1,"fields",.25,second,why)) _exit(3);
			if (cut==5 && !writer.Seal(why)) _exit(4);
			_exit(0);
		}
		int status=0;
		Check(::waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
			"publication process stopped at specified boundary");
		std::string error;
		CacheSnapshot disk;
		Check(CacheSnapshot::Open(dir,budget,disk,error) && disk.Complete()==(cut==5) &&
			disk.Entries().size()==(cut<4?1u:2u),"only committed generation visible after interruption",error);
		std::shared_ptr<const Bytes> bytes;
		Check(disk.Read(0,"fields",budget,bytes,error)==FrameStatus::Ready && *bytes==first,
			"old frame readable after process loss");
		if (cut==5) continue;
		Check(InspectCache(dir,budget,error)==CacheState::Partial,"dead writer releases actual OS lock");
		CacheWriter recovery;
		Check(recovery.Begin(dir,d,disk.Digest(),true,budget,error) &&
			recovery.Append(1,"fields",.25,second,error) && recovery.Seal(error),
			"resume repairs interrupted publish including orphaned data and torn temp",error);
	}
}
#endif

void LimitsAndAdmission(const fs::path& root)
{
	std::string error;
	auto d=Description();
	d.firstIndex=0; d.count=3; d.startTime=1; d.sceneOrigin=1; d.interval=.1; d.timeScale=1;
	CacheDescription tooLarge=d;
	tooLarge.physicalRecords.resize(200000);
	CacheWriter writer;
	Check(!writer.Begin(root/"oversize",tooLarge,"",false,150000,error) &&
		!fs::exists(root/"oversize"),"oversized metadata refused before creating cache");
	Check(writer.Begin(root/"bounded",d,"",false,budget,error),"begin bounded cache");
	std::int64_t index=0;
	Check(writer.Snapshot().MapTime(1.2,index,error) && index==2 &&
		writer.Snapshot().MapTime(std::nextafter(1.2,1.),index,error) && index==1,
		"nonzero origin decimal cadence boundary and preceding representable time");
	const Bytes receipt(120000,1);
	Check(writer.Append(0,"fields",1,{1},error,receipt) &&
		writer.Append(1,"fields",1.1,{1},error,receipt),"first two receipts fit");
	const auto before=writer.Snapshot().Digest();
	Check(!writer.Append(2,"fields",1.2,{3},error,receipt) &&
		before==writer.Snapshot().Digest(),"cumulative receipt allocations bounded transactionally");
	const Bytes largeFrame(1u<<20,42);
	Check(writer.Append(2,"fields",1.2,largeFrame,error),"writer publishes payload inside single-copy budget");
	const auto replayBefore=writer.Snapshot().Digest();
	Check(!writer.Append(2,"fields",1.2,largeFrame,error) &&
		writer.Snapshot().Digest()==replayBefore,"budget accounts for verified duplicate read");
	Check(writer.Seal(error),"seal full bounded cache");
	auto forged=writer.Snapshot();
	Check(!forged.PreflightPredictive(d.key,QualificationFor(std::string(64,'0')),budget,error),
		"self-consistent data and copied producer metadata cannot forge trusted snapshot");
	Check(!forged.PreflightPredictive(d.key,QualificationFor(forged.Digest()),budget,error),
		"producer metadata and an approved snapshot cannot bypass payload admission");
	auto future=d;
	future.outputs.front().type="future.v9";
	CacheWriter futureWriter;
	Check(futureWriter.Begin(root/"future",future,"",false,budget,error),"unsupported type can be inspected");
	std::shared_ptr<const Bytes> bytes;
	Check(futureWriter.Snapshot().Read(0,"fields",budget,bytes,error)==FrameStatus::Unsupported,
		"unknown codec returns explicit unsupported status");
}

void ConcurrentDemand(const fs::path& root)
{
	const auto path=root/"demand";
	auto d=Description();
	d.firstIndex=0; d.fidelity="preview"; d.evidence.clear();
	std::string error;
	std::future<bool> second;
	{
		CacheWriter first;
		Check(first.PrepareExtend(path,d,budget,error),"first demand opens cache",error);
		std::promise<void> started;
		auto ready=started.get_future();
		second=std::async(std::launch::async,[&,signal=std::move(started)]() mutable {
			std::string why;
			CacheWriter next;
			signal.set_value();
			return next.PrepareExtend(path,d,budget,why) &&
				next.Append(1,"fields",.25,{2},why);
		});
		ready.wait();
		Check(first.Append(0,"fields",0,{1},error),"first demand publishes while another waits",error);
	}
	Check(second.get(),"concurrent demand waits and extends actual current generation");
	CacheSnapshot previous;
	Check(CacheSnapshot::Open(path,budget,previous,error) && previous.Entries().size()==2,
		"concurrent distinct demand imports both survive",error);
	Write(path/(RISE::RISECBOR64::SHA256Hex(Bytes{1})+".frame"),{8});
	{
		CacheWriter resume;
		Check(!resume.Begin(path,d,previous.Digest(),true,budget,error),
			"worker resume still verifies historical payloads");
	}
	{
		CacheWriter last;
		Check(last.PrepareExtend(path,d,budget,error) && last.Append(2,"fields",.5,{3},error) && last.Seal(error),
			"preview extension reads only demanded data despite old corruption",error);
		std::shared_ptr<const Bytes> bytes;
		Check(last.Snapshot().Read(2,"fields",budget,bytes,error)==FrameStatus::Ready &&
			last.Snapshot().Read(0,"fields",budget,bytes,error)==FrameStatus::Corrupt,
			"each selected preview frame verifies at point of use");
	}
	CacheWriter hit;
	Check(hit.PrepareExtend(path,d,budget,error) && hit.Snapshot().Complete() &&
		hit.Append(2,"fields",.5,{3},error),"concurrent import can join a matching sealed generation");
}

void RoundTrip(const fs::path& root)
{
	const auto path=root/"cache";
	auto d=Description();
	std::string error;
	std::shared_ptr<const Bytes> bytes;
	const Bytes a{1,2,3}, b{4,5}, c{6}, receipt{9};
	CacheSnapshot pinned,complete;
	Check(InspectCache(path,budget,error)==CacheState::Missing,"missing cache");
	{
		CacheWriter writer,other;
		Check(writer.Begin(path,d,"",false,budget,error),"start cache writer",error);
		Check(!other.Begin(path,d,writer.Snapshot().Digest(),true,budget,error),"second writer refused");
		Check(InspectCache(path,budget,error)==CacheState::Baking,"distinguish active empty generation");
		Check(writer.Append(0,"fields",.5,c,error),"out of order append",error);
		Check(writer.Append(-2,"fields",0,a,error,receipt),"publish first frame");
		const auto digest=writer.Snapshot().Digest();
		Check(writer.Append(-2,"fields",0,a,error,receipt) && digest==writer.Snapshot().Digest(),
			"idempotent replay preserves generation");
		Check(!writer.Append(-2,"fields",0,c,error,receipt) && !writer.Append(-2,"fields",0,a,error,{}),
			"conflicting data or receipt refused");
		Check(!writer.Seal(error),"seal rejects interior hole");
		Check(CacheSnapshot::Open(path,budget,pinned,error),"acquire partial snapshot",error);
		std::int64_t index=0;
		Check(pinned.MapTime(.125,index,error) && index==-1 &&
			pinned.Read(index,"fields",budget,bytes,error)==FrameStatus::Unavailable,"hole stays unavailable");
		Check(pinned.MapTime(std::nextafter(.125,0.),index,error)&&index==-2,"select previous frame below boundary");
		Check(pinned.MapTime(.375,index,error)&&index==0,"hold exact endpoint");
		Check(pinned.MapTime(-20,index,error)&&index==-2,"hold before support");
		Check(!pinned.PreflightPredictive(d.key,QualificationFor(pinned.Digest()),budget,error),"partial predictive refused");
	}
	Check(InspectCache(path,budget,error)==CacheState::Partial,"interrupted cache retains readable range");
	Write(path/".cache.pending",{42}); // crash while staging new data or the next manifest
	Write(path/(RISE::RISECBOR64::SHA256Hex(Bytes{77})+".frame"),{77}); // published data before pointer replacement
	{
		CacheWriter writer;
		Check(!writer.Begin(path,d,std::string(64,'0'),true,budget,error),"stale expected generation refused");
		Check(writer.Begin(path,d,pinned.Digest(),true,budget,error),"resume verified prefix",error);
		Check(writer.Append(-1,"fields",.25,b,error) && writer.Seal(error),"fill hole and seal",error);
		Check(CacheSnapshot::Open(path,budget,complete,error),"read sealed snapshot",error);
		Check(complete.PreflightPredictive(d.key,QualificationFor(complete.Digest()),budget,error),"verified evidence admits complete cache");
		Check(!complete.PreflightPredictive(std::string(64,'a'),QualificationFor(complete.Digest()),budget,error) &&
			!complete.PreflightPredictive(d.key,{},budget,error),"stale or unevidenced predictive refused");
		Check(pinned.Read(-1,"fields",budget,bytes,error)==FrameStatus::Unavailable,
			"pinned partial snapshot keeps original coverage");
	}
	Check(InspectCache(path,budget,error)==CacheState::Complete,"durable complete state");
	const auto oldFile=path/(RISE::RISECBOR64::SHA256Hex(a)+".frame");
	const auto otherFile=path/(RISE::RISECBOR64::SHA256Hex(b)+".frame");
	{
		CacheWriter writer;
		d.epoch=std::string(64,'8'); d.key=std::string(64,'7'); d.hold=false;
		Check(writer.Begin(path,d,complete.Digest(),false,budget,error),"new epoch rebakes",error);
		Check(complete.Stale(d.key) && complete.Read(-2,"fields",budget,bytes,error)==FrameStatus::Ready &&
			*bytes==a && complete.Entries().front().receipt==receipt,"old snapshot still reads original bytes across rebake");
		std::int64_t index=0;
		Check(!writer.Snapshot().MapTime(.375,index,error) &&
			writer.Snapshot().MapTime(std::nextafter(.375,0.),index,error) && index==0,"half-open error support");
	}
	Check(complete.Read(-2,"fields",a.size()-1,bytes,error)==FrameStatus::BudgetExceeded,"bounded payload read");
	fs::remove(otherFile);
	Check(complete.Read(-2,"fields",budget,bytes,error)==FrameStatus::Ready &&
		complete.Read(-1,"fields",budget,bytes,error)==FrameStatus::Corrupt,"defer unselected missing payload and refuse on use");
	Check(!complete.PreflightPredictive(complete.Description().key,QualificationFor(complete.Digest()),budget,error),"preflight verifies every payload");
	Write(oldFile,{1,2,9});
	Check(complete.Read(-2,"fields",budget,bytes,error)==FrameStatus::Corrupt,"same-length corruption refused");
#ifndef _WIN32
	const auto outside=root/"outside";
	Write(outside,a); fs::remove(oldFile); fs::create_symlink(outside,oldFile);
	Check(complete.Read(-2,"fields",budget,bytes,error)==FrameStatus::Corrupt,"refuse symlink to matching payload");
#endif
	const auto before=pinned.Digest();
	Write(path/"manifest.cbor",{0x9a,0xff,0xff,0xff,0xff});
	Check(!CacheSnapshot::Open(path,budget,pinned,error) && pinned.Digest()==before,"hostile count rejected without replacing snapshot");
	auto empty=Description(); empty.count=0;
	CacheWriter writer;
	Check(writer.Begin(root/"empty",empty,"",false,budget,error)&&writer.Seal(error),"empty authored sequence",error);
}


// Evidence conditions and scope round-trip through a format v2 manifest.
void EvidenceManifest(const fs::path& root)
{
	std::string error;
	auto d=Description();
	d.evidenceConditions={{"contact",{"barrier","none"}},{"mode",{"bake"}}};
	d.evidenceScope="what the label covers";
	{
		CacheWriter writer;
		Check(writer.Begin(root/"scoped",d,"",false,budget,error) && writer.Append(-2,"fields",0,{1,2,3},error),
			"write a scoped description",error);
	}
	CacheSnapshot s;
	Check(CacheSnapshot::Open(root/"scoped",budget,s,error) && s.Description().evidenceConditions==d.evidenceConditions &&
		s.Description().evidenceScope==d.evidenceScope && s.Statics().empty(),"conditions and scope round-trip",error);
	{
		CacheWriter resume;
		Check(resume.Begin(root/"scoped",d,s.Digest(),true,budget,error),"resume compares the scoped description",error);
	}
	auto changed=d;
	changed.evidenceScope="another scope";
	CacheWriter refused;
	Check(!refused.Begin(root/"scoped",changed,s.Digest(),true,budget,error),"a different scope is a different description");
	auto bad=d;
	bad.evidenceConditions={{"mode",{"bake"}},{"contact",{"none"}}};
	Check(!ValidateCacheDescription(bad,error),"unsorted conditions refused");
	bad.evidenceConditions={{"mode",{}}};
	Check(!ValidateCacheDescription(bad,error),"a condition without values refused");
}

#ifndef _WIN32
fs::perms Mode(const fs::path& p) { return fs::symlink_status(p).permissions(); }
bool OwnerOnly(const fs::path& p)
{
	return (Mode(p)&(fs::perms::group_all|fs::perms::others_all))==fs::perms::none;
}

// Location policy: group/world-writable ancestors refused unless below a
// trusted root; created directories owner-only; symlinks cannot redirect.
void LocationPolicy(const fs::path& root)
{
	std::string error;
	const auto base=root/"location";
	fs::create_directories(base);
	fs::permissions(base,fs::perms::owner_all,fs::perm_options::replace);
	auto d=Description(); d.fidelity="preview"; d.evidence.clear();
	const auto shared=base/"shared";
	fs::create_directory(shared);
	fs::permissions(shared,fs::perms::owner_all|fs::perms::group_all|fs::perms::others_read|fs::perms::others_exec,
		fs::perm_options::replace);
	::unsetenv(TrustedCacheRootsEnvironment);
	ClearTrustedCacheRoots();
	{
		CacheWriter w;
		Check(w.Begin(base/"a"/"b"/"cache",d,"",false,budget,error),"a private chain is accepted",error);
		Check(OwnerOnly(base/"a") && OwnerOnly(base/"a"/"b") && OwnerOnly(base/"a"/"b"/"cache"),
			"every directory the writer creates is owner-only");
	}
	{
		CacheWriter w;
		Check(!w.Begin(shared/"c",d,"",false,budget,error) && error.find("ancestor")!=std::string::npos &&
			!fs::exists(shared/"c"),"a group-writable ancestor is refused before anything is created",error);
		Check(!CheckCacheLocation(shared/"c",false,error),"CheckCacheLocation reports the refusal");
	}
	fs::create_directory(base/"loose");
	fs::permissions(base/"loose",fs::perms::owner_all|fs::perms::group_all,fs::perm_options::replace);
	{
		CacheWriter w;
		Check(!w.Begin(base/"loose",d,"",false,budget,error),"a group-writable cache directory is refused");
	}
	// Trusted root from the environment: the check stops at the root.
	::setenv(TrustedCacheRootsEnvironment,("relative/ignored:"+shared.string()+":/no/such/dir").c_str(),1);
	Check(TrustedCacheRoots().size()==1,"only existing absolute entries are trusted");
	{
		CacheWriter w;
		Check(w.Begin(shared/"c",d,"",false,budget,error) && OwnerOnly(shared/"c"),
			"below a trusted root the cache is accepted and created owner-only",error);
	}
	fs::create_directory(shared/"inner");
	fs::permissions(shared/"inner",fs::perms::owner_all|fs::perms::group_all,fs::perm_options::replace);
	{
		CacheWriter w;
		Check(!w.Begin(shared/"inner"/"c",d,"",false,budget,error),"a group-writable directory below the trusted root is still refused");
		CacheWriter self;
		Check(!self.Begin(shared,d,"",false,budget,error),"the trusted root itself is not an owner-only cache directory");
	}
	// Symlinks cannot redirect a trusted location.
	fs::create_directory(base/"private");
	fs::permissions(base/"private",fs::perms::owner_all,fs::perm_options::replace);
	fs::create_directory_symlink(base/"private",shared/"link");
	{
		CacheWriter w;
		Check(!w.Begin(shared/"link",d,"",false,budget,error),"a cache directory that is a symlink is refused");
	}
	const auto outside=base/"outside";
	fs::create_directory(outside);
	fs::permissions(outside,fs::perms::owner_all|fs::perms::group_all,fs::perm_options::replace);
	fs::create_directory_symlink(outside,shared/"out");
	{
		CacheWriter w;
		Check(!w.Begin(shared/"out"/"c",d,"",false,budget,error) && !fs::exists(outside/"c"),
			"a symlink inside the trusted root to an untrusted directory is checked at its target and refused",error);
	}
	fs::create_directory_symlink(shared,base/"via");
	::setenv(TrustedCacheRootsEnvironment,base.string().c_str(),1);
	{
		CacheWriter w;
		Check(!w.Begin(base/"via"/"d",d,"",false,budget,error),
			"trusting a parent does not trust a group-writable directory below it (reached through a symlink)",error);
	}
	// The API, the same rules.
	::unsetenv(TrustedCacheRootsEnvironment);
	Check(!AddTrustedCacheRoot("relative/root",error),"a relative trusted root is refused");
	Check(AddTrustedCacheRoot(shared,error) && TrustedCacheRoots().size()==1,"trust a root through the API",error);
	{
		CacheWriter w;
		Check(w.Begin(shared/"api",d,"",false,budget,error) && OwnerOnly(shared/"api"),"the API root is honoured",error);
	}
	ClearTrustedCacheRoots();
	{
		CacheWriter w;
		Check(!w.Begin(shared/"api2",d,"",false,budget,error),"clearing the API roots restores the refusal");
	}
}
#endif
} // namespace

int main()
{
	const fs::path root=RISE::TestSupport::ScratchRunDir("rise_sim_cache_","suite");
	try {
		KeyAndLive();
		PortablePlayback(root/"read-only");
#ifdef _WIN32
		{
			CacheWriter probe; std::string error;
			if (!probe.Begin(root/"durability-probe",Description(),"",false,budget,error)) {
				Check(error=="durable cache publication unavailable: directory flush",
					"unexpected writer probe failure",error);
				std::cout<<"Portable cache playback passed; disk publication unavailable on this volume\n";
				fs::remove_all(root);
				return 0;
			}
		}
#endif
		RoundTrip(root);
		EvidenceManifest(root);
#ifndef _WIN32
		LocationPolicy(root);
#endif
		LimitsAndAdmission(root);
		ConcurrentDemand(root);
#ifndef _WIN32
		PublicationCuts(root);
#endif
		fs::remove_all(root);
		std::cout<<"Simulation cache publication and playback checks passed\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr<<e.what()<<"\n";
		std::error_code ignored; fs::remove_all(root,ignored);
		return 1;
	}
}
