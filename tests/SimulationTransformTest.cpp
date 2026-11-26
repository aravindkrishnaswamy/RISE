// transform.v1 codec (rigid bodies, simulation M6): independent golden bytes,
// round trip, canonicality, refusals, budgets, quaternion helpers, the body
// domain digest, and a cache accepting the payload type.
#include "../src/Library/Simulation/SimulationTransform.h"
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

// Independent fixture from the wire table (Python struct.pack '<' with H, d,
// I and 13d per body): units "m", time 0.5, bodies "a" and "b".
Bytes Golden()
{
	return Hex(
		"524953455452464d0100000001006d000000000000e03f020000000100610000"
		"00000000f03f00000000000000400000000000000840000000000000f03f0000"
		"00000000000000000000000000000000000000000000000000000000e03f0000"
		"0000000000000000000000000000000000000000000000000000000000000000"
		"00000000f03f010062000000000000f0bf0000000000000000000000000000d0"
		"3f000000000000e03f000000000000e03f000000000000e03f000000000000e0"
		"3f00000000000000000000000000000000000000000000000000000000000000"
		"00000000000000f03f0000000000000000");
}

TransformFrame Fixture()
{
	TransformFrame f;
	f.lengthUnits = "m";
	f.time = 0.5;
	TransformBody a;
	a.name = "a"; a.position = {{1, 2, 3}}; a.velocity = {{0.5, 0, 0}}; a.angularVelocity = {{0, 0, 1}};
	TransformBody b;
	b.name = "b"; b.position = {{-1, 0, 0.25}}; b.orientation = {{0.5, 0.5, 0.5, 0.5}}; b.angularVelocity = {{0, 1, 0}};
	f.bodies = {a, b};
	return f;
}

bool Same(const TransformFrame& a, const TransformFrame& b)
{
	if (a.lengthUnits != b.lengthUnits || a.time != b.time || a.bodies.size() != b.bodies.size()) return false;
	for (std::size_t i = 0; i < a.bodies.size(); ++i) {
		const auto& x = a.bodies[i];
		const auto& y = b.bodies[i];
		if (x.name != y.name || x.position != y.position || x.orientation != y.orientation || x.velocity != y.velocity ||
			x.angularVelocity != y.angularVelocity) return false;
	}
	return true;
}
}

