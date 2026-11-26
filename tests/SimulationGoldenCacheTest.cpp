// Simulation M8: cross-platform golden cache.
//
// tests/data/simulation_golden_cache/ is a small cache committed to git, written
// on macOS arm64 by this program's --write mode. It holds every native codec:
// grid.v1 (dense and constant blocks, a non-trivial transform, a background),
// mesh.v1, mesh.v2 with inline connectivity whose topology changes and then
// empties, mesh.v2 referencing a static mesh.topology.v1 record, field.v1,
// transform.v1 and points.v1, three frames each, in one manifest.
//
// The default mode, on any platform:
//   1. opens the committed cache and checks the manifest (description, every
//      index entry's time, size and digest) against values in this source;
//   2. decodes every payload and compares every decoded value with the frames
//      built below from literals;
//   3. re-encodes those frames into a fresh cache and requires every file of
//      the fresh cache to equal the committed file byte for byte.
// A platform whose codecs differ in byte order, widths, padding or float
// handling fails 1-3. Regenerate (only for a deliberate format change) with
//   bin/tests/SimulationGoldenCacheTest --write tests/data/simulation_golden_cache
// after removing the old directory, and record why in the M8 results note.

#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationGrid.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationPoints.h"
#include "../src/Library/Simulation/SimulationTransform.h"
#include "../src/Library/Utilities/RISECBOR64.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace RISE::Simulation;
namespace fs = std::filesystem;

