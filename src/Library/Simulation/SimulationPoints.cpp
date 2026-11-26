#include "pch.h"
#include "SimulationPoints.h"
#include "../Utilities/RISECBOR64.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr char Magic[] = "RISEPNTS";
constexpr std::uint32_t FlagIds = 1, FlagVelocities = 2, FlagRadii = 4, FlagUniformRadius = 8;
constexpr std::uint32_t KnownFlags = FlagIds|FlagVelocities|FlagRadii|FlagUniformRadius;
constexpr unsigned MaxComponents = 16;
constexpr std::size_t HeaderBytes = 8 + 4 + 4;

void Require(bool ok, const char* message)
{
	if (!ok) throw std::runtime_error(message);
}

std::size_t Product(std::size_t a, std::size_t b)
{
	Require(a == 0 || b <= (std::numeric_limits<std::size_t>::max)()/a, "points size overflow");
	return a*b;
}

struct Budget {
	std::size_t left;
	void Take(std::size_t bytes) {
		Require(bytes <= left, "points memory budget exceeded");
		left -= bytes;
	}
};

bool LittleEndianHost()
{
	const std::uint32_t one = 1;
	std::uint8_t first;
	std::memcpy(&first, &one, 1);
	return first == 1;
}

void ChargeStorage(const PointsFrame& f, Budget& budget)
{
	budget.Take(f.lengthUnits.capacity() + 1);
	budget.Take(f.velocityUnits.capacity() + 1);
	for (const auto* v : {&f.positions, &f.velocities, &f.radii})
		budget.Take(Product(v->capacity(), sizeof(std::uint32_t)));
	budget.Take(Product(f.ids.capacity(), sizeof(std::uint64_t)));
	budget.Take(Product(f.attributes.capacity(), sizeof(PointAttribute)));
	for (const auto& a : f.attributes) {
		budget.Take(a.name.capacity() + a.units.capacity() + 2);
		budget.Take(Product(a.values.capacity(), sizeof(std::uint32_t)));
	}
}

void CheckText(const std::string& s, Budget scratch)
{
	Require(!s.empty() && s.size() <= 65535 && s.find('\0') == std::string::npos, "invalid points units or name text");
	if (std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x80; })) return;
	scratch.Take(Product(s.size(), 18*sizeof(std::uint32_t)));
	Require(RISE::RISECBOR64::IsUTF8NFC(s), "points units or name text is not NFC UTF-8");
}

bool Finite(std::uint32_t bits) { return ((bits >> 23) & 0xff) != 0xff; }
bool Radius(std::uint32_t bits) { return Finite(bits) && (bits & 0x80000000u) == 0; }

bool Reserved(const std::string& name)
{
	return name == "position" || name == "velocity" || name == "radius" || name == "id";
}

void CheckFrame(const PointsFrame& f, Budget& scratch)
{
	CheckText(f.lengthUnits, scratch);
	Require(f.positions.size() % 3 == 0, "points positions are not xyz triples");
	const std::size_t n = f.positions.size()/3;
	Require(n <= (std::numeric_limits<std::uint32_t>::max)(), "too many points");
	Require(f.velocities.empty() || f.velocities.size() == f.positions.size(), "points velocity count mismatch");
	Require(f.velocities.empty() == f.velocityUnits.empty(), "points velocity units without velocities or vice versa");
	if (!f.velocities.empty()) CheckText(f.velocityUnits, scratch);
	Require(f.radii.empty() || f.radii.size() == n, "points radius count mismatch");
	Require(!(f.hasUniformRadius && !f.radii.empty()), "a points frame has a uniform or a per-point radius, not both");
	Require(f.hasUniformRadius || f.uniformRadius == 0, "uniform radius bits without the uniform radius");
	Require(!f.hasUniformRadius || Radius(f.uniformRadius), "points radius must be finite and nonnegative");
	Require(f.ids.empty() || f.ids.size() == n, "points id count mismatch");
	for (std::size_t i = 1; i < f.ids.size(); ++i) Require(f.ids[i-1] < f.ids[i], "points ids must be strictly increasing");
	for (const auto* v : {&f.positions, &f.velocities})
		Require(std::all_of(v->begin(), v->end(), Finite), "nonfinite points sample");
	Require(std::all_of(f.radii.begin(), f.radii.end(), Radius), "points radius must be finite and nonnegative");
	Require(f.attributes.size() <= 65535, "too many points attributes");
	for (std::size_t i = 0; i < f.attributes.size(); ++i) {
		const auto& a = f.attributes[i];
		CheckText(a.name, scratch);
		CheckText(a.units, scratch);
		Require(!Reserved(a.name), "points attribute name is reserved (position, velocity, radius, id)");
		Require(i == 0 || f.attributes[i-1].name < a.name, "points attributes must be sorted by unique name");
		Require(a.components >= 1 && a.components <= MaxComponents && a.values.size() == Product(n, a.components),
			"points attribute components or count mismatch");
		Require(std::all_of(a.values.begin(), a.values.end(), Finite), "nonfinite points sample");
	}
}

