//////////////////////////////////////////////////////////////////////
//
//  ClothParameterSchema.cpp - Scene-chunk parameter schema and text-to-
//  CBOR translator for the cloth.shell solver (materials, pins,
//  analytic/mesh/port colliders, and solver defaults; SI units).
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothParameterSchema.h"
#include "ClothSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

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

// `count` numbers from tokens[at...] as an array of floats.
bool Numbers(const std::vector<std::string>& tokens, std::size_t at, std::size_t count, V& out)
{
	if (tokens.size() < at + count) return false;
	V::Values xs;
	for (std::size_t i = 0; i < count; ++i) {
		double x = 0;
		if (!Real(tokens[at + i], x)) return false;
		xs.push_back(V::Float(x));
	}
	out = V::ArrayValue(xs);
	return true;
}
bool Number(const std::vector<std::string>& tokens, std::size_t at, V& out)
{
	double x = 0;
	if (tokens.size() <= at || !Real(tokens[at], x)) return false;
	out = V::Float(x);
	return true;
}

// ------------------------------------------------------------------ token & field translators

const char* const MaterialFields[][2] = {
	{"material_name", "name"}, {"material_source", "source"},
	{"material_density", "density"}, {"material_stretch_warp", "stretch_warp"},
	{"material_stretch_weft", "stretch_weft"}, {"material_shear", "shear"}, {"material_poisson", "poisson"},
	{"material_bend_warp", "bend_warp"}, {"material_bend_weft", "bend_weft"},
	{"material_damping_mass", "damping_mass"}, {"material_damping_stiffness", "damping_stiffness"}};

// `pin <select> [depth N] [fixed | translate vx vy vz | oscillate ax ay az f |
//  rotate cx cy cz ax ay az rate | follow <object>]`
bool TranslatePin(const std::string& text, const std::vector<std::string>& followable, V& pin,
	std::vector<std::string>& used, std::string& error)
{
	const auto t = Tokens(text);
	const std::string where = "pin `" + text + "`: ";
	if (t.empty()) return Fail(error, "pin needs a vertex selection");
	static const char* const selections[] = {"u0v0", "u1v0", "u0v1", "u1v1", "u0", "u1", "v0", "v1", "boundary"};
	if (std::none_of(std::begin(selections), std::end(selections), [&](const char* s) { return t[0] == s; }))
		return Fail(error, where + "selection must be u0v0|u1v0|u0v1|u1v1|u0|u1|v0|v1|boundary");
	V::Members m{{"select", V::String(t[0])}};
	std::size_t i = 1;
	if (i < t.size() && t[i] == "depth") {
		double d = 0;
		if (i + 1 >= t.size() || !Real(t[i + 1], d) || d < 1 || std::floor(d) != d || d > 1000)
			return Fail(error, where + "depth must be an integer from 1 to 1000");
		m.emplace_back("depth", V::Unsigned(static_cast<std::uint64_t>(d)));
		i += 2;
	}
	if (i < t.size()) {
		const std::string kind = t[i];
		V motion;
		std::size_t used_tokens = 0;
		if (kind == "fixed") {
			motion = V::MapValue({{"type", V::String("fixed")}});
			used_tokens = 1;
		} else if (kind == "translate") {
			V velocity;
			if (!Numbers(t, i + 1, 3, velocity)) return Fail(error, where + "translate needs a velocity vx vy vz (m/s)");
			motion = V::MapValue({{"type", V::String("translate")}, {"velocity", velocity}});
			used_tokens = 4;
		} else if (kind == "oscillate") {
			V amplitude, frequency;
			if (!Numbers(t, i + 1, 3, amplitude) || !Number(t, i + 4, frequency))
				return Fail(error, where + "oscillate needs an amplitude ax ay az (m) and a frequency (Hz)");
			motion = V::MapValue({{"type", V::String("oscillate")}, {"amplitude", amplitude}, {"frequency", frequency}});
			used_tokens = 5;
		} else if (kind == "rotate") {
			V center, axis, rate;
			if (!Numbers(t, i + 1, 3, center) || !Numbers(t, i + 4, 3, axis) || !Number(t, i + 7, rate))
				return Fail(error, where + "rotate needs a centre cx cy cz, an axis ax ay az and a rate (rad/s)");
			motion = V::MapValue({{"type", V::String("rotate")}, {"center", center}, {"axis", axis}, {"rate", rate}});
			used_tokens = 8;
		} else if (kind == "follow") {
			if (i + 1 >= t.size()) return Fail(error, where + "follow names an object");
			const std::string object = t[i + 1];
			if (std::find(followable.begin(), followable.end(), object) == followable.end())
				return Fail(error, where + "follows `" + object + "`, which no `follow_object` line names");
			if (std::find(used.begin(), used.end(), object) == used.end()) used.push_back(object);
			motion = V::MapValue({{"type", V::String("scene_transform")}, {"input", V::String(object)}});
			used_tokens = 2;
		} else {
			return Fail(error, where + "unknown motion `" + kind + "` (fixed|translate|rotate|oscillate|follow)");
		}
		if (i + used_tokens != t.size()) return Fail(error, where + "unexpected text after the motion");
		m.emplace_back("motion", motion);
	}
	pin = V::MapValue(m);
	return true;
}

