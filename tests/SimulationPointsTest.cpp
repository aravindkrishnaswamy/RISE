// points.v1 codec (particle sets, simulation M7 prerequisites): independent
// golden bytes, round trip, canonicality, refusals, budgets, the per-point
// domain digest, and the disk cache and in-memory publisher accepting the
// payload type.
#include "../src/Library/Simulation/SimulationPoints.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
int failures = 0;
constexpr std::size_t Budget = 16*1024*1024;

void Check(bool condition, const char* message, const std::string& detail = {})
{
	if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s %s\n", message, detail.c_str()); }
}

Bytes Hex(const char* hex)
{
	auto digit = [](char c) { return c >= 'a' ? c-'a'+10 : c-'0'; };
	Bytes result;
	for (const char* p = hex; *p; p += 2) result.push_back(static_cast<std::uint8_t>(16*digit(p[0])+digit(p[1])));
	return result;
}

std::uint32_t Bits(float x) { std::uint32_t b; std::memcpy(&b, &x, 4); return b; }
std::vector<std::uint32_t> Bits(std::initializer_list<float> xs)
{
	std::vector<std::uint32_t> v;
	for (float x : xs) v.push_back(Bits(x));
	return v;
}

// Independent fixtures from the wire table (Python struct.pack '<': 8s, I, I,
// H-prefixed text, I count, [f uniform radius], [Q ids], f samples, H
// attribute count, per attribute text, text, B, f samples), hashlib for the
// domain digests.  Script: docs/SIMULATION_POINTS_FORMAT.md.
// A: 2 points, ids 7 and 42, velocities m/s, per-point radii, attributes
// color (1, x3) and temperature (K, x1).
Bytes GoldenA()
{
	return Hex(
		"52495345504e5453010000000700000001006d03006d2f730200000007000000000000002a000000000000000000803f"
		"0000004000004040000000bf0000803e000000000000003f000000000000000000000000000080bf000000400ad7233c"
		"0ad7a33c02000500636f6c6f72010031030000803f0000003f0000000000000000000000000000803f0b0074656d7065"
		"72617475726501004b01000096430040af43");
}
const char* DomainA = "0f8a54182deb4e937a3d30287ee1dfa879d68d87768d638f2ee78dc9cf97d8ff";
// B: 1 point at (0, 1, 0), uniform radius 0.05, nothing else.
Bytes GoldenB() { return Hex("52495345504e5453010000000800000001006d01000000cdcc4c3d000000000000803f000000000000"); }
const char* DomainB = "3695fc5961dab4c804313662c5662248a3536224fb1dd93f8fb23304c437ff0c";

PointsFrame FixtureA()
{
	PointsFrame f;
	f.lengthUnits = "m";
	f.velocityUnits = "m/s";
	f.ids = {7, 42};
	f.positions = Bits({1, 2, 3, -0.5f, 0.25f, 0});
	f.velocities = Bits({0.5f, 0, 0, 0, -1, 2});
	f.radii = Bits({0.01f, 0.02f});
	f.attributes = {{"color", "1", 3, Bits({1, 0.5f, 0, 0, 0, 1})}, {"temperature", "K", 1, Bits({300, 350.5f})}};
	return f;
}

PointsFrame FixtureB()
{
	PointsFrame f;
	f.positions = Bits({0, 1, 0});
	f.hasUniformRadius = true;
	f.uniformRadius = Bits(0.05f);
	return f;
}

bool Same(const PointsFrame& a, const PointsFrame& b)
{
	if (a.lengthUnits != b.lengthUnits || a.velocityUnits != b.velocityUnits || a.positions != b.positions ||
		a.velocities != b.velocities || a.radii != b.radii || a.hasUniformRadius != b.hasUniformRadius ||
		a.uniformRadius != b.uniformRadius || a.ids != b.ids || a.attributes.size() != b.attributes.size()) return false;
	for (std::size_t i = 0; i < a.attributes.size(); ++i) {
		const auto& x = a.attributes[i];
		const auto& y = b.attributes[i];
		if (x.name != y.name || x.units != y.units || x.components != y.components || x.values != y.values) return false;
	}
	return true;
}

CacheDescription Description(const char* producer)
{
	CacheDescription d;
	d.producer = producer; d.producerVersion = "1";
	d.count = 2; d.interval = 0.5;
	d.outputs = {{"particles", PointsCodec, {{"position", "m", "f32x3"}, {"radius", "m", "f32"}}}};
	std::string key, error;
	MakeCacheKey(producer, "1", RISE::RISECBOR64::Value::MapValue({}), {}, key, error);
	d.key = key;
	d.epoch = DeriveEpoch(key, "");
	return d;
}
}

