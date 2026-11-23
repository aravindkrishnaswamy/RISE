#include "pch.h"
#include "SimulationCache.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RISE { namespace Simulation {
namespace {
namespace fs = std::filesystem;
using V = RISECBOR64::Value;
constexpr char manifestName[] = "manifest.cbor";
constexpr std::uint64_t maximumBytes = UINT64_C(32) << 30;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool DigestValid(const std::string& digest)
{
	return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c){
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}

bool TextValid(const std::string& text)
{
	return !text.empty() && text.find('\0') == std::string::npos && RISECBOR64::IsUTF8NFC(text);
}

bool InRange(const CacheDescription& d, std::int64_t index)
{
	return index >= d.firstIndex &&
		static_cast<std::uint64_t>(index) - static_cast<std::uint64_t>(d.firstIndex) < d.count;
}

bool DescriptionValid(const CacheDescription& d, std::string& error)
{
	const auto maxIndex = std::numeric_limits<std::int64_t>::max();
	if (!DigestValid(d.key) || !DigestValid(d.epoch) || !TextValid(d.producer) ||
		!TextValid(d.producerVersion) || d.outputs.empty() ||
		d.count > (UINT64_C(1) << 53) ||
		(d.count && d.firstIndex > maxIndex - static_cast<std::int64_t>(d.count-1)) ||
		!std::isfinite(d.startTime) || !std::isfinite(d.interval) || d.interval <= 0 ||
		!std::isfinite(d.sceneOrigin) || !std::isfinite(d.timeScale) ||
		!std::isfinite(d.startTime + static_cast<double>(d.count)*d.interval))
		return Fail(error,"invalid cache identity, outputs or time support");
	if (d.count && !(d.startTime + static_cast<double>(d.count)*d.interval > d.startTime))
		return Fail(error,"cache time extent is not representable");
	if ((d.fidelity != "preview" && d.fidelity != "predictive") ||
		(d.fidelity == "predictive" && d.evidence.empty()) ||
		!std::all_of(d.evidence.begin(),d.evidence.end(),DigestValid))
		return Fail(error,"invalid fidelity or evidence digest");
	std::set<std::string> names;
	for (const auto& out : d.outputs) {
		if (!TextValid(out.name) || !TextValid(out.type) || !names.insert(out.name).second || out.channels.empty())
			return Fail(error,"invalid or duplicate output");
		std::set<std::string> channels;
		for (const auto& ch : out.channels)
			if (!TextValid(ch.name) || !TextValid(ch.units) || !TextValid(ch.representation) ||
				!channels.insert(ch.name).second) return Fail(error,"invalid or duplicate channel");
	}
	return true;
}

V Describe(const CacheDescription& d)
{
	V::Values outputs, evidence;
	for (const auto& out : d.outputs) {
		V::Values channels;
		for (const auto& ch : out.channels)
			channels.push_back(V::ArrayValue({V::String(ch.name), V::String(ch.units), V::String(ch.representation)}));
		outputs.push_back(V::ArrayValue({V::String(out.name),V::String(out.type),V::ArrayValue(channels)}));
	}
	for (const auto& hash : d.evidence) evidence.push_back(V::String(hash));
	return V::MapValue({{"key",V::String(d.key)},{"epoch",V::String(d.epoch)},
		{"producer",V::String(d.producer)},{"version",V::String(d.producerVersion)},
		{"first",V::Signed(d.firstIndex)},{"count",V::Unsigned(d.count)},{"start",V::Float(d.startTime)},
		{"interval",V::Float(d.interval)},{"origin",V::Float(d.sceneOrigin)},{"scale",V::Float(d.timeScale)},
		{"hold",V::Bool(d.hold)},{"outputs",V::ArrayValue(outputs)},{"fidelity",V::String(d.fidelity)},
		{"evidence",V::ArrayValue(evidence)},{"records",V::BytesValue(d.physicalRecords)}});
}

bool EncodeManifest(const CacheSnapshot& snapshot, Bytes& encoded, std::string& error)
{
	V::Values frames;
	frames.reserve(snapshot.Entries().size());
	for (const auto& f : snapshot.Entries())
		frames.push_back(V::ArrayValue({V::Signed(f.index),V::String(f.output),V::Float(f.time),
			V::Unsigned(f.size),V::String(f.digest),V::BytesValue(f.receipt)}));
	return RISECBOR64::Encode(V::MapValue({{"format",V::String("rise.simulation.cache.v1")},
		{"description",Describe(snapshot.Description())},{"frames",V::ArrayValue(frames)},
		{"complete",V::Bool(snapshot.Complete())}}),encoded,&error);
}

bool Signed(const V& v, std::int64_t& n)
{
	if ((v.GetType()!=V::UnsignedInteger && v.GetType()!=V::NegativeInteger) ||
		v.GetIntegerArgument()>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
	const auto x=static_cast<std::int64_t>(v.GetIntegerArgument());
	n=v.GetType()==V::UnsignedInteger ? x : -1-x;
	return true;
}

bool Member(const V& map, const char* name, V::Type type)
{
	const V* value=map.Find(name);
	return value && value->GetType()==type;
}

bool DecodeDescription(const V& v, CacheDescription& d, std::string& error)
{
	if (v.GetType()!=V::Map || v.GetMap().size()!=15 ||
		!Member(v,"key",V::Text) || !Member(v,"epoch",V::Text) || !Member(v,"producer",V::Text) ||
		!Member(v,"version",V::Text) || !v.Find("first") || !Signed(*v.Find("first"),d.firstIndex) ||
		!Member(v,"count",V::UnsignedInteger) || !Member(v,"start",V::Float64) || !Member(v,"interval",V::Float64) ||
		!Member(v,"origin",V::Float64) || !Member(v,"scale",V::Float64) || !Member(v,"hold",V::Boolean) ||
		!Member(v,"outputs",V::Array) || !Member(v,"evidence",V::Array) || !Member(v,"fidelity",V::Text) ||
		!Member(v,"records",V::ByteString)) return Fail(error,"invalid cache description fields");
	d.key=v.Find("key")->GetText(); d.epoch=v.Find("epoch")->GetText();
	d.producer=v.Find("producer")->GetText(); d.producerVersion=v.Find("version")->GetText();
	d.count=v.Find("count")->GetIntegerArgument(); d.startTime=v.Find("start")->GetFloat();
	d.interval=v.Find("interval")->GetFloat(); d.sceneOrigin=v.Find("origin")->GetFloat();
	d.timeScale=v.Find("scale")->GetFloat(); d.hold=v.Find("hold")->GetBoolean();
	d.fidelity=v.Find("fidelity")->GetText(); d.physicalRecords=v.Find("records")->GetBytes();
	for (const auto& hash : v.Find("evidence")->GetArray()) {
		if (hash.GetType()!=V::Text) return Fail(error,"invalid evidence entry");
		d.evidence.push_back(hash.GetText());
	}
	for (const auto& out : v.Find("outputs")->GetArray()) {
		const auto& a=out.GetArray();
		if (out.GetType()!=V::Array || a.size()!=3 || a[0].GetType()!=V::Text ||
			a[1].GetType()!=V::Text || a[2].GetType()!=V::Array) return Fail(error,"invalid output entry");
		CacheOutput output{a[0].GetText(),a[1].GetText(),{}};
		for (const auto& ch : a[2].GetArray()) {
			const auto& c=ch.GetArray();
			if (ch.GetType()!=V::Array || c.size()!=3 || c[0].GetType()!=V::Text ||
				c[1].GetType()!=V::Text || c[2].GetType()!=V::Text) return Fail(error,"invalid channel entry");
			output.channels.push_back({c[0].GetText(),c[1].GetText(),c[2].GetText()});
		}
		d.outputs.push_back(std::move(output));
	}
	return DescriptionValid(d,error);
}

bool KnownOutput(const CacheDescription& d, const std::string& name)
{
	return std::any_of(d.outputs.begin(),d.outputs.end(),[&](const CacheOutput& output){return output.name==name;});
}

bool Full(const CacheDescription& d, const std::vector<CacheEntry>& entries)
{
	return d.outputs.size() && entries.size()%d.outputs.size()==0 && entries.size()/d.outputs.size()==d.count;
}
bool Earlier(const CacheEntry& a, const CacheEntry& b)
{
	return std::tie(a.index,a.output)<std::tie(b.index,b.output);
}

// The parameter objects are caller-owned and may be much larger than a valid
// wire record. Check a conservative encode/copy peak before allocating a value
// tree (including resume's two descriptions and previous snapshot).
class AllocationBudget {
public:
	explicit AllocationBudget(std::uint64_t bytes) : remaining_(std::min(bytes,maximumBytes)) {}
	bool Charge(std::uint64_t count,std::uint64_t width=1) {
		if (count>remaining_/width) return false;
		remaining_-=count*width;
		return true;
	}
	bool Text(const std::string& value) {return Charge(value.size()+1,32);}
	bool Nodes(std::size_t count) {return Charge(count,8*(sizeof(V)+sizeof(std::string)+32));}
	std::uint64_t Remaining() const {return remaining_;}
private:
	std::uint64_t remaining_;
};

bool DescribeFits(AllocationBudget& b,const CacheDescription& d)
{
	if (!b.Nodes(46) || !b.Text(d.key) || !b.Text(d.epoch) || !b.Text(d.producer) ||
		!b.Text(d.producerVersion) || !b.Text(d.fidelity) || !b.Charge(d.physicalRecords.size(),12)) return false;
	for (const auto& out : d.outputs) {
		if (!b.Nodes(4) || !b.Text(out.name) || !b.Text(out.type)) return false;
		for (const auto& ch : out.channels)
			if (!b.Nodes(4) || !b.Text(ch.name) || !b.Text(ch.representation) || !b.Text(ch.units)) return false;
	}
	for (const auto& hash : d.evidence) if (!b.Nodes(1) || !b.Text(hash)) return false;
	return true;
}

bool EntryFits(AllocationBudget& b,const std::string& output,const Bytes& receipt)
{
	return b.Nodes(7) && b.Text(output) && b.Charge(65,32) && b.Charge(receipt.size(),12);
}

bool SnapshotFits(AllocationBudget& b,const CacheSnapshot& s)
{
	if (!DescribeFits(b,s.Description())) return false;
	for (const auto& f : s.Entries()) if (!EntryFits(b,f.output,f.receipt)) return false;
	return true;
}

// Calculate a conservative peak for the canonical value tree, its copies during
// re-encoding, NFC checks and the output descriptors BEFORE invoking the decoder.
bool ScanToken(const Bytes& bytes,std::size_t& offset,unsigned depth,std::uint64_t& budget)
{
	constexpr std::uint64_t nodeCost=8*(sizeof(V)+sizeof(std::string)+32);
	if (offset>=bytes.size() || depth>12 || budget<nodeCost) return false;
	budget-=nodeCost;
	const unsigned token=bytes[offset++], major=token>>5, ai=token&31;
	if (ai>=28) return false;
	std::uint64_t n=ai;
	if (ai>=24) {
		const unsigned width=1u<<(ai-24);
		if (width>bytes.size()-offset) return false;
		n=0;
		for (unsigned i=0;i<width;++i) n=(n<<8)|bytes[offset++];
	}
	if (major==2 || major==3) {
		const unsigned copies=major==2?8:32;
		if (n>bytes.size()-offset || n>budget/copies) return false;
		budget-=n*copies;
		offset+=static_cast<std::size_t>(n);
	} else if (major==4 || major==5) {
		const unsigned items=major==5?2:1;
		if (n>(bytes.size()-offset)/items || n>budget/nodeCost/items) return false;
		for (std::uint64_t i=0;i<n*items;++i)
			if (!ScanToken(bytes,offset,depth+1,budget)) return false;
	} else if (major!=0 && major!=1 && major!=7) return false;
	return true;
}
bool WireFits(const Bytes& bytes,std::uint64_t budget)
{
	if (bytes.size()>budget/2) return false;
	budget-=2*bytes.size();
	std::size_t pos=0;
	return ScanToken(bytes,pos,0,budget) && pos==bytes.size();
}

#if !defined(_WIN32)
struct Descriptor {
	int fd;
	explicit Descriptor(int value) : fd(value) {}
	~Descriptor(){if(fd>=0) ::close(fd);}
	Descriptor(const Descriptor&)=delete;
	Descriptor& operator=(const Descriptor&)=delete;
};
#endif

bool SyncDirectory(const fs::path& path,std::string& error)
{
#if defined(_WIN32)
	HANDLE directory=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
		nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
	if (directory==INVALID_HANDLE_VALUE) return Fail(error,"durable cache publication unavailable: directory flush");
	const bool synced=FlushFileBuffers(directory)!=0;
	CloseHandle(directory);
#else
	Descriptor directory(::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
	const bool synced=directory.fd>=0 && ::fsync(directory.fd)==0;
#endif
	return synced || Fail(error,"durable cache publication unavailable: directory flush");
}

// All leaf names are fixed literals or lower-case content digests.
FrameStatus ReadFile(const fs::path& root,const std::string& name,std::uint64_t budget,
	const std::uint64_t* expected,Bytes& bytes,std::string& error,bool ensureDurable=false)
{
#if defined(_WIN32)
	const fs::path path=root/name;
	HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|(ensureDurable?GENERIC_WRITE:0),FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
	struct Close { HANDLE h; ~Close(){if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);} } guard{file};
	BY_HANDLE_FILE_INFORMATION info{};
	if (file==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file,&info) ||
		(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) {
		error="unreadable or nonregular cache file"; return FrameStatus::Corrupt;
	}
	const std::uint64_t size=(static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
#else
	Descriptor dir(::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
	Descriptor file(dir.fd<0?-1:openat(dir.fd,name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
	struct stat info{};
	if (file.fd<0 || fstat(file.fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size<0) {
		error="unreadable or nonregular cache file: "+name; return FrameStatus::Corrupt;
	}
	const auto size=static_cast<std::uint64_t>(info.st_size);
#endif
	if (expected && size!=*expected) { error="cache payload length mismatch"; return FrameStatus::Corrupt; }
	if (size>budget || size>maximumBytes || size>bytes.max_size()) {
		error="cache file exceeds read budget"; return FrameStatus::BudgetExceeded;
	}
	bytes.resize(static_cast<std::size_t>(size));
	std::size_t pos=0;
	while (pos<bytes.size()) {
		const auto remaining=std::min<std::size_t>(bytes.size()-pos,1u<<20);
#if defined(_WIN32)
		DWORD got=0;
		if (!::ReadFile(file,bytes.data()+pos,static_cast<DWORD>(remaining),&got,nullptr) || got==0) {
#else
		const auto got=::read(file.fd,bytes.data()+pos,remaining);
		if (got<0 && errno==EINTR) continue;
		if (got<=0) {
#endif
			error="short cache file read"; return FrameStatus::Corrupt;
		}
		pos+=static_cast<std::size_t>(got);
	}
	unsigned char tail=0;
#if defined(_WIN32)
	DWORD extra=0;
	if (!::ReadFile(file,&tail,1,&extra,nullptr) || extra!=0) {
#else
	ssize_t extra;
	do {extra=::read(file.fd,&tail,1);} while(extra<0 && errno==EINTR);
	if (extra!=0) {
#endif
		error="cache file changed length during read"; return FrameStatus::Corrupt;
	}
	// A surviving immutable filename may be an orphan from a crash before its
	// directory barrier. Establish the barrier before referencing it in a new generation.
	if (ensureDurable) {
#if defined(_WIN32)
		if (!FlushFileBuffers(file) || !SyncDirectory(root,error)) {
#else
		if (::fsync(file.fd)!=0 || ::fsync(dir.fd)!=0) {
#endif
			error="cannot sync reused cache payload"; return FrameStatus::Corrupt;
		}
	}

	return FrameStatus::Ready;
}

bool CreateDurableDirectory(const fs::path& path,std::string& error)
{
	std::vector<fs::path> missing;
	auto parent=path;
	std::error_code ec;
	while (!fs::exists(parent,ec)) {
		if (ec || parent.empty() || parent==parent.parent_path()) return Fail(error,"invalid cache ancestor");
		missing.push_back(parent);
		parent=parent.parent_path();
	}
	// A predecessor may have died just after creating the last visible
	// directory, before syncing its parent's entry. Repair that cut too.
	if (parent!=parent.root_path() && !SyncDirectory(parent.parent_path(),error)) return false;
	for (auto it=missing.rbegin(); it!=missing.rend(); ++it) {
		fs::create_directory(*it,ec);
		if (ec || !fs::is_directory(fs::symlink_status(*it,ec)) || ec)
			return Fail(error,"cannot create cache directory");
		if (!SyncDirectory(it->parent_path(),error)) return false;
	}
	return true;
}

#if !defined(_WIN32)
bool SyncWrite(int dir,const std::string& name,const Bytes& bytes,bool replace,std::string& error)
{
	// The writer lock covers the directory. Remove only a prior interrupted temp.
	const char* pending=".cache.pending";
	if (::unlinkat(dir,pending,0)!=0 && errno!=ENOENT) return Fail(error,"cannot clean interrupted cache write");
	Descriptor file(::openat(dir,pending,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
	if (file.fd<0) return Fail(error,"cannot create staged cache file");
	std::size_t pos=0;
	while (pos<bytes.size()) {
		auto n=::write(file.fd,bytes.data()+pos,std::min<std::size_t>(bytes.size()-pos,1u<<20));
		if (n<0 && errno==EINTR) continue;
		if (n<=0) return Fail(error,"cache file write failed");
		pos+=static_cast<std::size_t>(n);
	}
	if (::fsync(file.fd)!=0) return Fail(error,"cannot sync staged cache file");
	if (replace) {
		if (::renameat(dir,pending,dir,name.c_str())!=0) return Fail(error,"cache manifest replace failed");
	} else {
		if (::linkat(dir,pending,dir,name.c_str(),0)!=0) return Fail(error,"immutable cache publish failed");
		if (::unlinkat(dir,pending,0)!=0) return Fail(error,"cannot unlink staged payload");
	}
	return ::fsync(dir)==0 || Fail(error,"cannot sync cache directory");
}
#endif

} // namespace

bool CanDecodeCanonicalRecord(const Bytes& encoded,std::size_t budget)
{
	return WireFits(encoded,std::min<std::uint64_t>(budget,maximumBytes));
}

bool MakeCacheKey(const std::string& solver,const std::string& version,const V& params,
	const std::vector<std::pair<std::string,std::string>>& inputs,std::string& key,std::string& error)
{
	if (!TextValid(solver) || !TextValid(version)) return Fail(error,"empty or invalid solver identity");
	std::set<std::string> names;
	V::Values links;
	for (const auto& input : inputs) {
		if (!TextValid(input.first) || !DigestValid(input.second) || !names.insert(input.first).second)
			return Fail(error,"invalid or duplicate named cache input");
		links.push_back(V::ArrayValue({V::String(input.first),V::String(input.second)}));
	}
	Bytes encoded;
	if (!RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.key.v1"),V::String(solver),
		V::String(version),params,V::ArrayValue(links)}),encoded,&error)) return false;
	key=RISECBOR64::SHA256Hex(encoded);
	return true;
}

bool CacheSnapshot::Open(const fs::path& root,std::size_t budget,CacheSnapshot& output,std::string& error)
{
	CacheSnapshot candidate;
	std::error_code ec;
	candidate.root_=fs::absolute(root,ec);
	if (ec) return Fail(error,"cannot resolve cache root");
	if (!fs::is_directory(fs::symlink_status(candidate.root_,ec)) || ec)
		return Fail(error,"cache root is unavailable or not a directory");
	Bytes bytes;
	if (ReadFile(candidate.root_,manifestName,budget,nullptr,bytes,error)!=FrameStatus::Ready) return false;
	if (!WireFits(bytes,std::min<std::uint64_t>(budget,maximumBytes))) return Fail(error,"manifest exceeds decode budget");
	V wire;
	if (!RISECBOR64::DecodeCanonical(bytes,wire,&error)) return false;
	if (wire.GetType()!=V::Map || wire.GetMap().size()!=4 || !Member(wire,"format",V::Text) ||
		wire.Find("format")->GetText()!="rise.simulation.cache.v1" || !Member(wire,"description",V::Map) ||
		!Member(wire,"frames",V::Array) || !Member(wire,"complete",V::Boolean))
		return Fail(error,"unrecognized cache manifest");
	if (!DecodeDescription(*wire.Find("description"),candidate.description_,error)) return false;
	candidate.complete_=wire.Find("complete")->GetBoolean();
	for (const auto& frame : wire.Find("frames")->GetArray()) {
		const auto& row=frame.GetArray();
		CacheEntry entry;
		if (frame.GetType()!=V::Array || row.size()!=6 || !Signed(row[0],entry.index) ||
			row[1].GetType()!=V::Text || row[2].GetType()!=V::Float64 || row[3].GetType()!=V::UnsignedInteger ||
			row[4].GetType()!=V::Text || row[5].GetType()!=V::ByteString) return Fail(error,"invalid frame entry");
		entry.output=row[1].GetText(); entry.time=row[2].GetFloat(); entry.size=row[3].GetIntegerArgument();
		entry.digest=row[4].GetText(); entry.receipt=row[5].GetBytes();
		if (!InRange(candidate.description_,entry.index) || !KnownOutput(candidate.description_,entry.output) ||
			!DigestValid(entry.digest) || !std::isfinite(entry.time) || entry.size>maximumBytes ||
			(!candidate.entries_.empty() && !Earlier(candidate.entries_.back(),entry)))
			return Fail(error,"invalid, duplicate or unsorted frame entry");
		candidate.entries_.push_back(std::move(entry));
	}
	if (candidate.complete_ && !Full(candidate.description_,candidate.entries_))
		return Fail(error,"sealed cache has incomplete coverage");
	candidate.digest_=RISECBOR64::SHA256Hex(bytes);
	output=std::move(candidate);
	return true;
}

bool IReadOnlyCacheSnapshot::MapTime(double sceneTime,std::int64_t& index,std::string& error) const
{
	const auto& d=Description();
	if (!d.count || !std::isfinite(sceneTime)) return Fail(error,"invalid time or empty cache support");
	const double time=d.startTime+d.timeScale*(sceneTime-d.sceneOrigin);
	const double end=d.startTime+static_cast<double>(d.count)*d.interval;
	if (!std::isfinite(time) || (!d.hold && (time<d.startTime || time>=end)))
		return Fail(error,"time outside cache support");
	// Select by the representable authored boundaries themselves. Dividing by
	// a decimal cadence can round its exact Nth boundary below integer N.
	std::uint64_t low=0,high=d.count;
	while (high-low>1) {
		const auto mid=low+(high-low)/2;
		if (time>=d.startTime+static_cast<double>(mid)*d.interval) low=mid;
		else high=mid;
	}
	index=d.firstIndex+static_cast<std::int64_t>(low);
	return true;
}

FrameStatus CacheSnapshot::Read(std::int64_t index,const std::string& output,std::size_t budget,
	std::shared_ptr<const Bytes>& result,std::string& error) const
{
	result.reset();
	const auto type=std::find_if(description_.outputs.begin(),description_.outputs.end(),
		[&](const CacheOutput& descriptor){return descriptor.name==output;});
	if (type==description_.outputs.end() || type->type!="grid.v1") {
		error="undeclared output or unsupported native payload type"; return FrameStatus::Unsupported;
	}
	CacheEntry needle;
	needle.index=index; needle.output=output;
	auto it=std::lower_bound(entries_.begin(),entries_.end(),needle,Earlier);
	if (it==entries_.end() || it->index!=index || it->output!=output) {
		error="cache frame unavailable"; return FrameStatus::Unavailable;
	}
	auto bytes=std::make_shared<Bytes>();
	auto status=ReadFile(root_,it->digest+".frame",budget,&it->size,*bytes,error);
	if (status!=FrameStatus::Ready) return status;
	if (RISECBOR64::SHA256Hex(*bytes)!=it->digest) { error="cache frame digest mismatch"; return FrameStatus::Corrupt; }
	result=std::move(bytes);
	return FrameStatus::Ready;
}

bool IReadOnlyCacheSnapshot::PreflightPredictive(const std::string& expectedKey,
	const Qualification& qualification,std::size_t budget,std::string& error) const
{
	if (!Complete() || Stale(expectedKey) || Description().fidelity!="predictive" ||
		!qualification.snapshot || !qualification.frame) return Fail(error,"unqualified predictive cache");
	if (!qualification.snapshot(*this,error)) return false;
	for (const auto& entry : Entries()) {
		std::shared_ptr<const Bytes> bytes;
		if (Read(entry.index,entry.output,budget,bytes,error)!=FrameStatus::Ready ||
			!qualification.frame(entry,*bytes,error)) return false;
	}
	return true;
}

struct CacheWriter::Lock {
#if defined(_WIN32)
	HANDLE handle=INVALID_HANDLE_VALUE;
	~Lock(){if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);}
	bool Acquire(const fs::path& root,bool wait,std::string& error) {
		handle=CreateFileW((root/"writer.lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
			nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
		if (handle==INVALID_HANDLE_VALUE) return Fail(error,"cache writer lock busy or unavailable");
		BY_HANDLE_FILE_INFORMATION info{};
		if (!GetFileInformationByHandle(handle,&info) ||
			(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
			return Fail(error,"invalid writer lock");
		OVERLAPPED range{};
		const DWORD flags=LOCKFILE_EXCLUSIVE_LOCK|(wait?0:LOCKFILE_FAIL_IMMEDIATELY);
		return LockFileEx(handle,flags,0,1,0,&range)!=0 || Fail(error,"cache writer lock busy or unavailable");
	}
	bool Write(const fs::path& root,const std::string& name,const Bytes& bytes,bool replace,std::string& error) {
		const auto pending=root/".cache.pending";
		if (!DeleteFileW(pending.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND)
			return Fail(error,"cannot clear interrupted cache write");
		HANDLE file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
		if (file==INVALID_HANDLE_VALUE) return Fail(error,"cannot create staged cache file");
		std::size_t pos=0;
		while (pos<bytes.size()) {
			DWORD wrote=0;
			const auto chunk=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-pos,1u<<20));
			if (!WriteFile(file,bytes.data()+pos,chunk,&wrote,nullptr) || !wrote) break;
			pos+=wrote;
		}
		const bool durable=pos==bytes.size() && FlushFileBuffers(file);
		const bool closed=CloseHandle(file)!=0;
		if (!durable || !closed) return Fail(error,"cache file flush failed");
		const DWORD flags=MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0);
		if (!MoveFileExW(pending.c_str(),(root/name).c_str(),flags))
			return Fail(error,"durable cache publish failed");
		return SyncDirectory(root,error);
	}
#else
	Descriptor dir{-1},file{-1};
	bool Acquire(const fs::path& root,bool wait,std::string& error) {
		dir.fd=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
		if (dir.fd<0) return Fail(error,"cannot open cache directory");
		file.fd=::openat(dir.fd,"writer.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
		struct stat info{};
		if (file.fd<0 || ::fstat(file.fd,&info)!=0 || !S_ISREG(info.st_mode))
			return Fail(error,"invalid writer lock");
		int rc;
		do {rc=::flock(file.fd,LOCK_EX|(wait?0:LOCK_NB));} while(rc!=0 && errno==EINTR);
		return rc==0 || Fail(error,"cache writer lock busy or unavailable");
	}
	bool Write(const fs::path&,const std::string& name,const Bytes& bytes,bool replace,std::string& error){
		return SyncWrite(dir.fd,name,bytes,replace,error);
	}
#endif
};

CacheState InspectCache(const fs::path& root,std::size_t budget,std::string& error)
{
	std::error_code ec;
	const auto dir=fs::symlink_status(root,ec);
	if (ec==std::errc::no_such_file_or_directory || dir.type()==fs::file_type::not_found) return CacheState::Missing;
	if (ec || !fs::is_directory(dir)) {error="invalid cache directory"; return CacheState::Corrupt;}
	bool writing=false;
	const auto path=root/"writer.lock";
	const auto lockStatus=fs::symlink_status(path,ec);
	if (lockStatus.type()!=fs::file_type::not_found) {
		if (ec || !fs::is_regular_file(lockStatus)) {error="invalid cache lock"; return CacheState::Corrupt;}
#if defined(_WIN32)
		HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
			OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
		if (file==INVALID_HANDLE_VALUE) {error="cannot inspect cache lock"; return CacheState::Corrupt;}
		OVERLAPPED range{};
		const bool acquired=LockFileEx(file,LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&range)!=0;
		const DWORD reason=acquired?ERROR_SUCCESS:GetLastError();
		CloseHandle(file);
		if (!acquired && reason!=ERROR_LOCK_VIOLATION) {error="cannot inspect cache lock"; return CacheState::Corrupt;}
		writing=!acquired;
#else
		Descriptor lock(::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
		if (lock.fd<0) {error="cannot inspect cache lock"; return CacheState::Corrupt;}
		if (::flock(lock.fd,LOCK_SH|LOCK_NB)!=0) {
			if (errno!=EWOULDBLOCK) {error="cannot inspect cache lock"; return CacheState::Corrupt;}
			writing=true;
		}
#endif
	}
	auto state=fs::symlink_status(root/manifestName,ec);
	if (state.type()==fs::file_type::not_found) return writing?CacheState::Baking:CacheState::Missing;
	CacheSnapshot snapshot;
	if (!CacheSnapshot::Open(root,budget,snapshot,error)) return CacheState::Corrupt;
	return snapshot.Complete()?CacheState::Complete:(writing?CacheState::Baking:CacheState::Partial);
}

CacheWriter::CacheWriter()=default;
CacheWriter::~CacheWriter()=default;

bool CacheWriter::Begin(const fs::path& root,const CacheDescription& d,const std::string& expectedParent,
	bool resume,std::size_t budget,std::string& error)
{
	return Open(root,d,expectedParent,resume?OpenMode::ResumeVerified:OpenMode::NewEpoch,budget,error);
}

bool CacheWriter::PrepareExtend(const fs::path& root,const CacheDescription& d,
	std::size_t budget,std::string& error)
{
	return Open(root,d,"",OpenMode::ExtendCurrent,budget,error);
}

bool CacheWriter::Open(const fs::path& root,const CacheDescription& d,const std::string& expectedParent,
	OpenMode mode,std::size_t budget,std::string& error)
{
	if (lock_) return Fail(error,"cache writer already open");
	AllocationBudget beginBudget(budget);
	if (!DescribeFits(beginBudget,d)) return Fail(error,"description exceeds writer budget");
	if (!DescriptionValid(d,error)) return false;
	std::error_code ec;
	auto absolute=fs::absolute(root,ec);
	if (ec) return Fail(error,"invalid cache root");
	if (!CreateDurableDirectory(absolute,error)) return false;
	if (ec || !fs::is_directory(fs::symlink_status(absolute,ec)) || ec)
		return Fail(error,"invalid cache root directory");
	auto lock=std::make_unique<Lock>();
	if (!lock->Acquire(absolute,mode==OpenMode::ExtendCurrent,error)) return false;
	const auto state=fs::symlink_status(absolute/manifestName,ec);
	if (ec && ec!=std::errc::no_such_file_or_directory) return Fail(error,"cannot inspect previous cache generation");
	CacheSnapshot old;
	const bool exists=state.type()!=fs::file_type::not_found;
	if (exists && !CacheSnapshot::Open(absolute,static_cast<std::size_t>(beginBudget.Remaining()),old,error)) return false;
	if (mode!=OpenMode::ExtendCurrent && old.Digest()!=expectedParent)
		return Fail(error,"cache parent generation changed");
	if (mode==OpenMode::ResumeVerified && (!exists || old.Complete()))
		return Fail(error,"cache unavailable for resume");
	const bool appendCurrent=exists && mode!=OpenMode::NewEpoch;
	if (appendCurrent) {
		if (!SnapshotFits(beginBudget,old)) return Fail(error,"resumed cache exceeds memory budget");
		Bytes left,right;
		if (!RISECBOR64::Encode(Describe(old.Description()),left,&error) ||
			!RISECBOR64::Encode(Describe(d),right,&error) || left!=right)
			return Fail(error,"resumed cache description differs");
	}
	if (mode==OpenMode::ResumeVerified) {
		for (const auto& f : old.Entries()) {
			std::shared_ptr<const Bytes> verified;
			if (old.Read(f.index,f.output,static_cast<std::size_t>(beginBudget.Remaining()),verified,error)!=FrameStatus::Ready) return false;
		}
	}
	if (mode==OpenMode::NewEpoch && exists && d.epoch==old.Description().epoch) return Fail(error,"rebake requires new epoch");
	lock_=std::move(lock);
	budget_=budget;
	if (appendCurrent) {snapshot_=std::move(old); return true;}
	CacheSnapshot first;
	first.description_=d;
	first.root_=std::move(absolute);
	if (Publish(first,error)) return true;
	lock_.reset();
	return false;
}

bool CacheWriter::Publish(CacheSnapshot& next,std::string& error)
{
	Bytes wire;
	if (!EncodeManifest(next,wire,error) || !WireFits(wire,std::min<std::uint64_t>(budget_,maximumBytes)))
		return Fail(error,"manifest exceeds budget or cannot be encoded");
	next.digest_=RISECBOR64::SHA256Hex(wire);
	if (!lock_->Write(next.root_,manifestName,wire,true,error)) {
		// A post-rename durability failure has an uncertain commit point.
		// Refuse further appends until the caller reopens and validates disk.
		lock_.reset();
		return false;
	}
	snapshot_=std::move(next);
	return true;
}

bool CacheWriter::Append(std::int64_t index,const std::string& output,double time,
	const Bytes& frame,std::string& error,const Bytes& receipt)
{
	if (!lock_ || !InRange(snapshot_.description_,index) || !KnownOutput(snapshot_.description_,output) ||
		!std::isfinite(time) || frame.size()>budget_ || frame.size()>maximumBytes ||
		receipt.size()>budget_ || receipt.size()>maximumBytes)
		return Fail(error,"invalid frame append or byte budget exceeded");
	AllocationBudget appendBudget(budget_);
	if (!SnapshotFits(appendBudget,snapshot_) || !EntryFits(appendBudget,output,receipt) ||
		!appendBudget.Charge(frame.size())) return Fail(error,"append exceeds working memory budget");
	CacheEntry entry{index,output,time,frame.size(),RISECBOR64::SHA256Hex(frame),receipt};
	auto it=std::lower_bound(snapshot_.entries_.begin(),snapshot_.entries_.end(),entry,Earlier);
	const bool duplicate=it!=snapshot_.entries_.end() && it->index==index && it->output==output;
	if (duplicate && (it->digest!=entry.digest || it->receipt!=receipt || it->time!=time || it->size!=frame.size()))
		return Fail(error,"conflicting replay of committed frame");
	if (!duplicate && snapshot_.complete_) return Fail(error,"cannot append to sealed cache");
	const auto file=entry.digest+".frame";
	std::error_code ec;
	const auto status=fs::symlink_status(snapshot_.root_/file,ec);
	if (ec && ec!=std::errc::no_such_file_or_directory) return Fail(error,"cannot inspect cached payload");
	if (status.type()!=fs::file_type::not_found) {
		Bytes existing;
		if (ReadFile(snapshot_.root_,file,appendBudget.Remaining(),&entry.size,existing,error,true)!=FrameStatus::Ready || existing!=frame)
			return Fail(error,"existing immutable payload differs");
	} else {
		if (duplicate) return Fail(error,"committed immutable payload missing");
		if (!lock_->Write(snapshot_.root_,file,frame,false,error)) return false;
	}
	if (duplicate) return true;
	CacheSnapshot next=snapshot_;
	next.entries_.insert(next.entries_.begin()+(it-snapshot_.entries_.begin()),std::move(entry));
	return Publish(next,error);
}

bool CacheWriter::Seal(std::string& error)
{
	if (!lock_) return Fail(error,"cache writer unavailable");
	if (snapshot_.complete_) return true;
	if (!Full(snapshot_.description_,snapshot_.entries_)) return Fail(error,"cannot seal incomplete output coverage");
	CacheSnapshot next=snapshot_;
	next.complete_=true;
	return Publish(next,error);
}
} }