namespace {
using Bytes = std::vector<std::uint8_t>;
int failures = 0;
constexpr std::size_t Budget = 16u << 20;
const char* const FixturePath = "tests/data/simulation_golden_cache";

void Check(bool condition, const char* message, const std::string& detail = {})
{
	if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s %s\n", message, detail.c_str()); }
}

std::uint32_t Bits(float f) { std::uint32_t b; std::memcpy(&b, &f, 4); return b; }
std::vector<std::uint32_t> Bits(std::initializer_list<float> values)
{
	std::vector<std::uint32_t> out;
	for (float v : values) out.push_back(Bits(v));
	return out;
}

// ------------------------------------------------------------------ the frames (expected values)

GridFrame Grid(int frame)
{
	const float s = float(frame);
	GridFrame g;
	GridChannel density;
	density.name = "density"; density.units = "kg/m3"; density.components = 1;
	density.origin = {{-1, 0, 2}}; density.dimensions = {{3, 2, 2}};
	// Row-major index-to-world: scale, shear and translation (nonsymmetric).
	density.indexToWorld = {{0.25, 0.0625, 0, -1.5,  0, 0.25, 0, 0.75,  0, 0, 0.5, 3,  0, 0, 0, 1}};
	density.background = {{Bits(0.125f), 0, 0}};
	GridBlock dense;
	dense.origin = {{-1, 0, 2}}; dense.dimensions = {{2, 2, 2}}; dense.constant = false;
	dense.active = {0x5b};                    // 8 cells, x fastest, bit i = cell i
	dense.values = Bits({1.0f + s, 2.5f, -0.0f, 3.75f, 1e-30f, -7.0f, 0.5f + s, 65504.0f});
	GridBlock constant;
	constant.origin = {{1, 0, 2}}; constant.dimensions = {{1, 2, 2}}; constant.constant = true;
	constant.active = {1}; constant.values = {Bits(0.25f*s)};
	density.blocks = {dense, constant};
	GridChannel velocity;
	velocity.name = "velocity"; velocity.units = "m/s"; velocity.components = 3;
	velocity.origin = {{0, 0, 0}}; velocity.dimensions = {{1, 1, 3}};
	velocity.background = {{Bits(0.0f), Bits(0.0f), Bits(-1.0f)}};
	GridBlock column;
	column.origin = {{0, 0, 0}}; column.dimensions = {{1, 1, 3}};
	column.active = {0x05};
	column.values = Bits({0.5f, -0.25f, 1.0f + s,  0, 0, 0,  -2.0f, 3.0f*s, 0.125f});
	velocity.blocks = {column};
	g.channels = {density, velocity};
	return g;
}

MeshFrame MeshV1(int frame)
{
	const float s = float(frame);
	MeshFrame m;
	m.positionUnits = "m"; m.velocityUnits = "m/s";
	m.positions = Bits({0, 0, 0.1f*s,  1, 0, 0,  0, 2, -0.5f});
	m.velocities = Bits({0, 0, 0.1f,  0, 0, 0,  0.25f, 0, 0});
	m.normals = Bits({0, 0, 1,  0, 0, 1,  0, 0.6f, 0.8f});
	m.indices = {0, 1, 2};
	return m;
}

// Inline connectivity that changes every frame; the last frame is empty.
MeshFrame Splash(int frame)
{
	MeshFrame m;
	m.positionUnits = "m";
	if (frame == 0) {
		m.positions = Bits({0, 0, 0,  1, 0, 0,  1, 1, 0,  0, 1, 0});
		m.indices = {0, 1, 2,  0, 2, 3};
		m.uvSets = {{"uv", "1", Bits({0, 0,  1, 0,  1, 1,  0, 1})}};
		m.attributes = {{"temperature", "K", 1, Bits({300, 301.5f, 302, 299.25f})}};
	} else if (frame == 1) {
		m.velocityUnits = "m/s";
		m.positions = Bits({0, 0, 0,  2, 0, 0,  0, 2, 0,  0, 0, 2,  0.5f, 0.5f, 0.5f});
		m.velocities = Bits({0, 0, -1,  0, 0, -1,  0, 0, -1,  0, 0, -1,  0, 0, 0});
		m.indices = {0, 2, 1,  0, 1, 3,  0, 3, 2};    // vertex 4 unreferenced
		m.uvSets = {{"uv", "1", Bits({0, 0,  1, 0,  0, 1,  1, 1,  0.5f, 0.5f})}};
		m.attributes = {{"temperature", "K", 1, Bits({310, 311, 312, 313, 314})},
			{"wetness", "1", 2, Bits({0, 1,  0.5f, 0.5f,  1, 0,  0.25f, 0.75f,  0, 0})}};
	}
	// frame 2: V = T = 0
	return m;
}

MeshFrame Sheet(int frame)
{
	const float s = float(frame);
	MeshFrame m;
	m.positionUnits = "m";
	m.positions = Bits({0, 0, 0,  1, 0, 0.05f*s,  0, 1, 0,  1, 1, -0.05f*s});
	m.normals = Bits({0, 0, 1,  0, 0, 1,  0, 0, 1,  0, 0, 1});
	m.indices = {0, 1, 3,  0, 3, 2};
	m.uvSets = {{"material", "m", Bits({0, 0,  0.5f, 0,  0, 0.5f,  0.5f, 0.5f})}};
	m.attributes = {{"strain", "1", 1, Bits({0, 0.01f*s, 0, -0.01f*s})}};
	return m;
}

FieldFrame Field(int frame)
{
	const float s = float(frame);
	FieldFrame f;
	f.domain = std::string(32, 'a') + std::string(32, '7');
	f.count = 3;
	f.channels = {{"pressure", "Pa", 1, Bits({101325.0f + s, 0.5f, -12.0f})},
		{"traction", "N/m2", 3, Bits({1, 0, 0,  0, -1, 0,  0.25f*s, 0, 1e-20f})}};
	return f;
}

TransformFrame Transforms(int frame)
{
	TransformFrame t;
	t.lengthUnits = "m";
	t.time = 0.5*frame;
	TransformBody a;
	a.name = "box";
	a.position = {{1.0 + 0.1*frame, -2.0, 0.3333333333333333}};
	a.orientation = {{0.5, 0.5, 0.5, 0.5}};
	a.velocity = {{0.2, 0, -9.80665*0.5*frame}};
	a.angularVelocity = {{0, 1e-300, -3.0}};
	TransformBody b;
	b.name = "wheel";
	b.position = {{-0.0, 1e10, 2.5}};
	b.orientation = {{0.6, 0, 0.8, 0}};
	b.velocity = {{0, 0, 0}};
	b.angularVelocity = {{6.283185307179586, 0, 0}};
	t.bodies = {a, b};
	return t;
}

PointsFrame Points(int frame)
{
	const float s = float(frame);
	PointsFrame p;
	p.lengthUnits = "m"; p.velocityUnits = "m/s";
	p.positions = Bits({0, 0, 0,  0.5f, 0.25f, 1.0f - 0.1f*s,  -1, 2, 3});
	p.velocities = Bits({0, 0, -1,  0.125f, 0, 0,  0, 0, 0});
	p.radii = Bits({0.01f, 0.02f, 0.0f});
	p.ids = {UINT64_C(7), UINT64_C(0x100000000) + std::uint64_t(frame), UINT64_C(0xfedcba9876543210)};
	p.attributes = {{"age", "s", 1, Bits({0.5f*s, 1.0f, 2.0f})},
		{"color", "1", 3, Bits({1, 0, 0,  0, 1, 0,  0, 0, 1})}};
	return p;
}

CacheDescription Description()
{
	CacheDescription d;
	d.key = std::string(64, 'c'); d.epoch = std::string(63, '0') + "8";
	d.producer = "rise.m8.golden"; d.producerVersion = "1";
	d.firstIndex = 0; d.count = 3; d.startTime = 0; d.interval = 0.5; d.sceneOrigin = 0; d.timeScale = 1;
	d.outputs = {
		{"field", "field.v1", {{"pressure", "Pa", "f32"}, {"traction", "N/m2", "f32x3"}}},
		{"grid", "grid.v1", {{"density", "kg/m3", "f32"}, {"velocity", "m/s", "f32x3"}}},
		{"mesh1", "mesh.v1", {{"position", "m", "f32x3"}, {"velocity", "m/s", "f32x3"}}},
		{"points", "points.v1", {{"position", "m", "f32x3"}, {"radius", "m", "f32"}}},
		{"sheet", "mesh.v2", {{"position", "m", "f32x3"}}},
		{"splash", "mesh.v2", {{"position", "m", "f32x3"}}},
		{"transforms", "transform.v1", {{"body", "m", "f64"}}}};
	return d;
}

// ------------------------------------------------------------------ encoding

struct Encoded { std::string output; int frame; Bytes bytes; };

bool EncodeAll(std::vector<Encoded>& out, MeshStaticRecord& sheetStatic, std::string& error)
{
	MeshStaticLayout fixed; fixed.topology = true; fixed.staticUVSets = {"material"};
	const MeshStaticLayout inlineLayout;
	if (!PrepareMeshStatic(Sheet(0), fixed, Budget, sheetStatic, error)) return false;
	for (int i = 0; i < 3; ++i) {
		Bytes b, st; std::string digest;
		if (!EncodeField(Field(i), Budget, b, error)) return false;
		out.push_back({"field", i, b});
		if (!EncodeGrid(Grid(i), Budget, b, error)) return false;
		out.push_back({"grid", i, b});
		if (!EncodeMesh(MeshV1(i), Budget, b, error)) return false;
		out.push_back({"mesh1", i, b});
		if (!EncodePoints(Points(i), Budget, b, error)) return false;
		out.push_back({"points", i, b});
		if (!EncodeMeshV2(Sheet(i), fixed, Budget, sheetStatic, b, error)) return false;
		out.push_back({"sheet", i, b});
		if (!EncodeMeshV2(Splash(i), inlineLayout, Budget, b, st, digest, error) || !st.empty()) return false;
		out.push_back({"splash", i, b});
		if (!EncodeTransforms(Transforms(i), Budget, b, error)) return false;
		out.push_back({"transforms", i, b});
	}
	return true;
}

bool WriteCache(const fs::path& root, std::string& error)
{
	std::vector<Encoded> frames;
	MeshStaticRecord sheetStatic;
	if (!EncodeAll(frames, sheetStatic, error)) return false;
	CacheWriter writer;
	if (!writer.Begin(root, Description(), "", false, Budget, error)) return false;
	std::string digest;
	if (!writer.AppendStatic(MeshTopologyStaticType, sheetStatic.payload, digest, error)) return false;
	if (digest != sheetStatic.digest) { error = "static digest differs"; return false; }
	for (const auto& f : frames)
		if (!writer.Append(f.frame, f.output, 0.5*f.frame, f.bytes, error)) return false;
	return writer.Seal(error);
}

// Regular files of a cache directory by name. The writer's lock file is
// process state, not cache content, and is not committed.
std::map<std::string, Bytes> Files(const fs::path& root)
{
	std::map<std::string, Bytes> files;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(root, ec)) {
		if (!e.is_regular_file()) continue;
		const std::string name = e.path().filename().string();
		if (name.find("lock") != std::string::npos) continue;
		std::ifstream in(e.path(), std::ios::binary);
		files[name] = Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	return files;
}

// ------------------------------------------------------------------ comparisons

bool SameMesh(const MeshFrame& a, const MeshFrame& b)
{
	if (a.uvSets.size() != b.uvSets.size() || a.attributes.size() != b.attributes.size()) return false;
	for (std::size_t i = 0; i < a.uvSets.size(); ++i)
		if (a.uvSets[i].name != b.uvSets[i].name || a.uvSets[i].units != b.uvSets[i].units ||
			a.uvSets[i].values != b.uvSets[i].values) return false;
	for (std::size_t i = 0; i < a.attributes.size(); ++i)
		if (a.attributes[i].name != b.attributes[i].name || a.attributes[i].units != b.attributes[i].units ||
			a.attributes[i].components != b.attributes[i].components ||
			a.attributes[i].values != b.attributes[i].values) return false;
	return a.positionUnits == b.positionUnits && a.velocityUnits == b.velocityUnits &&
		a.positions == b.positions && a.velocities == b.velocities && a.normals == b.normals &&
		a.indices == b.indices;
}

bool SameGrid(const GridFrame& a, const GridFrame& b)
{
	if (a.channels.size() != b.channels.size()) return false;
	for (std::size_t i = 0; i < a.channels.size(); ++i) {
		const auto& x = a.channels[i]; const auto& y = b.channels[i];
		if (x.name != y.name || x.units != y.units || x.components != y.components || x.origin != y.origin ||
			x.dimensions != y.dimensions || x.background != y.background || x.blocks.size() != y.blocks.size())
			return false;
		for (unsigned k = 0; k < 16; ++k)   // bit patterns, so -0 and NaN payloads would count
			if (std::memcmp(&x.indexToWorld[k], &y.indexToWorld[k], 8) != 0) return false;
		for (std::size_t j = 0; j < x.blocks.size(); ++j) {
			const auto& p = x.blocks[j]; const auto& q = y.blocks[j];
			if (p.origin != q.origin || p.dimensions != q.dimensions || p.constant != q.constant ||
				p.active != q.active || p.values != q.values) return false;
		}
	}
	return true;
}

bool SameField(const FieldFrame& a, const FieldFrame& b)
{
	if (a.domain != b.domain || a.count != b.count || a.channels.size() != b.channels.size()) return false;
	for (std::size_t i = 0; i < a.channels.size(); ++i)
		if (a.channels[i].name != b.channels[i].name || a.channels[i].units != b.channels[i].units ||
			a.channels[i].components != b.channels[i].components || a.channels[i].values != b.channels[i].values)
			return false;
	return true;
}

template<std::size_t N> bool SameBits(const std::array<double, N>& a, const std::array<double, N>& b)
{
	return std::memcmp(a.data(), b.data(), 8*N) == 0;
}

bool SameTransforms(const TransformFrame& a, const TransformFrame& b)
{
	if (a.lengthUnits != b.lengthUnits || std::memcmp(&a.time, &b.time, 8) != 0 || a.bodies.size() != b.bodies.size())
		return false;
	for (std::size_t i = 0; i < a.bodies.size(); ++i) {
		const auto& x = a.bodies[i]; const auto& y = b.bodies[i];
		if (x.name != y.name || !SameBits(x.position, y.position) || !SameBits(x.orientation, y.orientation) ||
			!SameBits(x.velocity, y.velocity) || !SameBits(x.angularVelocity, y.angularVelocity)) return false;
	}
	return true;
}

bool SamePoints(const PointsFrame& a, const PointsFrame& b)
{
	if (a.attributes.size() != b.attributes.size()) return false;
	for (std::size_t i = 0; i < a.attributes.size(); ++i)
		if (a.attributes[i].name != b.attributes[i].name || a.attributes[i].units != b.attributes[i].units ||
			a.attributes[i].components != b.attributes[i].components ||
			a.attributes[i].values != b.attributes[i].values) return false;
	return a.lengthUnits == b.lengthUnits && a.velocityUnits == b.velocityUnits && a.positions == b.positions &&
		a.velocities == b.velocities && a.radii == b.radii && a.hasUniformRadius == b.hasUniformRadius &&
		a.uniformRadius == b.uniformRadius && a.ids == b.ids;
}

// Spot values written as raw bit patterns, independent of the builders above:
// these are what a foreign platform must reproduce from the committed bytes.
void CheckLiteralBits(const GridFrame& g, const MeshFrame& splash1, const TransformFrame& t2, const PointsFrame& p1)
{
	Check(g.channels.size() == 2 && g.channels[0].blocks.size() == 2 &&
		g.channels[0].blocks[0].values.size() == 8 &&
		g.channels[0].blocks[0].values[0] == 0x40000000u &&      // 1 + 1 (frame 1)
		g.channels[0].blocks[0].values[2] == 0x80000000u &&      // -0.0
		g.channels[0].blocks[0].values[7] == 0x477fe000u &&      // 65504
		g.channels[0].background[0] == 0x3e000000u,              // 0.125
		"grid frame 1 literal bit patterns");
	std::uint64_t shear = 0;
	if (!g.channels.empty()) std::memcpy(&shear, &g.channels[0].indexToWorld[1], 8);
	Check(shear == UINT64_C(0x3fb0000000000000), "grid transform 0.0625 as binary64 bits");
	Check(splash1.indices == std::vector<std::uint32_t>({0, 2, 1, 0, 1, 3, 0, 3, 2}) &&
		splash1.positions.size() == 15 && splash1.positions[12] == 0x3f000000u &&
		splash1.attributes.size() == 2 && splash1.attributes[1].values[7] == 0x3f400000u,
		"mesh.v2 changing-topology frame literal values");
	std::uint64_t vz = 0;
	if (!t2.bodies.empty()) std::memcpy(&vz, &t2.bodies[0].velocity[2], 8);
	Check(t2.bodies.size() == 2 && vz == UINT64_C(0xc0239d013a92a305), "transform -9.80665 as binary64 bits");
	Check(p1.ids.size() == 3 && p1.ids[1] == UINT64_C(0x100000001) && p1.ids[2] == UINT64_C(0xfedcba9876543210),
		"points 64-bit ids");
}

void Verify(const fs::path& root)
{
	CacheSnapshot snapshot;
	std::string error;
	if (!CacheSnapshot::Open(root, Budget, snapshot, error)) { Check(false, "open committed golden cache", error); return; }
	Check(snapshot.Complete(), "golden cache is sealed");
	Bytes described, expected;
	const CacheDescription& d = snapshot.Description();
	const CacheDescription e = Description();
	Check(d.key == e.key && d.epoch == e.epoch && d.producer == e.producer && d.producerVersion == e.producerVersion &&
		d.firstIndex == 0 && d.count == 3 && d.startTime == 0 && d.interval == 0.5 && d.timeScale == 1 && d.hold &&
		d.fidelity == "preview" && d.outputs.size() == e.outputs.size(), "golden description");
	for (std::size_t i = 0; i < d.outputs.size() && i < e.outputs.size(); ++i)
		Check(d.outputs[i].name == e.outputs[i].name && d.outputs[i].type == e.outputs[i].type &&
			d.outputs[i].channels.size() == e.outputs[i].channels.size(), "golden output", d.outputs[i].name);
	Check(snapshot.Entries().size() == 21 && snapshot.Statics().size() == 1, "golden entry and static counts");

	std::vector<Encoded> frames;
	MeshStaticRecord sheetStatic;
	Check(EncodeAll(frames, sheetStatic, error), "encode reference frames", error);
	Check(snapshot.Statics().size() == 1 && snapshot.Statics()[0].digest == sheetStatic.digest &&
		snapshot.Statics()[0].type == MeshTopologyStaticType, "static topology record digest");
	const StaticPayloadResolver resolve = [&](const std::string& digest, std::size_t budget,
		std::shared_ptr<const Bytes>& bytes, std::string& why) {
		return snapshot.ReadStatic(digest, budget, bytes, why) == FrameStatus::Ready;
	};

	GridFrame grid1; MeshFrame splash1; TransformFrame transforms2; PointsFrame points1;
	for (const auto& f : frames) {
		CacheEntry entry;
		const std::string label = f.output + "#" + std::to_string(f.frame);
		if (!snapshot.FindEntry(f.frame, f.output, entry)) { Check(false, "entry present", label); continue; }
		Check(entry.time == 0.5*f.frame && entry.size == f.bytes.size() &&
			entry.digest == RISE::RISECBOR64::SHA256Hex(f.bytes), "entry time, size and digest", label);
		std::shared_ptr<const Bytes> bytes;
		if (snapshot.Read(f.frame, f.output, Budget, bytes, error) != FrameStatus::Ready || !bytes) {
			Check(false, "read committed payload", label + " " + error); continue;
		}
		Check(*bytes == f.bytes, "committed payload equals the re-encoded frame", label);
		bool same = false;
		if (f.output == "grid") {
			GridFrame g; same = DecodeGrid(*bytes, Budget, g, error) && SameGrid(g, Grid(f.frame));
			if (f.frame == 1) grid1 = g;
		} else if (f.output == "mesh1") {
			MeshFrame m; same = DecodeMesh(*bytes, Budget, m, error) && SameMesh(m, MeshV1(f.frame));
		} else if (f.output == "sheet") {
			MeshFrame m; same = DecodeMesh(*bytes, Budget, resolve, m, error) && SameMesh(m, Sheet(f.frame));
		} else if (f.output == "splash") {
			MeshFrame m; same = DecodeMesh(*bytes, Budget, m, error) && SameMesh(m, Splash(f.frame));
			if (f.frame == 1) splash1 = m;
			if (f.frame == 2) Check(same && m.positions.empty() && m.indices.empty(), "empty mesh.v2 frame");
		} else if (f.output == "field") {
			FieldFrame x; same = DecodeField(*bytes, Budget, x, error) && SameField(x, Field(f.frame));
		} else if (f.output == "transforms") {
			TransformFrame t; same = DecodeTransforms(*bytes, Budget, t, error) && SameTransforms(t, Transforms(f.frame));
			if (f.frame == 2) transforms2 = t;
		} else if (f.output == "points") {
			PointsFrame p; same = DecodePoints(*bytes, Budget, p, error) && SamePoints(p, Points(f.frame));
			if (f.frame == 1) points1 = p;
		}
		Check(same, "decoded values equal the source literals", label + " " + error);
	}
	CheckLiteralBits(grid1, splash1, transforms2, points1);

	// Re-encode into a fresh cache: every committed file must come back byte for byte.
	const fs::path scratch = fs::path(RISE::TestSupport::ScratchBaseDir()) /
		("rise_golden_cache_" + std::to_string(RISE::TestSupport::CurrentProcessId()));
	std::error_code ec;
	fs::remove_all(scratch, ec);
	fs::create_directories(scratch, ec);
	const fs::path fresh = scratch / "cache";
	if (!WriteCache(fresh, error)) Check(false, "write fresh cache", error);
	else {
		const auto committed = Files(root), rewritten = Files(fresh);
		Check(committed.size() == rewritten.size() && committed.size() == 23,
			"file count (manifest, 21 frames, 1 static)", std::to_string(committed.size()) + " vs " +
			std::to_string(rewritten.size()));
		for (const auto& f : committed) {
			const auto it = rewritten.find(f.first);
			Check(it != rewritten.end() && it->second == f.second, "re-encoded file is byte-identical", f.first);
		}
	}
	fs::remove_all(scratch, ec);
}
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::string(argv[1]) == "--write") {
		std::string error;
		if (fs::exists(argv[2])) { std::fprintf(stderr, "refusing to overwrite %s\n", argv[2]); return 2; }
		// The writer accepts only private locations; stage there, then copy the
		// cache files (not the lock) to the destination.
		const fs::path stage = fs::path(RISE::TestSupport::ScratchBaseDir()) /
			("rise_golden_cache_write_" + std::to_string(RISE::TestSupport::CurrentProcessId()));
		std::error_code ec;
		fs::remove_all(stage, ec);
		if (!WriteCache(stage / "cache", error)) { std::fprintf(stderr, "write failed: %s\n", error.c_str()); return 1; }
		fs::create_directories(argv[2], ec);
		for (const auto& f : Files(stage / "cache")) {
			std::ofstream out(fs::path(argv[2]) / f.first, std::ios::binary);
			out.write(reinterpret_cast<const char*>(f.second.data()), static_cast<std::streamsize>(f.second.size()));
			if (!out) { std::fprintf(stderr, "copy failed: %s\n", f.first.c_str()); return 1; }
		}
		fs::remove_all(stage, ec);
		std::printf("wrote %s\n", argv[2]);
		return 0;
	}
	Verify(argc == 2 ? fs::path(argv[1]) : fs::path(FixturePath));
	if (failures) { std::fprintf(stderr, "SimulationGoldenCacheTest: %d failure(s)\n", failures); return 1; }
	std::printf("SimulationGoldenCacheTest passed\n");
	return 0;
}