int main()
{
	std::string error;
	// Golden bytes, both directions.
	{
		Bytes encoded;
		Check(EncodeTransforms(Fixture(), Budget, encoded, error), "encode fixture", error);
		Check(encoded == Golden(), "encoding equals the independent golden fixture");
		TransformFrame decoded;
		Check(DecodeTransforms(Golden(), Budget, decoded, error) && Same(decoded, Fixture()), "golden decodes to the fixture", error);
		Bytes again;
		Check(EncodeTransforms(decoded, Budget, again, error) && again == Golden(), "decode then encode is the identity");
	}
	// Refusals on encode (never silently changed).
	{
		auto bad = Fixture();
		bad.bodies[0].orientation = {{0.9, 0, 0, 0}};
		Bytes out{1, 2, 3};
		Check(!EncodeTransforms(bad, Budget, out, error) && out == Bytes({1, 2, 3}), "non-unit quaternion refused, output untouched");
		bad = Fixture();
		bad.bodies[1].orientation = {{-0.5, -0.5, -0.5, -0.5}};
		Check(!EncodeTransforms(bad, Budget, out, error), "negative-hemisphere quaternion refused");
		bad = Fixture();
		std::swap(bad.bodies[0], bad.bodies[1]);
		Check(!EncodeTransforms(bad, Budget, out, error), "unsorted bodies refused");
		bad = Fixture();
		bad.bodies[1].name = "a";
		Check(!EncodeTransforms(bad, Budget, out, error), "duplicate body names refused");
		bad = Fixture();
		bad.bodies[0].velocity[1] = std::nan("");
		Check(!EncodeTransforms(bad, Budget, out, error), "nonfinite sample refused");
		bad = Fixture();
		bad.time = INFINITY;
		Check(!EncodeTransforms(bad, Budget, out, error), "nonfinite time refused");
		bad = Fixture();
		bad.lengthUnits.clear();
		Check(!EncodeTransforms(bad, Budget, out, error), "empty units refused");
		bad = Fixture();
		bad.bodies[0].name = std::string("e\xcc\x81", 3); // e + combining acute: not NFC
		Check(!EncodeTransforms(bad, Budget, out, error), "non-NFC name refused");
		TransformFrame empty;
		Check(EncodeTransforms(empty, Budget, out, error), "an empty body set is valid", error);
		TransformFrame back;
		Check(DecodeTransforms(out, Budget, back, error) && back.bodies.empty(), "the empty set round-trips", error);
	}
	// Refusals on decode.
	{
		const Bytes g = Golden();
		TransformFrame out = Fixture();
		out.time = 7;
		for (std::size_t cut = 0; cut < g.size(); cut += 7) {
			Bytes t(g.begin(), g.begin() + static_cast<std::ptrdiff_t>(cut));
			if (DecodeTransforms(t, Budget, out, error)) { Check(false, "truncated payload decodes"); break; }
		}
		Check(out.time == 7, "failed decodes leave the output untouched");
		Bytes trailing = g;
		trailing.push_back(0);
		Check(!DecodeTransforms(trailing, Budget, out, error), "trailing bytes refused");
		Bytes version = g;
		version[8] = 2;
		Check(!DecodeTransforms(version, Budget, out, error), "unknown version refused");
		Bytes count = g;
		count[23] = 0xff; count[24] = 0xff; count[25] = 0xff; count[26] = 0x7f;   // body count at offset 23
		Check(!DecodeTransforms(count, Budget, out, error), "body count beyond the payload refused before allocating");
		Bytes sign = g;
		// Body b's w (offset of b's orientation): negate the whole quaternion's w only -> non-unit or sign-wrong.
		const std::size_t bOrientation = 27 + 2 + 1 + 13*8 + 2 + 1 + 3*8;
		sign[bOrientation + 7] ^= 0x80;
		Check(!DecodeTransforms(sign, Budget, out, error), "non-canonical quaternion refused on decode");
		Check(!DecodeTransforms(g, 64, out, error), "budget below the payload refused", error);
	}
	// Quaternion helpers.
	{
		std::array<double,4> q{{-2, 0, 0, 2}};
		Check(CanonicalQuaternion(q) && std::fabs(q[0] - std::sqrt(0.5)) < 1e-15 && q[3] < 0, "canonical: unit, w > 0");
		std::array<double,4> zero{{0, 0, 0, 0}};
		Check(!CanonicalQuaternion(zero), "zero quaternion refused");
		std::array<double,4> r{{0.3, -0.5, 0.7, 0.4}};
		CanonicalQuaternion(r);
		const auto m = QuaternionMatrix(r);
		const auto back = MatrixQuaternion(m);
		double diff = 0;
		for (int k = 0; k < 4; ++k) diff = std::max(diff, std::fabs(back[k] - r[k]));
		Check(diff < 1e-14, "matrix -> quaternion inverts quaternion -> matrix", std::to_string(diff));
		// 180 degrees about x: w = 0, x > 0.
		const auto flip = MatrixQuaternion({{1, 0, 0, 0, -1, 0, 0, 0, -1}});
		Check(flip[0] == 0 && flip[1] == 1, "half turn about x is (0, 1, 0, 0)");
	}
	// Body domain digest: names order-sensitive, deterministic, 64 hex digits.
	{
		const auto d1 = TransformBodyDomain({"a", "b"}), d2 = TransformBodyDomain({"a", "b"}), d3 = TransformBodyDomain({"a", "c"});
		Check(d1.size() == 64 && d1 == d2 && d1 != d3, "body domain digest");
	}
	// The cache accepts transform.v1 frames and serves them back verified.
	{
		namespace fs = std::filesystem;
		const fs::path root = RISE::TestSupport::ScratchRunDir("rise_sim_transform_", "cache");
		CacheDescription d;
		d.producer = "test.transforms"; d.producerVersion = "1";
		d.count = 2; d.interval = 0.5;
		d.outputs = {{"bodies", TransformCodec, {{"position", "m", "f64x3"}}}};
		std::string key;
		Check(MakeCacheKey("test.transforms", "1", RISE::RISECBOR64::Value::MapValue({}), {}, key, error), "key", error);
		d.key = key;
		d.epoch = DeriveEpoch(key, "");
		CacheWriter writer;
		Check(writer.Begin(root, d, "", false, Budget, error), "begin a transform cache", error);
		Bytes f0, f1;
		auto a = Fixture();
		a.time = 0;
		EncodeTransforms(a, Budget, f0, error);
		a.time = 0.5;
		EncodeTransforms(a, Budget, f1, error);
		Check(writer.Append(0, "bodies", 0.0, f0, error) && writer.Append(1, "bodies", 0.5, f1, error) && writer.Seal(error),
			"append and seal transform frames", error);
		CacheSnapshot snapshot;
		Check(CacheSnapshot::Open(root, Budget, snapshot, error) && snapshot.Complete(), "reopen", error);
		std::shared_ptr<const Bytes> read;
		TransformFrame back;
		Check(snapshot.Read(1, "bodies", Budget, read, error) == FrameStatus::Ready && read && *read == f1 &&
			DecodeTransforms(*read, Budget, back, error) && back.time == 0.5, "the cache serves transform.v1 back", error);
		std::error_code ec;
		fs::remove_all(root, ec);
	}
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("SimulationTransformTest: all checks passed\n");
	return 0;
}
