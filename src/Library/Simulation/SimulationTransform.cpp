#include "pch.h"
#include "SimulationTransform.h"
#include "../Utilities/RISECBOR64.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr char Magic[] = "RISETRFM";
constexpr std::size_t BodyNumbers = 13;          // position 3, orientation 4, velocity 3, angular velocity 3
constexpr std::size_t MinimumBodyBytes = 2 + 1 + 8*BodyNumbers;
constexpr double UnitTolerance = 1e-9;

void Require(bool ok, const char* message)
{
	if (!ok) throw std::runtime_error(message);
}

std::size_t Product(std::size_t a, std::size_t b)
{
	Require(a == 0 || b <= (std::numeric_limits<std::size_t>::max)()/a, "transform size overflow");
	return a*b;
}

struct Budget {
	std::size_t left;
	void Take(std::size_t bytes) {
		Require(bytes <= left, "transform memory budget exceeded");
		left -= bytes;
	}
};

void ChargeStorage(const TransformFrame& f, Budget& budget)
{
	budget.Take(f.lengthUnits.capacity() + 1);
	budget.Take(Product(f.bodies.capacity(), sizeof(TransformBody)));
	for (const auto& b : f.bodies) budget.Take(b.name.capacity() + 1);
}

void CheckText(const std::string& s, Budget scratch)
{
	Require(!s.empty() && s.size() <= 65535 && s.find('\0') == std::string::npos, "invalid transform name or units text");
	if (std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x80; })) return;
	scratch.Take(Product(s.size(), 18*sizeof(std::uint32_t)));
	Require(RISE::RISECBOR64::IsUTF8NFC(s), "transform name or units text is not NFC UTF-8");
}

bool CanonicalSign(const std::array<double,4>& q)
{
	if (q[0] != 0) return q[0] > 0;
	for (int k = 1; k < 4; ++k) if (q[k] != 0) return q[k] > 0;
	return false;
}

void CheckFrame(const TransformFrame& f, Budget& scratch)
{
	CheckText(f.lengthUnits, scratch);
	Require(std::isfinite(f.time), "nonfinite transform time");
	Require(f.bodies.size() <= 0xffffffffu, "too many transform bodies");
	for (std::size_t i = 0; i < f.bodies.size(); ++i) {
		const auto& b = f.bodies[i];
		CheckText(b.name, scratch);
		Require(i == 0 || f.bodies[i-1].name < b.name, "transform bodies must be sorted by unique name");
		for (double x : b.position) Require(std::isfinite(x), "nonfinite transform sample");
		for (double x : b.orientation) Require(std::isfinite(x), "nonfinite transform sample");
		for (double x : b.velocity) Require(std::isfinite(x), "nonfinite transform sample");
		for (double x : b.angularVelocity) Require(std::isfinite(x), "nonfinite transform sample");
		const auto& q = b.orientation;
		const double n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
		Require(std::fabs(n - 1) <= UnitTolerance, "transform orientation is not a unit quaternion");
		Require(CanonicalSign(q), "transform orientation is not in the canonical sign (w > 0)");
	}
}

struct Sink {
	Bytes* data;
	std::size_t count = 0;
	void Raw(const void* ptr, std::size_t n) {
		Require(n <= (std::numeric_limits<std::size_t>::max)() - count, "transform wire size overflow");
		count += n;
		if (data && n) {
			const auto p = static_cast<const std::uint8_t*>(ptr);
			data->insert(data->end(), p, p + n);
		}
	}
	void Word(std::uint64_t value, unsigned width) {
		std::uint8_t bytes[8];
		for (unsigned i = 0; i < width; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8*i));
		Raw(bytes, width);
	}
	void Real(double x) {
		std::uint64_t w;
		std::memcpy(&w, &x, 8);
		Word(w, 8);
	}
	void Text(const std::string& s) { Word(s.size(), 2); Raw(s.data(), s.size()); }
};

void Write(Sink& dst, const TransformFrame& f)
{
	dst.Raw(Magic, 8); dst.Word(1, 4);
	dst.Text(f.lengthUnits);
	dst.Real(f.time);
	dst.Word(f.bodies.size(), 4);
	for (const auto& b : f.bodies) {
		dst.Text(b.name);
		for (double x : b.position) dst.Real(x);
		for (double x : b.orientation) dst.Real(x);
		for (double x : b.velocity) dst.Real(x);
		for (double x : b.angularVelocity) dst.Real(x);
	}
}

struct Source {
	const Bytes& bytes;
	std::size_t pos = 0;
	std::size_t Left() const { return bytes.size() - pos; }
	const std::uint8_t* Read(std::size_t n) {
		Require(n <= Left(), "truncated transform payload");
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
	double Real() {
		const std::uint64_t w = Word(8);
		double x;
		std::memcpy(&x, &w, 8);
		return x;
	}
	std::string Text(Budget& budget) {
		const auto n = static_cast<std::size_t>(Word(2));
		const auto p = Read(n);
		std::string text(reinterpret_cast<const char*>(p), n);
		budget.Take(text.capacity() + 1);
		return text;
	}
};
}