// `collider plane px py pz nx ny nz | sphere cx cy cz r | box x0 y0 z0 x1 y1 z1 |
//  cylinder px py pz ax ay az r`
bool TranslateCollider(const std::string& text, V& collider, std::string& error)
{
	const auto t = Tokens(text);
	const std::string where = "collider `" + text + "`: ";
	if (t.empty()) return Fail(error, "collider needs a shape");
	V a, b, r;
	if (t[0] == "plane" && t.size() == 7 && Numbers(t, 1, 3, a) && Numbers(t, 4, 3, b)) {
		collider = V::MapValue({{"type", V::String("plane")}, {"point", a}, {"normal", b}});
	} else if (t[0] == "sphere" && t.size() == 5 && Numbers(t, 1, 3, a) && Number(t, 4, r)) {
		collider = V::MapValue({{"type", V::String("sphere")}, {"center", a}, {"radius", r}});
	} else if (t[0] == "box" && t.size() == 7 && Numbers(t, 1, 3, a) && Numbers(t, 4, 3, b)) {
		collider = V::MapValue({{"type", V::String("box")}, {"min", a}, {"max", b}});
	} else if (t[0] == "cylinder" && t.size() == 8 && Numbers(t, 1, 3, a) && Numbers(t, 4, 3, b) && Number(t, 7, r)) {
		collider = V::MapValue({{"type", V::String("cylinder")}, {"point", a}, {"axis", b}, {"radius", r}});
	} else {
		return Fail(error, where + "expected `plane px py pz nx ny nz`, `sphere cx cy cz r`, "
			"`box x0 y0 z0 x1 y1 z1` or `cylinder px py pz ax ay az r`");
	}
	return true;
}

