// Simulation M6 port adapters (SimulationPortAdapters.h): unit_scale on
// field.v1 / transform.v1 / grid.v1, field_relabel, rigid_surface,
// aerodynamic_drag (against the closed-form drag of a uniform flow, density
// from a channel / the ideal gas law / a constant, velocity in cm/s),
// contact_reaction; purity (same bytes in, same bytes out), identities, the
// automatic lookup and refusals.
#include "../src/Library/Simulation/SimulationPortAdapters.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationGrid.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationTransform.h"

#include <cmath>
#include <cstring>
#include <iostream>

namespace {
using namespace RISE::Simulation;
using V = RISE::RISECBOR64::Value;
constexpr std::size_t Budget = std::size_t(64) << 20;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
std::uint32_t Bits(double x) { const float f = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &f, 4); return b; }
double Float(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
const StaticPayloadResolver NoStatics = [](const std::string&, std::size_t, std::shared_ptr<const Bytes>&, std::string& e) {
	e = "none"; return false; };

PortDeclaration Port(const char* name, const char* codec, bool offers, const char* quantity, const char* units,
	std::vector<PortChannel> channels, std::string domain = {})
{
	PortDeclaration p;
	p.name = name; p.codec = codec; p.offers = offers; p.quantity = quantity; p.units = units;
	p.channels = std::move(channels); p.domain = std::move(domain);
	return p;
}

// A dense grid channel over [0, 1]^3 with spacing h, constant value per component.
GridChannel Uniform(const char* name, const char* units, std::vector<double> value, double h = 0.25)
{
	GridChannel c;
	c.name = name; c.units = units;
	c.components = static_cast<std::uint8_t>(value.size());
	c.dimensions = {5, 5, 5};
	c.indexToWorld = {h, 0, 0, 0, 0, h, 0, 0, 0, 0, h, 0, 0, 0, 0, 1};
	GridBlock b;
	b.dimensions = c.dimensions;
	b.active.assign((125 + 7)/8, 0xff);
	b.active.back() = 0x1f;
	for (int k = 0; k < 125; ++k) for (double v : value) b.values.push_back(Bits(v));
	c.blocks.push_back(b);
	return c;
}
}