bool RISE::Simulation::EncodeTransforms(const TransformFrame& frame, std::size_t budget, Bytes& out, std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(out.capacity());
		ChargeStorage(frame, remaining);
		CheckFrame(frame, remaining);
		Sink sizing{nullptr};
		Write(sizing, frame);
		remaining.Take(sizing.count);
		Bytes candidate;
		candidate.reserve(sizing.count);
		Sink dst{&candidate};
		Write(dst, frame);
		out.swap(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error = e.what(); return false; }
}

bool RISE::Simulation::DecodeTransforms(const Bytes& bytes, std::size_t budget, TransformFrame& out, std::string& error)
{
	try {
		Budget remaining{budget};
		remaining.Take(bytes.capacity());
		ChargeStorage(out, remaining);
		Source src{bytes};
		Require(src.Left() >= 12 && std::memcmp(src.Read(8), Magic, 8) == 0 && src.Word(4) == 1,
			"unsupported transform payload");
		TransformFrame candidate;
		candidate.lengthUnits = src.Text(remaining);
		candidate.time = src.Real();
		const auto n = static_cast<std::size_t>(src.Word(4));
		// Bound by the bytes present before allocating.
		Require(n <= src.Left()/MinimumBodyBytes, "invalid transform body count");
		remaining.Take(Product(n, sizeof(TransformBody)));
		candidate.bodies.resize(n);
		for (auto& b : candidate.bodies) {
			b.name = src.Text(remaining);
			for (double& x : b.position) x = src.Real();
			for (double& x : b.orientation) x = src.Real();
			for (double& x : b.velocity) x = src.Real();
			for (double& x : b.angularVelocity) x = src.Real();
		}
		Require(src.Left() == 0, "trailing transform payload bytes");
		CheckFrame(candidate, remaining);
		out = std::move(candidate);
		error.clear();
		return true;
	} catch (const std::exception& e) { error = e.what(); return false; }
}

bool RISE::Simulation::CanonicalQuaternion(std::array<double,4>& q)
{
	const double n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	if (!(n > 0) || !std::isfinite(n)) return false;
	for (double& x : q) x /= n;
	if (!CanonicalSign(q)) for (double& x : q) x = -x;
	return true;
}

std::array<double,9> RISE::Simulation::QuaternionMatrix(const std::array<double,4>& q)
{
	const double w = q[0], x = q[1], y = q[2], z = q[3];
	return {{1 - 2*(y*y + z*z), 2*(x*y - w*z), 2*(x*z + w*y),
		2*(x*y + w*z), 1 - 2*(x*x + z*z), 2*(y*z - w*x),
		2*(x*z - w*y), 2*(y*z + w*x), 1 - 2*(x*x + y*y)}};
}

std::array<double,4> RISE::Simulation::MatrixQuaternion(const std::array<double,9>& r)
{
	// Shepperd's method: the largest of the four squared components first.
	const double t = r[0] + r[4] + r[8];
	std::array<double,4> q;
	if (t >= r[0] && t >= r[4] && t >= r[8]) {
		const double s = 2*std::sqrt(1 + t);
		q = {{s/4, (r[7] - r[5])/s, (r[2] - r[6])/s, (r[3] - r[1])/s}};
	} else if (r[0] >= r[4] && r[0] >= r[8]) {
		const double s = 2*std::sqrt(1 + r[0] - r[4] - r[8]);
		q = {{(r[7] - r[5])/s, s/4, (r[1] + r[3])/s, (r[2] + r[6])/s}};
	} else if (r[4] >= r[8]) {
		const double s = 2*std::sqrt(1 + r[4] - r[0] - r[8]);
		q = {{(r[2] - r[6])/s, (r[1] + r[3])/s, s/4, (r[5] + r[7])/s}};
	} else {
		const double s = 2*std::sqrt(1 + r[8] - r[0] - r[4]);
		q = {{(r[3] - r[1])/s, (r[2] + r[6])/s, (r[5] + r[7])/s, s/4}};
	}
	CanonicalQuaternion(q);
	return q;
}

std::string RISE::Simulation::TransformBodyDomain(const std::vector<std::string>& names)
{
	using V = RISE::RISECBOR64::Value;
	V::Values list;
	for (const auto& n : names) list.push_back(V::String(n));
	Bytes encoded;
	if (!RISE::RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.bodies.v1"), V::ArrayValue(list)}), encoded, nullptr))
		return std::string();
	return RISE::RISECBOR64::SHA256Hex(encoded);
}