// Converts parsed cloth chunk fields into canonical CBOR solver parameters (material
// overrides, pin motions, analytic/mesh/port colliders) and collects ordered sceneInputs.
bool Translate(const SchemaFieldValues& values, SchemaTranslation& out, std::string& error)
{
	auto all = [&](const char* name) -> const std::vector<std::string>& {
		static const std::vector<std::string> none;
		const auto it = values.find(name);
		return it == values.end() ? none : it->second;
	};
	// Material: a built-in record by name, or that record with overrides.
	{
		V::Members overrides;
		for (const auto& f : MaterialFields) {
			const auto& given = all(f[0]);
			if (given.empty()) continue;
			const bool text = std::string(f[1]) == "name" || std::string(f[1]) == "source";
			double x = 0;
			if (!text && (given.size() != 1 || !Real(given.front(), x)))
				return Fail(error, std::string(f[0]) + " must be a finite number");
			overrides.emplace_back(f[1], text ? V::String(given.front()) : V::Float(x));
		}
		const auto& base = all("material");
		if (base.size() > 1) return Fail(error, "material is given more than once");
		if (!overrides.empty()) {
			if (!base.empty()) overrides.emplace_back("base", V::String(base.front()));
			out.parameters.emplace_back("material", V::MapValue(overrides));
		} else if (!base.empty()) {
			out.parameters.emplace_back("material", V::String(base.front()));
		}
	}
	// Pins, then colliders: the solver names its scene inputs in this order.
	const auto& follow = all("follow_object");
	std::vector<std::string> followed;
	V::Values pins;
	for (const auto& text : all("pin")) {
		V pin;
		if (!TranslatePin(text, follow, pin, followed, error)) return false;
		pins.push_back(pin);
	}
	for (const auto& object : follow)
		if (std::find(followed.begin(), followed.end(), object) == followed.end())
			return Fail(error, "follow_object `" + object + "` is not followed by any pin");
	if (!pins.empty()) out.parameters.emplace_back("pins", V::ArrayValue(pins));
	for (const auto& object : followed) out.sceneInputs.push_back(object);
	V::Values colliders;
	for (const auto& text : all("collider")) {
		V collider;
		if (!TranslateCollider(text, collider, error)) return false;
		colliders.push_back(collider);
	}
	for (const auto& object : all("collider_object")) {
		const auto names = Tokens(object);
		if (names.size() != 1) return Fail(error, "collider_object names one object");
		colliders.push_back(V::MapValue({{"type", V::String("mesh")}, {"scene_input", V::String(names[0])},
			{"motion", V::MapValue({{"type", V::String("scene_transform")}, {"input", V::String(names[0])}})}}));
		if (std::find(out.sceneInputs.begin(), out.sceneInputs.end(), names[0]) == out.sceneInputs.end())
			out.sceneInputs.push_back(names[0]);
	}
	// M6: `collider_port <object> [<port>]`: the object's geometry (world, t = 0) as a
	// mesh collider that follows a coupling port (default collider_surface).
	for (const auto& text : all("collider_port")) {
		const auto t = Tokens(text);
		if (t.empty() || t.size() > 2) return Fail(error, "collider_port names one object and optionally its port");
		colliders.push_back(V::MapValue({{"type", V::String("mesh")}, {"scene_input", V::String(t[0])},
			{"motion", V::MapValue({{"type", V::String("port")}, {"port", V::String(t.size() == 2 ? t[1] : "collider_surface")}})}}));
		if (std::find(out.sceneInputs.begin(), out.sceneInputs.end(), t[0]) == out.sceneInputs.end()) out.sceneInputs.push_back(t[0]);
	}
	if (!colliders.empty()) out.parameters.emplace_back("colliders", V::ArrayValue(colliders));
	return true;
}

// ------------------------------------------------------------------ schema definition

