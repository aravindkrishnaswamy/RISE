//////////////////////////////////////////////////////////////////////
//
//  LiquidParameterSchema.cpp - see LiquidParameterSchema.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidParameterSchema.h"
#include "LiquidSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;

bool Fail(std::string& error, const std::string& message) { error = message; return false; }

std::vector<std::string> Tokens(const std::string& text)
{
	std::istringstream in(text);
	std::vector<std::string> out;
	std::string t;
	while (in >> t) out.push_back(t);
	return out;
}

bool Real(const std::string& token, double& x)
{
	if (token.empty()) return false;
	char* end = nullptr;
	x = std::strtod(token.c_str(), &end);
	return end && *end == '\0' && std::isfinite(x);
}

bool Numbers(const std::vector<std::string>& t, std::size_t at, std::size_t n, V& out)
{
	if (t.size() < at + n) return false;
	V::Values xs;
	for (std::size_t i = 0; i < n; ++i) { double x = 0; if (!Real(t[at + i], x)) return false; xs.push_back(V::Float(x)); }
	out = V::ArrayValue(xs);
	return true;
}

// `liquid box x0 y0 z0 x1 y1 z1 | sphere cx cy cz r | fill h`
bool TranslateLiquid(const std::string& text, V& region, std::string& error)
{
	const auto t = Tokens(text);
	V a, b;
	if (t.size() == 7 && t[0] == "box" && Numbers(t, 1, 3, a) && Numbers(t, 4, 3, b))
		region = V::MapValue({{"type", V::String("box")}, {"min", a}, {"max", b}});
	else if (t.size() == 5 && t[0] == "sphere" && Numbers(t, 1, 3, a) && Numbers(t, 4, 1, b))
		region = V::MapValue({{"type", V::String("sphere")}, {"center", a}, {"radius", b.GetArray()[0]}});
	else if (t.size() == 2 && t[0] == "fill" && Numbers(t, 1, 1, a))
		region = V::MapValue({{"type", V::String("fill")}, {"height", a.GetArray()[0]}});
	else return Fail(error, "liquid `" + text + "`: expected `box x0 y0 z0 x1 y1 z1`, `sphere cx cy cz r` or `fill h` (metres)");
	return true;
}

// `solid plane px py pz nx ny nz | sphere cx cy cz r | box x0 y0 z0 x1 y1 z1 | cylinder cx cy cz ax ay az r h`
bool TranslateSolid(const std::string& text, V& solid, std::string& error)
{
	const auto t = Tokens(text);
	V a, b, c;
	if (t.size() == 7 && t[0] == "plane" && Numbers(t, 1, 3, a) && Numbers(t, 4, 3, b))
		solid = V::MapValue({{"type", V::String("plane")}, {"point", a}, {"normal", b}});
	else if (t.size() == 5 && t[0] == "sphere" && Numbers(t, 1, 3, a) && Numbers(t, 4, 1, b))
		solid = V::MapValue({{"type", V::String("sphere")}, {"center", a}, {"radius", b.GetArray()[0]}});
	else if (t.size() == 7 && t[0] == "box" && Numbers(t, 1, 3, a) && Numbers(t, 4, 3, b))
		solid = V::MapValue({{"type", V::String("box")}, {"min", a}, {"max", b}});
	else if (t.size() == 9 && t[0] == "cylinder" && Numbers(t, 1, 3, a) && Numbers(t, 4, 3, b) && Numbers(t, 7, 2, c))
		solid = V::MapValue({{"type", V::String("cylinder")}, {"center", a}, {"axis", b}, {"radius", c.GetArray()[0]},
			{"height", c.GetArray()[1]}});
	else return Fail(error, "solid `" + text + "`: expected `plane px py pz nx ny nz`, `sphere cx cy cz r`, "
		"`box x0 y0 z0 x1 y1 z1` or `cylinder cx cy cz ax ay az r h` (metres, world space)");
	return true;
}