int main()
{
	std::string error;
	// Golden bytes, both directions, both fixtures.
	{
		const std::pair<PointsFrame, Bytes> cases[] = {{FixtureA(), GoldenA()}, {FixtureB(), GoldenB()}};
		for (const auto& c : cases) {
			Bytes encoded;
			Check(EncodePoints(c.first, Budget, encoded, error), "encode fixture", error);
			Check(encoded == c.second, "encoding equals the independent golden fixture");
			PointsFrame decoded;
			Check(DecodePoints(c.second, Budget, decoded, error) && Same(decoded, c.first), "golden decodes to the fixture", error);
			Bytes again;
			Check(EncodePoints(decoded, Budget, again, error) && again == c.second, "decode then encode is the identity");
		}
		PointsHeader h;
		Check(PeekPoints(GoldenA(), h, error) && h.count == 2 && h.ids && h.velocities && h.radii && !h.uniformRadius,
			"peek reads the header facts", error);
		Check(PeekPoints(GoldenB(), h, error) && h.count == 1 && !h.ids && h.uniformRadius, "peek, uniform radius", error);
		Check(PointsDomain(FixtureA()) == DomainA, "domain digest of the id words (hashlib)");
		Check(PointsDomain(FixtureB()) == DomainB, "domain digest of the position words without ids (hashlib)");
		auto moved = FixtureA();
		moved.positions[0] = Bits(9.f);
		Check(PointsDomain(moved) == DomainA, "with ids the domain does not depend on positions");
	}
	// Refusals on encode (never silently changed); output untouched.
	{
		Bytes out{1, 2, 3};
		auto refuse = [&](PointsFrame f, const char* what) {
			Check(!EncodePoints(f, Budget, out, error) && out == Bytes({1, 2, 3}), what);
		};
		auto f = FixtureA(); f.ids = {42, 7}; refuse(f, "decreasing ids refused");
		f = FixtureA(); f.ids = {7, 7}; refuse(f, "repeated ids refused");
		f = FixtureA(); f.ids = {7}; refuse(f, "id count mismatch refused");
		f = FixtureA(); f.radii[1] = Bits(-0.02f); refuse(f, "negative radius refused");
		f = FixtureA(); f.radii[1] = 0x80000000u; refuse(f, "negative zero radius refused");
		f = FixtureA(); f.hasUniformRadius = true; f.uniformRadius = Bits(0.1f); refuse(f, "uniform and per-point radius refused");
		f = FixtureB(); f.uniformRadius = Bits(INFINITY); refuse(f, "nonfinite uniform radius refused");
		f = FixtureB(); f.hasUniformRadius = false; refuse(f, "uniform radius bits without the flag refused");
		f = FixtureA(); f.positions[4] = Bits(NAN); refuse(f, "nonfinite position refused");
		f = FixtureA(); f.velocities.pop_back(); refuse(f, "velocity count mismatch refused");
		f = FixtureA(); f.velocityUnits.clear(); refuse(f, "velocities without units refused");
		f = FixtureB(); f.velocityUnits = "m/s"; refuse(f, "velocity units without velocities refused");
		f = FixtureA(); f.positions.pop_back(); refuse(f, "positions not xyz refused");
		f = FixtureA(); std::swap(f.attributes[0], f.attributes[1]); refuse(f, "unsorted attributes refused");
		f = FixtureA(); f.attributes[1].name = "color"; refuse(f, "duplicate attribute names refused");
		f = FixtureA(); f.attributes[0].name = "radius"; refuse(f, "reserved attribute name refused");
		f = FixtureA(); f.attributes[0].components = 17; refuse(f, "17 components refused");
		f = FixtureA(); f.attributes[0].values.pop_back(); refuse(f, "attribute count mismatch refused");
		f = FixtureA(); f.lengthUnits.clear(); refuse(f, "empty units refused");
		f = FixtureA(); f.attributes[0].units = std::string("e\xcc\x81", 3); refuse(f, "non-NFC units refused");
		Check(!EncodePoints(FixtureA(), 64, out, error) && out == Bytes({1, 2, 3}), "encode over budget refused", error);
		PointsFrame empty;
		Check(EncodePoints(empty, Budget, out, error), "an empty point set is valid", error);
		PointsFrame back = FixtureA();
		Check(DecodePoints(out, Budget, back, error) && back.Count() == 0 && back.ids.empty(), "the empty set round-trips", error);
	}
	// Refusals on decode; failed decodes leave the output untouched.
	{
		const Bytes g = GoldenA();
		PointsFrame out = FixtureB();
		for (std::size_t cut = 0; cut < g.size(); ++cut) {
			Bytes t(g.begin(), g.begin() + static_cast<std::ptrdiff_t>(cut));
			if (DecodePoints(t, Budget, out, error)) { Check(false, "truncated payload decodes"); break; }
		}
		Check(Same(out, FixtureB()), "failed decodes leave the output untouched");
		Bytes trailing = g; trailing.push_back(0);
		Check(!DecodePoints(trailing, Budget, out, error), "trailing bytes refused");
		Bytes version = g; version[8] = 2;
		Check(!DecodePoints(version, Budget, out, error), "unknown version refused");
		Bytes flags = g; flags[12] |= 0x10;
		Check(!DecodePoints(flags, Budget, out, error), "unknown flag refused");
		Bytes both = GoldenB(); both[12] |= 0x04;
		Check(!DecodePoints(both, Budget, out, error), "uniform and per-point radius flags together refused");
		Bytes count = g;   // count at offset 16 + 3 + 5 = 24
		count[24] = 0xff; count[25] = 0xff; count[26] = 0xff; count[27] = 0x7f;
		Check(!DecodePoints(count, Budget, out, error), "point count beyond the payload refused before allocating");
		Bytes ids = g;     // ids at 28 (7) and 36 (42): make them equal
		ids[36] = 7;
		Check(!DecodePoints(ids, Budget, out, error), "non-increasing ids refused on decode");
		Bytes radius = g;  // radii at 28 + 16 + 24 + 24 = 92
		radius[95] |= 0x80;
		Check(!DecodePoints(radius, Budget, out, error), "negative radius refused on decode");
		Bytes comps = g;   // first attribute's component byte: 92 + 8 + 2 + 2 + 5 + 2 + 1 = 112
		comps[112] = 0;
		Check(!DecodePoints(comps, Budget, out, error), "zero components refused on decode");
		Check(!DecodePoints(g, 64, out, error), "budget below the payload refused", error);
		Check(Same(out, FixtureB()), "output still untouched");
		// A large set: bounded and exact.
		PointsFrame big;
		const std::size_t n = 100000;
		big.positions.resize(3*n);
		big.ids.resize(n);
		for (std::size_t i = 0; i < n; ++i) {
			big.ids[i] = 3*i + 1;
			for (int k = 0; k < 3; ++k) big.positions[3*i + k] = Bits(float(i)*0.001f + float(k));
		}
		big.hasUniformRadius = true;
		big.uniformRadius = Bits(0.002f);
		Bytes wire;
		PointsFrame back;
		Check(EncodePoints(big, Budget, wire, error) && wire.size() == 16 + 3 + 4 + 4 + 8*n + 12*n + 2, "large set encodes", error);
		Check(DecodePoints(wire, Budget, back, error) && Same(back, big), "large set round-trips", error);
		Check(!DecodePoints(wire, wire.size() + 8*n, back, error), "large set over a too-small budget refused");
	}
	// The disk cache accepts points.v1 frames and serves them back verified.
	{
		namespace fs = std::filesystem;
		const fs::path root = RISE::TestSupport::ScratchRunDir("rise_sim_points_", "cache");
		const CacheDescription d = Description("test.points");
		CacheWriter writer;
		Check(writer.Begin(root, d, "", false, Budget, error), "begin a points cache", error);
		Bytes f0, f1;
		EncodePoints(FixtureA(), Budget, f0, error);
		EncodePoints(FixtureB(), Budget, f1, error);
		Check(writer.Append(0, "particles", 0.0, f0, error) && writer.Append(1, "particles", 0.5, f1, error) && writer.Seal(error),
			"append and seal points frames", error);
		CacheSnapshot snapshot;
		Check(CacheSnapshot::Open(root, Budget, snapshot, error) && snapshot.Complete(), "reopen", error);
		std::shared_ptr<const Bytes> read;
		PointsFrame back;
		Check(snapshot.Read(0, "particles", Budget, read, error) == FrameStatus::Ready && read && *read == f0 &&
			DecodePoints(*read, Budget, back, error) && Same(back, FixtureA()), "the cache serves points.v1 back", error);
		std::error_code ec;
		fs::remove_all(root, ec);
	}
	// The in-memory publisher accepts and serves the type.
	{
		MemoryPublisher publisher;
		publisher.Reset(Description("test.points.memory"));
		Bytes f0;
		EncodePoints(FixtureA(), Budget, f0, error);
		Check(publisher.Publish(0, "particles", 0.0, std::make_shared<const Bytes>(f0), {}, error), "publish points frame", error);
		std::shared_ptr<const Bytes> read;
		Check(publisher.Current()->Read(0, "particles", Budget, read, error) == FrameStatus::Ready && read && *read == f0,
			"the memory snapshot serves points.v1", error);
	}
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("SimulationPointsTest: all checks passed\n");
	return 0;
}
