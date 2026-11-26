#include "pch.h"
#include "SimulationCache.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
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
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif

namespace RISE { namespace Simulation {
namespace {
namespace fs = std::filesystem;
using V = RISECBOR64::Value;
constexpr char manifestName[] = "manifest.cbor";
constexpr char formatV1[] = "rise.simulation.cache.v1";  // no static payloads, no evidence scope
constexpr char formatV2[] = "rise.simulation.cache.v2";
using Parts = std::vector<std::pair<const std::uint8_t*,std::size_t>>;
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
	if (!ValidEvidenceConditions(d.evidenceConditions) || (!d.evidenceScope.empty() && !TextValid(d.evidenceScope)))
		return Fail(error,"invalid evidence conditions or scope");
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
	V::Members m{{"key",V::String(d.key)},{"epoch",V::String(d.epoch)},
		{"producer",V::String(d.producer)},{"version",V::String(d.producerVersion)},
		{"first",V::Signed(d.firstIndex)},{"count",V::Unsigned(d.count)},{"start",V::Float(d.startTime)},
		{"interval",V::Float(d.interval)},{"origin",V::Float(d.sceneOrigin)},{"scale",V::Float(d.timeScale)},
		{"hold",V::Bool(d.hold)},{"outputs",V::ArrayValue(outputs)},{"fidelity",V::String(d.fidelity)},
		{"evidence",V::ArrayValue(evidence)},{"records",V::BytesValue(d.physicalRecords)}};
	// Optional members, present only when nonempty, so a description without
	// them encodes exactly as a format v1 description.
	if (!d.evidenceConditions.empty()) {
		V::Members conditions;
		for (const auto& c : d.evidenceConditions) {
			V::Values values;
			for (const auto& v : c.second) values.push_back(V::String(v));
			conditions.emplace_back(c.first,V::ArrayValue(values));
		}
		m.emplace_back("conditions",V::MapValue(conditions));
	}
	if (!d.evidenceScope.empty()) m.emplace_back("scope",V::String(d.evidenceScope));
	return V::MapValue(m);
}

bool Extended(const CacheDescription& d)
{
	return !d.evidenceConditions.empty() || !d.evidenceScope.empty();
}

bool EncodeManifest(const CacheSnapshot& snapshot, Bytes& encoded, std::string& error)
{
	V::Values frames;
	frames.reserve(snapshot.Entries().size());
	for (const auto& f : snapshot.Entries())
		frames.push_back(V::ArrayValue({V::Signed(f.index),V::String(f.output),V::Float(f.time),
			V::Unsigned(f.size),V::String(f.digest),V::BytesValue(f.receipt)}));
	V::Members m{{"description",Describe(snapshot.Description())},{"frames",V::ArrayValue(frames)},
		{"complete",V::Bool(snapshot.Complete())}};
	// Format v1 stays byte-identical for caches without static payloads or
	// evidence scope, so older readers keep reading them.
	if (snapshot.Statics().empty() && !Extended(snapshot.Description())) {
		m.emplace_back("format",V::String(formatV1));
	} else {
		V::Values statics;
		for (const auto& st : snapshot.Statics())
			statics.push_back(V::ArrayValue({V::String(st.digest),V::String(st.type),V::Unsigned(st.size)}));
		m.emplace_back("format",V::String(formatV2));
		m.emplace_back("statics",V::ArrayValue(statics));
	}
	return RISECBOR64::Encode(V::MapValue(m),encoded,&error);
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

bool DecodeDescription(const V& v, CacheDescription& d, bool extended, std::string& error)
{
	const V* conditions=extended ? v.Find("conditions") : nullptr;
	const V* scope=extended ? v.Find("scope") : nullptr;
	if (v.GetType()!=V::Map || v.GetMap().size()!=15+(conditions?1:0)+(scope?1:0) ||
		(conditions && conditions->GetType()!=V::Map) || (scope && scope->GetType()!=V::Text) ||
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
	if (conditions) {
		if (conditions->GetMap().empty()) return Fail(error,"empty evidence conditions are written as absent");
		for (const auto& c : conditions->GetMap()) {
			if (c.second.GetType()!=V::Array) return Fail(error,"invalid evidence condition");
			std::vector<std::string> values;
			for (const auto& x : c.second.GetArray()) {
				if (x.GetType()!=V::Text) return Fail(error,"invalid evidence condition value");
				values.push_back(x.GetText());
			}
			d.evidenceConditions.emplace_back(c.first,std::move(values));
		}
	}
	if (scope) {
		if (scope->GetText().empty()) return Fail(error,"an empty evidence scope is written as absent");
		d.evidenceScope=scope->GetText();
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
	if (!b.Nodes(2) || !b.Text(d.evidenceScope)) return false;
	for (const auto& c : d.evidenceConditions) {
		if (!b.Nodes(2) || !b.Text(c.first)) return false;
		for (const auto& v : c.second) if (!b.Nodes(1) || !b.Text(v)) return false;
	}
	return true;
}

bool StaticFits(AllocationBudget& b,const std::string& type)
{
	return b.Nodes(4) && b.Text(type) && b.Charge(65,32);
}

bool EntryFits(AllocationBudget& b,const std::string& output,const Bytes& receipt)
{
	return b.Nodes(7) && b.Text(output) && b.Charge(65,32) && b.Charge(receipt.size(),12);
}

bool SnapshotFits(AllocationBudget& b,const CacheSnapshot& s)
{
	if (!DescribeFits(b,s.Description())) return false;
	for (const auto& f : s.Entries()) if (!EntryFits(b,f.output,f.receipt)) return false;
	for (const auto& st : s.Statics()) if (!StaticFits(b,st.type)) return false;
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

#if defined(_WIN32)
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
#endif

#if defined(_WIN32)
bool WindowsWrite(const fs::path& root,const std::string& name,const Parts& parts,bool replace,std::string& error,
	const char* pendingName)
{
	const auto pending=root/pendingName;
	if (!DeleteFileW(pending.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND)
		return Fail(error,"cannot clear interrupted cache write");
	HANDLE file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
	if (file==INVALID_HANDLE_VALUE) return Fail(error,"cannot create staged cache file");
	bool written=true;
	for (const auto& part : parts) {
		std::size_t pos=0;
		while (written && pos<part.second) {
			DWORD wrote=0;
			const auto chunk=static_cast<DWORD>(std::min<std::size_t>(part.second-pos,1u<<20));
			if (!WriteFile(file,part.first+pos,chunk,&wrote,nullptr) || !wrote) written=false;
			else pos+=wrote;
		}
	}
	const bool durable=written && FlushFileBuffers(file);
	const bool closed=CloseHandle(file)!=0;
	if (!durable || !closed) return Fail(error,"cache file flush failed");
	const DWORD flags=MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0);
	if (!MoveFileExW(pending.c_str(),(root/name).c_str(),flags))
		return Fail(error,"durable cache publish failed");
	return SyncDirectory(root,error);
}
#endif

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

#if defined(_WIN32)
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
#endif


// ---------------------------------------------------------------- cache location policy

std::mutex trustedMutex;
std::vector<fs::path> trustedRoots; // canonical, added through AddTrustedCacheRoot

bool Canonical(const fs::path& path,fs::path& out)
{
	std::error_code ec;
	if (!path.is_absolute()) return false;
	auto c=fs::canonical(path,ec);
	if (ec || !fs::is_directory(c,ec) || ec) return false;
	out=c;
	return true;
}

// `inner` equals `root` or lies below it, component by component.
bool Within(const fs::path& inner,const fs::path& root)
{
	auto i=inner.begin();
	for (auto r=root.begin(); r!=root.end(); ++r, ++i) {
		if (r->empty()) continue;
		if (i==inner.end() || *i!=*r) return false;
	}
	return true;
}

#if !defined(_WIN32)
// An ancestor protects our entry from other users if only this user or root
// can rename entries in it (owner this user or root; group/world write only
// with the sticky bit; no ACL that grants rights). The cache directory
// itself must be owned by this user and writable by nobody else.
bool TrustedDirectory(int fd,bool cacheDirectory,const fs::path& path,std::string& error)
{
	struct stat info{};
	if (::fstat(fd,&info)!=0 || !S_ISDIR(info.st_mode)) return Fail(error,"cache path component is not a directory: "+path.string());
	const uid_t me=::geteuid();
	if (cacheDirectory) {
		if (info.st_uid!=me || (info.st_mode&0022)!=0)
			return Fail(error,"cache directory must be owned by this user and writable by nobody else: "+path.string());
	} else if ((info.st_uid!=me && info.st_uid!=0) || ((info.st_mode&0022)!=0 && (info.st_mode&S_ISVTX)==0)) {
		return Fail(error,"cache directory has an ancestor other users can modify: "+path.string()+
			" (use a private directory, or trust a root with "+TrustedCacheRootsEnvironment+")");
	}
#ifdef __APPLE__
	// Extended ALLOW entries can grant deletion or writes despite the mode;
	// deny-only ACLs (such as a home directory's) are fine.
	acl_t acl=::acl_get_fd_np(fd,ACL_TYPE_EXTENDED);
	if (!acl) {
		if (errno==ENOENT) return true; // no extended ACL
		return Fail(error,"cannot read the ACL of "+path.string());
	}
	bool allowOnly=true;
	for (int id=ACL_FIRST_ENTRY;;id=ACL_NEXT_ENTRY) {
		acl_entry_t entry;
		if (::acl_get_entry(acl,id,&entry)!=0) break;
		acl_tag_t tag;
		if (::acl_get_tag_type(entry,&tag)!=0 || tag!=ACL_EXTENDED_DENY) { allowOnly=false; break; }
	}
	::acl_free(acl);
	if (!allowOnly) return Fail(error,"cache path component carries an ACL that grants rights: "+path.string());
#endif
	return true;
}
#endif

// Verifies (and with `create`, creates owner-only) the cache directory
// `root` through one chain of descriptors from "/" or from a trusted root,
// never following a symlink below the canonical starting point. `location`
// is the canonical path every later operation must use.
bool SecureCacheDirectory(const fs::path& root,bool create,fs::path& location,std::string& error)
{
	std::error_code ec;
	fs::path wanted=fs::absolute(root,ec).lexically_normal();
	if (ec || wanted.empty()) return Fail(error,"invalid cache root");
	if (wanted.filename().empty() && wanted!=wanted.root_path()) wanted=wanted.parent_path();
	std::vector<fs::path> missing;
	fs::path existing=wanted;
	for (;;) {
		const auto st=fs::symlink_status(existing,ec);
		if (!ec && st.type()!=fs::file_type::not_found) break;
		if (existing==existing.root_path() || existing.filename().empty() || existing.filename()=="..")
			return Fail(error,"invalid cache root");
		missing.push_back(existing.filename());
		existing=existing.parent_path();
		ec.clear();
	}
	if (missing.empty() && fs::is_symlink(fs::symlink_status(wanted,ec)))
		return Fail(error,"cache directory is a symbolic link: "+wanted.string());
	fs::path base;
	if (!Canonical(existing,base)) return Fail(error,"invalid cache ancestor: "+existing.string());
#if defined(_WIN32)
	location=base;
	for (auto it=missing.rbegin(); it!=missing.rend(); ++it) location/=*it;
	if (!create) return true;
	return CreateDurableDirectory(location,error);
#else
	fs::path start("/");
	FindTrustedCacheRoot(base,start);
	Descriptor current(::open(start.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|(start==fs::path("/")?0:O_NOFOLLOW)));
	if (current.fd<0) return Fail(error,"cannot open cache ancestor "+start.string());
	fs::path walked=start;
	const fs::path below=base.lexically_relative(start);
	std::vector<fs::path> components;
	for (const auto& c : below) if (!c.empty() && c!=".") components.push_back(c);
	// The starting point: "/" is checked like any ancestor; a trusted root is not.
	if (start==fs::path("/") && !TrustedDirectory(current.fd,components.empty() && missing.empty(),walked,error)) return false;
	if (start!=fs::path("/") && components.empty() && missing.empty() &&
		!TrustedDirectory(current.fd,true,walked,error)) return false;
	for (std::size_t i=0;i<components.size();++i) {
		const bool last=i+1==components.size();
		Descriptor next(::openat(current.fd,components[i].c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
		walked/=components[i];
		if (next.fd<0) return Fail(error,"cache ancestor changed or is a symbolic link: "+walked.string());
		if (!TrustedDirectory(next.fd,last && missing.empty(),walked,error)) return false;
		// A predecessor may have died just after creating the deepest visible
		// directory, before syncing its parent's entry. Repair that cut too.
		if (last && create && ::fsync(current.fd)!=0) return Fail(error,"durable cache publication unavailable: directory flush");
		std::swap(current.fd,next.fd);
	}
	location=walked;
	for (auto it=missing.rbegin(); it!=missing.rend(); ++it) {
		const bool last=it+1==missing.rend();
		location/=*it;
		if (!create) return true; // the rest would be created below a verified directory
		if (::mkdirat(current.fd,it->c_str(),0700)!=0 && errno!=EEXIST)
			return Fail(error,"cannot create cache directory "+location.string());
		if (::fsync(current.fd)!=0) return Fail(error,"durable cache publication unavailable: directory flush");
		Descriptor next(::openat(current.fd,it->c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
		if (next.fd<0) return Fail(error,"cache directory changed or is a symbolic link: "+location.string());
		if (!TrustedDirectory(next.fd,last,location,error)) return false;
		std::swap(current.fd,next.fd);
	}
	return true;
#endif
}

#if !defined(_WIN32)
bool SyncWrite(int dir,const std::string& name,const Parts& parts,bool replace,std::string& error,
	const char* pending=".cache.pending")
{
	// The writer lock covers the directory. Remove only a prior interrupted temp.
	if (::unlinkat(dir,pending,0)!=0 && errno!=ENOENT) return Fail(error,"cannot clean interrupted cache write");
	Descriptor file(::openat(dir,pending,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
	if (file.fd<0) return Fail(error,"cannot create staged cache file");
	for (const auto& part : parts) {
		std::size_t pos=0;
		while (pos<part.second) {
			auto n=::write(file.fd,part.first+pos,std::min<std::size_t>(part.second-pos,1u<<20));
			if (n<0 && errno==EINTR) continue;
			if (n<=0) return Fail(error,"cache file write failed");
			pos+=static_cast<std::size_t>(n);
		}
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

bool IsNativePayloadType(const std::string& type)
{
	return type=="grid.v1" || type=="mesh.v1" || type=="mesh.v2" || type=="field.v1" ||
		type=="transform.v1" || // M6: rigid bodies (SimulationTransform.h)
		type=="points.v1"; // M7 prerequisites: particle sets (SimulationPoints.h)
}

bool IsNativeStaticType(const std::string& type)
{
	return type==MeshTopologyStaticType;
}

bool NativePayloadReferences(const std::string& type,const Bytes& payload,std::vector<std::string>& digests,
	std::string& error)
{
	digests.clear();
	if (type!="mesh.v2") return true;
	return MeshPayloadReferences(payload,digests,error);
}

bool ValidEvidenceConditions(const EvidenceConditions& conditions)
{
	for (std::size_t i=0;i<conditions.size();++i) {
		const auto& c=conditions[i];
		if (!TextValid(c.first) || (i && !(conditions[i-1].first<c.first)) || c.second.empty()) return false;
		for (std::size_t k=0;k<c.second.size();++k)
			if (!TextValid(c.second[k]) || (k && !(c.second[k-1]<c.second[k]))) return false;
	}
	return true;
}

bool ValidateCacheDescription(const CacheDescription& description,std::string& error)
{
	return DescriptionValid(description,error);
}

namespace {
bool SidecarName(const std::string& name)
{
	return TextValid(name) && name.size()<=200 && name!="." && name!=".." && name!=manifestName &&
		name!="writer.lock" && name!=".cache.pending" && name.find_first_of("/\\:")==std::string::npos &&
		(name.size()<6 || name.compare(name.size()-6,6,".frame")!=0) &&
		(name.size()<7 || name.compare(name.size()-7,7,".static")!=0) &&
		(name.size()<8 || name.compare(name.size()-8,8,".pending")!=0);
}
}

bool ReplaceFileDurably(const fs::path& directory,const std::string& name,const Bytes& bytes,std::string& error)
{
	return ReplaceFileDurably(directory,name,Parts{{bytes.data(),bytes.size()}},error);
}

bool ReplaceFileDurably(const fs::path& directory,const std::string& name,const Parts& parts,std::string& error)
{
	if (!SidecarName(name)) return Fail(error,"invalid sidecar file name");
	const std::string pending="."+name+".pending";
#if defined(_WIN32)
	return WindowsWrite(directory,name,parts,true,error,pending.c_str());
#else
	Descriptor dir(::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
	if (dir.fd<0) return Fail(error,"cannot open sidecar directory");
	return SyncWrite(dir.fd,name,parts,true,error,pending.c_str());
#endif
}

bool AddTrustedCacheRoot(const fs::path& root,std::string& error)
{
	fs::path canonical;
	if (!Canonical(root,canonical)) return Fail(error,"a trusted cache root must be an existing absolute directory");
	std::lock_guard<std::mutex> hold(trustedMutex);
	if (std::find(trustedRoots.begin(),trustedRoots.end(),canonical)==trustedRoots.end()) trustedRoots.push_back(canonical);
	return true;
}

void ClearTrustedCacheRoots()
{
	std::lock_guard<std::mutex> hold(trustedMutex);
	trustedRoots.clear();
}

std::vector<fs::path> TrustedCacheRoots()
{
	std::vector<fs::path> roots;
	if (const char* list=std::getenv(TrustedCacheRootsEnvironment)) {
		const std::string text(list);
#if defined(_WIN32)
		const char separator=';';  // PATH convention; ':' is part of a drive letter
#else
		const char separator=':';
#endif
		std::size_t begin=0;
		while (begin<=text.size()) {
			const auto end=std::min(text.find(separator,begin),text.size());
			fs::path canonical;
			// Relative, missing or unresolvable entries are ignored (never trusted).
			if (end>begin && Canonical(fs::path(text.substr(begin,end-begin)),canonical) &&
				std::find(roots.begin(),roots.end(),canonical)==roots.end()) roots.push_back(canonical);
			begin=end+1;
		}
	}
	std::lock_guard<std::mutex> hold(trustedMutex);
	for (const auto& r : trustedRoots) if (std::find(roots.begin(),roots.end(),r)==roots.end()) roots.push_back(r);
	return roots;
}

bool FindTrustedCacheRoot(const fs::path& path,fs::path& root)
{
	fs::path best("/");
	bool found=false;
	for (const auto& r : TrustedCacheRoots())
		if (Within(path,r) && std::distance(r.begin(),r.end())>std::distance(best.begin(),best.end())) { best=r; found=true; }
	if (found) root=best;
	return found;
}

bool CheckCacheLocation(const fs::path& root,bool create,std::string& error)
{
	fs::path location;
	return SecureCacheDirectory(root,create,location,error);
}

bool ReadSidecarFile(const fs::path& directory,const std::string& name,std::size_t budget,Bytes& bytes,
	std::string& error)
{
	if (!SidecarName(name)) return Fail(error,"invalid sidecar file name");
	Bytes candidate;
	if (ReadFile(directory,name,budget,nullptr,candidate,error)!=FrameStatus::Ready) return false;
	bytes.swap(candidate);
	return true;
}

// ---------------------------------------------------------------- streamed sidecars (M7 prerequisites)

DurableSidecarWriter::~DurableSidecarWriter() { Abandon(); }

void DurableSidecarWriter::Abandon()
{
#if defined(_WIN32)
	if (stream_) { std::fclose(stream_); stream_=nullptr; std::error_code ec; fs::remove(directory_/pending_,ec); }
#else
	if (file_>=0) { ::close(file_); file_=-1; if (dir_>=0) ::unlinkat(dir_,pending_.c_str(),0); }
	if (dir_>=0) { ::close(dir_); dir_=-1; }
#endif
}

bool DurableSidecarWriter::Begin(const fs::path& directory,const std::string& name,std::string& error)
{
	Abandon();
	if (!SidecarName(name)) return Fail(error,"invalid sidecar file name");
	directory_=directory; name_=name; pending_="."+name+".pending"; written_=0;
#if defined(_WIN32)
	std::error_code ec;
	fs::remove(directory/pending_,ec);
	stream_=_wfopen((directory/pending_).c_str(),L"wb");
	if (!stream_) return Fail(error,"cannot create staged sidecar file");
	return true;
#else
	dir_=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (dir_<0) return Fail(error,"cannot open sidecar directory");
	if (::unlinkat(dir_,pending_.c_str(),0)!=0 && errno!=ENOENT) { Abandon(); return Fail(error,"cannot clean interrupted sidecar write"); }
	file_=::openat(dir_,pending_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
	if (file_<0) { Abandon(); return Fail(error,"cannot create staged sidecar file"); }
	return true;
#endif
}

bool DurableSidecarWriter::Write(const void* data,std::size_t size,std::string& error)
{
	const auto* p=static_cast<const std::uint8_t*>(data);
#if defined(_WIN32)
	if (!stream_) return Fail(error,"sidecar writer is not open");
	if (size && std::fwrite(p,1,size,stream_)!=size) return Fail(error,"sidecar write failed");
#else
	if (file_<0) return Fail(error,"sidecar writer is not open");
	std::size_t pos=0;
	while (pos<size) {
		const auto n=::write(file_,p+pos,std::min<std::size_t>(size-pos,std::size_t(1)<<20));
		if (n<0 && errno==EINTR) continue;
		if (n<=0) return Fail(error,"sidecar write failed");
		pos+=static_cast<std::size_t>(n);
	}
#endif
	written_+=size;
	return true;
}

bool DurableSidecarWriter::Commit(std::string& error)
{
#if defined(_WIN32)
	if (!stream_) return Fail(error,"sidecar writer is not open");
	const bool flushed=std::fflush(stream_)==0;
	std::fclose(stream_); stream_=nullptr;
	if (!flushed) return Fail(error,"cannot flush staged sidecar file");
	std::error_code ec;
	fs::rename(directory_/pending_,directory_/name_,ec);
	if (ec) return Fail(error,"sidecar replace failed");
	return SyncDirectory(directory_,error);
#else
	if (file_<0) return Fail(error,"sidecar writer is not open");
	if (::fsync(file_)!=0) { Abandon(); return Fail(error,"cannot sync staged sidecar file"); }
	::close(file_); file_=-1;
	if (::renameat(dir_,pending_.c_str(),dir_,name_.c_str())!=0) { ::unlinkat(dir_,pending_.c_str(),0); Abandon(); return Fail(error,"sidecar replace failed"); }
	const bool synced=::fsync(dir_)==0;
	Abandon();
	return synced || Fail(error,"cannot sync sidecar directory");
#endif
}

SidecarReader::~SidecarReader()
{
#if defined(_WIN32)
	if (stream_) std::fclose(stream_);
#else
	if (file_>=0) ::close(file_);
#endif
}

bool SidecarReader::Open(const fs::path& directory,const std::string& name,std::string& error)
{
	if (!SidecarName(name)) return Fail(error,"invalid sidecar file name");
	position_=0;
#if defined(_WIN32)
	std::error_code ec;
	const auto status=fs::symlink_status(directory/name,ec);
	if (ec || !fs::is_regular_file(status)) return Fail(error,"sidecar is missing or not a regular file");
	size_=fs::file_size(directory/name,ec);
	if (ec) return Fail(error,"cannot size sidecar");
	stream_=_wfopen((directory/name).c_str(),L"rb");
	return stream_ || Fail(error,"cannot open sidecar");
#else
	Descriptor dir(::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
	if (dir.fd<0) return Fail(error,"cannot open sidecar directory");
	file_=::openat(dir.fd,name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
	if (file_<0) return Fail(error,"cannot open sidecar");
	struct stat st{};
	if (::fstat(file_,&st)!=0 || !S_ISREG(st.st_mode)) return Fail(error,"sidecar is not a regular file");
	size_=static_cast<std::uint64_t>(st.st_size);
	return true;
#endif
}

bool SidecarReader::Read(void* data,std::size_t size,std::string& error)
{
	if (size>size_-position_) return Fail(error,"sidecar is truncated");
	auto* p=static_cast<std::uint8_t*>(data);
#if defined(_WIN32)
	if (!stream_ || (size && std::fread(p,1,size,stream_)!=size)) return Fail(error,"sidecar read failed");
#else
	std::size_t pos=0;
	while (pos<size) {
		const auto n=::read(file_,p+pos,std::min<std::size_t>(size-pos,std::size_t(1)<<20));
		if (n<0 && errno==EINTR) continue;
		if (n<=0) return Fail(error,"sidecar read failed");
		pos+=static_cast<std::size_t>(n);
	}
#endif
	position_+=size;
	return true;
}

bool SidecarReader::Seek(std::uint64_t offset,std::string& error)
{
	if (offset>size_) return Fail(error,"sidecar seek past the end");
#if defined(_WIN32)
	if (!stream_ || _fseeki64(stream_,static_cast<long long>(offset),SEEK_SET)!=0) return Fail(error,"sidecar seek failed");
#else
	if (::lseek(file_,static_cast<off_t>(offset),SEEK_SET)<0) return Fail(error,"sidecar seek failed");
#endif
	position_=offset;
	return true;
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
	const bool v2=Member(wire,"format",V::Text) && wire.Find("format")->GetText()==formatV2;
	if (wire.GetType()!=V::Map || wire.GetMap().size()!=(v2?5u:4u) || !Member(wire,"format",V::Text) ||
		(!v2 && wire.Find("format")->GetText()!=formatV1) || !Member(wire,"description",V::Map) ||
		!Member(wire,"frames",V::Array) || !Member(wire,"complete",V::Boolean) || (v2 && !Member(wire,"statics",V::Array)))
		return Fail(error,"unrecognized cache manifest");
	if (!DecodeDescription(*wire.Find("description"),candidate.description_,v2,error)) return false;
	if (v2) for (const auto& item : wire.Find("statics")->GetArray()) {
		const auto& row=item.GetArray();
		if (item.GetType()!=V::Array || row.size()!=3 || row[0].GetType()!=V::Text || row[1].GetType()!=V::Text ||
			row[2].GetType()!=V::UnsignedInteger || !DigestValid(row[0].GetText()) || !TextValid(row[1].GetText()) ||
			row[2].GetIntegerArgument()>maximumBytes ||
			(!candidate.statics_.empty() && !(candidate.statics_.back().digest<row[0].GetText())))
			return Fail(error,"invalid, duplicate or unsorted static payload entry");
		candidate.statics_.push_back({row[0].GetText(),row[1].GetText(),row[2].GetIntegerArgument()});
	}
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
	candidate.IndexSpans();
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
	if (type==description_.outputs.end() || !IsNativePayloadType(type->type)) {
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

FrameStatus CacheSnapshot::ReadStatic(const std::string& digest,std::size_t budget,
	std::shared_ptr<const Bytes>& result,std::string& error) const
{
	result.reset();
	const auto it=std::lower_bound(statics_.begin(),statics_.end(),digest,
		[](const CacheStatic& s,const std::string& d){return s.digest<d;});
	if (it==statics_.end() || it->digest!=digest) { error="static payload not declared: "+digest; return FrameStatus::Unavailable; }
	auto bytes=std::make_shared<Bytes>();
	const auto status=ReadFile(root_,digest+".static",budget,&it->size,*bytes,error);
	if (status!=FrameStatus::Ready) return status;
	if (RISECBOR64::SHA256Hex(*bytes)!=digest) { error="static payload digest mismatch"; return FrameStatus::Corrupt; }
	result=std::move(bytes);
	return FrameStatus::Ready;
}

const std::vector<CacheStatic>& IReadOnlyCacheSnapshot::Statics() const
{
	static const std::vector<CacheStatic> none;
	return none;
}

FrameStatus IReadOnlyCacheSnapshot::ReadStatic(const std::string& digest,std::size_t,std::shared_ptr<const Bytes>& bytes,
	std::string& error) const
{
	bytes.reset();
	error="static payload not declared: "+digest;
	return FrameStatus::Unavailable;
}

std::size_t IReadOnlyCacheSnapshot::EntryCount() const { return Entries().size(); }

OutputSpan IReadOnlyCacheSnapshot::Span(const std::string& output) const
{
	OutputSpan span;
	for (const auto& e : Entries()) {
		if (e.output!=output) continue;
		if (span.count==0 || e.index<span.first) span.first=e.index;
		if (span.count==0 || e.index>span.last) span.last=e.index;
		++span.count;
	}
	return span;
}

void CacheSnapshot::IndexSpans()
{
	spans_.clear();
	for (const auto& e : entries_) {
		auto it=std::find_if(spans_.begin(),spans_.end(),[&](const auto& s){return s.first==e.output;});
		if (it==spans_.end()) { spans_.push_back({e.output,OutputSpan{}}); it=spans_.end()-1; }
		auto& span=it->second;
		if (span.count==0 || e.index<span.first) span.first=e.index;
		if (span.count==0 || e.index>span.last) span.last=e.index;
		++span.count;
	}
}

OutputSpan CacheSnapshot::Span(const std::string& output) const
{
	const auto it=std::find_if(spans_.begin(),spans_.end(),[&](const auto& s){return s.first==output;});
	return it==spans_.end() ? OutputSpan{} : it->second;
}

bool IReadOnlyCacheSnapshot::FindEntry(std::int64_t index,const std::string& output,CacheEntry& entry) const
{
	CacheEntry needle;
	needle.index=index; needle.output=output;
	const auto& entries=Entries();
	const auto it=std::lower_bound(entries.begin(),entries.end(),needle,Earlier);
	if (it==entries.end() || it->index!=index || it->output!=output) return false;
	entry=*it;
	return true;
}

StaticPayloadResolver IReadOnlyCacheSnapshot::Resolver() const
{
	return [this](const std::string& digest,std::size_t budget,std::shared_ptr<const Bytes>& bytes,std::string& error) {
		return ReadStatic(digest,budget,bytes,error)==FrameStatus::Ready;
	};
}

bool IReadOnlyCacheSnapshot::PreflightPredictive(const std::string& expectedKey,
	const Qualification& qualification,std::size_t budget,std::string& error) const
{
	if (!Complete() || Stale(expectedKey) || Description().fidelity!="predictive" ||
		!qualification.snapshot || !qualification.frame) return Fail(error,"unqualified predictive cache");
	if (!qualification.snapshot(*this,error)) return false;
	for (const auto& st : Statics()) {
		std::shared_ptr<const Bytes> bytes;
		if (ReadStatic(st.digest,budget,bytes,error)!=FrameStatus::Ready) return false;
	}
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
		return WindowsWrite(root,name,Parts{{bytes.data(),bytes.size()}},replace,error,".cache.pending");
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
		return SyncWrite(dir.fd,name,Parts{{bytes.data(),bytes.size()}},replace,error);
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
	fs::path absolute;
	// Location policy: verified (or owner-only created) chain, canonical path.
	if (!SecureCacheDirectory(root,true,absolute,error)) return false;
	if (!fs::is_directory(fs::symlink_status(absolute,ec)) || ec)
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
		for (const auto& st : old.Statics()) {
			std::shared_ptr<const Bytes> verified;
			if (old.ReadStatic(st.digest,static_cast<std::size_t>(beginBudget.Remaining()),verified,error)!=FrameStatus::Ready) return false;
		}
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
	// A native frame may reference only static payloads already declared.
	const auto type=std::find_if(snapshot_.description_.outputs.begin(),snapshot_.description_.outputs.end(),
		[&](const CacheOutput& o){return o.name==output;});
	std::vector<std::string> references;
	if (!NativePayloadReferences(type->type,frame,references,error)) return Fail(error,"invalid native frame payload: "+error);
	for (const auto& r : references)
		if (!std::binary_search(snapshot_.statics_.begin(),snapshot_.statics_.end(),CacheStatic{r,"",0},
			[](const CacheStatic& a,const CacheStatic& b){return a.digest<b.digest;}))
			return Fail(error,"frame references an undeclared static payload "+r);
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
	next.IndexSpans();
	return Publish(next,error);
}

bool CacheWriter::AppendStatic(const std::string& type,const Bytes& payload,std::string& digest,std::string& error)
{
	if (!lock_ || !TextValid(type) || payload.size()>budget_ || payload.size()>maximumBytes)
		return Fail(error,"invalid static payload append or byte budget exceeded");
	AllocationBudget appendBudget(budget_);
	if (!SnapshotFits(appendBudget,snapshot_) || !StaticFits(appendBudget,type) || !appendBudget.Charge(payload.size()))
		return Fail(error,"static payload exceeds working memory budget");
	CacheStatic entry{RISECBOR64::SHA256Hex(payload),type,payload.size()};
	auto it=std::lower_bound(snapshot_.statics_.begin(),snapshot_.statics_.end(),entry,
		[](const CacheStatic& a,const CacheStatic& b){return a.digest<b.digest;});
	const bool declared=it!=snapshot_.statics_.end() && it->digest==entry.digest;
	if (declared && (it->type!=type || it->size!=entry.size)) return Fail(error,"conflicting static payload declaration");
	if (!declared && snapshot_.complete_) return Fail(error,"cannot add a static payload to a sealed cache");
	const auto file=entry.digest+".static";
	std::error_code ec;
	const auto status=fs::symlink_status(snapshot_.root_/file,ec);
	if (ec && ec!=std::errc::no_such_file_or_directory) return Fail(error,"cannot inspect static payload");
	if (status.type()!=fs::file_type::not_found) {
		Bytes existing;
		if (ReadFile(snapshot_.root_,file,appendBudget.Remaining(),&entry.size,existing,error,true)!=FrameStatus::Ready ||
			existing!=payload) return Fail(error,"existing immutable static payload differs");
	} else {
		if (declared) return Fail(error,"declared static payload missing");
		if (!lock_->Write(snapshot_.root_,file,payload,false,error)) return false;
	}
	digest=entry.digest;
	if (declared) return true;
	CacheSnapshot next=snapshot_;
	next.statics_.insert(next.statics_.begin()+(it-snapshot_.statics_.begin()),std::move(entry));
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