// M7b: `body <name> sphere r | box sx sy sz | cylinder r h [options]`, options
// `motion dynamic|prescribed|port`, `density d`, `mass m`, `at x y z`,
// `orientation w x y z`, `velocity vx vy vz`, `spin wx wy wz`, `lock <xyzr>`,
// `amplitude ax ay az`, `frequency f`, `phase p`, `coupling implicit|staggered`.
// The options of `body` and `body_object` (from token i).
bool BodyOptions(const std::vector<std::string>& t, std::size_t i, V::Members& m, const std::string& where, std::string& error)
{
	V v;
	while (i < t.size()) {
		const std::string& k = t[i];
		double x = 0;
		if ((k == "density" || k == "mass" || k == "frequency" || k == "phase") && i + 1 < t.size() && Real(t[i + 1], x)) {
			m.emplace_back(k, V::Float(x)); i += 2;
		} else if ((k == "motion" || k == "coupling") && i + 1 < t.size()) { m.emplace_back(k, V::String(t[i + 1])); i += 2; }
		else if (k == "at" && Numbers(t, i + 1, 3, v)) { m.emplace_back("position", v); i += 4; }
		else if (k == "orientation" && Numbers(t, i + 1, 4, v)) { m.emplace_back("orientation", v); i += 5; }
		else if (k == "velocity" && Numbers(t, i + 1, 3, v)) { m.emplace_back("velocity", v); i += 4; }
		else if (k == "spin" && Numbers(t, i + 1, 3, v)) { m.emplace_back("angular_velocity", v); i += 4; }
		else if (k == "amplitude" && Numbers(t, i + 1, 3, v)) { m.emplace_back("amplitude", v); i += 4; }
		else if (k == "lock" && i + 1 < t.size() && t[i + 1].find_first_not_of("xyzr") == std::string::npos) {
			const std::string& l = t[i + 1];
			m.emplace_back("free", V::ArrayValue({V::Bool(l.find('x') == std::string::npos), V::Bool(l.find('y') == std::string::npos),
				V::Bool(l.find('z') == std::string::npos), V::Bool(l.find('r') == std::string::npos)}));
			i += 2;
		} else return Fail(error, where + ": unexpected `" + k + "` (motion dynamic|prescribed|port | density d | mass m | at x y z | "
			"orientation w x y z | velocity vx vy vz | spin wx wy wz | lock <xyzr> | amplitude ax ay az | frequency f | phase p | "
			"coupling implicit|staggered)");
	}
	return true;
}

bool TranslateBody(const std::string& text, V& body, std::string& error)
{
	const auto t = Tokens(text);
	const std::string where = "body `" + text + "`";
	if (t.size() < 2) return Fail(error, where + ": expected `<name> sphere r | box sx sy sz | cylinder r h [options]`");
	V::Members m{{"name", V::String(t[0])}, {"shape", V::String(t[1])}};
	std::size_t i = 2;
	V v;
	if (t[1] == "sphere" && Numbers(t, 2, 1, v)) { m.emplace_back("radius", v.GetArray()[0]); i = 3; }
	else if (t[1] == "box" && Numbers(t, 2, 3, v)) { m.emplace_back("size", v); i = 5; }
	else if (t[1] == "cylinder" && Numbers(t, 2, 2, v)) { m.emplace_back("radius", v.GetArray()[0]); m.emplace_back("height", v.GetArray()[1]); i = 4; }
	else return Fail(error, where + ": the shape is `sphere r`, `box sx sy sz` or `cylinder r h` (metres, body frame, cylinder axis z)");
	if (!BodyOptions(t, i, m, where, error)) return false;
	body = V::MapValue(m);
	return true;
}

// m7d: `body_object <object> [options]`: the object's closed mesh (world space at
// t = 0) as a body named after it, with the options of `body` but `at` and
// `orientation` (the solver refuses those: the pose is the mesh's own).
bool TranslateBodyObject(const std::string& text, V& body, std::string& error)
{
	const auto t = Tokens(text);
	if (t.empty()) return Fail(error, "body_object names a scene object");
	V::Members m{{"name", V::String(t[0])}, {"shape", V::String("mesh")}, {"scene_input", V::String(t[0])}};
	if (!BodyOptions(t, 1, m, "body_object `" + text + "`", error)) return false;
	body = V::MapValue(m);
	return true;
}