std::uint32_t Flags(const PointsFrame& f)
{
	return (f.ids.empty() ? 0 : FlagIds) | (f.velocities.empty() ? 0 : FlagVelocities) |
		(f.radii.empty() ? 0 : FlagRadii) | (f.hasUniformRadius ? FlagUniformRadius : 0);
}

// Counts bytes (data == nullptr) or appends them into a presized buffer.
struct Sink {
	std::uint8_t* data;
	std::size_t count = 0;
	void Raw(const void* ptr, std::size_t n) {
		Require(n <= (std::numeric_limits<std::size_t>::max)() - count, "points wire size overflow");
		if (data && n) std::memcpy(data + count, ptr, n);
		count += n;
	}
	void Word(std::uint64_t value, unsigned width) {
		std::uint8_t bytes[8];
		for (unsigned i = 0; i < width; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8*i));
		Raw(bytes, width);
	}
	void Text(const std::string& s) { Word(s.size(), 2); Raw(s.data(), s.size()); }
	template<class W> void Words(const std::vector<W>& v) {
		const std::size_t n = Product(v.size(), sizeof(W));
		if (!data) { Raw(nullptr, n); return; }
		if (LittleEndianHost()) { Raw(v.data(), n); return; }
		for (auto w : v) Word(w, sizeof(W));
	}
};

void Write(Sink& dst, const PointsFrame& f)
{
	dst.Raw(Magic, 8); dst.Word(1, 4); dst.Word(Flags(f), 4);
	dst.Text(f.lengthUnits);
	if (!f.velocities.empty()) dst.Text(f.velocityUnits);
	dst.Word(f.Count(), 4);
	if (f.hasUniformRadius) dst.Word(f.uniformRadius, 4);
	dst.Words(f.ids);
	dst.Words(f.positions);
	dst.Words(f.velocities);
	dst.Words(f.radii);
	dst.Word(f.attributes.size(), 2);
	for (const auto& a : f.attributes) {
		dst.Text(a.name);
		dst.Text(a.units);
		dst.Word(a.components, 1);
		dst.Words(a.values);
	}
}

struct Source {
	const Bytes& bytes;
	std::size_t pos = 0;
	std::size_t Left() const { return bytes.size() - pos; }
	const std::uint8_t* Read(std::size_t n) {
		Require(n <= Left(), "truncated points payload");
		auto p = bytes.data() + pos;
		pos += n;
		return p;
	}
	std::uint64_t Word(unsigned width) {
		const auto p = Read(width);
		std::uint64_t value = 0;
		for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(p[i]) << (8*i);
		return value;
	}
	std::string Text(Budget& budget) {
		const auto n = static_cast<std::size_t>(Word(2));
		const auto p = Read(n);
		std::string text(reinterpret_cast<const char*>(p), n);
		budget.Take(text.capacity() + 1);
		return text;
	}
	// Bounded by the bytes present before allocating, then charged.
	template<class W> void Words(std::vector<W>& v, std::size_t count, Budget& budget) {
		Require(count <= Left()/sizeof(W), "truncated points payload");
		budget.Take(Product(count, sizeof(W)));
		v.resize(count);
		const auto p = Read(count*sizeof(W));
		if (LittleEndianHost()) { if (count) std::memcpy(v.data(), p, count*sizeof(W)); return; }
		for (std::size_t i = 0; i < count; ++i) {
			std::uint64_t value = 0;
			for (unsigned k = 0; k < sizeof(W); ++k) value |= std::uint64_t(p[i*sizeof(W) + k]) << (8*k);
			v[i] = static_cast<W>(value);
		}
	}
};

