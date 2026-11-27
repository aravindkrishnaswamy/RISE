//////////////////////////////////////////////////////////////////////
//
//  RigidParameterSchema.cpp - see RigidParameterSchema.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RigidParameterSchema.h"
#include "RigidSolver.h"

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

// The options after a body's shape: mass m | density d | at x y z |
// orientation w x y z | velocity vx vy vz | spin wx wy wz; m7h (body-body
// contact): restitution e | friction mu | contact on|off.
bool Options(const std::vector<std::string>& t, std::size_t i, bool scene, V::Members& m, const std::string& where, std::string& error)
{
	while (i < t.size()) {
		const std::string& k = t[i];
		V v;
		if ((k == "mass" || k == "density" || k == "restitution" || k == "friction") && i + 1 < t.size()) {
			double x = 0;
			if (!Real(t[i + 1], x)) return Fail(error, where + ": " + k + " takes a number");
			m.emplace_back(k, V::Float(x));
			i += 2;
		} else if (k == "at" && !scene && Numbers(t, i + 1, 3, v)) { m.emplace_back("position", v); i += 4; }
		else if (k == "orientation" && !scene && Numbers(t, i + 1, 4, v)) { m.emplace_back("orientation", v); i += 5; }
		else if (k == "velocity" && Numbers(t, i + 1, 3, v)) { m.emplace_back("velocity", v); i += 4; }
		else if (k == "spin" && Numbers(t, i + 1, 3, v)) { m.emplace_back("angular_velocity", v); i += 4; }
		else if (k == "contact" && i + 1 < t.size() && (t[i + 1] == "on" || t[i + 1] == "off")) { m.emplace_back("contact", V::Bool(t[i + 1] == "on")); i += 2; }
		else return Fail(error, where + ": unexpected `" + k + "` (mass m | density d | at x y z | orientation w x y z | "
			"velocity vx vy vz | spin wx wy wz | restitution e | friction mu | contact on|off" +
			(scene ? "; a scene-object body takes its pose from the object" : "") + ")");
	}
	return true;
}

// `body <name> box sx sy sz | sphere r | cylinder r h [options]`
bool TranslateBody(const std::string& text, V& body, std::string& error)
{
	const auto t = Tokens(text);
	const std::string where = "body `" + text + "`";
	if (t.size() < 2) return Fail(error, where + ": expected `<name> box sx sy sz | sphere r | cylinder r h [options]`");
	V::Members m{{"name", V::String(t[0])}, {"shape", V::String(t[1])}};
	std::size_t i = 2;
	V v;
	if (t[1] == "box" && Numbers(t, 2, 3, v)) { m.emplace_back("size", v); i = 5; }
	else if (t[1] == "sphere" && Numbers(t, 2, 1, v)) { m.emplace_back("radius", v.GetArray()[0]); i = 3; }
	else if (t[1] == "cylinder" && Numbers(t, 2, 2, v)) { m.emplace_back("radius", v.GetArray()[0]); m.emplace_back("height", v.GetArray()[1]); i = 4; }
	else return Fail(error, where + ": the shape is `box sx sy sz`, `sphere r` or `cylinder r h` (metres)");
	if (!Options(t, i, false, m, where, error)) return false;
	body = V::MapValue(m);
	return true;
}