// m7d: `sheet <name> port [port <accepted>] [load_port <offered>] [relaxation w]`
// (a mesh surface through a port, e.g. a cloth's surface_state; m7e: `coupling
// implicit` with `mechanics_port`, `stiffness_port`, `steps_port`, `response`; m7h: `elements_port`) or `sheet <name>
// rect sx sy [divisions nx ny] [at x y z] [orientation w x y z] [velocity vx vy vz]
// [spin wx wy wz] [amplitude ax ay az] [frequency f] [phase p]` (a prescribed
// rectangle in its frame's xy plane, normal +z).
bool TranslateSheet(const std::string& text, V& sheet, std::string& error)
{
	const auto t = Tokens(text);
	const std::string where = "sheet `" + text + "`";
	if (t.size() < 2) return Fail(error, where + ": expected `<name> port [options]` or `<name> rect sx sy [options]`");
	V::Members m{{"name", V::String(t[0])}};
	std::size_t i = 2;
	V v;
	if (t[1] == "port") {
		m.emplace_back("motion", V::String("port"));
		while (i < t.size()) {
			double x = 0;
			if ((t[i] == "port" || t[i] == "load_port" || t[i] == "mechanics_port" || t[i] == "stiffness_port" || t[i] == "steps_port" || t[i] == "elements_port" ||
				t[i] == "coupling" || t[i] == "response") && i + 1 < t.size()) { m.emplace_back(t[i], V::String(t[i + 1])); i += 2; }
			else if (t[i] == "relaxation" && i + 1 < t.size() && Real(t[i + 1], x)) { m.emplace_back("relaxation", V::Float(x)); i += 2; }
			else return Fail(error, where + ": unexpected `" + t[i] + "` (port <name> | load_port <name> | relaxation w | coupling implicit|staggered | "
				"mechanics_port <name> | stiffness_port <name> | steps_port <name> | elements_port <name> | response stiffness|lumped)");
		}
	} else if (t[1] == "rect" && Numbers(t, 2, 2, v)) {
		m.emplace_back("size", v);
		i = 4;
		while (i < t.size()) {
			const std::string& k = t[i];
			double x = 0;
			if ((k == "frequency" || k == "phase") && i + 1 < t.size() && Real(t[i + 1], x)) { m.emplace_back(k, V::Float(x)); i += 2; }
			else if (k == "divisions" && i + 2 < t.size()) {
				V::Values d;
				for (int q = 1; q <= 2; ++q) {
					double c = 0;
					if (!Real(t[i + std::size_t(q)], c) || c < 1 || std::floor(c) != c) return Fail(error, where + ": divisions are two counts");
					d.push_back(V::Unsigned(std::uint64_t(c)));
				}
				m.emplace_back("divisions", V::ArrayValue(d));
				i += 3;
			}
			else if (k == "at" && Numbers(t, i + 1, 3, v)) { m.emplace_back("position", v); i += 4; }
			else if (k == "orientation" && Numbers(t, i + 1, 4, v)) { m.emplace_back("orientation", v); i += 5; }
			else if (k == "velocity" && Numbers(t, i + 1, 3, v)) { m.emplace_back("velocity", v); i += 4; }
			else if (k == "spin" && Numbers(t, i + 1, 3, v)) { m.emplace_back("angular_velocity", v); i += 4; }
			else if (k == "amplitude" && Numbers(t, i + 1, 3, v)) { m.emplace_back("amplitude", v); i += 4; }
			else return Fail(error, where + ": unexpected `" + k + "` (divisions nx ny | at x y z | orientation w x y z | velocity vx vy vz | "
				"spin wx wy wz | amplitude ax ay az | frequency f | phase p)");
		}
	} else return Fail(error, where + ": the kind is `port` or `rect sx sy`");
	sheet = V::MapValue(m);
	return true;
}

bool Translate(const SchemaFieldValues& values, SchemaTranslation& out, std::string& error)
{
	auto all = [&](const char* name) -> const std::vector<std::string>& {
		static const std::vector<std::string> none;
		const auto it = values.find(name);
		return it == values.end() ? none : it->second;
	};
	V::Values liquid, solids;
	for (const auto& text : all("liquid")) {
		V r;
		if (!TranslateLiquid(text, r, error)) return false;
		liquid.push_back(r);
	}
	for (const auto& text : all("solid")) {
		V s;
		if (!TranslateSolid(text, s, error)) return false;
		solids.push_back(s);
	}
	for (const auto& text : all("solid_object")) {
		const auto t = Tokens(text);
		if (t.size() != 1) return Fail(error, "solid_object names one scene object");
		solids.push_back(V::MapValue({{"type", V::String("mesh")}, {"scene_input", V::String(t[0])}}));
		if (std::find(out.sceneInputs.begin(), out.sceneInputs.end(), t[0]) == out.sceneInputs.end()) out.sceneInputs.push_back(t[0]);
	}
	V::Values bodies;
	for (const auto& text : all("body")) {
		V b;
		if (!TranslateBody(text, b, error)) return false;
		bodies.push_back(b);
	}
	for (const auto& text : all("body_object")) {
		V b;
		if (!TranslateBodyObject(text, b, error)) return false;
		bodies.push_back(b);
		const std::string object = Tokens(text)[0];
		if (std::find(out.sceneInputs.begin(), out.sceneInputs.end(), object) == out.sceneInputs.end()) out.sceneInputs.push_back(object);
	}
	V::Values sheets;
	for (const auto& text : all("sheet")) {
		V sh;
		if (!TranslateSheet(text, sh, error)) return false;
		sheets.push_back(sh);
	}
	if (!liquid.empty()) out.parameters.emplace_back("liquid", V::ArrayValue(liquid));
	if (!sheets.empty()) out.parameters.emplace_back("sheets", V::ArrayValue(sheets));
	if (!solids.empty()) out.parameters.emplace_back("solids", V::ArrayValue(solids));
	if (!bodies.empty()) out.parameters.emplace_back("bodies", V::ArrayValue(bodies));
	return true;
}