void ReadHeader(Source& src, std::uint32_t& flags)
{
	Require(src.Left() >= HeaderBytes && std::memcmp(src.Read(8), Magic, 8) == 0 && src.Word(4) == 1,
		"unsupported points payload");
	flags = static_cast<std::uint32_t>(src.Word(4));
	Require((flags & ~KnownFlags) == 0, "unknown points flags");
	Require((flags & (FlagRadii|FlagUniformRadius)) != (FlagRadii|FlagUniformRadius),
		"a points frame has a uniform or a per-point radius, not both");
}
}

bool RISE::Simulation::EncodePoints(const PointsFrame& frame, std::size_t budget, Bytes& out, std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(out.capacity());
		ChargeStorage(frame, remaining);
		CheckFrame(frame, remaining);
		Sink sizing{nullptr};
		Write(sizing, frame);
		remaining.Take(sizing.count);
		Bytes candidate(sizing.count);
		Sink dst{candidate.data()};
		Write(dst, frame);
		out.swap(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error = e.what(); return false; }
}

bool RISE::Simulation::DecodePoints(const Bytes& bytes, std::size_t budget, PointsFrame& out, std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(bytes.capacity());
		ChargeStorage(out, remaining);
		Source src{bytes};
		std::uint32_t flags = 0;
		ReadHeader(src, flags);
		PointsFrame candidate;
		candidate.lengthUnits = src.Text(remaining);
		if (flags & FlagVelocities) candidate.velocityUnits = src.Text(remaining);
		const auto n = static_cast<std::size_t>(src.Word(4));
		if (flags & FlagUniformRadius) {
			candidate.hasUniformRadius = true;
			candidate.uniformRadius = static_cast<std::uint32_t>(src.Word(4));
		}
		// The smallest payload N points can have: 12 bytes of positions each.
		Require(n <= src.Left()/12, "invalid points count");
		if (flags & FlagIds) src.Words(candidate.ids, n, remaining);
		src.Words(candidate.positions, Product(n, 3), remaining);
		if (flags & FlagVelocities) src.Words(candidate.velocities, Product(n, 3), remaining);
		if (flags & FlagRadii) src.Words(candidate.radii, n, remaining);
		const auto count = static_cast<std::size_t>(src.Word(2));
		Require(count <= src.Left()/(2 + 2 + 1), "invalid points attribute count");
		remaining.Take(Product(count, sizeof(PointAttribute)));
		candidate.attributes.resize(count);
		for (auto& a : candidate.attributes) {
			a.name = src.Text(remaining);
			a.units = src.Text(remaining);
			const auto c = static_cast<unsigned>(src.Word(1));
			Require(c >= 1 && c <= MaxComponents, "points attribute components outside 1-16");
			a.components = static_cast<std::uint8_t>(c);
			src.Words(a.values, Product(n, c), remaining);
		}
		Require(src.Left() == 0, "trailing points payload bytes");
		CheckFrame(candidate, remaining);
		out = std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error = e.what(); return false; }
}

bool RISE::Simulation::PeekPoints(const Bytes& bytes, PointsHeader& out, std::string& error)
{
	try {
		Source src{bytes};
		std::uint32_t flags = 0;
		ReadHeader(src, flags);
		Budget unlimited{(std::numeric_limits<std::size_t>::max)()};
		src.Text(unlimited);
		if (flags & FlagVelocities) src.Text(unlimited);
		PointsHeader h;
		h.count = static_cast<std::uint32_t>(src.Word(4));
		h.ids = (flags & FlagIds) != 0;
		h.velocities = (flags & FlagVelocities) != 0;
		h.radii = (flags & FlagRadii) != 0;
		h.uniformRadius = (flags & FlagUniformRadius) != 0;
		out = h;
		error.clear();
		return true;
	} catch (const std::exception& e) { error = e.what(); return false; }
}

std::string RISE::Simulation::PointsDomain(const PointsFrame& f)
{
	Bytes words;
	if (!f.ids.empty()) {
		words.resize(f.ids.size()*8);
		Sink dst{words.data()};
		dst.Words(f.ids);
	} else {
		words.resize(f.positions.size()*4);
		Sink dst{words.data()};
		dst.Words(f.positions);
	}
	return RISE::RISECBOR64::SHA256Hex(words);
}
