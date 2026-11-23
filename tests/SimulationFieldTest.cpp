// field.v1 codec (fields on a discrete element set, for coupling ports):
// independent golden bytes, round trip, canonicality, refusals, budgets.
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationMesh.h"

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

std::uint32_t Bits(float f) { std::uint32_t b; std::memcpy(&b, &f, 4); return b; }

Bytes Hex(const char* hex)
{
	auto digit = [](char c) { return c >= 'a' ? c-'a'+10 : c-'0'; };
	Bytes result;
	for (const char* p = hex; *p; p += 2) result.push_back(static_cast<std::uint8_t>(16*digit(p[0])+digit(p[1])));
	return result;
}

// Independent fixture from the wire table (Python struct): three elements of
// the mesh.v1 golden triangle's topology, "pressure" [Pa] x1, "traction" [Pa] x3.
Bytes Golden()
{
	return Hex(
		"5249534546454c44010000009c6eb9ab5cc157f4fa4f21539d5fe086be26ef18"
		"24c92bc40d303e5c90b502540300000002000800707265737375726502005061"
		"010000803f000000400000404008007472616374696f6e020050610300000000"
		"000000000000803f000000000000000000000040000000000000000000004040");
}

FieldFrame Fixture()
{
	FieldFrame f;
	f.domain = MeshTopologyDigest(3, {0, 1, 2});
	f.count = 3;
	f.channels.push_back({"pressure", "Pa", 1, {Bits(1), Bits(2), Bits(3)}});
	f.channels.push_back({"traction", "Pa", 3, {Bits(0), Bits(0), Bits(1), Bits(0), Bits(0), Bits(2), Bits(0), Bits(0), Bits(3)}});
	return f;
}

bool Same(const FieldFrame& a, const FieldFrame& b)
{
	if (a.domain != b.domain || a.count != b.count || a.channels.size() != b.channels.size()) return false;
	for (std::size_t i = 0; i < a.channels.size(); ++i)
		if (a.channels[i].name != b.channels[i].name || a.channels[i].units != b.channels[i].units ||
			a.channels[i].components != b.channels[i].components || a.channels[i].values != b.channels[i].values) return false;
	return true;
}

void GoldenAndRoundTrip()
{
	std::string error;
	Bytes encoded;
	Check(EncodeField(Fixture(), Budget, encoded, error) && encoded == Golden(), "encoder matches independent golden bytes", error);
	FieldFrame decoded;
	Check(DecodeField(Golden(), Budget, decoded, error) && Same(decoded, Fixture()), "golden decodes to the fixture", error);
	Bytes again;
	Check(EncodeField(decoded, Budget, again, error) && again == Golden(), "round trip is canonical", error);
	std::string domain;
	std::uint32_t count = 0;
	Check(PeekFieldDomain(Golden(), domain, count, error) && domain == Fixture().domain && count == 3,
		"domain and count readable from the header", error);
	FieldFrame empty = Fixture();
	empty.count = 0;
	for (auto& c : empty.channels) c.values.clear();
	Check(EncodeField(empty, Budget, encoded, error) && DecodeField(encoded, Budget, decoded, error) && decoded.count == 0,
		"an empty index space is valid", error);
}

void Refusals()
{
	std::string error;
	Bytes out{1, 2, 3};
	const Bytes keep = out;
	auto refuse = [&](FieldFrame f, const char* what) { Check(!EncodeField(f, Budget, out, error) && out == keep, what); };
	auto f = Fixture(); f.domain = "xyz"; refuse(f, "invalid domain refused");
	f = Fixture(); f.domain[0] = 'A'; refuse(f, "upper-case domain refused");
	f = Fixture(); f.channels.clear(); refuse(f, "no channels refused");
	f = Fixture(); std::swap(f.channels[0], f.channels[1]); refuse(f, "unsorted channels refused");
	f = Fixture(); f.channels[1].name = "pressure"; refuse(f, "duplicate channel refused");
	f = Fixture(); f.channels[0].values.pop_back(); refuse(f, "sample count mismatch refused");
	f = Fixture(); f.channels[0].values[0] = 0x7fc00000u; refuse(f, "NaN refused");
	f = Fixture(); f.channels[0].components = 0; refuse(f, "zero components refused");
	f = Fixture(); f.channels[0].units.clear(); refuse(f, "empty units refused");
	f = Fixture(); f.channels[0].units = "A\xcc\x8a"; refuse(f, "non-NFC units refused");
	Check(!EncodeField(Fixture(), 64, out, error) && out == keep, "encode budget refusal leaves output untouched");
	FieldFrame previous = Fixture(), decoded = previous;
	const auto golden = Golden();
	for (std::size_t n = 0; n < golden.size(); ++n) {
		Bytes cut(golden.begin(), golden.begin()+static_cast<std::ptrdiff_t>(n));
		if (DecodeField(cut, Budget, decoded, error)) { Check(false, "truncated payload refused"); break; }
	}
	Check(Same(decoded, previous), "truncation leaves output untouched");
	auto g = golden; g.push_back(0);
	Check(!DecodeField(g, Budget, decoded, error), "trailing byte refused");
	g = golden; g[8] = 2;
	Check(!DecodeField(g, Budget, decoded, error), "unknown version refused");
	g = golden; g[44] = 0xff; g[45] = 0xff; g[46] = 0xff; g[47] = 0x7f;
	Check(!DecodeField(g, Budget, decoded, error), "hostile element count refused before allocating");
	Check(!DecodeField(golden, golden.size(), decoded, error) && Same(decoded, previous), "decode budget charges input and output");
}
} // namespace

int main()
{
	GoldenAndRoundTrip();
	Refusals();
	if (failures) { std::fprintf(stderr, "%d field codec check(s) failed\n", failures); return 1; }
	std::printf("Simulation field codec checks passed\n");
	return 0;
}