bool Translate(const SchemaFieldValues& values, SchemaTranslation& out, std::string& error)
{
	auto all = [&](const char* name) -> const std::vector<std::string>& {
		static const std::vector<std::string> none;
		const auto it = values.find(name);
		return it == values.end() ? none : it->second;
	};
	V::Values bodies, planes;
	for (const auto& text : all("body")) {
		V b;
		if (!TranslateBody(text, b, error)) return false;
		bodies.push_back(b);
	}
	for (const auto& text : all("body_object")) {
		const auto t = Tokens(text);
		if (t.empty()) return Fail(error, "body_object names a scene object");
		V::Members m{{"name", V::String(t[0])}, {"shape", V::String("mesh")}, {"scene_input", V::String(t[0])}};
		if (!Options(t, 1, true, m, "body_object `" + text + "`", error)) return false;
		bodies.push_back(V::MapValue(m));
		if (std::find(out.sceneInputs.begin(), out.sceneInputs.end(), t[0]) == out.sceneInputs.end()) out.sceneInputs.push_back(t[0]);
	}
	for (const auto& text : all("plane")) {
		const auto t = Tokens(text);
		V a, n;
		if (!Numbers(t, 0, 3, a) || !Numbers(t, 3, 3, n)) return Fail(error, "plane `" + text + "`: expected `px py pz nx ny nz [restitution e] [friction mu]`");
		V::Members m{{"point", a}, {"normal", n}};
		for (std::size_t i = 6; i < t.size(); i += 2) {
			double x = 0;
			if ((t[i] != "restitution" && t[i] != "friction") || i + 1 >= t.size() || !Real(t[i + 1], x))
				return Fail(error, "plane `" + text + "`: options are restitution e and friction mu");
			m.emplace_back(t[i], V::Float(x));
		}
		planes.push_back(V::MapValue(m));
	}
	if (!bodies.empty()) out.parameters.emplace_back("bodies", V::ArrayValue(bodies));
	if (!planes.empty()) out.parameters.emplace_back("planes", V::ArrayValue(planes));
	return true;
}

ParameterSchema Build()
{
	ParameterSchema s;
	s.solverId = RigidSolverId;
	s.summary = "Rigid bodies simulated by the rigid.bodies solver (mass and principal inertia from density and shape, "
		"gravity, loads from coupling ports, impulse contact with static planes; symplectic principal-axis splitting), "
		"baked by `rise-sim bake <scene>`; couple it to a cloth with a `simulation_group`.  Publishes `bodies` "
		"(transform.v1); a `standard_object` follows a body with `transform_source <name>.bodies[<body>]`.  "
		"Parameters and evidence: docs/SIMULATION_M6_RESULTS.md.";
	auto P = [&s](const char* name, SchemaType type, const char* def, const char* units, const char* doc) -> SchemaParameter& {
		s.parameters.emplace_back();
		SchemaParameter& p = s.parameters.back();
		p.name = name; p.type = type; p.defaultText = def; p.units = units; p.doc = doc;
		return p;
	};
	{ auto& p = P("frames", SchemaType::Count, "121", "", "Output frames (the first at t = 0)"); p.hasRange = true; p.minimum = 1; p.maximum = 1000000; }
	P("frame_rate", SchemaType::Real, "30", "Hz", "Output rate; frame i is at t = i / frame_rate");
	{ auto& p = P("substeps", SchemaType::Count, "8", "", "Substeps per output frame (the evidence runs at this substep)"); p.hasRange = true; p.minimum = 1; p.maximum = 100000; }
	P("gravity", SchemaType::Vector3, "0 -9.81 0", "m/s^2", "Gravitational acceleration");
	auto F = [&s](const char* name, const char* doc) {
		s.chunkFields.emplace_back();
		SchemaChunkField& f = s.chunkFields.back();
		f.name = name; f.kind = SchemaChunkField::Kind::Text; f.repeatable = true; f.doc = doc;
	};
	F("body", "A body: `<name> box sx sy sz | sphere r | cylinder r h` (metres, cylinder axis z) then any of `mass m` | "
		"`density d` (kg/m^3, default 1000) | `at x y z` | `orientation w x y z` | `velocity vx vy vz` | `spin wx wy wz` (rad/s, world) | "
		"`restitution e` | `friction mu` (body-body contact, defaults 0.5) | `contact on|off`");
	F("body_object", "A body made from a scene object's geometry (closed mesh, world space at t = 0; the pose is its centroid "
		"and principal axes): `<object> [mass m | density d] [velocity vx vy vz] [spin wx wy wz] [contact off]`; the body is named after the "
		"object (a mesh body takes no body-body contact: among other bodies it needs `contact off`)");
	F("plane", "A static plane the bodies bounce on (impulse contact, preview): `px py pz nx ny nz [restitution e] [friction mu]`");
	s.outputs = {{"bodies", "transform.v1", "Every body's position, orientation, velocity and angular velocity"}};
	s.translate = Translate;
	return s;
}
} // namespace

const ParameterSchema& RigidParameterSchema()
{
	static const ParameterSchema schema = Build();
	return schema;
}

} }