// Builds the authoritative cloth.shell parameter schema (scalar/enum/vector defaults,
// mode overrides, repeatable chunk directives, and mesh.v2 surface output).
ParameterSchema Build()
{
	ParameterSchema s;
	s.solverId = ClothSolverId;
	s.summary = "A cloth sheet simulated by the cloth.shell solver (continuum shell: membrane + bending, "
		"implicit integration, barrier contact), baked by `rise-sim bake <scene>` or run live in the "
		"viewport (mode interactive).  Publishes `surface` (mesh.v2: positions, velocities, normals, "
		"material UVs).  Parameters, defaults and evidence: docs/SIMULATION_SOLVER_CONTRACT.md.";
	auto P = [&s](const char* name, SchemaType type, const char* def, const char* units, const char* doc)
		-> SchemaParameter& {
		s.parameters.emplace_back();
		SchemaParameter& p = s.parameters.back();
		p.name = name; p.type = type; p.defaultText = def; p.units = units; p.doc = doc;
		return p;
	};
	auto range = [](SchemaParameter& p, double lo, double hi) { p.hasRange = true; p.minimum = lo; p.maximum = hi; };
	{ auto& p = P("mode", SchemaType::Enum, "bake", "", "bake: BDF2, Newton to a stated residual (can earn predictive); "
		"interactive: backward Euler, truncated Newton, live in the viewport (preview)"); p.values = {"bake", "interactive"}; }
	{ auto& p = P("integrator", SchemaType::Enum, "bdf2", "", "Time integrator (interactive mode needs backward_euler)");
	  p.values = {"bdf2", "backward_euler", "implicit_midpoint"}; p.modeDefaults = "interactive: backward_euler"; }
	P("size_u", SchemaType::Real, "1", "m", "Sheet extent along axis_u");
	P("size_v", SchemaType::Real, "1", "m", "Sheet extent along axis_v");
	{ auto& p = P("vertices_u", SchemaType::Count, "51", "", "Vertices along u"); range(p, 2, 2000); }
	{ auto& p = P("vertices_v", SchemaType::Count, "51", "", "Vertices along v"); range(p, 2, 2000); }
	{ auto& p = P("triangulation", SchemaType::Enum, "alternating", "", "Diagonal pattern of the grid"); p.values = {"alternating", "consistent"}; }
	{ auto& p = P("bending_model", SchemaType::Enum, "lines", "", "Bending discretisation"); p.values = {"lines", "hinges"}; }
	P("origin", SchemaType::Vector3, "-0.5 1 0.5", "m", "World position of vertex (0, 0)");
	P("axis_u", SchemaType::Vector3, "1 0 0", "", "Direction of the u edge (normalised)");
	P("axis_v", SchemaType::Vector3, "0 0 -1", "", "Direction of the v edge (made orthogonal to axis_u)");
	P("stretch_u", SchemaType::Real, "1", "", "Initial stretch along u (1 = at rest)");
	P("stretch_v", SchemaType::Real, "1", "", "Initial stretch along v (1 = at rest)");
	{ auto& p = P("frames", SchemaType::Count, "121", "", "Output frames (the first at t = 0)"); range(p, 1, 1000000); }
	P("frame_rate", SchemaType::Real, "30", "Hz", "Output rate; frame i is at t = i / frame_rate (simulation time = scene time)");
	{ auto& p = P("substeps", SchemaType::Count, "8", "", "Substeps per output frame"); range(p, 1, 100000); p.modeDefaults = "interactive: 2"; }
	P("gravity", SchemaType::Vector3, "0 -9.81 0", "m/s^2", "Gravitational acceleration");
	{ auto& p = P("contact", SchemaType::Enum, "barrier", "", "Contact model (mesh colliders and self-collision need barrier)");
	  p.values = {"barrier", "penalty"}; p.modeDefaults = "interactive: penalty"; }
	P("contact_stiffness", SchemaType::Real, "100000", "N/m", "Penalty contact stiffness");
	P("contact_thickness", SchemaType::Real, "0.002", "m", "Contact offset (the sheet keeps this distance from colliders)");
	P("barrier_stiffness", SchemaType::Real, "100000", "", "Barrier contact stiffness");
	P("contact_activation", SchemaType::Real, "0.001", "m", "Barrier activation distance");
	P("friction", SchemaType::Real, "0.3", "", "Coulomb friction coefficient");
	P("friction_velocity", SchemaType::Real, "0.001", "m/s", "Friction smoothing velocity");
	P("self_collision", SchemaType::Bool, "FALSE", "", "Barrier self-collision (cloth-cloth contact)");
	P("self_friction", SchemaType::Real, "0.3", "", "Coulomb friction coefficient between cloth layers (self-collision)");
	P("pressure", SchemaType::Real, "0", "Pa", "Uniform pressure on the front face");
	P("drag_coefficient", SchemaType::Real, "0", "", "Quadratic air drag coefficient (explicit, unvalidated)");
	P("air_density", SchemaType::Real, "1.2", "kg/m^3", "Air density for the drag");
	P("damping", SchemaType::Bool, "TRUE", "", "Rayleigh damping from the material record");
	{ auto& p = P("newton_tolerance", SchemaType::Real, "0.001", "", "Newton residual tolerance"); p.modeDefaults = "interactive: 0.01"; }
	{ auto& p = P("newton_iterations", SchemaType::Count, "50", "", "Newton iteration limit"); range(p, 1, 1000); p.modeDefaults = "interactive: 3"; }
	{ auto& p = P("cg_tolerance", SchemaType::Real, "1e-06", "", "Linear solver relative tolerance (below 1)"); p.modeDefaults = "interactive: 0.01"; }
	{ auto& p = P("cg_iterations", SchemaType::Count, "2000", "", "Linear solver iteration limit"); range(p, 1, 1000000); }
	{ auto& p = P("linear_solver", SchemaType::Enum, "auto", "", "Linear solver"); p.values = {"auto", "mg_pcg", "pcg", "direct"}; }
	{ auto& p = P("pace_ms", SchemaType::Count, "0", "ms", "Sleep per output frame (demonstrations); never keyed"); range(p, 0, 10000); p.keyed = false; }
	{ auto& p = P("threads", SchemaType::Count, "", "", "Solver threads (default min(4, cores)); results do not depend on it, never keyed");
	  range(p, 1, 256); p.keyed = false; }

	auto F = [&s](const char* name, SchemaChunkField::Kind kind, bool repeatable, const char* doc) -> SchemaChunkField& {
		s.chunkFields.emplace_back();
		SchemaChunkField& f = s.chunkFields.back();
		f.name = name; f.kind = kind; f.repeatable = repeatable; f.doc = doc;
		return f;
	};
	{ auto& f = F("material", SchemaChunkField::Kind::Text, false, "Built-in material record (the base for any material_* override)");
	  f.defaultText = "cotton-plain-weave"; f.values = {"cotton-plain-weave", "denim-twill", "silk-lightweight", "polyester-knit"}; }
	F("material_name", SchemaChunkField::Kind::Text, false, "Material override: record name (default: base + \" (modified)\")");
	F("material_source", SchemaChunkField::Kind::Text, false, "Material override: where the numbers come from");
	F("material_density", SchemaChunkField::Kind::Text, false, "Material override: areal density, kg/m^2");
	F("material_stretch_warp", SchemaChunkField::Kind::Text, false, "Material override: warp stretch stiffness, N/m");
	F("material_stretch_weft", SchemaChunkField::Kind::Text, false, "Material override: weft stretch stiffness, N/m");
	F("material_shear", SchemaChunkField::Kind::Text, false, "Material override: shear stiffness, N/m");
	F("material_poisson", SchemaChunkField::Kind::Text, false, "Material override: Poisson ratio");
	F("material_bend_warp", SchemaChunkField::Kind::Text, false, "Material override: warp bending stiffness, N*m");
	F("material_bend_weft", SchemaChunkField::Kind::Text, false, "Material override: weft bending stiffness, N*m");
	F("material_damping_mass", SchemaChunkField::Kind::Text, false, "Material override: Rayleigh mass damping, 1/s");
	F("material_damping_stiffness", SchemaChunkField::Kind::Text, false, "Material override: Rayleigh stiffness damping, s");
	F("pin", SchemaChunkField::Kind::Text, true, "A pinned vertex group: `<u0v0|u1v0|u0v1|u1v1|u0|u1|v0|v1|boundary> "
		"[depth N] [fixed | translate vx vy vz | oscillate ax ay az hz | rotate cx cy cz ax ay az rad_per_s | follow <object>]`; "
		"`follow` moves the group rigidly with a scene object named by a follow_object line");
	F("follow_object", SchemaChunkField::Kind::ObjectReference, true, "A scene object a pin follows (its keyframed transform, "
		"sampled through the animator at solver times; rigid part only)");
	F("collider", SchemaChunkField::Kind::Text, true, "An analytic collider: `plane px py pz nx ny nz`, `sphere cx cy cz r`, "
		"`box x0 y0 z0 x1 y1 z1` or `cylinder px py pz ax ay az r` (metres, world space)");
	F("collider_object", SchemaChunkField::Kind::ObjectReference, true, "A scene object the cloth collides with: its geometry "
		"(tessellated, world space at t = 0) as a mesh collider moving rigidly with the object's transform");
	F("collider_port", SchemaChunkField::Kind::Text, true, "M6: `<object> [<port>]`: the object's geometry (world space at "
		"t = 0) as a mesh collider moved by a coupling port (default `collider_surface`, fed by a simulation_group "
		"connection, e.g. from a rigid body's body_state via rigid_surface); the cloth then offers `collider_reaction`");
	s.outputs = {{"surface", "mesh.v2", "The sheet: positions (m), velocities (m/s), normals, material UVs (m)"}};
	s.translate = Translate;
	return s;
}
} // namespace

const ParameterSchema& ClothParameterSchema()
{
	static const ParameterSchema schema = Build();
	return schema;
}

} }
