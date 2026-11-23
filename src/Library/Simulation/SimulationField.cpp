#include "pch.h"
#include "SimulationField.h"
#include "../Utilities/RISECBOR64.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr char Magic[] = "RISEFELD";
constexpr std::size_t DigestBytes = 32;
constexpr unsigned MaxComponents = 16;

void Require(bool ok, const char* message)
{
	if (!ok) throw std::runtime_error(message);
}

std::size_t Product(std::size_t a, std::size_t b)
{
	Require(a==0 || b <= (std::numeric_limits<std::size_t>::max)()/a, "field size overflow");
	return a*b;
}

struct Budget {
	std::size_t left;
	void Take(std::size_t bytes) {
		Require(bytes<=left, "field memory budget exceeded");
		left-=bytes;
	}
};

void ChargeStorage(const FieldFrame& f, Budget& budget)
{
	budget.Take(f.domain.capacity()+1);
	budget.Take(Product(f.channels.capacity(),sizeof(FieldChannel)));
	for (const auto& c : f.channels) {
		budget.Take(c.name.capacity()+c.units.capacity()+2);
		budget.Take(Product(c.values.capacity(),sizeof(std::uint32_t)));
	}
}

void CheckText(const std::string& s, Budget scratch)
{
	Require(!s.empty() && s.size()<=65535 && s.find('\0')==std::string::npos, "invalid field name or units text");
	if (std::all_of(s.begin(),s.end(),[](unsigned char c) { return c<0x80; })) return;
	scratch.Take(Product(s.size(),18*sizeof(std::uint32_t)));
	Require(RISE::RISECBOR64::IsUTF8NFC(s), "field name or units text is not NFC UTF-8");
}

bool Finite(std::uint32_t bits) { return ((bits>>23)&0xff)!=0xff; }

bool HexDigest(const std::string& d)
{
	return d.size()==2*DigestBytes && std::all_of(d.begin(),d.end(),[](char c){
		return (c>='0' && c<='9') || (c>='a' && c<='f');
	});
}

void CheckFrame(const FieldFrame& f, Budget& scratch)
{
	Require(HexDigest(f.domain), "field domain must be 64 lower-case hex digits");
	Require(!f.channels.empty() && f.channels.size()<=65535, "a field needs one to 65535 channels");
	for (std::size_t i=0;i<f.channels.size();++i) {
		const auto& c=f.channels[i];
		CheckText(c.name,scratch);
		CheckText(c.units,scratch);
		Require(i==0 || f.channels[i-1].name<c.name, "field channels must be sorted by unique name");
		Require(c.components>=1 && c.components<=MaxComponents, "field channel components must be 1 to 16");
		Require(c.values.size()==Product(f.count,c.components), "field channel sample count mismatch");
		Require(std::all_of(c.values.begin(),c.values.end(),Finite), "nonfinite field sample");
	}
}

struct Sink {
	Bytes* data;
	std::size_t count=0;
	void Raw(const void* ptr,std::size_t n) {
		Require(n <= (std::numeric_limits<std::size_t>::max)()-count,"field wire size overflow");
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

void Write(Sink& dst,const FieldFrame& f)
{
	std::uint8_t domain[DigestBytes];
	auto nibble=[](char c) { return static_cast<std::uint8_t>(c<='9' ? c-'0' : c-'a'+10); };
	for (std::size_t i=0;i<DigestBytes;++i)
		domain[i]=static_cast<std::uint8_t>((nibble(f.domain[2*i])<<4)|nibble(f.domain[2*i+1]));
	dst.Raw(Magic,8); dst.Word(1,4);
	dst.Raw(domain,DigestBytes);
	dst.Word(f.count,4);
	dst.Word(f.channels.size(),2);
	for (const auto& c : f.channels) { dst.Text(c.name); dst.Text(c.units); dst.Word(c.components,1); dst.Words(c.values); }
}

std::string Hex(const std::uint8_t* p)
{
	static const char digits[]="0123456789abcdef";
	std::string s(2*DigestBytes,'0');
	for (std::size_t i=0;i<DigestBytes;++i) { s[2*i]=digits[p[i]>>4]; s[2*i+1]=digits[p[i]&15]; }
	return s;
}

struct Source {
	const Bytes& bytes;
	std::size_t pos=0;
	std::size_t Left() const { return bytes.size()-pos; }
	const std::uint8_t* Read(std::size_t n) {
		Require(n<=Left(),"truncated field payload");
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
		Require(n<=Left()/4,"truncated field samples");
		budget.Take(Product(n,4));
		v.resize(n);
		for (auto& w:v) w=static_cast<std::uint32_t>(Word(4));
	}
};

void ReadHeader(Source& src,std::string& domain,std::uint32_t& count)
{
	Require(src.Left()>=8 && std::memcmp(src.Read(8),Magic,8)==0 && src.Word(4)==1,"unsupported field payload");
	domain=Hex(src.Read(DigestBytes));
	count=static_cast<std::uint32_t>(src.Word(4));
}
}

bool RISE::Simulation::EncodeField(const FieldFrame& frame,std::size_t budget,Bytes& out,std::string& error)
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
		Write(dst,frame);
		out.swap(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}

bool RISE::Simulation::DecodeField(const Bytes& bytes,std::size_t budget,FieldFrame& out,std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(bytes.capacity());
		ChargeStorage(out,remaining);
		Source src{bytes};
		FieldFrame candidate;
		ReadHeader(src,candidate.domain,candidate.count);
		remaining.Take(candidate.domain.capacity()+1);
		const auto n=static_cast<std::size_t>(src.Word(2));
		Require(n>=1 && n<=src.Left()/7,"invalid field channel count");
		remaining.Take(Product(n,sizeof(FieldChannel)));
		candidate.channels.resize(n);
		for (auto& c : candidate.channels) {
			c.name=src.Text(remaining);
			c.units=src.Text(remaining);
			const auto components=static_cast<std::size_t>(src.Word(1));
			Require(components>=1 && components<=MaxComponents,"field channel components must be 1 to 16");
			c.components=static_cast<std::uint8_t>(components);
			// Bound by the bytes present before allocating.
			Require(candidate.count<=src.Left()/4/components,"truncated field samples");
			src.Words(c.values,Product(candidate.count,components),remaining);
		}
		Require(src.Left()==0,"trailing field payload bytes");
		CheckFrame(candidate,remaining);
		out=std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}

bool RISE::Simulation::PeekFieldDomain(const Bytes& bytes,std::string& domain,std::uint32_t& count,std::string& error)
{
	try {
		Source src{bytes};
		std::string d;
		std::uint32_t n=0;
		ReadHeader(src,d,n);
		domain.swap(d);
		count=n;
		error.clear();
		return true;
	} catch (const std::exception& e) { error=e.what(); return false; }
}