int main()
{
	std::string error;
	Check(RegisterBuiltinPortAdapters(error), "register", error);
	const auto ids = RegisteredPortAdapterFactories();
	Check(ids.size() >= 5, "five built-in factories");
	// ---------------------------------------------------------------- unit table
	{
		double f = 0;
		Check(UnitFactor("cm/s", "m/s", f) && f == 1e-2, "cm/s -> m/s");
		Check(UnitFactor("g/m^3", "kg/m^3", f) && f == 1e-3, "g/m^3 -> kg/m^3");
		Check(UnitFactor("kN", "N", f) && f == 1e3, "kN -> N");
		Check(!UnitFactor("m", "s", f) && !UnitFactor("K", "C", f), "no conversion between quantities or affine units");
	}
	// ---------------------------------------------------------------- unit_scale
	{
		const std::string dom = RISE::RISECBOR64::SHA256Hex(Bytes{1});
		const auto offer = Port("out", "field.v1", true, "load", "kN", {{"force", "force", "kN", 3}}, dom);
		const auto accept = Port("in", "field.v1", false, "load", "N", {{"force", "force", "N", 3}}, dom);
		auto adapter = FindPortAdapter(offer, accept);
		Check(adapter != nullptr, "unit_scale is found automatically");
		std::string why;
		Check(!PortsCompatible(offer, accept, why), "the ports themselves are incompatible");
		FieldFrame f; f.domain = dom; f.count = 1; f.channels = {{"force", "kN", 3, {Bits(1), Bits(-2), Bits(0.5)}}};
		Bytes in; EncodeField(f, Budget, in, error);
		std::shared_ptr<const Bytes> out, again;
		Check(adapter && adapter->Convert(offer, accept, in, NoStatics, out, error), "convert", error);
		FieldFrame g;
		Check(out && DecodeField(*out, Budget, g, error) && g.channels[0].units == "N" && Float(g.channels[0].values[1]) == -2000,
			"kN to N on a field", error);
		Check(adapter->Convert(offer, accept, in, NoStatics, again, error) && *again == *out, "pure: the same bytes give the same bytes");
		Check(adapter->Fidelity() == "predictive" && !adapter->Identity().empty(), "exact adapter");
		// transform.v1 cm -> m.
		const auto to = Port("s", TransformCodec, true, "rigid body state", "cm", {});
		const auto ta = Port("s", TransformCodec, false, "rigid body state", "m", {});
		TransformFrame t; t.lengthUnits = "cm"; TransformBody b; b.name = "x"; b.position = {{100, 0, 50}}; b.velocity = {{10, 0, 0}};
		t.bodies = {b};
		Bytes tin; EncodeTransforms(t, Budget, tin, error);
		auto ta2 = FindPortAdapter(to, ta);
		TransformFrame tout;
		Check(ta2 && ta2->Convert(to, ta, tin, NoStatics, out, error) && DecodeTransforms(*out, Budget, tout, error) &&
			tout.lengthUnits == "m" && tout.bodies[0].position[0] == 1 && tout.bodies[0].velocity[0] == 0.1, "cm to m on transforms", error);
		// grid.v1 cm/s -> m/s.
		const auto go = Port("v", "grid.v1", true, "flow", "cm/s", {{"velocity", "velocity", "cm/s", 3}});
		const auto ga = Port("v", "grid.v1", false, "flow", "m/s", {{"velocity", "velocity", "m/s", 3}});
		GridFrame gf; gf.channels = {Uniform("velocity", "cm/s", {100, 0, -50})};
		Bytes gin; Check(EncodeGrid(gf, Budget, gin, error), "encode grid", error);
		GridFrame gout;
		auto ga2 = FindPortAdapter(go, ga);
		Check(ga2 && ga2->Convert(go, ga, gin, NoStatics, out, error) && DecodeGrid(*out, Budget, gout, error) &&
			gout.channels[0].units == "m/s" && Float(gout.channels[0].blocks[0].values[0]) == 1, "cm/s to m/s on a grid", error);
	}
	// ---------------------------------------------------------------- field_relabel
	{
		const std::string dom = RISE::RISECBOR64::SHA256Hex(Bytes{2});
		const auto offer = Port("out", "field.v1", true, "surface load", "Pa", {{"push", "pressure", "Pa", 1}}, dom);
		const auto accept = Port("in", "field.v1", false, "surface load", "Pa", {{"pressure", "pressure", "Pa", 1}}, dom);
		auto adapter = FindPortAdapter(offer, accept);
		Check(adapter != nullptr, "field_relabel is found automatically (positional)");
		FieldFrame f; f.domain = dom; f.count = 2; f.channels = {{"push", "Pa", 1, {Bits(3), Bits(4)}}};
		Bytes in; EncodeField(f, Budget, in, error);
		std::shared_ptr<const Bytes> out;
		FieldFrame g;
		Check(adapter && adapter->Convert(offer, accept, in, NoStatics, out, error) && DecodeField(*out, Budget, g, error) &&
			g.channels[0].name == "pressure" && g.channels[0].values == f.channels[0].values, "relabelled, values bit for bit", error);
		PortAdapterContext ctx; ctx.offer = &offer; ctx.accept = &accept;
		auto explicitMap = CreatePortAdapter(FieldRelabelAdapterId, V::MapValue({{"channels", V::ArrayValue({V::ArrayValue({
			V::String("push"), V::String("pressure")})})}}), ctx, error);
		Check(explicitMap && explicitMap->Identity() != adapter->Identity(), "an explicit map has its own identity", error);
		const auto otherDomain = Port("in", "field.v1", false, "surface load", "Pa", {{"pressure", "pressure", "Pa", 1}},
			RISE::RISECBOR64::SHA256Hex(Bytes{3}));
		Check(!FindPortAdapter(offer, otherDomain), "no relabel across different domains");
	}
	// ---------------------------------------------------------------- rigid_surface
	{
		const std::vector<double> local = {-0.5, 0, 0, 0.5, 0, 0, 0, 0.5, 0, 0, 0, 0.5};
		const std::vector<std::uint32_t> tris = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
		V::Values vs, ts;
		for (double x : local) vs.push_back(V::Float(x));
		for (auto k : tris) ts.push_back(V::Unsigned(k));
		const auto offer = Port("body_state", TransformCodec, true, "rigid body state", "m", {});
		const auto accept = Port("collider_surface", "mesh.v2", false, "collider surface", "m", {},
			MeshTopologyDigest(4, tris));
		PortAdapterContext ctx; ctx.offer = &offer; ctx.accept = &accept;
		auto adapter = CreatePortAdapter(RigidSurfaceAdapterId, V::MapValue({{"body", V::String("box")},
			{"vertices", V::ArrayValue(vs)}, {"triangles", V::ArrayValue(ts)}}), ctx, error);
		Check(adapter != nullptr, "rigid_surface with an explicit surface", error);
		TransformFrame t;
		TransformBody b; b.name = "box"; b.position = {{1, 2, 3}};
		const double c = std::cos(0.25), s = std::sin(0.25);
		b.orientation = {{c, 0, 0, s}};               // 0.5 rad about z
		b.velocity = {{0.5, 0, 0}}; b.angularVelocity = {{0, 0, 2}};
		t.bodies = {b};
		Bytes in; EncodeTransforms(t, Budget, in, error);
		std::shared_ptr<const Bytes> out;
		MeshFrame m;
		Check(adapter && adapter->Convert(offer, accept, in, NoStatics, out, error) && DecodeMesh(*out, Budget, m, error), "convert", error);
		// Vertex 0: r = R (-0.5, 0, 0) = (-0.5 cos 0.5, -0.5 sin 0.5, 0); v = v + w x r.
		const double rx = -0.5*std::cos(0.5), ry = -0.5*std::sin(0.5);
		Check(std::fabs(Float(m.positions[0]) - (1 + rx)) < 1e-6 && std::fabs(Float(m.positions[1]) - (2 + ry)) < 1e-6,
			"placed by the rigid transform");
		Check(std::fabs(Float(m.velocities[0]) - (0.5 - 2*ry)) < 1e-6 && std::fabs(Float(m.velocities[1]) - 2*rx) < 1e-6,
			"rigid-motion vertex velocity v + w x r");
		Check(MeshTopologyDigest(4, m.indices) == accept.domain && m.normals.size() == 12, "the collider's topology, with normals");
		const auto wrong = Port("collider_surface", "mesh.v2", false, "collider surface", "m", {}, MeshTopologyDigest(4, {0, 1, 2}));
		ctx.accept = &wrong;
		Check(!CreatePortAdapter(RigidSurfaceAdapterId, V::MapValue({{"body", V::String("box")}, {"vertices", V::ArrayValue(vs)},
			{"triangles", V::ArrayValue(ts)}}), ctx, error), "a surface of another topology is refused");
		ctx.accept = &accept;
		Check(!CreatePortAdapter(RigidSurfaceAdapterId, V::MapValue({{"body", V::String("box")}}), ctx, error),
			"no surface and no rigid record: refused");
		TransformFrame other; TransformBody o; o.name = "ball"; other.bodies = {o};
		Bytes oin; EncodeTransforms(other, Budget, oin, error);
		Check(!adapter->Convert(offer, accept, oin, NoStatics, out, error), "a transform set without the body is refused");
	}
	// ---------------------------------------------------------------- aerodynamic_drag
	{
		// A unit square sheet in z = 0.5 (normal +z), two triangles, at rest; flow u = (1, 0, 4) m/s.
		MeshFrame sheet;
		const double pts[4][3] = {{0.25, 0.25, 0.5}, {0.75, 0.25, 0.5}, {0.75, 0.75, 0.5}, {0.25, 0.75, 0.5}};
		for (auto& p : pts) for (double x : p) sheet.positions.push_back(Bits(x));
		for (int k = 0; k < 4; ++k) { sheet.velocities.insert(sheet.velocities.end(), {Bits(0), Bits(0), Bits(0)}); sheet.normals.insert(sheet.normals.end(), {Bits(0), Bits(0), Bits(1)}); }
		sheet.velocities[0] = Bits(0.5);   // vertex 0 moves at 0.5 m/s in x
		sheet.velocityUnits = "m/s";
		sheet.indices = {0, 1, 2, 0, 2, 3};
		auto surface = std::make_shared<Bytes>();
		Check(EncodeMesh(sheet, Budget, *surface, error), "encode sheet", error);
		const auto offer = Port("volume", "grid.v1", true, "volume", "-", {});
		const auto accept = Port("surface_load", "field.v1", false, "surface load", "Pa",
			{{"pressure", "pressure", "Pa", 1}, {"traction", "traction", "Pa", 3}}, MeshTopologyDigest(4, sheet.indices));
		PortAdapterContext ctx; ctx.offer = &offer; ctx.accept = &accept;
		auto check = [&](const char* what, GridFrame g, V params, double rho, double uScale) {
			Bytes in; EncodeGrid(g, Budget, in, error);
			auto adapter = CreatePortAdapter(AerodynamicDragAdapterId, params, ctx, error);
			std::shared_ptr<const Bytes> out;
			FieldFrame f;
			const bool ok = adapter && adapter->ConvertWithAuxiliary(offer, accept, in, {surface}, NoStatics, out, error) &&
				DecodeField(*out, Budget, f, error) && f.channels.size() == 1 && f.channels[0].name == "traction";
			Check(ok, std::string("drag: ") + what, error);
			if (!ok) return;
			// Vertex 1 (at rest): rel = u; un = 4 -> normal 0.5 rho Cn 16; tangential 0.5 rho Ct |1| (1, 0, 0).
			const double cn = 1.2, ct = 0.02;
			const double tz = 0.5*rho*cn*16*uScale*uScale, tx = 0.5*rho*ct*1*1*uScale*uScale;
			Check(std::fabs(Float(f.channels[0].values[5]) - tz) < 1e-5*tz && std::fabs(Float(f.channels[0].values[3]) - tx) < 1e-4*tx,
				std::string("closed-form drag on a vertex at rest: ") + what, std::to_string(Float(f.channels[0].values[5])) + " vs " + std::to_string(tz));
			// Vertex 0 moves with 0.5 m/s in x: tangential relative speed 0.5 (in m/s units of the flow).
			const double rel = uScale*1 - 0.5;
			Check(std::fabs(Float(f.channels[0].values[0]) - 0.5*rho*ct*std::fabs(rel)*rel) < 1e-6, std::string("relative velocity: ") + what);
			Check(adapter->Fidelity() == "preview", "drag is a model: preview");
		};
		GridFrame g1; g1.channels = {Uniform("velocity", "m/s", {1, 0, 4})};
		check("constant density", g1, V::MapValue({}), 1.2, 1);
		GridFrame g2; g2.channels = {Uniform("density", "kg/m^3", {0.4}), Uniform("velocity", "m/s", {1, 0, 4})};
		check("density channel", g2, V::MapValue({{"density_channel", V::String("density")}}), 0.4, 1);
		GridFrame g3; g3.channels = {Uniform("temperature", "K", {600}), Uniform("velocity", "cm/s", {100, 0, 400})};
		check("ideal gas, cm/s", g3, V::MapValue({{"temperature_channel", V::String("temperature")}}), 101325*0.028964/(8.314462618*600), 1);
		// Without the surface: refused.
		Bytes in; EncodeGrid(g1, Budget, in, error);
		auto adapter = CreatePortAdapter(AerodynamicDragAdapterId, V::MapValue({}), ctx, error);
		std::shared_ptr<const Bytes> out;
		Check(!adapter->ConvertWithAuxiliary(offer, accept, in, {}, NoStatics, out, error), "drag needs the surface");
		Check(!CreatePortAdapter(AerodynamicDragAdapterId, V::MapValue({{"normal_coefficient", V::Float(-1)}}), ctx, error),
			"a negative coefficient is refused");
		Check(!CreatePortAdapter(AerodynamicDragAdapterId, V::MapValue({{"wind", V::Float(1)}}), ctx, error), "unknown parameters are refused");
		Check(CreatePortAdapter(AerodynamicDragAdapterId, V::MapValue({{"normal_coefficient", V::Float(2)}}), ctx, error)->Identity() !=
			adapter->Identity(), "the coefficients enter the identity");
	}
	// ---------------------------------------------------------------- contact_reaction
	{
		const std::vector<std::string> bodies = {"ball", "box"};
		const auto offer = Port("collider_reaction", "field.v1", true, "contact reaction", "N",
			{{"force", "force", "N", 3}, {"point", "position", "m", 3}, {"torque", "torque", "N m", 3}});
		const auto accept = Port("body_load", "field.v1", false, "body load", "N",
			{{"force", "force", "N", 3}, {"point", "position", "m", 3}, {"torque", "torque", "N m", 3}}, TransformBodyDomain(bodies));
		PortAdapterContext ctx; ctx.offer = &offer; ctx.accept = &accept;
		auto adapter = CreatePortAdapter(ContactReactionAdapterId, V::MapValue({{"targets", V::ArrayValue({V::String("box"), V::String("box")})},
			{"bodies", V::ArrayValue({V::String("ball"), V::String("box")})}}), ctx, error);
		Check(adapter != nullptr, "contact_reaction", error);
		FieldFrame f; f.domain = RISE::RISECBOR64::SHA256Hex(Bytes{9}); f.count = 2;
		f.channels = {{"force", "N", 3, {Bits(1), Bits(0), Bits(0), Bits(0), Bits(2), Bits(0)}},
			{"point", "m", 3, {Bits(0), Bits(0), Bits(0), Bits(1), Bits(0), Bits(0)}},
			{"torque", "N m", 3, {Bits(0), Bits(0), Bits(0.5), Bits(0), Bits(0), Bits(0)}}};
		Bytes in; EncodeField(f, Budget, in, error);
		std::shared_ptr<const Bytes> out;
		FieldFrame g;
		Check(adapter && adapter->Convert(offer, accept, in, NoStatics, out, error) && DecodeField(*out, Budget, g, error) &&
			g.domain == accept.domain && g.count == 2, "convert", error);
		// box (index 1): force (1, 2, 0) at (0, 0, 0); torque 0.5 + (1, 0, 0) x (0, 2, 0) = (0, 0, 2.5).
		Check(Float(g.channels[0].values[3]) == 1 && Float(g.channels[0].values[4]) == 2 && Float(g.channels[2].values[5]) == 2.5 &&
			Float(g.channels[0].values[0]) == 0, "two colliders onto one body, torque moved to its point; the other body gets nothing");
		Check(!CreatePortAdapter(ContactReactionAdapterId, V::MapValue({{"targets", V::ArrayValue({V::String("cup")})},
			{"bodies", V::ArrayValue({V::String("ball"), V::String("box")})}}), ctx, error), "an unknown target body is refused");
	}
	Check(!CreatePortAdapter("no_such_adapter", V::MapValue({}), PortAdapterContext(), error), "an unknown adapter id is refused");
	if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
	std::cout << "SimulationPortAdapterTest: all checks passed\n";
	return 0;
}