ParameterSchema Build()
{
	ParameterSchema s;
	s.solverId = LiquidSolverId;
	s.summary = "A free-surface liquid simulated by the liquid.mac solver (staggered grid, level-set interface advected "
		"by WENO5, two-stage WENO5 momentum, ghost-fluid free surface and cut-cell static solids in one pressure solve, "
		"optionally viscous (implicit, strain-rate form) and with surface tension (ghost-fluid Laplace jump), m7c; rigid bodies including closed meshes and thin sheets as moving boundaries, m7d), baked by `rise-sim bake <scene>`.  Publishes `surface` (mesh.v2, a new topology "
		"every frame; render it with simulated_mesh_geometry and a dielectric), optionally `fields` (grid.v1: phi, "
		"pressure, velocity) and `particles` (points.v1, the marker particles); rigid bodies (`body`) move with the liquid "
		"in its pressure solve and publish `bodies` (transform.v1).  Parameters, evidence and limits: "
		"docs/SIMULATION_M7A_RESULTS.md, docs/SIMULATION_M7B_RESULTS.md, docs/SIMULATION_M7C_RESULTS.md, docs/SIMULATION_M7D_RESULTS.md.";
	auto P = [&s](const char* name, SchemaType type, const char* def, const char* units, const char* doc) -> SchemaParameter& {
		s.parameters.emplace_back();
		SchemaParameter& p = s.parameters.back();
		p.name = name; p.type = type; p.defaultText = def; p.units = units; p.doc = doc;
		return p;
	};
	auto range = [](SchemaParameter& p, double lo, double hi) { p.hasRange = true; p.minimum = lo; p.maximum = hi; };
	{ auto& p = P("frames", SchemaType::Count, "121", "", "Output frames (the first at t = 0)"); range(p, 1, 1000000); }
	P("frame_rate", SchemaType::Real, "30", "Hz", "Output rate; frame i is at t = i / frame_rate");
	P("domain_min", SchemaType::Vector3, "-0.5 0 -0.25", "m", "Lower corner of the simulation box (world space)");
	P("domain_size", SchemaType::Vector3, "1 0.5 0.5", "m", "Extent of the box; rounded to whole cells of the size the resolution gives");
	{ auto& p = P("resolution", SchemaType::Count, "64", "", "Cells along the longest side of the box (cubic cells)"); range(p, 1, 4096); }
	P("gravity", SchemaType::Vector3, "0 -9.81 0", "m/s^2", "Gravitational acceleration");
	P("density", SchemaType::Real, "1000", "kg/m^3", "Liquid density (the pressure output, and the forces on rigid bodies; without bodies the flow does not depend on it)");
	{ auto& p = P("top", SchemaType::Enum, "wall", "", "The +y face of the box: a free-slip wall or open (p = 0)"); p.values = {"wall", "open"}; }
	P("initial_velocity", SchemaType::Vector3, "0 0 0", "m/s", "Initial velocity of the liquid (projected)");
	P("viscosity", SchemaType::Real, "0", "Pa s", "Dynamic viscosity (m7c; 0: inviscid).  Implicit, the strain-rate (variational) form with zero viscous traction at the free surface");
	{ auto& p = P("viscous_walls", SchemaType::Enum, "no_slip", "", "With viscosity: the domain walls, static solids and bodies are no-slip or free-slip"); p.values = {"no_slip", "free_slip"}; }
	P("surface_tension", SchemaType::Real, "0", "N/m", "Surface tension coefficient (m7c; 0: none).  The Laplace jump sigma kappa at the free surface; without viscosity the label is preview");
	{ auto& p = P("momentum", SchemaType::Enum, "weno5", "", "Momentum advection (weno5 is second order with two stages; the others are first order, preview)");
	  p.values = {"weno5", "maccormack", "semi_lagrangian"}; }
	{ auto& p = P("momentum_stages", SchemaType::Count, "2", "", "1: advect then project (first order in time); 2: Heun with a projection per stage"); range(p, 1, 2); }
	{ auto& p = P("reinit", SchemaType::Enum, "sussman", "", "Level-set reinitialisation scheme");
	  p.values = {"sussman", "none", "frozen", "russo_smereka", "russo_smereka_central", "subcell"}; }
	{ auto& p = P("reinit_iterations", SchemaType::Count, "1", "", "Reinitialisation pseudo-time steps when it runs"); range(p, 1, 100); }
	P("reinit_threshold", SchemaType::Real, "0.1", "", "Reinitialise only when | |grad phi| - 1 | exceeds this within 3 cells of the surface (0: every step)");
	P("particles", SchemaType::Bool, "TRUE", "", "Marker particles (particle level set) correcting the interface");
	{ auto& p = P("particles_per_cell", SchemaType::Count, "16", "", "Marker particles seeded per cell near the surface"); range(p, 1, 256); }
	P("volume_correction", SchemaType::Bool, "TRUE", "", "Shift the level set each step to keep the initial liquid volume (the shift is reported)");
	P("cfl", SchemaType::Real, "0.5", "", "Advective CFL number (in (0, 1])");
	P("wave_cfl", SchemaType::Real, "0.5", "", "CFL number on the shallow-water wave speed sqrt(g D)");
	P("pressure_tolerance", SchemaType::Real, "1e-08", "", "Relative residual of the pressure solve");
	P("theta_min", SchemaType::Real, "0.001", "", "Smallest ghost-fluid interface fraction");
	P("speed_limit", SchemaType::Real, "3", "", "Safeguard: valid face speeds above this x sqrt(2 g D) are clamped and counted (0: off; not part of the validated model)");
	P("min_face_fraction", SchemaType::Real, "0", "", "Cut faces less open than this are closed (0: none; closing locates solids only to half a cell)");
	P("min_transport_fraction", SchemaType::Real, "0.25", "", "Cut faces less open than this keep their velocity in the pressure solve but are re-extrapolated before it is transported");
	P("viscosity_tolerance", SchemaType::Real, "1e-08", "", "Relative residual of the viscous solve");
	P("viscous_cfl", SchemaType::Real, "4", "", "With viscosity: dt <= viscous_cfl h^2 / nu (0: no limit), so the split viscous and pressure steps converge to Stokes flow");
	P("capillary_cfl", SchemaType::Real, "0.5", "", "With surface tension: dt <= capillary_cfl sqrt(rho h^3 / (2 pi sigma))");
	{ auto& p = P("curvature", SchemaType::Enum, "interpolated", "", "Surface tension: the curvature at the sub-cell interface position (interpolated) or the liquid cell's (cell)"); p.values = {"interpolated", "cell"}; }
	{ auto& p = P("extrapolation_layers", SchemaType::Count, "6", "", "Cells the velocity is extrapolated into the air for momentum advection"); range(p, 1, 64); }
	{ auto& p = P("interface_extrapolation_layers", SchemaType::Count, "4", "", "Cells the velocity is extrapolated into the air for the interface"); range(p, 1, 64); }
	{ auto& p = P("linear_extrapolation_layers", SchemaType::Count, "1", "", "Of the interface layers, those extrapolated linearly (second-order surface velocity)"); range(p, 0, 8); }
	P("publish_fields", SchemaType::Bool, "FALSE", "", "Publish the `fields` output (grid.v1) and offer it as a port");
	P("publish_particles", SchemaType::Bool, "FALSE", "", "Publish the `particles` output (needs particles; about 40 bytes per particle per frame)");
	{ auto& p = P("threads", SchemaType::Count, "", "", "Solver threads (default min(16, cores)); results do not depend on it, never keyed");
	  range(p, 1, 256); p.keyed = false; }
	auto F = [&s](const char* name, SchemaChunkField::Kind kind, const char* doc) {
		s.chunkFields.emplace_back();
		SchemaChunkField& f = s.chunkFields.back();
		f.name = name; f.kind = kind; f.repeatable = true; f.doc = doc;
	};
	F("liquid", SchemaChunkField::Kind::Text, "An initial liquid region (their union; default a box against the -x wall): "
		"`box x0 y0 z0 x1 y1 z1`, `sphere cx cy cz r` or `fill h` (everything below y = h), metres, world space");
	F("solid", SchemaChunkField::Kind::Text, "A static solid: `plane px py pz nx ny nz` (solid below), `sphere cx cy cz r`, "
		"`box x0 y0 z0 x1 y1 z1` or `cylinder cx cy cz ax ay az r h` (capped, centred), metres, world space");
	F("solid_object", SchemaChunkField::Kind::ObjectReference, "A scene object as a static solid: its geometry (closed mesh, "
		"world space at t = 0)");
	F("body", SchemaChunkField::Kind::Text, "A rigid body in the liquid (m7b): `<name> sphere r | box sx sy sz | cylinder r h` "
		"(metres, body frame at the centre of mass, cylinder axis z) then `motion dynamic|prescribed|port` (dynamic: moved "
		"by the liquid, coupled in its pressure solve; port: follows rigid.bodies in a simulation_group, `coupling "
		"implicit|staggered`), `at x y z`, `orientation w x y z`, `velocity vx vy vz`, `spin wx wy wz`, dynamic `density d` "
		"(default 1000) or `mass m` and `lock <xyzr>`, prescribed `amplitude ax ay az`, `frequency f`, `phase p`; "
		"render it with transform_source <simulation>.bodies[<name>]");
	F("sheet", SchemaChunkField::Kind::Text, "A thin open sheet as a moving boundary (m7d, preview): `<name> port [port <accepted>] "
		"[load_port <offered>] [relaxation w]` follows a mesh surface (connect a cloth's surface_state to <accepted>, default "
		"sheet_state, and <offered>, default sheet_load, to its surface_load in a simulation_group; the load is lagged one macro "
		"step and unstable for light sheets: docs/SIMULATION_M7D_RESULTS.md; m7e: `coupling implicit` solves the cloth's linearised "
		"response with the pressure, stable for cloth of any weight: also connect its surface_mechanics and surface_stiffness to "
		"<sheet_mechanics> and <sheet_stiffness>, and <sheet_load_steps> to its surface_load_steps, with `owned <liquid> <cloth>`; "
		"docs/SIMULATION_M7E_RESULTS.md), or `<name> rect sx sy [divisions nx ny] [at x y z] "
		"[orientation w x y z] [velocity vx vy vz] [spin wx wy wz] [amplitude ax ay az] [frequency f] [phase p]` a prescribed "
		"rectangle (its frame's xy plane, normal +z).  With `viscosity` (m7m) prescribed and implicit sheets are no-slip walls "
		"on each wetted side, an implicit cloth's velocities solved with the viscous step and its drag returned to it; a "
		"staggered or one-way sheet with viscosity is refused (docs/SIMULATION_M7M_RESULTS.md).  Stated limits (preview): "
		"liquid resting on an implicit sheet needs a layer about 2.5 cells deep, as on a rigid floor (m7l, two-valued level "
		"set; measured without the volume correction over 2 s: a held flat sheet's 2.4-cell sloshing layer loses 6%, a "
		"sagging bag's 4%, a held bowl's 1.3-cell rim 2.5%, a rigid floor's 2.4 cells 4%); very light cloth (below about "
		"0.1 kg/m^2) with a free edge at a waterline on a coarse grid is kicked at the edge (m7k: 0.05 kg/m^2 at 24 cells, "
		"edge peaks of 4-9 m/s in a dam break), which the progress detail flags as a rotation stretch or linearisation "
		"correction above 1%: use heavier cloth, pin the edge, or a finer grid (docs/SIMULATION_M7M_RESULTS.md, Known limits)");
	// Text, as rigid_simulation's body_object: the object's name is followed by options.
	F("body_object", SchemaChunkField::Kind::Text, "A rigid body made from a scene object's closed mesh (m7d; world "
		"space at t = 0, the body frame at its centroid and principal axes, mass properties from the mesh): `<object>` then the "
		"options of `body` but `at` and `orientation`; the body is named after the object");
	s.outputs = {{"surface", "mesh.v2", "The liquid surface: positions (m), velocities (m/s), normals; a new topology every frame"},
		{"fields", "grid.v1", "phi (m), pressure (Pa), cell-centred velocity (m/s), when publish_fields"},
		{"particles", "points.v1", "Marker particles: positions, velocities, radii, sign attribute, when publish_particles"},
		{"bodies", "transform.v1", "The rigid bodies' poses and velocities, when there are bodies"}};
	s.translate = Translate;
	return s;
}
} // namespace

const ParameterSchema& LiquidParameterSchema()
{
	static const ParameterSchema schema = Build();
	return schema;
}

} }
